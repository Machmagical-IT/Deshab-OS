/* startmenu.c — M3 Win11 开始菜单
 *
 * 任务栏开始按钮弹出：搜索框 + "已固定" 应用瓦片网格 + 底部用户/电源行。
 * 条目来自 appreg.c 的 g_sm_ids/g_sm_labels（STARTMENU.CNF，缺失回退全量）。
 *
 * 布局（浅色主题，深色壁纸衬托亚克力）：
 *   面板 520 × min(600, H-128)，锚在开始按钮上方
 *   [搜索框 36px] [已固定 标签] [瓦片网格 6 列 × N 行]
 *   [分隔线] [用户 | 电源]
 *
 * 交互：
 *   搜索：ASCII 输入实时过滤（display/name 不区分大小写子串），
 *         过滤态切 du_wf_list_item 列表，Up/Down + Enter 键盘启动
 *   瓦片：点击 → launch_app + 关闭
 *   电源：g_quit（与托盘电源一致）；Esc/点击面板外关闭
 *
 * 绘制挂在 taskbar_draw 末尾（叠加在任务栏之上）；
 * 点击/键盘经 taskbar_click / handle_key 路由（模态，参考 WiFi 卡片）。
 */
#include "desktop.h"
#include "ascii_font.h"          /* 顺序敏感：先于 fluent_ui.h */
#include "../UTSM/include/utsm/fluent_ui.h"
#include "zhfont.h"

/* ---- 全局状态 ---- */
int g_sm_open = 0;

#define SM_PANEL_W     520
#define SM_PANEL_MIN_H 380
#define SM_SEARCH_MAX  28         /* 搜索串字节上限（ASCII） */
#define SM_COLS        6
#define SM_TILE_H      84
#define SM_TILE_GAP    4
#define SM_GRID_PAD    16
#define SM_BOTTOM_H    56

static char g_sm_search[SM_SEARCH_MAX + 1];
static int  g_sm_search_len;
static int  g_sm_sel;             /* 过滤列表选中行 */

static int g_sm_panel_rect[4];
static int g_sm_search_rect[4];
static int g_sm_tile_rect[SM_MAX_APPS][4];
static int g_sm_pwr_rect[4];
static int g_sm_user_rect[4];
/* 过滤结果（g_sm 下标），-1 结尾 */
static int g_sm_filter[SM_MAX_APPS + 1];
static int g_sm_filter_n;
/* 网格实际显示的条目数（容量截断时串口留档） */
static int g_sm_grid_shown;
static int g_sm_grid_dropped;
/* 当前模式实际绘制（可点击）的行数：grid=瓦片数 / list=过滤行数 */
static int g_sm_rows_visible;

static const wf_theme *sm_theme(void) { return &g_wf_light; }

static wf_btn_state sm_hover_state(const int r[4]) {
    if (g_mouse_x >= r[0] && g_mouse_x < r[0] + r[2] &&
        g_mouse_y >= r[1] && g_mouse_y < r[1] + r[3])
        return WF_BTN_HOVER;
    return WF_BTN_NORMAL;
}

/* ============================================================
 *  开 / 关 / 过滤
 * ============================================================ */

static void sm_filter_rebuild(void) {
    g_sm_filter_n = 0;
    for (int i = 0; i < g_sm_count && g_sm_filter_n < SM_MAX_APPS; i++) {
        if (g_sm_search_len == 0) {
            g_sm_filter[g_sm_filter_n++] = i;
            continue;
        }
        /* 不区分大小写 ASCII 子串匹配：display 标签 + 内部名 */
        const char *hay[2] = { g_sm_labels[i],
                               g_apps[g_sm_ids[i]].name };
        int hit = 0;
        for (int h = 0; h < 2 && !hit; h++) {
            const char *s = hay[h];
            if (!s) continue;
            for (int b = 0; s[b] && !hit; b++) {
                int m = 1;
                for (int k = 0; k < g_sm_search_len; k++) {
                    char c1 = s[b + k], c2 = g_sm_search[k];
                    if (!c1) { m = 0; break; }
                    if (c1 >= 'a' && c1 <= 'z') c1 = (char)(c1 - 32);
                    if (c2 >= 'a' && c2 <= 'z') c2 = (char)(c2 - 32);
                    if (c1 != c2) { m = 0; break; }
                }
                if (m) hit = 1;
            }
        }
        if (hit) g_sm_filter[g_sm_filter_n++] = i;
    }
    g_sm_filter[g_sm_filter_n] = -1;
    if (g_sm_sel >= g_sm_filter_n) g_sm_sel = 0;
}

