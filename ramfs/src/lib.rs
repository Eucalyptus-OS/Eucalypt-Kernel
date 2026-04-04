#![no_std]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use spin::Mutex;
use vfs::{DirEntry, FileStat, FsStat, FileSystem, NodeKind, VfsError};
use fat12::{BiosParameterBlock, DirectoryEntry};

const SECTOR_SIZE: usize = 512;
const FAT12_EOF: u16 = 0xFF8;
const FAT12_BAD: u16 = 0xFF7;

struct RamFile {
    path: String,
    data: Vec<u8>,
    mode: u32,
}

struct RamDir {
    path: String,
    mode: u32,
}

struct Inner {
    files: Vec<RamFile>,
    dirs:  Vec<RamDir>,
}

pub struct RamFs {
    inner: Mutex<Inner>,
}

impl RamFs {
    pub fn new() -> Self {
        RamFs {
            inner: Mutex::new(Inner {
                files: Vec::new(),
                dirs:  Vec::new(),
            }),
        }
    }

    pub fn load_from_fat12(self: &Self, image: &[u8]) -> Result<(), VfsError> {
        if image.len() < SECTOR_SIZE {
            return Err(VfsError::IoError);
        }

        let bpb = unsafe { &*(image.as_ptr() as *const BiosParameterBlock) };

        if bpb.bytes_per_sector as usize != SECTOR_SIZE {
            return Err(VfsError::IoError);
        }

        let fat_start      = bpb.reserved_sectors as usize * SECTOR_SIZE;
        let root_dir_sects = ((bpb.root_entry_count as usize * 32) + (SECTOR_SIZE - 1)) / SECTOR_SIZE;
        let root_dir_start = fat_start + bpb.num_fats as usize * bpb.fat_size_16 as usize * SECTOR_SIZE;
        let data_start     = root_dir_start + root_dir_sects * SECTOR_SIZE;
        let cluster_size   = bpb.sectors_per_cluster as usize * SECTOR_SIZE;

        let fat_entry = |cluster: u16| -> u16 {
            let off = fat_start + (cluster as usize * 3) / 2;
            if off + 1 >= image.len() { return 0xFFF; }
            let val = u16::from_le_bytes([image[off], image[off + 1]]);
            if cluster & 1 == 0 { val & 0x0FFF } else { val >> 4 }
        };

        let cluster_offset = |cluster: u16| -> usize {
            data_start + (cluster as usize - 2) * cluster_size
        };

        let mut inner = self.inner.lock();

        for i in 0..bpb.root_entry_count as usize {
            let off = root_dir_start + i * 32;
            if off + 32 > image.len() { break; }

            let entry = unsafe { &*(image.as_ptr().add(off) as *const DirectoryEntry) };

            if entry.name[0] == 0x00 { break; }
            if entry.name[0] == 0xE5 { continue; }
            if entry.is_lfn() || entry.is_volume_id() { continue; }

            let name = entry.get_name().map_err(|_| VfsError::IoError)?;

            if entry.is_directory() {
                inner.dirs.push(RamDir { path: name, mode: 0o755 });
                continue;
            }

            let mut data = Vec::with_capacity(entry.file_size as usize);
            let mut cluster = entry.first_cluster;

            while cluster >= 2 && cluster < FAT12_BAD {
                let src = cluster_offset(cluster);
                let remaining = entry.file_size as usize - data.len();
                let to_copy   = remaining.min(cluster_size);

                if src + to_copy > image.len() { break; }
                data.extend_from_slice(&image[src..src + to_copy]);

                if data.len() >= entry.file_size as usize { break; }

                let next = fat_entry(cluster);
                if next >= FAT12_EOF { break; }
                cluster = next;
            }

            inner.files.push(RamFile { path: name, data, mode: 0o644 });
        }

        Ok(())
    }
}

impl FileSystem for RamFs {
    fn stat(&self, path: &str) -> Result<FileStat, VfsError> {
        let inner = self.inner.lock();
        if let Some(f) = inner.files.iter().find(|f| f.path == path) {
            return Ok(FileStat { size: f.data.len() as u64, kind: NodeKind::File, mode: f.mode });
        }
        if path == "." || inner.dirs.iter().any(|d| d.path == path) {
            return Ok(FileStat { size: 0, kind: NodeKind::Dir, mode: 0o755 });
        }
        Err(VfsError::NotFound)
    }

    fn read(&self, path: &str) -> Result<Vec<u8>, VfsError> {
        let inner = self.inner.lock();
        inner.files
            .iter()
            .find(|f| f.path == path)
            .map(|f| f.data.clone())
            .ok_or(VfsError::NotFound)
    }

