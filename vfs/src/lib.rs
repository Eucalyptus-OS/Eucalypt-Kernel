#![no_std]

extern crate alloc;

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

pub use fat12::{
    fat12_append_file, fat12_create_directory, fat12_create_file, fat12_delete_directory,
    fat12_delete_file, fat12_file_exists, fat12_get_attributes, fat12_get_file_size,
    fat12_init, fat12_list_entries, fat12_list_files, fat12_read_file, fat12_rename_file,
    fat12_stat, fat12_write_file, DirectoryEntry,
};

/// Open for reading only.
pub const O_RDONLY: u32 = 0x0000;
/// Open for writing only.
pub const O_WRONLY: u32 = 0x0001;
/// Open for reading and writing.
pub const O_RDWR:   u32 = 0x0002;
/// Create file if it doesn't exist.
pub const O_CREAT:  u32 = 0x0040;
/// Truncate file to zero length on open.
pub const O_TRUNC:  u32 = 0x0200;
/// Writes always append to end of file.
pub const O_APPEND: u32 = 0x0400;
/// Fail if the file already exists (requires O_CREAT).
pub const O_EXCL:   u32 = 0x0800;

/// User read permission.
pub const S_IRUSR: u32 = 0o400;
/// User write permission.
pub const S_IWUSR: u32 = 0o200;
/// User execute permission.
pub const S_IXUSR: u32 = 0o100;
/// Group read permission.
pub const S_IRGRP: u32 = 0o040;
/// Group write permission.
pub const S_IWGRP: u32 = 0o020;
/// Other read permission.
pub const S_IROTH: u32 = 0o004;
/// Convenience mode: rw-r--r--
pub const S_IFREG: u32 = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

/// VFS node ID reserved for the process stdin stream.
pub const STDIN_NODE_ID:  u32 = 1;
/// VFS node ID reserved for the process stdout stream.
pub const STDOUT_NODE_ID: u32 = 2;
/// VFS node ID reserved for the process stderr stream.
pub const STDERR_NODE_ID: u32 = 3;

/// Volume statistics returned by [`FileSystem::stat_fs`].
pub struct FsInfo {
    pub total_bytes: u64,
    pub free_bytes:  u64,
    /// Human-readable tag, e.g. `"FAT12"` or `"RAMFS"`.
    pub fs_type: &'static str,
}

/// A single directory entry returned by [`FileSystem::list_dir`].
#[derive(Clone, Debug)]
pub struct VfsDirEntry {
    pub name:   String,
    pub is_dir: bool,
    /// `0` for directories.
    pub size: u32,
}

/// The kind of resource a [`VfsNode`] represents.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NodeKind {
    File,
    Dir,
}

/// A stable handle to an open file or directory.
#[derive(Clone, Debug)]
pub struct VfsNode {
    /// Unique node ID. Pass to [`VfsTarget::Node`] to skip path parsing.
    pub id:     u32,
    pub kind:   NodeKind,
    /// Mount-point this node belongs to.
    pub mount:  &'static str,
    /// Filename within the mount (no mount-point prefix).
    pub name:   String,
    /// Cached file size in bytes; `0` for directories.
    pub size:   u32,
    /// Byte offset for sequential reads/writes.
    pub cursor: u32,
    /// Flags this node was opened with (e.g. [`O_RDWR`]).
    pub flags:  u32,
    /// Mode bits supplied at creation time.
    pub mode:   u32,
}

/// A kernel file descriptor stored in each process's per-process FD table.
///
/// Node IDs 1–3 are reserved for stdin, stdout, and stderr and are never
/// backed by a real VFS node. Node ID 0 means the slot is empty/closed.
#[derive(Clone, Copy, Debug)]
pub struct FD {
    /// Backing VFS node ID. `0` means this slot is empty/closed.
    pub node_id:   u32,
    /// Open flags (e.g. [`O_RDWR`]).
    pub flags:     u32,
    /// Number of file descriptors in this process sharing this VFS node.
    /// Reaches 0 only via `dup`/`dup2` semantics; starts at 1 on `open`.
    pub ref_count: u32,
}

