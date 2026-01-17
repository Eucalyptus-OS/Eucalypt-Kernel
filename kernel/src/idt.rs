//! The IDT(Interrupt Descriptor Table) is a data structure used by the CPU for interrupts handling

use core::ptr::addr_of_mut;
use core::sync::atomic::{AtomicU64, Ordering};
use framebuffer::{print, println};
use spin::Mutex;
use pic8259::ChainedPics;
use syscall::syscall_handler::syscall_handler;
use x86_64::structures::idt::{InterruptDescriptorTable, InterruptStackFrame, PageFaultErrorCode};
use x86_64::registers::control::Cr2;
use x86_64::instructions::port::Port;
use ide::ide_irq_handler;

static mut IDT: InterruptDescriptorTable = InterruptDescriptorTable::new();
static TIMER_TICKS: AtomicU64 = AtomicU64::new(0);

pub const PIC_1_OFFSET: u8 = 32;
pub const PIC_2_OFFSET: u8 = PIC_1_OFFSET + 8;
pub static PICS: Mutex<ChainedPics> =
    spin::Mutex::new(unsafe { ChainedPics::new(PIC_1_OFFSET, PIC_2_OFFSET) });

const PIT_FREQUENCY: u32 = 1193182;
const TARGET_FREQUENCY: u32 = 10000;
const PIT_CMD_PORT: u16 = 0x43;
const PIT_DATA_PORT: u16 = 0x40;

pub unsafe fn idt_init() {
    let idt = unsafe { &mut *addr_of_mut!(IDT) };
    
    register_exception_handlers(idt);
    configure_pics();
    init_pit(TARGET_FREQUENCY);
    register_interrupt_handlers(idt);
    
    idt.load();
}

fn register_exception_handlers(idt: &mut InterruptDescriptorTable) {
    idt.divide_error.set_handler_fn(divide_error_handler);
    idt.debug.set_handler_fn(debug_handler);
    idt.non_maskable_interrupt.set_handler_fn(nmi_handler);
    idt.breakpoint.set_handler_fn(breakpoint_handler);
    idt.overflow.set_handler_fn(overflow_handler);
    idt.bound_range_exceeded.set_handler_fn(bound_range_handler);
    idt.invalid_opcode.set_handler_fn(invalid_opcode_handler);
    idt.device_not_available.set_handler_fn(device_not_available_handler);
    idt.double_fault.set_handler_fn(double_fault_handler);
    idt.invalid_tss.set_handler_fn(invalid_tss_handler);
    idt.segment_not_present.set_handler_fn(segment_not_present_handler);
    idt.stack_segment_fault.set_handler_fn(stack_segment_fault_handler);
    idt.general_protection_fault.set_handler_fn(general_protection_fault_handler);
    idt.page_fault.set_handler_fn(page_fault_handler);
    idt.x87_floating_point.set_handler_fn(x87_floating_point_handler);
    idt.alignment_check.set_handler_fn(alignment_check_handler);
    idt.machine_check.set_handler_fn(machine_check_handler);
    idt.simd_floating_point.set_handler_fn(simd_floating_point_handler);
    idt.virtualization.set_handler_fn(virtualization_handler);
    idt.security_exception.set_handler_fn(security_exception_handler);
}

fn configure_pics() {
    let mut pics = PICS.lock();
    unsafe { pics.initialize() };

    let mut masks = unsafe { pics.read_masks() };
    masks[0] &= !(1 << 0);
    masks[1] &= !(1 << 6 | 1 << 7);
    unsafe { pics.write_masks(masks[0], masks[1]) };
}

fn register_interrupt_handlers(idt: &mut InterruptDescriptorTable) {
    set_raw_idt_entry(idt, PIC_1_OFFSET, timer_handler as *const () as u64);
    idt[PIC_2_OFFSET + 6].set_handler_fn(ide_primary_handler);
    idt[PIC_2_OFFSET + 7].set_handler_fn(ide_secondary_handler);
    idt[128].set_handler_fn(isr128_handler);
}

fn init_pit(frequency: u32) {
    let divisor = PIT_FREQUENCY / frequency;
    let mut cmd_port: Port<u8> = Port::new(PIT_CMD_PORT);
    let mut data_port: Port<u8> = Port::new(PIT_DATA_PORT);
    
    unsafe {
        cmd_port.write(0x36);
        data_port.write((divisor & 0xFF) as u8);
        data_port.write((divisor >> 8) as u8);
    }
}

fn set_raw_idt_entry(idt: &mut InterruptDescriptorTable, index: u8, handler_addr: u64) {
    let idt_ptr = idt as *mut InterruptDescriptorTable as *mut u64;
    let entry_ptr = unsafe { idt_ptr.add(index as usize * 2) };
    
    let low = (handler_addr & 0xFFFF) 
        | (0x08 << 16)
        | (0x8E00 << 32)
        | ((handler_addr & 0xFFFF_0000) << 32);
    let high = handler_addr >> 32;
    
    unsafe {
        *entry_ptr = low;
        *entry_ptr.add(1) = high;
    }
}

