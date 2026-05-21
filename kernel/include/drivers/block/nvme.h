#pragma once

#include <stdint.h>

#define NVME_MAX_CONTROLLERS    8
#define NVME_MAX_NAMESPACES     32

typedef struct {
    uint8_t type;
    uint8_t present;
    char assigned_letter;
} nvme_namespace_info_t;

typedef struct {
    nvme_namespace_info_t namespaces[NVME_MAX_NAMESPACES];
} nvme_controller_t;

typedef struct {
    nvme_controller_t controllers[NVME_MAX_CONTROLLERS];
    uint8_t count;
} nvme_state_t;

uint8_t nvme_read(uint8_t controller, uint32_t nsid, uint32_t lba, uint8_t count, void *buf);
uint8_t nvme_write(uint8_t controller, uint32_t nsid, uint32_t lba, uint8_t count, const void *buf);
uint8_t nvme_get_controller_count();
nvme_controller_t *nvme_get_controller(uint8_t index);
uint32_t nvme_get_namespace_count(nvme_controller_t *controller);
uint8_t nvme_namespace_present(nvme_controller_t *controller, uint32_t nsid);
char nvme_namespace_letter(nvme_controller_t *controller, uint32_t nsid);
void nvme_set_namespace_letter(nvme_controller_t *controller, uint32_t nsid, char letter);
uint8_t nvme_init();
