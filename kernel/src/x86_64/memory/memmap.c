#include <x86_64/memory/memmap.h>
#include <stdint.h>
#include <limine.h>

extern volatile struct limine_hhdm_request hhdm_request;

void *phys_to_virt(uint64_t phys_addr) {
    return (void *)(phys_addr + hhdm_request.response->offset);
}