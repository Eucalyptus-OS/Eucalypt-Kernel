#![no_std]
#![no_main]

extern crate alloc;

// Eucalypt
use eucalypt_os::gdt::gdt_init;
use eucalypt_os::idt::{self, idt_init};
use eucalypt_os::mp::init_mp;

// Limine
use limine::BaseRevision;
use limine::{RequestsEndMarker, RequestsStartMarker, request::{
    FramebufferRequest, MemmapRequest, ModulesRequest, MpRequest
}};

// Hardware
use framebuffer::println;

use bare_x86_64::cpu::apic::{
    enable_apic,
    get_apic_base,
    set_apic_virt_base,
    init_apic_timer,
    calibrate_apic_timer,
};

use memory::{
    mmio::{
        map_mmio,
        mmio_map_range,
    },
    vmm::VMM,
    allocator::init_allocator,
};

use pci::check_all_buses;
use ide::ide_init;
use ahci::init_ahci;
use ramfs::mount_ramdisk;
use usb::init_usb;

//use sched::{
//    init_scheduler,
//    enable_scheduler,
//    sleep_proc_ms,
//};
//use process::{
//    init_kernel_process,
//    create_process,
//};

use framebuffer::{
    ScrollingTextRenderer,
};

// FS
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
    assert!(BASE_REVISION.is_supported());

    let framebuffer_response = FRAMEBUFFER_REQUEST.response().expect("No framebuffer response");
    let framebuffer = framebuffer_response.framebuffers().first().copied().expect("No framebuffer available");
    ScrollingTextRenderer::init(
        framebuffer.address() as *mut u8,
        framebuffer.width as usize,
        framebuffer.height as usize,
        framebuffer.pitch as usize,
        framebuffer.bpp as usize,
        FONT,
    );
    println!("eucalyptOS Starting...");

    let memmap_response = MEMMAP_REQUEST.response().expect("No memory map available");
    let _vmm = VMM::init(memmap_response);
    init_allocator(memmap_response);

    gdt_init();

    // 4. MMIO & APIC/IOAPIC Setup (Must be done before IDT init to avoid 0x0 Page Fault)
    mmio_map_range(0xFFFF800000000000, 0xFFFF8000FFFFFFFF);

    let apic_virt = map_mmio(VMM::get_page_table(), get_apic_base() as u64, 0x1000).expect("Failed to map APIC");
    set_apic_virt_base(apic_virt as usize);

    let ioapic_virt = map_mmio(VMM::get_page_table(), 0xFEC00000, 0x1000).expect("Failed to map IOAPIC");
    bare_x86_64::cpu::apic::set_ioapic_virt_base(ioapic_virt as usize);
    bare_x86_64::cpu::apic::init_ioapic();

    idt_init();

    process::thread::init_kernel_thread();
    
    // Test threads with 16KB stack
    process::thread::TCB::new(16384, test_process_1 as *const () as u64);
    process::thread::TCB::new(16384, test_process_2 as *const () as u64);

    enable_apic();
    unsafe { core::arch::asm!("sti"); }

    let initial_count = calibrate_apic_timer(1000);
    init_apic_timer(32, initial_count);

    ide_init(0, 0, 0, 0, 0);
    check_all_buses();
    init_usb();
    init_ahci();

    let mp_response = MP_REQUEST.response().expect("No MP response");
    init_mp(mp_response);

    vfs_init();
    if let Some(module_response) = MODULE_REQUEST.response() {
        mount_ramdisk(module_response, "ram").expect("Failed to mount ramdisk");
    }

    loop {
        unsafe { core::arch::asm!("hlt"); }
    }
}

fn test_process_1() {
    loop {
        println!("A");
    }
}

fn test_process_2() {
    loop {
        println!("B");
    }
}

