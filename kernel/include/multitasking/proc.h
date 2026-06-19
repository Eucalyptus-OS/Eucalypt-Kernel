#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <ipc/signal.h>
#include <multitasking/thread.h>
#include <drivers/fs/vfs/vfs.h>

#define MAX_FDS  64
#define NSIG     32

typedef enum {
    PROC_RUNNING,
    PROC_ZOMBIE,
    PROC_STOPPED,
} proc_state_t;

struct tcb;

struct pcb {
    int32_t  pid;
    int32_t  parent_pid;
    int32_t  pgid;
    int32_t  sid;
    bool     user;
    proc_state_t state;
    int      exit_code;

    uintptr_t heap_start;
    uintptr_t heap_end;
    uintptr_t cr3;

    void (*signal_handler[NSIG])(int);
    uint32_t signal_pending;

    vfs_file_t *fd_table[MAX_FDS];

    struct tcb *threads[MAX_THREADS];
};

struct pcb *proc_create(void *entry, bool user);
struct pcb *proc_create_loaded_user(uintptr_t entry, uintptr_t cr3,
                                    char **argv, char **envp,
                                    const elf_load_info_t *info);
struct pcb *proc_fork(void);
int         proc_exec(const char *path, char **argv, char **envp);
void        proc_exit(int code);
void        proc_exit_group(int code);
int32_t     proc_waitpid(int32_t pid, int *status, int flags);
int         proc_signal(int32_t pid, int sig);
struct pcb *proc_get(int32_t pid);
struct pcb *add_thread(struct pcb *proc, void *entry);
void        proc_destroy(struct pcb *proc);
