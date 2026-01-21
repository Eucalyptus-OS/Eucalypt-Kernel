use core::arch::asm;
use core::ptr::addr_of_mut;
use core::sync::atomic::{AtomicU64, Ordering};
use ide::ide_irq_handler;
use pic8259::ChainedPics;
use spin::Mutex;
use syscall::syscall_handler::syscall_handler;
use x86_64::registers::control::Cr2;
use x86_64::structures::idt::{InterruptDescriptorTable, InterruptStackFrame, PageFaultErrorCode};
use bare_x86_64::cpu::apic;

const PIC_1_OFFSET: u8 = 32;
const PIC_2_OFFSET: u8 = PIC_1_OFFSET + 8;
const APIC_TIMER_VECTOR: u8 = 32;

static mut IDT: InterruptDescriptorTable = InterruptDescriptorTable::new();
static TIMER_TICKS: AtomicU64 = AtomicU64::new(0);
static PICS: Mutex<ChainedPics> = Mutex::new(unsafe { ChainedPics::new(PIC_1_OFFSET, PIC_2_OFFSET) });

pub fn idt_init() {
    let idt = unsafe { &mut *addr_of_mut!(IDT) };

    register_exception_handlers(idt);
    configure_pics();
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
    masks[1] &= !(1 << 6 | 1 << 7);
    unsafe { pics.write_masks(masks[0], masks[1]) };
}

fn register_interrupt_handlers(idt: &mut InterruptDescriptorTable) {
    set_raw_idt_entry(idt, APIC_TIMER_VECTOR, apic_timer_handler as *const () as u64);
    idt[PIC_2_OFFSET + 6].set_handler_fn(ide_primary_handler);
    idt[PIC_2_OFFSET + 7].set_handler_fn(ide_secondary_handler);
    idt[128].set_handler_fn(isr128_handler);
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
extern "C" fn apic_timer_handler() {
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
        "mov rdi, rsp",
        "call {handler}",
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
        handler = sym apic_timer_interrupt_handler,
    );
}

#[unsafe(no_mangle)]
extern "C" fn apic_timer_interrupt_handler(rsp: u64) -> u64 {
    TIMER_TICKS.fetch_add(1, Ordering::Relaxed);
    apic::apic_eoi();
    sched::handle_timer_interrupt(rsp)
}

pub fn timer_wait_ms(ms: u64) {
    let start = TIMER_TICKS.load(Ordering::Relaxed);
    let target = start + (ms * 10000) / 1000;

    while TIMER_TICKS.load(Ordering::Relaxed) < target {
        core::hint::spin_loop();
    }
}

pub fn timer_wait_us(us: u64) {
    let start = TIMER_TICKS.load(Ordering::Relaxed);
    let target = start + (us * 10000) / 1_000_000;

    if target > start {
        while TIMER_TICKS.load(Ordering::Relaxed) < target {
            core::hint::spin_loop();
        }
    } else {
        for _ in 0..(us / 10) {
            unsafe { asm!("pause") };
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

extern "x86-interrupt" fn page_fault_handler(
    stack_frame: InterruptStackFrame,
    error_code: PageFaultErrorCode,
) {
    panic!(
        "EXCEPTION: PAGE FAULT\nAccessed Address: {:?}\nError Code: {:?}\n{:#?}",
        Cr2::read(),
        error_code,
        stack_frame
    );
}

extern "x86-interrupt" fn ide_primary_handler(_stack_frame: InterruptStackFrame) {
    ide_irq_handler();
    unsafe {
        PICS.lock().notify_end_of_interrupt(PIC_2_OFFSET + 6);
    }
}

extern "x86-interrupt" fn ide_secondary_handler(_stack_frame: InterruptStackFrame) {
    ide_irq_handler();
    unsafe {
        PICS.lock().notify_end_of_interrupt(PIC_2_OFFSET + 7);
    }
}

extern "x86-interrupt" fn isr128_handler(_stack_frame: InterruptStackFrame) {
    unsafe {
        asm!(
            "mov rcx, r10",
            "call {handler}",
            "iretq",
            handler = sym syscall_handler,
            options(noreturn)
        );
    }
}