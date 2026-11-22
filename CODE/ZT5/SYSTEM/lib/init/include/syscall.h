#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

/* UTS 信息 */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

/* 系统信息 */
struct sysinfo {
    uint64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
};

/* 时间 */
struct timeval {
    uint64_t tv_sec;
    uint64_t tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

/* 系统调用号 */
#define SYS_EXIT        0
#define SYS_READ        1
#define SYS_WRITE       2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_SEEK        5
#define SYS_STAT        6
#define SYS_EXEC        7
#define SYS_YIELD       8
#define SYS_SLEEP       9
#define SYS_GETPID      10
#define SYS_GETPPID     110
#define SYS_UNAME       111
#define SYS_SYSINFO     112
#define SYS_GETTIMEOFDAY 113
#define SYS_MMAP        11
#define SYS_MUNMAP      12
#define SYS_BRK         13
#define SYS_GETCWD      14
#define SYS_CHDIR       15
#define SYS_MKDIR       16
#define SYS_RMDIR       17
#define SYS_UNLINK      18
#define SYS_OPENDIR     19
#define SYS_READDIR     20
#define SYS_CLOSEDIR    21
#define SYS_FORK        22
#define SYS_WAIT        23
#define SYS_KILL        24
#define SYS_SIGNAL      25
#define SYS_DUP         26
#define SYS_DUP2        27
#define SYS_PIPE        28
#define SYS_GETUID      29
#define SYS_GETGID      30
#define SYS_SETUID      31
#define SYS_SETGID      32
#define SYS_TIME        33
#define SYS_UPTIME      34

/* 文件标志 */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_DIRECTORY 0x1000

/* 系统调用实现 */
static inline uint64_t syscall0(uint64_t num) {
    uint64_t ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "memory"
    );
    return ret;
}

static inline uint64_t syscall1(uint64_t num, uint64_t arg0) {
    uint64_t ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg0)
        : "memory"
    );
    return ret;
}

static inline uint64_t syscall2(uint64_t num, uint64_t arg0, uint64_t arg1) {
    uint64_t ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg0), "S"(arg1)
        : "memory"
    );
    return ret;
}

static inline uint64_t syscall3(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = arg2;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg0), "S"(arg1), "r"(r10)
        : "memory"
    );
    return ret;
}

static inline uint64_t syscall4(uint64_t num, uint64_t arg0, uint64_t arg1, 
                                 uint64_t arg2, uint64_t arg3) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = arg2;
    register uint64_t r8 __asm__("r8") = arg3;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg0), "S"(arg1), "r"(r10), "r"(r8)
        : "memory"
    );
    return ret;
}

/* 进程控制 */
static inline void sys_exit(int status) {
    syscall1(SYS_EXIT, status);
}

static inline pid_t sys_fork(void) {
    return (pid_t)syscall0(SYS_FORK);
}

static inline pid_t sys_wait(int *status) {
    return (pid_t)syscall1(SYS_WAIT, (uint64_t)status);
}

static inline pid_t sys_getpid(void) {
    return (pid_t)syscall0(SYS_GETPID);
}

static inline int sys_exec(const char *path) {
    return (int)syscall1(SYS_EXEC, (uint64_t)path);
}

static inline int sys_kill(pid_t pid, int sig) {
    return (int)syscall2(SYS_KILL, pid, sig);
}

static inline void sys_yield(void) {
    syscall0(SYS_YIELD);
}

static inline void sys_sleep(uint64_t ticks) {
    syscall1(SYS_SLEEP, ticks);
}

/* 文件操作 */
static inline int sys_open(const char *path, int flags, mode_t mode) {
    return (int)syscall3(SYS_OPEN, (uint64_t)path, flags, mode);
}

static inline int sys_close(int fd) {
    return (int)syscall1(SYS_CLOSE, fd);
}

static inline ssize_t sys_read(int fd, void *buf, size_t count) {
    return (ssize_t)syscall3(SYS_READ, fd, (uint64_t)buf, count);
}

static inline ssize_t sys_write(int fd, const void *buf, size_t count) {
    return (ssize_t)syscall3(SYS_WRITE, fd, (uint64_t)buf, count);
}

static inline int sys_seek(int fd, uint64_t offset) {
    return (int)syscall2(SYS_SEEK, fd, offset);
}

static inline int sys_mkdir(const char *path, mode_t mode) {
    return (int)syscall2(SYS_MKDIR, (uint64_t)path, mode);
}

static inline int sys_rmdir(const char *path) {
    return (int)syscall1(SYS_RMDIR, (uint64_t)path);
}

static inline int sys_unlink(const char *path) {
    return (int)syscall1(SYS_UNLINK, (uint64_t)path);
}

static inline int sys_chdir(const char *path) {
    return (int)syscall1(SYS_CHDIR, (uint64_t)path);
}

static inline char *sys_getcwd(char *buf, size_t size) {
    return (char *)syscall2(SYS_GETCWD, (uint64_t)buf, size);
}

/* 内存操作 */
static inline void *sys_mmap(void *addr, size_t len, int prot, int flags, int fd, uint64_t offset) {
    return (void *)syscall4(SYS_MMAP, (uint64_t)addr, len, prot, flags);
}

static inline int sys_munmap(void *addr, size_t len) {
    return (int)syscall2(SYS_MUNMAP, (uint64_t)addr, len);
}

static inline int sys_brk(void *addr) {
    return (int)syscall1(SYS_BRK, (uint64_t)addr);
}

/* 时间 */
static inline uint64_t sys_time(void) {
    return syscall0(SYS_TIME);
}

static inline uint64_t sys_uptime(void) {
    return syscall0(SYS_UPTIME);
}

static inline pid_t sys_getppid(void) {
    return (pid_t)syscall0(SYS_GETPPID);
}

static inline int sys_uname(struct utsname *buf) {
    return (int)syscall1(SYS_UNAME, (uint64_t)buf);
}

static inline int sys_sysinfo(struct sysinfo *info) {
    return (int)syscall1(SYS_SYSINFO, (uint64_t)info);
}

static inline int sys_gettimeofday(struct timeval *tv, struct timezone *tz) {
    return (int)syscall2(SYS_GETTIMEOFDAY, (uint64_t)tv, (uint64_t)tz);
}

#endif
