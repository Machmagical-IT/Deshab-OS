/* dock.c — Aqua Dock（底部居中毛玻璃）
 *
 * 条目：[启动台] [STARTMENU.CNF 前 8 项] [未固定的运行中应用] | [调度中心]
 * 放大：每个图标一条弹簧轨道，48→72px。目标按鼠标到图标"基准中心"
 *       （全部 48px 时的布局）的距离做 smoothstep 衰减，影响半径 3 个图标；
 *       用基准中心算距离，放大本身不会反过来推动目标，避免抖动。
 * 图标底对齐，面板高度固定，放大的图标向上伸出面板。
 * 运行点：4px 圆点；启动弹跳：12px / 6px 两次 ease。
 */
#include "desktop.h"
#include "aqua_draw.h"
#include "zhfont.h"

#define DK_GAP     6
#define DK_PAD_X   10
#define DK_SEP_W   13
#define DK_PINNED  8
#define DK_MAX     (2 + DK_PINNED + MAX_WINDOWS + 1)
#define DK_RADIUS  3            /* 影响半径（图标数） */

enum { K_LP = 1, K_APP, K_SEP, K_MC };

typedef struct {
    int kind;
    int app_id;
    int x, y, size;             /* 本帧图标矩形（动画后） */
    int base_cx;                /* 全 48px 布局时的中心 */
} dock_item;

static dock_item g_it[DK_MAX];
static int g_n;
static int g_panel[4];
static int g_base_panel[4];
static int g_hover = -1;

static motion_track g_sz_app[MAX_APPS];
static motion_track g_sz_lp, g_sz_mc;
static motion_track g_bounce[MAX_APPS];
static int g_bounce_stage[MAX_APPS];
static int g_inited;

static void dock_init_tracks(void) {
    int i;
    if (g_inited) return;
    for (i = 0; i < MAX_APPS; i++) {
        motion_bind(&g_sz_app[i], AQUA_DOCK_ICON);
        motion_bind(&g_bounce[i], 0);
        g_bounce_stage[i] = 0;
    }
    motion_bind(&g_sz_lp, AQUA_DOCK_ICON);
    motion_bind(&g_sz_mc, AQUA_DOCK_ICON);
    g_inited = 1;
}

static motion_track *size_track(const dock_item *it) {
    if (it->kind == K_LP) return &g_sz_lp;
    if (it->kind == K_MC) return &g_sz_mc;
    if (it->kind == K_APP && it->app_id >= 0 && it->app_id < MAX_APPS)
        return &g_sz_app[it->app_id];
    return 0;
}

static int app_running(int app_id) {
    int i;
    for (i = 0; i < g_win_count; i++) {
        desktop_window *w = &g_windows[i];
        if (w->visible && w->app_id == app_id && w->anim != WIN_ANIM_CLOSE) return 1;
    }
    return 0;
}

static int has_item(int app_id) {
    int i;
    for (i = 0; i < g_n; i++)
        if (g_it[i].kind == K_APP && g_it[i].app_id == app_id) return 1;
    return 0;
}

static void push(int kind, int app_id) {
    if (g_n >= DK_MAX) return;
    g_it[g_n].kind = kind;
    g_it[g_n].app_id = app_id;
    g_n++;
}

static int item_w(const dock_item *it, int animated) {
    motion_track *t;
    if (it->kind == K_SEP) return DK_SEP_W;
    if (!animated) return AQUA_DOCK_ICON;
    t = size_track(it);
    return t ? motion_px(t) : AQUA_DOCK_ICON;
}

