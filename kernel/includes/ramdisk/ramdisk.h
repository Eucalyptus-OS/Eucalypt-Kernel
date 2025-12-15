#ifndef RAMDISK_H
#define RAMDISK_H

#include <stdint.h>

void init_ramdisk();
void write_ramdisk_sector(uint32_t sector, const uint8_t *data);
void read_ramdisk_sector(uint32_t sector, uint8_t *buffer);
void write_ramdisk_sectors(uint32_t start_sector, const uint8_t *data, uint32_t num_sectors);
void read_ramdisk_sectors(uint32_t start_sector, uint8_t *buffer, uint32_t num_sectors);

#endif