/* ===========================================================================
 *  Nutro+OS - About box and the Paint application
 * ===========================================================================
 */
#include "kernel.h"

extern const boot_info_t *boot_info_get(void);
extern fat32_t *fs_get(void);

/* ============================================================== About ==== */
static void about_draw(window_t *w)
{
    const boot_info_t *bi = boot_info_get();
    int cx = win_content_x(w), cy = win_content_y(w);
    int wd = w->w, ht = w->h - TITLEBAR_H;

    fill_rect(cx, cy, wd, ht, COL_WIN_CONTENT);
    fill_gradient_v(cx, cy, wd, 56, 0xF0F3FA, COL_WIN_CONTENT);

    draw_text_scaled(cx + 18, cy + 12, "Nutro+OS", COL_TEXT, 2);
    draw_text(cx + 20, cy + 44, "version " NUTRO_VERSION "  (x86, 32-bit protected mode)", COL_ACCENT);

    char buf[128];
    int y = cy + 78;
    char vendor[16] = {0};
    cpu_vendor(vendor);

    struct { const char *k; char v[80]; } rows[10];
    int n = 0;

    ksnprintf(rows[n].v, 80, "%s", vendor);                       rows[n++].k = "CPU";
    ksnprintf(rows[n].v, 80, "%u KiB usable", mem_total_kb(bi));  rows[n++].k = "Memory";
    ksnprintf(rows[n].v, 80, "%ux%u x %u bpp, pitch %u", fb_width, fb_height, fb_bpp, fb_pitch);
    rows[n++].k = "Graphics";
    ksnprintf(rows[n].v, 80, "%x (linear framebuffer)", bi ? bi->fb_addr : 0); rows[n++].k = "Framebuffer";
    ksnprintf(rows[n].v, 80, "FAT32 @ LBA %u", bi ? bi->part_lba : 0); rows[n++].k = "Filesystem";
    ksnprintf(rows[n].v, 80, "%s", kernel_build_date());          rows[n++].k = "Kernel built";
    ksnprintf(rows[n].v, 80, "%u KiB of %u KiB heap used", heap_used() / 1024, heap_size() / 1024);
    rows[n++].k = "Heap";
    ksnprintf(rows[n].v, 80, "%u.%03u s", timer_ms() / 1000, timer_ms() % 1000); rows[n++].k = "Uptime";

    for (int i = 0; i < n; i++) {
        draw_text(cx + 18, y, rows[i].k, COL_TEXT_DIM);
        draw_text(cx + 130, y, rows[i].v, COL_TEXT);
        y += 18;
    }
    UNUSED(buf);
}

static int about_event(window_t *w, int type, int a, int b, int c)
{
    UNUSED(w); UNUSED(a); UNUSED(b); UNUSED(c);
    return type == EV_KEY;
}

window_t *app_about_open(void)
{
    return win_create("About Nutro+OS", 320, 160, 470, 330, about_draw, about_event, NULL);
}

/* ============================================================== Paint ==== */
#define PAINT_COLORS 9
static const u32 paint_palette[PAINT_COLORS] = {
    0x1E2430, 0xFFFFFF, 0xFF5F5F, 0xFFB454, 0xFFE066,
    0x54D98C, 0x4FA8FF, 0xB48CFF, 0xFF6FA5
};

typedef struct {
    window_t *win;
    int  w, h;
    u32 *canvas;            /* packed framebuffer values */
    u32  color;
    int  painting;
    int  last_x, last_y;
    int  size;              /* brush radius, driven by the slider */
    /* dirty box of the last stroke, so only that part is repainted */
    int  dirty, dx0, dy0, dx1, dy1;
} paint_t;

static void paint_mark(paint_t *p, int x, int y)
{
    if (!p->dirty) { p->dx0 = x; p->dy0 = y; p->dx1 = x; p->dy1 = y; p->dirty = 1; return; }
    if (x < p->dx0) p->dx0 = x;
    if (y < p->dy0) p->dy0 = y;
    if (x > p->dx1) p->dx1 = x;
    if (y > p->dy1) p->dy1 = y;
}

static paint_t g_paint;

static void paint_clear(paint_t *p)
{
    u32 bg = pack_color(paint_palette[0]);
    for (int i = 0; i < p->w * p->h; i++) p->canvas[i] = bg;
}