impl FD {
    pub const EMPTY: Self = Self { node_id: 0, flags: 0, ref_count: 0 };

    /// Returns `true` when this descriptor slot is not in use.
    #[inline]
    pub fn is_empty(&self) -> bool { self.node_id == 0 }

    /// Returns `true` for the three reserved pseudo-descriptors (stdin/stdout/stderr).
    /// These are never closed through the VFS node table.
    #[inline]
    pub fn is_special(&self) -> bool {
        self.node_id >= STDIN_NODE_ID && self.node_id <= STDERR_NODE_ID
    }
}

/// Selects whether a VFS I/O operation targets a path or an open node.
pub enum VfsTarget<'a> {
    Path(&'a str),
    Node(u32),
}

/// Backend trait every VFS driver must implement.
/// Receives the filename portion only — mount-point prefix is stripped before dispatch.
pub trait FileSystem {
    fn read_file(&self, filename: &str) -> Result<Vec<u8>, &'static str>;
    fn create_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str>;
    fn write_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str>;
    fn append_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str>;
    fn delete_file(&self, filename: &str) -> Result<(), &'static str>;
    fn rename_file(&self, old_name: &str, new_name: &str) -> Result<(), &'static str>;
    fn file_exists(&self, filename: &str) -> bool;
    fn get_file_size(&self, filename: &str) -> Option<u32>;
    fn list_dir(&self) -> Result<Vec<VfsDirEntry>, &'static str>;
    fn create_dir(&self, dirname: &str) -> Result<(), &'static str>;
    fn delete_dir(&self, dirname: &str) -> Result<(), &'static str>;
    fn stat_fs(&self) -> FsInfo;
}

/// VFS adapter for the static FAT12 driver. Call [`fat12_init`] before mounting.
pub struct Fat12Driver {
    pub drive: usize,
}

impl Fat12Driver {
    pub fn new(drive: usize) -> Self {
        Fat12Driver { drive }
    }
}

impl FileSystem for Fat12Driver {
    fn read_file(&self, filename: &str) -> Result<Vec<u8>, &'static str> {
        fat12_read_file(filename)
    }
    fn create_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str> {
        fat12_create_file(filename, data)
    }
    fn write_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str> {
        fat12_write_file(filename, data)
    }
    fn append_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str> {
        fat12_append_file(filename, data)
    }
    fn delete_file(&self, filename: &str) -> Result<(), &'static str> {
        fat12_delete_file(filename)
    }
    fn rename_file(&self, old_name: &str, new_name: &str) -> Result<(), &'static str> {
        fat12_rename_file(old_name, new_name)
    }
    fn file_exists(&self, filename: &str) -> bool {
        fat12_file_exists(filename)
    }
    fn get_file_size(&self, filename: &str) -> Option<u32> {
        fat12_get_file_size(filename)
    }
    fn list_dir(&self) -> Result<Vec<VfsDirEntry>, &'static str> {
        let entries = fat12_list_entries()?;
        Ok(entries
            .into_iter()
            .filter_map(|e| {
                e.get_name().ok().map(|name| VfsDirEntry {
                    is_dir: e.is_directory(),
                    size: e.file_size,
                    name,
                })
            })
            .collect())
    }
    fn create_dir(&self, dirname: &str) -> Result<(), &'static str> {
        fat12_create_directory(dirname)
    }
    fn delete_dir(&self, dirname: &str) -> Result<(), &'static str> {
        fat12_delete_directory(dirname)
    }
    fn stat_fs(&self) -> FsInfo {
        let (total, free) = fat12_stat();
        FsInfo { total_bytes: total, free_bytes: free, fs_type: "FAT12" }
    }
}

/// Volatile heap-backed filesystem. All data is lost on reboot.
/// No subdirectory support. Useful as a tmpfs-style scratch mount.
pub struct RamFs {
    files: spin::Mutex<Vec<RamFile>>,
}

struct RamFile {
    name: String,
    data: Vec<u8>,
}

impl RamFs {
    pub fn new() -> Self {
        RamFs { files: spin::Mutex::new(Vec::new()) }
    }
}

