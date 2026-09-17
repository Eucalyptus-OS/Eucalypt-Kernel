#pragma once

#include <stdint.h>
#include <stddef.h>

#include <abi/types.h>
#include <abi/errno.h>
#include <abi/fcntl.h>
#include <abi/seek.h>
#include <abi/mmap.h>
#include <abi/time.h>

typedef long ssize_t;
typedef long off_t;

// Return codes for vfs_init/vfs_mount and filesystem mount helpers
#define VFS_OK 0
#define VFS_ERR_LETTER_IN_USE 1
#define VFS_ERR_NO_SLOTS 2
#define VFS_ERR_INVALID_DEV 3
#define VFS_ERR_ALREADY_MOUNTED 4
#define VFS_ERR_FS_INIT 5

// Node types stored in vfs_node_t::type
#define VFS_NODE_FILE 0
#define VFS_NODE_DIR 1
#define VFS_NODE_DEV 2
#define VFS_NODE_MOUNTPOINT 3
#define VFS_NODE_SYMLINK 4
#define VFS_NODE_PIPE 5

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define MAX_NAME_LEN 256
#define PATH_MAX 4096

#define VFS_SEEK_SET    SEEK_SET
#define VFS_SEEK_CUR    SEEK_CUR
#define VFS_SEEK_END    SEEK_END
#define VFS_O_RDONLY    O_RDONLY
#define VFS_O_WRONLY    O_WRONLY
#define VFS_O_RDWR      O_RDWR
#define VFS_O_CREAT     O_CREAT
#define VFS_O_TRUNC     O_TRUNC
#define VFS_O_APPEND    O_APPEND
#define VFS_STDIN       STDIN_FILENO
#define VFS_STDOUT      STDOUT_FILENO
#define VFS_STDERR      STDERR_FILENO

extern int errno;

// Filesystem layouts a block device can be detected as
typedef enum { fat12, fat16, fat32, exfat } fs_t;

typedef struct vfs_blockdev {
    // Raw 512-byte sector read/write interface (lba, count, buffer)
    uint8_t (*read)(struct vfs_blockdev *dev, uint32_t lba, uint8_t count, void *buf);
    uint8_t (*write)(struct vfs_blockdev *dev, uint32_t lba, uint8_t count, const void *buf);
    void *priv;
} vfs_blockdev_t;

// One directory entry, as produced by readdir-type filesystem ops
typedef struct {
    char d_name[MAX_NAME_LEN];
    uint32_t d_type;
    uint32_t d_ino;
} vfs_dirent_t;

// The operation vtable every filesystem implements for its inodes
typedef struct {
    struct vfs_node *(*lookup)(struct vfs_node *dir, const char *name);   // find child |name| in |dir|
    ssize_t (*read)(struct vfs_node *node, void *buf, size_t count, off_t offset);   // read at file |offset|
    ssize_t (*write)(struct vfs_node *node, const void *buf, size_t count, off_t offset);   // write at file |offset|
    int (*readdir)(struct vfs_node *dir, uint32_t index, vfs_dirent_t *out);   // fetch index-th dirent of |dir|
    int (*create)(struct vfs_node *dir, const char *name, uint32_t type, uint32_t mode);   // make a child node
    int (*unlink)(struct vfs_node *dir, const char *name);   // remove a non-directory child
    int (*rmdir)(struct vfs_node *dir, const char *name);   // remove an empty child directory
    int (*rename)(struct vfs_node *old_dir, const char *old_name, struct vfs_node *new_dir, const char *new_name);   // move a child between parents
    int (*truncate)(struct vfs_node *node, off_t length);   // resize |node| to |length| bytes
    int (*symlink)(struct vfs_node *dir, const char *name, const char *target);   // create symlink |name| -> |target|
    ssize_t (*readlink)(struct vfs_node *node, char *buf, size_t bufsiz);   // read a symlink's target
} vfs_node_ops_t;

typedef struct vfs_node {
    char name[MAX_NAME_LEN];       // leaf name only, not the full path
    uint32_t type;                 // VFS_NODE_* type
    uint32_t mode;                 // file type bits (S_IFMT) | permission bits
    uint32_t uid;
    uint32_t gid;
    uint32_t flags;
    size_t size;                   // file length in bytes
    uint32_t ref_count;            // number of open vfs_file_t descriptors
    uint32_t ino;                  // VFS-wide unique inode number
    int64_t atime;                 // seconds-since-epoch access/modify/change times
    int64_t mtime;
    int64_t ctime;
    vfs_node_ops_t *ops;           // filesystem operations for this node
    void *priv;                    // filesystem-private data (buffer, FAT handle, etc.)
    struct vfs_node *parent;       // tree linkage: parent, first child, next sibling
    struct vfs_node *children;
    struct vfs_node *next;
} vfs_node_t;

// Kernel-internal stat result, converted to the ABI struct on syscall return
typedef struct {
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    size_t st_size;
    int64_t st_atime;
    int64_t st_mtime;
    int64_t st_ctime;
    uint32_t st_blksize;
    uint32_t st_blocks;
} vfs_stat_t;

// An open file description; dup() shares this same object across fds
typedef struct vfs_file {
    vfs_node_t *node;
    off_t offset;      // current read/write position
    int flags;         // open() flags for this descriptor
    uint32_t ref_count;
} vfs_file_t;

// Directory stream state used by opendir/readdir
typedef struct {
    vfs_node_t *node;
    off_t pos;         // index of the next dirent to return
} vfs_dir_t;

