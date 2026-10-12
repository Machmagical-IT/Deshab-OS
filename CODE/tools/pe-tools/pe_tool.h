/* pe_tool.h — Deshab PE GUI 工具公共运行时
 *
 * 目标环境：Deshab UTSM PE 兼容层（pe_shim 窗口模式 run_windowed）。
 * 编译：mingw-w64 g++ -nostartfiles -Wl,--entry,main -Wl,--image-base,0x140000000
 *
 * 可用 Win32 面（pe_shim 表内子集）：
 *   user32: RegisterClassExW/CreateWindowExW/PeekMessageW/TranslateMessage/
 *           DispatchMessageW/PostQuitMessage/DefWindowProcW/GetSystemMetrics
 *   gdi32:  CreateCompatibleDC/CreateDIBSection/SelectObject/BitBlt
 *   kernel32: ExitProcess/GetTickCount/QueryPerformanceCounter/Frequency
 *
 * 不可用（调了即未实现 stub / panic）：
 *   TextOut*/SetPixel 等 GDI 绘制（自带 11x18 位图字体 + DIB 直写像素）
 *   文件 IO（CreateFileA → PE-E05 panic）
 *   WM_TIMER（SetTimer 为 no-op；渲染循环每帧自驱）
 *
 * PE32+ 在 Ring0 原生执行 → 端口 I/O（CMOS RTC）直接可用。
 */
#ifndef DESHAB_PE_TOOL_H
#define DESHAB_PE_TOOL_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef unsigned int   u32;
typedef unsigned char  u8;
typedef unsigned short u16;

/* ---- 端口 I/O（Ring0 原生） ---- */
static inline void pt_outb(u16 port, u8 v) {
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(port));
}
static inline u8 pt_inb(u16 port) {
    u8 v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ---- 11x18 ASCII 位图字体（与 firstInit/desktop 同表） ---- */
#include "../../firstInit/ascii_bitmaps.c"

/* ---- 画布（32bpp top-down DIB） ---- */
struct PT_Canvas {
    int w, h;
    u32 *bits;
};

#define PTCOL(r, g, b) (0xFF000000u | ((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

/* Win11 Light 主题令牌（与 fluent_ui.h g_wf_light 对齐） */
#define PT_BG          PTCOL(0xF3, 0xF3, 0xF3)   /* 页面底 */
#define PT_CARD        PTCOL(0xFF, 0xFF, 0xFF)   /* 卡片底 */
#define PT_STROKE      PTCOL(0xD6, 0xD6, 0xD6)   /* 控件描边 */
#define PT_TEXT        PTCOL(0x1A, 0x1A, 0x1A)   /* 主文本 */
#define PT_TEXT2       PTCOL(0x5D, 0x5D, 0x5D)   /* 次文本 */
#define PT_TEXT_DIM    PTCOL(0x8A, 0x8A, 0x8A)   /* 弱文本 */
#define PT_ACCENT      PTCOL(0x00, 0x67, 0xC0)   /* Win11 蓝 */
#define PT_ACCENT_LT   PTCOL(0x00, 0x78, 0xD4)
#define PT_ERROR       PTCOL(0xC4, 0x2B, 0x1C)
#define PT_SUCCESS     PTCOL(0x0F, 0x7B, 0x0F)

static inline void pt_px(PT_Canvas *c, int x, int y, u32 col) {
    if (x < 0 || x >= c->w || y < 0 || y >= c->h) return;
    c->bits[(long)y * c->w + x] = col;
}

static inline void pt_fill(PT_Canvas *c, int x, int y, int w, int h, u32 col) {
    for (int r = 0; r < h; r++)
        for (int q = 0; q < w; q++)
            pt_px(c, x + q, y + r, col);
}

static inline void pt_rect(PT_Canvas *c, int x, int y, int w, int h, u32 col) {
    for (int q = 0; q < w; q++) { pt_px(c, x + q, y, col); pt_px(c, x + q, y + h - 1, col); }
    for (int r = 0; r < h; r++) { pt_px(c, x, y + r, col); pt_px(c, x + w - 1, y + r, col); }
}

static inline void pt_round_fill(PT_Canvas *c, int x, int y, int w, int h,
                                 u32 col, int rad) {
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    for (int r = 0; r < h; r++)
        for (int q = 0; q < w; q++) {
            int dx = 0, dy = 0, corner = 0;
            if (q < rad && r < rad)               { dx = rad - 1 - q; dy = rad - 1 - r; corner = 1; }
            else if (q >= w - rad && r < rad)     { dx = q - (w - rad); dy = rad - 1 - r; corner = 1; }
            else if (q < rad && r >= h - rad)     { dx = rad - 1 - q; dy = r - (h - rad); corner = 1; }
            else if (q >= w - rad && r >= h - rad){ dx = q - (w - rad); dy = r - (h - rad); corner = 1; }
            if (corner && dx * dx + dy * dy > rad * rad) continue;
            pt_px(c, x + q, y + r, col);
        }
}

/* Bresenham 直线（带厚度） */
static inline void pt_line(PT_Canvas *c, int x0, int y0, int x1, int y1,
                           int w, u32 col) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy, half = w / 2;
    for (;;) {
        pt_fill(c, x0 - half, y0 - half, w, w, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* 圆盘 / 圆环 */
static inline void pt_disc(PT_Canvas *c, int cx, int cy, int r, u32 col) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                pt_px(c, cx + dx, cy + dy, col);
}
static inline void pt_ring(PT_Canvas *c, int cx, int cy, int r, int s, u32 col) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 > (r - s) * (r - s))
                pt_px(c, cx + dx, cy + dy, col);
        }
}

