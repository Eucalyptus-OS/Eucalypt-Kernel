#include <stdint.h>
#include <multitasking/sched.h>
#include <multitasking/proc.h>
#include <multitasking/thread.h>
#include <logging/print.h>
#include <signal.h>
#include <lib/errno.h>
#include <abi/syscalls.h>

#define USER_STACK_TOP 0x0000700000000000UL
#define USTACK_SIZE 0x10000
#define USER_STACK_BASE (USER_STACK_TOP - USTACK_SIZE)

#define STOP_BITS ((1ULL << (SIGSTOP - 1)) | (1ULL << (SIGTSTP - 1)) | \
                   (1ULL << (SIGTTIN - 1)) | (1ULL << (SIGTTOU - 1)))
#define CONT_BIT ((sigset_t)1 << (SIGCONT - 1))

static const sig_default_action_t default_actions[NSIG] = {
    [SIGHUP]  = SIG_ACTION_TERMINATE,
    [SIGINT]  = SIG_ACTION_TERMINATE,
    [SIGQUIT] = SIG_ACTION_CORE,
    [SIGILL]  = SIG_ACTION_CORE,
    [SIGTRAP] = SIG_ACTION_CORE,
    [SIGABRT] = SIG_ACTION_CORE,
    [SIGEMT]  = SIG_ACTION_CORE,
    [SIGFPE]  = SIG_ACTION_CORE,
    [SIGKILL] = SIG_ACTION_TERMINATE,
    [SIGBUS]  = SIG_ACTION_CORE,
    [SIGSEGV] = SIG_ACTION_CORE,
    [SIGSYS]  = SIG_ACTION_CORE,
    [SIGPIPE] = SIG_ACTION_TERMINATE,
    [SIGALRM] = SIG_ACTION_TERMINATE,
    [SIGTERM] = SIG_ACTION_TERMINATE,
    [SIGURG]  = SIG_ACTION_IGNORE,
    [SIGSTOP] = SIG_ACTION_STOP,
    [SIGTSTP] = SIG_ACTION_STOP,
    [SIGCONT] = SIG_ACTION_CONTINUE,
    [SIGCHLD] = SIG_ACTION_IGNORE,
    [SIGTTIN] = SIG_ACTION_STOP,
    [SIGTTOU] = SIG_ACTION_STOP,
    [SIGUSR1] = SIG_ACTION_TERMINATE,
    [SIGUSR2] = SIG_ACTION_TERMINATE,
};

void wake_threads(struct pcb *p) {
    struct tcb *t = p->t;
    if (!t) {
        return;
    }
    struct tcb *it = t;
    do {
        if (it->state == Blocked || it->state == Sleeping) {
            it->state = Ready;
        }
        it = it->proc_next;
    } while (it != t);
}

static void sig_default_action(struct pcb *p, int sig) {
    if (!p || sig <= 0 || sig >= NSIG) {
        return;
    }
    struct tcb *t = p->t;
    switch (default_actions[sig]) {
        case SIG_ACTION_TERMINATE:
        case SIG_ACTION_CORE:
            if (t) {
                struct tcb *it = t;
                do {
                    it->state = Exited;
                    it = it->proc_next;
                } while (it != t);
            }
            p->exit_code = 128 + sig;
            if (p == sched_current_proc()) {
                schedule();
                asm volatile ("sti");
                for (;;) asm volatile ("hlt");
            }
            break;
        case SIG_ACTION_STOP:
            if (!p->stopped) {
                p->wait_events |= WAIT_EVT_STOPPED;
                p->wait_stop_sig = sig;
                if (p->ppcb) {
                    wake_threads(p->ppcb);
                }
            }
            p->stopped = 1;
            if (t) {
                struct tcb *it = t;
                do {
                    if (it->state == Ready || it->state == Running) {
                        it->state = Blocked;
                    }
                    it = it->proc_next;
                } while (it != t);
            }
            if (p == sched_current_proc()) {
                schedule();
            }
            break;
        case SIG_ACTION_CONTINUE:
            p->stopped = 0;
            wake_threads(p);
            break;
        case SIG_ACTION_IGNORE:
            break;
    }
}

void default_sig_handler(int sig) {
    struct pcb *p = sched_current_proc();
    if (p) {
        sig_default_action(p, sig);
    }
}

