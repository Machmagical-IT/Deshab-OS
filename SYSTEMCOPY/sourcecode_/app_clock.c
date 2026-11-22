/* ===========================================================================
 *  Nutro+OS - analog + digital clock application (CMOS RTC)
 * ===========================================================================
 */
#include "kernel.h"

typedef struct {
    window_t *win;
} clock_t;

static clock_t g_clock;

/* 12 hour tick directions (unit vector * 256) */
static const s16 tick_dx[12] = { 0, 128, 222, 256, 222, 128, 0, -128, -222, -256, -222, -128 };
static const s16 tick_dy[12] = { -256, -222, -128, 0, 128, 222, 256, 222, 128, 0, -128, -222 };

static void hand(int cx, int cy, int len, int angle_deg, int width, u32 col)
{
    int x = cx + len * fsin_deg(angle_deg) / 1000;
    int y = cy - len * fcos_deg(angle_deg) / 1000;
    draw_line(cx, cy, x, y, col, width);
}

static void clock_draw(window_t *w)
{
    int cx = win_content_x(w), cy = win_content_y(w);
    int wd = w->w, ht = w->h - TITLEBAR_H;
    fill_rect(cx, cy, wd, ht, COL_WIN_CONTENT);

    rtc_time_t t;
    rtc_read(&t);

    int ccx = cx + wd / 2;
    int ccy = cy + 106;
    int r = 88;

    /* face */
    fill_circle(ccx, ccy, r, 0x121A26);
    draw_circle(ccx, ccy, r, COL_ACCENT, 3);
    draw_circle(ccx, ccy, r - 12, 0x1B2534, 1);

    /* hour ticks */
    for (int i = 0; i < 12; i++) {
        int x1 = ccx + (r - 16) * tick_dx[i] / 256;
        int y1 = ccy + (r - 16) * tick_dy[i] / 256;
        int x2 = ccx + (r - 8) * tick_dx[i] / 256;
        int y2 = ccy + (r - 8) * tick_dy[i] / 256;
        draw_line(x1, y1, x2, y2, (i % 3 == 0) ? 0xFFFFFF : COL_TEXT_DIM, (i % 3 == 0) ? 3 : 2);
    }

    /* hands: second 6 deg/step, minute 6, hour 30/60 */
    int sec_a = t.second * 6;
    int min_a = t.minute * 6 + t.second / 10;
    int hr_a  = (t.hour % 12) * 30 + t.minute / 2;

    hand(ccx, ccy, r - 30, hr_a, 5, 0xF0F4FF);
    hand(ccx, ccy, r - 20, min_a, 3, COL_ACCENT);
    hand(ccx, ccy, r - 12, sec_a, 1, COL_CLOSE);
    fill_circle(ccx, ccy, 5, 0xF0F4FF);

    /* digital readout */
    char buf[32];
    ksnprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.hour, t.minute, t.second);
    int tw = (int)strlen(buf) * 8 * 3;
    draw_text_scaled(ccx - tw / 2, ccy + r + 14, buf, COL_ACCENT, 3);

    ksnprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.year, t.month, t.day);
    tw = (int)strlen(buf) * 8;
    draw_text(ccx - tw / 2, ccy + r + 66, buf, COL_TEXT_DIM);

    draw_text(cx + 14, cy + ht - 18, "source: CMOS RTC", COL_TEXT_DIM);
}

static int clock_event(window_t *w, int type, int a, int b, int c)
{
    UNUSED(a); UNUSED(b); UNUSED(c);
    return type == EV_MOUSE_DOWN;      /* just take focus */
}

window_t *app_clock_open(void)
{
    clock_t *c = &g_clock;
    if (c->win && c->win->visible) { win_focus(c->win); return c->win; }

    window_t *w = win_create("Clock", 300, 110, 330, 330, clock_draw, clock_event, c);
    if (w) w->anim = 1;                /* redraw once a second */
    c->win = w;
    return w;
}