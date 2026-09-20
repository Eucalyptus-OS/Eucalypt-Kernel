#pragma once

#include <stdint.h>

typedef struct linked_list_node {
    uint64_t base;            // virtual address where the region begins
    uint64_t end;             // first virtual address past the end of the region
    uint64_t len;             // number of 4 KiB pages in the region
    uint64_t flags;           // page flags the region was mapped with
    struct linked_list_node *next; // next region in the registry
    struct linked_list_node *prev; // previous region in the registry
} linked_list_node_t;

typedef struct linked_list {
    uint64_t count;           // number of tracked regions
    linked_list_node_t *head; // first region node
    linked_list_node_t *tail; // last region node
} linked_list_t;

// Allocate and map a region of anonymous pages, returning its base virtual address
void *vmm_map_region(uint64_t *pml4_phys, void *vaddr, uint64_t flags, int pages_needed);
// Unmap and release a previously mapped region
void vmm_free_region(uint64_t *pml4, linked_list_node_t *node);
// Find the tracked region that begins at vaddr
linked_list_node_t *vmm_find_region(uint64_t vaddr);
// Reset the region registry to empty
uint8_t vmm_init();