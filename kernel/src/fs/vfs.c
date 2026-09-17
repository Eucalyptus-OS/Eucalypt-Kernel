#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <mm/memory.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <mm/heap.h>
#include <storage/ahci.h>
#include <storage/disk_writer.h>
#include <storage/drive_map.h>
#include <fs/fat16.h>
#include <fs/devfs.h>
#include <multitasking/thread.h>
#include <multitasking/sched.h>
#include <multitasking/proc.h>
#include <fs/vfs.h>
#include <fs/pipe.h>
#include <logging/print.h>

#define MAX_DRIVES  254
#define MAX_FD      256

int errno = 0;

static vfs_mount_t  mount_table[MAX_DRIVES];
static uint8_t      mount_count = 0;
static uint8_t      vfs_ready   = 0;

static vfs_file_t  *kernel_fd_table[MAX_FD];
static uint8_t      fd_ready = 0;

static vfs_node_t  *vfs_root = NULL;
static vfs_node_t  *vfs_cwd  = NULL;

static uint32_t     next_ino = 1;

typedef struct {
    uint8_t *data;
    size_t   capacity;
} ramfs_priv_t;

static uint32_t alloc_ino() {
    return next_ino++;
}

void vfs_fd_table_init(vfs_file_t **table, size_t count) {
    if (!table) return;
    for (size_t i = 0; i < count; i++)
        table[i] = NULL;
}

static vfs_file_t **current_fd_table(size_t *count) {
    struct tcb *thread = sched_current_thread();
    if (thread && thread->parent) {
        if (count) *count = MAX_FDS;
        return thread->parent->fd_table;
    }
    if (count) *count = MAX_FD;
    return kernel_fd_table;
}

static vfs_file_t *fd_get(int fd) {
    size_t count;
    vfs_file_t **table = current_fd_table(&count);

    if (fd < 0 || (size_t)fd >= count)
        return NULL;

    return table[fd];
}

static int fd_alloc(vfs_file_t **table, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!table[i]) return (int)i;
    }
    errno = ENFILE;
    return -1;
}

vfs_file_t *vfs_file_create(vfs_node_t *node, int flags) {
    vfs_file_t *file = kmalloc(sizeof(vfs_file_t));
    if (!file) {
        errno = ENOSPC;
        return NULL;
    }

    file->node      = node;
    file->offset    = (flags & O_APPEND) ? (off_t)node->size : 0;
    file->flags     = flags;
    file->ref_count = 1;
    node->ref_count++;
    return file;
}

static void vfs_file_ref(vfs_file_t *file) {
    if (file)
        file->ref_count++;
}

static void vfs_file_unref(vfs_file_t *file) {
    if (!file)
        return;

    if (file->ref_count > 0)
        file->ref_count--;

    if (file->ref_count == 0) {
        if (file->node && file->node->type == VFS_NODE_PIPE && file->node->priv) {
            pipe_t *p = (pipe_t *)file->node->priv;
            if ((int)file->offset == 0) {
                if (p->readers > 0) p->readers--;
                pipe_wake_writers(p);
            } else {
                if (p->writers > 0) p->writers--;
                pipe_wake_readers(p);
            }
            if (p->readers == 0 && p->writers == 0) {
                pipe_destroy(p);
            }
        }
        if (file->node && file->node->ref_count > 0)
            file->node->ref_count--;
        kfree(file);
    }
}

vfs_node_t *vfs_node_alloc(const char *name, uint32_t type) {
    size_t len = strlen(name);
    if (len >= MAX_NAME_LEN) { errno = ENAMETOOLONG; return NULL; }

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (!node) { 
        errno = ENOSPC; return NULL; 
    }
    memset(node, 0, sizeof(vfs_node_t));
    memcpy(node->name, name, len);
    node->name[len]  = '\0';
    node->type       = type;
    node->ino        = alloc_ino();
    node->ref_count  = 0;
    node->parent     = NULL;
    node->children   = NULL;
    node->next       = NULL;
    node->ops        = NULL;
    node->priv       = NULL;

    switch (type) {
        case VFS_NODE_DIR:  node->mode = S_IFDIR | 0755; break;
        case VFS_NODE_DEV:  node->mode = S_IFCHR | 0600; break;
        case VFS_NODE_SYMLINK: node->mode = S_IFLNK | 0777; break;
        default:            node->mode = S_IFREG | 0644; break;
    }
    return node;
}

void vfs_node_link_child(vfs_node_t *parent, vfs_node_t *child) {
    child->parent    = parent;
    child->next      = parent->children;
    parent->children = child;
}

void vfs_node_unlink_child(vfs_node_t *parent, vfs_node_t *child) {
    vfs_node_t **cur = &parent->children;
    while (*cur) {
        if (*cur == child) {
            *cur        = child->next;
            child->next = NULL;
            child->parent = NULL;
            return;
        }
        cur = &(*cur)->next;
    }
}

