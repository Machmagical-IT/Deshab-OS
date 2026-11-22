/*
 * desk_minimal.c - 最小化桌面测试
 * 只绘制纯色背景和测试方块
 */

#include <stdint.h>
#include <stddef.h>

/* 系统调用 */
extern void sys_exit(int status);
extern long sys_write(int fd, const void *buf, long count);

/* framebuffer 信息 */
struct fb_info {
    uint64_t base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
};

/* 内联汇编调用系统调用 */
static int sys_getfbinfo(struct fb_info *info) {
    int ret;
    __asm__ volatile (
        "mov $15, %%rax\n"
        "mov %1, %%rdi\n"
        "syscall\n"
        : "=a"(ret)
        : "r"(info)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"
    );
    return ret;
}

static long sys_fb_sync(void *user_fb) {
    long ret;
    __asm__ volatile (
        "mov $16, %%rax\n"
        "mov %1, %%rdi\n"
        "syscall\n"
        : "=a"(ret)
        : "r"(user_fb)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"
    );
    return ret;
}

void _start(void) {
    /* 获取 framebuffer 信息 */
    struct fb_info fbinfo;
    int ret = sys_getfbinfo(&fbinfo);
    if (ret != 0) {
        const char msg[] = "[MIN] sys_getfbinfo failed\n";
        sys_write(2, msg, sizeof(msg)-1);
        sys_exit(1);
    }
    
    uint32_t *fb = (uint32_t *)fbinfo.base;
    int w = fbinfo.width;
    int h = fbinfo.height;
    int stride = fbinfo.pitch / 4;
    
    /* 调试输出 */
    const char msg1[] = "[MIN] fb OK: ";
    sys_write(2, msg1, sizeof(msg1)-1);
    
    /* 检查参数 */
    if (w == 0 || h == 0 || stride == 0 || fbinfo.base == 0) {
        const char msg2[] = "[MIN] INVALID fb params\n";
        sys_write(2, msg2, sizeof(msg2)-1);
        sys_exit(1);
    }
    
    const char msg3[] = "[MIN] drawing...\n";
    sys_write(2, msg3, sizeof(msg3)-1);
    
    /* 绘制深蓝色背景 */
    for (int y = 0; y < h; y++) {
        uint32_t *row = &fb[y * stride];
        for (int x = 0; x < w; x++) {
            row[x] = 0x001a1a3a; /* 深蓝色 */
        }
    }
    
    /* 绘制红色方块测试 */
    for (int y = 100; y < 300; y++) {
        for (int x = 100; x < 300; x++) {
            if (x < w && y < h) {
                fb[y * stride + x] = 0x00FF0000; /* 红色 */
            }
        }
    }
    
    /* 绘制绿色方块测试 */
    for (int y = 100; y < 300; y++) {
        for (int x = 400; x < 600; x++) {
            if (x < w && y < h) {
                fb[y * stride + x] = 0x0000FF00; /* 绿色 */
            }
        }
    }
    
    /* 绘制蓝色方块测试 */
    for (int y = 400; y < 600; y++) {
        for (int x = 100; x < 300; x++) {
            if (x < w && y < h) {
                fb[y * stride + x] = 0x000000FF; /* 蓝色 */
            }
        }
    }
    
    const char msg4[] = "[MIN] sync...\n";
    sys_write(2, msg4, sizeof(msg4)-1);
    
    /* 同步 */
    sys_fb_sync(fb);
    
    const char msg5[] = "[MIN] done, looping...\n";
    sys_write(2, msg5, sizeof(msg5)-1);
    
    /* 保持运行 */
    for (;;) {
        for (volatile int j = 0; j < 100000000; j++);
    }
}