#[unsafe(naked)]
unsafe extern "C" fn timer_handler() {
    core::arch::naked_asm!(
        "push rax",
        "push rbx",
        "push rcx",
        "push rdx",
        "push rsi",
        "push rdi",
        "push rbp",
        "push r8",
        "push r9",
        "push r10",
        "push r11",
        "push r12",
        "push r13",
        "push r14",
        "push r15",
        
        "mov al, 0x20",
        "out 0x20, al",
        
        "mov rdi, rsp",
        "call timer_interrupt_handler",
        "mov rsp, rax",
        
        "pop r15",
        "pop r14",
        "pop r13",
        "pop r12",
        "pop r11",
        "pop r10",
        "pop r9",
        "pop r8",
        "pop rbp",
        "pop rdi",
        "pop rsi",
        "pop rdx",
        "pop rcx",
        "pop rbx",
        "pop rax",
        "iretq",
    );
}

#[unsafe(no_mangle)]
unsafe extern "C" fn timer_interrupt_handler(rsp: u64) -> u64 {
    use sched::handle_timer_interrupt;
    
    TIMER_TICKS.fetch_add(1, Ordering::Relaxed);
    
    let ticks = TIMER_TICKS.load(Ordering::Relaxed);
    
    handle_timer_interrupt(rsp)
}

pub fn timer_wait_ms(ms: u64) {
    let start = TIMER_TICKS.load(Ordering::Relaxed);
    let target = start + (ms * TARGET_FREQUENCY as u64) / 1000;
    
    while TIMER_TICKS.load(Ordering::Relaxed) < target {
        core::hint::spin_loop();
    }
}

pub fn timer_wait_us(us: u64) {
    let start = TIMER_TICKS.load(Ordering::Relaxed);
    let target = start + (us * TARGET_FREQUENCY as u64) / 1_000_000;
    
    if target > start {
        while TIMER_TICKS.load(Ordering::Relaxed) < target {
            core::hint::spin_loop();
        }
    } else {
        for _ in 0..(us / 10) {
            unsafe { core::arch::asm!("pause"); }
        }
    }
}

pub fn get_timer_ticks() -> u64 {
    TIMER_TICKS.load(Ordering::Relaxed)
}

macro_rules! exception_handler {
    ($name:ident, $msg:expr) => {
        extern "x86-interrupt" fn $name(stack_frame: InterruptStackFrame) {
            panic!("EXCEPTION: {}\n{:#?}", $msg, stack_frame);
        }
    };
    ($name:ident, $msg:expr, error) => {
        extern "x86-interrupt" fn $name(stack_frame: InterruptStackFrame, error_code: u64) {
            panic!("EXCEPTION: {}\nError Code: {}\n{:#?}", $msg, error_code, stack_frame);
        }
    };
    ($name:ident, $msg:expr, diverging) => {
        extern "x86-interrupt" fn $name(stack_frame: InterruptStackFrame, error_code: u64) -> ! {
            panic!("EXCEPTION: {}\nError Code: {}\n{:#?}", $msg, error_code, stack_frame);
        }
    };
}

exception_handler!(divide_error_handler, "DIVIDE ERROR");
exception_handler!(debug_handler, "DEBUG");
exception_handler!(nmi_handler, "NON-MASKABLE INTERRUPT");
exception_handler!(breakpoint_handler, "BREAKPOINT");
exception_handler!(overflow_handler, "OVERFLOW");
exception_handler!(bound_range_handler, "BOUND RANGE EXCEEDED");
exception_handler!(invalid_opcode_handler, "INVALID OPCODE");
exception_handler!(device_not_available_handler, "DEVICE NOT AVAILABLE");
exception_handler!(double_fault_handler, "DOUBLE FAULT", diverging);
exception_handler!(invalid_tss_handler, "INVALID TSS", error);
exception_handler!(segment_not_present_handler, "SEGMENT NOT PRESENT", error);
exception_handler!(stack_segment_fault_handler, "STACK SEGMENT FAULT", error);
exception_handler!(general_protection_fault_handler, "GENERAL PROTECTION FAULT", error);
exception_handler!(x87_floating_point_handler, "x87 FLOATING POINT");
exception_handler!(alignment_check_handler, "ALIGNMENT CHECK", error);
exception_handler!(simd_floating_point_handler, "SIMD FLOATING POINT");
exception_handler!(virtualization_handler, "VIRTUALIZATION");
exception_handler!(security_exception_handler, "SECURITY EXCEPTION", error);

extern "x86-interrupt" fn machine_check_handler(stack_frame: InterruptStackFrame) -> ! {
    panic!("EXCEPTION: MACHINE CHECK\n{:#?}", stack_frame);
}

extern "x86-interrupt" fn page_fault_handler(stack_frame: InterruptStackFrame, error_code: PageFaultErrorCode) {
    panic!(
        "EXCEPTION: PAGE FAULT\nAccessed Address: {:?}\nError Code: {:?}\n{:#?}",
        Cr2::read(),
        error_code,
        stack_frame
    );
}

extern "x86-interrupt" fn ide_primary_handler(_stack_frame: InterruptStackFrame) {
    ide_irq_handler();
    unsafe { PICS.lock().notify_end_of_interrupt(PIC_2_OFFSET + 6); }
}

extern "x86-interrupt" fn ide_secondary_handler(_stack_frame: InterruptStackFrame) {
    ide_irq_handler();
    unsafe { PICS.lock().notify_end_of_interrupt(PIC_2_OFFSET + 7); }
}

extern "x86-interrupt" fn isr128_handler(_stack_frame: InterruptStackFrame) {
    unsafe {
        core::arch::asm!(
            "mov rcx, r10",
            "call {handler}",
            "iretq",
            handler = sym syscall_handler,
            options(noreturn)
        );
    }
}