impl FileSystem for RamFs {
    fn read_file(&self, filename: &str) -> Result<Vec<u8>, &'static str> {
        let files = self.files.lock();
        files
            .iter()
            .find(|f| f.name.eq_ignore_ascii_case(filename))
            .map(|f| f.data.clone())
            .ok_or("File not found")
    }
    fn create_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str> {
        let mut files = self.files.lock();
        if files.iter().any(|f| f.name.eq_ignore_ascii_case(filename)) {
            return Err("File already exists");
        }
        files.push(RamFile { name: String::from(filename), data: data.to_vec() });
        Ok(())
    }
    fn write_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str> {
        let mut files = self.files.lock();
        files
            .iter_mut()
            .find(|f| f.name.eq_ignore_ascii_case(filename))
            .map(|f| f.data = data.to_vec())
            .ok_or("File not found")
    }
    fn append_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str> {
        let mut files = self.files.lock();
        files
            .iter_mut()
            .find(|f| f.name.eq_ignore_ascii_case(filename))
            .map(|f| f.data.extend_from_slice(data))
            .ok_or("File not found")
    }
    fn delete_file(&self, filename: &str) -> Result<(), &'static str> {
        let mut files = self.files.lock();
        let before = files.len();
        files.retain(|f| !f.name.eq_ignore_ascii_case(filename));
        if files.len() == before { Err("File not found") } else { Ok(()) }
    }
    fn rename_file(&self, old_name: &str, new_name: &str) -> Result<(), &'static str> {
        let mut files = self.files.lock();
        if files.iter().any(|f| f.name.eq_ignore_ascii_case(new_name)) {
            return Err("Destination filename already exists");
        }
        files
            .iter_mut()
            .find(|f| f.name.eq_ignore_ascii_case(old_name))
            .map(|f| f.name = String::from(new_name))
            .ok_or("File not found")
    }
    fn file_exists(&self, filename: &str) -> bool {
        self.files.lock().iter().any(|f| f.name.eq_ignore_ascii_case(filename))
    }
    fn get_file_size(&self, filename: &str) -> Option<u32> {
        self.files
            .lock()
            .iter()
            .find(|f| f.name.eq_ignore_ascii_case(filename))
            .map(|f| f.data.len() as u32)
    }
    fn list_dir(&self) -> Result<Vec<VfsDirEntry>, &'static str> {
        Ok(self
            .files
            .lock()
            .iter()
            .map(|f| VfsDirEntry {
                name:   f.name.clone(),
                is_dir: false,
                size:   f.data.len() as u32,
            })
            .collect())
    }
    fn create_dir(&self, _dirname: &str) -> Result<(), &'static str> {
        Err("RamFs: subdirectories not supported")
    }
    fn delete_dir(&self, _dirname: &str) -> Result<(), &'static str> {
        Err("RamFs: subdirectories not supported")
    }
    fn stat_fs(&self) -> FsInfo {
        let used: u64 = self.files.lock().iter().map(|f| f.data.len() as u64).sum();
        FsInfo { total_bytes: u64::MAX, free_bytes: u64::MAX - used, fs_type: "RAMFS" }
    }
}

const MAX_MOUNTS: usize = 8;
const MAX_NODES:  usize = 64;

struct MountEntry {
    point: &'static str,
    fs:    Box<dyn FileSystem>,
}

struct MountTable(UnsafeCell<Option<Vec<MountEntry>>>);
unsafe impl Sync for MountTable {}

struct NodeTable(UnsafeCell<Option<Vec<VfsNode>>>);
unsafe impl Sync for NodeTable {}

static VFS_LOCK:    AtomicBool = AtomicBool::new(false);
static MOUNT_TABLE: MountTable = MountTable(UnsafeCell::new(None));
static NODE_TABLE:  NodeTable  = NodeTable(UnsafeCell::new(None));
// Node IDs 1–3 are reserved for stdin/stdout/stderr; real nodes start at 4.
static NEXT_NODE_ID: AtomicU32 = AtomicU32::new(4);

fn vfs_lock() {
    while VFS_LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
}

