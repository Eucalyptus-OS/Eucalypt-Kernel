#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <mem.h>
#include <mm/heap.h>
#include <logging/printk.h>
#include <drivers/fs/vfs/vfs.h>
#include <drivers/block/ramfs.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

void    *ramfs_addr = NULL;
uint64_t ramfs_size = 0;

#define TAR_BLOCK           512
#define TAR_NAME_OFF        0
#define TAR_NAME_LEN        100
#define TAR_SIZE_OFF        124
#define TAR_SIZE_LEN        12
#define TAR_TYPE_OFF        156
#define TAR_PREFIX_OFF      345
#define TAR_PREFIX_LEN      155

#define TAR_TYPE_FILE        '0'
#define TAR_TYPE_HARDLINK    '1'
#define TAR_TYPE_SYMLINK     '2'
#define TAR_TYPE_DIR         '5'
#define TAR_TYPE_GNU_LONG    'L'
#define TAR_TYPE_GNU_LONGLNK 'K'

#define MAX_NAME 256
#define MAX_PATH 4096

typedef struct {
    uint8_t  *buf;
    uint64_t  size;
    uint64_t  capacity;
} ramfs_vol_t;

typedef struct {
    ramfs_vol_t *vol;
    int64_t      hdr_offset;
    char         tar_path[MAX_PATH];
} ramfs_node_priv_t;

static ramfs_vol_t *g_vol  = NULL;
static vfs_node_t  *g_root = NULL;

static unsigned long tar_octal(const uint8_t *s, int n) {
    unsigned long v = 0;
    int i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\0')) i++;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '7') break;
        v = (v << 3) + (s[i] - '0');
    }
    return v;
}

static void tar_write_octal(uint8_t *dst, unsigned long val, int width) {
    dst[--width] = '\0';
    while (width > 0) {
        dst[--width] = '0' + (uint8_t)(val & 7);
        val >>= 3;
    }
}

static void tar_set_checksum(uint8_t *hdr) {
    memset(hdr + 148, ' ', 8);
    unsigned long sum = 0;
    for (int i = 0; i < TAR_BLOCK; i++) sum += hdr[i];
    tar_write_octal(hdr + 148, sum, 8);
    hdr[154] = '\0';
    hdr[155] = ' ';
}

