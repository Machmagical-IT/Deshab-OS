/* paint.cpp — Deshab 画板（GUI PE64 工具）
 *
 * 顶栏：色板（10 色）+ 笔刷（2/6/12px）+ Clear。
 * 客户区：左键拖拽画线（独立持久层，UI 每帧叠加）。
 * 键盘：C = 清空；Esc 关闭。
 * 无文件 IO（shim CreateFileA → PE-E05），本工具为纯画布。
 */
#include "pe_tool.h"

#define TB_H      52
#define SWATCH_N  10
#define SWATCH_S  24

static const u32 g_swatches[SWATCH_N] = {
    PTCOL(0x1A, 0x1A, 0x1A), PTCOL(0xFF, 0xFF, 0xFF),
    PTCOL(0xC4, 0x2B, 0x1C), PTCOL(0xF7, 0x63, 0x16),
    PTCOL(0xFF, 0xB9, 0x00), PTCOL(0x0F, 0x7B, 0x0F),
    PTCOL(0x00, 0x67, 0xC0), PTCOL(0x8B, 0x5C, 0xF6),
    PTCOL(0xE3, 0x00, 0x8B), PTCOL(0x8A, 0x8A, 0x8A),
};

static u32 *g_layer;          /* 持久绘画层（画布区） */
static int  g_layer_w, g_layer_h;
static int  g_cvw;            /* 实际画布宽（窗口可能被 surface 钳位） */
static int  g_color = 6;      /* 默认 Win11 蓝 */
static int  g_brush = 2;      /* 笔刷索引：0=2px 1=6px 2=12px */
static const int g_brush_px[3] = { 2, 6, 12 };
static int g_drawing;
static int g_last_x, g_last_y;

static void layer_alloc(int w, int h) {
    g_layer_w = w;
    g_layer_h = h;
    for (long i = 0; i < (long)w * h; i++) g_layer[i] = PTCOL(0xFF, 0xFF, 0xFF);
}

static void stroke_to(int x, int y) {
    int px = g_brush_px[g_brush];
    /* 持久层画线（构造视图 canvas） */
    PT_Canvas cv;
    cv.w = g_layer_w; cv.h = g_layer_h; cv.bits = g_layer;
    pt_line(&cv, g_last_x, g_last_y, x, y, px, g_swatches[g_color]);
    pt_disc(&cv, x, y, px / 2, g_swatches[g_color]);
    g_last_x = x; g_last_y = y;
}

static void mouse(int x, int y, int buttons, int edge) {
    int cw = g_cvw;

    /* 顶栏命中 */
    if (edge & 1) {   /* 左键按下 */
        if (y < TB_H) {
            /* 色板 */
            for (int i = 0; i < SWATCH_N; i++) {
                int sx = 10 + i * (SWATCH_S + 6);
                if (x >= sx && x < sx + SWATCH_S &&
                    y >= (TB_H - SWATCH_S) / 2 && y < (TB_H - SWATCH_S) / 2 + SWATCH_S) {
                    g_color = i;
                    return;
                }
            }
            /* 笔刷 */
            for (int i = 0; i < 3; i++) {
                int bx = 10 + SWATCH_N * (SWATCH_S + 6) + 12 + i * 40;
                if (x >= bx && x < bx + 36 && y >= 8 && y < TB_H - 8) {
                    g_brush = i;
                    return;
                }
            }
            /* Clear */
            int cx = cw - 80;
            if (x >= cx && x < cx + 72 && y >= 12 && y < TB_H - 12) {
                layer_alloc(g_layer_w, g_layer_h);
                return;
            }
            return;
        }
        /* 画布起笔 */
        g_drawing = 1;
        g_last_x = x; g_last_y = y - TB_H;
        stroke_to(x, y - TB_H);
        return;
    }
    if (edge & 2) g_drawing = 0;
    if (buttons & 1 && g_drawing && y >= TB_H)
        stroke_to(x, y - TB_H);
}

static void key(unsigned vk, unsigned ch) {
    if (ch == 'c' || ch == 'C') layer_alloc(g_layer_w, g_layer_h);
}

static void frame(PT_Canvas *cv) {
    g_cvw = cv->w;
    /* 画布层 → DIB */
    for (int r = 0; r < g_layer_h && r + TB_H < cv->h; r++)
        for (int q = 0; q < g_layer_w && q < cv->w; q++)
            cv->bits[(long)(r + TB_H) * cv->w + q] = g_layer[(long)r * g_layer_w + q];

    /* 顶栏 */
    pt_fill(cv, 0, 0, cv->w, TB_H, PT_BG);
    pt_fill(cv, 0, TB_H - 1, cv->w, 1, PT_STROKE);
    for (int i = 0; i < SWATCH_N; i++) {
        int sx = 10 + i * (SWATCH_S + 6);
        int sy = (TB_H - SWATCH_S) / 2;
        pt_round_fill(cv, sx, sy, SWATCH_S, SWATCH_S, g_swatches[i], 6);
        if (g_color == i)
            pt_rect(cv, sx - 2, sy - 2, SWATCH_S + 4, SWATCH_S + 4, PT_ACCENT);
    }
    for (int i = 0; i < 3; i++) {
        int bx = 10 + SWATCH_N * (SWATCH_S + 6) + 12 + i * 40;
        int bc = g_brush == i ? PT_ACCENT : PT_TEXT2;
        pt_disc(cv, bx + 18, TB_H / 2, g_brush_px[i] / 2 + 1, bc);
        if (g_brush == i)
            pt_rect(cv, bx, 8, 36, TB_H - 16, PT_ACCENT);
    }
    pt_round_fill(cv, cv->w - 80, 12, 72, TB_H - 24, PT_CARD, 4);
    pt_rect(cv, cv->w - 80, 12, 72, TB_H - 24, PT_STROKE);
    pt_text_center(cv, "Clear", cv->w - 44, TB_H / 2 - 9, PT_TEXT);

    pt_text(cv, "C = clear   Esc = close", 10, cv->h - 24, PT_TEXT_DIM);
}

int main(void) {
    static u32 layer_mem[900 * 700];   /* 静态层：msvcrt malloc shim 可用，但静态最稳 */
    g_layer = layer_mem;
    PT_Desc d;
    d.w = 700; d.h = 560;
    d.title = L"Paint";
    d.frame = frame;
    d.key = key;
    d.mouse = mouse;
    /* 层尺寸按期望值预分配；pt_run 钳位后窗口只会更小（绘制有界） */
    layer_alloc(700, 560 - TB_H);
    return pt_run(&d);
}
