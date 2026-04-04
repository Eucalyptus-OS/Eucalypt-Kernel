#![no_std]

extern crate alloc;

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use spin::Mutex;

pub const O_RDONLY: u32 = 0x0000;
pub const O_WRONLY: u32 = 0x0001;
pub const O_RDWR:   u32 = 0x0002;
pub const O_CREAT:  u32 = 0x0040;
pub const O_TRUNC:  u32 = 0x0200;
pub const O_APPEND: u32 = 0x0400;
pub const O_EXCL:   u32 = 0x0800;

pub const S_IRUSR: u32 = 0o400;
pub const S_IWUSR: u32 = 0o200;
pub const S_IXUSR: u32 = 0o100;
pub const S_IRGRP: u32 = 0o040;
pub const S_IWGRP: u32 = 0o020;
pub const S_IROTH: u32 = 0o004;
pub const S_IFREG: u32 = 0o100000;
pub const S_IFDIR: u32 = 0o040000;
pub const S_IMODE: u32 = 0o777;

pub const STDIN_FD:  u32 = 0;
pub const STDOUT_FD: u32 = 1;
pub const STDERR_FD: u32 = 2;

pub const STDIN_NODE_ID:  u32 = 0;
pub const STDOUT_NODE_ID: u32 = 1;
pub const STDERR_NODE_ID: u32 = 2;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FD {
    pub node_id:   u32,
    pub flags:     u32,
    pub ref_count: u32,
}

impl FD {
    pub const EMPTY: Self = Self {
        node_id:   u32::MAX,
        flags:     0,
        ref_count: 0,
    };

    pub fn is_empty(&self) -> bool {
        self.node_id == u32::MAX
    }

    pub fn is_special(&self) -> bool {
        self.node_id <= 2
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NodeKind {
    File,
    Dir,
}

#[derive(Clone, Debug)]
pub struct DirEntry {
    pub name:   String,
    pub kind:   NodeKind,
    pub size:   u64,
}

#[derive(Clone, Debug)]
pub struct FileStat {
    pub size:  u64,
    pub kind:  NodeKind,
    pub mode:  u32,
}

#[derive(Clone, Debug)]
pub struct FsStat {
    pub total_bytes: u64,
    pub free_bytes:  u64,
    pub fs_type:     &'static str,
}

pub trait FileSystem: Send + Sync {
    fn stat(&self, path: &str) -> Result<FileStat, VfsError>;
    fn read(&self, path: &str) -> Result<Vec<u8>, VfsError>;
    fn write(&self, path: &str, data: &[u8]) -> Result<(), VfsError>;
    fn create(&self, path: &str, data: &[u8], mode: u32) -> Result<(), VfsError>;
    fn append(&self, path: &str, data: &[u8]) -> Result<(), VfsError>;
    fn unlink(&self, path: &str) -> Result<(), VfsError>;
    fn rename(&self, from: &str, to: &str) -> Result<(), VfsError>;
    fn mkdir(&self, path: &str, mode: u32) -> Result<(), VfsError>;
    fn rmdir(&self, path: &str) -> Result<(), VfsError>;
    fn readdir(&self, path: &str) -> Result<Vec<DirEntry>, VfsError>;
    fn stat_fs(&self) -> FsStat;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VfsError {
    NotFound,
    AlreadyExists,
    NotAFile,
    NotADir,
    NotEmpty,
    PermissionDenied,
    InvalidPath,
    NotSupported,
    IoError,
    NoSpace,
    NotMounted,
    BadDescriptor,
    NotOpen,
}

impl VfsError {
    pub fn as_str(self) -> &'static str {
        match self {
            VfsError::NotFound        => "not found",
            VfsError::AlreadyExists   => "already exists",
            VfsError::NotAFile        => "not a file",
            VfsError::NotADir         => "not a directory",
            VfsError::NotEmpty        => "directory not empty",
            VfsError::PermissionDenied => "permission denied",
            VfsError::InvalidPath     => "invalid path",
            VfsError::NotSupported    => "not supported",
            VfsError::IoError         => "I/O error",
            VfsError::NoSpace         => "no space left",
            VfsError::NotMounted      => "not mounted",
            VfsError::BadDescriptor   => "bad file descriptor",
            VfsError::NotOpen         => "file not open",
        }
    }
}

impl core::fmt::Display for VfsError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{}", self.as_str())
    }
}

#[derive(Debug)]
pub struct OpenFile {
    pub id:     u32,
    pub kind:   NodeKind,
    pub flags:  u32,
    pub mode:   u32,
    pub size:   u64,
    pub cursor: u64,
    mount_idx:  usize,
    path:       String,
}

struct MountEntry {
    point: String,
    fs:    Box<dyn FileSystem>,
}

struct Vfs {
    mounts:      Vec<MountEntry>,
    open_files:  Vec<OpenFile>,
    next_id:     AtomicU32,
}

impl Vfs {
    const fn new_uninit() -> Self {
        Vfs {
            mounts:     Vec::new(),
            open_files: Vec::new(),
            next_id:    AtomicU32::new(3),
        }
    }

    fn next_id(&self) -> u32 {
        self.next_id.fetch_add(1, Ordering::Relaxed)
    }