fn vfs_unlock() {
    VFS_LOCK.store(false, Ordering::Release);
}

fn mounts() -> &'static mut Option<Vec<MountEntry>> {
    unsafe { &mut *MOUNT_TABLE.0.get() }
}

fn nodes() -> &'static mut Option<Vec<VfsNode>> {
    unsafe { &mut *NODE_TABLE.0.get() }
}

fn get_fs(mount_point: &str) -> Result<&'static dyn FileSystem, &'static str> {
    mounts()
        .as_ref()
        .ok_or("VFS not initialised")?
        .iter()
        .find(|e| e.point == mount_point)
        .map(|e| e.fs.as_ref())
        .ok_or("Mount point not found")
}

fn get_node(id: u32) -> Result<&'static mut VfsNode, &'static str> {
    nodes()
        .as_mut()
        .ok_or("VFS not initialised")?
        .iter_mut()
        .find(|n| n.id == id)
        .ok_or("Node not found")
}

/// Splits `"mount/filename"` into `("mount", "filename")`.
fn split_path(path: &str) -> Result<(&str, &str), &'static str> {
    let slash = path.find('/').ok_or("Invalid path: missing mount-point separator")?;
    let mount = &path[..slash];
    let file  = &path[slash + 1..];
    if mount.is_empty() || file.is_empty() {
        return Err("Invalid path: empty mount-point or filename");
    }
    Ok((mount, file))
}

/// Initialises the VFS. Safe to call multiple times — subsequent calls are no-ops.
pub fn vfs_init() {
    vfs_lock();
    if mounts().is_none() { *mounts() = Some(Vec::new()); }
    if nodes().is_none()  { *nodes()  = Some(Vec::new()); }
    vfs_unlock();
}

/// Mounts a filesystem driver at `mount_point` (must be a `'static` str).
pub fn vfs_mount(mount_point: &'static str, fs: Box<dyn FileSystem>) -> Result<(), &'static str> {
    vfs_lock();
    let result = (|| {
        let t = mounts().as_mut().ok_or("VFS not initialised")?;
        if t.iter().any(|e| e.point == mount_point) {
            return Err("Mount point already in use");
        }
        if t.len() >= MAX_MOUNTS {
            return Err("Mount table full");
        }
        t.push(MountEntry { point: mount_point, fs });
        Ok(())
    })();
    vfs_unlock();
    result
}

/// Unmounts the filesystem at `mount_point`. Close all open nodes first.
pub fn vfs_unmount(mount_point: &str) -> Result<(), &'static str> {
    vfs_lock();
    let result = (|| {
        let t = mounts().as_mut().ok_or("VFS not initialised")?;
        let before = t.len();
        t.retain(|e| e.point != mount_point);
        if t.len() == before { Err("Mount point not found") } else { Ok(()) }
    })();
    vfs_unlock();
    result
}

/// Opens a file or directory. `flags` controls access and creation (see `O_*`). `mode` sets
/// permission bits when creating a file (see `S_*`). Returns a [`VfsNode`] for subsequent I/O.
pub fn vfs_open(path: &str, flags: u32, mode: u32) -> Result<VfsNode, &'static str> {
    let (mount, name) = split_path(path)?;
    vfs_lock();
    let result = (|| {
        let ns = nodes().as_mut().ok_or("VFS not initialised")?;
        if ns.len() >= MAX_NODES {
            return Err("Node table full");
        }
        let fs = get_fs(mount)?;

        let file_exists = fs.file_exists(name);

        if flags & O_CREAT != 0 && flags & O_EXCL != 0 && file_exists {
            return Err("File already exists");
        }

        if flags & O_CREAT != 0 && !file_exists {
            fs.create_file(name, &[])?;
        }

        if flags & O_TRUNC != 0 && file_exists {
            fs.write_file(name, &[])?;
        }

        let (kind, size) = if fs.file_exists(name) {
            let sz = fs.get_file_size(name).unwrap_or(0);
            (NodeKind::File, sz)
        } else {
            let entries = fs.list_dir().unwrap_or_default();
            if entries.iter().any(|e| e.name.eq_ignore_ascii_case(name) && e.is_dir) {
                (NodeKind::Dir, 0)
            } else {
                return Err("File not found");
            }
        };

        let cursor = if flags & O_APPEND != 0 { size } else { 0 };

        let id = NEXT_NODE_ID.fetch_add(1, Ordering::Relaxed);
        let node = VfsNode {
            id,
            kind,
            mount: unsafe {
                core::mem::transmute::<&str, &'static str>(
                    mounts()
                        .as_ref()
                        .unwrap()
                        .iter()
                        .find(|e| e.point == mount)
                        .unwrap()
                        .point,
                )
            },
            name: String::from(name),
            size,
            cursor,
            flags,
            mode,
        };
        ns.push(node.clone());
        Ok(node)
    })();
    vfs_unlock();
    result
}

