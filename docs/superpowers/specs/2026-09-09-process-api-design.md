# Process API Design

Date: 2026-09-09

## Overview

Complete the process subsystem in `kernel/src/multitasking/proc.c` with a full
process lifecycle API. The API is custom (not POSIX). Includes an ELF64 loader
for `proc_exec`, full-copy address spaces for `proc_fork`, and zombie-based
wait semantics.

## PCB Structure

Replace the raw `uintptr_t cr3` with an embedded `struct vmm_space` so the
existing VMM region tracking handles address space cleanup on exit.

```c
struct pcb {
    uint64_t pid;
    struct vmm_space space;     // replaces raw uintptr_t cr3
    uint64_t t_count;
    struct tcb *threads;        // linked list of threads
    struct pcb *parent;         // for wait/kill
    struct pcb *children;       // head of child list
    struct pcb *sibling_next;   // next sibling in parent's children list
    int exit_code;              // set by proc_exit, read by proc_wait
    uint8_t zombie;             // 1 = exited, waiting to be reaped
    struct pcb *next;           // global process list
} __attribute__((packed));
```

## Process API

```c
struct pcb *proc_create(void *entry, void *stack);
struct pcb *proc_find(uint64_t pid);
struct pcb *proc_add_thread(struct pcb *p, void *entry, void *stack);
struct pcb *proc_wait(struct pcb *p);          // blocks until a child exits, returns the zombie child
struct pcb *proc_kill(uint64_t pid);           // kills a child, returns the reaped pcb (or NULL)
int  proc_fork(void);                          // clones current process, returns 0 to child, child pid to parent
int  proc_exec(void *elf, uintptr_t size, void *stack);  // replaces current process image
void proc_exit(int code);                      // marks process zombie, never returns
void proc_reap(struct pcb *zombie);            // frees zombie resources
void proc_destroy(struct pcb *p);              // removes from list + frees all at once (for init/no-parent)
```

### Function Semantics

- **proc_create(entry, stack)** — Allocate PCB, create a fresh address space via
  `vmm_create_space_into`, create the first thread via `thread_create`. Append
  to the global `proc_list` (singly-linked). Properly maintain `t_count`.

- **proc_exit(code)** — Mark all remaining threads `Dead`, set `exit_code`,
  mark the process `zombie`, wake any blocked parent, then `schedule()`
  away immediately. Never returns.

- **proc_wait(p)** — Block until a child of `p` becomes a zombie, then detach it
  from the children list, reap it, and return it. Uses `block_current()`.
  Returns NULL if woken spuriously (no zombie child available).

- **proc_kill(pid)** — Look up a child of the calling process by PID. Mark its
  threads `Dead`, set `exit_code` to a non-zero signal-like value, mark it a
  zombie, wake its parent. Returns NULL if no matching child. Only callable on
  own children — there is no privilege check because the kernel has no
  users/permissions yet.

- **proc_fork()** — Clone the current process:
  1. Allocate a new PCB and `vmm_create_space_into` a fresh address space.
  2. Walk the parent's low-half mappings (from the VMM region list plus page
     table walk for any raw mappings), allocate + copy each frame into the
     child's space.
  3. Create one new thread for the child whose saved kernel context is a copy
     of the parent's most recent context, but altered so the child appears to
     "return 0" from the fork call site.
  4. Return 0 in the child (via the forked resume path), parent gets child PID.
  On any allocation failure: destroy the partial child space + PCB, return -1.

- **proc_exec(elf, size, stack)** — Parse an ELF64 file in memory:
  1. Validate magic, class, machine.
  2. Iterate program headers; map `PT_LOAD` segments into the process's existing
     space (low half only, honoring `p_flags`).
  3. Kill all existing threads, create one fresh thread with the new entry point
     and `stack`.
  On any failure: leave the process untouched, return -1.

- **proc_reap(zombie)** — Free the zombie: destroy its address space, free its
  PCB, remove from all lists.

- **proc_destroy(p)** — For processes with no parent to wait on them (e.g.,
  init). Remove from global list, kill threads, destroy space, free PCB.

- **proc_find(pid)** — Linear walk of `proc_list`, return matching PCB or NULL.

