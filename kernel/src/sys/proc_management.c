#include <stdint.h>
#include <logging/print.h>
#include <fs/vfs.h>
#include <multitasking/sched.h>
#include <multitasking/proc.h>
#include <multitasking/thread.h>
#include <mm/page.h>
#include <mm/hhdm.h>
#include <mm/vmm.h>
#include <mm/heap.h>
#include <mm/memory.h>
#include <abi/auxv.h>
#include <abi/errno.h>
#include <abi/types.h>
#include <binary_loaders/elf.h>
#include <signal.h>

typedef int pid_t;

#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8

#define USER_HEAP_START 0x0000600000000000UL
#define USER_STACK_TOP 0x0000700000000000UL
#define USER_MMAP_START 0x0000620000000000UL
#define USTACK_SIZE 0x10000

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9
#define AT_RANDOM 25
#define AT_EXECFN 31

extern void fork_child_restore();
extern void jump_to_user(uint64_t entry, uint64_t stack);
extern void exec_switch_resume(void *ksp);

struct exec_args {
    char *path;
    char **argv;
    char **envp;
};

int setup_user_stack(uintptr_t pml4, char *const argv[], char *const envp[],
                            const char *path, const struct elf64_load_info *info,
                            uint64_t *out_rsp) {
    void *base = vmm_map_region((uint64_t *)pml4,
                                (void *)(USER_STACK_TOP - USTACK_SIZE),
                                PAGE_WRITABLE | PAGE_USER,
                                USTACK_SIZE / PAGE_SIZE);
    if (!base) {
        return 1;
    }

    uint64_t nargv = 0;
    while (argv && argv[nargv]) nargv++;
    uint64_t nenvp = 0;
    while (envp && envp[nenvp]) nenvp++;

    char **arg_strs = (char **)kmalloc((nargv ? nargv : 1) * sizeof(char *));
    char **env_strs = (char **)kmalloc((nenvp ? nenvp : 1) * sizeof(char *));
    if (!arg_strs || !env_strs) {
        kfree(arg_strs);
        kfree(env_strs);
        return 1;
    }

    uint8_t *sp = (uint8_t *)USER_STACK_TOP;

    for (uint64_t i = 0; i < nargv; i++) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        memcpy(sp, argv[i], len);
        arg_strs[i] = (char *)sp;
    }

    for (uint64_t i = 0; i < nenvp; i++) {
        size_t len = strlen(envp[i]) + 1;
        sp -= len;
        memcpy(sp, envp[i], len);
        env_strs[i] = (char *)sp;
    }

    size_t path_len = strlen(path) + 1;
    sp -= path_len;
    memcpy(sp, path, path_len);
    uint64_t execfn = (uint64_t)sp;

    uint8_t random_bytes[16];
    static uint64_t rng = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 16; i += 8) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        memcpy(random_bytes + i, &rng, 8);
    }
    sp -= 16;
    memcpy(sp, random_bytes, 16);
    uint64_t random_ptr = (uint64_t)sp;

    if ((uint64_t)sp < (uint64_t)base) {
        kfree(arg_strs);
        kfree(env_strs);
        return 1;
    }

    uint64_t auxv[18];
    uint64_t ai = 0;
    auxv[ai++] = AT_PHDR;    auxv[ai++] = info->phdr;
    auxv[ai++] = AT_PHENT;   auxv[ai++] = info->phent;
    auxv[ai++] = AT_PHNUM;   auxv[ai++] = info->phnum;
    auxv[ai++] = AT_ENTRY;   auxv[ai++] = info->entry;
    auxv[ai++] = AT_PAGESZ;  auxv[ai++] = 0x1000;
    auxv[ai++] = AT_BASE;    auxv[ai++] = 0;
    auxv[ai++] = AT_EXECFN;  auxv[ai++] = execfn;
    auxv[ai++] = AT_RANDOM;  auxv[ai++] = random_ptr;
    auxv[ai++] = AT_NULL;    auxv[ai++] = 0;

    uint64_t array_top = (uint64_t)sp & ~0xFULL;
    uint64_t total = (1 + (nargv + 1) + (nenvp + 1) + ai) * 8;
    uint64_t *p = (uint64_t *)((array_top - total) & ~0xFULL);

    if ((uint64_t)p < (uint64_t)base) {
        kfree(arg_strs);
        kfree(env_strs);
        return 1;
    }

    uint64_t q = 0;
    p[q++] = nargv;
    for (uint64_t i = 0; i < nargv; i++) p[q++] = (uint64_t)arg_strs[i];
    p[q++] = 0;
    for (uint64_t i = 0; i < nenvp; i++) p[q++] = (uint64_t)env_strs[i];
    p[q++] = 0;
    for (uint64_t i = 0; i < ai; i++) p[q++] = auxv[i];

    kfree(arg_strs);
    kfree(env_strs);

    *out_rsp = (uint64_t)p;
    return 0;
}

