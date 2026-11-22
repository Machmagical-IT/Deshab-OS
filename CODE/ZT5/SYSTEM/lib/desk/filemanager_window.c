/*
 * filemanager_window.c - File Manager Window for Desk
 * 
 * 文件管理器窗口回调
 */

#include "include/desk.h"
#include "../zrrExfileManager/zrrExfileManager.h"

/* 存储文件管理器上下文（最多支持 MAX_WINDOWS 个实例）*/
static exfile_context_t *fm_contexts[MAX_WINDOWS];

/* 通过窗口指针找到上下文索引 */
static int fm_find_slot(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!fm_contexts[i]) return i;
    }
    return -1;
}

static exfile_context_t *fm_get_ctx(struct window *win) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (fm_contexts[i]) {
            /* 简单匹配：通过窗口 ID */
            return fm_contexts[i];
        }
    }
    return 0;
}

/* 文件管理器窗口回调 */

void filemanager_draw(struct window *win, uint32_t *fb, int pitch) {
    if (!win) return;
    
    exfile_context_t *ctx = fm_get_ctx(win);
    if (!ctx) return;
    
    /* 更新帧缓冲指针 */
    if (win->content) {
        ctx->fb = win->content;
        ctx->pitch = win->content_w * 4;
    }
    
    /* 渲染文件管理器 */
    exfile_render(ctx);
}

void filemanager_key(struct window *win, uint32_t keycode) {
    if (!win) return;
    exfile_context_t *ctx = fm_get_ctx(win);
    if (!ctx) return;
    exfile_handle_key(ctx, keycode, 1);
}

void filemanager_click(struct window *win, int x, int y, int button) {
    if (!win) return;
    exfile_context_t *ctx = fm_get_ctx(win);
    if (!ctx) return;
    exfile_handle_mouse_click(ctx, x, y, button);
}

void filemanager_open(const char *path) {
    /* 创建文件管理器窗口 */
    int win_x = 150;
    int win_y = 100;
    int win_w = 750;
    int win_h = 480;
    
    struct window *fm_win = win_create("File Manager", win_x, win_y, win_w, win_h,
                                       WIN_FLAG_BORDER | WIN_FLAG_TITLEBAR | WIN_FLAG_SHADOW);
    if (!fm_win) return;
    
    /* 分配文件管理器上下文 */
    int slot = fm_find_slot();
    if (slot < 0) return;
    
    exfile_context_t *fm_ctx = (exfile_context_t*)calloc(1, sizeof(exfile_context_t));
    if (!fm_ctx) return;
    
    fm_contexts[slot] = fm_ctx;
    
    /* 分配内容帧缓冲 */
    int content_w = win_w - 4;
    int content_h = win_h - 32;
    fm_win->content = (uint32_t*)calloc(1, content_w * content_h * 4);
    if (!fm_win->content) return;
    
    fm_win->content_w = content_w;
    fm_win->content_h = content_h;
    
    /* 初始化文件管理器 */
    exfile_init(fm_ctx, fm_win->content, content_w * 4,
                0, 0, content_w, content_h);
    
    /* 加载初始目录 */
    if (path) {
        exfile_navigate(fm_ctx, path);
    } else {
        exfile_navigate(fm_ctx, "/");
    }
    
    /* 设置回调 */
    fm_win->on_draw = filemanager_draw;
    fm_win->on_key = filemanager_key;
    fm_win->on_click = filemanager_click;
    
    /* 显示窗口 */
    win_show(fm_win);
}