/* 重建条目 + 计算本帧几何（基准 / 动画两套） */
static void layout(void) {
    int i, W = (int)g_fb_w, H = (int)g_fb_h;
    int total, x, py;
    dock_init_tracks();
    g_n = 0;
    push(K_LP, -1);
    for (i = 0; i < g_sm_count && i < DK_PINNED; i++) {
        int id = g_sm_ids[i];
        if (id < 0 || id >= g_app_count || has_item(id)) continue;
        push(K_APP, id);
    }
    for (i = 0; i < g_win_count; i++) {
        int id = g_windows[i].app_id;
        if (!g_windows[i].visible || id < 0 || id >= g_app_count || has_item(id)) continue;
        push(K_APP, id);
    }
    push(K_SEP, -1);
    push(K_MC, -1);

    py = H - AQUA_DOCK_BOTTOM - AQUA_DOCK_PANEL_H;

    total = 2 * DK_PAD_X + (g_n - 1) * DK_GAP;
    for (i = 0; i < g_n; i++) total += item_w(&g_it[i], 0);
    x = (W - total) / 2;
    g_base_panel[0] = x; g_base_panel[1] = py;
    g_base_panel[2] = total; g_base_panel[3] = AQUA_DOCK_PANEL_H;
    x += DK_PAD_X;
    for (i = 0; i < g_n; i++) {
        int w = item_w(&g_it[i], 0);
        g_it[i].base_cx = x + w / 2;
        x += w + DK_GAP;
    }

    total = 2 * DK_PAD_X + (g_n - 1) * DK_GAP;
    for (i = 0; i < g_n; i++) total += item_w(&g_it[i], 1);
    x = (W - total) / 2;
    g_panel[0] = x; g_panel[1] = py;
    g_panel[2] = total; g_panel[3] = AQUA_DOCK_PANEL_H;
    x += DK_PAD_X;
    for (i = 0; i < g_n; i++) {
        int w = item_w(&g_it[i], 1);
        int bounce = 0;
        if (g_it[i].kind == K_APP && g_it[i].app_id >= 0 && g_it[i].app_id < MAX_APPS)
            bounce = motion_px(&g_bounce[g_it[i].app_id]);
        g_it[i].x = x;
        g_it[i].size = w;
        g_it[i].y = py + AQUA_DOCK_PAD_T + AQUA_DOCK_ICON - w - bounce;
        x += w + DK_GAP;
    }
}

static int in_zone(int mx, int my) {
    return mx >= g_base_panel[0] - 8 && mx < g_base_panel[0] + g_base_panel[2] + 8 &&
           my >= g_base_panel[1] - (AQUA_DOCK_ICON_MAX - AQUA_DOCK_ICON) - 4 &&
           my < (int)g_fb_h;
}

/* smoothstep 衰减：d=0 → 256，d>=R → 0 */
static int falloff(int d, int R) {
    int t;
    if (d < 0) d = -d;
    if (d >= R) return 0;
    t = 256 - d * 256 / R;
    return t * t * (768 - 2 * t) / 65536;
}

void dock_pointer(int mx, int my) {
    int i, R, active;
    layout();
    active = in_zone(mx, my) && !g_lp_open;
    R = DK_RADIUS * (AQUA_DOCK_ICON + DK_GAP);
    g_hover = -1;
    for (i = 0; i < g_n; i++) {
        motion_track *t = size_track(&g_it[i]);
        int tgt = AQUA_DOCK_ICON;
        if (!t) continue;
        if (active)
            tgt += (AQUA_DOCK_ICON_MAX - AQUA_DOCK_ICON) * falloff(mx - g_it[i].base_cx, R) / 256;
        if (t->target != tgt * MOTION_ONE) motion_spring_to(t, tgt);
        if (in_zone(mx, my) && mx >= g_it[i].x && mx < g_it[i].x + g_it[i].size)
            g_hover = i;
    }
}

int dock_hover_key(void) { return g_hover + 1; }

static void bounce_start(int app_id) {
    if (app_id < 0 || app_id >= MAX_APPS) return;
    g_bounce_stage[app_id] = 1;
    motion_ease_to(&g_bounce[app_id], 12, 150);
}

int dock_tick(int dt_ms) {
    int i, act = 0;
    dock_init_tracks();
    act |= motion_step(&g_sz_lp, dt_ms);
    act |= motion_step(&g_sz_mc, dt_ms);
    for (i = 0; i < MAX_APPS; i++) {
        act |= motion_step(&g_sz_app[i], dt_ms);
        if (!g_bounce_stage[i]) continue;
        act |= motion_step(&g_bounce[i], dt_ms);
        if (g_bounce[i].active) continue;
        act = 1;
        switch (g_bounce_stage[i]++) {
        case 1: motion_ease_to(&g_bounce[i], 0, 150); break;
        case 2: motion_ease_to(&g_bounce[i], 6, 120); break;
        case 3: motion_ease_to(&g_bounce[i], 0, 120); break;
        default: g_bounce_stage[i] = 0; motion_bind(&g_bounce[i], 0); break;
        }
    }
    return act;
}

static const char *item_label(const dock_item *it) {
    int i;
    if (it->kind == K_LP) return "启动台";
    if (it->kind == K_MC) return "调度中心";
    for (i = 0; i < g_sm_count; i++)
        if (g_sm_ids[i] == it->app_id && g_sm_labels[i][0]) return g_sm_labels[i];
    if (it->app_id >= 0 && it->app_id < g_app_count && g_apps[it->app_id].display_name)
        return g_apps[it->app_id].display_name;
    return "";
}

