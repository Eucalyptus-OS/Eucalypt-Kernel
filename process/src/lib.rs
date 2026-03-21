#![no_std]

/// Creation yes and destruction yes

extern crate alloc;

use core::alloc::Layout;
use framebuffer::println;
use memory::vmm::{PageTable, VMM};
use vfs::{FD, STDIN_NODE_ID, STDOUT_NODE_ID, STDERR_NODE_ID, O_RDONLY, O_WRONLY, vfs_close};

const KERNEL_STACK_SIZE: usize = 64 * 1024;
const MAX_PROCESSES: usize = 64;

pub static mut PROCESS_COUNT: u64 = 0;
pub static mut PROCESS_TABLE: ProcessTable = ProcessTable {
    processes: [const { None }; MAX_PROCESSES],
    current: usize::MAX,
};

#[derive(Clone, Copy, PartialEq, PartialOrd)]
pub enum Priority {
    Idle = 0,
    Normal = 1,
    High = 2,
    Realtime = 3,
}

#[derive(Clone, Copy, PartialEq)]
pub enum ProcessState {
    Ready,
    Running,
    Blocked,
    Sleeping,
    Terminated,
}

pub struct Process {
    pub pid: u64,
    pub rsp: u64,
    pub stack_base: *mut u8,
    pub entry: *mut (),
    pub pml4: *mut PageTable,
    pub state: ProcessState,
    pub priority: Priority,
    pub fildes: [FD; 1024], 
    pub ticks_ready: u64,
    pub wake_at_tick: u64,
}

impl Process {
    /// Returns the index of the first empty FD slot at index ≥ 3 (0–2 are
    /// reserved for stdin/stdout/stderr).
    pub fn get_free_fd_index(&self) -> Option<usize> {
        self.fildes[3..].iter().position(|fd| fd.is_empty()).map(|i| i + 3)
    }

    /// Opens `node_id` in this process and returns the assigned FD index.
    /// Returns `None` when the FD table is full.
    pub fn open_fd(&mut self, node_id: u32, flags: u32) -> Option<usize> {
        let idx = self.get_free_fd_index()?;
        self.fildes[idx] = FD { node_id, flags, ref_count: 1 };
        Some(idx)
    }

    /// Closes the FD at `fd`. Decrements the ref-count and removes the
    /// backing VFS node when it reaches zero.
    /// Returns `false` when `fd` is out of range or already closed.
    pub fn close_fd(&mut self, fd: usize) -> bool {
        if fd >= self.fildes.len() {
            return false;
        }
        let entry = &mut self.fildes[fd];
        if entry.is_empty() {
            return false;
        }
        entry.ref_count = entry.ref_count.saturating_sub(1);
        if entry.ref_count == 0 {
            if !entry.is_special() {
                let _ = vfs_close(entry.node_id);
            }
            *entry = FD::EMPTY;
        }
        true
    }
}

pub struct ProcessTable {
    pub processes: [Option<Process>; MAX_PROCESSES],
    pub current: usize,
}

pub fn init_kernel_process(rsp: u64) {
    let kernel_pml4 = VMM::get_page_table();

    let mut fildes = [FD::EMPTY; 1024];
    fildes[0] = FD { node_id: STDIN_NODE_ID,  flags: O_RDONLY, ref_count: 1 };
    fildes[1] = FD { node_id: STDOUT_NODE_ID, flags: O_WRONLY, ref_count: 1 };
    fildes[2] = FD { node_id: STDERR_NODE_ID, flags: O_WRONLY, ref_count: 1 };

    let process = Process {
        pid: 0,
        rsp,
        stack_base: core::ptr::null_mut(),
        entry: core::ptr::null_mut(),
        pml4: kernel_pml4,
        state: ProcessState::Running,
        priority: Priority::Idle,
        fildes,
        ticks_ready: 0,
        wake_at_tick: 0,
    };

    unsafe {
        PROCESS_TABLE.processes[0] = Some(process);
        PROCESS_TABLE.current = 0;
        PROCESS_COUNT = 1;
    }

    println!("Kernel process initialized at RSP: 0x{:x}", rsp);
}

