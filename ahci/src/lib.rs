#![no_std]

extern crate alloc;

use framebuffer::println;
use memory::mmio::map_mmio;
use pci::{pci_enable_bus_master, pci_enable_memory_space, pci_find_ahci_controller};
use core::sync::atomic::{AtomicBool, Ordering};

pub use types::*;
mod types;

static AHCI_LOCK: AtomicBool = AtomicBool::new(false);

fn ahci_lock() {
    while AHCI_LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
}

fn ahci_unlock() {
    AHCI_LOCK.store(false, Ordering::Release);
}

fn start_cmd(port: &mut HbaPort) {
    while port.read_cmd() & HBA_PORT_CMD_CR != 0 {
        core::hint::spin_loop();
    }
    port.write_cmd(port.read_cmd() | HBA_PORT_CMD_FRE);
    port.write_cmd(port.read_cmd() | HBA_PORT_CMD_ST);
}

fn stop_cmd(port: &mut HbaPort) {
    port.write_cmd(port.read_cmd() & !HBA_PORT_CMD_ST);
    while port.read_cmd() & HBA_PORT_CMD_CR != 0 {
        core::hint::spin_loop();
    }
    port.write_cmd(port.read_cmd() & !HBA_PORT_CMD_FRE);
    while port.read_cmd() & HBA_PORT_CMD_FR != 0 {
        core::hint::spin_loop();
    }
}

fn rebase_port(port: &mut HbaPort, portno: u32) -> bool {
    stop_cmd(port);

    let clb_frame = memory::frame_allocator::FrameAllocator::alloc_frame();
    let fb_frame  = memory::frame_allocator::FrameAllocator::alloc_frame();

    let (clb_phys, fb_phys) = match (clb_frame, fb_frame) {
        (Some(c), Some(f)) => (c.as_u64(), f.as_u64()),
        _ => {
            println!("Failed to allocate frames for port {}", portno);
            return false;
        }
    };

    let clb_virt = match map_mmio(memory::vmm::VMM::get_page_table(), clb_phys, 0x1000) {
        Ok(v) => v,
        Err(_) => {
            println!("Failed to map CLB for port {}", portno);
            return false;
        }
    };

    let fb_virt = match map_mmio(memory::vmm::VMM::get_page_table(), fb_phys, 0x1000) {
        Ok(v) => v,
        Err(_) => {
            println!("Failed to map FB for port {}", portno);
            return false;
        }
    };

    port.set_clb(clb_phys);
    unsafe { core::ptr::write_bytes(clb_virt as *mut u8, 0, 1024); }

    port.set_fb(fb_phys);
    unsafe { core::ptr::write_bytes(fb_virt as *mut u8, 0, 256); }

    let cmdheader = clb_virt as *mut HbaCmdHeader;
    for i in 0..32usize {
        let frame = match memory::frame_allocator::FrameAllocator::alloc_frame() {
            Some(f) => f,
            None => {
                println!("Failed to allocate command table frame for port {}, slot {}", portno, i);
                return false;
            }
        };
        let ctba_phys = frame.as_u64();

        let ctba_virt = match map_mmio(memory::vmm::VMM::get_page_table(), ctba_phys, 0x1000) {
            Ok(v) => v,
            Err(_) => {
                println!("Failed to map command table for port {}, slot {}", portno, i);
                return false;
            }
        };

        unsafe {
            let hdr = &mut *cmdheader.add(i);
            hdr.prdtl = 8;
            hdr.set_ctba(ctba_phys);
            core::ptr::write_bytes(ctba_virt as *mut u8, 0, core::mem::size_of::<HbaCmdTbl>());
        }
    }

    start_cmd(port);
    true
}

pub fn probe_ports(abar: &mut HbaMem) {
    let pi = abar.read_pi();
    for i in 0..32usize {
        if (pi >> i) & 1 == 0 {
            continue;
        }
        let dt = check_type(&abar.ports[i]);
        match dt {
            AHCI_DEV_SATA => {
                println!("SATA drive found at port {}", i);
                rebase_port(&mut abar.ports[i], i as u32);
            }
            AHCI_DEV_SATAPI => {
                println!("SATAPI drive found at port {}", i);
                rebase_port(&mut abar.ports[i], i as u32);
            }
            AHCI_DEV_SEMB => println!("SEMB drive found at port {}", i),
            AHCI_DEV_PM   => println!("PM drive found at port {}", i),
            _ => {}
        }
    }
}

fn check_type(port: &HbaPort) -> u8 {
    let ssts = port.read_ssts();
    let ipm = (ssts >> 8) & 0x0F;
    let det = ssts & 0x0F;

    if det != HBA_PORT_DET_PRESENT || ipm != HBA_PORT_IPM_ACTIVE {
        return AHCI_DEV_NULL;
    }

    match port.read_sig() {
        HBA_PORT_SIG_ATAPI => AHCI_DEV_SATAPI,
        HBA_PORT_SIG_SEMB  => AHCI_DEV_SEMB,
        HBA_PORT_SIG_PM    => AHCI_DEV_PM,
        _                  => AHCI_DEV_SATA,
    }
}

