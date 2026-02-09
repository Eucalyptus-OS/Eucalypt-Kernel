#![no_std]
#![no_main]

extern crate alloc;

// Core
use core::arch::asm;

// Eucalypt
use eucalypt_os::gdt::gdt_init;
use eucalypt_os::idt::idt_init;
use eucalypt_os::mp::init_mp;

// Limine
use limine::BaseRevision;
use limine::request::{
    FramebufferRequest,
    MemoryMapRequest,
    RsdpRequest,
    MpRequest,
    RequestsStartMarker,
    RequestsEndMarker,
};

// Hardware
use serial::serial_println;

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
use usb::init_usb;

use sched::{
    init_scheduler,
    enable_scheduler,
    sleep_proc_ms,
};
use process::{
    init_kernel_process,
    create_process,
};

use framebuffer::{
    ScrollingTextRenderer,
    panic_print,
    println,
};

// FS
use fat12::Fat12FileSystem;

static FONT: &[u8] = include_bytes!("../../framebuffer/font/def2_8x16.psf");

#[used]
#[unsafe(link_section = ".requests")]
static BASE_REVISION: BaseRevision = BaseRevision::new();

#[used]
#[unsafe(no_mangle)]
#[unsafe(link_section = ".requests")]
pub static FRAMEBUFFER_REQUEST: FramebufferRequest = FramebufferRequest::new();

#[used]
#[unsafe(link_section = ".requests")]
static MEMMAP_REQUEST: MemoryMapRequest = MemoryMapRequest::new();

#[used]
#[unsafe(no_mangle)]
#[unsafe(link_section = ".requests")]
pub static RSDP_REQUEST: RsdpRequest = RsdpRequest::new();

#[used]
#[unsafe(no_mangle)]
#[unsafe(link_section = ".requests")]
pub static MP_REQUEST: MpRequest = MpRequest::new();

#[used]
#[unsafe(link_section = ".requests_start_marker")]
static _START_MARKER: RequestsStartMarker = RequestsStartMarker::new();

#[used]
#[unsafe(link_section = ".requests_end_marker")]
static _END_MARKER: RequestsEndMarker = RequestsEndMarker::new();

#[unsafe(no_mangle)]
extern "C" fn kmain() -> ! {
    assert!(BASE_REVISION.is_supported());

    let framebuffer_response = FRAMEBUFFER_REQUEST
        .get_response()
        .expect("No framebuffer response");
    let framebuffer = framebuffer_response
        .framebuffers()
        .next()
        .expect("No framebuffer available");
    ScrollingTextRenderer::init(
        framebuffer.addr(),
        framebuffer.width() as usize,
        framebuffer.height() as usize,
        framebuffer.pitch() as usize,
        framebuffer.bpp() as usize,
        FONT,
    );
    serial_println!("eucalyptOS Starting...");

    let memmap_response = MEMMAP_REQUEST
        .get_response()
        .expect("No memory map available");
    VMM::init(memmap_response);
    init_allocator(memmap_response);

    gdt_init();
    idt_init();
    unsafe {
        asm!("sti");
    }
    mmio_map_range(0xFFFF800000000000, 0xFFFF8000FFFFFFFF);
    let apic_virt = map_mmio(get_apic_base() as u64, 0x1000)
        .expect("Failed to map APIC MMIO region");
    set_apic_virt_base(apic_virt as usize);
    enable_apic();
    let initial_count = calibrate_apic_timer(1000);
    init_apic_timer(32, initial_count);
    ide_init(0, 0, 0, 0, 0);
    check_all_buses();
    init_usb();
    init_ahci();
    let mp_response = MP_REQUEST.get_response().expect("No MP response from Limine");
    init_mp(mp_response);

    serial_println!("Initializing FAT12 filesystem...");
    match Fat12FileSystem::new(0) {
        Ok(mut fs) => {
            serial_println!("FAT12 initialized successfully");
            let data = b"Hello, World";
            match fs.create_file("hello.txt", data) {
                Ok(_) => {
                    serial_println!("Created hello.txt");
                    match fs.read_root_directory() {
                        Ok(entries) => {
                            for entry in entries {
                                if let Ok(name) = entry.get_name() {
                                    if name == "HELLO.TXT" {
                                        match fs.read_file(&entry) {
                                            Ok(content) => {
                                                let content_str = core::str::from_utf8(&content).unwrap_or("Invalid UTF-8");
                                                serial_println!("File content: {}", content_str);
                                            }
                                            Err(e) => serial_println!("Failed to read file: {}", e),
                                        }
                                    }
                                }
                            }
                        }
                        Err(e) => serial_println!("Failed to read directory: {}", e),
                    }
                }
                Err(e) => serial_println!("Failed to create file: {}", e),
            }
        }
        Err(e) => {
            serial_println!("FAT12 initialization failed: {}", e);
            serial_println!("This is expected if the disk is not formatted as FAT12");
        }
    }

    let kernel_main_rsp: u64;
    serial_println!("Getting RSP");
    unsafe {
        asm!("mov {}, rsp", out(reg) kernel_main_rsp);
    }
    serial_println!("Kernel RSP: {}", kernel_main_rsp);
    init_kernel_process(kernel_main_rsp);
    serial_println!("Creating Processes");
    create_process(test_process_1 as *mut ()).expect("Failed to create process 1");
    create_process(test_process_2 as *mut ()).expect("Failed to create process 2");
    init_scheduler();
    enable_scheduler();

    loop {
        unsafe {
            asm!("hlt");
        }
    }
}

