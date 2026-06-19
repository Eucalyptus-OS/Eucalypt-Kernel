#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <mem.h>
#include <mm/heap.h>
#include <mm/paging.h>
#include <mm/frame.h>
#include <multitasking/thread.h>
#include <multitasking/sched.h>
#include <binl/elf64.h>
#include <drivers/fs/vfs/vfs.h>
#include <auxv.h>
#include <logging/printk.h>
#include <ipc/signal.h>
#include <multitasking/proc.h>

#define MAX_PROCS 256

static struct pcb *proc_table[MAX_PROCS];
static int32_t     next_pid = 0;

struct pcb *proc_get(int32_t pid) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (proc_table[i] && proc_table[i]->pid == pid)
            return proc_table[i];
    return NULL;
}

static int proc_table_insert(struct pcb *proc) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!proc_table[i]) {
            proc_table[i] = proc;
            return 0;
        }
    }
    return -1;
}

static void proc_init_common(struct pcb *proc, bool user) {
    memset(proc, 0, sizeof(struct pcb));
    proc->pid        = next_pid++;
    proc->parent_pid = -1;
    proc->pgid       = proc->pid;
    proc->sid        = proc->pid;
    proc->user       = user;
    proc->state      = PROC_RUNNING;

    for (int i = 0; i < NSIG; i++)
        proc->signal_handler[i] = default_sig_handler;

    vfs_fd_table_init(proc->fd_table, MAX_FDS);
    vfs_fd_table_setup_stdio(proc->fd_table, MAX_FDS);
}

struct pcb *proc_create(void *entry, bool user) {
    struct pcb *proc = kmalloc(sizeof(struct pcb));
    if (!proc) return NULL;

    proc_init_common(proc, user);
    proc->cr3        = paging_create_pml4();

    proc->threads[0] = user
        ? create_user_thread((uint64_t)entry, proc->cr3)
        : create_thread(entry, proc->cr3);

    if (!proc->threads[0]) {
        vfs_fd_table_close(proc->fd_table, MAX_FDS);
        kfree(proc);
        return NULL;
    }

    proc->threads[0]->parent = proc;
    proc_table_insert(proc);
    return proc;
}

struct pcb *proc_create_loaded_user(uintptr_t entry, uintptr_t cr3,
                                    char **argv, char **envp,
                                    const elf_load_info_t *info) {
    struct pcb *proc = kmalloc(sizeof(struct pcb));
    if (!proc) return NULL;

    proc_init_common(proc, true);
    proc->cr3 = cr3;

    proc->threads[0] = create_user_thread_with_stack(entry, proc->cr3,
                                                      argv, envp, info);
    if (!proc->threads[0]) {
        vfs_fd_table_close(proc->fd_table, MAX_FDS);
        kfree(proc);
        return NULL;
    }

    proc->threads[0]->parent = proc;
    proc_table_insert(proc);
    return proc;
}

struct pcb *proc_fork(void) {
    struct pcb *parent = proc_get(get_current_pid());
    if (!parent) return NULL;

    struct pcb *child = kmalloc(sizeof(struct pcb));
    if (!child) return NULL;

    memset(child, 0, sizeof(struct pcb));
    child->pid        = next_pid++;
    child->parent_pid = parent->pid;
    child->pgid       = parent->pgid;
    child->sid        = parent->sid;
    child->user       = parent->user;
    child->state      = PROC_RUNNING;
    child->heap_start = parent->heap_start;
    child->heap_end   = parent->heap_end;
    child->cr3        = paging_fork_pml4(parent->cr3);

    memcpy(child->signal_handler,
           parent->signal_handler,
           sizeof(parent->signal_handler));

    vfs_fd_table_clone(child->fd_table, parent->fd_table, MAX_FDS);

    child->threads[0] = thread_fork(parent->threads[0], child->cr3);
    if (!child->threads[0]) {
        vfs_fd_table_close(child->fd_table, MAX_FDS);
        kfree(child);
        return NULL;
    }
    child->threads[0]->parent = child;

