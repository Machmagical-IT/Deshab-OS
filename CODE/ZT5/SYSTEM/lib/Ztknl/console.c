#include "console.h"
#include "limine.h"
#include "string.h"
#include "heap.h"  /* for kfree */
#include "chinese_font.h"  /* 中文字符支持 */
#include <stdarg.h>  /* for printk */

/* ═══════════════════════════════════════════════════════════════════════════
 * 中文字符绘制实现 (供 chinese_font.c 调用)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * cn_draw_char_impl - 绘制中文字符的内部实现
 * 注意: 由于当前帧缓冲已禁用, 此函数仅作为占位符
 * 当启用帧缓冲时, 需要实现实际的16x16点阵绘制
 */
void cn_draw_char_impl(uint32_t utf8_code, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    /* 当前帧缓冲已禁用, 不执行绘制 */
    /* 当启用帧缓冲时, 这里需要:
     * 1. 调用 cn_font_find_char(utf8_code) 获取点阵数据
     * 2. 遍历16x16点阵, 逐像素绘制到帧缓冲
     * 3. 1表示前景色, 0表示背景色
     */
    (void)utf8_code;
    (void)x;
    (void)y;
    (void)fg_color;
    (void)bg_color;
}

/* ========== 串口调试输出 ========== */
#define COM1_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static int serial_initialized = 0;

void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);    /* 禁用中断 */
    outb(COM1_PORT + 3, 0x80);    /* 启用 DLAB */
    outb(COM1_PORT + 0, 0x03);    /* 除数低字节 (38400 baud) */
    outb(COM1_PORT + 1, 0x00);    /* 除数高字节 */
    outb(COM1_PORT + 3, 0x03);    /* 8 bits, no parity, one stop bit */
    outb(COM1_PORT + 2, 0xC7);    /* 启用 FIFO */
    outb(COM1_PORT + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
    serial_initialized = 1;
}

void serial_putchar(char c) {
    if (!serial_initialized) return;
    /* 等待传输缓冲区为空 */
    while ((inb(COM1_PORT + 5) & 0x20) == 0);
    outb(COM1_PORT, c);
}

/* 从串口读取字符，无输入返回 -1 */
int serial_getchar(void) {
    if (!serial_initialized) return -1;
    /* 检查接收缓冲区是否有数据 */
    if ((inb(COM1_PORT + 5) & 0x01) == 0) {
        return -1;  /* 无数据 */
    }
    return inb(COM1_PORT);
}

/* 串口打印函数 - 仅输出到串口 */
void serial_print(const char *str) {
    while (*str) {
        if (*str == '\n') {
            serial_putchar('\r');
            serial_putchar('\n');
            str++;
        } else if ((uint8_t)*str >= 0x80) {
            int char_len = utf8_char_length((uint8_t)*str);
            for (int i = 0; i < char_len && *str; i++) {
                serial_putchar(*str++);
            }
        } else {
            serial_putchar(*str++);
        }
    }
}

void serial_print_hex(uint64_t val) {
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    int i = 0;
    buf[i++] = '0';
    buf[i++] = 'x';
    int started = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
        int digit = (val >> shift) & 0xF;
        if (digit != 0 || started || shift == 0) {
            buf[i++] = hex[digit];
            started = 1;
        }
    }
    buf[i] = 0;
    serial_print(buf);
}

/* 帧缓冲信息 - 已禁用，仅保留定义以免破坏其他代码 */
__attribute__((unused)) static struct limine_framebuffer *fb = NULL;
static uint32_t *fb_base = NULL;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0;

/* 光标位置 */
static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;

/* 字体尺寸 (7x10 小字体) */
#define CHAR_WIDTH  7
#define CHAR_HEIGHT 10

/* ========== 现代化颜色主题 ========== */
/* 主色调 - 科技蓝 */
#define COLOR_PRIMARY        0x00D4FF  /* 青色 */
#define COLOR_SECONDARY      0x7B68EE  /* 中紫色 */
#define COLOR_ACCENT         0xFF6B6B  /* 珊瑚红 */
#define COLOR_SUCCESS        0x00FF88  /* 翠绿色 */
#define COLOR_WARNING        0xFFB800  /* 金黄色 */
#define COLOR_ERROR          0xFF4757  /* 红色 */
#define COLOR_INFO           0x70A1FF  /* 淡蓝色 */

/* 背景色 */
#define COLOR_BG_DARK        0x0D1117  /* 深黑 */
#define COLOR_BG_PANEL       0x161B22  /* 面板背景 */
#define COLOR_BG_HOVER       0x21262D  /* 悬停背景 */

/* 文字颜色 */
#define COLOR_TEXT_PRIMARY   0xF0F6FC  /* 主要文字 */
#define COLOR_TEXT_SECONDARY 0x8B949E  /* 次要文字 */
#define COLOR_TEXT_MUTED     0x484F58  /* 弱化文字 */

/* 默认颜色 */
static uint32_t fg_color = 0xF0F6FC;  /* 主要文字颜色 */
static uint32_t bg_color = 0x0D1117;  /* 深黑背景 */

/* 日志功能前向声明 */
static void klog_write(const char *str);

