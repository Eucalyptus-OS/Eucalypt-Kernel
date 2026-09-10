#pragma once

#include <multitasking/thread.h>
#include <mm/vmm.h>
#include <stdint.h>

struct pcb {
    uint64_t pid;
    struct vmm_space space;
    uint64_t t_count;
    struct tcb *threads;
    struct pcb *parent;
    struct pcb *children;
    struct pcb *sibling_next;
    struct tcb *waiter;
    int exit_code;
    uint8_t zombie;
    struct pcb *next;
};

struct pcb *proc_create(void *entry, void *stack);
struct pcb *proc_find(uint64_t pid);
struct pcb *proc_add_thread(struct pcb *p, void *entry, void *stack);
void proc_remove_thread(struct pcb *p, struct tcb *t);
int proc_fork(void);
int proc_exec(void *elf, uintptr_t size, void *stack);
void proc_exit(int code);
struct pcb *proc_wait(struct pcb *p);
struct pcb *proc_kill(uint64_t pid);
void proc_reap(struct pcb *zombie);
void proc_destroy(struct pcb *p);
