# Thread Reaping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add automatic thread reaping so that terminated threads are unlinked from the run list and have all their resources freed by the scheduler.

**Architecture:** A new `thread_exit()` marks the calling thread `Exited` and enters the scheduler. The existing `schedule()` round-robin scan doubles as the reaper: while hunting for the next `Ready` thread it unlinks and frees any `Exited` threads it passes, releasing the kernel stack frame, user stack frame, address-space root, and TCB.

**Tech Stack:** C (gnu11, freestanding, `-ffreestanding`), NASM (x86_64), Limine bootloader, Make. No libc — no unit test framework available; verification is a clean kernel build plus an optional QEMU runtime smoke test.

## Global Constraints

- Kernel is freestanding: no libc, no `printf` beyond `kernel/src/logging/print.c`'s `print()`. No unit test framework.
- Threads live in a single **circular** linked list `thread_list` (`thread.c`), with `thread_count` tracking the number of live threads.
- Memory helpers: `kmalloc()`/`kfree()` (kernel heap), `frame_alloc()`/`frame_free()` (physical frames), `virt_to_phys()`/`phys_to_virt()` (HHDM).
- The `tcb` struct is `__attribute__((packed))` and its layout is mirrored in `switch.asm` via the `struc tcb`. Adding fields is acceptable but must keep the struct packed layout consistent with any new asm usage; no asm layout change is planned for this work.
- `schedule()` runs under `sched_lock` (acquired via `spinlock_acquire_irqsave`); the reaper inherits that lock.
- `STACK_SIZE` is defined as `4096` in `thread.c`.
- Build command: `make -C kernel` (from repo root). Output: `kernel/bin-x86_64/kernel`.
- Do not add comments unless they explain a non-obvious safety invariant (kernel code convention here avoids decorative comments).

---

### Task 1: Add `thread_exit()` and expose it in the header

**Files:**
- Modify: `kernel/include/multitasking/thread.h`
- Modify: `kernel/src/multitasking/thread.c`

**Interfaces:**
- Consumes: `get_current_thread()` (from `sched.h`), `schedule()` (from `sched.h`).
- Produces: `void thread_exit(void)` — marks the current thread `Exited` and schedules away. Never returns.

- [ ] **Step 1: Declare `thread_exit` in the header**

Add to `kernel/include/multitasking/thread.h` after the existing `thread_create` declaration (line 29), and add the needed `sched.h` include at the top:

```c
#pragma once

#include <stdint.h>

#include <multitasking/sched.h>
```

and after line 29:

```c
void thread_exit(void);
```

- [ ] **Step 2: Implement `thread_exit` in `thread.c`**

Add `#include <multitasking/sched.h>` to the includes at the top of `kernel/src/multitasking/thread.c`, and append the function after `thread_create`:

```c
void thread_exit(void) {
    struct tcb *t = get_current_thread();
    if (t) {
        t->state = Exited;
    }
    schedule();
    for (;;) asm volatile ("hlt");
}
```

- [ ] **Step 3: Build to verify it compiles**

Run: `make -C kernel`
Expected: The kernel links cleanly into `kernel/bin-x86_64/kernel` with no errors or new warnings.

- [ ] **Step 4: Commit**

```bash
git add kernel/include/multitasking/thread.h kernel/src/multitasking/thread.c
git commit -m "feat: add thread_exit to terminate a thread"
```

---

### Task 2: Reap `Exited` threads inside the `schedule()` scan

**Files:**
- Modify: `kernel/src/multitasking/sched.c`

**Interfaces:**
- Consumes: `thread_list`, `thread_count`, `struct tcb` fields (`state`, `kstack_top`, `tsp`, `addr_space`, `next`, `tid`) from `thread.h`; `STACK_SIZE` value `4096`; `kfree()` from `mm/heap.h`; `frame_free()` from `mm/frame.h`; `virt_to_phys()` from `mm/hhdm.h`.
- Produces: (no new public API) Reaping behavior inside `schedule()`; refactors to `schedule()`'s scan loop.

- [ ] **Step 1: Add includes needed for freeing**

Add to the top of `kernel/src/multitasking/sched.c` (they are not currently present):

```c
#include <mm/heap.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <stdint.h>
```

- [ ] **Step 2: Add a static `reap_thread` helper**

Insert a static helper before `schedule()`. Given the packed `tcb`, guard the user-stack free so a NULL `tsp` is not dereferenced:

```c
#define THREAD_STACK_SIZE 4096

static void reap_thread(struct tcb *t) {
    frame_free(virt_to_phys(t->kstack_top - THREAD_STACK_SIZE));
    if (t->tsp) {
        frame_free(virt_to_phys(t->tsp));
    }
    frame_free(t->addr_space);
    kfree(t);
}
```

- [ ] **Step 3: Unlink and reap `Exited` threads in the scan loop**

Replace the scan loop in `schedule()` (currently lines 30–35):

```c
    for (uint64_t i = 0; i < thread_count; i++) {
        if (next->state == Ready) {
            break;
        }
        next = next->next;
    }

    if (next->state != Ready) {
        spinlock_release_irqrestore(&sched_lock, flags);
        return;
    }
```

with a version that unlinks `Exited` nodes it encounters while searching for a `Ready` one, keeping `prev` correct across removals, and re-checking the find condition each iteration:

