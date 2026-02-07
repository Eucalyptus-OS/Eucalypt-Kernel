#![no_std]
//! This file is for initializing and writing to AHCI (Advanced Host Controller Interface) drives
//! In 2004 Intel created AHCI to replace the older Parallel ATA (PATA) interface
//! AHCI provided native command queuing hot-plug support and better performance
//! It became the standard for SATA (Serial ATA) controllers on modern systems
//! AHCI controllers support multiple ports and provide a more efficient way to manage SATA devices

extern crate alloc;

use serial::serial_println;
use memory::mmio::map_mmio;
use pci::{PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA, pci_config_read_dword, pci_enable_bus_master, pci_enable_memory_space, pci_find_ahci_controller};
use core::sync::atomic::{AtomicBool, Ordering};

pub use types::*;
mod types;

static AHCI_LOCK: AtomicBool = AtomicBool::new(false);

fn ahci_lock() {
    while AHCI_LOCK.compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
        core::hint::spin_loop();
    }
}

fn ahci_unlock() {
    AHCI_LOCK.store(false, Ordering::Release);
}

fn start_cmd(port: &mut HbaPort) {
    let mut cmd = port.read_cmd();
    while (cmd & (1 << 15)) != 0 {
        core::hint::spin_loop();
        cmd = port.read_cmd();
    }
    
    cmd = port.read_cmd();
    if (cmd & (1 << 4)) == 0 {
        port.write_cmd(cmd | (1 << 4));
    }
    
    cmd = port.read_cmd();
    port.write_cmd(cmd | (1 << 0));
}

fn stop_cmd(port: &mut HbaPort) {
    let mut cmd = port.read_cmd();
    port.write_cmd(cmd & !(1 << 0));
    
    cmd = port.read_cmd();
    while (cmd & (1 << 15)) != 0 {
        core::hint::spin_loop();
        cmd = port.read_cmd();
    }
    
    cmd = port.read_cmd();
    port.write_cmd(cmd & !(1 << 4));
    
    cmd = port.read_cmd();
    while (cmd & (1 << 14)) != 0 {
        core::hint::spin_loop();
        cmd = port.read_cmd();
    }
}

fn rebase_port(port: &mut HbaPort, portno: u32) {
    stop_cmd(port);
    
    let clb_frame = unsafe { memory::frame_allocator::FrameAllocator::alloc_frame() };
    let fb_frame = unsafe { memory::frame_allocator::FrameAllocator::alloc_frame() };
    
    if clb_frame.is_none() || fb_frame.is_none() {
        serial_println!("Failed to allocate frames for port {}", portno);
        return;
    }
    
    let clb_phys = clb_frame.unwrap().as_u64();
    let fb_phys = fb_frame.unwrap().as_u64();
    
    let clb_virt = match map_mmio(clb_phys, 0x1000) {
        Ok(v) => v,
        Err(_) => {
            serial_println!("Failed to map CLB for port {}", portno);
            return;
        }
    };
    
    let fb_virt = match map_mmio(fb_phys, 0x1000) {
        Ok(v) => v,
        Err(_) => {
            serial_println!("Failed to map FB for port {}", portno);
            return;
        }
    };
    
    port.clb = clb_phys;
    unsafe { core::ptr::write_bytes(clb_virt as *mut u8, 0, 1024); }
    
    port.fb = fb_phys;
    unsafe { core::ptr::write_bytes(fb_virt as *mut u8, 0, 256); }
    
    let cmdheader = clb_virt as *mut HbaCmdHeader;
    for i in 0..32 {
        let ctba_frame = unsafe { memory::frame_allocator::FrameAllocator::alloc_frame() };
        if let Some(frame) = ctba_frame {
            let ctba_phys = frame.as_u64();
            
            let ctba_virt = match map_mmio(ctba_phys, 0x1000) {
                Ok(v) => v,
                Err(_) => continue,
            };
            
            unsafe {
                (*cmdheader.add(i)).prdtl = 8;
                (*cmdheader.add(i)).ctba = ctba_phys;
                core::ptr::write_bytes(ctba_virt as *mut u8, 0, 256);
            }
        }
    }
    
    start_cmd(port);
}