vfs_node_t *vfs_node_find_child(vfs_node_t *parent, const char *name) {
    for (vfs_node_t *c = parent->children; c; c = c->next) {
        if (strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

static vfs_node_t *resolve_from(vfs_node_t *start, const char *path) {
    vfs_node_t *node = start;
    const char *p    = path;

    while (*p) {
        if (*p == '/') { p++; continue; }

        char component[MAX_NAME_LEN];
        size_t len = 0;
        while (*p && *p != '/') {
            if (len >= MAX_NAME_LEN - 1) { errno = ENAMETOOLONG; return NULL; }
            component[len++] = *p;
            p++;
        }
        component[len] = '\0';

        if (strcmp(component, ".") == 0)  continue;
        if (strcmp(component, "..") == 0) {
            if (node->parent) {
                vfs_node_t *p = node->parent;
                if (p->type == VFS_NODE_MOUNTPOINT && p->parent)
                    p = p->parent;
                node = p;
            }
            continue;
        }

        if (node->type == VFS_NODE_MOUNTPOINT) {
            vfs_node_t *inner = (vfs_node_t *)node->priv;
            if (inner) node = inner;
        }

        vfs_node_t *child = NULL;
        if (node->ops && node->ops->lookup)
            child = node->ops->lookup(node, component);
        else
            child = vfs_node_find_child(node, component);

        if (!child) { errno = ENOENT; return NULL; }

        if (child->type == VFS_NODE_SYMLINK) {
            if (child->ops && child->ops->readlink) {
                char target[PATH_MAX];
                ssize_t n = child->ops->readlink(child, target, sizeof(target) - 1);
                if (n < 0) return NULL;
                target[n] = '\0';
                vfs_node_t *sym_start = (target[0] == '/') ? vfs_root : node;
                child = resolve_from(sym_start, (target[0] == '/') ? target + 1 : target);
                if (!child) return NULL;
            }
        }

        node = child;
    }
    return node;
}

vfs_node_t *vfs_resolve_path(const char *path) {
    if (!path) { errno = EINVAL; return NULL; }
    if (path[0] == '\0') { errno = ENOENT; return NULL; }
    if (path[0] == '/')
        return resolve_from(vfs_root, path + 1);
    if (vfs_cwd)
        return resolve_from(vfs_cwd, path);
    errno = ENOENT;
    return NULL;
}

vfs_node_t *vfs_resolve_parent(const char *path, char *name_out) {
    if (!path || path[0] == '\0') { errno = EINVAL; return NULL; }

    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len >= PATH_MAX) { errno = EINVAL; return NULL; }
    memcpy(buf, path, len + 1);

    char *last_slash = NULL;
    for (char *p = buf; *p; p++)
        if (*p == '/') last_slash = p;

    if (name_out) {
        const char *base = last_slash ? last_slash + 1 : buf;
        size_t nlen = strlen(base);
        if (nlen >= MAX_NAME_LEN) { errno = ENAMETOOLONG; return NULL; }
        memcpy(name_out, base, nlen);
        name_out[nlen] = '\0';
    }

    if (!last_slash) return vfs_cwd ? vfs_cwd : vfs_root;
    if (last_slash == buf) return vfs_root;

    *last_slash = '\0';
    return vfs_resolve_path(buf);
}

int vfs_chdir(const char *path) {
    vfs_node_t *node = vfs_resolve_path(path);
    if (!node) return -1;
    if (node->type != VFS_NODE_DIR && node->type != VFS_NODE_MOUNTPOINT) {
        errno = ENOTDIR;
        return -1;
    }
    if (node->type == VFS_NODE_MOUNTPOINT) {
        vfs_node_t *inner = (vfs_node_t *)node->priv;
        if (inner) node = inner;
    }
    vfs_cwd = node;
    return 0;
}

char *vfs_getcwd(char *buf, size_t size) {
    if (!buf || size == 0) { errno = EINVAL; return NULL; }

    char tmp[PATH_MAX];
    size_t pos = 0;

    vfs_node_t *node = vfs_cwd ? vfs_cwd : vfs_root;
    vfs_node_t *cur = node;

    char segments[64][MAX_NAME_LEN];
    int depth = 0;

    while (cur && cur != vfs_root) {
        if (depth >= 64) break;
        size_t nlen = strlen(cur->name);
        if (nlen >= MAX_NAME_LEN) nlen = MAX_NAME_LEN - 1;
        memcpy(segments[depth++], cur->name, nlen + 1);
        cur = cur->parent;
    }

    pos = 0;
    tmp[pos++] = '/';
    for (int i = depth - 1; i >= 0; i--) {
        size_t nlen = strlen(segments[i]);
        if (pos + nlen + 2 > PATH_MAX) { errno = ENOSPC; return NULL; }
        memcpy(tmp + pos, segments[i], nlen);
        pos += nlen;
        if (i > 0) tmp[pos++] = '/';
    }
    tmp[pos] = '\0';

    if (pos + 1 > size) { errno = ENOSPC; return NULL; }
    memcpy(buf, tmp, pos + 1);
    return buf;
}

static int find_letter_slot(char *name) {
    for (int i = 0; i < mount_count; i++)
        if (mount_table[i].name == name) return i;
    return -1;
}

vfs_mount_t *vfs_get_mount(char *name) {
    int slot = find_letter_slot(name);
    return (slot < 0) ? NULL : &mount_table[slot];
}

static uint8_t ahci_blockdev_read(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return disk_reader(priv->drive_number, lba, count, buf);
}

static uint8_t ahci_blockdev_write(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, const void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return disk_writer(priv->drive_number, lba, count, buf);
}

fs_t vfs_get_type(vfs_blockdev_t *blockdev) {
    uintptr_t frame = frame_alloc();
    if (!frame) return (fs_t)-1;
    uint8_t *buf = (uint8_t *)phys_to_virt(frame);

    uint8_t type;
    if (blockdev->read(blockdev, 0, 1, buf) != 0) {
        frame_free(frame);
        return (fs_t)-1;
    }
    if (memcmp(buf + 0x36, "FAT12   ", 8) == 0) type = fat12;
    else if (memcmp(buf + 0x36, "FAT16   ", 8) == 0) type = fat16;
    else if (memcmp(buf + 0x52, "FAT32   ", 8) == 0) type = fat32;
    else if (memcmp(buf + 0x03, "EXFAT   ", 8) == 0) type = exfat;
    else type = (uint8_t)-1;

    frame_free(frame);
    return (fs_t)type;
}

static ssize_t fat16_vfs_read(vfs_node_t *node, void *buf, size_t count, off_t offset) {
    vfs_fat16_priv_t *priv = (vfs_fat16_priv_t *)node->priv;
    if (!priv) { errno = EBADF; return -1; }
    if ((size_t)offset >= priv->size) return 0;
    size_t remaining = priv->size - (size_t)offset;
    if (count > remaining) count = remaining;

    uint8_t *tmp = kmalloc(priv->size);
    if (!tmp) { errno = ENOSPC; return -1; }

    uint32_t bytes_read = 0;
    if (fat16_read_file(priv->vol, priv->start_cluster, priv->size, tmp, &bytes_read) != 0) {
        kfree(tmp);
        errno = EBADF;
        return -1;
    }

    memcpy(buf, tmp + offset, count);
    kfree(tmp);
    return (ssize_t)count;
}

static ssize_t fat16_vfs_write(vfs_node_t *node, const void *buf, size_t count, off_t offset) {
    vfs_fat16_priv_t *priv = (vfs_fat16_priv_t *)node->priv;
    if (!priv) { errno = EBADF; return -1; }

    size_t new_size = ((size_t)offset + count > priv->size) ? (size_t)offset + count : priv->size;
    uint8_t *tmp = kmalloc(new_size);
    if (!tmp) { errno = ENOSPC; return -1; }
    memset(tmp, 0, new_size);

    if (priv->size > 0) {
        uint32_t bytes_read = 0;
        fat16_read_file(priv->vol, priv->start_cluster, priv->size, tmp, &bytes_read);
    }

    memcpy(tmp + offset, buf, count);

    if (priv->start_cluster) {
        fat16_free_cluster_chain(priv->vol, priv->start_cluster);
        priv->start_cluster = 0;
    }

    uint32_t bytes_written = 0;
    uint16_t new_cluster = priv->start_cluster;
    if (fat16_write_file(priv->vol, &new_cluster, tmp, new_size, &bytes_written) != 0) {
        kfree(tmp);
        errno = ENOSPC;
        return -1;
    }

    if (fat16_create_dirent_update(priv->vol, priv->dir_cluster, node->name, new_cluster, new_size) != 0) {
        kfree(tmp);
        errno = ENOSPC;
        return -1;
    }

    priv->start_cluster = new_cluster;
    priv->size = new_size;
    node->size = new_size;
    kfree(tmp);
    return (ssize_t)bytes_written;
}

static int fat16_vfs_readdir(vfs_node_t *node, uint32_t index, vfs_dirent_t *out) {
    vfs_fat16_priv_t *priv = (vfs_fat16_priv_t *)node->priv;
    if (!priv) { errno = EBADF; return -1; }

    fat16_dir_entry entries[512];
    uint16_t count = 512;
    if (fat16_list_directory(priv->vol, priv->start_cluster, entries, &count) != 0) return -1;
    if (index >= count) return -1;

    size_t nlen = strlen(entries[index].name);
    if (nlen >= MAX_NAME_LEN) nlen = MAX_NAME_LEN - 1;
    memcpy(out->d_name, entries[index].name, nlen);
    out->d_name[nlen] = '\0';
    out->d_type = (entries[index].attr & 0x10) ? VFS_NODE_DIR : VFS_NODE_FILE;
    out->d_ino = 0;
    return 0;
}

static vfs_node_t *fat16_vfs_lookup(vfs_node_t *dir, const char *name) {
    vfs_node_t *existing = vfs_node_find_child(dir, name);
    if (existing) return existing;

    vfs_fat16_priv_t *dpriv = (vfs_fat16_priv_t *)dir->priv;
    if (!dpriv) { errno = EBADF; return NULL; }

    fat16_file_handle handle;
    if (fat16_find_file(dpriv->vol, dpriv->start_cluster, name, &handle) != 0) {
        errno = ENOENT;
        return NULL;
    }

    uint32_t type = (handle.attr & 0x10) ? VFS_NODE_DIR : VFS_NODE_FILE;
    vfs_node_t *child = vfs_node_alloc(name, type);
    if (!child) return NULL;

    vfs_fat16_priv_t *cpriv = kmalloc(sizeof(vfs_fat16_priv_t));
    if (!cpriv) { kfree(child); errno = ENOSPC; return NULL; }

    cpriv->vol = dpriv->vol;
    cpriv->start_cluster = handle.start_cluster;
    cpriv->size = handle.size;
    cpriv->dir_cluster = dpriv->start_cluster;
    child->priv = cpriv;
    child->size = handle.size;
    child->ops = dir->ops;

    vfs_node_link_child(dir, child);
    return child;
}

static int fat16_vfs_create(vfs_node_t *dir, const char *name, uint32_t type, uint32_t mode) {
    (void)mode;
    vfs_fat16_priv_t *dpriv = (vfs_fat16_priv_t *)dir->priv;
    if (!dpriv) { errno = EBADF; return -1; }

    if (type == VFS_NODE_DIR)
        return (fat16_create_directory(dpriv->vol, dpriv->start_cluster, name) == 0) ? 0 : -1;
    return (fat16_create_file(dpriv->vol, dpriv->start_cluster, name, NULL, 0) == 0) ? 0 : -1;
}

static int fat16_vfs_unlink(vfs_node_t *dir, const char *name) {
    vfs_fat16_priv_t *dpriv = (vfs_fat16_priv_t *)dir->priv;
    if (!dpriv) { errno = EBADF; return -1; }
    return (fat16_delete_file(dpriv->vol, dpriv->start_cluster, name) == 0) ? 0 : -1;
}

static int fat16_vfs_rmdir(vfs_node_t *dir, const char *name) {
    vfs_fat16_priv_t *dpriv = (vfs_fat16_priv_t *)dir->priv;
    if (!dpriv) { errno = EBADF; return -1; }
    return (fat16_delete_directory(dpriv->vol, dpriv->start_cluster, name) == 0) ? 0 : -1;
}

static int fat16_vfs_truncate(vfs_node_t *node, off_t length) {
    vfs_fat16_priv_t *priv = (vfs_fat16_priv_t *)node->priv;
    if (!priv) { errno = EBADF; return -1; }

    size_t new_size = (size_t)length;
    size_t alloc_size = new_size > priv->size ? new_size : priv->size;
    uint8_t *tmp = kmalloc(alloc_size);
    if (!tmp) { errno = ENOSPC; return -1; }
    memset(tmp, 0, alloc_size);

    if (priv->size > 0) {
        uint32_t bytes_read = 0;
        fat16_read_file(priv->vol, priv->start_cluster, priv->size, tmp, &bytes_read);
    }

    if (priv->start_cluster) {
        fat16_free_cluster_chain(priv->vol, priv->start_cluster);
        priv->start_cluster = 0;
    }

    uint32_t bytes_written = 0;
    uint16_t new_cluster = 0;
    if (fat16_write_file(priv->vol, &new_cluster, tmp, new_size, &bytes_written) != 0) {
        kfree(tmp);
        errno = ENOSPC;
        return -1;
    }

    if (fat16_create_dirent_update(priv->vol, priv->dir_cluster, node->name, new_cluster, new_size) != 0) {
        kfree(tmp);
        errno = ENOSPC;
        return -1;
    }

    priv->start_cluster = new_cluster;
    priv->size = new_size;
    node->size = new_size;
    kfree(tmp);
    return 0;
}

static vfs_node_ops_t fat16_ops = {
    .read = fat16_vfs_read,
    .write = fat16_vfs_write,
    .readdir = fat16_vfs_readdir,
    .lookup = fat16_vfs_lookup,
    .create = fat16_vfs_create,
    .unlink = fat16_vfs_unlink,
    .rmdir = fat16_vfs_rmdir,
    .truncate = fat16_vfs_truncate,
    .rename = NULL,
    .symlink = NULL,
    .readlink = NULL,
};

static ssize_t ramfs_read(vfs_node_t *node, void *buf, size_t count, off_t offset) {
    ramfs_priv_t *priv = (ramfs_priv_t *)node->priv;
    if (!priv) { errno = EBADF; return -1; }
    if ((size_t)offset >= node->size) return 0;
    size_t remaining = node->size - (size_t)offset;
    if (count > remaining) count = remaining;
    memcpy(buf, priv->data + offset, count);
    return (ssize_t)count;
}

static ssize_t ramfs_write(vfs_node_t *node, const void *buf, size_t count, off_t offset) {
    ramfs_priv_t *priv = (ramfs_priv_t *)node->priv;
    if (!priv) { errno = EBADF; return -1; }

    size_t needed = (size_t)offset + count;
    if (needed > priv->capacity) {
        size_t new_capacity = priv->capacity ? priv->capacity : 4096;
        while (new_capacity < needed) new_capacity *= 2;
        uint8_t *new_data = kmalloc(new_capacity);
        if (!new_data) { errno = ENOSPC; return -1; }
        memset(new_data, 0, new_capacity);
        if (priv->data) {
            memcpy(new_data, priv->data, node->size);
            kfree(priv->data);
        }
        priv->data = new_data;
        priv->capacity = new_capacity;
    }

    memcpy(priv->data + offset, buf, count);
    if (needed > node->size) node->size = needed;
    return (ssize_t)count;
}

static int ramfs_truncate(vfs_node_t *node, off_t length) {
    ramfs_priv_t *priv = (ramfs_priv_t *)node->priv;
    if (!priv) { errno = EBADF; return -1; }

    size_t new_size = (size_t)length;
    if (new_size > priv->capacity) {
        size_t new_capacity = priv->capacity ? priv->capacity : 4096;
        while (new_capacity < new_size) new_capacity *= 2;
        uint8_t *new_data = kmalloc(new_capacity);
        if (!new_data) { errno = ENOSPC; return -1; }
        memset(new_data, 0, new_capacity);
        if (priv->data) {
            memcpy(new_data, priv->data, node->size < new_size ? node->size : new_size);
            kfree(priv->data);
        }
        priv->data = new_data;
        priv->capacity = new_capacity;
    } else if (new_size > node->size && priv->data) {
        memset(priv->data + node->size, 0, new_size - node->size);
    }

    node->size = new_size;
    return 0;
}

static vfs_node_ops_t ramfs_ops = {
    .read = ramfs_read,
    .write = ramfs_write,
    .readdir = NULL,
    .lookup = NULL,
    .create = NULL,
    .unlink = NULL,
    .rmdir = NULL,
    .truncate = ramfs_truncate,
    .rename = NULL,
    .symlink = NULL,
    .readlink = NULL,
};

static vfs_node_t *vfs_create_ramfs_file(vfs_node_t *parent, const char *name, uint32_t mode) {
    vfs_node_t *node = vfs_node_alloc(name, VFS_NODE_FILE);
    if (!node) return NULL;

    ramfs_priv_t *priv = kmalloc(sizeof(ramfs_priv_t));
    if (!priv) { kfree(node); errno = ENOSPC; return NULL; }
    priv->data = NULL;
    priv->capacity = 0;

    node->mode = S_IFREG | (mode & 0777);
    node->ops = &ramfs_ops;
    node->priv = priv;
    vfs_node_link_child(parent, node);
    return node;
}

uint8_t vfs_mount(char *name, uint8_t drive_number) {
    if (mount_count >= MAX_DRIVES) return VFS_ERR_NO_SLOTS;

    uint8_t controller, port;
    if (drive_map_resolve(drive_number, &controller, &port) != 0) return VFS_ERR_INVALID_DEV;

    ahci_controller_t *c = ahci_get_controller(controller);
    if (!c || !c->ports[port].present) return VFS_ERR_INVALID_DEV;
    if (c->ports[port].assigned_name != 0) return VFS_ERR_ALREADY_MOUNTED;

    vfs_blockdev_priv_t *priv = kmalloc(sizeof(vfs_blockdev_priv_t));
    if (!priv) return VFS_ERR_NO_SLOTS;
    priv->drive_number = drive_number;

    vfs_blockdev_t blockdev = {0};
    blockdev.priv = priv;
    blockdev.read = ahci_blockdev_read;
    blockdev.write = ahci_blockdev_write;

    fs_t type = vfs_get_type(&blockdev);
    if (type != fat16) { kfree(priv); return VFS_ERR_FS_INIT; }

    void *vol_ptr = fat16_init(&blockdev);
    if (!vol_ptr) { kfree(priv); return VFS_ERR_FS_INIT; }

    vfs_node_t *mp_outer = vfs_node_alloc(name, VFS_NODE_MOUNTPOINT);
    if (!mp_outer) { kfree(priv); return VFS_ERR_NO_SLOTS; }

    vfs_fat16_priv_t *fat_priv = kmalloc(sizeof(vfs_fat16_priv_t));
    if (!fat_priv) { kfree(mp_outer); kfree(priv); return VFS_ERR_NO_SLOTS; }
    fat_priv->vol = vol_ptr;
    fat_priv->start_cluster = 0;
    fat_priv->size = 0;
    fat_priv->dir_cluster = 0;

    vfs_node_t *root_dir = vfs_node_alloc("/", VFS_NODE_DIR);
    if (!root_dir) { kfree(fat_priv); kfree(mp_outer); kfree(priv); return VFS_ERR_NO_SLOTS; }
    root_dir->priv = fat_priv;
    root_dir->ops = &fat16_ops;

    mp_outer->priv = root_dir;
    root_dir->parent = mp_outer;
    vfs_node_link_child(vfs_root, mp_outer);

    mount_table[mount_count].name = name;
    mount_table[mount_count].blockdev = blockdev;
    mount_table[mount_count].drive_number = drive_number;
    mount_table[mount_count].priv = vol_ptr;
    mount_count++;

    c->ports[port].assigned_name = name;

    return VFS_OK;
}

static int vfs_err_to_errno(uint8_t vfs_err) {
    switch (vfs_err) {
        case VFS_OK:                  return 0;
        case VFS_ERR_LETTER_IN_USE:   return -EBUSY;
        case VFS_ERR_NO_SLOTS:        return -ENOMEM;
        case VFS_ERR_INVALID_DEV:     return -ENODEV;
        case VFS_ERR_ALREADY_MOUNTED: return -EBUSY;
        case VFS_ERR_FS_INIT:         return -EINVAL;
        default:                      return -EIO;
    }
}

static vfs_blockdev_t vfs_make_blockdev(uint8_t drive_number) {
    vfs_blockdev_priv_t *priv = kmalloc(sizeof(vfs_blockdev_priv_t));
    vfs_blockdev_t blockdev = {0};
    if (!priv) return blockdev;
    priv->drive_number = drive_number;
    blockdev.priv = priv;
    blockdev.read = ahci_blockdev_read;
    blockdev.write = ahci_blockdev_write;
    return blockdev;
}

int vfs_mkfs(const char *dev_path) {
    uint8_t drive_number;
    if (devfs_resolve_drive(dev_path, &drive_number) != 0) return -ENODEV;

    drive_t *d = drive_map_get(drive_number);
    if (!d) return -ENODEV;

    vfs_blockdev_t blockdev = vfs_make_blockdev(drive_number);
    if (!blockdev.priv) return -ENOMEM;

    int rc = fat16_format(&blockdev, (uint32_t)d->sector_count);
    kfree(blockdev.priv);
    if (rc != 0) return -EIO;
    return 0;
}

int vfs_mount_by_path(const char *dev_path, const char *target) {
    uint8_t drive_number;
    if (devfs_resolve_drive(dev_path, &drive_number) != 0) return -ENODEV;
    return vfs_err_to_errno(vfs_mount((char *)target, drive_number));
}

int vfs_autmount_bins(void) {
    for (uint8_t i = 0; i < drive_map_count(); i++) {
        drive_t *d = drive_map_get(i);
        if (!d || d->sector_count == 0) continue;

        vfs_blockdev_t blockdev = vfs_make_blockdev(i);
        if (!blockdev.priv) continue;

        if (vfs_get_type(&blockdev) == fat16) {
            kfree(blockdev.priv);
            return vfs_err_to_errno(vfs_mount("bins", i));
        }
        kfree(blockdev.priv);
    }
    return -1;
}

void vfs_unmount(char *name) {
    int slot = find_letter_slot(name);
    vfs_mount_t *m = &mount_table[slot];
    vfs_blockdev_priv_t *bpriv = (vfs_blockdev_priv_t *)m->blockdev.priv;

    uint8_t controller, port;
    if (drive_map_resolve(m->drive_number, &controller, &port) == 0) {
        ahci_controller_t *c = ahci_get_controller(controller);
        if (c) c->ports[port].assigned_name = 0;
    }

    vfs_node_t *mp = vfs_node_find_child(vfs_root, name);
    if (mp) {
        vfs_node_unlink_child(vfs_root, mp);
        kfree(mp);
    }

    if (bpriv) kfree(bpriv);

    for (int i = slot; i < mount_count - 1; i++) mount_table[i] = mount_table[i + 1];
    mount_count--;
}

static int path_ends_with_slash(const char *path) {
    if (!path || *path == '\0') return 0;
    size_t len = strlen(path);
    return path[len - 1] == '/';
}

static void node_to_stat(vfs_node_t *node, vfs_stat_t *st) {
    st->st_ino = node->ino;
    st->st_mode = node->mode;
    st->st_nlink = 1;
    st->st_uid = node->uid;
    st->st_gid = node->gid;
    st->st_size = node->size;
    st->st_atime = node->atime;
    st->st_mtime = node->mtime;
    st->st_ctime = node->ctime;
    st->st_blksize = 512;
    st->st_blocks = (node->size + 511) / 512;
}

int open(const char *path, int flags, ...) {
    if (!vfs_ready || !fd_ready) { errno = EBADF; return -1; }

    uint32_t mode = 0644;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned int);
        va_end(ap);
    }

    vfs_node_t *node = vfs_resolve_path(path);

    if (!node) {
        if (!(flags & O_CREAT)) { errno = ENOENT; return -1; }

        char name[MAX_NAME_LEN];
        vfs_node_t *parent = vfs_resolve_parent(path, name);
        if (!parent) return -1;

        if (parent->type == VFS_NODE_MOUNTPOINT) {
            vfs_node_t *inner = (vfs_node_t *)parent->priv;
            if (inner) parent = inner;
        }

        if (parent->ops && parent->ops->create) {
            if (parent->ops->create(parent, name, VFS_NODE_FILE, mode) != 0) return -1;
        } else {
            vfs_node_t *new_node = vfs_create_ramfs_file(parent, name, mode);
            if (!new_node) return -1;
        }

        node = vfs_resolve_path(path);
        if (!node) return -1;
    } else {
        if ((flags & O_CREAT) && (flags & O_EXCL)) { errno = EEXIST; return -1; }
        if ((flags & O_DIRECTORY) && node->type != VFS_NODE_DIR) { errno = ENOTDIR; return -1; }
    }

    if (node->type == VFS_NODE_DIR && (flags & (O_WRONLY | O_RDWR))) { errno = EISDIR; return -1; }
    if (path_ends_with_slash(path) && node->type != VFS_NODE_DIR) { errno = ENOTDIR; return -1; }

    if ((flags & O_TRUNC) && node->type == VFS_NODE_FILE) {
        if (node->ops && node->ops->truncate) node->ops->truncate(node, 0);
        else node->size = 0;
    }

    size_t fd_count;
    vfs_file_t **fd_table = current_fd_table(&fd_count);

    int fd = fd_alloc(fd_table, fd_count);
    if (fd < 0) return -1;

    fd_table[fd] = vfs_file_create(node, flags);
    if (!fd_table[fd]) return -1;

    return fd;
}

