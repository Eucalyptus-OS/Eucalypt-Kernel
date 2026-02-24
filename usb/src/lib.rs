#![no_std]

use core::sync::atomic::{AtomicBool, Ordering};
use memory;
use pci;
use serial::serial_println;
use xhci;

static USB_LOCK: AtomicBool = AtomicBool::new(false);

fn usb_lock() {
    while USB_LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
}

fn usb_unlock() {
    USB_LOCK.store(false, Ordering::Release);
}

#[derive(Clone, Copy)]
pub struct UsbMapper(pub memory::vmm::Mapper);

impl xhci::accessor::Mapper for UsbMapper {
    unsafe fn map(&mut self, phys_start: usize, bytes: usize) -> core::num::NonZeroUsize {
        const HHDM_OFFSET: u64 = 0xFFFF_8000_0000_0000;
        let phys_u64 = phys_start as u64;
        let virt_u64 = phys_u64 | HHDM_OFFSET;
        let virt = memory::addr::VirtAddr::new(virt_u64);
        let phys = memory::addr::PhysAddr::new(phys_u64);
        let flags = memory::vmm::PageTableEntry::WRITABLE;

        self.0
            .map_range(virt, phys, bytes, flags)
            .expect("UsbMapper: map_range failed");
        unsafe { core::num::NonZeroUsize::new_unchecked(virt_u64 as usize) }
    }

    fn unmap(&mut self, virt_start: usize, bytes: usize) {
        let virt = memory::addr::VirtAddr::new(virt_start as u64);
        self.0.unmap_range(virt, bytes);
    }
}

pub fn init_usb() {
    usb_lock();
    let mapper = memory::vmm::VMM::get_mapper();
    let mapper = UsbMapper(mapper);
    let mut phys_base: u64;
    match pci::pci_find_xhci_controller() {
        Some(device) => {
            serial_println!(
                "Found XHCI controller at bus {}, device {}, function {}",
                device.bus,
                device.device,
                device.function
            );
            pci::pci_enable_memory_space(device.bus, device.device, device.function);
            pci::pci_enable_bus_master(device.bus, device.device, device.function);

            let bar0 = pci::pci_read_bar(device.bus, device.device, device.function, 0);
            if bar0 & 0x1 != 0 {
                usb_unlock();
                return;
            } else {
                let bar_type = (bar0 >> 1) & 0x3;
                phys_base = (bar0 & 0xFFFFFFF0) as u64;
                if bar_type == 0x2 {
                    let bar1 = pci::pci_read_bar(device.bus, device.device, device.function, 1);
                    phys_base |= (bar1 as u64) << 32;
                }
            }
        }
        None => {
            serial_println!("No XHCI controller found");
            usb_unlock();
            return;
        }
    }

    let mut xhci_regs = unsafe { xhci::Registers::new(phys_base as usize, mapper) };
    let xhci_operational_regs = &mut xhci_regs.operational;
    xhci_operational_regs.usbcmd.update_volatile(|u| {
        u.clear_run_stop();
    });
    while !xhci_operational_regs.usbsts.read_volatile().hc_halted() {}

    xhci_operational_regs.usbcmd.update_volatile(|u| {
        u.set_host_controller_reset();
    });
    while xhci_operational_regs
        .usbcmd
        .read_volatile()
        .host_controller_reset()
    {}
    while !xhci_operational_regs.usbsts.read_volatile().hc_halted() {}
    const RING_PAGES: usize = 1;
    const PAGE_SIZE: usize = 0x1000;

    let mut cmd_phys = None;
    let mut evt_phys = None;
    for _ in 0..RING_PAGES {
        let f = unsafe { memory::frame_allocator::FrameAllocator::alloc_frame() };
        if cmd_phys.is_none() {
            cmd_phys = f;
        } else if evt_phys.is_none() {
            evt_phys = f;
        }
    }

    let cmd_phys = match cmd_phys {
        Some(p) => p,
        None => {
            serial_println!("Failed to allocate command ring frame");
            usb_unlock();
            return;
        }
    };

    let evt_phys = match evt_phys {
        Some(p) => p,
        None => {
            serial_println!("Failed to allocate event ring frame");
            usb_unlock();
            return;
        }
    };

    const HHDM_OFFSET: u64 = 0xFFFF_8000_0000_0000;
    let cmd_virt = (cmd_phys.as_u64() | HHDM_OFFSET) as usize;
    let evt_virt = (evt_phys.as_u64() | HHDM_OFFSET) as usize;

    let mut inner_mapper = mapper.0;
    let _ = inner_mapper.map_range(
        memory::addr::VirtAddr::new(cmd_virt as u64),
        memory::addr::PhysAddr::new(cmd_phys.as_u64()),
        PAGE_SIZE,
        memory::vmm::PageTableEntry::WRITABLE,
    );
    let _ = inner_mapper.map_range(
        memory::addr::VirtAddr::new(evt_virt as u64),
        memory::addr::PhysAddr::new(evt_phys.as_u64()),
        PAGE_SIZE,
        memory::vmm::PageTableEntry::WRITABLE,
    );

    serial_println!(
        "Command ring phys=0x{:x} virt=0x{:x}",
        cmd_phys.as_u64(),
        cmd_virt
    );
    serial_println!(
        "Event ring phys=0x{:x} virt=0x{:x}",
        evt_phys.as_u64(),
        evt_virt
    );

    xhci_operational_regs.usbcmd.update_volatile(|u| {
        u.set_run_stop();
    });

    while xhci_operational_regs.usbsts.read_volatile().hc_halted() {}
    let erst_phys = match unsafe { memory::frame_allocator::FrameAllocator::alloc_frame() } {
        Some(p) => p,
        None => {
            serial_println!("Failed to allocate ERST frame");
            usb_unlock();
            return;
        }
    };
    let erst_virt = (erst_phys.as_u64() | HHDM_OFFSET) as usize;
    let _ = inner_mapper.map_range(
        memory::addr::VirtAddr::new(erst_virt as u64),
        memory::addr::PhysAddr::new(erst_phys.as_u64()),
        PAGE_SIZE,
        memory::vmm::PageTableEntry::WRITABLE,
    );

    unsafe {
        let p = erst_virt as *mut u8;
        (p as *mut u64).write_volatile(evt_phys.as_u64());
        let seg_size: u32 = (PAGE_SIZE / xhci::ring::trb::BYTES) as u32;
        (p.add(8) as *mut u32).write_volatile(seg_size);
        (p.add(12) as *mut u32).write_volatile(0);
    }

    let mut interrupter = xhci_regs.interrupter_register_set.interrupter_mut(0);
    interrupter.erstsz.update_volatile(|s| s.set(1));
    interrupter
        .erstba
        .update_volatile(|b| b.set(erst_phys.as_u64()));
    interrupter
        .erdp
        .update_volatile(|d| d.set_event_ring_dequeue_pointer(evt_phys.as_u64()));
    interrupter.iman.update_volatile(|i| {
        i.clear_interrupt_pending();
        i.set_interrupt_enable();
    });

    xhci_operational_regs.crcr.update_volatile(|c| {
        c.set_command_ring_pointer(cmd_phys.as_u64());
        c.set_ring_cycle_state();
    });

    let trb_addr = cmd_virt as *mut u32;
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

    xhci_operational_regs.crcr.update_volatile(|c| {
        c.set_command_ring_pointer(cmd_phys.as_u64());
        c.set_ring_cycle_state();
    });

    usb_unlock();
}
