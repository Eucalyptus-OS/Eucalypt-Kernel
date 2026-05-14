#include <stdint.h>
#include <drivers/block/ahci.h>

uint8_t get_total_drive_count() {
    uint8_t count = 0;

    count += ahci_get_controller_count();

    return count;
}