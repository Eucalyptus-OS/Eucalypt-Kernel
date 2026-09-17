#pragma once

#include <stdint.h>

typedef struct linked_list_node {
    uint64_t base;
    uint64_t end;
    uint64_t len;
    uint64_t flags;
    struct linked_list_node *next;
    struct linked_list_node *prev;
} linked_list_node_t;

typedef struct linked_list {
    uint64_t count;
    linked_list_node_t *head;
    linked_list_node_t *tail;
} linked_list_t;

void *vmm_map_region(uint64_t *pml4_phys, void *vaddr, uint64_t flags, int pages_needed);
void vmm_free_region(uint64_t *pml4, linked_list_node_t *node);
linked_list_node_t *vmm_find_region(uint64_t vaddr);
uint8_t vmm_init();