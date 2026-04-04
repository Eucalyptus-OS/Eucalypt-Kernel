#![no_std]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use ide::{ide_read_sectors, ide_write_sectors};

const SECTOR_SIZE: usize    = 512;
const FAT12_EOF: u16        = 0xFF8;
const FAT12_BAD: u16        = 0xFF7;
const FAT12_FREE: u16       = 0x000;
const ENTRY_DELETED: u8     = 0xE5;
const ENTRY_END: u8         = 0x00;
const ATTR_DIRECTORY: u8    = 0x10;
const ATTR_VOLUME_ID: u8    = 0x08;
const ATTR_LFN: u8          = 0x0F;
const ATTR_ARCHIVE: u8      = 0x20;

#[repr(C, packed)]
#[derive(Clone, Copy)]
pub struct BiosParameterBlock {
    pub jmp_boot:           [u8; 3],
    pub oem_name:           [u8; 8],
    pub bytes_per_sector:   u16,
    pub sectors_per_cluster: u8,
    pub reserved_sectors:   u16,
    pub num_fats:           u8,
    pub root_entry_count:   u16,
    pub total_sectors_16:   u16,
    pub media_type:         u8,
    pub fat_size_16:        u16,
    pub sectors_per_track:  u16,
    pub num_heads:          u16,
    pub hidden_sectors:     u32,
    pub total_sectors_32:   u32,
    pub drive_number:       u8,
    pub reserved1:          u8,
    pub boot_signature:     u8,
    pub volume_id:          u32,
    pub volume_label:       [u8; 11],
    pub fs_type:            [u8; 8],
}

#[repr(C, packed)]
#[derive(Clone, Copy, Debug)]
pub struct DirectoryEntry {
    pub name:               [u8; 11],
    pub attributes:         u8,
    reserved:               u8,
    creation_time_tenth:    u8,
    creation_time:          u16,
    creation_date:          u16,
    last_access_date:       u16,
    first_cluster_high:     u16,
    last_mod_time:          u16,
    last_mod_date:          u16,
    pub first_cluster:      u16,
    pub file_size:          u32,
}

impl DirectoryEntry {
    pub fn is_end(&self) -> bool      { self.name[0] == ENTRY_END }
    pub fn is_deleted(&self) -> bool  { self.name[0] == ENTRY_DELETED }
    pub fn is_lfn(&self) -> bool      { self.attributes == ATTR_LFN }
    pub fn is_directory(&self) -> bool { (self.attributes & ATTR_DIRECTORY) != 0 }
    pub fn is_volume_id(&self) -> bool { (self.attributes & ATTR_VOLUME_ID) != 0 }

    pub fn is_visible(&self) -> bool {
        !self.is_end() && !self.is_deleted() && !self.is_lfn() && !self.is_volume_id()
    }

    pub fn get_name(&self) -> Result<String, &'static str> {
        if !self.is_visible() {
            return Err("invalid entry");
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

    pub fn set_name(&mut self, filename: &str) -> Result<(), &'static str> {
        let dot = filename.find('.');
        let (base, ext) = match dot {
            Some(i) => (&filename[..i], &filename[i + 1..]),
            None    => (filename, ""),
        };
        if base.len() > 8 || ext.len() > 3 {
            return Err("filename too long");
        }
        if base.is_empty() {
            return Err("filename is empty");
        }
        self.name = [b' '; 11];
        for (i, b) in base.bytes().enumerate() {
            self.name[i] = b.to_ascii_uppercase();
        }
        for (i, b) in ext.bytes().enumerate() {
            self.name[8 + i] = b.to_ascii_uppercase();
        }
        Ok(())
    }

    fn new_file(filename: &str, first_cluster: u16, size: u32) -> Result<Self, &'static str> {
        let mut e = DirectoryEntry {
            name: [b' '; 11],
            attributes: ATTR_ARCHIVE,
            reserved: 0, creation_time_tenth: 0, creation_time: 0,
            creation_date: 0, last_access_date: 0, first_cluster_high: 0,
            last_mod_time: 0, last_mod_date: 0,
            first_cluster, file_size: size,
        };
        e.set_name(filename)?;
        Ok(e)
    }

    fn new_dir(dirname: &str, first_cluster: u16) -> Result<Self, &'static str> {
        let mut e = DirectoryEntry {
            name: [b' '; 11],
            attributes: ATTR_DIRECTORY,
            reserved: 0, creation_time_tenth: 0, creation_time: 0,
            creation_date: 0, last_access_date: 0, first_cluster_high: 0,
            last_mod_time: 0, last_mod_date: 0,
            first_cluster, file_size: 0,
        };
        e.set_name(dirname)?;
        Ok(e)
    }
}

