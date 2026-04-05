use core::sync::atomic::{AtomicPtr, AtomicUsize, Ordering};

use crate::thread::{TCB, ThreadState, get_thread, get_thread_count};

pub static CURRENT_THREAD: AtomicPtr<TCB> = AtomicPtr::new(core::ptr::null_mut());
static CURRENT_INDEX: AtomicUsize = AtomicUsize::new(0);
static TICK_COUNT: AtomicUsize = AtomicUsize::new(0);
const TICKS_PER_SLICE: usize = 10;

pub fn schedule(old_rsp: u64) -> u64 {
    let count = get_thread_count();
    if count < 2 {
        return old_rsp;
    }

    let old_index = CURRENT_INDEX.load(Ordering::Acquire);
    let old = get_thread(old_index);

    unsafe {
        if (*old).state == ThreadState::Running {
            (*old).state = ThreadState::Ready;
        }
        (*old).cpu_context.rsp = old_rsp;
    }

    let ticks = TICK_COUNT.fetch_add(1, Ordering::AcqRel);
    if ticks % TICKS_PER_SLICE != 0 {
        return old_rsp;
    }

    let mut next_index = (old_index + 1) % count;
    let mut new = get_thread(next_index);

    unsafe {
        while (*new).state == ThreadState::Blocked || (*new).cpu_context.rsp == 0 {
            next_index = (next_index + 1) % count;
            if next_index == old_index {
                return old_rsp;
            }
            new = get_thread(next_index);
        }

        (*new).state = ThreadState::Running;
    }

    CURRENT_INDEX.store(next_index, Ordering::Release);
    CURRENT_THREAD.store(new, Ordering::Release);

    unsafe {
        let new_cr3 = (*new).cr3;
        let mut current_cr3: u64;
        core::arch::asm!("mov {}, cr3", out(reg) current_cr3);
        
        if current_cr3 != new_cr3 && new_cr3 != 0 {
            core::arch::asm!("mov cr3, {}", in(reg) new_cr3);
        }

        let final_rsp = (*new).cpu_context.rsp;
        if final_rsp == 0 {
            old_rsp
        } else {
            final_rsp
        }
    }
}

pub fn get_current_thread() -> *mut TCB {
    CURRENT_THREAD.load(Ordering::Acquire)
}

pub fn set_current_thread(tcb: *mut TCB) {
    CURRENT_THREAD.store(tcb, Ordering::Release);
}

pub fn set_current_index(index: usize) {
    CURRENT_INDEX.store(index, Ordering::Release);
}

pub fn get_current_index() -> usize {
    CURRENT_INDEX.load(Ordering::Acquire)
}