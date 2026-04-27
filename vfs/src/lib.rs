#![no_std]

extern crate alloc;

use alloc::{boxed::Box, format};
use alloc::string::{String, ToString};
use alloc::vec::Vec;
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

pub const D_STDIN:  u32 = 0;
pub const D_STDOUT: u32 = 1;
pub const D_STDERR: u32 = 2;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NodeKind {
    File,
    Dir,
}

#[derive(Clone, Debug)]
pub struct DirEntry {
    pub name: String,
    pub kind: NodeKind,
    pub size: u64,
}

#[derive(Clone, Debug)]
pub struct FileStat {
    pub size: u64,
    pub kind: NodeKind,
    pub mode: u32,
}

#[derive(Clone, Debug)]
pub struct FsStat {
    pub total_bytes: u64,
    pub free_bytes:  u64,
    pub fs_type:     &'static str,
}

#[derive(Clone, Debug)]
pub struct FD {
    pub node:   VfsNode,
    pub offset: u64,
    pub flags:  u32,
}

impl FD {
    pub fn new(fd_num: u64, flags: u32) -> Self {
        FD {
            node: VfsNode {
                point: String::new(),
                rel:   String::new(),
                flags,
            },
            offset: fd_num,
            flags,
        }
    }

    // closes the fd by clearing its slot in the global table, no-ops for stdio
    pub fn close(&self) {
        let fd_num = self.offset as u32;
        if fd_num < 3 {
            return;
        }
        let mut table = FD_TABLE.lock();
        if let Some(slot) = table.get_mut(fd_num as usize) {
            *slot = None;
        }
    }
}

pub trait FileSystem: Send + Sync {
    fn stat(&self, path: &str) -> Result<FileStat, VfsError>;
    fn read(&self, path: &str) -> Result<Vec<u8>, VfsError>;
    fn write(&self, path: &str, data: &[u8]) -> Result<(), VfsError>;
    fn create(&self, path: &str, data: &[u8], mode: u32) -> Result<(), VfsError>;
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
    FdNotFound,
}

impl VfsError {
    pub fn as_str(self) -> &'static str {
        match self {
            VfsError::NotFound         => "not found",
            VfsError::AlreadyExists    => "already exists",
            VfsError::NotAFile         => "not a file",
            VfsError::NotADir          => "not a directory",
            VfsError::NotEmpty         => "directory not empty",
            VfsError::PermissionDenied => "permission denied",
            VfsError::InvalidPath      => "invalid path",
            VfsError::NotSupported     => "not supported",
            VfsError::IoError          => "I/O error",
            VfsError::NoSpace          => "no space left",
            VfsError::NotMounted       => "not mounted",
            VfsError::FdNotFound       => "file descriptor not found",
        }
    }
}

impl core::fmt::Display for VfsError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{}", self.as_str())
    }
}

struct MountEntry {
    point: String,
    fs:    Box<dyn FileSystem>,
}

struct Vfs {
    mounts: Vec<MountEntry>,
}

impl Vfs {
    const fn new_uninit() -> Self {
        Vfs { mounts: Vec::new() }
    }

    /// Returns the index and a reference to the mount whose point equals `point`, if any.
    fn find_mount(&self, point: &str) -> Option<(usize, &MountEntry)> {
        self.mounts.iter().enumerate().find(|(_, e)| e.point == point)
    }
}

// Single mutex guards both the mount table and all path operations; no
// secondary VFS_LOCK spinlock is needed — that was a double-lock bug.
static VFS: Mutex<Vfs> = Mutex::new(Vfs::new_uninit());
static FD_TABLE: Mutex<Vec<Option<FD>>> = Mutex::new(Vec::new());

