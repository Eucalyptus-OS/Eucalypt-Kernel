#![no_std]
//! FAT12 Filesystem Driver
//! Supports reading, writing and creating files on FAT12 formatted drives
extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicBool, Ordering};
use ide::{ide_read_sectors, ide_write_sectors};
use framebuffer::println;

const SECTOR_SIZE: usize = 512;
const FAT12_EOF: u16 = 0xFF8;
const FAT12_BAD_CLUSTER: u16 = 0xFF7;
const FAT12_FREE_CLUSTER: u16 = 0x000;
const DIR_ENTRY_DELETED: u8 = 0xE5;
const DIR_ENTRY_END: u8 = 0x00;
const ATTR_DIRECTORY: u8 = 0x10;
const ATTR_VOLUME_ID: u8 = 0x08;
const ATTR_LFN: u8 = 0x0F;
const ATTR_ARCHIVE: u8 = 0x20;

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

/// Raw 32-byte FAT12 directory entry.
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
    pub fn is_empty(&self) -> bool { self.name[0] == DIR_ENTRY_END }
    pub fn is_deleted(&self) -> bool { self.name[0] == DIR_ENTRY_DELETED }
    pub fn is_lfn(&self) -> bool { self.attributes == ATTR_LFN }
    pub fn is_directory(&self) -> bool { (self.attributes & ATTR_DIRECTORY) != 0 }
    pub fn is_volume_id(&self) -> bool { (self.attributes & ATTR_VOLUME_ID) != 0 }

    /// Decodes the 8.3 name into a human-readable string e.g. "README.TXT".
    pub fn get_name(&self) -> Result<String, &'static str> {
        if self.is_empty() || self.is_deleted() || self.is_lfn() || self.is_volume_id() {
            return Err("Invalid entry");
        }
        let mut name = String::new();
        for i in 0..8 {
            if self.name[i] == b' ' { break; }
            name.push(self.name[i] as char);
        }
        let mut has_ext = false;
        for i in 8..11 {
            if self.name[i] != b' ' {
                if !has_ext { name.push('.'); has_ext = true; }
                name.push(self.name[i] as char);
            }
        }
        Ok(name)
    }

    /// Encodes an 8.3 filename string into the 11-byte name field.
    pub fn set_name(&mut self, filename: &str) -> Result<(), &'static str> {
        let parts: Vec<&str> = filename.split('.').collect();
        let (name_part, ext_part) = match parts.len() {
            2 => (parts[0], parts[1]),
            1 => (parts[0], ""),
            _ => return Err("Invalid filename format"),
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

    /// Creates a new file directory entry.
    pub fn new_file(name: &str, first_cluster: u16, size: u32) -> Result<Self, &'static str> {
        let mut entry = DirectoryEntry {
            name: [b' '; 11],
            attributes: ATTR_ARCHIVE,
            reserved: 0, creation_time_tenth: 0, creation_time: 0,
            creation_date: 0, last_access_date: 0, first_cluster_high: 0,
            last_mod_time: 0, last_mod_date: 0, first_cluster, file_size: size,
        };
        entry.set_name(name)?;
        Ok(entry)
    }

    /// Creates a new directory entry for a subdirectory.
    pub fn new_directory(name: &str, first_cluster: u16) -> Result<Self, &'static str> {
        let mut entry = DirectoryEntry {
            name: [b' '; 11],
            attributes: ATTR_DIRECTORY,
            reserved: 0, creation_time_tenth: 0, creation_time: 0,
            creation_date: 0, last_access_date: 0, first_cluster_high: 0,
            last_mod_time: 0, last_mod_date: 0, first_cluster, file_size: 0,
        };
        entry.set_name(name)?;
        Ok(entry)
    }
}

// unlocked cluster I/O — callers must hold fat_lock

fn cluster_to_sector(cluster: u16) -> u64 {
    unsafe { DATA_START_SECTOR + ((cluster as u64 - 2) * BPB.sectors_per_cluster as u64) }
}

fn read_cluster_unlocked(cluster: u16) -> Result<Vec<u8>, &'static str> {
    unsafe {
        let sector = cluster_to_sector(cluster);
        let cluster_size = BPB.sectors_per_cluster as usize * SECTOR_SIZE;
        let mut data = alloc::vec![0u8; cluster_size];
        for i in 0..BPB.sectors_per_cluster {
            let s = &mut data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            if ide_read_sectors(FAT_DRIVE, sector + i as u64, s) != 0 {
                return Err("Failed to read cluster");
            }
        }
        Ok(data)
    }
}

