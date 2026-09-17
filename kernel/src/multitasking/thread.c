#include <stdint.h>
#include <stddef.h>
#include <mm/frame.h>
#include <mm/page.h>
#include <mm/hhdm.h>
#include <mm/heap.h>
#include <mm/vmm.h>
#include <logging/print.h>
#include <multitasking/proc.h>
#include <multitasking/thread.h>

struct tcb *thread_list = NULL;

uint64_t thread_count = 0;

#define KSTACK_REGION_BASE KERNEL_STACK_REGION

static uint64_t next_kstack_vaddr = KSTACK_REGION_BASE;

void *alloc_kernel_stack() {
    void *base = vmm_map_region((uint64_t *)kernel_pml4,
                                (void *)next_kstack_vaddr,
                                PAGE_WRITABLE,
                                KSTACK_SIZE / PAGE_SIZE);
    if (!base) {
        return NULL;
    }

    next_kstack_vaddr += KSTACK_SIZE;
    return base;
}

void free_kernel_stack(void *base) {
    if (!base) {
        return;
    }

    linked_list_node_t *node = vmm_find_region((uint64_t)base);
    if (node) {
        vmm_free_region((uint64_t *)kernel_pml4, node);
    }
}

struct tcb *create_thread(void *entry, struct pcb *p, void *ustack) {
    if (!p || !ustack) {
        return NULL;
    }

    struct tcb *t = (struct tcb *)kmalloc(sizeof(struct tcb));
    if (!t) {
        return NULL;
    }

    uint8_t *kstack = alloc_kernel_stack();
    if (!kstack) {
        kfree(t);
        return NULL;
    }

    uint8_t *fpu = (uint8_t *)kmalloc(512);
    if (!fpu) {
        free_kernel_stack(kstack);
        kfree(t);
        return NULL;
    }
    asm volatile ("fxsave %0" : : "m"(*(uint8_t (*)[512])fpu) : "memory");

    uintptr_t *sp = (uintptr_t *)(kstack + KSTACK_SIZE);

    *--sp = (uintptr_t)entry;
    *--sp = 0x202;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;

    t->tid = thread_count++;
    t->ksp = sp;
    t->kstack_top = kstack + KSTACK_SIZE;
    t->tsp = ustack;
    t->addr_space = p->addr_space;
    t->fpu_area = fpu;
    t->parent = p;
    t->state = Ready;
    t->fs_base = 0;
    t->wake_tick = 0;
    t->timed = 0;

    if (thread_list == NULL) {
        thread_list = t;
        t->next = t;
    } else {
        t->next = thread_list->next;
        thread_list->next = t;
    }

    if (p->t == NULL) {
        p->t = t;
        t->proc_next = t;
    } else {
        t->proc_next = p->t->proc_next;
        p->t->proc_next = t;
    }

    p->t_count++;

    return t;
}

void destroy_thread(struct tcb *t) {
    if (!t) {
        return;
    }

    struct pcb *p = t->parent;

    if (thread_list->next == thread_list) {
        thread_list = NULL;
    } else {
        struct tcb *prev = thread_list;
        while (prev->next != t) {
            prev = prev->next;
        }
        prev->next = t->next;
        if (thread_list == t) {
            thread_list = t->next;
        }
    }

    if (p->t->proc_next == p->t) {
        p->t = NULL;
    } else {
        struct tcb *prev = p->t;
        while (prev->proc_next != t) {
            prev = prev->proc_next;
        }
        prev->proc_next = t->proc_next;
        if (p->t == t) {
            p->t = t->proc_next;
        }
    }

    p->t_count--;

    print("DESTROY_THREAD tid=%d pid=%d\n", t->tid, p ? p->pid : -1);
    kfree(t->fpu_area);
    free_kernel_stack((void *)(t->kstack_top - KSTACK_SIZE));
    kfree(t);
}