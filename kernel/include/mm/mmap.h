#pragma once

#include <stdint.h>
#include <stddef.h>
#include <fs/vfs.h>

// mmap(2): map a file or anonymous memory region into the address space
intptr_t sys_mmap(uintptr_t addr, size_t len, int prot, int flags, int fd, off_t offset);
// munmap(2): remove a mapped region
int sys_munmap(uintptr_t addr, size_t len);
// mprotect(2): change the protection of an address range
int sys_mprotect(uintptr_t addr, size_t len, int prot);