pub fn probe_ports(abar: &mut HbaMem) {
    let pi = abar.read_pi();
    
    for i in 0..32 {
        if (pi >> i) & 1 != 0 {
            let dt = check_type(&abar.ports[i]);
            match dt {
                AHCI_DEV_SATA => {
                    serial_println!("SATA drive found at port {}", i);
                    rebase_port(&mut abar.ports[i], i as u32);
                }
                AHCI_DEV_SATAPI => {
                    serial_println!("SATAPI drive found at port {}", i);
                    rebase_port(&mut abar.ports[i], i as u32);
                }
                AHCI_DEV_SEMB => {
                    serial_println!("SEMB drive found at port {}", i);
                }
                AHCI_DEV_PM => {
                    serial_println!("PM drive found at port {}", i);
                }
                _ => {
                }
            }
        }
    }
}

fn check_type(port: &HbaPort) -> u8 {
    let ssts = port.read_ssts();
    let ipm = (ssts >> 8) & 0x0F;
    let det = ssts & 0x0F;
    
    if det != HBA_PORT_DET_PRESENT {
        return AHCI_DEV_NULL;
    }
    if ipm != HBA_PORT_IPM_ACTIVE {
        return AHCI_DEV_NULL;
    }
    
    match port.read_sig() {
        HBA_PORT_SIG_ATAPI => AHCI_DEV_SATAPI,
        HBA_PORT_SIG_SEMB => AHCI_DEV_SEMB,
        HBA_PORT_SIG_PM => AHCI_DEV_PM,
        _ => AHCI_DEV_SATA,
    }
}

pub fn find_ahci_controller() -> Option<u64> {
    serial_println!("Scanning PCI for AHCI controller...");
    
    for bus in 0..=255u16 {
        for device in 0..32u8 {
            for function in 0..8u8 {
                let vendor_id = pci_config_read_dword(bus as u8, device, function, 0x00) & 0xFFFF;
                
                if vendor_id == 0xFFFF || vendor_id == 0x0000 {
                    continue;
                }
                
                let class_reg = pci_config_read_dword(bus as u8, device, function, 0x08);
                let class_code = (class_reg >> 24) & 0xFF;
                let subclass = (class_reg >> 16) & 0xFF;
                let prog_if = (class_reg >> 8) & 0xFF;
                
                if class_code == PCI_CLASS_MASS_STORAGE as u32 && 
                   subclass == PCI_SUBCLASS_SATA as u32 && 
                   prog_if == 0x01 {
                    serial_println!("Found AHCI controller at {}:{}:{}", bus, device, function);
                    let bar5 = pci_config_read_dword(bus as u8, device, function, 0x24);
                    let abar = (bar5 & !0xF) as u64;
                    serial_println!("BAR5 = 0x{:X}", abar);
                    return Some(abar);
                }
            }
        }
    }
    
    serial_println!("No AHCI controller found");
    None
}

pub fn ahci_read(port: &HbaPort, lba: u64, count: u32, buffer: *mut u8) -> bool {
    ahci_lock();
    let result = {
        let ci = port.read_ci();
        if ci != 0 {
            ahci_unlock();
            return false;
        }

        let cmdheader_virt = match map_mmio(port.clb, 0x1000) {
            Ok(v) => v as *mut HbaCmdHeader,
            Err(_) => {
                ahci_unlock();
                return false;
            }
        };
        
        unsafe {
            (*cmdheader_virt).prdtl = 1;
            
            let ctba_phys = (*cmdheader_virt).ctba;
            let cmdtbl_virt = match map_mmio(ctba_phys, 0x1000) {
                Ok(v) => v as *mut HbaCmdTbl,
                Err(_) => {
                    ahci_unlock();
                    return false;
                }
            };
            
            core::ptr::write_bytes(cmdtbl_virt as *mut u8, 0, 256);
            
            let fis = &mut (*cmdtbl_virt).cfis;
            fis[0] = 0x27;
            fis[1] = 0x80;
            fis[2] = 0xC8;
            fis[3] = 0x00;
            fis[4] = (lba & 0xFF) as u8;
            fis[5] = ((lba >> 8) & 0xFF) as u8;
            fis[6] = ((lba >> 16) & 0xFF) as u8;
            fis[7] = 0xE0 | ((lba >> 24) & 0x0F) as u8;
            fis[8] = ((lba >> 32) & 0xFF) as u8;
            fis[9] = ((lba >> 40) & 0xFF) as u8;
            fis[10] = ((lba >> 48) & 0xFF) as u8;
            fis[11] = 0x00;
            fis[12] = (count & 0xFF) as u8;
            fis[13] = ((count >> 8) & 0xFF) as u8;
            
            (*cmdtbl_virt).prdt_entry[0].dba = buffer as u64;
            (*cmdtbl_virt).prdt_entry[0].dbc = (count as u32 * 512) - 1;
            
            let port_mut = port as *const HbaPort as *mut HbaPort;
            (*port_mut).ci = 1;
            
            let mut timeout = 1000000;
            while ((*port_mut).ci & 1) != 0 && timeout > 0 {
                timeout -= 1;
                core::hint::spin_loop();
            }
            
            timeout > 0
        }
    };
    ahci_unlock();
    result
}

