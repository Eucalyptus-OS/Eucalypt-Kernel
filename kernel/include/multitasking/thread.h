#pragma once

#include <stddef.h>
#include <stdint.h>
#include <mm/types.h>

#define KERNEL_STACK_SIZE 8192
#define USER_STACK_SIZE   8192

typedef enum {
    ready = 0,
    running,
    sleeping,
    blocked,
} state_t;

typedef struct tcb {
    uint16_t  tid;
    uintptr_t rsp;
    paddr    cr3;
    state_t state;
    void     *stack_base;   // kernel stack
    void     *ustack_base;  // user stack
    void     *entry;
} tcb_t;

static_assert(offsetof(tcb_t, rsp) == 8, "Unexpected TCB offset, correct in switch.asm");

struct tcb *create_thread(void *entry, bool user);
struct tcb *get_thread_copy(uint16_t tid);
struct tcb **get_thread(uint16_t tid);
void remove_thread(uint16_t tid);