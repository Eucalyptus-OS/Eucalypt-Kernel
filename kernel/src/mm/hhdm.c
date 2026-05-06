#include <limine.h>
#include <mm/hhdm.h>

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

uint64_t offset = 0;

void hhdm_init() {
    offset = hhdm_request.response->offset;
}

uint64_t phys_virt(uint64_t phys) {
    return offset + phys;
}

uint64_t virt_phys(uint64_t virt) {
    return virt - offset;
}