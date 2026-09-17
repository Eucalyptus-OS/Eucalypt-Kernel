#pragma once

#include <stdint.h>
#include <abi/signal.h>

// Per-process signal state: one action per signal plus the pending/blocked sets
typedef struct sigstate {
    sigaction_t actions[NSIG];
    sigset_t pending;
    sigset_t blocked;
} sigstate_t;

// The five dispositions a signal can have when not installed with a handler
typedef enum {
    SIG_ACTION_TERMINATE,
    SIG_ACTION_IGNORE,
    SIG_ACTION_CORE,
    SIG_ACTION_STOP,
    SIG_ACTION_CONTINUE,
} sig_default_action_t;

// Full user register save as pushed by the syscall/interrupt entry path
typedef struct user_context {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} user_context_t;

// On-stack save area; sigreturn pops it to resume the interrupted user code
typedef struct sigframe {
    user_context_t ctx;
    sigset_t old_mask;
    siginfo_t info;
    uint64_t saved_syscall;
    uint64_t sa_flags;
} sigframe_t;

typedef void (*sig_handler_t)(int);

struct pcb;

void (*signal(int sig, void (*handler)(int)))(int);
int sigaction(int sig, const sigaction_t *act, sigaction_t *oldact);
int kill(int pid, int sig);
int raise(int sig);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigpending(sigset_t *set);
int sigsuspend(const sigset_t *mask);
int sigreturn(user_context_t *ctx);
void default_sig_handler(int sig);
void sig_deliver_current(user_context_t *ctx, uint64_t syscall_num);
void sig_deliver(struct pcb *p, int sig, user_context_t *ctx, uint64_t syscall_num);
void sig_queue(struct pcb *p, int sig);
void wake_threads(struct pcb *p);

static inline int sigemptyset(sigset_t *set) {
    *set = 0;
    return 0;
}

static inline int sigfillset(sigset_t *set) {
    *set = ~(sigset_t)0;
    return 0;
}

static inline int sigaddset(sigset_t *set, int sig) {
    if (sig <= 0 || sig >= NSIG) return -1;
    // Signal n occupies bit (n-1), matching the kernel's pending/blocked layout
    *set |= (sigset_t)1 << (sig - 1);
    return 0;
}

static inline int sigdelset(sigset_t *set, int sig) {
    if (sig <= 0 || sig >= NSIG) return -1;
    *set &= ~((sigset_t)1 << (sig - 1));
    return 0;
}

static inline int sigismember(const sigset_t *set, int sig) {
    if (sig <= 0 || sig >= NSIG) return -1;
    return (int)((*set >> (sig - 1)) & 1);
}
