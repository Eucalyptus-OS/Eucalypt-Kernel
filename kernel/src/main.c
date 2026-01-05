#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include <flanterm/flanterm.h>
#include <flanterm/fb.h>

#include <x86_64/serial.h>
#include <x86_64/gdt.h>
#include <x86_64/interrupts/pic.h>
#include <x86_64/idt/idt.h>
#include <x86_64/interrupts/timer.h>
#include <x86_64/interrupts/keyboard.h>
#include <x86_64/allocator/heap.h>
#include <x86_64/memory/pmm.h>
#include <x86_64/memory/vmm.h>
#include <ramdisk/ramdisk.h>
#include <ramdisk/fat12.h>
#include <elf.h>
#include <shell.h>

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(4);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

static void hcf(void) {
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

struct flanterm_context *ft_ctx = NULL;

void kmain(void) {
    __asm__ volatile ("cli");
    
    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        hcf();
    }

    if (module_request.response == NULL || module_request.response->module_count < 1) {
        hcf();
    }
    
    if (hhdm_request.response == NULL) {
        hcf();
    }

    if (framebuffer_request.response == NULL
    || framebuffer_request.response->framebuffer_count < 1) {
        hcf();
    }

    struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];
    uint32_t *fb_ptr = framebuffer->address;
    uint64_t fb_width = framebuffer->width;
    uint64_t fb_height = framebuffer->height;
    uint64_t fb_pitch = framebuffer->pitch;

    ft_ctx = flanterm_fb_init(
        NULL,
        NULL,
        fb_ptr, fb_width, fb_height, fb_pitch,
        framebuffer->red_mask_size, framebuffer->red_mask_shift,
        framebuffer->green_mask_size, framebuffer->green_mask_shift,
        framebuffer->blue_mask_size, framebuffer->blue_mask_shift,
        NULL,
        NULL, NULL,
        NULL, NULL,
        NULL, NULL,
        NULL, 0, 0, 1,
        0, 0,
        0, 0
    );

    serial_init();
    init_gdt();
    load_gdt();
    PIC_remap(32, 47);

    vmm_init();
    pmm_init();
    
    serial_print("\nTesting PMM BEFORE heap_init...\n");
    void *test = pmm_alloc();
    if (test) {
        serial_print("SUCCESS: Allocated at ");
        serial_print_hex((uint64_t)test);
        serial_print("\n");
        pmm_free(test);
    } else {
        serial_print("FAILED\n");
    }
    
    serial_print("Free memory before heap: ");
    serial_print_hex(pmm_get_free_memory() / 1024 / 1024);
    serial_print(" MB\n");
    
    heap_init();
    
    serial_print("Free memory after heap: ");
    serial_print_hex(pmm_get_free_memory() / 1024 / 1024);
    serial_print(" MB\n");
    
    serial_print("\nTesting PMM AFTER heap_init...\n");
    test = pmm_alloc();
    if (test) {
        serial_print("SUCCESS: Allocated at ");
        serial_print_hex((uint64_t)test);
        serial_print("\n");
        pmm_free(test);
    } else {
        serial_print("FAILED - heap_init consumed all memory!\n");
    }

    idt_init();
    init_timer();
    init_keyboard();

    __asm__ volatile ("sti");
    
    init_ramdisk();
    init_fat12();

    shell_init();

    hcf();
}