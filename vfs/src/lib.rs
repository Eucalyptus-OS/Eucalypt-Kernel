#![no_std]
/// Virtual Filesystem Layer
///
/// Provides a unified interface over multiple concrete filesystem implementations.
/// Filesystems are mounted at string mount-points (e.g. `"hda"`, `"ram"`), and every
/// VFS call accepts a path of the form `"<mountpoint>/<filename>"`.
///
/// Path format: `hda/README.TXT` → mount-point `"hda"`, filename `"README.TXT"`.
/// A path with no `/` separator is rejected with `Err("Invalid path")`.
///
/// Nodes are stable numeric handles (`u32`) returned by [`vfs_open`]. Read/write
/// ops accept either a path string or an open node ID via the [`VfsTarget`] enum.

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

/// Volume statistics returned by [`FileSystem::stat_fs`].
pub struct FsInfo {
    pub total_bytes: u64,
    pub free_bytes: u64,
    /// Human-readable tag, e.g. `"FAT12"` or `"RAMFS"`.
    pub fs_type: &'static str,
}

/// A single directory entry returned by [`FileSystem::list_dir`].
#[derive(Clone, Debug)]
pub struct VfsDirEntry {
    pub name: String,
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
///
/// Nodes are created by [`vfs_open`] and released by [`vfs_close`].
/// The `id` is unique for the lifetime of the node and is never reused
/// until after the node is closed.
#[derive(Clone, Debug)]
pub struct VfsNode {
    /// Unique node ID (inode-style). Use this with [`VfsTarget::Node`].
    pub id: u32,
    pub kind: NodeKind,
    /// Mount-point the node belongs to.
    pub mount: &'static str,
    /// Filename within the mount (no mount-point prefix).
    pub name: String,
    /// Cached file size in bytes; `0` for directories.
    pub size: u32,
    /// Byte offset for sequential reads/writes via this node.
    pub cursor: u32,
}

/// Selects whether a VFS I/O operation targets a path string or an open node.
pub enum VfsTarget<'a> {
    Path(&'a str),
    Node(u32),
}

/// Every VFS backend implements this trait.
///
/// Methods receive the filename portion of the path only — the mount-point
/// prefix is stripped before dispatch. Stored as `Box<dyn FileSystem>` so
/// the trait must be object-safe.
pub trait FileSystem {
    /// Reads a file and returns its raw contents.
    fn read_file(&self, filename: &str) -> Result<Vec<u8>, &'static str>;

    /// Creates a new file with the supplied contents. Fails if it already exists.
    fn create_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str>;

    /// Overwrites an existing file. Use [`create_file`] for new files.
    fn write_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str>;

    /// Appends `data` to the end of an existing file.
    fn append_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str>;

    fn delete_file(&self, filename: &str) -> Result<(), &'static str>;

    fn rename_file(&self, old_name: &str, new_name: &str) -> Result<(), &'static str>;

    fn file_exists(&self, filename: &str) -> bool;

    /// Returns file size in bytes, or `None` if not found.
    fn get_file_size(&self, filename: &str) -> Option<u32>;

    /// Lists all visible entries in the root directory.
    fn list_dir(&self) -> Result<Vec<VfsDirEntry>, &'static str>;

    /// May return `Err("Unsupported")` if not implemented.
    fn create_dir(&self, dirname: &str) -> Result<(), &'static str>;

    /// May return `Err("Unsupported")` if not implemented.
    fn delete_dir(&self, dirname: &str) -> Result<(), &'static str>;

    fn stat_fs(&self) -> FsInfo;
}

/// VFS adapter wrapping the static FAT12 driver.
///
/// The `drive` field is informational only — FAT12 uses process-global state.
/// Call [`fat12_init`] before mounting.
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
///
/// No subdirectory support. Useful as a `tmpfs`-style scratch mount or
/// for testing the VFS layer without real hardware.
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
            .map(|f| VfsDirEntry { name: f.name.clone(), is_dir: false, size: f.data.len() as u32 })
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
const MAX_NODES: usize = 64;

struct MountEntry {
    point: &'static str,
    fs: Box<dyn FileSystem>,
}

struct MountTable(UnsafeCell<Option<Vec<MountEntry>>>);
unsafe impl Sync for MountTable {}

struct NodeTable(UnsafeCell<Option<Vec<VfsNode>>>);
unsafe impl Sync for NodeTable {}

static VFS_LOCK: AtomicBool = AtomicBool::new(false);
static MOUNT_TABLE: MountTable = MountTable(UnsafeCell::new(None));
static NODE_TABLE: NodeTable = NodeTable(UnsafeCell::new(None));
static NEXT_NODE_ID: AtomicU32 = AtomicU32::new(1);

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

fn split_path(path: &str) -> Result<(&str, &str), &'static str> {
    let slash = path.find('/').ok_or("Invalid path: missing mount-point separator")?;
    let mount = &path[..slash];
    let file = &path[slash + 1..];
    if mount.is_empty() || file.is_empty() {
        return Err("Invalid path: empty mount-point or filename");
    }
    Ok((mount, file))
}

/// Initialises the VFS subsystem. Safe to call multiple times — subsequent calls are no-ops.
pub fn vfs_init() {
    vfs_lock();
    if mounts().is_none() {
        *mounts() = Some(Vec::new());
    }
    if nodes().is_none() {
        *nodes() = Some(Vec::new());
    }
    vfs_unlock();
}

/// Mounts a filesystem driver at `mount_point`.
///
/// `mount_point` must be a `'static` string literal. Returns `Err` if the
/// table is full or the mount point is already in use.
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

/// Unmounts the filesystem at `mount_point`, dropping the driver.
///
/// Any open nodes on this mount point become dangling — close them first.
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

