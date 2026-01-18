#![no_std]

use core::arch::asm;
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use framebuffer::println;
use process::{ProcessState, PROCESS_COUNT, PROCESS_TABLE};

static SCHEDULER_ENABLED: AtomicBool = AtomicBool::new(false);
static QUANTUM: AtomicU64 = AtomicU64::new(10);
static TICK_COUNT: AtomicU64 = AtomicU64::new(0);

pub fn init_scheduler() {
    unsafe {
        if PROCESS_COUNT == 0 {
            return;
        }

        PROCESS_TABLE.current = 0;
        if let Some(proc) = PROCESS_TABLE.processes[0].as_mut() {
            proc.state = ProcessState::Running;
        }
    }
}

pub fn enable_scheduler() {
    SCHEDULER_ENABLED.store(true, Ordering::Release);
}

pub fn disable_scheduler() {
    SCHEDULER_ENABLED.store(false, Ordering::Release);
}

pub fn handle_timer_interrupt(current_rsp: u64) -> u64 {
    if !SCHEDULER_ENABLED.load(Ordering::Relaxed) {
        return current_rsp;
    }

    let ticks = TICK_COUNT.fetch_add(1, Ordering::Relaxed) + 1;
    let quantum = QUANTUM.load(Ordering::Relaxed);

    if ticks % quantum == 0 {
        schedule_preemptive(current_rsp)
    } else {
        current_rsp
    }
}

fn schedule_preemptive(current_rsp: u64) -> u64 {
    unsafe {
        if PROCESS_COUNT <= 1 {
            return current_rsp;
        }

        let current = PROCESS_TABLE.current;
        if current == usize::MAX {
            return current_rsp;
        }

        if let Some(next) = find_next_ready_process(current) {
            save_and_switch(current, current_rsp, next)
        } else {
            current_rsp
        }
    }
}

fn save_and_switch(current: usize, current_rsp: u64, next: usize) -> u64 {
    unsafe {
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            if proc.state == ProcessState::Running {
                proc.rsp = current_rsp;
                proc.state = ProcessState::Ready;
            }
        }

        if let Some(proc) = PROCESS_TABLE.processes[next].as_mut() {
            proc.state = ProcessState::Running;
            PROCESS_TABLE.current = next;
            proc.rsp
        } else {
            current_rsp
        }
    }
}

fn find_next_ready_process(current: usize) -> Option<usize> {
    unsafe {
        let process_count = PROCESS_COUNT as usize;
        let start = (current + 1) % process_count;
        let mut next = start;

        loop {
            if let Some(proc) = PROCESS_TABLE.processes[next].as_ref() {
                if proc.state == ProcessState::Ready {
                    return Some(next);
                }
            }

            next = (next + 1) % process_count;
            if next == start {
                break;
            }
        }

        None
    }
}

pub fn schedule() {
    if !SCHEDULER_ENABLED.load(Ordering::Relaxed) {
        return;
    }

    unsafe {
        if PROCESS_COUNT <= 1 {
            return;
        }

        let current = PROCESS_TABLE.current;
        if current == usize::MAX {
            return;
        }

        if let Some(next) = find_next_ready_process(current) {
            if next != current {
                println!("Yield: Switching from {} to {}", current, next);
                perform_context_switch(current, next);
            }
        }
    }
}

fn perform_context_switch(from: usize, to: usize) {
    unsafe {
        if let Some(from_proc) = PROCESS_TABLE.processes[from].as_mut() {
            from_proc.state = ProcessState::Ready;
        }

        if let Some(to_proc) = PROCESS_TABLE.processes[to].as_mut() {
            to_proc.state = ProcessState::Running;
        }

        PROCESS_TABLE.current = to;

        let from_rsp_ptr = &mut PROCESS_TABLE.processes[from]
            .as_mut()
            .unwrap()
            .rsp as *mut u64;
        let to_rsp = PROCESS_TABLE.processes[to].as_ref().unwrap().rsp;

        context_switch(from_rsp_ptr, to_rsp);

        PROCESS_TABLE.current = from;
    }
}

#[inline(never)]
fn context_switch(curr_rsp_ptr: *mut u64, next_rsp: u64) {
    unsafe {
        asm!(
            "push rbp",
            "push rbx",
            "push r12",
            "push r13",
            "push r14",
            "push r15",
            "mov [rdi], rsp",
            "mov rsp, rsi",
            "pop r15",
            "pop r14",
            "pop r13",
            "pop r12",
            "pop rbx",
            "pop rbp",
            "ret",
            in("rdi") curr_rsp_ptr,
            in("rsi") next_rsp,
            options(noreturn)
        );
    }
}

pub fn yield_process() {
    if SCHEDULER_ENABLED.load(Ordering::Relaxed) {
        schedule();
    }
}

pub fn block_current() {
    unsafe {
        let current = PROCESS_TABLE.current;
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            proc.state = ProcessState::Blocked;
        }
        schedule();
    }
}

pub fn unblock_process(pid: u64) {
    if let Some(proc) = process::get_process_mut(pid) {
        if proc.state == ProcessState::Blocked {
            proc.state = ProcessState::Ready;
        }
    }
}