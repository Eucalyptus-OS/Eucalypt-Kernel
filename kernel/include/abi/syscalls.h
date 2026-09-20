#pragma once

// Syscall numbers shared between kernel and libc, ordered like Linux's table
#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_LSEEK           4
#define SYS_STAT            5
#define SYS_FSTAT           6
#define SYS_LSTAT           7
#define SYS_READV           8
#define SYS_WRITEV          9
#define SYS_DUP             10
#define SYS_DUP2            11
#define SYS_MMAP            12
#define SYS_MUNMAP          13
#define SYS_MPROTECT        14
#define SYS_BRK             15
#define SYS_GETCWD          16
#define SYS_CHDIR           17
#define SYS_UMASK           18
#define SYS_UNAME           19
#define SYS_GETPID          20
#define SYS_GETPPID         21
#define SYS_GETUID          22
#define SYS_GETGID          23
#define SYS_GETEUID         24
#define SYS_GETEGID         25
#define SYS_FORK            26
#define SYS_EXECVE          27
#define SYS_EXIT            28
#define SYS_EXIT_GROUP      29
#define SYS_KILL            30
#define SYS_SIGACTION       31
#define SYS_SIGPROCMASK     32
#define SYS_SIGPENDING      33
#define SYS_SIGSUSPEND      34
#define SYS_SIGRETURN       35
#define SYS_MOUNT           36
#define SYS_GETDENTS        37
#define SYS_CLOCK_GETTIME   38
#define SYS_FSTATAT         39
#define SYS_ARCH_PRCTL      40
#define SYS_PAUSE           41
#define SYS_FCNTL           42
#define SYS_TCGETATTR       43
#define SYS_TCSETATTR       44
#define SYS_TTYINFO         45
#define SYS_WAITPID         46
#define SYS_UNLINKAT        47
#define SYS_MKFS            48
#define SYS_PIPE            49
#define SYS_IOCTL           50
#define SYS_SETPGID          51
#define SYS_GETPGID          52
#define SYS_GETPGRP          53

// One past the highest syscall number: usable as a table size or bounds check
#define SYS_MAX             54

// Sub-commands for the arch_prctl syscall
#define ARCH_SET_FS         1
#define ARCH_GET_FS         2
