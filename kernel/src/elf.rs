use framebuffer::println;
use elf_parser::elf64::Elf64;
use vfs::{vfs_open, vfs_read, vfs_close, vfs_stat, O_RDONLY};
use memory::vmm::VMM;
use memory::paging::PageTableEntry;
use memory::addr::VirtAddr;
use memory::frame_allocator::FrameAllocator;

const PAGE_SIZE: usize = 0x1000;
const HHDM_OFFSET: u64 = 0xFFFF800000000000;
const PT_LOAD: u32 = 1;

pub fn load_elf(filename: &str) -> Option<(u64, u64)> {
    let stat = match vfs_stat(filename) {
        Ok(s) => s,
        Err(_) => {
            println!("File does not exist: {}", filename);
            return None;
        }
    };

    let fd = match vfs_open(filename, O_RDONLY, 0) {
        Ok(id) => id,
        Err(e) => {
            println!("Failed to open {}: {:?}", filename, e);
            return None;
        }
    };

    let contents = match vfs_read(fd, stat.size as usize) {
        Ok(data) => data,
        Err(e) => {
            println!("Failed to read {}: {:?}", filename, e);
            let _ = vfs_close(fd);
            return None;
        }
    };
    let _ = vfs_close(fd);

    if contents.len() < 4 || &contents[0..4] != b"\x7fELF" {
        println!("{} is not a valid ELF file.", filename);
        return None;
    }

    let elf = match Elf64::from_bytes(&contents) {
        Ok(e) => e,
        Err(e) => {
            println!("Failed to parse ELF {}: {:?}", filename, e);
            return None;
        }
    };

    let ehdr = elf.ehdr();
    let mut mapper = VMM::get_kernel_mapper();
    let pml4 = mapper.create_user_pml4()?;

    for ph in elf.phdr_iter() {
        if ph.p_type != PT_LOAD {
            continue;
        }

        let vaddr = ph.p_vaddr as usize;
        let filesz = ph.p_filesz as usize;
        let memsz = ph.p_memsz as usize;
        let offset = ph.p_offset as usize;

        if offset + filesz > contents.len() {
            println!("Segment at {:#x} exceeds file bounds.", offset);
            return None;
        }

        let mut flags = PageTableEntry::USER | PageTableEntry::PRESENT;
        if ph.p_flags & 0x2 != 0 {
            flags |= PageTableEntry::WRITABLE;
        }
        if ph.p_flags & 0x1 == 0 {
            flags |= PageTableEntry::NO_EXECUTE;
        }

        let virt_base = (vaddr & !(PAGE_SIZE - 1)) as u64;
        let page_offset = vaddr & (PAGE_SIZE - 1);
        let pages = (memsz + page_offset + PAGE_SIZE - 1) / PAGE_SIZE;

        for i in 0..pages {
            let frame = FrameAllocator::alloc_frame()?;
            let virt = VirtAddr::new(virt_base + (i * PAGE_SIZE) as u64);
            mapper.map_page(pml4, virt, frame, flags)?;

            let dest = (frame.as_u64() + HHDM_OFFSET) as *mut u8;

            let page_start_vaddr = virt_base as usize + i * PAGE_SIZE;
            let page_end_vaddr = page_start_vaddr + PAGE_SIZE;
            let file_start_vaddr = vaddr;
            let file_end_vaddr = vaddr + filesz;

            let copy_start = page_start_vaddr.max(file_start_vaddr);
            let copy_end = page_end_vaddr.min(file_end_vaddr);

            unsafe {
                core::ptr::write_bytes(dest, 0, PAGE_SIZE);
                if copy_start < copy_end {
                    let dest_off = copy_start - page_start_vaddr;
                    let src_off = offset + (copy_start - file_start_vaddr);
                    let len = copy_end - copy_start;
                    core::ptr::copy_nonoverlapping(
                        contents[src_off..src_off + len].as_ptr(),
                        dest.add(dest_off),
                        len,
                    );
                }
            }
        }
    }

    let entry = ehdr.e_entry;
    let pml4_phys = pml4 as u64;
    Some((entry, pml4_phys))
}