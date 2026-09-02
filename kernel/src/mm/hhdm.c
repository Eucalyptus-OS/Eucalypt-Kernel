#include <stdint.h>
#include <mm/hhdm.h>

uint64_t hhdm_offset;

uint8_t hhdm_init(uint64_t offset) {
    hhdm_offset = offset;
    return 0;
}

void *phys_to_virt(uintptr_t phys) {
    uint64_t offset = hhdm_offset;
    return (void *)(phys + offset);
}

uintptr_t virt_to_phys(void *virt) {
    uint64_t offset = hhdm_offset;
    return (uintptr_t)virt - offset;
}