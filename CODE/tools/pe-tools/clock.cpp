/* clock.cpp — Deshab 时钟（GUI PE64 工具）
 *
 * 模拟表盘（时/分/秒针）+ 数字时间/日期，RTC 实时刷新。
 * 渲染循环每帧自驱（无 WM_TIMER，SetTimer 在 shim 中是 no-op）。
 * Esc 关闭。
 */
#include "pe_tool.h"

static void draw_hand(PT_Canvas *cv, int cx, int cy,
                      double angle, int len, int width, u32 col) {
    /* 角度 0 = 12 点方向，顺时针 */
    int ex = cx + (int)(len * angle_sin(angle));
    int ey = cy - (int)(len * angle_cos(angle));
    pt_line(cv, cx, cy, ex, ey, width, col);
}

/* sin/cos：角度单位 = 圈（0..1）。表盘刻度只需粗精度，
 * 用 0.1° 查表 + 线性插值避免引入浮点库调用。 */
static const short S_SIN[91] = { /* sin(0..90°)*10000 */
    0, 175, 349, 523, 698, 872, 1045, 1219, 1392, 1564,
    1736, 1908, 2079, 2250, 2419, 2588, 2756, 2924, 3090, 3256,
    3420, 3584, 3746, 3907, 4067, 4226, 4384, 4540, 4695, 4848,
    5000, 5150, 5299, 5446, 5592, 5736, 5878, 6018, 6157, 6293,
    6428, 6561, 6691, 6820, 6947, 7071, 7193, 7314, 7431, 7547,
    7660, 7771, 7880, 7986, 8090, 8192, 8290, 8387, 8480, 8572,
    8660, 8746, 8829, 8910, 8988, 9063, 9135, 9205, 9272, 9336,
    9397, 9455, 9511, 9563, 9613, 9659, 9703, 9744, 9781, 9816,
    9848, 9877, 9903, 9925, 9945, 9962, 9976, 9986, 9994, 9998,
    10000,
};

static double sin_deg(double deg) {
    /* 折叠到 [0,90] 再查表插值；deg ∈ [0,360) */
    int neg = 0;
    if (deg >= 180.0) { deg -= 180.0; neg = 1; }
    if (deg >= 90.0) deg = 180.0 - deg;
    int i = (int)deg;
    if (i > 89) i = 89;      /* deg==90 → [89]→[90] 插值 1.0 */
    double frac = deg - i;
    double v = (S_SIN[i] + (S_SIN[i + 1] - S_SIN[i]) * frac) / 10000.0;
    return neg ? -v : v;
}

static double angle_sin(double turns) {
    double d = turns * 360.0;
    d -= (long)(d / 360.0) * 360.0;
    if (d < 0) d += 360.0;
    return sin_deg(d);
}

static double angle_cos(double turns) {
    return angle_sin(turns + 0.25);
}

static void frame(PT_Canvas *cv) {
    pt_fill(cv, 0, 0, cv->w, cv->h, PT_BG);
    PT_Time t = pt_rtc_now();

    /* 表盘卡 */
    int cx = cv->w / 2;
    int cy = cv->h / 2 - 20;
    int r = cv->h / 3;
    if (r > cv->w / 3) r = cv->w / 3;
    if (r > 200) r = 200;
    if (r < 60) r = 60;

    pt_disc(cv, cx, cy, r + 14, PT_CARD);
    pt_ring(cv, cx, cy, r + 14, 2, PT_STROKE);
    pt_ring(cv, cx, cy, r, 2, PT_STROKE);

    /* 刻度：12 大格 */
    for (int i = 0; i < 12; i++) {
        double a = i / 12.0;
        int sx = cx + (int)((r - 14) * angle_sin(a));
        int sy = cy - (int)((r - 14) * angle_cos(a));
        int ex = cx + (int)((r - 4) * angle_sin(a));
        int ey = cy - (int)((r - 4) * angle_cos(a));
        pt_line(cv, sx, sy, ex, ey, i % 3 == 0 ? 4 : 2, PT_TEXT2);
    }

    /* 指针 */
    double h_turns = (t.hour % 12) / 12.0 + t.min / 720.0;
    double m_turns = t.min / 60.0 + t.sec / 3600.0;
    double s_turns = t.sec / 60.0;
    draw_hand(cv, cx, cy, h_turns, r / 2,      7, PT_TEXT);
    draw_hand(cv, cx, cy, m_turns, r * 3 / 4,  5, PT_TEXT);
    draw_hand(cv, cx, cy, s_turns, r - 18,     2, PT_ERROR);
    pt_disc(cv, cx, cy, 6, PT_TEXT);
    pt_disc(cv, cx, cy, 3, PT_CARD);

    /* 数字时间/日期 */
    char b[16];
    pt_fmt_time(t, b);
    pt_text_center(cv, b, cx, cy + r + 26, PT_TEXT);
    pt_fmt_date(t, b);
    pt_text_center(cv, b, cx, cy + r + 50, PT_TEXT_DIM);

    pt_text(cv, "Esc = close", 16, cv->h - 26, PT_TEXT_DIM);
}

int main(void) {
    PT_Desc d;
    d.w = 420; d.h = 560;
    d.title = L"Clock";
    d.frame = frame;
    d.key = 0;
    d.mouse = 0;
    return pt_run(&d);
}
