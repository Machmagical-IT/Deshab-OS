/* aqua_draw.c — 抗锯齿圆角、投影、圆点、毛玻璃、应用图标缓存。 */
#include "desktop.h"
#include "aqua_draw.h"
#include "glass.h"
#include "blit.h"

#define ICON_MASTER 96
#define ICON_MIP    48
#define ICON_SLOTS  (MAX_APPS + 4)

static du_u32 g_icon_master[ICON_SLOTS][ICON_MASTER * ICON_MASTER];
static du_u32 g_icon_mip[ICON_SLOTS][ICON_MIP * ICON_MIP];
static u8 g_icon_ready[ICON_SLOTS];

du_u32 aqua_mix(du_u32 a, du_u32 b, int t) {
    du_u32 ar, ag, ab, br, bg, bb;
    if (t <= 0) return a | 0xFF000000u;
    if (t >= 256) return b | 0xFF000000u;
    ar = (a >> 16) & 0xFF; ag = (a >> 8) & 0xFF; ab = a & 0xFF;
    br = (b >> 16) & 0xFF; bg = (b >> 8) & 0xFF; bb = b & 0xFF;
    ar = (ar * (du_u32)(256 - t) + br * (du_u32)t) >> 8;
    ag = (ag * (du_u32)(256 - t) + bg * (du_u32)t) >> 8;
    ab = (ab * (du_u32)(256 - t) + bb * (du_u32)t) >> 8;
    return 0xFF000000u | (ar << 16) | (ag << 8) | ab;
}

/* alpha 0..255 → 混合权重 0..256 */
static int a256(int alpha) {
    if (alpha <= 0) return 0;
    if (alpha >= 255) return 256;
    return alpha + (alpha >> 7);
}

static void plot(du_context *ctx, int x, int y, du_u32 c, int t) {
    du_u32 *p;
    if (t <= 0) return;
    if (x < 0 || y < 0 || (du_u64)x >= ctx->width || (du_u64)y >= ctx->height) return;
    p = (du_u32 *)((du_u8 *)ctx->fb + (du_u64)y * ctx->pitch) + x;
    *p = (t >= 256) ? (c | 0xFF000000u) : aqua_mix(*p, c, t);
}

static int clamp_r(int r, int w, int h) {
    if (r < 0) r = 0;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    return r;
}

/* 角区像素 (i,j)，圆心在 (c,c)、半径 rad（像素单位）。4x4 子采样。 */
static int cov_circle(int i, int j, int c, int rad) {
    int n = 0, ky, kx;
    int c8 = c * 8, r8 = rad * 8;
    int far_dx = c - i, far_dy = c - j;
    int near_dx = c - i - 1, near_dy = c - j - 1;
    if (near_dx < 0) near_dx = 0;
    if (near_dy < 0) near_dy = 0;
    if (far_dx * far_dx + far_dy * far_dy <= rad * rad) return 16;
    if (near_dx * near_dx + near_dy * near_dy >= rad * rad) return 0;
    for (ky = 0; ky < 4; ky++) {
        int dy = c8 - (8 * j + 2 * ky + 1);
        for (kx = 0; kx < 4; kx++) {
            int dx = c8 - (8 * i + 2 * kx + 1);
            if (dx * dx + dy * dy <= r8 * r8) n++;
        }
    }
    return n;
}

int aqua_round_cov(int col, int row, int w, int h, int r) {
    int i, j;
    if (col < 0 || row < 0 || col >= w || row >= h) return 0;
    r = clamp_r(r, w, h);
    if (r <= 0) return 16;
    if (col < r) i = col;
    else if (col >= w - r) i = w - 1 - col;
    else return 16;
    if (row < r) j = row;
    else if (row >= h - r) j = h - 1 - row;
    else return 16;
    return cov_circle(i, j, r, r);
}

