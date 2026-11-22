/* blit.c — 16.16 以外的 8.8 双线性缩放，混合方式对齐 textures.c。 */
#include "blit.h"

static du_u32 ch_at(du_u32 p, int shift) {
    return (p >> shift) & 0xFF;
}

static du_u32 bilerp(du_u32 p00, du_u32 p10, du_u32 p01, du_u32 p11,
                     int fx, int fy, int shift) {
    du_u32 top = (ch_at(p00, shift) * (256 - fx) + ch_at(p10, shift) * fx) >> 8;
    du_u32 bot = (ch_at(p01, shift) * (256 - fx) + ch_at(p11, shift) * fx) >> 8;
    return (top * (256 - fy) + bot * fy) >> 8;
}

void du_blit_scaled_alpha(du_context *ctx,
                          const du_u32 *src, int src_stride,
                          int sw, int sh,
                          int x, int y, int w, int h,
                          int alpha) {
    int dy;
    if (!ctx || !src || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
    if (src_stride < sw) return;
    if (alpha < 0) alpha = 0;
    if (alpha > 256) alpha = 256;

    for (dy = 0; dy < h; dy++) {
        int py = y + dy;
        int sy0, sy1, fy, dx;
        du_i64 fy64;
        if (py < 0 || (du_u64)py >= ctx->height) continue;
        fy64 = ((du_i64)dy * sh << 8) / h;
        sy0 = (int)(fy64 >> 8);
        fy = (int)(fy64 & 0xFF);
        sy1 = (sy0 + 1 < sh) ? sy0 + 1 : sy0;
        if (sy0 >= sh) { sy0 = sh - 1; sy1 = sy0; }
        for (dx = 0; dx < w; dx++) {
            int px = x + dx;
            int sx0, sx1, fx;
            du_i64 fx64;
            du_u32 p00, p10, p01, p11;
            du_u32 r, g, b, a, dst;
            if (px < 0 || (du_u64)px >= ctx->width) continue;
            fx64 = ((du_i64)dx * sw << 8) / w;
            sx0 = (int)(fx64 >> 8);
            fx = (int)(fx64 & 0xFF);
            sx1 = (sx0 + 1 < sw) ? sx0 + 1 : sx0;
            if (sx0 >= sw) { sx0 = sw - 1; sx1 = sx0; }
            p00 = src[(du_i64)sy0 * src_stride + sx0];
            p10 = src[(du_i64)sy0 * src_stride + sx1];
            p01 = src[(du_i64)sy1 * src_stride + sx0];
            p11 = src[(du_i64)sy1 * src_stride + sx1];
            r = bilerp(p00, p10, p01, p11, fx, fy, 16);
            g = bilerp(p00, p10, p01, p11, fx, fy, 8);
            b = bilerp(p00, p10, p01, p11, fx, fy, 0);
            a = bilerp(p00, p10, p01, p11, fx, fy, 24);
            a = a * (du_u32)alpha >> 8;
            if (a == 0) continue;
            if (a >= 255) {
                du_pixel(ctx, px, py, 0xFF000000u | (r << 16) | (g << 8) | b);
                continue;
            }
            dst = du_pixel_read(ctx, px, py);
            {
                du_u32 dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
                du_u32 na = 255 - a;
                du_pixel(ctx, px, py, 0xFF000000u |
                         (((r * a + dr * na) >> 8) << 16) |
                         (((g * a + dg * na) >> 8) << 8) |
                         ((b * a + db * na) >> 8));
            }
        }
    }
}
