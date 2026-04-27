use limine::request::FramebufferRequest;
use framebuffer::println;
use memory::{
    addr::{PhysAddr, VirtAddr},
    paging::{PageTable, PageTableEntry},
    vmm::VMM,
    frame_allocator::FrameAllocator,
};
use process::{
    proc::{destroy_process, with_process, with_process_mut, ProcessState},
    scheduler::get_current_pid,
};
use vfs::VfsNode;

unsafe extern "C" {
    static FRAMEBUFFER_REQUEST: FramebufferRequest;
}

const ENOMEM: i64 = -12;
const ENOSYS: i64 = -38;
const EINVAL: i64 = -22;
const EFAULT: i64 = -14;
const USER_FB_VA: u64 = 0x0000_7000_0000_0000;
const PAGE_SIZE:  u64 = 4096;

#[repr(u64)]
pub enum Syscall {
    PlotPoint       = 0,
    GetFramebuffer  = 1,
    FramebufferInfo = 2,
    Print           = 3,
    TtyWrite        = 4,
    ProcCreate      = 5,
    ProcDestroy     = 6,
    Sbrk            = 7,
}

impl Syscall {
    pub fn from_u64(n: u64) -> Option<Self> {
        match n {
            0 => Some(Self::PlotPoint),
            1 => Some(Self::GetFramebuffer),
            2 => Some(Self::FramebufferInfo),
            3 => Some(Self::Print),
            4 => Some(Self::TtyWrite),
            5 => Some(Self::ProcCreate),
            6 => Some(Self::ProcDestroy),
            7 => Some(Self::Sbrk),
            _ => None,
        }
    }
}

pub struct SyscallHandler;

impl SyscallHandler {
    pub fn new() -> Self { Self }

    pub fn handle(&self, syscall_number: u64, arg1: i64, arg2: i64, arg3: i64) -> i64 {
        match Syscall::from_u64(syscall_number) {
            Some(Syscall::PlotPoint)       => self.plot_point(arg1, arg2, arg3),
            Some(Syscall::GetFramebuffer)  => self.handle_get_framebuffer(),
            Some(Syscall::FramebufferInfo) => self.framebuffer_info(arg1),
            Some(Syscall::Print)           => self.print(arg1, arg2),
            Some(Syscall::TtyWrite)        => self.tty_write(arg1, arg2),
            Some(Syscall::ProcCreate)      => {
                if arg1 == 0 { return EFAULT; }
                let node   = unsafe { (arg1 as *const VfsNode).read() };
                let parent = if arg2 == 0 { None } else { Some(arg2 as u64) };
                self.proc_create(node, parent)
            },
            Some(Syscall::ProcDestroy) => {
                let pid = arg1 as u64;
                match with_process(pid, |p| p.state) {
                    None
                    | Some(ProcessState::Dead)
                    | Some(ProcessState::Zombie) => EINVAL,
                    _ => { destroy_process(pid); 0 }
                }
            },
            Some(Syscall::Sbrk) => self.sbrk(arg1),
            None                => ENOSYS,
        }
    }

    fn get_framebuffer(&self) -> Option<&'static limine::framebuffer::Framebuffer> {
        unsafe { FRAMEBUFFER_REQUEST.response() }?
            .framebuffers()
            .first()
            .copied()
    }

    fn handle_get_framebuffer(&self) -> i64 {
        let fb = match self.get_framebuffer() {
            Some(fb) => fb,
            None     => return EFAULT,
        };

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

    fn plot_point(&self, x: i64, y: i64, color: i64) -> i64 {
        let fb = match self.get_framebuffer() {
            Some(fb) => fb,
            None     => return EFAULT,
        };

        if x < 0 || y < 0 || x >= fb.width as i64 || y >= fb.height as i64 {
            return EINVAL;
        }

        let bpp    = fb.bpp as usize / 8;
        let offset = y as usize * fb.pitch as usize + x as usize * bpp;
        unsafe {
            (fb.address() as *mut u8)
                .add(offset)
                .cast::<u32>()
                .write_volatile(color as u32);
        }
        0
    }

    fn framebuffer_info(&self, query: i64) -> i64 {
        let fb = match self.get_framebuffer() {
            Some(fb) => fb,
            None     => return EFAULT,
        };
        match query {
            0 => fb.width  as i64,
            1 => fb.height as i64,
            2 => fb.pitch  as i64,
            3 => fb.bpp    as i64,
            _ => EINVAL,
        }
    }

    fn print(&self, ptr: i64, len: i64) -> i64 {
        if ptr == 0 || len <= 0 || len > 65536 { return EINVAL; }
        let slice = unsafe { core::slice::from_raw_parts(ptr as *const u8, len as usize) };
        match core::str::from_utf8(slice) {
            Ok(s)  => { println!("{}", s); 0 }
            Err(_) => EINVAL,
        }
    }

    fn tty_write(&self, ptr: i64, len: i64) -> i64 {
        if ptr == 0 || len <= 0 || len > 65536 { return EINVAL; }
        let slice = unsafe { core::slice::from_raw_parts(ptr as *const u8, len as usize) };
        tty::tty_write(slice);
        len
    }

    fn proc_create(&self, file: VfsNode, parent: Option<u64>) -> i64 {
        match file.stat() {
            Ok(s) if s.size == 0 => return EINVAL,
            Err(_)               => return EINVAL,
            _                    => {}
        }
        match process::proc::new_process(parent) {
            Some(pid) => pid as i64,
            None      => ENOMEM,
        }
    }

    // grows the calling process's heap by increment bytes, mapping new physical frames with USER bit
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
                let mapper = VMM::get_kernel_mapper();
                let pml4   = pcb.cr3 as *mut PageTable;
                let flags  = PageTableEntry::PRESENT
                           | PageTableEntry::WRITABLE
                           | PageTableEntry::USER;

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
    
    pub fn get_kb_cur_input() -> u8 {
        let kb = ps_2_devices::KEYBOARD.lock();
        kb.get_current_scan_code()
    }
    
    pub fn get_kb_prev_input() -> u8 {
        let kb = ps_2_devices::KEYBOARD.lock();
        kb.get_prev_scan_code()
    }
}

pub extern "C" fn syscall_handler(syscall_number: u64, arg1: i64, arg2: i64, arg3: i64) -> i64 {
    SyscallHandler::new().handle(syscall_number, arg1, arg2, arg3)
}