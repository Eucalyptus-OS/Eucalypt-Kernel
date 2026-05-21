#pragma once

#include <stdint.h>
#include <drivers/fs/vfs/vfs.h>

typedef struct {
    uint16_t start_cluster;
    uint32_t size;
    uint8_t  attr;
    char     name[256];
} fat16_file_handle;

typedef struct {
    uint16_t start_cluster;
    uint8_t  attr;
    char     name[256];
} fat16_dir_entry;

void *fat16_init(vfs_blockdev_t *dev);
uint8_t fat16_format(vfs_blockdev_t *dev, uint32_t total_sectors);
uint8_t fat16_read(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, void *buffer);
uint8_t fat16_read_file(const void *vol_ptr, uint16_t start_cluster, uint32_t size, uint8_t *buffer, uint32_t *bytes_read);
uint8_t fat16_write_file(const void *vol_ptr, uint16_t *start_cluster, const uint8_t *buffer, uint32_t size, uint32_t *bytes_written);
uint8_t fat16_create_file(const void *vol_ptr, uint16_t dir_cluster, const char *name, const uint8_t *buffer, uint32_t size);
uint8_t fat16_delete_file(const void *vol_ptr, uint16_t dir_cluster, const char *name);
uint8_t fat16_create_directory(const void *vol_ptr, uint16_t parent_cluster, const char *name);
uint8_t fat16_delete_directory(const void *vol_ptr, uint16_t parent_cluster, const char *name);
uint8_t fat16_list_directory(const void *vol_ptr, uint16_t dir_cluster, fat16_dir_entry *entries, uint16_t *count);
uint8_t fat16_find_file(const void *vol_ptr, uint16_t dir_cluster, const char *name, fat16_file_handle *handle);
uint32_t fat16_get_volume_id(const void *vol_ptr);