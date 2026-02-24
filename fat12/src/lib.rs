#![no_std]
/// FAT12 Filesystem Driver - Static Function API
/// Supports reading, writing and creating files on FAT12 formatted drives
extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicBool, Ordering};
use ide::{ide_read_sectors, ide_write_sectors};
use serial::serial_println;

const SECTOR_SIZE: usize = 512;
const FAT12_EOF: u16 = 0xFF8;
const FAT12_BAD_CLUSTER: u16 = 0xFF7;
const FAT12_FREE_CLUSTER: u16 = 0x000;

static FAT_LOCK: AtomicBool = AtomicBool::new(false);
static mut FAT_INITIALIZED: bool = false;
static mut FAT_DRIVE: usize = 0;
static mut BPB: BiosParameterBlock = BiosParameterBlock {
    jmp_boot: [0; 3],
    oem_name: [0; 8],
    bytes_per_sector: 0,
    sectors_per_cluster: 0,
    reserved_sectors: 0,
    num_fats: 0,
    root_entry_count: 0,
    total_sectors_16: 0,
    media_type: 0,
    fat_size_16: 0,
    sectors_per_track: 0,
    num_heads: 0,
    hidden_sectors: 0,
    total_sectors_32: 0,
    drive_number: 0,
    reserved1: 0,
    boot_signature: 0,
    volume_id: 0,
    volume_label: [0; 11],
    fs_type: [0; 8],
};
static mut FAT_START_SECTOR: u64 = 0;
static mut ROOT_DIR_START_SECTOR: u64 = 0;
static mut DATA_START_SECTOR: u64 = 0;
static mut ROOT_DIR_SECTORS: u32 = 0;
static mut FAT_CACHE: Vec<u8> = Vec::new();

fn fat_lock() {
    while FAT_LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
}

fn fat_unlock() {
    FAT_LOCK.store(false, Ordering::Release);
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct BiosParameterBlock {
    jmp_boot: [u8; 3],
    oem_name: [u8; 8],
    bytes_per_sector: u16,
    sectors_per_cluster: u8,
    reserved_sectors: u16,
    num_fats: u8,
    root_entry_count: u16,
    total_sectors_16: u16,
    media_type: u8,
    fat_size_16: u16,
    sectors_per_track: u16,
    num_heads: u16,
    hidden_sectors: u32,
    total_sectors_32: u32,
    drive_number: u8,
    reserved1: u8,
    boot_signature: u8,
    volume_id: u32,
    volume_label: [u8; 11],
    fs_type: [u8; 8],
}

#[repr(C, packed)]
#[derive(Clone, Copy, Debug)]
pub struct DirectoryEntry {
    pub name: [u8; 11],
    pub attributes: u8,
    reserved: u8,
    creation_time_tenth: u8,
    creation_time: u16,
    creation_date: u16,
    last_access_date: u16,
    first_cluster_high: u16,
    last_mod_time: u16,
    last_mod_date: u16,
    pub first_cluster: u16,
    pub file_size: u32,
}

impl DirectoryEntry {
    pub fn is_empty(&self) -> bool {
        self.name[0] == 0x00
    }

    pub fn is_deleted(&self) -> bool {
        self.name[0] == 0xE5
    }

    pub fn is_lfn(&self) -> bool {
        self.attributes == 0x0F
    }

    pub fn is_directory(&self) -> bool {
        (self.attributes & 0x10) != 0
    }

    pub fn is_volume_id(&self) -> bool {
        (self.attributes & 0x08) != 0
    }

    pub fn get_name(&self) -> Result<String, &'static str> {
        if self.is_empty() || self.is_deleted() || self.is_lfn() || self.is_volume_id() {
            return Err("Invalid entry");
        }

        let mut name = String::new();

        for i in 0..8 {
            if self.name[i] == b' ' {
                break;
            }
            name.push(self.name[i] as char);
        }

        let mut has_ext = false;
        for i in 8..11 {
            if self.name[i] != b' ' {
                if !has_ext {
                    name.push('.');
                    has_ext = true;
                }
                name.push(self.name[i] as char);
            }
        }

        Ok(name)
    }

    pub fn set_name(&mut self, filename: &str) -> Result<(), &'static str> {
        let parts: Vec<&str> = filename.split('.').collect();
        let (name_part, ext_part) = if parts.len() == 2 {
            (parts[0], parts[1])
        } else if parts.len() == 1 {
            (parts[0], "")
        } else {
            return Err("Invalid filename format");
        };

        if name_part.len() > 8 || ext_part.len() > 3 {
            return Err("Filename too long");
        }

        self.name = [b' '; 11];

        for (i, byte) in name_part.bytes().enumerate() {
            self.name[i] = byte.to_ascii_uppercase();
        }

        for (i, byte) in ext_part.bytes().enumerate() {
            self.name[8 + i] = byte.to_ascii_uppercase();
        }

        Ok(())
    }

    pub fn new_file(name: &str, first_cluster: u16, size: u32) -> Result<Self, &'static str> {
        let mut entry = DirectoryEntry {
            name: [b' '; 11],
            attributes: 0x20,
            reserved: 0,
            creation_time_tenth: 0,
            creation_time: 0,
            creation_date: 0,
            last_access_date: 0,
            first_cluster_high: 0,
            last_mod_time: 0,
            last_mod_date: 0,
            first_cluster,
            file_size: size,
        };

        entry.set_name(name)?;
        Ok(entry)
    }
}

