use core::{alloc::Layout, sync::atomic::{AtomicU64, AtomicUsize, Ordering}};
use alloc::alloc::alloc_zeroed;
use framebuffer::println;
use crate::scheduler;
use memory::vmm::VMM;

pub type ThreadId = u64;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ThreadState {
    Ready,
    Running,
    Blocked,
    Sleeping,
    Zombie,
    Dead,
}

#[derive(Debug, Default, Clone)]
#[repr(C)]
pub struct CpuContext {
    pub rbx: u64,
    pub rbp: u64,
    pub r12: u64,
    pub r13: u64,
    pub r14: u64,
    pub r15: u64,
    pub rip: u64,
    pub rsp: u64,
    pub rflags: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct Priority(pub u8);

impl Priority {
    pub const IDLE: Self     = Self(0);
    pub const LOW: Self      = Self(64);
    pub const NORMAL: Self   = Self(128);
    pub const HIGH: Self     = Self(192);
    pub const REALTIME: Self = Self(255);
}

#[derive(Debug)]
pub struct TCB {
    pub tid: ThreadId,
    pub stack_size: u64,
    pub stack_base: *mut u8,
    pub stack_top: *mut u8,
    pub cpu_context: CpuContext,
    pub next: *mut TCB,
    pub cr3: u64,
    pub state: ThreadState,
    pub priority: Priority,
    pub is_userspace: bool,
    pub kernel_stack_top: u64,
}

const MAX_THREADS: usize = 16;
static mut THREAD_STORAGE: [core::mem::MaybeUninit<TCB>; MAX_THREADS] =
    [const { core::mem::MaybeUninit::uninit() }; MAX_THREADS];
static THREAD_COUNT: AtomicUsize = AtomicUsize::new(0);
static NEXT_THREAD_ID: AtomicU64 = AtomicU64::new(1);

pub fn get_thread_count() -> usize {
    THREAD_COUNT.load(Ordering::Acquire)
}

pub fn get_thread(index: usize) -> *mut TCB {
    unsafe { THREAD_STORAGE[index].as_mut_ptr() }
}

#[unsafe(naked)]
extern "C" fn thread_entry_wrapper() {
    core::arch::naked_asm!(
        "sti",
        "call rbx",
        "ud2",
    );
}

#[unsafe(naked)]
#[unsafe(no_mangle)]
unsafe extern "C" fn iretq_trampoline() {
    core::arch::naked_asm!(
        "iretq",
    );
}

fn setup_stack(stack_base: *mut u8, stack_size: u64, entry: u64) -> u64 {
    unsafe {
        let stack_top = stack_base.add(stack_size as usize) as *mut u64;
        let mut rsp = stack_top;

        // iretq in 64-bit mode always pops all 5 items: RIP, CS, RFLAGS, RSP, SS.
        // Build the frame in reverse push order (SS first = highest address).
        rsp = rsp.sub(1); *rsp = 0x10;                                     // SS: kernel data segment
        rsp = rsp.sub(1); *rsp = stack_top as u64;                         // RSP: thread's initial stack top
        rsp = rsp.sub(1); *rsp = 0x202;                                    // RFLAGS: IF set, bit 1 always set
        rsp = rsp.sub(1); *rsp = 0x08;                                     // CS: kernel code segment
        rsp = rsp.sub(1); *rsp = thread_entry_wrapper as *const () as u64; // RIP

        // 15 saved registers matching the push order in apic_timer_handler:
        // push rax, rbx, ..., r15  (rax first = highest addr, r15 last = lowest)
        // pop order: r15, r14, ..., rbx, rax  (i=14 is r15, i=1 is rbx, i=0 is rax)
        // rbx (i=1) holds the thread entry point for thread_entry_wrapper's `call rbx`.
        for i in 0..15usize {
            rsp = rsp.sub(1);
            *rsp = if i == 1 { entry } else { 0 };
        }

        rsp as u64
    }
}

impl TCB {
    pub fn new(stack_size: u64, entry: u64) -> *mut TCB {
        let layout = Layout::from_size_align(stack_size as usize, 4096).unwrap();
        let stack_base = unsafe { alloc_zeroed(layout) };
        assert!(!stack_base.is_null());

        let rsp = setup_stack(stack_base, stack_size, entry);
        let kernel_cr3 = VMM::get_page_table() as u64;
        let index = THREAD_COUNT.fetch_add(1, Ordering::AcqRel);
        assert!(index < MAX_THREADS);

        let stack_top = unsafe { stack_base.add(stack_size as usize) };

        let tcb = TCB {
            tid: NEXT_THREAD_ID.fetch_add(1, Ordering::Relaxed),
            stack_size,
            stack_base,
            stack_top,
            cpu_context: CpuContext {
                rsp,
                ..CpuContext::default()
            },
            next: core::ptr::null_mut(),
            cr3: kernel_cr3,
            state: ThreadState::Ready,
            priority: Priority::NORMAL,
            is_userspace: false,
            kernel_stack_top: stack_top as u64,
        };

        unsafe {
            THREAD_STORAGE[index].write(tcb);
            THREAD_STORAGE[index].as_mut_ptr()
        }
    }

    pub fn from_current_stack(tid: u64, cr3: u64, rsp: u64) -> *mut TCB {
        let index = THREAD_COUNT.fetch_add(1, Ordering::AcqRel);
        assert!(index < MAX_THREADS);

        let tcb = TCB {
            tid,
            stack_size: 0,
            stack_base: core::ptr::null_mut(),
            stack_top: core::ptr::null_mut(),
            cpu_context: CpuContext {
                rsp,
                ..CpuContext::default()
            },
            next: core::ptr::null_mut(),
            cr3,
            state: ThreadState::Running,
            priority: Priority::HIGH,
            is_userspace: false,
            kernel_stack_top: 0,
        };

        unsafe {
            THREAD_STORAGE[index].write(tcb);
            THREAD_STORAGE[index].as_mut_ptr()
        }
    }
}

pub fn init_kernel_thread() {
    let kernel_pml4 = VMM::get_page_table() as u64;
    let current_rsp: u64;

    unsafe {
        core::arch::asm!("mov {}, rsp", out(reg) current_rsp);
    }

    let tcb = TCB::from_current_stack(0, kernel_pml4, current_rsp);
    scheduler::set_current_thread(tcb);
    scheduler::set_current_index(0);
    println!("Kernel process initialized at RSP: {:#x}", current_rsp);
}