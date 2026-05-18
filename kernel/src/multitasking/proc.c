#include <stddef.h>
#include <stdint.h>
#include <mm/types.h>
#include <mm/paging.h>
#include <mm/heap.h>
#include <multitasking/thread.h>
#include <multitasking/sched.h>

struct pcb {
    uint16_t pid;
    paddr cr3;
    struct tcb *threads[MAX_THREADS];
};

uint16_t next_pid = 0;

struct pcb *proc_create(void *entry, bool user) {
    struct pcb *pcb = kmalloc(sizeof(struct pcb));
    if (!pcb) {
        return NULL;
    }

    pcb->pid = next_pid++;
    pcb->cr3 = paging_create_pml4();
    // Set all the threads to NULL
    for (int i = 0; i < MAX_THREADS; i++) {
        pcb->threads[i] = NULL;
    }

    pcb->threads[0] = create_thread(entry, pcb->cr3, user);
    return pcb;
}

struct pcb *add_thread(struct pcb *proc, void *entry, bool user) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (proc->threads[i] == NULL) {
            proc->threads[i] = create_thread(entry, proc->cr3, user);
            return proc;
        }
    }
    return NULL;
}

void proc_destroy(struct pcb *proc) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (proc->threads[i]) {
            thread_destroy(proc->threads[i]);
        }
    }
    kfree(proc);
}