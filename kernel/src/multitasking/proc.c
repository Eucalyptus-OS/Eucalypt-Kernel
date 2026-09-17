#include <stdint.h>
#include <stddef.h>
#include <sync/spinlock.h>
#include <mm/page.h>
#include <mm/frame.h>
#include <mm/vmm.h>
#include <mm/heap.h>
#include <mm/memory.h>
#include <fs/vfs.h>
#include <multitasking/thread.h>
#include <multitasking/proc.h>

#define USER_HEAP_START 0x0000600000000000UL
#define USER_STACK_TOP 0x0000700000000000UL
#define USER_MMAP_START 0x0000620000000000UL
#define USTACK_SIZE 0x10000

struct pcb *proc_list = NULL;
uint64_t proc_count = 0;
struct pcb *zombie_head = NULL;
static struct pcb *zombie_tail = NULL;
static spinlock_t lock = 0;
static spinlock_t proc_lock = 0;

void zombie_enqueue(struct pcb *p) {
    uint64_t flags = spinlock_acquire_irqsave(&lock);
    if (p->z_next || p->z_prev) {
        spinlock_release_irqrestore(&lock, flags);
        return;
    }
    if (zombie_head == NULL) {
        zombie_head = zombie_tail = p;
        p->z_next = p->z_prev = NULL;
    } else {
        zombie_tail->z_next = p;
        p->z_prev = zombie_tail;
        p->z_next = NULL;
        zombie_tail = p;
    }
    spinlock_release_irqrestore(&lock, flags);
}

static void zombie_remove_locked(struct pcb *p) {
    if (!p || !zombie_head) {
        return;
    }
    struct pcb *z = zombie_head;
    int found = 0;
    do {
        if (z == p) {
            found = 1;
            break;
        }
        z = z->z_next;
    } while (z && z != zombie_head);
    if (!found) {
        return;
    }
    struct pcb *n = p->z_next;
    struct pcb *pr = p->z_prev;
    if (pr) {
        pr->z_next = n;
    } else {
        zombie_head = n;
    }
    if (n) {
        n->z_prev = pr;
    } else {
        zombie_tail = pr;
    }
    p->z_next = p->z_prev = NULL;
}

void zombie_remove(struct pcb *p) {
    uint64_t flags = spinlock_acquire_irqsave(&lock);
    zombie_remove_locked(p);
    spinlock_release_irqrestore(&lock, flags);
}

struct pcb *zombie_pop() {
    uint64_t flags = spinlock_acquire_irqsave(&lock);
    if (!zombie_head) {
        spinlock_release_irqrestore(&lock, flags);
        return NULL;
    }
    struct pcb *p = zombie_head;
    zombie_remove_locked(p);   // no re-acquire
    spinlock_release_irqrestore(&lock, flags);
    return p;
}

static void reparent_children(struct pcb *p) {
    struct pcb *target = proc_find(1);
    uint64_t flags = spinlock_acquire_irqsave(&lock);
    if (!target || target == p) {
        struct pcb *r = proc_list;
        if (r == p) {
            r = r->next;
        }
        target = (r && r != p) ? r : NULL;
    }
    if (!target || !proc_list) {
        spinlock_release_irqrestore(&lock, flags);
        return;
    }
    struct pcb *r = proc_list;
    do {
        if (r->ppcb == p) {
            r->ppcb = target;
        }
        r = r->next;
    } while (r != proc_list);
    spinlock_release_irqrestore(&lock, flags);
}

struct pcb *proc_create(void *entry) {
    struct pcb *p = (struct pcb *)kmalloc(sizeof(struct pcb));
    uint64_t flags = spinlock_acquire_irqsave(&lock);
    if (!p) {
        spinlock_release_irqrestore(&lock, flags);
        return NULL;
    }

    p->pid = ++proc_count;
    p->pgid = p->pid;
    p->t_count = 0;
    p->addr_space = paging_create_pml4();
    p->t = NULL;
    p->heap_begin = USER_HEAP_START;
    p->heap_end = USER_HEAP_START;
    p->exit_code = 0;
    p->stopped = 0;
    p->is_zombie = 0;
    p->wait_events = 0;
    p->wait_stop_sig = 0;
    p->ppcb = NULL;
    p->z_prev = NULL;
    p->z_next = NULL;
    p->umask = 0022;
    p->mmaps = NULL;
    p->mmap_cursor = USER_MMAP_START;
    memset(&p->sigstate, 0, sizeof(p->sigstate));

    vfs_fd_table_init(p->fd_table, MAX_FDS);
    vfs_fd_table_setup_stdio(p->fd_table, MAX_FDS);

    void *ustack = vmm_map_region(
        (uint64_t *)p->addr_space,
        (void *)(USER_STACK_TOP - USTACK_SIZE),
        PAGE_WRITABLE | PAGE_USER,
        USTACK_SIZE / PAGE_SIZE
    );

    if (!ustack) {
        kfree(p);
        spinlock_release_irqrestore(&lock, flags);
        return NULL;
    }

    if (proc_list == NULL) {
        proc_list = p;
        p->next = p;
    } else {
        p->next = proc_list->next;
        proc_list->next = p;
    }

    struct tcb *t = create_thread(entry, p, ustack);
    if (!t) {
        kfree(p);
        spinlock_release_irqrestore(&lock, flags);
        return NULL;
    }
    spinlock_release_irqrestore(&lock, flags);
    return p;
}

struct pcb *proc_find(uint64_t pid) {
    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    if (!proc_list) {
        spinlock_release_irqrestore(&proc_lock, flags);
        return NULL;
    }
    struct pcb *p = proc_list;
    do {
        if (p->pid == pid) {
            spinlock_release_irqrestore(&proc_lock, flags);
            return p;
        }
        p = p->next;
    } while (p != proc_list);
    spinlock_release_irqrestore(&proc_lock, flags);
    return NULL;
}

uintptr_t proc_sbrk(struct pcb *p, intptr_t increment) {
    uintptr_t old_end = p->heap_end;

    if (increment == 0) {
        return old_end;
    }

    if (increment > 0) {
        uint64_t bytes_needed = (uint64_t)increment;
        int pages_needed = (bytes_needed + PAGE_SIZE - 1) / PAGE_SIZE;

        void *mapped = vmm_map_region(
            (uint64_t *)p->addr_space,
            (void *)p->heap_end,
            PAGE_WRITABLE | PAGE_USER,
            pages_needed
        );

        if (!mapped) {
            return (uintptr_t)-1;
        }

        p->heap_end += pages_needed * PAGE_SIZE;
    } else {
        uintptr_t shrink_target = old_end + increment;

        linked_list_node_t *node = vmm_find_region(p->heap_end);
        if (node) {
            vmm_free_region((uint64_t *)p->addr_space, node);
        }

        p->heap_end = shrink_target;
    }

    return old_end;
}

void proc_destroy(struct pcb *p) {
    reparent_children(p);
    zombie_remove(p);
    vfs_fd_table_close(p->fd_table, MAX_FDS);

    while (p->t != NULL) {
        destroy_thread(p->t);
    }

    uint64_t flags = spinlock_acquire_irqsave(&proc_lock);
    if (proc_list) {
        if (proc_list->next == proc_list) {
            proc_list = NULL;
        } else {
            struct pcb *prev = proc_list;
            while (prev->next != p) {
                prev = prev->next;
            }
            prev->next = p->next;
            if (proc_list == p) {
                proc_list = p->next;
            }
        }
    }
    spinlock_release_irqrestore(&proc_lock, flags);

    paging_destroy_address_space(p->addr_space);
    kfree(p);
}