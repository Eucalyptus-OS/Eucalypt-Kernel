#pragma once

#include <stdint.h>
#include <stddef.h>

// Extract a ustar tar archive |image| (|size| bytes) into the VFS tree at |path|
uint8_t ustar_mount(const char *path, const void *image, size_t size);
