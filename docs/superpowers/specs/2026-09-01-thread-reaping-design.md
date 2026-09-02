# Thread Reaping Design

## Goal

Add automatic thread reaping to the multitasking system. When a thread
terminates itself, the scheduler unlinks it from the ready list and frees all
of its resources (kernel stack frame, user stack frame, address space root, and
the TCB itself) on the next scheduling pass. No join API is needed; reaping
happens automatically.

## Current State

- Threads are `struct tcb` entries in `thread_list`, a single circular linked
  list managed by `thread.c` and `sched.c`.
- `thread_create()` allocates a TCB via `kmalloc`, a kernel stack frame via
  `frame_alloc`, and an independent address space root via `create_pml4()`.
- There is an `Exited` state in `state_t` but nothing ever sets it, and no code
  removes a thread from the list or frees its resources.
- `schedule()` round-robins through the list looking for a `Ready` thread and
  `switch_task`s to it.
- Memory available: `kfree()` for heap, `frame_free()` for physical frames.

## Design

### 1. `thread_exit()` — new function in `thread.c`

Declared in `thread.h` and defined so a thread can terminate itself:

```
void thread_exit(void) {
    struct tcb *t = get_current_thread();
    if (t) {
        t->state = Exited;
    }
    schedule();
    for (;;) asm volatile("hlt");  // unreachable; safety net
}
```

The current thread marks itself `Exited` and calls `schedule()`. The switch away
saves its context normally. When the reaper later unlinks and frees it, the
thread is no longer running, so freeing is safe.

### 2. Reap logic inside the `schedule()` scan

The existing scan that hunts for the next `Ready` thread becomes the reaper.
Whenever the scan encounters an `Exited` thread, it unlinks it from
`thread_list`, decrements `thread_count`, and frees its resources.

Reaped per thread `t`:

- kernel stack frame: `frame_free(virt_to_phys(t->kstack_top - STACK_SIZE))`
- user stack frame (if present): `frame_free(virt_to_phys(t->tsp))`
- address space root: `frame_free(t->addr_space)`
- TCB: `kfree(t)`

A small `reap_thread()` helper encapsulates the unlink + free so the scan loop
stays readable.

### 3. Safety rules

- The scan never reaps the thread it is currently switching to (`next` once a
  `Ready` thread is found), nor the currently-running `current_tcb`.
- The scan frees `Exited` threads it passes while hunting for a `Ready` one;
  it continues past them rather than stopping.
- Because unlinking `next` changes list linkage, the walk updates its
  `prev`/`next` pointers whenever it removes a node, keeping iteration correct.
- The circular list edge cases are covered: if reaping empties the list or
  shrinks it, the index guards use the updated `thread_count`, and the existing
  "no runnable thread" return path (spinlock release + return) still works.
- Reaping holds the `sched_lock` (the scan already runs under it), so it is
  safe from concurrent `schedule()` calls.

### Error handling / edge cases

- If no `Ready` thread exists, any `Exited` threads encountered are still
  reaped, then schedule falls through to the existing no-runnable return.
- If the last remaining thread exits, `thread_list` becomes `NULL` /
  `thread_count == 0`, and `schedule()` returns as it does today.
- `thread_exit()` never returns; the final `hlt` loop is a safety net.

## Testing

There is no user-space harness in this tree (`main.c` just `hlt`s after init),
so verification is build-based plus an optional runtime check:

1. The kernel builds cleanly with the new `thread_exit()` and reaper changes.
2. Optionally, an init-time routine spawns a few short-lived kernel threads that
   call `thread_exit()` after a bounded loop, confirming they are reaped rather
   than re-entering the run queue (they never run again and their resources are
   freed).

## Non-Goals

- No `thread_join` / exit-code collection API (reaping is fully automatic).
- No `struct vmm_space` refactor of the TCB: reaping uses the bare `addr_space`
  pml4 directly, matching how `thread_create()` builds threads today.
- No user-mode teardown beyond freeing the mapped stack frames.
