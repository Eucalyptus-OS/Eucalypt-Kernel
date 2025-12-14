#ifndef RAMDISK_H
#define RAMDISK_H

#include <stdint.h>

void init_ramdisk();
void write_ramdisk(const uint8_t *data, uint32_t size);
void read_ramdisk(uint8_t *buffer, uint32_t size);

#endif