/* 6x10 位图字体 (ASCII 32-126) */
static const uint8_t font[95][10] = {
    /* 空格 */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* ! */     {0x00,0x00,0x10,0x10,0x10,0x10,0x10,0x00,0x10,0x00},
    /* " */     {0x00,0x00,0x28,0x28,0x28,0x00,0x00,0x00,0x00,0x00},
    /* # */     {0x00,0x00,0x28,0x28,0x7C,0x28,0x7C,0x28,0x28,0x00},
    /* $ */     {0x00,0x10,0x3C,0x50,0x38,0x14,0x78,0x10,0x00,0x00},
    /* % */     {0x00,0x00,0x64,0x64,0x08,0x10,0x20,0x4C,0x4C,0x00},
    /* & */     {0x00,0x00,0x30,0x48,0x30,0x54,0x48,0x34,0x00,0x00},
    /* ' */     {0x00,0x00,0x10,0x10,0x20,0x00,0x00,0x00,0x00,0x00},
    /* ( */     {0x00,0x08,0x10,0x20,0x20,0x20,0x10,0x08,0x00,0x00},
    /* ) */     {0x00,0x20,0x10,0x08,0x08,0x08,0x10,0x20,0x00,0x00},
    /* * */     {0x00,0x00,0x10,0x54,0x38,0x54,0x10,0x00,0x00,0x00},
    /* + */     {0x00,0x00,0x00,0x10,0x10,0x7C,0x10,0x10,0x00,0x00},
    /* , */     {0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x10,0x20,0x00},
    /* - */     {0x00,0x00,0x00,0x00,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* . */     {0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x10,0x00,0x00},
    /* / */     {0x00,0x04,0x08,0x10,0x10,0x20,0x40,0x00,0x00,0x00},
    /* 0 */     {0x00,0x00,0x38,0x44,0x4C,0x54,0x64,0x38,0x00,0x00},
    /* 1 */     {0x00,0x00,0x10,0x30,0x10,0x10,0x10,0x38,0x00,0x00},
    /* 2 */     {0x00,0x00,0x38,0x44,0x04,0x18,0x20,0x7C,0x00,0x00},
    /* 3 */     {0x00,0x00,0x38,0x44,0x18,0x04,0x44,0x38,0x00,0x00},
    /* 4 */     {0x00,0x00,0x08,0x18,0x28,0x7C,0x08,0x08,0x00,0x00},
    /* 5 */     {0x00,0x00,0x7C,0x40,0x78,0x04,0x44,0x38,0x00,0x00},
    /* 6 */     {0x00,0x00,0x18,0x20,0x78,0x44,0x44,0x38,0x00,0x00},
    /* 7 */     {0x00,0x00,0x7C,0x04,0x08,0x10,0x10,0x10,0x00,0x00},
    /* 8 */     {0x00,0x00,0x38,0x44,0x38,0x44,0x44,0x38,0x00,0x00},
    /* 9 */     {0x00,0x00,0x38,0x44,0x44,0x3C,0x04,0x30,0x00,0x00},
    /* : */     {0x00,0x00,0x00,0x10,0x00,0x00,0x10,0x00,0x00,0x00},
    /* ; */     {0x00,0x00,0x00,0x10,0x00,0x00,0x10,0x10,0x20,0x00},
    /* < */     {0x00,0x04,0x08,0x10,0x20,0x10,0x08,0x04,0x00,0x00},
    /* = */     {0x00,0x00,0x00,0x7C,0x00,0x7C,0x00,0x00,0x00,0x00},
    /* > */     {0x00,0x20,0x10,0x08,0x04,0x08,0x10,0x20,0x00,0x00},
    /* ? */     {0x00,0x00,0x38,0x44,0x08,0x10,0x00,0x10,0x00,0x00},
    /* @ */     {0x00,0x00,0x38,0x44,0x5C,0x54,0x5C,0x40,0x38,0x00},
    /* A */     {0x00,0x00,0x38,0x44,0x44,0x7C,0x44,0x44,0x00,0x00},
    /* B */     {0x00,0x00,0x78,0x44,0x78,0x44,0x44,0x78,0x00,0x00},
    /* C */     {0x00,0x00,0x38,0x44,0x40,0x40,0x44,0x38,0x00,0x00},
    /* D */     {0x00,0x00,0x78,0x44,0x44,0x44,0x44,0x78,0x00,0x00},
    /* E */     {0x00,0x00,0x7C,0x40,0x78,0x40,0x40,0x7C,0x00,0x00},
    /* F */     {0x00,0x00,0x7C,0x40,0x78,0x40,0x40,0x40,0x00,0x00},
    /* G */     {0x00,0x00,0x38,0x44,0x40,0x4C,0x44,0x3C,0x00,0x00},
    /* H */     {0x00,0x00,0x44,0x44,0x7C,0x44,0x44,0x44,0x00,0x00},
    /* I */     {0x00,0x00,0x38,0x10,0x10,0x10,0x10,0x38,0x00,0x00},
    /* J */     {0x00,0x00,0x04,0x04,0x04,0x44,0x44,0x38,0x00,0x00},
    /* K */     {0x00,0x00,0x44,0x48,0x70,0x48,0x44,0x44,0x00,0x00},
    /* L */     {0x00,0x00,0x40,0x40,0x40,0x40,0x40,0x7C,0x00,0x00},
    /* M */     {0x00,0x00,0x44,0x6C,0x54,0x44,0x44,0x44,0x00,0x00},
    /* N */     {0x00,0x00,0x44,0x64,0x54,0x4C,0x44,0x44,0x00,0x00},
    /* O */     {0x00,0x00,0x38,0x44,0x44,0x44,0x44,0x38,0x00,0x00},
    /* P */     {0x00,0x00,0x78,0x44,0x78,0x40,0x40,0x40,0x00,0x00},
    /* Q */     {0x00,0x00,0x38,0x44,0x44,0x54,0x48,0x34,0x00,0x00},
    /* R */     {0x00,0x00,0x78,0x44,0x78,0x48,0x44,0x44,0x00,0x00},
    /* S */     {0x00,0x00,0x38,0x40,0x38,0x04,0x44,0x38,0x00,0x00},
    /* T */     {0x00,0x00,0x7C,0x10,0x10,0x10,0x10,0x10,0x00,0x00},
    /* U */     {0x00,0x00,0x44,0x44,0x44,0x44,0x44,0x38,0x00,0x00},
    /* V */     {0x00,0x00,0x44,0x44,0x44,0x28,0x28,0x10,0x00,0x00},
    /* W */     {0x00,0x00,0x44,0x44,0x54,0x54,0x6C,0x44,0x00,0x00},
    /* X */     {0x00,0x00,0x44,0x28,0x10,0x28,0x44,0x44,0x00,0x00},
    /* Y */     {0x00,0x00,0x44,0x44,0x28,0x10,0x10,0x10,0x00,0x00},
    /* Z */     {0x00,0x00,0x7C,0x08,0x10,0x20,0x40,0x7C,0x00,0x00},
    /* [ */     {0x00,0x00,0x38,0x20,0x20,0x20,0x20,0x38,0x00,0x00},
    /* \ */     {0x00,0x40,0x20,0x10,0x10,0x08,0x04,0x00,0x00,0x00},
    /* ] */     {0x00,0x00,0x38,0x08,0x08,0x08,0x08,0x38,0x00,0x00},
    /* ^ */     {0x00,0x10,0x28,0x44,0x00,0x00,0x00,0x00,0x00,0x00},
    /* _ */     {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7C,0x00},
    /* ` */     {0x00,0x20,0x10,0x08,0x00,0x00,0x00,0x00,0x00,0x00},
    /* a */     {0x00,0x00,0x00,0x38,0x04,0x3C,0x44,0x3C,0x00,0x00},
    /* b */     {0x00,0x40,0x40,0x58,0x64,0x44,0x64,0x58,0x00,0x00},
    /* c */     {0x00,0x00,0x00,0x38,0x44,0x40,0x44,0x38,0x00,0x00},
    /* d */     {0x00,0x04,0x04,0x34,0x4C,0x44,0x4C,0x34,0x00,0x00},
    /* e */     {0x00,0x00,0x00,0x38,0x44,0x7C,0x40,0x38,0x00,0x00},
    /* f */     {0x00,0x18,0x24,0x20,0x78,0x20,0x20,0x20,0x00,0x00},
    /* g */     {0x00,0x00,0x00,0x34,0x4C,0x44,0x3C,0x04,0x44,0x38},
    /* h */     {0x00,0x40,0x40,0x58,0x64,0x44,0x44,0x44,0x00,0x00},
    /* i */     {0x00,0x10,0x00,0x30,0x10,0x10,0x10,0x38,0x00,0x00},
    /* j */     {0x00,0x08,0x00,0x18,0x08,0x08,0x08,0x48,0x48,0x30},
    /* k */     {0x00,0x40,0x40,0x48,0x50,0x70,0x48,0x44,0x00,0x00},
    /* l */     {0x00,0x30,0x10,0x10,0x10,0x10,0x10,0x38,0x00,0x00},
    /* m */     {0x00,0x00,0x00,0x68,0x54,0x54,0x44,0x44,0x00,0x00},
    /* n */     {0x00,0x00,0x00,0x58,0x64,0x44,0x44,0x44,0x00,0x00},
    /* o */     {0x00,0x00,0x00,0x38,0x44,0x44,0x44,0x38,0x00,0x00},
    /* p */     {0x00,0x00,0x00,0x58,0x64,0x64,0x58,0x40,0x40,0x40},
    /* q */     {0x00,0x00,0x00,0x34,0x4C,0x4C,0x34,0x04,0x04,0x04},
    /* r */     {0x00,0x00,0x00,0x58,0x64,0x40,0x40,0x40,0x00,0x00},
    /* s */     {0x00,0x00,0x00,0x3C,0x40,0x38,0x04,0x78,0x00,0x00},
    /* t */     {0x00,0x20,0x20,0x78,0x20,0x20,0x24,0x18,0x00,0x00},
    /* u */     {0x00,0x00,0x00,0x44,0x44,0x44,0x4C,0x34,0x00,0x00},
    /* v */     {0x00,0x00,0x00,0x44,0x44,0x28,0x28,0x10,0x00,0x00},
    /* w */     {0x00,0x00,0x00,0x44,0x44,0x54,0x54,0x28,0x00,0x00},
    /* x */     {0x00,0x00,0x00,0x44,0x28,0x10,0x28,0x44,0x00,0x00},
    /* y */     {0x00,0x00,0x00,0x44,0x44,0x3C,0x04,0x44,0x38,0x00},
    /* z */     {0x00,0x00,0x00,0x7C,0x08,0x10,0x20,0x7C,0x00,0x00},
    /* { */     {0x00,0x08,0x10,0x10,0x20,0x10,0x10,0x08,0x00,0x00},
    /* | */     {0x00,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x00,0x00},
    /* } */     {0x00,0x20,0x10,0x10,0x08,0x10,0x10,0x20,0x00,0x00},
    /* ~ */     {0x00,0x00,0x00,0x34,0x58,0x00,0x00,0x00,0x00,0x00},
};

static void draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_width || y >= fb_height) return;
    fb_base[y * fb_pitch + x] = color;
}

