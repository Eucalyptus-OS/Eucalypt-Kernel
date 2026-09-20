#pragma once

#include <stdint.h>
#include <storage/ahci.h>

// One drive number exists per (controller, port) pair: 8 controllers * 32 ports = 256 max
#define DRIVE_MAP_MAX_DRIVES (AHCI_MAX_CONTROLLERS * AHCI_MAX_PORTS)

// One mapping slot: the array index is the drive number, the entry is the physical device
typedef struct {
    uint8_t controller;  // AHCI controller index
    uint8_t port;        // port index on that controller
    uint8_t valid;       // 1 once this slot holds a discovered drive
} drive_map_entry_t;

// Enumerate all discovered drives and assign them sequential drive numbers
void drive_map_init();
// Return the number of drives currently mapped
uint8_t drive_map_count();
// Translate a drive number into its physical (controller, port); -1 if invalid
int drive_map_resolve(uint8_t drive_number, uint8_t *controller, uint8_t *port);
// Return geometry info for a drive number, or NULL when invalid
drive_t *drive_map_get(uint8_t drive_number);