void aqua_fill_round_rows(du_context *ctx, int x, int y, int w, int h,
                          int r, du_u32 color, int alpha, int row0, int row1) {
    int row, col, t;
    if (!ctx || w <= 0 || h <= 0) return;
    t = a256(alpha);
    if (t <= 0) return;
    r = clamp_r(r, w, h);
    if (row0 < 0) row0 = 0;
    if (row1 > h) row1 = h;
    for (row = row0; row < row1; row++) {
        int py = y + row;
        int corner_row = (row < r || row >= h - r);
        du_u32 *line;
        int c0, c1;
        if (py < 0 || (du_u64)py >= ctx->height) continue;
        line = (du_u32 *)((du_u8 *)ctx->fb + (du_u64)py * ctx->pitch);
        c0 = 0; c1 = w;
        if (corner_row) {
            for (col = 0; col < r; col++) {
                int cv = aqua_round_cov(col, row, w, h, r);
                plot(ctx, x + col, py, color, t * cv / 16);
                cv = aqua_round_cov(w - 1 - col, row, w, h, r);
                plot(ctx, x + w - 1 - col, py, color, t * cv / 16);
            }
            c0 = r; c1 = w - r;
        }
        if (x + c0 < 0) c0 = -x;
        if (x + c1 > (int)ctx->width) c1 = (int)ctx->width - x;
        if (t >= 256) {
            du_u32 c = color | 0xFF000000u;
            for (col = c0; col < c1; col++) line[x + col] = c;
        } else {
            for (col = c0; col < c1; col++)
                line[x + col] = aqua_mix(line[x + col], color, t);
        }
    }
}

void aqua_fill_round_alpha(du_context *ctx, int x, int y, int w, int h,
                           int r, du_u32 color, int alpha) {
    aqua_fill_round_rows(ctx, x, y, w, h, r, color, alpha, 0, h);
}

static void band_px(du_context *ctx, int x, int y, int w, int h, int r,
                    int col, int row, du_u32 color, int t) {
    int o = aqua_round_cov(col, row, w, h, r);
    int in = aqua_round_cov(col - 1, row - 1, w - 2, h - 2, r > 0 ? r - 1 : 0);
    int c = o - in;
    if (c <= 0) return;
    plot(ctx, x + col, y + row, color, t * c / 16);
}

void aqua_round_band(du_context *ctx, int x, int y, int w, int h,
                     int r, du_u32 color, int alpha) {
    int t, rr, row, col;
    if (!ctx || w < 2 || h < 2) return;
    t = a256(alpha);
    if (t <= 0) return;
    r = clamp_r(r, w, h);
    rr = r + 1;
    if (rr * 2 > w) rr = w / 2;
    if (rr * 2 > h) rr = h / 2;
    for (row = 0; row < rr; row++) {
        for (col = 0; col < rr; col++) {
            band_px(ctx, x, y, w, h, r, col, row, color, t);
            band_px(ctx, x, y, w, h, r, w - 1 - col, row, color, t);
            band_px(ctx, x, y, w, h, r, col, h - 1 - row, color, t);
            band_px(ctx, x, y, w, h, r, w - 1 - col, h - 1 - row, color, t);
        }
    }
    for (col = rr; col < w - rr; col++) {
        plot(ctx, x + col, y, color, t);
        plot(ctx, x + col, y + h - 1, color, t);
    }
    for (row = rr; row < h - rr; row++) {
        plot(ctx, x, y + row, color, t);
        plot(ctx, x + w - 1, y + row, color, t);
    }
}

void aqua_shadow(du_context *ctx, int x, int y, int w, int h, int r,
                 int strength, int layers) {
    int dy = 3, i, base, n1;
    if (!ctx || strength <= 0 || w <= 0 || h <= 0) return;
    if (strength > 255) strength = 255;
    if (layers < 1) layers = 1;
    if (layers > 16) layers = 16;
    base = strength / 2;
    /* 窗口底边以下露出的 dy 行 + 底角外侧 */
    aqua_fill_round_rows(ctx, x, y + dy, w, h, r, 0xFF000000u, base,
                         h - dy - r - 1, h);
    n1 = layers + 1;
    for (i = 1; i <= layers; i++) {
        int a = base * (n1 - i) * (n1 - i) / (n1 * n1);
        if (a <= 0) continue;
        aqua_round_band(ctx, x - i, y + dy - i, w + 2 * i, h + 2 * i, r + i,
                        0xFF000000u, a);
    }
}

