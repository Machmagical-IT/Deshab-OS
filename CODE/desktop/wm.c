/* wm.c — 窗口管理（win_*）+ 窗口装饰绘制（M0 自 main.c 平移）
 * Aqua：红黄绿三钮、按呈现矩形绘制、开/关/最小化/恢复/缩放动画。
 */
#include "desktop.h"

/* freestanding：大结构体赋值会被 clang 降成 memcpy。 */
void *memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    unsigned long i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
#include "aqua_draw.h"
#include "zhfont.h"

/* ---- 全局状态 ---- */
desktop_window g_windows[MAX_WINDOWS];
int g_win_count = 0;
static int g_next_win_id = 1;
int g_focused_win = -1;

/* 拖拽状态 */
int g_dragging = 0;
int g_drag_win = -1;
int g_drag_off_x, g_drag_off_y;

/* ============================================================
 *  窗口管理
 * ============================================================ */

desktop_window *win_find(int id) {
    for (int i = 0; i < g_win_count; i++) {
        if (g_windows[i].id == id) return &g_windows[i];
    }
    return 0;
}

desktop_window *win_find_at(int mx, int my) {
    for (int i = g_win_count - 1; i >= 0; i--) {
        desktop_window *w = &g_windows[i];
        if (!w->visible || w->minimized) continue;
        if (w->anim == WIN_ANIM_CLOSE || w->anim == WIN_ANIM_MIN) continue;
        if (w->ws != g_ws_cur) continue;
        if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
            return w;
        }
    }
    return 0;
}

static void win_bring_to_front(desktop_window *w) {
    int idx = -1;
    for (int i = 0; i < g_win_count; i++) {
        if (g_windows[i].id == w->id) { idx = i; break; }
    }
    if (idx < 0 || idx == g_win_count - 1) return;
    desktop_window tmp = g_windows[idx];
    for (int i = idx; i < g_win_count - 1; i++) {
        g_windows[i] = g_windows[i+1];
    }
    g_windows[g_win_count - 1] = tmp;
}

void win_focus(desktop_window *w) {
    int id = w->id;   /* bring_to_front 会重排数组，先保存 id 避免悬空指针 */
    win_bring_to_front(w);
    for (int i = 0; i < g_win_count; i++)
        g_windows[i].focused = (g_windows[i].id == id);
    g_focused_win = id;
    if (g_win_count > 0) {
        desktop_window *f = &g_windows[g_win_count - 1];
        if (f->minimized || f->anim == WIN_ANIM_MIN) {
            f->minimized = 0;
            if (g_fluent) win_anim_restore(f);
        }
    }
}

desktop_window *win_create(int app_id, const char *title, int x, int y, int w, int h) {
    if (g_win_count >= MAX_WINDOWS) return 0;
    desktop_window *win = &g_windows[g_win_count++];
    int id = g_next_win_id++;
    win->id = id;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->visible = 1;
    win->focused = 1;
    win->minimized = 0;
    win->app_id = app_id;
    win->app_state = 0;
    win->closing = 0;
    win->dirty = 1;
    win->ws = g_ws_cur;
    win->anim = WIN_ANIM_NONE;
    win->zoomed = 0;
    win->zx = win->zy = win->zw = win->zh = 0;
    for (int i = 0; i < 47 && title[i]; i++) win->title[i] = title[i];
    win->title[47] = 0;
    for (int i = 0; i < g_win_count - 1; i++) g_windows[i].focused = 0;
    g_focused_win = id;
    win_present_sync(win);
    if (g_fluent) win_anim_open(win);
    return win;
}

void win_present_sync(desktop_window *w) {
    if (!w) return;
    motion_bind(&w->present_x, w->x);
    motion_bind(&w->present_y, w->y);
    motion_bind(&w->present_w, w->w);
    motion_bind(&w->present_h, w->h);
    motion_bind(&w->present_opacity, 255);
}