fn build_fis(fis: &mut [u8; 64], command: u8, lba: u64, count: u32) {
    fis[0]  = FIS_TYPE_REG_H2D;
    fis[1]  = 1 << 7;
    fis[2]  = command;
    fis[3]  = 0x00;
    fis[4]  = (lba & 0xFF) as u8;
    fis[5]  = ((lba >> 8)  & 0xFF) as u8;
    fis[6]  = ((lba >> 16) & 0xFF) as u8;
    fis[7]  = 0x40;
    fis[8]  = ((lba >> 24) & 0xFF) as u8;
    fis[9]  = ((lba >> 32) & 0xFF) as u8;
    fis[10] = ((lba >> 40) & 0xFF) as u8;
    fis[11] = 0x00;
    fis[12] = (count & 0xFF) as u8;
    fis[13] = ((count >> 8) & 0xFF) as u8;
}

fn issue_command(port: &mut HbaPort, buffer: u64, count: u32, command: u8) -> bool {
    if port.read_ci() != 0 {
        return false;
    }

    let clb_phys = port.clb();
    let cmdheader_virt = match map_mmio(memory::vmm::VMM::get_page_table(), clb_phys, 0x1000) {
        Ok(v) => v as *mut HbaCmdHeader,
        Err(_) => return false,
    };

    unsafe {
        let hdr = &mut *cmdheader_virt;
        hdr.prdtl = 1;
        hdr.flags = AHCI_CMD_HEADER_FLAGS_FIS_LEN
            | if command == ATA_CMD_WRITE_DMA_EX { AHCI_CMD_HEADER_FLAGS_WRITE } else { 0 };

        let ctba_phys = hdr.ctba();
        let cmdtbl_virt = match map_mmio(memory::vmm::VMM::get_page_table(), ctba_phys, 0x1000) {
            Ok(v) => v as *mut HbaCmdTbl,
            Err(_) => return false,
        };

        let tbl = &mut *cmdtbl_virt;
        core::ptr::write_bytes(tbl as *mut HbaCmdTbl as *mut u8, 0, core::mem::size_of::<HbaCmdTbl>());

        build_fis(&mut tbl.cfis, command, 0, count);

        tbl.prdt_entry[0].set_dba(buffer);
        tbl.prdt_entry[0].dbc = (count * 512) - 1;
    }

    port.write_ci(1);

    let mut timeout = 1_000_000u32;
    loop {
        if timeout == 0 {
            return false;
        }
        let tfd = port.read_tfd();
        if tfd & (ATA_DEV_BUSY as u32 | ATA_DEV_DRQ as u32) == 0 && port.read_ci() & 1 == 0 {
            break;
        }
        if port.read_is() & HBA_PX_IS_TFES != 0 {
            return false;
        }
        timeout -= 1;
        core::hint::spin_loop();
    }

    port.read_is() & HBA_PX_IS_TFES == 0
}

pub fn ahci_read(port: &mut HbaPort, lba: u64, count: u32, buffer: *mut u8) -> bool {
    ahci_lock();

    let mut fis_buf = [0u8; 64];
    build_fis(&mut fis_buf, ATA_CMD_READ_DMA_EX, lba, count);

    let result = issue_command(port, buffer as u64, count, ATA_CMD_READ_DMA_EX);
    ahci_unlock();
    result
}

pub fn ahci_write(port: &mut HbaPort, _lba: u64, count: u32, buffer: *const u8) -> bool {
    ahci_lock();
    let result = issue_command(port, buffer as u64, count, ATA_CMD_WRITE_DMA_EX);
    ahci_unlock();
    result
}

pub fn init_ahci() {
    let ahci_dev = match pci_find_ahci_controller() {
        Some(d) => d,
        None => {
            println!("No AHCI controller found");
            return;
        }
    };

    let abar_phys = ahci_dev.bar[5] as u64 & !0xF;
    println!("AHCI controller at {}:{}:{}", ahci_dev.bus, ahci_dev.device, ahci_dev.function);

    if abar_phys == 0 {
        println!("Invalid AHCI BAR address");
        return;
    }

    pci_enable_bus_master(ahci_dev.bus, ahci_dev.device, ahci_dev.function);
    pci_enable_memory_space(ahci_dev.bus, ahci_dev.device, ahci_dev.function);

    let abar_virt = match map_mmio(memory::vmm::VMM::get_page_table(), abar_phys, 0x4000) {
        Ok(v) => v,
        Err(e) => {
            println!("Failed to map AHCI MMIO: {}", e);
            return;
        }
    };

    let abar = unsafe { &mut *(abar_virt as *mut HbaMem) };
    probe_ports(abar);
    println!("AHCI initialization complete");
}