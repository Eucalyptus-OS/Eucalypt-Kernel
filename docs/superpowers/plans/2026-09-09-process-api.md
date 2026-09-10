# Process API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the process subsystem in `kernel/src/multitasking/proc.c` with a custom (non-POSIX) lifecycle API: create, find, add/remove thread, exit, wait, kill, reap, destroy, fork, exec (with an inline ELF64 loader).

**Architecture:** The PCB embeds a `struct vmm_space` so the existing VMM region tracking owns address-space cleanup. Threads are reaped safely inside `schedule()`'s scan. `proc_fork` uses a new `fork_call` asm entry that snapshots the parent's context frame (same layout `switch_task` restores) and copies it onto the child's fresh kernel stack with rax forced to 0, so parent and child both resume "after the fork call". `proc_exec` parses an in-memory ELF64 and maps its `PT_LOAD` segments into the process's existing space.

**Tech Stack:** C (gnu11, freestanding, `-ffreestanding`), NASM (x86_64), Limine bootloader, Make. No libc. Verification is a clean kernel build plus a QEMU runtime smoke test (no unit-test framework available).

## Global Constraints

- Kernel is freestanding: no libc, no unit test framework. Verify with `make -C kernel` (from repo root) and `make run-x86_64` (QEMU with `-device isa-debugcon` routing port 0xE9 to stdout).
- Build flags include `-Wall -Wextra`; each task must leave the build with **no new warnings**.
- Kernel convention: **no decorative comments**. Only add a comment where a safety invariant is non-obvious.
- `struct tcb` is `__attribute__((packed))`; `struct pcb` is **not** packed. The TCB layout is mirrored in `switch.asm` via `struc tcb` (reads offsets 0–57). Appending new TCB fields at the **end** is safe; do not move existing fields. The PCB embeds a `struct vmm_space` that vmm functions take by address (`&p->space`), and the PCB has no asm mirror, so packing it would trigger `-Waddress-of-packed-member`; pack only the tcb.
- Never take the address of a packed struct member (triggers `-Waddress-of-packed-member`). Use previous-pointer walks + struct-member assignment instead.
- Per-thread kernel stacks are 4096 bytes allocated at `thread_create` time via `frame_alloc()` + `phys_to_virt()`. TCB field `kstack_top` = top of that page. Any reaper must free the stack as a physical frame: `frame_free(virt_to_phys(t->kstack_top - 4096))`, never `kfree`.
- `threads` list: threads live in ONE global **circular** linked list (`thread_list`, `tcb->next`) plus a per-process singly-linked list (`tcb->pthread_next`). `next` is owned by the scheduler; `pthread_next` is owned by the process.
- `vmm_map_at()` maps at a page-aligned base, allocates frames internally, and records the mapping in `space->regions` so `vmm_destroy_space()` recovers it.
- Hierarchy semantics: a process becomes `zombie=1` with `exit_code` set before its parent detaches/reaps it. Threads marked `Dead` while their parent PCB may be freed must have `t->parent = NULL` first so the scheduler reaper never dereferences the freed PCB.

---

### Task 1: Per-process thread linkage and FIFO thread insertion

Adds `pthread_next` to the TCB, links each new thread into its parent PCB's thread list, increments `t_count`, and changes `thread_create` to append threads to the global circular list in creation order (tail insertion).

**Files:**
- Modify: `kernel/include/multitasking/thread.h`
- Modify: `kernel/src/multitasking/thread.c`

**Interfaces:**
- Consumes: existing `struct tcb`, `struct pcb`, `thread_list`, `thread_count`.
- Produces: `tcb->pthread_next` (per-process thread link), `t_count` maintained by `thread_create`, FIFO insertion into `thread_list`. Later tasks rely on `p->threads` and `p->t_count` being accurate.

- [ ] **Step 1: Append `pthread_next` to the TCB**

In `kernel/include/multitasking/thread.h`, after the `uint8_t timed;` line inside `struct tcb`, add:

```c
    uint8_t timed;
    struct tcb *pthread_next;
```

The asm `struc tcb` in `switch.asm` reads only offsets 0–57, so appending at offset 58+ is safe. No asm change.

- [ ] **Step 2: Link threads into the parent process in `thread_create`**

In `kernel/src/multitasking/thread.c`, `thread_create` currently builds the save frame, then ends with:

```c
    t->tid = thread_count++;
    t->ksp = sp;
    t->kstack_top = kstack + STACK_SIZE;
    t->tsp = ustack;
    t->state = Ready;
    t->addr_space = p->cr3;
    t->parent = p;
    if (thread_list == NULL) {
        thread_list = t;
        t->next = t;
    } else {
        t->next = thread_list->next;
        thread_list->next = t;
    }
    return t;
```

Zero the TCB first (fields `wake_tick`/`timed`/`pthread_next` were previously uninitialized) and replace the tail block with per-process linkage + FIFO insertion:

```c
    memset(t, 0, sizeof(struct tcb));

    t->tid = thread_count++;
    t->ksp = sp;
    t->kstack_top = kstack + STACK_SIZE;
    t->tsp = ustack;
    t->state = Ready;
    t->addr_space = p->cr3;
    t->parent = p;

    if (p) {
        if (!p->threads) {
            p->threads = t;
        } else {
            struct tcb *pt = p->threads;
            while (pt->pthread_next) {
                pt = pt->pthread_next;
            }
            pt->pthread_next = t;
        }
        p->t_count++;
    }

    if (thread_list == NULL) {
        thread_list = t;
        t->next = t;
    } else {
        struct tcb *tail = thread_list;
        while (tail->next != thread_list) {
            tail = tail->next;
        }
        tail->next = t;
        t->next = thread_list;
    }
    return t;
```

