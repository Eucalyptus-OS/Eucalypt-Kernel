#include <stdint.h>
#include <drivers/block/nvme.h>
#include <mem.h>

static nvme_state_t nvme_state = {0};

uint8_t nvme_read(uint8_t controller, uint32_t nsid, uint32_t lba, uint8_t count, void *buf) {
    // Placeholder NVMe read function
    if (controller >= NVME_MAX_CONTROLLERS || nsid == 0 || nsid > NVME_MAX_NAMESPACES) return 1;
    if (!nvme_state.controllers[controller].namespaces[nsid - 1].present) return 1;
    
    // TODO: Implement actual NVMe read logic
    (void)lba;
    memset(buf, 0, count * 512);
    return 0;
}

uint8_t nvme_write(uint8_t controller, uint32_t nsid, uint32_t lba, uint8_t count, const void *buf) {
    // Placeholder NVMe write function
    if (controller >= NVME_MAX_CONTROLLERS || nsid == 0 || nsid > NVME_MAX_NAMESPACES) return 1;
    if (!nvme_state.controllers[controller].namespaces[nsid - 1].present) return 1;
    
    // TODO: Implement actual NVMe write logic
    (void)lba;
    (void)count;
    (void)buf;
    return 0;
}

uint8_t nvme_get_controller_count() {
    return nvme_state.count;
}

nvme_controller_t *nvme_get_controller(uint8_t index) {
    if (index >= nvme_state.count) return 0;
    return &nvme_state.controllers[index];
}

uint32_t nvme_get_namespace_count(nvme_controller_t *controller) {
    if (!controller) return 0;
    // Count the number of present namespaces
    uint32_t count = 0;
    for (int i = 0; i < NVME_MAX_NAMESPACES; i++) {
        if (controller->namespaces[i].present) count++;
    }
    return count;
}

uint8_t nvme_namespace_present(nvme_controller_t *controller, uint32_t nsid) {
    if (!controller || nsid == 0 || nsid > NVME_MAX_NAMESPACES) return 0;
    return controller->namespaces[nsid - 1].present;
}

char nvme_namespace_letter(nvme_controller_t *controller, uint32_t nsid) {
    if (!controller || nsid == 0 || nsid > NVME_MAX_NAMESPACES) return 0;
    return controller->namespaces[nsid - 1].assigned_letter;
}

void nvme_set_namespace_letter(nvme_controller_t *controller, uint32_t nsid, char letter) {
    if (!controller || nsid == 0 || nsid > NVME_MAX_NAMESPACES) return;
    controller->namespaces[nsid - 1].assigned_letter = letter;
}

uint8_t nvme_init() {
    // Placeholder NVMe initialization
    // TODO: Implement actual NVMe discovery and initialization via PCI
    nvme_state.count = 0;
    return 0;
}