void win_present_rect(const desktop_window *w, int *x, int *y, int *ww, int *hh, int *opacity) {
    if (!w) return;
    if (x) *x = motion_px(&w->present_x);
    if (y) *y = motion_px(&w->present_y);
    if (ww) *ww = motion_px(&w->present_w);
    if (hh) *hh = motion_px(&w->present_h);
    if (opacity) *opacity = motion_px(&w->present_opacity);
}

void win_present_animate_to(desktop_window *w) {
    if (!w) return;
    motion_spring_to(&w->present_x, w->x);
    motion_spring_to(&w->present_y, w->y);
    motion_spring_to(&w->present_w, w->w);
    motion_spring_to(&w->present_h, w->h);
    motion_spring_to(&w->present_opacity, 255);
}

int win_present_settled(const desktop_window *w) {
    if (!w) return 1;
    return !w->present_x.active && !w->present_y.active &&
           !w->present_w.active && !w->present_h.active &&
           !w->present_opacity.active;
}

/* ============================================================
 *  Aqua 窗口动画（开 / 关 / 最小化 / 恢复 / 缩放）
 * ============================================================ */

/* 呈现轨道吸附到逻辑矩形绕中心缩放 num/256 的位置 */
static void present_bind_scaled(desktop_window *w, int num, int opacity) {
    int sw = w->w * num / 256, sh = w->h * num / 256;
    motion_bind(&w->present_x, w->x + (w->w - sw) / 2);
    motion_bind(&w->present_y, w->y + (w->h - sh) / 2);
    motion_bind(&w->present_w, sw);
    motion_bind(&w->present_h, sh);
    motion_bind(&w->present_opacity, opacity);
}

static void present_spring_rect(desktop_window *w, int x, int y, int ww, int hh) {
    motion_spring_to(&w->present_x, x);
    motion_spring_to(&w->present_y, y);
    motion_spring_to(&w->present_w, ww);
    motion_spring_to(&w->present_h, hh);
}

/* 焦点移交给 w 之外最上层的可交互窗口 */
static void focus_next_excluding(int id) {
    int i;
    for (i = 0; i < g_win_count; i++)
        if (g_windows[i].id == id) g_windows[i].focused = 0;
    if (g_focused_win != id) return;
    g_focused_win = -1;
    for (i = g_win_count - 1; i >= 0; i--) {
        desktop_window *o = &g_windows[i];
        if (o->id == id || o->ws != g_ws_cur || !o->visible || o->minimized) continue;
        if (o->anim == WIN_ANIM_CLOSE || o->anim == WIN_ANIM_MIN) continue;
        o->focused = 1;
        g_focused_win = o->id;
        break;
    }
}

void win_anim_open(desktop_window *w) {
    present_bind_scaled(w, 236, 0);
    present_spring_rect(w, w->x, w->y, w->w, w->h);
    motion_ease_to(&w->present_opacity, 255, 160);
    w->anim = WIN_ANIM_OPEN;
}

void win_anim_close(desktop_window *w) {
    int sw, sh;
    if (!w) return;
    if (!g_fluent) { win_destroy(w); return; }
    if (w->anim == WIN_ANIM_CLOSE) return;
    sw = w->w * 230 / 256;
    sh = w->h * 230 / 256;
    motion_ease_to(&w->present_x, w->x + (w->w - sw) / 2, 160);
    motion_ease_to(&w->present_y, w->y + (w->h - sh) / 2, 160);
    motion_ease_to(&w->present_w, sw, 160);
    motion_ease_to(&w->present_h, sh, 160);
    motion_ease_to(&w->present_opacity, 0, 160);
    w->anim = WIN_ANIM_CLOSE;
    focus_next_excluding(w->id);
}

void win_anim_minimize(desktop_window *w) {
    int r[4];
    if (!w || w->minimized || w->anim == WIN_ANIM_CLOSE) return;
    if (!g_fluent) {
        w->minimized = 1;
        focus_next_excluding(w->id);
        return;
    }
    dock_icon_rect(w->app_id, r);
    present_spring_rect(w, r[0], r[1], r[2], r[3]);
    motion_ease_to(&w->present_opacity, 0, 260);
    w->anim = WIN_ANIM_MIN;
    focus_next_excluding(w->id);
}

