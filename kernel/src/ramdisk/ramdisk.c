#include <ramdisk/ramdisk.h>
#include <x86_64/serial.h>

#include <limine.h>

extern struct limine_module_request module_request;

void init_ramdisk() {
    struct limine_module_response *module_response = module_request.response;
    uint32_t ramdisk_size = module_response->modules[0]->size;

    serial_print("Ramdisk Size: ");
    serial_print_num(ramdisk_size);
    serial_print(" bytes\n");
}

void write_ramdisk(const uint8_t *data, uint32_t size) {
    struct limine_module_response *module_response = module_request.response;
    if (size > module_response->modules[0]->size) {
        serial_print("Error: Data size exceeds ramdisk size\n");
        return;
    }

    uint8_t *ramdisk_ptr = (uint8_t *)module_response->modules[0]->address;
    for (uint32_t i = 0; i < size; i++) {
        ramdisk_ptr[i] = data[i];
    }
}

void read_ramdisk(uint8_t *buffer, uint32_t size) {
    struct limine_module_response *module_response = module_request.response;
    if (size > module_response->modules[0]->size) {
        serial_print("Error: Read size exceeds ramdisk size\n");
        return;
    }

    uint8_t *ramdisk_ptr = (uint8_t *)module_response->modules[0]->address;
    for (uint32_t i = 0; i < size; i++) {
        buffer[i] = ramdisk_ptr[i];
    }
}