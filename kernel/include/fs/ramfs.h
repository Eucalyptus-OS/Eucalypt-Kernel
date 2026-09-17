#pragma once

#include <stdint.h>

// Mount an in-memory file tree at |path|; returns VFS_OK or an error code
uint8_t ramfs_mount(const char *path);
