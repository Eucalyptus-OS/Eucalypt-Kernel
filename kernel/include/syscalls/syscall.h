#pragma once
#include <stdint.h>

#define SYS_READ           0
#define SYS_WRITE          1
#define SYS_OPEN           2
#define SYS_CLOSE          3
#define SYS_SEEK           4
#define SYS_STAT           5
#define SYS_MKDIR          6
#define SYS_RMDIR          7
#define SYS_UNLINK         8
#define SYS_READDIR        9
#define SYS_DUP            10
#define SYS_DUP2           11
#define SYS_TELL           12
#define SYS_FSTAT          13
#define SYS_GETPID         14
#define SYS_FORK           15
#define SYS_EXEC           16
#define SYS_EXIT           17
#define SYS_WAITPID        18
#define SYS_MMAP           19
#define SYS_MUNMAP         20
#define SYS_GETPPID        21
#define SYS_EXIT_GROUP     22
#define SYS_KILL           23
#define SYS_SIGNAL         24
#define SYS_SIGDEFAULT     25
#define SYS_GETPGID        26
#define SYS_SETPGID        27
#define SYS_GETSID         28
#define SYS_SETSID         29
#define FRAMEBUFFER_PX     30
#define FRAMEBUFFER_RECT   31
#define FRAMEBUFFER_CIRCLE 32
#define INPUT_SUBSCRIBE    33
#define INPUT_UNSUBSCRIBE  34
#define INPUT_READ         35
#define THREAD_CREATE      36
#define THREAD_REMOVE      37
#define SYS_CHDIR          38
#define SYS_GETCWD         39
#define SYS_RENAME         40
#define SYS_CHMOD          41
#define SYS_FCHMOD         42
#define NR_SYSCALLS        43

typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

uint64_t do_syscall(uint64_t syscall_num, uint64_t arg0, uint64_t arg1, uint64_t arg2,
                    uint64_t arg3, uint64_t arg4, uint64_t arg5);
