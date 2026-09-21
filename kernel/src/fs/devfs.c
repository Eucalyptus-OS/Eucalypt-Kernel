#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <mm/memory.h>
#include <mm/heap.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <logging/print.h>
#include <tty.h>
#include <storage/drive_map.h>
#include <storage/disk_writer.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <input/keyboard.h>

#define DEV_BLOCK_SIZE 512

#define DEVFS_MAX_DEVS  64
#define MAX_NAME        256

extern volatile struct limine_framebuffer_request framebuffer_request;

static devfs_dev_t  devfs_devs[DEVFS_MAX_DEVS];
static devfs_block_t g_block_devs[DEVFS_MAX_DEVS];
static uint8_t      devfs_dev_count = 0;
static uint8_t      devfs_ready     = 0;
static vfs_node_t  *devfs_root      = NULL;

static vfs_node_t *devfs_lookup(vfs_node_t *dir, const char *name);
static int         devfs_readdir(vfs_node_t *dir, uint32_t index, vfs_dirent_t *out);

static vfs_node_ops_t devfs_dir_ops = {
    .read    = NULL,
    .write   = NULL,
    .readdir = devfs_readdir,
    .lookup  = devfs_lookup,
    .create  = NULL,
    .unlink  = NULL,
    .rmdir   = NULL,
};

static vfs_node_t *devfs_lookup(vfs_node_t *dir, const char *name);

// Sector-backed read: bounce each partially-aligned range through a scratch frame
static ssize_t blockdev_read(devfs_dev_t *dev, void *buf, size_t size, off_t offset) {
    devfs_block_t *blk = (devfs_block_t *)dev->priv;
    if (!blk || offset < 0) return -1;
    uint64_t dev_size = (uint64_t)blk->sector_count * DEV_BLOCK_SIZE;
    if ((uint64_t)offset >= dev_size) return 0;
    if ((uint64_t)offset + size > dev_size) size = (size_t)(dev_size - (uint64_t)offset);

    uint64_t frame = frame_alloc();
    if (!frame) return -1;
    uint8_t *tmp = (uint8_t *)phys_to_virt(frame);

    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;
    while (done < size) {
        uint64_t lba = ((uint64_t)offset + done) / DEV_BLOCK_SIZE;
        uint32_t in_sector = ((uint64_t)offset + done) % DEV_BLOCK_SIZE;
        uint32_t take = DEV_BLOCK_SIZE - in_sector;
        if (take > size - done) take = (uint32_t)(size - done);

        // One sector at a time through a kernel frame so unaligned requests work
        if (disk_reader(blk->drive_number, lba, 1, tmp) != 0) {
            frame_free(frame);
            return done ? (ssize_t)done : -1;
        }
        memcpy(dst + done, tmp + in_sector, take);
        done += take;
    }
    frame_free(frame);
    return (ssize_t)done;
}

// Sector-backed write: read-modify-write any sector touched only partially
static ssize_t blockdev_write(devfs_dev_t *dev, const void *buf, size_t size, off_t offset) {
    devfs_block_t *blk = (devfs_block_t *)dev->priv;
    if (!blk || offset < 0) return -1;
    uint64_t dev_size = (uint64_t)blk->sector_count * DEV_BLOCK_SIZE;
    if ((uint64_t)offset >= dev_size) return 0;
    if ((uint64_t)offset + size > dev_size) size = (size_t)(dev_size - (uint64_t)offset);

    uint64_t frame = frame_alloc();
    if (!frame) return -1;
    uint8_t *tmp = (uint8_t *)phys_to_virt(frame);

    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    while (done < size) {
        uint64_t lba = ((uint64_t)offset + done) / DEV_BLOCK_SIZE;
        uint32_t in_sector = ((uint64_t)offset + done) % DEV_BLOCK_SIZE;
        uint32_t take = DEV_BLOCK_SIZE - in_sector;
        if (take > size - done) take = (uint32_t)(size - done);

        if (in_sector || take != DEV_BLOCK_SIZE) {
            // Partial sector: fetch the existing contents first, then merge the update in
            if (disk_reader(blk->drive_number, lba, 1, tmp) != 0) {
                frame_free(frame);
                return done ? (ssize_t)done : -1;
            }
        }
        memcpy(tmp + in_sector, src + done, take);
        if (disk_writer(blk->drive_number, lba, 1, tmp) != 0) {
            frame_free(frame);
            return done ? (ssize_t)done : -1;
        }
        done += take;
    }
    frame_free(frame);
    return (ssize_t)done;
}