/// Initializes the FAT12 filesystem on the specified drive
///
/// # Arguments
/// * `drive` - The drive number to initialize
///
/// # Returns
/// * `Ok(())` if initialization succeeded
/// * `Err` with error message if failed
pub fn fat12_init(drive: usize) -> Result<(), &'static str> {
    fat_lock();

    serial_println!("FAT12: Reading boot sector from drive {}...", drive);

    let mut boot_sector = [0u8; SECTOR_SIZE];
    let err = ide_read_sectors(drive, 0, &mut boot_sector);

    serial_println!("FAT12: Boot sector read returned error code: {}", err);

    if err != 0 {
        fat_unlock();
        return Err("Failed to read boot sector");
    }

    let bpb = unsafe { *(boot_sector.as_ptr() as *const BiosParameterBlock) };

    let boot_sig_0 = boot_sector[510];
    let boot_sig_1 = boot_sector[511];
    let bytes_per_sector = bpb.bytes_per_sector;
    let fs_type = bpb.fs_type;

    serial_println!(
        "Boot sector signature: 0x{:02x}{:02x}",
        boot_sig_0,
        boot_sig_1
    );
    serial_println!("Bytes per sector: {}", bytes_per_sector);
    serial_println!("FS Type: {:?}", core::str::from_utf8(&fs_type));

    if boot_sig_0 != 0x55 || boot_sig_1 != 0xAA {
        fat_unlock();
        return Err("Invalid boot sector signature");
    }

    let is_fat12 =
        &fs_type[0..5] == b"FAT12" || bytes_per_sector == 512 && bpb.sectors_per_cluster > 0;

    if !is_fat12 {
        fat_unlock();
        return Err("Not a FAT12 filesystem");
    }

    let fat_start_sector = bpb.reserved_sectors as u64;
    let root_dir_sectors = ((bpb.root_entry_count as u32 * 32) + (bpb.bytes_per_sector as u32 - 1))
        / bpb.bytes_per_sector as u32;
    let root_dir_start_sector = fat_start_sector + (bpb.num_fats as u64 * bpb.fat_size_16 as u64);
    let data_start_sector = root_dir_start_sector + root_dir_sectors as u64;

    let fat_size_bytes = bpb.fat_size_16 as usize * SECTOR_SIZE;
    let mut fat_cache = alloc::vec![0u8; fat_size_bytes];

    for i in 0..bpb.fat_size_16 {
        let sector_data = &mut fat_cache[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
        let err = ide_read_sectors(drive, fat_start_sector + i as u64, sector_data);
        if err != 0 {
            fat_unlock();
            return Err("Failed to read FAT");
        }
    }

    unsafe {
        FAT_DRIVE = drive;
        BPB = bpb;
        FAT_START_SECTOR = fat_start_sector;
        ROOT_DIR_START_SECTOR = root_dir_start_sector;
        DATA_START_SECTOR = data_start_sector;
        ROOT_DIR_SECTORS = root_dir_sectors;
        FAT_CACHE = fat_cache;
        FAT_INITIALIZED = true;
    }

    fat_unlock();
    Ok(())
}