void sig_queue(struct pcb *p, int sig) {
    if (!p || sig <= 0 || sig >= NSIG) {
        return;
    }
    sigset_t bit = (sigset_t)1 << (sig - 1);
    sig_handler_t h = p->sigstate.actions[sig].sa_handler;

    if (sig == SIGCONT) {
        int was_stopped = p->stopped;
        p->stopped = 0;
        p->sigstate.pending &= ~STOP_BITS;
        wake_threads(p);
        if (was_stopped) {
            p->wait_events &= ~WAIT_EVT_STOPPED;
            p->wait_events |= WAIT_EVT_CONTINUED;
            if (p->ppcb) {
                wake_threads(p->ppcb);
            }
        }
        if (h != SIG_DFL && h != SIG_IGN) {
            p->sigstate.pending |= bit;
        }
        return;
    }

    if (h == SIG_IGN) {
        return;
    }

    if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
        p->sigstate.pending &= ~CONT_BIT;
    }

    if (h == SIG_DFL) {
        sig_default_action(p, sig);
        return;
    }

    p->sigstate.pending |= bit;
    if (!p->stopped) {
        wake_threads(p);
    }
}

static void put_sigreturn_tramp(uint8_t *dst) {
    dst[0] = 0xBF;
    dst[1] = SYS_SIGRETURN;
    dst[2] = 0;
    dst[3] = 0;
    dst[4] = 0;
    dst[5] = 0xCD;
    dst[6] = 0x80;
}

void sig_deliver(struct pcb *p, int sig, user_context_t *ctx, uint64_t syscall_num) {
    if (!p || !ctx || sig <= 0 || sig >= NSIG) {
        return;
    }
    sig_handler_t h = p->sigstate.actions[sig].sa_handler;
    p->sigstate.pending &= ~((sigset_t)1 << (sig - 1));

    if (sig == SIGKILL || sig == SIGSTOP) {
        sig_default_action(p, sig);
        return;
    }
    if (h == SIG_IGN) {
        return;
    }
    if (h == SIG_DFL) {
        sig_default_action(p, sig);
        return;
    }

    uintptr_t frame_addr = (ctx->rsp - sizeof(sigframe_t)) & ~0xFULL;
    if (frame_addr < USER_STACK_BASE || frame_addr > USER_STACK_TOP) {
        sig_default_action(p, sig);
        return;
    }

    sigframe_t *fr = (sigframe_t *)frame_addr;
    fr->ctx = *ctx;
    fr->old_mask = p->sigstate.blocked;
    fr->info.si_signo = sig;
    fr->info.si_code = SI_USER;
    fr->info.si_errno = 0;
    fr->info.si_pid = (int)p->pid;
    fr->info.si_addr = ctx->rip;
    fr->sa_flags = p->sigstate.actions[sig].sa_flags;
    fr->saved_syscall = 0;

    if (!(p->sigstate.actions[sig].sa_flags & SA_NODEFER)) {
        p->sigstate.blocked |= (sigset_t)1 << (sig - 1);
    }
    p->sigstate.blocked |= p->sigstate.actions[sig].sa_mask;

    if (p->sigstate.actions[sig].sa_flags & SA_RESETHAND) {
        p->sigstate.actions[sig].sa_handler = SIG_DFL;
    }

    uint8_t *base = (uint8_t *)fr;
    uintptr_t tramp = (uintptr_t)base - 4096;
    if (tramp < USER_STACK_BASE) {
        sig_default_action(p, sig);
        return;
    }
    *(uint64_t *)(base - 8) = tramp;
    put_sigreturn_tramp((uint8_t *)tramp);

    if (fr->sa_flags & SA_RESTART) {
        uint8_t *inst = (uint8_t *)(ctx->rip - 2);
        if ((inst[0] == 0xCD && inst[1] == 0x80) ||
            (inst[0] == 0x0F && inst[1] == 0x05)) {
            fr->ctx.rip -= 2;
            fr->ctx.rax = syscall_num;
        }
    }

    ctx->rsp = (uint64_t)(base - 8);
    ctx->rip = (uint64_t)h;
    ctx->rdi = (uint64_t)sig;
    if (p->sigstate.actions[sig].sa_flags & SA_SIGINFO) {
        ctx->rsi = (uint64_t)&fr->info;
        ctx->rdx = 0;
    }
}

void sig_deliver_current(user_context_t *ctx, uint64_t syscall_num) {
    struct pcb *p = sched_current_proc();
    if (!p || !ctx) {
        return;
    }
    sigset_t ready = p->sigstate.pending & ~p->sigstate.blocked;
    if (!ready) {
        return;
    }
    int sig = (int)__builtin_ctzll(ready) + 1;
    sig_deliver(p, sig, ctx, syscall_num);
}

