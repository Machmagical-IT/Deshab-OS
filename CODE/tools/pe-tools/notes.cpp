/* notes.cpp — Deshab 记事本（GUI PE64 工具）
 *
 * 单缓冲文本编辑：WM_CHAR 键入、Backspace 删、Enter 换行、
 * 自动折行 + 尾部自动滚动；底部状态栏显示字符数。
 * 无文件 IO（shim CreateFileA → PE-E05），纯内存便签。
 * Esc 关闭。
 */
#include "pe_tool.h"

#define NOTE_CAP   4096
#define LINE_H     20
#define MARGIN     10

static char g_buf[NOTE_CAP + 1];
static int  g_len;
static int  g_view_line;        /* 顶部可见行 */

/* 折行游标走查：返回总行数；*end_line 与 *end_col 为缓冲末尾（光标）位置 */
static int layout_walk(int cols, int *end_line, int *end_col) {
    int line = 0, col = 0;
    for (int i = 0; i < g_len; i++) {
        if (g_buf[i] == '\n') { line++; col = 0; continue; }
        col++;
        if (col >= cols) { line++; col = 0; }
    }
    *end_line = line;
    *end_col = col;
    return line + 1;
}

/* shim 键盘合成契约：可打印字符（>=0x20）走 WM_CHAR；Backspace/Enter/Esc
 * 等控制键只走 WM_KEYDOWN（vk），WM_CHAR 不合成。 */
static void key(unsigned vk, unsigned ch) {
    if (ch) {
        if (ch >= 32 && ch < 127 && g_len < NOTE_CAP)
            g_buf[g_len++] = (char)ch;
        return;
    }
    if (vk == 8) {              /* Backspace */
        if (g_len > 0) g_len--;
        return;
    }
    if (vk == 13) {             /* Enter */
        if (g_len < NOTE_CAP) g_buf[g_len++] = '\n';
        return;
    }
}

static void frame(PT_Canvas *cv) {
    pt_fill(cv, 0, 0, cv->w, cv->h, PT_BG);

    int cols = (cv->w - MARGIN * 2) / PT_STEP;
    if (cols < 8) cols = 8;
    int view_h = cv->h - LINE_H;          /* 底部状态栏高度 LINE_H */
    int rows = view_h / LINE_H;
    if (rows < 1) rows = 1;

    int cur_line, cur_col;
    int total = layout_walk(cols, &cur_line, &cur_col);
    (void)total;

    /* 自动滚动：光标保持可见 */
    if (cur_line >= g_view_line + rows) g_view_line = cur_line - rows + 1;
    if (cur_line < g_view_line) g_view_line = cur_line;
    if (g_view_line < 0) g_view_line = 0;

    /* 文本区卡片 */
    pt_round_fill(cv, MARGIN / 2, MARGIN / 2, cv->w - MARGIN, view_h - MARGIN,
                  PT_CARD, 8);

    /* 折行绘制（只画可见行窗口） */
    {
        int line = 0, col = 0;
        for (int i = 0; i < g_len; i++) {
            char c = g_buf[i];
            if (c == '\n') { line++; col = 0; continue; }
            if (line >= g_view_line && line < g_view_line + rows)
                pt_char(cv, c, MARGIN + col * PT_STEP,
                        MARGIN + (line - g_view_line) * LINE_H, PT_TEXT);
            col++;
            if (col >= cols) { line++; col = 0; }
            if (line >= g_view_line + rows && line > cur_line) break;
        }
    }

    /* 光标（RTC 秒闪烁） */
    {
        PT_Time t = pt_rtc_now();
        if ((t.sec & 1) == 0 && cur_line >= g_view_line &&
            cur_line < g_view_line + rows) {
            pt_fill(cv, MARGIN + cur_col * PT_STEP,
                    MARGIN + (cur_line - g_view_line) * LINE_H,
                    2, LINE_H - 4, PT_ACCENT);
        }
    }

    /* 状态栏 */
    char b[24];
    int n = pt_dec((u32)g_len, b);
    b[n++] = '/';
    n += pt_dec(NOTE_CAP, b + n);
    b[n] = 0;
    pt_fill(cv, 0, cv->h - LINE_H, cv->w, LINE_H, PT_BG);
    pt_text(cv, b, MARGIN, cv->h - LINE_H + 2, PT_TEXT_DIM);
    pt_text(cv, "Esc = close", cv->w - MARGIN - 11 * PT_STEP,
            cv->h - LINE_H + 2, PT_TEXT_DIM);
}

int main(void) {
    PT_Desc d;
    d.w = 560; d.h = 480;
    d.title = L"Notes";
    d.frame = frame;
    d.key = key;
    d.mouse = 0;
    return pt_run(&d);
}
