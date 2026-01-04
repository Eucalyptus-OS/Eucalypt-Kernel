#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

void pmm_init();
void* pmm_alloc();
void* pmm_alloc_pages(size_t count);
void pmm_free(void* ptr);
void pmm_free_pages(void* ptr, size_t count);
uint64_t pmm_get_total_memory();
uint64_t pmm_get_free_memory();

#endif