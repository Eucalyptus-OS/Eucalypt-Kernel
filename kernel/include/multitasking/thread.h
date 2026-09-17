#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <multitasking/proc.h>

#define KSTACK_SIZE 0x10000

extern uint64_t thread_count;

typedef enum {
    Ready,
    Running,
    Blocked,
    Exited,
    Sleeping,
} state_t;

struct tcb {
    uint64_t tid;
    void *ksp;
    void *kstack_top;
    void *tsp;
    uintptr_t addr_space;
    struct tcb *next;
    struct tcb *proc_next;
    struct pcb *parent;
    void *fpu_area;
    uint8_t state;
    uint64_t fs_base;
    uint64_t wake_tick;
    uint8_t timed;
} __attribute__((packed));

extern struct tcb *thread_list;

struct tcb *create_thread(void *entry, struct pcb *p, void *ustack);
void destroy_thread(struct tcb *t);
void *alloc_kernel_stack();
void free_kernel_stack(void *base);