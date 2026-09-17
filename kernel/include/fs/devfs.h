#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

#define MAX_NAME_LEN 256

// Character/block device registered in /dev
typedef struct devfs_dev {
    char     name[MAX_NAME_LEN];
    ssize_t (*read) (struct devfs_dev *dev, void *buf, size_t count);
    ssize_t (*write)(struct devfs_dev *dev, const void *buf, size_t count);
    int     (*ioctl)(struct devfs_dev *dev, unsigned long req, void *arg);
    void    *priv;          // driver-private data (e.g. devfs_block_t for block devs)
    uint8_t  is_block;      // nonzero => route through the sector-based blockdev path
} devfs_dev_t;

// Metadata for a raw block device exposed through /dev/sdX
typedef struct {
    uint8_t  drive_number;      // AHCI drive number backing this device
    uint64_t sector_count;      // device size in 512-byte sectors
} devfs_block_t;

// Framebuffer geometry for /dev/fb0 reads/writes and ioctl
typedef struct {
    uint32_t *addr;
    size_t    size;
    uintptr_t phys;
    uint64_t  width;
    uint64_t  height;
    uint64_t  pitch;
    uint32_t  bpp;
} fb_info_t;

void devfs_init();
int devfs_register(const char *name,
                   ssize_t (*read) (devfs_dev_t *, void *,       size_t),
                   ssize_t (*write)(devfs_dev_t *, const void *, size_t),
                   void *priv);
// Register a sector-backed device; sets is_block and node->size from sector_count
int devfs_register_block(const char *name, devfs_block_t *blk);
int devfs_unregister(const char *name);
devfs_dev_t *devfs_get(const char *name);
// Strip "/dev/" prefix if present and recover the drive number for a block device
int devfs_resolve_drive(const char *path, uint8_t *drive_number);
int tty_ioctl(devfs_dev_t *dev, unsigned long req, void *arg);