fn write_cluster_unlocked(cluster: u16, data: &[u8]) -> Result<(), &'static str> {
    unsafe {
        let sector = cluster_to_sector(cluster);
        let cluster_size = BPB.sectors_per_cluster as usize * SECTOR_SIZE;
        let copy_len = core::cmp::min(data.len(), cluster_size);
        let mut padded = alloc::vec![0u8; cluster_size];
        padded[..copy_len].copy_from_slice(&data[..copy_len]);
        for i in 0..BPB.sectors_per_cluster {
            let s = &padded[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            if ide_write_sectors(FAT_DRIVE, sector + i as u64, s) != 0 {
                return Err("Failed to write cluster");
            }
        }
        Ok(())
    }
}

fn read_root_directory_unlocked() -> Result<Vec<DirectoryEntry>, &'static str> {
    unsafe {
        let root_size = ROOT_DIR_SECTORS as usize * SECTOR_SIZE;
        let mut root_data = alloc::vec![0u8; root_size];
        for i in 0..ROOT_DIR_SECTORS {
            let s = &mut root_data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            if ide_read_sectors(FAT_DRIVE, ROOT_DIR_START_SECTOR + i as u64, s) != 0 {
                return Err("Failed to read root directory");
            }
        }
        let mut entries = Vec::new();
        for i in 0..BPB.root_entry_count as usize {
            let entry = *(root_data.as_ptr().add(i * 32) as *const DirectoryEntry);
            if entry.is_empty() { break; }
            if !entry.is_deleted() && !entry.is_lfn() && !entry.is_volume_id() {
                entries.push(entry);
            }
        }
        Ok(entries)
    }
}

fn write_root_directory_unlocked(entries: &[DirectoryEntry]) -> Result<(), &'static str> {
    unsafe {
        let root_size = ROOT_DIR_SECTORS as usize * SECTOR_SIZE;
        let mut root_data = alloc::vec![0u8; root_size];
        for (i, entry) in entries.iter().enumerate() {
            *(root_data.as_mut_ptr().add(i * 32) as *mut DirectoryEntry) = *entry;
        }
        for i in 0..ROOT_DIR_SECTORS {
            let s = &root_data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            if ide_write_sectors(FAT_DRIVE, ROOT_DIR_START_SECTOR + i as u64, s) != 0 {
                return Err("Failed to write root directory");
            }
        }
        Ok(())
    }
}

// writes the in-memory FAT cache to all FAT copies on disk
fn flush_fat_unlocked() -> Result<(), &'static str> {
    unsafe {
        for i in 0..BPB.fat_size_16 {
            let s = &FAT_CACHE[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            if ide_write_sectors(FAT_DRIVE, FAT_START_SECTOR + i as u64, s) != 0 {
                return Err("Failed to write FAT");
            }
        }
        for fat_num in 1..BPB.num_fats {
            let backup_start = FAT_START_SECTOR + (fat_num as u64 * BPB.fat_size_16 as u64);
            for i in 0..BPB.fat_size_16 {
                let s = &FAT_CACHE[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
                if ide_write_sectors(FAT_DRIVE, backup_start + i as u64, s) != 0 {
                    return Err("Failed to write backup FAT");
                }
            }
        }
        Ok(())
    }
}

// FAT entry read/write — operates on the in-memory cache only

fn get_fat_entry(cluster: u16) -> u16 {
    unsafe {
        let off = (cluster as usize * 3) / 2;
        let val = u16::from_le_bytes([FAT_CACHE[off], FAT_CACHE[off + 1]]);
        if cluster & 1 == 0 { val & 0x0FFF } else { val >> 4 }
    }
}

fn set_fat_entry(cluster: u16, value: u16) {
    unsafe {
        let off = (cluster as usize * 3) / 2;
        if cluster & 1 == 0 {
            FAT_CACHE[off] = (value & 0xFF) as u8;
            FAT_CACHE[off + 1] = (FAT_CACHE[off + 1] & 0xF0) | ((value >> 8) & 0x0F) as u8;
        } else {
            FAT_CACHE[off] = (FAT_CACHE[off] & 0x0F) | ((value & 0x0F) << 4) as u8;
            FAT_CACHE[off + 1] = ((value >> 4) & 0xFF) as u8;
        }
    }
}

fn allocate_cluster() -> Result<u16, &'static str> {
    for cluster in 2..0xFF0u16 {
        if get_fat_entry(cluster) == FAT12_FREE_CLUSTER {
            set_fat_entry(cluster, FAT12_EOF);
            return Ok(cluster);
        }
    }
    Err("No free clusters")
}