static void draw_char(char c, uint32_t x, uint32_t y) {
    if (c < 32 || c > 126) return;
    
    const uint8_t *glyph = font[c - 32];
    for (uint32_t row = 0; row < CHAR_HEIGHT; row++) {
        for (uint32_t col = 0; col < CHAR_WIDTH; col++) {
            uint32_t color = (glyph[row] & (0x40 >> col)) ? fg_color : bg_color;
            draw_pixel(x + col, y + row, color);
        }
    }
}

static void scroll(void) {
    for (uint32_t y = CHAR_HEIGHT; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            fb_base[(y - CHAR_HEIGHT) * fb_pitch + x] = fb_base[y * fb_pitch + x];
        }
    }
    for (uint32_t y = fb_height - CHAR_HEIGHT; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            fb_base[y * fb_pitch + x] = bg_color;
        }
    }
}

/* 控制台输出禁用标志 */
static int console_output_disabled = 0;

void console_init_with_fb(volatile struct limine_framebuffer_request *req) {
    /* 初始化串口 (调试输出) */
    serial_init();

    /* 初始化帧缓冲输出 */
    if (req != NULL && req->response != NULL && req->response->framebuffer_count > 0) {
        struct limine_framebuffer *lfb = req->response->framebuffers[0];
        fb_base = (uint32_t *)(uintptr_t)lfb->address;
        fb_width = (uint32_t)lfb->width;
        fb_height = (uint32_t)lfb->height;
        fb_pitch = (uint32_t)(lfb->pitch / 4);
        cursor_x = 0;
        cursor_y = 0;

        /* 清屏 */
        clear_screen();
    }

    /* 启用帧缓冲输出 - 同时输出到串口和屏幕 */
    console_output_disabled = 0;
}

void putchar(char c) {
    if (fb_base == NULL) return;
    
    if (c == '\n') {
        cursor_x = 0;
        cursor_y += CHAR_HEIGHT;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\t') {
        cursor_x = (cursor_x + CHAR_WIDTH * 4) & ~(CHAR_WIDTH * 4 - 1);
    } else {
        draw_char(c, cursor_x, cursor_y);
        cursor_x += CHAR_WIDTH;
    }
    
    if (cursor_x >= fb_width) {
        cursor_x = 0;
        cursor_y += CHAR_HEIGHT;
    }
    
    if (cursor_y + CHAR_HEIGHT > fb_height) {
        scroll();
        cursor_y -= CHAR_HEIGHT;
    }
}

/* 禁用控制台输出到屏幕 (仍写入日志) */
void console_disable_output(void) {
    console_output_disabled = 1;
}

/* 启用控制台输出 */
void console_enable_output(void) {
    console_output_disabled = 0;
}

void print(const char *str) {
    /* 输出到串口 */
    serial_print(str);
    
    /* 写入日志缓冲区 */
    klog_write(str);
    
    /* 输出到屏幕帧缓冲区 - 带颜色增强 (ERROR红色, FAIL红色, WARNING黄色, [OK]绿色) */
    if (fb_base != NULL && !console_output_disabled) {
        const char *p = str;
        while (*p) {
            if (p == str || *(p-1) == '\n') {
                if (p[0]=='E' && p[1]=='R' && p[2]=='R' && p[3]=='O' && p[4]=='R') {
                    uint32_t saved = fg_color;
                    fg_color = COLOR_ERROR;
                    while (*p && *p != '\n') putchar(*p++);
                    fg_color = saved;
                    if (*p == '\n') putchar(*p++);
                    continue;
                }
                if (p[0]=='F' && p[1]=='A' && p[2]=='I' && p[3]=='L') {
                    uint32_t saved = fg_color;
                    fg_color = COLOR_ERROR;
                    while (*p && *p != '\n') putchar(*p++);
                    fg_color = saved;
                    if (*p == '\n') putchar(*p++);
                    continue;
                }
                if (p[0]=='W' && p[1]=='A' && p[2]=='R' && p[3]=='N') {
                    uint32_t saved = fg_color;
                    fg_color = COLOR_WARNING;
                    while (*p && *p != '\n') putchar(*p++);
                    fg_color = saved;
                    if (*p == '\n') putchar(*p++);
                    continue;
                }
                if (p[0]=='[' && p[1]=='O' && p[2]=='K' && p[3]==']') {
                    uint32_t saved = fg_color;
                    fg_color = COLOR_SUCCESS;
                    while (*p && *p != '\n') putchar(*p++);
                    fg_color = saved;
                    if (*p == '\n') putchar(*p++);
                    continue;
                }
            }
            putchar(*p++);
        }
    }
}

