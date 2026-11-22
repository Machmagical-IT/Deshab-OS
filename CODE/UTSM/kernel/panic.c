#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/panic.h>
#include <utsm/drr.h>

/* ===================================================================
 *  F1: machmagic 崩溃屏（panic_full）
 *
 *  帧缓冲直绘（fbcon 抽象，F2）：machmagic Logo（TEXTURES/panic/machmagic.png
 *  内嵌）+ panic_symbol + 消息 + 寄存器简表，背景色与图片背景一致，
 *  DRR crash_buf 归档后 halt。不依赖驱动/堆/中断。
 * =================================================================== */

void arch_halt_forever(void);

/* 11x18 Consolas 灰度字库（同 firstInit/ascii_bitmaps.c，自动生成）。
 * 先包含字库再定义 FBCON_HAVE_FONT 引出 fbcon 文本接口。 */
#include "panic_font.inc"
#define FBCON_HAVE_FONT 1

#include <utsm/fbcon.h>

/* machmagic Logo（TEXTURES/panic/machmagic.png 缩放 384px，RGBA8888）+
 * 背景色（图片四角平均色）。生成：kernel/gen_panic_logo.py */
#include "panic_logo_data.inc"

/* ---- hex 输出 ---- */

static void phex64(fbc_ctx *p, u64 v, i64 x, i64 y, u32 fg) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (i32 i = 0; i < 16; i++) {
        u32 nib = (u32)((v >> ((15 - i) * 4)) & 0xF);
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
    buf[18] = 0;
    fbc_string(p, buf, x, y, fg);
}

static void pregline(fbc_ctx *p, const char *name, u64 v, i64 x, i64 y, u32 fg) {
    fbc_string(p, name, x, y, fg);
    phex64(p, v, x + 5 * g_ascii_w, y, fg);
}

/* 按屏宽折行居中。返回下一行 y。超出 y_limit 的行不画。 */
static i64 pwrap(fbc_ctx *p, const char *s, i64 y, i64 y_limit, u32 fg) {
    i64 cols;
    if (!s) return y;
    cols = (i64)p->w / g_ascii_w - 2;
    if (cols < 8) cols = 8;
    if (cols > 120) cols = 120;
    while (*s && y + g_ascii_h <= y_limit) {
        char line[121];
        i64 n = 0;
        while (s[n] && n < cols) { line[n] = s[n]; n++; }
        line[n] = 0;
        s += n;
        fbc_string_center(p, line, y, fg);
        y += g_ascii_h + 4;
    }
    return y;
}

/* ---- panic 模式（panic.h 头注释） ---- */

static int g_panic_fallback;
static u32 g_panic_fallback_hits;

void panic_set_fallback(int enable) {
    (void)enable;
    g_panic_fallback = 0;
    log_info("[PANIC] mode=strict (fallback ignored, bug points halt)");
}

int panic_fallback_enabled(void) {
    return g_panic_fallback;
}

u32 panic_fallback_hits(void) {
    return g_panic_fallback_hits;
}

void panic_full(const char *symbol, const char *msg, const panic_regs *regs) {
    panic_halt(symbol, msg, regs);
}

/* ---- DRR 归档（drr.h 声明，drr_stub.c 提供；未初始化时返回 -1） ---- */

