#include <stdint.h>
#include <syscalls/syscall_pid.h>
#include <logging/printk.h>
#include <syscalls/syscall_io.h>
#include <syscalls/syscall_fs.h>
#include <syscalls/syscall_memory.h>
#include <syscalls/syscall_framebuffer.h>
#include <syscalls/syscall_input.h>
#include <syscalls/syscall_thread.h>
#include <syscalls/syscall.h>

static const syscall_fn_t syscall_table[NR_SYSCALLS] = {
    [SYS_READ]           = sys_read,
    [SYS_WRITE]          = sys_write,
    [SYS_OPEN]           = sys_open,
    [SYS_CLOSE]          = sys_close,
    [SYS_SEEK]           = sys_seek,
    [SYS_STAT]           = sys_stat,
    [SYS_MKDIR]          = sys_mkdir,
    [SYS_RMDIR]          = sys_rmdir,
    [SYS_UNLINK]         = sys_unlink,
    [SYS_READDIR]        = sys_readdir,
    [SYS_DUP]            = sys_dup,
    [SYS_DUP2]           = sys_dup2,
    [SYS_TELL]           = sys_tell,
    [SYS_FSTAT]          = sys_fstat,
    [SYS_GETPID]         = sys_getpid,
    [SYS_FORK]           = sys_fork,
    [SYS_EXEC]           = sys_exec,
    [SYS_EXIT]           = sys_exit,
    [SYS_WAITPID]        = sys_waitpid,
    [SYS_MMAP]           = sys_mmap,
    [SYS_MUNMAP]         = sys_munmap,
    [SYS_GETPPID]        = sys_getppid,
    [SYS_EXIT_GROUP]     = sys_exit_group,
    [SYS_KILL]           = sys_kill,
    [SYS_SIGNAL]         = sys_signal,
    [SYS_SIGDEFAULT]     = sys_sigdefault,
    [SYS_GETPGID]        = sys_getpgid,
    [SYS_SETPGID]        = sys_setpgid,
    [SYS_GETSID]         = sys_getsid,
    [SYS_SETSID]         = sys_setsid,
    [SYS_CHDIR]          = sys_chdir,
    [SYS_GETCWD]         = sys_getcwd,
    [SYS_RENAME]         = sys_rename,
    [SYS_CHMOD]          = sys_chmod,
    [SYS_FCHMOD]         = sys_fchmod,
    [FRAMEBUFFER_PX]     = sys_fb_plot_pixel,
    [FRAMEBUFFER_RECT]   = sys_fb_draw_rect,
    [FRAMEBUFFER_CIRCLE] = sys_fb_draw_circle,
    [INPUT_SUBSCRIBE]    = syscall_input_subscribe,
    [INPUT_UNSUBSCRIBE]  = syscall_input_unsubscribe,
    [INPUT_READ]         = syscall_input_read,
    [THREAD_CREATE]      = sys_create_thread,
    [THREAD_REMOVE]      = sys_thread_exit,
};

uint64_t do_syscall(uint64_t syscall_num, uint64_t arg0, uint64_t arg1, uint64_t arg2,
                    uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    if (syscall_num >= NR_SYSCALLS || !syscall_table[syscall_num]) {
        log_debug("Unknown syscall: %llu\n", (unsigned long long)syscall_num);
        return (uint64_t)-1;
    }
    return syscall_table[syscall_num](arg0, arg1, arg2, arg3, arg4, arg5);
}
