use core::sync::atomic::{AtomicU64, Ordering};
use alloc::vec::Vec;
use vfs::{FD, D_STDIN, D_STDOUT, D_STDERR};
use memory::vmm::VMM;
use crate::thread::ThreadId;

static NEXT_PID: AtomicU64 = AtomicU64::new(1);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProcessState {
    Running,
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

impl PCB {
    /// Allocates a new process with its own page table, a fresh fd table, and a unique PID.
    pub fn new(parent: Option<u64>) -> Option<Self> {
        let pid = NEXT_PID.fetch_add(1, Ordering::Relaxed);
        let mapper = VMM::get_mapper();
        let pml4 = mapper.create_user_pml4()?;
        let cr3 = pml4 as u64;

        let mut fd_table = Vec::new();
        fd_table.push(FD::new(0, D_STDIN));
        fd_table.push(FD::new(1, D_STDOUT));
        fd_table.push(FD::new(2, D_STDERR));

        Some(PCB {
            pid,
            cr3,
            fd_table,
            threads: Vec::new(),
            state: ProcessState::Running,
            parent,
        })
    }

    /// Returns true if this process has no remaining live threads.
    pub fn is_threadless(&self) -> bool {
        self.threads.is_empty()
    }

    /// Adds a thread to this process.
    pub fn add_thread(&mut self, tid: ThreadId) {
        self.threads.push(tid);
    }

    /// Removes a thread from this process, transitioning to Zombie if none remain.
    pub fn remove_thread(&mut self, tid: ThreadId) {
        self.threads.retain(|&t| t != tid);
        if self.threads.is_empty() {
            self.state = ProcessState::Zombie;
        }
    }
}