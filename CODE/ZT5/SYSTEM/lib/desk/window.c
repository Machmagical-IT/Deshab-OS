/*
 * Zratro OS - Window Manager
 * 
 * 窗口管理器 - GUI 核心组件
 * 
 * @file window.c
 * @brief 窗口管理器实现
 * 
 * 功能:
 *   - 多窗口管理
 *   - 窗口装饰 (标题栏/边框)
 *   - 焦点管理
 *   - 图层合成
 *   - 事件分发
 */

#include <stdint.h>
#include <stddef.h>

#define NULL ((void*)0)

static char wm_pool[8 * 1024 * 1024];
static int wm_pool_pos = 0;

static void *wm_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    total = (total + 15) & ~(size_t)15;
    if (wm_pool_pos + total > sizeof(wm_pool)) return NULL;
    void *p = &wm_pool[wm_pool_pos];
    wm_pool_pos += total;
    for (size_t i = 0; i < total; i++) ((char*)p)[i] = 0;
    return p;
}

static void wm_free(void *ptr) {
    (void)ptr;
}

static void wm_strncpy(char *dst, const char *src, size_t n) {
    while (n > 1 && *src) { *dst++ = *src++; n--; }
    if (n > 0) *dst = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 基础数据结构
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_WINDOWS     64
#define WINDOW_TITLE_MAX 128

/**
 * @brief 颜色结构 (32 位 ARGB)
 */
struct color {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t a;
};

/**
 * @brief 矩形区域
 */
struct rect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

/**
 * @brief 窗口状态
 */
typedef enum {
    WIN_STATE_HIDDEN    = 0,
    WIN_STATE_VISIBLE   = 1,
    WIN_STATE_MINIMIZED = 2,
    WIN_STATE_MAXIMIZED = 3,
    WIN_STATE_FULLSCREEN = 4,
} window_state_t;

/**
 * @brief 窗口标志
 */
#define WIN_FLAG_BORDER     0x01    /* 显示边框 */
#define WIN_FLAG_TITLEBAR   0x02    /* 显示标题栏 */
#define WIN_FLAG_RESIZABLE  0x04    /* 可调整大小 */
#define WIN_FLAG_TOPMOST    0x08    /* 总在最上 */
#define WIN_FLAG_TRANSPARENT 0x10   /* 透明窗口 */

/**
 * @brief 窗口结构
 */
struct window {
    uint64_t id;                  /* 窗口 ID */
    char title[WINDOW_TITLE_MAX]; /* 窗口标题 */
    
    struct rect bounds;           /* 窗口位置和大小 */
    struct rect client_rect;      /* 客户区位置 */
    
    uint32_t flags;               /* 窗口标志 */
    uint32_t state;               /* 窗口状态 */
    
    void *framebuffer;            /* 窗口帧缓冲 */
    size_t fb_size;               /* 帧缓冲大小 */
    
    int32_t z_order;              /* Z 轴顺序 */
    uint64_t owner_pid;           /* 所有者进程 */
    
    struct window *parent;        /* 父窗口 */
    struct window *next;          /* 下一个兄弟窗口 */
    struct window *children;      /* 子窗口列表 */
    
    int has_focus;                /* 是否有焦点 */
    int is_hovered;               /* 鼠标是否悬停 */
    
    struct color bg_color;        /* 背景色 */
    struct color border_color;    /* 边框颜色 */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 窗口管理器全局状态
 * ═══════════════════════════════════════════════════════════════════════════ */

static struct {
    struct window *windows[MAX_WINDOWS];
    int window_count;
    
    struct window *focused_window;
    struct window *hovered_window;
    struct window *capture_window;  /* 鼠标捕获窗口 */
    
    void *screen_buffer;
    int screen_width;
    int screen_height;
    int screen_pitch;
    
    int cursor_x;
    int cursor_y;
    int cursor_visible;
} wm_state = {0};

/* ═══════════════════════════════════════════════════════════════════════════
 * 内部辅助函数
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 分配新窗口 ID
 */
static uint64_t allocate_window_id(void) {
    static uint64_t next_id = 1;
    return next_id++;
}

/**
 * @brief 查找空闲窗口槽位
 */
static int find_free_window_slot(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (wm_state.windows[i] == NULL) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief 按 Z-order 排序窗口
 */
static void sort_windows_by_zorder(void) {
    /* 简单的冒泡排序 */
    for (int i = 0; i < wm_state.window_count - 1; i++) {
        for (int j = 0; j < wm_state.window_count - i - 1; j++) {
            if (wm_state.windows[j]->z_order > wm_state.windows[j + 1]->z_order) {
                struct window *temp = wm_state.windows[j];
                wm_state.windows[j] = wm_state.windows[j + 1];
                wm_state.windows[j + 1] = temp;
            }
        }
    }
}

/**
 * @brief 计算客户区矩形 (减去标题栏和边框)
 */
static void calculate_client_rect(struct window *win) {
    win->client_rect = win->bounds;
    
    if (win->flags & WIN_FLAG_BORDER) {
        win->client_rect.x += 2;
        win->client_rect.y += 2;
        win->client_rect.width -= 4;
        win->client_rect.height -= 4;
    }
    
    if (win->flags & WIN_FLAG_TITLEBAR) {
        win->client_rect.y += 24;  /* 标题栏高度 */
        win->client_rect.height -= 24;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 窗口管理 API
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 初始化窗口管理器
 */
int wm_init(void *framebuffer, int width, int height, int pitch) {
    wm_state.screen_buffer = framebuffer;
    wm_state.screen_width = width;
    wm_state.screen_height = height;
    wm_state.screen_pitch = pitch;
    wm_state.window_count = 0;
    wm_state.cursor_visible = 1;
    
    return 0;
}

/**
 * @brief 创建新窗口
 */
uint64_t wm_create_window(const char *title, int x, int y, 
                          int width, int height, uint32_t flags) {
    int slot = find_free_window_slot();
    if (slot < 0) {
        return 0;  /* 窗口表已满 */
    }
    
    struct window *win = wm_calloc(1, sizeof(struct window));
    if (!win) {
        return 0;
    }
    
    win->id = allocate_window_id();
    wm_strncpy(win->title, title, WINDOW_TITLE_MAX - 1);
    
    win->bounds.x = x;
    win->bounds.y = y;
    win->bounds.width = width;
    win->bounds.height = height;
    
    win->flags = flags;
    win->state = WIN_STATE_VISIBLE;
    win->z_order = wm_state.window_count;
    
    /* 默认颜色 */
    win->bg_color.r = 45;
    win->bg_color.g = 45;
    win->bg_color.b = 45;
    win->bg_color.a = 255;
    
    win->border_color.r = 0;
    win->border_color.g = 120;
    win->border_color.b = 210;
    win->border_color.a = 255;
    
    calculate_client_rect(win);
    
    /* 分配窗口帧缓冲 */
    win->fb_size = width * height * 4;  /* 32 位色 */
    win->framebuffer = wm_calloc(1, win->fb_size);
    
    wm_state.windows[slot] = win;
    wm_state.window_count++;
    
    sort_windows_by_zorder();
    
    return win->id;
}

/**
 * @brief 销毁窗口
 */
int wm_destroy_window(uint64_t window_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *win = wm_state.windows[i];
        if (win && win->id == window_id) {
            /* 释放资源 */
            if (win->framebuffer) {
                wm_free(win->framebuffer);
            }
            
            /* 从数组中移除 */
            wm_state.windows[i] = NULL;
            wm_state.window_count--;
            
            /* 更新焦点 */
            if (wm_state.focused_window == win) {
                wm_state.focused_window = NULL;
            }
            
            wm_free(win);
            return 0;
        }
    }
    
    return -1;
}

/**
 * @brief 显示窗口
 */
int wm_show_window(uint64_t window_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *win = wm_state.windows[i];
        if (win && win->id == window_id) {
            win->state = WIN_STATE_VISIBLE;
            return 0;
        }
    }
    return -1;
}

/**
 * @brief 隐藏窗口
 */
int wm_hide_window(uint64_t window_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *win = wm_state.windows[i];
        if (win && win->id == window_id) {
            win->state = WIN_STATE_HIDDEN;
            return 0;
        }
    }
    return -1;
}

/**
 * @brief 设置窗口焦点
 */
int wm_set_focus(uint64_t window_id) {
    /* 清除当前焦点 */
    if (wm_state.focused_window) {
        wm_state.focused_window->has_focus = 0;
    }
    
    /* 设置新焦点 */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *win = wm_state.windows[i];
        if (win && win->id == window_id) {
            wm_state.focused_window = win;
            win->has_focus = 1;
            
            /* 将窗口置顶 */
            win->z_order = MAX_WINDOWS;
            sort_windows_by_zorder();
            
            return 0;
        }
    }
    
    return -1;
}

/**
 * @brief 移动窗口
 */
int wm_move_window(uint64_t window_id, int x, int y) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *win = wm_state.windows[i];
        if (win && win->id == window_id) {
            win->bounds.x = x;
            win->bounds.y = y;
            calculate_client_rect(win);
            return 0;
        }
    }
    return -1;
}

