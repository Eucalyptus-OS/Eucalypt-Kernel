#pragma once

#include <stddef.h>
#include <stdint.h>

#include <abi/shm.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

#define MAX_NAME_LEN 256

// How a device's priv is interpreted by the devfs branch of sys_mmap. The
// discriminator lives on the device rather than being sniffed from priv so that
// /dev/fb0 and /dev/shm can hand mmap completely different private structs.
#define DEVMAP_NONE 0   // device is not mappable
#define DEVMAP_FB   1   // priv is an fb_info_t; maps the whole device
#define DEVMAP_SHM  2   // priv is an shm_dev_t; offset selects one region

// Character/block device registered in /dev
typedef struct devfs_dev {
    char     name[MAX_NAME_LEN];
    ssize_t (*read) (struct devfs_dev *dev, void *buf, size_t count);
    // Optional non-blocking read, used instead of read when the descriptor was
    // opened with O_NONBLOCK. Must return -EAGAIN rather than sleeping when no
    // data is available. NULL for drivers whose read is always non-blocking.
    ssize_t (*read_nb) (struct devfs_dev *dev, void *buf, size_t count);
    ssize_t (*write)(struct devfs_dev *dev, const void *buf, size_t count);
    int     (*ioctl)(struct devfs_dev *dev, unsigned long req, void *arg);
    void    *priv;          // driver-private data (e.g. devfs_block_t for block devs)
    uint8_t  is_block;      // nonzero => route through the sector-based blockdev path
    uint8_t  mmap_kind;     // one of DEVMAP_*; selects the sys_mmap strategy
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

// --- Shared memory regions (/dev/shm) ---------------------------------------
// The userspace-visible types and ioctl numbers live in <abi/shm.h> so that
// kernel and mlibc share one definition. What follows is kernel-internal.

// A single region: the contiguous frame run plus its bookkeeping
typedef struct {
    uintptr_t phys;     // physical base of the contiguous frame run
    size_t    size;     // usable bytes (rounded up to a page multiple)
    uint32_t  pages;
    int       used;
    char      name[SHM_NAME_LEN];
} shm_region_t;

// priv for the /dev/shm device: the whole region table
typedef struct {
    shm_region_t regions[SHM_MAX_REGIONS];
} shm_dev_t;

// Register /dev/shm; call once during device init
int shm_devfs_init(void);
int shm_region_create(const char *name, size_t size, shm_region_t **out);
int shm_region_destroy(const char *name);
shm_region_t *shm_region_find(shm_dev_t *shm, const char *name);
// Resolve an mmap offset to its region, or NULL if out of range
shm_region_t *shm_region_by_offset(shm_dev_t *shm, uint64_t offset);

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