fn free_cluster_chain(first_cluster: u16) {
    let mut cluster = first_cluster;
    while cluster >= 2 && cluster < FAT12_BAD_CLUSTER {
        let next = get_fat_entry(cluster);
        set_fat_entry(cluster, FAT12_FREE_CLUSTER);
        if next >= FAT12_EOF { break; }
        cluster = next;
    }
}

fn count_free_clusters() -> u16 {
    (2..0xFF0u16).filter(|&c| get_fat_entry(c) == FAT12_FREE_CLUSTER).count() as u16
}

// directory search helpers — unlocked, used inside locked public functions

fn find_entry_unlocked(filename: &str) -> Result<DirectoryEntry, &'static str> {
    for entry in read_root_directory_unlocked()? {
        if let Ok(name) = entry.get_name() {
            if name.eq_ignore_ascii_case(filename) { return Ok(entry); }
        }
    }
    Err("File not found")
}

fn find_entry_with_index_unlocked(filename: &str) -> Result<(usize, DirectoryEntry), &'static str> {
    for (i, entry) in read_root_directory_unlocked()?.iter().enumerate() {
        if let Ok(name) = entry.get_name() {
            if name.eq_ignore_ascii_case(filename) { return Ok((i, *entry)); }
        }
    }
    Err("File not found")
}

fn read_subdirectory_unlocked(first_cluster: u16) -> Result<Vec<DirectoryEntry>, &'static str> {
    let mut entries = Vec::new();
    let mut cluster = first_cluster;
    loop {
        if cluster < 2 || cluster >= FAT12_BAD_CLUSTER { break; }
        let data = read_cluster_unlocked(cluster)?;
        for i in 0..data.len() / 32 {
            let entry = unsafe { *(data.as_ptr().add(i * 32) as *const DirectoryEntry) };
            if entry.is_empty() { return Ok(entries); }
            if !entry.is_deleted() && !entry.is_lfn() && !entry.is_volume_id() {
                entries.push(entry);
            }
        }
        let next = get_fat_entry(cluster);
        if next >= FAT12_EOF { break; }
        cluster = next;
    }
    Ok(entries)
}

#[allow(unused)]
fn write_subdirectory_unlocked(first_cluster: u16, entries: &[DirectoryEntry]) -> Result<(), &'static str> {
    unsafe {
        let cluster_size = BPB.sectors_per_cluster as usize * SECTOR_SIZE;
        let entries_per_cluster = cluster_size / 32;
        // +1 for the end-of-directory marker
        let clusters_needed = (entries.len() + 1 + entries_per_cluster - 1) / entries_per_cluster;
        let mut dir_data = alloc::vec![0u8; clusters_needed * cluster_size];
        for (i, entry) in entries.iter().enumerate() {
            *(dir_data.as_mut_ptr().add(i * 32) as *mut DirectoryEntry) = *entry;
        }
        let mut cluster = first_cluster;
        let mut chunk = 0usize;
        loop {
            let off = chunk * cluster_size;
            write_cluster_unlocked(cluster, &dir_data[off..off + cluster_size])?;
            chunk += 1;
            let next = get_fat_entry(cluster);
            if chunk >= clusters_needed {
                if next < FAT12_EOF { free_cluster_chain(next); }
                set_fat_entry(cluster, FAT12_EOF);
                break;
            }
            if next >= FAT12_EOF {
                let new_cluster = allocate_cluster()?;
                set_fat_entry(cluster, new_cluster);
                cluster = new_cluster;
            } else {
                cluster = next;
            }
        }
        Ok(())
    }
}