    proc_table_insert(child);
    enqueue(child->threads[0]);

    return child;
}

int proc_exec(const char *path, char **argv, char **envp) {
    struct pcb *proc = proc_get(get_current_pid());
    if (!proc) return -1;

    int fd = vfs_open(path, 0);
    if (fd < 0) return -1;

    elf_load_info_t info = {0};
    uintptr_t entry = elf64_parse(fd, proc->cr3, &info);
    vfs_close(fd);

    if (!entry) return -1;

    info.execfn = (char *)path;

    if (proc->threads[0])
        thread_destroy(proc->threads[0]);

    proc->threads[0] = create_user_thread_with_stack(entry, proc->cr3,
                                                      argv, envp, &info);
    for (int i = 0; i < NSIG; i++)
        proc->signal_handler[i] = default_sig_handler;

    if (!proc->threads[0]) return -1;
    proc->threads[0]->parent = proc;

    enqueue(proc->threads[0]);
    sched_yield();

    __builtin_unreachable();
}

void proc_exit(int code) {
    struct pcb *proc = proc_get(get_current_pid());
    if (!proc) return;

    proc->state     = PROC_ZOMBIE;
    proc->exit_code = code;

    struct pcb *parent = proc_get(proc->parent_pid);
    if (parent)
        sched_wake(parent->threads[0]);

    for (int i = 0; i < MAX_THREADS; i++) {
        if (proc->threads[i])
            thread_destroy(proc->threads[i]);
        proc->threads[i] = NULL;
    }

    sched_yield();
}

void proc_exit_group(int code) {
    proc_exit(code);
}

int32_t proc_waitpid(int32_t pid, int *status, int flags) {
    (void)flags;

    struct pcb *parent = proc_get(get_current_pid());
    if (!parent) return -1;

    while (1) {
        for (int i = 0; i < MAX_PROCS; i++) {
            struct pcb *child = proc_table[i];
            if (!child) continue;
            if (child->parent_pid != parent->pid) continue;
            if (pid != -1 && child->pid != pid) continue;

            if (child->state == PROC_ZOMBIE) {
                int32_t cpid = child->pid;
                if (status) *status = child->exit_code;
                proc_destroy(child);
                proc_table[i] = NULL;
                return cpid;
            }
        }
        sched_sleep(parent->threads[0]);
    }
}

int proc_signal(int32_t pid, int sig) {
    if (sig < 0 || sig >= NSIG)
        return -1;

    struct pcb *proc = proc_get(pid);
    if (!proc || proc->state == PROC_ZOMBIE)
        return -1;

    if (sig == SIGKILL) {
        proc->state     = PROC_ZOMBIE;
        proc->exit_code = 128 + SIGKILL;
        for (int i = 0; i < MAX_THREADS; i++) {
            if (proc->threads[i])
                sched_wake(proc->threads[i]);
        }
        return 0;
    }

    if (sig == SIGSTOP) {
        proc->state = PROC_STOPPED;
        for (int i = 0; i < MAX_THREADS; i++) {
            if (proc->threads[i])
                sched_sleep(proc->threads[i]);
        }
        return 0;
    }

    proc->signal_pending |= (1U << sig);

    for (int i = 0; i < MAX_THREADS; i++) {
        if (proc->threads[i])
            sched_wake(proc->threads[i]);
    }

    return 0;
}

struct pcb *add_thread(struct pcb *proc, void *entry) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!proc->threads[i]) {
            proc->threads[i] = proc->user
                ? create_user_thread((uint64_t)entry, proc->cr3)
                : create_thread(entry, proc->cr3);
            if (proc->threads[i])
                proc->threads[i]->parent = proc;
            return proc->threads[i] ? proc : NULL;
        }
    }
    return NULL;
}

void proc_destroy(struct pcb *proc) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (proc->threads[i])
            thread_destroy(proc->threads[i]);
    }
    vfs_fd_table_close(proc->fd_table, MAX_FDS);
    frame_free(proc->cr3);
    kfree(proc);
}
