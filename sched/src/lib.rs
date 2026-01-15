#![no_std]

use process::{PROCESS_COUNT, PROCESS_TABLE, ProcessState};
use core::sync::atomic::{AtomicU64, AtomicBool, Ordering};
use core::arch::asm;

static SCHEDULER_ENABLED: AtomicBool = AtomicBool::new(false);
static QUANTUM: AtomicU64 = AtomicU64::new(3);

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
    static TICK_COUNT: AtomicU64 = AtomicU64::new(0);
    
    if !SCHEDULER_ENABLED.load(Ordering::Relaxed) {
        return;
    }

    let ticks = TICK_COUNT.fetch_add(1, Ordering::Relaxed);
    let quantum = QUANTUM.load(Ordering::Relaxed);
    
    if ticks % quantum == 0 {
        schedule();
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
        let to_rsp = &PROCESS_TABLE.processes[to].as_ref().unwrap().rsp as *const u64;
        
        context_switch(from_rsp, to_rsp);
    }
}

fn context_switch(curr_rsp: *mut u64, next_rsp: *const u64) {
    unsafe {
        asm!(
            "push rax",
            "push rbx",
            "push rcx",
            "push rdx",
            "push rbp",
            "push rsi",
            "push rdi",
            "push r8",
            "push r9",
            "push r10",
            "push r11",
            "push r12",
            "push r13",
            "push r14",
            "push r15",
            "mov [rdi], rsp",
            "mov rsp, [rsi]",
            "pop r15",
            "pop r14",
            "pop r13",
            "pop r12",
            "pop r11",
            "pop r10",
            "pop r9",
            "pop r8",
            "pop rdi",
            "pop rsi",
            "pop rbp",
            "pop rdx",
            "pop rcx",
            "pop rbx",
            "pop rax",
            in("rdi") curr_rsp,
            in("rsi") next_rsp,
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