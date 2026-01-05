#include <elf.h>
#include <ramdisk/fat12.h>
#include <x86_64/serial.h>
#include <x86_64/allocator/heap.h>
#include <x86_64/memory/vmm.h>
#include <x86_64/memory/pmm.h>
#include <string.h>

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_TLS     7

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

#define ALIGN_DOWN(addr, align) ((addr) & ~((align) - 1))
#define ALIGN_UP(addr, align) (((addr) + (align) - 1) & ~((align) - 1))

extern page_table_t kernel_pml4;

elf_file_t *read_elf_header(const char *filename) {
    dir_entry_t *file = find_file(filename);
    uint32_t size;
    uint8_t *contents = read_file(file, &size);
    
    serial_print("File size: ");
    serial_print_hex(size);
    serial_print("\n");
    
    elf_file_t *elf = kmalloc(sizeof(elf_file_t));
    elf->contents = contents;
    elf->file_size = size;
    
    serial_print("ELF Magic: ");
    for (int i = 0; i < 16; i++) {
        elf->header.elf_ident[i] = contents[i];
        serial_print_hex(elf->header.elf_ident[i]);
        serial_print(" ");
    }
    serial_print("\n");
    
    if (elf->header.elf_ident[0] != 0x7F || elf->header.elf_ident[1] != 'E' || 
        elf->header.elf_ident[2] != 'L' || elf->header.elf_ident[3] != 'F') {
        serial_print("Not a valid elf\n");
        kfree(contents);
        kfree(elf);
        return NULL;
    }
    
    if (elf->header.elf_ident[4] != 0x02) {
        serial_print("Error: Not an elf64 executable\n");
        kfree(contents);
        kfree(elf);
        return NULL;
    }
    
    elf->header.elf_type = *(Elf64_Half *)(contents + 16);
    elf->header.elf_machine_type = *(Elf64_Half *)(contents + 18);
    elf->header.elf_version = *(Elf64_Word *)(contents + 20);
    elf->header.elf_entry = *(Elf64_Addr *)(contents + 24);
    elf->header.elf_phoff = *(Elf64_Off *)(contents + 32);
    elf->header.elf_shoff = *(Elf64_Off *)(contents + 40);
    elf->header.elf_flags = *(Elf64_Word *)(contents + 48);
    elf->header.elf_ehsize = *(Elf64_Half *)(contents + 52);
    elf->header.elf_phentsize = *(Elf64_Half *)(contents + 54);
    elf->header.elf_phnum = *(Elf64_Half *)(contents + 56);
    elf->header.elf_shentsize = *(Elf64_Half *)(contents + 58);
    elf->header.elf_shnum = *(Elf64_Half *)(contents + 60);
    elf->header.elf_shstrndx = *(Elf64_Half *)(contents + 62);
    
    serial_print("ELF Type: ");
    serial_print_hex(elf->header.elf_type);
    serial_print("\n");
    
    serial_print("Machine Type: ");
    serial_print_hex(elf->header.elf_machine_type);
    serial_print("\n");
    
    serial_print("Version: ");
    serial_print_hex(elf->header.elf_version);
    serial_print("\n");
    
    serial_print("Entry Point: ");
    serial_print_hex(elf->header.elf_entry);
    serial_print("\n");
    
    serial_print("Program Header Offset: ");
    serial_print_hex(elf->header.elf_phoff);
    serial_print("\n");
    
    serial_print("Section Header Offset: ");
    serial_print_hex(elf->header.elf_shoff);
    serial_print("\n");
    
    serial_print("Flags: ");
    serial_print_hex(elf->header.elf_flags);
    serial_print("\n");
    
    serial_print("ELF Header Size: ");
    serial_print_hex(elf->header.elf_ehsize);
    serial_print("\n");
    
    serial_print("Program Header Entry Size: ");
    serial_print_hex(elf->header.elf_phentsize);
    serial_print("\n");
    
    serial_print("Number of Program Headers: ");
    serial_print_hex(elf->header.elf_phnum);
    serial_print("\n");
    
    serial_print("Section Header Entry Size: ");
    serial_print_hex(elf->header.elf_shentsize);
    serial_print("\n");
    
    serial_print("Number of Section Headers: ");
    serial_print_hex(elf->header.elf_shnum);
    serial_print("\n");
    
    serial_print("Section Header String Index: ");
    serial_print_hex(elf->header.elf_shstrndx);
    serial_print("\n");
    
    elf->program_headers = (Elf64_Phdr *)(contents + elf->header.elf_phoff);
    elf->section_headers = (Elf64_Shdr *)(contents + elf->header.elf_shoff);
    
    serial_print("Program Headers Address: ");
    serial_print_hex((uint64_t)elf->program_headers);
    serial_print("\n");
    
    serial_print("Section Headers Address: ");
    serial_print_hex((uint64_t)elf->section_headers);
    serial_print("\n");
    
    return elf;
}

void free_elf(elf_file_t *elf) {
    if (elf) {
        if (elf->contents) {
            kfree(elf->contents);
        }
        kfree(elf);
    }
}

