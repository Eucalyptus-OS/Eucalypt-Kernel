#pragma once

#include <stdint.h>

enum StorageDevType {
    AHCI = 0,
    NVME = 1,
    RAMFS = 2,
};

typedef enum {
    VFS_OK                  = 0,
    VFS_ERR_LETTER_IN_USE   = 1,
    VFS_ERR_NO_SLOTS        = 2,
    VFS_ERR_INVALID_DEV     = 3,
    VFS_ERR_ALREADY_MOUNTED = 4,
    VFS_ERR_UNKNOWN_DEV     = 5,
    VFS_ERR_FS_INIT         = 6,
} vfs_err_t;

typedef struct vfs_blockdev {
    uint8_t (*read) (struct vfs_blockdev *dev, uint32_t lba, uint8_t count, void *buf);
    uint8_t (*write)(struct vfs_blockdev *dev, uint32_t lba, uint8_t count, const void *buf);
    void *priv;
} vfs_blockdev_t;

typedef struct {
    char           letter;
    vfs_blockdev_t blockdev;
    enum StorageDevType dev;
} vfs_mount_t;

uint8_t vfs_mount(enum StorageDevType dev, char letter, uint8_t controller, uint8_t port);
void vfs_unmount(char letter);
uint8_t vfs_init();
vfs_mount_t *vfs_get_mount(char letter);