void println(const char *str) {
    print(str);
    /* 只有在未禁用输出时才输出换行到屏幕 */
    if (!console_output_disabled) {
        putchar('\n');
    } else {
        /* 禁用输出时，仍然写入串口和日志的换行 */
        serial_print("\n");
        klog_write("\n");
    }
}

void print_hex(uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
    
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[value & 0xF];
        value >>= 4;
    }
    buf[16] = '\0';
    
    print("0x");
    print(buf);
}

void print_dec(int64_t value) {
    char buf[21];
    int i = 20;
    int neg = 0;
    
    buf[i] = '\0';
    
    /* 特殊处理 INT64_MIN，因为 -INT64_MIN 会溢出 */
    if (value == (-9223372036854775807LL - 1LL)) {
        print("-9223372036854775808");
        return;
    }
    
    if (value < 0) {
        neg = 1;
        value = -value;
    }
    
    if (value == 0) {
        buf[--i] = '0';
    } else {
        while (value > 0) {
            buf[--i] = '0' + (value % 10);
            value /= 10;
        }
    }
    
    if (neg) {
        buf[--i] = '-';
    }
    
    print(&buf[i]);
}

/* printk - Linux 内核风格格式化打印 */
void printk(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);

    int i = 0;
    while (*fmt && i < 1020) {
        if (*fmt == '%' && *(fmt+1)) {
            fmt++;
            /* Parse flags: '0' for zero-pad */
            int zero_pad = 0;
            int field_width = 0;
            if (*fmt == '0') { zero_pad = 1; fmt++; }
            /* Parse field width */
            while (*fmt >= '0' && *fmt <= '9') {
                field_width = field_width * 10 + (*fmt - '0');
                fmt++;
            }
            switch (*fmt) {
            case 's': {
                const char *s = va_arg(args, const char *);
                if (s) {
                    while (*s && i < 1020) buf[i++] = *s++;
                }
                break;
            }
            case 'd': case 'i': {
                int val = va_arg(args, int);
                if (val < 0) { buf[i++] = '-'; val = -val; }
                char tmp[12]; int t = 0;
                if (val == 0) tmp[t++] = '0';
                while (val > 0) { tmp[t++] = '0' + (val % 10); val /= 10; }
                while (t < field_width && i < 1020) { buf[i++] = zero_pad ? '0' : ' '; t++; }
                while (t > 0) buf[i++] = tmp[--t];
                break;
            }
            case 'x': {
                unsigned int val = va_arg(args, unsigned int);
                char tmp[10]; int t = 0;
                if (val == 0) { tmp[t++] = '0'; }
                while (val > 0) {
                    int d = val % 16;
                    tmp[t++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
                    val /= 16;
                }
                while (t < field_width && i < 1020) { buf[i++] = '0'; t++; }
                while (t > 0) buf[i++] = tmp[--t];
                break;
            }
            case 'X': {
                unsigned int val = va_arg(args, unsigned int);
                char tmp[10]; int t = 0;
                if (val == 0) { tmp[t++] = '0'; }
                while (val > 0) {
                    int d = val % 16;
                    tmp[t++] = (d < 10) ? ('0' + d) : ('A' + d - 10);
                    val /= 16;
                }
                while (t < field_width && i < 1020) { buf[i++] = '0'; t++; }
                while (t > 0) buf[i++] = tmp[--t];
                break;
            }
            case 'p': {
                void *ptr = va_arg(args, void *);
                unsigned long val = (unsigned long)ptr;
                buf[i++] = '0'; buf[i++] = 'x';
                for (int sh = 60; sh >= 0; sh -= 4) {
                    int d = (val >> sh) & 0xF;
                    buf[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
                }
                break;
            }
            case 'l':
                if (*(fmt+1) == 'l') { fmt++;
                    if (*(fmt+1) == 'x') { fmt++;
                        unsigned long long val = va_arg(args, unsigned long long);
                        char tmp[18]; int t = 0;
                        if (val == 0) { buf[i++] = '0'; break; }
                        while (val > 0) {
                            int d = val % 16;
                            tmp[t++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
                            val /= 16;
                        }
                        while (t < field_width && i < 1020) { buf[i++] = '0'; t++; }
                        while (t > 0) buf[i++] = tmp[--t];
                    } else if (*(fmt+1) == 'u') { fmt++;
                        unsigned long long val = va_arg(args, unsigned long long);
                        char tmp[22]; int t = 0;
                        if (val == 0) tmp[t++] = '0';
                        while (val > 0) { tmp[t++] = '0' + (val % 10); val /= 10; }
                        while (t > 0) buf[i++] = tmp[--t];
                    }
                }
                break;
            case 'u': {
                unsigned int val = va_arg(args, unsigned int);
                char tmp[12]; int t = 0;
                if (val == 0) tmp[t++] = '0';
                while (val > 0) { tmp[t++] = '0' + (val % 10); val /= 10; }
                while (t > 0) buf[i++] = tmp[--t];
                break;
            }
            case '%': buf[i++] = '%'; break;
            default: buf[i++] = '%'; buf[i++] = *fmt; break;
            }
        } else {
            buf[i++] = *fmt;
        }
        fmt++;
    }
    buf[i] = '\0';
    va_end(args);
    print(buf);
}

void clear_screen(void) {
    if (!fb_base) return;
    for (uint32_t y = 0; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            fb_base[y * fb_pitch + x] = bg_color;
        }
    }
    cursor_x = 0;
    cursor_y = 0;
}

/* 重置光标位置 */
void console_reset_cursor(void) {
    cursor_x = 0;
    cursor_y = 0;
}

void console_set_color(uint32_t fg, uint32_t bg) {
    fg_color = fg;
    bg_color = bg;
}

/* ========== 现代化调试输出函数 ========== */

/* 绘制填充矩形 */
__attribute__((unused)) static void draw_rect_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t py = y; py < y + h && py < fb_height; py++) {
        for (uint32_t px = x; px < x + w && px < fb_width; px++) {
            fb_base[py * fb_pitch + px] = color;
        }
    }
}

