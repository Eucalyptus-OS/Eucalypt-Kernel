#include <stdint.h>
#include <stdbool.h>
#include <limine.h>
#include <logging/printk.h>
#include <gdt/gdt.h>
#include <idt/idt.h>
#include <mm/hhdm.h>
#include <mm/frame.h>
#include <mm/paging.h>
#include <mm/heap.h>
#include <interrupts/apic.h>
#include <multitasking/thread.h>
#include <multitasking/sched.h>
#include <drivers/pci.h>
#include <drivers/block/ahci.h>

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

void idle_thread(void) {
    while (1) __asm__ volatile("hlt");
}

void thread_a(void) {
    while (1) {
        log_info("Thread A running\n");
        // busy loop to simulate work
        for (volatile int i = 0; i < 1000000; i++);
    }
}

void thread_b(void) {
    while (1) {
        log_info("Thread B running\n");
        for (volatile int i = 0; i < 1000000; i++);
    }
}

// The following will be our kernel's entry point.
// If renaming kmain() to something else, make sure to change the
// linker script accordingly.
void kmain(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false)
        hcf();

    printk_init();
    gdt_init();
    log_info("GDT initialized\n");
    idt_init();
    log_info("IDT initialized\n");
    hhdm_init();
    log_info("HHDM initialized\n");
    frame_init();
    log_info("Frame allocator initialized\n");
    paging_init();
    log_info("Paging initialized\n");
    heap_init();
    log_info("Heap initialized\n");
    enable_apic(true);
    log_info("APIC enabled\n");
    ahci_init();
    log_info("AHCI initialized\n");

    scheduler_init();

    create_thread(idle_thread, false);

    for (int i = 0; i < 50; i++)
        create_thread(thread_a, false);

    for (int i = 0; i < 50; i++)
        create_thread(thread_b, false);

    log_info("Threads created count=%d\n", tq->count);

    enable_sched();
    log_info("Scheduler enabled\n");

    apic_timer_init(1000);
    log_info("APIC timer initialized at 1000 Hz\n");

    hcf();
}