int close(int fd) {
    size_t fd_count;
    vfs_file_t **fd_table = current_fd_table(&fd_count);

    if (fd < 0 || (size_t)fd >= fd_count || !fd_table[fd]) { errno = EBADF; return -1; }
    vfs_file_unref(fd_table[fd]);
    fd_table[fd] = NULL;
    return 0;
}

ssize_t read(int fd, void *buf, size_t count) {
    vfs_file_t *file = fd_get(fd);
    if (!file) { errno = EBADF; return -1; }
    if (!buf) { errno = EINVAL; return -1; }
    if (!count) return 0;

    if ((file->flags & O_ACCMODE) == O_WRONLY) {
        errno = EBADF;
        return -1;
    }

    vfs_node_t *node = file->node;
    if (!node) { errno = EBADF; return -1; }

    if (node->type == VFS_NODE_DIR) { errno = EISDIR; return -1; }

    if (node->type == VFS_NODE_DEV) {
        devfs_dev_t *ddev = (devfs_dev_t *)node->priv;
        if (ddev && !ddev->is_block && ddev->read) {
            ssize_t n = ddev->read(ddev, buf, count);
            if (n > 0) file->offset += n;
            return n;
        }
    }

    if (!node->ops || !node->ops->read) { errno = EBADF; return -1; }
    ssize_t n = node->ops->read(node, buf, count, file->offset);
    if (n > 0) file->offset += n;
    return n;
}

