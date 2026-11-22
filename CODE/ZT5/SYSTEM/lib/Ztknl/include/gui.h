#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include <stddef.h>

/* 颜色格式 (ARGB32) */
typedef uint32_t color_t;

#define COLOR(r, g, b, a) (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))

/* 预定义颜色 */
#define COLOR_BLACK       COLOR(0, 0, 0, 255)
#define COLOR_WHITE       COLOR(255, 255, 255, 255)
#define COLOR_RED         COLOR(255, 0, 0, 255)
#define COLOR_GREEN       COLOR(0, 255, 0, 255)
#define COLOR_BLUE        COLOR(0, 0, 255, 255)
#define COLOR_YELLOW      COLOR(255, 255, 0, 255)
#define COLOR_CYAN        COLOR(0, 255, 255, 255)
#define COLOR_MAGENTA     COLOR(255, 0, 255, 255)
#define COLOR_GRAY        COLOR(128, 128, 128, 255)
#define COLOR_LIGHTGRAY   COLOR(192, 192, 192, 255)
#define COLOR_DARKGRAY    COLOR(64, 64, 64, 255)
#define COLOR_TRANSPARENT COLOR(0, 0, 0, 0)

/* 事件类型 */
typedef enum {
    EVENT_NONE = 0,
    EVENT_MOUSE_MOVE,
    EVENT_MOUSE_DOWN,
    EVENT_MOUSE_UP,
    EVENT_MOUSE_CLICK,
    EVENT_MOUSE_SCROLL,
    EVENT_KEY_DOWN,
    EVENT_KEY_UP,
    EVENT_KEY_PRESS,
    EVENT_RESIZE,
    EVENT_CLOSE,
    EVENT_FOCUS,
    EVENT_BLUR,
    EVENT_PAINT,
    EVENT_TIMER,
    EVENT_CUSTOM,
} event_type_t;

/* 鼠标按钮 */
#define MOUSE_BUTTON_LEFT    1
#define MOUSE_BUTTON_RIGHT   2
#define MOUSE_BUTTON_MIDDLE  4

/* 键盘修饰键 */
#define KEY_MOD_SHIFT    0x01
#define KEY_MOD_CTRL     0x02
#define KEY_MOD_ALT      0x04
#define KEY_MOD_SUPER    0x08
#define KEY_MOD_CAPSLOCK 0x10
#define KEY_MOD_NUMLOCK  0x20

/* 事件结构 */
struct gui_event {
    event_type_t type;
    int target;             /* 目标窗口 ID */
    
    union {
        struct {
            int x, y;
            int buttons;
            int scroll;
        } mouse;
        
        struct {
            uint32_t keycode;
            uint32_t keysym;
            char ch;
            int mods;
        } key;
        
        struct {
            int width, height;
        } resize;
        
        struct {
            int timer_id;
            void *data;
        } timer;
        
        struct {
            void *data1;
            void *data2;
        } custom;
    } data;
};

/* 点 */
struct point {
    int x, y;
};

/* 矩形 */
struct rect {
    int x, y;
    int width, height;
};

/* 图形上下文 */
struct graphics {
    uint32_t *buffer;       /* 像素缓冲区 */
    int width;
    int height;
    int stride;             /* 每行像素数 */
    
    /* 裁剪区域 */
    struct rect clip;
    
    /* 绘制状态 */
    color_t fill_color;
    color_t stroke_color;
    int line_width;
    
    /* 字体 */
    void *font;
    int font_size;
};

/* 窗口 */
struct window {
    int id;
    char title[64];
    struct rect bounds;
    int visible;
    int minimized;
    int maximized;
    int focused;
    
    /* 图形上下文 */
    struct graphics *gfx;
    
    /* 事件回调 */
    void (*on_event)(struct window *, struct gui_event *);
    void *user_data;
    
    /* 窗口装饰 */
    int has_frame;
    int has_title_bar;
    int resizable;
    
    /* 父/子窗口 */
    int parent_id;
    int first_child;
    int next_sibling;
};

/* 字体 */
struct font {
    char name[32];
    int size;
    int ascent;
    int descent;
    int line_height;
    void *data;
};

/* 光标 */
struct cursor {
    int x, y;
    int visible;
    uint8_t *image;
    int hot_x, hot_y;
};

/* 初始化 GUI */
int gui_init(void);

/* 清理 GUI */
void gui_cleanup(void);

/* ============ 图形操作 ============ */

/* 创建图形上下文 */
struct graphics *gfx_create(int width, int height);

/* 销毁图形上下文 */
void gfx_destroy(struct graphics *gfx);

/* 调整大小 */
int gfx_resize(struct graphics *gfx, int width, int height);

/* 清空 */
void gfx_clear(struct graphics *gfx, color_t color);

/* 填充颜色 */
void gfx_set_fill_color(struct graphics *gfx, color_t color);
void gfx_set_stroke_color(struct graphics *gfx, color_t color);

/* 绘制点 */
void gfx_draw_point(struct graphics *gfx, int x, int y, color_t color);

/* 绘制线 */
void gfx_draw_line(struct graphics *gfx, int x1, int y1, int x2, int y2);