// helper used by create and write to allocate + fill a cluster chain
fn allocate_and_write_chain(data: &[u8]) -> Result<Vec<u16>, &'static str> {
    unsafe {
        let cluster_size = BPB.sectors_per_cluster as usize * SECTOR_SIZE;
        let num_clusters = (data.len() + cluster_size - 1) / cluster_size;
        let mut clusters = Vec::new();
        for _ in 0..num_clusters { clusters.push(allocate_cluster()?); }
        for i in 0..clusters.len() - 1 { set_fat_entry(clusters[i], clusters[i + 1]); }
        set_fat_entry(clusters[clusters.len() - 1], FAT12_EOF);
        for (i, &cluster) in clusters.iter().enumerate() {
            let off = i * cluster_size;
            let end = core::cmp::min(off + cluster_size, data.len());
            write_cluster_unlocked(cluster, &data[off..end])?;
        }
        Ok(clusters)
    }
}

/// Reads the BPB, loads the FAT into cache, and computes region offsets.
pub fn fat12_init(drive: usize) -> Result<(), &'static str> {
    fat_lock();
    let result = (|| {
        println!("FAT12: Reading boot sector from drive {}...", drive);
        let mut boot_sector = [0u8; SECTOR_SIZE];
        let err = ide_read_sectors(drive, 0, &mut boot_sector);
        println!("FAT12: Boot sector read returned error code: {}", err);
        if err != 0 { return Err("Failed to read boot sector"); }

        let bpb = unsafe { *(boot_sector.as_ptr() as *const BiosParameterBlock) };
        let bytes_per_sector = bpb.bytes_per_sector;
        let fs_type = bpb.fs_type;
        println!("Boot sector signature: 0x{:02x}{:02x}", boot_sector[510], boot_sector[511]);
        println!("Bytes per sector: {}", bytes_per_sector);
        println!("FS Type: {:?}", core::str::from_utf8(&fs_type));

        if boot_sector[510] != 0x55 || boot_sector[511] != 0xAA {
            return Err("Invalid boot sector signature");
        }
        if &bpb.fs_type[0..5] != b"FAT12"
            && !(bpb.bytes_per_sector == 512 && bpb.sectors_per_cluster > 0)
        {
            return Err("Not a FAT12 filesystem");
        }

        let fat_start = bpb.reserved_sectors as u64;
        let root_dir_sectors = ((bpb.root_entry_count as u32 * 32)
            + (bpb.bytes_per_sector as u32 - 1))
            / bpb.bytes_per_sector as u32;
        let root_dir_start = fat_start + (bpb.num_fats as u64 * bpb.fat_size_16 as u64);
        let data_start = root_dir_start + root_dir_sectors as u64;

        let fat_size_bytes = bpb.fat_size_16 as usize * SECTOR_SIZE;
        let mut fat_cache = alloc::vec![0u8; fat_size_bytes];
        for i in 0..bpb.fat_size_16 {
            let s = &mut fat_cache[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            if ide_read_sectors(drive, fat_start + i as u64, s) != 0 {
                return Err("Failed to read FAT");
            }
        }

        unsafe {
            FAT_DRIVE = drive;
            BPB = bpb;
            FAT_START_SECTOR = fat_start;
            ROOT_DIR_START_SECTOR = root_dir_start;
            DATA_START_SECTOR = data_start;
            ROOT_DIR_SECTORS = root_dir_sectors;
            FAT_CACHE = fat_cache;
            FAT_INITIALIZED = true;
        }
        Ok(())
    })();
    fat_unlock();
    result
}

/// Reads a file by name and returns its raw contents.
pub fn fat12_read_file(filename: &str) -> Result<Vec<u8>, &'static str> {
    fat_lock();
    let result = (|| {
        let entry = find_entry_unlocked(filename)?;
        if entry.file_size == 0 { return Ok(Vec::new()); }
        let mut data = Vec::new();
        let mut cluster = entry.first_cluster;
        loop {
            if cluster < 2 || cluster >= FAT12_BAD_CLUSTER { break; }
            data.extend_from_slice(&read_cluster_unlocked(cluster)?);
            cluster = get_fat_entry(cluster);
            if cluster >= FAT12_EOF { break; }
        }
        data.truncate(entry.file_size as usize);
        Ok(data)
    })();
    fat_unlock();
    result
}