// VFS read dispatch: block devices take offsets, char devices read whole-buffer
static ssize_t dev_node_read(vfs_node_t *node, void *buf, size_t size, off_t offset) {
    devfs_dev_t *dev = (devfs_dev_t *)node->priv;
    if (!dev) return -1;
    if (dev->is_block) return blockdev_read(dev, buf, size, offset);
    if (!dev->read) return -1;
    return dev->read(dev, buf, size);
}

// VFS write dispatch: block devices take offsets, char devices write whole-buffer
static ssize_t dev_node_write(vfs_node_t *node, const void *buf, size_t size, off_t offset) {
    devfs_dev_t *dev = (devfs_dev_t *)node->priv;
    if (!dev) return -1;
    if (dev->is_block) return blockdev_write(dev, buf, size, offset);
    if (!dev->write) return -1;
    return dev->write(dev, buf, size);
}

static vfs_node_ops_t dev_node_ops = {
    .read    = dev_node_read,
    .write   = dev_node_write,
    .readdir = NULL,
    .lookup  = NULL,
    .create  = NULL,
    .unlink  = NULL,
    .rmdir   = NULL,
};

// Lazily create and cache a dev node the first time /dev/<name> is looked up
static vfs_node_t *devfs_lookup(vfs_node_t *dir, const char *name) {
    (void)dir;
    for (uint8_t i = 0; i < devfs_dev_count; i++) {
        if (strcmp(devfs_devs[i].name, name) == 0) {
            vfs_node_t *existing = vfs_node_find_child_pub(devfs_root, name);
            if (existing) return existing;

            vfs_node_t *node = vfs_node_alloc_pub(name, VFS_NODE_DEV);
            if (!node) return NULL;
            node->ops  = &dev_node_ops;
            node->priv = &devfs_devs[i];
            vfs_node_link_child_pub(devfs_root, node);
            return node;
        }
    }
    return NULL;
}

// Fill a dirent for the index-th registered device
static int devfs_readdir(vfs_node_t *dir, uint32_t index, vfs_dirent_t *out) {
    (void)dir;
    if (index >= devfs_dev_count) return -1;
    size_t nlen = strlen(devfs_devs[index].name);
    if (nlen >= MAX_NAME) nlen = MAX_NAME - 1;
    memcpy(out->d_name, devfs_devs[index].name, nlen);
    out->d_name[nlen] = '\0';
    out->d_type = VFS_NODE_DEV;
    out->d_ino  = 0;
    return 0;
}

// /dev/null: reads give zero bytes, writes are discarded
static ssize_t null_read(devfs_dev_t *dev, void *buf, size_t count) {
    (void)dev; (void)buf; (void)count;
    return 0;
}

static ssize_t null_write(devfs_dev_t *dev, const void *buf, size_t count) {
    (void)dev; (void)buf;
    return (ssize_t)count;   // accept and drop everything
}

// /dev/zero: reads return zeros, writes are discarded
static ssize_t zero_read(devfs_dev_t *dev, void *buf, size_t count) {
    (void)dev;
    memset(buf, 0, count);
    return (ssize_t)count;
}

static ssize_t zero_write(devfs_dev_t *dev, const void *buf, size_t count) {
    (void)dev; (void)buf;
    return (ssize_t)count;   // accept and drop everything
}

// /dev/tty: plumb reads/writes to the active terminal
static ssize_t console_read(devfs_dev_t *dev, void *buf, size_t count) {
    (void)dev;
    return (ssize_t)tty_read(tty_get_active(), (uint8_t *)buf, (uint32_t)count);
}

