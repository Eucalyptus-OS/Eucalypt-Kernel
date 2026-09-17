#include <stdint.h>
#include <stddef.h>
#include <mm/memory.h>
#include <mm/heap.h>
#include <logging/print.h>
#include <fs/vfs.h>
#include <fs/ustar.h>

#define USTAR_BLOCK 512           // every tar entry header and data chunk is 512 bytes

// On-disk ustar header; the fields below are byte offsets inside the 512-byte block
typedef struct {
    char name[100];          // member path, NUL-or-space padded
    char mode[8];            // permissions as octal digits
    char uid[8];
    char gid[8];
    char size[12];           // file size as octal digits (offset 124)
    char mtime[12];
    char checksum[8];
    char typeflag;           // '0'/'file', '5'=dir, '2'=symlink
    char linkname[100];      // symlink target (offset 157)
    char magic[6];           // "ustar\0" marks a ustar format header
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];        // length extension prepended to name, joined with '/'
    char pad[12];
} ustar_header_t;

// Decode an octal-encoded numeric field, stopping at spaces or NULs
static uint64_t ustar_octal(const char *s, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] >= '0' && s[i] <= '7') {
            v = (v << 3) | (uint64_t)(s[i] - '0');
        } else if (s[i] == ' ' || s[i] == '\0') {
            break;
        } else {
            break;
        }
    }
    return v;
}

// True when the whole block is zeroes, i.e. the archive's end marker
static int ustar_is_zero_block(const uint8_t *b) {
    for (size_t i = 0; i < USTAR_BLOCK; i++) {
        if (b[i]) return 0;
    }
    return 1;
}

// Bounded strlen for fixed-width, non-NUL-terminated header fields
static size_t ustar_strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n] != '\0') n++;
    return n;
}

// Copy file bytes out of the archive-resident copy, clamped to the file size
static ssize_t ustar_file_read(vfs_node_t *node, void *buf, size_t count, off_t offset) {
    if (offset < 0) return -1;
    if ((uint64_t)offset >= node->size) return 0;

    size_t avail = node->size - (size_t)offset;
    if (count > avail) count = avail;

    memcpy(buf, (uint8_t *)node->priv + offset, count);
    return (ssize_t)count;
}

// Grow the node's buffer if needed, then copy the write payload in
static ssize_t ustar_file_write(vfs_node_t *node, const void *buf, size_t count, off_t offset) {
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
            memset(ndata + node->size, 0, (size_t)offset - node->size);
        }

        node->priv = ndata;
        node->size = new_size;
    }

    memcpy((uint8_t *)node->priv + offset, buf, count);
    return (ssize_t)count;
}

