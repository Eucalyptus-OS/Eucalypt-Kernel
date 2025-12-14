#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>

#define MAX_NAME_LENGTH 129

typedef struct {
    char name[MAX_NAME_LENGTH];
    uint8_t *data;
    uint32_t size;
} file_t;

typedef struct {
    file_t *files;
    uint32_t file_count;
} file_system_t;

void init_ramfs(file_system_t *fs);
void list_files(file_system_t *fs, void *buf);
void write_file(const char *name, const uint8_t *data, uint32_t size, file_system_t *fs);
void read_file(const char *name, void *buf, uint32_t size, file_system_t *fs);

#endif