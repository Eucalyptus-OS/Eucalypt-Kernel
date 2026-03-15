#![no_std]
/// Virtual Filesystem Layer
///
/// Provides a unified interface over multiple concrete filesystem implementations.
/// Filesystems are mounted at string mount-points (e.g. `"hda"`, `"ram"`), and every
/// VFS call accepts a path of the form `"<mountpoint>/<filename>"`.
///
/// # Path format
///
/// ```text
/// hda/README.TXT   ->  mount-point "hda", filename "README.TXT"
/// ram/SAVE.DAT     ->  mount-point "ram", filename "SAVE.DAT"
/// ```
///
/// A path with no `/` separator is rejected with `Err("Invalid path")`.
///
/// # Example
///
/// ```rust
/// vfs_init();
/// fat12_init(0).unwrap();
/// vfs_mount("hda", Box::new(Fat12Driver::new(0))).unwrap();
/// vfs_mount("ram", Box::new(RamFs::new())).unwrap();
///
/// vfs_write_file("hda/HELLO.TXT", b"hello world").unwrap();
/// let data = vfs_read_file("hda/HELLO.TXT").unwrap();
/// ```

extern crate alloc;

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, Ordering};

pub use fat12::{
    fat12_append_file, fat12_create_directory, fat12_create_file, fat12_delete_directory,
    fat12_delete_file, fat12_file_exists, fat12_get_attributes, fat12_get_file_size,
    fat12_init, fat12_list_entries, fat12_list_files, fat12_read_file, fat12_rename_file,
    fat12_stat, fat12_write_file, DirectoryEntry,
};

/// Metadata returned by [`FileSystem::stat_fs`].
pub struct FsInfo {
    /// Total capacity of the volume in bytes.
    pub total_bytes: u64,
    /// Number of bytes not yet allocated.
    pub free_bytes: u64,
    /// Human-readable filesystem type tag (e.g. `"FAT12"`, `"RAMFS"`).
    pub fs_type: &'static str,
}

/// Information about a single directory entry.
#[derive(Clone, Debug)]
pub struct VfsDirEntry {
    /// Decoded filename.
    pub name: String,
    /// `true` if the entry is a subdirectory.
    pub is_dir: bool,
    /// File size in bytes; `0` for directories.
    pub size: u32,
}

/// The core trait that every VFS backend must implement.
///
/// All methods receive the filename portion of the path only — the
/// mount-point prefix is stripped by the VFS before dispatch.
///
/// Implementors are stored as `Box<dyn FileSystem>` inside the mount table,
/// so the trait must be object-safe (no generic methods, no `Self`-returning
/// methods).
pub trait FileSystem {
    /// Reads a file by name and returns its raw contents.
    fn read_file(&self, filename: &str) -> Result<Vec<u8>, &'static str>;

    /// Creates a new file with the supplied contents.
    /// Fails if the file already exists.
    fn create_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str>;

    /// Overwrites an existing file's contents.
    /// Fails if the file does not exist — use [`create_file`] for that.
    fn write_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str>;

    /// Appends `data` to the end of an existing file.
    fn append_file(&self, filename: &str, data: &[u8]) -> Result<(), &'static str>;

    /// Deletes a file by name.
    fn delete_file(&self, filename: &str) -> Result<(), &'static str>;

    /// Renames `old_name` to `new_name`.
    fn rename_file(&self, old_name: &str, new_name: &str) -> Result<(), &'static str>;

    /// Returns `true` if `filename` exists.
    fn file_exists(&self, filename: &str) -> bool;

    /// Returns the size of `filename` in bytes, or `None` if not found.
    fn get_file_size(&self, filename: &str) -> Option<u32>;

    /// Lists all visible entries in the root (or only) directory.
    fn list_dir(&self) -> Result<Vec<VfsDirEntry>, &'static str>;

    /// Creates a subdirectory. May return `Err("Unsupported")` if not implemented.
    fn create_dir(&self, dirname: &str) -> Result<(), &'static str>;

    /// Deletes an empty subdirectory. May return `Err("Unsupported")` if not implemented.
    fn delete_dir(&self, dirname: &str) -> Result<(), &'static str>;

    /// Returns volume statistics.
    fn stat_fs(&self) -> FsInfo;
}

/// VFS adapter wrapping the static FAT12 driver.
///
/// Because the FAT12 module uses process-global state, the `drive` parameter
/// is informational only. Call [`fat12_init`] before mounting.
///
/// # Example
/// ```rust
/// fat12_init(0).expect("FAT12 init failed");
/// vfs_mount("hda", Box::new(Fat12Driver::new(0)));
/// ```
pub struct Fat12Driver {
    /// IDE drive index this driver was initialised for (informational only).
    pub drive: usize,
}