    fn find_mount(&self, point: &str) -> Option<(usize, &MountEntry)> {
        self.mounts.iter().enumerate().find(|(_, e)| e.point == point)
    }

    #[allow(unused)]
    fn find_mount_mut(&mut self, point: &str) -> Option<(usize, &mut MountEntry)> {
        self.mounts.iter_mut().enumerate().find(|(_, e)| e.point == point)
    }

    #[allow(unused)]
    fn resolve<'p>(&self, path: &'p str) -> Result<(usize, &MountEntry, &'p str), VfsError> {
        let (point, rel) = split_path(path)?;
        let (idx, entry) = self.find_mount(point).ok_or(VfsError::NotMounted)?;
        Ok((idx, entry, rel))
    }

    fn find_open(&mut self, id: u32) -> Result<&mut OpenFile, VfsError> {
        self.open_files
            .iter_mut()
            .find(|f| f.id == id)
            .ok_or(VfsError::BadDescriptor)
    }

    fn fs_by_idx(&self, idx: usize) -> &dyn FileSystem {
        self.mounts[idx].fs.as_ref()
    }
}

static VFS_LOCK: AtomicBool = AtomicBool::new(false);

static VFS: Mutex<Vfs> = Mutex::new(Vfs::new_uninit());

fn lock() {
    while VFS_LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
}

fn unlock() {
    VFS_LOCK.store(false, Ordering::Release);
}

fn vfs() -> spin::MutexGuard<'static, Vfs> {
    VFS.lock()
}

fn split_path(path: &str) -> Result<(&str, &str), VfsError> {
    let path = path.trim_start_matches('/');
    match path.find('/') {
        Some(i) => {
            let point = &path[..i];
            let rel   = &path[i + 1..];
            if point.is_empty() || rel.is_empty() {
                Err(VfsError::InvalidPath)
            } else {
                Ok((point, rel))
            }
        }
        None => Err(VfsError::InvalidPath),
    }
}

pub fn vfs_file_exists(path: &str) -> bool {
    vfs_stat(path).is_ok()
}

pub fn vfs_read_file(path: &str) -> Result<Vec<u8>, VfsError> {
    let id = vfs_open(path, O_RDONLY, 0)?;
    let data = vfs_read_all(id);
    let _ = vfs_close(id);
    data
}

pub fn vfs_init() {
    // Already initialized via static Mutex
}

pub fn vfs_mount(point: &str, fs: Box<dyn FileSystem>) -> Result<(), VfsError> {
    lock();
    let mut v = vfs();
    let result = if v.find_mount(point).is_some() {
        Err(VfsError::AlreadyExists)
    } else {
        v.mounts.push(MountEntry { point: String::from(point), fs });
        Ok(())
    };
    unlock();
    result
}

pub fn vfs_unmount(point: &str) -> Result<(), VfsError> {
    lock();
    let mut v = vfs();
    let before = v.mounts.len();
    v.mounts.retain(|e| e.point != point);
    let result = if v.mounts.len() == before {
        Err(VfsError::NotMounted)
    } else {
        Ok(())
    };
    unlock();
    result
}

pub fn vfs_open(path: &str, flags: u32, mode: u32) -> Result<u32, VfsError> {
    let (point, rel) = split_path(path)?;
    lock();
    let result = (|| {
        let mut v = vfs();
        let (idx, entry) = v.find_mount(point).ok_or(VfsError::NotMounted)?;
        let fs = entry.fs.as_ref();

        let exists = fs.stat(rel).is_ok();

        if flags & O_EXCL != 0 && flags & O_CREAT != 0 && exists {
            return Err(VfsError::AlreadyExists);
        }

        if flags & O_CREAT != 0 && !exists {
            fs.create(rel, &[], mode)?;
        } else if !exists {
            return Err(VfsError::NotFound);
        }

        if flags & O_TRUNC != 0 {
            fs.write(rel, &[])?;
        }

        let stat = fs.stat(rel)?;
        let cursor = if flags & O_APPEND != 0 { stat.size } else { 0 };
        let id = v.next_id();

        v.open_files.push(OpenFile {
            id,
            kind: stat.kind,
            flags,
            mode: stat.mode,
            size: stat.size,
            cursor,
            mount_idx: idx,
            path: String::from(rel),
        });

        Ok(id)
    })();
    unlock();
    result
}

pub fn vfs_close(id: u32) -> Result<(), VfsError> {
    lock();
    let mut v = vfs();
    let before = v.open_files.len();
    v.open_files.retain(|f| f.id != id);
    let result = if v.open_files.len() == before {
        Err(VfsError::BadDescriptor)
    } else {
        Ok(())
    };
    unlock();
    result
}

pub fn vfs_read(id: u32, count: usize) -> Result<Vec<u8>, VfsError> {
    lock();
    let result = (|| {
        let mut v = vfs();
        let file = v.find_open(id)?;

        if file.kind != NodeKind::File {
            return Err(VfsError::NotAFile);
        }
        if file.flags & 0x3 == O_WRONLY {
            return Err(VfsError::PermissionDenied);
        }

        let path  = file.path.clone();
        let start = file.cursor as usize;
        let idx   = file.mount_idx;

        let data  = v.fs_by_idx(idx).read(&path)?;
        let avail = data.len().saturating_sub(start);
        let n     = avail.min(count);
        let slice = data[start..start + n].to_vec();

        v.find_open(id)?.cursor += n as u64;
        Ok(slice)
    })();
    unlock();
    result
}