fn get_fat_entry(cluster: u16) -> u16 {
    unsafe {
        let fat_offset = (cluster as usize * 3) / 2;
        let entry = if cluster & 1 == 0 {
            u16::from_le_bytes([FAT_CACHE[fat_offset], FAT_CACHE[fat_offset + 1]]) & 0x0FFF
        } else {
            u16::from_le_bytes([FAT_CACHE[fat_offset], FAT_CACHE[fat_offset + 1]]) >> 4
        };
        entry
    }
}

fn set_fat_entry(cluster: u16, value: u16) {
    unsafe {
        let fat_offset = (cluster as usize * 3) / 2;

        if cluster & 1 == 0 {
            FAT_CACHE[fat_offset] = (value & 0xFF) as u8;
            FAT_CACHE[fat_offset + 1] =
                (FAT_CACHE[fat_offset + 1] & 0xF0) | ((value >> 8) & 0x0F) as u8;
        } else {
            FAT_CACHE[fat_offset] = (FAT_CACHE[fat_offset] & 0x0F) | ((value & 0x0F) << 4) as u8;
            FAT_CACHE[fat_offset + 1] = ((value >> 4) & 0xFF) as u8;
        }
    }
}

fn flush_fat() -> Result<(), &'static str> {
    fat_lock();

    unsafe {
        for i in 0..BPB.fat_size_16 {
            let sector_data = &FAT_CACHE[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            let err = ide_write_sectors(FAT_DRIVE, FAT_START_SECTOR + i as u64, sector_data);
            if err != 0 {
                fat_unlock();
                return Err("Failed to write FAT");
            }
        }

        for fat_num in 1..BPB.num_fats {
            let backup_start = FAT_START_SECTOR + (fat_num as u64 * BPB.fat_size_16 as u64);
            for i in 0..BPB.fat_size_16 {
                let sector_data =
                    &FAT_CACHE[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
                let err = ide_write_sectors(FAT_DRIVE, backup_start + i as u64, sector_data);
                if err != 0 {
                    fat_unlock();
                    return Err("Failed to write backup FAT");
                }
            }
        }
    }

    fat_unlock();
    Ok(())
}

fn find_free_cluster() -> Option<u16> {
    for cluster in 2..0xFF0 {
        if get_fat_entry(cluster) == FAT12_FREE_CLUSTER {
            return Some(cluster);
        }
    }
    None
}

fn allocate_cluster() -> Result<u16, &'static str> {
    let cluster = find_free_cluster().ok_or("No free clusters")?;
    set_fat_entry(cluster, FAT12_EOF);
    Ok(cluster)
}

fn cluster_to_sector(cluster: u16) -> u64 {
    unsafe { DATA_START_SECTOR + ((cluster as u64 - 2) * BPB.sectors_per_cluster as u64) }
}

fn read_cluster(cluster: u16) -> Result<Vec<u8>, &'static str> {
    fat_lock();

    unsafe {
        let sector = cluster_to_sector(cluster);
        let cluster_size = BPB.sectors_per_cluster as usize * SECTOR_SIZE;
        let mut data = alloc::vec![0u8; cluster_size];

        for i in 0..BPB.sectors_per_cluster {
            let sector_data = &mut data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            let err = ide_read_sectors(FAT_DRIVE, sector + i as u64, sector_data);
            if err != 0 {
                fat_unlock();
                return Err("Failed to read cluster");
            }
        }

        fat_unlock();
        Ok(data)
    }
}

