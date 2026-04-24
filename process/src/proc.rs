use core::sync::atomic::{AtomicU64, Ordering};
use alloc::vec::Vec;
use spin::Mutex;
use vfs::{FD, D_STDIN, D_STDOUT, D_STDERR};
use memory::vmm::VMM;
use crate::thread::ThreadId;

static NEXT_PID: AtomicU64 = AtomicU64::new(1);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProcessState {
    Running,
    Sleeping,
    Zombie,
    Dead,
}

pub struct PCB {
    pub pid:      u64,
    pub cr3:      u64,
    pub fd_table: Vec<FD>,
    pub threads:  Vec<ThreadId>,
    pub state:    ProcessState,
    pub parent:   Option<u64>,
}

/// Global process list; grows dynamically, no fixed cap.
static PROCESS_LIST: Mutex<Vec<PCB>> = Mutex::new(Vec::new());

/// Allocates a new process, registers it in the global list, and returns its PID.
pub fn new_process(parent: Option<u64>) -> Option<u64> {
    let pid = NEXT_PID.fetch_add(1, Ordering::Relaxed);
    let mapper = VMM::get_mapper();
    let pml4 = mapper.create_user_pml4()?;
    let cr3 = pml4 as u64;

    let mut fd_table = Vec::new();
    fd_table.push(FD::new(0, D_STDIN));
    fd_table.push(FD::new(1, D_STDOUT));
    fd_table.push(FD::new(2, D_STDERR));

    let pcb = PCB {
        pid,
        cr3,
        fd_table,
        threads: Vec::new(),
        state: ProcessState::Running,
        parent,
    };

    PROCESS_LIST.lock().push(pcb);
    Some(pid)
}

/// Returns the number of live processes.
pub fn get_process_count() -> usize {
    PROCESS_LIST.lock().len()
}

/// Calls `f` with a mutable reference to the PCB whose PID matches, returning the result, or `None` if not found.
pub fn with_process_mut<R, F: FnOnce(&mut PCB) -> R>(pid: u64, f: F) -> Option<R> {
    let mut list = PROCESS_LIST.lock();
    list.iter_mut().find(|p| p.pid == pid).map(f)
}

/// Calls `f` with a shared reference to the PCB whose PID matches, returning the result, or `None` if not found.
pub fn with_process<R, F: FnOnce(&PCB) -> R>(pid: u64, f: F) -> Option<R> {
    let list = PROCESS_LIST.lock();
    list.iter().find(|p| p.pid == pid).map(f)
}

/// Adds `tid` to the thread list of the process identified by `pid`.
pub fn add_thread_to_process(pid: u64, tid: ThreadId) {
    with_process_mut(pid, |pcb| pcb.threads.push(tid));
}

/// Removes `tid` from the process; transitions the process to Zombie if no threads remain.
pub fn remove_thread_from_process(pid: u64, tid: ThreadId) {
    with_process_mut(pid, |pcb| {
        pcb.threads.retain(|&t| t != tid);
        if pcb.threads.is_empty() {
            pcb.state = ProcessState::Zombie;
        }
    });
}

/// Returns true if the process has no remaining threads.
pub fn is_threadless(pid: u64) -> bool {
    with_process(pid, |pcb| pcb.threads.is_empty()).unwrap_or(true)
}

/// Marks a Zombie process as Dead so its resources can be reclaimed.
pub fn reap_process(pid: u64) {
    with_process_mut(pid, |pcb| {
        if pcb.state == ProcessState::Zombie {
            pcb.state = ProcessState::Dead;
        }
    });
}

/// Removes all Dead processes from the list, freeing their entries.
pub fn collect_dead_processes() {
    PROCESS_LIST.lock().retain(|p| p.state != ProcessState::Dead);
}

/// Reaps the threads along with removing then and then destroys the process.
pub fn destroy_process(pid: u64) {
    reap_process(pid);
    let thread_ids: Vec<ThreadId> = with_process(pid, |pcb| pcb.threads.clone())
        .unwrap_or_default();
    for tid in thread_ids {
        remove_thread_from_process(pid, tid);
    }
}