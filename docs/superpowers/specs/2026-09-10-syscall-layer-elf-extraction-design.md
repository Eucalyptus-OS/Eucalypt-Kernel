# Syscall Layer and ELF Extraction Design

Date: 2026-09-10

## Overview

Refactor of the completed process subsystem into better-separated units:

1. Extract the inline ELF64 loader out of `proc.c` into its own translation
   unit (`elf.c`), with the page-walk helper promoted into the VMM.
2. Add a `syscalls/` layer — a numeric syscall ID table and ABI-shaped wrapper
   functions — as a staging point for a future user-mode syscall entry.

The process API in `proc.c` remains the ring-0 internal API. The syscall layer
sits on top of it and is the only component shaped like a future user-facing
ABI. No user-mode trap/entry path exists yet, and none is added here.

## File Layout

| File | Change | Role |
|------|--------|------|
| `kernel/include/syscalls/sys.h` | new | `SYS_*` enum, `sys_*` + `syscall_dispatch` declarations |
| `kernel/src/syscalls/sys.c` | new | dispatch table + 5 ABI wrappers |
| `kernel/src/multitasking/elf.c` | new | `elf_load`, `elf_fill_segment` |
| `kernel/include/multitasking/elf.h` | modify | add declarations for the two moved functions |
| `kernel/src/multitasking/proc.c` | modify | delete the moved functions; call `vmm_walk_phys` |
| `kernel/src/mm/vmm.c` | modify | receive promoted `vmm_walk_phys` |
| `kernel/include/mm/vmm.h` | modify | declare `vmm_walk_phys` |

New source files are picked up automatically: the kernel GNUmakefile globs all
`src/**/*.c` via `find`.

## Syscall Surface (`sys.h`)

```c
enum {
    SYS_FORK,
    SYS_EXEC,
    SYS_EXIT,
    SYS_WAIT,
    SYS_KILL,
    SYS_COUNT
};

long syscall_dispatch(int n, long a1, long a2, long a3, long a4, long a5);
long sys_fork(void);
long sys_exec(void *elf, uintptr_t size, void *stack);
long sys_exit(int code);          /* never returns */
long sys_wait(int *status);
long sys_kill(uint64_t pid);
```

Only the user-facing subset gets syscall IDs: fork, exec, exit, wait, kill.
`proc_create`/`proc_find`/`proc_add_thread`/`proc_remove_thread`/`proc_reap`/
`proc_destroy` stay kernel-internal (bootstrap, teardown, plumbing that no
user process may invoke).

### Function Semantics (ABI-shaped; no kernel pointers cross the layer)

- **sys_fork()** — forwards to `proc_fork()`. Returns the child's pid to the
  parent, 0 to the child, -1 on failure. Frame-copy and child-context behavior
  are unchanged from the existing API.
- **sys_exec(elf, size, stack)** — forwards to `proc_exec()`. Returns 0 on
  success, -1 on failure; never returns on success. Process is left untouched
  on failure.
- **sys_exit(code)** — forwards to `proc_exit()`. Marks the process zombie and
  schedules away; never returns.
