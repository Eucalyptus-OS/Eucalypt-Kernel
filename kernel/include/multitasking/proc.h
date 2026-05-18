#pragma once

#include <stdint.h>
#include <mm/types.h>
#include <multitasking/thread.h>

#define MAX_PROCS 1024

struct pcb {
    uint16_t pid;
    paddr cr3;
    struct tcb *threads[MAX_THREADS];
};

extern struct pcb *proc_table[MAX_PROCS];
extern uint16_t next_pid;

struct pcb *proc_create(void *entry, bool user);
struct pcb *proc_add_thread(struct pcb *pcb, void *entry, bool user);
struct pcb *proc_get_copy(uint16_t pid);
void proc_destroy(struct pcb *pcb);