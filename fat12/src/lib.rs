#![no_std]
/// FAT12 Filesystem Driver - Static Function API
/// Supports reading, writing and creating files on FAT12 formatted drives

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
/// Marker byte placed in `name[0]` when a directory entry is deleted.
const DIR_ENTRY_DELETED: u8 = 0xE5;
/// Marker byte placed in `name[0]` of the first unused entry in a directory.
const DIR_ENTRY_END: u8 = 0x00;
/// Directory attribute flag.
const ATTR_DIRECTORY: u8 = 0x10;
/// Volume-ID attribute flag.
const ATTR_VOLUME_ID: u8 = 0x08;
/// Long-file-name pseudo-attribute value.
const ATTR_LFN: u8 = 0x0F;
/// Archive attribute flag (normal file).
const ATTR_ARCHIVE: u8 = 0x20;

// ---------------------------------------------------------------------------
// Globals / spinlock
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// On-disk structures
// ---------------------------------------------------------------------------

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

/// A raw 32-byte FAT directory entry.
///
/// Both the root directory and subdirectories consist of a linear array of
/// these entries.  The `name` field uses the classic 8.3 encoding (name in
/// bytes 0-7, extension in bytes 8-10, space-padded, all upper-case).
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
    /// Returns `true` if this entry marks the end of the directory (no more entries follow).
    pub fn is_empty(&self) -> bool {
        self.name[0] == DIR_ENTRY_END
    }

    /// Returns `true` if this entry has been deleted.
    pub fn is_deleted(&self) -> bool {
        self.name[0] == DIR_ENTRY_DELETED
    }

    /// Returns `true` if this entry is part of a Long File Name sequence.
    pub fn is_lfn(&self) -> bool {
        self.attributes == ATTR_LFN
    }

    /// Returns `true` if this entry represents a subdirectory.
    pub fn is_directory(&self) -> bool {
        (self.attributes & ATTR_DIRECTORY) != 0
    }

    /// Returns `true` if this entry is the volume-ID pseudo-entry.
    pub fn is_volume_id(&self) -> bool {
        (self.attributes & ATTR_VOLUME_ID) != 0
    }

    /// Decodes the 8.3 name into a human-readable `String` (e.g. `"README.TXT"`).
    ///
    /// Returns `Err` for entries that should not be shown to the user
    /// (deleted, LFN, volume-ID, or end-of-directory).
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

    /// Encodes an 8.3 filename string into the 11-byte `name` field.
    ///
    /// The filename must be in `"NAME"` or `"NAME.EXT"` format, with the
    /// base name at most 8 characters and the extension at most 3.
    /// Both parts are converted to upper-case automatically.
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

    /// Constructs a new directory entry for a regular file.
    ///
    /// # Arguments
    /// * `name` - 8.3 filename string.
    /// * `first_cluster` - Starting cluster of the file's data.
    /// * `size` - File size in bytes.
    pub fn new_file(name: &str, first_cluster: u16, size: u32) -> Result<Self, &'static str> {
        let mut entry = DirectoryEntry {
            name: [b' '; 11],
            attributes: ATTR_ARCHIVE,
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

    /// Constructs a new directory entry for a subdirectory.
    ///
    /// # Arguments
    /// * `name` - 8.3 directory name string.
    /// * `first_cluster` - Starting cluster of the directory.
    pub fn new_directory(name: &str, first_cluster: u16) -> Result<Self, &'static str> {
        let mut entry = DirectoryEntry {
            name: [b' '; 11],
            attributes: ATTR_DIRECTORY,
            reserved: 0,
            creation_time_tenth: 0,
            creation_time: 0,
            creation_date: 0,
            last_access_date: 0,
            first_cluster_high: 0,
            last_mod_time: 0,
            last_mod_date: 0,
            first_cluster,
            file_size: 0,
        };

        entry.set_name(name)?;
        Ok(entry)
    }
}

