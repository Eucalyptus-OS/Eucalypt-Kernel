use crate::event::InputEvent;
use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicUsize, Ordering};

const RING_SIZE: usize = 256;
const RING_MASK: usize = RING_SIZE - 1;

pub struct EventQueue {
    buf: UnsafeCell<[InputEvent; RING_SIZE]>,
    write: AtomicUsize,
    read: AtomicUsize,
}

unsafe impl Sync for EventQueue {}

impl EventQueue {
    pub const fn new() -> Self {
        Self {
            buf: UnsafeCell::new([InputEvent::zeroed(); RING_SIZE]),
            write: AtomicUsize::new(0),
            read: AtomicUsize::new(0),
        }
    }

    /// Push an event.
    pub fn push(&self, event: InputEvent) -> bool {
        let w = self.write.load(Ordering::Relaxed);
        let next_w = (w + 1) & RING_MASK;

        if next_w == self.read.load(Ordering::Acquire) {
            return false;
        }

        unsafe {
            (*self.buf.get())[w] = event;
        }

        self.write.store(next_w, Ordering::Release);
        true
    }

    /// Pop the oldest event.
    pub fn pop(&self) -> Option<InputEvent> {
        let r = self.read.load(Ordering::Relaxed);

        if r == self.write.load(Ordering::Acquire) {
            return None;
        }

        let event = unsafe { (*self.buf.get())[r] };

        self.read.store((r + 1) & RING_MASK, Ordering::Release);
        Some(event)
    }

    /// Returns `true` when there are no pending events.
    pub fn is_empty(&self) -> bool {
        self.read.load(Ordering::Acquire) == self.write.load(Ordering::Acquire)
    }
}