/// Closes a node by ID, removing it from the node table.
pub fn vfs_close(node_id: u32) -> Result<(), &'static str> {
    vfs_lock();
    let result = (|| {
        let ns = nodes().as_mut().ok_or("VFS not initialised")?;
        let before = ns.len();
        ns.retain(|n| n.id != node_id);
        if ns.len() == before { Err("Node not found") } else { Ok(()) }
    })();
    vfs_unlock();
    result
}

/// Returns a snapshot of an open node's current state.
pub fn vfs_node_stat(node_id: u32) -> Result<VfsNode, &'static str> {
    vfs_lock();
    let result = get_node(node_id).map(|n| n.clone());
    vfs_unlock();
    result
}

/// Sets the cursor position on an open node. Clamped to file size.
pub fn vfs_seek(node_id: u32, offset: u32) -> Result<(), &'static str> {
    vfs_lock();
    let result = get_node(node_id).map(|n| { n.cursor = offset.min(n.size); });
    vfs_unlock();
    result
}

/// Lists all currently open nodes.
pub fn vfs_list_nodes() -> Vec<VfsNode> {
    vfs_lock();
    let result = nodes().as_ref().map(|ns| ns.clone()).unwrap_or_default();
    vfs_unlock();
    result
}

/// Reads a file by path.
pub fn vfs_read_file(path: &str) -> Result<Vec<u8>, &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.read_file(file);
    vfs_unlock();
    result
}

/// Reads up to `max_count` bytes from an open node at its cursor position.
/// Advances the cursor by the number of bytes actually read.
/// Fails if the node was opened write-only.
pub fn vfs_read(node_id: u32, max_count: usize) -> Result<Vec<u8>, &'static str> {
    vfs_lock();
    let result = (|| {
        let node = get_node(node_id)?;
        if node.kind != NodeKind::File {
            return Err("Node is not a file");
        }
        let access = node.flags & 0x3;
        if access == O_WRONLY {
            return Err("Node not open for reading");
        }
        let fs    = get_fs(node.mount)?;
        let data  = fs.read_file(&node.name.clone())?;
        let start = node.cursor as usize;
        let avail = data.len().saturating_sub(start);
        let count = avail.min(max_count);
        let slice = if count > 0 { data[start..start + count].to_vec() } else { Vec::new() };
        node.cursor += count as u32;
        Ok(slice)
    })();
    vfs_unlock();
    result
}

/// Reads from an open node starting at its cursor all the way to EOF.
/// Advances the cursor to the end of the file.
/// Fails if the node was opened write-only.
pub fn vfs_read_node(node_id: u32) -> Result<Vec<u8>, &'static str> {
    vfs_read(node_id, usize::MAX)
}

/// Creates a new file by path.
pub fn vfs_create_file(path: &str, data: &[u8]) -> Result<(), &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.create_file(file, data);
    vfs_unlock();
    result
}

/// Overwrites an existing file by path.
pub fn vfs_write_file(path: &str, data: &[u8]) -> Result<(), &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.write_file(file, data);
    vfs_unlock();
    result
}

