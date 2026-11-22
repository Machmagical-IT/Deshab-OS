/* ===========================================================================
 *  Nutro+OS - VESA linear framebuffer graphics library
 *  Supports 32/24/16 bpp direct-colour modes with arbitrary channel masks.
 *  All primitive drawing goes to the current target surface (LFB or back
 *  buffer) so the window manager can compose frames off-screen.
 * ===========================================================================
 */
#include "kernel.h"
#include "font8x16.h"           /* crisp 1bpp 8x16 bitmap font */

u32  fb_width = 0, fb_height = 0, fb_pitch = 0, fb_bpp = 0;
u8  *fb_mem = NULL;
u8  *fb_back = NULL;

static u8  *g_target = NULL;
static u32  g_pitch = 0;            /* stride of the current target surface */
static u32  bytes_pp = 4;

/* clip rectangle honoured by every primitive (used by partial redraws) */
static int c_x0 = 0, c_y0 = 0, c_x1 = 0, c_y1 = 0;
static u32  r_size = 8, r_pos = 16, g_size = 8, g_pos = 8, b_size = 8, b_pos = 0;
static u32  r_mask = 0x00FF0000, g_mask = 0x0000FF00, b_mask = 0x000000FF;

void fb_init(const boot_info_t *bi)
{
    fb_width  = bi->fb_width;
    fb_height = bi->fb_height;
    fb_pitch  = bi->fb_pitch;
    fb_bpp    = bi->fb_bpp;
    fb_mem    = (u8 *)(unsigned long)bi->fb_addr;
    g_target  = fb_mem;
    bytes_pp  = fb_bpp >> 3;
    if (bytes_pp < 2 || bytes_pp > 4) bytes_pp = 4;
    fb_reset_clip();

    if (bi->red_size) {
        r_size = bi->red_size; r_pos = bi->red_pos;
        g_size = bi->green_size; g_pos = bi->green_pos;
        b_size = bi->blue_size; b_pos = bi->blue_pos;
    } else if (fb_bpp == 16) {
        r_size = 5; r_pos = 11; g_size = 6; g_pos = 5; b_size = 5; b_pos = 0;
    } else {
        r_size = 8; r_pos = 16; g_size = 8; g_pos = 8; b_size = 8; b_pos = 0;
    }
    r_mask = ((0xFFu >> (8 - r_size)) << r_pos);
    g_mask = ((0xFFu >> (8 - g_size)) << g_pos);
    b_mask = ((0xFFu >> (8 - b_size)) << b_pos);

    /* use the 256 glyph BIOS font when the loader could grab it */
    UNUSED(bi);
    debug_write("fb: 1bpp text rendering (crisp)\n");
}

static inline const u8 *glyph_1bpp(char c)
{
    int cc = (u8)c;
    if (cc < 32 || cc > 126) return NULL;
    return font8x16[cc - 32];
}

void fb_set_target(u8 *target)
{
    g_target = target ? target : fb_mem;
    g_pitch = fb_pitch;
}

/* switch to an arbitrary surface (e.g. a window's content cache) */
void fb_set_target2(u8 *target, u32 pitch)
{
    g_target = target;
    g_pitch = pitch;
}

u8  *fb_get_target(void) { return g_target; }
u32  fb_get_pitch(void) { return g_pitch; }

/* stride aware raw blit, used for window caches */
void fb_blit_buffer(u8 *dst, u32 dst_pitch, int dx, int dy,
                    const u8 *src, u32 src_pitch, int w, int h)
{
    if (!src || !dst || w <= 0 || h <= 0) return;
    int x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (dx < 0) x0 = -dx;
    if (dy < 0) y0 = -dy;
    if (dx + w > (int)fb_width) x1 = (int)fb_width - dx;
    if (dy + h > (int)fb_height) y1 = (int)fb_height - dy;
    /* honour the active clip rectangle as well */
    if (dx + x0 < c_x0) x0 = c_x0 - dx;
    if (dy + y0 < c_y0) y0 = c_y0 - dy;
    if (dx + x1 > c_x1) x1 = c_x1 - dx;
    if (dy + y1 > c_y1) y1 = c_y1 - dy;
    if (x1 <= x0 || y1 <= y0) return;
    u32 rowbytes = (u32)(x1 - x0) * bytes_pp;
    for (int y = y0; y < y1; y++)
        memcpy(dst + (u32)(dy + y) * dst_pitch + (u32)(dx + x0) * bytes_pp,
               src + (u32)y * src_pitch + (u32)x0 * bytes_pp, rowbytes);
}

