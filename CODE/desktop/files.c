/* files.c — 窗口化文件资源管理器
 *
 * 在桌面窗口里遍历 FAT32（含子目录与长文件名）。
 * 单击选中，双击或 Enter 进入目录；文本文件交给内置编辑器。
 * 左侧地点：此电脑 / 用户 / 桌面 / 系统。
 */
#include "desktop.h"
#include "zhfont.h"

#define FM_MAX     160
#define FM_NAME    64
#define FM_DEPTH   12
#define FM_TB      34
#define FM_HDR     22
#define FM_ST      22
#define FM_ROW     24
#define FM_SIDE    132
#define FM_READ_MAX 60000u

typedef struct {
    char name[FM_NAME];
    u32  clus;
    u32  size;
    u8   is_dir;
    u8   is_up;
} fm_ent;

typedef struct {
    fm_ent ents[FM_MAX];
    int    count;
    int    depth;
    int    sel;
    int    scroll;
    int    last_row;
    u64    last_tsc;
    u32    path_clus[FM_DEPTH];
    char   path_name[FM_DEPTH][FM_NAME];
    char   path_disp[160];
    char   status[96];
} fm_state;

static u8 g_fm_blob[FM_READ_MAX];

typedef struct {
    u32 bg, side, bar, line, text, dim, sel, accent, folder;
} fm_pal;

static void fm_colors(fm_pal *p) {
    if (g_fluent) {
        p->bg = 0xFF1E1E1Eu;
        p->side = 0xFF252526u;
        p->bar = 0xFF2D2D2Du;
        p->line = 0xFF3A3A3Au;
        p->text = 0xFFF3F3F3u;
        p->dim = 0xFF9A9A9Au;
        p->sel = 0xFF094771u;
        p->accent = 0xFF60CDFFu;
        p->folder = 0xFFE8B84Au;
    } else {
        p->bg = KS_BG_PRIMARY;
        p->side = KS_BG_SECONDARY;
        p->bar = KS_BG_TERTIARY;
        p->line = KS_BORDER;
        p->text = KS_TEXT_PRIMARY;
        p->dim = KS_TEXT_DIM;
        p->sel = KS_ACCENT_DIM;
        p->accent = KS_ACCENT;
        p->folder = KS_ACCENT2;
    }
}

