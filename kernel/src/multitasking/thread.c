#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <mm/types.h>
#include <mm/heap.h>
#include <mm/frame.h>
#include <mm/paging.h>
#include <mm/vm.h>
#include <mm/hhdm.h>

#include <gdt/gdt.h>

#include <multitasking/sched.h>
#include <multitasking/thread.h>

#define USER_CS   0x1B
#define USER_SS   0x23
#define KERNEL_CS 0x08
#define KERNEL_SS 0x10

extern void thread_trampoline(void);

uint16_t next_tid = 0;

struct stack_alloc {
    void *raw;
    void *aligned;
};

static struct stack_alloc alloc_aligned_stack(size_t size) {
    struct stack_alloc stack = {0};

    uintptr_t raw = (uintptr_t)kmalloc(size + 0x1000);

    if (!raw)
        return stack;

    uintptr_t aligned = (raw + 0xFFF) & ~0xFFFULL;

    stack.raw = (void *)raw;
    stack.aligned = (void *)aligned;

    return stack;
}

uint64_t setup_stack(uint8_t *stack_base, uint64_t stack_size, void *entry) {
    uint64_t *stack_top = (uint64_t *)(stack_base + stack_size);
    uint64_t *rsp = stack_top;

    *--rsp = KERNEL_SS;
    *--rsp = (uint64_t)stack_top;
    *--rsp = 0x202;
    *--rsp = KERNEL_CS;
    *--rsp = (uint64_t)thread_trampoline;

    for (int i = 0; i < 15; i++)
        *--rsp = 0;

    rsp[13] = (uint64_t)entry;

    return (uint64_t)rsp;
}

struct tcb *create_thread(void *entry, bool user) {
    struct stack_alloc kstack = alloc_aligned_stack(KERNEL_STACK_SIZE);

    if (!kstack.aligned)
        return NULL;

    struct stack_alloc ustack = {0};

    if (user) {
        ustack = alloc_aligned_stack(USER_STACK_SIZE);

        if (!ustack.aligned) {
            kfree(kstack.raw);
            return NULL;
        }
    }

    struct tcb *tcb = kmalloc(sizeof(struct tcb));

    if (!tcb) {
        kfree(kstack.raw);

        if (ustack.raw)
            kfree(ustack.raw);

        return NULL;
    }

    paddr current_cr3;

    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));

    tcb->tid         = next_tid++;
    tcb->cr3         = current_cr3;
    tcb->stack_base  = kstack.raw;
    tcb->ustack_base = ustack.raw;
    tcb->entry       = entry;

    tcb->rsp = setup_stack(
        (uint8_t *)kstack.aligned,
        KERNEL_STACK_SIZE,
        entry
    );

    enqueue(tcb);

    return tcb;
}

struct tcb *get_thread_copy(uint16_t tid) {
    for (int i = 0; i < rq->count; i++) {
        int idx = (rq->front + i) % MAX_THREADS;

        if (rq->threads[idx]->tid == tid)
            return rq->threads[idx];
    }

    return NULL;
}

struct tcb **get_thread(uint16_t tid) {
    for (int i = 0; i < rq->count; i++) {
        int idx = (rq->front + i) % MAX_THREADS;

        if (rq->threads[idx]->tid == tid)
            return &rq->threads[idx];
    }

    return NULL;
}

void remove_thread(uint16_t tid) {
    int found_index = -1;

    for (int i = 0; i < rq->count; i++) {
        int idx = (rq->front + i) % MAX_THREADS;

        if (rq->threads[idx]->tid == tid) {
            found_index = idx;
            break;
        }
    }

    if (found_index == -1)
        return;

    struct tcb *tcb_to_remove = rq->threads[found_index];

    int current = found_index;

    for (int i = 0; i < rq->count - 1; i++) {
        int next = (current + 1) % MAX_THREADS;

        rq->threads[current] = rq->threads[next];

        current = next;
    }

    rq->rear = (rq->rear - 1 + MAX_THREADS) % MAX_THREADS;
    rq->count--;

    if (tcb_to_remove->ustack_base)
        kfree(tcb_to_remove->ustack_base);

    if (tcb_to_remove->stack_base)
        kfree(tcb_to_remove->stack_base);

    kfree(tcb_to_remove);
}