/// Creates a new file with the given contents. Fails if it already exists.
pub fn fat12_create_file(filename: &str, data: &[u8]) -> Result<(), &'static str> {
    fat_lock();
    let result = (|| unsafe {
        if !FAT_INITIALIZED { return Err("FAT12 not initialized"); }
        if data.is_empty() { return Err("Cannot create empty file"); }

        let clusters = allocate_and_write_chain(data)?;
        let mut entries = read_root_directory_unlocked()?;
        for e in &entries {
            if let Ok(name) = e.get_name() {
                if name.eq_ignore_ascii_case(filename) { return Err("File already exists"); }
            }
        }
        entries.push(DirectoryEntry::new_file(filename, clusters[0], data.len() as u32)?);
        write_root_directory_unlocked(&entries)?;
        flush_fat_unlocked()
    })();
    fat_unlock();
    result
}

/// Overwrites an existing file's contents. Fails if it doesn't exist.
pub fn fat12_write_file(filename: &str, data: &[u8]) -> Result<(), &'static str> {
    fat_lock();
    let result = (|| unsafe {
        if !FAT_INITIALIZED { return Err("FAT12 not initialized"); }

        let (index, old_entry) = find_entry_with_index_unlocked(filename)?;
        free_cluster_chain(old_entry.first_cluster);

        let mut entries = read_root_directory_unlocked()?;

        if data.is_empty() {
            // truncate to zero
            entries[index].first_cluster = 0;
            entries[index].file_size = 0;
            write_root_directory_unlocked(&entries)?;
            return flush_fat_unlocked();
        }

        let clusters = allocate_and_write_chain(data)?;
        entries[index].first_cluster = clusters[0];
        entries[index].file_size = data.len() as u32;
        write_root_directory_unlocked(&entries)?;
        flush_fat_unlocked()
    })();
    fat_unlock();
    result
}

/// Appends data to an existing file by reading, concatenating, and rewriting.
pub fn fat12_append_file(filename: &str, data: &[u8]) -> Result<(), &'static str> {
    // read + write each take the lock internally so don't nest
    let mut existing = fat12_read_file(filename)?;
    existing.extend_from_slice(data);
    fat12_write_file(filename, &existing)
}

/// Renames a file in the root directory without touching data clusters.
pub fn fat12_rename_file(old_name: &str, new_name: &str) -> Result<(), &'static str> {
    fat_lock();
    let result = (|| unsafe {
        if !FAT_INITIALIZED { return Err("FAT12 not initialized"); }
        let mut entries = read_root_directory_unlocked()?;
        // reject if destination already exists
        for e in &entries {
            if let Ok(name) = e.get_name() {
                if name.eq_ignore_ascii_case(new_name) {
                    return Err("Destination filename already exists");
                }
            }
        }
        let (index, _) = find_entry_with_index_unlocked(old_name)?;
        entries[index].set_name(new_name)?;
        write_root_directory_unlocked(&entries)
    })();
    fat_unlock();
    result
}

/// Marks a file deleted and frees its cluster chain.
pub fn fat12_delete_file(filename: &str) -> Result<(), &'static str> {
    fat_lock();
    let result = (|| {
        let (index, entry) = find_entry_with_index_unlocked(filename)?;
        free_cluster_chain(entry.first_cluster);
        let mut entries = read_root_directory_unlocked()?;
        entries[index].name[0] = DIR_ENTRY_DELETED;
        write_root_directory_unlocked(&entries)?;
        flush_fat_unlocked()
    })();
    fat_unlock();
    result
}

