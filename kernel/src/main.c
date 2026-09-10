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
#include <multitasking/proc.h>
#include <multitasking/elf.h>
#include <multitasking/sched.h>

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

static void child_exit_42(void) {
    print("child: exiting with 42\n");
    proc_exit(42);
}

static void child_long_running(void) {
    volatile uint64_t sink = 0;
    for (;;) {
        sink++;
        (void)sink;
    }
}

static void test_create_wait_exit(void) {
    struct pcb *me = get_current_thread()->parent;
    struct pcb *child = proc_create(child_exit_42, 0);
    if (!child) {
        print("test create/wait/exit: FAIL (create)\n");
        return;
    }
    struct pcb *z = proc_wait(me);
    if (z && z->exit_code == 42) {
        print("test create/wait/exit: PASS\n");
    } else {
        print("test create/wait/exit: FAIL\n");
    }
    if (z) {
        proc_reap(z);
    }
}

static void test_kill(void) {
    struct pcb *me = get_current_thread()->parent;
    (void)me;
    struct pcb *child = proc_create(child_long_running, 0);
    if (!child) {
        print("test kill: FAIL (create)\n");
        return;
    }
    struct pcb *z = proc_kill(child->pid);
    if (z && z->exit_code == 1) {
        print("test kill: PASS\n");
    } else {
        print("test kill: FAIL\n");
    }
    if (z) {
        proc_reap(z);
    }
}

static void test_fork(void) {
    int r = proc_fork();
    if (r == 0) {
        print("fork: in child\n");
        proc_exit(7);
    }
    if (r <= 0) {
        print("test fork: FAIL (return %d)\n", r);
        return;
    }
    struct pcb *me = get_current_thread()->parent;
    struct pcb *z = proc_wait(me);
    if (z && z->exit_code == 7) {
        print("test fork: PASS\n");
    } else {
        print("test fork: FAIL\n");
    }
    if (z) {
        proc_reap(z);
    }
}

static const uint8_t exec_code[] = { 0xBA, 0xE9, 0x00, 0x00, 0x00, 0xB0, 0x58, 0xEE, 0xEB, 0xFD };

static void test_exec(void) {
    uint8_t blob[136] __attribute__((aligned(8)));
    memset(blob, 0, sizeof(blob));

    struct elf64_hdr *h = (struct elf64_hdr *)blob;
    h->e_ident[0] = 0x7F;
    h->e_ident[1] = 'E';
    h->e_ident[2] = 'L';
    h->e_ident[3] = 'F';
    h->e_ident[4] = ELFCLASS64;
    h->e_ident[5] = 1;
    h->e_type = ET_EXEC;
    h->e_machine = 62;
    h->e_version = 1;
    h->e_entry = 0x400000;
    h->e_phoff = 64;
    h->e_phentsize = sizeof(struct elf64_phdr);
    h->e_phnum = 1;

    struct elf64_phdr *ph = (struct elf64_phdr *)(blob + 64);
    ph->p_type = PT_LOAD;
    ph->p_flags = PF_R | PF_X;
    ph->p_offset = 120;
    ph->p_vaddr = 0x400000;
    ph->p_paddr = 0x400000;
    ph->p_filesz = sizeof(exec_code);
    ph->p_memsz = sizeof(exec_code);
    ph->p_align = 0x1000;

    memcpy(blob + 120, exec_code, sizeof(exec_code));

    int e = proc_exec(blob, sizeof(blob), 0);
    print("test exec: FAIL (proc_exec returned %d)\n", e);
}

static void proc_tests(void) {
    print("proc: running tests\n");
    test_create_wait_exit();
    test_kill();
    test_fork();
    test_exec();
    print("proc: tests done (exec FAIL means still here)\n");
}

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
    proc_create(proc_tests, 0);

    for (;;) {
        asm volatile ("hlt");
    }
}
