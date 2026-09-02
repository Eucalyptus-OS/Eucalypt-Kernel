#pragma once

#include <stdint.h>
#include <drivers/ahci.h>

#define DRIVE_MAP_MAX_DRIVES (AHCI_MAX_CONTROLLERS * AHCI_MAX_PORTS)

typedef struct {
    uint8_t controller;
    uint8_t port;
    uint8_t valid;
} drive_map_entry_t;

uint8_t drive_map_init();
uint8_t drive_map_count();
int drive_map_resolve(uint8_t drive_number, uint8_t *controller, uint8_t *port);
drive_t *drive_map_get(uint8_t drive_number);