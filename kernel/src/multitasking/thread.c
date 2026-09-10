#include <multitasking/thread.h>
#include <multitasking/proc.h>
#include <mm/frame.h>
#include <mm/heap.h>
#include <mm/hhdm.h>
#include <mm/paging.h>
#include <memory.h>
#include <stddef.h>
#include <stdint.h>

// One page for now
// TODO: Add bigger stack sizes for threads
#define STACK_SIZE 4096

struct tcb *thread_list = NULL;

uint64_t thread_count = 0;

struct tcb *thread_create(void *entry, void *ustack, struct pcb *p) {
    if (!entry) {
        return NULL;
    }

    struct tcb *t = (struct tcb *)kmalloc(sizeof(struct tcb));
    if (!t) {
        return NULL;
    }
    memset(t, 0, sizeof(struct tcb));

    uintptr_t kstack_phys = frame_alloc();
    if (!kstack_phys) {
        kfree(t);
        return NULL;
    }

    uint8_t *kstack = phys_to_virt(kstack_phys);

    uintptr_t *sp = (uintptr_t *)(kstack + STACK_SIZE);
    // ret
    *--sp = (uintptr_t)entry;
    // rflags
    *--sp = 0x202;
    // rax
    *--sp = 0;
    // rbx
    *--sp = 0;
    // rcx
    *--sp = 0;
    // rdx
    *--sp = 0;
    // rdi
    *--sp = 0;
    // rsi
    *--sp = 0;
    //rbp
    *--sp = 0;
    // r8
    *--sp = 0;
    // r9
    *--sp = 0;
    // r10
    *--sp = 0;
    // r11
    *--sp = 0;
    // r12
    *--sp = 0;
    // r13
    *--sp = 0;
    // r14
    *--sp = 0;
    // r15
    *--sp = 0;

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
}