/// Initialises the FAT12 filesystem on the specified IDE drive.
///
/// Reads and validates the Boot Parameter Block, loads the entire FAT into an
/// in-memory cache, and computes the sector addresses of the FAT, root
/// directory, and data regions.
///
/// # Arguments
/// * `drive` - IDE drive index (0 = primary master, 1 = primary slave, …).
///
/// # Returns
/// * `Ok(())` on success.
/// * `Err(&str)` with a diagnostic message on failure.
pub fn fat12_init(drive: usize) -> Result<(), &'static str> {
    fat_lock();

    println!("FAT12: Reading boot sector from drive {}...", drive);

    let mut boot_sector = [0u8; SECTOR_SIZE];
    let err = ide_read_sectors(drive, 0, &mut boot_sector);

    println!("FAT12: Boot sector read returned error code: {}", err);

    if err != 0 {
        fat_unlock();
        return Err("Failed to read boot sector");
    }

    let bpb = unsafe { *(boot_sector.as_ptr() as *const BiosParameterBlock) };

    let boot_sig_0 = boot_sector[510];
    let boot_sig_1 = boot_sector[511];
    let bytes_per_sector = bpb.bytes_per_sector;
    let fs_type = bpb.fs_type;

    println!("Boot sector signature: 0x{:02x}{:02x}", boot_sig_0, boot_sig_1);
    println!("Bytes per sector: {}", bytes_per_sector);
    println!("FS Type: {:?}", core::str::from_utf8(&fs_type));

    if boot_sig_0 != 0x55 || boot_sig_1 != 0xAA {
        fat_unlock();
        return Err("Invalid boot sector signature");
    }

    let is_fat12 = &fs_type[0..5] == b"FAT12"
        || bytes_per_sector == 512 && bpb.sectors_per_cluster > 0;

    if !is_fat12 {
        fat_unlock();
        return Err("Not a FAT12 filesystem");
    }

    let fat_start_sector = bpb.reserved_sectors as u64;
    let root_dir_sectors = ((bpb.root_entry_count as u32 * 32)
        + (bpb.bytes_per_sector as u32 - 1))
        / bpb.bytes_per_sector as u32;
    let root_dir_start_sector =
        fat_start_sector + (bpb.num_fats as u64 * bpb.fat_size_16 as u64);
    let data_start_sector = root_dir_start_sector + root_dir_sectors as u64;

    let fat_size_bytes = bpb.fat_size_16 as usize * SECTOR_SIZE;
    let mut fat_cache = alloc::vec![0u8; fat_size_bytes];

    for i in 0..bpb.fat_size_16 {
        let sector_data =
            &mut fat_cache[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
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

/// Returns the 12-bit FAT entry for `cluster`.
fn get_fat_entry(cluster: u16) -> u16 {
    unsafe {
        let fat_offset = (cluster as usize * 3) / 2;
        if cluster & 1 == 0 {
            u16::from_le_bytes([FAT_CACHE[fat_offset], FAT_CACHE[fat_offset + 1]]) & 0x0FFF
        } else {
            u16::from_le_bytes([FAT_CACHE[fat_offset], FAT_CACHE[fat_offset + 1]]) >> 4
        }
    }
}

/// Writes a 12-bit value into the FAT cache at the slot for `cluster`.
fn set_fat_entry(cluster: u16, value: u16) {
    unsafe {
        let fat_offset = (cluster as usize * 3) / 2;

        if cluster & 1 == 0 {
            FAT_CACHE[fat_offset] = (value & 0xFF) as u8;
            FAT_CACHE[fat_offset + 1] =
                (FAT_CACHE[fat_offset + 1] & 0xF0) | ((value >> 8) & 0x0F) as u8;
        } else {
            FAT_CACHE[fat_offset] =
                (FAT_CACHE[fat_offset] & 0x0F) | ((value & 0x0F) << 4) as u8;
            FAT_CACHE[fat_offset + 1] = ((value >> 4) & 0xFF) as u8;
        }
    }
}

/// Writes the in-memory FAT cache to all FAT copies on disk.
fn flush_fat() -> Result<(), &'static str> {
    fat_lock();

    unsafe {
        for i in 0..BPB.fat_size_16 {
            let sector_data =
                &FAT_CACHE[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            let err = ide_write_sectors(FAT_DRIVE, FAT_START_SECTOR + i as u64, sector_data);
            if err != 0 {
                fat_unlock();
                return Err("Failed to write FAT");
            }
        }

        for fat_num in 1..BPB.num_fats {
            let backup_start =
                FAT_START_SECTOR + (fat_num as u64 * BPB.fat_size_16 as u64);
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

/// Scans the FAT cache for the first cluster marked as free.
///
/// Returns `None` when the disk is full.
fn find_free_cluster() -> Option<u16> {
    for cluster in 2..0xFF0 {
        if get_fat_entry(cluster) == FAT12_FREE_CLUSTER {
            return Some(cluster);
        }
    }
    None
}

/// Allocates a single free cluster and marks it as EOF in the FAT cache.
fn allocate_cluster() -> Result<u16, &'static str> {
    let cluster = find_free_cluster().ok_or("No free clusters")?;
    set_fat_entry(cluster, FAT12_EOF);
    Ok(cluster)
}

/// Frees the entire cluster chain starting at `first_cluster`.
///
/// Each cluster in the chain has its FAT entry set to `FAT12_FREE_CLUSTER`.
fn free_cluster_chain(first_cluster: u16) {
    let mut cluster = first_cluster;
    while cluster >= 2 && cluster < FAT12_BAD_CLUSTER {
        let next = get_fat_entry(cluster);
        set_fat_entry(cluster, FAT12_FREE_CLUSTER);
        if next >= FAT12_EOF {
            break;
        }
        cluster = next;
    }
}

/// Returns the number of free clusters on the volume.
///
/// Useful for reporting available space (`free_clusters * sectors_per_cluster * 512`).
fn count_free_clusters() -> u16 {
    let mut count = 0u16;
    for cluster in 2..0xFF0u16 {
        if get_fat_entry(cluster) == FAT12_FREE_CLUSTER {
            count += 1;
        }
    }
    count
}

/// Converts a cluster number into the LBA sector address of its first sector.
fn cluster_to_sector(cluster: u16) -> u64 {
    unsafe { DATA_START_SECTOR + ((cluster as u64 - 2) * BPB.sectors_per_cluster as u64) }
}

/// Reads all sectors belonging to `cluster` and returns the raw bytes.
fn read_cluster(cluster: u16) -> Result<Vec<u8>, &'static str> {
    fat_lock();

    unsafe {
        let sector = cluster_to_sector(cluster);
        let cluster_size = BPB.sectors_per_cluster as usize * SECTOR_SIZE;
        let mut data = alloc::vec![0u8; cluster_size];

        for i in 0..BPB.sectors_per_cluster {
            let sector_data =
                &mut data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
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

/// Writes `data` into `cluster`, zero-padding to fill the cluster if needed.
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

// ---------------------------------------------------------------------------
// Root directory helpers
// ---------------------------------------------------------------------------

/// Reads all valid (non-deleted, non-LFN) entries from the root directory.
fn fat12_read_root_directory() -> Result<Vec<DirectoryEntry>, &'static str> {
    fat_lock();

    unsafe {
        let root_size = ROOT_DIR_SECTORS as usize * SECTOR_SIZE;
        let mut root_data = alloc::vec![0u8; root_size];

        for i in 0..ROOT_DIR_SECTORS {
            let sector_data =
                &mut root_data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            let err =
                ide_read_sectors(FAT_DRIVE, ROOT_DIR_START_SECTOR + i as u64, sector_data);
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

/// Reads the *raw* root directory sector data, including deleted and LFN entries.
///
/// Used internally when we need to find a free slot (deleted entry) without
/// filtering the list.
fn read_raw_root_directory_data() -> Result<Vec<u8>, &'static str> {
    fat_lock();

    unsafe {
        let root_size = ROOT_DIR_SECTORS as usize * SECTOR_SIZE;
        let mut root_data = alloc::vec![0u8; root_size];

        for i in 0..ROOT_DIR_SECTORS {
            let sector_data =
                &mut root_data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            let err =
                ide_read_sectors(FAT_DRIVE, ROOT_DIR_START_SECTOR + i as u64, sector_data);
            if err != 0 {
                fat_unlock();
                return Err("Failed to read root directory");
            }
        }

        fat_unlock();
        Ok(root_data)
    }
}

/// Overwrites the entire root directory region with the provided entries.
///
/// Entries not covered by `entries` are zeroed, which terminates the
/// directory correctly.
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
            let err =
                ide_write_sectors(FAT_DRIVE, ROOT_DIR_START_SECTOR + i as u64, sector_data);
            if err != 0 {
                fat_unlock();
                return Err("Failed to write root directory");
            }
        }

        fat_unlock();
        Ok(())
    }
}

/// Searches the root directory for an entry whose decoded name matches
/// `filename` (case-insensitive).
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

/// Same as `find_file_entry` but also returns the index of the entry within
/// the root directory array (needed for in-place updates).
fn find_file_entry_with_index(filename: &str) -> Result<(usize, DirectoryEntry), &'static str> {
    let entries = fat12_read_root_directory()?;

    for (i, entry) in entries.iter().enumerate() {
        if let Ok(name) = entry.get_name() {
            if name.to_uppercase() == filename.to_uppercase() {
                return Ok((i, *entry));
            }
        }
    }

    Err("File not found")
}

/// Reads all valid entries from a subdirectory whose first cluster is
/// `first_cluster`.
///
/// Follows the cluster chain until EOF. Returns only non-deleted, non-LFN,
/// non-volume-ID entries.
fn read_subdirectory(first_cluster: u16) -> Result<Vec<DirectoryEntry>, &'static str> {
    let mut entries = Vec::new();
    let mut cluster = first_cluster;

    loop {
        if cluster < 2 || cluster >= FAT12_BAD_CLUSTER {
            break;
        }

        let data = read_cluster(cluster)?;
        let num_entries = data.len() / 32;

        for i in 0..num_entries {
            let offset = i * 32;
            let entry =
                unsafe { *(data.as_ptr().add(offset) as *const DirectoryEntry) };

            if entry.is_empty() {
                return Ok(entries); // end-of-directory marker
            }

            if !entry.is_deleted() && !entry.is_lfn() && !entry.is_volume_id() {
                entries.push(entry);
            }
        }

        let next = get_fat_entry(cluster);
        if next >= FAT12_EOF {
            break;
        }
        cluster = next;
    }

    Ok(entries)
}

/// Writes a complete set of entries back to a subdirectory cluster chain.
///
/// If the entries no longer fit in the existing chain, new clusters are
/// allocated automatically. Any surplus clusters at the end of the old chain
/// are freed.
fn write_subdirectory(first_cluster: u16, entries: &[DirectoryEntry]) -> Result<(), &'static str> {
    unsafe {
        let cluster_size = BPB.sectors_per_cluster as usize * SECTOR_SIZE;
        let entries_per_cluster = cluster_size / 32;

        // Build a flat byte buffer for all entries + a terminating zero entry.
        let total_entries = entries.len() + 1; // +1 for end-of-dir marker
        let clusters_needed = (total_entries + entries_per_cluster - 1) / entries_per_cluster;

        let mut dir_data = alloc::vec![0u8; clusters_needed * cluster_size];

        for (i, entry) in entries.iter().enumerate() {
            let offset = i * 32;
            let entry_ptr = dir_data.as_mut_ptr().add(offset) as *mut DirectoryEntry;
            *entry_ptr = *entry;
        }

        // Walk the existing cluster chain, writing data and freeing excess.
        let mut cluster = first_cluster;
        let mut chunk_index = 0usize;

        loop {
            let offset = chunk_index * cluster_size;
            write_cluster(cluster, &dir_data[offset..offset + cluster_size])?;

            chunk_index += 1;
            let next = get_fat_entry(cluster);

            if chunk_index >= clusters_needed {
                // Free any remaining clusters in the old chain.
                if next < FAT12_EOF {
                    free_cluster_chain(next);
                }
                set_fat_entry(cluster, FAT12_EOF);
                break;
            }

            if next >= FAT12_EOF {
                // Need more clusters than currently allocated.
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

/// Reads a file from the root directory by name.
///
/// Follows the cluster chain recorded in the directory entry and assembles the
/// raw file bytes, truncating to the stored `file_size`.
///
/// # Arguments
/// * `filename` - 8.3 filename string (case-insensitive).
///
/// # Returns
/// * `Ok(Vec<u8>)` containing the complete file contents.
/// * `Err(&str)` if the file does not exist or an I/O error occurs.
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

/// Returns all visible directory entries in the root directory.
///
/// Unlike [`fat12_list_files`] this returns the full `DirectoryEntry` structs,
/// which include attributes, first cluster, and file size.
///
/// # Returns
/// * `Ok(Vec<DirectoryEntry>)` on success.
/// * `Err(&str)` if an I/O error occurs.
pub fn fat12_list_entries() -> Result<Vec<DirectoryEntry>, &'static str> {
    fat12_read_root_directory()
}

/// Returns all visible entries inside a named subdirectory.
///
/// # Arguments
/// * `dirname` - 8.3 name of the subdirectory (case-insensitive).
///
/// # Returns
/// * `Ok(Vec<DirectoryEntry>)` on success.
/// * `Err(&str)` if the directory does not exist or an I/O error occurs.
pub fn fat12_list_directory(dirname: &str) -> Result<Vec<DirectoryEntry>, &'static str> {
    let entry = find_file_entry(dirname)?;

    if !entry.is_directory() {
        return Err("Not a directory");
    }

    read_subdirectory(entry.first_cluster)
}

/// Returns the number of free bytes available on the volume.
///
/// Computed as `free_clusters × sectors_per_cluster × SECTOR_SIZE`.
pub fn fat12_free_bytes() -> u64 {
    unsafe {
        count_free_clusters() as u64
            * BPB.sectors_per_cluster as u64
            * SECTOR_SIZE as u64
    }
}

/// Creates a new file in the root directory with the supplied contents.
///
/// Allocates as many clusters as needed, writes the data, creates a directory
/// entry, and flushes both the FAT and the root directory to disk.
///
/// # Arguments
/// * `filename` - 8.3 filename string (case-insensitive).
/// * `data` - Raw bytes to write as the file's contents.
///
/// # Errors
/// Returns `Err` if the FAT is not initialised, the disk is full, the
/// filename is already in use, or any I/O operation fails.
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

/// Overwrites an existing file's contents in-place.
///
/// The old cluster chain is freed, a new chain is allocated for the new data,
/// and the directory entry's `first_cluster` and `file_size` fields are updated.
/// If the file does not exist, `Err("File not found")` is returned — use
/// [`fat12_create_file`] to create new files.
///
/// # Arguments
/// * `filename` - 8.3 filename string (case-insensitive).
/// * `data` - New file contents.
///
/// # Errors
/// Returns `Err` if the file is not found, the disk is full, or any I/O
/// operation fails.
pub fn fat12_write_file(filename: &str, data: &[u8]) -> Result<(), &'static str> {
    unsafe {
        if !FAT_INITIALIZED {
            return Err("FAT12 not initialized");
        }

        let (index, old_entry) = find_file_entry_with_index(filename)?;

        // Free the existing cluster chain.
        free_cluster_chain(old_entry.first_cluster);

        let cluster_size = BPB.sectors_per_cluster as usize * SECTOR_SIZE;
        let num_clusters = (data.len() + cluster_size - 1) / cluster_size;

        if num_clusters == 0 {
            // Truncate to zero: update the directory entry with cluster 0 and size 0.
            let mut entries = fat12_read_root_directory()?;
            entries[index].first_cluster = 0;
            entries[index].file_size = 0;
            write_root_directory(&entries)?;
            flush_fat()?;
            return Ok(());
        }

        // Allocate fresh cluster chain.
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

        // Update the directory entry.
        let mut entries = fat12_read_root_directory()?;
        entries[index].first_cluster = clusters[0];
        entries[index].file_size = data.len() as u32;
        write_root_directory(&entries)?;

        flush_fat()?;
        Ok(())
    }
}

/// Appends `data` to the end of an existing file.
///
/// Reads the current file contents, concatenates `data`, and calls
/// [`fat12_write_file`] to persist the combined result.  For large files
/// this is a whole-file rewrite — if you need efficient append you should
/// extend this to walk to the last cluster directly.
///
/// # Arguments
/// * `filename` - 8.3 filename string (case-insensitive).
/// * `data` - Bytes to append.
///
/// # Errors
/// Returns `Err` if the file does not exist or any I/O operation fails.
pub fn fat12_append_file(filename: &str, data: &[u8]) -> Result<(), &'static str> {
    let mut existing = fat12_read_file(filename)?;
    existing.extend_from_slice(data);
    fat12_write_file(filename, &existing)
}

/// Renames a file in the root directory.
///
/// Updates the 8.3 name field of the directory entry in-place without
/// touching the data clusters or FAT.
///
/// # Arguments
/// * `old_name` - Current 8.3 filename (case-insensitive).
/// * `new_name` - New 8.3 filename.
///
/// # Errors
/// Returns `Err` if `old_name` is not found, `new_name` already exists, the
/// new name is invalid, or an I/O error occurs.
pub fn fat12_rename_file(old_name: &str, new_name: &str) -> Result<(), &'static str> {
    unsafe {
        if !FAT_INITIALIZED {
            return Err("FAT12 not initialized");
        }
    }

    // Reject if the target name already exists.
    if fat12_file_exists(new_name) {
        return Err("Destination filename already exists");
    }

    let (index, _) = find_file_entry_with_index(old_name)?;
    let mut entries = fat12_read_root_directory()?;
    entries[index].set_name(new_name)?;
    write_root_directory(&entries)
}