fn write_cluster(cluster: u16, data: &[u8]) -> Result<(), &'static str> {
    fat_lock();

    unsafe {
        let sector = cluster_to_sector(cluster);
        let cluster_size = BPB.sectors_per_cluster as usize * SECTOR_SIZE;

        let mut padded_data = alloc::vec![0u8; cluster_size];
        let copy_len = core::cmp::min(data.len(), cluster_size);
        padded_data[..copy_len].copy_from_slice(&data[..copy_len]);

        for i in 0..BPB.sectors_per_cluster {
            let sector_data =
                &padded_data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            let err = ide_write_sectors(FAT_DRIVE, sector + i as u64, sector_data);
            if err != 0 {
                fat_unlock();
                return Err("Failed to write cluster");
            }
        }

        fat_unlock();
        Ok(())
    }
}

fn fat12_read_root_directory() -> Result<Vec<DirectoryEntry>, &'static str> {
    fat_lock();

    unsafe {
        let root_size = ROOT_DIR_SECTORS as usize * SECTOR_SIZE;
        let mut root_data = alloc::vec![0u8; root_size];

        for i in 0..ROOT_DIR_SECTORS {
            let sector_data =
                &mut root_data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            let err = ide_read_sectors(FAT_DRIVE, ROOT_DIR_START_SECTOR + i as u64, sector_data);
            if err != 0 {
                fat_unlock();
                return Err("Failed to read root directory");
            }
        }

        fat_unlock();

        let num_entries = BPB.root_entry_count as usize;
        let mut entries = Vec::new();

        for i in 0..num_entries {
            let offset = i * 32;
            let entry = *(root_data.as_ptr().add(offset) as *const DirectoryEntry);

            if entry.is_empty() {
                break;
            }

            if !entry.is_deleted() && !entry.is_lfn() && !entry.is_volume_id() {
                entries.push(entry);
            }
        }

        Ok(entries)
    }
}

fn write_root_directory(entries: &[DirectoryEntry]) -> Result<(), &'static str> {
    fat_lock();

    unsafe {
        let root_size = ROOT_DIR_SECTORS as usize * SECTOR_SIZE;
        let mut root_data = alloc::vec![0u8; root_size];

        for (i, entry) in entries.iter().enumerate() {
            let offset = i * 32;
            let entry_ptr = root_data.as_mut_ptr().add(offset) as *mut DirectoryEntry;
            *entry_ptr = *entry;
        }

        for i in 0..ROOT_DIR_SECTORS {
            let sector_data = &root_data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            let err = ide_write_sectors(FAT_DRIVE, ROOT_DIR_START_SECTOR + i as u64, sector_data);
            if err != 0 {
                fat_unlock();
                return Err("Failed to write root directory");
            }
        }

        fat_unlock();
        Ok(())
    }
}

fn find_file_entry(filename: &str) -> Result<DirectoryEntry, &'static str> {
    let entries = fat12_read_root_directory()?;

    for entry in entries {
        if let Ok(name) = entry.get_name() {
            if name.to_uppercase() == filename.to_uppercase() {
                return Ok(entry);
            }
        }
    }

    Err("File not found")
}

/// Reads a file by filename
///
/// # Arguments
/// * `filename` - Name of the file to read
///
/// # Returns
/// * `Ok(Vec<u8>)` containing file contents if successful
/// * `Err` with error message if failed
pub fn fat12_read_file(filename: &str) -> Result<Vec<u8>, &'static str> {
    let entry = find_file_entry(filename)?;

    if entry.file_size == 0 {
        return Ok(Vec::new());
    }

    let mut data = Vec::new();
    let mut cluster = entry.first_cluster;

    loop {
        if cluster < 2 || cluster >= FAT12_BAD_CLUSTER {
            break;
        }

        let cluster_data = read_cluster(cluster)?;
        data.extend_from_slice(&cluster_data);

        cluster = get_fat_entry(cluster);
        if cluster >= FAT12_EOF {
            break;
        }
    }

    data.truncate(entry.file_size as usize);
    Ok(data)
}