void win_anim_restore(desktop_window *w) {
    int r[4];
    if (!w) return;
    if (w->anim != WIN_ANIM_MIN) {
        dock_icon_rect(w->app_id, r);
        motion_bind(&w->present_x, r[0]);
        motion_bind(&w->present_y, r[1]);
        motion_bind(&w->present_w, r[2]);
        motion_bind(&w->present_h, r[3]);
        motion_bind(&w->present_opacity, 0);
    }
    present_spring_rect(w, w->x, w->y, w->w, w->h);
    motion_ease_to(&w->present_opacity, 255, 200);
    w->anim = WIN_ANIM_RESTORE;
}

void win_zoom_toggle(desktop_window *w) {
    if (!w) return;
    if (w->zoomed) {
        w->x = w->zx; w->y = w->zy; w->w = w->zw; w->h = w->zh;
        w->zoomed = 0;
    } else {
        w->zx = w->x; w->zy = w->y; w->zw = w->w; w->zh = w->h;
        w->x = 4;
        w->y = AQUA_MENUBAR_H + 4;
        w->w = (int)g_fb_w - 8;
        w->h = (int)g_fb_h - AQUA_MENUBAR_H - AQUA_DOCK_RESERVE - 4;
        w->zoomed = 1;
    }
    if (g_fluent) {
        win_present_animate_to(w);
        w->anim = WIN_ANIM_ZOOM;
    } else {
        win_present_sync(w);
    }
}

void wm_anim_settle(void) {
    int i;
    for (i = g_win_count - 1; i >= 0; i--) {
        desktop_window *w = &g_windows[i];
        if (w->anim == WIN_ANIM_NONE || !win_present_settled(w)) continue;
        if (w->anim == WIN_ANIM_CLOSE) {
            w->anim = WIN_ANIM_NONE;
            win_destroy(w);
        } else if (w->anim == WIN_ANIM_MIN) {
            w->anim = WIN_ANIM_NONE;
            w->minimized = 1;
            win_present_sync(w);
        } else {
            w->anim = WIN_ANIM_NONE;
        }
    }
}

void win_destroy(desktop_window *w) {
    int idx = -1;
    int id = w->id;
    for (int i = 0; i < g_win_count; i++) {
        if (g_windows[i].id == id) { idx = i; break; }
    }
    if (idx < 0) return;

    /* Aqua 关闭动画已在 win_anim_close 播完；这里直接摘除 */
    w->visible = 0;

    if (w->app_id >= 0 && w->app_id < g_app_count && g_apps[w->app_id].on_destroy && w->app_state) {
        g_apps[w->app_id].on_destroy(w->app_state);
    }
    /* 挪位之后 w 指向的是下一扇窗口，关闭判断必须用挪位前记下的 id。 */
    for (int i = idx; i < g_win_count - 1; i++) g_windows[i] = g_windows[i+1];
    g_win_count--;
    if (g_focused_win == id) {
        g_focused_win = -1;
        for (int i = g_win_count - 1; i >= 0; i--) {
            if (g_windows[i].ws == g_ws_cur && g_windows[i].visible && !g_windows[i].minimized) {
                g_focused_win = g_windows[i].id;
                g_windows[i].focused = 1;
                break;
            }
        }
    }
}

/* 窗口标题栏高度：Aqua 与 KATE 都是 28px */
int win_titlebar_h(void) {
    return g_fluent ? AQUA_TITLEBAR_H : KATE_TITLEBAR_H;
}

/* P5d: 关闭请求。运行中的 PE 窗口不能直接 win_destroy（run_windowed 阻塞中，
 * pump 持有窗口 id）——按 pe_window_host 协作契约 inject WM_CLOSE，
 * PE 自愿退出后由 launch_pe_app 收尾销毁。 */