void aqua_disc(du_context *ctx, int x, int y, int d, du_u32 color, int alpha) {
    int t, i, j;
    if (!ctx || d <= 0) return;
    t = a256(alpha);
    if (t <= 0) return;
    for (j = 0; j < d; j++) {
        for (i = 0; i < d; i++) {
            /* 圆心 (d/2,d/2)：以 1/8 像素为单位直接采样 */
            int n = 0, ky, kx;
            int c8 = d * 4;
            for (ky = 0; ky < 4; ky++) {
                int dy = 8 * j + 2 * ky + 1 - c8;
                for (kx = 0; kx < 4; kx++) {
                    int dx = 8 * i + 2 * kx + 1 - c8;
                    if (dx * dx + dy * dy <= c8 * c8) n++;
                }
            }
            if (n) plot(ctx, x + i, y + j, color, t * n / 16);
        }
    }
}

void aqua_glass(du_context *ctx, int x, int y, int w, int h, int r,
                du_u32 tint, int tint_a, int see, int opacity) {
    const du_u32 *gl;
    int gw = 0, gh = 0, row, col, tt;
    if (!ctx || w <= 0 || h <= 0 || opacity <= 0) return;
    if (opacity > 256) opacity = 256;
    if (see < 0) see = 0;
    if (see > 256) see = 256;
    tt = a256(tint_a);
    r = clamp_r(r, w, h);
    gl = glass_pixels(&gw, &gh);
    for (row = 0; row < h; row++) {
        int py = y + row;
        du_u32 *line;
        if (py < 0 || (du_u64)py >= ctx->height) continue;
        line = (du_u32 *)((du_u8 *)ctx->fb + (du_u64)py * ctx->pitch);
        for (col = 0; col < w; col++) {
            int px = x + col;
            int cov, a;
            du_u32 cur, s;
            if (px < 0 || (du_u64)px >= ctx->width) continue;
            cov = (row < r || row >= h - r) ? aqua_round_cov(col, row, w, h, r) : 16;
            if (!cov) continue;
            cur = line[px];
            if (gl && px < gw && py < gh)
                s = aqua_mix(gl[(du_i64)py * gw + px], cur, see);
            else
                s = cur;
            s = aqua_mix(s, tint, tt);
            a = opacity * cov / 16;
            line[px] = aqua_mix(cur, s, a);
        }
    }
}

void aqua_glass_hgrad(du_context *ctx, int x, int y, int w, int h, int r,
                      du_u32 tint0, du_u32 tint1, int tint_a, int see, int opacity) {
    const du_u32 *gl;
    int gw = 0, gh = 0, row, col, tt, denom;
    if (!ctx || w <= 0 || h <= 0 || opacity <= 0) return;
    if (opacity > 256) opacity = 256;
    if (see < 0) see = 0;
    if (see > 256) see = 256;
    tt = a256(tint_a);
    r = clamp_r(r, w, h);
    denom = w > 1 ? w - 1 : 1;
    gl = glass_pixels(&gw, &gh);
    for (row = 0; row < h; row++) {
        int py = y + row;
        du_u32 *line;
        if (py < 0 || (du_u64)py >= ctx->height) continue;
        line = (du_u32 *)((du_u8 *)ctx->fb + (du_u64)py * ctx->pitch);
        for (col = 0; col < w; col++) {
            int px = x + col;
            int cov, a;
            du_u32 cur, s, tint;
            if (px < 0 || (du_u64)px >= ctx->width) continue;
            cov = (row < r || row >= h - r) ? aqua_round_cov(col, row, w, h, r) : 16;
            if (!cov) continue;
            cur = line[px];
            if (gl && px < gw && py < gh)
                s = aqua_mix(gl[(du_i64)py * gw + px], cur, see);
            else
                s = cur;
            tint = aqua_mix(tint0, tint1, col * 256 / denom);
            s = aqua_mix(s, tint, tt);
            a = opacity * cov / 16;
            line[px] = aqua_mix(cur, s, a);
        }
    }
}

void aqua_panel(du_context *ctx, int x, int y, int w, int h, int r) {
    aqua_shadow(ctx, x, y, w, h, r, 90, 8);
    aqua_glass(ctx, x, y, w, h, r, 0xFFF6F6F6u, 215, 120, 256);
    aqua_round_band(ctx, x, y, w, h, r, 0xFF000000u, 36);
}

/* ============================================================
 *  应用图标
 * ============================================================ */

static int name_is(const char *n, const char *p) {
    while (*p) { if (*n++ != *p++) return 0; }
    return 1;
}

