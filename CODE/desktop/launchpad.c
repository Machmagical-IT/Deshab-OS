/* launchpad.c — Aqua Launchpad（替代 M3 Win11 开始菜单）
 *
 * 全屏毛玻璃（模糊壁纸 + 深色 tint），顶部居中搜索框，7 列 64px 图标网格，
 * 图标下方标签。本机条目来自 STARTMENU.CNF；后面接 GUESTMENU.CNF
 * 扫到的 Linux .desktop 与 Windows「开始菜单」快捷方式。多过一屏时
 * 方向键把选中项滚进画面，PgUp/PgDn 翻页。
 *
 * 开关：opacity ease 180ms；网格从 0.94 倍绕屏幕中心缩放到 1 倍
 * （图标尺寸与间距一起缩放，标签在不透明度接近满时才画）。
 * 交互：点击图标启动并关闭；点空白 / Esc / Dock 启动台按钮关闭；
 *       ASCII 输入实时过滤（标签 + 内部名，不区分大小写子串），
 *       方向键移动选中，Enter 启动选中项。
 * Launchpad 画在顶栏之上、Dock 之下：打开时 Dock 仍可点击。
 */
#include "desktop.h"
#include "aqua_draw.h"
#include "zhfont.h"

int g_lp_open = 0;
static int g_lp_visible;
static motion_track g_lp_fade;
static int g_lp_inited;

#define LP_SEARCH_MAX 28
#define LP_COLS       7
#define LP_ICON       64
#define LP_CELL_H     112
#define LP_GRID_TOP   104
#define LP_FADE_MS    180
#define LP_CAT_MAX    (SM_MAX_APPS + GUEST_MENU_MAX)
#define LP_PAGE_MAX   96

/* kind 0 = 本机 g_sm_* 下标，kind 1 = g_guest_* 下标 */
typedef struct { u8 kind; u16 idx; } lp_ent;

static char g_lp_search[LP_SEARCH_MAX + 1];
static int  g_lp_search_len;
static int  g_lp_sel;          /* g_lp_filter 下标 */
static lp_ent g_lp_filter[LP_CAT_MAX];
static int  g_lp_filter_n;
static int  g_lp_shown;
static int  g_lp_rect[LP_PAGE_MAX][4];
static int  g_lp_hover = -1;
static int  g_lp_row0;
static int  g_lp_rows = 1;

static int lp_text_hit(const char *s) {
    int b, k, m;
    if (!s) return 0;
    for (b = 0; s[b]; b++) {
        m = 1;
        for (k = 0; k < g_lp_search_len; k++) {
            char c1 = s[b + k], c2 = g_lp_search[k];
            if (!c1) { m = 0; break; }
            if (c1 >= 'a' && c1 <= 'z') c1 = (char)(c1 - 32);
            if (c2 >= 'a' && c2 <= 'z') c2 = (char)(c2 - 32);
            if (c1 != c2) { m = 0; break; }
        }
        if (m) return 1;
    }
    return 0;
}

static void lp_put_num(char *d, int *o, int cap, int n);

static int lp_vis(void) {
    int v = g_lp_rows * LP_COLS;
    if (v < 1) v = 1;
    if (v > LP_PAGE_MAX) v = LP_PAGE_MAX;
    return v;
}

static void lp_init(void) {
    if (g_lp_inited) return;
    motion_bind(&g_lp_fade, 0);
    g_lp_inited = 1;
}

static void lp_push(u8 kind, int idx, const char *a, const char *b) {
    if (g_lp_filter_n >= LP_CAT_MAX) return;
    if (g_lp_search_len && !lp_text_hit(a) && !lp_text_hit(b)) return;
    g_lp_filter[g_lp_filter_n].kind = kind;
    g_lp_filter[g_lp_filter_n].idx = (u16)idx;
    g_lp_filter_n++;
}

