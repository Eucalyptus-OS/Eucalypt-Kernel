#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <mem.h>
#include <mm/heap.h>
#include <drivers/block/ahci.h>
#include <drivers/block/nvme.h>
#include <drivers/block/ide.h>
#include <drivers/fs/vfs/blockdev.h>
#include <drivers/fs/fat16/fat16.h>
#include <drivers/fs/devfs/devfs.h>
#include <drivers/fs/vfs/vfs.h>
#include <multitasking/proc.h>
#include <multitasking/sched.h>

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

static uint32_t alloc_ino(void) {
    return next_ino++;
}

void vfs_fd_table_init(vfs_file_t **table, size_t count) {
    if (!table) return;
    for (size_t i = 0; i < count; i++)
        table[i] = NULL;
}

static vfs_file_t **current_fd_table(size_t *count) {
    struct tcb *thread = get_current_thread();
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

static vfs_file_t *vfs_file_create(vfs_node_t *node, int flags) {
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
        if (file->node && file->node->ref_count > 0)
            file->node->ref_count--;
        kfree(file);
    }
}

vfs_node_t *vfs_node_alloc(const char *name, uint32_t type) {
    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (!node) { 
        errno = ENOSPC; return NULL; 
    }
    memset(node, 0, sizeof(vfs_node_t));
    size_t len = strlen(name);
    if (len >= MAX_NAME_LEN) len = MAX_NAME_LEN - 1;
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
            if (len < MAX_NAME_LEN - 1) component[len++] = *p;
            p++;
        }
        component[len] = '\0';

        if (strcmp(component, ".") == 0)  continue;
        if (strcmp(component, "..") == 0) {
            if (node->parent) node = node->parent;
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
        if (nlen >= MAX_NAME_LEN) nlen = MAX_NAME_LEN - 1;
        memcpy(name_out, base, nlen);
        name_out[nlen] = '\0';
    }

    if (!last_slash)
        return vfs_cwd ? vfs_cwd : vfs_root;

    if (last_slash == buf)
        return vfs_root;

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
    tmp[pos] = '\0';

    vfs_node_t *node = vfs_cwd ? vfs_cwd : vfs_root;
    vfs_node_t *cur  = node;

    char segments[64][MAX_NAME_LEN];
    int  depth = 0;

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

static int find_letter_slot(char letter) {
    for (int i = 0; i < mount_count; i++)
        if (mount_table[i].letter == letter) return i;
    return -1;
}

vfs_mount_t *vfs_get_mount(char letter) {
    int slot = find_letter_slot(letter);
    return (slot < 0) ? NULL : &mount_table[slot];
}

static uint8_t ahci_blockdev_read(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return ahci_read(priv->addr.ahci.controller, priv->addr.ahci.port, lba, count, buf);
}

static uint8_t ahci_blockdev_write(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, const void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return ahci_write(priv->addr.ahci.controller, priv->addr.ahci.port, lba, count, buf);
}

static uint8_t nvme_blockdev_read(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return nvme_read(priv->addr.nvme.controller, priv->addr.nvme.nsid, lba, count, buf);
}

static uint8_t nvme_blockdev_write(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, const void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return nvme_write(priv->addr.nvme.controller, priv->addr.nvme.nsid, lba, count, buf);
}

static uint8_t ide_blockdev_read(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return ide_read(priv->addr.ide.bus, priv->addr.ide.drive, lba, count, buf);
}

static uint8_t ide_blockdev_write(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, const void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return ide_write(priv->addr.ide.bus, priv->addr.ide.drive, lba, count, buf);
}