    fn write(&self, path: &str, data: &[u8]) -> Result<(), VfsError> {
        let mut inner = self.inner.lock();
        inner.files
            .iter_mut()
            .find(|f| f.path == path)
            .map(|f| { f.data = data.to_vec(); })
            .ok_or(VfsError::NotFound)
    }

    fn create(&self, path: &str, data: &[u8], mode: u32) -> Result<(), VfsError> {
        let mut inner = self.inner.lock();
        if inner.files.iter().any(|f| f.path == path) {
            return Err(VfsError::AlreadyExists);
        }
        inner.files.push(RamFile { path: String::from(path), data: data.to_vec(), mode });
        Ok(())
    }

    fn append(&self, path: &str, data: &[u8]) -> Result<(), VfsError> {
        let mut inner = self.inner.lock();
        inner.files
            .iter_mut()
            .find(|f| f.path == path)
            .map(|f| f.data.extend_from_slice(data))
            .ok_or(VfsError::NotFound)
    }

    fn unlink(&self, path: &str) -> Result<(), VfsError> {
        let mut inner = self.inner.lock();
        let pos = inner.files
            .iter()
            .position(|f| f.path == path)
            .ok_or(VfsError::NotFound)?;
        inner.files.remove(pos);
        Ok(())
    }

    fn rename(&self, from: &str, to: &str) -> Result<(), VfsError> {
        let mut inner = self.inner.lock();
        if inner.files.iter().any(|f| f.path == to) {
            return Err(VfsError::AlreadyExists);
        }
        inner.files
            .iter_mut()
            .find(|f| f.path == from)
            .map(|f| { f.path = String::from(to); })
            .ok_or(VfsError::NotFound)
    }

    fn mkdir(&self, path: &str, mode: u32) -> Result<(), VfsError> {
        let mut inner = self.inner.lock();
        if inner.dirs.iter().any(|d| d.path == path) {
            return Err(VfsError::AlreadyExists);
        }
        inner.dirs.push(RamDir { path: String::from(path), mode });
        Ok(())
    }

    fn rmdir(&self, path: &str) -> Result<(), VfsError> {
        let mut inner = self.inner.lock();
        let has_children = inner.files.iter().any(|f| f.path.starts_with(path))
            || inner.dirs.iter().any(|d| d.path != path && d.path.starts_with(path));
        if has_children {
            return Err(VfsError::NotEmpty);
        }
        let pos = inner.dirs
            .iter()
            .position(|d| d.path == path)
            .ok_or(VfsError::NotFound)?;
        inner.dirs.remove(pos);
        Ok(())
    }

    fn readdir(&self, path: &str) -> Result<Vec<DirEntry>, VfsError> {
        let inner = self.inner.lock();
        let is_root = path == ".";

        let files = inner.files.iter().filter(|f| {
            if is_root {
                !f.path.contains('/')
            } else {
                f.path.starts_with(path) && {
                    let rest = &f.path[path.len()..];
                    rest.starts_with('/') && !rest[1..].contains('/')
                }
            }
        }).map(|f| DirEntry {
            name: basename(&f.path).into(),
            kind: NodeKind::File,
            size: f.data.len() as u64,
        });

        let dirs = inner.dirs.iter().filter(|d| {
            if is_root {
                !d.path.contains('/')
            } else {
                d.path.starts_with(path) && {
                    let rest = &d.path[path.len()..];
                    rest.starts_with('/') && !rest[1..].contains('/')
                }
            }
        }).map(|d| DirEntry {
            name: basename(&d.path).into(),
            kind: NodeKind::Dir,
            size: 0,
        });

        Ok(files.chain(dirs).collect())
    }

    fn stat_fs(&self) -> FsStat {
        let inner = self.inner.lock();
        let used: usize = inner.files.iter().map(|f| f.data.len()).sum();
        FsStat { total_bytes: used as u64, free_bytes: u64::MAX, fs_type: "ramfs" }
    }
}

pub fn mount_ramdisk(module_response: &limine::request::ModulesResponse, mount_point: &str) -> Result<(), VfsError> {
    // 1. Safely extract the first module
    let module = module_response
        .modules()
        .first()
        .ok_or(VfsError::NotFound)?;
    let data_ptr = module.data().as_ptr();
    
    if data_ptr.is_null() {
        return Err(VfsError::InvalidPath);
    }

    let data = unsafe { 
        core::slice::from_raw_parts(
            data_ptr as *const u8, 
            module.data().len() as usize
        ) 
    };
    let ramfs = RamFs::new();
    ramfs.load_from_fat12(data)?;
    vfs::vfs_mount(mount_point, alloc::boxed::Box::new(ramfs))?;

    Ok(())
}
fn basename(path: &str) -> &str {
    path.rfind('/').map(|i| &path[i + 1..]).unwrap_or(path)
}