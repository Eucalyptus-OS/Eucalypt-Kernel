#include <logging/printk.h>

#include <gdt/gdt.h>

#include <multitasking/thread.h>
#include <multitasking/sched.h>

extern void context_switch(struct tcb *current, struct tcb *next);

static threads_t ready_queue_data;

threads_t *rq = &ready_queue_data;

static bool enabled = false;

void enable_sched() {
    __atomic_store_n(&enabled, true, __ATOMIC_RELEASE);
}

void disable_sched() {
    __atomic_store_n(&enabled, false, __ATOMIC_RELEASE);
}

void scheduler_init() {
    rq->front = 0;
    rq->rear  = 0;
    rq->count = 0;

    disable_sched();
}

bool enqueue(struct tcb *thread) {
    if (rq->count == MAX_THREADS)
        return false;

    rq->threads[rq->rear] = thread;

    rq->rear = (rq->rear + 1) % MAX_THREADS;

    rq->count++;

    return true;
}

struct tcb *dequeue() {
    if (rq->count == 0)
        return NULL;

    struct tcb *thread = rq->threads[rq->front];

    rq->front = (rq->front + 1) % MAX_THREADS;

    rq->count--;

    return thread;
}

uintptr_t schedule(uintptr_t rsp) {
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        return rsp;

    if (rq->count < 2)
        return rsp;

    struct tcb *current = dequeue();

    current->rsp = rsp;

    enqueue(current);

    struct tcb *next = rq->threads[rq->front];

    tss.rsp0 = ((((uintptr_t)next->stack_base + 0xFFF) & ~0xFFFULL)
               + KERNEL_STACK_SIZE) & ~0xFULL;

    paddr current_cr3;

    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));

    if (next->cr3 != current_cr3)
        __asm__ volatile("mov %0, %%cr3" :: "r"(next->cr3));

    return next->rsp;
}