/// Deletes a file from the root directory.
///
/// Marks the directory entry as deleted (`0xE5`) and frees its cluster chain
/// in the FAT.
///
/// # Arguments
/// * `filename` - 8.3 filename string (case-insensitive).
///
/// # Returns
/// * `Ok(())` if the file was deleted.
/// * `Err(&str)` if the file does not exist or an I/O error occurs.
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

    free_cluster_chain(first_cluster);

    entries[index].name[0] = DIR_ENTRY_DELETED;
    write_root_directory(&entries)?;

    flush_fat()?;
    Ok(())
}

/// Creates a new subdirectory in the root directory.
///
/// Allocates one cluster for the new directory, writes the mandatory `.` and
/// `..` entries, and adds an entry in the root directory.
///
/// # Arguments
/// * `dirname` - 8.3 directory name string.
///
/// # Errors
/// Returns `Err` if the name already exists, the disk is full, or any I/O
/// operation fails.
pub fn fat12_create_directory(dirname: &str) -> Result<(), &'static str> {
    unsafe {
        if !FAT_INITIALIZED {
            return Err("FAT12 not initialized");
        }

        if fat12_file_exists(dirname) {
            return Err("Directory already exists");
        }

        let cluster = allocate_cluster()?;
        let cluster_size = BPB.sectors_per_cluster as usize * SECTOR_SIZE;
        let mut dir_data = alloc::vec![0u8; cluster_size];

        // Write the '.' self-reference entry.
        let dot_entry = DirectoryEntry {
            name: *b".          ",
            attributes: ATTR_DIRECTORY,
            reserved: 0,
            creation_time_tenth: 0,
            creation_time: 0,
            creation_date: 0,
            last_access_date: 0,
            first_cluster_high: 0,
            last_mod_time: 0,
            last_mod_date: 0,
            first_cluster: cluster,
            file_size: 0,
        };

        // Write the '..' parent reference entry (0 = root for FAT12).
        let dotdot_entry = DirectoryEntry {
            name: *b"..         ",
            attributes: ATTR_DIRECTORY,
            reserved: 0,
            creation_time_tenth: 0,
            creation_time: 0,
            creation_date: 0,
            last_access_date: 0,
            first_cluster_high: 0,
            last_mod_time: 0,
            last_mod_date: 0,
            first_cluster: 0, // 0 means root directory on FAT12
            file_size: 0,
        };

        let dot_ptr = dir_data.as_mut_ptr() as *mut DirectoryEntry;
        *dot_ptr = dot_entry;
        *(dot_ptr.add(1)) = dotdot_entry;

        write_cluster(cluster, &dir_data)?;

        // Add the entry in the root directory.
        let dir_entry = DirectoryEntry::new_directory(dirname, cluster)?;
        let mut entries = fat12_read_root_directory()?;
        entries.push(dir_entry);
        write_root_directory(&entries)?;

        flush_fat()?;
        Ok(())
    }
}