pub fn create_process(entry: *mut ()) -> Option<u64> {
    unsafe {
        let mut free_slot = None;
        for i in 0..MAX_PROCESSES {
            if PROCESS_TABLE.processes[i].is_none() {
                free_slot = Some(i);
                break;
            }
        }

        let pid = free_slot? as u64;
        
        let stack_base = allocate_kernel_stack()?;
        let rsp = setup_initial_stack(stack_base, entry);

        let mut mapper = VMM::get_mapper();
        let user_pml4 = mapper.create_user_pml4()?;

        let mut fildes = [FD::EMPTY; 1024];
        fildes[0] = FD { node_id: STDIN_NODE_ID,  flags: O_RDONLY, ref_count: 1 };
        fildes[1] = FD { node_id: STDOUT_NODE_ID, flags: O_WRONLY, ref_count: 1 };
        fildes[2] = FD { node_id: STDERR_NODE_ID, flags: O_WRONLY, ref_count: 1 };

        let process = Process {
            pid,
            rsp,
            stack_base,
            entry,
            pml4: user_pml4,
            state: ProcessState::Ready,
            priority: Priority::Normal,
            fildes,
            ticks_ready: 0,
            wake_at_tick: 0,
        };

        PROCESS_TABLE.processes[pid as usize] = Some(process);
        if pid >= PROCESS_COUNT {
            PROCESS_COUNT = pid + 1;
        }

        Some(pid)
    }
}

pub fn destroy_process(pid: u64) -> bool {
    unsafe {
        if pid >= MAX_PROCESSES as u64 {
            return false;
        }

        if let Some(mut process) = PROCESS_TABLE.processes[pid as usize].take() {
            for fd in process.fildes.iter_mut() {
                if !fd.is_empty() {
                    fd.ref_count = fd.ref_count.saturating_sub(1);
                    if fd.ref_count == 0 {
                        if !fd.is_special() {
                            let _ = vfs_close(fd.node_id);
                            println!("Closed VFS node {}", fd.node_id);
                        }
                        *fd = FD::EMPTY;
                    }
                }
            }
            if !process.stack_base.is_null() {
                let layout = Layout::from_size_align(KERNEL_STACK_SIZE, 4096).unwrap();
                alloc::alloc::dealloc(process.stack_base, layout);
            }

            if PROCESS_TABLE.current == pid as usize {
                PROCESS_TABLE.current = 0;
            }
            true
        } else {
            false
        }
    }
}

pub fn cleanup_terminated_processes() {
    unsafe {
        for i in 0..PROCESS_COUNT as usize {
            if let Some(proc) = PROCESS_TABLE.processes[i].as_ref() {
                if proc.state == ProcessState::Terminated {
                    println!("Cleaning up terminated process {}", proc.pid);
                    destroy_process(proc.pid);
                }
            }
        }
    }
}

pub fn exit_current_process() {
    unsafe {
        let current = PROCESS_TABLE.current;
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            println!("Process {} exiting", proc.pid);
            proc.state = ProcessState::Terminated;
        }
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
        if pid >= MAX_PROCESSES as u64 {
            return None;
        }
        PROCESS_TABLE.processes[pid as usize].as_ref()
    }
}

pub fn does_pid_exist(pid: u64) -> bool {
    unsafe {
        if pid >= MAX_PROCESSES as u64 && pid > PROCESS_COUNT as u64 {
            return false
        }
    }
    true
}

pub fn get_process_mut(pid: u64) -> Option<&'static mut Process> {
    unsafe {
        if pid >= MAX_PROCESSES as u64 {
            return None;
        }
        PROCESS_TABLE.processes[pid as usize].as_mut()
    }
}

fn allocate_kernel_stack() -> Option<*mut u8> {
    let layout = Layout::from_size_align(KERNEL_STACK_SIZE, 4096).ok()?;
    let ptr = unsafe { alloc::alloc::alloc_zeroed(layout) };
    
    if ptr.is_null() {
        None
    } else {
        Some(ptr)
    }
}

fn setup_initial_stack(stack_base: *mut u8, entry: *mut ()) -> u64 {
    unsafe {
        let stack_top = stack_base.add(KERNEL_STACK_SIZE) as *mut u64;
        let mut rsp = stack_top;

        rsp = rsp.sub(1);
        *rsp = 0x10;

        rsp = rsp.sub(1);
        *rsp = stack_top as u64;

        rsp = rsp.sub(1);
        *rsp = 0x202;

        rsp = rsp.sub(1);
        *rsp = 0x08;

        rsp = rsp.sub(1);
        *rsp = process_entry_wrapper as *const () as u64;

        for i in 0..15 {
            rsp = rsp.sub(1);
            *rsp = if i == 1 { entry as u64 } else { 0 };
        }

        rsp as u64
    }
}

#[unsafe(naked)]
extern "C" fn process_entry_wrapper() {
    core::arch::naked_asm!(
        "call rbx",
        "mov rdi, rax",
        "call {exit}",
        "ud2",
        exit = sym process_exit,
    );
}

#[unsafe(no_mangle)]
fn process_exit(return_value: u64) {
    unsafe {
        let current = PROCESS_TABLE.current;
        if let Some(proc) = PROCESS_TABLE.processes[current].as_mut() {
            println!("Process {} exited\nReturn val: {}", proc.pid, return_value);
            proc.state = ProcessState::Terminated;
        }
        core::arch::asm!("int 32");
        loop {
            core::arch::asm!("hlt");
        }
    }
}