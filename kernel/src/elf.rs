use alloc::string::String;
use alloc::vec::Vec;
use fat12::fat12_read_file;
use serial::serial_println;

#[repr(C)]
struct ElfHeader {
    e_ident: [u8; 16],
    e_type: u16,
    e_machine: u16,
    e_version: u32,
    e_entry: u64,
    e_phoff: u64,
    e_shoff: u64,
    e_flags: u32,
    e_ehsize: u16,
    e_phentsize: u16,
    e_phnum: u16,
    e_shentsize: u16,
    e_shnum: u16,
    e_shstrndx: u16,
}

#[repr(C)]
struct ProgramHeader {
    p_type: u32,
    p_flags: u32,
    p_offset: u64,
    p_vaddr: u64,
    p_paddr: u64,
    p_filesz: u64,
    p_memsz: u64,
    p_align: u64,
}

#[repr(C)]
struct SectionHeader {
    sh_name: u32,
    sh_type: u32,
    sh_flags: u64,
    sh_addr: u64,
    sh_offset: u64,
    sh_size: u64,
    sh_link: u32,
    sh_info: u32,
    sh_addralign: u64,
    sh_entsize: u64,
}

const SHT_NULL: u32 = 0;
const SHT_PROGBITS: u32 = 1;
const SHT_SYMTAB: u32 = 2;
const SHT_STRTAB: u32 = 3;
const SHT_RELA: u32 = 4;
const SHT_NOBITS: u32 = 8;

fn section_type_name(sh_type: u32) -> &'static str {
    match sh_type {
        SHT_NULL => "NULL",
        SHT_PROGBITS => "PROGBITS",
        SHT_SYMTAB => "SYMTAB",
        SHT_STRTAB => "STRTAB",
        SHT_RELA => "RELA",
        SHT_NOBITS => "NOBITS",
        _ => "UNKNOWN",
    }
}

fn read_file_contents(filename: &str) -> Result<Vec<u8>, String> {
    Ok(fat12_read_file(filename)?)
}

fn is_elf(contents: &[u8]) -> bool {
    contents.starts_with(b"\x7fELF")
}

fn read_u16_le(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes(bytes[offset..offset + 2].try_into().unwrap())
}

