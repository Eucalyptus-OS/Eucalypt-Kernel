use core::sync::atomic::Ordering;
use spin::Mutex;
use limine::request::MemmapResponse;
use crate::paging::KERNEL_PAGE_TABLE;
use crate::frame_allocator::{self, FrameAllocator};
use crate::addr::{PhysAddr, VirtAddr};
use crate::paging::{PageTable, PageTableEntry};

const HHDM_OFFSET: u64 = 0xFFFF800000000000;

static MAPPER_LOCK: Mutex<()> = Mutex::new(());

#[derive(Clone, Copy)]
pub struct Mapper {
    page_table: *mut PageTable,
}

unsafe impl Send for Mapper {}
unsafe impl Sync for Mapper {}

impl Mapper {
    fn get_or_create_table(&self, entry: &mut PageTableEntry) -> Option<*mut PageTable> {
        if entry.is_present() {
            let addr = entry.get_addr().as_u64();
            Some((addr | HHDM_OFFSET) as *mut PageTable)
        } else {
            let frame = FrameAllocator::alloc_frame()?;
            unsafe {
                let table = &mut *((frame.as_u64() | HHDM_OFFSET) as *mut PageTable);
                table.zero();
                entry.set_addr(
                    frame,
                    PageTableEntry::PRESENT | PageTableEntry::WRITABLE | PageTableEntry::USER,
                );
                Some(table as *mut PageTable)
            }
        }
    }

    #[inline]
    fn flush_tlb(virt: VirtAddr) {
        unsafe {
            core::arch::asm!(
                "invlpg [{}]",
                in(reg) virt.as_u64(),
                options(nostack, preserves_flags)
            );
        }
    }

    // walks or creates the 4-level page table structure and maps virt -> phys with flags
    pub fn map_page(
        &self,
        pml4: *mut PageTable,
        virt: VirtAddr,
        phys: PhysAddr,
        flags: u64,
    ) -> Option<()> {
        let _guard = MAPPER_LOCK.lock();
        unsafe {
            let p4 = &mut *((pml4 as u64 | HHDM_OFFSET) as *mut PageTable);
            let p3 = self.get_or_create_table(&mut p4.entries[virt.p4_index()])?;
            let p2 = self.get_or_create_table(&mut (*p3).entries[virt.p3_index()])?;
            let p1 = self.get_or_create_table(&mut (*p2).entries[virt.p2_index()])?;
            (*p1).entries[virt.p1_index()].set_addr(phys, flags | PageTableEntry::PRESENT);
            Self::flush_tlb(virt);
            Some(())
        }
    }

