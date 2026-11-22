#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stddef.h>

/* 像素格式 */
typedef enum {
    FB_FORMAT_UNKNOWN = 0,
    FB_FORMAT_RGB565,       /* 16位: R5 G6 B5 */
    FB_FORMAT_RGB888,       /* 24位: R8 G8 B8 */
    FB_FORMAT_XRGB8888,     /* 32位: X8 R8 G8 B8 */
    FB_FORMAT_ARGB8888,     /* 32位: A8 R8 G8 B8 */
    FB_FORMAT_BGR888,       /* 24位: B8 G8 R8 */
    FB_FORMAT_XBGR8888,     /* 32位: X8 B8 G8 R8 */
    FB_FORMAT_ABGR8888,     /* 32位: A8 B8 G8 R8 */
} fb_format_t;

/* 颜色 (ARGB32) */
typedef uint32_t color_t;

#define COLOR_R(c) (((c) >> 16) & 0xFF)
#define COLOR_G(c) (((c) >> 8) & 0xFF)
#define COLOR_B(c) ((c) & 0xFF)
#define COLOR_A(c) (((c) >> 24) & 0xFF)

#define MAKE_COLOR(r, g, b, a) (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))
#define MAKE_COLOR_RGB(r, g, b) MAKE_COLOR(r, g, b, 255)

/* 预定义颜色 */
#define COLOR_BLACK       MAKE_COLOR_RGB(0, 0, 0)
#define COLOR_WHITE       MAKE_COLOR_RGB(255, 255, 255)
#define COLOR_RED         MAKE_COLOR_RGB(255, 0, 0)
#define COLOR_GREEN       MAKE_COLOR_RGB(0, 255, 0)
#define COLOR_BLUE        MAKE_COLOR_RGB(0, 0, 255)
#define COLOR_YELLOW      MAKE_COLOR_RGB(255, 255, 0)
#define COLOR_CYAN        MAKE_COLOR_RGB(0, 255, 255)
#define COLOR_MAGENTA     MAKE_COLOR_RGB(255, 0, 255)
#define COLOR_GRAY        MAKE_COLOR_RGB(128, 128, 128)
#define COLOR_LIGHTGRAY   MAKE_COLOR_RGB(192, 192, 192)
#define COLOR_DARKGRAY    MAKE_COLOR_RGB(64, 64, 64)
#define COLOR_TRANSPARENT MAKE_COLOR(0, 0, 0, 0)

/* Framebuffer 信息 */
struct fb_info {
    void *buffer;           /* 显存地址 */
    uint32_t width;         /* 宽度 */
    uint32_t height;        /* 高度 */
    uint32_t pitch;         /* 每行字节数 */
    uint32_t bpp;           /* 每像素位数 */
    fb_format_t format;     /* 像素格式 */
    
    /* 物理地址 */
    uint64_t physical;
    uint64_t size;
    
    /* 双缓冲 */
    void *back_buffer;
    int double_buffered;
};

/* 矩形区域 */
struct fb_rect {
    int x, y;
    int width, height;
};

/* 点 */
struct fb_point {
    int x, y;
};

/* 初始化 framebuffer */
int fb_init(void);

/* 获取 framebuffer 信息 */
struct fb_info *fb_get_info(void);

/* 设置像素 */
void fb_set_pixel(int x, int y, color_t color);

/* 获取像素 */
color_t fb_get_pixel(int x, int y);

/* 绘制直线 */
void fb_draw_line(int x1, int y1, int x2, int y2, color_t color);

/* 绘制矩形 */
void fb_draw_rect(int x, int y, int w, int h, color_t color);
void fb_fill_rect(int x, int y, int w, int h, color_t color);

/* 绘制圆形 */
void fb_draw_circle(int cx, int cy, int r, color_t color);
void fb_fill_circle(int cx, int cy, int r, color_t color);

/* 清屏 */
void fb_clear(color_t color);

/* 复制区域 */
void fb_copy(int dst_x, int dst_y, int src_x, int src_y, int w, int h);

/* 滚动 */
void fb_scroll(int lines, color_t bg_color);

/* 双缓冲 */
void fb_swap_buffers(void);
void fb_set_double_buffer(int enable);

/* 绘制图像 */
void fb_draw_image(int x, int y, const uint32_t *image, int w, int h);
void fb_draw_image_scaled(int x, int y, int w, int h, const uint32_t *image, int iw, int ih);
void fb_draw_image_alpha(int x, int y, const uint32_t *image, int w, int h, uint8_t alpha);

/* 字体渲染 */
void fb_draw_char(int x, int y, char c, color_t fg, color_t bg);
void fb_draw_text(int x, int y, const char *text, color_t fg, color_t bg);
int fb_text_width(const char *text);
int fb_text_height(void);

/* 颜色转换 */
uint32_t fb_color_to_native(color_t color);
color_t fb_color_from_native(uint32_t native);

/* Limine framebuffer 支持 */
int fb_init_limine(void);

#endif
