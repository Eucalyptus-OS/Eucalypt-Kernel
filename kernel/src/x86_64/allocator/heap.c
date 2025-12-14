#include <x86_64/allocator/heap.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct linked_list_node {
    struct linked_list_node *next;
    struct linked_list_node *prev;
    void *data;
    size_t size;
    bool used;
};

struct linked_list {
    struct linked_list_node *first;
    struct linked_list_node *last;
};

static struct linked_list heap_list;
static uint8_t *heap_start = NULL;
static size_t heap_size = 0;
static size_t heap_offset = 0;

void linked_list_init(struct linked_list *list) {
    list->first = NULL;
    list->last = NULL;
}

void heap_init(void *start, size_t size) {
    heap_start = (uint8_t *)start;
    heap_size = size;
    heap_offset = 0;
    linked_list_init(&heap_list);
    if (heap_start && heap_size > sizeof(struct linked_list_node)) {
        struct linked_list_node *node = (struct linked_list_node *)heap_start;
        node->prev = NULL;
        node->next = NULL;
        node->data = (void *)((uintptr_t)node + sizeof(struct linked_list_node));
        node->size = heap_size - sizeof(struct linked_list_node);
        node->used = false;
        heap_list.first = node;
        heap_list.last = node;
    }
}

void *kmalloc(size_t size) {
    if (!heap_start || size == 0) {
        return NULL;
    }

    const size_t alignment = 8;
    size = (size + (alignment - 1)) & ~(alignment - 1);

    for (struct linked_list_node *node = heap_list.first; node; node = node->next) {
        if (!node->used && node->size >= size) {
            size_t remaining = node->size - size;
            if (remaining >= sizeof(struct linked_list_node) + 8) {
                uint8_t *new_node_addr = (uint8_t *)node->data + size;
                struct linked_list_node *new_node = (struct linked_list_node *)new_node_addr;
                new_node->prev = node;
                new_node->next = node->next;
                if (new_node->next) {
                    new_node->next->prev = new_node;
                } else {
                    heap_list.last = new_node;
                }
                new_node->data = (void *)((uintptr_t)new_node + sizeof(struct linked_list_node));
                new_node->size = remaining - sizeof(struct linked_list_node);
                new_node->used = false;

                node->next = new_node;
                node->size = size;
            }

            node->used = true;
            node->data = (void *)((uintptr_t)node + sizeof(struct linked_list_node));
            return node->data;
        }
    }

    return NULL;
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct linked_list_node *node = NULL;
    for (struct linked_list_node *n = heap_list.first; n; n = n->next) {
        if (n->data == ptr) {
            node = n;
            break;
        }
    }
    if (!node) return;

    node->used = false;

    if (node->prev && node->prev->used == false) {
        struct linked_list_node *prev = node->prev;
        prev->size += sizeof(struct linked_list_node) + node->size;
        prev->next = node->next;
        if (node->next) node->next->prev = prev;
        else heap_list.last = prev;
        node = prev;
    }

    if (node->next && node->next->used == false) {
        struct linked_list_node *next = node->next;
        node->size += sizeof(struct linked_list_node) + next->size;
        node->next = next->next;
        if (next->next) next->next->prev = node;
        else heap_list.last = node;
    }
}