/// Opens a file or directory, returning a [`VfsNode`] handle.
///
/// The returned `id` is stable until [`vfs_close`] is called. Use it with
/// [`VfsTarget::Node`] to avoid repeated path parsing on hot paths.
pub fn vfs_open(path: &str) -> Result<VfsNode, &'static str> {
    let (mount, name) = split_path(path)?;
    vfs_lock();
    let result = (|| {
        let ns = nodes().as_mut().ok_or("VFS not initialised")?;
        if ns.len() >= MAX_NODES {
            return Err("Node table full");
        }
        let fs = get_fs(mount)?;

        // determine kind and size from the filesystem
        let (kind, size) = if fs.file_exists(name) {
            let sz = fs.get_file_size(name).unwrap_or(0);
            (NodeKind::File, sz)
        } else {
            // check if it appears as a directory entry
            let entries = fs.list_dir().unwrap_or_default();
            let dir_entry = entries.iter().find(|e| e.name.eq_ignore_ascii_case(name) && e.is_dir);
            if dir_entry.is_some() {
                (NodeKind::Dir, 0)
            } else {
                return Err("File not found");
            }
        };

        let id = NEXT_NODE_ID.fetch_add(1, Ordering::Relaxed);
        let node = VfsNode {
            id,
            kind,
            mount: unsafe {
                // SAFETY: mount was obtained from a `&'static str` in the mount table
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
            cursor: 0,
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

/// Sets the cursor position on an open node for sequential I/O.
pub fn vfs_seek(node_id: u32, offset: u32) -> Result<(), &'static str> {
    vfs_lock();
    let result = get_node(node_id).map(|n| {
        n.cursor = offset.min(n.size);
    });
    vfs_unlock();
    result
}

/// Lists all currently open nodes.
pub fn vfs_list_nodes() -> Vec<VfsNode> {
    vfs_lock();
    let result = nodes()
        .as_ref()
        .map(|ns| ns.clone())
        .unwrap_or_default();
    vfs_unlock();
    result
}

/// Reads a file. `path` must be `"<mountpoint>/<filename>"`.
pub fn vfs_read_file(path: &str) -> Result<Vec<u8>, &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.read_file(file);
    vfs_unlock();
    result
}

/// Reads from an open node starting at its current cursor position.
///
/// Advances the cursor by the number of bytes returned.
pub fn vfs_read_node(node_id: u32) -> Result<Vec<u8>, &'static str> {
    vfs_lock();
    let result = (|| {
        let node = get_node(node_id)?;
        if node.kind != NodeKind::File {
            return Err("Node is not a file");
        }
        let fs = get_fs(node.mount)?;
        let data = fs.read_file(&node.name.clone())?;
        let start = node.cursor as usize;
        let slice = if start < data.len() { data[start..].to_vec() } else { Vec::new() };
        node.cursor = node.size; // advance to end
        Ok(slice)
    })();
    vfs_unlock();
    result
}

/// Creates a new file. `path` must be `"<mountpoint>/<filename>"`.
pub fn vfs_create_file(path: &str, data: &[u8]) -> Result<(), &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.create_file(file, data);
    vfs_unlock();
    result
}

/// Overwrites an existing file. The file must already exist.
pub fn vfs_write_file(path: &str, data: &[u8]) -> Result<(), &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.write_file(file, data);
    vfs_unlock();
    result
}

/// Writes to an open node, overwriting from the current cursor position.
///
/// Updates the node's cached size and advances the cursor.
pub fn vfs_write_node(node_id: u32, data: &[u8]) -> Result<(), &'static str> {
    vfs_lock();
    let result = (|| {
        let node = get_node(node_id)?;
        if node.kind != NodeKind::File {
            return Err("Node is not a file");
        }
        let mount = node.mount;
        let name = node.name.clone();
        let cursor = node.cursor;
        let fs = get_fs(mount)?;

        if cursor == 0 {
            // write from start: replace entire file
            fs.write_file(&name, data)?;
        } else {
            // partial write: read-modify-write
            let mut existing = fs.read_file(&name)?;
            let end = cursor as usize + data.len();
            if end > existing.len() {
                existing.resize(end, 0);
            }
            existing[cursor as usize..end].copy_from_slice(data);
            fs.write_file(&name, &existing)?;
        }

        let node = get_node(node_id)?;
        node.size = fs.get_file_size(&node.name.clone()).unwrap_or(node.size);
        node.cursor += data.len() as u32;
        Ok(())
    })();
    vfs_unlock();
    result
}

/// Appends data to an existing file.
pub fn vfs_append_file(path: &str, data: &[u8]) -> Result<(), &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.append_file(file, data);
    vfs_unlock();
    result
}

/// Deletes a file.
pub fn vfs_delete_file(path: &str) -> Result<(), &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.delete_file(file);
    vfs_unlock();
    result
}

/// Renames a file. Both paths must share the same mount point.
///
/// For cross-mount moves, use read + create + delete manually.
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

/// Returns the size of `path` in bytes, or `None` if not found.
pub fn vfs_get_file_size(path: &str) -> Option<u32> {
    let (mount, file) = split_path(path).ok()?;
    vfs_lock();
    let result = get_fs(mount).ok()?.get_file_size(file);
    vfs_unlock();
    result
}

/// Lists the root directory of a mounted filesystem.
///
/// `mount_point` is the label only, no trailing `/`.
pub fn vfs_list_dir(mount_point: &str) -> Result<Vec<VfsDirEntry>, &'static str> {
    vfs_lock();
    let result = get_fs(mount_point)?.list_dir();
    vfs_unlock();
    result
}

/// Creates a subdirectory. `path` is e.g. `"hda/SAVES"`.
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