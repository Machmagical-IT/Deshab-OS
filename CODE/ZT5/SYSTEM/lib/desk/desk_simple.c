/*
 * desk.c - 极简桌面测试
 * 只绘制三个色块来验证 framebuffer 写入
 */

#include <stdint.h>
#include <stddef.h>

/* ========== 系统调用（使用 syscall 指令，64 位模式） ========== */

static long syscall1(uint64_t num, uint64_t arg1) {
    long ret;
    asm volatile(
        "syscall\n"
        : "=a"(ret)
        : "a"(num), "D"(arg1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static long syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    long ret;
    asm volatile(
        "syscall\n"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

void sys_exit(int status) {
    syscall1(0, (uint64_t)status);
    for (;;);
}

long sys_write(int fd, const void *buf, long count) {
    return syscall3(2, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

long sys_fb_sync(void *user_fb) {
    return syscall1(155, (uint64_t)user_fb);
}

/* Framebuffer 信息 */
struct fb_info {
    uint64_t base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
};

int sys_getfbinfo(struct fb_info *info) {
    return (int)syscall1(156, (uint64_t)info);
}

/* 简单的 heap */
static char heap[65536];
static int heap_pos = 0;

void *calloc(int n, int s) {
    int total = n * s;
    if (heap_pos + total > (int)sizeof(heap)) return 0;
    void *p = &heap[heap_pos];
    heap_pos += total;
    /* 清零 */
    for (int i = 0; i < total; i++) ((char*)p)[i] = 0;
    return p;
}

/* ========== 主程序 ========== */

void _start(void) {
    /* 测试 sys_write */
    const char msg1[] = "[DESK] _start entered\n";
    sys_write(2, msg1, sizeof(msg1)-1);
    
    /* 获取 framebuffer 信息 */
    struct fb_info fbinfo;
    fbinfo.base = 0;
    fbinfo.width = 0;
    fbinfo.height = 0;
    fbinfo.pitch = 0;
    
    int ret = sys_getfbinfo(&fbinfo);
    
    /* 输出返回值 */
    const char msg2[] = "[DESK] sys_getfbinfo returned\n";
    sys_write(2, msg2, sizeof(msg2)-1);
    
    /* 检查是否成功 */
    if (ret != 0) {
        const char msg3[] = "[DESK] ERROR: sys_getfbinfo failed, ret=";
        sys_write(2, msg3, sizeof(msg3)-1);
        sys_exit(1);
    }
    
    /* 检查 framebuffer 参数 */
    if (fbinfo.base == 0 || fbinfo.width == 0 || fbinfo.height == 0 || fbinfo.pitch == 0) {
        const char msg4[] = "[DESK] ERROR: Invalid framebuffer\n";
        sys_write(2, msg4, sizeof(msg4)-1);
        /* 输出 fb info */
        const char msg5[] = "[DESK] base=";
        sys_write(2, msg5, sizeof(msg5)-1);
        const char msg6[] = " width=";
        sys_write(2, msg6, sizeof(msg6)-1);
        const char msg7[] = " height=";
        sys_write(2, msg7, sizeof(msg7)-1);
        sys_exit(1);
    }
    
    const char msg8[] = "[DESK] fb OK, drawing...\n";
    sys_write(2, msg8, sizeof(msg8)-1);
    
    uint32_t *fb = (uint32_t *)(uint64_t)fbinfo.base;
    int w = (int)fbinfo.width;
    int h = (int)fbinfo.height;
    int stride = (int)(fbinfo.pitch / 4);
    
    /* 1. 填充深蓝色背景 */
    for (int y = 0; y < h; y++) {
        uint32_t *row = &fb[y * stride];
        for (int x = 0; x < w; x++) {
            row[x] = 0x001a1a3a;
        }
    }
    
    const char msg9[] = "[DESK] Background filled\n";
    sys_write(2, msg9, sizeof(msg9)-1);
    
    /* 2. 绘制红色方块 (100,100)-(300,300) */
    for (int y = 100; y < 300 && y < h; y++) {
        uint32_t *row = &fb[y * stride];
        for (int x = 100; x < 300 && x < w; x++) {
            row[x] = 0x00FF0000;
        }
    }
    
    /* 3. 绘制绿色方块 (400,100)-(600,300) */
    for (int y = 100; y < 300 && y < h; y++) {
        uint32_t *row = &fb[y * stride];
        for (int x = 400; x < 600 && x < w; x++) {
            row[x] = 0x0000FF00;
        }
    }
    
    /* 4. 绘制蓝色方块 (100,400)-(300,600) */
    for (int y = 400; y < 600 && y < h; y++) {
        uint32_t *row = &fb[y * stride];
        for (int x = 100; x < 300 && x < w; x++) {
            row[x] = 0x000000FF;
        }
    }
    
    const char msg10[] = "[DESK] Squares drawn, syncing...\n";
    sys_write(2, msg10, sizeof(msg10)-1);
    
    /* 同步 */
    sys_fb_sync(fb);
    
    const char msg11[] = "[DESK] Done, looping...\n";
    sys_write(2, msg11, sizeof(msg11)-1);
    
    /* 保持运行 */
    while (1) {
        for (volatile int j = 0; j < 100000000; j++);
    }
}
