#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Intrusive doubly-linked list. Nodes embed in the objects they link; every
// operation is O(1) and removal never scans. A node lives on exactly one
// list at a time.

struct list_node {
    struct list_node *next;
    struct list_node *prev;
};

struct list {
    struct list_node *head;
    struct list_node *tail;
    uint64_t count;
};

#define container_of(ptr, type, member) \
    ((type *)((uintptr_t)(ptr) - offsetof(type, member)))

#define list_foreach(l, n) \
    for (struct list_node *n = (l)->head; n; n = n->next)

static inline void list_init(struct list *l) {
    l->head = NULL;
    l->tail = NULL;
    l->count = 0;
}

static inline void list_push_back(struct list *l, struct list_node *n) {
    n->next = NULL;
    n->prev = l->tail;
    if (l->tail) {
        l->tail->next = n;
    } else {
        l->head = n;
    }
    l->tail = n;
    l->count++;
}

static inline struct list_node *list_pop_front(struct list *l) {
    struct list_node *n = l->head;
    if (!n) {
        return NULL;
    }
    l->head = n->next;
    if (l->head) {
        l->head->prev = NULL;
    } else {
        l->tail = NULL;
    }
    n->next = NULL;
    n->prev = NULL;
    l->count--;
    return n;
}

static inline void list_remove(struct list *l, struct list_node *n) {
    if (n->prev) {
        n->prev->next = n->next;
    } else {
        l->head = n->next;
    }
    if (n->next) {
        n->next->prev = n->prev;
    } else {
        l->tail = n->prev;
    }
    n->next = NULL;
    n->prev = NULL;
    l->count--;
}

static inline bool list_empty(const struct list *l) {
    return l->head == NULL;
}
