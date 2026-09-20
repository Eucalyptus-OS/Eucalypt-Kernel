#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <multitasking/proc.h>

// Kernel stack size (64 KiB) per thread.
#define KSTACK_SIZE 0x10000

extern uint64_t thread_count;

// Thread lifecycle states, tracked in tcb.state.
typedef enum {
    Ready,
    Running,
    Blocked,
    Exited,
    Sleeping,
} state_t;

struct tcb {
    uint64_t tid;
    // Saved kernel-stack pointer (stored/loaded by the context switch).
    void *ksp;
    // Top of the mapped kernel stack (used to free it on destroy).
    void *kstack_top;
    // User stack pointer this thread runs on in ring 3.
    void *tsp;
    // Root paging table (PML4) the thread executes under.
    uintptr_t addr_space;
    // Links in the global, all-threads circular list.
    struct tcb *next;
    // Link in the owner process's circular thread list.
    struct tcb *proc_next;
    // The PCB that owns this thread.
    struct pcb *parent;
    // Private FXSAVE/FXRSTOR area (512 bytes) for FPU state.
    void *fpu_area;
    uint8_t state;
    // User FS base to program into MSR 0xC0000100 on every switch.
    uint64_t fs_base;
    // Tick at which a timed block is allowed to wake.
    uint64_t wake_tick;
    // Non-zero while the thread is waiting on wake_tick.
    uint8_t timed;
} __attribute__((packed));

// Global circular list of every live thread.
extern struct tcb *thread_list;

// Create a thread for process p, including its kernel stack and FPU area.
struct tcb *create_thread(void *entry, struct pcb *p, void *ustack);
// Free a finished thread's resources and unlink it from both lists.
void destroy_thread(struct tcb *t);
// Allocate the next kernel stack from the reserved region.
void *alloc_kernel_stack();
// Return a kernel stack region to the virtual memory manager.
void free_kernel_stack(void *base);