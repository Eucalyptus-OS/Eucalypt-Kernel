#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

#define MAX_NAME_LEN 256

typedef struct devfs_dev {
    char     name[MAX_NAME_LEN];
    ssize_t (*read) (struct devfs_dev *dev, void *buf, size_t count);
    ssize_t (*write)(struct devfs_dev *dev, const void *buf, size_t count);
    int     (*ioctl)(struct devfs_dev *dev, unsigned long req, void *arg);
    void    *priv;
    uint8_t  is_block;
} devfs_dev_t;

typedef struct {
    uint8_t  drive_number;
    uint64_t sector_count;
} devfs_block_t;

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
int devfs_register_block(const char *name, devfs_block_t *blk);
int devfs_unregister(const char *name);
devfs_dev_t *devfs_get(const char *name);
int devfs_resolve_drive(const char *path, uint8_t *drive_number);
int tty_ioctl(devfs_dev_t *dev, unsigned long req, void *arg);