/**
 * @brief 调整窗口大小
 */
int wm_resize_window(uint64_t window_id, int width, int height) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *win = wm_state.windows[i];
        if (win && win->id == window_id) {
            win->bounds.width = width;
            win->bounds.height = height;
            
            /* 重新分配帧缓冲 */
            if (win->framebuffer) {
                wm_free(win->framebuffer);
            }
            win->fb_size = width * height * 4;
            win->framebuffer = wm_calloc(1, win->fb_size);
            
            calculate_client_rect(win);
            return 0;
        }
    }
    return -1;
}

/**
 * @brief 最大化窗口
 */
int wm_maximize_window(uint64_t window_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *win = wm_state.windows[i];
        if (win && win->id == window_id) {
            win->state = WIN_STATE_MAXIMIZED;
            win->bounds.x = 0;
            win->bounds.y = 0;
            win->bounds.width = wm_state.screen_width;
            win->bounds.height = wm_state.screen_height;
            calculate_client_rect(win);
            return 0;
        }
    }
    return -1;
}

/**
 * @brief 最小化窗口
 */
int wm_minimize_window(uint64_t window_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *win = wm_state.windows[i];
        if (win && win->id == window_id) {
            win->state = WIN_STATE_MINIMIZED;
            return 0;
        }
    }
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 绘图函数
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 绘制像素
 */
