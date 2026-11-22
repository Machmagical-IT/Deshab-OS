/*
 * syscall.c - 系统调用包装函数
 *
 * x86-64 ZratroOS ABI 调用约定:
 * RAX = 系统调用号
 * RDI = 参数1, RSI = 参数2, RDX = 参数3
 * R10 = 参数4, R8  = 参数5, R9  = 参数6
 * 返回值在 RAX
 */

#include "zsh.h"

/* 内联汇编系统调用 - 使用 int 0x80 */
static inline long syscall0(long num)
{
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "memory"
    );
    return ret;
}

static inline long syscall1(long num, long arg1)
{
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1)
        : "memory"
    );
    return ret;
}

static inline long syscall2(long num, long arg1, long arg2)
{
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "memory"
    );
    return ret;
}

static inline long syscall3(long num, long arg1, long arg2, long arg3)
{
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline long syscall4(long num, long arg1, long arg2, long arg3, long arg4)
{
    long ret;
    register long r10 __asm__("r10") = arg4;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10)
        : "memory"
    );
    return ret;
}

static inline long syscall5(long num, long arg1, long arg2, long arg3, long arg4, long arg5)
{
    long ret;
    register long r10 __asm__("r10") = arg4;
    register long r8  __asm__("r8")  = arg5;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8)
        : "memory"
    );
    return ret;
}

static inline long syscall6(long num, long arg1, long arg2, long arg3, long arg4, long arg5, long arg6)
{
    long ret;
    register long r10 __asm__("r10") = arg4;
    register long r8  __asm__("r8")  = arg5;
    register long r9  __asm__("r9")  = arg6;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "memory"
    );
    return ret;
}

/* ============================================
 * 基础 I/O 系统调用
 * ============================================ */

long sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_read, fd, (long)buf, (long)count);
}

long sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_write, fd, (long)buf, (long)count);
}

long sys_open(const char *pathname, int flags, int mode)
{
    return syscall3(SYS_open, (long)pathname, flags, mode);
}

long sys_close(int fd)
{
    return syscall1(SYS_close, fd);
}

long sys_seek(int fd, off_t offset, int whence)
{
    return syscall3(SYS_seek, fd, offset, whence);
}

long sys_stat(const char *pathname, struct zsh_stat *statbuf)
{
    return syscall2(SYS_stat, (long)pathname, (long)statbuf);
}

/* ============================================
 * 目录和文件系统
 * ============================================ */

long sys_chdir(const char *path)
{
    return syscall1(SYS_chdir, (long)path);
}

long sys_getcwd(char *buf, size_t size)
{
    return syscall2(SYS_getcwd, (long)buf, (long)size);
}

long sys_mkdir(const char *pathname, int mode)
{
    return syscall2(SYS_mkdir, (long)pathname, mode);
}

long sys_rmdir(const char *pathname)
{
    return syscall1(SYS_rmdir, (long)pathname);
}

long sys_unlink(const char *pathname)
{
    return syscall1(SYS_unlink, (long)pathname);
}

long sys_chmod(const char *pathname, int mode)
{
    return syscall2(SYS_chmod, (long)pathname, mode);
}

long sys_opendir(const char *name)
{
    return syscall1(SYS_opendir, (long)name);
}

long sys_readdir(int fd, void *dirp, unsigned int count)
{
    return syscall3(SYS_readdir, fd, (long)dirp, (long)count);
}

long sys_closedir(int fd)
{
    return syscall1(SYS_closedir, fd);
}

/* ============================================
 * 进程控制
 * ============================================ */

long sys_execve(const char *pathname, char *const argv[], char *const envp[])
{
    return syscall3(SYS_execve, (long)pathname, (long)argv, (long)envp);
}

long sys_exit(int status)
{
    return syscall1(SYS_exit, status);
}

long sys_getpid(void)
{
    return syscall0(SYS_getpid);
}

long sys_getppid(void)
{
    return syscall0(SYS_getppid);
}

long sys_getuid(void)
{
    return syscall0(SYS_getuid);
}

long sys_getgid(void)
{
    return syscall0(SYS_getgid);
}

long sys_uname(struct utsname *buf)
{
    return syscall1(SYS_uname, (long)buf);
}

/* ============================================
 * 内存管理
 * ============================================ */

long sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    return syscall6(SYS_mmap, (long)addr, (long)length, prot, flags, fd, offset);
}

long sys_munmap(void *addr, size_t length)
{
    return syscall2(SYS_munmap, (long)addr, (long)length);
}

/* ============================================
 * 终端控制 (ioctl)
 * ============================================ */

long sys_ioctl(int fd, unsigned long request, void *argp)
{
    return syscall3(SYS_ioctl, fd, (long)request, (long)argp);
}

/* ============================================
 * 进程控制 (fork, wait)
 * ============================================ */

long sys_fork(void)
{
    return syscall0(SYS_fork);
}

long sys_wait(int pid, int *wstatus, int options, void *rusage)
{
    return syscall4(SYS_wait, pid, (long)wstatus, options, (long)rusage);
}

/* ============================================
 * 管道和文件描述符复制
 * ============================================ */

long sys_pipe(int pipefd[2])
{
    return syscall1(SYS_pipe, (long)pipefd);
}

long sys_dup(int oldfd)
{
    return syscall1(SYS_dup, oldfd);
}

long sys_dup2(int oldfd, int newfd)
{
    return syscall2(SYS_dup2, oldfd, newfd);
}

/* ============================================
 * 权限系统调用
 * ============================================ */

long sys_sudo(const char *hash)
{
    return syscall1(SYS_sudo, (long)hash);
}
