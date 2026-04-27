use core::sync::atomic::{AtomicU64, Ordering};
use alloc::vec::Vec;
use spin::Mutex;
use vfs::{FD, D_STDIN, D_STDOUT, D_STDERR};
use memory::vmm::VMM;
use memory::paging::PageTable;
use crate::thread::{ThreadId, destroy_thread};

static NEXT_PID: AtomicU64 = AtomicU64::new(1);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProcessState {
    Running,
    Sleeping,
    Zombie,
    Dead,
}

pub struct PCB {
    pub pid:        u64,
    pub cr3:        u64,
    pub heap_start: u64,
    pub heap_end:   u64,
    pub fd_table:   Vec<FD>,
    pub threads:    Vec<ThreadId>,
    pub state:      ProcessState,
    pub parent:     Option<u64>,
}

static PROCESS_LIST: Mutex<Vec<PCB>> = Mutex::new(Vec::new());

// allocates a pml4, finds a free heap region, and registers a new pcb returning its pid
pub fn new_process(parent: Option<u64>) -> Option<u64> {
    let pid = NEXT_PID.fetch_add(1, Ordering::Relaxed);
    let mapper = VMM::get_kernel_mapper();
    let pml4 = mapper.create_user_pml4()?;
    let heap_base = mapper.find_free_virt_region(pml4, 4096)?;

    let mut fd_table = Vec::new();
    fd_table.push(FD::new(0, D_STDIN));
    fd_table.push(FD::new(1, D_STDOUT));
    fd_table.push(FD::new(2, D_STDERR));

    let pcb = PCB {
        pid,
        cr3:        pml4 as u64,
        heap_start: heap_base,
        heap_end:   heap_base,
        fd_table,
        threads:    Vec::new(),
        state:      ProcessState::Running,
        parent,
    };

    PROCESS_LIST.lock().push(pcb);
    Some(pid)
}

// returns the total number of entries in the process list including zombies and dead
pub fn get_process_count() -> usize {
    PROCESS_LIST.lock().len()
}

// locks the list and calls f with a mutable reference to the pcb matching pid
pub fn with_process_mut<R, F: FnOnce(&mut PCB) -> R>(pid: u64, f: F) -> Option<R> {
    let mut list = PROCESS_LIST.lock();
    list.iter_mut().find(|p| p.pid == pid).map(f)
}

// locks the list and calls f with a shared reference to the pcb matching pid
pub fn with_process<R, F: FnOnce(&PCB) -> R>(pid: u64, f: F) -> Option<R> {
    let list = PROCESS_LIST.lock();
    list.iter().find(|p| p.pid == pid).map(f)
}

// appends tid to the thread list of the process identified by pid
pub fn add_thread_to_process(pid: u64, tid: ThreadId) {
    with_process_mut(pid, |pcb| pcb.threads.push(tid));
}

// removes tid from the process and transitions it to zombie if no threads remain
pub fn remove_thread_from_process(pid: u64, tid: ThreadId) {
    with_process_mut(pid, |pcb| {
        pcb.threads.retain(|&t| t != tid);
        if pcb.threads.is_empty() {
            pcb.state = ProcessState::Zombie;
        }
    });
}

// returns true if the process has no live threads or does not exist
pub fn is_threadless(pid: u64) -> bool {
    with_process(pid, |pcb| pcb.threads.is_empty()).unwrap_or(true)
}

// drops all dead pcbs from the list and frees their page tables
pub fn collect_dead_processes() {
    let mut list = PROCESS_LIST.lock();
    list.retain(|p| {
        if p.state == ProcessState::Dead {
            let mapper = VMM::get_kernel_mapper();
            unsafe { mapper.free_user_pml4(p.cr3 as *mut PageTable); }
            false
        } else {
            true
        }
    });
}

// closes all fds, drains threads, marks dead, and collects in one pass
pub fn destroy_process(pid: u64) {
    let thread_ids = {
        let mut list = PROCESS_LIST.lock();
        let pcb = match list.iter_mut().find(|p| p.pid == pid) {
            Some(p) => p,
            None    => return,
        };
        for fd in pcb.fd_table.drain(..) {
            fd.close();
        }
        pcb.state = ProcessState::Dead;
        core::mem::take(&mut pcb.threads)
    };

    for tid in thread_ids {
        destroy_thread(tid);
    }

    collect_dead_processes();
}