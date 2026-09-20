#pragma once

#include <stdint.h>
#include <stddef.h>
#include <signal.h>

#define MAX_FDS 256

// Wait event bits: set on a child when it exits/stops/continues.
#define WAIT_EVT_EXITED    1
#define WAIT_EVT_STOPPED   2
#define WAIT_EVT_CONTINUED 4

extern uint64_t process_count;

// A single mmap() allocation tracked by the process.
struct mmap_region {
    uintptr_t base;
    size_t len;
    uint32_t prot;
    struct mmap_region *next;
};

struct pcb {
    uint64_t pid;
    uint64_t pgid;
    uint64_t t_count;
    // Root paging table (PML4) this process executes under.
    uintptr_t addr_space;
    // First thread of the process; threads are a circular list off this.
    struct tcb *t;
    // sbrk heap bounds: the break lives in [heap_begin, heap_end).
    uintptr_t heap_begin;
    uintptr_t heap_end;
    uint64_t exit_code;
    uint8_t stopped;
    uint8_t is_zombie;
    uint8_t wait_events;
    int wait_stop_sig;
    // Parent PCB; children are re-parented here when this process dies.
    struct pcb *ppcb;
    // Links in the zombie reap list.
    struct pcb *z_prev;
    struct pcb *z_next;
    // Pending/ignored signals and signal handlers, inherited by children.
    sigstate_t sigstate;
    uint32_t umask;
    // Outstanding mmap() allocations and the next placement hint.
    struct mmap_region *mmaps;
    uintptr_t mmap_cursor;
    // Link in the global circular process list.
    struct pcb *next;
    // Open file descriptors, indexed by fd number.
    struct vfs_file *fd_table[MAX_FDS];
};

// Global circular list of all live processes, with the pid counter.
extern struct pcb *proc_list;
extern uint64_t proc_count;

// Spawn a new process whose first thread runs the given entry function.
struct pcb *proc_create(void *entry);
// Return the live process with the given pid, or NULL.
struct pcb *proc_find(uint64_t pid);
// Free all resources of a dead process and clear it from the process list.
void proc_destroy(struct pcb *p);
// Reserve or release user heap memory; returns the previous break address.
uintptr_t proc_sbrk(struct pcb *p, intptr_t increment);
// Wait for a child to exit/stop/continue and collect its status.
int waitpid(int pid, int *status, int options);

// Queue an exited process onto the zombie reap list.
void zombie_enqueue(struct pcb *p);
// Take a process off the zombie reap list.
void zombie_remove(struct pcb *p);
// Remove and return the oldest zombie (used by reaping logic).
struct pcb *zombie_pop();
// Head of the zombie reap list.
extern struct pcb *zombie_head;