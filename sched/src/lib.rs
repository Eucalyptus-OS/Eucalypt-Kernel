#![no_std]

use core::sync::atomic::{AtomicBool, Ordering};
use process::{PROCESS_COUNT, PROCESS_TABLE, ProcessState};

unsafe extern "C" {
    static APIC_TICKS_PER_SEC: u64;
}

const QUANTUM_TICKS: u64 = 10;

static SCHEDULER_ENABLED: AtomicBool = AtomicBool::new(false);
static mut CURRENT_TICKS: u64 = 0;

pub fn init_scheduler() {
    unsafe {
        if PROCESS_COUNT == 0 {
            return;
        }
        PROCESS_TABLE.current = 0;
        if let Some(proc) = PROCESS_TABLE.processes[0].as_mut() {
            proc.state = ProcessState::Running;
            proc.ticks_ready = 0;
        }
    }
}

pub fn enable_scheduler() {
    SCHEDULER_ENABLED.store(true, Ordering::Release);
}

pub fn disable_scheduler() {
    SCHEDULER_ENABLED.store(false, Ordering::Release);
}

#[inline(always)]
pub fn handle_timer_interrupt(current_rsp: u64) -> u64 {
    if !SCHEDULER_ENABLED.load(Ordering::Acquire) {
        return current_rsp;
    }

    unsafe {
        CURRENT_TICKS += 1;

        let count = PROCESS_COUNT as usize;
        let mut woke_any = false;

        for i in 0..count {
            if let Some(proc) = PROCESS_TABLE.processes[i].as_mut() {
                match proc.state {
                    ProcessState::Ready => {
                        proc.ticks_ready += 1;
                    }
                    ProcessState::Sleeping => {
                        if CURRENT_TICKS >= proc.wake_at_tick {
                            proc.state = ProcessState::Ready;
                            proc.ticks_ready = 0;
                            woke_any = true;
                        }
                    }
                    _ => {}
                }
            }
        }

        let current = PROCESS_TABLE.current;

        if let Some(proc) = PROCESS_TABLE.processes[current].as_ref() {
            if proc.state != ProcessState::Running {
                if let Some(next) = find_next_process(current) {
                    return switch_to(current, current_rsp, next);
                }
                return current_rsp;
            }
        }

        if woke_any || CURRENT_TICKS % QUANTUM_TICKS == 0 {
            if let Some(next) = find_next_process(current) {
                return switch_to(current, current_rsp, next);
            }
        }

        current_rsp
    }
}

fn find_next_process(current: usize) -> Option<usize> {
    unsafe {
        let count = PROCESS_COUNT as usize;
        if count <= 1 {
            return None;
        }

        let mut best = None;
        let mut best_ticks = 0;

        for offset in 1..count {
            let idx = (current + offset) % count;
            if let Some(proc) = PROCESS_TABLE.processes[idx].as_ref() {
                if proc.state == ProcessState::Ready {
                    if best.is_none() || proc.ticks_ready > best_ticks {
                        best = Some(idx);
                        best_ticks = proc.ticks_ready;
                    }
                }
            }
        }

        best
    }
}

#[inline(always)]
fn switch_to(current: usize, current_rsp: u64, next: usize) -> u64 {
    unsafe {
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            proc.rsp = current_rsp;
            if proc.state == ProcessState::Running {
                proc.state = ProcessState::Ready;
                proc.ticks_ready = 0;
            }
        }

        let proc = PROCESS_TABLE.processes[next].as_mut().unwrap();
        proc.state = ProcessState::Running;
        proc.ticks_ready = 0;
        PROCESS_TABLE.current = next;
        proc.rsp
    }
}

pub fn yield_process() {
    unsafe {
        let current = PROCESS_TABLE.current;
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            proc.state = ProcessState::Ready;
            proc.ticks_ready = 0;
        }
        core::arch::asm!("hlt");
    }
}

pub fn block_current() {
    unsafe {
        let current = PROCESS_TABLE.current;
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            proc.state = ProcessState::Blocked;
        }
        core::arch::asm!("hlt");
    }
}

pub fn unblock_process(pid: u64) {
    if let Some(proc) = process::get_process_mut(pid) {
        if proc.state == ProcessState::Blocked {
            proc.state = ProcessState::Ready;
            proc.ticks_ready = 0;
        }
    }
}

pub fn sleep_proc_ms(ms: u64) {
    unsafe {
        let ticks = ((ms * APIC_TICKS_PER_SEC + 999) / 1000).max(1);

        let current = PROCESS_TABLE.current;
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            proc.wake_at_tick = CURRENT_TICKS + ticks;
            proc.state = ProcessState::Sleeping;
        }

        loop {
            core::arch::asm!("hlt");
            let current = PROCESS_TABLE.current;
            if let Some(proc) = PROCESS_TABLE.processes[current].as_ref() {
                if proc.state != ProcessState::Sleeping {
                    return;
                }
            }
        }
    }
}

pub fn sleep_proc_us(us: u64) {
    unsafe {
        let ticks = ((us * APIC_TICKS_PER_SEC + 999_999) / 1_000_000).max(1);

        let current = PROCESS_TABLE.current;
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            proc.wake_at_tick = CURRENT_TICKS + ticks;
            proc.state = ProcessState::Sleeping;
        }

        loop {
            core::arch::asm!("hlt");
            let current = PROCESS_TABLE.current;
            if let Some(proc) = PROCESS_TABLE.processes[current].as_ref() {
                if proc.state != ProcessState::Sleeping {
                    return;
                }
            }
        }
    }
}