// Resize the file, keeping min(old,new) contents and zero-filling when growing
static int ustar_file_truncate(vfs_node_t *node, off_t length) {
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

// Read the symlink target stored in node->priv
static ssize_t ustar_symlink_readlink(vfs_node_t *node, char *buf, size_t bufsiz) {
    if (!node->priv) return -1;

    size_t len = strlen((const char *)node->priv);
    if (len > bufsiz - 1) len = bufsiz - 1;
    memcpy(buf, node->priv, len);
    buf[len] = '\0';
    return (ssize_t)len;
}

static vfs_node_ops_t ustar_dir_ops;
static vfs_node_ops_t ustar_file_ops;
static vfs_node_ops_t ustar_symlink_ops;

// Allocate a directory or file node with the right ops and link it into |dir|
static int ustar_create(vfs_node_t *dir, const char *name, uint32_t type, uint32_t mode) {
    (void)mode;
    vfs_node_t *node = vfs_node_alloc_pub(name, type);
    if (!node) return -1;

    if (type == VFS_NODE_DIR) {
        node->ops = &ustar_dir_ops;
    } else {
        node->ops = &ustar_file_ops;
        node->priv = NULL;
        node->size = 0;
    }

    vfs_node_link_child_pub(dir, node);
    return 0;
}

// Free a file's buffer and mark it gone; directories are rejected
static int ustar_unlink(vfs_node_t *dir, const char *name) {
    vfs_node_t *target = vfs_node_find_child_pub(dir, name);
    if (!target) { errno = ENOENT; return -1; }
    if (target->type == VFS_NODE_DIR) { errno = EISDIR; return -1; }

    if (target->priv) kfree(target->priv);
    target->priv = NULL;
    target->size = 0;
    return 0;
}

// Remove a directory, which must exist and be empty
static int ustar_rmdir(vfs_node_t *dir, const char *name) {
    vfs_node_t *target = vfs_node_find_child_pub(dir, name);
    if (!target) { errno = ENOENT; return -1; }
    if (target->type != VFS_NODE_DIR) { errno = ENOTDIR; return -1; }
    if (target->children) { errno = ENOTEMPTY; return -1; }
    return 0;
}

// Re-parent a node (or rename) into a new directory under a new name
static int ustar_rename(vfs_node_t *old_dir, const char *old_name,
                        vfs_node_t *new_dir, const char *new_name) {
    vfs_node_t *target = vfs_node_find_child_pub(old_dir, old_name);
    if (!target) { errno = ENOENT; return -1; }
    if (vfs_node_find_child_pub(new_dir, new_name)) { errno = EEXIST; return -1; }

    vfs_node_unlink_child_pub(old_dir, target);
    vfs_node_link_child_pub(new_dir, target);
    memcpy(target->name, new_name, strlen(new_name) + 1);
    return 0;
}

static vfs_node_ops_t ustar_dir_ops = {
    .read     = NULL,
    .write    = NULL,
    .readdir  = NULL,
    .lookup   = NULL,
    .create   = ustar_create,
    .unlink   = ustar_unlink,
    .rmdir    = ustar_rmdir,
    .rename   = ustar_rename,
    .truncate = NULL,
    .symlink  = NULL,
    .readlink = NULL,
};

static vfs_node_ops_t ustar_file_ops = {
    .read     = ustar_file_read,
    .write    = ustar_file_write,
    .readdir  = NULL,
    .lookup   = NULL,
    .create   = NULL,
    .unlink   = NULL,
    .rmdir    = NULL,
    .rename   = NULL,
    .truncate = ustar_file_truncate,
    .symlink  = NULL,
    .readlink = NULL,
};

// Strip leading "./" segments that tar uses to mark plain relative paths
static const char *ustar_strip_dotslash(const char *path) {
    while (path[0] == '.' && path[1] == '/') {
        path += 2;
    }
    return path;
}

// Walk |path| creating any missing directory components under |root|
static vfs_node_t *ustar_ensure_dir(vfs_node_t *root, const char *path) {
    vfs_node_t *cur = root;
    const char *p = path;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != '/') p++;

        size_t len = (size_t)(p - start);
        if (len >= MAX_NAME_LEN) return NULL;

        char comp[MAX_NAME_LEN];
        memcpy(comp, start, len);
        comp[len] = '\0';

        vfs_node_t *child = vfs_node_find_child_pub(cur, comp);
        if (!child) {
            child = vfs_node_alloc_pub(comp, VFS_NODE_DIR);
            if (!child) return NULL;
            child->ops = &ustar_dir_ops;
            vfs_node_link_child_pub(cur, child);
        }

        cur = child;
    }
    return cur;
}

// Materialize a regular file node holding a heap copy of the archive's bytes
static int ustar_add_file(vfs_node_t *root, const char *path,
                          const uint8_t *data, uint64_t size) {
    char full[PATH_MAX];
    size_t plen = strlen(path);
    if (plen >= PATH_MAX) return -1;
    memcpy(full, path, plen + 1);

    char *last = NULL;
    for (char *c = full; *c; c++) {
        if (*c == '/') last = c;
    }

    char *name = (last != NULL) ? last + 1 : full;
    if (*name == '\0') return 0;

    vfs_node_t *dir = root;
    if (last != NULL) {
        *last = '\0';
        dir = ustar_ensure_dir(root, full);
        if (!dir) return -1;
    }

    if (vfs_node_find_child_pub(dir, name)) return 0;

    vfs_node_t *node = vfs_node_alloc_pub(name, VFS_NODE_FILE);
    if (!node) return -1;
    node->ops = &ustar_file_ops;

    if (size > 0) {
        uint8_t *copy = kmalloc(size);
        if (!copy) {
            kfree(node);
            return -1;
        }
        memcpy(copy, data, size);
        node->priv = copy;
    }
    node->size = size;

    vfs_node_link_child_pub(dir, node);
    return 0;
}