/* nearest neighbour scaled blit - used by the window transition animations */
void fb_blit_scaled(u8 *dst, u32 dst_pitch, int dx, int dy, int dw, int dh,
                    const u8 *src, u32 src_pitch, int sw, int sh)
{
    if (!dst || !src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    u32 xstep = ((u32)sw << 16) / (u32)dw;
    u32 ystep = ((u32)sh << 16) / (u32)dh;
    u32 sy = 0;
    for (int y = 0; y < dh; y++) {
        int py = dy + y;
        if (py < c_y0 || py >= c_y1) { sy += ystep; continue; }
        const u8 *srow = src + (u32)(sy >> 16) * src_pitch;
        u32 sx = 0;
        for (int x = 0; x < dw; x++) {
            int px = dx + x;
            if (px >= c_x0 && px < c_x1) {
                const u8 *sp = srow + (u32)(sx >> 16) * bytes_pp;
                u8 *dp = dst + (u32)py * dst_pitch + (u32)px * bytes_pp;
                for (u32 b = 0; b < bytes_pp; b++) dp[b] = sp[b];
            }
            sx += xstep;
        }
        sy += ystep;
    }
}

void fb_reset_clip(void)
{
    c_x0 = 0; c_y0 = 0;
    c_x1 = (int)fb_width; c_y1 = (int)fb_height;
}

void fb_set_clip(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) { c_x0 = c_y0 = 0; c_x1 = c_y1 = 0; return; }
    c_x0 = x < 0 ? 0 : x;
    c_y0 = y < 0 ? 0 : y;
    c_x1 = x + w;
    c_y1 = y + h;
    if (c_x1 > (int)fb_width) c_x1 = (int)fb_width;
    if (c_y1 > (int)fb_height) c_y1 = (int)fb_height;
}

u32 pack_color(u32 rgb)
{
    u32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    u32 v = 0;
    if (r_size) v |= (r >> (8 - r_size)) << r_pos;
    if (g_size) v |= (g >> (8 - g_size)) << g_pos;
    if (b_size) v |= (b >> (8 - b_size)) << b_pos;
    return v;
}

void fb_present(void)
{
    if (fb_back)
        memcpy(fb_mem, fb_back, fb_pitch * fb_height);
}

u32 fb_get_pixel(int x, int y)
{
    if (x < 0 || y < 0 || x >= (int)fb_width || y >= (int)fb_height) return 0;
    u8 *p = g_target + (u32)y * g_pitch + (u32)x * bytes_pp;
    if (fb_bpp == 32) return *(u32 *)p;
    if (fb_bpp == 24) return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
    return *(u16 *)p;
}

void put_pixel(int x, int y, u32 rgb)
{
    if (x < c_x0 || y < c_y0 || x >= c_x1 || y >= c_y1) return;
    u8 *p = g_target + (u32)y * g_pitch + (u32)x * bytes_pp;
    u32 c = pack_color(rgb);
    if (fb_bpp == 32) {
        *(u32 *)p = c;
    } else if (fb_bpp == 24) {
        p[0] = (u8)c; p[1] = (u8)(c >> 8); p[2] = (u8)(c >> 16);
    } else {
        *(u16 *)p = (u16)c;
    }
}

