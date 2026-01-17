#![no_std]
#![no_main]

extern crate alloc;

use core::arch::asm;
use eucalypt_os::idt::timer_wait_ms;
use limine::BaseRevision;
use limine::request::{FramebufferRequest, MemoryMapRequest, RequestsEndMarker, RequestsStartMarker};
use framebuffer::{ScrollingTextRenderer, println, panic_print};
use ide::ide_init;
use eucalypt_fs::{SuperBlock, write_eucalypt_fs};
use eucalypt_fs::file_ops::{create_file, read_file, delete_file};
use eucalypt_fs::directory::DirectoryManager;
use eucalypt_fs::inodes::InodeManager;
use ahci::init_ahci;
use pci::check_all_buses;
use eucalypt_os::{gdt, idt, init_allocator, VMM};
use memory::mmio::mmio_map_range;
use process::create_process;
use usb;

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
#[unsafe(link_section = ".requests_start_marker")]
static _START_MARKER: RequestsStartMarker = RequestsStartMarker::new();

#[used]
#[unsafe(link_section = ".requests_end_marker")]
static _END_MARKER: RequestsEndMarker = RequestsEndMarker::new();

#[unsafe(no_mangle)]
unsafe extern "C" fn kmain() -> ! {
    unsafe {
        assert!(BASE_REVISION.is_supported());

        let framebuffer_response = FRAMEBUFFER_REQUEST.get_response().expect("No framebuffer");
        let framebuffer = framebuffer_response.framebuffers().next().expect("No framebuffer available");

        ScrollingTextRenderer::init(
            framebuffer.addr(),
            framebuffer.width() as usize,
            framebuffer.height() as usize,
            framebuffer.pitch() as usize,
            framebuffer.bpp() as usize,
            FONT,
        );

        println!("eucalyptOS Starting...");
        println!("Initializing Memory Management...");

        if let Some(memmap_response) = MEMMAP_REQUEST.get_response() {
            VMM::init(memmap_response);
            println!("VMM Initialized");

            init_allocator(memmap_response);
            println!("Heap Allocator Initialized");
        } else {
            panic!("No memory map available!");
        }

        println!("Initializing GDT...");
        gdt::gdt_init();

        println!("Initializing IDT...");
        idt::idt_init();
        println!("IDT Initialized");

        asm!("sti");
        println!("Interrupts enabled");

        println!("Setting up MMIO region...");
        mmio_map_range(0xFFFF800000000000, 0xFFFF8000FFFFFFFF);
        println!("MMIO range configured");

        println!("Initializing IDE");
        ide_init(0, 0, 0, 0, 0);
        println!("IDE Initialized");

        println!("Initializing PCI");
        check_all_buses();
        println!("PCI scan complete");

        println!("Writing filesystem...");
        write_eucalypt_fs(0);
        
        println!("Initializing USB...");
        usb::init_usb();
        
        println!("Initializing AHCI");
        init_ahci();
        
        println!("\nStarting multitasking...");
        
        let kernel_main_rsp: u64;
        asm!("mov {}, rsp", out(reg) kernel_main_rsp);
        
        process::init_kernel_process(kernel_main_rsp);
        
        create_process(test1 as *mut ()).expect("Failed to create process 1");
        create_process(test2 as *mut ()).expect("Failed to create process 2");
        
        sched::init_scheduler();
        sched::enable_scheduler();
        
        println!("Scheduler enabled - preemptive multitasking active\n");
        
        loop {
            hcf();
        }
    }
}

#[panic_handler]
fn rust_panic(info: &core::panic::PanicInfo) -> ! {
    let (rax, rbx, rcx, rdx): (u64, u64, u64, u64);
    let (rsi, rdi, rbp, rsp): (u64, u64, u64, u64);
    let (r8, r9, r10, r11): (u64, u64, u64, u64);
    let (r12, r13, r14, r15): (u64, u64, u64, u64);
    let (rflags, cs, ss): (u64, u16, u16);
    
    unsafe {
        asm!(
            "mov {}, rax",
            "mov {}, rbx",
            "mov {}, rcx",
            "mov {}, rdx",
            out(reg) rax,
            out(reg) rbx,
            out(reg) rcx,
            out(reg) rdx,
        );
        
        asm!(
            "mov {}, rsi",
            "mov {}, rdi",
            "mov {}, rbp",
            "mov {}, rsp",
            out(reg) rsi,
            out(reg) rdi,
            out(reg) rbp,
            out(reg) rsp,
        );
        
        asm!(
            "mov {}, r8",
            "mov {}, r9",
            "mov {}, r10",
            "mov {}, r11",
            out(reg) r8,
            out(reg) r9,
            out(reg) r10,
            out(reg) r11,
        );
        
        asm!(
            "mov {}, r12",
            "mov {}, r13",
            "mov {}, r14",
            "mov {}, r15",
            out(reg) r12,
            out(reg) r13,
            out(reg) r14,
            out(reg) r15,
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
        rflags,
        cs, ss
    );
    
    hcf();
}

fn hcf() -> ! {
    loop {
        unsafe {
            asm!("hlt");
        }
    }
}

fn test1() {
    loop {
        println!("Process 1 running");
        timer_wait_ms(1000);
    }
}

fn test2() {
    loop {
        println!("Process 2 running");
        timer_wait_ms(1000);
    }
}