// Materialize a symlink node whose priv holds a heap copy of the target
static int ustar_add_symlink(vfs_node_t *root, const char *path, const char *target) {
    char full[PATH_MAX];
    size_t plen = strlen(path);
    if (plen >= PATH_MAX) return -1;
    memcpy(full, path, plen + 1);

    char *last = NULL;
    for (char *c = full; *c; c++) {
        if (*c == '/') last = c;
    }

    char *name = (last != NULL) ? last + 1 : full;
    if (*name == '\0') return 0;

    vfs_node_t *dir = root;
    if (last != NULL) {
        *last = '\0';
        dir = ustar_ensure_dir(root, full);
        if (!dir) return -1;
    }

    if (vfs_node_find_child_pub(dir, name)) return 0;

    vfs_node_t *node = vfs_node_alloc_pub(name, VFS_NODE_SYMLINK);
    if (!node) return -1;
    node->ops = &ustar_symlink_ops;

    size_t tlen = strlen(target);
    char *copy = kmalloc(tlen + 1);
    if (!copy) {
        kfree(node);
        return -1;
    }
    memcpy(copy, target, tlen + 1);
    node->priv = copy;

    vfs_node_link_child_pub(dir, node);
    return 0;
}

static vfs_node_ops_t ustar_symlink_ops = {
    .read     = NULL,
    .write    = NULL,
    .readdir  = NULL,
    .lookup   = NULL,
    .create   = NULL,
    .unlink   = NULL,
    .rmdir    = NULL,
    .rename   = NULL,
    .truncate = NULL,
    .symlink  = NULL,
    .readlink = ustar_symlink_readlink,
};

// Register |path| and replay the whole tar image into the VFS tree
uint8_t ustar_mount(const char *path, const void *image, size_t size) {
    const uint8_t *p = (const uint8_t *)image;
    const uint8_t *end = p + size;

    vfs_node_t *root = vfs_register_node(path, VFS_NODE_DIR, &ustar_dir_ops, NULL);
    if (!root) {
        print("ustar: failed to register %s\n", path);
        return VFS_ERR_FS_INIT;
    }

    uint64_t entries = 0;
    while (p + USTAR_BLOCK <= end) {
        if (ustar_is_zero_block(p)) break;   // two zero blocks end the archive

        const ustar_header_t *h = (const ustar_header_t *)p;

        char full[PATH_MAX];
        if (h->prefix[0] != '\0') {
            // Long names split across prefix/name: rejoin with a single '/'
            size_t plen = ustar_strnlen(h->prefix, sizeof(h->prefix));
            size_t nlen = ustar_strnlen(h->name, sizeof(h->name));
            if (plen + 1 + nlen >= PATH_MAX) {
                p += USTAR_BLOCK;
                continue;
            }
            memcpy(full, h->prefix, plen);
            full[plen] = '/';
            memcpy(full + plen + 1, h->name, nlen);
            full[plen + 1 + nlen] = '\0';
        } else {
            size_t nlen = ustar_strnlen(h->name, sizeof(h->name));
            memcpy(full, h->name, nlen);
            full[nlen] = '\0';
        }

        const char *rel = ustar_strip_dotslash(full);
        uint64_t fsize = ustar_octal(h->size, sizeof(h->size));
        char typeflag = h->typeflag;
        const uint8_t *data = p + USTAR_BLOCK;   // file contents follow the header block

        // A directory is a '5' typeflag, or any name ending in '/'
        int is_dir = (typeflag == '5');
        if (rel[0] == '\0') is_dir = 1;
        if (rel[0] != '\0' && rel[strlen(rel) - 1] == '/') is_dir = 1;

        if (is_dir) {
            ustar_ensure_dir(root, rel);
        } else if (typeflag == '2') {   // typeflag '2' = link entry; target string lives in linkname
            size_t tlen = ustar_strnlen(h->linkname, sizeof(h->linkname));
            char lt[sizeof(h->linkname) + 1];
            memcpy(lt, h->linkname, tlen);
            lt[tlen] = '\0';
            ustar_add_symlink(root, rel, lt);
        } else {
            ustar_add_file(root, rel, data, fsize);
        }

        // Advance past the header, the entry data, and its padding to the next 512-byte boundary
        p += USTAR_BLOCK + ((fsize + USTAR_BLOCK - 1) / USTAR_BLOCK) * USTAR_BLOCK;
        entries++;
    }

    print("ustar: mounted %s (%d entries)\n", path, (int)entries);
    return VFS_OK;
}
