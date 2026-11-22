/*
 * desk.h - ZratroOS Desktop Environment 主头文件
 * 
 * 现代化桌面环境，支持：
 * - 高级动画系统（淡入淡出、滑动、缩放、弹性）
 * - 窗口管理器（多窗口、拖拽、调整大小）
 * - 任务栏（开始菜单、系统托盘、时钟）
 * - 桌面图标
 * - 系统监控窗口
 * 
 * 作者：ZratroOS 开发团队
 * 日期：2026-04-12
 */

#ifndef DESK_H
#define DESK_H

#include <stdint.h>
#include <stddef.h>

/* ========== 类型定义 ========== */
typedef uint32_t color_t;

/* ========== 颜色系统 ========== */
#define MAKE_COLOR(r,g,b)     (((r) << 16) | ((g) << 8) | (b))
#define MAKE_COLORA(r,g,b,a)  (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))
#define COLOR_R(c) (((c) >> 16) & 0xFF)
#define COLOR_G(c) (((c) >> 8) & 0xFF)
#define COLOR_B(c) ((c) & 0xFF)
#define COLOR_A(c) (((c) >> 24) & 0xFF)

/* 现代配色方案 */
#define COLOR_BG_DARKEST  MAKE_COLOR(18, 18, 22)
#define COLOR_BG_DARK     MAKE_COLOR(28, 28, 34)
#define COLOR_BG          MAKE_COLOR(38, 38, 46)
#define COLOR_BG_LIGHT    MAKE_COLOR(52, 52, 64)
#define COLOR_BG_LIGHTER  MAKE_COLOR(72, 72, 86)
#define COLOR_ACCENT      MAKE_COLOR(90, 131, 242)   /* 现代蓝 */
#define COLOR_ACCENT2     MAKE_COLOR(166, 113, 239)  /* 紫 */
#define COLOR_ACCENT3     MAKE_COLOR(128, 226, 190)  /* 绿 */
#define COLOR_ACCENT4     MAKE_COLOR(247, 179, 112)  /* 橙 */
#define COLOR_TEXT        MAKE_COLOR(220, 220, 230)
#define COLOR_TEXT_DIM    MAKE_COLOR(140, 140, 160)
#define COLOR_WHITE       MAKE_COLOR(255, 255, 255)
#define COLOR_BLACK       MAKE_COLOR(0, 0, 0)
#define COLOR_RED         MAKE_COLOR(255, 85, 85)
#define COLOR_GREEN       MAKE_COLOR(80, 220, 120)
#define COLOR_YELLOW      MAKE_COLOR(241, 250, 140)

/* ========== 矩形 ========== */
struct rect {
    int x, y, w, h;
};

/* ========== 动画系统 ========== */
typedef enum {
    ANIM_NONE = 0,
    ANIM_FADE_IN,
    ANIM_FADE_OUT,
    ANIM_SLIDE_LEFT,
    ANIM_SLIDE_RIGHT,
    ANIM_SLIDE_UP,
    ANIM_SLIDE_DOWN,
    ANIM_SCALE_IN,
    ANIM_SCALE_OUT,
    ANIM_BOUNCE,
    ANIM_ELASTIC,
} anim_type_t;

typedef enum {
    EASE_LINEAR = 0,
    EASE_IN,
    EASE_OUT,
    EASE_IN_OUT,
    EASE_ELASTIC,
    EASE_BOUNCE,
} easing_t;

struct animation {
    anim_type_t type;
    easing_t easing;
    uint64_t start_time;
    uint64_t duration_ms;
    float progress;      /* 0.0 - 1.0 */
    int done;
    
    /* 值动画 */
    float from_val;
    float to_val;
    float current_val;
};

/* ========== 窗口系统 ========== */
#define MAX_WINDOWS     32
#define WIN_TITLE_MAX   128

typedef enum {
    WIN_HIDDEN = 0,
    WIN_VISIBLE,
    WIN_MINIMIZED,
    WIN_MAXIMIZED,
    WIN_CLOSING,  /* 退出动画中 */
} win_state_t;

#define WIN_FLAG_BORDER     0x01
#define WIN_FLAG_TITLEBAR   0x02
#define WIN_FLAG_RESIZABLE  0x04
#define WIN_FLAG_TOPMOST    0x08
#define WIN_FLAG_SHADOW     0x10