void read_elf_sections(elf_file_t *elf) {
    if (!elf) return;
    
    serial_print("\nSection Headers\n");
    for (int i = 0; i < elf->header.elf_shnum; i++) {
        Elf64_Shdr *shdr = &elf->section_headers[i];
        
        serial_print("Section ");
        serial_print_hex(i);
        serial_print(":\n");
        
        serial_print("  Type: ");
        serial_print_hex(shdr->sh_type);
        serial_print("\n");
        
        serial_print("  Addr: ");
        serial_print_hex(shdr->sh_addr);
        serial_print("\n");
        
        serial_print("  Offset: ");
        serial_print_hex(shdr->sh_offset);
        serial_print("\n");
        
        serial_print("  Size: ");
        serial_print_hex(shdr->sh_size);
        serial_print("\n");
    }
}

uint8_t load_elf(elf_file_t *elf, page_table_t pml4) {
    if (!elf) {
        serial_print("Invalid ELF\n");
        return 1;
    }
    
    if (!pml4) {
        pml4 = kernel_pml4;
        serial_print("Using kernel address space\n");
    }
    
    serial_print("Free memory before loading: ");
    serial_print_hex(pmm_get_free_memory());
    serial_print("\n");
    
    serial_print("Loading ELF segments into memory...\n");
    
    for (int i = 0; i < elf->header.elf_phnum; i++) {
        Elf64_Phdr *phdr = &elf->program_headers[i];
        
        if (phdr->p_type != PT_LOAD) {
            continue;
        }
        
        serial_print("Loading segment ");
        serial_print_hex(i);
        serial_print("\n");
        
        serial_print("  Virtual Address: ");
        serial_print_hex(phdr->p_vaddr);
        serial_print("\n");
        
        serial_print("  Size in file: ");
        serial_print_hex(phdr->p_filesz);
        serial_print("\n");
        
        serial_print("  Size in memory: ");
        serial_print_hex(phdr->p_memsz);
        serial_print("\n");
        
        uint64_t virt_start = ALIGN_DOWN(phdr->p_vaddr, PAGE_SIZE);
        uint64_t virt_end = ALIGN_UP(phdr->p_vaddr + phdr->p_memsz, PAGE_SIZE);
        size_t num_pages = (virt_end - virt_start) / PAGE_SIZE;
        
        serial_print("  Mapping ");
        serial_print_hex(num_pages);
        serial_print(" pages\n");
        
        uint64_t flags = PTE_PRESENT | PTE_WRITABLE;
        
        for (size_t page = 0; page < num_pages; page++) {
            uint64_t virt_addr = virt_start + (page * PAGE_SIZE);
            
            void *phys_page = pmm_alloc();
            if (!phys_page) {
                serial_print("Failed to allocate physical memory at page ");
                serial_print_hex(page);
                serial_print("\n");
                return 1;
            }
            
            uint64_t phys_addr = (uint64_t)phys_page;
            
            if (!vmm_map_page(pml4, virt_addr, phys_addr, flags)) {
                serial_print("Failed to map page at ");
                serial_print_hex(virt_addr);
                serial_print("\n");
                pmm_free(phys_page);
                return 1;
            }
            
            void *page_virt = phys_to_virt(phys_addr);
            memset(page_virt, 0, PAGE_SIZE);
        }
        
        if (phdr->p_filesz > 0) {
            serial_print("  Copying ");
            serial_print_hex(phdr->p_filesz);
            serial_print(" bytes\n");
            
            uint8_t *src = elf->contents + phdr->p_offset;
            
            for (size_t offset = 0; offset < phdr->p_filesz; offset += PAGE_SIZE) {
                uint64_t virt_addr = phdr->p_vaddr + offset;
                uint64_t phys_addr = vmm_virt_to_phys(pml4, virt_addr);
                
                if (phys_addr == 0) {
                    serial_print("Failed to translate virtual address ");
                    serial_print_hex(virt_addr);
                    serial_print("\n");
                    return 1;
                }
                
                void *dest = phys_to_virt(phys_addr);
                
                size_t copy_size = PAGE_SIZE;
                if (offset + PAGE_SIZE > phdr->p_filesz) {
                    copy_size = phdr->p_filesz - offset;
                }
                
                size_t page_offset = (phdr->p_vaddr + offset) & (PAGE_SIZE - 1);
                memcpy((uint8_t*)dest + page_offset, src + offset, copy_size);
            }
        }
        
        serial_print("  Segment loaded successfully\n");
    }
    
    flush_tlb();
    serial_print("All segments loaded\n");
    
    serial_print("Free memory after loading: ");
    serial_print_hex(pmm_get_free_memory());
    serial_print("\n");
    
    return 0;
}

uint8_t execute_elf(const char *filename) {
    serial_print("Executing ELF: ");
    serial_print(filename);
    serial_print("\n");
    
    elf_file_t *elf = read_elf_header(filename);
    if (!elf) {
        serial_print("Failed to load ELF\n");
        return 1;
    }
    
    if (load_elf(elf, NULL) != 0) {
        serial_print("Failed to load ELF into memory\n");
        free_elf(elf);
        return 1;
    }
    
    uint64_t entry_point = elf->header.elf_entry;
    
    serial_print("Jumping to entry point: ");
    serial_print_hex(entry_point);
    serial_print("\n");
    
    void (*entry)() = (void(*)())entry_point;
    entry();
    
    serial_print("ELF execution completed\n");
    
    free_elf(elf);
    
    return 0;
}