pub fn vfs_read_all(id: u32) -> Result<Vec<u8>, VfsError> {
    vfs_read(id, usize::MAX)
}

pub fn vfs_write(id: u32, data: &[u8]) -> Result<(), VfsError> {
    lock();
    let result = (|| {
        let mut v = vfs();
        let file = v.find_open(id)?;

        if file.kind != NodeKind::File {
            return Err(VfsError::NotAFile);
        }
        if file.flags & 0x3 == O_RDONLY {
            return Err(VfsError::PermissionDenied);
        }

        let path   = file.path.clone();
        let append = file.flags & O_APPEND != 0;
        let cursor = if append { file.size as usize } else { file.cursor as usize };
        let idx    = file.mount_idx;
        let fs     = v.fs_by_idx(idx);

        if cursor == 0 && !append {
            fs.write(&path, data)?;
        } else {
            let mut existing = fs.read(&path)?;
            let end = cursor + data.len();
            if end > existing.len() {
                existing.resize(end, 0);
            }
            existing[cursor..end].copy_from_slice(data);
            fs.write(&path, &existing)?;
        }

        let new_size = fs.stat(&path)?.size;
        let file = v.find_open(id)?;
        file.size   = new_size;
        file.cursor = if append { new_size } else { file.cursor + data.len() as u64 };
        Ok(())
    })();
    unlock();
    result
}

pub fn vfs_seek(id: u32, offset: u64) -> Result<(), VfsError> {
    lock();
    let result = vfs()
        .find_open(id)
        .map(|f| { f.cursor = offset.min(f.size); });
    unlock();
    result
}

pub fn vfs_stat_fd(id: u32) -> Result<FileStat, VfsError> {
    lock();
    let result = vfs().find_open(id).map(|f| FileStat {
        size: f.size,
        kind: f.kind,
        mode: f.mode,
    });
    unlock();
    result
}

pub fn vfs_stat(path: &str) -> Result<FileStat, VfsError> {
    let (point, rel) = split_path(path)?;
    lock();
    let result = vfs()
        .find_mount(point)
        .ok_or(VfsError::NotMounted)?
        .1
        .fs
        .stat(rel);
    unlock();
    result
}

pub fn vfs_create(path: &str, data: &[u8], mode: u32) -> Result<(), VfsError> {
    let (point, rel) = split_path(path)?;
    lock();
    let result = vfs()
        .find_mount(point)
        .ok_or(VfsError::NotMounted)?
        .1
        .fs
        .create(rel, data, mode);
    unlock();
    result
}

pub fn vfs_unlink(path: &str) -> Result<(), VfsError> {
    let (point, rel) = split_path(path)?;
    lock();
    let result = vfs()
        .find_mount(point)
        .ok_or(VfsError::NotMounted)?
        .1
        .fs
        .unlink(rel);
    unlock();
    result
}

pub fn vfs_rename(from: &str, to: &str) -> Result<(), VfsError> {
    let (fm, fr) = split_path(from)?;
    let (tm, tr) = split_path(to)?;
    if fm != tm {
        return Err(VfsError::NotSupported);
    }
    lock();
    let result = vfs()
        .find_mount(fm)
        .ok_or(VfsError::NotMounted)?
        .1
        .fs
        .rename(fr, tr);
    unlock();
    result
}

pub fn vfs_mkdir(path: &str, mode: u32) -> Result<(), VfsError> {
    let (point, rel) = split_path(path)?;
    lock();
    let result = vfs()
        .find_mount(point)
        .ok_or(VfsError::NotMounted)?
        .1
        .fs
        .mkdir(rel, mode);
    unlock();
    result
}

pub fn vfs_rmdir(path: &str) -> Result<(), VfsError> {
    let (point, rel) = split_path(path)?;
    lock();
    let result = vfs()
        .find_mount(point)
        .ok_or(VfsError::NotMounted)?
        .1
        .fs
        .rmdir(rel);
    unlock();
    result
}

pub fn vfs_readdir(path: &str) -> Result<Vec<DirEntry>, VfsError> {
    let (point, rel) = split_path(path)?;
    lock();
    let result = vfs()
        .find_mount(point)
        .ok_or(VfsError::NotMounted)?
        .1
        .fs
        .readdir(rel);
    unlock();
    result
}

pub fn vfs_stat_fs(mount_point: &str) -> Result<FsStat, VfsError> {
    lock();
    let result = vfs()
        .find_mount(mount_point)
        .ok_or(VfsError::NotMounted)
        .map(|(_, e)| e.fs.stat_fs());
    unlock();
    result
}

pub fn vfs_list_mounts() -> Vec<String> {
    lock();
    let result = vfs().mounts.iter().map(|e| e.point.clone()).collect();
    unlock();
    result
}