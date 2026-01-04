#include <ramdisk/ramdisk.h>
#include <x86_64/serial.h>
#include <limine.h>

extern struct limine_module_request module_request;

#define SECTOR_SIZE 512

#ifdef DEBUG
#define DPRINT(s) serial_print(s)
#define DPRINTNUM(s) serial_print_num(s)
#else
#define DPRINT(s)
#define DPRINTNUM(s)
#endif

void init_ramdisk() {
    struct limine_module_response *module_response = module_request.response;
    uint32_t ramdisk_size = module_response->modules[0]->size;
    DPRINT("Ramdisk Size: ");
    DPRINTNUM(ramdisk_size);
    DPRINT(" bytes\n");
    DPRINT("Ramdisk Sectors: ");
    DPRINTNUM(ramdisk_size / SECTOR_SIZE);
    DPRINT("\n");
}

void write_ramdisk_sector(uint32_t sector, const uint8_t *data) {
    struct limine_module_response *module_response = module_request.response;
    uint32_t ramdisk_size = module_response->modules[0]->size;
    uint32_t offset = sector * SECTOR_SIZE;
    
    if (offset + SECTOR_SIZE > ramdisk_size) {
        DPRINT("Error: Sector ");
        DPRINTNUM(sector);
        DPRINT(" exceeds ramdisk size\n");
        return;
    }
    
    uint8_t *ramdisk_ptr = (uint8_t *)module_response->modules[0]->address;
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        ramdisk_ptr[offset + i] = data[i];
    }
}

void read_ramdisk_sector(uint32_t sector, uint8_t *buffer) {
    struct limine_module_response *module_response = module_request.response;
    uint32_t ramdisk_size = module_response->modules[0]->size;
    uint32_t offset = sector * SECTOR_SIZE;
    
    if (offset + SECTOR_SIZE > ramdisk_size) {
        DPRINT("Error: Sector ");
        DPRINTNUM(sector);
        DPRINT(" exceeds ramdisk size\n");
        return;
    }
    
    uint8_t *ramdisk_ptr = (uint8_t *)module_response->modules[0]->address;
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        buffer[i] = ramdisk_ptr[offset + i];
    }
}

void write_ramdisk_sectors(uint32_t start_sector, const uint8_t *data, uint32_t num_sectors) {
    for (uint32_t i = 0; i < num_sectors; i++) {
        write_ramdisk_sector(start_sector + i, data + (i * SECTOR_SIZE));
    }
}

void read_ramdisk_sectors(uint32_t start_sector, uint8_t *buffer, uint32_t num_sectors) {
    for (uint32_t i = 0; i < num_sectors; i++) {
        read_ramdisk_sector(start_sector + i, buffer + (i * SECTOR_SIZE));
    }
}