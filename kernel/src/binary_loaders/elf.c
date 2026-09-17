#include <stdint.h>
#include <mm/memory.h>
#include <mm/hhdm.h>
#include <mm/frame.h>
#include <mm/heap.h>
#include <mm/page.h>
#include <fs/vfs.h>
#include <logging/print.h>
#include <binary_loaders/elf.h>

uint64_t elf64_parse(int fd, uintptr_t cr3, struct elf64_load_info *info) {
    Elf64_Ehdr eh;
    if (vfs_seek(fd, 0, VFS_SEEK_SET) < 0)
        return 0;
    if (vfs_read(fd, &eh, sizeof(eh)) != sizeof(eh))
        return 0;

    if (eh.e_ident[EI_MAG0] != ELFMAG0 ||
        eh.e_ident[EI_MAG1] != ELFMAG1 ||
        eh.e_ident[EI_MAG2] != ELFMAG2 ||
        eh.e_ident[EI_MAG3] != ELFMAG3)
        return 0;

    if (eh.e_ident[EI_CLASS] != ELFCLASS64)
        return 0;

    uint64_t phnum = eh.e_phnum;
    if (phnum == 0)
        return 0;

    Elf64_Phdr *ph = kmalloc(phnum * sizeof(Elf64_Phdr));
    if (!ph)
        return 0;

    if (vfs_seek(fd, eh.e_phoff, VFS_SEEK_SET) < 0) {
        kfree(ph);
        return 0;
    }
    if (vfs_read(fd, ph, phnum * sizeof(Elf64_Phdr)) != (int)(phnum * sizeof(Elf64_Phdr))) {
        kfree(ph);
        return 0;
    }

    uint64_t *pml4 = (uint64_t *)cr3;

    for (uint64_t i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;

        uint64_t seg_vaddr   = ph[i].p_vaddr;
        uint64_t seg_offset  = ph[i].p_offset;
        uint64_t filesz      = ph[i].p_filesz;
        uint64_t memsz       = ph[i].p_memsz;
        uint64_t page_base   = seg_vaddr & ~0xFFFULL;
        uint64_t page_offset = seg_vaddr & 0xFFFULL;
        uint64_t to_cover    = page_offset + memsz;
        uint64_t pages       = (to_cover + 0xFFF) / 0x1000;

        uint64_t flags = PAGE_PRESENT | PAGE_USER;
        if (ph[i].p_flags & PF_W)
            flags |= PAGE_WRITABLE;
        if (!(ph[i].p_flags & PF_X))
            flags |= PAGE_NXE;

        for (uint64_t p = 0; p < pages; p++) {
            uint64_t va = page_base + (p * 0x1000);
            uintptr_t phys = frame_alloc();
            if (!phys) {
                kfree(ph);
                return 0;
            }

            paging_map_page(pml4, (void *)va, phys, flags);

            void *dst = (void *)phys_to_virt(phys);
            memset(dst, 0, 0x1000);

            if (va + 0x1000 > seg_vaddr && filesz > 0) {
                uint64_t dst_off = (va < seg_vaddr) ? (seg_vaddr - va) : 0;
                uint64_t seg_file_off = (va < seg_vaddr) ? 0 : (va - seg_vaddr);

                if (seg_file_off < filesz) {
                    uint64_t avail = filesz - seg_file_off;
                    uint64_t space = 0x1000 - dst_off;
                    uint64_t copy  = avail > space ? space : avail;

                    if (vfs_seek(fd, seg_offset + seg_file_off, VFS_SEEK_SET) < 0) {
                        kfree(ph);
                        return 0;
                    }
                    if (vfs_read(fd, (uint8_t *)dst + dst_off, copy) != copy) {
                        kfree(ph);
                        return 0;
                    }
                }
            }
        }
    }

    uint64_t entry = eh.e_entry;

    if (info) {
        info->entry = entry;
        info->phent = eh.e_phentsize;
        info->phnum = (uint64_t)phnum;
        info->phdr = 0;
        for (uint64_t i = 0; i < phnum; i++) {
            if (ph[i].p_type != PT_LOAD)
                continue;
            if (eh.e_phoff >= ph[i].p_offset &&
                eh.e_phoff < ph[i].p_offset + ph[i].p_filesz) {
                info->phdr = ph[i].p_vaddr + (eh.e_phoff - ph[i].p_offset);
                break;
            }
        }
    }

    kfree(ph);
    return entry;
}