    // clears the pte for virt and returns the physical address it pointed to
    pub fn unmap_page(&self, pml4: *mut PageTable, virt: VirtAddr) -> Option<PhysAddr> {
        let _guard = MAPPER_LOCK.lock();
        unsafe {
            let p4 = &mut *((pml4 as u64 | HHDM_OFFSET) as *mut PageTable);
            if !p4.entries[virt.p4_index()].is_present() { return None; }
            let p3 = (p4.entries[virt.p4_index()].get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable;
            if !(*p3).entries[virt.p3_index()].is_present() { return None; }
            let p2 = ((*p3).entries[virt.p3_index()].get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable;
            if !(*p2).entries[virt.p2_index()].is_present() { return None; }
            let p1 = ((*p2).entries[virt.p2_index()].get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable;
            let entry = &mut (*p1).entries[virt.p1_index()];
            if !entry.is_present() { return None; }
            let phys = entry.get_addr();
            entry.clear();
            Self::flush_tlb(virt);
            Some(phys)
        }
    }

    // maps a contiguous phys range into a contiguous virt range page by page
    pub fn map_range(
        &self,
        pml4: *mut PageTable,
        virt_start: VirtAddr,
        phys_start: PhysAddr,
        size: usize,
        flags: u64,
    ) -> Option<()> {
        let pages = (size + 0xFFF) / 0x1000;
        let mut virt = virt_start.as_u64();
        let mut phys = phys_start.as_u64();
        for _ in 0..pages {
            self.map_page(pml4, VirtAddr::new(virt), PhysAddr::new(phys), flags)?;
            virt += 0x1000;
            phys += 0x1000;
        }
        Some(())
    }

    // unmaps every page in the virt range, ignoring pages that were not mapped
    pub fn unmap_range(&self, pml4: *mut PageTable, virt_start: VirtAddr, size: usize) {
        let pages = (size + 0xFFF) / 0x1000;
        let mut virt = virt_start.as_u64();
        for _ in 0..pages {
            self.unmap_page(pml4, VirtAddr::new(virt));
            virt += 0x1000;
        }
    }

    // scans the user canonical range for a contiguous run of unmapped pages large enough to hold size bytes
    pub fn find_free_virt_region(&self, pml4: *mut PageTable, size: u64) -> Option<u64> {
        let region_start = 0x0000_1000_0000_0000u64;
        let region_end   = 0x0000_7FFF_FFFF_FFFFu64;
        let page_size    = 4096u64;
        let pages_needed = (size + page_size - 1) / page_size;
        let mut candidate = region_start;

        while candidate + size <= region_end {
            let mut conflict = false;
            for i in 0..pages_needed {
                let virt = VirtAddr::new(candidate + i * page_size);
                if self.translate(pml4, virt).is_some() {
                    candidate = candidate + (i + 1) * page_size;
                    conflict = true;
                    break;
                }
            }
            if !conflict {
                return Some(candidate);
            }
        }
        None
    }

    // walks the page table for pml4 and returns the physical address mapped at virt, or None
    pub fn translate(&self, pml4: *mut PageTable, virt: VirtAddr) -> Option<PhysAddr> {
        let _guard = MAPPER_LOCK.lock();
        unsafe {
            let p4 = &*((pml4 as u64 | HHDM_OFFSET) as *mut PageTable);
            if !p4.entries[virt.p4_index()].is_present() { return None; }
            let p3 = (p4.entries[virt.p4_index()].get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable;
            if !(*p3).entries[virt.p3_index()].is_present() { return None; }
            let p2 = ((*p3).entries[virt.p3_index()].get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable;
            if !(*p2).entries[virt.p2_index()].is_present() { return None; }
            let p1 = ((*p2).entries[virt.p2_index()].get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable;
            let entry = &(*p1).entries[virt.p1_index()];
            if !entry.is_present() { return None; }
            Some(entry.get_addr())
        }
    }

    // allocates a fresh pml4 and copies kernel upper-half entries (256-511) from the global kernel table
    pub fn create_user_pml4(&self) -> Option<*mut PageTable> {
        let _guard = MAPPER_LOCK.lock();
        unsafe {
            let frame = FrameAllocator::alloc_frame()?;
            let pml4 = (frame.as_u64() | HHDM_OFFSET) as *mut PageTable;
            (*pml4).zero();
            let kernel_pml4 = &*((KERNEL_PAGE_TABLE.load(Ordering::Acquire) as u64 | HHDM_OFFSET) as *mut PageTable);
            for i in 256..512 {
                (*pml4).entries[i] = kernel_pml4.entries[i];
            }
            Some(frame.as_u64() as *mut PageTable)
        }
    }

    // allocates and zeroes a single page table frame, returning its virtual pointer
    pub fn create_user_page_table(&self) -> Option<*mut PageTable> {
        let _guard = MAPPER_LOCK.lock();
        unsafe {
            let frame = FrameAllocator::alloc_frame()?;
            let table = (frame.as_u64() | HHDM_OFFSET) as *mut PageTable;
            (*table).zero();
            Some(table)
        }
    }

    // frees all user-space page table frames (p3/p2/p1) and the pml4 frame itself
    pub unsafe fn free_user_pml4(&self, pml4_phys: *mut PageTable) {
        let _guard = MAPPER_LOCK.lock();
        unsafe {
            let p4 = &mut *((pml4_phys as u64 | HHDM_OFFSET) as *mut PageTable);
            for i in 0..256 {
                let p3_entry = &mut p4.entries[i];
                if !p3_entry.is_present() { continue; }
                let p3 = (p3_entry.get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable;
                for j in 0..512 {
                    let p2_entry = &mut (*p3).entries[j];
                    if !p2_entry.is_present() { continue; }
                    let p2 = (p2_entry.get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable;
                    for k in 0..512 {
                        let p1_entry = &mut (*p2).entries[k];
                        if !p1_entry.is_present() { continue; }
                        let p1 = (p1_entry.get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable;
                        FrameAllocator::free_frame(PhysAddr::new((*p1).entries[0].get_addr().as_u64()));
                        FrameAllocator::free_frame(p1_entry.get_addr());
                    }
                    FrameAllocator::free_frame(p2_entry.get_addr());
                }
                FrameAllocator::free_frame(p3_entry.get_addr());
            }
            FrameAllocator::free_frame(PhysAddr::new(pml4_phys as u64));
        }
    }

    // switches the active address space by loading pml4's physical address into cr3
    pub fn switch_page_table(&mut self, pml4: *mut PageTable) {
        unsafe {
            let phys = if (pml4 as u64) >= HHDM_OFFSET {
                (pml4 as u64) - HHDM_OFFSET
            } else {
                pml4 as u64
            };
            self.page_table = pml4;
            core::arch::asm!("mov cr3, {}", in(reg) phys, options(nostack, preserves_flags));
        }
    }

    // reads cr3 and returns the physical pml4 pointer with flag bits masked off
    pub fn get_current_page_table() -> *mut PageTable {
        unsafe {
            let mut cr3: u64;
            core::arch::asm!("mov {}, cr3", out(reg) cr3, options(nomem, nostack));
            (cr3 & 0x000F_FFFF_FFFF_F000) as *mut PageTable
        }
    }

    // returns the active pml4 as a mutable reference via the hhdm
    pub unsafe fn active_level_4_table() -> &'static mut PageTable {
        let (frame, _) = x86_64::registers::control::Cr3::read();
        let virt = VirtAddr::new(frame.start_address().as_u64() + HHDM_OFFSET);
        unsafe { &mut *(virt.as_mut_ptr::<PageTable>()) }
    }

    // constructs a mapper from the globally stored kernel pml4
    pub fn get_kernel_mapper() -> Mapper {
        Mapper { page_table: KERNEL_PAGE_TABLE.load(Ordering::Acquire) }
    }
}

pub struct VMM;

impl VMM {
    // initialises the frame allocator and captures the boot-time cr3 as the kernel page table
    pub fn init(memmap_response: &MemmapResponse) -> Mapper {
        unsafe {
            frame_allocator::init_frame_allocator(memmap_response);
            let mut cr3: u64;
            core::arch::asm!("mov {}, cr3", out(reg) cr3, options(nomem, nostack));
            let kernel_pt = (cr3 & 0x000F_FFFF_FFFF_F000) as *mut PageTable;
            KERNEL_PAGE_TABLE.store(kernel_pt, Ordering::Release);
            Mapper { page_table: kernel_pt }
        }
    }

    // returns a mapper reflecting whichever pml4 is currently loaded in cr3
    pub fn get_mapper() -> Mapper {
        Mapper { page_table: Mapper::get_current_page_table() }
    }

    // returns a mapper backed by the kernel pml4 regardless of the active address space
    pub fn get_kernel_mapper() -> Mapper {
        Mapper::get_kernel_mapper()
    }

    // returns the raw physical pointer to the kernel pml4
    pub fn get_page_table() -> *mut PageTable {
        KERNEL_PAGE_TABLE.load(Ordering::Acquire)
    }
}