fs_t vfs_get_type(vfs_blockdev_t *blockdev) {
    uint8_t buf[512];
    if (blockdev->read(blockdev, 0, 1, buf) != 0) return (fs_t)-1;
    if (memcmp(buf + 0x36, "FAT12   ", 8) == 0) return fat12;
    if (memcmp(buf + 0x36, "FAT16   ", 8) == 0) return fat16;
    if (memcmp(buf + 0x52, "FAT32   ", 8) == 0) return fat32;
    if (memcmp(buf + 0x03, "EXFAT   ", 8) == 0) return exfat;
    return (fs_t)-1;
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
        fat16_delete_file(priv->vol, priv->dir_cluster, node->name);
        priv->start_cluster = 0;
    }

    uint32_t bytes_written = 0;
    uint16_t new_cluster   = priv->start_cluster;
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
    priv->size          = new_size;
    node->size          = new_size;
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
    out->d_ino  = 0;
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

    cpriv->vol           = dpriv->vol;
    cpriv->start_cluster = handle.start_cluster;
    cpriv->size          = handle.size;
    cpriv->dir_cluster   = dpriv->start_cluster;
    child->priv          = cpriv;
    child->size          = handle.size;
    child->ops           = dir->ops;

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
    uint8_t *tmp = kmalloc(new_size > priv->size ? new_size : priv->size);
    if (!tmp) { errno = ENOSPC; return -1; }
    memset(tmp, 0, new_size > priv->size ? new_size : priv->size);

    if (priv->size > 0) {
        uint32_t bytes_read = 0;
        fat16_read_file(priv->vol, priv->start_cluster, priv->size, tmp, &bytes_read);
    }

    if (priv->start_cluster) {
        fat16_delete_file(priv->vol, priv->dir_cluster, node->name);
        priv->start_cluster = 0;
    }

    uint32_t bytes_written = 0;
    uint16_t new_cluster   = 0;
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
    priv->size          = new_size;
    node->size          = new_size;
    kfree(tmp);
    return 0;
}

static vfs_node_ops_t fat16_ops = {
    .read     = fat16_vfs_read,
    .write    = fat16_vfs_write,
    .readdir  = fat16_vfs_readdir,
    .lookup   = fat16_vfs_lookup,
    .create   = fat16_vfs_create,
    .unlink   = fat16_vfs_unlink,
    .rmdir    = fat16_vfs_rmdir,
    .truncate = fat16_vfs_truncate,
    .rename   = NULL,
    .symlink  = NULL,
    .readlink = NULL,
};

