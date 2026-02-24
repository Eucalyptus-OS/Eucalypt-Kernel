#![allow(unused)]

use super::addr::{PhysAddr, VirtAddr};
use super::frame_allocator::FrameAllocator;
use core::ptr::null_mut;
use core::sync::atomic::{AtomicPtr, Ordering};
use limine::response::MemoryMapResponse;

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

static KERNEL_PAGE_TABLE: AtomicPtr<PageTable> = AtomicPtr::new(null_mut());

#[derive(Clone, Copy)]
pub struct Mapper {
    page_table: *mut PageTable,
}

impl Mapper {
    fn get_or_create_table(&mut self, entry: &mut PageTableEntry) -> Option<*mut PageTable> {
        if entry.is_present() {
            let addr = entry.get_addr().as_u64();
            Some((addr | HHDM_OFFSET) as *mut PageTable)
        } else {
            let frame = unsafe { FrameAllocator::alloc_frame() }?;
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
            core::arch::asm!("invlpg [{}]", in(reg) virt.as_u64(), options(nostack, preserves_flags));
        }
    }

    pub fn map_page(&mut self, virt: VirtAddr, phys: PhysAddr, flags: u64) -> Option<()> {
        unsafe {
            let p4 = &mut *((self.page_table as u64 | HHDM_OFFSET) as *mut PageTable);

            let p3_entry = &mut p4.entries[virt.p4_index()];
            let p3 = self.get_or_create_table(p3_entry)?;

            let p2_entry = &mut (*p3).entries[virt.p3_index()];
            let p2 = self.get_or_create_table(p2_entry)?;

            let p1_entry = &mut (*p2).entries[virt.p2_index()];
            let p1 = self.get_or_create_table(p1_entry)?;

            let final_entry = &mut (*p1).entries[virt.p1_index()];
            final_entry.set_addr(phys, flags | PageTableEntry::PRESENT);
            Self::flush_tlb(virt);

            Some(())
        }
    }

    pub fn unmap_page(&mut self, virt: VirtAddr) -> Option<PhysAddr> {
        unsafe {
            let p4 = &mut *((self.page_table as u64 | HHDM_OFFSET) as *mut PageTable);

            if !p4.entries[virt.p4_index()].is_present() {
                return None;
            }

            let p3 =
                (p4.entries[virt.p4_index()].get_addr().as_u64() | HHDM_OFFSET) as *mut PageTable;
            if !(*p3).entries[virt.p3_index()].is_present() {
                return None;
            }

            let p2 = ((*p3).entries[virt.p3_index()].get_addr().as_u64() | HHDM_OFFSET)
                as *mut PageTable;
            if !(*p2).entries[virt.p2_index()].is_present() {
                return None;
            }

            let p1 = ((*p2).entries[virt.p2_index()].get_addr().as_u64() | HHDM_OFFSET)
                as *mut PageTable;
            let entry = &mut (*p1).entries[virt.p1_index()];

            if !entry.is_present() {
                return None;
            }

            let phys = entry.get_addr();
            entry.clear();
            Self::flush_tlb(virt);

            Some(phys)
        }
    }

    pub fn map_range(
        &mut self,
        virt_start: VirtAddr,
        phys_start: PhysAddr,
        size: usize,
        flags: u64,
    ) -> Option<()> {
        let pages = (size + 0xFFF) / 0x1000;
        let mut current_virt = virt_start.as_u64();
        let mut current_phys = phys_start.as_u64();

        for _ in 0..pages {
            self.map_page(
                VirtAddr::new(current_virt),
                PhysAddr::new(current_phys),
                flags,
            )?;
            current_virt += 0x1000;
            current_phys += 0x1000;
        }

        Some(())
    }

    pub fn unmap_range(&mut self, virt_start: VirtAddr, size: usize) {
        let pages = (size + 0xFFF) / 0x1000;
        let mut current_virt = virt_start.as_u64();

        for _ in 0..pages {
            self.unmap_page(VirtAddr::new(current_virt));
            current_virt += 0x1000;
        }
    }

    pub fn create_user_pml4(&mut self) -> Option<*mut PageTable> {
        unsafe {
            let frame = FrameAllocator::alloc_frame()?;
            let pml4 = (frame.as_u64() | HHDM_OFFSET) as *mut PageTable;
            (*pml4).zero();

            let kernel_pml4_ptr = KERNEL_PAGE_TABLE.load(Ordering::Acquire);
            let kernel_pml4 = &*((kernel_pml4_ptr as u64 | HHDM_OFFSET) as *mut PageTable);

            for i in 256..512 {
                (*pml4).entries[i] = kernel_pml4.entries[i];
            }

            Some(frame.as_u64() as *mut PageTable)
        }
    }

    pub fn create_user_page_table(&mut self) -> Option<*mut PageTable> {
        unsafe {
            let frame = FrameAllocator::alloc_frame()?;
            let table = (frame.as_u64() | HHDM_OFFSET) as *mut PageTable;
            (*table).zero();
            Some(table)
        }
    }

    pub fn switch_page_table(&mut self, pml4: *mut PageTable) {
        unsafe {
            let phys_addr = if (pml4 as u64) >= HHDM_OFFSET {
                (pml4 as u64) - HHDM_OFFSET
            } else {
                pml4 as u64
            };

            self.page_table = pml4;

            core::arch::asm!(
                "mov cr3, {}",
                in(reg) phys_addr,
                options(nostack, preserves_flags)
            );
        }
    }

    pub fn get_current_page_table() -> *mut PageTable {
        unsafe {
            let mut cr3: u64;
            core::arch::asm!("mov {}, cr3", out(reg) cr3, options(nomem, nostack));
            (cr3 & 0x000F_FFFF_FFFF_F000) as *mut PageTable
        }
    }

    pub fn get_kernel_mapper() -> Mapper {
        Mapper {
            page_table: KERNEL_PAGE_TABLE.load(Ordering::Acquire),
        }
    }
}

pub struct VMM;

impl VMM {
    pub fn init(memory_map: &MemoryMapResponse) -> Mapper {
        unsafe {
            FrameAllocator::init(memory_map);

            let mut cr3: u64;
            core::arch::asm!("mov {}, cr3", out(reg) cr3, options(nomem, nostack));
            let kernel_pt = (cr3 & 0x000F_FFFF_FFFF_F000) as *mut PageTable;

            KERNEL_PAGE_TABLE.store(kernel_pt, Ordering::Release);

            Mapper {
                page_table: kernel_pt,
            }
        }
    }

    pub fn get_mapper() -> Mapper {
        Mapper {
            page_table: Mapper::get_current_page_table(),
        }
    }

    pub fn get_kernel_mapper() -> Mapper {
        Mapper::get_kernel_mapper()
    }

    pub fn get_page_table() -> *mut PageTable {
        KERNEL_PAGE_TABLE.load(Ordering::Acquire)
    }
}