void fill_rect(int x, int y, int w, int h, u32 rgb)
{
    if (w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1;
    if (x0 < c_x0) x0 = c_x0;
    if (y0 < c_y0) y0 = c_y0;
    if (x1 > c_x1 - 1) x1 = c_x1 - 1;
    if (y1 > c_y1 - 1) y1 = c_y1 - 1;
    if (x0 > x1 || y0 > y1) return;
    u32 c = pack_color(rgb);
    u32 n = (u32)(x1 - x0 + 1);
    for (int yy = y0; yy <= y1; yy++) {
        u8 *p = g_target + (u32)yy * g_pitch + (u32)x0 * bytes_pp;
        if (fb_bpp == 32) {
            u32 *p32 = (u32 *)p;
            for (u32 i = 0; i < n; i++) p32[i] = c;
        } else if (fb_bpp == 24) {
            for (u32 i = 0; i < n; i++) { p[0] = (u8)c; p[1] = (u8)(c >> 8); p[2] = (u8)(c >> 16); p += 3; }
        } else {
            u16 *p16 = (u16 *)p;
            for (u32 i = 0; i < n; i++) p16[i] = (u16)c;
        }
    }
}

void fill_hline(int x, int y, int w, u32 rgb) { fill_rect(x, y, w, 1, rgb); }
void fill_vline(int x, int y, int h, u32 rgb) { fill_rect(x, y, 1, h, rgb); }

void fill_rect_blend(int x, int y, int w, int h, u32 rgb, u32 alpha)
{
    if (alpha == 0 || w <= 0 || h <= 0) return;
    if (alpha >= 255) { fill_rect(x, y, w, h, rgb); return; }
    int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1;
    if (x0 < c_x0) x0 = c_x0;
    if (y0 < c_y0) y0 = c_y0;
    if (x1 > c_x1 - 1) x1 = c_x1 - 1;
    if (y1 > c_y1 - 1) y1 = c_y1 - 1;
    if (x0 > x1 || y0 > y1) return;

    u32 c = pack_color(rgb);
    u32 sa = alpha, da = 255 - alpha;
    u32 sr = (c & r_mask) >> r_pos, sg = (c & g_mask) >> g_pos, sb = (c & b_mask) >> b_pos;

    for (int yy = y0; yy <= y1; yy++) {
        u8 *p = g_target + (u32)yy * g_pitch + (u32)x0 * bytes_pp;
        for (int xx = x0; xx <= x1; xx++) {
            u32 pc;
            if (fb_bpp == 32) pc = *(u32 *)p;
            else if (fb_bpp == 24) pc = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
            else pc = *(u16 *)p;
            u32 pr = (pc & r_mask) >> r_pos, pg = (pc & g_mask) >> g_pos, pb = (pc & b_mask) >> b_pos;
            u32 nr = (pr * da + sr * sa) / 255;
            u32 ng = (pg * da + sg * sa) / 255;
            u32 nb = (pb * da + sb * sa) / 255;
            u32 out = ((nr << r_pos) & r_mask) | ((ng << g_pos) & g_mask) | ((nb << b_pos) & b_mask);
            if (fb_bpp == 32) *(u32 *)p = out;
            else if (fb_bpp == 24) { p[0] = (u8)out; p[1] = (u8)(out >> 8); p[2] = (u8)(out >> 16); }
            else *(u16 *)p = (u16)out;
            p += bytes_pp;
        }
    }
}

void draw_rect(int x, int y, int w, int h, u32 rgb)
{
    fill_hline(x, y, w, rgb);
    fill_hline(x, y + h - 1, w, rgb);
    fill_vline(x, y, h, rgb);
    fill_vline(x + w - 1, y, h, rgb);
}

static int isqrt_u32(u32 v)
{
    u32 r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        u32 t = r + b;
        r >>= 1;
        if (t <= v) { v -= t; r += b; }
        b >>= 2;
    }
    return (int)r;
}

