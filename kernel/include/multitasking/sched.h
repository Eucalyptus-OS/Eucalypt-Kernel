#pragma once

#include <multitasking/thread.h>

#define MAX_THREADS 1024

typedef struct ready_queue {
    struct tcb *threads[MAX_THREADS];
    int front;
    int rear;
    int count;
} threads_t;

extern threads_t *rq;
 
void enable_sched();
void disable_sched();
void scheduler_init();
bool enqueue(struct tcb *thread);
struct tcb *dequeue();
uintptr_t schedule(uintptr_t rsp);