#ifndef ELF_H
#define ELF_H

#include <stdint.h>

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

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Word  sh_name;
    Elf64_Word  sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr  sh_addr;
    Elf64_Off   sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word  sh_link;
    Elf64_Word  sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;

typedef struct elf_hdr {
    unsigned char elf_ident[16];
    Elf64_Half elf_type;
    Elf64_Half elf_machine_type;
    Elf64_Word elf_version;
    Elf64_Addr elf_entry;
    Elf64_Off elf_phoff;
    Elf64_Off elf_shoff;
    Elf64_Word elf_flags;
    Elf64_Half elf_ehsize;
    Elf64_Half elf_phentsize;
    Elf64_Half elf_phnum;
    Elf64_Half elf_shentsize;
    Elf64_Half elf_shnum;
    Elf64_Half elf_shstrndx;
} elf_hdr_t;

typedef struct elf_file {
    elf_hdr_t header;
    uint8_t *contents;
    uint32_t file_size;
    Elf64_Phdr *program_headers;
    Elf64_Shdr *section_headers;
} elf_file_t;

elf_file_t *read_elf_header(const char *filename);
void free_elf(elf_file_t *elf);
void read_elf_sections(elf_file_t *elf);
uint8_t execute_elf(const char *filename);

#endif