/// Splits `path` into a (mount_point, relative_path) pair, returning `InvalidPath` if either part is empty.
fn split_path(path: &str) -> Result<(&str, &str), VfsError> {
    let path = path.trim_start_matches('/');
    if path.is_empty() {
        return Err(VfsError::InvalidPath);
    }
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

/// Pre-populates the global FD table with 256 empty slots (indices 0–255).
pub fn vfs_init() {
    let mut table = FD_TABLE.lock();
    for _ in 0..256 {
        table.push(None);
    }
}

/// Mounts `fs` at the given `point`, failing if that point is already mounted.
pub fn vfs_mount(point: &str, fs: Box<dyn FileSystem>) -> Result<(), VfsError> {
    let mut v = VFS.lock();
    if v.find_mount(point).is_some() {
        return Err(VfsError::AlreadyExists);
    }
    v.mounts.push(MountEntry { point: String::from(point), fs });
    Ok(())
}

/// Removes the filesystem mounted at `point`, returning `NotMounted` if it was not found.
pub fn vfs_unmount(point: &str) -> Result<(), VfsError> {
    let mut v = VFS.lock();
    let before = v.mounts.len();
    v.mounts.retain(|e| e.point != point);
    if v.mounts.len() == before {
        Err(VfsError::NotMounted)
    } else {
        Ok(())
    }
}

#[derive(Clone, Debug)]
pub struct VfsNode {
    point: String,
    rel:   String,
    flags: u32,
}

impl VfsNode {
    /// Reads the full contents of this node's file.
    pub fn read(&self) -> Result<Vec<u8>, VfsError> {
        let path = format!("{}/{}", self.point, self.rel);
        vfs_read(&path)
    }

    /// Writes `data` to this node's file using the flags it was opened with.
    pub fn write(&self, data: &[u8]) -> Result<(), VfsError> {
        let path = format!("{}/{}", self.point, self.rel);
        vfs_write(&path, data, self.flags, 0)
    }

    /// Returns metadata for this node's file.
    pub fn stat(&self) -> Result<FileStat, VfsError> {
        let path = format!("{}/{}", self.point, self.rel);
        vfs_stat(&path)
    }

    /// Returns the absolute path string for this node.
    pub fn path(&self) -> String {
        format!("{}/{}", self.point, self.rel)
    }
}

/// Opens (and optionally creates) the file at `path`, returning a `VfsNode` for it.
pub fn vfs_open_node(path: &str, flags: u32, mode: u32) -> Result<VfsNode, VfsError> {
    let (point, rel) = split_path(path)?;
    let v = VFS.lock();
    let (_idx, entry) = v.find_mount(point).ok_or(VfsError::NotMounted)?;
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

    Ok(VfsNode {
        point: point.to_string(),
        rel:   rel.to_string(),
        flags,
    })
}

/// Creates a new file at `path` with the given initial `data` and permission `mode`.
pub fn vfs_create(path: &str, data: &[u8], mode: u32) -> Result<(), VfsError> {
    let (point, rel) = split_path(path)?;
    let v = VFS.lock();
    let (_idx, entry) = v.find_mount(point).ok_or(VfsError::NotMounted)?;
    let fs = entry.fs.as_ref();

    if fs.stat(rel).is_ok() {
        return Err(VfsError::AlreadyExists);
    }

    fs.create(rel, data, mode)
}

/// Reads and returns the full contents of the file at `path`.
pub fn vfs_read(path: &str) -> Result<Vec<u8>, VfsError> {
    let (point, rel) = split_path(path)?;
    let v = VFS.lock();
    let (_idx, entry) = v.find_mount(point).ok_or(VfsError::NotMounted)?;
    entry.fs.read(rel)
}

/// Writes `data` to the file at `path`, honouring `O_CREAT`, `O_TRUNC`, `O_APPEND`, and `O_EXCL`.
pub fn vfs_write(path: &str, data: &[u8], flags: u32, mode: u32) -> Result<(), VfsError> {
    let (point, rel) = split_path(path)?;
    let v = VFS.lock();
    let (_idx, entry) = v.find_mount(point).ok_or(VfsError::NotMounted)?;
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

    if (flags & 0x3) == O_RDONLY {
        return Err(VfsError::PermissionDenied);
    }

    let mut contents = if flags & O_APPEND != 0 && exists {
        fs.read(rel)?
    } else {
        Vec::new()
    };

    if flags & O_TRUNC != 0 {
        contents.clear();
    }

    if flags & O_APPEND != 0 {
        contents.extend_from_slice(data);
    } else {
        contents = data.to_vec();
    }

    fs.write(rel, &contents)
}

/// Returns metadata for the file at `path`.
pub fn vfs_stat(path: &str) -> Result<FileStat, VfsError> {
    let (point, rel) = split_path(path)?;
    let v = VFS.lock();
    v.find_mount(point)
        .ok_or(VfsError::NotMounted)
        .and_then(|(_, e)| e.fs.stat(rel))
}

/// Returns `true` if the file at `path` exists and can be stat'd.
pub fn vfs_file_exists(path: &str) -> bool {
    vfs_stat(path).is_ok()
}

/// Returns the directory entries under `path`.
pub fn vfs_readdir(path: &str) -> Result<Vec<DirEntry>, VfsError> {
    let (point, rel) = split_path(path)?;
    let v = VFS.lock();
    v.find_mount(point)
        .ok_or(VfsError::NotMounted)
        .and_then(|(_, e)| e.fs.readdir(rel))
}

/// Opens `path` and installs it into the global FD table, returning the assigned descriptor number.
pub fn fd_open(path: &str, flags: u32, mode: u32) -> Result<u32, VfsError> {
    let node = vfs_open_node(path, flags, mode)?;
    let fd = FD { node, offset: 0, flags };
    let mut table = FD_TABLE.lock();
    let idx = table.iter().skip(3).position(|e| e.is_none())
        .map(|i| i + 3)
        .ok_or(VfsError::NoSpace)?;
    table[idx] = Some(fd);
    Ok(idx as u32)
}

/// Closes the file descriptor `fd`, freeing its slot; refuses to close stdio descriptors 0–2.
pub fn fd_close(fd: u32) -> Result<(), VfsError> {
    if fd < 3 {
        return Err(VfsError::PermissionDenied);
    }
    let mut table = FD_TABLE.lock();
    match table.get_mut(fd as usize) {
        Some(slot) if slot.is_some() => { *slot = None; Ok(()) }
        _ => Err(VfsError::FdNotFound),
    }
}

/// Reads up to `buf.len()` bytes from `fd` at its current offset, advancing it by the number of bytes copied.
pub fn fd_read(fd: u32, buf: &mut [u8]) -> Result<usize, VfsError> {
    let (node, offset) = {
        let table = FD_TABLE.lock();
        let entry = table.get(fd as usize)
            .and_then(|e| e.as_ref())
            .ok_or(VfsError::FdNotFound)?;
        if (entry.flags & 0x3) == O_WRONLY {
            return Err(VfsError::PermissionDenied);
        }
        (entry.node.clone(), entry.offset)
    };

    let data = node.read()?;
    let start = offset as usize;
    let available = data.len().saturating_sub(start);
    let to_copy = buf.len().min(available);
    buf[..to_copy].copy_from_slice(&data[start..start + to_copy]);

    {
        let mut table = FD_TABLE.lock();
        if let Some(Some(entry)) = table.get_mut(fd as usize) {
            entry.offset += to_copy as u64;
        }
    }

    Ok(to_copy)
}

/// Writes `data` to `fd`, advancing the offset (or moving it to end-of-file for `O_APPEND`).
pub fn fd_write(fd: u32, data: &[u8]) -> Result<usize, VfsError> {
    let (node, offset, flags) = {
        let table = FD_TABLE.lock();
        let entry = table.get(fd as usize)
            .and_then(|e| e.as_ref())
            .ok_or(VfsError::FdNotFound)?;
        if (entry.flags & 0x3) == O_RDONLY {
            return Err(VfsError::PermissionDenied);
        }
        (entry.node.clone(), entry.offset, entry.flags)
    };

    node.write(data)?;

    {
        let mut table = FD_TABLE.lock();
        if let Some(Some(entry)) = table.get_mut(fd as usize) {
            if flags & O_APPEND != 0 {
                let size = entry.node.stat().map(|s| s.size).unwrap_or(0);
                entry.offset = size;
            } else {
                entry.offset = offset + data.len() as u64;
            }
        }
    }

    Ok(data.len())
}