/* desk_test.c - 最小化测试程序 */
/* 不使用 stdint.h，直接定义类型 */

typedef unsigned int uint32_t;
typedef long ssize_t;

extern long sys_write(int fd, const void *buf, long count);
extern long sys_fb_sync(void *user_fb);

void _start(void) {
    /* 输出启动信息 */
    const char msg1[] = "[TEST] START\n";
    sys_write(2, msg1, 14);
    
    /* 直接写入 framebuffer - 填充蓝色 */
    volatile uint32_t *fb = (volatile uint32_t *)0x08000000;
    int w = 1280, h = 800, pitch = 1280 * 4;
    int stride = pitch / 4;
    
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            fb[y * stride + x] = 0x000080FF; /* BGR: 蓝色 */
        }
    }
    
    const char msg2[] = "[TEST] Blue fill done\n";
    sys_write(2, msg2, 21);
    
    /* 同步 */
    sys_fb_sync((void *)fb);
    
    const char msg3[] = "[TEST] Synced\n";
    sys_write(2, msg3, 14);
    
    /* 无限循环 */
    while (1) {
        for (volatile int j = 0; j < 100000000; j++);
    }
}
