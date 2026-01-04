#include <x86_64/allocator/heap.h>
#include <x86_64/memory/pmm.h>
#include <x86_64/memory/vmm.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct linked_list_node {
    struct linked_list_node *next;
    struct linked_list_node *prev;
    size_t size;
    bool used;
};

struct linked_list {
    struct linked_list_node *first;
    struct linked_list_node *last;
};

static struct linked_list heap_list;
static uint8_t *heap_start = NULL;
static uint8_t *heap_current = NULL;
static size_t heap_size = 0;
static page_table_t kernel_page_table = NULL;

#define INITIAL_HEAP_PAGES 256
#define HEAP_EXPANSION_PAGES 64
#define HEAP_VIRTUAL_BASE 0xFFFF800000000000ULL

void heap_init() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    kernel_page_table = (page_table_t)phys_to_virt(cr3);
    
    void *phys_pages = pmm_alloc_pages(INITIAL_HEAP_PAGES);
    if (!phys_pages) {
        return;
    }
    
    heap_start = (uint8_t *)HEAP_VIRTUAL_BASE;
    heap_current = heap_start;
    heap_size = INITIAL_HEAP_PAGES * PAGE_SIZE;
    
    if (!vmm_map_range(kernel_page_table, (uint64_t)heap_start, (uint64_t)phys_pages, INITIAL_HEAP_PAGES, PTE_WRITABLE)) {
        pmm_free_pages(phys_pages, INITIAL_HEAP_PAGES);
        return;
    }
    
    heap_list.first = NULL;
    heap_list.last = NULL;
    
    struct linked_list_node *node = (struct linked_list_node *)heap_start;
    node->prev = NULL;
    node->next = NULL;
    node->size = heap_size - sizeof(struct linked_list_node);
    node->used = false;
    heap_list.first = node;
    heap_list.last = node;
}

static bool heap_expand() {
    void *phys_pages = pmm_alloc_pages(HEAP_EXPANSION_PAGES);
    if (!phys_pages) {
        return false;
    }
    
    size_t expansion_size = HEAP_EXPANSION_PAGES * PAGE_SIZE;
    
    uint8_t *new_virt = heap_current + heap_size;
    
    if (!vmm_map_range(kernel_page_table, (uint64_t)new_virt, (uint64_t)phys_pages, HEAP_EXPANSION_PAGES, PTE_WRITABLE)) {
        pmm_free_pages(phys_pages, HEAP_EXPANSION_PAGES);
        return false;
    }
    
    struct linked_list_node *new_node = (struct linked_list_node *)new_virt;
    new_node->prev = heap_list.last;
    new_node->next = NULL;
    new_node->size = expansion_size - sizeof(struct linked_list_node);
    new_node->used = false;
    
    if (heap_list.last) {
        if (!heap_list.last->used) {
            uintptr_t last_end = (uintptr_t)heap_list.last + sizeof(struct linked_list_node) + heap_list.last->size;
            if (last_end == (uintptr_t)new_node) {
                heap_list.last->size += expansion_size;
                heap_size += expansion_size;
                return true;
            }
        }
        heap_list.last->next = new_node;
    }
    heap_list.last = new_node;
    
    if (!heap_list.first) {
        heap_list.first = new_node;
    }
    
    heap_size += expansion_size;
    
    return true;
}

static inline void* node_to_data(struct linked_list_node *node) {
    return (void*)((uintptr_t)node + sizeof(struct linked_list_node));
}

static inline struct linked_list_node* data_to_node(void *ptr) {
    return (struct linked_list_node*)((uintptr_t)ptr - sizeof(struct linked_list_node));
}

void *kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    const size_t alignment = 16;
    size = (size + (alignment - 1)) & ~(alignment - 1);

    for (struct linked_list_node *node = heap_list.first; node; node = node->next) {
        if (!node->used && node->size >= size) {
            size_t remaining = node->size - size;
            
            if (remaining >= sizeof(struct linked_list_node) + alignment) {
                struct linked_list_node *new_node = 
                    (struct linked_list_node*)((uintptr_t)node + sizeof(struct linked_list_node) + size);
                new_node->prev = node;
                new_node->next = node->next;
                if (new_node->next) {
                    new_node->next->prev = new_node;
                } else {
                    heap_list.last = new_node;
                }
                new_node->size = remaining - sizeof(struct linked_list_node);
                new_node->used = false;

                node->next = new_node;
                node->size = size;
            }

            node->used = true;
            return node_to_data(node);
        }
    }

    if (heap_expand()) {
        return kmalloc(size);
    }

    return NULL;
}

void *kcalloc(size_t num, size_t size) {
    size_t total = num * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        uint8_t *byte_ptr = (uint8_t *)ptr;
        for (size_t i = 0; i < total; i++) {
            byte_ptr[i] = 0;
        }
    }
    return ptr;
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) {
        return kmalloc(new_size);
    }
    
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    struct linked_list_node *node = data_to_node(ptr);
    if (!node || !node->used) {
        return NULL;
    }
    
    if (node->size >= new_size) {
        return ptr;
    }
    
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) {
        return NULL;
    }
    
    uint8_t *src = (uint8_t *)ptr;
    uint8_t *dst = (uint8_t *)new_ptr;
    for (size_t i = 0; i < node->size; i++) {
        dst[i] = src[i];
    }
    
    kfree(ptr);
    
    return new_ptr;
}

void kfree(void *ptr) {
    if (!ptr) return;
    
    struct linked_list_node *node = data_to_node(ptr);
    if (!node || !node->used) return;

    node->used = false;

    if (node->prev && !node->prev->used) {
        struct linked_list_node *prev = node->prev;
        prev->size += sizeof(struct linked_list_node) + node->size;
        prev->next = node->next;
        if (node->next) {
            node->next->prev = prev;
        } else {
            heap_list.last = prev;
        }
        node = prev;
    }

    if (node->next && !node->next->used) {
        struct linked_list_node *next = node->next;
        node->size += sizeof(struct linked_list_node) + next->size;
        node->next = next->next;
        if (next->next) {
            next->next->prev = node;
        } else {
            heap_list.last = node;
        }
    }
}

void heap_get_stats(size_t *total, size_t *used, size_t *free) {
    if (total) *total = heap_size;
    
    size_t used_bytes = 0;
    for (struct linked_list_node *node = heap_list.first; node; node = node->next) {
        if (node->used) {
            used_bytes += node->size + sizeof(struct linked_list_node);
        }
    }
    
    if (used) *used = used_bytes;
    if (free) *free = heap_size - used_bytes;
}