#![allow(unused)]

use core::ptr::null_mut;
use limine::response::MemoryMapResponse;
use super::addr::{PhysAddr, VirtAddr};
use super::frame_allocator::FrameAllocator;

const ENTRIES_PER_TABLE: usize = 512;
const HHDM_OFFSET: u64 = 0xFFFF800000000000;

#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct PageTableEntry(u64);

impl PageTableEntry {
    pub const PRESENT: u64 = 1 << 0;
    pub const WRITABLE: u64 = 1 << 1;
    pub const USER: u64 = 1 << 2;
    pub const WRITE_THROUGH: u64 = 1 << 3;
    pub const NO_CACHE: u64 = 1 << 4;
    pub const ACCESSED: u64 = 1 << 5;
    pub const DIRTY: u64 = 1 << 6;
    pub const HUGE: u64 = 1 << 7;
    pub const GLOBAL: u64 = 1 << 8;
    pub const NO_EXECUTE: u64 = 1 << 63;
    
    pub fn new() -> Self {
        PageTableEntry(0)
    }
    
    pub fn is_present(&self) -> bool {
        (self.0 & Self::PRESENT) != 0
    }
    
    pub fn set_addr(&mut self, addr: PhysAddr, flags: u64) {
        self.0 = (addr.as_u64() & 0x000F_FFFF_FFFF_F000) | flags;
    }
    
    pub fn get_addr(&self) -> PhysAddr {
        PhysAddr::new(self.0 & 0x000F_FFFF_FFFF_F000)
    }
    
    pub fn clear(&mut self) {
        self.0 = 0;
    }
}

#[repr(align(4096))]
pub struct PageTable {
    entries: [PageTableEntry; ENTRIES_PER_TABLE],
}

impl PageTable {
    pub fn new() -> Self {
        PageTable {
            entries: [PageTableEntry::new(); ENTRIES_PER_TABLE],
        }
    }
    
    pub fn zero(&mut self) {
        for entry in self.entries.iter_mut() {
            entry.clear();
        }
    }
}

static mut KERNEL_PAGE_TABLE: *mut PageTable = null_mut();

#[derive(Clone, Copy)]
pub struct Mapper {
    page_table: *mut PageTable,
}

impl Mapper {
    unsafe fn get_or_create_table(&mut self, entry: &mut PageTableEntry) -> Option<*mut PageTable> {
        unsafe {
            if entry.is_present() {
                Some((entry.get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable)
            } else {
                let frame = FrameAllocator::alloc_frame()?;
                let table = (frame.as_u64() | HHDM_OFFSET) as *mut PageTable;
                (*table).zero();
                entry.set_addr(frame, PageTableEntry::PRESENT | PageTableEntry::WRITABLE | PageTableEntry::USER);
                Some(table)
            }
        }
    }
    
    pub unsafe fn map_page(&mut self, virt: VirtAddr, phys: PhysAddr, flags: u64) -> Option<()> {
        unsafe {
            let p4 = &mut *((self.page_table as u64 | HHDM_OFFSET) as *mut PageTable);

            let p3_entry = &mut p4.entries[virt.p4_index()];
            let p3 = self.get_or_create_table(p3_entry)?;

            let p2_entry = &mut (*p3).entries[virt.p3_index()];
            let p2 = self.get_or_create_table(p2_entry)?;

            let p1_entry = &mut (*p2).entries[virt.p2_index()];
            let p1 = self.get_or_create_table(p1_entry)?;

            (*p1).entries[virt.p1_index()].set_addr(phys, flags | PageTableEntry::PRESENT);

            core::arch::asm!("invlpg [{}]", in(reg) virt.as_u64(), options(nostack, preserves_flags));

            Some(())
        }
    }
    
    pub unsafe fn unmap_page(&mut self, virt: VirtAddr) {
        unsafe {
            let p4 = &mut *((self.page_table as u64 | HHDM_OFFSET) as *mut PageTable);
            
            if !p4.entries[virt.p4_index()].is_present() {
                return;
            }
            
            let p3 = (p4.entries[virt.p4_index()].get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable;
            if !(*p3).entries[virt.p3_index()].is_present() {
                return;
            }
            
            let p2 = ((*p3).entries[virt.p3_index()].get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable;
            if !(*p2).entries[virt.p2_index()].is_present() {
                return;
            }
            
            let p1 = ((*p2).entries[virt.p2_index()].get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable;
            (*p1).entries[virt.p1_index()].clear();
            
            core::arch::asm!("invlpg [{}]", in(reg) virt.as_u64(), options(nostack, preserves_flags));
        }
    }

    pub unsafe fn map_range(&mut self, virt_start: VirtAddr, phys_start: PhysAddr, size: usize, flags: u64) -> Option<()> {
        unsafe {
            let pages = (size + 0xFFF) / 0x1000;
            let mut current_virt = virt_start.as_u64();
            let mut current_phys = phys_start.as_u64();
            
            for _ in 0..pages {
                self.map_page(VirtAddr::new(current_virt), PhysAddr::new(current_phys), flags)?;
                current_virt += 0x1000;
                current_phys += 0x1000;
            }
            Some(())
        }
    }
    
    pub unsafe fn unmap_range(&mut self, virt_start: VirtAddr, size: usize) {
        unsafe {
            let pages = (size + 0xFFF) / 0x1000;
            let mut current_virt = virt_start.as_u64();
            
            for _ in 0..pages {
                self.unmap_page(VirtAddr::new(current_virt));
                current_virt += 0x1000;
            }
        }
    }
}

pub struct VMM;

impl VMM {
    pub unsafe fn init(memory_map: &MemoryMapResponse) -> Mapper {
        unsafe {
            FrameAllocator::init(memory_map);
            
            let mut cr3: u64;
            core::arch::asm!("mov {}, cr3", out(reg) cr3, options(nomem, nostack));
            KERNEL_PAGE_TABLE = (cr3 & 0x000F_FFFF_FFFF_F000) as *mut PageTable;
            
            Mapper {
                page_table: KERNEL_PAGE_TABLE,
            }
        }
    }
    
    pub unsafe fn get_mapper() -> Mapper {
        Mapper {
            page_table: unsafe { KERNEL_PAGE_TABLE },
        }
    }
    
    pub fn get_page_table() -> *mut PageTable {
        unsafe { KERNEL_PAGE_TABLE }
    }
}