/*
 * 桌面程序的 syscall 包装器
 * 使用 int 0x80 中断（兼容模式）
 */

#include <stdint.h>
#include <stddef.h>

/* 系统调用号 (Zratro OS ABI) */
#define SYS_EXIT    0
#define SYS_WRITE   2
#define SYS_READ    0
#define SYS_OPEN    3
#define SYS_CLOSE   4
#define SYS_MMAP    11
#define SYS_MUNMAP  12
#define SYS_FB_SYNC 155
#define SYS_GETFBINFO 156
#define SYS_GETTIME 100
#define SYS_GETEVENT 101
#define SYS_EXEC    102

/* 系统调用宏 - 使用 int 0x80，参数通过 rdi/rsi/rdx 传递（64位 ABI） */
/* 关键：不能是 inline，编译器会错误优化参数传递 */
__attribute__((noinline))
static long syscall1_int(long num, long arg1) {
    long ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

__attribute__((noinline))
static long syscall3_int(long num, long arg1, long arg2, long arg3) {
    long ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

__attribute__((noinline))
static long syscall2_int(long num, long arg1, long arg2) {
    long ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

__attribute__((noinline))
static long syscall0_int(long num) {
    long ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* 系统调用包装函数 - 禁止内联以确保参数正确传递 */
__attribute__((noinline))
void sys_exit(int status) {
    syscall1_int(SYS_EXIT, (long)status);
    for (;;);
}

__attribute__((noinline))
long sys_write(int fd, const void *buf, long count) {
    return syscall3_int(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

__attribute__((noinline))
long sys_read(int fd, void *buf, long count) {
    return syscall3_int(SYS_READ, (long)fd, (long)buf, (long)count);
}

__attribute__((noinline))
long sys_fb_sync(void *user_fb) {
    return syscall1_int(SYS_FB_SYNC, (long)user_fb);
}

/* Framebuffer 信息 */
struct fb_info {
    uint64_t base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
} __attribute__((packed));

int sys_getfbinfo(struct fb_info *info) {
    return (int)syscall1_int(SYS_GETFBINFO, (long)info);
}

/* 文件操作 */
int sys_open(const char *path, int flags, int mode) {
    return (int)syscall3_int(SYS_OPEN, (long)path, (long)flags, (long)mode);
}

int sys_close(int fd) {
    return (int)syscall1_int(SYS_CLOSE, (long)fd);
}

/* 获取系统时间 (毫秒) */
uint64_t sys_gettime(void) {
    return (uint64_t)syscall1_int(SYS_GETTIME, 0);
}

/* 获取输入事件 */
int sys_getevent(void *data, int data_size) {
    return (int)syscall2_int(SYS_GETEVENT, (long)data, (long)data_size);
}

/* 执行程序 */
int sys_exec(const char *path, int argc, const char **argv) {
    return (int)syscall3_int(SYS_EXEC, (long)path, (long)argc, (long)argv);
}
