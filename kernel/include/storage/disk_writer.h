#pragma once

#include <stdint.h>

// Synchronously write `count` sectors at LBA `sector`; blocks until the AHCI I/O completes (0 = ok)
uint8_t disk_writer(uint8_t drive_number, uint64_t sector, uint8_t count, const void *data);
// Synchronously read `count` sectors at LBA `sector` into `data`; blocks until the AHCI I/O completes (0 = ok)
uint8_t disk_reader(uint8_t drive_number, uint64_t sector, uint8_t count, void *data);