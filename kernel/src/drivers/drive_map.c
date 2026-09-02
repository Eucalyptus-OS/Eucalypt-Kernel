#include <drivers/ahci.h>
#include <drivers/drive_map.h>
#include <logging/print.h>
#include <stdint.h>
#include <stddef.h>

static drive_map_entry_t g_drive_map[DRIVE_MAP_MAX_DRIVES];
static uint8_t g_drive_count = 0;

uint8_t drive_map_init() {
    g_drive_count = 0;

    uint8_t controller_count = ahci_get_controller_count();

    for (uint8_t c = 0; c < controller_count; c++) {
        uint8_t port_count = ahci_get_port_count(c);

        for (uint8_t n = 0; n < port_count; n++) {
            uint8_t port = ahci_get_port_index(c, n);
            if (port == 0xFF)
                continue;

            if (g_drive_count >= DRIVE_MAP_MAX_DRIVES - 1) {
                print("drive table full, dropping controller: %u port: %u\n", c, port);
                continue;
            }

            g_drive_map[g_drive_count].controller = c;
            g_drive_map[g_drive_count].port = port;
            g_drive_map[g_drive_count].valid = 1;

            print("drive %u -> controller: %u port: %u\n", g_drive_count, c, port);

            g_drive_count++;
        }
    }

    print("total drives: %u\n", g_drive_count);
    return 0;
}

uint8_t drive_map_count() {
    return g_drive_count;
}

int drive_map_resolve(uint8_t drive_number, uint8_t *controller, uint8_t *port) {
    if (drive_number >= g_drive_count || !g_drive_map[drive_number].valid)
        return -1;

    *controller = g_drive_map[drive_number].controller;
    *port = g_drive_map[drive_number].port;
    return 0;
}

drive_t *drive_map_get(uint8_t drive_number) {
    uint8_t controller, port;
    if (drive_map_resolve(drive_number, &controller, &port) != 0)
        return NULL;

    return ahci_get_drive(controller, port);
}