static ssize_t console_write(devfs_dev_t *dev, const void *buf, size_t count) {
    (void)dev;
    return tty_write(tty_get_active(), (const uint8_t *)buf, count);
}

// /dev/stdin: reads from the console, writes are rejected
static ssize_t stdin_read(devfs_dev_t *dev, void *buf, size_t count) {
    (void)dev;
    return console_read(dev, buf, count);
}

static ssize_t stdin_write(devfs_dev_t *dev, const void *buf, size_t count) {
    (void)dev; (void)buf; (void)count;
    return -1;
}

// /dev/stdout: writes go to the console, reads are rejected
static ssize_t stdout_read(devfs_dev_t *dev, void *buf, size_t count) {
    (void)dev; (void)buf; (void)count;
    return -1;
}

static ssize_t stdout_write(devfs_dev_t *dev, const void *buf, size_t count) {
    (void)dev;
    return console_write(dev, buf, count);
}

static ssize_t stderr_read(devfs_dev_t *dev, void *buf, size_t count) {
    (void)dev; (void)buf; (void)count;
    return -1;
}

static ssize_t stderr_write(devfs_dev_t *dev, const void *buf, size_t count) {
    (void)dev;
    return console_write(dev, buf, count);
}

static fb_info_t g_fb;

// /dev/fb0: expose the framebuffer as a bulk-accessible memory range
static ssize_t fb_read(devfs_dev_t *dev, void *buf, size_t count) {
    (void)dev;
    if (count > g_fb.size) count = g_fb.size;
    memcpy(buf, g_fb.addr, count);
    return (ssize_t)count;
}

static ssize_t fb_write(devfs_dev_t *dev, const void *buf, size_t count) {
    (void)dev;
    if (count > g_fb.size) count = g_fb.size;
    memcpy(g_fb.addr, buf, count);
    return (ssize_t)count;
}

struct fb_info_user {
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint32_t bpp;
    uint32_t pad;
};

// fb0 ioctl: return framebuffer geometry to userspace under request 0x4600
static int fb_ioctl(devfs_dev_t *dev, unsigned long req, void *arg) {
    (void)dev;
    if (req == 0x4600) {
        fb_info_t *fb = (fb_info_t *)dev->priv;
        struct fb_info_user info;
        info.width = fb->width;
        info.height = fb->height;
        info.pitch = fb->pitch;
        info.bpp = fb->bpp;
        info.pad = 0;
        memcpy(arg, &info, sizeof(info));
        return 0;
    }
    return -1;
}

// Add a character device to the devfs table and publish a /dev node for it
int devfs_register(const char *name, ssize_t (*read)(devfs_dev_t *, void *, size_t),
                   ssize_t (*write)(devfs_dev_t *, const void *, size_t), void *priv) {
    if (devfs_dev_count >= DEVFS_MAX_DEVS) return -1;

    devfs_dev_t *dev = &devfs_devs[devfs_dev_count];
    size_t nlen = strlen(name);
    if (nlen >= MAX_NAME) nlen = MAX_NAME - 1;
    memcpy(dev->name, name, nlen);
    dev->name[nlen] = '\0';
    dev->read  = read;
    dev->write = write;
    dev->ioctl = NULL;
    dev->priv  = priv;
    dev->is_block = 0;

    vfs_node_t *node = vfs_node_alloc_pub(name, VFS_NODE_DEV);
    if (node) {
        node->ops  = &dev_node_ops;
        node->priv = dev;
        vfs_node_link_child_pub(devfs_root, node);
    }

    devfs_dev_count++;
    return 0;
}

// Register a raw-disk device whose node size equals sector_count * 512
int devfs_register_block(const char *name, devfs_block_t *blk) {
    if (devfs_register(name, NULL, NULL, blk) != 0) return -1;

    devfs_dev_t *dev = devfs_get(name);
    if (!dev) return -1;
    dev->is_block = 1;

    vfs_node_t *node = vfs_node_find_child_pub(devfs_root, name);
    if (node) node->size = (size_t)blk->sector_count * DEV_BLOCK_SIZE;
    return 0;
}

