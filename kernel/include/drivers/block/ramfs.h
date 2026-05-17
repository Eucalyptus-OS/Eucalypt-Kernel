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

typedef struct {
    uint64_t offset;
    uint64_t size;  
    uint64_t type;
} ramfs_t;

typedef struct {
    char *name;
    void *data;
} ramfs_file_t;