/* ---- 文本（11x18 位图，透明底） ---- */
#define PT_FONT_W  11
#define PT_FONT_H  18
#define PT_STEP    10          /* 字距（与 DU_ASCII_STEP 一致） */

static inline int pt_strw(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n * PT_STEP;
}

static inline void pt_char(PT_Canvas *c, char ch, int x, int y, u32 fg) {
    u32 idx = (u32)(ch - ' ');
    if (idx > 94) idx = 0;
    const u8 *glyph = g_ascii[idx];
    for (int r = 0; r < PT_FONT_H; r++)
        for (int q = 0; q < PT_FONT_W; q++) {
            u8 a = glyph[r * PT_FONT_W + q];
            if (a) pt_px(c, x + q, y + r, fg);
        }
}

static inline void pt_text(PT_Canvas *c, const char *s, int x, int y, u32 fg) {
    int cx = x;
    while (*s) { pt_char(c, *s, cx, y, fg); cx += PT_STEP; s++; }
}

static inline void pt_text_center(PT_Canvas *c, const char *s, int cx, int y, u32 fg) {
    pt_text(c, s, cx - pt_strw(s) / 2, y, fg);
}

/* 十进制格式化（无 CRT） */
static inline int pt_dec(u32 v, char *out) {
    char tmp[10];
    int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}
static inline int pt_hex32(u32 v, char *out) {
    static const char hd[] = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++)
        out[2 + i] = hd[(v >> (28 - i * 4)) & 0xF];
    out[10] = 0;
    return 10;
}

/* ---- CMOS RTC（Ring0 端口直读，BCD） ---- */
static inline u8 pt_rtc_reg(int reg) {
    pt_outb(0x70, (u8)reg);
    return pt_inb(0x71);
}
static inline int pt_rtc_bcd(u8 v) { return (v >> 4) * 10 + (v & 0xF); }

struct PT_Time {
    int year, month, day, hour, min, sec;
};
static inline PT_Time pt_rtc_now(void) {
    PT_Time t;
    t.year  = pt_rtc_bcd(pt_rtc_reg(0x09)) + (pt_rtc_bcd(pt_rtc_reg(0x32)) * 100);
    t.month = pt_rtc_bcd(pt_rtc_reg(0x08));
    t.day   = pt_rtc_bcd(pt_rtc_reg(0x07));
    t.hour  = pt_rtc_bcd(pt_rtc_reg(0x04));
    t.min   = pt_rtc_bcd(pt_rtc_reg(0x02));
    t.sec   = pt_rtc_bcd(pt_rtc_reg(0x00));
    if (t.year < 2000) t.year += 2000;   /* 无世纪寄存器的 RTC */
    return t;
}
static inline void pt_fmt_time(PT_Time t, char *out) {
    out[0] = (char)('0' + t.hour / 10); out[1] = (char)('0' + t.hour % 10);
    out[2] = ':';
    out[3] = (char)('0' + t.min / 10);  out[4] = (char)('0' + t.min % 10);
    out[5] = ':';
    out[6] = (char)('0' + t.sec / 10);  out[7] = (char)('0' + t.sec % 10);
    out[8] = 0;
}
static inline void pt_fmt_date(PT_Time t, char *out) {
    out[0] = (char)('0' + t.year / 1000 % 10);
    out[1] = (char)('0' + t.year / 100 % 10);
    out[2] = (char)('0' + t.year / 10 % 10);
    out[3] = (char)('0' + t.year % 10);
    out[4] = '-';
    out[5] = (char)('0' + t.month / 10); out[6] = (char)('0' + t.month % 10);
    out[7] = '-';
    out[8] = (char)('0' + t.day / 10);  out[9] = (char)('0' + t.day % 10);
    out[10] = 0;
}