static void lp_filter_rebuild(void) {
    int i;
    g_lp_filter_n = 0;
    g_lp_row0 = 0;
    g_lp_sel = 0;
    for (i = 0; i < g_sm_count; i++) {
        const char *nm = 0;
        int id = g_sm_ids[i];
        if (id >= 0 && id < g_app_count) nm = g_apps[id].name;
        lp_push(0, i, g_sm_labels[i], nm);
    }
    for (i = 0; i < g_guest_count; i++)
        lp_push(1, i, g_guest_label[i], g_guest_name[i]);
}

void launchpad_open(void) {
    lp_init();
    g_lp_open = 1;
    g_lp_visible = 1;
    g_lp_search[0] = 0;
    g_lp_search_len = 0;
    g_lp_sel = 0;
    g_lp_hover = -1;
    lp_filter_rebuild();
    taskbar_cards_close();
    menubar_close_menus();
    g_taskview = 0;
    motion_ease_to(&g_lp_fade, 256, LP_FADE_MS);
    slog("[launchpad] open");
}

void launchpad_close(void) {
    if (!g_lp_open) return;
    lp_init();
    g_lp_open = 0;
    motion_ease_to(&g_lp_fade, 0, LP_FADE_MS);
    slog("[launchpad] close");
}

void launchpad_toggle(void) {
    if (g_lp_open) launchpad_close();
    else launchpad_open();
}

int launchpad_tick(int dt_ms) {
    int act;
    lp_init();
    act = motion_step(&g_lp_fade, dt_ms);
    if (!g_lp_open && !g_lp_fade.active) g_lp_visible = 0;
    return act;
}

int launchpad_hover_key(void) {
    if (!g_lp_open) return 0;
    return ((g_lp_hover + 1) << 8) | (g_lp_sel & 0xFF);
}

/* 标签截断：zh 16px ASCII 半宽 12 / CJK 全宽 16 */
static void lp_label_clip(char *dst, int cap, const char *src, int max_w) {
    int w = 0, o = 0;
    for (int i = 0; src[i] && o < cap - 4; ) {
        unsigned char c = (unsigned char)src[i];
        int cw = (c < 0x80) ? 12 : 16;
        if (w + cw > max_w) break;
        dst[o++] = src[i++];
        if (c >= 0x80) {
            int cont = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
            while (cont-- > 0 && src[i] && o < cap - 4)
                dst[o++] = src[i++];
        }
        w += cw;
    }
    dst[o] = 0;
}

