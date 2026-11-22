#ifndef UNISTD_H
#define UNISTD_H

#include "types.h"
#include "syscall.h"

/* 标准文件描述符 */
#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

/* 内存保护标志 */
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

/* 内存映射标志 */
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20

/* 进程控制 */
static inline void exit(int status) {
    sys_exit(status);
    __builtin_unreachable();
}

static inline pid_t fork(void) {
    return sys_fork();
}

static inline pid_t getpid(void) {
    return sys_getpid();
}

static inline int exec(const char *path) {
    return sys_exec(path);
}

static inline int kill(pid_t pid, int sig) {
    return sys_kill(pid, sig);
}

static inline void yield(void) {
    sys_yield();
}

static inline void sleep(uint64_t ticks) {
    sys_sleep(ticks);
}

/* 文件操作 */
static inline int open(const char *path, int flags, ...) {
    return sys_open(path, flags, 0);
}

static inline int close(int fd) {
    return sys_close(fd);
}

static inline ssize_t read(int fd, void *buf, size_t count) {
    return sys_read(fd, buf, count);
}

static inline ssize_t write(int fd, const void *buf, size_t count) {
    return sys_write(fd, buf, count);
}

static inline int mkdir(const char *path, mode_t mode) {
    return sys_mkdir(path, mode);
}

static inline int rmdir(const char *path) {
    return sys_rmdir(path);
}

static inline int unlink(const char *path) {
    return sys_unlink(path);
}

static inline int chdir(const char *path) {
    return sys_chdir(path);
}

static inline char *getcwd(char *buf, size_t size) {
    return sys_getcwd(buf, size);
}

/* 用户/组 */
static inline uid_t getuid(void) {
    return (uid_t)syscall0(SYS_GETUID);
}

static inline gid_t getgid(void) {
    return (gid_t)syscall0(SYS_GETGID);
}

#endif
