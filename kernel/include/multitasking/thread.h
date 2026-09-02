#pragma once

#include <stdint.h>

typedef enum {
    Ready,
    Running,
    Blocked,
    Exited,
    Sleeping,
} state_t;

// Linked list of threads.
struct tcb {
    uint64_t tid;
    void *ksp;
    void *kstack_top;
    void *tsp;
    uintptr_t addr_space;
    struct tcb *next;
    uint8_t state;
    uint64_t wake_tick;
    uint8_t timed;
} __attribute__((packed));

extern struct tcb *thread_list;
extern uint64_t thread_count;

struct tcb *thread_create(void *entry, void *ustack);