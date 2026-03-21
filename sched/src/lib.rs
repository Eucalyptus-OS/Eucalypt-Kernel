#![no_std]

/// Round robin scheduler
/// How it works
/// first we have a list of processes and the scheduler goes from pid1 -> pid2 and so on

use core::sync::atomic::{AtomicBool, Ordering};
use memory::vmm::PageTable;
use process::{PROCESS_COUNT, PROCESS_TABLE, Priority, ProcessState};

unsafe extern "C" {
    static APIC_TICKS_PER_SEC: u64;
}

const QUANTUM_TICKS: u64 = 25;

static SCHEDULER_ENABLED: AtomicBool = AtomicBool::new(false);
static mut CURRENT_TICKS: u64 = 0;
static mut QUANTUM_REMAINING: u64 = QUANTUM_TICKS;

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

#[inline(always)]
pub fn handle_timer_interrupt(current_rsp: u64) -> u64 {
    if !SCHEDULER_ENABLED.load(Ordering::Acquire) {
        return current_rsp;
    }
    
    unsafe {
        CURRENT_TICKS += 1;
        
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

#[inline(always)]
fn schedule(current_rsp: u64) -> u64 {
    unsafe {
        let current = PROCESS_TABLE.current;
        let current_state = PROCESS_TABLE.processes[current]
            .as_ref()
            .map(|p| p.state);
        
        match current_state {
            Some(ProcessState::Running) => {
                if QUANTUM_REMAINING > 0 {
                    QUANTUM_REMAINING -= 1;
                }
                
                if QUANTUM_REMAINING == 0 {
                    if let Some(next) = find_next_ready(current, true) {
                        QUANTUM_REMAINING = QUANTUM_TICKS;
                        return switch_to(current, current_rsp, next);
                    } else {
                        QUANTUM_REMAINING = QUANTUM_TICKS;
                    }
                }
            }
            Some(ProcessState::Terminated | ProcessState::Sleeping | ProcessState::Blocked) => {
                if let Some(next) = find_next_ready(current, true) {
                    QUANTUM_REMAINING = QUANTUM_TICKS;
                    return switch_to(current, current_rsp, next);
                }
            }
            _ => {}
        }
        
        current_rsp
    }
}

fn find_next_ready(current: usize, allow_idle: bool) -> Option<usize> {
    unsafe {
        let count = PROCESS_COUNT as usize;
        let mut best_idx = None;
        let mut best_priority = Priority::Idle;

        for offset in 1..=count {
            let idx = (current + offset) % count;
            if let Some(proc) = PROCESS_TABLE.processes[idx].as_ref() {
                if proc.state != ProcessState::Ready {
                    continue;
                }
                
                if proc.priority > best_priority {
                    best_priority = proc.priority;
                    best_idx = Some(idx);
                    
                    // Optimization: if we found a realtime task, we can stop if we just want round-robin within it.
                    // But we want to preserve round-robin, so we should actually prefer the first one we find
                    // that has the highest possible priority.
                    if best_priority == Priority::Realtime {
                        return Some(idx);
                    }
                } else if proc.priority == best_priority && best_idx.is_none() {
                    best_idx = Some(idx);
                }
            }
        }
        
        if best_priority == Priority::Idle && !allow_idle {
            return None;
        }

        best_idx
    }
}

fn load_cr3(pml4: *mut PageTable) {
    const HHDM_OFFSET: u64 = 0xFFFF_8000_0000_0000;
    let phys = if (pml4 as u64) >= HHDM_OFFSET {
        (pml4 as u64) - HHDM_OFFSET
    } else {
        pml4 as u64
    };

    let current_cr3: u64;
    unsafe { core::arch::asm!("mov {}, cr3", out(reg) current_cr3, options(nomem, nostack)) };
    if current_cr3 & 0x000F_FFFF_FFFF_F000 == phys {
        return;
    }

    unsafe { core::arch::asm!("mov cr3, {}", in(reg) phys, options(nostack, preserves_flags)) };
}

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

        let new_pml4 = proc.pml4;
        let new_rsp = proc.rsp;

        load_cr3(new_pml4);

        new_rsp
    }
}


#[inline(always)]
pub fn reschedule() {
    unsafe {
        core::arch::asm!("int 32");
    }
}

pub fn yield_process() {
    unsafe {
        let current = PROCESS_TABLE.current;
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            proc.state = ProcessState::Ready;
        }
        QUANTUM_REMAINING = 0;
        reschedule();
    }
}

pub fn block_current() {
    unsafe {
        let current = PROCESS_TABLE.current;
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            proc.state = ProcessState::Blocked;
        }
        reschedule();
    }
}

pub fn unblock_process(pid: u64) {
    if let Some(proc) = process::get_process_mut(pid) {
        if proc.state == ProcessState::Blocked {
            proc.state = ProcessState::Ready;
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
        reschedule();
        
        while let Some(proc) = PROCESS_TABLE.processes[PROCESS_TABLE.current].as_ref() {
            if proc.state == ProcessState::Sleeping {
                reschedule();
            } else {
                break;
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
        reschedule();
        
        while let Some(proc) = PROCESS_TABLE.processes[PROCESS_TABLE.current].as_ref() {
            if proc.state == ProcessState::Sleeping {
                reschedule();
            } else {
                break;
            }
        }
    }
}