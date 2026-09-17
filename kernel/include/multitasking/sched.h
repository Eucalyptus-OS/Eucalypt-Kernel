#pragma once

extern struct tcb *current_tcb;

void schedule();
struct tcb *sched_current_thread();
struct pcb *sched_current_proc();
struct tcb *block_current();
struct tcb *block_current_timeout(uint64_t wake_tick);
void sched_check_timeouts();
void sched_sleep_thread(struct tcb *t);
void sched_wake_thread(struct tcb *t);
void unblock(struct tcb *t);
