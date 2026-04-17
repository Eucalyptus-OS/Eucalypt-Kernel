use core::sync::atomic::Ordering;
use spin::Mutex;

use limine::request::MemmapResponse;

use crate::paging::KERNEL_PAGE_TABLE;
use crate::frame_allocator::{self, FrameAllocator};
use crate::addr::{PhysAddr, VirtAddr};
use crate::paging::{PageTable, PageTableEntry};

const HHDM_OFFSET: u64 = 0xFFFF800000000000;

/// Global lock protecting all page table mutations across CPUs.
static MAPPER_LOCK: Mutex<()> = Mutex::new(());

#[derive(Clone, Copy)]
pub struct Mapper {
    page_table: *mut PageTable,
}

// SAFETY: Mapper holds a raw pointer to a page table. All mutations are
// serialized through MAPPER_LOCK, so it is safe to send across threads.
unsafe impl Send for Mapper {}
unsafe impl Sync for Mapper {}

impl Mapper {
    /// Returns a virtual pointer to the page table behind `entry`, allocating a new frame if the entry is not yet present.
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

    /// Invalidates the TLB entry for `virt` on the current CPU.
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

    /// Maps a single 4 KiB page from `virt` to `phys` in `pml4`, creating intermediate tables as needed.
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

    /// Removes the mapping for `virt` from `pml4` and returns the physical address that was mapped, or `None` if it was not mapped.
    pub fn unmap_page(&self, pml4: *mut PageTable, virt: VirtAddr) -> Option<PhysAddr> {
        let _guard = MAPPER_LOCK.lock();
        unsafe {
            let p4 = &mut *((pml4 as u64 | HHDM_OFFSET) as *mut PageTable);

            if !p4.entries[virt.p4_index()].is_present() {
                return None;
            }

            let p3 = (p4.entries[virt.p4_index()].get_addr().as_u64() | HHDM_OFFSET)
                as *mut PageTable;
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

    /// Maps a contiguous range of `size` bytes from `virt_start` to `phys_start`, rounding up to the nearest page.
    pub fn map_range(
        &self,
        pml4: *mut PageTable,
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
                pml4,
                VirtAddr::new(current_virt),
                PhysAddr::new(current_phys),
                flags,
            )?;
            current_virt += 0x1000;
            current_phys += 0x1000;
        }

        Some(())
    }

    /// Unmaps a contiguous range of `size` bytes starting at `virt_start`, rounding up to the nearest page.
    pub fn unmap_range(&self, pml4: *mut PageTable, virt_start: VirtAddr, size: usize) {
        let pages = (size + 0xFFF) / 0x1000;
        let mut current_virt = virt_start.as_u64();

        for _ in 0..pages {
            self.unmap_page(pml4, VirtAddr::new(current_virt));
            current_virt += 0x1000;
        }
    }

    /// Allocates a new PML4 for a user process and copies the upper-half kernel entries (indices 256–511) from the kernel page table.
    pub fn create_user_pml4(&self) -> Option<*mut PageTable> {
        let _guard = MAPPER_LOCK.lock();
        unsafe {
            let frame = FrameAllocator::alloc_frame()?;
            let pml4 = (frame.as_u64() | HHDM_OFFSET) as *mut PageTable;
            (*pml4).zero();

            let kernel_pml4_ptr = KERNEL_PAGE_TABLE.load(Ordering::Acquire);
            let kernel_pml4 = &*((kernel_pml4_ptr as u64 | HHDM_OFFSET) as *mut PageTable);

            for i in 256..512 {
                (*pml4).entries[i] = kernel_pml4.entries[i];
            }

            // Return physical address so callers can store or load it into CR3
            Some(frame.as_u64() as *mut PageTable)
        }
    }

    /// Allocates and zeroes a fresh page table frame, returning a virtual pointer to it.
    pub fn create_user_page_table(&self) -> Option<*mut PageTable> {
        let _guard = MAPPER_LOCK.lock();
        unsafe {
            let frame = FrameAllocator::alloc_frame()?;
            let table = (frame.as_u64() | HHDM_OFFSET) as *mut PageTable;
            (*table).zero();
            Some(table)
        }
    }

    /// Loads `pml4` into CR3, switching the active address space on the current CPU.
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

    /// Reads CR3 and returns the physical address of the currently active PML4, masking flag bits.
    pub fn get_current_page_table() -> *mut PageTable {
        unsafe {
            let mut cr3: u64;
            core::arch::asm!("mov {}, cr3", out(reg) cr3, options(nomem, nostack));
            (cr3 & 0x000F_FFFF_FFFF_F000) as *mut PageTable
        }
    }

    /// Returns a mutable reference to the active level-4 page table via the HHDM, reading its physical address from CR3.
    pub unsafe fn active_level_4_table() -> &'static mut PageTable {
        let offset = HHDM_OFFSET;
        let (level_4_table_frame, _) = x86_64::registers::control::Cr3::read();
        let phys = level_4_table_frame.start_address().as_u64();
        let virt = VirtAddr::new(phys + offset);
        let page_table_ptr: *mut PageTable = virt.as_mut_ptr();
        unsafe { &mut *page_table_ptr }
    }

    /// Constructs a `Mapper` whose page table pointer is the globally stored kernel PML4.
    pub fn get_kernel_mapper() -> Mapper {
        Mapper {
            page_table: KERNEL_PAGE_TABLE.load(Ordering::Acquire),
        }
    }
}

pub struct VMM;

impl VMM {
    /// Initialises the frame allocator from the Limine memory map and stores the current CR3 as the kernel page table.
    pub fn init(memmap_response: &MemmapResponse) -> Mapper {
        unsafe {
            frame_allocator::init_frame_allocator(memmap_response);
            let mut cr3: u64;
            core::arch::asm!("mov {}, cr3", out(reg) cr3, options(nomem, nostack));
            let kernel_pt = (cr3 & 0x000F_FFFF_FFFF_F000) as *mut PageTable;

            KERNEL_PAGE_TABLE.store(kernel_pt, Ordering::Release);

            Mapper {
                page_table: kernel_pt,
            }
        }
    }

    /// Returns a `Mapper` whose page table pointer reflects whichever PML4 is loaded in CR3 right now.
    pub fn get_mapper() -> Mapper {
        Mapper {
            page_table: Mapper::get_current_page_table(),
        }
    }

    /// Returns a `Mapper` backed by the kernel PML4 regardless of which address space is currently active.
    pub fn get_kernel_mapper() -> Mapper {
        Mapper::get_kernel_mapper()
    }

    /// Returns the raw physical pointer to the kernel PML4 as stored in the global atomic.
    pub fn get_page_table() -> *mut PageTable {
        KERNEL_PAGE_TABLE.load(Ordering::Acquire)
    }
}