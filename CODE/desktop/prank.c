/* prank.c — Deshab 整蛊系统（"致力于打造极差的用户体验"）
 *
 * 设计目标：在用户正常使用桌面时，不定时播放一段"恶作剧"叠加层，
 * 制造"这系统是不是坏了"的错觉，随后若无其事地消失。
 *
 * 已实现整蛊（全部由 FUCK [prank] 段控制，见 build/configs/<variant>/FUCK）：
 *   PK_BSOD     — 假蓝屏（Win 风格 + 哭泣脸 + 伪造 Stop code）
 *   PK_INVERT   — 全屏反色闪烁
 *   PK_HANG     — 假"程序未响应"对话框（带等待点点动画）
 *   PK_GARBLE   — 打字乱码：按键被随机映射到别的字母/数字
 *   PK_FLY      — 任务栏图标乱飞（任务栏溶解，应用图标四散飞出）
 *   PK_SHUTDOWN — 假"系统即将关机"倒计时（数到 0 什么也不发生）
 *
 * 触发方式：
 *   1) 开机进桌面后 boot_demo_sec 秒的"串烧演示"（用于验证效果）
 *   2) 使用中每隔 interval_sec 秒随机播放一个已启用的整蛊
 *
 * 渲染：只往 sprite buffer(g_fb.fb) 上叠画，绝不直接写真实 fb；
 * 由 compositor 在光标之前调用 prank_draw()。运行时用 TSC 计时，
 * 遵守 -mno-sse/-msoft-float，全程整数运算。
 */
#include "desktop.h"
#include "zhfont.h"   /* PK_SHUTDOWN 的中文文案 */

/* ============================================================
 *  配置（FUCK [prank]；extern 声明见 desktop.h）
 *  默认全部打开：无 FUCK 或缺键时也整蛊。FUCK 里写 0 才单独关掉。
 * ============================================================ */
int g_prank_enabled   = 1;   /* 总开关，enable=0 关闭 */
int g_prank_boot_demo = 5;   /* 进桌面后演示秒数，0=关 */
int g_prank_interval  = 60;  /* 使用中自动整蛊间隔（秒），0=关 */
int g_prank_bsod      = 1;   /* 假蓝屏 */
int g_prank_invert    = 1;   /* 反色闪屏 */
int g_prank_hang      = 1;   /* 假"未响应"对话框 */
int g_prank_garble    = 1;   /* 打字乱码 */
int g_prank_fly       = 1;   /* 任务栏图标乱飞 */
int g_prank_shutdown  = 1;   /* 假"系统即将关机"倒计时 */

/* ============================================================
 *  运行时状态
 * ============================================================ */
enum { PK_NONE = 0, PK_BSOD, PK_INVERT, PK_HANG,
       PK_GARBLE, PK_FLY, PK_SHUTDOWN };

#define PK_KIND_MAX  6

#define PK_MS_BSOD      3000u
#define PK_MS_INVERT    1500u
#define PK_MS_HANG      2600u
#define PK_MS_GARBLE    5000u
#define PK_MS_FLY       2600u
#define PK_MS_SHUTDOWN  6000u

static int  s_kind  = PK_NONE;
static u64  s_start = 0;
static u64  s_dur   = 0;
static int  s_need_clear = 0;  /* 1 = 可视整蛊刚结束，需要一次全屏重绘擦掉叠加层 */

static u64  s_next  = 0;      /* 下次自动整蛊的 TSC 时刻 */
static int  s_armed = 0;

static int  s_demo    = 0;    /* 1 = 开机演示串烧进行中 */
static int  s_q[PK_KIND_MAX];
static int  s_qn      = 0;
static int  s_qi      = 0;
static int  s_demo_ms = 0;

static u32  s_rng  = 0x12345678u;
static u32  s_seed = 0;       /* 本次整蛊的随机种子（图标飞散用） */

/* ============================================================
 *  小工具
 * ============================================================ */

