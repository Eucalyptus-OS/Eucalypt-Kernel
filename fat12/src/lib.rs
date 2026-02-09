#![no_std]
//! FAT12 Filesystem Driver
//! Supports reading, writing, and creating files on FAT12 formatted drives

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
        
        // Get filename (first 8 bytes)
        for i in 0..8 {
            if self.name[i] == b' ' {
                break;
            }
            name.push(self.name[i] as char);
        }

        // Get extension (last 3 bytes)
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
        // Split filename and extension
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

        // Clear the name field
        self.name = [b' '; 11];

        // Set name part
        for (i, byte) in name_part.bytes().enumerate() {
            self.name[i] = byte.to_ascii_uppercase();
        }

        // Set extension part
        for (i, byte) in ext_part.bytes().enumerate() {
            self.name[8 + i] = byte.to_ascii_uppercase();
        }

        Ok(())
    }

    pub fn new_file(name: &str, first_cluster: u16, size: u32) -> Result<Self, &'static str> {
        let mut entry = DirectoryEntry {
            name: [b' '; 11],
            attributes: 0x20, // Archive attribute
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

pub struct Fat12FileSystem {
    drive: usize,
    bpb: BiosParameterBlock,
    fat_start_sector: u64,
    root_dir_start_sector: u64,
    data_start_sector: u64,
    root_dir_sectors: u32,
    fat_cache: Vec<u8>,
}

impl Fat12FileSystem {
    pub fn new(drive: usize) -> Result<Self, &'static str> {
        fat_lock();
        
        let mut boot_sector = [0u8; SECTOR_SIZE];
        let err = ide_read_sectors(drive, 0, &mut boot_sector);
        
        if err != 0 {
            fat_unlock();
            return Err("Failed to read boot sector");
        }

        let bpb = unsafe { *(boot_sector.as_ptr() as *const BiosParameterBlock) };

        let boot_sig_0 = boot_sector[510];
        let boot_sig_1 = boot_sector[511];
        let bytes_per_sector = bpb.bytes_per_sector;
        let fs_type = bpb.fs_type;
        
        serial_println!("Boot sector signature: 0x{:02x}{:02x}", boot_sig_0, boot_sig_1);
        serial_println!("Bytes per sector: {}", bytes_per_sector);
        serial_println!("FS Type: {:?}", core::str::from_utf8(&fs_type));

        if boot_sig_0 != 0x55 || boot_sig_1 != 0xAA {
            fat_unlock();
            return Err("Invalid boot sector signature");
        }

        let is_fat12 = &fs_type[0..5] == b"FAT12" || 
                       bytes_per_sector == 512 && bpb.sectors_per_cluster > 0;
        
        if !is_fat12 {
            fat_unlock();
            return Err("Not a FAT12 filesystem");
        }

        let fat_start_sector = bpb.reserved_sectors as u64;
        let root_dir_sectors = ((bpb.root_entry_count as u32 * 32) 
            + (bpb.bytes_per_sector as u32 - 1)) / bpb.bytes_per_sector as u32;
        let root_dir_start_sector = fat_start_sector + (bpb.num_fats as u64 * bpb.fat_size_16 as u64);
        let data_start_sector = root_dir_start_sector + root_dir_sectors as u64;

        // Load FAT into cache
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

        fat_unlock();

        Ok(Fat12FileSystem {
            drive,
            bpb,
            fat_start_sector,
            root_dir_start_sector,
            data_start_sector,
            root_dir_sectors,
            fat_cache,
        })
    }

    fn get_fat_entry(&self, cluster: u16) -> u16 {
        let fat_offset = (cluster as usize * 3) / 2;
        let entry = if cluster & 1 == 0 {
            // Even cluster
            u16::from_le_bytes([
                self.fat_cache[fat_offset],
                self.fat_cache[fat_offset + 1],
            ]) & 0x0FFF
        } else {
            // Odd cluster
            u16::from_le_bytes([
                self.fat_cache[fat_offset],
                self.fat_cache[fat_offset + 1],
            ]) >> 4
        };
        entry
    }

    fn set_fat_entry(&mut self, cluster: u16, value: u16) {
        let fat_offset = (cluster as usize * 3) / 2;
        
        if cluster & 1 == 0 {
            // Even cluster
            self.fat_cache[fat_offset] = (value & 0xFF) as u8;
            self.fat_cache[fat_offset + 1] = (self.fat_cache[fat_offset + 1] & 0xF0) 
                | ((value >> 8) & 0x0F) as u8;
        } else {
            // Odd cluster
            self.fat_cache[fat_offset] = (self.fat_cache[fat_offset] & 0x0F) 
                | ((value & 0x0F) << 4) as u8;
            self.fat_cache[fat_offset + 1] = ((value >> 4) & 0xFF) as u8;
        }
    }

    fn flush_fat(&mut self) -> Result<(), &'static str> {
        fat_lock();
        
        for i in 0..self.bpb.fat_size_16 {
            let sector_data = &self.fat_cache[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            let err = ide_write_sectors(self.drive, self.fat_start_sector + i as u64, sector_data);
            if err != 0 {
                fat_unlock();
                return Err("Failed to write FAT");
            }
        }
        
        // Write to backup FAT
        for fat_num in 1..self.bpb.num_fats {
            let backup_start = self.fat_start_sector + (fat_num as u64 * self.bpb.fat_size_16 as u64);
            for i in 0..self.bpb.fat_size_16 {
                let sector_data = &self.fat_cache[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
                let err = ide_write_sectors(self.drive, backup_start + i as u64, sector_data);
                if err != 0 {
                    fat_unlock();
                    return Err("Failed to write backup FAT");
                }
            }
        }
        
        fat_unlock();
        Ok(())
    }

    fn find_free_cluster(&self) -> Option<u16> {
        // Start from cluster 2 (0 and 1 are reserved)
        for cluster in 2..0xFF0 {
            if self.get_fat_entry(cluster) == FAT12_FREE_CLUSTER {
                return Some(cluster);
            }
        }
        None
    }

    fn allocate_cluster(&mut self) -> Result<u16, &'static str> {
        let cluster = self.find_free_cluster().ok_or("No free clusters")?;
        self.set_fat_entry(cluster, FAT12_EOF);
        Ok(cluster)
    }

    fn cluster_to_sector(&self, cluster: u16) -> u64 {
        self.data_start_sector + ((cluster as u64 - 2) * self.bpb.sectors_per_cluster as u64)
    }

    fn read_cluster(&self, cluster: u16) -> Result<Vec<u8>, &'static str> {
        fat_lock();
        
        let sector = self.cluster_to_sector(cluster);
        let cluster_size = self.bpb.sectors_per_cluster as usize * SECTOR_SIZE;
        let mut data = alloc::vec![0u8; cluster_size];
        
        for i in 0..self.bpb.sectors_per_cluster {
            let sector_data = &mut data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            let err = ide_read_sectors(self.drive, sector + i as u64, sector_data);
            if err != 0 {
                fat_unlock();
                return Err("Failed to read cluster");
            }
        }
        
        fat_unlock();
        Ok(data)
    }

    fn write_cluster(&mut self, cluster: u16, data: &[u8]) -> Result<(), &'static str> {
        fat_lock();
        
        let sector = self.cluster_to_sector(cluster);
        let cluster_size = self.bpb.sectors_per_cluster as usize * SECTOR_SIZE;
        
        let mut padded_data = alloc::vec![0u8; cluster_size];
        let copy_len = core::cmp::min(data.len(), cluster_size);
        padded_data[..copy_len].copy_from_slice(&data[..copy_len]);
        
        for i in 0..self.bpb.sectors_per_cluster {
            let sector_data = &padded_data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            let err = ide_write_sectors(self.drive, sector + i as u64, sector_data);
            if err != 0 {
                fat_unlock();
                return Err("Failed to write cluster");
            }
        }
        
        fat_unlock();
        Ok(())
    }

    pub fn read_root_directory(&self) -> Result<Vec<DirectoryEntry>, &'static str> {
        fat_lock();
        
        let root_size = self.root_dir_sectors as usize * SECTOR_SIZE;
        let mut root_data = alloc::vec![0u8; root_size];
        
        for i in 0..self.root_dir_sectors {
            let sector_data = &mut root_data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            let err = ide_read_sectors(self.drive, self.root_dir_start_sector + i as u64, sector_data);
            if err != 0 {
                fat_unlock();
                return Err("Failed to read root directory");
            }
        }
        
        fat_unlock();

        let num_entries = self.bpb.root_entry_count as usize;
        let mut entries = Vec::new();

        for i in 0..num_entries {
            let offset = i * 32;
            let entry = unsafe { *(root_data.as_ptr().add(offset) as *const DirectoryEntry) };
            
            if entry.is_empty() {
                break;
            }
            
            if !entry.is_deleted() && !entry.is_lfn() && !entry.is_volume_id() {
                entries.push(entry);
            }
        }

        Ok(entries)
    }

    fn write_root_directory(&mut self, entries: &[DirectoryEntry]) -> Result<(), &'static str> {
        fat_lock();
        
        let root_size = self.root_dir_sectors as usize * SECTOR_SIZE;
        let mut root_data = alloc::vec![0u8; root_size];
        
        // Copy entries
        for (i, entry) in entries.iter().enumerate() {
            let offset = i * 32;
            unsafe {
                let entry_ptr = root_data.as_mut_ptr().add(offset) as *mut DirectoryEntry;
                *entry_ptr = *entry;
            }
        }
        
        // Write back to disk
        for i in 0..self.root_dir_sectors {
            let sector_data = &root_data[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            let err = ide_write_sectors(self.drive, self.root_dir_start_sector + i as u64, sector_data);
            if err != 0 {
                fat_unlock();
                return Err("Failed to write root directory");
            }
        }
        
        fat_unlock();
        Ok(())
    }

    pub fn read_file(&self, entry: &DirectoryEntry) -> Result<Vec<u8>, &'static str> {
        if entry.file_size == 0 {
            return Ok(Vec::new());
        }

        let mut data = Vec::new();
        let mut cluster = entry.first_cluster;
        let _cluster_size = self.bpb.sectors_per_cluster as usize * SECTOR_SIZE;

        loop {
            if cluster < 2 || cluster >= FAT12_BAD_CLUSTER {
                break;
            }

            let cluster_data = self.read_cluster(cluster)?;
            data.extend_from_slice(&cluster_data);

            cluster = self.get_fat_entry(cluster);
            if cluster >= FAT12_EOF {
                break;
            }
        }

        // Trim to actual file size
        data.truncate(entry.file_size as usize);
        Ok(data)
    }

    pub fn create_file(&mut self, filename: &str, data: &[u8]) -> Result<(), &'static str> {
        // Allocate clusters for the file
        let cluster_size = self.bpb.sectors_per_cluster as usize * SECTOR_SIZE;
        let num_clusters = (data.len() + cluster_size - 1) / cluster_size;
        
        if num_clusters == 0 {
            return Err("Cannot create empty file");
        }

        let mut clusters = Vec::new();
        for _ in 0..num_clusters {
            clusters.push(self.allocate_cluster()?);
        }

        // Link clusters in FAT
        for i in 0..clusters.len() - 1 {
            self.set_fat_entry(clusters[i], clusters[i + 1]);
        }
        self.set_fat_entry(clusters[clusters.len() - 1], FAT12_EOF);

        // Write data to clusters
        for (i, &cluster) in clusters.iter().enumerate() {
            let offset = i * cluster_size;
            let end = core::cmp::min(offset + cluster_size, data.len());
            self.write_cluster(cluster, &data[offset..end])?;
        }

        // Create directory entry
        let entry = DirectoryEntry::new_file(filename, clusters[0], data.len() as u32)?;

        // Add to root directory
        let mut entries = self.read_root_directory()?;
        
        // Check if file already exists
        for existing in &entries {
            if let Ok(name) = existing.get_name() {
                if name.to_uppercase() == filename.to_uppercase() {
                    return Err("File already exists");
                }
            }
        }

        entries.push(entry);
        self.write_root_directory(&entries)?;

        // Flush FAT
        self.flush_fat()?;

        Ok(())
    }

    pub fn delete_file(&mut self, filename: &str) -> Result<(), &'static str> {
        let mut entries = self.read_root_directory()?;
        let mut found_index = None;
        let mut first_cluster = 0u16;

        // Find the file
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

        // Free clusters
        let mut cluster = first_cluster;
        while cluster >= 2 && cluster < FAT12_BAD_CLUSTER {
            let next_cluster = self.get_fat_entry(cluster);
            self.set_fat_entry(cluster, FAT12_FREE_CLUSTER);
            
            if next_cluster >= FAT12_EOF {
                break;
            }
            cluster = next_cluster;
        }

        // Mark directory entry as deleted
        entries[index].name[0] = 0xE5;
        self.write_root_directory(&entries)?;
        
        // Flush FAT
        self.flush_fat()?;

        Ok(())
    }

    pub fn list_files(&self) -> Result<Vec<String>, &'static str> {
        let entries = self.read_root_directory()?;
        let mut files = Vec::new();

        for entry in entries {
            if let Ok(name) = entry.get_name() {
                files.push(name);
            }
        }

        Ok(files)
    }

    pub fn file_exists(&self, filename: &str) -> bool {
        if let Ok(entries) = self.read_root_directory() {
            for entry in entries {
                if let Ok(name) = entry.get_name() {
                    if name.to_uppercase() == filename.to_uppercase() {
                        return true;
                    }
                }
            }
        }
        false
    }

    pub fn get_file_size(&self, filename: &str) -> Option<u32> {
        if let Ok(entries) = self.read_root_directory() {
            for entry in entries {
                if let Ok(name) = entry.get_name() {
                    if name.to_uppercase() == filename.to_uppercase() {
                        return Some(entry.file_size);
                    }
                }
            }
        }
        None
    }
}