/// Deletes an empty subdirectory from the root directory.
///
/// Returns `Err` if the directory is not empty (contains entries other than
/// `.` and `..`).
///
/// # Arguments
/// * `dirname` - 8.3 directory name string (case-insensitive).
pub fn fat12_delete_directory(dirname: &str) -> Result<(), &'static str> {
    let (index, entry) = find_file_entry_with_index(dirname)?;

    if !entry.is_directory() {
        return Err("Not a directory");
    }

    // Verify the directory is empty (only . and .. are allowed).
    let sub_entries = read_subdirectory(entry.first_cluster)?;
    let real_entries: Vec<_> = sub_entries
        .iter()
        .filter(|e| {
            let n = e.get_name().unwrap_or_default();
            n != "." && n != ".."
        })
        .collect();

    if !real_entries.is_empty() {
        return Err("Directory not empty");
    }

    free_cluster_chain(entry.first_cluster);

    let mut entries = fat12_read_root_directory()?;
    entries[index].name[0] = DIR_ENTRY_DELETED;
    write_root_directory(&entries)?;

    flush_fat()?;
    Ok(())
}

/// Lists all visible filenames in the root directory.
///
/// # Returns
/// * `Ok(Vec<String>)` of 8.3 filename strings.
/// * `Err(&str)` if an I/O error occurs.
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