pub fn ahci_write(port: &HbaPort, lba: u64, count: u32, buffer: *const u8) -> bool {
    ahci_lock();
    let result = {
        let ci = port.read_ci();
        if ci != 0 {
            ahci_unlock();
            return false;
        }

        let cmdheader_virt = match map_mmio(port.clb, 0x1000) {
            Ok(v) => v as *mut HbaCmdHeader,
            Err(_) => {
                ahci_unlock();
                return false;
            }
        };
        
        unsafe {
            (*cmdheader_virt).prdtl = 1;
            
            let ctba_phys = (*cmdheader_virt).ctba;
            let cmdtbl_virt = match map_mmio(ctba_phys, 0x1000) {
                Ok(v) => v as *mut HbaCmdTbl,
                Err(_) => {
                    ahci_unlock();
                    return false;
                }
            };
            
            core::ptr::write_bytes(cmdtbl_virt as *mut u8, 0, 256);
            
            let fis = &mut (*cmdtbl_virt).cfis;
            fis[0] = 0x27;
            fis[1] = 0x80;
            fis[2] = 0xCA;
            fis[3] = 0x00;
            fis[4] = (lba & 0xFF) as u8;
            fis[5] = ((lba >> 8) & 0xFF) as u8;
            fis[6] = ((lba >> 16) & 0xFF) as u8;
            fis[7] = 0xE0 | ((lba >> 24) & 0x0F) as u8;
            fis[8] = ((lba >> 32) & 0xFF) as u8;
            fis[9] = ((lba >> 40) & 0xFF) as u8;
            fis[10] = ((lba >> 48) & 0xFF) as u8;
            fis[11] = 0x00;
            fis[12] = (count & 0xFF) as u8;
            fis[13] = ((count >> 8) & 0xFF) as u8;
            
            (*cmdtbl_virt).prdt_entry[0].dba = buffer as u64;
            (*cmdtbl_virt).prdt_entry[0].dbc = (count as u32 * 512) - 1;
            
            let port_mut = port as *const HbaPort as *mut HbaPort;
            (*port_mut).ci = 1;
            
            let mut timeout = 1000000;
            while ((*port_mut).ci & 1) != 0 && timeout > 0 {
                timeout -= 1;
                core::hint::spin_loop();
            }
            
            timeout > 0
        }
    };
    ahci_unlock();
    result
}

pub fn init_ahci() {
    ahci_lock();
    match pci_find_ahci_controller() {
        Some(ahci_dev) => {
            let abar_phys = ahci_dev.bar[5] as u64 & !0xF;
            serial_println!("AHCI controller found at {}:{}:{}", ahci_dev.bus, ahci_dev.device, ahci_dev.function);
            serial_println!("AHCI BAR5 (physical): 0x{:X}", abar_phys);

            if abar_phys == 0 {
                serial_println!("Invalid AHCI BAR address");
                ahci_unlock();
            } else {
                pci_enable_bus_master(ahci_dev.bus, ahci_dev.device, ahci_dev.function);
                pci_enable_memory_space(ahci_dev.bus, ahci_dev.device, ahci_dev.function);

                serial_println!("Mapping AHCI MMIO region...");
                match map_mmio(abar_phys, 0x4000) {
                    Ok(abar_virt) => {
                        serial_println!("AHCI ABAR mapped at virtual: 0x{:X}", abar_virt);
                        serial_println!("AHCI ABAR mapped successfully");
                        
                        let abar = unsafe { &mut *(abar_virt as *mut HbaMem) };
                        
                        ahci_unlock();
                        probe_ports(abar);
                        serial_println!("AHCI initialization complete");
                        return;
                    }
                    Err(e) => {
                        serial_println!("Failed to map AHCI MMIO: {}", e);
                        ahci_unlock();
                    }
                }
            }
        }
        None => {
            serial_println!("No AHCI controller found");
            ahci_unlock();
        }
    }
}