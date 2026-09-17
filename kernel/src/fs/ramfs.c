#include <stdint.h>
#include <stddef.h>
#include <mm/memory.h>
#include <mm/heap.h>
#include <fs/vfs.h>
#include <fs/ramfs.h>
#include <logging/print.h>

// Copy file bytes out of the ramfs buffer, clamped to the file size
static ssize_t ramfs_file_read(vfs_node_t *node, void *buf, size_t count, off_t offset) {
    if (offset < 0) return -1;
    if ((uint64_t)offset >= node->size) return 0;

    size_t avail = node->size - (size_t)offset;
    if (count > avail) count = avail;

    memcpy(buf, (uint8_t *)node->priv + offset, count);
    return (ssize_t)count;
}

// Grow the node's buffer if needed, then copy the write payload in
static ssize_t ramfs_file_write(vfs_node_t *node, const void *buf, size_t count, off_t offset) {
    if (offset < 0) return -1;

    size_t new_size = (size_t)offset + count;
    if (new_size > node->size) {
        uint8_t *ndata = kmalloc(new_size);
        if (!ndata) return -1;

        if (node->priv) {
            memcpy(ndata, node->priv, node->size);
            kfree(node->priv);
        }
        if ((size_t)offset > node->size) {
            memset(ndata + node->size, 0, (size_t)offset - node->size);   // zero-fill the write gap
        }

        node->priv = ndata;
        node->size = new_size;
    }

    memcpy((uint8_t *)node->priv + offset, buf, count);
    return (ssize_t)count;
}

// Resize the file, keeping min(old,new) contents and zero-filling when growing
static int ramfs_file_truncate(vfs_node_t *node, off_t length) {
    if (length < 0) return -1;
    if ((uint64_t)length == node->size) return 0;

    if (length == 0) {
        if (node->priv) kfree(node->priv);
        node->priv = NULL;
        node->size = 0;
        return 0;
    }

    uint8_t *ndata = kmalloc((size_t)length);
    if (!ndata) return -1;

    if (node->priv) {
        size_t keep = node->size < (size_t)length ? node->size : (size_t)length;
        memcpy(ndata, node->priv, keep);
        kfree(node->priv);
    }

    node->priv = ndata;
    node->size = (size_t)length;
    return 0;
}

static vfs_node_ops_t ramfs_dir_ops;
static vfs_node_ops_t ramfs_file_ops;

// Allocate a directory or file node with the right ops and link it into |dir|
static int ramfs_create(vfs_node_t *dir, const char *name, uint32_t type, uint32_t mode) {
    (void)mode;
    vfs_node_t *node = vfs_node_alloc_pub(name, type);
    if (!node) return -1;

    if (type == VFS_NODE_DIR) {
        node->ops = &ramfs_dir_ops;
    } else {
        node->ops = &ramfs_file_ops;
        node->priv = NULL;
        node->size = 0;
    }

    vfs_node_link_child_pub(dir, node);
    return 0;
}

// Free a file's buffer and mark it gone; directories are rejected
static int ramfs_unlink(vfs_node_t *dir, const char *name) {
    vfs_node_t *target = vfs_node_find_child_pub(dir, name);
    if (!target) { errno = ENOENT; return -1; }
    if (target->type == VFS_NODE_DIR) { errno = EISDIR; return -1; }

    if (target->priv) kfree(target->priv);
    target->priv = NULL;
    target->size = 0;
    return 0;
}

// Remove a directory, which must exist and be empty
static int ramfs_rmdir(vfs_node_t *dir, const char *name) {
    vfs_node_t *target = vfs_node_find_child_pub(dir, name);
    if (!target) { errno = ENOENT; return -1; }
    if (target->type != VFS_NODE_DIR) { errno = ENOTDIR; return -1; }
    if (target->children) { errno = ENOTEMPTY; return -1; }
    return 0;
}

// Re-parent a node (or rename) into a new directory under a new name
static int ramfs_rename(vfs_node_t *old_dir, const char *old_name,
                        vfs_node_t *new_dir, const char *new_name) {
    vfs_node_t *target = vfs_node_find_child_pub(old_dir, old_name);
    if (!target) { errno = ENOENT; return -1; }
    if (vfs_node_find_child_pub(new_dir, new_name)) { errno = EEXIST; return -1; }

    vfs_node_unlink_child_pub(old_dir, target);
    vfs_node_link_child_pub(new_dir, target);
    memcpy(target->name, new_name, strlen(new_name) + 1);
    return 0;
}

static vfs_node_ops_t ramfs_dir_ops = {
    .read     = NULL,
    .write    = NULL,
    .readdir  = NULL,
    .lookup   = NULL,
    .create   = ramfs_create,
    .unlink   = ramfs_unlink,
    .rmdir    = ramfs_rmdir,
    .rename   = ramfs_rename,
    .truncate = NULL,
    .symlink  = NULL,
    .readlink = NULL,
};

static vfs_node_ops_t ramfs_file_ops = {
    .read     = ramfs_file_read,
    .write    = ramfs_file_write,
    .readdir  = NULL,
    .lookup   = NULL,
    .create   = NULL,
    .unlink   = NULL,
    .rmdir    = NULL,
    .rename   = NULL,
    .truncate = ramfs_file_truncate,
    .symlink  = NULL,
    .readlink = NULL,
};

// Register a new empty directory at |path| backed by ramfs node ops
uint8_t ramfs_mount(const char *path) {
    vfs_node_t *root = vfs_register_node(path, VFS_NODE_DIR, &ramfs_dir_ops, NULL);
    if (!root) {
        print("ramfs: failed to register %s\n", path);
        return VFS_ERR_FS_INIT;
    }
    print("ramfs: mounted %s\n", path);
    return VFS_OK;
}
