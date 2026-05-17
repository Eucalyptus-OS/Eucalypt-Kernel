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

    pcb->threads[0] = create_thread(entry, user);
    return pcb;
}