/* 绘制水平线 */
static void draw_hline(uint32_t x, uint32_t y, uint32_t len, uint32_t color) {
    for (uint32_t i = 0; i < len && x + i < fb_width; i++) {
        fb_base[y * fb_pitch + x + i] = color;
    }
}

/* 在指定位置绘制字符（带颜色） */
static void draw_char_colored(char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
    if (c < 32 || c > 126) return;
    
    const uint8_t *glyph = font[c - 32];
    for (uint32_t row = 0; row < CHAR_HEIGHT; row++) {
        for (uint32_t col = 0; col < CHAR_WIDTH; col++) {
            uint32_t color = (glyph[row] & (0x40 >> col)) ? fg : bg;
            draw_pixel(x + col, y + row, color);
        }
    }
}

/* 在指定位置打印字符串 */
static void print_at(const char *str, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
    while (*str) {
        draw_char_colored(*str, x, y, fg, bg);
        x += CHAR_WIDTH;
        str++;
    }
}

/* 自定义字符串长度函数 */
static int strlen_custom(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

/* 打印带状态图标的日志 */
void console_log_status(int level, const char *tag, const char *msg) {
    if (fb_base == NULL) return;
    
    /* 定义颜色 */
    uint32_t icon_color, tag_color;
    const char *icon;
    
    switch (level) {
        case 0: /* SUCCESS */
            icon_color = COLOR_SUCCESS;
            tag_color = COLOR_SUCCESS;
            icon = "[OK]";
            break;
        case 1: /* INFO */
            icon_color = COLOR_INFO;
            tag_color = COLOR_INFO;
            icon = "[..]";
            break;
        case 2: /* WARNING */
            icon_color = COLOR_WARNING;
            tag_color = COLOR_WARNING;
            icon = "[!!]";
            break;
        case 3: /* ERROR */
            icon_color = COLOR_ERROR;
            tag_color = COLOR_ERROR;
            icon = "[XX]";
            break;
        case 4: /* DEBUG */
            icon_color = COLOR_SECONDARY;
            tag_color = COLOR_SECONDARY;
            icon = "[--]";
            break;
        default:
            icon_color = COLOR_TEXT_SECONDARY;
            tag_color = COLOR_TEXT_SECONDARY;
            icon = "[??]";
    }
    
    /* 打印图标 */
    print_at(icon, cursor_x, cursor_y, icon_color, bg_color);
    cursor_x += CHAR_WIDTH * 5;
    
    /* 打印标签 */
    if (tag && tag[0]) {
        print_at(tag, cursor_x, cursor_y, tag_color, bg_color);
        cursor_x += CHAR_WIDTH * (strlen_custom(tag) + 1);
    }
    
    /* 打印消息 */
    fg_color = COLOR_TEXT_PRIMARY;
    print(msg);
    putchar('\n');
    
    /* 重置颜色 */
    fg_color = COLOR_TEXT_PRIMARY;
}

/* 打印阶段标题 */
void console_print_header(const char *title) {
    if (fb_base == NULL) return;
    
    /* 保存当前颜色 */
    uint32_t saved_fg = fg_color;
    uint32_t saved_bg = bg_color;
    
    /* 空行 */
    putchar('\n');
    
    /* 打印带颜色的标题 */
    fg_color = COLOR_PRIMARY;
    print("=== ");
    fg_color = COLOR_TEXT_PRIMARY;
    print(title);
    fg_color = COLOR_PRIMARY;
    println(" ===");
    
    /* 恢复颜色 */
    fg_color = saved_fg;
    bg_color = saved_bg;
}

/* 打印分隔线 */
void console_print_separator(void) {
    if (fb_base == NULL) return;
    
    fg_color = COLOR_TEXT_MUTED;
    for (int i = 0; i < 60; i++) {
        putchar('-');
    }
    putchar('\n');
    fg_color = COLOR_TEXT_PRIMARY;
}

/* 打印键值对 */
void console_print_kv(const char *key, const char *value) {
    if (fb_base == NULL) return;
    
    fg_color = COLOR_TEXT_SECONDARY;
    print("  ");
    print(key);
    print(": ");
    fg_color = COLOR_TEXT_PRIMARY;
    println(value);
}

/* 打印十六进制键值对 */
void console_print_kv_hex(const char *key, uint64_t value) {
    if (fb_base == NULL) return;
    
    fg_color = COLOR_TEXT_SECONDARY;
    print("  ");
    print(key);
    print(": ");
    fg_color = COLOR_INFO;
    print_hex(value);
    putchar('\n');
    fg_color = COLOR_TEXT_PRIMARY;
}

/* 打印十进制键值对 */
void console_print_kv_dec(const char *key, int64_t value) {
    if (fb_base == NULL) return;
    
    fg_color = COLOR_TEXT_SECONDARY;
    print("  ");
    print(key);
    print(": ");
    fg_color = COLOR_TEXT_PRIMARY;
    print_dec(value);
    putchar('\n');
}

/* 打印进度条 */
void console_print_progress(int percent, const char *label) {
    if (fb_base == NULL) return;
    
    /* 打印标签 */
    fg_color = COLOR_TEXT_SECONDARY;
    print("  ");
    print(label);
    print(" [");
    
    /* 绘制进度条背景 */
    int bar_width = 30;
    int filled = (percent * bar_width) / 100;
    
    fg_color = COLOR_SUCCESS;
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) {
            putchar('#');
        } else {
            putchar('-');
        }
    }
    
    fg_color = COLOR_TEXT_SECONDARY;
    print("] ");
    fg_color = COLOR_TEXT_PRIMARY;
    print_dec(percent);
    println("%");
}

/* 打印成功消息 */
void console_success(const char *msg) {
    console_log_status(0, NULL, msg);
}

/* 打印信息消息 */
void console_info(const char *msg) {
    console_log_status(1, NULL, msg);
}

/* 打印警告消息 */
void console_warning(const char *msg) {
    console_log_status(2, NULL, msg);
}

/* 打印错误消息 */
void console_error(const char *msg) {
    console_log_status(3, NULL, msg);
}

/* 打印调试消息 */
void console_debug(const char *msg) {
    console_log_status(4, NULL, msg);
}

/* ========== 简洁彩色调试输出 API (实机无串口时直接显示在屏幕上) ==========
 * 输出到屏幕和串口, 自动加换行, 用颜色区分级别:
 *   screen_error -> 红色 (致命错误)
 *   screen_warn  -> 黄色 (警告)
 *   screen_info  -> 白色 (普通信息)
 *   screen_ok    -> 绿色 (成功) */