static void paint_dot(paint_t *p, int x, int y)
{
    int r = p->size;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            int px = x + dx, py = y + dy;
            if (px < 0 || py < 0 || px >= p->w || py >= p->h) continue;
            p->canvas[py * p->w + px] = pack_color(p->color);
            paint_mark(p, px, py);
        }
    }
}

/* repaint only the touched part of the canvas */
static void paint_flush(window_t *w, paint_t *p)
{
    if (!p->dirty) return;
    int ox = win_content_x(w), oy = win_content_y(w);
    gui_invalidate_rect(ox + p->dx0 - 6, oy + p->dy0 - 6,
                        p->dx1 - p->dx0 + 14, p->dy1 - p->dy0 + 14);
    p->dirty = 0;
}

static void paint_line(paint_t *p, int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx >= 0 ? 1 : -1, sy = dy >= 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        paint_dot(p, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void paint_draw(window_t *w)
{
    paint_t *p = (paint_t *)w->user;
    int cx = win_content_x(w), cy = win_content_y(w);

    fill_rect(cx, cy, w->w, w->h - TITLEBAR_H, COL_WIN_CONTENT);
    blit_u32(cx, cy, p->w, p->h, p->canvas, 0xFFFFFFFF);

    /* palette */
    int py = cy + p->h + 6;
    for (int i = 0; i < PAINT_COLORS; i++) {
        int px = cx + 8 + i * 30;
        fill_round_rect(px, py, 24, 24, 6, paint_palette[i]);
        if (paint_palette[i] == p->color)
            draw_round_rect(px - 2, py - 2, 28, 28, 8, 0xFFFFFF);
    }
    int bx = cx + 8 + PAINT_COLORS * 30 + 12;
    ui_button(bx, py, 70, 24, "Clear", UI_BORDERED);
    ui_slider(bx + 84, py - 2, 120, p->size, 1, 12);
    draw_text(bx + 210, py + 5, "brush", COL_TEXT_DIM);
}

static int paint_event(window_t *w, int type, int a, int b, int c)
{
    paint_t *p = (paint_t *)w->user;
    int cx = win_content_x(w), cy = win_content_y(w);
    int lx = a - cx, ly = b - cy;

    if (type == EV_MOUSE_DOWN) {
        /* palette row? */
        if (ly >= p->h && ly <= p->h + 30) {
            for (int i = 0; i < PAINT_COLORS; i++) {
                int px = 8 + i * 30;
                if (lx >= px && lx < px + 24) {
                    p->color = paint_palette[i];
                    gui_invalidate();
                    return 1;
                }
            }
            int bx = 8 + PAINT_COLORS * 30 + 12;
            if (lx >= bx && lx < bx + 84) { paint_clear(p); gui_invalidate(); return 1; }
            return 1;
        }
        if (lx >= 0 && ly >= 0 && lx < p->w && ly < p->h) {
            p->painting = 1;
            p->last_x = lx;
            p->last_y = ly;
            paint_dot(p, lx, ly);
            paint_flush(w, p);
        }
        return 1;
    }

    if (type == EV_MOUSE_MOVE) {
        if ((gui_mouse_buttons() & 1) && ly >= p->h && ly <= p->h + 34) {
            int sx = 8 + PAINT_COLORS * 30 + 12 + 84;
            if (lx >= sx - 10 && lx <= sx + 130) {
                int v = ui_slider_value(sx, 120, 1, 12, lx);
                if (v != p->size) { p->size = v; gui_invalidate(); }
                return 1;
            }
        }
        if (p->painting) {
            paint_line(p, p->last_x, p->last_y, lx, ly);
            p->last_x = lx;
            p->last_y = ly;
            paint_flush(w, p);
        }
        return 1;
    }

    if (type == EV_MOUSE_UP) {
        p->painting = 0;
        return 1;
    }
    return 0;
}

window_t *app_paint_open(void)
{
    paint_t *p = &g_paint;
    if (p->win && p->win->visible) { win_focus(p->win); return p->win; }

    memset(p, 0, sizeof(*p));
    p->w = 620;
    p->h = 330;
    p->size = 3;
    p->color = paint_palette[6];
    p->canvas = (u32 *)kmalloc((u32)p->w * (u32)p->h * 4);
    if (!p->canvas) return NULL;
    paint_clear(p);

    window_t *w = win_create("Paint", 220, 130, 620, 420, paint_draw, paint_event, p);
    p->win = w;
    return w;
}