static u64 ms_to_tsc(u32 ms) {
    if (g_tsc_per_sec) return g_tsc_per_sec * (u64)ms / 1000u;
    return (u64)ms * 1000000u;   /* 未校准兜底（约数） */
}

static unsigned int rng_next(void) {
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

static int key_is(const char *k, int kl, const char *name) {
    int i = 0;
    while (name[i]) i++;
    if (i != kl) return 0;
    for (i = 0; i < kl; i++) if (k[i] != name[i]) return 0;
    return 1;
}

static int parse_int(const char *s, int len) {
    int v = 0, neg = 0, i = 0;
    if (len > 0 && s[0] == '-') { neg = 1; i = 1; }
    for (; i < len && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (s[i] - '0');
    return neg ? -v : v;
}

static u32 kind_ms(int k) {
    switch (k) {
    case PK_BSOD:     return PK_MS_BSOD;
    case PK_INVERT:   return PK_MS_INVERT;
    case PK_HANG:     return PK_MS_HANG;
    case PK_GARBLE:   return PK_MS_GARBLE;
    case PK_FLY:      return PK_MS_FLY;
    case PK_SHUTDOWN: return PK_MS_SHUTDOWN;
    default:          return 1000u;
    }
}

static void start_prank(int kind, u32 ms) {
    s_kind  = kind;
    s_start = rdtsc();
    s_dur   = ms_to_tsc(ms);
    if (s_dur == 0) s_dur = 1;
    s_seed  = (u32)rdtsc() ^ (rng_next() << 8);
}

/* ============================================================
 *  FUCK [prank] 单行解析（"key=value"，注释/空行由 main 过滤）
 * ============================================================ */
void prank_config_line(const char *line, int len) {
    int eq = -1;
    for (int i = 0; i < len; i++) if (line[i] == '=') { eq = i; break; }
    if (eq <= 0) return;

    const char *k = line;
    int kl = eq;
    const char *v = line + eq + 1;
    int vl = len - eq - 1;
    while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t' || v[vl - 1] == '\r')) vl--;
    int val = parse_int(v, vl);

    if      (key_is(k, kl, "enable"))        g_prank_enabled   = val ? 1 : 0;
    else if (key_is(k, kl, "boot_demo_sec")) g_prank_boot_demo = val;
    else if (key_is(k, kl, "interval_sec"))  g_prank_interval  = val;
    else if (key_is(k, kl, "fake_bsod"))     g_prank_bsod      = val ? 1 : 0;
    else if (key_is(k, kl, "invert_flash"))  g_prank_invert    = val ? 1 : 0;
    else if (key_is(k, kl, "fake_hang"))     g_prank_hang      = val ? 1 : 0;
    else if (key_is(k, kl, "garble_typing")) g_prank_garble    = val ? 1 : 0;
    else if (key_is(k, kl, "taskbar_fly"))   g_prank_fly       = val ? 1 : 0;
    else if (key_is(k, kl, "fake_shutdown")) g_prank_shutdown  = val ? 1 : 0;
}

/* ============================================================
 *  调度
 * ============================================================ */

/* 收集当前启用的整蛊种类（写入 out，返回个数，≤ PK_KIND_MAX）。 */
static int collect_kinds(int *out) {
    int n = 0;
    if (g_prank_bsod)     out[n++] = PK_BSOD;
    if (g_prank_invert)   out[n++] = PK_INVERT;
    if (g_prank_hang)     out[n++] = PK_HANG;
    if (g_prank_garble)   out[n++] = PK_GARBLE;
    if (g_prank_fly)      out[n++] = PK_FLY;
    if (g_prank_shutdown) out[n++] = PK_SHUTDOWN;
    return n;
}

/* 开机进桌面后的串烧演示：把已启用的整蛊平分 boot_demo_sec 秒依次播放。 */
void prank_boot_demo_start(void) {
    if (!g_prank_enabled || g_prank_boot_demo <= 0) return;

    s_qn = collect_kinds(s_q);
    if (s_qn == 0) return;

    u32 each = (u32)g_prank_boot_demo * 1000u / (u32)s_qn;
    if (each < 300u) each = 300u;

    s_rng     ^= (u32)rdtsc();
    s_demo     = 1;
    s_qi       = 0;
    s_demo_ms  = (int)each;
    s_armed    = 0;
    start_prank(s_q[0], each);
    slog("[prank] boot demo start");
}

void prank_tick(void) {
    if (!g_prank_enabled) {
        if (s_kind != PK_NONE && s_kind != PK_GARBLE) s_need_clear = 1;
        s_kind = PK_NONE;
        s_demo = 0;
        s_armed = 0;
        return;
    }
    if (!g_tsc_per_sec) return;   /* TSC 未校准前不动 */

    u64 now = rdtsc();

    if (s_kind != PK_NONE) {
        if (now - s_start >= s_dur) {
            if (s_kind != PK_GARBLE) s_need_clear = 1;
            s_kind = PK_NONE;
            if (s_demo) {
                s_qi++;
                if (s_qi < s_qn) start_prank(s_q[s_qi], (u32)s_demo_ms);
                else { s_demo = 0; s_armed = 0; slog("[prank] boot demo done"); }
            } else {
                s_armed = 0;
            }
        }
        return;
    }

    if (s_demo) return;
    if (g_prank_interval <= 0) return;

    if (!s_armed) {
        s_next  = now + ms_to_tsc((u32)g_prank_interval * 1000u);
        s_armed = 1;
        return;
    }
    if (now < s_next) return;

    int cand[PK_KIND_MAX];
    int n = collect_kinds(cand);
    if (n == 0) { s_armed = 0; return; }

    int k = cand[(rng_next() >> 16) % (u32)n];
    start_prank(k, kind_ms(k));
    s_armed = 0;
}

int prank_active(void) { return s_kind != PK_NONE; }

/* 是否需要在整蛊期间强制全屏重绘（打字乱码纯作用于输入，无需重绘）。 */
int prank_wants_redraw(void) {
    if (s_need_clear) return 1;   /* 结束后补一帧全屏重绘，擦掉残留叠加层 */
    return s_kind != PK_NONE && s_kind != PK_GARBLE;
}

/* 打字乱码：把可打印键的按下码随机映射到另一个可打印码。
 * 释放码与特殊键（Esc/回车/退格/Tab/方向键/修饰键/E0 前缀）原样放行。 */
u8 prank_filter_key(u8 sc) {
    if (s_kind != PK_GARBLE) return sc;
    if (sc & 0x80) return sc;          /* 释放码不动 */

    static const u8 keys[] = {
        0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,   /* 1..0 */
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,   /* qwertyuiop */
        0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,        /* asdfghjkl */
        0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32,                  /* zxcvbnm */
        0x39                                                  /* space */
    };
    int n = (int)(sizeof(keys) / sizeof(keys[0]));
    int idx = -1;
    for (int i = 0; i < n; i++) if (keys[i] == sc) { idx = i; break; }
    if (idx < 0) return sc;            /* 非可打印键原样 */

    return keys[rng_next() % (unsigned int)n];
}

/* ============================================================
 *  绘制原语（全部只作用于 sprite buffer）
 * ============================================================ */

/* 全屏压暗：每通道 * (256-a)/256 */
static void blend_dark(du_context *ctx, u32 a) {
    if (a == 0) return;
    if (a > 255) a = 255;
    u32 ia = 256u - a;
    for (u64 y = 0; y < ctx->height; y++) {
        u32 *line = (u32 *)((u8 *)ctx->fb + y * ctx->pitch);
        for (u64 x = 0; x < ctx->width; x++) {
            u32 c = line[x];
            u32 r = ((c & 0xFF) * ia) >> 8;
            u32 g = (((c >> 8) & 0xFF) * ia) >> 8;
            u32 b = (((c >> 16) & 0xFF) * ia) >> 8;
            line[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
}

/* 全屏反色（保留 alpha） */
static void invert_all(du_context *ctx) {
    for (u64 y = 0; y < ctx->height; y++) {
        u32 *line = (u32 *)((u8 *)ctx->fb + y * ctx->pitch);
        for (u64 x = 0; x < ctx->width; x++) line[x] ^= 0x00FFFFFFu;
    }
}

/* 放大版 ASCII 字符串：以 (cx,cy) 为中心，每字形像素放大 scale 倍。
 * alpha 0..255 控制整体不透明度。 */
static void draw_scale_text(du_context *ctx, const char *s, int cx, int cy,
                            int scale, u32 fg, int alpha) {
    if (scale < 1) scale = 1;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    if (alpha == 0) return;

    int n = kstrlen(s);
    if (n <= 0) return;
    int gw = g_ascii_w, gh = g_ascii_h;
    int step = (gw + 1) * scale;
    int total_w = n * step - scale;
    int x0 = cx - total_w / 2;
    int y0 = cy - gh * scale / 2;

    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 32 || ch > 126) continue;
        const unsigned char *g = g_ascii[ch - 32];
        int gx = x0 + i * step;
        for (int r = 0; r < gh; r++) {
            for (int c = 0; c < gw; c++) {
                unsigned char ga = g[r * gw + c];
                if (!ga) continue;
                int aa = (int)ga * alpha / 255;
                if (aa <= 0) continue;
                for (int sy = 0; sy < scale; sy++) {
                    int py = y0 + r * scale + sy;
                    if (py < 0 || (u64)py >= ctx->height) continue;
                    u32 *line = (u32 *)((u8 *)ctx->fb + (u64)py * ctx->pitch);
                    for (int sx = 0; sx < scale; sx++) {
                        int px = gx + c * scale + sx;
                        if (px < 0 || (u64)px >= ctx->width) continue;
                        line[px] = du_blend(line[px], fg, (u32)aa);
                    }
                }
            }
        }
    }
}

/* ============================================================
 *  各整蛊的绘制
 * ============================================================ */

/* PK_BSOD — 假蓝屏 */
static void draw_bsod(du_context *ctx, int pct) {
    int W = (int)ctx->width, H = (int)ctx->height;
    du_fill_bg_solid(ctx, 0xFF0078D7u);

    int x = W / 8; if (x < 80) x = 80;
    int y = H / 2 - 150;
    if (y < 40) y = 40;

    draw_scale_text(ctx, ":(", x + 90, y + 40, 5, 0xFFFFFFFFu, 255);

    const char *l1 = "Your Deshab ran into a problem and needs to restart.";
    const char *l2 = "We're just collecting some error info, and then";
    const char *l3 = "we'll restart for you.";
    int ty = y + 110;
    du_draw_string(ctx, l1, x, ty, 0xFFFFFFFFu, 0xFF0078D7u, DU_ASCII_STEP); ty += 34;
    du_draw_string(ctx, l2, x, ty, 0xFFFFFFFFu, 0xFF0078D7u, DU_ASCII_STEP); ty += 22;
    du_draw_string(ctx, l3, x, ty, 0xFFFFFFFFu, 0xFF0078D7u, DU_ASCII_STEP); ty += 50;

    /* "(NN% complete)" */
    int n = pct; if (n < 0) n = 0; if (n > 100) n = 100;
    char pb[40]; int i = 0;
    pb[i++] = '(';
    if (n >= 100) { pb[i++] = '1'; pb[i++] = '0'; pb[i++] = '0'; }
    else { pb[i++] = (char)('0' + n / 10); pb[i++] = (char)('0' + n % 10); }
    const char *suf = "% complete)";
    while (*suf) pb[i++] = *suf++;
    pb[i] = 0;
    du_draw_string(ctx, pb, x, ty, 0xFFFFFFFFu, 0xFF0078D7u, DU_ASCII_STEP); ty += 44;

    du_draw_string(ctx, "Stop code: FUCKED_BY_DESIGN", x, ty,
                   0xFFFFFFFFu, 0xFF0078D7u, DU_ASCII_STEP);
}

/* PK_INVERT — 全屏反色 */
static void draw_invert(du_context *ctx) {
    invert_all(ctx);
}

/* PK_HANG — 假"程序未响应"对话框 */
static void draw_hang(du_context *ctx, int pct) {
    int W = (int)ctx->width, H = (int)ctx->height;
    blend_dark(ctx, 110);

    int w = 520, h = 200;
    int x = (W - w) / 2, y = (H - h) / 2;

    /* 阴影 + 卡片 */
    du_fill_rect(ctx, x + 8, y + 8, w, h, 0xFF6B7280u);
    du_fill_rect(ctx, x, y, w, h, 0xFFF3F4F6u);
    du_rect_outline(ctx, x, y, w, h, 0xFF9CA3AFu, 0);

    /* 标题栏 */
    du_fill_rect(ctx, x, y, w, 34, 0xFFE5E7EBu);
    du_draw_string(ctx, "Deshab - Not Responding", x + 12, y + 8,
                   0xFF111827u, 0xFFE5E7EBu, DU_ASCII_STEP);

    /* 正文 */
    du_draw_string(ctx, "This program is not responding.", x + 20, y + 56,
                   0xFF1F2937u, 0xFFF3F4F6u, DU_ASCII_STEP);
    du_draw_string(ctx, "It may respond if you wait a bit.", x + 20, y + 80,
                   0xFF1F2937u, 0xFFF3F4F6u, DU_ASCII_STEP);
    du_draw_string(ctx, "Do you want to close it?", x + 20, y + 104,
                   0xFF1F2937u, 0xFFF3F4F6u, DU_ASCII_STEP);

    /* 等待点点动画（0..3 个点循环） */
    int dots = (pct / 12) % 4;
    char db[8]; db[0] = '.'; db[1] = '.'; db[2] = '.'; db[3] = '.'; db[4] = 0;
    db[dots] = 0;
    du_draw_string(ctx, "Waiting", x + 20, y + 138, 0xFF6B7280u, 0xFFF3F4F6u, DU_ASCII_STEP);
    du_draw_string(ctx, db, x + 20 + 7 * (int)DU_ASCII_STEP, y + 138,
                   0xFF6B7280u, 0xFFF3F4F6u, DU_ASCII_STEP);

    /* 按钮 */
    int bw = 120, bh = 34, by = y + h - 54;
    int bx2 = x + w - 20 - bw, bx1 = bx2 - bw - 12;

    du_fill_rect(ctx, bx1, by, bw, bh, 0xFFE5E7EBu);
    du_rect_outline(ctx, bx1, by, bw, bh, 0xFF9CA3AFu, 0);
    du_draw_string(ctx, "Wait", bx1 + (bw - 4 * (int)DU_ASCII_STEP) / 2, by + 8,
                   0xFF111827u, 0xFFE5E7EBu, DU_ASCII_STEP);

    du_fill_rect(ctx, bx2, by, bw, bh, 0xFF2563EBu);
    du_draw_string(ctx, "Close", bx2 + (bw - 5 * (int)DU_ASCII_STEP) / 2, by + 8,
                   0xFFFFFFFFu, 0xFF2563EBu, DU_ASCII_STEP);
}

/* PK_FLY — 任务栏图标乱飞：底部任务栏"溶解"，应用图标四散飞出屏幕 */
static void draw_fly(du_context *ctx, int pct) {
    int W = (int)ctx->width, H = (int)ctx->height;
    blend_dark(ctx, 90);

    /* 任务栏条带溶解为深色（视觉上任务栏没了） */
    int strip_h = 72;
    int sy = H - strip_h; if (sy < 0) sy = 0;
    du_fill_rect(ctx, 0, sy, W, strip_h, 0xFF14161Au);

    /* 应用图标从任务栏位置四散飞出 */
    int n = g_app_count; if (n < 6) n = 6; if (n > 8) n = 8;
    int base_y = H - 40;
    for (int i = 0; i < n; i++) {
        u32 r = s_seed ^ ((u32)i * 2654435761u);
        int vx = (int)(r & 0xFFu) - 128;                 /* -128..127 */
        vx = vx * 3;                                     /* -384..381 px */
        int vy = -(int)(((r >> 8) & 0xFFu) * 3u + 120u); /* 向上飞 */
        int start_x = W / 2 + (i - n / 2) * 56;
        int x = start_x + vx * pct / 100;
        int y = base_y + vy * pct / 100;
        int appid = g_app_count > 0 ? (i % g_app_count) : 0;
        du_fill_rect(ctx, x + 3, y + 3, 40, 40, 0xFF000000u);   /* 阴影 */
        draw_app_icon(ctx, appid, x, y);
    }
}

/* PK_SHUTDOWN — 假"系统即将关机"倒计时 */
static void draw_shutdown(du_context *ctx, int pct) {
    int W = (int)ctx->width, H = (int)ctx->height;
    du_fill_bg_solid(ctx, 0xFF0A0E14u);

    int remain = 6 - (pct * 6 / 100);
    if (remain < 0) remain = 0;

    /* 中文标题（16px 位图字库；24px 未懒加载前不使用） */
    const char *zh_title = "系统即将关机";
    int zw = zh_string_width(zh_title, 0);
    zh_draw_string(ctx, zh_title, (W - zw) / 2, H / 2 - 130, 0xFFFFD24Au, 0);

    /* 大号倒计时数字 */
    char num[4];
    num[0] = (char)('0' + remain);
    num[1] = 0;
    draw_scale_text(ctx, num, W / 2, H / 2 + 10, 10, 0xFFFFFFFFu, 255);

    /* 中英提示 */
    const char *zh_sub = "请立即保存您的工作";
    int zw2 = zh_string_width(zh_sub, 0);
    zh_draw_string(ctx, zh_sub, (W - zw2) / 2, H / 2 + 120, 0xFFB0B6C0u, 0);

    const char *en = "System will shut down automatically.";
    int ew = kstrlen(en) * (int)DU_ASCII_STEP;
    du_draw_string(ctx, en, (W - ew) / 2, H / 2 + 152,
                   0xFF9AA0A8u, 0xFF0A0E14u, DU_ASCII_STEP);

    /* 底部进度条（时间越少越红） */
    int bw = W / 3, bx = (W - bw) / 2, byy = H - 80;
    du_fill_rect(ctx, bx, byy, bw, 8, 0xFF2A2F38u);
    int fw = bw * (100 - pct) / 100;
    if (fw > 0) du_fill_rect(ctx, bx, byy, fw, 8, 0xFFE5484Du);
}

/* ============================================================
 *  对外绘制入口（compositor 在光标之前调用）
 * ============================================================ */
void prank_draw(du_context *ctx) {
    /* 本函数只在 redraw_all() 内部被调用：走到这里说明本帧已经整屏重绘过，
     * 无论这一帧画不画叠加层，残留标记都可以清掉。 */
    s_need_clear = 0;
    if (s_kind == PK_NONE) return;

    u64 el = rdtsc() - s_start;
    int pct = (int)(el * 100u / (s_dur ? s_dur : 1u));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    switch (s_kind) {
    case PK_BSOD:     draw_bsod(ctx, pct);     break;
    case PK_INVERT:   draw_invert(ctx);        break;
    case PK_HANG:     draw_hang(ctx, pct);     break;
    case PK_GARBLE:   break;                   /* 纯输入效果，无叠加层 */
    case PK_FLY:      draw_fly(ctx, pct);      break;
    case PK_SHUTDOWN: draw_shutdown(ctx, pct); break;
    default: break;
    }
}