static void free_exec_args(struct exec_args *a) {
    if (a->argv) {
        for (int i = 0; a->argv[i]; i++) kfree(a->argv[i]);
        kfree(a->argv);
    }
    if (a->envp) {
        for (int i = 0; a->envp[i]; i++) kfree(a->envp[i]);
        kfree(a->envp);
    }
    kfree(a->path);
}

static int snapshot_exec_args(const char *path, char *const argv[], char *const envp[],
                              struct exec_args *out) {
    memset(out, 0, sizeof(*out));

    uint64_t nargv = 0;
    while (argv && argv[nargv]) nargv++;
    uint64_t nenvp = 0;
    while (envp && envp[nenvp]) nenvp++;

    size_t path_len = strlen(path) + 1;
    out->path = (char *)kmalloc(path_len);
    if (!out->path) {
        return 1;
    }
    memcpy(out->path, path, path_len);

    if (nargv) {
        out->argv = (char **)kmalloc((nargv + 1) * sizeof(char *));
        if (!out->argv) goto fail;
        memset(out->argv, 0, (nargv + 1) * sizeof(char *));
        for (uint64_t i = 0; i < nargv; i++) {
            char *s = (char *)kmalloc(strlen(argv[i]) + 1);
            if (!s) goto fail;
            memcpy(s, argv[i], strlen(argv[i]) + 1);
            out->argv[i] = s;
        }
    }

    if (nenvp) {
        out->envp = (char **)kmalloc((nenvp + 1) * sizeof(char *));
        if (!out->envp) goto fail;
        memset(out->envp, 0, (nenvp + 1) * sizeof(char *));
        for (uint64_t i = 0; i < nenvp; i++) {
            char *s = (char *)kmalloc(strlen(envp[i]) + 1);
            if (!s) goto fail;
            memcpy(s, envp[i], strlen(envp[i]) + 1);
            out->envp[i] = s;
        }
    }

    return 0;

fail:
    free_exec_args(out);
    return 1;
}

void _exit(uint64_t exit_code) {
    struct pcb *p = sched_current_proc();
    if (p) {
        p->exit_code = exit_code;
        p->wait_events |= WAIT_EVT_EXITED;
        if (p->ppcb) {
            sig_queue(p->ppcb, SIGCHLD);
            if (!p->ppcb->stopped) {
                wake_threads(p->ppcb);
            }
        }
        p->is_zombie = 1;
        zombie_enqueue(p);
        print("EXIT pid=%d code=%lu\n", p->pid, (unsigned long)exit_code);
    } else {
        print("EXIT tid=%d code=%lu (no proc)\n", current_tcb->tid, (unsigned long)exit_code);
    }

    current_tcb->state = Exited;

    schedule();

    for (;;) {
        asm volatile ("hlt");
    }
}

