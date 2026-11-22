/* ===========================================================================
 *  Nutro+OS - SwiftUI style control kit
 *
 *  Widgets follow Apple's look: 8 px corner radii, the system accent colour,
 *  tinted backgrounds, hairline borders, soft shadows and hover states.
 *  Drawing and hit testing are separate so the existing event model of the
 *  apps stays unchanged:
 *      draw:  ui_button(...)              (called from the window draw)
 *      input: if (ui_hit(...)) ...        (called from the window event)
 * ===========================================================================
 */
#include "kernel.h"

int gui_mouse_buttons(void);

/* --------------------------------------------------------------- helpers -- */
int ui_hit(int x, int y, int w, int h)
{
    return gui_mouse_over(x, y, w, h);
}

/* a circle with a soft 1 px halo - cheap anti-aliasing for round controls */
static void soft_circle(int cx, int cy, int r, u32 rgb)
{
    fill_circle(cx, cy, r, rgb);
    draw_circle(cx, cy, r + 1, rgb, 1);       /* the ring blends with the bg */
}

static u32 lighten(u32 c, u32 pct)
{
    u32 r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r += (255 - r) * pct / 100;
    g += (255 - g) * pct / 100;
    b += (255 - b) * pct / 100;
    return (r << 16) | (g << 8) | b;
}

static void label_center(int x, int y, int w, int h, const char *s, u32 col, int bold)
{
    int tw = text_width(s);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - 16) / 2;
    if (bold) draw_text_bold(tx, ty, s, col);
    else draw_text(tx, ty, s, col);
}

/* --------------------------------------------------------------- button --- */
void ui_button(int x, int y, int w, int h, const char *label, int style)
{
    int hover = gui_mouse_over(x, y, w, h);
    int pressed = hover && (gui_mouse_buttons() & 1);

    switch (style) {
    case UI_FILLED: {
        u32 base = pressed ? 0x0868CC : (hover ? lighten(COL_ACCENT, 12) : COL_ACCENT);
        fill_round_rect(x, y, w, h, 8, base);
        fill_rect_blend(x + 1, y + 1, w - 2, h / 2, 0xFFFFFF, 28);
        label_center(x, y, w, h, label, 0xFFFFFF, 1);
        break;
    }
    case UI_TINTED: {
        fill_round_rect(x, y, w, h, 8, COL_ACCENT);
        fill_rect_blend(x, y, w, h, 0xFFFFFF, pressed ? 55 : (hover ? 78 : 86));
        label_center(x, y, w, h, label, COL_ACCENT, 1);
        break;
    }
    case UI_PLAIN: {
        if (hover) fill_round_rect(x, y, w, h, 8, 0x000000);
        if (hover) fill_rect_blend(x, y, w, h, 0xFFFFFF, 94);
        label_center(x, y, w, h, label, COL_ACCENT, 0);
        break;
    }
    case UI_DESTRUCT: {
        fill_round_rect(x, y, w, h, 8, COL_CLOSE);
        fill_rect_blend(x, y, w, h, 0xFFFFFF, pressed ? 55 : (hover ? 78 : 86));
        label_center(x, y, w, h, label, 0xC0261C, 1);
        break;
    }
    default: {                                    /* UI_BORDERED */
        u32 bg = pressed ? 0xE2E4EA : (hover ? 0xF2F4F8 : 0xFFFFFF);
        fill_round_rect(x, y, w, h, 8, bg);
        draw_round_rect(x, y, w, h, 8, 0xD2D6DE);
        label_center(x, y, w, h, label, COL_TEXT, 0);
        break;
    }
    }
}

/* --------------------------------------------------------------- toggle --- */
static void toggle_knob(int cx, int cy, int r)
{
    fill_circle(cx, cy, r + 1, 0xD8DCE4);
    fill_circle(cx, cy, r, 0xFFFFFF);
}

