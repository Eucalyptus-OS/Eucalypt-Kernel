#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include <logging/printk.h>
#include <gdt/gdt.h>
#include <idt/idt.h>
#include <mm/hhdm.h>
#include <mm/frame.h>

// Set the base revision to 6, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .c file, as seen fit.

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

// Halt and catch fire function.
static void hcf(void) {
    for (;;) {
#if defined (__x86_64__)
        asm ("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm ("wfi");
#elif defined (__loongarch64)
        asm ("idle 0");
#endif
    }
}

// The following will be our kernel's entry point.
// If renaming kmain() to something else, make sure to change the
// linker script accordingly.
void kmain(void) {
    // Ensure the bootloader actually understands our base revision (see spec).
    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        hcf();
    }
    printk_init();

    log_info("Initializing GDT\n");
    gdt_init();
    log_info("GDT initialized\n");
    log_info("Initializng HHDM\n");
    hhdm_init();
    log_info("HHDM initialized\n");
    log_info("Initializing frame allocator\n");
    frame_init();
    log_info("Frame allocator initialized\n");
    log_info("Initializing IDT\n");
    idt_init();
    log_info("IDT initialized\n");

    // We're done, just hang...
    hcf();
}
