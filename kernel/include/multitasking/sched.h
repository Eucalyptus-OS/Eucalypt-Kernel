#pragma once

// The thread currently running (or about to run) on this CPU.
extern struct tcb *current_tcb;

// Pick the next Ready thread and context-switch to it.
void schedule();
// Return the thread that owns the CPU.
struct tcb *sched_current_thread();
// Return the process owning the running thread.
struct pcb *sched_current_proc();
// Sleep the current thread until it is explicitly woken.
struct tcb *block_current();
// Sleep the current thread until the system clock passes wake_tick.
struct tcb *block_current_timeout(uint64_t wake_tick);
// Wake all blocked threads whose deadline has passed (timer tick hook).
void sched_check_timeouts();
// Mark t asleep without blocking the calling thread.
void sched_sleep_thread(struct tcb *t);
// Wake a sleeping thread; it resumes on the next scheduling pass.
void sched_wake_thread(struct tcb *t);
// Make t Ready again, safe to call from any context.
void unblock(struct tcb *t);
