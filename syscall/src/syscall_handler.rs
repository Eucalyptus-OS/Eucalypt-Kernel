//! Object based syscall dispatch and handler implementations.

use limine::request::FramebufferRequest;
use framebuffer::{print, println};
use vfs::{vfs_close, vfs_open, vfs_read, vfs_write_node, STDIN_NODE_ID, STDOUT_NODE_ID, STDERR_NODE_ID};
use memory::allocator::sbrk;
use process;

unsafe extern "C" {
    static FRAMEBUFFER_REQUEST: FramebufferRequest;
}

const ENOSYS: i64 = -38;
const EBADF:  i64 = -9;
const EINVAL: i64 = -22;

#[repr(u64)]
pub enum Syscall {
    PlotPoint       = 0,
    FramebufferInfo = 1,
    Print           = 2,
    Open            = 3,
    Close           = 4,
    Sbrk            = 5,
    GetPid          = 6,
    Read            = 7,
    Write           = 8,
}

impl Syscall {
    pub fn from_u64(n: u64) -> Option<Self> {
        match n {
            0 => Some(Self::PlotPoint),
            1 => Some(Self::FramebufferInfo),
            2 => Some(Self::Print),
            3 => Some(Self::Open),
            4 => Some(Self::Close),
            5 => Some(Self::Sbrk),
            6 => Some(Self::GetPid),
            7 => Some(Self::Read),
            8 => Some(Self::Write),
            _ => None,
        }
    }
}

pub struct SyscallHandler;

impl SyscallHandler {
    pub fn new() -> Self {
        Self
    }

    pub fn handle(&self, syscall_number: u64, arg1: i64, arg2: i64, arg3: i64) -> i64 {
        match Syscall::from_u64(syscall_number) {
            Some(Syscall::PlotPoint)       => self.plot_point(arg1, arg2, arg3),
            Some(Syscall::FramebufferInfo) => self.framebuffer_info(arg1),
            Some(Syscall::Print)           => self.print(arg1, arg2),
            Some(Syscall::Open)            => self.open(arg1, arg2, arg3),
            Some(Syscall::Close)           => self.close(arg1),
            Some(Syscall::Sbrk)            => self.sbrk(arg1),
            Some(Syscall::GetPid)          => self.get_pid(),
            Some(Syscall::Read)            => self.read(arg1, arg2, arg3),
            Some(Syscall::Write)           => self.write(arg1, arg2, arg3),
            None => ENOSYS,
        }
    }

