#pragma once

#include <stdint.h>

uint8_t disk_writer(uint8_t drive_number, uint64_t sector, uint8_t count, const void *data);
uint8_t disk_reader(uint8_t drive_number, uint64_t sector, uint8_t count, void *data);