#![no_std]

extern crate alloc;

use alloc::vec::Vec;
use framebuffer::println;
use memory::hhdm::virt_to_phys;
use memory::mmio::map_mmio;
use pci::{pci_enable_bus_master, pci_enable_memory_space, pci_find_ahci_controller};
use core::sync::atomic::{AtomicBool, Ordering};
use spin::Mutex;

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

// -- drive registry ----------------------------------------------------------
// after init we keep a list of every drive we found so the rest of the
// kernel can just say "read from drive 0" without juggling port pointers

pub struct AhciDrive {
    pub port_ptr: *mut HbaPort,
    pub port_type: u8,
    pub sector_count: u64,
}

// the port pointer is MMIO that lives forever, and we only touch it under
// AHCI_LOCK, so shipping it across threads is fine
unsafe impl Send for AhciDrive {}
unsafe impl Sync for AhciDrive {}

static AHCI_DRIVES: Mutex<Vec<AhciDrive>> = Mutex::new(Vec::new());

pub fn get_drive_count() -> usize {
    AHCI_DRIVES.lock().len()
}

pub fn get_drive_sector_count(index: usize) -> Option<u64> {
    AHCI_DRIVES.lock().get(index).map(|d| d.sector_count)
}

/// Read `count` sectors from drive `index` at `lba` into `buffer`.
pub fn ahci_read_drive(index: usize, lba: u64, count: u32, buffer: *mut u8) -> bool {
    let port_ptr = match AHCI_DRIVES.lock().get(index) {
        Some(d) => d.port_ptr,
        None => return false,
    };
    ahci_read(unsafe { &mut *port_ptr }, lba, count, buffer)
}

/// Write `count` sectors from `buffer` to drive `index` at `lba`.
pub fn ahci_write_drive(index: usize, lba: u64, count: u32, buffer: *const u8) -> bool {
    let port_ptr = match AHCI_DRIVES.lock().get(index) {
        Some(d) => d.port_ptr,
        None => return false,
    };
    ahci_write(unsafe { &mut *port_ptr }, lba, count, buffer)
}

// -- port command engine -----------------------------------------------------