/// Writes to an open node at its cursor position.
/// Fails if the node was opened read-only.
/// Honours [`O_APPEND`] by always writing at end of file.
pub fn vfs_write_node(node_id: u32, data: &[u8]) -> Result<(), &'static str> {
    vfs_lock();
    let result = (|| {
        let node = get_node(node_id)?;
        if node.kind != NodeKind::File {
            return Err("Node is not a file");
        }
        let access = node.flags & 0x3;
        if access == O_RDONLY {
            return Err("Node not open for writing");
        }
        let mount  = node.mount;
        let name   = node.name.clone();
        let append = node.flags & O_APPEND != 0;
        let cursor = if append { node.size } else { node.cursor };
        let fs     = get_fs(mount)?;

        if cursor == 0 {
            fs.write_file(&name, data)?;
        } else {
            let mut existing = fs.read_file(&name)?;
            let end = cursor as usize + data.len();
            if end > existing.len() { existing.resize(end, 0); }
            existing[cursor as usize..end].copy_from_slice(data);
            fs.write_file(&name, &existing)?;
        }

        let node = get_node(node_id)?;
        node.size   = fs.get_file_size(&node.name.clone()).unwrap_or(node.size);
        node.cursor = if append { node.size } else { node.cursor + data.len() as u32 };
        Ok(())
    })();
    vfs_unlock();
    result
}

/// Appends data to an existing file by path.
pub fn vfs_append_file(path: &str, data: &[u8]) -> Result<(), &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.append_file(file, data);
    vfs_unlock();
    result
}

/// Deletes a file by path.
pub fn vfs_delete_file(path: &str) -> Result<(), &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.delete_file(file);
    vfs_unlock();
    result
}

/// Renames a file. Both paths must share the same mount point.
pub fn vfs_rename_file(old_path: &str, new_path: &str) -> Result<(), &'static str> {
    let (old_mount, old_file) = split_path(old_path)?;
    let (new_mount, new_file) = split_path(new_path)?;
    if old_mount != new_mount {
        return Err("Cross-mount rename not supported");
    }
    vfs_lock();
    let result = get_fs(old_mount)?.rename_file(old_file, new_file);
    vfs_unlock();
    result
}

/// Returns `true` if `path` refers to an existing file.
pub fn vfs_file_exists(path: &str) -> bool {
    let Ok((mount, file)) = split_path(path) else { return false };
    vfs_lock();
    let result = get_fs(mount).map(|fs| fs.file_exists(file)).unwrap_or(false);
    vfs_unlock();
    result
}

/// Returns the size of a file in bytes, or `None` if not found.
pub fn vfs_get_file_size(path: &str) -> Option<u32> {
    let (mount, file) = split_path(path).ok()?;
    vfs_lock();
    let result = get_fs(mount).ok()?.get_file_size(file);
    vfs_unlock();
    result
}

/// Lists the root directory of a mounted filesystem. `mount_point` has no trailing `/`.
pub fn vfs_list_dir(mount_point: &str) -> Result<Vec<VfsDirEntry>, &'static str> {
    vfs_lock();
    let result = get_fs(mount_point)?.list_dir();
    vfs_unlock();
    result
}

/// Creates a subdirectory, e.g. `"hda/SAVES"`.
pub fn vfs_create_dir(path: &str) -> Result<(), &'static str> {
    let (mount, dir) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.create_dir(dir);
    vfs_unlock();
    result
}

/// Deletes an empty subdirectory.
pub fn vfs_delete_dir(path: &str) -> Result<(), &'static str> {
    let (mount, dir) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.delete_dir(dir);
    vfs_unlock();
    result
}

/// Returns volume statistics for a mounted filesystem.
pub fn vfs_stat(mount_point: &str) -> Result<FsInfo, &'static str> {
    vfs_lock();
    let result = Ok(get_fs(mount_point)?.stat_fs());
    vfs_unlock();
    result
}

/// Returns all mounted filesystems as `(mount_point, fs_type)` pairs.
pub fn vfs_list_mounts() -> Vec<(&'static str, &'static str)> {
    vfs_lock();
    let result = mounts()
        .as_ref()
        .map(|t| t.iter().map(|e| (e.point, e.fs.stat_fs().fs_type)).collect())
        .unwrap_or_default();
    vfs_unlock();
    result
}