void launchpad_draw(void) {
    int W = (int)g_fb_w, H = (int)g_fb_h;
    int op, s, i, cell_w, gx, rows_cap, cap;
    if (!g_lp_visible) return;
    op = motion_px(&g_lp_fade);
    if (op <= 0) return;
    if (op > 256) op = 256;
    s = 240 + 16 * op / 256;

    aqua_glass(&g_fb, 0, 0, W, H, 0, 0xFF1C1C1Eu, 96, 0, op);

    /* 搜索框 */
    {
        int bw = 240 * s / 256, bh = 28;
        int bx = (W - bw) / 2, by = 44;
        aqua_fill_round_alpha(&g_fb, bx, by, bw, bh, 7, 0xFFFFFFFFu, 46 * op / 256);
        aqua_round_band(&g_fb, bx, by, bw, bh, 7, 0xFFFFFFFFu, 60 * op / 256);
        if (op >= 200) {
            const char *txt = g_lp_search_len ? g_lp_search : "搜索";
            int tw = zh_string_width(txt, 0);
            int tx = g_lp_search_len ? bx + 30 : (W - tw) / 2 + 10;
            aqua_glyph(&g_fb, WF_ICON_SEARCH, tx - 22, by + 6, 16, 0xFFE5E5EAu);
            zh_draw_string(&g_fb, txt, tx, by + (bh - zh_line_height(0)) / 2 + 1,
                           g_lp_search_len ? 0xFFFFFFFFu : 0xFFC7C7CCu, 0);
        }
    }

    cell_w = (W - 160) / LP_COLS;
    if (cell_w > 150) cell_w = 150;
    if (cell_w < 84) cell_w = 84;
    gx = (W - cell_w * LP_COLS) / 2;
    rows_cap = (H - LP_GRID_TOP - AQUA_DOCK_RESERVE - 24) / LP_CELL_H;
    if (rows_cap < 1) rows_cap = 1;
    g_lp_rows = rows_cap;
    cap = lp_vis();
    {
        int first, max_row, vis_n;
        vis_n = g_lp_filter_n;
        if (g_lp_sel >= vis_n) g_lp_sel = vis_n ? vis_n - 1 : 0;
        if (g_lp_sel < 0) g_lp_sel = 0;
        if (vis_n > cap) {
            max_row = (vis_n - cap + LP_COLS - 1) / LP_COLS;
            if (g_lp_row0 > max_row) g_lp_row0 = max_row;
        }
        if (g_lp_row0 < 0) g_lp_row0 = 0;
        first = g_lp_row0 * LP_COLS;
        if (vis_n && g_lp_sel < first) g_lp_row0 = g_lp_sel / LP_COLS;
        else if (vis_n && g_lp_sel >= first + cap)
            g_lp_row0 = g_lp_sel / LP_COLS - (g_lp_rows - 1);
        if (g_lp_row0 < 0) g_lp_row0 = 0;
        first = g_lp_row0 * LP_COLS;
        g_lp_shown = vis_n - first;
        if (g_lp_shown > cap) g_lp_shown = cap;
        if (g_lp_shown < 0) g_lp_shown = 0;
    }

    g_lp_hover = -1;
    for (i = 0; i < g_lp_shown; i++) {
        int abs = g_lp_row0 * LP_COLS + i;
        lp_ent e = g_lp_filter[abs];
        int id = AQUA_ICON_LINUX;
        const char *lab = "";
        int col = i % LP_COLS, row = i / LP_COLS;
        if (e.kind == 0) {
            lab = g_sm_labels[e.idx];
            id = g_sm_ids[e.idx];
        } else if (e.idx < GUEST_MENU_MAX) {
            lab = g_guest_label[e.idx];
            id = (g_guest_kind[e.idx] == 'W') ? AQUA_ICON_WINDOWS : AQUA_ICON_LINUX;
        }
        int cx = gx + col * cell_w + cell_w / 2;
        int cy = LP_GRID_TOP + row * LP_CELL_H + LP_ICON / 2 + 8;
        int sx = W / 2 + (cx - W / 2) * s / 256;
        int sy = H / 2 + (cy - H / 2) * s / 256;
        int sz = LP_ICON * s / 256;
        int hit_w = cell_w * s / 256;
        g_lp_rect[i][0] = sx - hit_w / 2;
        g_lp_rect[i][1] = sy - sz / 2 - 8;
        g_lp_rect[i][2] = hit_w;
        g_lp_rect[i][3] = sz + 40;
        if (g_lp_open && rect_hit(g_lp_rect[i], g_mouse_x, g_mouse_y)) g_lp_hover = i;
        if (g_lp_open && (i == g_lp_hover || abs == g_lp_sel))
            aqua_fill_round_alpha(&g_fb, sx - sz / 2 - 10, sy - sz / 2 - 8, sz + 20,
                                  sz + 16, 14, 0xFFFFFFFFu, 40 * op / 256);
        aqua_app_icon(&g_fb, id, sx - sz / 2, sy - sz / 2, sz, op);
        if (op >= 200) {
            char label[52];
            int tw, tx, ty;
            lp_label_clip(label, 52, lab, cell_w - 16);
            tw = zh_string_width(label, 0);
            tx = sx - tw / 2;
            ty = sy + sz / 2 + 6;
            zh_draw_string(&g_fb, label, tx + 1, ty + 1, 0xFF000000u, 0);
            zh_draw_string(&g_fb, label, tx, ty, 0xFFFFFFFFu, 0);
        }
    }
    if (g_lp_search_len && g_lp_filter_n == 0 && op >= 200) {
        const char *m = "无匹配应用";
        zh_draw_string(&g_fb, m, (W - zh_string_width(m, 0)) / 2, LP_GRID_TOP + 40,
                       0xFFC7C7CCu, 0);
    }
    if (op >= 200 && g_lp_filter_n > g_lp_shown) {
        char buf[24];
        int o = 0, first = g_lp_row0 * LP_COLS;
        int from = first + 1, to = first + g_lp_shown;
        lp_put_num(buf, &o, 24, from);
        if (o < 23) buf[o++] = '-';
        lp_put_num(buf, &o, 24, to);
        if (o < 22) { buf[o++] = ' '; buf[o++] = '/'; buf[o++] = ' '; }
        lp_put_num(buf, &o, 24, g_lp_filter_n);
        buf[o] = 0;
        zh_draw_string(&g_fb, buf, (W - zh_string_width(buf, 0)) / 2,
                       H - AQUA_DOCK_RESERVE - 18, 0xFFC7C7CCu, 0);
    }
}