fn start_cmd(port: &mut HbaPort) {
    // wait for any in-flight command DMA to drain before flipping the bits
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

// scan sact (NCQ active) and ci (command issued) to find a slot nobody is
// using right now
fn find_cmdslot(port: &HbaPort) -> Option<u32> {
    let busy = port.read_sact() | port.read_ci();
    for i in 0..32u32 {
        if (busy >> i) & 1 == 0 {
            return Some(i);
        }
    }
    None
}

fn rebase_port(port: &mut HbaPort, portno: u32) -> bool {
    stop_cmd(port);

    // grab two fresh pages: one for the command list, one for received FISes
    let clb_frame = memory::frame_allocator::FrameAllocator::alloc_frame();
    let fb_frame  = memory::frame_allocator::FrameAllocator::alloc_frame();

    let (clb_phys, fb_phys) = match (clb_frame, fb_frame) {
        (Some(c), Some(f)) => (c.as_u64(), f.as_u64()),
        _ => {
            println!("AHCI: out of frames for port {}", portno);
            return false;
        }
    };

    let clb_virt = match map_mmio(memory::vmm::VMM::get_page_table(), clb_phys, 0x1000) {
        Ok(v) => v,
        Err(_) => {
            println!("AHCI: couldn't map CLB for port {}", portno);
            return false;
        }
    };

    let fb_virt = match map_mmio(memory::vmm::VMM::get_page_table(), fb_phys, 0x1000) {
        Ok(v) => v,
        Err(_) => {
            println!("AHCI: couldn't map FIS buffer for port {}", portno);
            return false;
        }
    };

    port.set_clb(clb_phys);
    unsafe { core::ptr::write_bytes(clb_virt as *mut u8, 0, 1024); }

    port.set_fb(fb_phys);
    unsafe { core::ptr::write_bytes(fb_virt as *mut u8, 0, 256); }

    // each of the 32 command slots needs its own command table page
    let cmdheader = clb_virt as *mut HbaCmdHeader;
    for i in 0..32usize {
        let frame = match memory::frame_allocator::FrameAllocator::alloc_frame() {
            Some(f) => f,
            None => {
                println!("AHCI: out of frames for port {} slot {}", portno, i);
                return false;
            }
        };
        let ctba_phys = frame.as_u64();

        let ctba_virt = match map_mmio(memory::vmm::VMM::get_page_table(), ctba_phys, 0x1000) {
            Ok(v) => v,
            Err(_) => {
                println!("AHCI: couldn't map command table for port {} slot {}", portno, i);
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

// fills in a Host-to-Device Register FIS — the packet we send the drive to
// tell it what we want
fn build_fis(fis: &mut [u8; 64], command: u8, lba: u64, count: u32) {
    fis[0]  = FIS_TYPE_REG_H2D;
    fis[1]  = 1 << 7;          // C=1 means this is a command, not a control write
    fis[2]  = command;
    fis[3]  = 0x00;             // features low (unused here)
    fis[4]  = (lba & 0xFF) as u8;
    fis[5]  = ((lba >> 8)  & 0xFF) as u8;
    fis[6]  = ((lba >> 16) & 0xFF) as u8;
    fis[7]  = 0x40;             // device register: bit 6 selects LBA mode
    fis[8]  = ((lba >> 24) & 0xFF) as u8;
    fis[9]  = ((lba >> 32) & 0xFF) as u8;
    fis[10] = ((lba >> 40) & 0xFF) as u8;
    fis[11] = 0x00;             // features high (unused here)
    fis[12] = (count & 0xFF) as u8;
    fis[13] = ((count >> 8) & 0xFF) as u8;
}

/// The core function that actually kicks off a command.
///
/// `fis_count` is the sector count we put in the FIS itself — for normal
/// reads/writes this equals `count`, but IDENTIFY doesn't use it so pass 0.
/// `byte_count` is how many bytes the PRDT entry should cover.
fn issue_command(
    port: &mut HbaPort,
    lba: u64,
    buffer: u64,
    fis_count: u32,
    byte_count: u32,
    command: u8,
) -> bool {
    // wipe any leftover interrupt/error bits so they don't confuse our poll loop
    port.write_is(!0u32);
    port.write_serr(!0u32);

    let slot = match find_cmdslot(port) {
        Some(s) => s,
        None => {
            println!("AHCI: all command slots busy");
            return false;
        }
    };

    let clb_phys = port.clb();
    let cmdheader_virt = match map_mmio(memory::vmm::VMM::get_page_table(), clb_phys, 0x1000) {
        Ok(v) => v as *mut HbaCmdHeader,
        Err(_) => return false,
    };

    // the HBA talks to physical addresses directly over DMA — it has no idea
    // our virtual address space exists, so we have to give it the real thing
    let buf_phys = virt_to_phys(buffer as usize) as u64;

    unsafe {
        let hdr = &mut *cmdheader_virt.add(slot as usize);
        hdr.prdtl = 1;
        hdr.prdbc = 0;
        hdr.flags = AHCI_CMD_HEADER_FLAGS_FIS_LEN
            | if command == ATA_CMD_WRITE_DMA_EX {
                AHCI_CMD_HEADER_FLAGS_WRITE  // tell the HBA data flows host→device
            } else {
                0
            };

        let ctba_phys = hdr.ctba();
        let cmdtbl_virt = match map_mmio(memory::vmm::VMM::get_page_table(), ctba_phys, 0x1000) {
            Ok(v) => v as *mut HbaCmdTbl,
            Err(_) => return false,
        };

        let tbl = &mut *cmdtbl_virt;
        core::ptr::write_bytes(
            tbl as *mut HbaCmdTbl as *mut u8,
            0,
            core::mem::size_of::<HbaCmdTbl>(),
        );

        build_fis(&mut tbl.cfis, command, lba, fis_count);

        tbl.prdt_entry[0].set_dba(buf_phys);
        tbl.prdt_entry[0].dbc = byte_count - 1; // DBC is 0-based per the spec
    }

    // writing to CI is the "doorbell" — this is what actually starts the command
    port.write_ci(1 << slot);

    // spin until the slot clears or something goes wrong
    let mut timeout = 1_000_000u32;
    loop {
        if timeout == 0 {
            println!("AHCI: drive didn't respond (slot {})", slot);
            return false;
        }
        if port.read_is() & HBA_PX_IS_TFES != 0 {
            println!("AHCI: drive reported error — IS={:#010x} TFD={:#010x}", port.read_is(), port.read_tfd());
            return false;
        }
        if port.read_ci() & (1 << slot) == 0 {
            break; // slot cleared, command done
        }
        timeout -= 1;
        core::hint::spin_loop();
    }

    port.read_is() & HBA_PX_IS_TFES == 0
}

// -- public port-level API ---------------------------------------------------

/// Read `count` 512-byte sectors from `port` at `lba` into `buffer`.
pub fn ahci_read(port: &mut HbaPort, lba: u64, count: u32, buffer: *mut u8) -> bool {
    ahci_lock();
    let result = issue_command(port, lba, buffer as u64, count, count * 512, ATA_CMD_READ_DMA_EX);
    ahci_unlock();
    result
}

/// Write `count` 512-byte sectors from `buffer` to `port` at `lba`.
pub fn ahci_write(port: &mut HbaPort, lba: u64, count: u32, buffer: *const u8) -> bool {
    ahci_lock();
    let result = issue_command(port, lba, buffer as u64, count, count * 512, ATA_CMD_WRITE_DMA_EX);
    ahci_unlock();
    result
}

/// Ask the drive to identify itself and return how many sectors it has.
/// Returns None if the drive doesn't respond or the command fails.
pub fn ahci_identify(port: &mut HbaPort) -> Option<u64> {
    use alloc::vec;
    let mut buf = vec![0u8; 512];

    ahci_lock();
    // IDENTIFY always returns exactly 512 bytes and ignores the sector count
    // field in the FIS, so we pass 0 there
    let ok = issue_command(port, 0, buf.as_mut_ptr() as u64, 0, 512, ATA_CMD_IDENTIFY);
    ahci_unlock();

    if !ok {
        return None;
    }

    // the IDENTIFY response is 256 little-endian 16-bit words
    let words = unsafe { core::slice::from_raw_parts(buf.as_ptr() as *const u16, 256) };

    // word 83 bit 10: drive supports 48-bit LBA (most drives made after ~2003)
    let lba48 = (words[83] & (1 << 10)) != 0;

    let sector_count = if lba48 {
        // words 100–103 hold the full 64-bit sector count
        (words[100] as u64)
            | ((words[101] as u64) << 16)
            | ((words[102] as u64) << 32)
            | ((words[103] as u64) << 48)
    } else {
        // older 28-bit LBA: sector count is in words 60–61, max ~128 GiB
        (words[60] as u64) | ((words[61] as u64) << 16)
    };

    Some(sector_count)
}

// -- device detection --------------------------------------------------------

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

fn probe_ports(abar: &mut HbaMem) {
    let pi = abar.read_pi(); // port-implemented bitmask
    for i in 0..32usize {
        if (pi >> i) & 1 == 0 {
            continue; // controller says nothing is here
        }
        let dt = check_type(&abar.ports[i]);
        match dt {
            AHCI_DEV_SATA | AHCI_DEV_SATAPI => {
                let label = if dt == AHCI_DEV_SATA { "SATA" } else { "SATAPI" };
                println!("AHCI: {} drive at port {}", label, i);

                let port = &mut abar.ports[i] as *mut HbaPort;
                if !rebase_port(unsafe { &mut *port }, i as u32) {
                    continue;
                }

                let sector_count = ahci_identify(unsafe { &mut *port }).unwrap_or(0);
                if sector_count > 0 {
                    println!(
                        "AHCI:   port {} -> {} sectors ({} MiB)",
                        i,
                        sector_count,
                        (sector_count * 512) / (1024 * 1024)
                    );
                }

                AHCI_DRIVES.lock().push(AhciDrive {
                    port_ptr: port,
                    port_type: dt,
                    sector_count,
                });
            }
            AHCI_DEV_SEMB => println!("AHCI: SEMB device at port {}", i),
            AHCI_DEV_PM   => println!("AHCI: port multiplier at port {}", i),
            _ => {}
        }
    }
}

// -- initialization ----------------------------------------------------------

pub fn init_ahci() {
    let ahci_dev = match pci_find_ahci_controller() {
        Some(d) => d,
        None => {
            println!("AHCI: no controller found on PCI bus");
            return;
        }
    };

    let abar_phys = ahci_dev.bar[5] as u64 & !0xF; // BAR5 is the AHCI base address register
    println!(
        "AHCI: controller at {}:{}:{}",
        ahci_dev.bus, ahci_dev.device, ahci_dev.function
    );

    if abar_phys == 0 {
        println!("AHCI: BAR5 is zero, controller didn't initialize properly");
        return;
    }

    pci_enable_bus_master(ahci_dev.bus, ahci_dev.device, ahci_dev.function);
    pci_enable_memory_space(ahci_dev.bus, ahci_dev.device, ahci_dev.function);

    let abar_virt = match map_mmio(memory::vmm::VMM::get_page_table(), abar_phys, 0x4000) {
        Ok(v) => v,
        Err(e) => {
            println!("AHCI: couldn't map controller registers: {}", e);
            return;
        }
    };

    let abar = unsafe { &mut *(abar_virt as *mut HbaMem) };

    // GHC.AE (bit 31) switches the controller from legacy IDE compat mode to
    // AHCI mode. must be set before we touch any port registers
    abar.write_ghc(abar.read_ghc() | HBA_GHC_AE);

    probe_ports(abar);
    println!("AHCI: done, found {} drive(s)", get_drive_count());
}
