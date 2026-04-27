#![no_std]

use framebuffer::println;
use pci;
use xhci;
use memory;
use core::sync::atomic::{AtomicBool, Ordering};

/// Simple spinlock for USB initialization so only one core/thread
/// can touch the controller during setup.
static USB_LOCK: AtomicBool = AtomicBool::new(false);

/// Acquire USB spinlock.
fn usb_lock() {
    while USB_LOCK
        .compare_exchange_weak(
            false,
            true,
            Ordering::Acquire,
            Ordering::Relaxed
        )
        .is_err()
    {
        core::hint::spin_loop();
    }
}

/// Release USB spinlock.
fn usb_unlock() {
    USB_LOCK.store(false, Ordering::Release);
}

/// Mapper wrapper required by xhci crate.
/// Converts physical MMIO addresses into virtual HHDM mappings.
#[derive(Clone, Copy)]
pub struct UsbMapper(pub memory::vmm::Mapper);

impl xhci::accessor::Mapper for UsbMapper {

    /// Maps physical MMIO memory into kernel virtual address space.
    unsafe fn map(
        &mut self,
        phys_start: usize,
        bytes: usize
    ) -> core::num::NonZeroUsize {

        // Higher Half Direct Mapping offset
        const HHDM_OFFSET: u64 = 0xFFFF_8000_0000_0000;

        let phys_u64 = phys_start as u64;

        // Convert physical to higher-half virtual
        let virt_u64 = phys_u64 | HHDM_OFFSET;

        let virt = memory::addr::VirtAddr::new(virt_u64);
        let phys = memory::addr::PhysAddr::new(phys_u64);

        let flags = memory::paging::PageTableEntry::WRITABLE;
        let kernel_pml4 = memory::vmm::VMM::get_page_table();

        // Map MMIO region
        self.0
            .map_range(
                kernel_pml4,
                virt,
                phys,
                bytes,
                flags
            )
            .expect("UsbMapper: map_range failed");

        unsafe {
            core::num::NonZeroUsize::new_unchecked(
                virt_u64 as usize
            )
        }
    }

    /// Unmap MMIO region.
    fn unmap(&mut self, virt_start: usize, bytes: usize) {
        let virt = memory::addr::VirtAddr::new(
            virt_start as u64
        );

        let kernel_pml4 = memory::vmm::VMM::get_page_table();

        self.0.unmap_range(
            kernel_pml4,
            virt,
            bytes
        );
    }
}