static void screen_print_color(uint32_t color, const char *prefix, const char *msg) {
    /* 串口输出 (带前缀) */
    if (prefix) serial_print(prefix);
    serial_print(msg);
    serial_print("\n");

    /* 日志缓冲区 */
    if (prefix) klog_write(prefix);
    klog_write(msg);
    klog_write("\n");

    /* 屏幕输出: 直接以指定颜色绘制 */
    if (fb_base != NULL && !console_output_disabled) {
        uint32_t saved = fg_color;
        fg_color = color;
        if (prefix) {
            const char *p = prefix;
            while (*p) putchar(*p++);
        }
        const char *p = msg;
        while (*p) putchar(*p++);
        putchar('\n');
        fg_color = saved;
    }
}

/* 错误信息: 红色 */
void screen_error(const char *msg) {
    screen_print_color(COLOR_ERROR, "[ERROR] ", msg);
}

/* 警告信息: 黄色 */
void screen_warn(const char *msg) {
    screen_print_color(COLOR_WARNING, "[WARN]  ", msg);
}

/* 普通信息: 白色 */
void screen_info(const char *msg) {
    screen_print_color(COLOR_TEXT_PRIMARY, "[INFO]  ", msg);
}

/* 成功信息: 绿色 */
void screen_ok(const char *msg) {
    screen_print_color(COLOR_SUCCESS, "[OK]    ", msg);
}

/* ========== 内核日志文件功能 ========== */

/* 日志缓冲区大小 (256KB) */
#define KLOG_BUFFER_SIZE    (256 * 1024)

/* 日志缓冲区 */
static char *klog_buffer = NULL;
static size_t klog_pos = 0;
static int klog_enabled = 0;
static int klog_wrapped = 0;  /* 是否已回绕 */

/* 全局日志级别 - 低于此级别的消息不输出到屏幕 */
static z_log_level_t klog_screen_level = Z_LOG_LEVEL_INFO;
/* 串口日志级别 - 低于此级别的消息不输出到串口 */
static z_log_level_t klog_serial_level = Z_LOG_LEVEL_DEBUG;

/* 设置屏幕日志级别 */
void klog_set_screen_level(z_log_level_t level) {
    klog_screen_level = level;
}

/* 设置串口日志级别 */
void klog_set_serial_level(z_log_level_t level) {
    klog_serial_level = level;
}

/* 获取当前屏幕日志级别 */
z_log_level_t klog_get_screen_level(void) {
    return klog_screen_level;
}

/* 日志级别名称 - 中文版 */
static const char *klog_level_name(z_log_level_t level) {
    switch (level) {
        case Z_LOG_LEVEL_TRACE: return "跟踪";
        case Z_LOG_LEVEL_DEBUG: return "调试";
        case Z_LOG_LEVEL_INFO:  return "信息";
        case Z_LOG_LEVEL_WARN:  return "警告";
        case Z_LOG_LEVEL_ERROR: return "错误";
        case Z_LOG_LEVEL_FATAL: return "致命";
        default:                return "未知";
    }
}

/* 日志级别对应颜色 */
static uint32_t klog_level_color(z_log_level_t level) {
    switch (level) {
        case Z_LOG_LEVEL_TRACE: return Z_COLOR_GRAY;
        case Z_LOG_LEVEL_DEBUG: return Z_COLOR_CYAN;
        case Z_LOG_LEVEL_INFO:  return Z_COLOR_GREEN;
        case Z_LOG_LEVEL_WARN:  return Z_COLOR_YELLOW;
        case Z_LOG_LEVEL_ERROR: return Z_COLOR_RED;
        case Z_LOG_LEVEL_FATAL: return Z_COLOR_MAGENTA;
        default:                return Z_COLOR_WHITE;
    }
}

/* 初始化日志系统 */
void klog_init(void) {
    /* 分配日志缓冲区 (使用内核堆) */
    extern void *kmalloc(size_t size);
    klog_buffer = (char *)kmalloc(KLOG_BUFFER_SIZE);
    if (klog_buffer) {
        klog_pos = 0;
        klog_enabled = 1;
        
        /* 写入日志头 */
        const char *header = "=== Zratro OS 内核启动日志 ===\n";
        while (*header && klog_pos < KLOG_BUFFER_SIZE - 1) {
            klog_buffer[klog_pos++] = *header++;
        }
        
        /* 调试信息：记录缓冲区地址 */
        int i = 0;
        const char *prefix = "[日志] 缓冲区地址 0x";
        while (prefix[i] && klog_pos < KLOG_BUFFER_SIZE - 1) {
            klog_buffer[klog_pos++] = prefix[i++];
        }
        
        /* 写入地址 */
        uint64_t addr = (uint64_t)klog_buffer;
        const char hex[] = "0123456789ABCDEF";
        for (int j = 60; j >= 0 && klog_pos < KLOG_BUFFER_SIZE - 1; j -= 4) {
            klog_buffer[klog_pos++] = hex[(addr >> j) & 0xF];
        }
        if (klog_pos < KLOG_BUFFER_SIZE - 1) {
            klog_buffer[klog_pos++] = '\n';
        }
        klog_buffer[klog_pos] = '\0';
    } else {
        klog_enabled = 0;
        klog_pos = 0;
    }
}

/* 内部函数: 写入日志缓冲区 (环形缓冲区) */
static void klog_write(const char *str) {
    if (!klog_enabled || !klog_buffer) return;
    
    while (*str) {
        klog_buffer[klog_pos] = *str++;
        klog_pos++;
        if (klog_pos >= KLOG_BUFFER_SIZE - 1) {
            klog_pos = 0;
            klog_wrapped = 1;
        }
    }
    klog_buffer[klog_pos] = '\0';
}

/* 刷新日志缓冲区到串口 */
void klog_flush_to_file(void) {
    if (!klog_enabled || !klog_buffer || klog_pos == 0) {
        return;
    }
    
    /* 通过串口输出完整日志供宿主机捕获 */
    if (klog_buffer && klog_pos > 0) {
        serial_print(klog_buffer);
    }
}

/* 关闭日志系统 */
void klog_close(void) {
    /* 日志系统已简化，无需关闭文件 */
}

/* 获取日志缓冲区大小 */
size_t klog_get_size(void) {
    return klog_pos;
}

/* 检查日志系统是否启用 */
int klog_is_enabled(void) {
    return klog_enabled && klog_buffer != NULL;
}

/* 获取日志缓冲区指针和大小 (用于调试导出) */
const char *klog_get_buffer(size_t *size_out) {
    if (size_out) {
        *size_out = klog_pos;
    }
    return klog_buffer;
}