/* same shape as fill_round_rect but alpha blended, for soft shadows */
void fill_round_rect_blend(int x, int y, int w, int h, int radius, u32 rgb, int alpha)
{
    if (radius <= 0) { fill_rect_blend(x, y, w, h, rgb, alpha); return; }
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    int r2 = radius * radius;
    for (int row = 0; row < h; row++) {
        int inset = 0, dy = -1;
        if (row < radius) dy = radius - 1 - row;
        else if (row >= h - radius) dy = row - (h - radius);
        if (dy >= 0) {
            int dx = isqrt_u32((u32)(r2 - dy * dy));
            inset = radius - dx;
        }
        int core = w - 2 * inset;
        if (core <= 0) continue;
        fill_rect_blend(x + inset, y + row, core, 1, rgb, alpha);
    }
}

void fill_round_rect(int x, int y, int w, int h, int radius, u32 rgb)
{
    if (radius <= 0 || radius * 2 > h || radius * 2 > w) { fill_rect(x, y, w, h, rgb); return; }
    int r2 = radius * radius;
    for (int row = 0; row < h; row++) {
        int inset = 0;
        u32 cov = 255;
        /* circular corner: inset plus the anti-aliased edge coverage */
        int dy = -1;
        if (row < radius) dy = radius - 1 - row;
        else if (row >= h - radius) dy = row - (h - radius);
        if (dy >= 0) {
            u32 v = (u32)(r2 - dy * dy);
            int dx = isqrt_u32(v);
            inset = radius - dx;
            u32 rem = v - (u32)(dx * dx);
            cov = 255 - (255u * rem / (u32)(2 * dx + 1));
            if (cov > 255) cov = 255;
        }
        int core_w = w - 2 * inset - 2;
        if (core_w > 0) fill_rect(x + inset + 1, y + row, core_w, 1, rgb);
        else { fill_rect(x + inset, y + row, w - 2 * inset, 1, rgb); continue; }
        fill_rect_blend(x + inset, y + row, 1, 1, rgb, cov);
        fill_rect_blend(x + w - 1 - inset, y + row, 1, 1, rgb, cov);
    }
}

void draw_round_rect(int x, int y, int w, int h, int radius, u32 rgb)
{
    /* simply overwrite corners with background-free outline: draw 4 lines */
    fill_hline(x + radius, y, w - 2 * radius, rgb);
    fill_hline(x + radius, y + h - 1, w - 2 * radius, rgb);
    fill_vline(x, y + radius, h - 2 * radius, rgb);
    fill_vline(x + w - 1, y + radius, h - 2 * radius, rgb);
    int r2 = radius * radius;
    for (int i = 0; i < radius; i++) {
        int dx = isqrt_u32((u32)(r2 - (radius - 1 - i) * (radius - 1 - i)));
        /* four corner arcs */
        put_pixel(x + radius - dx, y + i, rgb);                 /* top left */
        put_pixel(x + w - 1 - radius + dx, y + i, rgb);         /* top right */
        put_pixel(x + radius - dx, y + h - 1 - i, rgb);         /* bottom left */
        put_pixel(x + w - 1 - radius + dx, y + h - 1 - i, rgb); /* bottom right */
    }
}

void fill_gradient_v(int x, int y, int w, int h, u32 top, u32 bottom)
{
    if (h <= 0) return;
    int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    int br = (bottom >> 16) & 0xFF, bg = (bottom >> 8) & 0xFF, bb = bottom & 0xFF;
    for (int i = 0; i < h; i++) {
        int r = tr + (br - tr) * i / h;
        int g = tg + (bg - tg) * i / h;
        int b = tb + (bb - tb) * i / h;
        u32 c = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        fill_rect(x, y + i, w, 1, c);
    }
}

