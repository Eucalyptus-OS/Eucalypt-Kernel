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

#define VFS_OK 0
#define VFS_ERR_LETTER_IN_USE 1
#define VFS_ERR_NO_SLOTS 2
#define VFS_ERR_INVALID_DEV 3
#define VFS_ERR_ALREADY_MOUNTED 4
#define VFS_ERR_FS_INIT 5

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

typedef enum { fat12, fat16, fat32, exfat } fs_t;

typedef struct vfs_blockdev {
    uint8_t (*read)(struct vfs_blockdev *dev, uint32_t lba, uint8_t count, void *buf);
    uint8_t (*write)(struct vfs_blockdev *dev, uint32_t lba, uint8_t count, const void *buf);
    void *priv;
} vfs_blockdev_t;

typedef struct {
    char d_name[MAX_NAME_LEN];
    uint32_t d_type;
    uint32_t d_ino;
} vfs_dirent_t;

typedef struct {
    struct vfs_node *(*lookup)(struct vfs_node *dir, const char *name);
    ssize_t (*read)(struct vfs_node *node, void *buf, size_t count, off_t offset);
    ssize_t (*write)(struct vfs_node *node, const void *buf, size_t count, off_t offset);
    int (*readdir)(struct vfs_node *dir, uint32_t index, vfs_dirent_t *out);
    int (*create)(struct vfs_node *dir, const char *name, uint32_t type, uint32_t mode);
    int (*unlink)(struct vfs_node *dir, const char *name);
    int (*rmdir)(struct vfs_node *dir, const char *name);
    int (*rename)(struct vfs_node *old_dir, const char *old_name, struct vfs_node *new_dir, const char *new_name);
    int (*truncate)(struct vfs_node *node, off_t length);
    int (*symlink)(struct vfs_node *dir, const char *name, const char *target);
    ssize_t (*readlink)(struct vfs_node *node, char *buf, size_t bufsiz);
} vfs_node_ops_t;

typedef struct vfs_node {
    char name[MAX_NAME_LEN];
    uint32_t type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t flags;
    size_t size;
    uint32_t ref_count;
    uint32_t ino;
    int64_t atime;
    int64_t mtime;
    int64_t ctime;
    vfs_node_ops_t *ops;
    void *priv;
    struct vfs_node *parent;
    struct vfs_node *children;
    struct vfs_node *next;
} vfs_node_t;

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

typedef struct vfs_file {
    vfs_node_t *node;
    off_t offset;
    int flags;
    uint32_t ref_count;
} vfs_file_t;

typedef struct {
    vfs_node_t *node;
    off_t pos;
} vfs_dir_t;

typedef struct {
    char *name;
    vfs_blockdev_t blockdev;
    uint8_t drive_number;
    void *priv;
} vfs_mount_t;

typedef struct {
    uint8_t drive_number;
} vfs_blockdev_priv_t;

typedef struct {
    void *vol;
    uint16_t start_cluster;
    uint32_t size;
    uint16_t dir_cluster;
} vfs_fat16_priv_t;

vfs_node_t *vfs_node_alloc(const char *name, uint32_t type);
void vfs_node_link_child(vfs_node_t *parent, vfs_node_t *child);
void vfs_node_unlink_child(vfs_node_t *parent, vfs_node_t *child);
vfs_node_t *vfs_node_find_child(vfs_node_t *parent, const char *name);
vfs_node_t *vfs_resolve_path(const char *path);
vfs_node_t *vfs_resolve_parent(const char *path, char *name_out);

uint8_t vfs_init();
uint8_t vfs_mount(char *name, uint8_t drive_number);
int vfs_mount_by_path(const char *dev_path, const char *target);
int vfs_mkfs(const char *dev_path);
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

vfs_node_t *vfs_register_node(const char *path, uint32_t type, vfs_node_ops_t *ops, void *priv);
vfs_file_t *vfs_file_create(vfs_node_t *node, int flags);
fs_t vfs_get_type(vfs_blockdev_t *blockdev);

uint8_t fat16_create_dirent_update(const void *vol_ptr, uint16_t dir_cluster, const char *name, uint16_t cluster, uint32_t size);

vfs_node_t *vfs_node_alloc_pub(const char *name, uint32_t type);
void vfs_node_link_child_pub(vfs_node_t *parent, vfs_node_t *child);
void vfs_node_unlink_child_pub(vfs_node_t *parent, vfs_node_t *child);
vfs_node_t *vfs_node_find_child_pub(vfs_node_t *parent, const char *name);

static inline int32_t vfs_filesize(int fd) {
    vfs_stat_t st;
    if (vfs_fstat(fd, &st) != 0) return -1;
    return (int32_t)st.st_size;
}
static inline int32_t vfs_seek(int fd, int32_t offset, int whence) { return (int32_t)lseek(fd, offset, whence); }
static inline int32_t vfs_read(int fd, void *buf, uint32_t count)  { return (int32_t)read(fd, buf, count); }
static inline int32_t vfs_write(int fd, const void *buf, uint32_t count) { return (int32_t)write(fd, buf, count); }
static inline int32_t vfs_tell(int fd) { return (int32_t)lseek(fd, 0, VFS_SEEK_CUR); }