pub struct Fat12Volume {
    drive:              usize,
    fat_start:          u64,
    root_dir_start:     u64,
    data_start:         u64,
    root_dir_sectors:   u32,
    sectors_per_cluster: u8,
    root_entry_count:   u16,
    total_sectors:      u64,
    fat_size_sectors:   u16,
    num_fats:           u8,
    fat_cache:          Vec<u8>,
}

impl Fat12Volume {
    pub fn open(drive: usize) -> Result<Self, &'static str> {
        let mut boot = [0u8; SECTOR_SIZE];
        if ide_read_sectors(drive, 0, &mut boot) != 0 {
            return Err("failed to read boot sector");
        }
        if boot[510] != 0x55 || boot[511] != 0xAA {
            return Err("invalid boot sector signature");
        }

        let bpb = unsafe { core::ptr::read_unaligned(boot.as_ptr() as *const BiosParameterBlock) };
        let bps  = bpb.bytes_per_sector as usize;
        let spc  = bpb.sectors_per_cluster;
        let rsec = bpb.reserved_sectors as u64;
        let nfat = bpb.num_fats;
        let rec  = bpb.root_entry_count;
        let fsz  = bpb.fat_size_16;

        if bps != SECTOR_SIZE { return Err("unsupported sector size"); }
        if spc == 0           { return Err("sectors per cluster is zero"); }
        if nfat == 0          { return Err("num fats is zero"); }

        let fat_start      = rsec;
        let root_dir_sects = ((rec as u32 * 32) + (SECTOR_SIZE as u32 - 1)) / SECTOR_SIZE as u32;
        let root_dir_start = fat_start + nfat as u64 * fsz as u64;
        let data_start     = root_dir_start + root_dir_sects as u64;
        let total_sectors  = if bpb.total_sectors_16 != 0 {
            bpb.total_sectors_16 as u64
        } else {
            bpb.total_sectors_32 as u64
        };

        let fat_size_bytes = fsz as usize * SECTOR_SIZE;
        let mut fat_cache = alloc::vec![0u8; fat_size_bytes];
        for i in 0..fsz as u64 {
            let sector = &mut fat_cache[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            if ide_read_sectors(drive, fat_start + i, sector) != 0 {
                return Err("failed to read FAT");
            }
        }

        Ok(Fat12Volume {
            drive,
            fat_start,
            root_dir_start,
            data_start,
            root_dir_sectors: root_dir_sects,
            sectors_per_cluster: spc,
            root_entry_count: rec,
            total_sectors,
            fat_size_sectors: fsz,
            num_fats: nfat,
            fat_cache,
        })
    }

    fn cluster_to_sector(&self, cluster: u16) -> u64 {
        self.data_start + (cluster as u64 - 2) * self.sectors_per_cluster as u64
    }

    fn fat_entry(&self, cluster: u16) -> u16 {
        let off = (cluster as usize * 3) / 2;
        let val = u16::from_le_bytes([self.fat_cache[off], self.fat_cache[off + 1]]);
        if cluster & 1 == 0 { val & 0x0FFF } else { val >> 4 }
    }

    fn set_fat_entry(&mut self, cluster: u16, value: u16) {
        let off = (cluster as usize * 3) / 2;
        if cluster & 1 == 0 {
            self.fat_cache[off]     = (value & 0xFF) as u8;
            self.fat_cache[off + 1] = (self.fat_cache[off + 1] & 0xF0) | ((value >> 8) & 0x0F) as u8;
        } else {
            self.fat_cache[off]     = (self.fat_cache[off] & 0x0F) | ((value & 0x0F) << 4) as u8;
            self.fat_cache[off + 1] = ((value >> 4) & 0xFF) as u8;
        }
    }