uint8_t vfs_mount(StorageDevType dev, char letter, vfs_dev_addr_t addr) {
    if (find_letter_slot(letter) >= 0) return VFS_ERR_LETTER_IN_USE;
    if (mount_count >= MAX_DRIVES)     return VFS_ERR_NO_SLOTS;

    vfs_blockdev_priv_t *priv = kmalloc(sizeof(vfs_blockdev_priv_t));
    if (!priv) return VFS_ERR_NO_SLOTS;
    priv->addr = addr;

    vfs_blockdev_t blockdev = {0};
    blockdev.priv = priv;

    if (dev == AHCI) {
        uint8_t controller = addr.ahci.controller;
        uint8_t port       = addr.ahci.port;
        if (controller >= ahci_get_controller_count()) { kfree(priv); return VFS_ERR_INVALID_DEV; }
        ahci_controller_t *c = ahci_get_controller(controller);
        if (!c || !c->ports[port].present)              { kfree(priv); return VFS_ERR_INVALID_DEV; }
        if (c->ports[port].assigned_letter != 0)        { kfree(priv); return VFS_ERR_ALREADY_MOUNTED; }
        blockdev.read  = ahci_blockdev_read;
        blockdev.write = ahci_blockdev_write;

    } else if (dev == NVME) {
        uint8_t  controller = addr.nvme.controller;
        uint32_t nsid       = addr.nvme.nsid;
        if (controller >= nvme_get_controller_count()) { kfree(priv); return VFS_ERR_INVALID_DEV; }
        nvme_controller_t *c = nvme_get_controller(controller);
        if (!c || !nvme_namespace_present(c, nsid))    { kfree(priv); return VFS_ERR_INVALID_DEV; }
        if (nvme_namespace_letter(c, nsid) != 0)       { kfree(priv); return VFS_ERR_ALREADY_MOUNTED; }
        blockdev.read  = nvme_blockdev_read;
        blockdev.write = nvme_blockdev_write;

    } else if (dev == IDE) {
        uint8_t bus   = addr.ide.bus;
        uint8_t drive = addr.ide.drive;
        if (bus > 1 || drive > 1)          { kfree(priv); return VFS_ERR_INVALID_DEV; }
        if (!ide_drive_present(bus, drive)) { kfree(priv); return VFS_ERR_INVALID_DEV; }
        if (ide_drive_letter(bus, drive))   { kfree(priv); return VFS_ERR_ALREADY_MOUNTED; }
        blockdev.read  = ide_blockdev_read;
        blockdev.write = ide_blockdev_write;

    } else {
        kfree(priv);
        return VFS_ERR_UNKNOWN_DEV;
    }

    fs_t type = vfs_get_type(&blockdev);
    if (type != fat16) { kfree(priv); return VFS_ERR_FS_INIT; }

    void *vol_ptr = fat16_init(&blockdev);
    if (!vol_ptr)  { kfree(priv); return VFS_ERR_FS_INIT; }

    char mnt_name[3] = { letter, ':', '\0' };
    vfs_node_t *mp_outer = vfs_node_alloc(mnt_name, VFS_NODE_MOUNTPOINT);
    if (!mp_outer) { kfree(priv); return VFS_ERR_NO_SLOTS; }

    vfs_fat16_priv_t *fat_priv = kmalloc(sizeof(vfs_fat16_priv_t));
    if (!fat_priv) { kfree(mp_outer); kfree(priv); return VFS_ERR_NO_SLOTS; }
    fat_priv->vol           = vol_ptr;
    fat_priv->start_cluster = 0;
    fat_priv->size          = 0;
    fat_priv->dir_cluster   = 0;

    vfs_node_t *root_dir = vfs_node_alloc("/", VFS_NODE_DIR);
    if (!root_dir) { kfree(fat_priv); kfree(mp_outer); kfree(priv); return VFS_ERR_NO_SLOTS; }
    root_dir->priv = fat_priv;
    root_dir->ops  = &fat16_ops;

    mp_outer->priv = root_dir;
    vfs_node_link_child(vfs_root, mp_outer);

    mount_table[mount_count].letter   = letter;
    mount_table[mount_count].blockdev = blockdev;
    mount_table[mount_count].dev      = dev;
    mount_table[mount_count].addr     = addr;
    mount_table[mount_count].priv     = vol_ptr;
    mount_count++;

    if (dev == AHCI) {
        ahci_controller_t *c = ahci_get_controller(addr.ahci.controller);
        if (c) c->ports[addr.ahci.port].assigned_letter = letter;
    } else if (dev == NVME) {
        nvme_controller_t *c = nvme_get_controller(addr.nvme.controller);
        if (c) nvme_set_namespace_letter(c, addr.nvme.nsid, letter);
    } else if (dev == IDE) {
        ide_set_drive_letter(addr.ide.bus, addr.ide.drive, letter);
    }

    return VFS_OK;
}

void vfs_unmount(char letter) {
    int slot = find_letter_slot(letter);
    if (slot < 0) return;

    vfs_mount_t *m     = &mount_table[slot];
    vfs_blockdev_priv_t *bpriv = (vfs_blockdev_priv_t *)m->blockdev.priv;

    if (m->dev == AHCI) {
        ahci_controller_t *c = ahci_get_controller(m->addr.ahci.controller);
        if (c) c->ports[m->addr.ahci.port].assigned_letter = 0;
    } else if (m->dev == NVME) {
        nvme_controller_t *c = nvme_get_controller(m->addr.nvme.controller);
        if (c) nvme_set_namespace_letter(c, m->addr.nvme.nsid, 0);
    } else if (m->dev == IDE) {
        ide_set_drive_letter(m->addr.ide.bus, m->addr.ide.drive, 0);
    }

    char mnt_name[3] = { letter, ':', '\0' };
    vfs_node_t *mp = vfs_node_find_child(vfs_root, mnt_name);
    if (mp) {
        vfs_node_unlink_child(vfs_root, mp);
        kfree(mp);
    }

    if (bpriv) kfree(bpriv);

    for (int i = slot; i < mount_count - 1; i++) mount_table[i] = mount_table[i + 1];
    mount_count--;
}