ssize_t write(int fd, const void *buf, size_t count) {
    extern struct pcb *sched_current_proc(void);
    struct pcb *wproc = sched_current_proc();
    vfs_file_t *file = fd_get(fd);
    if (!file) {
        print("WRITE pid=%d fd=%d FAIL no-file\n", wproc ? wproc->pid : -1, fd);
        errno = EBADF; return -1;
    }
    if (!buf) { errno = EINVAL; return -1; }
    if (!count) return 0;

    int acc = file->flags & O_ACCMODE;
    if (acc == O_RDONLY) {
        print("WRITE pid=%d fd=%d FAIL readonly\n", wproc ? wproc->pid : -1, fd);
        errno = EBADF; return -1;
    }

    vfs_node_t *node = file->node;
    if (!node) {
        print("WRITE pid=%d fd=%d FAIL no-node\n", wproc ? wproc->pid : -1, fd);
        errno = EBADF; return -1;
    }

    if (node->type == VFS_NODE_DIR) { errno = EISDIR; return -1; }

    if (file->flags & O_APPEND) file->offset = (off_t)node->size;

    if (node->type == VFS_NODE_DEV) {
        devfs_dev_t *ddev = (devfs_dev_t *)node->priv;
        if (ddev && !ddev->is_block && ddev->write) {
            ssize_t n = ddev->write(ddev, buf, count);
            if (n > 0) file->offset += n;
            return n;
        }
    }

    if (!node->ops || !node->ops->write) {
        print("WRITE pid=%d fd=%d FAIL no-ops\n", wproc ? wproc->pid : -1, fd);
        errno = EBADF; return -1;
    }
    ssize_t n = node->ops->write(node, buf, count, file->offset);
    if (n > 0) file->offset += n;
    return n;
}