static void draw_pixel(void *fb, int x, int y, int pitch, struct color c) {
    if (x < 0 || x >= wm_state.screen_width ||
        y < 0 || y >= wm_state.screen_height) {
        return;
    }
    
    uint32_t *pixel = (uint32_t *)((uint8_t *)fb + y * pitch + x * 4);
    *pixel = (c.a << 24) | (c.r << 16) | (c.g << 8) | c.b;
}

/**
 * @brief 绘制矩形
 */
static void draw_rect(void *fb, struct rect *r, int pitch, struct color c) {
    for (int y = r->y; y < r->y + r->height; y++) {
        for (int x = r->x; x < r->x + r->width; x++) {
            draw_pixel(fb, x, y, pitch, c);
        }
    }
}

/**
 * @brief 绘制字符串 (简化版)
 */
static void draw_text(void *fb, int x, int y, int pitch, 
                      const char *text, struct color c) {
    /* TODO: 实现字体渲染 */
    (void)fb;
    (void)x;
    (void)y;
    (void)pitch;
    (void)text;
    (void)c;
}

/**
 * @brief 绘制窗口装饰
 */
static void draw_window_decoration(struct window *win) {
    struct color titlebar_color = {
        .r = 0, .g = 120, .b = 210, .a = 255
    };
    
    struct color border_color = win->border_color;
    
    /* 绘制标题栏 */
    if (win->flags & WIN_FLAG_TITLEBAR) {
        struct rect titlebar = {
            .x = win->bounds.x,
            .y = win->bounds.y,
            .width = win->bounds.width,
            .height = 24
        };
        draw_rect(wm_state.screen_buffer, &titlebar, 
                  wm_state.screen_pitch, titlebar_color);
        
        /* 绘制窗口标题 */
        draw_text(wm_state.screen_buffer, 
                  win->bounds.x + 8, win->bounds.y + 4,
                  wm_state.screen_pitch, win->title, 
                  (struct color){.r=255, .g=255, .b=255, .a=255});
    }
    
    /* 绘制边框 */
    if (win->flags & WIN_FLAG_BORDER) {
        struct rect border = win->bounds;
        border.width = 2;
        draw_rect(wm_state.screen_buffer, &border, 
                  wm_state.screen_pitch, border_color);
        
        border.x = win->bounds.x + win->bounds.width - 2;
        draw_rect(wm_state.screen_buffer, &border, 
                  wm_state.screen_pitch, border_color);
    }
}

