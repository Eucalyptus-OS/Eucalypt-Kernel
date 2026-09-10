#include <multitasking/proc.h>
#include <multitasking/thread.h>
#include <multitasking/sched.h>
#include <multitasking/elf.h>
#include <mm/paging.h>
#include <mm/heap.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <mm/vmm.h>
#include <lib/list.h>
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

#define STACK_SIZE 4096
#define PAGE_PHYS_MASK 0x000FFFFFFFFFF000ULL

extern int fork_call(void);

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