- **proc_add_thread(p, entry, stack)** — Create a new thread bound to `p`,
  increment `t_count`, link it into `p->threads` (and the scheduler's circular
  `thread_list` via `thread_create`).

### Helper Functions

Internal helpers in proc.c:
- `static void proc_free_children(struct pcb *p)` — reap all children.
- `static int copy_user_space(struct pcb *src, struct pcb *dst)` — low-half
  frame-by-frame copy for fork.
- `static int elf_load(struct pcb *p, void *elf, void *stack)` — ELF64 parser
  and segment mapper used by proc_exec.

## Thread/Scheduler Changes Required

### thread.c fix — circular list insertion order

`thread_create` currently inserts new TCBs at the list head (right after
`thread_list`). To preserve round-robin order, insert at the tail:

```c
if (thread_list == NULL) {
    thread_list = t;
    t->next = t;
} else {
    // append at tail: keep thread_list pointing at the oldest thread,
    // insert new thread just before it
    t->next = thread_list->next;
    thread_list->next = t;
}
```

### sched.c fix — reap_dead frame leak

`reap_dead` calls `kfree(t->kstack_top)` but the thread's kernel stack is a
physical frame mapped at `phys_to_virt(kstack_phys)`; `kstack_top` is the
virtual top. The stack frame must be returned to the frame allocator:

```c
frame_free(virt_to_phys(t->kstack_top) - 0x1000);
```

Explanation: `kstack_top = kstack + STACK_SIZE` and `kstack =
phys_to_virt(kstack_phys)`, so `virt_to_phys(kstack_top) - 0x1000` yields the
stack's physical frame. Both `kfree(t)` of the TCB and `frame_free` of the
stack frame apply.

### switch.asm changes — fork resume path

Add a context-endpoint for the forked child. The child's thread must "win" the
fork: the child thread's saved kernel stack contains a context whose return
address points into a small restore stub that performs iretq-like return to user
mode with RAX = 0. The parent's fork returns normally with RAX = child pid.

## Data Flow

1. **proc_create** → `vmm_create_space_into(&p->space)` → `thread_create(entry,
   stack, p)` → insert into `thread_list` + `proc_list`.
2. **proc_exit** → mark all threads Dead → mark zombie → wake waiting parent →
   `schedule()` away. Scheduler's `reap_dead` frees threads when it sees Dead.
3. **proc_wait** → `block_current()` until child zombie → detach + `proc_reap`
   → return zombie.
4. **proc_fork** → new vmm_space → copy low-half frames → child thread with
   copied context → child returns 0, parent gets PID.
5. **proc_exec** → parse ELF64 → map PT_LOAD segments into existing space →
   reset to one fresh thread with new entry.

## Error Handling

- All functions return NULL / -1 on allocation failure (existing `proc_create`
  pattern).
- `proc_exec`: on ELF parse failure, process unchanged, return -1.
- `proc_fork`: on any frame copy failure, destroy partial child space + PCB,
  return -1.
- `proc_kill(unknown pid)` → NULL.
- `proc_wait` with no children: blocks indefinitely, like a real wait.

## Files Touched

- `kernel/include/multitasking/proc.h` — new PCB, function declarations
- `kernel/src/multitasking/proc.c` — all implementations
- `kernel/include/multitasking/thread.h` — possibly helpers for killing all
  threads of a proc
- `kernel/src/multitasking/thread.c` — circular insertion fix
- `kernel/src/multitasking/sched.c` — reap_dead frame-free fix
- `kernel/src/multitasking/switch.asm` — fork child resume path
- `kernel/src/main.c` (or wherever init lives) — test harness exercising the API

## Testing

- Build: `make -C kernel` (uses `-Wall -Wextra`).
- Runtime: `make run-x86_64` (QEMU with isa-debugcon → stdout).
- Test harness in the init code:
  1. `proc_create` a child + `proc_wait`, child calls `proc_exit(42)`, parent
     verifies `exit_code == 42`.
  2. `proc_fork`: parent and child print different PIDs; child takes the "return
     0" branch.
  3. `proc_kill` a long-running child.
  4. `proc_exec` on a small in-memory ELF blob.
  5. `proc_add_thread` and verify both threads of a process run.