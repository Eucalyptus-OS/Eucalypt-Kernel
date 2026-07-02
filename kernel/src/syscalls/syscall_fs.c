#include <stdint.h>
#include <drivers/fs/vfs/vfs.h>
#include <syscalls/syscall_fs.h>

uint64_t sys_stat(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    const char *path = (const char *)arg0;
    vfs_stat_t *st   = (vfs_stat_t *)arg1;
    return (uint64_t)vfs_stat(path, st);
}

uint64_t sys_mkdir(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    const char *path = (const char *)arg0;
    return (uint64_t)vfs_mkdir(path);
}

uint64_t sys_rmdir(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    const char *path = (const char *)arg0;
    return (uint64_t)vfs_rmdir(path);
}

uint64_t sys_unlink(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    const char *path = (const char *)arg0;
    return (uint64_t)vfs_unlink(path);
}

uint64_t sys_readdir(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg3; (void)arg4; (void)arg5;
    const char *path  = (const char *)arg0;
    uint32_t index    = (uint32_t)arg1;
    vfs_dirent_t *out = (vfs_dirent_t *)arg2;
    return (uint64_t)vfs_readdir(path, index, out);
}

uint64_t sys_chdir(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    const char *path = (const char *)arg0;
    return (uint64_t)vfs_chdir(path);
}

uint64_t sys_getcwd(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    char *buf = (char *)arg0;
    size_t size = (size_t)arg1;
    return (uint64_t)vfs_getcwd(buf, size);
}

uint64_t sys_rename(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    const char *old_path = (const char *)arg0;
    const char *new_path = (const char *)arg1;
    return (uint64_t)rename(old_path, new_path);
}

uint64_t sys_chmod(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    const char *path = (const char *)arg0;
    uint32_t mode = (uint32_t)arg1;
    return (uint64_t)chmod(path, mode);
}

uint64_t sys_fchmod(uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    int fd = (int)arg0;
    uint32_t mode = (uint32_t)arg1;
    return (uint64_t)fchmod(fd, mode);
}