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

// A: is always the ramdisk, B: onwards are physical drives
pub const RAMDISK_DRIVE: char = 'A';

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NodeKind { File, Dir }

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
    // create a placeholder fd for stdin/stdout/stderr slots
    pub fn new(fd_num: u64, flags: u32) -> Self {
        FD {
            node: VfsNode {
                drive: RAMDISK_DRIVE,
                rel:   String::new(),
                flags,
            },
            offset: fd_num,
            flags,
        }
    }

    // clear this fd's slot in the global table
    pub fn close(&self) {
        let fd_num = self.offset as u32;
        if fd_num < 3 { return; }
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
            VfsError::FdNotFound       => "fd not found",
        }
    }
}

impl core::fmt::Display for VfsError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{}", self.as_str())
    }
}

struct DriveEntry {
    letter: char,
    fs:     Box<dyn FileSystem>,
}

struct Vfs {
    drives: Vec<DriveEntry>,
}

impl Vfs {
    const fn new() -> Self {
        Vfs { drives: Vec::new() }
    }

    // find a mounted drive by letter, case-insensitive
    fn find_drive(&self, letter: char) -> Option<&DriveEntry> {
        let letter = letter.to_ascii_uppercase();
        self.drives.iter().find(|d| d.letter == letter)
    }

    // find the next free drive letter starting from B:
    fn next_free_letter(&self) -> Option<char> {
        for c in 'B'..='Z' {
            if self.find_drive(c).is_none() {
                return Some(c);
            }
        }
        None
    }
}

static VFS:      Mutex<Vfs>             = Mutex::new(Vfs::new());
static FD_TABLE: Mutex<Vec<Option<FD>>> = Mutex::new(Vec::new());

// parse "C:path/to/file" into ('C', "path/to/file")
fn split_drive_path(path: &str) -> Result<(char, &str), VfsError> {
    let mut chars = path.chars();
    let letter = chars.next().ok_or(VfsError::InvalidPath)?;
    if !letter.is_ascii_alphabetic() {
        return Err(VfsError::InvalidPath);
    }
    if chars.next() != Some(':') {
        return Err(VfsError::InvalidPath);
    }
    let rel = path[2..].trim_start_matches('/');
    if rel.is_empty() {
        return Err(VfsError::InvalidPath);
    }
    Ok((letter.to_ascii_uppercase(), rel))
}

// initialise the fd table with 256 slots, 0/1/2 reserved for stdin/stdout/stderr
pub fn vfs_init() {
    let mut table = FD_TABLE.lock();
    for _ in 0..256 {
        table.push(None);
    }
}

// mount a filesystem at a specific drive letter
pub fn vfs_mount(letter: char, fs: Box<dyn FileSystem>) -> Result<(), VfsError> {
    let letter = letter.to_ascii_uppercase();
    let mut v = VFS.lock();
    if v.find_drive(letter).is_some() {
        return Err(VfsError::AlreadyExists);
    }
    v.drives.push(DriveEntry { letter, fs });
    Ok(())
}

// mount a filesystem at the next free drive letter and return the letter assigned
pub fn vfs_automount(fs: Box<dyn FileSystem>) -> Result<char, VfsError> {
    let mut v = VFS.lock();
    let letter = v.next_free_letter().ok_or(VfsError::NoSpace)?;
    v.drives.push(DriveEntry { letter, fs });
    Ok(letter)
}

// unmount the drive at the given letter
pub fn vfs_unmount(letter: char) -> Result<(), VfsError> {
    let letter = letter.to_ascii_uppercase();
    let mut v = VFS.lock();
    let before = v.drives.len();
    v.drives.retain(|d| d.letter != letter);
    if v.drives.len() == before { Err(VfsError::NotMounted) } else { Ok(()) }
}

// return true if a drive letter has a filesystem mounted
pub fn vfs_is_mounted(letter: char) -> bool {
    VFS.lock().find_drive(letter).is_some()
}

#[derive(Clone, Debug)]
pub struct VfsNode {
    pub drive: char,
    pub rel:   String,
    pub flags: u32,
}

impl VfsNode {
    // read the full contents of the file this node points to
    pub fn read(&self) -> Result<Vec<u8>, VfsError> {
        let v = VFS.lock();
        let entry = v.find_drive(self.drive).ok_or(VfsError::NotMounted)?;
        entry.fs.read(&self.rel)
    }