void startmenu_open(void) {
    g_sm_open = 1;
    g_sm_search[0] = 0;
    g_sm_search_len = 0;
    g_sm_sel = 0;
    sm_filter_rebuild();
    g_wifi_open = 0;      /* 互斥：收起托盘弹卡 */
    g_qs_open = 0;
    slog("[startmenu] open");
}

void startmenu_close(void) {
    if (!g_sm_open) return;
    g_sm_open = 0;
    slog("[startmenu] close");
}

void startmenu_toggle(void) {
    if (g_sm_open) startmenu_close();
    else startmenu_open();
}

/* 瓦片标签截断：zh 16px ASCII 半宽 12 / CJK 全宽 16 → 瓦片 ~80px 最多 4 个 CJK */
static void sm_label_clip(char *dst, int cap, const char *src) {
    int w = 0, o = 0;
    for (int i = 0; src[i] && o < cap - 4; ) {
        unsigned char c = (unsigned char)src[i];
        int cw = (c < 0x80) ? 12 : 16;   /* ASCII 半宽 12px / CJK 全宽 16px */
        if (w + cw > 68) break;
        dst[o++] = src[i++];
        if (c >= 0x80) {                  /* UTF-8 续字节整组拷贝 */
            int cont = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
            while (cont-- > 0 && src[i] && o < cap - 4)
                dst[o++] = src[i++];
        }
        w += cw;
    }
    dst[o] = 0;
}

/* ============================================================
 *  绘制（taskbar_draw 末尾调用）
 * ============================================================ */