fn read_u32_le(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

fn read_u64_le(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(bytes[offset..offset + 8].try_into().unwrap())
}

/// Read a null-terminated string from a byte slice at the given offset
fn read_cstr(bytes: &[u8], offset: usize) -> &str {
    let start = &bytes[offset..];
    let len = start.iter().position(|&b| b == 0).unwrap_or(start.len());
    core::str::from_utf8(&start[..len]).unwrap_or("<invalid utf8>")
}

/// parses the elf header
fn parse_elf_header(bytes: &[u8]) -> ElfHeader {
    let mut e_ident = [0u8; 16];
    e_ident.copy_from_slice(&bytes[0..16]);

    ElfHeader {
        e_ident,
        e_type: read_u16_le(bytes, 16),
        e_machine: read_u16_le(bytes, 18),
        e_version: read_u32_le(bytes, 20),
        e_entry: read_u64_le(bytes, 24),
        e_phoff: read_u64_le(bytes, 32),
        e_shoff: read_u64_le(bytes, 40),
        e_flags: read_u32_le(bytes, 48),
        e_ehsize: read_u16_le(bytes, 52),
        e_phentsize: read_u16_le(bytes, 54),
        e_phnum: read_u16_le(bytes, 56),
        e_shentsize: read_u16_le(bytes, 58),
        e_shnum: read_u16_le(bytes, 60),
        e_shstrndx: read_u16_le(bytes, 62),
    }
}

/// parses the program header
fn parse_program_header(bytes: &[u8], offset: usize) -> ProgramHeader {
    let b = &bytes[offset..];
    ProgramHeader {
        p_type: read_u32_le(b, 0),
        p_flags: read_u32_le(b, 4),
        p_offset: read_u64_le(b, 8),
        p_vaddr: read_u64_le(b, 16),
        p_paddr: read_u64_le(b, 24),
        p_filesz: read_u64_le(b, 32),
        p_memsz: read_u64_le(b, 40),
        p_align: read_u64_le(b, 48),
    }
}

/// parses the section header
fn parse_section_header(bytes: &[u8], offset: usize) -> SectionHeader {
    let b = &bytes[offset..];
    SectionHeader {
        sh_name: read_u32_le(b, 0),
        sh_type: read_u32_le(b, 4),
        sh_flags: read_u64_le(b, 8),
        sh_addr: read_u64_le(b, 16),
        sh_offset: read_u64_le(b, 24),
        sh_size: read_u64_le(b, 32),
        sh_link: read_u32_le(b, 40),
        sh_info: read_u32_le(b, 44),
        sh_addralign: read_u64_le(b, 48),
        sh_entsize: read_u64_le(b, 56),
    }
}

/// Parse an ELF binary
pub fn parse_elf(filename: &str) {
    match read_file_contents(filename) {
        Ok(contents) => {
            if !is_elf(&contents) {
                serial_println!("{} is not an ELF file.", filename);
                return;
            }
            if contents.len() < core::mem::size_of::<ElfHeader>() {
                serial_println!("File too small to be a valid ELF");
                return;
            }

            let header = parse_elf_header(&contents);

            serial_println!("--- ELF Header ---");
            serial_println!("  Entry point:     {:#x}", header.e_entry);
            serial_println!("  Program headers: {}", header.e_phnum);
            serial_println!("  Section headers: {}", header.e_shnum);
            serial_println!("  ELF type:        {}", header.e_type);
            serial_println!("  Machine:         {}", header.e_machine);
            serial_println!("  Version:         {}", header.e_version);
            serial_println!("  Flags:           {:#x}", header.e_flags);
            serial_println!("  ELF header size: {}", header.e_ehsize);
            serial_println!("  PH entry size:   {}", header.e_phentsize);
            serial_println!("  SH entry size:   {}", header.e_shentsize);
            serial_println!("  SH str index:    {}", header.e_shstrndx);
            serial_println!("  PH offset:       {:#x}", header.e_phoff);
            serial_println!("  SH offset:       {:#x}", header.e_shoff);

            serial_println!("--- Program Headers ---");
            for i in 0..header.e_phnum as usize {
                let offset = header.e_phoff as usize + i * header.e_phentsize as usize;
                if offset + header.e_phentsize as usize > contents.len() {
                    serial_println!("  Warning: program header {} out of bounds, stopping", i);
                    break;
                }
                let ph = parse_program_header(&contents, offset);
                let type_name = match ph.p_type {
                    1 => "PT_LOAD",
                    2 => "PT_DYNAMIC",
                    3 => "PT_INTERP",
                    4 => "PT_NOTE",
                    6 => "PT_PHDR",
                    7 => "PT_TLS",
                    _ => "PT_OTHER",
                };
                serial_println!(
                    "  [{}] {} vaddr={:#x} offset={:#x} filesz={} memsz={} flags={:#x}",
                    i,
                    type_name,
                    ph.p_vaddr,
                    ph.p_offset,
                    ph.p_filesz,
                    ph.p_memsz,
                    ph.p_flags
                );
            }

            let shstrtab_bytes: Option<&[u8]> = if header.e_shstrndx != 0
                && (header.e_shstrndx as usize) < header.e_shnum as usize
            {
                let strtab_offset = header.e_shoff as usize
                    + header.e_shstrndx as usize * header.e_shentsize as usize;
                if strtab_offset + header.e_shentsize as usize <= contents.len() {
                    let strtab_sh = parse_section_header(&contents, strtab_offset);
                    let start = strtab_sh.sh_offset as usize;
                    let end = start + strtab_sh.sh_size as usize;
                    if end <= contents.len() {
                        Some(&contents[start..end])
                    } else {
                        None
                    }
                } else {
                    None
                }
            } else {
                None
            };

            serial_println!("--- Section Headers ---");
            for i in 0..header.e_shnum as usize {
                let offset = header.e_shoff as usize + i * header.e_shentsize as usize;
                if offset + header.e_shentsize as usize > contents.len() {
                    serial_println!("  Warning: section header {} out of bounds, stopping", i);
                    break;
                }
                let sh = parse_section_header(&contents, offset);

                let name = shstrtab_bytes
                    .filter(|strtab| (sh.sh_name as usize) < strtab.len())
                    .map(|strtab| read_cstr(strtab, sh.sh_name as usize))
                    .unwrap_or("<unknown>");

                serial_println!(
                    "  [{}] {} type={} addr={:#x} offset={:#x} size={} flags={:#x}",
                    i,
                    name,
                    section_type_name(sh.sh_type),
                    sh.sh_addr,
                    sh.sh_offset,
                    sh.sh_size,
                    sh.sh_flags,
                );
            }
        }
        Err(e) => serial_println!("Failed to read {}: {}", filename, e),
    }
}