static void copy_field(char *dest, const unsigned char *src, int size, int dest_size) {
    int i = 0;
    for (; i < size && i + 1 < dest_size; i++) {
        if (src[i] == '\0') break;
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

static int tar_fullname(const uint8_t *hdr, char *out, int out_sz) {
    int allzero = 1;
    for (int i = 0; i < TAR_BLOCK; i++) {
        if (hdr[i]) { allzero = 0; break; }
    }
    if (allzero) return 0;

    char name[TAR_NAME_LEN + 1];
    char prefix[TAR_PREFIX_LEN + 1];

    int nlen = 0;
    for (; nlen < TAR_NAME_LEN && hdr[TAR_NAME_OFF + nlen]; nlen++);
    memcpy(name, hdr + TAR_NAME_OFF, nlen);
    name[nlen] = '\0';

    int plen = 0;
    for (; plen < TAR_PREFIX_LEN && hdr[TAR_PREFIX_OFF + plen]; plen++);
    memcpy(prefix, hdr + TAR_PREFIX_OFF, plen);
    prefix[plen] = '\0';

    if (plen > 0) {
        int total = plen + 1 + nlen;
        if (total >= out_sz) total = out_sz - 1;
        memcpy(out, prefix, plen);
        out[plen] = '/';
        memcpy(out + plen + 1, name, nlen);
        out[total] = '\0';
    } else {
        if (nlen >= out_sz) nlen = out_sz - 1;
        memcpy(out, name, nlen);
        out[nlen] = '\0';
    }
    return 1;
}

static void strip_trailing_slash(char *s) {
    int len = strlen(s);
    if (len > 0 && s[len - 1] == '/') s[len - 1] = '\0';
}

static int path_depth(const char *p) {
    if (!p || *p == '\0') return 0;
    int d = 1;
    for (; *p; p++) if (*p == '/') d++;
    return d;
}

static unsigned long tar_lookup(unsigned char *archive, const char *filename, char **out) {
    unsigned char *ptr = archive;
    for (;;) {
        int allzero = 1;
        for (int i = 0; i < 512; i++) {
            if (ptr[i]) { allzero = 0; break; }
        }
        if (allzero) break;

        unsigned char type = ptr[156];
        unsigned long size = tar_octal(ptr + 124, 12);

        char fullname[512];
        fullname[0] = '\0';

        if (type == 'L' || type == 'K') {
            unsigned char *nameptr = ptr + 512;
            int namelen = (int)size;
            if (namelen >= (int)sizeof(fullname)) namelen = sizeof(fullname) - 1;
            memcpy(fullname, nameptr, namelen);
            fullname[namelen] = '\0';
            ptr += (((size + 511) / 512) + 1) * 512;
            type = ptr[156];
            size = tar_octal(ptr + 124, 12);
        }

        if (fullname[0] == '\0') {
            char name[101];
            char prefix[156];
            copy_field(name, ptr + 0, 100, sizeof(name));
            copy_field(prefix, ptr + 345, 155, sizeof(prefix));

            if (prefix[0]) {
                int plen = strlen(prefix);
                int nlen = strlen(name);
                if (plen + 1 + nlen >= (int)sizeof(fullname)) {
                    memcpy(fullname, prefix, sizeof(fullname) - 1);
                    fullname[sizeof(fullname) - 1] = '\0';
                } else {
                    memcpy(fullname, prefix, plen);
                    fullname[plen] = '/';
                    memcpy(fullname + plen + 1, name, nlen);
                    fullname[plen + 1 + nlen] = '\0';
                }
            } else {
                copy_field(fullname, ptr + 0, 100, sizeof(fullname));
            }
        }

        if (fullname[0] && (type == '0' || type == '\0' || type == '7')) {
            if (!memcmp(fullname, filename, strlen(filename) + 1)) {
                *out = (char *)(ptr + 512);
                return size;
            }
        }

        ptr += (((size + 511) / 512) + 1) * 512;
    }
    return 0;
}

static int64_t tar_find(ramfs_vol_t *vol, const char *tar_path) {
    uint8_t *ptr = vol->buf;
    uint8_t *end = vol->buf + vol->size;
    char cur_long_name[MAX_PATH];
    cur_long_name[0] = '\0';

    while (ptr + TAR_BLOCK <= end) {
        uint8_t type = ptr[TAR_TYPE_OFF];
        unsigned long size = tar_octal(ptr + TAR_SIZE_OFF, TAR_SIZE_LEN);

        int allzero = 1;
        for (int i = 0; i < TAR_BLOCK; i++) {
            if (ptr[i]) { allzero = 0; break; }
        }
        if (allzero) break;

        if (type == TAR_TYPE_GNU_LONG || type == TAR_TYPE_GNU_LONGLNK) {
            int namelen = (int)size;
            if (namelen >= MAX_PATH) namelen = MAX_PATH - 1;
            memcpy(cur_long_name, ptr + TAR_BLOCK, namelen);
            cur_long_name[namelen] = '\0';
            ptr += (((size + TAR_BLOCK - 1) / TAR_BLOCK) + 1) * TAR_BLOCK;
            continue;
        }

        char fullname[MAX_PATH];
        if (cur_long_name[0]) {
            memcpy(fullname, cur_long_name, MAX_PATH);
            cur_long_name[0] = '\0';
        } else {
            if (!tar_fullname(ptr, fullname, MAX_PATH)) break;
        }
        strip_trailing_slash(fullname);

        if (strcmp(fullname, tar_path) == 0)
            return (int64_t)(ptr - vol->buf);

        ptr += (((size + TAR_BLOCK - 1) / TAR_BLOCK) + 1) * TAR_BLOCK;
    }
    return -1;
}

static void tar_build_header(uint8_t *out, const char *tar_path,
                              uint8_t type, uint64_t size) {
    memset(out, 0, TAR_BLOCK);
    int nlen = strlen(tar_path);
    if (nlen > TAR_NAME_LEN - 1) nlen = TAR_NAME_LEN - 1;
    memcpy(out + TAR_NAME_OFF, tar_path, nlen);
    memcpy(out + 100, "0000644\0", 8);
    memcpy(out + 108, "0000000\0", 8);
    memcpy(out + 116, "0000000\0", 8);
    tar_write_octal(out + TAR_SIZE_OFF, (unsigned long)size, TAR_SIZE_LEN);
    tar_write_octal(out + 136, 0, 12);
    out[TAR_TYPE_OFF] = type;
    memcpy(out + 257, "ustar  \0", 8);
    tar_set_checksum(out);
}

static int tar_delete_entry(ramfs_vol_t *vol, int64_t hdr_off) {
    if (hdr_off < 0 || (uint64_t)hdr_off >= vol->size) return -1;
    uint8_t *hdr = vol->buf + hdr_off;
    unsigned long data_size = tar_octal(hdr + TAR_SIZE_OFF, TAR_SIZE_LEN);
    uint64_t entry_blocks = ((data_size + TAR_BLOCK - 1) / TAR_BLOCK) + 1;
    uint64_t entry_bytes  = entry_blocks * TAR_BLOCK;
    if ((uint64_t)hdr_off + entry_bytes > vol->size) return -1;
    uint64_t tail = vol->size - ((uint64_t)hdr_off + entry_bytes);
    memmove(hdr, hdr + entry_bytes, tail);
    vol->size -= entry_bytes;
    memset(vol->buf + vol->size, 0, entry_bytes);
    return 0;
}

static int64_t tar_append_entry(ramfs_vol_t *vol, const char *tar_path,
                                 uint8_t type, const uint8_t *data,
                                 uint64_t data_size) {
    uint64_t data_blocks = (data_size + TAR_BLOCK - 1) / TAR_BLOCK;
    uint64_t need = (1 + data_blocks) * TAR_BLOCK;
    if (vol->size + need + 2 * TAR_BLOCK > vol->capacity) return -1;

    uint64_t insert_at = vol->size;
    while (insert_at >= TAR_BLOCK) {
        int allzero = 1;
        for (int i = 0; i < TAR_BLOCK; i++) {
            if (vol->buf[insert_at - TAR_BLOCK + i]) { allzero = 0; break; }
        }
        if (!allzero) break;
        insert_at -= TAR_BLOCK;
    }

    uint8_t *hdr_ptr = vol->buf + insert_at;
    tar_build_header(hdr_ptr, tar_path, type, data_size);

    if (data && data_size > 0) {
        memcpy(hdr_ptr + TAR_BLOCK, data, data_size);
        uint64_t pad = data_blocks * TAR_BLOCK - data_size;
        if (pad) memset(hdr_ptr + TAR_BLOCK + data_size, 0, pad);
    }

    uint64_t end = insert_at + (1 + data_blocks) * TAR_BLOCK;
    memset(vol->buf + end, 0, 2 * TAR_BLOCK);
    vol->size = end + 2 * TAR_BLOCK;
    return (int64_t)insert_at;
}

static ssize_t     ramfs_vfs_read   (vfs_node_t *, void *, size_t, off_t);
static ssize_t     ramfs_vfs_write  (vfs_node_t *, const void *, size_t, off_t);
static int         ramfs_vfs_readdir(vfs_node_t *, uint32_t, vfs_dirent_t *);
static vfs_node_t *ramfs_vfs_lookup (vfs_node_t *, const char *);
static int         ramfs_vfs_create (vfs_node_t *, const char *, uint32_t, uint32_t);
static int         ramfs_vfs_unlink (vfs_node_t *, const char *);
static int         ramfs_vfs_rmdir  (vfs_node_t *, const char *);

static vfs_node_ops_t ramfs_ops = {
    .read    = ramfs_vfs_read,
    .write   = ramfs_vfs_write,
    .readdir = ramfs_vfs_readdir,
    .lookup  = ramfs_vfs_lookup,
    .create  = ramfs_vfs_create,
    .unlink  = ramfs_vfs_unlink,
    .rmdir   = ramfs_vfs_rmdir,
};

static vfs_node_t *ramfs_node_alloc(const char *vfs_name, uint32_t type,
                                     ramfs_vol_t *vol, int64_t hdr_offset,
                                     const char *tar_path, uint64_t size) {
    vfs_node_t *node = vfs_node_alloc_pub(vfs_name, type);
    if (!node) return NULL;

    ramfs_node_priv_t *priv = kmalloc(sizeof(ramfs_node_priv_t));
    if (!priv) { kfree(node); return NULL; }

    priv->vol        = vol;
    priv->hdr_offset = hdr_offset;
    int tlen = strlen(tar_path);
    if (tlen >= MAX_PATH) tlen = MAX_PATH - 1;
    memcpy(priv->tar_path, tar_path, tlen);
    priv->tar_path[tlen] = '\0';

    node->priv = priv;
    node->ops  = &ramfs_ops;
    node->size = size;
    return node;
}

static ssize_t ramfs_vfs_read(vfs_node_t *node, void *buf, size_t size, off_t offset) {
    ramfs_node_priv_t *priv = (ramfs_node_priv_t *)node->priv;
    if (!priv || priv->hdr_offset < 0) return -1;

    ramfs_vol_t *vol = priv->vol;
    uint8_t *hdr     = vol->buf + priv->hdr_offset;
    uint64_t file_sz = tar_octal(hdr + TAR_SIZE_OFF, TAR_SIZE_LEN);
    uint8_t *data    = hdr + TAR_BLOCK;

    if ((uint64_t)offset >= file_sz) return 0;
    uint64_t avail = file_sz - (uint64_t)offset;
    if (size > avail) size = avail;
    memcpy(buf, data + offset, size);
    return (ssize_t)size;
}

static ssize_t ramfs_vfs_write(vfs_node_t *node, const void *buf, size_t size, off_t offset) {
    ramfs_node_priv_t *priv = (ramfs_node_priv_t *)node->priv;
    if (!priv || priv->hdr_offset < 0) return -1;

    ramfs_vol_t *vol = priv->vol;
    uint8_t *hdr     = vol->buf + priv->hdr_offset;
    uint64_t old_sz  = tar_octal(hdr + TAR_SIZE_OFF, TAR_SIZE_LEN);

    uint64_t new_sz = ((uint64_t)offset + size > old_sz)
                      ? (uint64_t)offset + size
                      : old_sz;

    uint8_t *tmp = kmalloc(new_sz);
    if (!tmp) return -1;
    memset(tmp, 0, new_sz);
    if (old_sz > 0) memcpy(tmp, hdr + TAR_BLOCK, old_sz);
    if (buf && size > 0) memcpy(tmp + offset, buf, size);

    if (tar_delete_entry(vol, priv->hdr_offset) != 0) { kfree(tmp); return -1; }

    int64_t new_hdr_off = tar_append_entry(vol, priv->tar_path,
                                            TAR_TYPE_FILE, tmp, new_sz);
    kfree(tmp);
    if (new_hdr_off < 0) return -1;

    priv->hdr_offset = new_hdr_off;
    node->size       = new_sz;
    return (ssize_t)size;
}

static int ramfs_vfs_readdir(vfs_node_t *node, uint32_t index, vfs_dirent_t *out) {
    ramfs_node_priv_t *dpriv = (ramfs_node_priv_t *)node->priv;
    if (!dpriv) return -1;

    ramfs_vol_t *vol    = dpriv->vol;
    const char  *prefix = dpriv->tar_path;
    int          plen   = strlen(prefix);
    int          want_depth = (plen == 0) ? 1 : path_depth(prefix) + 1;

    uint8_t *ptr = vol->buf;
    uint8_t *end = vol->buf + vol->size;
    uint32_t found = 0;
    char cur_long[MAX_PATH];
    cur_long[0] = '\0';

    while (ptr + TAR_BLOCK <= end) {
        uint8_t type = ptr[TAR_TYPE_OFF];
        unsigned long size = tar_octal(ptr + TAR_SIZE_OFF, TAR_SIZE_LEN);

        int allzero = 1;
        for (int i = 0; i < TAR_BLOCK; i++) {
            if (ptr[i]) { allzero = 0; break; }
        }
        if (allzero) break;

        if (type == TAR_TYPE_GNU_LONG || type == TAR_TYPE_GNU_LONGLNK) {
            int namelen = (int)size;
            if (namelen >= MAX_PATH) namelen = MAX_PATH - 1;
            memcpy(cur_long, ptr + TAR_BLOCK, namelen);
            cur_long[namelen] = '\0';
            ptr += (((size + TAR_BLOCK - 1) / TAR_BLOCK) + 1) * TAR_BLOCK;
            continue;
        }

        char fullname[MAX_PATH];
        if (cur_long[0]) {
            memcpy(fullname, cur_long, MAX_PATH);
            cur_long[0] = '\0';
        } else {
            if (!tar_fullname(ptr, fullname, MAX_PATH)) break;
        }
        strip_trailing_slash(fullname);

        int match = 0;
        if (plen == 0) {
            match = (fullname[0] != '\0');
        } else {
            match = (strncmp(fullname, prefix, plen) == 0 &&
                     fullname[plen] == '/');
        }

        if (match && path_depth(fullname) == want_depth) {
            if (found == index) {
                const char *last = fullname;
                for (const char *p = fullname; *p; p++) {
                    if (*p == '/') last = p + 1;
                }
                int nlen = strlen(last);
                if (nlen >= MAX_NAME) nlen = MAX_NAME - 1;
                memcpy(out->d_name, last, nlen);
                out->d_name[nlen] = '\0';
                out->d_type = (type == TAR_TYPE_DIR) ? VFS_NODE_DIR : VFS_NODE_FILE;
                out->d_ino  = 0;
                return 0;
            }
            found++;
        }

        ptr += (((size + TAR_BLOCK - 1) / TAR_BLOCK) + 1) * TAR_BLOCK;
    }
    return -1;
}

static vfs_node_t *ramfs_vfs_lookup(vfs_node_t *dir, const char *name) {
    vfs_node_t *existing = vfs_node_find_child_pub(dir, name);
    if (existing) return existing;

    ramfs_node_priv_t *dpriv = (ramfs_node_priv_t *)dir->priv;
    if (!dpriv) return NULL;

    ramfs_vol_t *vol = dpriv->vol;

    char child_tar[MAX_PATH];
    int plen = strlen(dpriv->tar_path);
    int nlen = strlen(name);
    if (plen == 0) {
        if (nlen >= MAX_PATH) return NULL;
        memcpy(child_tar, name, nlen + 1);
    } else {
        if (plen + 1 + nlen >= MAX_PATH) return NULL;
        memcpy(child_tar, dpriv->tar_path, plen);
        child_tar[plen] = '/';
        memcpy(child_tar + plen + 1, name, nlen + 1);
    }

    uint8_t *ptr = vol->buf;
    uint8_t *end = vol->buf + vol->size;
    char cur_long[MAX_PATH];
    cur_long[0] = '\0';

    while (ptr + TAR_BLOCK <= end) {
        uint8_t type = ptr[TAR_TYPE_OFF];
        unsigned long size = tar_octal(ptr + TAR_SIZE_OFF, TAR_SIZE_LEN);

        int allzero = 1;
        for (int i = 0; i < TAR_BLOCK; i++) {
            if (ptr[i]) { allzero = 0; break; }
        }
        if (allzero) break;

        if (type == TAR_TYPE_GNU_LONG || type == TAR_TYPE_GNU_LONGLNK) {
            int namelen = (int)size;
            if (namelen >= MAX_PATH) namelen = MAX_PATH - 1;
            memcpy(cur_long, ptr + TAR_BLOCK, namelen);
            cur_long[namelen] = '\0';
            ptr += (((size + TAR_BLOCK - 1) / TAR_BLOCK) + 1) * TAR_BLOCK;
            continue;
        }

        char fullname[MAX_PATH];
        if (cur_long[0]) {
            memcpy(fullname, cur_long, MAX_PATH);
            cur_long[0] = '\0';
        } else {
            if (!tar_fullname(ptr, fullname, MAX_PATH)) break;
        }
        strip_trailing_slash(fullname);

        if (strcmp(fullname, child_tar) == 0) {
            uint32_t vfs_type = (type == TAR_TYPE_DIR) ? VFS_NODE_DIR : VFS_NODE_FILE;
            uint64_t file_sz  = (type == TAR_TYPE_DIR) ? 0
                                : tar_octal(ptr + TAR_SIZE_OFF, TAR_SIZE_LEN);
            int64_t  hdr_off  = (type == TAR_TYPE_DIR) ? -1
                                : (int64_t)(ptr - vol->buf);
            vfs_node_t *child = ramfs_node_alloc(name, vfs_type, vol,
                                                  hdr_off, child_tar, file_sz);
            if (!child) return NULL;
            vfs_node_link_child_pub(dir, child);
            return child;
        }

        ptr += (((size + TAR_BLOCK - 1) / TAR_BLOCK) + 1) * TAR_BLOCK;
    }
    return NULL;
}

static int ramfs_vfs_create(vfs_node_t *dir, const char *name, uint32_t type, uint32_t mode) {
    (void)mode;
    ramfs_node_priv_t *dpriv = (ramfs_node_priv_t *)dir->priv;
    if (!dpriv) return -1;

    ramfs_vol_t *vol = dpriv->vol;

    char child_tar[MAX_PATH];
    int plen = strlen(dpriv->tar_path);
    int nlen = strlen(name);
    if (plen == 0) {
        if (nlen >= MAX_PATH) return -1;
        memcpy(child_tar, name, nlen + 1);
    } else {
        if (plen + 1 + nlen >= MAX_PATH) return -1;
        memcpy(child_tar, dpriv->tar_path, plen);
        child_tar[plen] = '/';
        memcpy(child_tar + plen + 1, name, nlen + 1);
    }

    if (tar_find(vol, child_tar) >= 0) return -1;

    uint8_t tar_type = (type == VFS_NODE_DIR) ? TAR_TYPE_DIR : TAR_TYPE_FILE;
    int64_t hdr_off  = tar_append_entry(vol, child_tar, tar_type, NULL, 0);
    if (hdr_off < 0) return -1;

    int64_t    node_hdr = (type == VFS_NODE_DIR) ? -1 : hdr_off;
    vfs_node_t *child   = ramfs_node_alloc(name, type, vol, node_hdr, child_tar, 0);
    if (!child) return -1;
    vfs_node_link_child_pub(dir, child);
    return 0;
}

static int ramfs_vfs_unlink(vfs_node_t *dir, const char *name) {
    ramfs_node_priv_t *dpriv = (ramfs_node_priv_t *)dir->priv;
    if (!dpriv) return -1;

    vfs_node_t *child = vfs_node_find_child_pub(dir, name);

    char child_tar[MAX_PATH];
    int plen = strlen(dpriv->tar_path);
    int nlen = strlen(name);
    if (plen == 0) {
        if (nlen >= MAX_PATH) return -1;
        memcpy(child_tar, name, nlen + 1);
    } else {
        if (plen + 1 + nlen >= MAX_PATH) return -1;
        memcpy(child_tar, dpriv->tar_path, plen);
        child_tar[plen] = '/';
        memcpy(child_tar + plen + 1, name, nlen + 1);
    }

    int64_t hdr_off = tar_find(dpriv->vol, child_tar);
    if (hdr_off < 0) return -1;
    if (tar_delete_entry(dpriv->vol, hdr_off) != 0) return -1;

    if (child) {
        vfs_node_unlink_child_pub(dir, child);
        if (child->ref_count == 0) {
            kfree(child->priv);
            kfree(child);
        }
    }
    return 0;
}

static int ramfs_vfs_rmdir(vfs_node_t *dir, const char *name) {
    vfs_node_t *child = vfs_node_find_child_pub(dir, name);
    if (child && child->children) return -1;
    return ramfs_vfs_unlink(dir, name);
}

void ramfs_init(void) {
    struct limine_module_response *response = module_request.response;
    if (!response || response->module_count == 0) {
        log_error("No modules found for ramfs\n");
        return;
    }
    ramfs_addr = response->modules[0]->address;
    ramfs_size = response->modules[0]->size;
    log_info("Ramfs module found at %llX with size %lu\n",
             (void *)ramfs_addr, ramfs_size);
}

ramfs_file_t *ramfs_read(char *archive, char *filename) {
    char *data;
    unsigned long size = tar_lookup((unsigned char *)archive, filename, &data);
    if (size == 0) {
        log_error("File not found in ramfs: %s\n", filename);
        return NULL;
    }

    ramfs_file_t *file = kmalloc(sizeof(ramfs_file_t));
    if (!file) { log_error("Failed to allocate ramfs_file_t\n"); return NULL; }

    int name_len = strlen(filename) + 1;
    file->name = kmalloc(name_len);
    if (!file->name) {
        log_error("Failed to allocate filename\n");
        kfree(file);
        return NULL;
    }
    memcpy(file->name, filename, name_len);

    file->data = kmalloc((int)size);
    if (!file->data) {
        log_error("Failed to allocate file data (%lu bytes)\n", size);
        kfree(file->name);
        kfree(file);
        return NULL;
    }

    memcpy(file->data, data, (int)size);
    file->size = (int)size;
    log_info("Loaded file %s (%lu bytes)\n", filename, size);
    return file;
}

int ramfs_list(char *archive, char *directory, char **filenames, int max_files) {
    unsigned char *ptr = (unsigned char *)archive;
    int count   = 0;
    int dir_len = strlen(directory);

    for (;;) {
        int allzero = 1;
        for (int i = 0; i < 512; i++) {
            if (ptr[i]) { allzero = 0; break; }
        }
        if (allzero) break;

        unsigned char type = ptr[156];
        unsigned long size = tar_octal(ptr + 124, 12);

        char fullname[512];
        fullname[0] = '\0';

        if (type == 'L' || type == 'K') {
            unsigned char *nameptr = ptr + 512;
            int namelen = (int)size;
            if (namelen >= (int)sizeof(fullname)) namelen = sizeof(fullname) - 1;
            memcpy(fullname, nameptr, namelen);
            fullname[namelen] = '\0';
            ptr += (((size + 511) / 512) + 1) * 512;
            type = ptr[156];
            size = tar_octal(ptr + 124, 12);
        }

        if (fullname[0] == '\0') {
            char name[101];
            char prefix[156];
            copy_field(name, ptr + 0, 100, sizeof(name));
            copy_field(prefix, ptr + 345, 155, sizeof(prefix));

            if (prefix[0]) {
                int plen = strlen(prefix);
                int nlen = strlen(name);
                if (plen + 1 + nlen >= (int)sizeof(fullname)) {
                    memcpy(fullname, prefix, sizeof(fullname) - 1);
                    fullname[sizeof(fullname) - 1] = '\0';
                } else {
                    memcpy(fullname, prefix, plen);
                    fullname[plen] = '/';
                    memcpy(fullname + plen + 1, name, nlen);
                    fullname[plen + 1 + nlen] = '\0';
                }
            } else {
                copy_field(fullname, ptr + 0, 100, sizeof(fullname));
            }
        }

        if (fullname[0]) {
            if (dir_len == 0 || !memcmp(fullname, directory, dir_len)) {
                if (type == '0' || type == '\0' || type == '7') {
                    if (count < max_files) {
                        int name_len = strlen(fullname) + 1;
                        filenames[count] = kmalloc(name_len);
                        if (filenames[count]) {
                            memcpy(filenames[count], fullname, name_len);
                            count++;
                        }
                    }
                }
            }
        }

        ptr += (((size + 511) / 512) + 1) * 512;
    }
    return count;
}

uint8_t ramfs_mount(void *archive, uint64_t archive_size, uint64_t capacity) {
    if (g_vol) return VFS_ERR_ALREADY_MOUNTED;

    vfs_node_t *vfs_root = vfs_get_root();
    if (!vfs_root) return VFS_ERR_NO_SLOTS;

    ramfs_vol_t *vol = kmalloc(sizeof(ramfs_vol_t));
    if (!vol) return VFS_ERR_NO_SLOTS;
    vol->buf      = (uint8_t *)archive;
    vol->size     = archive_size;
    vol->capacity = capacity;

    vfs_node_t *mp = vfs_node_alloc_pub("ram", VFS_NODE_MOUNTPOINT);
    if (!mp) { kfree(vol); return VFS_ERR_NO_SLOTS; }

    vfs_node_t *root_dir = ramfs_node_alloc("/", VFS_NODE_DIR, vol, -1, "", 0);
    if (!root_dir) {
        kfree(mp);
        kfree(vol);
        return VFS_ERR_NO_SLOTS;
    }

    mp->priv = root_dir;
    vfs_node_link_child_pub(vfs_root, mp);

    g_vol  = vol;
    g_root = mp;

    log_info("ramfs: mounted %llu bytes at /ram\n", archive_size);
    return VFS_OK;
}

void ramfs_unmount(void) {
    if (!g_root) return;

    vfs_node_t *vfs_root = vfs_get_root();
    if (vfs_root) vfs_node_unlink_child_pub(vfs_root, g_root);

    vfs_node_t *inner = (vfs_node_t *)g_root->priv;
    if (inner) {
        kfree(inner->priv);
        kfree(inner);
    }
    kfree(g_root);
    kfree(g_vol);

    g_vol  = NULL;
    g_root = NULL;

    log_info("ramfs: unmounted /ram\n");
}