static void fm_copy(char *dst, int cap, const char *src) {
    int i = 0;
    if (cap < 1) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void fm_set_status(fm_state *s, const char *msg) {
    fm_copy(s->status, (int)sizeof(s->status), msg);
}

static void fm_fit(char *dst, int cap, const char *src, int max_px) {
    fm_copy(dst, cap, src);
    int n = kstrlen(dst);
    while (n > 1 && zh_string_width(dst, 0) > max_px) {
        dst[--n] = 0;
        while (n > 0 && ((unsigned char)dst[n - 1] & 0xC0) == 0x80) dst[--n] = 0;
    }
}

static int fm_name_cmp(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 32);
        if (ca != cb) return ca < cb ? -1 : 1;
        a++; b++;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

static void fm_sort(fm_state *s) {
    for (int i = 1; i < s->count; i++) {
        fm_ent key = s->ents[i];
        int j = i - 1;
        while (j >= 0) {
            int dir_first = (s->ents[j].is_dir != key.is_dir)
                ? (key.is_dir && !s->ents[j].is_dir)
                : (fm_name_cmp(key.name, s->ents[j].name) < 0);
            if (!dir_first) break;
            s->ents[j + 1] = s->ents[j];
            j--;
        }
        s->ents[j + 1] = key;
    }
}

static void fm_build_path(fm_state *s) {
    int p = 0;
    s->path_disp[p++] = '/';
    for (int d = 0; d < s->depth && p < (int)sizeof(s->path_disp) - 2; d++) {
        const char *n = s->path_name[d];
        while (*n && p < (int)sizeof(s->path_disp) - 2) s->path_disp[p++] = *n++;
        s->path_disp[p++] = '/';
    }
    s->path_disp[p] = 0;
}

static void fm_status_count(fm_state *s) {
    int n = 0;
    char num[12];
    int p = 0;
    for (int i = 0; i < s->count; i++) if (!s->ents[i].is_up) n++;
    u32dec((u32)n, num);
    while (num[p] && p < 80) { s->status[p] = num[p]; p++; }
    {
        const char *b = " 个项目";
        while (*b && p < (int)sizeof(s->status) - 1) s->status[p++] = *b++;
    }
    s->status[p] = 0;
}

static int fm_collect(const f32_entry *e, void *ud) {
    fm_state *s = (fm_state *)ud;
    const char *nm;
    fm_ent *f;
    if (s->count >= FM_MAX) return 1;
    nm = (e->has_lfn && e->long_name[0]) ? e->long_name : e->name;
    if (!nm || !nm[0]) return 0;
    f = &s->ents[s->count];
    fm_copy(f->name, FM_NAME, nm);
    f->clus = e->clus;
    f->size = e->size;
    f->is_dir = e->is_dir ? 1 : 0;
    f->is_up = 0;
    s->count++;
    return 0;
}

static void fm_insert_up(fm_state *s) {
    if (s->depth <= 0) return;
    if (s->count >= FM_MAX) s->count = FM_MAX - 1;
    for (int i = s->count; i > 0; i--) s->ents[i] = s->ents[i - 1];
    fm_copy(s->ents[0].name, FM_NAME, "..");
    s->ents[0].clus = 0;
    s->ents[0].size = 0;
    s->ents[0].is_dir = 1;
    s->ents[0].is_up = 1;
    s->count++;
}

static int fm_reload(fm_state *s) {
    u32 clus;
    s->count = 0;
    s->sel = 0;
    s->scroll = 0;
    s->last_row = -1;
    fm_build_path(s);
    if (!g_block_read) {
        fm_set_status(s, "无块设备");
        return -1;
    }
    clus = (s->depth == 0) ? 0 : s->path_clus[s->depth - 1];
    if (f32_list_dir_w(clus, fm_collect, s) != 0) {
        s->count = 0;
        fm_set_status(s, "无法读取目录");
        slog("fileman list failed");
        return -1;
    }
    fm_sort(s);
    fm_insert_up(s);
    fm_status_count(s);
    return 0;
}

static void fm_up(fm_state *s) {
    if (s->depth <= 0) return;
    s->depth--;
    fm_reload(s);
}

static int fm_enter_idx(fm_state *s, int idx) {
    char name[FM_NAME];
    u32 clus;
    if (idx < 0 || idx >= s->count) return -1;
    if (s->ents[idx].is_up) { fm_up(s); return 0; }
    if (!s->ents[idx].is_dir) return -1;
    if (s->depth >= FM_DEPTH) { fm_set_status(s, "目录太深"); return -1; }
    clus = s->ents[idx].clus;
    if (clus < 2) { fm_set_status(s, "无法进入目录"); return -1; }
    fm_copy(name, FM_NAME, s->ents[idx].name);
    s->path_clus[s->depth] = clus;
    fm_copy(s->path_name[s->depth], FM_NAME, name);
    s->depth++;
    if (fm_reload(s) != 0) {
        s->depth--;
        fm_reload(s);
        fm_set_status(s, "无法读取目录");
        return -1;
    }
    slog("fileman enter dir");
    return 0;
}

static int fm_enter_named(fm_state *s, const char *name) {
    for (int i = 0; i < s->count; i++) {
        if (s->ents[i].is_up || !s->ents[i].is_dir) continue;
        if (fat32_lfn_streq_ci(s->ents[i].name, name))
            return fm_enter_idx(s, i);
    }
    return -1;
}

static void fm_place(fm_state *s, int place) {
    const char *a = 0;
    const char *b = 0;
    s->depth = 0;
    if (fm_reload(s) != 0) return;
    if (place == 1) a = "user";
    else if (place == 2) { a = "user"; b = "desktop"; }
    else if (place == 3) a = "system";
    if (a && fm_enter_named(s, a) != 0) { fm_set_status(s, "路径不存在"); return; }
    if (b && fm_enter_named(s, b) != 0) fm_set_status(s, "路径不存在");
}

static int fm_place_of(const fm_state *s) {
    if (s->depth == 0) return 0;
    if (s->depth == 1 && fat32_lfn_streq_ci(s->path_name[0], "user")) return 1;
    if (s->depth == 1 && fat32_lfn_streq_ci(s->path_name[0], "system")) return 3;
    if (s->depth == 2 && fat32_lfn_streq_ci(s->path_name[0], "user") &&
        fat32_lfn_streq_ci(s->path_name[1], "desktop")) return 2;
    return -1;
}

static int fm_looks_text(const u8 *p, u32 n) {
    u32 sample, bad = 0;
    if (n == 0) return 1;
    sample = n > 512 ? 512 : n;
    for (u32 i = 0; i < sample; i++) {
        u8 c = p[i];
        if (c == 0) return 0;
        if (c < 9 || (c > 13 && c < 32) || c == 127) bad++;
    }
    return bad * 10 < sample;
}

static void fm_open_file(fm_state *s, int idx) {
    fm_ent *e;
    u32 n;
    if (idx < 0 || idx >= s->count) return;
    e = &s->ents[idx];
    if (e->size == 0) {
        editor_open_text(e->name, (const u8 *)"", 0);
        fm_set_status(s, "已在编辑器中打开");
        slog("fileman open file");
        return;
    }
    if (e->clus < 2) { fm_set_status(s, "读取失败"); return; }
    n = e->size;
    if (n > FM_READ_MAX) n = FM_READ_MAX;
    for (u32 i = 0; i < n; i++) g_fm_blob[i] = 0;
    if (f32_read_file_by_clus_w(e->clus, e->size, g_fm_blob, FM_READ_MAX) != 0) {
        fm_set_status(s, "读取失败");
        slog("fileman read failed");
        return;
    }
    if (!fm_looks_text(g_fm_blob, n)) {
        fm_set_status(s, "二进制文件，未打开");
        return;
    }
    editor_open_text(e->name, g_fm_blob, n);
    if (e->size > n) fm_set_status(s, "已打开（内容已截断）");
    else fm_set_status(s, "已在编辑器中打开");
    slog("fileman open file");
}

static void fm_activate(fm_state *s, int idx) {
    if (idx < 0 || idx >= s->count) return;
    s->sel = idx;
    if (s->ents[idx].is_dir || s->ents[idx].is_up) fm_enter_idx(s, idx);
    else fm_open_file(s, idx);
}

static void fm_reveal(fm_state *s, int vis) {
    if (vis < 1) vis = 1;
    if (s->count <= 0) { s->sel = 0; s->scroll = 0; return; }
    if (s->sel < 0) s->sel = 0;
    if (s->sel >= s->count) s->sel = s->count - 1;
    if (s->sel < s->scroll) s->scroll = s->sel;
    if (s->sel >= s->scroll + vis) s->scroll = s->sel - vis + 1;
    if (s->scroll < 0) s->scroll = 0;
}

static void fm_geom(const app_ctx *ctx, int *side, int *lx, int *ly,
                    int *lw, int *lh, int *vis) {
    int sidew = (ctx->client_w < 420) ? 0 : FM_SIDE;
    int body = ctx->client_h - FM_TB - FM_ST;
    if (body < FM_HDR + FM_ROW) body = FM_HDR + FM_ROW;
    *side = sidew;
    *lx = ctx->client_x + sidew;
    *ly = ctx->client_y + FM_TB + FM_HDR;
    *lw = ctx->client_w - sidew;
    *lh = body - FM_HDR;
    *vis = *lh / FM_ROW;
    if (*vis < 1) *vis = 1;
}

static void fm_size_str(char *out, u32 sz) {
    char num[12];
    const char *unit;
    u32 v;
    int j = 0;
    if (sz < 1024u) { v = sz; unit = " B"; }
    else if (sz < 1024u * 1024u) { v = sz / 1024u; unit = " K"; }
    else { v = sz / (1024u * 1024u); unit = " M"; }
    u32dec(v, num);
    while (num[j] && j < 12) { out[j] = num[j]; j++; }
    while (*unit && j < 15) out[j++] = *unit++;
    out[j] = 0;
}

static const char *fm_kind(const fm_ent *e) {
    if (e->is_up) return "上级";
    if (e->is_dir) return "文件夹";
    if (fat32_lfn_ends_with_ci(e->name, ".txt") ||
        fat32_lfn_ends_with_ci(e->name, ".md") ||
        fat32_lfn_ends_with_ci(e->name, ".cnf") ||
        fat32_lfn_ends_with_ci(e->name, ".json") ||
        fat32_lfn_ends_with_ci(e->name, ".ini") ||
        fat32_lfn_ends_with_ci(e->name, ".log") ||
        fat32_lfn_ends_with_ci(e->name, ".h") ||
        fat32_lfn_ends_with_ci(e->name, ".c"))
        return "文本";
    if (fat32_lfn_ends_with_ci(e->name, ".elf") ||
        fat32_lfn_ends_with_ci(e->name, ".exe") ||
        fat32_lfn_ends_with_ci(e->name, ".drv"))
        return "程序";
    if (fat32_lfn_ends_with_ci(e->name, ".png") ||
        fat32_lfn_ends_with_ci(e->name, ".rgba"))
        return "图片";
    return "文件";
}

static int fm_hit_row(const app_ctx *ctx, int my, int scroll) {
    int side, lx, ly, lw, lh, vis, row;
    fm_geom(ctx, &side, &lx, &ly, &lw, &lh, &vis);
    (void)side; (void)lx; (void)lw;
    if (my < ly) return -1;
    row = (my - ly) / FM_ROW;
    if (row < 0 || row >= vis) return -1;
    return scroll + row;
}

void *fileman_on_create(app_ctx *ctx) {
    fm_state *s = (fm_state *)ADDR_FILEMAN;
    const char *t = "Files";
    u8 *raw = (u8 *)s;
    for (u32 i = 0; i < (u32)sizeof(fm_state); i++) raw[i] = 0;
    s->last_row = -1;
    fm_reload(s);
    for (int i = 0; i < 47; i++) ctx->win->title[i] = 0;
    for (int i = 0; t[i] && i < 47; i++) ctx->win->title[i] = t[i];
    slog("fileman window");
    return s;
}

void fileman_on_destroy(void *state) { (void)state; }

void fileman_on_event(void *state, app_ctx *ctx, desktop_event *ev) {
    fm_state *s = (fm_state *)state;
    int side, lx, ly, lw, lh, vis;
    int cx = ctx->client_x;
    int cy = ctx->client_y;
    fm_geom(ctx, &side, &lx, &ly, &lw, &lh, &vis);

    if (ev->type == EV_KEY_DOWN) {
        u8 sc = ev->scancode;
        if (sc == 0x48) {
            if (s->sel > 0) s->sel--;
        } else if (sc == 0x50) {
            if (s->sel < s->count - 1) s->sel++;
        } else if (sc == 0x49) {
            s->sel -= vis;
            if (s->sel < 0) s->sel = 0;
        } else if (sc == 0x51) {
            s->sel += vis;
            if (s->count > 0 && s->sel >= s->count) s->sel = s->count - 1;
        } else if (sc == 0x47) {
            s->sel = 0;
        } else if (sc == 0x4F) {
            s->sel = (s->count > 0) ? s->count - 1 : 0;
        } else if (sc == 0x0E || sc == 0x4B) {
            fm_up(s);
        } else if (sc == 0x1C || sc == 0x4D) {
            fm_activate(s, s->sel);
        } else if (sc == 0x3F) {
            int keep = s->sel;
            fm_reload(s);
            s->sel = keep;
            if (s->count > 0 && s->sel >= s->count) s->sel = s->count - 1;
        }
        fm_reveal(s, vis);
        ctx->win->dirty = 1;
        return;
    }

    if (ev->type != EV_MOUSE_DOWN || ev->button != 0) return;

    /* 上级 */
    {
        int bw = zh_string_width("上级", 0) + 16;
        if (ev->mx >= cx + 8 && ev->mx < cx + 8 + bw &&
            ev->my >= cy + 6 && ev->my < cy + 6 + 22) {
            fm_up(s);
            ctx->win->dirty = 1;
            return;
        }
    }

    /* 地点 */
    if (side > 0 && ev->mx >= cx && ev->mx < cx + side &&
        ev->my >= cy + FM_TB && ev->my < cy + ctx->client_h - FM_ST) {
        int rel = ev->my - (cy + FM_TB + 8);
        int slot = rel / 28;
        if (rel >= 0 && slot >= 0 && slot < 4) fm_place(s, slot);
        ctx->win->dirty = 1;
        return;
    }

    /* 滚动条 */
    if (s->count > vis && ev->mx >= lx + lw - 10 && ev->mx < lx + lw &&
        ev->my >= ly && ev->my < ly + lh) {
        int max_scroll = s->count - vis;
        int rel = ev->my - ly;
        if (lh > 0 && max_scroll > 0)
            s->scroll = rel * max_scroll / lh;
        if (s->scroll < 0) s->scroll = 0;
        if (s->scroll > max_scroll) s->scroll = max_scroll;
        ctx->win->dirty = 1;
        return;
    }

    /* 列表行：单击选中，双击打开 */
    if (ev->mx >= lx && ev->mx < lx + lw - 10) {
        int idx = fm_hit_row(ctx, ev->my, s->scroll);
        if (idx >= 0 && idx < s->count) {
            int dbl = 0;
            u64 now = rdtsc();
            if (s->last_row == idx) {
                if (!g_tsc_per_sec || (now - s->last_tsc) < g_tsc_per_sec / 2)
                    dbl = 1;
            }
            s->last_row = idx;
            s->last_tsc = now;
            s->sel = idx;
            if (dbl) fm_activate(s, idx);
            ctx->win->dirty = 1;
        }
    }
}

void fileman_on_draw(void *state, app_ctx *ctx) {
    fm_state *s = (fm_state *)state;
    fm_pal pal;
    int side, lx, ly, lw, lh, vis;
    int cx = ctx->client_x;
    int cy = ctx->client_y;
    int cw = ctx->client_w;
    int ch = ctx->client_h;
    static const char *places[4] = { "此电脑", "用户", "桌面", "系统" };
    int place = fm_place_of(s);
    int up_w;
    char fitted[FM_NAME];

    fm_colors(&pal);
    fm_geom(ctx, &side, &lx, &ly, &lw, &lh, &vis);
    if (s->scroll < 0) s->scroll = 0;
    if (s->count > vis && s->scroll > s->count - vis) s->scroll = s->count - vis;
    if (s->count <= vis) s->scroll = 0;

    du_fill_rect(&g_fb, cx, cy, cw, ch, pal.bg);

    /* 工具栏 */
    du_fill_rect(&g_fb, cx, cy, cw, FM_TB, pal.bar);
    du_divider_h(&g_fb, cx, cy + FM_TB, cw, pal.line);
    up_w = zh_string_width("上级", 0) + 16;
    du_fill_rounded_rect(&g_fb, cx + 8, cy + 6, up_w, 22,
                         (s->depth > 0) ? pal.side : pal.bg, 4);
    zh_draw_string(&g_fb, "上级", cx + 16, cy + 8,
                   (s->depth > 0) ? pal.text : pal.dim, 0);
    fm_fit(fitted, (int)sizeof(fitted), s->path_disp[0] ? s->path_disp : "/",
           cw - (up_w + 28));
    zh_draw_string(&g_fb, fitted, cx + 8 + up_w + 10, cy + 8, pal.accent, 0);

    /* 地点 */
    if (side > 0) {
        du_fill_rect(&g_fb, cx, cy + FM_TB, side, ch - FM_TB - FM_ST, pal.side);
        du_fill_rect(&g_fb, cx + side - 1, cy + FM_TB, 1, ch - FM_TB - FM_ST, pal.line);
        for (int i = 0; i < 4; i++) {
            int y = cy + FM_TB + 8 + i * 28;
            if (i == place)
                du_fill_rect(&g_fb, cx + 6, y, side - 14, 24, pal.sel);
            zh_draw_string(&g_fb, places[i], cx + 14, y + 3,
                           (i == place) ? pal.text : pal.dim, 0);
        }
    }

    /* 列头 */
    du_fill_rect(&g_fb, lx, cy + FM_TB, lw, FM_HDR, pal.bar);
    zh_draw_string(&g_fb, "名称", lx + 28, cy + FM_TB + 2, pal.dim, 0);
    zh_draw_string(&g_fb, "大小", lx + lw - 150, cy + FM_TB + 2, pal.dim, 0);
    zh_draw_string(&g_fb, "类型", lx + lw - 64, cy + FM_TB + 2, pal.dim, 0);
    du_divider_h(&g_fb, lx, ly, lw, pal.line);

    if (s->count == 0) {
        const char *empty = s->status[0] ? s->status : "此文件夹为空";
        zh_draw_string(&g_fb, empty, lx + 16, ly + 12, pal.dim, 0);
    }

    for (int i = 0; i < vis; i++) {
        int idx = s->scroll + i;
        int y, name_w;
        char shown[FM_NAME];
        char sz[16];
        if (idx >= s->count) break;
        y = ly + i * FM_ROW;
        if (idx == s->sel)
            du_fill_rect(&g_fb, lx, y, lw - 10, FM_ROW, pal.sel);
        if (s->ents[idx].is_dir) {
            du_fill_rounded_rect(&g_fb, lx + 8, y + 6, 14, 10, pal.folder, 2);
        } else {
            du_fill_rect(&g_fb, lx + 8, y + 5, 12, 14, pal.line);
            du_rect_outline(&g_fb, lx + 8, y + 5, 12, 14, pal.dim, 1);
        }
        name_w = lw - 28 - 160;
        if (name_w < 40) name_w = 40;
        fm_fit(shown, FM_NAME, s->ents[idx].name, name_w);
        zh_draw_string(&g_fb, shown, lx + 28, y + 3, pal.text, 0);
        if (!s->ents[idx].is_dir && !s->ents[idx].is_up) {
            fm_size_str(sz, s->ents[idx].size);
            zh_draw_string(&g_fb, sz, lx + lw - 150, y + 3, pal.dim, 0);
        }
        zh_draw_string(&g_fb, fm_kind(&s->ents[idx]), lx + lw - 64, y + 3, pal.dim, 0);
    }

    if (s->count > vis && lh > 0) {
        int max_scroll = s->count - vis;
        int thumb_h = lh * vis / s->count;
        int thumb_y;
        if (thumb_h < 16) thumb_h = 16;
        if (thumb_h > lh) thumb_h = lh;
        thumb_y = ly;
        if (max_scroll > 0 && lh > thumb_h)
            thumb_y = ly + (lh - thumb_h) * s->scroll / max_scroll;
        du_fill_rect(&g_fb, lx + lw - 8, ly, 6, lh, pal.line);
        du_fill_rounded_rect(&g_fb, lx + lw - 8, thumb_y, 6, thumb_h, pal.dim, 2);
    }

    /* 状态栏 */
    du_fill_rect(&g_fb, cx, cy + ch - FM_ST, cw, FM_ST, pal.bar);
    du_divider_h(&g_fb, cx, cy + ch - FM_ST, cw, pal.line);
    zh_draw_string(&g_fb, s->status, cx + 10, cy + ch - FM_ST + 2, pal.dim, 0);
}