- **sys_wait(status)** — the one behavioral change: wait + reap combined,
  waitpid-like. Calls `proc_wait(me)` (the current thread's parent process).
  If it returns NULL (no children), returns -1. Otherwise writes the child's
  `exit_code` through `status` when `status != NULL`, calls `proc_reap(child)`,
  and returns the child's pid.
- **sys_kill(pid)** — forwards to `proc_kill(pid)`. Returns 0 when a matching
  child was found and marked zombie, -1 when no matching child exists.

### Dispatch

`sys.c` holds a static, compile-time-visible table indexed by the enum values:

```c
typedef long (*sysfn)(long a1, long a2, long a3, long a4, long a5);

static const sysfn syscall_table[SYS_COUNT] = {
    [SYS_FORK] = sys_fork, [SYS_EXEC] = sys_exec,
    [SYS_EXIT] = sys_exit, [SYS_WAIT] = sys_wait,
    [SYS_KILL] = sys_kill,
};
```

- `syscall_dispatch(n, a1..a5)` bounds-checks `n`, returns -1 for out-of-range
  or NULL entries, otherwise calls the entry. The fixed 5-arg shape mirrors a
  register-based ABI a future entry trampoline would use.
- `syscall_dispatch` is exported (non-static) and `syscall_table` is `static
  const`; the layer is build-checked only and is not yet called from anywhere.
- Wrappers take typed arguments and adapt them to the `long`-shaped ABI slot.

## ELF Extraction

Move the two load-bearing helpers out of `proc.c` unchanged (they are already
`extern`-independent: they take a `struct pcb *` and use only public vmm APIs
plus `phys_to_virt`/`virt_to_phys`):

- `int elf_load(struct pcb *p, void *elf, uintptr_t size, void **entry)` —
  ELF64 validation, PT_LOAD mapping via `vmm_map_at`, on-disk struct access,
  rollback of previously mapped segments on failure (`mapped[]`).
- `void elf_fill_segment(struct pcb *p, struct elf64_phdr *ph)` — writes
  `p_filesz` bytes and zeroes the `p_memsz - p_filesz` BSS tail through the
  segment pages' physical frames (added in Task 8 as the fix for writing into
  an R+X mapping).

Both are declared in the existing `kernel/include/multitasking/elf.h`.

### `vmm_walk_phys` promotion

`walk_phys(uint64_t *pml4, void *vaddr)` is currently `static` in `proc.c` and
shared by `proc_fork_c` and `elf_fill_segment`. It resolves a virtual address
through a PML4 to its physical frame. Promote it into `mm/vmm.c` unchanged as:

```c
uintptr_t vmm_walk_phys(uint64_t *pml4, void *vaddr);
```

declared in `mm/vmm.h` (matching `struct vmm_space { uint64_t *pml4; ... }`).
Both callers keep their existing `walk_phys(...)` call sites, renamed to
`vmm_walk_phys(...)`. No behavior change; known limitation (no `PAGE_PRESENT`
check at intermediate levels, safe while all callers walk freshly mapped
regions) is preserved as-is and documented in the header comment.

## Build and Verification

- `make -C kernel` from the repo root: clean link into `kernel/bin-x86_64/kernel`,
  no new warnings under `-Wall -Wextra`.
- No runtime test: the syscall layer is deliberately not called yet (the
  dispatch table exists to be wired to a future user-mode entry); nothing else
  in the tree references it.

## Non-Goals

- No user-mode trap/entry path (no `syscall`/`sysenter`, no IDT gate, no
  ring-3 transition). `syscall_dispatch` is a staging seam, called later.
- No ABI stability or compatibility guarantees; enum values may be reordered
  before real user mode exists.
- No behavioral change to wait/kill/fork/exec semantics beyond the wait+reap
  combination inside `sys_wait`.
- No refactor of `proc.c` internals other than removing the moved functions
  and renaming the `walk_phys` call sites.
- No exposure of `proc_find`/`proc_add_thread`/`proc_reap`/`proc_destroy`/
  `proc_create` in the syscall layer.

## Data Flow

```
user-mode entry (future) → syscall_dispatch(n, a1..a5)
                          → sys_fork/sys_exec/sys_exit/sys_wait/sys_kill
                          → proc_fork/proc_exec/proc_exit/proc_wait/proc_reap/proc_kill
                          → vmm_* / thread_* / sched_* primitives

proc_exec → elf_load(p, elf, size, &entry)   [elf.c]
          → elf_fill_segment(p, ph)          [elf.c]
          → vmm_walk_phys(p->space.pml4, va) [vmm.c]
proc_fork → vmm_walk_phys(...)               [vmm.c] for frame copy
```