    // write data to the file this node points to
    pub fn write(&self, data: &[u8]) -> Result<(), VfsError> {
        let v = VFS.lock();
        let entry = v.find_drive(self.drive).ok_or(VfsError::NotMounted)?;
        entry.fs.write(&self.rel, data)
    }

    // stat the file this node points to
    pub fn stat(&self) -> Result<FileStat, VfsError> {
        let v = VFS.lock();
        let entry = v.find_drive(self.drive).ok_or(VfsError::NotMounted)?;
        entry.fs.stat(&self.rel)
    }

    // return the full drive-letter path e.g. "A:font.psf"
    pub fn path(&self) -> String {
        format!("{}:{}", self.drive, self.rel)
    }
}

// open a VfsNode for a drive-letter path like "A:font.psf"
pub fn vfs_open_node(path: &str, flags: u32, mode: u32) -> Result<VfsNode, VfsError> {
    let (drive, rel) = split_drive_path(path)?;
    let v = VFS.lock();
    let entry = v.find_drive(drive).ok_or(VfsError::NotMounted)?;
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
    Ok(VfsNode { drive, rel: rel.to_string(), flags })
}

// stat a file by drive-letter path
pub fn vfs_stat(path: &str) -> Result<FileStat, VfsError> {
    let (drive, rel) = split_drive_path(path)?;
    let v = VFS.lock();
    let entry = v.find_drive(drive).ok_or(VfsError::NotMounted)?;
    entry.fs.stat(rel)
}

// read a file by drive-letter path, returns full contents
pub fn vfs_read(path: &str) -> Result<Vec<u8>, VfsError> {
    let (drive, rel) = split_drive_path(path)?;
    let v = VFS.lock();
    let entry = v.find_drive(drive).ok_or(VfsError::NotMounted)?;
    entry.fs.read(rel)
}

// write a file by drive-letter path respecting flags
pub fn vfs_write(path: &str, data: &[u8], flags: u32, mode: u32) -> Result<(), VfsError> {
    let (drive, rel) = split_drive_path(path)?;
    let v = VFS.lock();
    let entry = v.find_drive(drive).ok_or(VfsError::NotMounted)?;
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
    if flags & O_TRUNC != 0 { contents.clear(); }
    if flags & O_APPEND != 0 {
        contents.extend_from_slice(data);
    } else {
        contents = data.to_vec();
    }
    fs.write(rel, &contents)
}

// return true if the path exists on its drive
pub fn vfs_file_exists(path: &str) -> bool {
    vfs_stat(path).is_ok()
}

// list directory entries for a drive-letter path
pub fn vfs_readdir(path: &str) -> Result<Vec<DirEntry>, VfsError> {
    let (drive, rel) = split_drive_path(path)?;
    let v = VFS.lock();
    let entry = v.find_drive(drive).ok_or(VfsError::NotMounted)?;
    entry.fs.readdir(rel)
}

// open a path and allocate a file descriptor, returns the fd number
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

// release a file descriptor
pub fn fd_close(fd: u32) -> Result<(), VfsError> {
    if fd < 3 { return Err(VfsError::PermissionDenied); }
    let mut table = FD_TABLE.lock();
    match table.get_mut(fd as usize) {
        Some(slot) if slot.is_some() => { *slot = None; Ok(()) }
        _ => Err(VfsError::FdNotFound),
    }
}

// read up to buf.len() bytes from fd at its current offset, returns bytes copied
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
    let start     = offset as usize;
    let available = data.len().saturating_sub(start);
    let to_copy   = buf.len().min(available);
    buf[..to_copy].copy_from_slice(&data[start..start + to_copy]);
    {
        let mut table = FD_TABLE.lock();
        if let Some(Some(entry)) = table.get_mut(fd as usize) {
            entry.offset += to_copy as u64;
        }
    }
    Ok(to_copy)
}

// write data into fd at its current offset, returns bytes written
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

// convert a VfsError to a unix-style negative errno value
pub fn errno_from_vfs(err: VfsError) -> i64 {
    match err {
        VfsError::NotFound         => -2,
        VfsError::AlreadyExists    => -17,
        VfsError::NotAFile         => -21,
        VfsError::NotADir          => -20,
        VfsError::NotEmpty         => -39,
        VfsError::PermissionDenied => -13,
        VfsError::InvalidPath      => -22,
        VfsError::NotSupported     => -38,
        VfsError::NoSpace          => -28,
        VfsError::NotMounted       => -2,
        VfsError::FdNotFound       => -9,
        VfsError::IoError          => -5,
    }
}