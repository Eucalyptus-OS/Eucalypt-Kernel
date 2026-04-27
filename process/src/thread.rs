use crate::{proc, scheduler};
use alloc::{alloc::alloc_zeroed, vec::Vec};
use core::{
    alloc::Layout,
    sync::atomic::{AtomicU64, AtomicUsize, Ordering},
};
use framebuffer::println;
use memory::vmm::VMM;
use spin::Mutex;

pub type ThreadId  = u64;
pub type ProcessId = u64;

pub enum ThreadError {
    StackAllocationFailed,
    StorageFull,
}

impl ThreadError {
    pub fn as_str(&self) -> &'static str {
        match self {
            ThreadError::StackAllocationFailed => "create_thread: failed to allocate stack",
            ThreadError::StorageFull           => "create_thread: thread storage full",
        }
    }
}

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
    pub rbx:    u64,
    pub rbp:    u64,
    pub r12:    u64,
    pub r13:    u64,
    pub r14:    u64,
    pub r15:    u64,
    pub rip:    u64,
    pub rsp:    u64,
    pub rflags: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct Priority(pub u8);

impl Priority {
    pub const IDLE:     Self = Self(0);
    pub const LOW:      Self = Self(64);
    pub const NORMAL:   Self = Self(128);
    pub const HIGH:     Self = Self(192);
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
}

unsafe impl Send for TCB {}
unsafe impl Sync for TCB {}

static THREAD_STORAGE: Mutex<Vec<TCB>> = Mutex::new(Vec::new());
static THREAD_COUNT:   AtomicUsize     = AtomicUsize::new(0);
static NEXT_THREAD_ID: AtomicU64       = AtomicU64::new(1);

// returns the number of threads currently in storage
pub fn get_thread_count() -> usize {
    THREAD_COUNT.load(Ordering::Acquire)
}

// returns a raw pointer to the tcb at index; caller must ensure the slot is initialised
pub fn get_thread(index: usize) -> *mut TCB {
    THREAD_STORAGE.lock().as_mut_ptr().wrapping_add(index)
}

#[unsafe(naked)]
extern "C" fn thread_entry_wrapper() {
    core::arch::naked_asm!("sti", "call rbx", "ud2");
}

#[unsafe(naked)]
#[unsafe(no_mangle)]
unsafe extern "C" fn iretq_trampoline() {
    core::arch::naked_asm!("iretq");
}

fn setup_stack(stack_base: *mut u8, stack_size: u64, entry: u64) -> u64 {
    unsafe {
        let stack_top = stack_base.add(stack_size as usize) as *mut u64;
        let mut rsp = stack_top;

        rsp = rsp.sub(1); *rsp = 0x10;
        rsp = rsp.sub(1); *rsp = stack_top as u64;
        rsp = rsp.sub(1); *rsp = 0x202;
        rsp = rsp.sub(1); *rsp = 0x08;
        rsp = rsp.sub(1); *rsp = thread_entry_wrapper as *const () as u64;

        for i in 0..15usize {
            rsp = rsp.sub(1);
            *rsp = if i == 1 { entry } else { 0 };
        }

        rsp as u64
    }
}

impl TCB {
    fn new(stack_size: u64, entry: u64, pid: ProcessId, cr3: u64) -> Result<(), ThreadError> {
        let layout     = Layout::from_size_align(stack_size as usize, 4096).unwrap();
        let stack_base = unsafe { alloc_zeroed(layout) };
        if stack_base.is_null() {
            return Err(ThreadError::StackAllocationFailed);
        }

        let rsp       = setup_stack(stack_base, stack_size, entry);
        let stack_top = unsafe { stack_base.add(stack_size as usize) };

        let tcb = TCB {
            tid:              NEXT_THREAD_ID.fetch_add(1, Ordering::Relaxed),
            pid,
            stack_size,
            stack_base,
            stack_top,
            cpu_context:      CpuContext { rsp, ..CpuContext::default() },
            next:             core::ptr::null_mut(),
            cr3,
            state:            ThreadState::Ready,
            priority:         Priority::NORMAL,
            is_userspace:     false,
            kernel_stack_top: stack_top as u64,
        };

        let mut storage = THREAD_STORAGE.lock();
        storage.push(tcb);
        THREAD_COUNT.fetch_add(1, Ordering::AcqRel);
        Ok(())
    }

    // allocates a stack, sets up the initial frame, registers the thread, and returns its tid
    pub fn create_thread(stack_size: u64, entry: u64, pid: ProcessId, cr3: u64) -> Result<ThreadId, &'static str> {
        TCB::new(stack_size, entry, pid, cr3).map_err(|e| e.as_str())?;
        let tid = {
            let storage = THREAD_STORAGE.lock();
            storage.last().map(|t| t.tid).unwrap()
        };
        proc::add_thread_to_process(pid, tid);
        Ok(tid)
    }

    // wraps the current stack as a tcb without allocation, used for the boot thread
    pub fn from_current_stack(tid: u64, pid: u64, cr3: u64, rsp: u64) -> *mut TCB {
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
        };

        let mut storage = THREAD_STORAGE.lock();
        storage.push(tcb);
        THREAD_COUNT.fetch_add(1, Ordering::AcqRel);
        storage.last_mut().unwrap() as *mut TCB
    }
}

// frees the thread's stack and removes its tcb from storage
pub fn destroy_thread(tid: ThreadId) {
    let mut storage = THREAD_STORAGE.lock();
    if let Some(pos) = storage.iter().position(|t| t.tid == tid) {
        let tcb = &storage[pos];
        if !tcb.stack_base.is_null() && tcb.stack_size > 0 {
            unsafe {
                let layout = Layout::from_size_align(tcb.stack_size as usize, 4096).unwrap();
                alloc::alloc::dealloc(tcb.stack_base, layout);
            }
        }
        storage.remove(pos);
        THREAD_COUNT.fetch_sub(1, Ordering::AcqRel);
    }
}

// returns a raw pointer to the tcb matching tid, or None if not found
pub fn get_tcb_by_tid(tid: ThreadId) -> Option<*mut TCB> {
    let mut storage = THREAD_STORAGE.lock();
    storage.iter_mut()
        .find(|t| t.tid == tid)
        .map(|t| t as *mut TCB)
}

// registers the boot stack as tid=0 pid=0 and installs it as the scheduler's current thread
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