/// Returns `true` if a file or directory with `filename` exists in the root
/// directory.
pub fn fat12_file_exists(filename: &str) -> bool {
    find_file_entry(filename).is_ok()
}

/// Returns the stored size of a file in bytes, or `None` if not found.
///
/// Note: the stored size is the *logical* file size, not the number of bytes
/// allocated on disk (which is rounded up to a whole cluster).
pub fn fat12_get_file_size(filename: &str) -> Option<u32> {
    find_file_entry(filename).ok().map(|entry| entry.file_size)
}

/// Returns the attributes byte of a named entry, or `None` if not found.
///
/// You can test individual bits against the `ATTR_*` constants defined in this
/// module (e.g., `ATTR_DIRECTORY`, `ATTR_ARCHIVE`).
pub fn fat12_get_attributes(filename: &str) -> Option<u8> {
    find_file_entry(filename).ok().map(|entry| entry.attributes)
}

/// Returns filesystem summary information.
///
/// # Returns
/// A tuple of `(total_bytes, free_bytes)` where both values are `u64`.
pub fn fat12_stat() -> (u64, u64) {
    unsafe {
        let total_sectors = if BPB.total_sectors_16 != 0 {
            BPB.total_sectors_16 as u64
        } else {
            BPB.total_sectors_32 as u64
        };

        let total_bytes = total_sectors * SECTOR_SIZE as u64;
        let free_bytes_val = fat12_free_bytes();
        (total_bytes, free_bytes_val)
    }
}