void ui_toggle(int x, int y, int on)
{
    int w = 42, h = 24;
    u32 track = on ? COL_ACCENT : 0xD4D8E0;
    if (gui_mouse_over(x - 2, y - 2, w + 4, h + 4)) track = lighten(track, 10);
    fill_round_rect(x, y, w, h, h / 2, track);
    if (on) toggle_knob(x + w - 12, y + h / 2, 9);
    else    toggle_knob(x + 12, y + h / 2, 9);
}

/* ------------------------------------------------------------- checkbox --- */
void ui_checkbox(int x, int y, const char *label, int checked)
{
    int s = 20;
    if (checked) {
        fill_round_rect(x, y, s, s, 6, COL_ACCENT);
        draw_line(x + 5, y + 10, x + 8, y + 14, 0xFFFFFF, 2);
        draw_line(x + 8, y + 14, x + 15, y + 6, 0xFFFFFF, 2);
    } else {
        fill_round_rect(x, y, s, s, 6, 0xFFFFFF);
        draw_round_rect(x, y, s, s, 6, 0xC6CAD3);
    }
    if (label) draw_text(x + s + 8, y + 2, label, COL_TEXT);
}

/* --------------------------------------------------------------- slider --- */
void ui_slider(int x, int y, int w, int value, int mn, int mx)
{
    int ty = y + 12;
    fill_round_rect(x, ty, w, 4, 2, 0xD0D4DC);
    int v = value;
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    int fill_w = (mx > mn) ? w * (v - mn) / (mx - mn) : 0;
    if (fill_w > 2) fill_round_rect(x, ty, fill_w, 4, 2, COL_ACCENT);

    int kx = x + fill_w;
    fill_circle(kx + 1, ty + 4, 10, 0xD8DCE4);
    fill_circle(kx, ty + 2, 9, 0xFFFFFF);
    draw_circle(kx, ty + 2, 9, 0xC6CAD3, 1);
}

/* map a mouse x position onto the slider value */
int ui_slider_value(int x, int w, int mn, int mx, int mouse_x)
{
    if (w <= 0) return mn;
    int rel = mouse_x - x;
    if (rel < 0) rel = 0;
    if (rel > w) rel = w;
    return mn + (mx - mn) * rel / w;
}

/* -------------------------------------------------------------- segment --- */
void ui_segment(int x, int y, int w, int h, const char *const *items, int n, int sel)
{
    fill_round_rect(x, y, w, h, 8, 0xE8EAF0);
    int iw = n ? w / n : w;
    for (int i = 0; i < n; i++) {
        int ix = x + i * iw;
        if (i == sel) {
            fill_round_rect(ix + 2, y + 2, iw - 4, h - 4, 6, 0xFFFFFF);
            fill_round_rect(ix + 2, y + h - 6, iw - 4, 4, 2, 0xDDE0E7);
            draw_round_rect(ix + 2, y + 2, iw - 4, h - 4, 6, 0xD8DBE3);
        }
        int tw = text_width(items[i]);
        if (i == sel) draw_text_bold(ix + (iw - tw) / 2, y + (h - 16) / 2, items[i], COL_TEXT);
        else draw_text(ix + (iw - tw) / 2, y + (h - 16) / 2, items[i], COL_TEXT_DIM);
    }
}

/* ------------------------------------------------------------- progress --- */
void ui_progress(int x, int y, int w, int h, int pct, u32 color)
{
    if (h < 6) h = 6;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    fill_round_rect(x, y, w, h, h / 2, 0xE4E7EE);
    int fill_w = w * pct / 100;
    if (fill_w >= h) fill_round_rect(x, y, fill_w, h, h / 2, color);
    else if (fill_w > 0) fill_rect(x, y, fill_w, h, color);
}

/* ------------------------------------------------------------ list rows --- */
void ui_row_bg(int x, int y, int w, int h, int selected, int hovered)
{
    if (selected) { fill_round_rect(x, y, w, h, 6, COL_SELECT); return; }
    if (hovered)  { fill_round_rect(x, y, w, h, 6, 0xF1F3F7); }
}

/* ------------------------------------------------------------- titlerow --- */
void ui_section(int x, int y, const char *title)
{
    draw_text_bold(x, y, title, COL_TEXT_DIM);
}