#![no_std]
use core::sync::atomic::{AtomicBool, Ordering};
use process::{PROCESS_COUNT, PROCESS_TABLE, Priority, ProcessState};

unsafe extern "C" {
    static APIC_TICKS_PER_SEC: u64;
}

const QUANTUM_TICKS: u64 = 5;

static SCHEDULER_ENABLED: AtomicBool = AtomicBool::new(false);
static mut CURRENT_TICKS: u64 = 0;
static mut QUANTUM_REMAINING: u64 = QUANTUM_TICKS;

/// Initializes the scheduler, setting the first process as running.
pub fn init_scheduler() {
    unsafe {
        if PROCESS_COUNT == 0 {
            return;
        }
        PROCESS_TABLE.current = 0;
        QUANTUM_REMAINING = QUANTUM_TICKS;
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

/// Handles timer interrupts and schedules the next process.
/// Returns the RSP of the process to resume.
#[inline(always)]
pub fn handle_timer_interrupt(current_rsp: u64) -> u64 {
    if !SCHEDULER_ENABLED.load(Ordering::Acquire) {
        return current_rsp;
    }

    unsafe {
        CURRENT_TICKS += 1;

        // Wake any sleeping processes whose timer has expired
        for i in 0..PROCESS_COUNT as usize {
            if let Some(proc) = PROCESS_TABLE.processes[i].as_mut() {
                if proc.state == ProcessState::Sleeping && CURRENT_TICKS >= proc.wake_at_tick {
                    proc.state = ProcessState::Ready;
                }
            }
        }

        schedule(current_rsp)
    }
}

/// Core scheduling logic. Decrements the quantum and switches tasks when needed.
#[inline(always)]
fn schedule(current_rsp: u64) -> u64 {
    unsafe {
        let current = PROCESS_TABLE.current;
        let current_state = PROCESS_TABLE.processes[current]
            .as_ref()
            .map(|p| p.state);

        match current_state {
            Some(ProcessState::Running) => {
                QUANTUM_REMAINING = QUANTUM_REMAINING.saturating_sub(1);

                if QUANTUM_REMAINING == 0 {
                    if let Some(next) = find_next_ready(current) {
                        QUANTUM_REMAINING = QUANTUM_TICKS;
                        return switch_to(current, current_rsp, next);
                    }
                    // No other process ready, keep running current
                    QUANTUM_REMAINING = QUANTUM_TICKS;
                }
            }
            Some(ProcessState::Terminated | ProcessState::Sleeping | ProcessState::Blocked) => {
                if let Some(next) = find_next_ready(current) {
                    QUANTUM_REMAINING = QUANTUM_TICKS;
                    return switch_to(current, current_rsp, next);
                }
                // Nothing ready at all — fall through and return current_rsp,
            }
            _ => {}
        }

        current_rsp
    }
}

/// Finds the next ready process, preferring non-idle priority.
fn find_next_ready(current: usize) -> Option<usize> {
    unsafe {
        let count = PROCESS_COUNT as usize;
        let mut idle_fallback: Option<usize> = None;

        for offset in 1..=count {
            let idx = (current + offset) % count;
            if let Some(proc) = PROCESS_TABLE.processes[idx].as_ref() {
                if proc.state != ProcessState::Ready {
                    continue;
                }
                if proc.priority == Priority::Idle {
                    idle_fallback.get_or_insert(idx);
                    continue;
                }
                return Some(idx);
            }
        }

        idle_fallback
    }
}

/// Saves current RSP, marks next process as running, returns its RSP.
#[inline(always)]
fn switch_to(current: usize, current_rsp: u64, next: usize) -> u64 {
    unsafe {
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            proc.rsp = current_rsp;
            if proc.state == ProcessState::Running {
                proc.state = ProcessState::Ready;
            }
        }

        let proc = PROCESS_TABLE.processes[next].as_mut().unwrap();
        proc.state = ProcessState::Running;
        PROCESS_TABLE.current = next;
        proc.rsp
    }
}

/// Yields the current process voluntarily and halts until the next interrupt.
/// The scheduler will pick it back up on the next timer tick.
pub fn yield_process() {
    unsafe {
        let current = PROCESS_TABLE.current;
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            proc.state = ProcessState::Ready;
        }
        QUANTUM_REMAINING = 0;
        core::arch::asm!("hlt");
    }
}

/// Blocks the current process until explicitly unblocked.
/// Halts in a loop so the CPU stays cool while waiting.
pub fn block_current() {
    unsafe {
        let current = PROCESS_TABLE.current;
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            proc.state = ProcessState::Blocked;
        }

        loop {
            core::arch::asm!("hlt");
            if let Some(proc) = PROCESS_TABLE.processes[PROCESS_TABLE.current].as_ref() {
                if proc.state != ProcessState::Blocked {
                    return;
                }
            }
        }
    }
}

/// Unblocks a process by PID, marking it as ready to run.
pub fn unblock_process(pid: u64) {
    if let Some(proc) = process::get_process_mut(pid) {
        if proc.state == ProcessState::Blocked {
            proc.state = ProcessState::Ready;
        }
    }
}

/// Puts the current process to sleep for a given duration in milliseconds.
/// Halts the CPU in a loop while sleeping so we don't spin wastefully.
pub fn sleep_proc_ms(ms: u64) {
    sleep_ticks(ms_to_ticks(ms));
}

/// Puts the current process to sleep for a given duration in microseconds.
pub fn sleep_proc_us(us: u64) {
    sleep_ticks(us_to_ticks(us));
}

/// Converts milliseconds to APIC ticks, rounding up to at least 1.
fn ms_to_ticks(ms: u64) -> u64 {
    unsafe { ((ms * APIC_TICKS_PER_SEC + 999) / 1000).max(1) }
}

/// Converts microseconds to APIC ticks, rounding up to at least 1.
fn us_to_ticks(us: u64) -> u64 {
    unsafe { ((us * APIC_TICKS_PER_SEC + 999_999) / 1_000_000).max(1) }
}

/// Core sleep implementation. Sets wake tick, marks process sleeping,
/// then halts in a loop — the timer IRQ will wake and reschedule us.
fn sleep_ticks(ticks: u64) {
    unsafe {
        let current = PROCESS_TABLE.current;
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            proc.wake_at_tick = CURRENT_TICKS + ticks;
            proc.state = ProcessState::Sleeping;
        }
        loop {
            core::arch::asm!("hlt");
            // After each interrupt, check if we've been woken
            if let Some(proc) = PROCESS_TABLE.processes[PROCESS_TABLE.current].as_ref() {
                if proc.state != ProcessState::Sleeping {
                    return;
                }
            }
        }
    }
}