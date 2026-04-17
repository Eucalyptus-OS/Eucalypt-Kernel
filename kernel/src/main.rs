#![no_std]
#![no_main]

extern crate alloc;

use alloc::boxed::Box;
// Eucalypt
use eucalypt_os::idt::idt_init;
use eucalypt_os::mp::init_mp;

use limine::BaseRevision;
use limine::{
    RequestsEndMarker, RequestsStartMarker,
    request::{FramebufferRequest, MemmapRequest, ModulesRequest, MpRequest},
};

use framebuffer::println;

use bare_x86_64::cpu::apic::{
    calibrate_apic_timer, enable_apic, get_apic_base, init_apic_timer, set_apic_virt_base,
};

use gdt::gdt_init;

use memory::{
    allocator::init_allocator,
    mmio::{map_mmio, mmio_map_range},
    vmm::VMM,
};

use ahci::{init_ahci, ahci_read_drive, get_drive_count};
use ide::ide_init;
use pci::check_all_buses;
#[allow(unused)]
use process::scheduler::{disable_scheduler, enable_scheduler};
use ramfs::mount_ramdisk;
use usb::init_usb;

use framebuffer::ScrollingTextRenderer;
use vfs::*;

static FONT: &[u8] = include_bytes!("../../framebuffer/font/altc-8x16.psf");

#[used]
#[unsafe(link_section = ".requests")]
static BASE_REVISION: BaseRevision = BaseRevision::new();

#[used]
#[unsafe(no_mangle)]
#[unsafe(link_section = ".requests")]
pub static FRAMEBUFFER_REQUEST: FramebufferRequest = FramebufferRequest::new();

#[used]
#[unsafe(link_section = ".requests")]
static MEMMAP_REQUEST: MemmapRequest = MemmapRequest::new();

#[used]
#[unsafe(no_mangle)]
#[unsafe(link_section = ".requests")]
pub static MP_REQUEST: MpRequest = MpRequest::new(0);

#[used]
#[unsafe(no_mangle)]
#[unsafe(link_section = ".requests")]
static MODULE_REQUEST: ModulesRequest = ModulesRequest::new();

#[used]
#[unsafe(link_section = ".requests_start_marker")]
static _START_MARKER: RequestsStartMarker = RequestsStartMarker::new();

#[used]
#[unsafe(link_section = ".requests_end_marker")]
static _END_MARKER: RequestsEndMarker = RequestsEndMarker::new();

#[unsafe(no_mangle)]
extern "C" fn kmain() -> ! {
    let framebuffer_response = FRAMEBUFFER_REQUEST
        .response()
        .expect("No framebuffer response");
    let framebuffer = framebuffer_response
        .framebuffers()
        .first()
        .copied()
        .expect("No framebuffer available");

    ScrollingTextRenderer::init(
        framebuffer.address() as *mut u8,
        framebuffer.width as usize,
        framebuffer.height as usize,
        framebuffer.pitch as usize,
        framebuffer.bpp as usize,
        FONT,
    );
    assert!(BASE_REVISION.is_supported());

    println!("eucalyptOS Starting...");

    let memmap_response = MEMMAP_REQUEST.response().expect("No memory map available");
    let _vmm = VMM::init(memmap_response);
    init_allocator(memmap_response);

    gdt_init();

    mmio_map_range(0xFFFF800000000000, 0xFFFF8000FFFFFFFF);

    let apic_virt = map_mmio(VMM::get_page_table(), get_apic_base() as u64, 0x1000)
        .expect("Failed to map APIC");
    set_apic_virt_base(apic_virt as usize);

    let ioapic_virt =
        map_mmio(VMM::get_page_table(), 0xFEC00000, 0x1000).expect("Failed to map IOAPIC");
    bare_x86_64::cpu::apic::set_ioapic_virt_base(ioapic_virt as usize);
    bare_x86_64::cpu::apic::init_ioapic();

    idt_init();

    process::thread::init_kernel_thread();    

    enable_apic();
    unsafe {
        core::arch::asm!("sti");
    }

    let initial_count = calibrate_apic_timer(1000);
    init_apic_timer(32, initial_count);

    ide_init(0, 0, 0, 0, 0);
    check_all_buses();
    init_usb();
    init_ahci();

    if get_drive_count() > 0 {
        let mut buf = [0u8; 512];
        let ok = ahci_read_drive(0, 0, 1, buf.as_mut_ptr());
        println!("AHCI read sector 0: ok={}", ok);
        if ok {
            println!("  first 16 bytes: {:02x?}", &buf[..16]);
        }
    }

    let mp_response = MP_REQUEST.response().expect("No MP response");
    init_mp(mp_response);

    vfs_init();
    if let Some(module_response) = MODULE_REQUEST.response() {
        mount_ramdisk(module_response, "ram").expect("Failed to mount ramdisk");
    } else {
        vfs_mount("ram", Box::new(ramfs::RamFs::new())).expect("Failed to mount empty ramfs");
    }

    tty::tty_init();
    tty::tty_write_str("eucalyptOS\n\n> ");

    loop {
        unsafe { core::arch::asm!("hlt"); }
    }
}

#[cfg(not(test))]
#[panic_handler]
fn rust_panic(info: &core::panic::PanicInfo) -> ! {
    disable_scheduler();

    fn lookup_symbol(addr: u64) -> Option<(&'static str, u64)> {
        static SYMBOL_MAP: &str = include_str!("../kernel.map");
        let mut best_name = None;
        let mut best_addr = 0u64;
        for line in SYMBOL_MAP.lines() {
            let mut parts = line.split_whitespace();
            let sym_addr = u64::from_str_radix(parts.next()?, 16).ok()?;
            let kind = parts.next()?;
            let name = parts.next()?;
            if kind == "T" || kind == "t" {
                if sym_addr <= addr && sym_addr > best_addr {
                    best_addr = sym_addr;
                    best_name = Some(name);
                }
            }
        }
        best_name.map(|n| (n, addr - best_addr))
    }

    use core::arch::asm;
    use framebuffer::{color, fill_screen, kprintln};

    let mut rbp: u64;
    let rip: u64;

    unsafe {
        asm!(
            "mov {}, rbp",
            "lea {}, [rip]",
            out(reg) rbp,
            out(reg) rip,
        );
    }

    fill_screen(color::DARK_BLUE);
    framebuffer::RENDERER.with(|r| r.set_colors(color::WHITE, color::DARK_BLUE));

    let file = info.location().map(|l| l.file()).unwrap_or("unknown");
    let line = info.location().map(|l| l.line()).unwrap_or(0);

    kprintln!("panic: {}", info.message());
    kprintln!("at {}:{}", file, line);

    unsafe {
        let mut depth = 0;
        while rbp != 0 && depth < 16 {
            let ret_addr = *((rbp + 8) as *const u64);
            if ret_addr == 0 {
                break;
            }
            if let Some((name, offset)) = lookup_symbol(ret_addr) {
                kprintln!("#{} {}+0x{:x}", depth, name, offset);
            } else {
                kprintln!("#{} 0x{:016x}", depth, ret_addr);
            }
            rbp = *(rbp as *const u64);
            depth += 1;
        }
    }

    if let Some((name, offset)) = lookup_symbol(rip) {
        kprintln!("rip {}+0x{:x}", name, offset);
    } else {
        kprintln!("rip 0x{:016x}", rip);
    }

    loop {
        unsafe {
            asm!("cli", "hlt");
        }
    }
}