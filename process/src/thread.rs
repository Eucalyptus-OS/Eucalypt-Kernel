use crate::scheduler;
use alloc::{alloc::alloc_zeroed, vec::Vec};
use core::{
    alloc::Layout,
    sync::atomic::{AtomicU64, AtomicUsize, Ordering},
};
use framebuffer::println;
use memory::vmm::VMM;
use spin::Mutex;
use vfs::{D_STDERR, D_STDIN, D_STDOUT, FD};

pub type ThreadId = u64;
pub type ProcessId = u64;

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
    pub tid:              ThreadId,
    pub pid:              ProcessId,
    pub stack_size:       u64,
    pub stack_base:       *mut u8,
    pub stack_top:        *mut u8,
    pub cpu_context:      CpuContext,
    pub next:             *mut TCB,
    pub cr3:              u64,
    pub state:            ThreadState,
    pub priority:         Priority,
    pub is_userspace:     bool,
    pub kernel_stack_top: u64,
    pub fd_table:         Vec<FD>,
}

// SAFETY: TCB contains raw pointers managed exclusively through THREAD_STORAGE's
// Mutex, guaranteeing no concurrent unsynchronised access.
unsafe impl Send for TCB {}
unsafe impl Sync for TCB {}

const MAX_THREADS: usize = 1000;

/// Global thread table; all access is serialised through this mutex.
static THREAD_STORAGE: Mutex<[core::mem::MaybeUninit<TCB>; MAX_THREADS]> =
    Mutex::new([const { core::mem::MaybeUninit::uninit() }; MAX_THREADS]);
static THREAD_COUNT: AtomicUsize = AtomicUsize::new(0);
static NEXT_THREAD_ID: AtomicU64 = AtomicU64::new(1);
static NEXT_PROCESS_ID: AtomicU64 = AtomicU64::new(1);

/// Returns the number of threads that have been allocated so far.
pub fn get_thread_count() -> usize {
    THREAD_COUNT.load(Ordering::Acquire)
}

/// Returns a raw pointer to the TCB at `index`; the caller must ensure the slot is initialised.
pub fn get_thread(index: usize) -> *mut TCB {
    THREAD_STORAGE.lock()[index].as_mut_ptr()
}

/// Naked entry stub: enables interrupts then calls the function pointer in RBX.
#[unsafe(naked)]
extern "C" fn thread_entry_wrapper() {
    core::arch::naked_asm!("sti", "call rbx", "ud2");
}

/// Naked trampoline that executes a single `iretq`, used to enter a new thread context.
#[unsafe(naked)]
#[unsafe(no_mangle)]
unsafe extern "C" fn iretq_trampoline() {
    core::arch::naked_asm!("iretq");
}

/// Builds an initial kernel stack frame for `entry` and returns the resulting RSP value.
fn setup_stack(stack_base: *mut u8, stack_size: u64, entry: u64) -> u64 {
    unsafe {
        let stack_top = stack_base.add(stack_size as usize) as *mut u64;
        let mut rsp = stack_top;

        // iretq in 64-bit mode always pops all 5 items: RIP, CS, RFLAGS, RSP, SS.
        // Build the frame in reverse push order (SS first = highest address).
        rsp = rsp.sub(1);
        *rsp = 0x10; // SS: kernel data segment
        rsp = rsp.sub(1);
        *rsp = stack_top as u64; // RSP: thread's initial stack top
        rsp = rsp.sub(1);
        *rsp = 0x202; // RFLAGS: IF set, bit 1 always set
        rsp = rsp.sub(1);
        *rsp = 0x08; // CS: kernel code segment
        rsp = rsp.sub(1);
        *rsp = thread_entry_wrapper as *const () as u64; // RIP

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
    /// Allocates a new kernel thread with a fresh zeroed stack, assigning it a unique PID and TID.
    pub fn new(stack_size: u64, entry: u64) -> *mut TCB {
        let layout = Layout::from_size_align(stack_size as usize, 4096).unwrap();
        let stack_base = unsafe { alloc_zeroed(layout) };
        assert!(!stack_base.is_null(), "TCB::new: stack allocation failed");

        let rsp       = setup_stack(stack_base, stack_size, entry);
        let cr3       = VMM::get_page_table() as u64;
        let stack_top = unsafe { stack_base.add(stack_size as usize) };

        let mut fd_table = Vec::new();
        fd_table.push(FD::new(0, D_STDIN));
        fd_table.push(FD::new(1, D_STDOUT));
        fd_table.push(FD::new(2, D_STDERR));

        let tcb = TCB {
            tid:              NEXT_THREAD_ID.fetch_add(1, Ordering::Relaxed),
            pid:              NEXT_PROCESS_ID.fetch_add(1, Ordering::Relaxed),
            stack_size,
            stack_base,
            stack_top,
            cpu_context: CpuContext { rsp, ..CpuContext::default() },
            next:             core::ptr::null_mut(),
            cr3,
            state:            ThreadState::Ready,
            priority:         Priority::NORMAL,
            is_userspace:     false,
            kernel_stack_top: stack_top as u64,
            fd_table,
        };

        // Publish the index only after the TCB is fully written.
        let mut storage = THREAD_STORAGE.lock();
        let index = THREAD_COUNT.fetch_add(1, Ordering::AcqRel);
        assert!(index < MAX_THREADS, "TCB::new: MAX_THREADS exceeded");
        storage[index].write(tcb);
        storage[index].as_mut_ptr()
    }

    /// Wraps the currently executing stack as a TCB without allocating, used to register the boot thread.
    pub fn from_current_stack(tid: u64, pid: u64, cr3: u64, rsp: u64) -> *mut TCB {
        let mut fd_table = Vec::new();
        fd_table.push(FD::new(0, D_STDIN));
        fd_table.push(FD::new(1, D_STDOUT));
        fd_table.push(FD::new(2, D_STDERR));

        let tcb = TCB {
            tid,
            pid,
            stack_size:       0,
            stack_base:       core::ptr::null_mut(),
            stack_top:        core::ptr::null_mut(),
            cpu_context:      CpuContext { rsp, ..CpuContext::default() },
            next:             core::ptr::null_mut(),
            cr3,
            state:            ThreadState::Running,
            priority:         Priority::HIGH,
            is_userspace:     false,
            kernel_stack_top: 0,
            fd_table,
        };

        let mut storage = THREAD_STORAGE.lock();
        let index = THREAD_COUNT.fetch_add(1, Ordering::AcqRel);
        assert!(index < MAX_THREADS, "from_current_stack: MAX_THREADS exceeded");
        storage[index].write(tcb);
        storage[index].as_mut_ptr()
    }
}

/// Registers the currently running kernel stack as the boot thread (TID 0, PID 0) and installs it as the scheduler's current thread.
pub fn init_kernel_thread() {
    let cr3: u64 = VMM::get_page_table() as u64;
    let rsp: u64;

    unsafe {
        core::arch::asm!("mov {}, rsp", out(reg) rsp);
    }

    let tcb = TCB::from_current_stack(0, 0, cr3, rsp);
    scheduler::set_current_thread(tcb);
    scheduler::set_current_index(0);
    println!("Kernel thread initialized: TID=0 PID=0 RSP={:#x}", rsp);
}