/* 绘制矩形 */
void gfx_draw_rect(struct graphics *gfx, struct rect *rect);
void gfx_fill_rect(struct graphics *gfx, struct rect *rect);

/* 绘制圆 */
void gfx_draw_circle(struct graphics *gfx, int cx, int cy, int radius);
void gfx_fill_circle(struct graphics *gfx, int cx, int cy, int radius);

/* 绘制椭圆 */
void gfx_draw_ellipse(struct graphics *gfx, struct rect *rect);
void gfx_fill_ellipse(struct graphics *gfx, struct rect *rect);

/* 绘制多边形 */
void gfx_draw_polygon(struct graphics *gfx, struct point *points, int count);
void gfx_fill_polygon(struct graphics *gfx, struct point *points, int count);

/* 绘制文本 */
void gfx_draw_text(struct graphics *gfx, int x, int y, const char *text);
void gfx_draw_text_len(struct graphics *gfx, int x, int y, const char *text, size_t len);

/* 绘制图像 */
void gfx_draw_image(struct graphics *gfx, int x, int y, 
                    const uint32_t *image, int w, int h);
void gfx_draw_image_scaled(struct graphics *gfx, struct rect *dest,
                           const uint32_t *image, int w, int h);

/* 混合 */
void gfx_blend(struct graphics *dest, int x, int y,
               struct graphics *src, int sx, int sy, int w, int h,
               uint8_t alpha);

/* ============ 窗口管理 ============ */

/* 创建窗口 */
struct window *window_create(const char *title, int x, int y, int w, int h, int flags);

/* 销毁窗口 */
void window_destroy(struct window *win);

/* 显示/隐藏 */
void window_show(struct window *win);
void window_hide(struct window *win);

/* 移动/调整大小 */
void window_move(struct window *win, int x, int y);
void window_resize(struct window *win, int w, int h);

/* 设置标题 */
void window_set_title(struct window *win, const char *title);

/* 获取图形上下文 */
struct graphics *window_get_graphics(struct window *win);

/* 刷新窗口 */
void window_invalidate(struct window *win);
void window_invalidate_rect(struct window *win, struct rect *rect);

/* 事件处理 */
void window_set_event_handler(struct window *win,
                               void (*handler)(struct window *, struct gui_event *));

/* 窗口层级 */
void window_raise(struct window *win);
void window_lower(struct window *win);

/* 焦点 */
void window_focus(struct window *win);

/* ============ 事件系统 ============ */

/* 处理事件 */
void gui_process_events(void);

/* 等待事件 */
int gui_wait_event(struct gui_event *event, int timeout_ms);

/* 推送事件 */
void gui_push_event(struct gui_event *event);

/* 鼠标输入 */
void gui_mouse_move(int x, int y);
void gui_mouse_down(int button);
void gui_mouse_up(int button);
void gui_mouse_scroll(int delta);

/* 键盘输入 */
void gui_key_down(uint32_t keycode, uint32_t keysym);
void gui_key_up(uint32_t keycode, uint32_t keysym);

/* ============ 字体系统 ============ */

/* 加载字体 */
struct font *font_load(const char *path, int size);

/* 获取字体 */
struct font *font_get(const char *name, int size);

/* 测量文本 */
int font_text_width(struct font *font, const char *text);
int font_text_height(struct font *font);

/* ============ 主题系统 ============ */

/* 主题颜色 */
struct theme_colors {
    color_t background;
    color_t foreground;
    color_t primary;
    color_t secondary;
    color_t accent;
    color_t error;
    color_t warning;
    color_t success;
    
    /* 窗口装饰 */
    color_t window_bg;
    color_t window_fg;
    color_t title_bar_bg;
    color_t title_bar_fg;
    color_t border;
    
    /* 按钮 */
    color_t button_bg;
    color_t button_fg;
    color_t button_hover;
    color_t button_pressed;
};

/* 设置主题 */
void gui_set_theme(struct theme_colors *theme);

/* ============ 光标 ============ */

/* 设置光标 */
void gui_set_cursor(struct cursor *cursor);

/* 隐藏/显示光标 */
void gui_show_cursor(int show);

/* ============ 窗口标志 ============ */

#define WINDOW_FLAG_FRAME       0x01
#define WINDOW_FLAG_TITLE_BAR   0x02
#define WINDOW_FLAG_RESIZABLE   0x04
#define WINDOW_FLAG_MINIMIZE    0x08
#define WINDOW_FLAG_MAXIMIZE    0x10
#define WINDOW_FLAG_CLOSE       0x20
#define WINDOW_FLAG_ALWAYS_TOP  0x40
#define WINDOW_FLAG_POPUP       0x80
#define WINDOW_FLAG_FULLSCREEN  0x100

/* 默认窗口标志 */
#define WINDOW_DEFAULT_FLAGS    (WINDOW_FLAG_FRAME | WINDOW_FLAG_TITLE_BAR | \
                                 WINDOW_FLAG_RESIZABLE | WINDOW_FLAG_MINIMIZE | \
                                 WINDOW_FLAG_MAXIMIZE | WINDOW_FLAG_CLOSE)

#endif