off_t lseek(int fd, off_t offset, int whence) {
    vfs_file_t *file = fd_get(fd);
    if (!file) { errno = EBADF; return -1; }
    vfs_node_t *node = file->node;
    if (!node) { errno = EBADF; return -1; }

    off_t new_offset;
    switch (whence) {
        case SEEK_SET: new_offset = offset; break;
        case SEEK_CUR: new_offset = file->offset + offset; break;
        case SEEK_END: new_offset = (off_t)node->size + offset; break;
        default: errno = EINVAL; return -1;
    }

    if (new_offset < 0) { errno = EINVAL; return -1; }
    file->offset = new_offset;
    return new_offset;
}

int vfs_stat(const char *path, vfs_stat_t *st) {
    if (!st) { errno = EINVAL; return -1; }
    vfs_node_t *node = vfs_resolve_path(path);
    if (!node) return -1;
    node_to_stat(node, st);
    return 0;
}

int vfs_lstat(const char *path, vfs_stat_t *st) {
    if (!st || !path) { errno = EINVAL; return -1; }

    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len >= PATH_MAX) { errno = EINVAL; return -1; }
    memcpy(buf, path, len + 1);

    char *last_slash = NULL;
    for (char *p = buf; *p; p++)
        if (*p == '/') last_slash = p;

    const char *base = last_slash ? last_slash + 1 : buf;
    char parent_path[PATH_MAX];
    if (last_slash) {
        size_t plen = (size_t)(last_slash - buf);
        if (plen == 0) plen = 1;
        memcpy(parent_path, buf, plen);
        parent_path[plen] = '\0';
    } else {
        parent_path[0] = '.';
        parent_path[1] = '\0';
    }

    vfs_node_t *parent = vfs_resolve_path(parent_path);
    if (!parent) return -1;

    vfs_node_t *node = vfs_node_find_child(parent, base);
    if (!node) { errno = ENOENT; return -1; }

    node_to_stat(node, st);
    return 0;
}