int dock_slide_y(void) {
    int y;
    layout();
    y = g_panel[1] - (AQUA_DOCK_ICON_MAX - AQUA_DOCK_ICON);
    if (y < AQUA_MENUBAR_H + 4) y = AQUA_MENUBAR_H + 4;
    return y;
}

void dock_draw(void) {
    int i;
    layout();
    aqua_shadow(&g_fb, g_panel[0], g_panel[1], g_panel[2], g_panel[3],
                g_aqua_theme.radius_dock, 60, 8);
    aqua_glass_hgrad(&g_fb, g_panel[0], g_panel[1], g_panel[2], g_panel[3],
                     g_aqua_theme.radius_dock, 0xFFB7E4FFu, 0xFFC8F6D4u, 150, 90, 256);
    aqua_round_band(&g_fb, g_panel[0], g_panel[1], g_panel[2], g_panel[3],
                    g_aqua_theme.radius_dock, 0xFFFFFFFFu, 110);

    for (i = 0; i < g_n; i++) {
        dock_item *it = &g_it[i];
        if (it->kind == K_SEP) {
            aqua_fill_round_alpha(&g_fb, it->x + DK_SEP_W / 2, g_panel[1] + 10, 1,
                                  g_panel[3] - 20, 0, 0xFF000000u, 40);
            continue;
        }
        aqua_app_icon(&g_fb,
                      it->kind == K_LP ? AQUA_ICON_LAUNCHPAD :
                      it->kind == K_MC ? AQUA_ICON_MISSION : it->app_id,
                      it->x, it->y, it->size, 256);
        if ((it->kind == K_APP && app_running(it->app_id)) ||
            (it->kind == K_LP && g_lp_open) || (it->kind == K_MC && g_taskview))
            aqua_disc(&g_fb, it->x + it->size / 2 - 2, g_panel[1] + g_panel[3] - 7, 4,
                      0xFF1D1D1Fu, 200);
    }

    if (g_hover >= 0 && g_hover < g_n && g_it[g_hover].kind != K_SEP) {
        const dock_item *it = &g_it[g_hover];
        const char *lab = item_label(it);
        int tw = zh_string_width(lab, 0);
        int bw = tw + 22, bh = 24;
        int bx = it->x + it->size / 2 - bw / 2;
        int by = it->y - bh - 10;
        if (lab[0]) {
            aqua_panel(&g_fb, bx, by, bw, bh, 6);
            zh_draw_string(&g_fb, lab, bx + 11, by + (bh - zh_line_height(0)) / 2 + 1,
                           g_aqua_theme.text_primary, 0);
        }
    }
}

static void activate_app(int app_id) {
    int i;
    for (i = g_win_count - 1; i >= 0; i--) {
        desktop_window *w = &g_windows[i];
        if (!w->visible || w->app_id != app_id || w->ws != g_ws_cur) continue;
        if (w->anim == WIN_ANIM_CLOSE) continue;
        win_focus(w);
        return;
    }
    bounce_start(app_id);
    launch_app(app_id);
}

int dock_click(int mx, int my) {
    int i;
    layout();
    for (i = 0; i < g_n; i++) {
        dock_item *it = &g_it[i];
        if (it->kind == K_SEP) continue;
        if (mx < it->x || mx >= it->x + it->size) continue;
        if (my < it->y || my >= g_panel[1] + g_panel[3]) continue;
        if (it->kind == K_LP) {
            launchpad_toggle();
        } else if (it->kind == K_MC) {
            if (g_lp_open) launchpad_close();
            g_taskview = !g_taskview;
        } else {
            if (g_lp_open) launchpad_close();
            activate_app(it->app_id);
        }
        return 1;
    }
    return rect_hit(g_panel, mx, my);
}

void dock_icon_rect(int app_id, int r[4]) {
    int i;
    layout();
    for (i = 0; i < g_n; i++) {
        if (g_it[i].kind != K_APP || g_it[i].app_id != app_id) continue;
        r[0] = g_it[i].x; r[1] = g_it[i].y;
        r[2] = g_it[i].size; r[3] = g_it[i].size;
        return;
    }
    r[0] = (int)g_fb_w / 2 - AQUA_DOCK_ICON / 2;
    r[1] = g_panel[1] + AQUA_DOCK_PAD_T;
    r[2] = AQUA_DOCK_ICON;
    r[3] = AQUA_DOCK_ICON;
}