pid_t fork() {
    struct pcb *p = sched_current_proc();
    if (!p) {
        return -1;
    }

    struct pcb *cp = (struct pcb *)kmalloc(sizeof(struct pcb));
    struct tcb *ct = (struct tcb *)kmalloc(sizeof(struct tcb));
    uint8_t *kstack = alloc_kernel_stack();
    uintptr_t address_space = fork_address_space();
    uint8_t *cfpu = (uint8_t *)kmalloc(512);

    if (!cp || !ct || !kstack || !address_space || !cfpu) {
        kfree(cp);
        kfree(ct);
        if (kstack) {
            free_kernel_stack(kstack);
        }
        if (cfpu) {
            kfree(cfpu);
        }
        return -1;
    }
    asm volatile ("fxsave %0" : : "m"(*(uint8_t (*)[512])cfpu) : "memory");

    cp->pid = ++proc_count;
    cp->pgid = p->pgid;
    cp->t_count = 1;
    cp->addr_space = address_space;
    cp->t = NULL;
    cp->heap_begin = p->heap_begin;
    cp->heap_end = p->heap_end;
    cp->exit_code = 0;
    cp->stopped = p->stopped;
    cp->is_zombie = 0;
    cp->wait_events = 0;
    cp->wait_stop_sig = 0;
    cp->z_prev = NULL;
    cp->z_next = NULL;
    cp->ppcb = p;
    cp->umask = p->umask;
    cp->mmap_cursor = p->mmap_cursor;
    cp->mmaps = NULL;
    for (struct mmap_region *r = p->mmaps; r; r = r->next) {
        struct mmap_region *nr = (struct mmap_region *)kmalloc(sizeof(struct mmap_region));
        if (!nr) break;
        nr->base = r->base;
        nr->len = r->len;
        nr->prot = r->prot;
        nr->next = cp->mmaps;
        cp->mmaps = nr;
    }
    cp->sigstate = p->sigstate;
    cp->sigstate.pending = 0;
    cp->next = NULL;
    vfs_fd_table_clone(cp->fd_table, p->fd_table, MAX_FDS);

    if (proc_list == NULL) {
        proc_list = cp;
        cp->next = cp;
    } else {
        cp->next = proc_list->next;
        proc_list->next = cp;
    }

    uint64_t *sp = (uint64_t *)(kstack + KSTACK_SIZE);
    uint64_t *src = (uint64_t *)current_tcb->kstack_top;

    sp -= 22;
    sp[0] = src[-21];
    sp[1] = src[-20];
    sp[2] = src[-19];
    sp[3] = src[-18];
    sp[4] = src[-17];
    sp[5] = src[-16];
    sp[6] = src[-15];
    sp[7] = src[-14];
    sp[8] = src[-13];
    sp[9] = src[-11];
    sp[10] = src[-12];
    sp[11] = src[-10];
    sp[12] = src[-9];
    sp[13] = src[-8];
    sp[14] = 0;
    sp[15] = src[-3];
    sp[16] = (uint64_t)fork_child_restore;
    sp[17] = src[-5];
    sp[18] = src[-4];
    sp[19] = src[-3];
    sp[20] = src[-2];
    sp[21] = src[-1];

    ct->tid = thread_count++;
    ct->ksp = sp;
    ct->kstack_top = kstack + KSTACK_SIZE;
    ct->tsp = (void *)src[-2];
    ct->addr_space = cp->addr_space;
    ct->fpu_area = cfpu;
    ct->parent = cp;
    ct->state = Ready;
    ct->fs_base = current_tcb->fs_base;

    if (thread_list == NULL) {
        thread_list = ct;
        ct->next = ct;
    } else {
        ct->next = thread_list->next;
        thread_list->next = ct;
    }

    cp->t = ct;
    ct->proc_next = ct;

    return (pid_t)cp->pid;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    struct pcb *p = sched_current_proc();
    if (!p || !path) {
        return -1;
    }

    print("EXECVE pid=%d path=%s\n", p->pid, path);

    struct exec_args args;
    if (snapshot_exec_args(path, argv, envp, &args)) {
        return -1;
    }

    int fd = open(args.path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    uintptr_t new_pml4 = paging_create_pml4();
    if (!new_pml4) {
        close(fd);
        return -1;
    }

    struct elf64_load_info info;
    uint64_t entry = elf64_parse(fd, new_pml4, &info);
    close(fd);
    if (!entry) {
        paging_destroy_address_space(new_pml4);
        return -1;
    }

    asm volatile ("cli");

    reload_cr3(new_pml4);

    uint64_t stack_top = 0;
    int sret = setup_user_stack(new_pml4, args.argv, args.envp, args.path, &info, &stack_top);

    free_exec_args(&args);

    if (sret) {
        reload_cr3(p->addr_space);
        asm volatile ("sti");
        paging_destroy_address_space(new_pml4);
        return -1;
    }

    uintptr_t old_pml4 = p->addr_space;
    p->addr_space = new_pml4;
    p->heap_begin = USER_HEAP_START;
    p->heap_end = USER_HEAP_START;
    p->mmaps = NULL;
    p->mmap_cursor = USER_MMAP_START;
    p->stopped = 0;
    for (int i = 1; i < NSIG; i++) {
        if (p->sigstate.actions[i].sa_handler != SIG_IGN) {
            p->sigstate.actions[i].sa_handler = SIG_DFL;
        }
    }
    p->sigstate.pending = 0;
    current_tcb->addr_space = new_pml4;
    current_tcb->tsp = (void *)stack_top;
    current_tcb->fs_base = 0;

    paging_destroy_address_space(old_pml4);

    uint64_t *sp = (uint64_t *)current_tcb->kstack_top;
    sp -= 17;
    for (int i = 0; i < 17; i++) {
        sp[i] = 0;
    }
    sp[9] = stack_top;
    sp[10] = entry;
    sp[15] = 0x202;
    sp[16] = (uint64_t)jump_to_user;
    current_tcb->ksp = sp;

    exec_switch_resume(sp);

    return -1;
}

int pause() {
    struct pcb *p = sched_current_proc();
    if (!p) {
        return -1;
    }

    current_tcb->state = Blocked;

    while (p->sigstate.pending == 0) {
        schedule();
        if (p->sigstate.pending == 0) {
            current_tcb->state = Blocked;
        }
    }

    current_tcb->state = Ready;

    return -1;
}

static int child_is_mine(struct pcb *p, struct pcb *c) {
    return c && c->ppcb == p;
}

static int child_event_requested(struct pcb *c, int options) {
    if (c->wait_events & WAIT_EVT_EXITED) return 1;
    if ((c->wait_events & WAIT_EVT_STOPPED) && (options & WUNTRACED)) return 1;
    if ((c->wait_events & WAIT_EVT_CONTINUED) && (options & WCONTINUED)) return 1;
    return 0;
}

static struct pcb *find_waitable_child(struct pcb *p, int pid, int options) {
    if (pid > 0) {
        struct pcb *c = proc_find((uint64_t)pid);
        if (!c) return NULL;
        return child_is_mine(p, c) && child_event_requested(c, options) ? c : NULL;
    }

    if (!proc_list) return NULL;

    struct pcb *r = proc_list;
    do {
        if (child_is_mine(p, r)) {
            if (pid == -1 || pid == 0) {
                if (child_event_requested(r, options)) return r;
            } else if (pid < -1 && r->pgid == (uint64_t)(-pid)) {
                if (child_event_requested(r, options)) return r;
            }
        }
        r = r->next;
    } while (r != proc_list);
    return NULL;
}

static int parent_has_any_child(struct pcb *p) {
    if (p == NULL || proc_list == NULL) return 0;
    struct pcb *r = proc_list;
    do {
        if (child_is_mine(p, r)) return 1;
        r = r->next;
    } while (r != proc_list);
    return 0;
}

int waitpid(int pid, int *status, int options) {
    struct pcb *p = sched_current_proc();
    if (!p) {
        errno = ECHILD;
        return -1;
    }

    for (;;) {
        if (pid > 0) {
            struct pcb *c = proc_find((uint64_t)pid);
            if (!c || !child_is_mine(p, c)) {
                errno = ECHILD;
                return -1;
            }
        }

        struct pcb *child = find_waitable_child(p, pid, options);

        if (child) {
            uint64_t cpid = child->pid;

            if (child->wait_events & WAIT_EVT_EXITED) {
                if (status) {
                    *status = (int)((unsigned)(child->exit_code & 0xff) << 8);
                }
                child->wait_events &= ~WAIT_EVT_EXITED;
                proc_destroy(child);
                return (int)cpid;
            }

            if ((child->wait_events & WAIT_EVT_STOPPED) && (options & WUNTRACED)) {
                child->wait_events &= ~WAIT_EVT_STOPPED;
                if (status) {
                    *status = (int)((unsigned)(child->wait_stop_sig & 0xff) << 8) | 0x7f;
                }
                return (int)cpid;
            }

            if ((child->wait_events & WAIT_EVT_CONTINUED) && (options & WCONTINUED)) {
                child->wait_events &= ~WAIT_EVT_CONTINUED;
                if (status) {
                    *status = 0xffff;
                }
                return (int)cpid;
            }
        }

        if (options & WNOHANG) {
            return 0;
        }

        if (pid <= 0) {
            if (!parent_has_any_child(p)) {
                errno = ECHILD;
                return -1;
            }
        }

        block_current();
    }
}

int setpgid(pid_t pid, pid_t pgid) {
    struct pcb *p = sched_current_proc();
    if (!p) {
        return -ESRCH;
    }
    if (pid == 0) {
        pid = (pid_t)p->pid;
    }
    struct pcb *target = proc_find((uint64_t)pid);
    if (!target) {
        return -ESRCH;
    }
    if (pgid == 0) {
        pgid = pid;
    }
    if (pgid < 0 || pgid == 1) {
        return -EINVAL;
    }
    target->pgid = (uint64_t)pgid;
    return 0;
}

pid_t getpgid(pid_t pid) {
    struct pcb *p = sched_current_proc();
    if (!p) {
        return -ESRCH;
    }
    if (pid == 0) {
        pid = (pid_t)p->pid;
    }
    struct pcb *target = proc_find((uint64_t)pid);
    if (!target) {
        return -ESRCH;
    }
    return (pid_t)target->pgid;
}

pid_t getpgrp(void) {
    return getpgid(0);
}