int vfs_fstat(int fd, vfs_stat_t *st) {
    vfs_file_t *file = fd_get(fd);
    if (!file || !st) { errno = EBADF; return -1; }
    vfs_node_t *node = file->node;
    if (!node) { errno = EBADF; return -1; }
    node_to_stat(node, st);
    return 0;
}

static void stat_to_abi(const vfs_stat_t *in, struct stat *out) {
    out->st_dev = 0;
    out->st_ino = in->st_ino;
    out->st_nlink = in->st_nlink;
    out->st_mode = in->st_mode;
    out->st_uid = in->st_uid;
    out->st_gid = in->st_gid;
    out->__pad0 = 0;
    out->st_rdev = 0;
    out->st_size = (int64_t)in->st_size;
    out->st_blksize = in->st_blksize;
    out->st_blocks = in->st_blocks;
    out->st_atim.tv_sec = in->st_atime;
    out->st_atim.tv_nsec = 0;
    out->st_mtim.tv_sec = in->st_mtime;
    out->st_mtim.tv_nsec = 0;
    out->st_ctim.tv_sec = in->st_ctime;
    out->st_ctim.tv_nsec = 0;
}

int stat(const char *path, struct stat *st) {
    vfs_stat_t in;
    if (vfs_stat(path, &in) != 0) return -1;
    stat_to_abi(&in, st);
    return 0;
}

int lstat(const char *path, struct stat *st) {
    vfs_stat_t in;
    if (vfs_lstat(path, &in) != 0) return -1;
    stat_to_abi(&in, st);
    return 0;
}

int fstat(int fd, struct stat *st) {
    vfs_stat_t in;
    if (vfs_fstat(fd, &in) != 0) return -1;
    stat_to_abi(&in, st);
    return 0;
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
    (void)dirfd;
    (void)flags;
    return stat(path, st);
}

int mkdir(const char *path, uint32_t mode) {
    char name[MAX_NAME_LEN];
    vfs_node_t *parent = vfs_resolve_parent(path, name);
    if (!parent) return -1;

    if (parent->type == VFS_NODE_MOUNTPOINT) {
        vfs_node_t *inner = (vfs_node_t *)parent->priv;
        if (inner) parent = inner;
    }

    if (parent->type != VFS_NODE_DIR) { errno = ENOTDIR; return -1; }

    if (vfs_node_find_child(parent, name)) { errno = EEXIST; return -1; }

    if (parent->ops && parent->ops->create)
        return parent->ops->create(parent, name, VFS_NODE_DIR, mode);

    vfs_node_t *node = vfs_node_alloc(name, VFS_NODE_DIR);
    if (!node) return -1;
    node->mode = S_IFDIR | (mode & 0777);
    vfs_node_link_child(parent, node);
    return 0;
}

int rmdir(const char *path) {
    char name[MAX_NAME_LEN];
    vfs_node_t *parent = vfs_resolve_parent(path, name);
    if (!parent) return -1;

    if (parent->type == VFS_NODE_MOUNTPOINT) {
        vfs_node_t *inner = (vfs_node_t *)parent->priv;
        if (inner) parent = inner;
    }

    if (parent->type != VFS_NODE_DIR) { errno = ENOTDIR; return -1; }

    vfs_node_t *target = vfs_node_find_child(parent, name);
    if (target) {
        if (target->type != VFS_NODE_DIR) { errno = ENOTDIR; return -1; }
        if (target->children) { errno = ENOTEMPTY; return -1; }
    }

    if (parent->ops && parent->ops->rmdir) {
        int ret = parent->ops->rmdir(parent, name);
        if (ret == 0 && target) {
            vfs_node_unlink_child(parent, target);
            kfree(target);
        }
        return ret;
    }

    if (!target) { errno = ENOENT; return -1; }
    vfs_node_unlink_child(parent, target);
    kfree(target);
    return 0;
}

int unlink(const char *path) {
    char name[MAX_NAME_LEN];
    vfs_node_t *parent = vfs_resolve_parent(path, name);
    if (!parent) return -1;

    vfs_node_t *target = vfs_node_find_child(parent, name);
    if (target && target->type == VFS_NODE_DIR) { errno = EISDIR; return -1; }

    if (parent->ops && parent->ops->unlink) {
        int ret = parent->ops->unlink(parent, name);
        if (ret == 0 && target) {
            vfs_node_unlink_child(parent, target);
            if (target->ref_count == 0) kfree(target);
        }
        return ret;
    }

    if (!target) { errno = ENOENT; return -1; }
    if (target->ref_count > 0) { errno = EACCES; return -1; }

    if (target->type == VFS_NODE_FILE && target->ops == &ramfs_ops && target->priv) {
        ramfs_priv_t *priv = (ramfs_priv_t *)target->priv;
        if (priv->data) kfree(priv->data);
        kfree(priv);
    }

    vfs_node_unlink_child(parent, target);
    kfree(target);
    return 0;
}

