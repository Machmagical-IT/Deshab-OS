/* menubar.c — Aqua 顶栏（24px 毛玻璃）
 *
 * 左：Apple logo 菜单（关于本机 / 重新启动… / 关机…）、聚焦应用名（粗体，
 *     菜单只有"退出"）、"窗口"菜单（最小化 / 缩放 / 关闭窗口）。
 * 右：Wi-Fi 图标（taskbar.c Wi-Fi 卡片）、控制中心图标（音量/亮度卡片）、
 *     时钟"周四 11月19日 16:25"。
 * 无聚焦窗口时应用菜单与窗口菜单的条目置灰；"关于本机"只在注册了
 * sysinfo 应用时可用。不提供睡眠：S3 尚无设备状态保存/恢复。
 */
#include "desktop.h"
#include "aqua_draw.h"
#include "zhfont.h"

#define MB_H        AQUA_MENUBAR_H
#define MB_ROW_H    22
#define MB_SEP_H    9

enum { MB_NONE = 0, MB_APPLE, MB_APP, MB_WINDOW, MB_COUNT };
enum { A_NONE = 0, A_ABOUT, A_POWER, A_QUIT, A_MIN, A_ZOOM, A_CLOSE };

typedef struct { const char *label; int action; } mb_item;

static const mb_item k_apple[] = {
    { "关于本机", A_ABOUT }, { 0, A_NONE },
    { "重新启动…", A_POWER }, { "关机…", A_POWER },
};
static const mb_item k_window[] = {
    { "最小化", A_MIN }, { "缩放", A_ZOOM }, { 0, A_NONE },
    { "关闭窗口", A_CLOSE },
};
#define MB_MAX_ITEMS 6

static int g_mb_menu = MB_NONE;
static int g_title_rect[MB_COUNT][4];
static int g_wifi_rect[4], g_cc_rect[4];
static int g_menu_rect[4];
static int g_item_rect[MB_MAX_ITEMS][4];
static int g_item_n;
static char g_quit_label[64];

int menubar_menu_open(void) { return g_mb_menu != MB_NONE; }
void menubar_close_menus(void) { g_mb_menu = MB_NONE; }

static desktop_window *focused_window(void) {
    desktop_window *w = g_focused_win >= 0 ? win_find(g_focused_win) : 0;
    if (!w || !w->visible || w->minimized || w->ws != g_ws_cur) return 0;
    if (w->anim == WIN_ANIM_CLOSE || w->anim == WIN_ANIM_MIN) return 0;
    return w;
}

static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int has_app(const char *name) {
    int i;
    for (i = 0; i < g_app_count; i++)
        if (g_apps[i].name && name_eq(g_apps[i].name, name)) return 1;
    return 0;
}

static const char *focused_name(void) {
    desktop_window *w = focused_window();
    if (w && w->app_id >= 0 && w->app_id < g_app_count && g_apps[w->app_id].display_name)
        return g_apps[w->app_id].display_name;
    if (w) return w->title;
    return "桌面";
}

static int item_enabled(int action) {
    switch (action) {
    case A_ABOUT: return has_app("sysinfo");
    case A_POWER: return 1;
    case A_QUIT: case A_MIN: case A_ZOOM: case A_CLOSE: return focused_window() != 0;
    default: return 0;
    }
}

static int menu_items(int menu, const mb_item **out) {
    static mb_item app_items[1];
    if (menu == MB_APPLE) { *out = k_apple; return 4; }
    if (menu == MB_WINDOW) { *out = k_window; return 4; }
    if (menu == MB_APP) {
        int p = 0;
        const char *n = focused_name();
        const char *pre = "退出 ";
        while (*pre && p < 40) g_quit_label[p++] = *pre++;
        while (*n && p < 62) g_quit_label[p++] = *n++;
        g_quit_label[p] = 0;
        app_items[0].label = g_quit_label;
        app_items[0].action = A_QUIT;
        *out = app_items;
        return 1;
    }
    *out = 0;
    return 0;
}

static void text(const char *s, int x, int y, du_u32 c) {
    zh_draw_string(&g_fb, s, x, y, c, 0);
}

static void put2(char *b, int *p, int v) {
    b[(*p)++] = (char)('0' + (v / 10) % 10);
    b[(*p)++] = (char)('0' + v % 10);
}

static void put_num(char *b, int *p, int v) {
    if (v >= 10) b[(*p)++] = (char)('0' + (v / 10) % 10);
    b[(*p)++] = (char)('0' + v % 10);
}

static void put_s(char *b, int *p, const char *s) {
    while (*s) b[(*p)++] = *s++;
}

