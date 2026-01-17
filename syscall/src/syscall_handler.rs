use limine::request::FramebufferRequest;

unsafe extern "C" {
    static FRAMEBUFFER_REQUEST: FramebufferRequest;
}

pub const SYSCALL_FRAMEBUFFER_POINTER: u64 = 0;

pub fn syscall_handler(syscall_number: u64, _arg1: u64, _arg2: u64, _arg3: u64) -> u64 {
    match syscall_number {
        SYSCALL_FRAMEBUFFER_POINTER => unsafe {
            let response = match FRAMEBUFFER_REQUEST.get_response() {
                Some(res) => res,
                None => panic!(
                    "Oh? You thought you were getting a framebuffer pointer? How adorable. 
            You weren't. You get nothing. In fact, I think I would like to panic now. 
            Your kernel is forfeit. Panicking in 3... 2... 1..."
                ),
            };
            
            let fb = match response.framebuffers().next() {
                Some(f) => f,
                None => panic!("A response exists, yet it is empty. You've been played. The framebuffer was a lie."),
            };
            
            fb.addr() as u64
        }
        _ => 0xFFFFFFFFFFFFFFFF,
    }
}