static void lp_put_num(char *d, int *o, int cap, int n) {
    char tmp[8];
    int k = 0;
    if (n < 0) n = 0;
    if (n == 0) tmp[k++] = '0';
    while (n && k < 8) { tmp[k++] = (char)('0' + (n % 10)); n /= 10; }
    while (k > 0 && *o < cap - 1) d[(*o)++] = tmp[--k];
}

static void lp_launch(int i) {
    int abs;
    lp_ent e;
    if (i < 0 || i >= g_lp_shown) return;
    abs = g_lp_row0 * LP_COLS + i;
    e = g_lp_filter[abs];
    slog("[launchpad] launch:");
    launchpad_close();
    if (e.kind == 0) {
        int id = g_sm_ids[e.idx];
        if (id >= 0 && id < g_app_count) slog(g_apps[id].name);
        launch_app(id);
        return;
    }
    if (e.idx < GUEST_MENU_MAX) slog(g_guest_label[e.idx]);
    launch_guest_entry((int)e.idx);
}

int launchpad_click(int mx, int my) {
    int i;
    if (!g_lp_open) return 0;
    if (dock_click(mx, my)) return 1;
    for (i = 0; i < g_lp_shown; i++) {
        if (!rect_hit(g_lp_rect[i], mx, my)) continue;
        lp_launch(i);
        return 1;
    }
    launchpad_close();
    return 1;
}

int launchpad_key(u8 sc, int shift) {
    if (!g_lp_open) return 0;
    if (sc == 0x01) { launchpad_close(); return 1; }        /* Esc */
    if (sc == 0x0E) {                                       /* Backspace */
        if (g_lp_search_len > 0) {
            g_lp_search[--g_lp_search_len] = 0;
            lp_filter_rebuild();
        }
        return 1;
    }
    if (sc == 0x1C) {                                       /* Enter */
        lp_launch(g_lp_sel - g_lp_row0 * LP_COLS);
        return 1;
    }
    if (sc == 0x4B) { if (g_lp_sel > 0) g_lp_sel--; return 1; }              /* ← */
    if (sc == 0x4D) { if (g_lp_sel + 1 < g_lp_filter_n) g_lp_sel++; return 1; } /* → */
    if (sc == 0x48) { if (g_lp_sel >= LP_COLS) g_lp_sel -= LP_COLS; return 1; } /* ↑ */
    if (sc == 0x50) {                                                         /* ↓ */
        if (g_lp_sel + LP_COLS < g_lp_filter_n) g_lp_sel += LP_COLS;
        return 1;
    }
    if (sc == 0x49 || sc == 0x51) {                         /* PgUp / PgDn */
        int page = g_lp_rows * LP_COLS;
        if (page < 1) page = 1;
        if (sc == 0x49) g_lp_sel -= page;
        else g_lp_sel += page;
        if (g_lp_sel < 0) g_lp_sel = 0;
        if (g_lp_sel >= g_lp_filter_n)
            g_lp_sel = g_lp_filter_n ? g_lp_filter_n - 1 : 0;
        return 1;
    }

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
            if (g_lp_search_len < LP_SEARCH_MAX) {
                g_lp_search[g_lp_search_len++] = c;
                g_lp_search[g_lp_search_len] = 0;
                g_lp_sel = 0;
                lp_filter_rebuild();
            }
        }
    }
    return 1;
}