/* Sakamoto：0=周日 */
static int weekday(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

static void clock_text(char *b) {
    static const char *wd[7] = { "日", "一", "二", "三", "四", "五", "六" };
    int yy, mo, dd, hh, mi, ss, p = 0;
    rtc_read(&yy, &mo, &dd, &hh, &mi, &ss);
    if (yy < 100) yy += 2000;
    if (mo < 1 || mo > 12) mo = 1;
    put_s(b, &p, "周");
    put_s(b, &p, wd[weekday(yy, mo, dd)]);
    b[p++] = ' ';
    put_num(b, &p, mo);
    put_s(b, &p, "月");
    put_num(b, &p, dd);
    put_s(b, &p, "日 ");
    put2(b, &p, hh);
    b[p++] = ':';
    put2(b, &p, mi);
    b[p] = 0;
}

static void set_rect(int r[4], int x, int y, int w, int h) {
    r[0] = x; r[1] = y; r[2] = w; r[3] = h;
}

/* 控制中心图标：上下两枚开关胶囊 */
static void draw_cc_icon(int x, int y, du_u32 c) {
    aqua_round_band(&g_fb, x, y, 16, 7, 3, c, 255);
    aqua_disc(&g_fb, x + 2, y + 2, 3, c, 255);
    aqua_round_band(&g_fb, x, y + 9, 16, 7, 3, c, 255);
    aqua_disc(&g_fb, x + 11, y + 11, 3, c, 255);
}

void menubar_draw(void) {
    const aqua_theme *t = &g_aqua_theme;
    int W = (int)g_fb_w;
    int ty = (MB_H - zh_line_height(0)) / 2 + 1;
    int x, tw, m;
    char clk[48];
    const char *name;

    /* 浅蓝 → 浅绿渐变毛玻璃 */
    aqua_glass_hgrad(&g_fb, 0, 0, W, MB_H, 0, 0xFFB7E4FFu, 0xFFC8F6D4u, 175, 70, 256);
    aqua_fill_round_alpha(&g_fb, 0, MB_H - 1, W, 1, 0, 0xFF000000u, 24);

    /* 左侧：Apple / 应用名 / 窗口 */
    set_rect(g_title_rect[MB_APPLE], 8, 0, 32, MB_H);
    name = focused_name();
    tw = zh_string_width(name, 0);
    set_rect(g_title_rect[MB_APP], 42, 0, tw + 18, MB_H);
    x = g_title_rect[MB_APP][0] + g_title_rect[MB_APP][2] + 2;
    set_rect(g_title_rect[MB_WINDOW], x, 0, zh_string_width("窗口", 0) + 18, MB_H);
    for (m = MB_APPLE; m < MB_COUNT; m++) {
        if (g_mb_menu == m)
            aqua_fill_round_alpha(&g_fb, g_title_rect[m][0], 2, g_title_rect[m][2],
                                  MB_H - 4, 5, 0xFF000000u, 34);
    }
    taskbar_logo_draw(g_title_rect[MB_APPLE][0] + 8, 4, 16);
    set_rect(g_tb2_pwr_rect, g_title_rect[MB_APPLE][0], 0, g_title_rect[MB_APPLE][2], MB_H);
    text(name, g_title_rect[MB_APP][0] + 9, ty, t->text_primary);
    text(name, g_title_rect[MB_APP][0] + 10, ty, t->text_primary);
    text("窗口", g_title_rect[MB_WINDOW][0] + 9, ty, t->text_primary);

    /* 右侧：时钟 ← 控制中心 ← Wi-Fi */
    clock_text(clk);
    tw = zh_string_width(clk, 0);
    x = W - 12 - tw;
    text(clk, x, ty, t->text_primary);

    x -= 34;
    set_rect(g_cc_rect, x, 0, 28, MB_H);
    if (g_qs_open)
        aqua_fill_round_alpha(&g_fb, x, 2, 28, MB_H - 4, 5, 0xFF000000u, 34);
    draw_cc_icon(x + 6, 4, t->text_primary);
    set_rect(g_tb2_vol_rect, x, 0, 28, MB_H);

    x -= 32;
    set_rect(g_wifi_rect, x, 0, 28, MB_H);
    if (g_wifi_open)
        aqua_fill_round_alpha(&g_fb, x, 2, 28, MB_H - 4, 5, 0xFF000000u, 34);
    aqua_glyph(&g_fb, WF_ICON_NETWORK, x + 5, 3, 18,
               taskbar_net_up() ? t->text_primary : 0xFFA1A1A6u);
    set_rect(g_tb2_net_rect, x, 0, 28, MB_H);
}

void menubar_draw_menus(void) {
    const mb_item *items;
    const wf_theme *th = &g_wf_aqua;
    int n, i, mw = 200, mh = 10, x, y;
    if (g_mb_menu == MB_NONE) return;
    n = menu_items(g_mb_menu, &items);
    if (n > MB_MAX_ITEMS) n = MB_MAX_ITEMS;
    for (i = 0; i < n; i++) {
        if (items[i].label) {
            int lw = zh_string_width(items[i].label, 0) + 48;
            if (lw > mw) mw = lw;
            mh += MB_ROW_H;
        } else {
            mh += MB_SEP_H;
        }
    }
    x = g_title_rect[g_mb_menu][0];
    if (x + mw > (int)g_fb_w - 4) x = (int)g_fb_w - 4 - mw;
    y = MB_H + 1;
    set_rect(g_menu_rect, x, y, mw, mh);
    aqua_panel(&g_fb, x, y, mw, mh, th->radius_sm + 2);
    y += 5;
    g_item_n = n;
    for (i = 0; i < n; i++) {
        if (!items[i].label) {
            set_rect(g_item_rect[i], 0, 0, 0, 0);
            aqua_fill_round_alpha(&g_fb, x + 10, y + MB_SEP_H / 2, mw - 20, 1, 0,
                                  0xFF000000u, 30);
            y += MB_SEP_H;
            continue;
        }
        {
            int en = item_enabled(items[i].action);
            int hov;
            set_rect(g_item_rect[i], x + 5, y, mw - 10, MB_ROW_H);
            hov = en && rect_hit(g_item_rect[i], g_mouse_x, g_mouse_y);
            if (hov)
                aqua_fill_round_alpha(&g_fb, x + 5, y, mw - 10, MB_ROW_H, 4, th->accent, 255);
            text(items[i].label, x + 16, y + (MB_ROW_H - zh_line_height(0)) / 2 + 1,
                 hov ? th->on_accent : en ? th->text_primary : 0xFFB0B0B5u);
        }
        y += MB_ROW_H;
    }
}

void menubar_pointer(int mx, int my) {
    int m;
    if (g_mb_menu == MB_NONE || my >= MB_H) return;
    for (m = MB_APPLE; m < MB_COUNT; m++)
        if (rect_hit(g_title_rect[m], mx, my)) g_mb_menu = m;
}

int menubar_hover_key(void) {
    int i;
    if (g_mb_menu == MB_NONE) return 0;
    for (i = 0; i < g_item_n; i++)
        if (rect_hit(g_item_rect[i], g_mouse_x, g_mouse_y)) return g_mb_menu * 16 + i + 1;
    return g_mb_menu * 16;
}

static void do_action(int action) {
    desktop_window *w = focused_window();
    switch (action) {
    case A_ABOUT:
        slog("[menubar] about this mac -> sysinfo");
        launch_app_by_name("sysinfo");
        break;
    case A_POWER:
        taskbar_power_open();
        break;
    case A_QUIT:
    case A_CLOSE:
        if (w) win_close_request(w);
        break;
    case A_MIN:
        if (w) win_anim_minimize(w);
        break;
    case A_ZOOM:
        if (w) win_zoom_toggle(w);
        break;
    default:
        break;
    }
}

int menubar_click(int mx, int my) {
    int m, i;
    if (g_mb_menu != MB_NONE) {
        if (rect_hit(g_menu_rect, mx, my)) {
            const mb_item *items;
            int n = menu_items(g_mb_menu, &items);
            for (i = 0; i < n && i < g_item_n; i++) {
                if (!items[i].label || !rect_hit(g_item_rect[i], mx, my)) continue;
                if (!item_enabled(items[i].action)) return 1;
                g_mb_menu = MB_NONE;
                do_action(items[i].action);
                return 1;
            }
            return 1;
        }
        for (m = MB_APPLE; m < MB_COUNT; m++) {
            if (rect_hit(g_title_rect[m], mx, my)) {
                g_mb_menu = (g_mb_menu == m) ? MB_NONE : m;
                return 1;
            }
        }
        g_mb_menu = MB_NONE;     /* 点外部只收起菜单，不落到下层 */
        return 1;
    }
    if (my < 0 || my >= MB_H) return 0;
    for (m = MB_APPLE; m < MB_COUNT; m++) {
        if (rect_hit(g_title_rect[m], mx, my)) {
            taskbar_cards_close();
            g_mb_menu = m;
            return 1;
        }
    }
    if (rect_hit(g_wifi_rect, mx, my)) { taskbar_wifi_toggle(); return 1; }
    if (rect_hit(g_cc_rect, mx, my)) { taskbar_cc_toggle(); return 1; }
    return 1;
}

int menubar_key(u8 sc) {
    if (g_mb_menu == MB_NONE) return 0;
    if (sc == 0x01) { g_mb_menu = MB_NONE; return 1; }
    return 1;
}
