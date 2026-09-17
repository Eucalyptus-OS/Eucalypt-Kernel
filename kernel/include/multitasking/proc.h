#pragma once

#include <stdint.h>
#include <stddef.h>
#include <signal.h>

#define MAX_FDS 256

#define WAIT_EVT_EXITED    1
#define WAIT_EVT_STOPPED   2
#define WAIT_EVT_CONTINUED 4

extern uint64_t process_count;

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
    uintptr_t addr_space;
    struct tcb *t;
    uintptr_t heap_begin;
    uintptr_t heap_end;
    uint64_t exit_code;
    uint8_t stopped;
    uint8_t is_zombie;
    uint8_t wait_events;
    int wait_stop_sig;
    struct pcb *ppcb;
    struct pcb *z_prev;
    struct pcb *z_next;
    sigstate_t sigstate;
    uint32_t umask;
    struct mmap_region *mmaps;
    uintptr_t mmap_cursor;
    struct pcb *next;
    struct vfs_file *fd_table[MAX_FDS];
};

extern struct pcb *proc_list;
extern uint64_t proc_count;

struct pcb *proc_create(void *entry);
struct pcb *proc_find(uint64_t pid);
void proc_destroy(struct pcb *p);
uintptr_t proc_sbrk(struct pcb *p, intptr_t increment);
int waitpid(int pid, int *status, int options);

void zombie_enqueue(struct pcb *p);
void zombie_remove(struct pcb *p);
struct pcb *zombie_pop();
extern struct pcb *zombie_head;