// One entry in the mount table, indexed by mount letter/name
typedef struct {
    char *name;
    vfs_blockdev_t blockdev;   // backing device with sector callbacks
    uint8_t drive_number;      // AHCI drive number of the backing device
    void *priv;
} vfs_mount_t;

// Payload of vfs_blockdev_t::priv: just records the drive number
typedef struct {
    uint8_t drive_number;
} vfs_blockdev_priv_t;

// Per-node FAT16 state: where the volume and this file's data live
typedef struct {
    void *vol;                 // fat_node volume handle from fat16_init
    uint16_t start_cluster;    // first cluster of this file's chain
    uint32_t size;             // file size in bytes
    uint16_t dir_cluster;      // parent directory cluster holding this dirent
} vfs_fat16_priv_t;

vfs_node_t *vfs_node_alloc(const char *name, uint32_t type);
void vfs_node_link_child(vfs_node_t *parent, vfs_node_t *child);
void vfs_node_unlink_child(vfs_node_t *parent, vfs_node_t *child);
vfs_node_t *vfs_node_find_child(vfs_node_t *parent, const char *name);
// Resolve |path| (absolute or cwd-relative) to a node, following symlinks
vfs_node_t *vfs_resolve_path(const char *path);
// Split |path| into its parent node and the leaf component |name_out|
vfs_node_t *vfs_resolve_parent(const char *path, char *name_out);

uint8_t vfs_init();
// Probe |drive_number| as FAT16 and mount it at |name| in the VFS tree
uint8_t vfs_mount(char *name, uint8_t drive_number);
// Mount block device |dev_path| onto directory |target|, errno-encoded result
int vfs_mount_by_path(const char *dev_path, const char *target);
// Format block device |dev_path| as FAT16
int vfs_mkfs(const char *dev_path);
// Mount the first FAT16 drive found, exposed as "bins"
int vfs_autmount_bins(void);
void vfs_unmount(char *name);
vfs_mount_t *vfs_get_mount(char *name);
vfs_node_t *vfs_get_root();
int vfs_chdir(const char *path);
char *vfs_getcwd(char *buf, size_t size);

int open(const char *path, int flags, ...);
int close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t lseek(int fd, off_t offset, int whence);
int dup(int fd);
int dup2(int old_fd, int new_fd);
ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
ssize_t getdents64(int fd, void *buf, size_t count);

void vfs_fd_table_init(vfs_file_t **table, size_t count);
// Bind fd table slots 0/1/2 to /dev/stdin, /dev/stdout, /dev/stderr
void vfs_fd_table_setup_stdio(vfs_file_t **table, size_t count);
void vfs_fd_table_clone(vfs_file_t **dst, vfs_file_t **src, size_t count);
void vfs_fd_table_close(vfs_file_t **table, size_t count);

int vfs_stat(const char *path, vfs_stat_t *st);
int vfs_fstat(int fd, vfs_stat_t *st);
int vfs_lstat(const char *path, vfs_stat_t *st);

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstatat(int dirfd, const char *path, struct stat *st, int flags);
int mkdir(const char *path, uint32_t mode);
int rmdir(const char *path);
int unlink(const char *path);
int rename(const char *old_path, const char *new_path);
int truncate(const char *path, off_t length);
int ftruncate(int fd, off_t length);
int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
int access(const char *path, int mode);
int chmod(const char *path, uint32_t mode);
int fchmod(int fd, uint32_t mode);

vfs_dir_t *opendir(const char *path);
vfs_dirent_t *readdir(vfs_dir_t *dir);
int closedir(vfs_dir_t *dir);
void rewinddir(vfs_dir_t *dir);
long telldir(vfs_dir_t *dir);
void seekdir(vfs_dir_t *dir, long pos);

// Create a node under |path| wired to |ops|/|priv| (used by fs mount helpers)
vfs_node_t *vfs_register_node(const char *path, uint32_t type, vfs_node_ops_t *ops, void *priv);
vfs_file_t *vfs_file_create(vfs_node_t *node, int flags);
fs_t vfs_get_type(vfs_blockdev_t *blockdev);

// Rewrite a directory entry to point at a new cluster/size (post-resize)
uint8_t fat16_create_dirent_update(const void *vol_ptr, uint16_t dir_cluster, const char *name, uint16_t cluster, uint32_t size);

// Public wrappers over the static tree helpers, used by ramfs/devfs/ustar
vfs_node_t *vfs_node_alloc_pub(const char *name, uint32_t type);
void vfs_node_link_child_pub(vfs_node_t *parent, vfs_node_t *child);
void vfs_node_unlink_child_pub(vfs_node_t *parent, vfs_node_t *child);
vfs_node_t *vfs_node_find_child_pub(vfs_node_t *parent, const char *name);

// Legacy convenience wrappers around the syscall-style fd functions
static inline int32_t vfs_filesize(int fd) {
    vfs_stat_t st;
    if (vfs_fstat(fd, &st) != 0) return -1;
    return (int32_t)st.st_size;
}
static inline int32_t vfs_seek(int fd, int32_t offset, int whence) { return (int32_t)lseek(fd, offset, whence); }
static inline int32_t vfs_read(int fd, void *buf, uint32_t count)  { return (int32_t)read(fd, buf, count); }
static inline int32_t vfs_write(int fd, const void *buf, uint32_t count) { return (int32_t)write(fd, buf, count); }
static inline int32_t vfs_tell(int fd) { return (int32_t)lseek(fd, 0, VFS_SEEK_CUR); }