void win_close_request(desktop_window *w) {
    if (g_pe_running && w->id == g_pe_win_id && g_pe_svc && g_pe_svc->inject_input) {
        g_pe_svc->inject_input(PE_WM_CLOSE, 0, 0, 0, 0);
        return;
    }
    win_anim_close(w);
}

int ws_win_count(int ws) {
    int n = 0;
    for (int i = 0; i < g_win_count; i++)
        if (g_windows[i].ws == ws && g_windows[i].visible) n++;
    return n;
}

/* ============================================================
 *  窗口装饰绘制
 * ============================================================ */

/* Aqua 窗口框架：圆角 12、28px 标题栏、左上红黄绿三钮、柔和投影。
 * 绘制读呈现矩形；三钮命中读逻辑矩形（交互时两者已吸附一致）。 */
#define AQ_TB      AQUA_TITLEBAR_H
#define AQ_BTN_HIT 10            /* 每钮命中半宽 */

static int aq_btn_cx(int x, int btn, int s256) {
    int d = g_aqua_theme.traffic, gap = g_aqua_theme.traffic_gap;
    return x + (g_aqua_theme.traffic_inset_x + d / 2 + btn * (d + gap)) * s256 / 256;
}

/* btn：0=关闭 1=最小化 2=缩放 */
static int aq_hit_btn(desktop_window *w, int mx, int my, int btn) {
    int cx = aq_btn_cx(w->x, btn, 256);
    return mx >= cx - AQ_BTN_HIT && mx < cx + AQ_BTN_HIT &&
           my >= w->y && my < w->y + AQ_TB;
}

/* 鼠标在三钮组上（显示 × − + 符号） */
static int aq_group_hover(const desktop_window *w) {
    int x0 = aq_btn_cx(w->x, 0, 256) - AQ_BTN_HIT;
    int x1 = aq_btn_cx(w->x, 2, 256) + AQ_BTN_HIT;
    return g_mouse_x >= x0 && g_mouse_x < x1 &&
           g_mouse_y >= w->y && g_mouse_y < w->y + AQ_TB;
}

int win_hit_close_btn(desktop_window *w, int mx, int my) {
    if (g_fluent) return aq_hit_btn(w, mx, my, 0);
    int bx = w->x + w->w - CLOSE_BTN_OFFSET;
    int by = w->y + (KATE_TITLEBAR_H - CLOSE_BTN_SIZE) / 2;
    return mx >= bx && mx < bx + CLOSE_BTN_SIZE &&
           my >= by && my < by + CLOSE_BTN_SIZE;
}

int win_hit_min_btn(desktop_window *w, int mx, int my) {
    return g_fluent ? aq_hit_btn(w, mx, my, 1) : 0;
}

int win_hit_max_btn(desktop_window *w, int mx, int my) {
    return g_fluent ? aq_hit_btn(w, mx, my, 2) : 0;
}

int win_hit_titlebar(desktop_window *w, int mx, int my) {
    int tb = g_fluent ? AQ_TB : KATE_TITLEBAR_H;
    if (g_fluent && (win_hit_close_btn(w, mx, my) || win_hit_max_btn(w, mx, my) ||
                     win_hit_min_btn(w, mx, my)))
        return 0;   /* 三钮区域不算标题栏拖拽 */
    return mx >= w->x && mx < w->x + w->w &&
           my >= w->y && my < w->y + tb;
}

/* 悬停在某窗口三钮组上时返回非零键（变化触发重绘） */
int wm_hover_key(void) {
    desktop_window *w;
    if (!g_fluent) return 0;
    w = win_find_at(g_mouse_x, g_mouse_y);
    if (!w || !aq_group_hover(w)) return 0;
    return (w->id << 1) | 1;
}

