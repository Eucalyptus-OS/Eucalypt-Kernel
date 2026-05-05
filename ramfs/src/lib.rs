#![no_std]

extern crate alloc;

use alloc::boxed::Box;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;
use spin::Mutex;
use vfs::{DirEntry, FileStat, FsStat, FileSystem, NodeKind, VfsError};
use fat12::{BiosParameterBlock, DirectoryEntry};

const SECTOR_SIZE: usize = 512;
const FAT12_EOF:   u16   = 0xFF8;
const FAT12_BAD:   u16   = 0xFF7;

struct RamFile {
    path: String,
    data: Vec<u8>,
    mode: u32,
}

struct RamDir {
    path: String,
}

struct Inner {
    files: Vec<RamFile>,
    dirs:  Vec<RamDir>,
}

pub struct RamFs {
    inner: Mutex<Inner>,
}

impl RamFs {
    // create an empty in-memory filesystem
    pub fn new() -> Self {
        RamFs {
            inner: Mutex::new(Inner { files: Vec::new(), dirs: Vec::new() }),
        }
    }

    // return the byte slice for a cluster from the raw image
    fn read_cluster_data<'a>(image: &'a [u8], cluster: u16, data_start: usize, cluster_size: usize) -> &'a [u8] {
        let off = data_start + (cluster as usize - 2) * cluster_size;
        &image[off..core::cmp::min(off + cluster_size, image.len())]
    }

    // follow the FAT12 chain and return the next cluster number
    fn next_cluster(image: &[u8], fat_start: usize, cluster: u16) -> u16 {
        let f_off = fat_start + (cluster as usize * 3) / 2;
        let val = u16::from_le_bytes([image[f_off], image[f_off + 1]]);
        if cluster & 1 == 0 { val & 0x0FFF } else { val >> 4 }
    }

    // read file_size bytes from a cluster chain into a Vec
    fn read_file_data(image: &[u8], mut cluster: u16, file_size: usize, fat_start: usize, data_start: usize, cluster_size: usize) -> Vec<u8> {
        let mut data = Vec::with_capacity(file_size);
        while cluster >= 2 && cluster < FAT12_BAD && data.len() < file_size {
            let src       = data_start + (cluster as usize - 2) * cluster_size;
            let remaining = file_size - data.len();
            let to_copy   = remaining.min(cluster_size);
            if src + to_copy > image.len() { break; }
            data.extend_from_slice(&image[src..src + to_copy]);
            let next = Self::next_cluster(image, fat_start, cluster);
            if next >= FAT12_EOF { break; }
            cluster = next;
        }
        data.truncate(file_size);
        data
    }

    // recursively walk a directory's entries and populate inner with files and dirs
    fn load_dir(image: &[u8], entries_slice: &[u8], prefix: &str, fat_start: usize, data_start: usize, cluster_size: usize, inner: &mut Inner) {
        let count = entries_slice.len() / 32;
        for i in 0..count {
            let off = i * 32;
            if off + 32 > entries_slice.len() { break; }
            let entry = unsafe { &*(entries_slice.as_ptr().add(off) as *const DirectoryEntry) };
            if entry.name[0] == 0x00 { break; }
            if entry.name[0] == 0xE5 { continue; }
            if entry.is_lfn() || entry.is_volume_id() { continue; }
            let raw_name = match entry.get_name() {
                Ok(n)  => n,
                Err(_) => continue,
            };
            if raw_name == "." || raw_name == ".." { continue; }
            let full_path = if prefix.is_empty() {
                raw_name.clone()
            } else {
                format!("{}/{}", prefix, raw_name)
            };
            if entry.is_directory() {
                inner.dirs.push(RamDir { path: full_path.clone() });
                let mut cluster = entry.first_cluster;
                while cluster >= 2 && cluster < FAT12_BAD {
                    let slice = Self::read_cluster_data(image, cluster, data_start, cluster_size);
                    Self::load_dir(image, slice, &full_path, fat_start, data_start, cluster_size, inner);
                    let next = Self::next_cluster(image, fat_start, cluster);
                    if next >= FAT12_EOF { break; }
                    cluster = next;
                }
            } else {
                let data = Self::read_file_data(image, entry.first_cluster, entry.file_size as usize, fat_start, data_start, cluster_size);
                inner.files.push(RamFile { path: full_path, data, mode: 0o644 });
            }
        }
    }

    // parse a FAT12 disk image and load all files into memory
    pub fn load_from_fat12(&self, image: &[u8]) -> Result<(), VfsError> {
        if image.len() < SECTOR_SIZE { return Err(VfsError::IoError); }
        let bpb = unsafe { &*(image.as_ptr() as *const BiosParameterBlock) };
        if bpb.bytes_per_sector as usize != SECTOR_SIZE { return Err(VfsError::IoError); }
        let fat_start      = bpb.reserved_sectors as usize * SECTOR_SIZE;
        let root_dir_sects = ((bpb.root_entry_count as usize * 32) + (SECTOR_SIZE - 1)) / SECTOR_SIZE;
        let root_dir_start = fat_start + bpb.num_fats as usize * bpb.fat_size_16 as usize * SECTOR_SIZE;
        let data_start     = root_dir_start + root_dir_sects * SECTOR_SIZE;
        let cluster_size   = bpb.sectors_per_cluster as usize * SECTOR_SIZE;
        let root_size      = root_dir_sects * SECTOR_SIZE;
        if root_dir_start + root_size > image.len() { return Err(VfsError::IoError); }
        let root_slice = &image[root_dir_start..root_dir_start + root_size];
        let mut inner = self.inner.lock();
        Self::load_dir(image, root_slice, "", fat_start, data_start, cluster_size, &mut inner);
        Ok(())
    }
}

