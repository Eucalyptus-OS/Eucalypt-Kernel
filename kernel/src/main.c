#include <stdint.h>
#include <stdbool.h>
#include <limine.h>
#include <logging/flanterm.h>
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
#include <auxv.h>
#include <interrupts/apic.h>
#include <multitasking/thread.h>
#include <multitasking/sched.h>
#include <multitasking/proc.h>
#include <drivers/pci.h>
#include <drivers/block/ahci.h>
#include <drivers/block/ramfs.h>
#include <drivers/acpi.h>
#include <drivers/fs/vfs/vfs.h>
#include <drivers/fs/devfs/devfs.h>
#include <drivers/tty.h>
#include <drivers/input.h>
#include <drivers/fs/gpt.h>
#include <smp.h>

extern void enable_sse();

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
    while (1) {
        __asm__ volatile("hlt");
    }
}

int test_thread() {
    return 1;
}

uint64_t alloc_user_stack(uint64_t *cr3) {
    uint64_t user_stack_base = 0x70000000000;
    uint64_t pages = 4;
    uint64_t flags =
        ENTRY_FLAG_PRESENT |
        ENTRY_FLAG_RW      |
        ENTRY_FLAG_NX      |
        ENTRY_FLAG_USER;

    for (uint64_t i = 0; i < pages; i++) {
        paddr frame = frame_alloc();
        vaddr virt  = user_stack_base + (i * 0x1000);
        paging_map_page(cr3, virt, frame, 0x1000, flags);
    }

    return user_stack_base + (pages * 0x1000);
}

extern struct flanterm_context *ft_ctx;

void putchar(tty_t *tty, char c) {
    (void)tty;
    flanterm_write(ft_ctx, &c, 1);
}

void kmain(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        hcf();
    }

    printk_init();
    gdt_init();
    log_info("GDT initialized\n");
    idt_init();
    log_info("IDT initialized\n");
    hhdm_init();
    log_info("HHDM initialized\n");
    acpi_log_tables();
    pci_log_ids_once();
    frame_init();
    log_info("Frame allocator initialized\n");
    paging_init();
    log_info("Paging initialized\n");
    heap_init();
    log_info("Heap initialized\n");
    enable_apic(0, true);
    log_info("APIC enabled\n");
    ioapic_init();
    log_info("IOAPIC initalized\n");
    apic_timer_init(1000);
    log_info("APIC timer initialized\n");
    ahci_init();
    log_info("AHCI initialized\n");
    enable_sse();
    vfs_init();
    log_info("VFS initialized\n");
    tty_init(putchar);
    input_init();
    ps2_keyboard_init();
    ps2_mouse_init();
    log_info("Input system initialized\n");
    ramfs_init();
    log_info("RAMFS initialized\n");
    gpt_init();
    log_info("GPT initialized\n");

    if (!ramfs_addr || ramfs_size == 0) {
        log_error("No ramfs module loaded\n");
    } else {
        uint64_t capacity = ramfs_size + (1024 * 1024);
        uint8_t  mret     = ramfs_mount(ramfs_addr, ramfs_size, capacity);
        if (mret != VFS_OK) {
            log_error("Failed to mount ramfs: %d\n", mret);
            hcf();
        }
        log_info("Ramfs mounted\n");
    }

    scheduler_init();
    
    enable_sched();
    log_info("Scheduler enabled");

    asm volatile ("sti");

    smp_init();
    log_info("SMP initialized\n");

    for (;;) {
        __asm__ volatile("hlt");
    }
}
