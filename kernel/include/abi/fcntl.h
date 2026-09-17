#pragma once

// open(2) flags and fcntl/access constants, matching the Linux ABI
// Mask that extracts the access-mode bits (O_RDONLY/O_WRONLY/O_RDWR)
#define O_ACCMODE   0x00000003
#define O_RDONLY    0x00000000
#define O_WRONLY    0x00000001
#define O_RDWR      0x00000002

#define O_CREAT     0x00000004
#define O_EXCL      0x00000008
#define O_TRUNC     0x00000010
#define O_APPEND    0x00000020
#define O_DIRECTORY 0x00000040
#define O_NONBLOCK  0x00000080
#define O_NOCTTY    0x00000100
#define O_CLOEXEC   0x00000200
#define O_SYNC      0x00000400
#define O_DSYNC     O_SYNC
#define O_RSYNC     O_SYNC
#define O_ASYNC     0x00000800
#define O_NOATIME   0x00001000
#define O_PATH      0x00002000

#define F_OK        0
#define X_OK        1
#define W_OK        2
#define R_OK        4

// Sentinel dirfd meaning "relative to the caller's current working directory"
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x0001
#define AT_SYMLINK_FOLLOW   0x0002
#define AT_REMOVEDIR        0x0004
#define AT_EACCESS          0x0008

// fcntl commands implemented by the kernel (see kernel/src/sys/handler.c)
#define F_DUPFD    0
#define F_GETFD    1
#define F_SETFD    2
#define F_GETFL    3
#define F_SETFL    4
// Descriptor flag returned by F_GETFD / set by F_SETFD
#define FD_CLOEXEC 1