impl FileSystem for RamFs {
    // return metadata for a file or directory, matching case-insensitively
    fn stat(&self, path: &str) -> Result<FileStat, VfsError> {
        let inner = self.inner.lock();
        if let Some(f) = inner.files.iter().find(|f| f.path.eq_ignore_ascii_case(path)) {
            return Ok(FileStat { size: f.data.len() as u64, kind: NodeKind::File, mode: f.mode });
        }
        if path == "." || path == "" || path == "/" || inner.dirs.iter().any(|d| d.path.eq_ignore_ascii_case(path)) {
            return Ok(FileStat { size: 0, kind: NodeKind::Dir, mode: 0o755 });
        }
        Err(VfsError::NotFound)
    }

    // return the full contents of a file, matching case-insensitively
    fn read(&self, path: &str) -> Result<Vec<u8>, VfsError> {
        let inner = self.inner.lock();
        inner.files.iter()
            .find(|f| f.path.eq_ignore_ascii_case(path))
            .map(|f| f.data.clone())
            .ok_or(VfsError::NotFound)
    }

    // overwrite a file's contents in place
    fn write(&self, path: &str, data: &[u8]) -> Result<(), VfsError> {
        let mut inner = self.inner.lock();
        let file = inner.files.iter_mut()
            .find(|f| f.path.eq_ignore_ascii_case(path))
            .ok_or(VfsError::NotFound)?;
        file.data = data.to_vec();
        Ok(())
    }

    // create a new file with initial data at the given path
    fn create(&self, path: &str, data: &[u8], mode: u32) -> Result<(), VfsError> {
        let mut inner = self.inner.lock();
        if inner.files.iter().any(|f| f.path.eq_ignore_ascii_case(path)) {
            return Err(VfsError::AlreadyExists);
        }
        inner.files.push(RamFile { path: String::from(path), data: data.to_vec(), mode });
        Ok(())
    }

    // remove a file from the filesystem
    fn unlink(&self, path: &str) -> Result<(), VfsError> {
        let mut inner = self.inner.lock();
        let pos = inner.files.iter().position(|f| f.path.eq_ignore_ascii_case(path)).ok_or(VfsError::NotFound)?;
        inner.files.remove(pos);
        Ok(())
    }