struct window {
    uint32_t id;
    char title[WIN_TITLE_MAX];
    struct rect bounds;
    struct rect prev_bounds;  /* 最大化前位置 */
    
    uint32_t flags;
    win_state_t state;
    
    int has_focus;
    int is_hovered;
    int z_order;
    
    /* 窗口内容帧缓冲 */
    uint32_t *content;
    int content_w, content_h;
    
    /* 窗口圆角 */
    int corner_radius;
    
    /* 窗口颜色 */
    color_t titlebar_color;
    color_t bg_color;
    color_t border_color;
    
    /* 动画 */
    struct animation open_anim;
    struct animation close_anim;
    struct animation move_anim;
    
    /* 回调 */
    void (*on_draw)(struct window *win, uint32_t *fb, int pitch);
    void (*on_click)(struct window *win, int x, int y, int button);
    void (*on_key)(struct window *win, uint32_t keycode);
};

/* ========== GUI 组件 ========== */

/* 按钮 */
struct gui_button {
    struct rect bounds;
    char text[64];
    color_t bg_color;
    color_t hover_color;
    color_t text_color;
    int is_hovered;
    int is_pressed;
    int corner_radius;
    void (*on_click)(void *ctx);
    void *ctx;
    
    /* 动画 */
    struct animation hover_anim;
};

/* 菜单项 */
struct menu_item {
    char text[64];
    int is_separator;
    void (*on_click)(void *ctx);
    void *ctx;
};

/* 弹出菜单 */
struct popup_menu {
    struct rect bounds;
    struct menu_item items[32];
    int item_count;
    int hover_idx;
    int visible;
    struct animation anim;
};

/* 任务栏 */
struct taskbar {
    struct rect bounds;
    int height;
    
    /* 开始按钮 */
    struct gui_button start_btn;
    
    /* 窗口按钮 */
    struct {
        uint32_t win_id;
        char title[64];
        int active;
        struct rect bounds;
    } win_btns[MAX_WINDOWS];
    int win_btn_count;
    
    /* 系统托盘区 */
    struct rect tray_area;
    
    /* 时钟 */
    struct rect clock_area;
    
    /* 动画 */
    struct animation show_anim;
};

/* 桌面图标 */
struct desktop_icon {
    struct rect bounds;
    char label[64];
    int icon_type;  /* 0=folder, 1=file, 2=terminal, 3=monitor */
    int is_selected;
    int is_hovered;
};

/* ========== 全局桌面状态 ========== */
struct desktop_state {
    /* 屏幕 */
    uint32_t *fb;
    int fb_w, fb_h, fb_pitch;
    
    /* 窗口 */
    struct window *windows[MAX_WINDOWS];
    int window_count;
    uint32_t next_win_id;
    
    struct window *focused_win;
    struct window *hovered_win;
    struct window *drag_win;
    int drag_offset_x, drag_offset_y;
    
    /* 桌面背景 */
    struct desktop_icon icons[32];
    int icon_count;
    
    /* 任务栏 */
    struct taskbar taskbar;
    
    /* 开始菜单 */
    struct popup_menu start_menu;
    
    /* 鼠标 */
    int mouse_x, mouse_y;
    int mouse_visible;
    int mouse_left, mouse_right;
    int mouse_l_pressed, mouse_r_pressed;
    
    /* 动画 */
    struct animation boot_anim;  /* 启动动画 */
    int animations_running;
    
    /* 时间 */
    uint64_t tick_ms;
    
    /* 运行标志 */
    int running;
    
    /* 双缓冲 */
    uint32_t *back_buffer;
};

extern struct desktop_state g_desk;

/* ========== 函数声明 ========== */

/* 系统调用 */
void sys_exit(int status);
ssize_t sys_write(int fd, const void *buf, size_t count);
ssize_t sys_read(int fd, void *buf, size_t count);
int sys_open(const char *path, int flags, int mode);
int sys_close(int fd);
int sys_mmap(void **addr, size_t length, int prot, int flags, int fd, uint64_t offset);

/* 内存 */
void *calloc(size_t nmemb, size_t size);
void free(void *ptr);
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
char *strcpy(char *dst, const char *src);
int strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, int n);
char *strncpy(char *dst, const char *src, int n);