#[panic_handler]
fn rust_panic(info: &core::panic::PanicInfo) -> ! {
    //disable_scheduler();
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
    use framebuffer::{fill_screen, color, kprintln};
    //use sched::disable_scheduler;

    #[repr(C)]
    struct Regs {
        rax: u64, rbx: u64, rcx: u64, rdx: u64,
        rsi: u64, rdi: u64, rbp: u64, rsp: u64,
        r8:  u64, r9:  u64, r10: u64, r11: u64,
        r12: u64, r13: u64, r14: u64, r15: u64,
        rflags: u64, cs: u64, ss: u64, rip: u64,
    }

    let mut regs = Regs {
        rax: 0, rbx: 0, rcx: 0, rdx: 0,
        rsi: 0, rdi: 0, rbp: 0, rsp: 0,
        r8:  0, r9:  0, r10: 0, r11: 0,
        r12: 0, r13: 0, r14: 0, r15: 0,
        rflags: 0, cs: 0, ss: 0, rip: 0,
    };

    unsafe {
        asm!(
            "mov [{0} + 0x00], rax",
            "mov [{0} + 0x08], rbx",
            "mov [{0} + 0x10], rcx",
            "mov [{0} + 0x18], rdx",
            "mov [{0} + 0x20], rsi",
            "mov [{0} + 0x28], rdi",
            "mov [{0} + 0x30], rbp",
            "mov [{0} + 0x38], rsp",
            "mov [{0} + 0x40], r8",
            "mov [{0} + 0x48], r9",
            "mov [{0} + 0x50], r10",
            "mov [{0} + 0x58], r11",
            "mov [{0} + 0x60], r12",
            "mov [{0} + 0x68], r13",
            "mov [{0} + 0x70], r14",
            "mov [{0} + 0x78], r15",
            "pushfq",
            "pop qword ptr [{0} + 0x80]",
            "mov rax, cs",
            "mov [{0} + 0x88], rax",
            "mov rax, ss",
            "mov [{0} + 0x90], rax",
            "lea rax, [rip]",
            "mov [{0} + 0x98], rax",
            in(reg) &mut regs as *mut Regs,
            out("rax") _,
        );
    }

    fill_screen(color::DARK_RED);
    framebuffer::RENDERER.with(|r| r.set_colors(color::WHITE, color::DARK_RED));

    let file = info.location().map(|l| l.file()).unwrap_or("unknown");
    let line = info.location().map(|l| l.line()).unwrap_or(0);
    let col  = info.location().map(|l| l.column()).unwrap_or(0);

    kprintln!("  KERNEL PANIC");
    kprintln!("  Message  : {}", info.message());
    kprintln!("  Location : {}:{}:{}", file, line, col);

    kprintln!("  Stack Trace:");
    unsafe {
        let mut rbp = regs.rbp;
        let mut depth = 0;
        while rbp != 0 && depth < 16 {
            let ret_addr = *((rbp + 8) as *const u64);
            if ret_addr == 0 { break; }
            match lookup_symbol(ret_addr) {
                Some((name, offset)) => kprintln!("    #{}: 0x{:016X} <{}+0x{:X}>", depth, ret_addr, name, offset),
                None                 => kprintln!("    #{}: 0x{:016X} <unknown>", depth, ret_addr),
            }
            rbp = *(rbp as *const u64);
            depth += 1;
        }
    }

    kprintln!("  RIP      : 0x{:016X}", regs.rip);
    match lookup_symbol(regs.rip) {
        Some((name, offset)) => kprintln!("           <{}+0x{:X}>", name, offset),
        None                 => kprintln!("           <unknown>"),
    }

    kprintln!("  RAX: 0x{:016X}   RBX: 0x{:016X}", regs.rax, regs.rbx);
    kprintln!("  RCX: 0x{:016X}   RDX: 0x{:016X}", regs.rcx, regs.rdx);
    kprintln!("  RSI: 0x{:016X}   RDI: 0x{:016X}", regs.rsi, regs.rdi);
    kprintln!("  RBP: 0x{:016X}   RSP: 0x{:016X}", regs.rbp, regs.rsp);
    kprintln!("  R8:  0x{:016X}   R9:  0x{:016X}", regs.r8,  regs.r9);
    kprintln!("  R10: 0x{:016X}   R11: 0x{:016X}", regs.r10, regs.r11);
    kprintln!("  R12: 0x{:016X}   R13: 0x{:016X}", regs.r12, regs.r13);
    kprintln!("  R14: 0x{:016X}   R15: 0x{:016X}", regs.r14, regs.r15);
    kprintln!("  RFLAGS: 0x{:016X}", regs.rflags);
    kprintln!("  CF={} PF={} AF={} ZF={} SF={} IF={} DF={} OF={}",
        (regs.rflags >> 0)  & 1, (regs.rflags >> 2)  & 1,
        (regs.rflags >> 4)  & 1, (regs.rflags >> 6)  & 1,
        (regs.rflags >> 7)  & 1, (regs.rflags >> 9)  & 1,
        (regs.rflags >> 10) & 1, (regs.rflags >> 11) & 1,
    );
    kprintln!("  CS: 0x{:04X}   SS: 0x{:04X}", regs.cs, regs.ss);
    kprintln!("  System Halted.");

    for _ in 0..100000 {
        loop { unsafe { asm!("hlt"); } }
    }

    loop { unsafe { asm!("cli", "hlt"); } }
}