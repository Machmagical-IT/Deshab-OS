/*
 * desk_integration.h - Desktop Integration for zrrWindowsManager and zrrExfileManager
 * 
 * 集成窗口管理器和文件管理器到桌面环境
 */

#ifndef DESK_INTEGRATION_H
#define DESK_INTEGRATION_H

#include "desk.h"
#include "../zrrWindowsManager/zrrWindowsManager.h"
#include "../zrrExfileManager/zrrExfileManager.h"

/* ========== 集成函数 ========== */

/**
 * @brief 使用 zrrWindowsManager 创建窗口
 */
static inline struct window *desk_create_window(const char *title, int x, int y, int w, int h) {
    wm_window_t *wm_win = wm_create_window(title, x, y, w, h, WM_WIN_FLAG_DEFAULT);
    if (!wm_win) return 0;
    
    /* 转换为 desk window */
    struct window *desk_win = (struct window*)calloc(1, sizeof(struct window));
    if (!desk_win) return 0;
    
    desk_win->id = wm_win->id;
    strcpy(desk_win->title, title);
    desk_win->bounds.x = x;
    desk_win->bounds.y = y;
    desk_win->bounds.w = w;
    desk_win->bounds.h = h;
    desk_win->flags = WIN_FLAG_BORDER | WIN_FLAG_TITLEBAR | WIN_FLAG_SHADOW;
    desk_win->state = WIN_HIDDEN;
    desk_win->corner_radius = 10;
    desk_win->titlebar_color = MAKE_COLOR(46, 110, 187);
    
    return desk_win;
}

/**
 * @brief 打开文件管理器窗口
 */
static inline void desk_open_file_manager(const char *path) {
    /* 创建文件管理器窗口 */
    struct window *fm_win = desk_create_window("File Manager", 100, 80, 700, 450);
    if (!fm_win) return;
    
    /* 初始化文件管理器上下文 */
    exfile_context_t *fm_ctx = (exfile_context_t*)calloc(1, sizeof(exfile_context_t));
    if (!fm_ctx) return;
    
    /* 分配内容帧缓冲 */
    int content_w = fm_win->bounds.w - 4;
    int content_h = fm_win->bounds.h - 32;
    fm_win->content = (uint32_t*)calloc(1, content_w * content_h * 4);
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
    fm_win->user_data = fm_ctx;
    
    /* 显示窗口 */
    win_show(fm_win);
}

/**
 * @brief 初始化桌面集成
 */
static inline void desk_integration_init(void) {
    /* 初始化窗口管理器 */
    extern struct desktop_state g_desk;
    wm_init(g_desk.fb, g_desk.fb_w, g_desk.fb_h, g_desk.fb_pitch);
}

#endif /* DESK_INTEGRATION_H */