    // rename a file from one path to another
    fn rename(&self, from: &str, to: &str) -> Result<(), VfsError> {
        let mut inner = self.inner.lock();
        if inner.files.iter().any(|f| f.path.eq_ignore_ascii_case(to)) { return Err(VfsError::AlreadyExists); }
        let file = inner.files.iter_mut().find(|f| f.path.eq_ignore_ascii_case(from)).ok_or(VfsError::NotFound)?;
        file.path = String::from(to);
        Ok(())
    }

    // create a new directory entry
    fn mkdir(&self, path: &str, _mode: u32) -> Result<(), VfsError> {
        let mut inner = self.inner.lock();
        if inner.dirs.iter().any(|d| d.path.eq_ignore_ascii_case(path)) { return Err(VfsError::AlreadyExists); }
        inner.dirs.push(RamDir { path: String::from(path) });
        Ok(())
    }

    // remove a directory if it has no children
    fn rmdir(&self, path: &str) -> Result<(), VfsError> {
        let mut inner = self.inner.lock();
        let has_children = inner.files.iter().any(|f| f.path.starts_with(path) && f.path != path)
            || inner.dirs.iter().any(|d| d.path.starts_with(path) && d.path != path);
        if has_children { return Err(VfsError::NotEmpty); }
        let pos = inner.dirs.iter().position(|d| d.path.eq_ignore_ascii_case(path)).ok_or(VfsError::NotFound)?;
        inner.dirs.remove(pos);
        Ok(())
    }

    // list the immediate children of a directory
    fn readdir(&self, path: &str) -> Result<Vec<DirEntry>, VfsError> {
        let inner = self.inner.lock();
        let mut entries = Vec::new();
        let is_root = path == "." || path == "" || path == "/";
        for f in &inner.files {
            if is_root {
                if !f.path.contains('/') {
                    entries.push(DirEntry { name: f.path.clone(), kind: NodeKind::File, size: f.data.len() as u64 });
                }
            } else if f.path.to_ascii_uppercase().starts_with(&path.to_ascii_uppercase()) {
                let rest = &f.path[path.len()..];
                if let Some(rel) = rest.strip_prefix('/') {
                    if !rel.is_empty() && !rel.contains('/') {
                        entries.push(DirEntry { name: String::from(rel), kind: NodeKind::File, size: f.data.len() as u64 });
                    }
                }
            }
        }
        for d in &inner.dirs {
            if is_root {
                if !d.path.contains('/') {
                    entries.push(DirEntry { name: d.path.clone(), kind: NodeKind::Dir, size: 0 });
                }
            } else if d.path.to_ascii_uppercase().starts_with(&path.to_ascii_uppercase()) {
                let rest = &d.path[path.len()..];
                if let Some(rel) = rest.strip_prefix('/') {
                    if !rel.is_empty() && !rel.contains('/') {
                        entries.push(DirEntry { name: String::from(rel), kind: NodeKind::Dir, size: 0 });
                    }
                }
            }
        }
        Ok(entries)
    }

    // return total and free byte counts for this filesystem
    fn stat_fs(&self) -> FsStat {
        let inner = self.inner.lock();
        let used: usize = inner.files.iter().map(|f| f.data.len()).sum();
        FsStat { total_bytes: used as u64, free_bytes: 0, fs_type: "ramfs" }
    }
}

// load the first limine module as a FAT12 image and mount it at the given drive letter
pub fn mount_ramdisk(module_response: &limine::request::ModulesResponse, letter: char) -> Result<(), VfsError> {
    let module = module_response.modules().first().ok_or(VfsError::NotFound)?;
    let data   = unsafe { core::slice::from_raw_parts(module.data().as_ptr(), module.data().len()) };
    let ramfs  = RamFs::new();
    ramfs.load_from_fat12(data)?;
    vfs::vfs_mount(letter, Box::new(ramfs))
}