```c
    struct tcb *prev = current_tcb;
    struct tcb *next = prev ? prev->next : thread_list;
    struct tcb *ready = NULL;

    // Bounded by the pre-reap count: the circular walk must terminate when
    // no Ready thread exists. Only removals happen in the loop, never adds.
    uint64_t bounds = thread_count;
    for (uint64_t i = 0; i < bounds && ready == NULL && next != NULL; i++) {
        if (next->state == Exited) {
            struct tcb *victim = next;
            if (prev) {
                prev->next = next->next;
            } else {
                thread_list = next->next;
            }
            next = next->next;
            if (next == victim) {
                next = NULL;
            }
            thread_count--;
            reap_thread(victim);
            continue;
        }
        if (next->state == Ready) {
            ready = next;
            break;
        }
        prev = next;
        next = next->next;
    }

    if (!ready) {
        spinlock_release_irqrestore(&sched_lock, flags);
        return;
    }
```

Note on the loop: each non-Ready, non-Exited iteration advances `prev`/`next`
to a distinct node; each Exited iteration removes a node and advances. Because
the walk never revisits a node and the count only shrinks, `bounds` iterations
are sufficient to visit every distinct node on a circular list that starts with
at most `bounds` nodes. The `next == NULL` guard handles the degenerate case
where the final node was reaped (empty list).

- [ ] **Step 4: Use `ready` as the switch target**

Update the section after the scan that sets up the switch so it uses `ready` instead of the now-removed `next` variable. Replace:

```c
    next->state = Running;
    if (prev && prev->state == Running) {
        prev->state = Ready;
    }

    spinlock_release(&sched_lock);
    switch_task(next);
    restore_irq(flags);
```

with:

```c
    ready->state = Running;
    if (prev && prev->state == Running) {
        prev->state = Ready;
    }

    spinlock_release(&sched_lock);
    switch_task(ready);
    restore_irq(flags);
```

Note: the `prev` variable at this point points at the node preceding `ready` (or `NULL`/`current_tcb`), which is correct for the Running→Ready demotion. Keep the existing `struct tcb *prev = current_tcb;` initialization from Step 3's replacement so `prev` is defined here.

- [ ] **Step 5: Build to verify it compiles**

Run: `make -C kernel`
Expected: The kernel links cleanly with no errors or new warnings.

- [ ] **Step 6: Commit**

```bash
git add kernel/src/multitasking/sched.c
git commit -m "feat: reap exited threads in scheduler scan"
```

---

### Task 3: Runtime smoke test with short-lived threads

**Files:**
- Modify: `kernel/src/main.c`

**Interfaces:**
- Consumes: `thread_create(void *entry, void *ustack)` from `thread.h`, `thread_exit(void)` from `thread.h`, `print()` from `logging/print.h` (signature: `void print(const char *fmt, ...)`).
- Produces: (no new public API) A temporary runtime proof that reaping works, to be removed after the smoke test passes.

- [ ] **Step 1: Add temporary smoke-test threads in `main.c`**

Add a static smoke helper before `kmain` and a call inside `kmain`, after `init()` and the `sti` (after line 74), spawning two short-lived threads on kernel stacks:

```c
static void smoke_thread_a(void) {
    volatile int i = 0;
    for (int k = 0; k < 100000; k++) {
        i += k;
    }
    print("thread A exiting\n");
    thread_exit();
}

static void smoke_thread_b(void) {
    volatile int j = 0;
    for (int k = 0; k < 100000; k++) {
        j += k;
    }
    print("thread B exiting\n");
    thread_exit();
}
```

And inside `kmain`, after the `sti`:

```c
    thread_create(smoke_thread_a, 0);
    thread_create(smoke_thread_b, 0);
```

- [ ] **Step 2: Build**

Run: `make -C kernel`
Expected: Clean build into `kernel/bin-x86_64/kernel`.

- [ ] **Step 3: Run in QEMU and observe reaping**

Run (from repo root): `make run-x86_64`
Expected: The smoke threads run, print `thread A exiting` and `thread B exiting`, and never re-run (no repeated `exiting` prints). The system keeps idling without triple-faulting. If a thread were not reaped, its `Exited` state (now treated as non-runnable) would still be skipped — the observable proof is that the scheduler continues running the remaining threads / idle loop without crashing after both exit.

- [ ] **Step 4: Remove the temporary smoke test**

Delete `smoke_thread_a`, `smoke_thread_b`, and the two `thread_create` calls added in Step 1, restoring `main.c` to its pre-test state (only `init()` and the `hlt` loop remain).

- [ ] **Step 5: Final build check**

Run: `make -C kernel`
Expected: Clean build with the smoke test removed.

- [ ] **Step 6: Commit**

```bash
git add kernel/src/main.c
git commit -m "test: temporary smoke test for thread reaping"
```

---

## Self-Review Notes

- **Spec coverage:** `thread_exit()` (Task 1), reaping in `schedule()` scan + full cleanup of kernel stack / user stack / address space / TCB (Task 2), and runtime verification (Task 3) map 1:1 to the spec's Design and Testing sections. The safety rules (never reap the running or next-Ready thread; update `prev`/`next` on unlink) are covered by the Step 3 loop implementation.
- **Edge cases handled:** empty/decayed list (`ready == NULL` returns under lock), last-thread exited, NULL `tsp`, `next == victim` circular wrap guard.
- **Type consistency:** `THREAD_STACK_SIZE` is used consistently as the stack size in the reaper; `STACK_SIZE` (4096) in `thread.c` and `thread_exit`/`thread_create` remain unchanged. `thread_exit` is declared in `thread.h` and defined in `thread.c`. `ready` replaces `next` consistently in Task 2 Steps 3–4. `print` is used with the exact signature from `logging/print.h`.