void startmenu_draw(void) {
    if (!g_sm_open) return;
    const wf_theme *t = sm_theme();
    int W = (int)g_fb_w, H = (int)g_fb_h;

    /* ---- 面板几何：开始按钮上方 ---- */
    int ph = H - 64 - 8 - 24;      /* 任务栏(48+8gap) 上方 8 间隙 + 顶部余量 */
    if (ph > 600) ph = 600;
    if (ph < SM_PANEL_MIN_H) ph = SM_PANEL_MIN_H;
    int px = g_tb2_start_rect[0] - 8;
    if (px < 8) px = 8;
    if (px + SM_PANEL_W > W - 8) px = W - 8 - SM_PANEL_W;
    int py = H - 64 - ph;          /* 任务栏区(56) 之上贴住 */
    if (py < 8) py = 8;

    g_sm_panel_rect[0] = px; g_sm_panel_rect[1] = py;
    g_sm_panel_rect[2] = SM_PANEL_W; g_sm_panel_rect[3] = ph;

    /* ---- 卡片底 ---- */
    du_wf_shadow(&g_fb, px, py, SM_PANEL_W, ph, (du_u32)t->radius_lg, t);
    du_wf_acrylic(&g_fb, px, py, SM_PANEL_W, ph, 235, (du_u32)t->radius_lg, t);

    /* ---- 搜索框 ---- */
    g_sm_search_rect[0] = px + 16; g_sm_search_rect[1] = py + 16;
    g_sm_search_rect[2] = SM_PANEL_W - 32; g_sm_search_rect[3] = 36;
    {
        /* blink：RTC 秒奇偶交替 */
        int yy, mo, dd, hh, mi, ss;
        rtc_read(&yy, &mo, &dd, &hh, &mi, &ss);
        du_wf_textbox(&g_fb, px + 16, py + 16, SM_PANEL_W - 32,
                      g_sm_search[0] ? g_sm_search : "search",
                      ss & 1, t, 1);
        if (!g_sm_search[0])
            du_wf_icon(&g_fb, WF_ICON_SEARCH,
                       px + SM_PANEL_W - 40, py + 22, 22, t->text_dim);
    }

    /* ---- 过滤列表模式（有搜索串）---- */
    if (g_sm_search_len > 0) {
        zh_draw_string(&g_fb, "搜索结果", px + 16, py + 66,
                       t->text_dim, 0);
        int y = py + 92;
        int max_rows = (py + ph - SM_BOTTOM_H - y) / 36;
        if (max_rows > SM_MAX_APPS) max_rows = SM_MAX_APPS;
        int rows = g_sm_filter_n;
        if (rows > max_rows) rows = max_rows;
        g_sm_rows_visible = rows;
        for (int i = 0; i < rows; i++) {
            int e = g_sm_filter[i];
            int id = g_sm_ids[e];
            char label[52];
            sm_label_clip(label, 52, g_sm_labels[e]);
            /* zh 列表项：底行 + 名字（ASCII name 右侧小字） */
            int rowy = y + i * 36;
            if (g_sm_sel == i)
                du_wf_overlay(&g_fb, px + 8, rowy, SM_PANEL_W - 16, 36, 0x28000000u);
            else if (sm_hover_state((int[4]){px + 8, rowy, SM_PANEL_W - 16, 36})
                     == WF_BTN_HOVER)
                du_wf_overlay(&g_fb, px + 8, rowy, SM_PANEL_W - 16, 36, 0x14000000u);
            if (g_sm_sel == i)
                du_fill_rect(&g_fb, px + 8, rowy + 8, 3, 20, t->accent);
            du_wf_icon(&g_fb, (wf_icon_id)desktop_app_icon(id),
                       px + 20, rowy + 7, 22, t->text_primary);
            zh_draw_string(&g_fb, label, px + 54, rowy + 10,
                           t->text_primary, 0);
            du_draw_string(&g_fb, g_apps[id].name,
                           px + SM_PANEL_W - 16 - kstrlen(g_apps[id].name) * 8,
                           rowy + 12, t->text_dim, 0, DU_ASCII_STEP);
            /* 行命中矩形（点击用） */
            g_sm_tile_rect[i][0] = px + 8; g_sm_tile_rect[i][1] = rowy;
            g_sm_tile_rect[i][2] = SM_PANEL_W - 16; g_sm_tile_rect[i][3] = 36;
        }
        if (g_sm_filter_n == 0)
            zh_draw_string(&g_fb, "无匹配应用", px + 24, y + 8, t->text_dim, 0);
        goto bottom_bar;
    }

    /* ---- 瓦片网格模式 ---- */
    zh_draw_string(&g_fb, "已固定", px + 16, py + 66, t->text_dim, 0);

    {
        int grid_y = py + 92;
        int grid_h = py + ph - SM_BOTTOM_H - grid_y;
        int rows_cap = grid_h / (SM_TILE_H + SM_TILE_GAP);
        int cap = rows_cap * SM_COLS;
        if (cap > SM_MAX_APPS) cap = SM_MAX_APPS;

        g_sm_grid_shown = g_sm_count;
        if (g_sm_grid_shown > cap) {
            g_sm_grid_dropped = g_sm_count - cap;
            g_sm_grid_shown = cap;
        } else {
            g_sm_grid_dropped = 0;
        }
        g_sm_rows_visible = g_sm_grid_shown;

        int tile_w = (SM_PANEL_W - SM_GRID_PAD * 2 - (SM_COLS - 1) * SM_TILE_GAP)
                     / SM_COLS;
        for (int i = 0; i < g_sm_grid_shown; i++) {
            int col = i % SM_COLS, row = i / SM_COLS;
            int tx = px + SM_GRID_PAD + col * (tile_w + SM_TILE_GAP);
            int ty = grid_y + row * (SM_TILE_H + SM_TILE_GAP);
            g_sm_tile_rect[i][0] = tx; g_sm_tile_rect[i][1] = ty;
            g_sm_tile_rect[i][2] = tile_w; g_sm_tile_rect[i][3] = SM_TILE_H;

            int id = g_sm_ids[i];
            du_wf_app_tile(&g_fb, tx, ty, tile_w, SM_TILE_H,
                           (wf_icon_id)desktop_app_icon(id), t,
                           sm_hover_state(g_sm_tile_rect[i]), 0);
            char label[52];
            sm_label_clip(label, 52, g_sm_labels[i]);
            zh_draw_string(&g_fb, label,
                           tx + (tile_w - zh_string_width(label, 0)) / 2,
                           ty + SM_TILE_H - 30, t->text_primary, 0);
        }
        if (g_sm_grid_dropped > 0) {
            char buf[24];
            buf[0] = '+';
            int n = u32dec((u32)g_sm_grid_dropped, buf + 1);
            buf[n + 1] = 0;
            zh_draw_string(&g_fb, buf, px + SM_PANEL_W - 60,
                           py + 66, t->text_dim, 0);
        }
    }

bottom_bar:
    /* ---- 底部：分隔线 + 用户 + 电源 ---- */
    {
        int by = py + ph - SM_BOTTOM_H;
        du_wf_separator(&g_fb, px + 16, by, SM_PANEL_W - 32, t);

        /* 用户行 */
        g_sm_user_rect[0] = px + 12; g_sm_user_rect[1] = by + 8;
        g_sm_user_rect[2] = 220; g_sm_user_rect[3] = 40;
        du_wf_icon_button(&g_fb, px + 12, by + 8, 40, t,
                          sm_hover_state(g_sm_user_rect));
        du_wf_icon(&g_fb, WF_ICON_USER, px + 20, by + 16, 24, t->text_primary);
        zh_draw_string(&g_fb, "Deshab 用户", px + 60, by + 16,
                       t->text_primary, 0);

        /* 电源钮 */
        g_sm_pwr_rect[0] = px + SM_PANEL_W - 52; g_sm_pwr_rect[1] = by + 8;
        g_sm_pwr_rect[2] = 40; g_sm_pwr_rect[3] = 40;
        du_wf_icon_button(&g_fb, px + SM_PANEL_W - 52, by + 8, 40, t,
                          sm_hover_state(g_sm_pwr_rect));
        du_wf_icon(&g_fb, WF_ICON_POWER, px + SM_PANEL_W - 44, by + 16, 24,
                   t->text_primary);
    }
}