static void node_to_stat(vfs_node_t *node, vfs_stat_t *st) {
    st->st_ino     = node->ino;
    st->st_mode    = node->mode;
    st->st_nlink   = 1;
    st->st_uid     = node->uid;
    st->st_gid     = node->gid;
    st->st_size    = node->size;
    st->st_atime   = node->atime;
    st->st_mtime   = node->mtime;
    st->st_ctime   = node->ctime;
    st->st_blksize = 512;
    st->st_blocks  = (node->size + 511) / 512;
}

static int check_access(vfs_node_t *node, int flags) {
    (void)node; (void)flags;
    return 0;
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
            vfs_node_t *new_node = vfs_node_alloc(name, VFS_NODE_FILE);
            if (!new_node) return -1;
            new_node->mode = S_IFREG | (mode & 0777);
            vfs_node_link_child(parent, new_node);
        }

        node = vfs_resolve_path(path);
        if (!node) return -1;
    } else {
        if ((flags & O_CREAT) && (flags & O_EXCL)) { errno = EEXIST; return -1; }
        if ((flags & O_DIRECTORY) && node->type != VFS_NODE_DIR) { errno = ENOTDIR; return -1; }
    }

    if (check_access(node, flags) != 0) { errno = EACCES; return -1; }

    if ((flags & O_TRUNC) && node->type == VFS_NODE_FILE) {
        if (node->ops && node->ops->truncate)
            node->ops->truncate(node, 0);
        else
            node->size = 0;
    }

    size_t fd_count;
    vfs_file_t **fd_table = current_fd_table(&fd_count);

    int fd = fd_alloc(fd_table, fd_count);
    if (fd < 0) return -1;

    fd_table[fd] = vfs_file_create(node, flags);
    if (!fd_table[fd])
        return -1;

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
    if (!buf)    { errno = EINVAL; return -1; }
    if (!count)  return 0;

    if ((file->flags & O_WRONLY) && !(file->flags & O_RDWR)) {
        errno = EACCES;
        return -1;
    }

    vfs_node_t *node = file->node;
    if (!node) { errno = EBADF; return -1; }

    if (node->type == VFS_NODE_DEV) {
        devfs_dev_t *ddev = (devfs_dev_t *)node->priv;
        if (ddev && ddev->read) {
            ssize_t n = ddev->read(ddev, buf, count);
            if (n > 0) file->offset += n;
            return n;
        }
        errno = EBADF;
        return -1;
    }

    if (!node->ops || !node->ops->read) { errno = EBADF; return -1; }
    ssize_t n = node->ops->read(node, buf, count, file->offset);
    if (n > 0) file->offset += n;
    return n;
}

