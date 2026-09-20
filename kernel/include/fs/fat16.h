#pragma once

#include <stdint.h>
#include <fs/vfs.h>

// Client-visible description of a FAT16 file, filled by fat16_find_file
typedef struct {
    uint16_t start_cluster;   // first cluster of the file's data chain
    uint32_t size;            // file size in bytes
    uint8_t  attr;            // FAT attribute byte (see FAT16_ATTR_*)
    char     name[256];
} fat16_file_handle;

// One entry of a FAT16 directory listing, filled by fat16_list_directory
typedef struct {
    uint16_t start_cluster;
    uint8_t  attr;
    char     name[256];
} fat16_dir_entry;

// Probe and open a FAT16 volume on |dev|, returning an opaque vol pointer (fat_node)
void   *fat16_init(vfs_blockdev_t *dev);
// Write a fresh FAT16 BPB, FATs and root directory onto |dev| sized |total_sectors|
uint8_t fat16_format(vfs_blockdev_t *dev, uint32_t total_sectors);
uint8_t fat16_read(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, void *buffer);
// Copy |size| bytes of a file's cluster chain into |buffer|
uint8_t fat16_read_file(const void *vol_ptr, uint16_t start_cluster, uint32_t size, uint8_t *buffer, uint32_t *bytes_read);
// Allocate clusters and write |size| bytes, returning the new chain head
uint8_t fat16_write_file(const void *vol_ptr, uint16_t *start_cluster, const uint8_t *buffer, uint32_t size, uint32_t *bytes_written);
uint8_t fat16_create_file(const void *vol_ptr, uint16_t dir_cluster, const char *name, const uint8_t *buffer, uint32_t size);
uint8_t fat16_delete_file(const void *vol_ptr, uint16_t dir_cluster, const char *name);
uint8_t fat16_create_directory(const void *vol_ptr, uint16_t parent_cluster, const char *name);
uint8_t fat16_delete_directory(const void *vol_ptr, uint16_t parent_cluster, const char *name);
// Enumerate a directory, returning up to *count fat16_dir_entry records
uint8_t fat16_list_directory(const void *vol_ptr, uint16_t dir_cluster, fat16_dir_entry *entries, uint16_t *count);
uint8_t fat16_find_file(const void *vol_ptr, uint16_t dir_cluster, const char *name, fat16_file_handle *handle);
uint8_t fat16_create_dirent_update(const void *vol_ptr, uint16_t dir_cluster,
                                   const char *name, uint16_t start_cluster,
                                   uint32_t size);
void fat16_free_cluster_chain(const void *vol_ptr, uint16_t start_cluster);
uint32_t fat16_get_volume_id(const void *vol_ptr);