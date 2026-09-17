#pragma once

#include <stdint.h>
#include <stddef.h>
#include <fs/vfs.h>

intptr_t sys_mmap(uintptr_t addr, size_t len, int prot, int flags, int fd, off_t offset);
int sys_munmap(uintptr_t addr, size_t len);
int sys_mprotect(uintptr_t addr, size_t len, int prot);