ssize_t write(int fd, const void *buf, size_t count) {
    vfs_file_t *file = fd_get(fd);
    if (!file) { errno = EBADF; return -1; }
    if (!buf)   { errno = EINVAL; return -1; }
    if (!count) return 0;

    int acc = file->flags & O_RDWR;
    if (acc == O_RDONLY) { errno = EACCES; return -1; }

    vfs_node_t *node = file->node;
    if (!node) { errno = EBADF; return -1; }

    if (file->flags & O_APPEND)
        file->offset = (off_t)node->size;

    if (node->type == VFS_NODE_DEV) {
        devfs_dev_t *ddev = (devfs_dev_t *)node->priv;
        if (ddev && ddev->write) {
            ssize_t n = ddev->write(ddev, buf, count);
            if (n > 0) file->offset += n;
            return n;
        }
        errno = EBADF;
        return -1;
    }

    if (!node->ops || !node->ops->write) { errno = EBADF; return -1; }
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

int stat(const char *path, vfs_stat_t *st) {
    if (!st) { errno = EINVAL; return -1; }
    vfs_node_t *node = vfs_resolve_path(path);
    if (!node) return -1;
    node_to_stat(node, st);
    return 0;
}

int lstat(const char *path, vfs_stat_t *st) {
    if (!st) { errno = EINVAL; return -1; }
    if (!path) { errno = EINVAL; return -1; }

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

int fstat(int fd, vfs_stat_t *st) {
    vfs_file_t *file = fd_get(fd);
    if (!file || !st) { errno = EBADF; return -1; }
    vfs_node_t *node = file->node;
    if (!node) { errno = EBADF; return -1; }
    node_to_stat(node, st);
    return 0;
}

int mkdir(const char *path, uint32_t mode) {
    char name[MAX_NAME_LEN];
    vfs_node_t *parent = vfs_resolve_parent(path, name);
    if (!parent) return -1;

    if (parent->type == VFS_NODE_MOUNTPOINT) {
        vfs_node_t *inner = (vfs_node_t *)parent->priv;
        if (inner) parent = inner;
    }

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

    vfs_node_t *target = vfs_node_find_child(parent, name);
    if (target) {
        if (target->type != VFS_NODE_DIR) { errno = ENOTDIR; return -1; }
        if (target->children)             { errno = ENOTEMPTY; return -1; }
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

    if (parent->ops && parent->ops->unlink) {
        int ret = parent->ops->unlink(parent, name);
        if (ret == 0 && target) {
            vfs_node_unlink_child(parent, target);
            if (target->ref_count == 0) kfree(target);
        }
        return ret;
    }

    if (!target)               { errno = ENOENT; return -1; }
    if (target->ref_count > 0) { errno = EACCES; return -1; }
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

    if (node->ops && node->ops->readlink)
        return node->ops->readlink(node, buf, bufsiz);

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
    dir->pos  = 0;
    node->ref_count++;
    return dir;
}

vfs_dirent_t *readdir(vfs_dir_t *dir) {
    if (!dir || !dir->node) { errno = EBADF; return NULL; }

    static vfs_dirent_t ent;

    if (dir->node->ops && dir->node->ops->readdir) {
        if (dir->node->ops->readdir(dir->node, (uint32_t)dir->pos, &ent) != 0)
            return NULL;
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
            ent.d_ino  = c->ino;
            dir->pos++;
            return &ent;
        }
        i++;
    }
    return NULL;
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
    if (new_fd < 0 || (size_t)new_fd >= fd_count)                      { errno = EBADF; return -1; }
    if (old_fd == new_fd) return new_fd;
    if (fd_table[new_fd]) close(new_fd);
    fd_table[new_fd] = fd_table[old_fd];
    vfs_file_ref(fd_table[new_fd]);
    return new_fd;
}

void vfs_fd_table_setup_stdio(vfs_file_t **table, size_t count) {
    if (!table || count < 3)
        return;

    vfs_node_t *stdin_node  = vfs_resolve_path("/dev/stdin");
    vfs_node_t *stdout_node = vfs_resolve_path("/dev/stdout");
    vfs_node_t *stderr_node = vfs_resolve_path("/dev/stderr");

    if (stdin_node) {
        if (table[STDIN_FILENO])
            vfs_file_unref(table[STDIN_FILENO]);
        table[STDIN_FILENO] = vfs_file_create(stdin_node, O_RDONLY);
    }

    if (stdout_node) {
        if (table[STDOUT_FILENO])
            vfs_file_unref(table[STDOUT_FILENO]);
        table[STDOUT_FILENO] = vfs_file_create(stdout_node, O_WRONLY);
    }

    if (stderr_node) {
        if (table[STDERR_FILENO])
            vfs_file_unref(table[STDERR_FILENO]);
        table[STDERR_FILENO] = vfs_file_create(stderr_node, O_WRONLY);
    }
}

void vfs_fd_table_clone(vfs_file_t **dst, vfs_file_t **src, size_t count) {
    if (!dst || !src)
        return;

    for (size_t i = 0; i < count; i++) {
        dst[i] = src[i];
        vfs_file_ref(dst[i]);
    }
}

void vfs_fd_table_close(vfs_file_t **table, size_t count) {
    if (!table)
        return;

    for (size_t i = 0; i < count; i++) {
        if (table[i]) {
            vfs_file_unref(table[i]);
            table[i] = NULL;
        }
    }
}

uint8_t vfs_init(void) {
    if (vfs_ready) return 0;

    vfs_root = vfs_node_alloc("/", VFS_NODE_DIR);
    if (!vfs_root) return VFS_ERR_NO_SLOTS;

    vfs_cwd = vfs_root;

    vfs_fd_table_init(kernel_fd_table, MAX_FD);
    fd_ready = 1;
    mount_count = 0;
    vfs_ready   = 1;

    devfs_init();
    vfs_fd_table_setup_stdio(kernel_fd_table, MAX_FD);

    char letter = 'C';
    uint8_t ctrl_count = ahci_get_controller_count();
    for (uint8_t c = 0; c < ctrl_count; c++) {
        ahci_controller_t *ctrl = ahci_get_controller(c);
        if (!ctrl) continue;
        for (uint8_t p = 0; p < AHCI_MAX_PORTS; p++) {
            if (!ctrl->ports[p].present) continue;
            if (letter > 'Z') return VFS_ERR_NO_SLOTS;
            vfs_dev_addr_t addr = { .ahci = { .controller = c, .port = p } };
            uint8_t err = vfs_mount(AHCI, letter, addr);
            if (err == VFS_ERR_NO_SLOTS) return VFS_ERR_NO_SLOTS;
            letter++;
        }
    }

    uint8_t nvme_count = nvme_get_controller_count();
    for (uint8_t c = 0; c < nvme_count; c++) {
        nvme_controller_t *ctrl = nvme_get_controller(c);
        if (!ctrl) continue;
        for (uint32_t ns = 1; ns <= nvme_get_namespace_count(ctrl); ns++) {
            if (!nvme_namespace_present(ctrl, ns)) continue;
            if (letter > 'Z') return VFS_ERR_NO_SLOTS;
            vfs_dev_addr_t addr = { .nvme = { .controller = c, .nsid = ns } };
            uint8_t err = vfs_mount(NVME, letter, addr);
            if (err == VFS_ERR_NO_SLOTS) return VFS_ERR_NO_SLOTS;
            letter++;
        }
    }

    for (uint8_t bus = 0; bus <= 1; bus++) {
        for (uint8_t drive = 0; drive <= 1; drive++) {
            if (!ide_drive_present(bus, drive)) continue;
            if (letter > 'Z') return VFS_ERR_NO_SLOTS;
            vfs_dev_addr_t addr = { .ide = { .bus = bus, .drive = drive } };
            uint8_t err = vfs_mount(IDE, letter, addr);
            if (err == VFS_ERR_NO_SLOTS) return VFS_ERR_NO_SLOTS;
            letter++;
        }
    }

    return VFS_OK;
}

vfs_node_t *vfs_get_root(void) {
    return vfs_root;
}

vfs_node_t *vfs_register_node(const char *path, uint32_t type, vfs_node_ops_t *ops, void *priv) {
    char name[MAX_NAME_LEN];
    vfs_node_t *parent = vfs_resolve_parent(path, name);
    if (!parent) return NULL;
    vfs_node_t *node = vfs_node_alloc(name, type);
    if (!node) return NULL;
    node->ops  = ops;
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
