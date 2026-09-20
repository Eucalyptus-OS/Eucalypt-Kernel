#include <stdint.h>
#include <mm/hhdm.h>

uint64_t hhdm_offset; // physical->virtual offset of the higher-half direct map

// Record the HHDM offset reported by the Limine HHDM request
void hhdm_init(uint64_t offset) {
    hhdm_offset = offset;
}

// Convert a physical address to its higher-half virtual alias
void *phys_to_virt(uintptr_t phys) {
    uint64_t offset = hhdm_offset;
    return (void *)(phys + offset);
}

// Convert a higher-half virtual address back to physical
uintptr_t virt_to_phys(void *virt) {
    uint64_t offset = hhdm_offset;
    return (uintptr_t)virt - offset;
}