/* ============================================================
 *  点击（taskbar_click 调用；返回 1 = 已消费）
 * ============================================================ */

int startmenu_click(int mx, int my) {
    if (!g_sm_open) return 0;
    if (!rect_hit(g_sm_panel_rect, mx, my)) {
        startmenu_close();       /* 点面板外：关闭并放行点击 */
        return 0;
    }

    /* 电源 → 退出桌面（与托盘电源一致） */
    if (rect_hit(g_sm_pwr_rect, mx, my)) {
        startmenu_close();
        g_quit = 1;
        return 1;
    }
    /* 用户行（预留：M7 用户菜单） */
    if (rect_hit(g_sm_user_rect, mx, my)) return 1;

    /* 瓦片 / 过滤行 → 启动（只认实际绘制的行，防越界脏矩形） */
    int n = g_sm_rows_visible;
    for (int i = 0; i < n; i++) {
        if (!rect_hit(g_sm_tile_rect[i], mx, my)) continue;
        int e = g_sm_filter[i];
        slog("[startmenu] launch:");
        slog(g_apps[g_sm_ids[e]].name);
        startmenu_close();
        launch_app(g_sm_ids[e]);
        return 1;
    }
    return 1;   /* 面板内空白：吞掉 */
}

/* ============================================================
 *  键盘（handle_key 调用；返回 1 = 已消费）
 * ============================================================ */

int startmenu_key(u8 sc, int shift) {
    if (!g_sm_open) return 0;

    if (sc == 0x01) { startmenu_close(); return 1; }        /* Esc */
    if (sc == 0x0E) {                                       /* Backspace */
        if (g_sm_search_len > 0) {
            g_sm_search[--g_sm_search_len] = 0;
            sm_filter_rebuild();
        }
        return 1;
    }
    if (sc == 0x1C) {                                       /* Enter 启动选中 */
        if (g_sm_search_len > 0 && g_sm_filter_n > 0) {
            int e = g_sm_filter[g_sm_sel];
            slog("[startmenu] launch(kbd):");
            slog(g_apps[g_sm_ids[e]].name);
            startmenu_close();
            launch_app(g_sm_ids[e]);
        } else if (g_sm_grid_shown > 0) {
            int id = g_sm_ids[0];
            startmenu_close();
            launch_app(id);
        }
        return 1;
    }
    if (g_sm_search_len > 0) {                              /* 过滤列表导航 */
        if (sc == 0x48) {                                   /* Up */
            if (g_sm_sel > 0) g_sm_sel--;
            return 1;
        }
        if (sc == 0x50) {                                   /* Down */
            if (g_sm_sel < g_sm_filter_n - 1) g_sm_sel++;
            return 1;
        }
    }

    /* 可打印字符 → 搜索串 */
    if (sc >= 58) return 1;
    {
        static const char normal[58] = {
            0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, '\t',
            'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
            'a','s','d','f','g','h','j','k','l',';','\'', '`',0,'\\',
            'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
        };
        static const char shifted[58] = {
            0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, '\t',
            'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
            'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
            'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
        };
        char c = shift ? shifted[sc] : normal[sc];
        if (c && c != '\n' && c != 27 && c != 8 && c != '\t') {
            if (g_sm_search_len < SM_SEARCH_MAX) {
                g_sm_search[g_sm_search_len++] = c;
                g_sm_search[g_sm_search_len] = 0;
                g_sm_sel = 0;
                sm_filter_rebuild();
            }
        }
    }
    return 1;
}