impl Fat12Driver {
    /// Creates a new adapter. You must have already called [`fat12_init`].
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

/// A trivial volatile RAM filesystem backed by a heap-allocated list.
///
/// Useful for a `tmpfs`-style scratch mount or for testing the VFS layer
/// without real hardware. All data is lost when the kernel reboots.
///
/// Limitations: no subdirectory support; all files stored as full byte vectors.
pub struct RamFs {
    files: spin::Mutex<Vec<RamFile>>,
}

struct RamFile {
    name: String,
    data: Vec<u8>,
}

impl RamFs {
    /// Creates an empty RAM filesystem.
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

struct MountEntry {
    point: &'static str,
    fs: Box<dyn FileSystem>,
}

/// Wrapper that lets us hold a `Vec` in a static without `static mut`.
/// All access is guarded by `VFS_LOCK`.
struct MountTable(UnsafeCell<Option<Vec<MountEntry>>>);

unsafe impl Sync for MountTable {}

static VFS_LOCK: AtomicBool = AtomicBool::new(false);
static MOUNT_TABLE: MountTable = MountTable(UnsafeCell::new(None));

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

/// Returns a `&mut Option<Vec<MountEntry>>` while the VFS lock is held.
/// Only called between `vfs_lock()` and `vfs_unlock()`.
fn table() -> &'static mut Option<Vec<MountEntry>> {
    unsafe { &mut *MOUNT_TABLE.0.get() }
}

fn get_fs(mount_point: &str) -> Result<&'static dyn FileSystem, &'static str> {
    table()
        .as_ref()
        .ok_or("VFS not initialised")?
        .iter()
        .find(|e| e.point == mount_point)
        .map(|e| e.fs.as_ref())
        .ok_or("Mount point not found")
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

/// Initialises the VFS subsystem.
///
/// Must be called once before any other `vfs_*` function. Safe to call
/// multiple times — subsequent calls are no-ops.
pub fn vfs_init() {
    vfs_lock();
    if table().is_none() {
        *table() = Some(Vec::new());
    }
    vfs_unlock();
}

/// Mounts a filesystem driver at `mount_point`.
///
/// `mount_point` must be a `'static` string literal so it can be stored
/// in the mount table without a heap allocation.
///
/// # Errors
/// Returns `Err` if the mount table is full or `mount_point` is already in use.
pub fn vfs_mount(mount_point: &'static str, fs: Box<dyn FileSystem>) -> Result<(), &'static str> {
    vfs_lock();
    let result = (|| {
        let t = table().as_mut().ok_or("VFS not initialised")?;
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
/// # Errors
/// Returns `Err` if the mount point is not found.
pub fn vfs_unmount(mount_point: &str) -> Result<(), &'static str> {
    vfs_lock();
    let result = (|| {
        let t = table().as_mut().ok_or("VFS not initialised")?;
        let before = t.len();
        t.retain(|e| e.point != mount_point);
        if t.len() == before { Err("Mount point not found") } else { Ok(()) }
    })();
    vfs_unlock();
    result
}

/// Reads a file via the VFS.
///
/// # Arguments
/// * `path` - VFS path in the form `"<mountpoint>/<filename>"`.
pub fn vfs_read_file(path: &str) -> Result<Vec<u8>, &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.read_file(file);
    vfs_unlock();
    result
}

/// Creates a new file via the VFS.
///
/// # Arguments
/// * `path` - VFS path in the form `"<mountpoint>/<filename>"`.
/// * `data` - File contents.
pub fn vfs_create_file(path: &str, data: &[u8]) -> Result<(), &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.create_file(file, data);
    vfs_unlock();
    result
}

/// Overwrites an existing file's contents via the VFS.
///
/// The file must already exist. Use [`vfs_create_file`] to create new files.
pub fn vfs_write_file(path: &str, data: &[u8]) -> Result<(), &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.write_file(file, data);
    vfs_unlock();
    result
}

/// Appends data to an existing file via the VFS.
pub fn vfs_append_file(path: &str, data: &[u8]) -> Result<(), &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.append_file(file, data);
    vfs_unlock();
    result
}

/// Deletes a file via the VFS.
pub fn vfs_delete_file(path: &str) -> Result<(), &'static str> {
    let (mount, file) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.delete_file(file);
    vfs_unlock();
    result
}

/// Renames a file via the VFS.
///
/// Both paths must share the same mount point. Cross-mount renames are not
/// supported — use read + create + delete for that.
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

/// Lists the contents of a mounted filesystem's root directory.
///
/// # Arguments
/// * `mount_point` - The mount-point label (without a trailing `/`).
pub fn vfs_list_dir(mount_point: &str) -> Result<Vec<VfsDirEntry>, &'static str> {
    vfs_lock();
    let result = get_fs(mount_point)?.list_dir();
    vfs_unlock();
    result
}

/// Creates a subdirectory on a mounted filesystem.
///
/// # Arguments
/// * `path` - VFS path to the new directory (e.g. `"hda/SAVES"`).
pub fn vfs_create_dir(path: &str) -> Result<(), &'static str> {
    let (mount, dir) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.create_dir(dir);
    vfs_unlock();
    result
}

/// Deletes an empty subdirectory on a mounted filesystem.
pub fn vfs_delete_dir(path: &str) -> Result<(), &'static str> {
    let (mount, dir) = split_path(path)?;
    vfs_lock();
    let result = get_fs(mount)?.delete_dir(dir);
    vfs_unlock();
    result
}

/// Returns volume statistics for a mounted filesystem.
///
/// # Arguments
/// * `mount_point` - The mount-point label.
pub fn vfs_stat(mount_point: &str) -> Result<FsInfo, &'static str> {
    vfs_lock();
    let result = Ok(get_fs(mount_point)?.stat_fs());
    vfs_unlock();
    result
}

/// Returns a list of currently mounted filesystems as `(mount_point, fs_type)` pairs.
pub fn vfs_list_mounts() -> Vec<(&'static str, &'static str)> {
    vfs_lock();
    let result = table()
        .as_ref()
        .map(|t| t.iter().map(|e| (e.point, e.fs.stat_fs().fs_type)).collect())
        .unwrap_or_default();
    vfs_unlock();
    result
}