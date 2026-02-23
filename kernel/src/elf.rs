use serial::serial_println;
use fat12::fat12_read_file;
use alloc::string::String;
use alloc::vec::Vec;

#[repr(C)]
struct ElfHeader {
    e_ident:     [u8; 16],
    e_type:      u16,
    e_machine:   u16,
    e_version:   u32,
    e_entry:     u64,
    e_phoff:     u64,
    e_shoff:     u64,
    e_flags:     u32,
    e_ehsize:    u16,
    e_phentsize: u16,
    e_phnum:     u16,
    e_shentsize: u16,
    e_shnum:     u16,
    e_shstrndx:  u16,
}

#[repr(C)]
struct ProgramHeader {
    p_type:   u32,
    p_flags:  u32,
    p_offset: u64,
    p_vaddr:  u64,
    p_paddr:  u64,
    p_filesz: u64,
    p_memsz:  u64,
    p_align:  u64,
}

#[repr(C)]
struct SectionHeader {
    sh_name:      u32,
    sh_type:      u32,
    sh_flags:     u64,
    sh_addr:      u64,
    sh_offset:    u64,
    sh_size:      u64,
    sh_link:      u32,
    sh_info:      u32,
    sh_addralign: u64,
    sh_entsize:   u64,
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

fn parse_elf_header(bytes: &[u8]) -> ElfHeader {
    let mut e_ident = [0u8; 16];
    e_ident.copy_from_slice(&bytes[0..16]);

    ElfHeader {
        e_ident,
        e_type:      read_u16_le(bytes, 16),
        e_machine:   read_u16_le(bytes, 18),
        e_version:   read_u32_le(bytes, 20),
        e_entry:     read_u64_le(bytes, 24),
        e_phoff:     read_u64_le(bytes, 32),
        e_shoff:     read_u64_le(bytes, 40),
        e_flags:     read_u32_le(bytes, 48),
        e_ehsize:    read_u16_le(bytes, 52),
        e_phentsize: read_u16_le(bytes, 54),
        e_phnum:     read_u16_le(bytes, 56),
        e_shentsize: read_u16_le(bytes, 58),
        e_shnum:     read_u16_le(bytes, 60),
        e_shstrndx:  read_u16_le(bytes, 62),
    }
}

fn parse_program_header(bytes: &[u8], offset: usize) -> ProgramHeader {
    let b = &bytes[offset..];
    ProgramHeader {
        p_type:   read_u32_le(b, 0),
        p_flags:  read_u32_le(b, 4),
        p_offset: read_u64_le(b, 8),
        p_vaddr:  read_u64_le(b, 16),
        p_paddr:  read_u64_le(b, 24),
        p_filesz: read_u64_le(b, 32),
        p_memsz:  read_u64_le(b, 40),
        p_align:  read_u64_le(b, 48),
    }
}

/// Parse an elf binary
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

            serial_println!("Entry point:     {:#x}", header.e_entry);
            serial_println!("Program headers: {}", header.e_phnum);
            serial_println!("Section headers: {}", header.e_shnum);
            serial_println!("ELF type:        {}", header.e_type);
            serial_println!("Machine:         {}", header.e_machine);
            serial_println!("Version:         {}", header.e_version);
            serial_println!("Flags:           {:#x}", header.e_flags);
            serial_println!("ELF header size: {}", header.e_ehsize);
            serial_println!("PH entry size:   {}", header.e_phentsize);
            serial_println!("SH entry size:   {}", header.e_shentsize);
            serial_println!("SH str index:    {}", header.e_shstrndx);
            serial_println!("PH offset:       {:#x}", header.e_phoff);
            serial_println!("SH offset:       {:#x}", header.e_shoff);

            for i in 0..header.e_phnum as usize {
                let offset = header.e_phoff as usize + i * core::mem::size_of::<ProgramHeader>();
                if offset + core::mem::size_of::<ProgramHeader>() > contents.len() {
                    break;
                }
                let ph = parse_program_header(&contents, offset);
                if ph.p_type == 1 {
                    serial_println!(
                        "PT_LOAD segment: vaddr={:#x} filesz={} memsz={}",
                        ph.p_vaddr, ph.p_filesz, ph.p_memsz
                    );
                }
            }
        }
        Err(e) => serial_println!("Failed to read {}: {}", filename, e),
    }
}