/**
 * @brief 合成所有窗口并输出到屏幕
 */
void wm_composite(void) {
    /* 清屏 */
    struct color bg = {.r=30, .g=30, .b=30, .a=255};
    struct rect screen = {
        .x = 0, .y = 0, 
        .width = wm_state.screen_width,
        .height = wm_state.screen_height
    };
    draw_rect(wm_state.screen_buffer, &screen, wm_state.screen_pitch, bg);
    
    /* 按 Z-order 绘制所有可见窗口 */
    for (int i = 0; i < wm_state.window_count; i++) {
        struct window *win = wm_state.windows[i];
        if (!win || win->state != WIN_STATE_VISIBLE) {
            continue;
        }
        
        /* 绘制窗口内容 */
        if (win->framebuffer) {
            /* TODO: 将窗口帧缓冲混合到屏幕 */
        }
        
        /* 绘制窗口装饰 */
        draw_window_decoration(win);
    }
    
    /* 绘制鼠标光标 */
    if (wm_state.cursor_visible) {
        /* TODO: 绘制光标 */
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 事件处理
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 处理鼠标移动事件
 */
void wm_handle_mouse_move(int x, int y) {
    wm_state.cursor_x = x;
    wm_state.cursor_y = y;
    
    /* 检测悬停窗口 */
    for (int i = wm_state.window_count - 1; i >= 0; i--) {
        struct window *win = wm_state.windows[i];
        if (!win || win->state != WIN_STATE_VISIBLE) {
            continue;
        }
        
        if (x >= win->bounds.x && x < win->bounds.x + win->bounds.width &&
            y >= win->bounds.y && y < win->bounds.y + win->bounds.height) {
            
            if (!win->is_hovered) {
                win->is_hovered = 1;
                wm_state.hovered_window = win;
            }
            break;
        } else {
            win->is_hovered = 0;
        }
    }
}

/**
 * @brief 处理鼠标点击事件
 */
void wm_handle_mouse_click(int button, int pressed) {
    if (pressed && wm_state.hovered_window) {
        /* 设置焦点 */
        wm_set_focus(wm_state.hovered_window->id);
        
        /* 检测是否点击标题栏 (移动窗口) */
        if (wm_state.hovered_window->flags & WIN_FLAG_TITLEBAR) {
            int titlebar_y = wm_state.hovered_window->bounds.y;
            if (wm_state.cursor_y >= titlebar_y && 
                wm_state.cursor_y < titlebar_y + 24) {
                /* TODO: 开始拖动窗口 */
            }
        }
    }
}

/**
 * @brief 处理键盘事件
 */
void wm_handle_key_event(uint32_t keycode, int pressed) {
    if (!wm_state.focused_window) {
        return;
    }
    
    /* TODO: 将键盘事件发送给焦点窗口 */
    (void)keycode;
    (void)pressed;
}
