use framebuffer::println;
use limine::request::FramebufferRequest;
use memory::{
    addr::{PhysAddr, VirtAddr},
    frame_allocator::FrameAllocator,
    paging::{PageTable, PageTableEntry},
    vmm::VMM,
};
use process::{
    proc::{ProcessState, destroy_process, with_process, with_process_mut},
    scheduler::get_current_pid,
    thread::{self, TCB},
};
use vfs::{errno_from_vfs, fd_close, fd_open, fd_read, fd_write};

unsafe extern "C" {
    static FRAMEBUFFER_REQUEST: FramebufferRequest;
}

const ENOMEM:       i64 = -12;
const ENOSYS:       i64 = -38;
const EINVAL:       i64 = -22;
const EFAULT:       i64 = -14;
const EBADF:        i64 = -9;
const USER_FB_VA:   u64 = 0x0000_7000_0000_0000;
const PAGE_SIZE:    u64 = 4096;

#[repr(u64)]
pub enum Syscall {
    PlotPoint       = 0,
    GetFramebuffer  = 1,
    FramebufferInfo = 2,
    Print           = 3,
    TtyWrite        = 4,
    ProcCreate      = 5,
    ProcDestroy     = 6,
    ThreadCreate    = 7,
    Sbrk            = 8,
    ReadEvent       = 9,
    Open            = 10,
    Close           = 11,
    Read            = 12,
    Write           = 13,
}

impl Syscall {
    // map a raw u64 syscall number to the enum variant
    pub fn from_u64(n: u64) -> Option<Self> {
        match n {
            0  => Some(Self::PlotPoint),
            1  => Some(Self::GetFramebuffer),
            2  => Some(Self::FramebufferInfo),
            3  => Some(Self::Print),
            4  => Some(Self::TtyWrite),
            5  => Some(Self::ProcCreate),
            6  => Some(Self::ProcDestroy),
            7  => Some(Self::ThreadCreate),
            8  => Some(Self::Sbrk),
            9  => Some(Self::ReadEvent),
            10 => Some(Self::Open),
            11 => Some(Self::Close),
            12 => Some(Self::Read),
            13 => Some(Self::Write),
            _  => None,
        }
    }
}

pub struct SyscallHandler;

impl SyscallHandler {
    pub fn new() -> Self { Self }

    // dispatch a syscall number and three arguments to the correct handler
    pub fn handle(&self, syscall_number: u64, arg1: i64, arg2: i64, arg3: i64) -> i64 {
        match Syscall::from_u64(syscall_number) {
            Some(Syscall::PlotPoint)       => self.plot_point(arg1, arg2, arg3),
            Some(Syscall::GetFramebuffer)  => self.handle_get_framebuffer(),
            Some(Syscall::FramebufferInfo) => self.framebuffer_info(arg1),
            Some(Syscall::Print)           => self.print(arg1, arg2),
            Some(Syscall::TtyWrite)        => self.tty_write(arg1, arg2),
            Some(Syscall::ProcCreate) => {
                let stack_size = arg3 as u64;
                let fd     = arg2 as u32;
                let parent = if arg3 == 0 { None } else { Some(arg2 as u64) };
                self.create_proc(stack_size, fd, parent)
            }
            Some(Syscall::ProcDestroy) => {
                let pid = arg1 as u64;
                match with_process(pid, |p| p.state) {
                    None | Some(ProcessState::Dead) | Some(ProcessState::Zombie) => EINVAL,
                    _ => { destroy_process(pid); 0 }
                }
            }
            Some(Syscall::ThreadCreate) => self.thread_create(arg1 as u64, arg2 as u64),
            Some(Syscall::Sbrk)      => self.sbrk(arg1),
            Some(Syscall::ReadEvent) => self.read_event(arg1),
            Some(Syscall::Open)      => self.open(arg1, arg2, arg3),
            Some(Syscall::Close)     => self.close(arg1),
            Some(Syscall::Read)      => self.read(arg1, arg2, arg3),
            Some(Syscall::Write)     => self.write(arg1, arg2, arg3),
            None => ENOSYS,
        }
    }