static du_u32 icon_base(int app_id) {
    static const du_u32 pal[8] = {
        0xFF0A84FFu, 0xFF30D158u, 0xFFFF9F0Au, 0xFFFF375Fu,
        0xFF5E5CE6u, 0xFF64D2FFu, 0xFFBF5AF2u, 0xFFFF6482u,
    };
    const char *n;
    u32 hsh = 2166136261u;
    if (app_id == AQUA_ICON_LAUNCHPAD) return 0xFF8E8E93u;
    if (app_id == AQUA_ICON_MISSION) return 0xFF3A3A3Cu;
    if (app_id == AQUA_ICON_LINUX) return 0xFFE95420u;
    if (app_id == AQUA_ICON_WINDOWS) return 0xFF0078D4u;
    if (app_id < 0 || app_id >= g_app_count) return 0xFF8E8E93u;
    n = g_apps[app_id].name;
    if (name_is(n, "shell") || name_is(n, "cmd")) return 0xFF2C2C2Eu;
    if (name_is(n, "fileman") || name_is(n, "files")) return 0xFF1E90FFu;
    if (name_is(n, "browser")) return 0xFF34AADCu;
    if (name_is(n, "settings")) return 0xFF8E8E93u;
    if (name_is(n, "calc")) return 0xFFFF9F0Au;
    if (name_is(n, "editor") || name_is(n, "proedit") || name_is(n, "notes"))
        return 0xFFFFB340u;
    if (name_is(n, "vscode")) return 0xFF007ACCu;
    if (name_is(n, "sysinfo")) return 0xFF5E5CE6u;
    if (name_is(n, "clock")) return 0xFF1C1C1Eu;
    if (name_is(n, "paint")) return 0xFFFF375Fu;
    if (name_is(n, "diskinfo")) return 0xFF30B158u;
    while (*n) { hsh ^= (u8)*n++; hsh *= 16777619u; }
    return pal[hsh & 7];
}

static du_u32 shade(du_u32 c, int d) {
    int r = (int)((c >> 16) & 0xFF) + d;
    int g = (int)((c >> 8) & 0xFF) + d;
    int b = (int)(c & 0xFF) + d;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xFF000000u | ((du_u32)r << 16) | ((du_u32)g << 8) | (du_u32)b;
}

static int icon_slot(int app_id) {
    if (app_id == AQUA_ICON_LAUNCHPAD) return MAX_APPS;
    if (app_id == AQUA_ICON_MISSION) return MAX_APPS + 1;
    if (app_id == AQUA_ICON_LINUX) return MAX_APPS + 2;
    if (app_id == AQUA_ICON_WINDOWS) return MAX_APPS + 3;
    if (app_id < 0 || app_id >= MAX_APPS) return -1;
    return app_id;
}