/* 打印日志统计信息 */
void klog_print_stats(void) {
    println("=== 内核日志统计 ===");
    print("  缓冲区地址: 0x");
    print_hex((uint64_t)klog_buffer);
    println("");
    print("  缓冲区大小: ");
    print_dec(KLOG_BUFFER_SIZE);
    println(" 字节");
    print("  已使用: ");
    print_dec(klog_pos);
    println(" 字节");
    print("  可用空间: ");
    print_dec(KLOG_BUFFER_SIZE - klog_pos);
    println(" 字节");
    print("  已回绕: ");
    println(klog_wrapped ? "是" : "否");
}

/* ========== z_log: 统一日志级别输出 ========== */

/*
 * z_log - 带级别控制的内核日志
 *
 * 根据 klog_screen_level / klog_serial_level 决定是否输出到屏幕/串口。
 * 始终写入 klog 环形缓冲区。
 *
 * 注意: 这是简化版 (不支持可变参数格式化)，fmt 为纯字符串。
 * 后续可扩展为支持 %d/%x/%s 等格式符。
 */
void z_log(z_log_level_t level, const char *fmt, ...) {
    /* 始终写入环形日志缓冲区 */
    klog_write("[");
    klog_write(klog_level_name(level));
    klog_write("] ");
    klog_write(fmt);
    klog_write("\n");

    /* 串口输出 */
    if (level >= klog_serial_level) {
        serial_print("[");
        serial_print(klog_level_name(level));
        serial_print("] ");
        serial_print(fmt);
        serial_print("\n");
    }

    /* 屏幕输出 */
    if (level >= klog_screen_level) {
        uint32_t saved_fg = fg_color;
        console_set_color(klog_level_color(level), Z_COLOR_BLACK);
        print("[");
        print(klog_level_name(level));
        print("] ");
        console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
        println(fmt);
        fg_color = saved_fg;
    }
}

/* z_log_raw - 不带级别前缀的原始日志 */
void z_log_raw(const char *fmt, ...) {
    klog_write(fmt);
    serial_print(fmt);
    print(fmt);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ZRATRO UNIQUE STYLE FUNCTIONS - 独特风格函数实现
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Zratro 独特区块输出 - 开始 */
void z_print_block_start(const char *title) {
    println("");
    print("┌──────────────────────────────────────────────────────────────────────────┐");
    print("│ ");
    console_set_color(Z_COLOR_AURORA_GREEN, Z_COLOR_BLACK);
    print(title);
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
    println("                                          │");
    print("├──────────────────────────────────────────────────────────────────────────┤");
}

/* Zratro 独特区块输出 - 结束 */
void z_print_block_end(const char *title) {
    (void)title;
    print("└──────────────────────────────────────────────────────────────────────────┘");
    println("");
}

/* Zratro 独特分隔线 */
void z_print_separator_line(char pattern, int length) {
    for (int i = 0; i < length; i++) {
        putchar(pattern);
    }
    println("");
}

/* Zratro 独特键值输出 */
void z_print_key_value(const char *key, const char *value, uint32_t key_color) {
    console_set_color(key_color, Z_COLOR_BLACK);
    print("│   ");
    print(key);
    print(": ");
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
    println(value);
}

/* Zratro 独特键值输出 (十六进制) */
void z_print_key_value_hex(const char *key, uint64_t value, uint32_t key_color) {
    console_set_color(key_color, Z_COLOR_BLACK);
    print("│   ");
    print(key);
    print(": ");
    console_set_color(Z_COLOR_CYAN, Z_COLOR_BLACK);
    print("0x");
    print_hex(value);
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
    println("");
}

/* Zratro 独特键值输出 (十进制) */
void z_print_key_value_dec(const char *key, int64_t value, uint32_t key_color) {
    console_set_color(key_color, Z_COLOR_BLACK);
    print("│   ");
    print(key);
    print(": ");
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
    print_dec(value);
    println("");
}

/* Zratro 独特时间戳输出 */
void z_print_timestamped(const char *msg) {
    extern uint64_t get_tick_count(void);
    uint64_t ticks = get_tick_count();
    
    console_set_color(Z_COLOR_GRAY, Z_COLOR_BLACK);
    print("[");
    print_dec((int64_t)(ticks / 1000));
    print(".");
    print_dec((int64_t)(ticks % 1000));
    print("s] ");
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
    println(msg);
}

/* Zratro 独特进度输出 - 开始 */
void z_progress_start(const char *operation) {
    console_set_color(Z_COLOR_AURORA_BLUE, Z_COLOR_BLACK);
    print("◇ ");
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
    print(operation);
    println("...");
}

/* Zratro 独特进度输出 - 更新 */
void z_progress_update(int percent, const char *status) {
    /* 进度条宽度 */
    int bar_width = 30;
    int filled = (percent * bar_width) / 100;
    
    print("\r│   [");
    console_set_color(Z_COLOR_AURORA_GREEN, Z_COLOR_BLACK);
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) {
            print("█");
        } else {
            print("░");
        }
    }
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
    print("] ");
    print_dec(percent);
    print("% ");
    print(status);
    print("    ");
}

/* Zratro 独特进度输出 - 结束 */
void z_progress_end(int success, const char *result) {
    println("");
    if (success) {
        console_set_color(Z_COLOR_GREEN, Z_COLOR_BLACK);
        print("│   ✓ ");
    } else {
        console_set_color(Z_COLOR_RED, Z_COLOR_BLACK);
        print("│   ✗ ");
    }
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
    println(result);
}

/* Zratro 独特十六进制转储 */
void z_dump_hex(const void *data, size_t len, const char *label) {
    const uint8_t *bytes = (const uint8_t *)data;
    
    console_set_color(Z_COLOR_PURPLE, Z_COLOR_BLACK);
    print("╔══ DUMP: ");
    print(label);
    println(" ══════════════════════════════════════════════════╗");
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
    
    for (size_t i = 0; i < len; i += 16) {
        /* 偏移 */
        console_set_color(Z_COLOR_GRAY, Z_COLOR_BLACK);
        print("║ ");
        print_hex((uint64_t)i);
        print(": ");
        
        /* 十六进制 */
        console_set_color(Z_COLOR_CYAN, Z_COLOR_BLACK);
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t b = bytes[i + j];
            if (b < 0x10) print("0");
            print_hex(b);
            print(" ");
            if (j == 7) print(" ");
        }
        
        /* ASCII */
        console_set_color(Z_COLOR_GREEN, Z_COLOR_BLACK);
        print(" |");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t b = bytes[i + j];
            if (b >= 32 && b < 127) {
                putchar(b);
            } else {
                print(".");
            }
        }
        console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
        println("|");
    }
    
    console_set_color(Z_COLOR_PURPLE, Z_COLOR_BLACK);
    println("╚════════════════════════════════════════════════════════════════════╝");
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
}

