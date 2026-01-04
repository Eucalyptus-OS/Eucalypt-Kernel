#include <elf.h>
#include <ramdisk/fat12.h>
#include <x86_64/serial.h>
#include <x86_64/allocator/heap.h>
#include <x86_64/memory/vmm.h>

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

uint8_t execute_elf(const char *filename) {
    serial_print("Executing ELF: ");
    serial_print(filename);
    serial_print("\n");
    
    elf_file_t *elf = read_elf_header(filename);
    if (!elf) {
        serial_print("Failed to load ELF\n");
        return 1;
    }
    
    read_elf_sections(elf);
    
    free_elf(elf);
    
    serial_print("ELF execution completed\n");
    return 0;
}