/* 图形绘制 */
void fb_fill_rect(uint32_t *fb, int pitch, int x, int y, int w, int h, color_t color);
void fb_draw_rect_border(uint32_t *fb, int pitch, int x, int y, int w, int h, color_t color, int thickness);
void fb_draw_rounded_rect(uint32_t *fb, int pitch, int x, int y, int w, int h, int radius, color_t color);
void fb_fill_rounded_rect(uint32_t *fb, int pitch, int x, int y, int w, int h, int radius, color_t color);
void fb_draw_line(uint32_t *fb, int pitch, int x1, int y1, int x2, int y2, color_t color);
void fb_draw_circle(uint32_t *fb, int pitch, int cx, int cy, int r, color_t color);
void fb_fill_circle(uint32_t *fb, int pitch, int cx, int cy, int r, color_t color);
void fb_draw_text(uint32_t *fb, int pitch, int x, int y, const char *text, color_t color);
void fb_draw_text_center(uint32_t *fb, int pitch, int x, int y, int width, const char *text, color_t color);
void fb_blend_alpha(uint32_t *fb, int pitch, int x, int y, int w, int h, const uint32_t *src, uint8_t alpha);
void fb_blit(uint32_t *fb, int pitch, int dst_x, int dst_y, int w, int h, const uint32_t *src, int src_pitch);
int fb_text_width(const char *text);

/* 渐变 */
void fb_fill_rect_gradient_h(uint32_t *fb, int pitch, int x, int y, int w, int h, color_t c1, color_t c2);
void fb_fill_rect_gradient_v(uint32_t *fb, int pitch, int x, int y, int w, int h, color_t c1, color_t c2);

/* 阴影 */
void fb_draw_shadow(uint32_t *fb, int pitch, int x, int y, int w, int h, int radius, uint8_t alpha);

/* 动画 */
void anim_start(struct animation *anim, anim_type_t type, uint64_t duration_ms);
void anim_start_val(struct animation *anim, anim_type_t type, uint64_t duration_ms, float from, float to);
float anim_update(struct animation *anim, uint64_t now);
float ease(float t, easing_t type);

/* 窗口管理 */
struct window *win_create(const char *title, int x, int y, int w, int h, uint32_t flags);
void win_destroy(struct window *win);
void win_show(struct window *win);
void win_hide(struct window *win);
void win_focus(struct window *win);
void win_move(struct window *win, int x, int y);
void win_resize(struct window *win, int w, int h);
void win_draw(struct window *win);
struct window *win_find(uint32_t id);
struct window *win_hit_test(int x, int y);

/* GUI 组件 */
void btn_draw(struct gui_button *btn, uint32_t *fb, int pitch);
void btn_update(struct gui_button *btn, int mx, int my, int pressed);
void menu_draw(struct popup_menu *menu, uint32_t *fb, int pitch);
void menu_update(struct popup_menu *menu, int mx, int my);

/* 任务栏 */
void taskbar_init(struct taskbar *tb);
void taskbar_draw(struct taskbar *tb, uint32_t *fb, int pitch);
void taskbar_update(struct taskbar *tb, int mx, int my, int pressed);
void taskbar_update_time(struct taskbar *tb);

/* 桌面图标 */
void desktop_icons_init(void);
void desktop_icons_draw(uint32_t *fb, int pitch);
void desktop_icons_update(int mx, int my);

/* 背景绘制 */
void draw_desktop_background(uint32_t *fb, int pitch);
void draw_boot_animation(uint32_t *fb, int pitch, float progress);

/* 合成器 */
void desktop_composite(void);

/* 事件处理 */
void desktop_handle_mouse(int x, int y, int button, int pressed);
void desktop_handle_key(uint32_t keycode, int pressed);

/* 系统监控 */
void sysmon_draw(struct window *win, uint32_t *fb, int pitch);

/* 终端模拟 */
void terminal_draw(struct window *win, uint32_t *fb, int pitch);
void terminal_key(struct window *win, uint32_t keycode);

/* 文件管理器 (zrrExfileManager 集成) */
void filemanager_open(const char *path);
void filemanager_draw(struct window *win, uint32_t *fb, int pitch);
void filemanager_key(struct window *win, uint32_t keycode);
void filemanager_click(struct window *win, int x, int y, int button);

/* 入口 */
void _start(void);

#endif /* DESK_H */
