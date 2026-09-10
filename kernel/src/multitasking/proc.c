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
