use core::ptr::addr_of_mut;
use core::sync::atomic::{AtomicU64, Ordering};
use bare_x86_64::cpu::apic;
use ide::ide_irq_handler;
use pic8259::ChainedPics;
use spin::Mutex;
use syscall::syscall_handler::syscall_handler;
use x86_64::registers::control::Cr2;
use x86_64::structures::idt::{InterruptDescriptorTable, InterruptStackFrame, PageFaultErrorCode};
use x86_64::registers::model_specific::Msr;

const PIC_1_OFFSET: u8 = 32;
const PIC_2_OFFSET: u8 = PIC_1_OFFSET + 8;
const APIC_TIMER_VECTOR: u8 = 32;

const IA32_STAR: u32 = 0xC000_0081;
const IA32_LSTAR: u32 = 0xC000_0082;
const IA32_FMASK: u32 = 0xC000_0084;

static mut IDT: InterruptDescriptorTable = InterruptDescriptorTable::new();
static TIMER_TICKS: AtomicU64 = AtomicU64::new(0);
static PICS: Mutex<ChainedPics> = Mutex::new(unsafe { ChainedPics::new(PIC_1_OFFSET, PIC_2_OFFSET) });

macro_rules! register_exceptions {
    ($idt:expr, $(
        $field:ident : $name:ident, $msg:literal $(, $kind:ident)?;
    )*) => {
        $(
            register_exceptions!(@handler $name, $msg $(, $kind)?);
            $idt.$field.set_handler_fn($name);
        )*
    };

    (@handler $name:ident, $msg:literal) => {
        extern "x86-interrupt" fn $name(sf: InterruptStackFrame) {
            panic!("EXCEPTION: {}\n{:#?}", $msg, sf);
        }
    };
    (@handler $name:ident, $msg:literal, error) => {
        extern "x86-interrupt" fn $name(sf: InterruptStackFrame, ec: u64) {
            panic!("EXCEPTION: {}\nError Code: {}\n{:#?}", $msg, ec, sf);
        }
    };
    (@handler $name:ident, $msg:literal, diverging) => {
        extern "x86-interrupt" fn $name(sf: InterruptStackFrame, ec: u64) -> ! {
            panic!("EXCEPTION: {}\nError Code: {}\n{:#?}", $msg, ec, sf);
        }
    };
    (@handler $name:ident, $msg:literal, diverging_no_error) => {
        extern "x86-interrupt" fn $name(sf: InterruptStackFrame) -> ! {
            panic!("EXCEPTION: {}\n{:#?}", $msg, sf);
        }
    };
}

pub fn idt_init() {
    let idt = unsafe { &mut *addr_of_mut!(IDT) };

    register_exceptions!(idt,
        divide_error             : divide_error_handler,            "DIVIDE ERROR";
        debug                    : debug_handler,                   "DEBUG";
        non_maskable_interrupt   : nmi_handler,                     "NON-MASKABLE INTERRUPT";
        breakpoint               : breakpoint_handler,              "BREAKPOINT";
        overflow                 : overflow_handler,                "OVERFLOW";
        bound_range_exceeded     : bound_range_handler,             "BOUND RANGE EXCEEDED";
        invalid_opcode           : invalid_opcode_handler,          "INVALID OPCODE";
        device_not_available     : device_not_available_handler,    "DEVICE NOT AVAILABLE";
        double_fault             : double_fault_handler,            "DOUBLE FAULT",             diverging;
        invalid_tss              : invalid_tss_handler,             "INVALID TSS",              error;
        segment_not_present      : segment_not_present_handler,     "SEGMENT NOT PRESENT",      error;
        stack_segment_fault      : stack_segment_fault_handler,     "STACK SEGMENT FAULT",      error;
        general_protection_fault : gpf_handler,                     "GENERAL PROTECTION FAULT", error;
        x87_floating_point       : x87_handler,                     "x87 FLOATING POINT";
        alignment_check          : alignment_check_handler,         "ALIGNMENT CHECK",          error;
        machine_check            : machine_check_handler,           "MACHINE CHECK",            diverging_no_error;
        simd_floating_point      : simd_handler,                    "SIMD FLOATING POINT";
        virtualization           : virtualization_handler,          "VIRTUALIZATION";
        security_exception       : security_exception_handler,      "SECURITY EXCEPTION",       error;
    );

    idt.page_fault.set_handler_fn(page_fault_handler);

    {
        let mut pics = PICS.lock();
        unsafe { pics.initialize() };
        let mut masks = unsafe { pics.read_masks() };
        masks[1] &= !(1 << 6 | 1 << 7);
        unsafe { pics.write_masks(masks[0], masks[1]) };
    }

    set_raw_idt_entry(idt, APIC_TIMER_VECTOR, apic_timer_handler as *const () as u64);

    idt[PIC_2_OFFSET + 6].set_handler_fn(ide_primary_handler);
    idt[PIC_2_OFFSET + 7].set_handler_fn(ide_secondary_handler);

    init_syscall();

    idt.load();
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

fn init_syscall() {
    unsafe {
        let mut star = Msr::new(IA32_STAR);
        let mut lstar = Msr::new(IA32_LSTAR);
        let mut fmask = Msr::new(IA32_FMASK);

        let kernel_cs: u64 = 0x08;
        let user_cs: u64 = 0x1b;

        let star_val = (kernel_cs << 32) | (user_cs << 48);
        star.write(star_val);
        lstar.write(syscall_entry as *const () as u64);
        fmask.write(1 << 9);
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

pub fn get_timer_ticks() -> u64 {
    TIMER_TICKS.load(Ordering::Relaxed)
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

#[unsafe(naked)]
extern "C" fn syscall_entry() {
    core::arch::naked_asm!(
        "swapgs",
        "push r11",
        "push rcx",
        "push rbx",
        "push rbp",
        "push r12",
        "push r13",
        "push r14",
        "push r15",
        "mov rdi, rax",
        "mov rsi, rdi",
        "mov rdx, rsi",
        "mov rcx, rdx",
        "call {handler}",
        "pop r15",
        "pop r14",
        "pop r13",
        "pop r12",
        "pop rbp",
        "pop rbx",
        "pop rcx",
        "pop r11",
        "swapgs",
        "sysretq",
        handler = sym syscall_handler
    );
}