/// Creates a new file with the given data
///
/// # Arguments
/// * `filename` - Name for the new file (8.3 format)
/// * `data` - File contents to write
///
/// # Returns
/// * `Ok(())` if file created successfully
/// * `Err` with error message if failed
pub fn fat12_create_file(filename: &str, data: &[u8]) -> Result<(), &'static str> {
    unsafe {
        if !FAT_INITIALIZED {
            return Err("FAT12 not initialized");
        }

        let cluster_size = BPB.sectors_per_cluster as usize * SECTOR_SIZE;
        let num_clusters = (data.len() + cluster_size - 1) / cluster_size;

        if num_clusters == 0 {
            return Err("Cannot create empty file");
        }

        let mut clusters = Vec::new();
        for _ in 0..num_clusters {
            clusters.push(allocate_cluster()?);
        }

        for i in 0..clusters.len() - 1 {
            set_fat_entry(clusters[i], clusters[i + 1]);
        }
        set_fat_entry(clusters[clusters.len() - 1], FAT12_EOF);

        for (i, &cluster) in clusters.iter().enumerate() {
            let offset = i * cluster_size;
            let end = core::cmp::min(offset + cluster_size, data.len());
            write_cluster(cluster, &data[offset..end])?;
        }

        let entry = DirectoryEntry::new_file(filename, clusters[0], data.len() as u32)?;

        let mut entries = fat12_read_root_directory()?;

        for existing in &entries {
            if let Ok(name) = existing.get_name() {
                if name.to_uppercase() == filename.to_uppercase() {
                    return Err("File already exists");
                }
            }
        }

        entries.push(entry);
        write_root_directory(&entries)?;

        flush_fat()?;

        Ok(())
    }
}

/// Deletes a file by filename
///
/// # Arguments
/// * `filename` - Name of the file to delete
///
/// # Returns
/// * `Ok(())` if file deleted successfully
/// * `Err` with error message if failed
pub fn fat12_delete_file(filename: &str) -> Result<(), &'static str> {
    let mut entries = fat12_read_root_directory()?;
    let mut found_index = None;
    let mut first_cluster = 0u16;

    for (i, entry) in entries.iter().enumerate() {
        if let Ok(name) = entry.get_name() {
            if name.to_uppercase() == filename.to_uppercase() {
                found_index = Some(i);
                first_cluster = entry.first_cluster;
                break;
            }
        }
    }

    let index = found_index.ok_or("File not found")?;

    let mut cluster = first_cluster;
    while cluster >= 2 && cluster < FAT12_BAD_CLUSTER {
        let next_cluster = get_fat_entry(cluster);
        set_fat_entry(cluster, FAT12_FREE_CLUSTER);

        if next_cluster >= FAT12_EOF {
            break;
        }
        cluster = next_cluster;
    }

    entries[index].name[0] = 0xE5;
    write_root_directory(&entries)?;

    flush_fat()?;

    Ok(())
}

/// Lists all files in the root directory
///
/// # Returns
/// * `Ok(Vec<String>)` containing filenames if successful
/// * `Err` with error message if failed
pub fn fat12_list_files() -> Result<Vec<String>, &'static str> {
    let entries = fat12_read_root_directory()?;
    let mut files = Vec::new();

    for entry in entries {
        if let Ok(name) = entry.get_name() {
            files.push(name);
        }
    }

    Ok(files)
}

/// Checks if a file exists
///
/// # Arguments
/// * `filename` - Name of the file to check
///
/// # Returns
/// * `true` if file exists, `false` otherwise
pub fn fat12_file_exists(filename: &str) -> bool {
    find_file_entry(filename).is_ok()
}

/// Gets the size of a file
///
/// # Arguments
/// * `filename` - Name of the file
///
/// # Returns
/// * `Some(u32)` with file size if file exists
/// * `None` if file does not exist
pub fn fat12_get_file_size(filename: &str) -> Option<u32> {
    find_file_entry(filename).ok().map(|entry| entry.file_size)
}
