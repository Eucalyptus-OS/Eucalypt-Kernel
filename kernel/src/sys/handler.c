#include <stdint.h>
#include <logging/print.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <fs/pipe.h>
#include <multitasking/sched.h>
#include <multitasking/proc.h>
#include <multitasking/thread.h>
#include <signal.h>
#include <mm/mmap.h>
#include <mm/memory.h>
#include <mm/heap.h>
#include <apic.h>
#include <tty.h>

#include <abi/syscalls.h>
#include <abi/errno.h>
#include <abi/types.h>
#include <abi/time.h>
#include <abi/fcntl.h>

extern void _exit(uint64_t exit_code);
extern int fork();
extern int execve(const char *path, char *const argv[], char *const envp[]);
extern int pause();
extern int dup(int fd);
extern int setpgid(int pid, int pgid);
extern int getpgid(int pid);
extern int getpgrp(void);

#define UNDEFINED_SYSCALL 10000000
#define MAX_FCNTL_FDS 256

#define IA32_FS_BASE 0xC0000100

static int fd_flags[MAX_FCNTL_FDS];
static int fd_status_flags[MAX_FCNTL_FDS];

static uint64_t ret_errno(long ret) {
    if (ret < 0) {
        int e = errno;
        if (e > 0) {
            return (uint64_t)(-(int64_t)e);
        }
    }
    return (uint64_t)ret;
}

static uint64_t rdtsc() {
    uint32_t lo, hi;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#define PIPE_END_READ  0
#define PIPE_END_WRITE 1

static ssize_t sys_pipe_vfs_read(vfs_node_t *node, void *buf, size_t count, off_t offset) {
    (void)offset;
    pipe_t *p = (pipe_t *)node->priv;
    return pipe_read(p, buf, count);
}

static ssize_t sys_pipe_vfs_write(vfs_node_t *node, const void *buf, size_t count, off_t offset) {
    (void)offset;
    pipe_t *p = (pipe_t *)node->priv;
    return pipe_write(p, buf, count);
}

static vfs_node_ops_t pipe_read_ops = {
    .read    = sys_pipe_vfs_read,
    .write   = NULL,
    .readdir = NULL,
    .lookup  = NULL,
    .create  = NULL,
    .unlink  = NULL,
    .rmdir   = NULL,
    .rename  = NULL,
    .truncate = NULL,
    .symlink = NULL,
    .readlink = NULL,
};

static vfs_node_ops_t pipe_write_ops = {
    .read    = NULL,
    .write   = sys_pipe_vfs_write,
    .readdir = NULL,
    .lookup  = NULL,
    .create  = NULL,
    .unlink  = NULL,
    .rmdir   = NULL,
    .rename  = NULL,
    .truncate = NULL,
    .symlink = NULL,
    .readlink = NULL,
};

static uint64_t sys_clock_gettime(int clk, struct timespec *ts) {
    if (!ts) return (uint64_t)-EFAULT;
    if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC) return (uint64_t)-EINVAL;
    uint64_t tsc = rdtsc();
    ts->tv_sec = (int64_t)(tsc / 1000000000ULL);
    ts->tv_nsec = (int64_t)(tsc % 1000000000ULL);
    return 0;
}

static uint64_t sys_uname(struct utsname *u) {
    if (!u) return (uint64_t)-EFAULT;
    strcpy(u->sysname, "TinyKern");
    strcpy(u->nodename, "TinyKern");
    strcpy(u->release, "0.0.1");
    strcpy(u->version, "TinyKern");
    strcpy(u->machine, "x86_64");
    strcpy(u->domainname, "");
    return 0;
}

static uint64_t sys_fcntl(int fd, int cmd, uint64_t arg) {
    if (fd < 0 || fd >= MAX_FCNTL_FDS) return (uint64_t)-EBADF;

    switch (cmd) {
        case F_DUPFD: {
            int newfd = dup(fd);
            if (newfd < 0) return ret_errno(newfd);
            return (uint64_t)newfd;
        }

        case F_GETFD:
            return (uint64_t)fd_flags[fd];

        case F_SETFD:
            fd_flags[fd] = (int)arg;
            return 0;

        case F_GETFL:
            return (uint64_t)fd_status_flags[fd];

        case F_SETFL:
            fd_status_flags[fd] = (int)arg;
            return 0;

        default:
            return (uint64_t)-EINVAL;
    }
}

uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                         uint64_t arg4, uint64_t arg5, user_context_t *ctx) {
    uint64_t result = UNDEFINED_SYSCALL;

    {
        struct pcb *tp = sched_current_proc();
        if (tp && tp->pid == 2) {
            print("STRACE pid2 syscall=%lu\n", num);
            if (num == SYS_KILL) {
                print("STRACE pid2 ctx: rax=%#lx rbx=%#lx rcx=%#lx rdx=%#lx\n",
                      ctx->rax, ctx->rbx, ctx->rcx, ctx->rdx);
                print("STRACE pid2 ctx: rsi=%#lx rdi=%#lx rbp=%#lx rsp=%#lx\n",
                      ctx->rsi, ctx->rdi, ctx->rbp, ctx->rsp);
                print("STRACE pid2 ctx: r8=%#lx r9=%#lx r10=%#lx r11=%#lx\n",
                      ctx->r8, ctx->r9, ctx->r10, ctx->r11);
                print("STRACE pid2 ctx: r12=%#lx r13=%#lx r14=%#lx r15=%#lx rip=%#lx\n",
                      ctx->r12, ctx->r13, ctx->r14, ctx->r15, ctx->rip);
                print("STRACE pid2 ctx: rflags=%#lx cs=%#lx ss=%#lx\n",
                      ctx->rflags, ctx->cs, ctx->ss);
                uint64_t f = current_tcb->fs_base;
                print("STRACE pid2 fs_base=%#lx tls_curloc=%#lx glob_locale=%#lx\n",
                      f,
                      *(uint64_t *)(f - 0x118),
                      *(uint64_t *)0x4e5e40);
                print("STRACE pid2 tcb self=%#lx dtvSize=%#lx dtv=%#lx tid=%#lx canary=%#lx\n",
                      *(uint64_t *)(f + 0x0),
                      *(uint64_t *)(f + 0x8),
                      *(uint64_t *)(f + 0x10),
                      *(uint64_t *)(f + 0x18),
                      *(uint64_t *)(f + 0x28));
                print("STRACE pid2 tlsblk: -0x130=%#lx -0x120=%#lx -0x110=%#lx -0x100=%#lx\n",
                      *(uint64_t *)(f - 0x130),
                      *(uint64_t *)(f - 0x120),
                      *(uint64_t *)(f - 0x110),
                      *(uint64_t *)(f - 0x100));
                print("STRACE pid2 dotdata: [0]=%#lx [8]=%#lx [0x10]=%#lx [0x18]=%#lx\n",
                      *(uint64_t *)0x4e5000,
                      *(uint64_t *)0x4e5008,
                      *(uint64_t *)0x4e5010,
                      *(uint64_t *)0x4e5018);
                uint64_t *lp = (uint64_t *)(*(uint64_t *)0x4e5e40);
                if ((uint64_t)lp < 0x700000000000ULL) {
                    print("STRACE pid2 locate_numeric: +870=%#lx +888=%#lx +8a0=%#lx\n",
                          *(uint64_t *)&((uint8_t *)lp)[0x870],
                          *(uint64_t *)&((uint8_t *)lp)[0x888],
                          *(uint64_t *)&((uint8_t *)lp)[0x8a0]);
                }
                uint64_t *sp = (uint64_t *)ctx->rsp;
                for (int i = 0; i < 180; i++) {
                    if (i % 4 == 0)
                        print("\nSTRACE pid2 stack[%d..]:", i);
                    if ((uint64_t)(sp + i) < 0x700000000000ULL)
                        print(" %#lx", sp[i]);
                    else
                        print(" <bad>");
                }
                print("\n");
            }
        }
    }

    switch (num) {
        case SYS_READ:
            result = ret_errno(read((int)arg1, (void *)arg2, (size_t)arg3));
            break;

        case SYS_WRITE:
            result = ret_errno(write((int)arg1, (const void *)arg2, (size_t)arg3));
            break;

        case SYS_OPEN:
            result = ret_errno(open((const char *)arg1, (int)arg2, (uint32_t)arg3));
            break;

        case SYS_CLOSE:
            result = ret_errno(close((int)arg1));
            break;

        case SYS_STAT:
            result = ret_errno(stat((const char *)arg1, (struct stat *)arg2));
            break;

        case SYS_FSTAT:
            result = ret_errno(fstat((int)arg1, (struct stat *)arg2));
            break;

        case SYS_LSTAT:
            result = ret_errno(lstat((const char *)arg1, (struct stat *)arg2));
            break;

        case SYS_FSTATAT:
            result = ret_errno(fstatat((int)arg1, (const char *)arg2, (struct stat *)arg3, (int)arg4));
            break;

        case SYS_LSEEK:
            result = ret_errno(lseek((int)arg1, (off_t)arg2, (int)arg3));
            break;

        case SYS_MMAP:
            result = (uint64_t)sys_mmap((uintptr_t)arg1, (size_t)arg2, (int)arg3,
                                        (int)arg4, (int)arg5, 0);
            break;

        case SYS_MPROTECT:
            result = (uint64_t)sys_mprotect((uintptr_t)arg1, (size_t)arg2, (int)arg3);
            break;

        case SYS_MUNMAP:
            result = (uint64_t)sys_munmap((uintptr_t)arg1, (size_t)arg2);
            break;

        case SYS_BRK: {
            struct pcb *p = sched_current_proc();
            if (!p) {
                result = (uint64_t)-1;
            } else {
                result = (uint64_t)proc_sbrk(p, (intptr_t)arg1);
            }
            break;
        }

        case SYS_SIGACTION:
            result = (uint64_t)sigaction((int)arg1, (const sigaction_t *)arg2, (sigaction_t *)arg3);
            break;

        case SYS_SIGPROCMASK:
            result = (uint64_t)sigprocmask((int)arg1, (const sigset_t *)arg2, (sigset_t *)arg3);
            break;

        case SYS_SIGPENDING:
            result = (uint64_t)sigpending((sigset_t *)arg1);
            break;

        case SYS_SIGSUSPEND:
            result = (uint64_t)sigsuspend((const sigset_t *)arg1);
            break;

        case SYS_SIGRETURN:
            sigreturn(ctx);
            break;

        case SYS_READV:
            result = ret_errno(readv((int)arg1, (const struct iovec *)arg2, (int)arg3));
            break;

        case SYS_WRITEV:
            result = ret_errno(writev((int)arg1, (const struct iovec *)arg2, (int)arg3));
            break;

        case SYS_DUP:
            result = ret_errno(dup((int)arg1));
            break;

        case SYS_DUP2:
            result = ret_errno(dup2((int)arg1, (int)arg2));
            break;

        case SYS_FCNTL:
            result = sys_fcntl((int)arg1, (int)arg2, (uint64_t)arg3);
            break;

        case SYS_GETPID: {
            struct pcb *p = sched_current_proc();
            result = p ? (uint64_t)p->pid : (uint64_t)-1;
            break;
        }

        case SYS_GETPPID:
            result = 0;
            break;

        case SYS_FORK:
            result = (uint64_t)fork();
            break;

        case SYS_EXECVE:
            result = (uint64_t)execve((const char *)arg1, (char *const *)arg2, (char *const *)arg3);
            break;

        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            _exit(arg1);
            break;

        case SYS_KILL:
            result = (uint64_t)kill((int)arg1, (int)arg2);
            break;

        case SYS_SETPGID:
            result = (uint64_t)setpgid((int)arg1, (int)arg2);
            break;

        case SYS_GETPGID:
            result = (uint64_t)getpgid((int)arg1);
            break;

        case SYS_GETPGRP:
            result = (uint64_t)getpgrp();
            break;

        case SYS_UNAME:
            result = sys_uname((struct utsname *)arg1);
            break;

        case SYS_GETCWD: {
            char *r = vfs_getcwd((char *)arg1, (size_t)arg2);
            result = r ? (uint64_t)arg1 : (uint64_t)(-(int64_t)errno);
            break;
        }

        case SYS_CHDIR:
            result = ret_errno(vfs_chdir((const char *)arg1));
            break;

        case SYS_UMASK: {
            struct pcb *p = sched_current_proc();
            uint32_t old = p ? p->umask : 0;
            if (p) {
                p->umask = (uint32_t)arg1 & 0777;
            }
            result = old;
            break;
        }

        case SYS_GETUID:
        case SYS_GETEUID:
            result = 0;
            break;

        case SYS_GETGID:
        case SYS_GETEGID:
            result = 0;
            break;

        case SYS_MOUNT:
            result = (uint64_t)vfs_mount_by_path((const char *)arg1, (const char *)arg2);
            break;

        case SYS_MKFS:
            result = (uint64_t)vfs_mkfs((const char *)arg1);
            break;

        case SYS_GETDENTS:
            result = ret_errno(getdents64((int)arg1, (void *)arg2, (size_t)arg3));
            break;

        case SYS_CLOCK_GETTIME:
            result = sys_clock_gettime((int)arg1, (struct timespec *)arg2);
            break;

        case SYS_ARCH_PRCTL: {
            struct tcb *t = current_tcb;
            int code = (int)arg1;
            if (code == ARCH_SET_FS) {
                t->fs_base = arg2;
                asm volatile ("wrmsr" : : "c"((uint64_t)IA32_FS_BASE),
                              "a"((uint32_t)arg2), "d"((uint32_t)(arg2 >> 32)));
                result = 0;
            } else if (code == ARCH_GET_FS) {
                *(uint64_t *)arg2 = t->fs_base;
                result = 0;
            } else {
                result = (uint64_t)-EINVAL;
            }
            break;
        }
        case SYS_PAUSE: {
            result = pause();
            break;
        }

        case SYS_TCGETATTR: {
            struct termios_user *u = (struct termios_user *)arg1;
            if (!u) {
                result = (uint64_t)-EFAULT;
                break;
            }
            result = (tty_getattr(tty_get_active(), u) == 0) ? 0 : (uint64_t)-EINVAL;
            break;
        }

        case SYS_TCSETATTR: {
            struct termios_user *u = (struct termios_user *)arg1;
            if (!u) {
                result = (uint64_t)-EFAULT;
                break;
            }
            result = (tty_setattr(tty_get_active(), u) == 0) ? 0 : (uint64_t)-EINVAL;
            break;
        }

        case SYS_TTYINFO: {
            struct ttyinfo *ti = (struct ttyinfo *)arg1;
            if (!ti) {
                result = (uint64_t)-EFAULT;
                break;
            }
            result = (tty_getinfo(tty_get_active(), ti) == 0) ? 0 : (uint64_t)-EINVAL;
            break;
        }

        case SYS_WAITPID:
            result = ret_errno((long)waitpid((int)arg1, (int *)arg2, (int)arg3));
            break;

        case SYS_UNLINKAT:
            result = ret_errno(unlink((const char *)arg2));
            break;

        case SYS_PIPE: {
            int *pipefd = (int *)arg1;
            if (!pipefd) { result = (uint64_t)-EFAULT; break; }

            pipe_t *p = pipe_create();
            if (!p) { result = (uint64_t)-ENOMEM; break; }

            vfs_node_t *rnode = vfs_node_alloc("pipe_r", VFS_NODE_PIPE);
            if (!rnode) { pipe_destroy(p); result = (uint64_t)-ENOMEM; break; }
            rnode->ops = &pipe_read_ops;
            rnode->priv = p;

            vfs_node_t *wnode = vfs_node_alloc("pipe_w", VFS_NODE_PIPE);
            if (!wnode) { pipe_destroy(p); kfree(rnode); result = (uint64_t)-ENOMEM; break; }
            wnode->ops = &pipe_write_ops;
            wnode->priv = p;

            struct pcb *cur = sched_current_proc();
            if (!cur) { pipe_destroy(p); kfree(rnode); kfree(wnode); result = (uint64_t)-EINVAL; break; }

            int rfd = -1, wfd = -1;
            for (int i = 0; i < MAX_FDS; i++) {
                if (!cur->fd_table[i]) { rfd = i; break; }
            }
            if (rfd < 0) { pipe_destroy(p); kfree(rnode); kfree(wnode); result = (uint64_t)-EMFILE; break; }

            for (int i = rfd + 1; i < MAX_FDS; i++) {
                if (!cur->fd_table[i]) { wfd = i; break; }
            }
            if (wfd < 0) { pipe_destroy(p); kfree(rnode); kfree(wnode); result = (uint64_t)-EMFILE; break; }

            cur->fd_table[rfd] = vfs_file_create(rnode, O_RDONLY);
            cur->fd_table[rfd]->offset = PIPE_END_READ;
            cur->fd_table[wfd] = vfs_file_create(wnode, O_WRONLY);
            cur->fd_table[wfd]->offset = PIPE_END_WRITE;

            pipefd[0] = rfd;
            pipefd[1] = wfd;
            result = 0;
            break;
        }

        case SYS_IOCTL: {
            int fd = (int)arg1;
            unsigned long req = (unsigned long)arg2;
            void *user_arg = (void *)arg3;
            struct pcb *proc = sched_current_proc();
            if (!proc || fd < 0 || fd >= MAX_FDS || !proc->fd_table[fd]) {
                result = (uint64_t)-EBADF;
                break;
            }
            vfs_node_t *node = proc->fd_table[fd]->node;
            if (!node || node->type != VFS_NODE_DEV || !node->priv) {
                result = (uint64_t)(-ENOTTY);
                break;
            }
            devfs_dev_t *dev = (devfs_dev_t *)node->priv;
            if (dev->ioctl) {
                result = (uint64_t)dev->ioctl(dev, req, user_arg);
            } else {
                result = (uint64_t)(-ENOTTY);
            }
            break;
        }
    }

    if (num != SYS_SIGRETURN) {
        ctx->rax = result;
    }
    sig_deliver_current(ctx, num);

    return result;
}