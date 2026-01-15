#![no_std]

use memory::vmm::{VMM, PageTable};
use core::alloc::Layout;
use framebuffer::println;

extern crate alloc;

pub static mut PROCESS_COUNT: u64 = 0;
pub static mut PROCESS_TABLE: ProcessTable = ProcessTable {
    processes: [const { None }; 64],
    current: usize::MAX,
};

const KERNEL_STACK_SIZE: usize = 64 * 1024;

pub struct Process {
    pub pid: u64,
    pub rsp: u64,
    pub stack_base: *mut u8,
    pub entry: *mut (),
    pub pml4: *mut PageTable,
    pub state: ProcessState,
}

#[derive(Clone, Copy, PartialEq)]
pub enum ProcessState {
    Ready,
    Running,
    Blocked,
}

pub struct ProcessTable {
    pub processes: [Option<Process>; 64],
    pub current: usize,
}

fn allocate_kernel_stack() -> Option<*mut u8> {
    unsafe {
        let layout = Layout::from_size_align(KERNEL_STACK_SIZE, 4096).ok()?;
        let ptr = alloc::alloc::alloc_zeroed(layout);
        if ptr.is_null() {
            None
        } else {
            Some(ptr)
        }
    }
}

fn setup_initial_stack(stack_base: *mut u8, entry: *mut ()) -> u64 {
    unsafe {
        let stack_top = stack_base.add(KERNEL_STACK_SIZE) as *mut u64;
        let mut rsp = stack_top;
        
        rsp = rsp.sub(1);
        *rsp = entry as u64;
        
        for _ in 0..15 {
            rsp = rsp.sub(1);
            *rsp = 0;
        }
        
        rsp as u64
    }
}

pub fn init_kernel_process(rsp: u64) {
    unsafe {
        let kernel_pml4 = VMM::get_page_table();
        
        let process = Process {
            pid: 0,
            rsp,
            stack_base: core::ptr::null_mut(),
            entry: core::ptr::null_mut(),
            pml4: kernel_pml4,
            state: ProcessState::Running,
        };
        
        PROCESS_TABLE.processes[0] = Some(process);
        PROCESS_TABLE.current = 0;
        PROCESS_COUNT = 1;
        
        println!("Kernel process initialized at RSP: 0x{:x}", rsp);
    }
}

pub fn create_process(entry: *mut ()) -> Option<u64> {
    unsafe {
        let pid = PROCESS_COUNT;
        if pid >= 64 {
            return None;
        }
        
        let stack_base = allocate_kernel_stack()?;
        let rsp = setup_initial_stack(stack_base, entry);
        
        let kernel_pml4 = VMM::get_page_table();
        
        let process = Process {
            pid,
            rsp,
            stack_base,
            entry,
            pml4: kernel_pml4,
            state: ProcessState::Ready,
        };
        
        PROCESS_TABLE.processes[pid as usize] = Some(process);
        PROCESS_COUNT += 1;
        
        println!("Created process {} at RSP: 0x{:x}", pid, rsp);
        
        Some(pid)
    }
}

pub fn get_current_process() -> Option<&'static Process> {
    unsafe {
        if PROCESS_TABLE.current == usize::MAX {
            return None;
        }
        PROCESS_TABLE.processes[PROCESS_TABLE.current].as_ref()
    }
}

pub fn get_current_process_mut() -> Option<&'static mut Process> {
    unsafe {
        if PROCESS_TABLE.current == usize::MAX {
            return None;
        }
        PROCESS_TABLE.processes[PROCESS_TABLE.current].as_mut()
    }
}

pub fn get_process(pid: u64) -> Option<&'static Process> {
    unsafe {
        if pid >= 64 {
            return None;
        }
        PROCESS_TABLE.processes[pid as usize].as_ref()
    }
}

pub fn get_process_mut(pid: u64) -> Option<&'static mut Process> {
    unsafe {
        if pid >= 64 {
            return None;
        }
        PROCESS_TABLE.processes[pid as usize].as_mut()
    }
}

pub fn destroy_process(pid: u64) -> bool {
    unsafe {
        if pid >= 64 {
            return false;
        }
        
        if let Some(process) = PROCESS_TABLE.processes[pid as usize].take() {
            let layout = Layout::from_size_align(KERNEL_STACK_SIZE, 4096).unwrap();
            alloc::alloc::dealloc(process.stack_base, layout);
            
            if PROCESS_TABLE.current == pid as usize {
                PROCESS_TABLE.current = 0;
            }
            true
        } else {
            false
        }
    }
}