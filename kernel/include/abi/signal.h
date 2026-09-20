#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Signal numbers, matching Linux values so POSIX userspace works unmodified
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGIOT   SIGABRT
#define SIGEMT   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGBUS   10
#define SIGSEGV  11
#define SIGSYS   12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGURG   16
#define SIGSTOP  17
#define SIGTSTP  18
#define SIGCONT  19
#define SIGCHLD  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGUSR1  23
#define SIGUSR2  24
#define SIGCANCEL 33
#define SIGTIMER 34
// Real-time signals live from SIGRTMIN up; NSIG is one past the last signal number
#define SIGRTMIN 35
#define SIGRTMAX 64
#define NSIG     65

// Sentinel handler values: 0 = default, 1 = ignore, -1 = error return
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

// how-values for sigprocmask
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// Flag bits for sigaction
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTART   0x00000008
#define SA_NODEFER   0x00000010
#define SA_RESETHAND 0x00000020

// siginfo si_code values: who raised the signal
#define SI_USER   0
#define SI_KERNEL 0x80

// Bitmask type: signal n is stored in bit (n-1)
typedef uint64_t sigset_t;

// Structured payload passed to a SA_SIGINFO handler
typedef struct siginfo {
	int si_signo;
	int si_errno;
	int si_code;
	int si_pid;
	uintptr_t si_addr;
} siginfo_t;

// Handler registration: the union is a plain handler or a three-arg SA_SIGINFO handler
typedef struct sigaction {
	union {
		void (*sa_handler)(int);
		void (*sa_sigaction)(int, siginfo_t *, void *);
	};
	sigset_t sa_mask;
	int sa_flags;
} sigaction_t;

#ifdef __cplusplus
}
#endif
