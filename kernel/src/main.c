#include <stdint.h>
#include <stdbool.h>
#include <limine.h>
#include <logging/printk.h>
#include <gdt/gdt.h>
#include <idt/idt.h>
#include <mm/hhdm.h>
#include <mm/frame.h>
#include <mm/paging.h>
#include <mm/vm.h>
#include <mm/heap.h>
#include <mm/types.h>
#include <binl/elf64.h>
#include <interrupts/apic.h>
#include <multitasking/thread.h>
#include <multitasking/sched.h>
#include <multitasking/proc.h>
#include <drivers/pci.h>
#include <drivers/block/ahci.h>
#include <drivers/block/ramfs.h>

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

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

[[gnu::noreturn]]
void idle_thread(void) {
    while (1) __asm__ volatile("hlt");
}

int thread_a(void) {
    while (1) {
        log_info("Thread A running\n");
        for (volatile int i = 0; i < 1000000; i++);
        return -1;
    }
}

int thread_b(void) {
    while (1) {
        log_info("Thread B running\n");
        for (volatile int i = 0; i < 1000000; i++);
        return 1;
    }
}

uint64_t alloc_user_stack(uint64_t *cr3) {
    uint64_t user_stack_base = 0x70000000000;
    uint64_t pages = 4;
    uint64_t flags = ENTRY_FLAG_PRESENT | ENTRY_FLAG_RW | ENTRY_FLAG_NX | ENTRY_FLAG_USER;
    for (uint64_t i = 0; i < pages; i++) {
        paddr frame = frame_alloc();
        vaddr virt = user_stack_base + (i * 0x1000);
        paging_map_page(cr3, virt, frame, 0x1000, flags);
    }
    return user_stack_base + (pages * 0x1000);
}

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
    ramfs_init();
    log_info("Ramfs initialized\n");

    scheduler_init();

    paddr idle_cr3 = paging_create_pml4();
    create_thread(idle_thread, idle_cr3, false);
    log_info("Idle thread created\n");

    ramfs_file_t *app = ramfs_read(ramfs_addr, "main");
    if (!app) {
        log_error("Failed to find user application in ramfs\n");
        hcf();
    }

    paddr user_cr3 = paging_create_pml4();
    uint64_t *pml4 = (uint64_t *)(offset + user_cr3);

    uint64_t entry = elf64_parse(app->data, user_cr3);
    if (entry == 0) {
        log_error("Failed to parse user application\n");
        hcf();
    }

    uint64_t user_stack = alloc_user_stack(pml4);
    log_info("Creating user thread: entry=%llx stack=%llx cr3=%llx\n", entry, user_stack, user_cr3);

    create_user_thread(entry, user_cr3, user_stack);

    enable_sched();
    apic_timer_init(1000);

    __asm__ volatile("sti");
    for (;;) __asm__ volatile("hlt");
}