int sigaction(int sig, const sigaction_t *act, sigaction_t *oldact) {
    struct pcb *p = sched_current_proc();
    if (!p) {
        return -ESRCH;
    }
    if (sig <= 0 || sig >= NSIG) {
        return -EINVAL;
    }
    if (sig == SIGKILL || sig == SIGSTOP) {
        return -EINVAL;
    }
    if (oldact) {
        *oldact = p->sigstate.actions[sig];
    }
    if (act) {
        p->sigstate.actions[sig] = *act;
        if (act->sa_handler == SIG_IGN || act->sa_handler == SIG_DFL) {
            p->sigstate.pending &= ~((sigset_t)1 << (sig - 1));
        }
    }
    return 0;
}

void (*signal(int sig, void (*handler)(int)))(int) {
    sigaction_t act, old;
    act.sa_handler = handler;
    act.sa_mask = 0;
    act.sa_flags = SA_RESTART;
    if (sigaction(sig, &act, &old) < 0) {
        return SIG_ERR;
    }
    return old.sa_handler;
}

static int kill_group(uint64_t pgid, int sig) {
    if (!proc_list) {
        return -ESRCH;
    }
    int found = 0;
    struct pcb *r = proc_list;
    do {
        if (r->pgid == pgid) {
            found = 1;
            sig_queue(r, sig);
        }
        r = r->next;
    } while (r != proc_list);
    return found ? 0 : -ESRCH;
}

int kill(int pid, int sig) {
    if (sig < 0 || sig >= NSIG) {
        return -EINVAL;
    }
    struct pcb *self = sched_current_proc();

    if (pid > 0) {
        struct pcb *p = proc_find((uint64_t)pid);
        if (!p) {
            return -ESRCH;
        }
        if (sig == 0) {
            return 0;
        }
        sig_queue(p, sig);
        return 0;
    }

    if (pid == -1) {
        if (!proc_list) {
            return -ESRCH;
        }
        struct pcb *r = proc_list;
        do {
            sig_queue(r, sig);
            r = r->next;
        } while (r != proc_list);
        return 0;
    }

    uint64_t pgid;
    if (pid < 0) {
        pgid = (uint64_t)(-pid);
    } else {
        if (!self) {
            return -ESRCH;
        }
        pgid = self->pgid;
    }
    if (sig == 0) {
        return proc_list ? 0 : -ESRCH;
    }
    return kill_group(pgid, sig);
}

int raise(int sig) {
    struct pcb *p = sched_current_proc();
    if (!p) {
        return -ESRCH;
    }
    return kill((int)p->pid, sig);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    struct pcb *p = sched_current_proc();
    if (!p) {
        return -ESRCH;
    }
    if (oldset) {
        *oldset = p->sigstate.blocked;
    }
    if (!set) {
        return 0;
    }
    switch (how) {
        case SIG_BLOCK:
            p->sigstate.blocked |= *set;
            break;
        case SIG_UNBLOCK:
            p->sigstate.blocked &= ~*set;
            break;
        case SIG_SETMASK:
            p->sigstate.blocked = *set;
            break;
        default:
            return -EINVAL;
    }
    p->sigstate.blocked &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    return 0;
}

int sigpending(sigset_t *set) {
    struct pcb *p = sched_current_proc();
    if (!p || !set) {
        return -ESRCH;
    }
    *set = p->sigstate.pending & p->sigstate.blocked;
    return 0;
}

int sigsuspend(const sigset_t *mask) {
    struct pcb *p = sched_current_proc();
    if (!p || !mask) {
        return -ESRCH;
    }
    sigset_t old = p->sigstate.blocked;
    p->sigstate.blocked = *mask;
    p->sigstate.blocked &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    for (;;) {
        if (p->sigstate.pending & ~p->sigstate.blocked) {
            p->sigstate.blocked = old;
            return -1;
        }
        current_tcb->state = Blocked;
        schedule();
    }
}

int sigreturn(user_context_t *ctx) {
    struct pcb *p = sched_current_proc();
    if (!p || !ctx) {
        return -1;
    }
    sigframe_t *fr = (sigframe_t *)ctx->rsp;
    p->sigstate.blocked = fr->old_mask;
    *ctx = fr->ctx;
    return 0;
}
