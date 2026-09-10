#include <drivers/elf.h>
#include <multitasking/proc.h>
#include <mm/paging.h>
#include <mm/hhdm.h>
#include <mm/vmm.h>
#include <memory.h>
#include <stddef.h>
#include <stdint.h>

void elf_fill_segment(struct pcb *p, void *elf,
                      struct elf64_phdr *ph) {
    uint64_t *pml4 = phys_to_virt((uintptr_t)p->space.pml4);
    uint64_t page_start = ph->p_vaddr & ~(PAGE_SIZE - 1);
    uint64_t data_end = ph->p_vaddr + ph->p_filesz;
    uint64_t seg_end = ph->p_vaddr + ph->p_memsz;

    for (uint64_t va = page_start; va < seg_end; va += PAGE_SIZE) {
        uintptr_t frame = vmm_walk_phys(pml4, (void *)va);
        if (!frame) {
            continue;
        }
        uint8_t *dst = phys_to_virt(frame);

        uint64_t copy_start = va > ph->p_vaddr ? va : ph->p_vaddr;
        if (copy_start < data_end) {
            uint64_t copy_end = va + PAGE_SIZE < data_end ? va + PAGE_SIZE : data_end;
            uint64_t off = copy_start - ph->p_vaddr;
            memcpy(dst + (copy_start - va),
                   (uint8_t *)elf + ph->p_offset + off,
                   copy_end - copy_start);
        }

        uint64_t zero_start = va > data_end ? va : data_end;
        if (zero_start < seg_end) {
            uint64_t zero_end = va + PAGE_SIZE < seg_end ? va + PAGE_SIZE : seg_end;
            memset(dst + (zero_start - va), 0, zero_end - zero_start);
        }
    }
}

int elf_load(struct pcb *p, void *elf, uintptr_t size, void **entry) {
    struct elf64_hdr *h = (struct elf64_hdr *)elf;
    if (size < sizeof(struct elf64_hdr)) {
        return -1;
    }
    if (h->e_ident[0] != 0x7F || h->e_ident[1] != 'E' ||
        h->e_ident[2] != 'L' || h->e_ident[3] != 'F') {
        return -1;
    }
    if (h->e_ident[4] != ELFCLASS64 || h->e_type != ET_EXEC) {
        return -1;
    }
    if ((uint64_t)h->e_phoff + (uint64_t)h->e_phnum * h->e_phentsize > size) {
        return -1;
    }

    struct vm_region *mapped[64];
    int nmap = 0;

    for (uint64_t i = 0; i < h->e_phnum; i++) {
        struct elf64_phdr *ph = (struct elf64_phdr *)((uint8_t *)elf + h->e_phoff + i * h->e_phentsize);
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (nmap >= 64 || (uint64_t)ph->p_offset + ph->p_filesz > size) {
            goto fail;
        }

        uint64_t base = ph->p_vaddr & ~(PAGE_SIZE - 1);
        uint64_t end_va = ph->p_vaddr + ph->p_memsz;
        uint64_t end_aligned = (end_va + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        int pages = (end_aligned - base) / PAGE_SIZE;

        uint64_t flags = 0;
        if (ph->p_flags & PF_W) {
            flags |= PAGE_WRITABLE;
        }
        if (!(ph->p_flags & PF_X)) {
            flags |= PAGE_NXE;
        }

        if (vmm_find_region(&p->space, base)) {
            continue;
        }
        if (!vmm_map_at(&p->space, (void *)base, flags, pages)) {
            goto fail;
        }
        mapped[nmap++] = vmm_find_region(&p->space, base);
        elf_fill_segment(p, elf, ph);
    }

    *entry = (void *)h->e_entry;
    return 0;

fail:
    for (int k = 0; k < nmap; k++) {
        vmm_free_region(&p->space, mapped[k]);
    }
    return -1;
}