static void aq_glyph(int cx, int cy, int btn, du_u32 c) {
    int i;
    for (i = -3; i <= 3; i++) {
        if (btn == 0) {
            du_pixel(&g_fb, cx + i, cy + i, c);
            du_pixel(&g_fb, cx + i, cy - i, c);
        } else if (btn == 1) {
            du_pixel(&g_fb, cx + i, cy, c);
        } else {
            du_pixel(&g_fb, cx + i, cy, c);
            du_pixel(&g_fb, cx, cy + i, c);
        }
    }
}

/* 应用 on_draw 会画满客户区的方角；先存底部两角的底图，之后按覆盖率回填 */
#define AQ_CORNER_MAX 16
static du_u32 g_corner_bg[2][AQ_CORNER_MAX * AQ_CORNER_MAX];
static int g_corner_x, g_corner_y, g_corner_w, g_corner_h, g_corner_r, g_corner_ok;

static void corner_save(int x, int y, int w, int h, int r) {
    int i, j;
    g_corner_ok = 0;
    if (r <= 0 || r > AQ_CORNER_MAX) return;
    for (j = 0; j < r; j++) {
        for (i = 0; i < r; i++) {
            g_corner_bg[0][j * r + i] = du_pixel_read(&g_fb, x + i, y + h - r + j);
            g_corner_bg[1][j * r + i] = du_pixel_read(&g_fb, x + w - r + i, y + h - r + j);
        }
    }
    g_corner_x = x; g_corner_y = y; g_corner_w = w; g_corner_h = h; g_corner_r = r;
    g_corner_ok = 1;
}

static void corner_restore(void) {
    int i, j, k;
    int r = g_corner_r;
    if (!g_corner_ok) return;
    for (k = 0; k < 2; k++) {
        int bx = k ? g_corner_x + g_corner_w - r : g_corner_x;
        int by = g_corner_y + g_corner_h - r;
        for (j = 0; j < r; j++) {
            for (i = 0; i < r; i++) {
                int col = (k ? g_corner_w - r : 0) + i;
                int cv = aqua_round_cov(col, g_corner_h - r + j, g_corner_w, g_corner_h, r);
                du_u32 cur, bg;
                if (cv >= 16) continue;
                bg = g_corner_bg[k][j * r + i];
                cur = du_pixel_read(&g_fb, bx + i, by + j);
                du_pixel(&g_fb, bx + i, by + j, aqua_mix(bg, cur, cv * 16));
            }
        }
    }
    g_corner_ok = 0;
}

static void draw_window_frame_aqua(desktop_window *w) {
    int x, y, ww, hh, op, r, tb, s256, b, hover;
    const aqua_theme *t = &g_aqua_theme;
    static const du_u32 glyph_c[3] = { 0xFF7A0A06u, 0xFF8F5A0Au, 0xFF0A5E14u };
    du_u32 btn_c[3];
    win_present_rect(w, &x, &y, &ww, &hh, &op);
    g_corner_ok = 0;
    if (op > 255) op = 255;
    if (op <= 0 || ww < 16 || hh < 16) return;
    r = t->radius_window;
    s256 = w->w > 0 ? ww * 256 / w->w : 256;
    tb = AQ_TB * s256 / 256;
    if (tb < 6) tb = 6;

    aqua_shadow(&g_fb, x, y, ww, hh, r,
                (w->focused ? 120 : 64) * op / 255, w->focused ? 14 : 8);
    if (w->anim == WIN_ANIM_NONE && win_present_settled(w))
        corner_save(x, y, ww, hh, r);
    aqua_fill_round_alpha(&g_fb, x, y, ww, hh, r, t->window, op);
    aqua_fill_round_rows(&g_fb, x, y, ww, hh, r,
                         w->focused ? 0xFFE8E8EAu : 0xFFF2F2F4u, op, 0, tb);
    aqua_fill_round_rows(&g_fb, x, y, ww, hh, r, 0xFFD4D4D6u, op, tb - 1, tb);

    hover = (w->anim == WIN_ANIM_NONE) && aq_group_hover(w);
    btn_c[0] = t->traffic_close;
    btn_c[1] = t->traffic_min;
    btn_c[2] = t->traffic_zoom;
    for (b = 0; b < 3; b++) {
        int d = t->traffic * s256 / 256;
        int cx = aq_btn_cx(x, b, s256);
        int cy = y + (t->traffic_inset_y + t->traffic / 2) * s256 / 256;
        du_u32 c = (w->focused || hover) ? btn_c[b] : 0xFFCDCDCFu;
        if (d < 4) d = 4;
        aqua_disc(&g_fb, cx - d / 2, cy - d / 2, d, aqua_mix(c, 0xFF000000u, 40), op);
        aqua_disc(&g_fb, cx - d / 2 + 1, cy - d / 2 + 1, d - 2, c, op);
        if (hover) aq_glyph(cx, cy, b, glyph_c[b]);
    }

    if (op >= 250 && tb >= 20) {
        int tw = zh_string_width(w->title, 0);
        int tx = x + (ww - tw) / 2;
        int min_x = aq_btn_cx(x, 2, 256) + AQ_BTN_HIT + 8;
        if (tx < min_x) tx = min_x;
        zh_draw_string(&g_fb, w->title, tx, y + (tb - zh_line_height(0)) / 2 + 1,
                       w->focused ? 0xFF3C3C3Eu : 0xFFA1A1A6u, 0);
    }
}

