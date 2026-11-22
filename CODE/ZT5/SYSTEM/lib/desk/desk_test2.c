/* desk_test2.c - 最小测试程序，不使用任何 syscall */

typedef unsigned int uint32_t;

void _start(void) {
    /* 直接写入 framebuffer - 填充蓝色 */
    volatile uint32_t *fb = (volatile uint32_t *)0x08000000;
    int w = 1280, h = 800, stride = 1280;
    
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            fb[y * stride + x] = 0x000080FF; /* BGR: 蓝色 */
        }
    }
    
    /* 无限循环 - 不使用 syscall */
    while (1) {
        for (volatile int j = 0; j < 100000000; j++);
    }
}