// Map a /dev/<name> (or bare <name>) path to its backing AHCI drive number
int devfs_resolve_drive(const char *path, uint8_t *drive_number) {
    if (!path || !drive_number) return -1;

    const char *name = path;
    if (strncmp(name, "/dev/", 5) == 0) name += 5;

    devfs_dev_t *dev = devfs_get(name);
    if (!dev || !dev->is_block || !dev->priv) return -1;

    devfs_block_t *blk = (devfs_block_t *)dev->priv;
    *drive_number = blk->drive_number;
    return 0;
}

// Remove a device: drop its node and shift the table down over the hole
int devfs_unregister(const char *name) {
    for (uint8_t i = 0; i < devfs_dev_count; i++) {
        if (strcmp(devfs_devs[i].name, name) == 0) {
            vfs_node_t *node = vfs_node_find_child_pub(devfs_root, name);
            if (node) {
                vfs_node_unlink_child_pub(devfs_root, node);
                kfree(node);
            }
            for (uint8_t j = i; j < devfs_dev_count - 1; j++)
                devfs_devs[j] = devfs_devs[j + 1];
            devfs_dev_count--;
            return 0;
        }
    }
    return -1;
}

// Linear scan for a registered device by name
devfs_dev_t *devfs_get(const char *name) {
    for (uint8_t i = 0; i < devfs_dev_count; i++) {
        if (strcmp(devfs_devs[i].name, name) == 0)
            return &devfs_devs[i];
    }
    return NULL;
}

// Create /dev, register built-in char devices, the framebuffer, and one sdX per disk
void devfs_init() {
    if (devfs_ready) return;

    devfs_root = vfs_register_node("/dev", VFS_NODE_DIR, &devfs_dir_ops, NULL);
    if (!devfs_root) {
        print("devfs: failed to create /dev\n");
        return;
    }

    devfs_dev_count = 0;
    devfs_ready     = 1;

    devfs_register("null",    null_read,    null_write,    NULL);
    devfs_register("zero",    zero_read,    zero_write,    NULL);
    devfs_register("tty",     console_read, console_write, NULL);
    devfs_register("stdin",   stdin_read,   stdin_write,   NULL);
    devfs_register("stdout",  stdout_read,  stdout_write,  NULL);
    devfs_register("stderr",  stderr_read,  stderr_write,  NULL);

    devfs_get("tty")->ioctl = tty_ioctl;
    devfs_get("stdin")->ioctl = tty_ioctl;
    devfs_get("stdout")->ioctl = tty_ioctl;
    devfs_get("stderr")->ioctl = tty_ioctl;

    struct limine_framebuffer *lfb = framebuffer_request.response->framebuffers[0];
    g_fb.addr = (uint32_t *)lfb->address;
    g_fb.size = lfb->width * lfb->height * (lfb->bpp / 8);
    g_fb.phys = virt_to_phys((void *)lfb->address);
    g_fb.width = lfb->width;
    g_fb.height = lfb->height;
    g_fb.pitch = lfb->pitch;
    g_fb.bpp = lfb->bpp;
    devfs_register("fb0", fb_read, fb_write, &g_fb);
    devfs_get("fb0")->ioctl = fb_ioctl;

    uint8_t drive_count = drive_map_count();
    for (uint8_t i = 0; i < drive_count && i < DEVFS_MAX_DEVS; i++) {
        drive_t *d = drive_map_get(i);
        if (!d || d->sector_count == 0) continue;

        // Name each disk /dev/sd<letter>, one letter per drive index
        char name[8] = "sda";
        name[2] = (char)('a' + i);
        g_block_devs[i].drive_number = i;
        g_block_devs[i].sector_count = d->sector_count;
        devfs_register_block(name, &g_block_devs[i]);
    }

    print("devfs: initialized with %d built-in devices\n", devfs_dev_count);
}