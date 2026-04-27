#![no_std]
#![no_main]

extern crate alloc;

use alloc::boxed::Box;
use eucalypt_os::elf::{jump_to_usermode, load_elf, alloc_user_stack};
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

use memory::hhdm::hhdm_init;
use memory::{
    allocator::init_allocator,
    mmio::{map_mmio, mmio_map_range},
    vmm::VMM,
};

use ahci::{init_ahci, ahci_read_drive, get_drive_count};
use ide::ide_init;
use pci::check_all_buses;
use process::proc::new_process;
use process::scheduler::enable_scheduler;
use process::thread::TCB;
use ramfs::mount_ramdisk;
use usb::init_usb;

use framebuffer::ScrollingTextRenderer;
use vfs::*;

use x86_64::registers::control::{Cr0, Cr0Flags, Cr4, Cr4Flags};

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
    
    unsafe {
        let mut cr0 = Cr0::read();
        cr0.remove(Cr0Flags::EMULATE_COPROCESSOR);
        cr0.insert(Cr0Flags::MONITOR_COPROCESSOR);
        Cr0::write(cr0);

        let mut cr4 = Cr4::read();
        cr4.insert(Cr4Flags::OSFXSR);
        cr4.insert(Cr4Flags::OSXMMEXCPT_ENABLE);
        Cr4::write(cr4);
    }

    println!("eucalyptOS Starting...");

    let memmap_response = MEMMAP_REQUEST.response().expect("No memory map available");
    hhdm_init();
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

    let (entry, pml4_phys) = load_elf("ram/USER").expect("Failed to load USER");
    let pml4_ptr = pml4_phys as *mut memory::paging::PageTable;
    let user_rsp = alloc_user_stack(pml4_ptr).expect("Failed to allocate user stack");

    let init_pid = new_process(None).expect("Failed to create user process");

    process::proc::with_process_mut(init_pid, |pcb| {
        pcb.cr3 = pml4_phys;
    });

    let init_tid = TCB::create_thread(0, entry, init_pid, pml4_phys)
        .expect("Failed to create init thread");

    {
        let tcb_ptr = process::thread::get_tcb_by_tid(init_tid)
            .expect("init TCB missing");
        process::scheduler::set_current_thread(tcb_ptr);
    }

    // idle process as before
    let idle_pid = new_process(None).expect("Failed to create idle process");
    let idle_cr3 = VMM::get_page_table() as u64;
    TCB::create_thread(0x4000, idle as *const () as u64, idle_pid, idle_cr3)
        .expect("Failed to create idle thread");

    enable_scheduler();

    unsafe {
        core::arch::asm!("mov cr3, {}", in(reg) pml4_phys);
        jump_to_usermode(entry, user_rsp)
    }
}

fn idle() -> ! {
    loop {
        unsafe {
            core::arch::asm!("hlt");
        }
    }
}

#[cfg(not(test))]
#[panic_handler]
fn rust_panic(info: &core::panic::PanicInfo) -> ! {
    use process::scheduler::disable_scheduler;
    disable_scheduler();

    use core::arch::asm;
    use framebuffer::{color, fill_screen, kprintln};

    fill_screen(color::DARK_BLUE);
    framebuffer::RENDERER.with(|r| r.set_colors(color::WHITE, color::DARK_BLUE));

    let file = info.location().map(|l| l.file()).unwrap_or("unknown");
    let line = info.location().map(|l| l.line()).unwrap_or(0);

    kprintln!("panic: {}", info.message());
    kprintln!("at {}:{}", file, line);

    loop {
        unsafe {
            asm!("cli", "hlt");
        }
    }
}