Move the `memset(t, 0, ...)` line to the top right after `kmalloc` succeeds, **before** the `frame_alloc` call (so every field is zeroed regardless of later failure paths). Add `#include <memory.h>` to `thread.c` for `memset`.

Note: `t->addr_space = p->cr3;` is still correct here; `p->cr3` changes to `p->space.pml4` in Task 3.

- [ ] **Step 3: Build to verify it compiles**

Run: `make -C kernel`
Expected: clean link into `kernel/bin-x86_64/kernel`, no new warnings.

- [ ] **Step 4: Commit**

```bash
git add kernel/include/multitasking/thread.h kernel/src/multitasking/thread.c
git commit -m "feat: per-process thread linkage and FIFO thread insertion"
```

---

### Task 2: Safe thread reaping in the scheduler scan

Replaces the buggy `reap_dead()` (which `kfree`'d a virtual stack address and never unlinked nodes) with an integrated reaper inside `schedule()`'s scan that unlinks `Dead` threads from the circular list, unhooks them from their parent's per-process list, frees the stack frame, and `kfree`'s the TCB.

**Files:**
- Modify: `kernel/src/multitasking/sched.c`

**Interfaces:**
- Consumes: `thread_list`, `thread_count`, `tcb` fields (`next`, `pthread_next`, `parent`, `state`, `kstack_top`, `t_count`), `frame_free()` (mm/frame.h), `virt_to_phys()` (mm/hhdm.h), `kfree()` (mm/heap.h).
- Produces: `Dead` threads are fully reclaimed inside `schedule()` (never the thread currently executing, which lives at `current_tcb` and is not visited by the scan).

- [ ] **Step 1: Add the frame/heap/hhdm includes**

Add to the includes at the top of `kernel/src/multitasking/sched.c`:

```c
#include <mm/frame.h>
#include <mm/hhdm.h>
```

- [ ] **Step 2: Add `reap_thread` and remove `reap_dead`**

Delete the existing `void reap_dead()` function entirely (including its `print` callbacks). Add before `schedule()`:

```c
#define THREAD_STACK_SIZE 4096

static void reap_thread(struct tcb *t) {
    if (t->parent) {
        struct tcb *pc = NULL;
        struct tcb *pt = t->parent->threads;
        while (pt && pt != t) {
            pc = pt;
            pt = pt->pthread_next;
        }
        if (pt == t) {
            if (pc) {
                pc->pthread_next = t->pthread_next;
            } else {
                t->parent->threads = t->pthread_next;
            }
            if (t->parent->t_count) {
                t->parent->t_count--;
            }
        }
    }
    frame_free(virt_to_phys(t->kstack_top - THREAD_STACK_SIZE));
    kfree(t);
}
```

- [ ] **Step 3: Integrate the reaper into `schedule()`'s scan**

Replace the body of `schedule()` (from the `struct tcb *prev = current_tcb;` line through the `switch_task(next);` line) with:

```c
    struct tcb *prev = current_tcb;
    struct tcb *next = prev ? prev->next : thread_list;
    struct tcb *ready = NULL;

    // Bounded by the pre-reap count: strips Dead nodes while hunting for a
    // Ready one. Removals only shrink the circular list, so bounds iterations
    // visit every distinct node. The scan never touches current_tcb itself
    // (the `next != current_tcb` guard), so an exiting thread's stack is
    // always safe until it has actually switched away.
    uint64_t bounds = thread_count;
    for (uint64_t i = 0; i < bounds && ready == NULL && next != NULL; i++) {
        if (next->state == Dead && next != current_tcb) {
            struct tcb *victim = next;
            if (prev) {
                prev->next = next->next;
            } else {
                thread_list = next->next;
            }
            if (next->next == next) {
                thread_list = NULL;
                next = NULL;
            } else {
                next = next->next;
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

    ready->state = Running;
    if (prev && prev->state == Running) {
        prev->state = Ready;
    }

    spinlock_release(&sched_lock);
    switch_task(ready);
    restore_irq(flags);
```

Make sure the include list has `<stdint.h>` and `<multitasking/thread.h>` (already present).

- [ ] **Step 4: Build to verify it compiles**

Run: `make -C kernel`
Expected: clean link, no new warnings.

- [ ] **Step 5: Commit**

```bash
git add kernel/src/multitasking/sched.c
git commit -m "feat: safely reap dead threads in the scheduler scan"
```

---

### Task 3: New PCB (embedded VMM space) and the create/find/thread-management API

Restructures `struct pcb` to embed a `vmm_space`, rewrites `proc_create`, and adds `proc_find`, `proc_add_thread`, `proc_remove_thread`.

**Files:**
- Modify: `kernel/include/multitasking/proc.h` (rewrite)
- Modify: `kernel/src/multitasking/proc.c` (rewrite)
- Modify: `kernel/src/multitasking/thread.c:1` (one line: `p->cr3` -> `p->space.pml4`)

**Interfaces:**
- Consumes: `vmm_create_space_into`, `vmm_destroy_space` (mm/vmm.h), `thread_create` (thread.h), `kmalloc`/`kfree` (mm/heap.h).
- Produces:
  - `struct pcb` with `pid`, `space` (vmm_space), `t_count`, `threads`, `parent`, `children`, `sibling_next`, `waiter`, `exit_code`, `zombie`, `next`.
  - `struct pcb *proc_create(void *entry, void *stack);`
  - `struct pcb *proc_find(uint64_t pid);`
  - `struct pcb *proc_add_thread(struct pcb *p, void *entry, void *stack);`
  - `void proc_remove_thread(struct pcb *p, struct tcb *t);`
  - `static uint64_t next_pid;` (private PID allocator).

- [ ] **Step 1: Rewrite `proc.h`**

Replace `kernel/include/multitasking/proc.h` with:

```c
#pragma once

#include <multitasking/thread.h>
#include <mm/vmm.h>
#include <stdint.h>

struct pcb {
    uint64_t pid;
    struct vmm_space space;
    uint64_t t_count;
    struct tcb *threads;
    struct pcb *parent;
    struct pcb *children;
    struct pcb *sibling_next;
    struct tcb *waiter;
    int exit_code;
    uint8_t zombie;
    struct pcb *next;
};

struct pcb *proc_create(void *entry, void *stack);
struct pcb *proc_find(uint64_t pid);
struct pcb *proc_add_thread(struct pcb *p, void *entry, void *stack);
void proc_remove_thread(struct pcb *p, struct tcb *t);
int proc_fork(void);
int proc_exec(void *elf, uintptr_t size, void *stack);
void proc_exit(int code);
struct pcb *proc_wait(struct pcb *p);
struct pcb *proc_kill(uint64_t pid);
void proc_reap(struct pcb *zombie);
void proc_destroy(struct pcb *p);
```

(The declarations for `proc_fork`/`proc_exec`/`proc_exit`/`proc_wait`/`proc_kill`/`proc_reap`/`proc_destroy` are used by later tasks; unlinked externs do not fail the build.)

- [ ] **Step 2: Rewrite `proc.c` foundation + create/find/add/remove**

Replace `kernel/src/multitasking/proc.c` with:

```c
#include <multitasking/proc.h>
#include <multitasking/thread.h>
#include <multitasking/sched.h>
#include <mm/paging.h>
#include <mm/heap.h>
#include <mm/vmm.h>
#include <memory.h>
#include <stddef.h>
#include <stdint.h>

struct pcb *proc_list = NULL;

static uint64_t next_pid = 0;

struct pcb *proc_create(void *entry, void *stack) {
    struct pcb *proc = (struct pcb *)kmalloc(sizeof(struct pcb));
    if (!proc) {
        return NULL;
    }
    memset(proc, 0, sizeof(struct pcb));

    struct tcb *cur = get_current_thread();
    struct pcb *caller = cur ? cur->parent : NULL;

    proc->pid = ++next_pid;
    if (vmm_create_space_into(&proc->space)) {
        kfree(proc);
        return NULL;
    }

    struct tcb *t = thread_create(entry, stack, proc);
    if (!t) {
        vmm_destroy_space(&proc->space);
        kfree(proc);
        return NULL;
    }

    proc->parent = caller;
    if (caller) {
        proc->sibling_next = caller->children;
        caller->children = proc;
    }

    if (!proc_list) {
        proc_list = proc;
    } else {
        struct pcb *curr = proc_list;
        while (curr->next) {
            curr = curr->next;
        }
        curr->next = proc;
    }

    return proc;
}

struct pcb *proc_find(uint64_t pid) {
    for (struct pcb *p = proc_list; p; p = p->next) {
        if (p->pid == pid) {
            return p;
        }
    }
    return NULL;
}

struct pcb *proc_add_thread(struct pcb *p, void *entry, void *stack) {
    if (!p || !entry) {
        return NULL;
    }
    if (!thread_create(entry, stack, p)) {
        return NULL;
    }
    return p;
}

void proc_remove_thread(struct pcb *p, struct tcb *t) {
    if (!p || !t) {
        return;
    }
    t->state = Dead;
    if (p->t_count) {
        p->t_count--;
    }
}
```

`proc_create` links the new process into the calling process's `children` list so `proc_wait` can find it. The first process (spawned before any thread exists) has `get_current_thread()` NULL, so its `parent` stays NULL.

Note: `next_pid` and the includes are intentionally minimal here; later tasks append additional includes (`mm/frame.h`, `mm/hhdm.h`, `multitasking/elf.h`) as they add code.

- [ ] **Step 3: Update `thread.c` for the PCB change**

In `kernel/src/multitasking/thread.c`, change the address-space assignment line:

```c
    t->addr_space = p->cr3;
```

to:

```c
    t->addr_space = (uintptr_t)p->space.pml4;
```

- [ ] **Step 4: Build to verify it compiles**

Run: `make -C kernel`
Expected: clean link, no new warnings.

- [ ] **Step 5: Commit**

```bash
git add kernel/include/multitasking/proc.h kernel/src/multitasking/proc.c kernel/src/multitasking/thread.c
git commit -m "feat: embed vmm_space in PCB, add create/find/thread mgmt"
```

---

### Task 4: `proc_exit` (zombie + waiter wake)

Implements process exit: mark all the process's threads dead, record the exit code, become a zombie, wake a blocked waiter, and schedule away (never returns).

**Files:**
- Modify: `kernel/src/multitasking/proc.c`

**Interfaces:**
- Consumes: `get_current_thread()`, `unblock()` (multitasking/sched.h), `schedule()` (multitasking/sched.h).
- Produces: `void proc_exit(int code)` — never returns on the success path.

- [ ] **Step 1: Implement `proc_exit`**

Append to `kernel/src/multitasking/proc.c`:

```c
void proc_exit(int code) {
    struct tcb *cur = get_current_thread();
    struct pcb *p = cur ? cur->parent : NULL;
    if (!p) {
        for (;;) {
            asm volatile ("cli; hlt");
        }
    }

    p->exit_code = code;
    struct tcb *t = p->threads;
    while (t) {
        struct tcb *nt = t->pthread_next;
        // parent is cleared so the reaper never touches the soon-freed PCB
        t->state = Dead;
        t->parent = NULL;
        t = nt;
    }
    p->zombie = 1;

    if (p->parent && p->parent->waiter) {
        unblock(p->parent->waiter);
        p->parent->waiter = NULL;
    }

    schedule();
    for (;;) {
        asm volatile ("hlt");
    }
}
```

- [ ] **Step 2: Build to verify it compiles**

Run: `make -C kernel`
Expected: clean link, no new warnings.

- [ ] **Step 3: Commit**

```bash
git add kernel/src/multitasking/proc.c
git commit -m "feat: proc_exit marks process zombie and wakes waiter"
```

---

### Task 5: `proc_wait`, `proc_kill`, `proc_reap`, `proc_destroy`

Adds the blocking wait, the forced kill of a child, and the two teardown paths.

**Files:**
- Modify: `kernel/src/multitasking/proc.c`

**Interfaces:**
- Consumes: `get_current_thread()`, `block_current()`, `unblock()` (multitasking/sched.h), `vmm_destroy_space()` (mm/vmm.h), `kfree()` (mm/heap.h).
- Produces:
  - `struct pcb *proc_wait(struct pcb *p)` — blocks until a child is zombie, detaches it, returns it (caller then reads `exit_code` and calls `proc_reap`).
  - `struct pcb *proc_kill(uint64_t pid)` — kills a child of the calling process, detaches it, returns it; NULL if no such child.
  - `void proc_reap(struct pcb *z)` — unlinks a zombie from the global list, destroys its space, frees the PCB. Refuses to reap the calling process.
  - `void proc_destroy(struct pcb *p)` — lethal teardown of another process (init/forced): dead-ify threads, then reap.

- [ ] **Step 1: Implement `proc_wait` and `proc_kill`**

Append to `kernel/src/multitasking/proc.c`:

```c
struct pcb *proc_wait(struct pcb *p) {
    if (!p) {
        return NULL;
    }
    for (;;) {
        struct pcb *c = p->children;
        struct pcb *pc = NULL;
        while (c) {
            if (c->zombie) {
                if (pc) {
                    pc->sibling_next = c->sibling_next;
                } else {
                    p->children = c->sibling_next;
                }
                c->sibling_next = NULL;
                c->parent = NULL;
                return c;
            }
            pc = c;
            c = c->sibling_next;
        }
        p->waiter = get_current_thread();
        block_current();
        p->waiter = NULL;
    }
}

struct pcb *proc_kill(uint64_t pid) {
    struct tcb *cur = get_current_thread();
    struct pcb *caller = cur ? cur->parent : NULL;
    if (!caller) {
        return NULL;
    }
    struct pcb *c = caller->children;
    struct pcb *pc = NULL;
    while (c) {
        if (c->pid == pid) {
            struct tcb *t = c->threads;
            while (t) {
                struct tcb *nt = t->pthread_next;
                t->state = Dead;
                t->parent = NULL;
                t = nt;
            }
            c->exit_code = 1;
            c->zombie = 1;
            if (pc) {
                pc->sibling_next = c->sibling_next;
            } else {
                caller->children = c->sibling_next;
            }
            c->sibling_next = NULL;
            c->parent = NULL;
            return c;
        }
        pc = c;
        c = c->sibling_next;
    }
    return NULL;
}
```

- [ ] **Step 2: Implement `proc_reap` and `proc_destroy`**

Append to `kernel/src/multitasking/proc.c`:

```c
void proc_reap(struct pcb *z) {
    if (!z) {
        return;
    }
    struct tcb *cur = get_current_thread();
    if (cur && cur->parent == z) {
        return;
    }

    if (proc_list == z) {
        proc_list = z->next;
    } else {
        struct pcb *p = proc_list;
        while (p && p->next != z) {
            p = p->next;
        }
        if (p) {
            p->next = z->next;
        }
    }

    z->next = NULL;
    vmm_destroy_space(&z->space);
    kfree(z);
}

void proc_destroy(struct pcb *p) {
    if (!p) {
        return;
    }
    struct tcb *t = p->threads;
    while (t) {
        struct tcb *nt = t->pthread_next;
        t->state = Dead;
        t->parent = NULL;
        t = nt;
    }
    p->zombie = 1;
    proc_reap(p);
}
```

- [ ] **Step 3: Build to verify it compiles**

Run: `make -C kernel`
Expected: clean link, no new warnings.

- [ ] **Step 4: Commit**

```bash
git add kernel/src/multitasking/proc.c
git commit -m "feat: proc_wait/proc_kill/proc_reap/proc_destroy teardown"
```

---

### Task 6: `proc_fork` via context-frame snapshot

Adds the `fork_call` asm entry (snapshots the current context in `switch_task` layout) and `proc_fork_c`, which builds the child: fresh address space, full frame copy of the parent's low-half regions, a fresh kernel stack containing a verbatim copy of the parent's captured stack slice with the `rax` slot zeroed, and a resume point at the parent's fork call site. The child first runs with rax = 0; the parent returns the child PID.

**Files:**
- Modify: `kernel/src/multitasking/switch.asm`
- Modify: `kernel/src/multitasking/proc.c`

**Interfaces:**
- Consumes: `struct tcb` fields (`ksp`, `kstack_top`, `tsp`, `addr_space`, `next`, `parent`, `pthread_next`), `thread_list`, `thread_count`, `get_current_thread()`, `vmm_create_space_into`, `vmm_map_at`, `walk_phys` (new static helper), `frame_alloc`/`frame_free`, `kmalloc`/`kfree`, `next_pid`.
- Produces:
  - `int fork_call(void)` — asm entry; returns 0 in the child, child PID in the parent, -1 on failure.
  - `int proc_fork(void)` — C wrapper: `return fork_call();`.
  - `int proc_fork_c(void *frame, uint64_t resume_rip)` — C worker (extern, so the asm can call it).

Context-frame layout (mirrors `switch_task` restore): `frame[0]`=r15 ... `frame[13]`=rbx, `frame[14]`=rax, `frame[15]`=rflags, `frame[16]`=resume_rip (the return address of the `call fork_call`). `proc_fork_c` receives `frame` = address of slot 0 and `resume_rip` = the value of slot 16.

- [ ] **Step 1: Add `fork_call` to `switch.asm`**

In `kernel/src/multitasking/switch.asm`, next to the `extern tss_set_kernel_stack` line add `extern proc_fork_c`, and after `switch_task` (or anywhere at top level) add:

```nasm
; int fork_call(void) - snapshots the caller's context and asks proc_fork_c
; to clone this process. The parent resumes here normally with the child PID
; in rax; the child is first scheduled by switch_task restoring a copy of this
; same frame with rax = 0, so it resumes just after the `call fork_call`.
global fork_call
fork_call:
    pushfq
    push rax
    push rbx
    push rcx
    push rdx
    push rdi
    push rsi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; rsp = r15 slot (frame[0]); rax slot is [rsp+112], rflags [rsp+120],
    ; resume_rip (the call's return address) is [rsp+128].
    mov rdi, rsp
    mov rsi, [rsp + 128]
    call proc_fork_c

    ; discard the 15 saved GPRs + rflags (128 bytes); ret pops resume_rip.
    add rsp, 128
    ret
```

- [ ] **Step 2: Add `walk_phys` and a local stack-size constant to `proc.c`**

First, extend the include block at the top of `kernel/src/multitasking/proc.c` (needed for `frame_alloc`/`frame_free`, `phys_to_virt`, `memcpy`, and the list macros):

```c
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <lib/list.h>
```

Then append to `kernel/src/multitasking/proc.c`:

```c
#define STACK_SIZE 4096
#define PAGE_PHYS_MASK 0x000FFFFFFFFFF000ULL

static uintptr_t walk_phys(uint64_t *pml4, void *vaddr) {
    uint64_t idx = ((uint64_t)vaddr >> 39) & 0x1FF;
    uint64_t *pml3 = (uint64_t *)phys_to_virt(pml4[idx] & PAGE_PHYS_MASK);
    idx = ((uint64_t)vaddr >> 30) & 0x1FF;
    uint64_t *pml2 = (uint64_t *)phys_to_virt(pml3[idx] & PAGE_PHYS_MASK);
    idx = ((uint64_t)vaddr >> 21) & 0x1FF;
    uint64_t *pml1 = (uint64_t *)phys_to_virt(pml2[idx] & PAGE_PHYS_MASK);
    idx = ((uint64_t)vaddr >> 12) & 0x1FF;
    if (!(pml1[idx] & PAGE_PRESENT)) {
        return 0;
    }
    return pml1[idx] & PAGE_PHYS_MASK;
}

int proc_fork(void) {
    return fork_call();
}
```

- [ ] **Step 3: Implement `proc_fork_c`**

Append to `kernel/src/multitasking/proc.c` (this is the non-static symbol `switch.asm` calls):

```c
int proc_fork_c(void *frame, uint64_t resume_rip) {
    (void)resume_rip;
    struct tcb *cur = get_current_thread();
    if (!cur || !cur->parent) {
        return -1;
    }
    struct pcb *parent = cur->parent;

    uint64_t frame_addr = (uint64_t)frame;
    uint64_t stack_base = (uint64_t)cur->kstack_top - STACK_SIZE;
    if (frame_addr < stack_base || frame_addr >= (uint64_t)cur->kstack_top) {
        return -1;
    }

    struct pcb *child = (struct pcb *)kmalloc(sizeof(struct pcb));
    if (!child) {
        return -1;
    }
    memset(child, 0, sizeof(struct pcb));
    if (vmm_create_space_into(&child->space)) {
        kfree(child);
        return -1;
    }
    child->pid = ++next_pid;

    uint64_t *cpml4 = phys_to_virt((uintptr_t)child->space.pml4);
    uint64_t *ppml4 = phys_to_virt((uintptr_t)parent->space.pml4);
    list_foreach(&parent->space.regions, n) {
        struct vm_region *r = container_of(n, struct vm_region, link);
        int npages = (r->end - r->base) / PAGE_SIZE;
        if (!vmm_map_at(&child->space, (void *)r->base, r->flags, npages)) {
            vmm_destroy_space(&child->space);
            kfree(child);
            return -1;
        }
        for (int i = 0; i < npages; i++) {
            void *va = (void *)(r->base + (uint64_t)i * PAGE_SIZE);
            uintptr_t cp = walk_phys(cpml4, va);
            uintptr_t pp = walk_phys(ppml4, va);
            if (cp && pp) {
                memcpy(phys_to_virt(cp), phys_to_virt(pp), PAGE_SIZE);
            }
        }
    }

    uint64_t slice_len = (uint64_t)cur->kstack_top - frame_addr;
    uint64_t child_off = frame_addr - stack_base;

    uintptr_t child_kstack_phys = frame_alloc();
    if (!child_kstack_phys) {
        vmm_destroy_space(&child->space);
        kfree(child);
        return -1;
    }
    uint8_t *child_kstack = phys_to_virt(child_kstack_phys);
    memcpy(child_kstack + child_off, (void *)frame_addr, slice_len);

    uint64_t *child_frame = (uint64_t *)(child_kstack + child_off);
    child_frame[14] = 0;

    struct tcb *tc = (struct tcb *)kmalloc(sizeof(struct tcb));
    if (!tc) {
        frame_free(child_kstack_phys);
        vmm_destroy_space(&child->space);
        kfree(child);
        return -1;
    }
    memset(tc, 0, sizeof(struct tcb));
    tc->tid = thread_count++;
    tc->ksp = child_frame;
    tc->kstack_top = child_kstack + STACK_SIZE;
    tc->tsp = cur->tsp;
    tc->addr_space = (uintptr_t)child->space.pml4;
    tc->state = Ready;
    tc->parent = child;

    if (thread_list == NULL) {
        thread_list = tc;
        tc->next = tc;
    } else {
        struct tcb *tail = thread_list;
        while (tail->next != thread_list) {
            tail = tail->next;
        }
        tail->next = tc;
        tc->next = thread_list;
    }

    child->threads = tc;
    child->t_count = 1;

    child->parent = parent;
    child->sibling_next = parent->children;
    parent->children = child;

    if (!proc_list) {
        proc_list = child;
    } else {
        struct pcb *pl = proc_list;
        while (pl->next) {
            pl = pl->next;
        }
        pl->next = child;
    }

    return (int)child->pid;
}
```

`child_frame[14]` is the `rax` slot (offset 112 bytes from the r15 slot). The parent's region copies rely on the kernel's low-half identity mapping, which exists today because `vmm_create_space_into` copies the whole kernel root — as soon as private user mappings land, this region-walk code must be revisited.

- [ ] **Step 4: Build to verify it compiles**

Run: `make -C kernel`
Expected: clean link into `kernel/bin-x86_64/kernel`, no new warnings.

- [ ] **Step 5: Commit**

```bash
git add kernel/src/multitasking/switch.asm kernel/src/multitasking/proc.c
git commit -m "feat: proc_fork with context-frame snapshot"
```

---

### Task 7: `proc_exec` with an inline ELF64 loader

Adds a minimal ELF64 header (`kernel/include/multitasking/elf.h`), an `elf_load` that maps `PT_LOAD` segments into the process's existing space (rollback on failure), and `proc_exec`, which swaps the process image: map new segments, create one fresh thread at the ELF entry, free the old regions, dead-ify the current thread, and schedule to the new thread.

**Files:**
- Create: `kernel/include/multitasking/elf.h`
- Modify: `kernel/src/multitasking/proc.c`

**Interfaces:**
- Consumes: `vmm_map_at`, `vmm_find_region`, `vmm_free_region`, `vmm_destroy_space` (mm/vmm.h), `PAGE_SIZE`, `PAGE_WRITABLE`, `PAGE_NXE` (mm/paging.h), `thread_create` (thread.h), `print` (logging/print.h, not used here), `get_current_thread`, `schedule`.
- Produces:
  - `struct elf64_hdr`, `struct elf64_phdr`, constants `PT_LOAD`, `PF_X/R/W`, `ET_EXEC`, `ELFCLASS64` (in elf.h, used by Task 8's test too).
  - `int proc_exec(void *elf, uintptr_t size, void *stack)` — -1 on validation/allocation failure (process untouched), never returns on success.
  - `static int elf_load(struct pcb *p, void *elf, uintptr_t size, void **entry)`.

- [ ] **Step 1: Create `elf.h`**

Create `kernel/include/multitasking/elf.h`:

```c
#pragma once

#include <stdint.h>

#define ELFCLASS64 2
#define ET_EXEC 2
#define PT_LOAD 1

#define PF_X 1
#define PF_W 2
#define PF_R 4

struct elf64_hdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));
```

- [ ] **Step 2: Implement `elf_load`**

Add `#include <multitasking/elf.h>` to the include block at the top of `kernel/src/multitasking/proc.c`, then append to the file:

```c
static int elf_load(struct pcb *p, void *elf, uintptr_t size, void **entry) {
    struct elf64_hdr *h = (struct elf64_hdr *)elf;
    if (size < sizeof(struct elf64_hdr)) {
        return -1;
    }
    if (h->e_ident[0] != 0x7F || h->e_ident[1] != 'E' ||
        h->e_ident[2] != 'L' || h->e_ident[3] != 'F') {
        return -1;
    }
    if (h->e_ident[4] != ELFCLASS64 || h->e_type != ET_EXEC) {
        return -1;
    }
    if ((uint64_t)h->e_phoff + (uint64_t)h->e_phnum * h->e_phentsize > size) {
        return -1;
    }

    struct vm_region *mapped[64];
    int nmap = 0;

    for (uint64_t i = 0; i < h->e_phnum; i++) {
        struct elf64_phdr *ph = (struct elf64_phdr *)((uint8_t *)elf + h->e_phoff + i * h->e_phentsize);
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (nmap >= 64 || (uint64_t)ph->p_offset + ph->p_filesz > size) {
            goto fail;
        }

        uint64_t base = ph->p_vaddr & ~(PAGE_SIZE - 1);
        uint64_t end_va = ph->p_vaddr + ph->p_memsz;
        uint64_t end_aligned = (end_va + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        int pages = (end_aligned - base) / PAGE_SIZE;

        uint64_t flags = 0;
        if (ph->p_flags & PF_W) {
            flags |= PAGE_WRITABLE;
        }
        if (!(ph->p_flags & PF_X)) {
            flags |= PAGE_NXE;
        }

        if (vmm_find_region(&p->space, base)) {
            continue;
        }
        if (!vmm_map_at(&p->space, (void *)base, flags, pages)) {
            goto fail;
        }
        mapped[nmap++] = vmm_find_region(&p->space, base);
        memcpy((void *)ph->p_vaddr, (uint8_t *)elf + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz) {
            memset((void *)((uint8_t *)ph->p_vaddr + ph->p_filesz), 0,
                   ph->p_memsz - ph->p_filesz);
        }
    }

    *entry = (void *)h->e_entry;
    return 0;

fail:
    for (int k = 0; k < nmap; k++) {
        vmm_free_region(&p->space, mapped[k]);
    }
    return -1;
}
```

- [ ] **Step 3: Implement `proc_exec`**

Append to `kernel/src/multitasking/proc.c`:

```c
int proc_exec(void *elf, uintptr_t size, void *stack) {
    struct tcb *cur = get_current_thread();
    if (!cur || !cur->parent) {
        return -1;
    }
    struct pcb *p = cur->parent;

    uint64_t old_regions = p->space.regions.count;
    void *entry = NULL;
    if (elf_load(p, elf, size, &entry)) {
        return -1;
    }

    struct tcb *nt = thread_create(entry, stack, p);
    if (!nt) {
        return -1;
    }
    (void)nt;

    for (uint64_t i = 0; i < old_regions; i++) {
        struct vm_region *r = container_of(p->space.regions.head,
                                           struct vm_region, link);
        vmm_free_region(&p->space, r);
    }

    cur->state = Dead;
    schedule();
    for (;;) {
        asm volatile ("hlt");
    }
}
```

Note: `old_regions` is captured **before** `elf_load`; new segments are mapped before the old regions are freed, so `elf_load` failure leaves the process untouched and the loop frees only the pre-existing regions (the new ones are appended after them in the region list).

- [ ] **Step 4: Build to verify it compiles**

Run: `make -C kernel`
Expected: clean link, no new warnings.

- [ ] **Step 5: Commit**

```bash
git add kernel/include/multitasking/elf.h kernel/src/multitasking/proc.c
git commit -m "feat: proc_exec with inline ELF64 loader"
```

---

### Task 8: Runtime smoke test harness

Adds a first process (`proc_tests`) spawned from `kmain` that exercises create/wait/exit, kill, fork, and exec in sequence, printing PASS/FAIL via `print()` on the isa-debugcon port. Runtime proof is run in QEMU.

**Files:**
- Modify: `kernel/src/main.c`

**Interfaces:**
- Consumes: `proc_create`, `proc_wait`, `proc_kill`, `proc_reap`, `proc_fork`, `proc_exec`, `proc_exit`, `proc_find` (multitasking/proc.h), `get_current_thread()` (multitasking/sched.h), `struct pcb` fields `exit_code`, `pid`, `parent` (multitasking/proc.h), `struct elf64_hdr`, `struct elf64_phdr`, `PT_LOAD`, `PF_R`, `PF_X`, `ELFCLASS64`, `ET_EXEC` (multitasking/elf.h), `print()` (logging/print.h), `memset`/`memcpy` (memory.h).
- Produces: Boot-time PASS/FAIL diagnostics.

- [ ] **Step 1: Add test functions to `main.c`**

Add the includes:

```c
#include <multitasking/proc.h>
#include <multitasking/elf.h>
#include <multitasking/sched.h>
#include <memory.h>
```

Add these functions before `kmain`:

```c
static void child_exit_42(void) {
    print("child: exiting with 42\n");
    proc_exit(42);
}

static void child_long_running(void) {
    volatile uint64_t sink = 0;
    for (;;) {
        sink++;
    }
}

static void test_create_wait_exit(void) {
    struct pcb *me = get_current_thread()->parent;
    struct pcb *child = proc_create(child_exit_42, 0);
    if (!child) {
        print("test create/wait/exit: FAIL (create)\n");
        return;
    }
    struct pcb *z = proc_wait(me);
    if (z && z->exit_code == 42) {
        print("test create/wait/exit: PASS\n");
    } else {
        print("test create/wait/exit: FAIL\n");
    }
    if (z) {
        proc_reap(z);
    }
}

static void test_kill(void) {
    struct pcb *me = get_current_thread()->parent;
    struct pcb *child = proc_create(child_long_running, 0);
    if (!child) {
        print("test kill: FAIL (create)\n");
        return;
    }
    struct pcb *z = proc_kill(child->pid);
    if (z && z->exit_code == 1) {
        print("test kill: PASS\n");
    } else {
        print("test kill: FAIL\n");
    }
    if (z) {
        proc_reap(z);
    }
}

static void test_fork(void) {
    int r = proc_fork();
    if (r == 0) {
        print("fork: in child\n");
        proc_exit(7);
    }
    if (r <= 0) {
        print("test fork: FAIL (return %d)\n", r);
        return;
    }
    struct pcb *me = get_current_thread()->parent;
    struct pcb *z = proc_wait(me);
    if (z && z->exit_code == 7) {
        print("test fork: PASS\n");
    } else {
        print("test fork: FAIL\n");
    }
    if (z) {
        proc_reap(z);
    }
}

// x86-64 code: mov $0xe9,%dx; mov $'X',%al; out %al,%dx; jmp .
static const uint8_t exec_code[] = { 0xBA, 0xE9, 0x00, 0xB0, 'X', 0xEE, 0xEB, 0xFD };

static void test_exec(void) {
    uint8_t blob[136] __attribute__((aligned(8)));
    memset(blob, 0, sizeof(blob));

    struct elf64_hdr *h = (struct elf64_hdr *)blob;
    h->e_ident[0] = 0x7F;
    h->e_ident[1] = 'E';
    h->e_ident[2] = 'L';
    h->e_ident[3] = 'F';
    h->e_ident[4] = ELFCLASS64;
    h->e_ident[5] = 1;
    h->e_type = ET_EXEC;
    h->e_machine = 62;
    h->e_version = 1;
    h->e_entry = 0x400000;
    h->e_phoff = 64;
    h->e_phentsize = sizeof(struct elf64_phdr);
    h->e_phnum = 1;

    struct elf64_phdr *ph = (struct elf64_phdr *)(blob + 64);
    ph->p_type = PT_LOAD;
    ph->p_flags = PF_R | PF_X;
    ph->p_offset = 120;
    ph->p_vaddr = 0x400000;
    ph->p_paddr = 0x400000;
    ph->p_filesz = sizeof(exec_code);
    ph->p_memsz = sizeof(exec_code);
    ph->p_align = 0x1000;

    memcpy(blob + 120, exec_code, sizeof(exec_code));

    int e = proc_exec(blob, sizeof(blob), 0);
    print("test exec: FAIL (proc_exec returned %d)\n", e);
}

static void proc_tests(void) {
    print("proc: running tests\n");
    test_create_wait_exit();
    test_kill();
    test_fork();
    test_exec();
    print("proc: tests done (exec FAIL means still here)\n");
}
```

- [ ] **Step 2: Spawn the test process from `kmain`**

In `kmain`, immediately after `asm volatile ("sti");`, add:

```c
    proc_create(proc_tests, 0);
```

- [ ] **Step 3: Build**

Run: `make -C kernel`
Expected: clean build into `kernel/bin-x86_64/kernel`, no new warnings.

- [ ] **Step 4: Run in QEMU and observe**

Run (from repo root): `make run-x86_64`

Expected: The ordering on stdout:
1. `proc: running tests`
2. `child: exiting with 42` then `test create/wait/exit: PASS`
3. `test kill: PASS`
4. `fork: in child` then `test fork: PASS` (order may interleave with the child's print)
5. one `X` byte then the kernel idles (exec succeeded; no `test exec: FAIL` line)

If `test create/wait/exit: FAIL` or `test fork: FAIL` or `test exec: FAIL` appears, or a triple-fault/panic happens, debug the failing path before proceeding.

- [ ] **Step 5: Commit**

```bash
git add kernel/src/main.c
git commit -m "test: smoke-test process API create/wait/exit/kill/fork/exec"
```

---

### Task 9: Remove the temporary harness and final build

Restores `main.c` to its pre-test state (matches repo convention from the thread-reaping plan) and does a final clean build.

**Files:**
- Modify: `kernel/src/main.c`

**Interfaces:**
- Consumes: nothing beyond Task 8.

- [ ] **Step 1: Remove the test code**

Delete from `main.c`: the `proc.h`/`elf.h`/`sched.h`/`memory.h` includes added in Task 8, all of `child_exit_42`, `child_long_running`, `test_create_wait_exit`, `test_kill`, `test_fork`, `test_exec`, `proc_tests`, and the `proc_create(proc_tests, 0);` line. Keep `#include <multitasking/thread.h>` which was previously present.

- [ ] **Step 2: Final build**

Run: `make -C kernel`
Expected: clean build into `kernel/bin-x86_64/kernel`.

- [ ] **Step 3: Commit**

```bash
git add kernel/src/main.c
git commit -m "test: remove temporary process API smoke test"
```

---

## Self-Review Notes

- **Spec coverage:** PCB/vmm_space + `waiter` (Task 3); `pthread_next` + FIFO insertion (Task 1); reaper fix (Task 2); `proc_create`/`proc_find`/`proc_add_thread`/`proc_remove_thread` (Task 3); `proc_exit` zombie + waiter wake (Task 4); `proc_wait`/`proc_kill`/`proc_reap`/`proc_destroy` (Task 5); `proc_fork` frame-copy (Task 6); `proc_exec` + ELF loader (Task 7); runtime verification (Task 8); harness cleanup (Task 9).
- **Type consistency:** `fork_call` returns int; `proc_fork_c(void*, uint64_t)` matches asm `rdi`/`rsi` passing and the `(int)child->pid` return. `proc_wait`/`proc_kill` return `struct pcb *`; `proc_reap`/`proc_destroy`/`proc_remove_thread` return void; `proc_add_thread` returns `struct pcb *`. `exit_code == 1` convention for kills, `exit_code == 42`/`7` verify wait. `walk_phys` returns physical frame or 0 (checked before use). `STACK_SIZE` (4096) is defined locally in `proc.c` and in `thread.c`; `THREAD_STACK_SIZE` (4096) lives in `sched.c`.
- **Edge cases:** reaper never frees `current_tcb` (scan starts at its `next`); single-node list removal sets `thread_list = NULL`; `proc_reap` refuses the calling process; `proc_kill`/`proc_exit` NULL the parents of dead threads so the reaper never dereferences a freed PCB; `elf_load` unwind frees only the segments it mapped (`mapped[]`); `proc_exec` frees old regions after new segments are mapped (untouched on load/alloc failure).