void panic_halt(const char *symbol, const char *msg, const panic_regs *regs) {
    log_error("[PANIC] ==========================================");
    log_error("[PANIC] panic_full: machmagic");
    if (symbol) { log_error("[PANIC] symbol:"); log_error(symbol); }
    if (msg)    { log_error("[PANIC] msg:");    log_error(msg); }
    if (regs) {
        log_hex64("[PANIC] vector=", regs->vector);
        log_hex64("[PANIC] err=", regs->err);
        log_hex64("[PANIC] rip=", regs->rip);
        log_hex64("[PANIC] rsp=", regs->rsp);
        log_hex64("[PANIC] rflags=", regs->rflags);
        log_hex64("[PANIC] cr2=", regs->cr2);
        log_hex64("[PANIC] rax=", regs->rax);
        log_hex64("[PANIC] rcx=", regs->rcx);
        log_hex64("[PANIC] rdx=", regs->rdx);
    }

    int archived = drr_crash_archive(symbol, msg, regs);
    if (archived == 0) {
        log_error("[PANIC] 本次崩溃已归档至 DRR");
    } else {
        log_warn("[PANIC] DRR 归档不可用（未初始化），仅留串口日志");
    }

    /* 屏幕绘制放在归档之后：绘制循环中再出异常也不丢归档 */
    fbc_ctx P, *p = &P;
    if (fbc_init(p) == 0) {
        /* 背景色 = Logo 图片背景色（四角平均，gen_panic_logo.py 生成） */
        fbc_fill_bg(p, g_panic_logo_bg);

        /* 底部留给错误码：Logo 按整数步进缩小，保证 symbol/msg 落在屏内。
         * 长串按屏宽折行，避免居中后错误码被裁出左右边缘。 */
        const u32 fg = 0xFFF0E8D8, dim = 0xFFB0A8C0, acc = 0xFFF0D080;
        i64 text_need = (g_ascii_h + 6) * (regs ? 8 : 5);
        i64 step = 1;
        while (step < 8 &&
               (i64)g_panic_logo_h / step + text_need + 24 > (i64)p->h)
            step++;
        i64 dw = (i64)g_panic_logo_w / step;
        i64 dh = (i64)g_panic_logo_h / step;
        i64 ly = ((i64)p->h - dh - text_need) / 2;
        if (ly < 4) ly = 4;
        {
            i64 lx = ((i64)p->w - dw) / 2;
            for (i64 r = 0; r < dh; r++) {
                for (i64 c = 0; c < dw; c++) {
                    const unsigned char *px4 =
                        g_panic_logo_rgba + (((u64)r * (u64)step) * (u64)g_panic_logo_w
                                             + (u64)c * (u64)step) * 4;
                    u32 a = px4[3];
                    if (a < 8) continue;
                    u32 col = 0xFF000000u | ((u32)px4[0] << 16) | ((u32)px4[1] << 8) | px4[2];
                    if (a < 250) col = fbc_blend(fbc_pixel_read(p, lx + c, ly + r), col, a);
                    fbc_pixel(p, lx + c, ly + r, col);
                }
            }
        }

        i64 y_limit = (i64)p->h - 4;
        i64 y = ly + dh + 8;
        if (y + text_need > y_limit) y = y_limit - text_need;
        if (y < 4) y = 4;
        y = pwrap(p, "DESHAB PANIC", y, y_limit, acc);
        y += 4;
        y = pwrap(p, symbol, y, y_limit, fg);
        y = pwrap(p, msg, y, y_limit, fg);
        if (regs && y + g_ascii_h * 3 < y_limit) {
            i64 rx = (i64)p->w / 2 - 12 * g_ascii_w;
            if (rx < 4) rx = 4;
            pregline(p, "RIP ", regs->rip, rx, y, dim); y += g_ascii_h + 2;
            if (y + g_ascii_h < y_limit) { pregline(p, "RSP ", regs->rsp, rx, y, dim); y += g_ascii_h + 2; }
            if (y + g_ascii_h < y_limit) { pregline(p, "FLG ", regs->rflags, rx, y, dim); y += g_ascii_h + 2; }
            if (regs->vector == 14 && y + g_ascii_h < y_limit)
                pregline(p, "CR2 ", regs->cr2, rx, y, dim);
        }
        if ((i64)p->h > g_ascii_h + 8) {
            fbc_string_center(p, archived == 0 ? "crash archived to DRR" : "DRR archive unavailable",
                              (i64)p->h - g_ascii_h - 8, dim);
        }
    }

    log_error("[PANIC] halt");
    arch_halt_forever();
    for (;;) __asm__ volatile("cli; hlt");
}

void panic(const char *msg) {
    panic_full("UTSM-PANIC", msg, 0);
}