/* ---- CPUID ---- */
static inline void pt_cpuid(u32 leaf, u32 sub,
                            u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}

/* ============================================================
 *  事件循环运行时
 * ============================================================ */

struct PT_Desc {
    int w, h;                       /* 期望窗口尺寸（自动钳位到 surface） */
    const wchar_t *title;
    void (*frame)(PT_Canvas *cv);   /* 每帧绘制（DIB 直写） */
    void (*key)(unsigned vk, unsigned ch);          /* ch!=0 → 可打印字符 */
    void (*mouse)(int x, int y, int buttons, int edge);
    /* edge: bit0=L down bit1=L up bit2=R down bit3=R up */
};

static PT_Desc  pt_g_desc;
static PT_Canvas pt_g_cv;
static int      pt_g_buttons;
static int      pt_g_last_x, pt_g_last_y;

static LRESULT CALLBACK pt_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: {
        int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
        int btn = 0, edge = 0;
        if (msg == WM_MOUSEMOVE) btn = (wp & 1) ? 1 : 0;
        else if (msg == WM_LBUTTONDOWN) { btn = 1; edge = 1; }
        else if (msg == WM_LBUTTONUP)   { btn = 0; edge = 2; }
        else if (msg == WM_RBUTTONDOWN) { btn = 2; edge = 4; }
        else                            { btn = 0; edge = 8; }
        pt_g_buttons = btn;
        if (pt_g_desc.mouse) pt_g_desc.mouse(x, y, btn, edge);
        pt_g_last_x = x; pt_g_last_y = y;
        return 0;
    }
    case WM_KEYDOWN:
        if (pt_g_desc.key) pt_g_desc.key((unsigned)wp, 0);
        return 0;
    case WM_CHAR:
        if (pt_g_desc.key) pt_g_desc.key(0, (unsigned)wp);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 渲染循环：PeekMessage 泵（空转时 shim 让出给 desktop 宿主帧）+ 每帧绘制 */
int pt_run(const PT_Desc *desc) {
    pt_g_desc = *desc;

    int sw = GetSystemMetrics(0);   /* 窗口模式 = 宿主 surface 尺寸 */
    int sh = GetSystemMetrics(1);
    if (sw > 0 && desc->w > sw) pt_g_desc.w = sw;
    if (sh > 0 && desc->h > sh) pt_g_desc.h = sh;

    WNDCLASSEXW wc;
    for (unsigned i = 0; i < sizeof(wc); i++) ((u8 *)&wc)[i] = 0;
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = pt_wndproc;
    wc.hInstance = 0;
    wc.lpszClassName = L"DESHABPT";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"DESHABPT", pt_g_desc.title, 0,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                pt_g_desc.w, pt_g_desc.h,
                                0, 0, 0, 0);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);

    RECT rc;
    rc.right = 0; rc.bottom = 0;
    GetClientRect(hwnd, &rc);
    pt_g_cv.w = rc.right  ? rc.right  : pt_g_desc.w;
    pt_g_cv.h = rc.bottom ? rc.bottom : pt_g_desc.h;

    /* DIB backbuffer（top-down 32bpp） */
    BITMAPINFO bmi;
    for (unsigned i = 0; i < sizeof(bmi); i++) ((u8 *)&bmi)[i] = 0;
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = pt_g_cv.w;
    bmi.bmiHeader.biHeight = -pt_g_cv.h;     /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC wnd_dc = GetDC(hwnd);
    HDC mem_dc = CreateCompatibleDC(wnd_dc);
    void *bits = 0;
    HBITMAP dib = CreateDIBSection(wnd_dc, &bmi, DIB_RGB_COLORS, &bits, 0, 0);
    if (!dib || !bits) return 2;
    SelectObject(mem_dc, dib);
    pt_g_cv.bits = (u32 *)bits;

    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                ExitProcess((unsigned)msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (pt_g_desc.frame) pt_g_desc.frame(&pt_g_cv);
        BitBlt(wnd_dc, 0, 0, pt_g_cv.w, pt_g_cv.h, mem_dc, 0, 0, SRCCOPY);
    }
    /* not reached */
}

#endif /* DESHAB_PE_TOOL_H */
