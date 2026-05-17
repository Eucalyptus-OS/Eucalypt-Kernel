#include <logging/printk.h>
#include <gdt/gdt.h>
#include <mm/heap.h>
#include <multitasking/thread.h>
#include <multitasking/sched.h>

extern void context_switch(struct tcb *current, struct tcb *next);
static threads_t ready_queue_data;
static struct tcb *to_reap = NULL;

struct tcb *current_thread = NULL;
threads_t *tq = &ready_queue_data;

static bool enabled = false;

void enable_sched() {
    __atomic_store_n(&enabled, true, __ATOMIC_RELEASE);
}

void disable_sched() {
    __atomic_store_n(&enabled, false, __ATOMIC_RELEASE);
}

void scheduler_init() {
    tq->front = 0;
    tq->rear  = 0;
    tq->count = 0;

    disable_sched();
}

bool enqueue(struct tcb *thread) {
    if (tq->count == MAX_THREADS)
        return false;

    tq->threads[tq->rear] = thread;
    tq->rear = (tq->rear + 1) % MAX_THREADS;
    tq->count++;

    return true;
}

struct tcb *dequeue() {
    if (tq->count == 0)
        return NULL;

    struct tcb *thread = tq->threads[tq->front];
    tq->front = (tq->front + 1) % MAX_THREADS;
    tq->count--;

    return thread;
}

uintptr_t schedule(uintptr_t rsp) {
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        return rsp;

    if (to_reap) {
        if (to_reap->stack_base) kfree(to_reap->stack_base);
        kfree(to_reap);
        to_reap = NULL;
    }

    if (current_thread == NULL) {
        current_thread = tq->threads[tq->front];
        return current_thread->rsp;
    }

    struct tcb *current = dequeue();
    if (current->state == dead) {
        if (current->ustack_base) kfree(current->ustack_base);
        to_reap = current;
    } else if (rsp != 0) {
        current->rsp = rsp;
        enqueue(current);
    }

    if (tq->count == 0)
        return 0;

    struct tcb *next = tq->threads[tq->front];
    current_thread = next;

    tss.rsp0 = (uintptr_t)next->stack_base + KERNEL_STACK_SIZE;

    paddr current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if (next->cr3 != current_cr3)
        __asm__ volatile("mov %0, %%cr3" :: "r"(next->cr3));

    return next->rsp;
}