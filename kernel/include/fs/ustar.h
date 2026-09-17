#pragma once

#include <stdint.h>
#include <stddef.h>

uint8_t ustar_mount(const char *path, const void *image, size_t size);
