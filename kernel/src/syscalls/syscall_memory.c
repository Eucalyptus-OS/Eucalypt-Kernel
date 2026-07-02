#include "logging/smp_console.h"
#include <stdint.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <mm/paging.h>
#include <multitasking/sched.h>
#include <multitasking/proc.h>
#include <syscalls/syscall_memory.h>

#define PML4_VIRT(cr3) ((uint64_t *)((cr3) + offset))

uint64_t sys_mmap(uint64_t hint, uint64_t size, uint64_t flags, uint64_t d, uint64_t e, uint64_t f) {
    (void)hint; (void)flags; (void)d; (void)e; (void)f;

    struct pcb *proc = proc_get(get_current_pid());
    if (!proc) return (uint64_t)-1;

    size = (size + 0xFFF) & ~(uint64_t)0xFFF;

    if (proc->heap_end + size < proc->heap_end)
        return (uint64_t)-1;

    uint64_t vaddr = proc->heap_end;

    smp_printf(3, 0xFFFFFFFF, "sys_mmap: Allocating %lu bytes at virtual address 0x%lx for process %d, Heap end: 0x%lX\n", size, vaddr, proc->pid, proc->heap_end);

    for (uint64_t i = 0; i < size; i += 0x1000) {
        uint64_t phys = frame_alloc();
        paging_map_page(PML4_VIRT(proc->cr3), vaddr + i, phys, 0x1000,
                        ENTRY_FLAG_PRESENT | ENTRY_FLAG_RW | ENTRY_FLAG_USER);
    }

    proc->heap_end += size;
    return vaddr;
}

uint64_t sys_munmap(uint64_t addr, uint64_t size, uint64_t unused, uint64_t d, uint64_t e, uint64_t f) {
    (void)unused; (void)d; (void)e; (void)f;

    struct pcb *proc = proc_get(get_current_pid());
    if (!proc) return (uint64_t)-1;

    size = (size + 0xFFF) & ~(uint64_t)0xFFF;

    for (uint64_t i = 0; i < size; i += 0x1000) {
        uint64_t entry = paging_get_entry(PML4_VIRT(proc->cr3), addr + i);
        if (entry & ENTRY_FLAG_PRESENT)
            frame_free(entry & ENTRY_4K_ADDRESS_MASK);
    }

    paging_unmap_page(PML4_VIRT(proc->cr3), addr, size);
    return 0;
}