#![no_std]

use framebuffer::println;
use process::{PROCESS_COUNT, PROCESS_TABLE, ProcessState};
use core::sync::atomic::{AtomicU64, AtomicBool, Ordering};
use core::arch::asm;

static SCHEDULER_ENABLED: AtomicBool = AtomicBool::new(false);
static QUANTUM: AtomicU64 = AtomicU64::new(3);
static TICK_COUNT: AtomicU64 = AtomicU64::new(0);

pub fn init_scheduler() {
    unsafe {
        if PROCESS_COUNT > 0 {
            PROCESS_TABLE.current = 0;
            if let Some(proc) = PROCESS_TABLE.processes[0].as_mut() {
                proc.state = ProcessState::Running;
            }
        }
    }
}

pub fn enable_scheduler() {
    SCHEDULER_ENABLED.store(true, Ordering::Release);
}

pub fn disable_scheduler() {
    SCHEDULER_ENABLED.store(false, Ordering::Release);
}

pub fn timer_tick() {
    if !SCHEDULER_ENABLED.load(Ordering::Relaxed) {
        return;
    }
    
    TICK_COUNT.fetch_add(1, Ordering::Relaxed);
}

pub fn handle_timer_interrupt(current_rsp: u64) -> u64 {
    if !SCHEDULER_ENABLED.load(Ordering::Relaxed) {
        return current_rsp;
    }
    
    let ticks = TICK_COUNT.fetch_add(1, Ordering::Relaxed);
    let quantum = QUANTUM.load(Ordering::Relaxed);
    
    if ticks % quantum == 0 {
        schedule_from_interrupt(current_rsp)
    } else {
        current_rsp
    }
}

fn schedule_from_interrupt(current_rsp: u64) -> u64 {
    unsafe {
        if PROCESS_COUNT <= 1 {
            return current_rsp;
        }

        let current = PROCESS_TABLE.current;
        if current == usize::MAX {
            return current_rsp;
        }
        
        if let Some(from_proc) = PROCESS_TABLE.processes[current].as_mut() {
            if from_proc.state != ProcessState::Terminated {
                from_proc.rsp = current_rsp;
                from_proc.state = ProcessState::Ready;
            }
        }
        
        let mut next = (current + 1) % (PROCESS_COUNT as usize);
        let mut attempts = 0;
        
        while attempts < PROCESS_COUNT {
            if let Some(proc) = PROCESS_TABLE.processes[next].as_ref() {
                if proc.state == ProcessState::Ready {
                    if next != current {
                        println!("IRQ: Switching from {} to {}", current, next);
                        
                        if let Some(to_proc) = PROCESS_TABLE.processes[next].as_mut() {
                            to_proc.state = ProcessState::Running;
                            PROCESS_TABLE.current = next;
                            return to_proc.rsp;
                        }
                    }
                }
            }
            next = (next + 1) % (PROCESS_COUNT as usize);
            attempts += 1;
        }
        
        if let Some(curr_proc) = PROCESS_TABLE.processes[current].as_mut() {
            if curr_proc.state == ProcessState::Ready {
                curr_proc.state = ProcessState::Running;
            }
        }
        
        current_rsp
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
        
        let mut next = (current + 1) % (PROCESS_COUNT as usize);
        let mut attempts = 0;
        
        while attempts < PROCESS_COUNT {
            if let Some(proc) = PROCESS_TABLE.processes[next].as_ref() {
                if proc.state == ProcessState::Ready || proc.state == ProcessState::Running {
                    if next != current {
                        println!("Yield: Switching from {} to {}", current, next);
                        do_switch(current, next);
                        return;
                    }
                }
            }
            next = (next + 1) % (PROCESS_COUNT as usize);
            attempts += 1;
        }
    }
}

fn do_switch(from: usize, to: usize) {
    unsafe {
        if let Some(from_proc) = PROCESS_TABLE.processes[from].as_mut() {
            from_proc.state = ProcessState::Ready;
        }
        
        if let Some(to_proc) = PROCESS_TABLE.processes[to].as_mut() {
            to_proc.state = ProcessState::Running;
        }
        
        PROCESS_TABLE.current = to;
        
        let from_rsp = &mut PROCESS_TABLE.processes[from].as_mut().unwrap().rsp as *mut u64;
        let to_rsp = PROCESS_TABLE.processes[to].as_ref().unwrap().rsp;
        
        context_switch(from_rsp, to_rsp);
        
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