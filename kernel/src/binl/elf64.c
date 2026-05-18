#include <stdint.h>
#include <mem.h>
#include <mm/hhdm.h>
#include <mm/frame.h>
#include <mm/paging.h>
#include <binl/elf64.h>

#define PF_X 1
#define PF_W 2
#define PF_R 4

uint64_t elf64_parse(void *elf, paddr cr3) {
    Elf64_Ehdr *eh = (Elf64_Ehdr *)elf;

    if (eh->e_ident[EI_MAG0] != ELFMAG0 ||
        eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 ||
        eh->e_ident[EI_MAG3] != ELFMAG3)
        return 0;

    if (eh->e_ident[EI_CLASS] != ELFCLASS64)
        return 0;

    uint64_t *pml4 = (uint64_t *)(offset + cr3);
    Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)elf + eh->e_phoff);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;

        uint64_t seg_vaddr   = ph[i].p_vaddr;
        uint64_t seg_offset  = ph[i].p_offset;
        uint64_t filesz      = ph[i].p_filesz;
        uint64_t memsz       = ph[i].p_memsz;
        uint64_t page_base   = seg_vaddr & ~0xFFFULL;
        uint64_t page_offset = seg_vaddr & 0xFFFULL;
        uint64_t to_cover    = page_offset + memsz;
        uint64_t pages       = (to_cover + 0xFFF) / 0x1000;

        uint64_t flags = ENTRY_FLAG_PRESENT | ENTRY_FLAG_USER;
        if (ph[i].p_flags & PF_W) flags |= ENTRY_FLAG_RW;
        if (!(ph[i].p_flags & PF_X)) flags |= ENTRY_FLAG_NX;

        for (uint64_t p = 0; p < pages; p++) {
            uint64_t va = page_base + p * 0x1000;
            paddr phys  = frame_alloc();
            paging_map_page(pml4, va, phys, 0x1000, flags);

            void *dst = (void *)(offset + phys);
            memset(dst, 0, 0x1000);

            if (va + 0x1000 > seg_vaddr && filesz > 0) {
                uint64_t seg_file_off = (va < seg_vaddr) ? 0 : (va - seg_vaddr);
                if (seg_file_off < filesz) {
                    uint64_t avail = filesz - seg_file_off;
                    uint64_t c     = avail > 0x1000 ? 0x1000 : avail;
                    memcpy(dst + ((va < seg_vaddr) ? (seg_vaddr - va) : 0),
                           (uint8_t *)elf + seg_offset + seg_file_off,
                           (size_t)c);
                }
            }
        }
    }

    return eh->e_entry;
}