static void icon_render(int slot, int app_id) {
    du_u32 *buf = g_icon_master[slot];
    du_u32 *mip = g_icon_mip[slot];
    du_u32 base = icon_base(app_id);
    du_u32 top = shade(base, 36), bot = shade(base, -18);
    int S = ICON_MASTER, r = ICON_MASTER * 58 / 256;
    int row, col, glyph;
    du_context c;
    for (row = 0; row < S; row++) {
        du_u32 g = aqua_mix(top, bot, row * 256 / (S - 1)) & 0x00FFFFFFu;
        for (col = 0; col < S; col++) {
            int cv = aqua_round_cov(col, row, S, S, r);
            du_u32 a = (du_u32)(cv * 255 / 16);
            buf[row * S + col] = (a << 24) | g;
        }
    }
    c.fb = buf;
    c.width = (du_u64)S;
    c.height = (du_u64)S;
    c.pitch = (du_u64)S * 4u;
    if (app_id == AQUA_ICON_LAUNCHPAD) glyph = WF_ICON_APPS;
    else if (app_id == AQUA_ICON_MISSION) glyph = WF_ICON_TASKVIEW;
    else if (app_id == AQUA_ICON_LINUX) glyph = WF_ICON_TERMINAL;
    else if (app_id == AQUA_ICON_WINDOWS) glyph = WF_ICON_APPS;
    else glyph = desktop_app_icon(app_id);
    du_wf_icon(&c, (wf_icon_id)glyph, (S - 48) / 2, (S - 48) / 2, 48, 0xFFFFFFFFu);
    /* 部分图标用 0 色"挖空"内圈：补回渐变 */
    for (row = 0; row < S; row++) {
        du_u32 g = aqua_mix(top, bot, row * 256 / (S - 1)) & 0x00FFFFFFu;
        for (col = 0; col < S; col++) {
            if (buf[row * S + col] == 0 && aqua_round_cov(col, row, S, S, r) == 16)
                buf[row * S + col] = 0xFF000000u | g;
        }
    }
    /* 顶部 1px 高光描边 */
    for (col = r; col < S - r; col++) {
        du_u32 p = buf[S + col];
        buf[S + col] = (p & 0xFF000000u) | (aqua_mix(p, 0xFFFFFFFFu, 60) & 0x00FFFFFFu);
    }
    for (row = 0; row < ICON_MIP; row++) {
        for (col = 0; col < ICON_MIP; col++) {
            du_u32 p[4];
            du_u32 a = 0, rr = 0, gg = 0, bb = 0;
            int k;
            p[0] = buf[(row * 2) * S + col * 2];
            p[1] = buf[(row * 2) * S + col * 2 + 1];
            p[2] = buf[(row * 2 + 1) * S + col * 2];
            p[3] = buf[(row * 2 + 1) * S + col * 2 + 1];
            for (k = 0; k < 4; k++) {
                du_u32 pa = p[k] >> 24;
                a += pa;
                rr += ((p[k] >> 16) & 0xFF) * pa;
                gg += ((p[k] >> 8) & 0xFF) * pa;
                bb += (p[k] & 0xFF) * pa;
            }
            if (a) { rr /= a; gg /= a; bb /= a; }
            else { rr = (base >> 16) & 0xFF; gg = (base >> 8) & 0xFF; bb = base & 0xFF; }
            mip[row * ICON_MIP + col] = ((a / 4) << 24) | (rr << 16) | (gg << 8) | bb;
        }
    }
    g_icon_ready[slot] = 1;
}

#define GLYPH_SLOTS 12
static du_u32 g_glyph_buf[GLYPH_SLOTS][ICON_MIP * ICON_MIP];
static int g_glyph_id[GLYPH_SLOTS];
static du_u32 g_glyph_color[GLYPH_SLOTS];
static int g_glyph_used;

void aqua_glyph(du_context *ctx, int id, int x, int y, int size, du_u32 color) {
    int i, slot = -1;
    if (!ctx || size <= 0) return;
    for (i = 0; i < g_glyph_used; i++)
        if (g_glyph_id[i] == id && g_glyph_color[i] == color) { slot = i; break; }
    if (slot < 0) {
        du_context c;
        du_u32 *buf;
        slot = g_glyph_used < GLYPH_SLOTS ? g_glyph_used++ : (id & 7);
        buf = g_glyph_buf[slot];
        for (i = 0; i < ICON_MIP * ICON_MIP; i++) buf[i] = color & 0x00FFFFFFu;
        c.fb = buf;
        c.width = ICON_MIP;
        c.height = ICON_MIP;
        c.pitch = ICON_MIP * 4u;
        du_wf_icon(&c, (wf_icon_id)id, 0, 0, ICON_MIP, color | 0xFF000000u);
        /* 挖空色 0 → 透明 */
        for (i = 0; i < ICON_MIP * ICON_MIP; i++)
            if (buf[i] == 0) buf[i] = color & 0x00FFFFFFu;
        g_glyph_id[slot] = id;
        g_glyph_color[slot] = color;
    }
    du_blit_scaled_alpha(ctx, g_glyph_buf[slot], ICON_MIP, ICON_MIP, ICON_MIP,
                         x, y, size, size, 256);
}

void aqua_app_icon(du_context *ctx, int app_id, int x, int y, int size,
                   int alpha) {
    int slot = icon_slot(app_id);
    if (!ctx || slot < 0 || size <= 0 || alpha <= 0) return;
    if (!g_icon_ready[slot]) icon_render(slot, app_id);
    if (size < 60)
        du_blit_scaled_alpha(ctx, g_icon_mip[slot], ICON_MIP, ICON_MIP, ICON_MIP,
                             x, y, size, size, alpha);
    else
        du_blit_scaled_alpha(ctx, g_icon_master[slot], ICON_MASTER, ICON_MASTER,
                             ICON_MASTER, x, y, size, size, alpha);
}