    fn get_framebuffer(&'_ self) -> Option<limine::framebuffer::Framebuffer<'_>> {
        unsafe { FRAMEBUFFER_REQUEST.get_response() }?
            .framebuffers()
            .next()
    }

    fn plot_point(&self, x: i64, y: i64, color: i64) -> i64 {
        if let Some(fb) = self.get_framebuffer() {
            if x < 0 || y < 0
                || x >= fb.width() as i64
                || y >= fb.height() as i64
            {
                return -1;
            }
            let pitch  = fb.pitch() as i64;
            let offset = (y * pitch + x * 4) as usize;
            unsafe {
                fb.addr().add(offset).cast::<u32>().write(color as u32);
            }
        }
        0
    }

    fn framebuffer_info(&self, query: i64) -> i64 {
        if let Some(fb) = self.get_framebuffer() {
            match query {
                0 => fb.width() as i64,
                1 => fb.height() as i64,
                2 => fb.pitch() as i64,
                3 => fb.bpp() as i64,
                _ => 0,
            }
        } else {
            0
        }
    }

    fn print(&self, ptr: i64, len: i64) -> i64 {
        let ptr = ptr as *const u8;
        let len = len as usize;
        if !ptr.is_null() && len > 0 {
            let slice = unsafe { core::slice::from_raw_parts(ptr, len) };
            if let Ok(s) = core::str::from_utf8(slice) {
                println!("{}", s);
            }
        }
        0
    }

    /// open(path_ptr, path_len, flags) -> fd | -errno
    ///
    /// Opens the file at `path` with the given `flags`, registers the backing
    /// VFS node in the current process's FD table, and returns the FD index.
    fn open(&self, ptr: i64, len: i64, flags: i64) -> i64 {
        let ptr = ptr as *const u8;
        let len = len as usize;
        if ptr.is_null() || len == 0 || len > 4096 {
            return EINVAL;
        }
        let slice = unsafe { core::slice::from_raw_parts(ptr, len) };
        let path = match core::str::from_utf8(slice) {
            Ok(s)  => s,
            Err(_) => return EINVAL,
        };
        let flags_u32 = flags as u32;
        let node = match vfs_open(path, flags_u32, 0) {
            Ok(n)  => n,
            Err(_) => return -1,
        };
        let node_id = node.id;
        let proc = match process::get_current_process_mut() {
            Some(p) => p,
            None => {
                let _ = vfs_close(node_id);
                return -1;
            }
        };
        match proc.open_fd(node_id, flags_u32) {
            Some(fd_idx) => fd_idx as i64,
            None => {
                let _ = vfs_close(node_id);
                -1
            }
        }
    }

    /// close(fd) -> 0 | -errno
    fn close(&self, fd: i64) -> i64 {
        if fd < 0 || fd > 1023 {
            return EBADF;
        }
        let proc = match process::get_current_process_mut() {
            Some(p) => p,
            None    => return -1,
        };
        if proc.close_fd(fd as usize) { 0 } else { EBADF }
    }

    /// read(fd, buf, count) -> bytes_read | -errno
    ///
    /// Reads up to `count` bytes from `fd` into `buf`.
    /// Returns the number of bytes placed in `buf`, or a negative errno.
    /// Reading from stdin (FD 0) always returns 0 (no data yet).
    fn read(&self, fd: i64, buf: i64, count: i64) -> i64 {
        if fd < 0 || fd > 1023 {
            return EBADF;
        }
        if buf == 0 || count < 0 {
            return EINVAL;
        }
        if count == 0 {
            return 0;
        }
        let proc = match process::get_current_process() {
            Some(p) => p,
            None    => return -1,
        };
        let entry = &proc.fildes[fd as usize];
        if entry.is_empty() {
            return EBADF;
        }
        // stdin has no backing data yet
        if entry.node_id == STDIN_NODE_ID {
            return 0;
        }
        let data = match vfs_read(entry.node_id, count as usize) {
            Ok(d)  => d,
            Err(_) => return -1,
        };
        let n = data.len();
        if n > 0 {
            unsafe {
                core::ptr::copy_nonoverlapping(data.as_ptr(), buf as *mut u8, n);
            }
        }
        n as i64
    }

    /// write(fd, buf, count) -> bytes_written | -errno
    ///
    /// Writes `count` bytes from `buf` to `fd`.
    /// Writes to stdout (FD 1) and stderr (FD 2) are routed to the framebuffer.
    fn write(&self, fd: i64, buf: i64, count: i64) -> i64 {
        if fd < 0 || fd > 1023 {
            return EBADF;
        }
        if buf == 0 || count < 0 {
            return EINVAL;
        }
        if count == 0 {
            return 0;
        }
        let proc = match process::get_current_process() {
            Some(p) => p,
            None    => return -1,
        };
        let entry = &proc.fildes[fd as usize];
        if entry.is_empty() {
            return EBADF;
        }
        let node_id = entry.node_id;
        let data = unsafe { core::slice::from_raw_parts(buf as *const u8, count as usize) };
        // stdout / stderr → framebuffer
        if node_id == STDOUT_NODE_ID || node_id == STDERR_NODE_ID {
            if let Ok(s) = core::str::from_utf8(data) {
                print!("{}", s);
            }
            return count;
        }
        match vfs_write_node(node_id, data) {
            Ok(())  => count,
            Err(_)  => -1,
        }
    }
    fn sbrk(&self, increment: i64) -> i64 {
        let ptr = sbrk(increment as isize);
        if ptr.is_null() {
            -1
        } else {
            ptr as i64
        }
    }

    fn get_pid(&self) -> i64 {
        process::get_current_process()
            .map(|proc| proc.pid as i64)
            .unwrap_or(-1) 
    }
}

pub extern "C" fn syscall_handler(syscall_number: u64, arg1: i64, arg2: i64, arg3: i64) -> i64 {
    SyscallHandler::new().handle(syscall_number, arg1, arg2, arg3)
}