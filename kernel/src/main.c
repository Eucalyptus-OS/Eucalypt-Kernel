#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include <init.h>
#include <mm/hhdm.h>
#include <mm/frame.h>
#include <mm/paging.h>
#include <mm/heap.h>
#include <mm/vmm.h>
#include <gdt.h>
#include <idt.h>
#include <acpi.h>
#include <apic.h>
#include <memory.h>
#include <logging/print.h>
#include <multitasking/thread.h>
#include <drivers/drive_map.h>
#include <drivers/ahci.h>

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request exec_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

void kmain(void) {
    register_function(gdt_init, 0, 0);
    register_function(hhdm_init, hhdm_request.response->offset, 0);
    register_function(frame_init, (uint64_t)memmap_request.response, 0);
    register_function(paging_init, (uint64_t)memmap_request.response, (uint64_t)exec_request.response);
    register_function(idt_init, 0, 0);
    register_function(acpi_init, (uint64_t)rsdp_request.response->address, 0);
    register_function(apic_init, 0, 0);
    register_function(heap_init, 0, 0);
    register_function(vmm_init, 0, 0);
    register_function(ahci_init, 0, 0);
    register_function(drive_map_init, 0, 0);
    init();

    asm volatile ("sti");

    for (;;) {
        asm volatile ("hlt");
    }
}