int rename(const char *old_path, const char *new_path) {
    char old_name[MAX_NAME_LEN], new_name[MAX_NAME_LEN];
    vfs_node_t *old_parent = vfs_resolve_parent(old_path, old_name);
    vfs_node_t *new_parent = vfs_resolve_parent(new_path, new_name);
    if (!old_parent || !new_parent) return -1;

    if (old_parent->ops && old_parent->ops->rename)
        return old_parent->ops->rename(old_parent, old_name, new_parent, new_name);

    vfs_node_t *target = vfs_node_find_child(old_parent, old_name);
    if (!target) { errno = ENOENT; return -1; }

    vfs_node_t *existing = vfs_node_find_child(new_parent, new_name);
    if (existing) {
        vfs_node_unlink_child(new_parent, existing);
        if (existing->ref_count == 0) kfree(existing);
    }

    vfs_node_unlink_child(old_parent, target);
    size_t nlen = strlen(new_name);
    if (nlen >= MAX_NAME_LEN) nlen = MAX_NAME_LEN - 1;
    memcpy(target->name, new_name, nlen);
    target->name[nlen] = '\0';
    vfs_node_link_child(new_parent, target);
    return 0;
}

int truncate(const char *path, off_t length) {
    if (length < 0) { errno = EINVAL; return -1; }
    vfs_node_t *node = vfs_resolve_path(path);
    if (!node) return -1;
    if (node->type != VFS_NODE_FILE) { errno = EINVAL; return -1; }
    if (node->ops && node->ops->truncate) return node->ops->truncate(node, length);
    node->size = (size_t)length;
    return 0;
}

int ftruncate(int fd, off_t length) {
    vfs_file_t *file = fd_get(fd);
    if (!file) { errno = EBADF; return -1; }
    if (length < 0) { errno = EINVAL; return -1; }
    vfs_node_t *node = file->node;
    if (!node) { errno = EBADF; return -1; }
    if (node->type != VFS_NODE_FILE) { errno = EINVAL; return -1; }
    if (node->ops && node->ops->truncate) return node->ops->truncate(node, length);
    node->size = (size_t)length;
    return 0;
}

int chmod(const char *path, uint32_t mode) {
    vfs_node_t *node = vfs_resolve_path(path);
    if (!node) return -1;
    node->mode = (node->mode & S_IFMT) | (mode & 0777);
    return 0;
}

int fchmod(int fd, uint32_t mode) {
    vfs_file_t *file = fd_get(fd);
    if (!file) { errno = EBADF; return -1; }
    vfs_node_t *node = file->node;
    if (!node) { errno = EBADF; return -1; }
    node->mode = (node->mode & S_IFMT) | (mode & 0777);
    return 0;
}

int symlink(const char *target, const char *linkpath) {
    char name[MAX_NAME_LEN];
    vfs_node_t *parent = vfs_resolve_parent(linkpath, name);
    if (!parent) return -1;

    if (parent->ops && parent->ops->symlink)
        return parent->ops->symlink(parent, name, target);

    vfs_node_t *node = vfs_node_alloc(name, VFS_NODE_SYMLINK);
    if (!node) return -1;

    size_t tlen = strlen(target);
    char *stored = kmalloc(tlen + 1);
    if (!stored) { kfree(node); errno = ENOSPC; return -1; }
    memcpy(stored, target, tlen + 1);
    node->priv = stored;
    vfs_node_link_child(parent, node);
    return 0;
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    if (!buf || bufsiz == 0) { errno = EINVAL; return -1; }

    char pbuf[PATH_MAX];
    size_t len = strlen(path);
    if (len >= PATH_MAX) { errno = EINVAL; return -1; }
    memcpy(pbuf, path, len + 1);

    char *last_slash = NULL;
    for (char *p = pbuf; *p; p++)
        if (*p == '/') last_slash = p;

    const char *base = last_slash ? last_slash + 1 : pbuf;
    if (last_slash) *last_slash = '\0';

    vfs_node_t *parent = last_slash ? vfs_resolve_path(pbuf) : (vfs_cwd ? vfs_cwd : vfs_root);
    if (!parent) return -1;

    vfs_node_t *node = vfs_node_find_child(parent, base);
    if (!node) { errno = ENOENT; return -1; }
    if (node->type != VFS_NODE_SYMLINK) { errno = EINVAL; return -1; }

    if (node->ops && node->ops->readlink) return node->ops->readlink(node, buf, bufsiz);

    const char *stored = (const char *)node->priv;
    if (!stored) { errno = ENOENT; return -1; }
    size_t slen = strlen(stored);
    if (slen > bufsiz) slen = bufsiz;
    memcpy(buf, stored, slen);
    return (ssize_t)slen;
}

int access(const char *path, int mode) {
    (void)mode;
    vfs_node_t *node = vfs_resolve_path(path);
    if (!node) { errno = ENOENT; return -1; }
    return 0;
}

vfs_dir_t *opendir(const char *path) {
    vfs_node_t *node = vfs_resolve_path(path);
    if (!node) { errno = ENOENT; return NULL; }

    if (node->type == VFS_NODE_MOUNTPOINT) {
        vfs_node_t *inner = (vfs_node_t *)node->priv;
        if (inner) node = inner;
    }

    if (node->type != VFS_NODE_DIR) { errno = ENOTDIR; return NULL; }

    vfs_dir_t *dir = kmalloc(sizeof(vfs_dir_t));
    if (!dir) { errno = ENOSPC; return NULL; }
    dir->node = node;
    dir->pos = 0;
    node->ref_count++;
    return dir;
}

vfs_dirent_t *readdir(vfs_dir_t *dir) {
    if (!dir || !dir->node) { errno = EBADF; return NULL; }
    if (dir->node->type != VFS_NODE_DIR) { errno = ENOTDIR; return NULL; }

    static vfs_dirent_t ent;

    if (dir->node->ops && dir->node->ops->readdir) {
        if (dir->node->ops->readdir(dir->node, (uint32_t)dir->pos, &ent) != 0) return NULL;
        dir->pos++;
        return &ent;
    }

    uint32_t i = 0;
    for (vfs_node_t *c = dir->node->children; c; c = c->next) {
        if (i == (uint32_t)dir->pos) {
            size_t nlen = strlen(c->name);
            if (nlen >= MAX_NAME_LEN) nlen = MAX_NAME_LEN - 1;
            memcpy(ent.d_name, c->name, nlen);
            ent.d_name[nlen] = '\0';
            ent.d_type = c->type;
            ent.d_ino = c->ino;
            dir->pos++;
            return &ent;
        }
        i++;
    }
    return NULL;
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt < 0) { errno = EINVAL; return -1; }
    if (iovcnt > 1024) { errno = EINVAL; return -1; }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t n = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return total ? total : -1;
        total += n;
        if ((size_t)n < iov[i].iov_len) break;
    }
    return total;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt < 0) { errno = EINVAL; return -1; }
    if (iovcnt > 1024) { errno = EINVAL; return -1; }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return total ? total : -1;
        total += n;
    }
    return total;
}