/// Initialize USB xHCI host controller.
pub fn init_usb() {

    usb_lock();

    let mapper = memory::vmm::VMM::get_mapper();
    let mapper = UsbMapper(mapper);

    let mut phys_base: u64;

    // Scan PCI bus for xHCI controller
    match pci::pci_find_xhci_controller() {

        Some(device) => {

            println!(
                "Found XHCI controller at bus {}, device {}, function {}",
                device.bus,
                device.device,
                device.function
            );

            // Enable MMIO and bus mastering
            pci::pci_enable_memory_space(
                device.bus,
                device.device,
                device.function
            );

            pci::pci_enable_bus_master(
                device.bus,
                device.device,
                device.function
            );

            // Read BAR0 (controller MMIO base)
            let bar0 = pci::pci_read_bar(
                device.bus,
                device.device,
                device.function,
                0
            );

            // Reject IO-space BARs
            if bar0 & 0x1 != 0 {
                usb_unlock();
                return;
            } else {

                // Determine if BAR is 64-bit
                let bar_type = (bar0 >> 1) & 0x3;

                phys_base = (bar0 & 0xFFFFFFF0) as u64;

                if bar_type == 0x2 {

                    // Read upper 32 bits for 64-bit BAR
                    let bar1 = pci::pci_read_bar(
                        device.bus,
                        device.device,
                        device.function,
                        1
                    );

                    phys_base |= (bar1 as u64) << 32;
                }
            }
        }

        None => {
            println!("No XHCI controller found");
            usb_unlock();
            return;
        }
    }

    // Map xHCI registers
    let mut xhci_regs = unsafe {
        xhci::Registers::new(
            phys_base as usize,
            mapper
        )
    };

    let xhci_operational_regs =
        &mut xhci_regs.operational;

    // Halt controller
    xhci_operational_regs.usbcmd.update_volatile(
        |u| {
            u.clear_run_stop();
        }
    );

    while !xhci_operational_regs
        .usbsts
        .read_volatile()
        .hc_halted()
    {}

    // Host controller reset
    xhci_operational_regs.usbcmd.update_volatile(
        |u| {
            u.set_host_controller_reset();
        }
    );

    while xhci_operational_regs
        .usbcmd
        .read_volatile()
        .host_controller_reset()
    {}

    while !xhci_operational_regs
        .usbsts
        .read_volatile()
        .hc_halted()
    {}

    const PAGE_SIZE: usize = 0x1000;

    // Allocate command ring + event ring pages
    let cmd_phys =
        memory::frame_allocator::FrameAllocator
            ::alloc_frame()
            .expect(
                "Failed to allocate command ring frame"
            );

    let evt_phys =
        memory::frame_allocator::FrameAllocator
            ::alloc_frame()
            .expect(
                "Failed to allocate event ring frame"
            );

    const HHDM_OFFSET: u64 =
        0xFFFF_8000_0000_0000;

    let cmd_virt =
        (cmd_phys.as_u64() | HHDM_OFFSET) as usize;

    let evt_virt =
        (evt_phys.as_u64() | HHDM_OFFSET) as usize;

    let inner_mapper = mapper.0;

    // Map command ring page
    let _ = inner_mapper.map_range(
        memory::vmm::VMM::get_page_table(),
        memory::addr::VirtAddr::new(
            cmd_virt as u64
        ),
        memory::addr::PhysAddr::new(
            cmd_phys.as_u64()
        ),
        PAGE_SIZE,
        memory::paging::PageTableEntry::WRITABLE
    );

    // Map event ring page
    let _ = inner_mapper.map_range(
        memory::vmm::VMM::get_page_table(),
        memory::addr::VirtAddr::new(
            evt_virt as u64
        ),
        memory::addr::PhysAddr::new(
            evt_phys.as_u64()
        ),
        PAGE_SIZE,
        memory::paging::PageTableEntry::WRITABLE
    );

    println!(
        "Command ring phys=0x{:X} virt=0x{:X}",
        cmd_phys.as_u64(),
        cmd_virt
    );

    println!(
        "Event ring phys=0x{:X} virt=0x{:X}",
        evt_phys.as_u64(),
        evt_virt
    );

    // Start controller
    xhci_operational_regs.usbcmd.update_volatile(
        |u| {
            u.set_run_stop();
        }
    );

    while xhci_operational_regs
        .usbsts
        .read_volatile()
        .hc_halted()
    {}

    // Allocate Event Ring Segment Table (ERST)
    let erst_phys =
        match memory::frame_allocator::FrameAllocator
            ::alloc_frame()
        {
            Some(p) => p,

            None => {
                println!(
                    "Failed to allocate ERST frame"
                );

                usb_unlock();
                return;
            }
        };

    let erst_virt =
        (erst_phys.as_u64() | HHDM_OFFSET)
            as usize;

    let _ = inner_mapper.map_range(
        memory::vmm::VMM::get_page_table(),
        memory::addr::VirtAddr::new(
            erst_virt as u64
        ),
        memory::addr::PhysAddr::new(
            erst_phys.as_u64()
        ),
        PAGE_SIZE,
        memory::paging::PageTableEntry::WRITABLE
    );

    // Build ERST entry
    unsafe {

        let p = erst_virt as *mut u8;

        // Segment base address
        (p as *mut u64)
            .write_volatile(
                evt_phys.as_u64()
            );

        // Number of TRBs in segment
        let seg_size: u32 =
            (PAGE_SIZE /
             xhci::ring::trb::BYTES)
             as u32;

        (p.add(8) as *mut u32)
            .write_volatile(seg_size);

        // Reserved
        (p.add(12) as *mut u32)
            .write_volatile(0);
    }

    // Configure interrupter 0
    let mut interrupter =
        xhci_regs
            .interrupter_register_set
            .interrupter_mut(0);

    interrupter.erstsz.update_volatile(
        |s| s.set(1)
    );

    interrupter.erstba.update_volatile(
        |b| b.set(
            erst_phys.as_u64()
        )
    );

    interrupter.erdp.update_volatile(
        |d| {
            d.set_event_ring_dequeue_pointer(
                evt_phys.as_u64()
            )
        }
    );

    interrupter.iman.update_volatile(
        |i| {
            i.clear_interrupt_pending();
            i.set_interrupt_enable();
        }
    );

    // Set command ring control register
    xhci_operational_regs.crcr.update_volatile(
        |c| {
            c.set_command_ring_pointer(
                cmd_phys.as_u64()
            );

            c.set_ring_cycle_state();
        }
    );

    // Queue Enable Slot command TRB
    let trb_addr =
        cmd_virt as *mut u32;

    unsafe {

        use xhci::ring::trb::command::EnableSlot;

        let mut trb = EnableSlot::new();

        trb.set_cycle_bit();

        let raw = trb.into_raw();

        trb_addr.write_volatile(raw[0]);
        trb_addr.add(1).write_volatile(raw[1]);
        trb_addr.add(2).write_volatile(raw[2]);
        trb_addr.add(3).write_volatile(raw[3]);
    }

    // Ring doorbell by updating CRCR
    xhci_operational_regs.crcr.update_volatile(
        |c| {
            c.set_command_ring_pointer(
                cmd_phys.as_u64()
            );
    
            c.set_ring_cycle_state();
        }
    );

    usb_unlock();
}