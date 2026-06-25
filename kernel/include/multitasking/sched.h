#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <multitasking/thread.h>

void        scheduler_init(void);
void        enable_sched(void);
void        disable_sched(void);
bool        enqueue(struct tcb *thread);
struct tcb *dequeue(void);
uintptr_t schedule(uintptr_t rsp);
void        sched_yield(void);
void        sched_sleep(struct tcb *t);
void        sched_wake(struct tcb *t);
int32_t     get_current_pid(void);
int32_t     get_current_ppid(void);
struct tcb *get_current_thread(void);
struct tcb *get_thread_copy(uint16_t tid);
struct tcb **get_thread(uint16_t tid);