static uint8_t dirent_type_to_dt(uint32_t type) {
    switch (type) {
        case VFS_NODE_DIR:      return DT_DIR;
        case VFS_NODE_SYMLINK:  return DT_LNK;
        case VFS_NODE_DEV:      return DT_CHR;
        default:                return DT_REG;
    }
}

ssize_t getdents64(int fd, void *buf, size_t count) {
    vfs_file_t *file = fd_get(fd);
    if (!file || !file->node) { errno = EBADF; return -1; }

    vfs_node_t *node = file->node;
    if (node->type == VFS_NODE_MOUNTPOINT) {
        vfs_node_t *inner = (vfs_node_t *)node->priv;
        if (inner) node = inner;
    }
    if (!node || node->type != VFS_NODE_DIR) { errno = ENOTDIR; return -1; }

    vfs_dir_t dir;
    dir.node = node;
    dir.pos = (long)file->offset;

    char *out = buf;
    size_t written = 0;

    for (;;) {
        vfs_dirent_t *ent = readdir(&dir);
        if (!ent) break;

        size_t namelen = strlen(ent->d_name);
        size_t reclen = offsetof(struct tinykern_dirent, d_name) + namelen + 1;
        reclen = (reclen + 7) & ~(size_t)7;

        if (written + reclen > count) break;

        struct tinykern_dirent *de = (struct tinykern_dirent *)(out + written);
        de->d_ino = ent->d_ino;
        de->d_off = dir.pos;
        de->d_reclen = (uint16_t)reclen;
        de->d_type = dirent_type_to_dt(ent->d_type);
        memcpy(de->d_name, ent->d_name, namelen);
        de->d_name[namelen] = '\0';

        written += reclen;
    }

    file->offset = dir.pos;
    return written ? (ssize_t)written : 0;
}

int closedir(vfs_dir_t *dir) {
    if (!dir) { errno = EBADF; return -1; }
    dir->node->ref_count--;
    kfree(dir);
    return 0;
}

void rewinddir(vfs_dir_t *dir) {
    if (dir) dir->pos = 0;
}

long telldir(vfs_dir_t *dir) {
    if (!dir) { errno = EBADF; return -1; }
    return (long)dir->pos;
}

void seekdir(vfs_dir_t *dir, long pos) {
    if (dir) dir->pos = (off_t)pos;
}

int dup(int fd) {
    size_t fd_count;
    vfs_file_t **fd_table = current_fd_table(&fd_count);

    if (fd < 0 || (size_t)fd >= fd_count || !fd_table[fd]) { errno = EBADF; return -1; }
    int new_fd = fd_alloc(fd_table, fd_count);
    if (new_fd < 0) return -1;
    fd_table[new_fd] = fd_table[fd];
    vfs_file_ref(fd_table[new_fd]);
    return new_fd;
}

int dup2(int old_fd, int new_fd) {
    size_t fd_count;
    vfs_file_t **fd_table = current_fd_table(&fd_count);

    if (old_fd < 0 || (size_t)old_fd >= fd_count || !fd_table[old_fd]) { errno = EBADF; return -1; }
    if (new_fd < 0 || (size_t)new_fd >= fd_count) { errno = EBADF; return -1; }
    if (old_fd == new_fd) return new_fd;
    if (fd_table[new_fd]) close(new_fd);
    fd_table[new_fd] = fd_table[old_fd];
    vfs_file_ref(fd_table[new_fd]);
    return new_fd;
}

void vfs_fd_table_setup_stdio(vfs_file_t **table, size_t count) {
    if (!table || count < 3) return;

    vfs_node_t *stdin_node = vfs_resolve_path("/dev/stdin");
    vfs_node_t *stdout_node = vfs_resolve_path("/dev/stdout");
    vfs_node_t *stderr_node = vfs_resolve_path("/dev/stderr");

    if (stdin_node) {
        if (table[STDIN_FILENO]) vfs_file_unref(table[STDIN_FILENO]);
        table[STDIN_FILENO] = vfs_file_create(stdin_node, O_RDONLY);
    }
    if (stdout_node) {
        if (table[STDOUT_FILENO]) vfs_file_unref(table[STDOUT_FILENO]);
        table[STDOUT_FILENO] = vfs_file_create(stdout_node, O_WRONLY);
    }
    if (stderr_node) {
        if (table[STDERR_FILENO]) vfs_file_unref(table[STDERR_FILENO]);
        table[STDERR_FILENO] = vfs_file_create(stderr_node, O_WRONLY);
    }
}

void vfs_fd_table_clone(vfs_file_t **dst, vfs_file_t **src, size_t count) {
    if (!dst || !src) return;
    for (size_t i = 0; i < count; i++) {
        dst[i] = src[i];
        vfs_file_ref(dst[i]);
    }
}

void vfs_fd_table_close(vfs_file_t **table, size_t count) {
    if (!table) return;
    for (size_t i = 0; i < count; i++) {
        if (table[i]) {
            vfs_file_unref(table[i]);
            table[i] = NULL;
        }
    }
}

uint8_t vfs_init() {
    if (vfs_ready) return 0;

    vfs_root = vfs_node_alloc("/", VFS_NODE_DIR);
    if (!vfs_root) return VFS_ERR_NO_SLOTS;

    vfs_cwd = vfs_root;

    vfs_fd_table_init(kernel_fd_table, MAX_FD);
    fd_ready = 1;
    mount_count = 0;
    vfs_ready = 1;

    devfs_init();
    vfs_fd_table_setup_stdio(kernel_fd_table, MAX_FD);

    return VFS_OK;
}

vfs_node_t *vfs_get_root() {
    return vfs_root;
}

vfs_node_t *vfs_register_node(const char *path, uint32_t type, vfs_node_ops_t *ops, void *priv) {
    char name[MAX_NAME_LEN];
    vfs_node_t *parent = vfs_resolve_parent(path, name);
    if (!parent) return NULL;
    vfs_node_t *node = vfs_node_alloc(name, type);
    if (!node) return NULL;
    node->ops = ops;
    node->priv = priv;
    vfs_node_link_child(parent, node);
    return node;
}

vfs_node_t *vfs_node_alloc_pub(const char *name, uint32_t type) {
    return vfs_node_alloc(name, type);
}

void vfs_node_link_child_pub(vfs_node_t *parent, vfs_node_t *child) {
    vfs_node_link_child(parent, child);
}

void vfs_node_unlink_child_pub(vfs_node_t *parent, vfs_node_t *child) {
    vfs_node_unlink_child(parent, child);
}

vfs_node_t *vfs_node_find_child_pub(vfs_node_t *parent, const char *name) {
    return vfs_node_find_child(parent, name);
}