/* Zratro 独特栈转储 */
void z_dump_stack(const void *stack, size_t depth) {
    const uint64_t *ptrs = (const uint64_t *)stack;
    
    console_set_color(Z_COLOR_ORANGE, Z_COLOR_BLACK);
    println("╔══ STACK DUMP ════════════════════════════════════════════════════╗");
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
    
    for (size_t i = 0; i < depth; i++) {
        print("║ [RSP+");
        print_dec((int64_t)(i * 8));
        print("] ");
        
        uint64_t val = ptrs[i];
        /* 检查是否可能是内核地址 */
        if (val >= 0xFFFFFFFF80000000ULL) {
            console_set_color(Z_COLOR_AURORA_GREEN, Z_COLOR_BLACK);
        } else if (val >= 0x1000 && val < 0x100000) {
            console_set_color(Z_COLOR_YELLOW, Z_COLOR_BLACK);
        } else {
            console_set_color(Z_COLOR_CYAN, Z_COLOR_BLACK);
        }
        print_hex(val);
        console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
        println("");
    }
    
    console_set_color(Z_COLOR_ORANGE, Z_COLOR_BLACK);
    println("╚══════════════════════════════════════════════════════════════════╝");
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
}

/* Zratro 独特寄存器转储 */
void z_dump_registers(const void *regs) {
    const uint64_t *r = (const uint64_t *)regs;
    static const char *reg_names[] = {
        "RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP",
        "R8",  "R9",  "R10", "R11", "R12", "R13", "R14", "R15",
        "RIP", "RFL", "CR2", "CR3"
    };
    
    console_set_color(Z_COLOR_MAGENTA, Z_COLOR_BLACK);
    println("╔══ REGISTER STATE ═════════════════════════════════════════════════╗");
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
    
    for (int i = 0; i < 20 && reg_names[i]; i++) {
        print("║ ");
        console_set_color(Z_COLOR_YELLOW, Z_COLOR_BLACK);
        print(reg_names[i]);
        print("=");
        console_set_color(Z_COLOR_CYAN, Z_COLOR_BLACK);
        print("0x");
        print_hex(r[i]);
        console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
        
        /* 每行两个寄存器 */
        if (i % 2 == 1) {
            println("");
        } else {
            print("  ");
        }
    }
    
    console_set_color(Z_COLOR_MAGENTA, Z_COLOR_BLACK);
    println("╚══════════════════════════════════════════════════════════════════╝");
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
}

/* Zratro 独特表格输出 - 开始 */
void z_table_start(const char *headers[], int col_count, int col_widths[]) {
    print("┌");
    for (int i = 0; i < col_count; i++) {
        for (int j = 0; j < col_widths[i]; j++) print("─");
        if (i < col_count - 1) print("┬");
    }
    println("┐");
    
    print("│");
    for (int i = 0; i < col_count; i++) {
        console_set_color(Z_COLOR_AURORA_GREEN, Z_COLOR_BLACK);
        print(" ");
        print(headers[i]);
        console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
        /* 填充空格 */
        int len = 0;
        while (headers[i][len]) len++;
        for (int j = len; j < col_widths[i] - 1; j++) print(" ");
        print("│");
    }
    println("");
    
    print("├");
    for (int i = 0; i < col_count; i++) {
        for (int j = 0; j < col_widths[i]; j++) print("─");
        if (i < col_count - 1) print("┼");
    }
    println("┤");
}

/* Zratro 独特表格输出 - 行 */
void z_table_row(const char *values[], int col_count) {
    print("│");
    for (int i = 0; i < col_count; i++) {
        print(" ");
        print(values[i]);
        print(" │");
    }
    println("");
}

/* Zratro 独特表格输出 - 结束 */
void z_table_end(void) {
    print("└");
    /* 默认 3 列，每列 20 宽度 */
    int col_widths[] = {20, 20, 20};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < col_widths[i]; j++) print("─");
        if (i < 2) print("┴");
    }
    println("┘");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * KERNEL PANIC - 遇错即终止
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 禁用 CPU 中断 */
static inline void cpu_disable_interrupts(void) {
    asm volatile("cli");
}

/* 永久停止 CPU */
static inline void cpu_halt_forever(void) {
    while (1) {
        asm volatile("hlt");
    }
}

/* 致命错误 - 立即停止内核
 * 参数: msg - 错误信息
 * 该函数永不返回
 */
__attribute__((noreturn))
void kernel_panic(const char *msg) {
    /* 禁用中断 */
    cpu_disable_interrupts();

    /* ===== 最早期诊断: 直接 outb 串口 + 直接 framebuffer 全屏红色 =====
     * 完全不依赖 console/heap/任何子系统, 防止 panic 内部自己再炸 */
    {
        const char *banner = "\n\n*** KERNEL PANIC ***\n";
        for (const char *p = banner; *p; p++) {
            while ((inb(0x3F8 + 5) & 0x20) == 0);
            outb(0x3F8, (uint8_t)*p);
        }
        if (msg) {
            for (const char *p = msg; *p; p++) {
                while ((inb(0x3F8 + 5) & 0x20) == 0);
                outb(0x3F8, (uint8_t)*p);
            }
        }
        const char *tail = "\n*** SYSTEM HALTED - DO NOT RESTART ***\n";
        for (const char *p = tail; *p; p++) {
            while ((inb(0x3F8 + 5) & 0x20) == 0);
            outb(0x3F8, (uint8_t)*p);
        }

        /* 把屏幕涂成纯红, 用户在实机能直接看到 panic */
        extern volatile struct limine_framebuffer_request fb_request;
        struct limine_framebuffer_response *resp = fb_request.response;
        if (resp != (void*)0 && resp->framebuffer_count > 0) {
            struct limine_framebuffer *lfb = resp->framebuffers[0];
            if (lfb && lfb->address) {
                volatile uint32_t *fb = (volatile uint32_t *)lfb->address;
                uint64_t pp = lfb->pitch / 4;
                uint64_t w = lfb->width;
                uint64_t h = lfb->height;
                /* 红色背景 */
                for (uint64_t y = 0; y < h; y++)
                    for (uint64_t x = 0; x < w; x++)
                        fb[y * pp + x] = 0x00FF0000U;
            }
        }
    }

    /* 输出错误信息到屏幕和串口 (console 路径, 可能依赖未初始化的子系统) */
    console_set_color(Z_COLOR_RED, Z_COLOR_BLACK);
    println("");
    println("╔══════════════════════════════════════════════════════════════════╗");
    println("║                    内核恐慌 - 致命错误                            ║");
    println("╚══════════════════════════════════════════════════════════════════╝");
    console_set_color(Z_COLOR_WHITE, Z_COLOR_BLACK);
    print("  错误: ");
    println(msg ? msg : "未知错误");
    println("");
    println("  系统已停止。请重新启动计算机。");
    println("");

    /* 永久停止 - 不重启! */
    cpu_halt_forever();

    /* 永不返回 */
    __builtin_unreachable();
}