void fill_circle(int cx, int cy, int r, u32 rgb)
{
    if (r <= 0) return;
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        /* exact half width in 8.8 fixed point -> true edge coverage */
        int hw = isqrt_u32((u32)((r2 - dy * dy) << 8));
        int L = (cx << 8) - hw, R = (cx << 8) + hw;
        int px0 = L >> 8, px1 = R >> 8;
        int covL = 255 - (L & 0xFF), covR = R & 0xFF;
        if (px0 == px1) {
            int cov = covL + covR - 255;
            if (cov < 0) cov = 0;
            if (cov > 255) cov = 255;
            fill_rect_blend(px0, cy + dy, 1, 1, rgb, cov);
        } else {
            fill_rect_blend(px0, cy + dy, 1, 1, rgb, covL);
            if (px1 > px0 + 1) fill_rect(px0 + 1, cy + dy, px1 - px0 - 1, 1, rgb);
            fill_rect_blend(px1, cy + dy, 1, 1, rgb, covR);
        }
    }
}

void draw_circle(int cx, int cy, int r, u32 rgb, int thickness)
{
    if (r <= 0) return;
    if (thickness < 1) thickness = 1;
    int ri = r - thickness;
    if (ri < 0) ri = 0;
    int ro2 = r * r, ri2 = ri * ri;
    for (int dy = -r; dy <= r; dy++) {
        int xo = isqrt_u32((u32)(ro2 - dy * dy));
        int xi = (dy >= -ri && dy <= ri) ? isqrt_u32((u32)(ri2 - dy * dy)) : -1;
        if (xi < 0) {
            fill_rect(cx - xo, cy + dy, xo * 2 + 1, 1, rgb);
        } else {
            fill_rect(cx - xo, cy + dy, xo - xi, 1, rgb);
            fill_rect(cx + xi + 1, cy + dy, xo - xi, 1, rgb);
        }
    }
}

/* fixed point trigonometry (result * 1000) - Bhaskara I approximation */
int fsin_deg(int deg)
{
    deg = deg % 360;
    if (deg < 0) deg += 360;
    int s = 1, x = deg;
    if (deg > 180) { s = -1; x = deg - 180; }
    int t = x * (180 - x);
    return s * (4 * t * 1000) / (40500 - t);
}

int fcos_deg(int deg) { return fsin_deg(deg + 90); }