fn test_process_1() {
    loop {
        serial_println!("Process 1 running");
        sleep_proc_ms(1000);
    }
}

fn test_process_2() {
    loop {
        serial_println!("Process 2 running");
        sleep_proc_ms(1000);
    }
}

#[panic_handler]
fn rust_panic(info: &core::panic::PanicInfo) -> ! {
    let (rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp): (u64, u64, u64, u64, u64, u64, u64, u64);
    let (r8, r9, r10, r11, r12, r13, r14, r15): (u64, u64, u64, u64, u64, u64, u64, u64);
    let (rflags, cs, ss): (u64, u16, u16);

    unsafe {
        asm!(
            "mov {}, rax", "mov {}, rbx", "mov {}, rcx", "mov {}, rdx",
            out(reg) rax, out(reg) rbx, out(reg) rcx, out(reg) rdx,
        );
        asm!(
            "mov {}, rsi", "mov {}, rdi", "mov {}, rbp", "mov {}, rsp",
            out(reg) rsi, out(reg) rdi, out(reg) rbp, out(reg) rsp,
        );
        asm!(
            "mov {}, r8", "mov {}, r9", "mov {}, r10", "mov {}, r11",
            out(reg) r8, out(reg) r9, out(reg) r10, out(reg) r11,
        );
        asm!(
            "mov {}, r12", "mov {}, r13", "mov {}, r14", "mov {}, r15",
            out(reg) r12, out(reg) r13, out(reg) r14, out(reg) r15,
        );
        asm!("pushfq", "pop {}", out(reg) rflags);
        asm!("mov {:x}, cs", out(reg) cs);
        asm!("mov {:x}, ss", out(reg) ss);
    }

    panic_print!(
        "KERNEL PANIC\n{}\n\n\
        Register Dump:\n\
        RAX: 0x{:016x}  RBX: 0x{:016x}  RCX: 0x{:016x}  RDX: 0x{:016x}\n\
        RSI: 0x{:016x}  RDI: 0x{:016x}  RBP: 0x{:016x}  RSP: 0x{:016x}\n\
        R8:  0x{:016x}  R9:  0x{:016x}  R10: 0x{:016x}  R11: 0x{:016x}\n\
        R12: 0x{:016x}  R13: 0x{:016x}  R14: 0x{:016x}  R15: 0x{:016x}\n\
        RFLAGS: 0x{:016x}\n\
        CS:  0x{:04x}      SS:  0x{:04x}",
        info,
        rax, rbx, rcx, rdx,
        rsi, rdi, rbp, rsp,
        r8, r9, r10, r11,
        r12, r13, r14, r15,
        rflags, cs, ss
    );

    loop {
        unsafe {
            asm!("cli", "hlt");
        }
    }
}