#pragma once

#include <stdint.h>

enum ramfs_type {
    RAMFS_FILE = 0,
    RAMFS_HARD_LINK = 1,
    RAMFS_SYMBOLIC_LINK = 2,
    RAMFS_CHAR_DEV = 3,
    RAMFS_BLOCK_DEV = 4,
    RAMFS_DIRECTORY = 5,
    RAMFS_NAMED_PIPE = 6,
};

extern void *ramfs_addr;
extern uint64_t ramfs_size;

typedef struct {
    uint64_t offset;
    uint64_t size;  
    uint64_t type;
} ramfs_t;

typedef struct {
    char *name;
    void *data;
    uint64_t size;
} ramfs_file_t;

void ramfs_init();
ramfs_file_t *ramfs_read(char *archive, char *filename);
int ramfs_list(char *archive, char *directory, char **filenames, int max_files);