    // get the first limine framebuffer
    fn get_framebuffer(&self) -> Option<&'static limine::framebuffer::Framebuffer> {
        unsafe { FRAMEBUFFER_REQUEST.response() }?.framebuffers().first().copied()
    }

    // map the framebuffer into userspace and return its virtual address
    fn handle_get_framebuffer(&self) -> i64 {
        let fb = match self.get_framebuffer() { Some(fb) => fb, None => return EFAULT };
        let fb_phys    = fb.address() as u64 - 0xFFFF800000000000;
        let fb_size    = fb.pitch as usize * fb.height as usize;
        let page_count = (fb_size + PAGE_SIZE as usize - 1) / PAGE_SIZE as usize;
        let mapper     = VMM::get_kernel_mapper();
        let pml4       = memory::vmm::Mapper::get_current_page_table();
        let flags      = PageTableEntry::PRESENT | PageTableEntry::WRITABLE | PageTableEntry::USER;
        for i in 0..page_count {
            let offset    = (i as u64) * PAGE_SIZE;
            let page_phys = PhysAddr::new(fb_phys + offset);
            let page_virt = VirtAddr::new(USER_FB_VA + offset);
            if mapper.map_page(pml4, page_virt, page_phys, flags).is_none() {
                return EFAULT;
            }
        }
        USER_FB_VA as i64
    }

    // write a single pixel directly to the framebuffer
    fn plot_point(&self, x: i64, y: i64, color: i64) -> i64 {
        let fb = match self.get_framebuffer() { Some(fb) => fb, None => return EFAULT };
        if x < 0 || y < 0 || x >= fb.width as i64 || y >= fb.height as i64 { return EINVAL; }
        let bpp    = fb.bpp as usize / 8;
        let offset = y as usize * fb.pitch as usize + x as usize * bpp;
        unsafe { (fb.address() as *mut u8).add(offset).cast::<u32>().write_volatile(color as u32); }
        0
    }

    // return a single framebuffer dimension or property by index
    fn framebuffer_info(&self, query: i64) -> i64 {
        let fb = match self.get_framebuffer() { Some(fb) => fb, None => return EFAULT };
        match query {
            0 => fb.width  as i64,
            1 => fb.height as i64,
            2 => fb.pitch  as i64,
            3 => fb.bpp    as i64,
            _ => EINVAL,
        }
    }

    // print a utf-8 string to the kernel framebuffer console
    fn print(&self, ptr: i64, len: i64) -> i64 {
        if ptr == 0 || len <= 0 || len > 65536 { return EINVAL; }
        let slice = unsafe { core::slice::from_raw_parts(ptr as *const u8, len as usize) };
        match core::str::from_utf8(slice) {
            Ok(s)  => { println!("{}", s); 0 }
            Err(_) => EINVAL,
        }
    }

    // write raw bytes to the kernel tty
    fn tty_write(&self, ptr: i64, len: i64) -> i64 {
        if ptr == 0 || len <= 0 || len > 65536 { return EINVAL; }
        let slice = unsafe { core::slice::from_raw_parts(ptr as *const u8, len as usize) };
        tty::tty_write(slice);
        len
    }

    // load and launch an elf from a VfsNode passed by userland pointer
    fn create_proc(&self, stack_size: u64, fd: u32, parent: Option<u64>) -> i64 {
        match process::proc::new_process(parent) {
            Some(pid) => {
                let (entry, cr3) = match elf::load_elf(fd) {
                    Some((e, cr3)) => (e, cr3),
                    None           => return ENOMEM,
                };
                let _tid = thread::TCB::create_thread(stack_size, entry, pid, cr3);
                pid as i64
            }
            None => ENOMEM,
        }
    }

    // Creates a new thread with the given function pointer
    fn thread_create(&self, stack_size: u64, entry: u64) -> i64 {
        if entry == 0 {
            return EINVAL;
        }
        let pid = match with_process(get_current_pid(), |p| p.pid) {
            Some(pid) => pid,
            None => return EINVAL,
        };
        let cr3 = match with_process(pid, |p| p.cr3) {
            Some(cr3) => cr3,
            None => return EINVAL,
        };
        let tid = match TCB::create_thread(stack_size, entry, pid, cr3) {
            Ok(tid) => tid,
            Err(_) => return ENOMEM,
        };
        tid as i64
    }

    // grow or shrink the heap of the current process and return the old break
    fn sbrk(&self, increment: i64) -> i64 {
        let pid = get_current_pid();
        if pid == 0 { return EINVAL; }
        with_process_mut(pid, |pcb| {
            let old_brk = pcb.heap_end;
            let new_brk = match (old_brk as i64).checked_add(increment) {
                Some(b) if b >= pcb.heap_start as i64 => b as u64,
                _ => return ENOMEM,
            };
            if new_brk > old_brk {
                let mapper     = VMM::get_kernel_mapper();
                let pml4       = pcb.cr3 as *mut PageTable;
                let flags      = PageTableEntry::PRESENT | PageTableEntry::WRITABLE | PageTableEntry::USER;
                let first_page = (old_brk + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
                let last_page  = (new_brk + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
                let mut virt   = first_page;
                while virt < last_page {
                    let phys = match FrameAllocator::alloc_frame() {
                        Some(p) => p,
                        None    => return ENOMEM,
                    };
                    if mapper.map_page(pml4, VirtAddr::new(virt), phys, flags).is_none() {
                        FrameAllocator::free_frame(phys);
                        return ENOMEM;
                    }
                    virt += PAGE_SIZE;
                }
            }
            pcb.heap_end = new_brk;
            old_brk as i64
        }).unwrap_or(EINVAL)
    }

    // pop one input event into the userland pointer, returns 1 if an event was available
    fn read_event(&self, ptr: i64) -> i64 {
        if ptr == 0 { return EFAULT; }
        match devices::EVENT_QUEUE.pop() {
            Some(ev) => { unsafe { core::ptr::write(ptr as *mut devices::InputEvent, ev); } 1 }
            None     => 0,
        }
    }

    // open a drive-letter path from userland and return a file descriptor
    fn open(&self, path: i64, flags: i64, mode: i64) -> i64 {
        if path == 0 { return EFAULT; }
        let path = unsafe { core::ffi::CStr::from_ptr(path as *const i8) }.to_string_lossy();
        match fd_open(path.as_ref(), flags as u32, mode as u32) {
            Ok(fd) => fd as i64,
            Err(e) => errno_from_vfs(e),
        }
    }

    // close a file descriptor
    fn close(&self, fd: i64) -> i64 {
        if fd < 3 { return EINVAL; }
        match fd_close(fd as u32) {
            Ok(())  => 0,
            Err(e)  => errno_from_vfs(e),
        }
    }

    // read count bytes from fd into the userland buffer, fd 0 returns 0 (stdin stub)
    fn read(&self, fd: i64, ptr: i64, count: i64) -> i64 {
        if ptr == 0   { return EFAULT; }
        if count <= 0 { return EINVAL; }
        if fd == 0    { return 0; }
        if fd < 0     { return EBADF; }
        let buf = unsafe { core::slice::from_raw_parts_mut(ptr as *mut u8, count as usize) };
        match fd_read(fd as u32, buf) {
            Ok(n)  => n as i64,
            Err(e) => errno_from_vfs(e),
        }
    }

    // write count bytes from the userland buffer into fd, fd 1/2 go to the tty
    fn write(&self, fd: i64, ptr: i64, count: i64) -> i64 {
        if ptr == 0   { return EFAULT; }
        if count <= 0 { return EINVAL; }
        let buf = unsafe { core::slice::from_raw_parts(ptr as *const u8, count as usize) };
        if fd == 1 || fd == 2 { tty::tty_write(buf); return count; }
        if fd < 0 { return EBADF; }
        match fd_write(fd as u32, buf) {
            Ok(n)  => n as i64,
            Err(e) => errno_from_vfs(e),
        }
    }
}

// entry point called from the interrupt handler with four arguments
pub extern "C" fn syscall_handler(syscall_number: u64, arg1: i64, arg2: i64, arg3: i64) -> i64 {
    SyscallHandler::new().handle(syscall_number, arg1, arg2, arg3)
}