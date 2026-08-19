#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include <init.h>
#include <mm/hhdm.h>
#include <mm/frame.h>
#include <gdt.h>
#include <paging.h>

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

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

static void fb_fill(struct limine_framebuffer *fb, uint32_t color) {
    volatile uint32_t *ptr = fb->address;
    size_t row_bytes = fb->pitch;
    for (size_t y = 0; y < 40 && y < fb->height; y++) {
        for (size_t x = 0; x < fb->width; x++) {
            ptr[y * (row_bytes / 4) + x] = color;
        }
    }
}

void kmain(void) {
    struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];

    register_function((init_func_t)(void *)gdt_init, 0, 0);
    register_function((init_func_t)(void *)hhdm_init, hhdm_request.response->offset, 0);
    register_function((init_func_t)(void *)frame_init, (uint64_t)memmap_request.response, 0);
    register_function((init_func_t)(void *)paging_init, (uint64_t)memmap_request.response, (uint64_t)exec_request.response);
    init();

    fb_fill(framebuffer, 0xFFFFFFFF);

    for (;;) {
        asm volatile ("hlt");
    }
}