/// Creates a subdirectory with the mandatory `.` and `..` entries.
pub fn fat12_create_directory(dirname: &str) -> Result<(), &'static str> {
    fat_lock();
    let result = (|| unsafe {
        if !FAT_INITIALIZED { return Err("FAT12 not initialized"); }

        let mut entries = read_root_directory_unlocked()?;
        for e in &entries {
            if let Ok(name) = e.get_name() {
                if name.eq_ignore_ascii_case(dirname) { return Err("Directory already exists"); }
            }
        }

        let cluster = allocate_cluster()?;
        let cluster_size = BPB.sectors_per_cluster as usize * SECTOR_SIZE;
        let mut dir_data = alloc::vec![0u8; cluster_size];

        let dot = DirectoryEntry {
            name: *b".          ", attributes: ATTR_DIRECTORY,
            reserved: 0, creation_time_tenth: 0, creation_time: 0,
            creation_date: 0, last_access_date: 0, first_cluster_high: 0,
            last_mod_time: 0, last_mod_date: 0, first_cluster: cluster, file_size: 0,
        };
        let dotdot = DirectoryEntry {
            name: *b"..         ", attributes: ATTR_DIRECTORY,
            reserved: 0, creation_time_tenth: 0, creation_time: 0,
            creation_date: 0, last_access_date: 0, first_cluster_high: 0,
            last_mod_time: 0, last_mod_date: 0,
            first_cluster: 0, // 0 = root on FAT12
            file_size: 0,
        };

        let ptr = dir_data.as_mut_ptr() as *mut DirectoryEntry;
        *ptr = dot;
        *(ptr.add(1)) = dotdot;

        write_cluster_unlocked(cluster, &dir_data)?;

        entries.push(DirectoryEntry::new_directory(dirname, cluster)?);
        write_root_directory_unlocked(&entries)?;
        flush_fat_unlocked()
    })();
    fat_unlock();
    result
}

/// Deletes an empty subdirectory. Fails if it contains any entries.
pub fn fat12_delete_directory(dirname: &str) -> Result<(), &'static str> {
    fat_lock();
    let result = (|| {
        let (index, entry) = find_entry_with_index_unlocked(dirname)?;
        if !entry.is_directory() { return Err("Not a directory"); }

        let sub = read_subdirectory_unlocked(entry.first_cluster)?;
        let non_dot = sub.iter().filter(|e| {
            e.get_name().map(|n| n != "." && n != "..").unwrap_or(false)
        }).count();
        if non_dot > 0 { return Err("Directory not empty"); }

        free_cluster_chain(entry.first_cluster);
        let mut entries = read_root_directory_unlocked()?;
        entries[index].name[0] = DIR_ENTRY_DELETED;
        write_root_directory_unlocked(&entries)?;
        flush_fat_unlocked()
    })();
    fat_unlock();
    result
}

/// Returns all visible directory entries from the root directory.
pub fn fat12_list_entries() -> Result<Vec<DirectoryEntry>, &'static str> {
    fat_lock();
    let result = read_root_directory_unlocked();
    fat_unlock();
    result
}

/// Returns all visible entries inside a named subdirectory.
pub fn fat12_list_directory(dirname: &str) -> Result<Vec<DirectoryEntry>, &'static str> {
    fat_lock();
    let result = (|| {
        let entry = find_entry_unlocked(dirname)?;
        if !entry.is_directory() { return Err("Not a directory"); }
        read_subdirectory_unlocked(entry.first_cluster)
    })();
    fat_unlock();
    result
}

/// Returns all visible filenames in the root directory.
pub fn fat12_list_files() -> Result<Vec<String>, &'static str> {
    fat_lock();
    let result = read_root_directory_unlocked().map(|entries| {
        entries.iter().filter_map(|e| e.get_name().ok()).collect()
    });
    fat_unlock();
    result
}

/// Returns true if a file or directory with the given name exists.
pub fn fat12_file_exists(filename: &str) -> bool {
    fat_lock();
    let result = find_entry_unlocked(filename).is_ok();
    fat_unlock();
    result
}

/// Returns the stored logical size of a file in bytes.
pub fn fat12_get_file_size(filename: &str) -> Option<u32> {
    fat_lock();
    let result = find_entry_unlocked(filename).ok().map(|e| e.file_size);
    fat_unlock();
    result
}

/// Returns the attributes byte of a named entry.
pub fn fat12_get_attributes(filename: &str) -> Option<u8> {
    fat_lock();
    let result = find_entry_unlocked(filename).ok().map(|e| e.attributes);
    fat_unlock();
    result
}

/// Returns (total_bytes, free_bytes) for the volume.
pub fn fat12_stat() -> (u64, u64) {
    fat_lock();
    let result = unsafe {
        let total_sectors = if BPB.total_sectors_16 != 0 {
            BPB.total_sectors_16 as u64
        } else {
            BPB.total_sectors_32 as u64
        };
        let total = total_sectors * SECTOR_SIZE as u64;
        let free = count_free_clusters() as u64
            * BPB.sectors_per_cluster as u64
            * SECTOR_SIZE as u64;
        (total, free)
    };
    fat_unlock();
    result
}