void draw_window_post(desktop_window *w) {
    int x, y, ww, hh, op;
    if (!g_fluent || !w->visible || w->minimized) return;
    win_present_rect(w, &x, &y, &ww, &hh, &op);
    if (op > 255) op = 255;
    if (op <= 0 || ww < 16 || hh < 16) return;
    corner_restore();
    aqua_round_band(&g_fb, x, y, ww, hh, g_aqua_theme.radius_window, 0xFF000000u,
                    (w->focused ? 46 : 30) * op / 255);
}

void draw_window_frame(desktop_window *w) {
    if (!w->visible || w->minimized) return;

    if (g_fluent) {
        draw_window_frame_aqua(w);
        return;
    }

    /* ---- Winux-Kate 风格（fluent=0 回退路径） ---- */
    u32 border = w->focused ? KS_BORDER_FOCUS : KS_BORDER;

    if (w->focused) {
        du_kate_glow_border(&g_fb, w->x, w->y, w->w, w->h, KS_ACCENT);
    }

    du_fill_rounded_rect(&g_fb, w->x, w->y, w->w, w->h, KS_BG_SECONDARY, 2);

    du_fill_rect_gradient(&g_fb, w->x + 1, w->y + 1, w->w - 2, KATE_TITLEBAR_H - 1,
                          KS_BG_TERTIARY, KS_BG_SECONDARY);
    du_divider_h(&g_fb, w->x, w->y + KATE_TITLEBAR_H, w->w, KS_BORDER);

    du_fill_rect(&g_fb, w->x + 6, w->y + KATE_TITLEBAR_H / 2 - 2, 4, 4, KS_ACCENT2);

    du_draw_string(&g_fb, w->title,
                   w->x + 14,
                   w->y + (KATE_TITLEBAR_H - (i64)DU_ASCII_LINE_H) / 2 + 1,
                   KS_ACCENT, KS_BG_SECONDARY, DU_ASCII_STEP);

    int bx = w->x + w->w - CLOSE_BTN_OFFSET;
    int by = w->y + (KATE_TITLEBAR_H - CLOSE_BTN_SIZE) / 2;
    du_rect_outline(&g_fb, bx, by, CLOSE_BTN_SIZE, CLOSE_BTN_SIZE, KS_DANGER, 1);
    du_draw_string(&g_fb, "X", bx + 3, by - 1,
                   KS_DANGER, KS_BG_SECONDARY, DU_ASCII_STEP);

    du_rect_outline(&g_fb, w->x, w->y, w->w, w->h, border, 2);
}
