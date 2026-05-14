#include <stdint.h>
#include <mm/types.h>
#include <mm/paging.h>
#include <multitasking/thread.h>

struct pcb {
    uint16_t pid;
    paddr cr3;
    struct tcb *threads[1024];
};

uint8_t proc_create() {
    
}