void draw_line(int x0, int y0, int x1, int y1, u32 rgb, int thickness)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx >= 0 ? 1 : -1;
    int sy = dy >= 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    if (thickness < 1) thickness = 1;
    int off = thickness / 2;
    int steep = dy > dx;
    int err_frac = 0;
    for (;;) {
        if (thickness <= 1) {
            /* Wu style: the error term gives a continuous coverage */
            int cov = (err_frac >> 8) & 0xFF;
            if (steep) {
                fill_rect_blend(x0 - 1, y0, 1, 1, rgb, 255 - cov);
                put_pixel(x0, y0, rgb);
                fill_rect_blend(x0 + 1, y0, 1, 1, rgb, cov);
            } else {
                fill_rect_blend(x0, y0 - 1, 1, 1, rgb, 255 - cov);
                put_pixel(x0, y0, rgb);
                fill_rect_blend(x0, y0 + 1, 1, 1, rgb, cov);
            }
        }
        else fill_rect(x0 - off, y0 - off, thickness, thickness, rgb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        err_frac = (2 * (err > 0 ? err : -err)) * 255 / ((steep ? dx : dy) ? (steep ? dx : dy) : 1);
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

/* ---------------------------------------------------------------- text --- */
void draw_char(int x, int y, char c, u32 rgb)
{
    const u8 *g = glyph_1bpp(c);
    if (!g) return;
    u32 col = pack_color(rgb);
    for (int row = 0; row < 16; row++) {
        u8 bits = g[row];
        if (!bits) continue;
        int py = y + row;
        if (py < c_y0 || py >= c_y1) continue;
        u8 *p = g_target + (u32)py * g_pitch;
        for (int col_i = 0; col_i < 8; col_i++) {
            if (!(bits & (0x80 >> col_i))) continue;
            int px = x + col_i;
            if (px < c_x0 || px >= c_x1) continue;
            u8 *q = p + (u32)px * bytes_pp;
            if (fb_bpp == 32) *(u32 *)q = col;
            else if (fb_bpp == 24) { q[0] = (u8)col; q[1] = (u8)(col >> 8); q[2] = (u8)(col >> 16); }
            else *(u16 *)q = (u16)col;
        }
    }
}

void draw_char_bold(int x, int y, char c, u32 rgb)
{
    draw_char(x, y, c, rgb);            /* crisp font has no separate bold face */
}

void draw_text(int x, int y, const char *s, u32 rgb)
{
    while (*s) {
        if (*s == '\n') break;
        draw_char(x, y, *s, rgb);
        x += 8;
        s++;
    }
}

void draw_char_scaled(int x, int y, char c, u32 rgb, int scale)
{
    const u8 *g = glyph_1bpp(c);
    if (!g || scale <= 0) return;
    for (int row = 0; row < 16; row++) {
        u8 bits = g[row];
        if (!bits) continue;
        for (int col_i = 0; col_i < 8; col_i++)
            if (bits & (0x80 >> col_i))
                fill_rect(x + col_i * scale, y + row * scale, scale, scale, rgb);
    }
}

void draw_text_scaled(int x, int y, const char *s, u32 rgb, int scale)
{
    while (*s) {
        draw_char_scaled(x, y, *s, rgb, scale);
        x += 8 * scale;
        s++;
    }
}

void draw_text_bold(int x, int y, const char *s, u32 rgb)
{
    while (*s) {
        if (*s == '\n') break;
        draw_char_bold(x, y, *s, rgb);
        x += 8;
        s++;
    }
}

int text_width(const char *s) { return (int)strlen(s) * 8; }

/* ---------------------------------------------------------------- blit --- */
void blit_u32_clip(int dx, int dy, int w, int h, const u32 *src, u32 key,
                   int cx, int cy, int cw, int ch)
{
    for (int y = 0; y < h; y++) {
        int py = dy + y;
        if (py < cy || py >= cy + ch) continue;
        if (py < c_y0 || py >= c_y1) continue;
        for (int x = 0; x < w; x++) {
            int px = dx + x;
            if (px < cx || px >= cx + cw) continue;
            if (px < c_x0 || px >= c_x1) continue;
            u32 v = src[(u32)y * (u32)w + (u32)x];
            if (key != 0xFFFFFFFFu && v == key) continue;
            u8 *p = g_target + (u32)py * g_pitch + (u32)px * bytes_pp;
            if (fb_bpp == 32) *(u32 *)p = v;
            else if (fb_bpp == 24) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); }
            else *(u16 *)p = (u16)v;
        }
    }
}

void blit_u32(int dx, int dy, int w, int h, const u32 *src, u32 key)
{
    blit_u32_clip(dx, dy, w, h, src, key, 0, 0, (int)fb_width, (int)fb_height);
}

void fb_copy_rect(u8 *dst, int dx, int dy, int w, int h, const u8 *src, int sx, int sy)
{
    if (w <= 0 || h <= 0) return;
    int x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (dx < 0) x0 = -dx;
    if (dy < 0) y0 = -dy;
    if (dx + w > (int)fb_width) x1 = (int)fb_width - dx;
    if (dy + h > (int)fb_height) y1 = (int)fb_height - dy;
    u32 rowbytes = (u32)(x1 - x0) * bytes_pp;
    if ((int)rowbytes <= 0) return;
    for (int y = y0; y < y1; y++) {
        memcpy(dst + (u32)(dy + y) * fb_pitch + (u32)(dx + x0) * bytes_pp,
               src + (u32)(sy + y) * fb_pitch + (u32)(sx + x0) * bytes_pp,
               rowbytes);
    }
}

void fb_fill_screen(u32 rgb)
{
    fill_rect(0, 0, (int)fb_width, (int)fb_height, rgb);
}