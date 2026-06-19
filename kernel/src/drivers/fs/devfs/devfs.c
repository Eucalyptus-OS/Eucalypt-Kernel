#include <stdint.h>
#include <stddef.h>
#include <mem.h>
#include <mm/heap.h>
#include <logging/printk.h>
#include <drivers/tty.h>
#include <drivers/fs/vfs/vfs.h>
#include <drivers/fs/devfs/devfs.h>

#define DEVFS_MAX_DEVS  64
#define MAX_NAME        256

static devfs_dev_t  devfs_devs[DEVFS_MAX_DEVS];
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

static ssize_t dev_node_read(vfs_node_t *node, void *buf, size_t size, off_t offset) {
    devfs_dev_t *dev = (devfs_dev_t *)node->priv;
    if (!dev || !dev->read) return -1;
    (void)offset;
    return dev->read(dev, buf, size);
}

static ssize_t dev_node_write(vfs_node_t *node, const void *buf, size_t size, off_t offset) {
    devfs_dev_t *dev = (devfs_dev_t *)node->priv;
    if (!dev || !dev->write) return -1;
    (void)offset;
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

static ssize_t null_read(devfs_dev_t *dev, void *buf, size_t count) {
    (void)dev; (void)buf; (void)count;
    return 0;
}

static ssize_t null_write(devfs_dev_t *dev, const void *buf, size_t count) {
    (void)dev; (void)buf;
    return (ssize_t)count;
}

static ssize_t zero_read(devfs_dev_t *dev, void *buf, size_t count) {
    (void)dev;
    memset(buf, 0, count);
    return (ssize_t)count;
}

static ssize_t zero_write(devfs_dev_t *dev, const void *buf, size_t count) {
    (void)dev; (void)buf;
    return (ssize_t)count;
}

static ssize_t console_read(devfs_dev_t *dev, void *buf, size_t count) {
    (void)dev; (void)buf; (void)count;
    return 0;
}

static ssize_t console_write(devfs_dev_t *dev, const void *buf, size_t count) {
    (void)dev;
    return tty_write(tty_get_active(), (const uint8_t *)buf, count);
}

static ssize_t stdin_read(devfs_dev_t *dev, void *buf, size_t count) {
    (void)dev;
    return console_read(dev, buf, count);
}

static ssize_t stdin_write(devfs_dev_t *dev, const void *buf, size_t count) {
    (void)dev; (void)buf; (void)count;
    return -1;
}

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
    dev->priv  = priv;

    vfs_node_t *node = vfs_node_alloc_pub(name, VFS_NODE_DEV);
    if (node) {
        node->ops  = &dev_node_ops;
        node->priv = dev;
        vfs_node_link_child_pub(devfs_root, node);
    }

    devfs_dev_count++;
    return 0;
}

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

devfs_dev_t *devfs_get(const char *name) {
    for (uint8_t i = 0; i < devfs_dev_count; i++) {
        if (strcmp(devfs_devs[i].name, name) == 0)
            return &devfs_devs[i];
    }
    return NULL;
}

void devfs_init(void) {
    if (devfs_ready) return;

    devfs_root = vfs_register_node("/dev", VFS_NODE_DIR, &devfs_dir_ops, NULL);
    if (!devfs_root) {
        log_error("devfs: failed to create /dev\n");
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

    log_info("devfs: initialized with %d built-in devices\n", devfs_dev_count);
}