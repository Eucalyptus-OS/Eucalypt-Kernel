#pragma once

// Small kernel-side errno set used by non-libc kernel paths; values match abi/errno.h
#define ENOMEM 0x1
#define ESRCH 0x3
#define EFAULT 0xe
#define EINVAL 0x16
