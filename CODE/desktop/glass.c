/* glass.c — 壁纸可分离盒式模糊 + 圆角采样。 */
#include "desktop.h"
#include "wallpaper.h"
#include "glass.h"

static du_u32 *g_glass = 0;
static int g_glass_w = 0;
static int g_glass_h = 0;

int glass_ready(void) { return g_glass != 0; }

const du_u32 *glass_pixels(int *w, int *h) {
    if (w) *w = g_glass_w;
    if (h) *h = g_glass_h;
    return g_glass;
}

static du_u32 pack_rgb(int r, int g, int b) {
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xFF000000u | ((du_u32)r << 16) | ((du_u32)g << 8) | (du_u32)b;
}

/* 边缘夹取。滑窗：发出 x 后去掉 clamp(x-r)，加上 clamp(x+r+1)。 */
static void box_blur_h(const du_u32 *src, du_u32 *dst, int w, int h, int radius) {
    int win = radius * 2 + 1;
    int y, x, k;
    for (y = 0; y < h; y++) {
        const du_u32 *srow = src + (du_i64)y * w;
        du_u32 *drow = dst + (du_i64)y * w;
        int sr = 0, sg = 0, sb = 0;
        for (k = -radius; k <= radius; k++) {
            int sx = k;
            du_u32 p;
            if (sx < 0) sx = 0;
            if (sx >= w) sx = w - 1;
            p = srow[sx];
            sb += (int)(p & 0xFF);
            sg += (int)((p >> 8) & 0xFF);
            sr += (int)((p >> 16) & 0xFF);
        }
        for (x = 0; x < w; x++) {
            int x0, x1;
            du_u32 rm, ad;
            drow[x] = pack_rgb(sr / win, sg / win, sb / win);
            x0 = x - radius;
            x1 = x + radius + 1;
            if (x0 < 0) x0 = 0;
            if (x1 >= w) x1 = w - 1;
            rm = srow[x0];
            ad = srow[x1];
            sb += (int)(ad & 0xFF) - (int)(rm & 0xFF);
            sg += (int)((ad >> 8) & 0xFF) - (int)((rm >> 8) & 0xFF);
            sr += (int)((ad >> 16) & 0xFF) - (int)((rm >> 16) & 0xFF);
        }
    }
}

static void box_blur_v(const du_u32 *src, du_u32 *dst, int w, int h, int radius) {
    int win = radius * 2 + 1;
    int x, y, k;
    for (x = 0; x < w; x++) {
        int sr = 0, sg = 0, sb = 0;
        for (k = -radius; k <= radius; k++) {
            int sy = k;
            du_u32 p;
            if (sy < 0) sy = 0;
            if (sy >= h) sy = h - 1;
            p = src[(du_i64)sy * w + x];
            sb += (int)(p & 0xFF);
            sg += (int)((p >> 8) & 0xFF);
            sr += (int)((p >> 16) & 0xFF);
        }
        for (y = 0; y < h; y++) {
            int y0, y1;
            du_u32 rm, ad;
            dst[(du_i64)y * w + x] = pack_rgb(sr / win, sg / win, sb / win);
            y0 = y - radius;
            y1 = y + radius + 1;
            if (y0 < 0) y0 = 0;
            if (y1 >= h) y1 = h - 1;
            rm = src[(du_i64)y0 * w + x];
            ad = src[(du_i64)y1 * w + x];
            sb += (int)(ad & 0xFF) - (int)(rm & 0xFF);
            sg += (int)((ad >> 8) & 0xFF) - (int)((rm >> 8) & 0xFF);
            sr += (int)((ad >> 16) & 0xFF) - (int)((rm >> 16) & 0xFF);
        }
    }
}

int glass_init(int fb_w, int fb_h) {
    du_u32 *src;
    du_u64 bytes;
    du_u32 *tmp;
    du_u32 *dst;
    if (fb_w <= 0 || fb_h <= 0) return -1;
    src = (du_u32 *)wallpaper_layer();
    if (!src) return -1;
    bytes = (du_u64)fb_w * (du_u64)fb_h * 4u;
    if (bytes > GLASS_MAX || bytes > WALLPAPER_STAGING_MAX) return -2;
    tmp = (du_u32 *)WALLPAPER_STAGING_ADDR;
    dst = (du_u32 *)GLASS_ADDR;
    box_blur_h(src, tmp, fb_w, fb_h, GLASS_RADIUS);
    box_blur_v(tmp, dst, fb_w, fb_h, GLASS_RADIUS);
    g_glass = dst;
    g_glass_w = fb_w;
    g_glass_h = fb_h;
    return 0;
}

static int glass_corner_out(int col, int row, int w, int h, int r) {
    int dx = 0, dy = 0, corner = 0;
    if (r <= 0) return 0;
    if (col < r && row < r) {
        dx = r - 1 - col; dy = r - 1 - row; corner = 1;
    } else if (col >= w - r && row < r) {
        dx = col - (w - r); dy = r - 1 - row; corner = 1;
    } else if (col < r && row >= h - r) {
        dx = r - 1 - col; dy = row - (h - r); corner = 1;
    } else if (col >= w - r && row >= h - r) {
        dx = col - (w - r); dy = row - (h - r); corner = 1;
    }
    return corner && dx * dx + dy * dy > r * r;
}

void du_glass_wallpaper(du_context *ctx, int x, int y, int w, int h,
                        int radius, du_u32 tint, du_u32 alpha) {
    int row, col;
    int r;
    if (!ctx || !g_glass || w <= 0 || h <= 0) return;
    if (alpha > 255) alpha = 255;
    r = radius;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (row = 0; row < h; row++) {
        int sy = y + row;
        for (col = 0; col < w; col++) {
            int sx;
            du_u32 sample;
            if (glass_corner_out(col, row, w, h, r)) continue;
            sx = x + col;
            if (sx < 0 || sy < 0 || sx >= g_glass_w || sy >= g_glass_h)
                sample = 0xFF000000u;
            else
                sample = g_glass[(du_i64)sy * g_glass_w + sx];
            du_pixel(ctx, sx, sy, du_blend(sample, tint, alpha));
        }
    }
}
