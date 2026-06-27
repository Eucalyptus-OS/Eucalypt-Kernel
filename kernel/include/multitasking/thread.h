#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <mm/types.h>
#include <auxv.h>

#define KERNEL_STACK_SIZE 0x4000
#define MAX_THREADS       1024

typedef enum {
    ready,
    running,
    blocked,
    dead,
} thread_state_t;

struct pcb;

struct tcb {
    uint16_t       tid;
    struct pcb    *parent;
    paddr          cr3;
    thread_state_t state;
    void          *stack_base;
    void          *ustack_base;
    void          *entry;
    uint64_t       rsp;
    uint8_t        owning_cpu;
};

extern struct tcb     *current_thread;

struct tcb *create_thread(void *entry, paddr cr3);
struct tcb *create_user_thread(uint64_t entry, paddr cr3);
struct tcb *create_user_thread_with_stack(uint64_t entry, paddr cr3,
                                           char **argv, char **envp,
                                           const elf_load_info_t *info);
struct tcb *thread_fork(struct tcb *parent, paddr cr3);
void        thread_destroy(struct tcb *thread);
void        handle_ret(int64_t code);
uint64_t    setup_stack(uint8_t *stack_base, uint64_t stack_size, void *entry);