    fn flush_fat(&mut self) -> Result<(), &'static str> {
        for fat_num in 0..self.num_fats as u64 {
            let fat_base = self.fat_start + fat_num * self.fat_size_sectors as u64;
            for i in 0..self.fat_size_sectors as u64 {
                let s = &self.fat_cache[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
                if ide_write_sectors(self.drive, fat_base + i, s) != 0 {
                    return Err("failed to write FAT");
                }
            }
        }
        Ok(())
    }

    fn allocate_cluster(&mut self) -> Result<u16, &'static str> {
        for c in 2..0xFF0 {
            if self.fat_entry(c) == FAT12_FREE {
                self.set_fat_entry(c, FAT12_EOF);
                return Ok(c);
            }
        }
        Err("no free clusters")
    }

    fn free_chain(&mut self, first: u16) {
        let mut c = first;
        while c >= 2 && c < FAT12_BAD {
            let next = self.fat_entry(c);
            self.set_fat_entry(c, FAT12_FREE);
            if next >= FAT12_EOF { break; }
            c = next;
        }
    }

    fn count_free(&self) -> u32 {
        (2..0xFF0u16).filter(|&c| self.fat_entry(c) == FAT12_FREE).count() as u32
    }

    fn read_cluster(&self, cluster: u16) -> Result<Vec<u8>, &'static str> {
        let sector = self.cluster_to_sector(cluster);
        let size   = self.sectors_per_cluster as usize * SECTOR_SIZE;
        let mut buf = alloc::vec![0u8; size];
        for i in 0..self.sectors_per_cluster as u64 {
            let s = &mut buf[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            if ide_read_sectors(self.drive, sector + i, s) != 0 {
                return Err("failed to read cluster");
            }
        }
        Ok(buf)
    }

    fn write_cluster(&self, cluster: u16, data: &[u8]) -> Result<(), &'static str> {
        let sector     = self.cluster_to_sector(cluster);
        let size       = self.sectors_per_cluster as usize * SECTOR_SIZE;
        let mut padded = alloc::vec![0u8; size];
        let n          = data.len().min(size);
        padded[..n].copy_from_slice(&data[..n]);
        for i in 0..self.sectors_per_cluster as u64 {
            let s = &padded[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            if ide_write_sectors(self.drive, sector + i, s) != 0 {
                return Err("failed to write cluster");
            }
        }
        Ok(())
    }

    fn read_chain(&self, first: u16, size: usize) -> Result<Vec<u8>, &'static str> {
        let mut data = Vec::new();
        let mut c    = first;
        while c >= 2 && c < FAT12_BAD {
            data.extend_from_slice(&self.read_cluster(c)?);
            let next = self.fat_entry(c);
            if next >= FAT12_EOF { break; }
            c = next;
        }
        data.truncate(size);
        Ok(data)
    }

    fn write_chain(&mut self, data: &[u8]) -> Result<u16, &'static str> {
        if data.is_empty() {
            return Ok(0);
        }
        let cluster_size  = self.sectors_per_cluster as usize * SECTOR_SIZE;
        let num_clusters  = (data.len() + cluster_size - 1) / cluster_size;
        let mut clusters  = Vec::with_capacity(num_clusters);
        for _ in 0..num_clusters {
            clusters.push(self.allocate_cluster()?);
        }
        for i in 0..clusters.len() - 1 {
            let next = clusters[i + 1];
            self.set_fat_entry(clusters[i], next);
        }
        self.set_fat_entry(clusters[clusters.len() - 1], FAT12_EOF);
        for (i, &c) in clusters.iter().enumerate() {
            let off = i * cluster_size;
            let end = (off + cluster_size).min(data.len());
            self.write_cluster(c, &data[off..end])?;
        }
        Ok(clusters[0])
    }

    fn read_root(&self) -> Result<Vec<DirectoryEntry>, &'static str> {
        let size     = self.root_dir_sectors as usize * SECTOR_SIZE;
        let mut buf  = alloc::vec![0u8; size];
        for i in 0..self.root_dir_sectors as u64 {
            let s = &mut buf[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            if ide_read_sectors(self.drive, self.root_dir_start + i, s) != 0 {
                return Err("failed to read root directory");
            }
        }
        let mut entries = Vec::new();
        for i in 0..self.root_entry_count as usize {
            let e = unsafe {
                core::ptr::read_unaligned(buf.as_ptr().add(i * 32) as *const DirectoryEntry)
            };
            if e.is_end() { break; }
            if e.is_visible() { entries.push(e); }
        }
        Ok(entries)
    }

    fn write_root(&self, entries: &[DirectoryEntry]) -> Result<(), &'static str> {
        let size    = self.root_dir_sectors as usize * SECTOR_SIZE;
        let mut buf = alloc::vec![0u8; size];
        for (i, e) in entries.iter().enumerate() {
            unsafe {
                core::ptr::write_unaligned(buf.as_mut_ptr().add(i * 32) as *mut DirectoryEntry, *e);
            }
        }
        for i in 0..self.root_dir_sectors as u64 {
            let s = &buf[i as usize * SECTOR_SIZE..(i as usize + 1) * SECTOR_SIZE];
            if ide_write_sectors(self.drive, self.root_dir_start + i, s) != 0 {
                return Err("failed to write root directory");
            }
        }
        Ok(())
    }

    fn read_subdir(&self, first_cluster: u16) -> Result<Vec<DirectoryEntry>, &'static str> {
        let mut entries = Vec::new();
        let mut c       = first_cluster;
        while c >= 2 && c < FAT12_BAD {
            let data = self.read_cluster(c)?;
            for i in 0..data.len() / 32 {
                let e = unsafe {
                    core::ptr::read_unaligned(data.as_ptr().add(i * 32) as *const DirectoryEntry)
                };
                if e.is_end() { return Ok(entries); }
                if e.is_visible() { entries.push(e); }
            }
            let next = self.fat_entry(c);
            if next >= FAT12_EOF { break; }
            c = next;
        }
        Ok(entries)
    }

    fn find_in_root(&self, name: &str) -> Result<(usize, DirectoryEntry), &'static str> {
        let entries = self.read_root()?;
        entries
            .into_iter()
            .enumerate()
            .find(|(_, e)| e.get_name().map(|n| n.eq_ignore_ascii_case(name)).unwrap_or(false))
            .ok_or("not found")
    }

    pub fn find_entry(&self, name: &str) -> Result<DirectoryEntry, &'static str> {
        self.find_in_root(name).map(|(_, e)| e)
    }

    pub fn stat(&self) -> (u64, u64) {
        let total = self.total_sectors * SECTOR_SIZE as u64;
        let free  = self.count_free() as u64 * self.sectors_per_cluster as u64 * SECTOR_SIZE as u64;
        (total, free)
    }

    pub fn read_file(&self, filename: &str) -> Result<Vec<u8>, &'static str> {
        let (_, e) = self.find_in_root(filename)?;
        if e.file_size == 0 { return Ok(Vec::new()); }
        self.read_chain(e.first_cluster, e.file_size as usize)
    }

    pub fn create_file(&mut self, filename: &str, data: &[u8]) -> Result<(), &'static str> {
        if self.find_in_root(filename).is_ok() {
            return Err("file already exists");
        }
        let first_cluster = self.write_chain(data)?;
        let mut entries   = self.read_root()?;
        entries.push(DirectoryEntry::new_file(filename, first_cluster, data.len() as u32)?);
        self.write_root(&entries)?;
        self.flush_fat()
    }

    pub fn write_file(&mut self, filename: &str, data: &[u8]) -> Result<(), &'static str> {
        let (idx, old) = self.find_in_root(filename)?;
        self.free_chain(old.first_cluster);
        let first_cluster = self.write_chain(data)?;
        let mut entries   = self.read_root()?;
        entries[idx].first_cluster = first_cluster;
        entries[idx].file_size     = data.len() as u32;
        self.write_root(&entries)?;
        self.flush_fat()
    }

    pub fn append_file(&mut self, filename: &str, data: &[u8]) -> Result<(), &'static str> {
        if data.is_empty() { return Ok(()); }
        let (idx, e) = self.find_in_root(filename)?;

        let cluster_size = self.sectors_per_cluster as usize * SECTOR_SIZE;
        let old_size     = e.file_size as usize;
        let last_offset  = if old_size == 0 { 0 } else { (old_size - 1) % cluster_size };
        let space_left   = if old_size == 0 { 0 } else { cluster_size - last_offset - 1 };

        if old_size == 0 || space_left < data.len() {
            let mut existing = if old_size > 0 {
                self.read_chain(e.first_cluster, old_size)?
            } else {
                Vec::new()
            };
            existing.extend_from_slice(data);
            self.free_chain(e.first_cluster);
            let first_cluster = self.write_chain(&existing)?;
            let mut entries   = self.read_root()?;
            entries[idx].first_cluster = first_cluster;
            entries[idx].file_size     = existing.len() as u32;
            self.write_root(&entries)?;
        } else {
            let last_cluster = {
                let mut c = e.first_cluster;
                loop {
                    let next = self.fat_entry(c);
                    if next >= FAT12_EOF { break c; }
                    c = next;
                }
            };
            let mut cluster_data = self.read_cluster(last_cluster)?;
            cluster_data[last_offset + 1..last_offset + 1 + data.len()].copy_from_slice(data);
            self.write_cluster(last_cluster, &cluster_data)?;
            let mut entries   = self.read_root()?;
            entries[idx].file_size = (old_size + data.len()) as u32;
            self.write_root(&entries)?;
        }

        self.flush_fat()
    }

    pub fn delete_file(&mut self, filename: &str) -> Result<(), &'static str> {
        let (idx, e) = self.find_in_root(filename)?;
        if e.is_directory() { return Err("not a file"); }
        self.free_chain(e.first_cluster);
        let mut entries       = self.read_root()?;
        entries[idx].name[0]  = ENTRY_DELETED;
        self.write_root(&entries)?;
        self.flush_fat()
    }

    pub fn rename_file(&mut self, old_name: &str, new_name: &str) -> Result<(), &'static str> {
        if self.find_in_root(new_name).is_ok() {
            return Err("destination already exists");
        }
        let (idx, _)  = self.find_in_root(old_name)?;
        let mut entries = self.read_root()?;
        entries[idx].set_name(new_name)?;
        self.write_root(&entries)
    }

    pub fn list_root(&self) -> Result<Vec<DirectoryEntry>, &'static str> {
        self.read_root()
    }

    pub fn list_directory(&self, dirname: &str) -> Result<Vec<DirectoryEntry>, &'static str> {
        let (_, e) = self.find_in_root(dirname)?;
        if !e.is_directory() { return Err("not a directory"); }
        self.read_subdir(e.first_cluster)
    }

    pub fn create_directory(&mut self, dirname: &str) -> Result<(), &'static str> {
        if self.find_in_root(dirname).is_ok() {
            return Err("directory already exists");
        }
        let cluster      = self.allocate_cluster()?;
        let cluster_size = self.sectors_per_cluster as usize * SECTOR_SIZE;
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
            last_mod_time: 0, last_mod_date: 0, first_cluster: 0, file_size: 0,
        };

        unsafe {
            core::ptr::write_unaligned(dir_data.as_mut_ptr() as *mut DirectoryEntry, dot);
            core::ptr::write_unaligned(dir_data.as_mut_ptr().add(32) as *mut DirectoryEntry, dotdot);
        }

        self.write_cluster(cluster, &dir_data)?;
        let mut entries = self.read_root()?;
        entries.push(DirectoryEntry::new_dir(dirname, cluster)?);
        self.write_root(&entries)?;
        self.flush_fat()
    }

    pub fn delete_directory(&mut self, dirname: &str) -> Result<(), &'static str> {
        let (idx, e) = self.find_in_root(dirname)?;
        if !e.is_directory() { return Err("not a directory"); }

        let sub = self.read_subdir(e.first_cluster)?;
        let non_dot = sub.iter().filter(|e| {
            e.get_name().map(|n| n != "." && n != "..").unwrap_or(false)
        }).count();
        if non_dot > 0 { return Err("directory not empty"); }

        self.free_chain(e.first_cluster);
        let mut entries      = self.read_root()?;
        entries[idx].name[0] = ENTRY_DELETED;
        self.write_root(&entries)?;
        self.flush_fat()
    }
}