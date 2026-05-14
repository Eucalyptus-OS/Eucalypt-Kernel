#pragma once

#include <stdint.h>
#include <drivers/fs/vfs/vfs.h>

uint8_t fat16_init(vfs_blockdev_t *dev);
uint8_t fat16_format(vfs_blockdev_t *dev, uint32_t total_sectors);
uint8_t fat16_read(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, void *buffer);