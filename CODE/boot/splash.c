/* splash.c — 引导器机械风格加载界面（由 main.c 直接 #include，共享其全局状态）
 *
 * 画面：拉丝枪灰底板 + 一钢一铜两只啮合齿轮 + 浮雕标题 + 铆接仪表面板
 * （40 段进度条、七段数码百分比、阶段/文件数/字节数读数）。
 *
 * 进度是真实的：装载前先做一遍 plan（只取文件大小，累计总字节数与文件数），
 * 装载时按已读字节推进到 90%，其余 10% 由内核重定位/页表/交接三个阶段占用。
 *
 * 防闪烁：静态底板只画一次进 g_spl_static；每帧把齿轮区与面板区从静态底板
 * 拷进 g_spl_back，在缓冲里合成，再整块拷到 GOP。屏幕上的像素每帧只写一次。
 * 全部整数运算（引导器以 -mno-sse -msoft-float 编译）。 */

static inline u64 spl_rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static u32 *g_spl_static, *g_spl_back;
static int g_sw, g_sh;
static int g_spl_on, g_spl_failed;
static u64 g_tsc_ms, g_tsc0, g_next_frame, g_last_ms;

static u64 g_bytes_loaded, g_bytes_total;
static u32 g_files_done, g_files_total;
static int g_plan;                 /* 1 = 只统计大小，不读正文 */
static u32 g_phase_q;              /* 阶段下限，0..10000 */
static u32 g_disp_q;               /* 显示值（缓动），0..10000 */
static const char *g_stage = "INITIALIZING";

static u64 spl_ms(void) { return g_tsc_ms ? (spl_rdtsc() - g_tsc0) / g_tsc_ms : 0; }

/* TSC 频率：PIT 通道 2 单次计数 10ms。部分固件的 BS->Stall 在虚拟机里
 * 偏慢数倍，不能拿来定标；无 PIT 的平台（OUT2 永不置位）才退回 Stall。 */
static u64 spl_tsc_calibrate(void) {
    u8 p61 = inb(0x61);
    outb(0x61, (u8)((p61 & ~0x02) | 0x01));
    outb(0x43, 0xB0);
    outb(0x42, 0x9C);                         /* 11932 = 10ms @ 1.193182MHz */
    outb(0x42, 0x2E);
    u64 t0 = spl_rdtsc();
    u64 t1 = t0;
    int ok = 0;
    while ((t1 = spl_rdtsc()) - t0 < 20000000000ULL) {
        if (inb(0x61) & 0x20) { ok = 1; break; }
    }
    outb(0x61, p61);
    if (ok && t1 - t0 > 100000) return (t1 - t0) / 10u;
    t0 = spl_rdtsc();
    BS->Stall(20000);
    return (spl_rdtsc() - t0) / 20u;
}

static void spl_wait_ms(u64 ms) {
    u64 end = spl_rdtsc() + ms * g_tsc_ms;
    while (spl_rdtsc() < end) __asm__ volatile("pause");
}

/* ---------------- 数学：正弦表 / 整数开方 / atan2 ---------------- */
static short g_sin[1024];          /* Q14，下标 = 角度 >> 6（整圈 65536） */

static void sin_init(void) {
    s64 c = 1LL << 30, s = 0;
    const s64 kc = 1073721611LL, ks = 6588397LL;   /* cos/sin(2π/1024)，Q30 */
    for (int i = 0; i < 1024; i++) {
        g_sin[i] = (short)(s >> 16);
        s64 nc = (c * kc - s * ks) >> 30;
        s64 ns = (s * kc + c * ks) >> 30;
        c = nc; s = ns;
    }
}
static inline int isin(u32 a) { return g_sin[(a >> 6) & 1023]; }
static inline int icos(u32 a) { return g_sin[((a + 16384u) >> 6) & 1023]; }

static u32 isqrt32(u32 v) {
    u32 r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* 返回 0..65535，0 = +x，16384 = +y（屏幕向下） */
static u32 atan2u(int y, int x) {
    if (!x && !y) return 0;
    u32 ax = (u32)(x < 0 ? -x : x), ay = (u32)(y < 0 ? -y : y);
    u32 a;
    if (ax >= ay) {
        u32 z = (ay << 12) / ax;
        a = (8192u * z + ((2847u * z) >> 12) * (4096u - z)) >> 12;
    } else {
        u32 z = (ax << 12) / ay;
        a = 16384u - ((8192u * z + ((2847u * z) >> 12) * (4096u - z)) >> 12);
    }
    if (x < 0) a = 32768u - a;
    if (y < 0) a = 65536u - a;
    return a & 0xFFFFu;
}

static u32 hash32(u32 x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iabs(int a) { return a < 0 ? -a : a; }

/* 1/16 像素有符号距离 → 0..256 覆盖率（1 像素抗锯齿带） */
static inline int cov16(int d16) {
    d16 += 8;
    if (d16 <= 0) return 0;
    if (d16 >= 16) return 256;
    return d16 * 16;
}

static inline u32 smooth256(u32 t) {
    if (t >= 256) return 256;
    return t * t * (768u - 2u * t) / 65536u;
}

/* ---------------- 颜色 ---------------- */
static inline u32 cmix(u32 a, u32 b, int t) {
    if (t <= 0) return a;
    if (t >= 256) return b;
    u32 na = 256u - (u32)t;
    u32 r = (((a >> 16) & 0xFF) * na + ((b >> 16) & 0xFF) * (u32)t) >> 8;
    u32 g = (((a >> 8) & 0xFF) * na + ((b >> 8) & 0xFF) * (u32)t) >> 8;
    u32 bl = ((a & 0xFF) * na + (b & 0xFF) * (u32)t) >> 8;
    return (r << 16) | (g << 8) | bl;
}

static inline u32 cadd(u32 c, int d) {
    int r = (int)((c >> 16) & 0xFF) + d, g = (int)((c >> 8) & 0xFF) + d, b = (int)(c & 0xFF) + d;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

static inline u32 cscale(u32 c, int f) {
    if (f >= 256) return c;
    if (f <= 0) return 0;
    u32 r = ((c >> 16) & 0xFF) * (u32)f >> 8, g = ((c >> 8) & 0xFF) * (u32)f >> 8, b = (c & 0xFF) * (u32)f >> 8;
    return (r << 16) | (g << 8) | b;
}

/* 金属明度 L(0..255) → 暗/中/亮三色插值 */
static inline u32 metal(u32 dark, u32 mid, u32 light, int L) {
    if (L < 0) L = 0;
    if (L > 255) L = 255;
    return L < 128 ? cmix(dark, mid, L * 2) : cmix(mid, light, (L - 128) * 2);
}

/* ---------------- 缓冲绘制原语 ---------------- */
static void buf_fill(u32 *b, int x, int y, int w, int h, u32 c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_sw) w = g_sw - x;
    if (y + h > g_sh) h = g_sh - y;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) b[yy * g_sw + xx] = c;
}

static void buf_blend(u32 *b, int x, int y, int w, int h, u32 c, int a) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_sw) w = g_sw - x;
    if (y + h > g_sh) h = g_sh - y;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) {
            u32 *p = &b[yy * g_sw + xx];
            *p = cmix(*p, c, a);
        }
}

/* 圆角矩形有符号距离（1/16 像素，内部为负） */
static int rrect_sd16(int x, int y, int x0, int y0, int x1, int y1, int rad) {
    int px = x * 16 + 8, py = y * 16 + 8;
    int cx = (x0 + x1) * 8, cy = (y0 + y1) * 8;
    int hx = (x1 - x0) * 8 - rad * 16, hy = (y1 - y0) * 8 - rad * 16;
    int qx = iabs(px - cx) - hx, qy = iabs(py - cy) - hy;
    int ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    int d = (ox || oy) ? (int)isqrt32((u32)(ox * ox + oy * oy)) : imax(qx, qy);
    return d - rad * 16;
}

/* 圆顶铆钉 / 螺丝（slot=1 带十字槽） */
static void draw_rivet(u32 *b, int cx, int cy, int R, int slot) {
    for (int y = cy - R - 3; y <= cy + R + 3; y++) {
        if (y < 0 || y >= g_sh) continue;
        for (int x = cx - R - 3; x <= cx + R + 3; x++) {
            if (x < 0 || x >= g_sw) continue;
            int dx = x * 16 + 8 - cx * 16 - 8, dy = y * 16 + 8 - cy * 16 - 8;
            int d = (int)isqrt32((u32)(dx * dx + dy * dy));
            u32 *p = &b[y * g_sw + x];
            /* 落影 */
            int sdx = dx - 24, sdy = dy - 32;
            int sd = (int)isqrt32((u32)(sdx * sdx + sdy * sdy)) - R * 16;
            int sh = sd < 32 ? imin(110, (32 - sd) * 110 / 64) : 0;
            if (sh > 0) *p = cscale(*p, 256 - sh);
            int cv = cov16(R * 16 - d);
            if (!cv) continue;
            int L = 140 - (dx + dy) * 60 / (R * 16);
            if (d > R * 16 - 24) L -= 50;
            if (slot && ((iabs(dx) < 20 && iabs(dy) < R * 11) || (iabs(dy) < 20 && iabs(dx) < R * 11))) L = 30;
            *p = cmix(*p, metal(0x24292F, 0x737D88, 0xD6DCE2, L), cv);
        }
    }
}

/* ---------------- 5x7 点阵字库 ---------------- */
static const u8 k_font[][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    {0x01,0x01,0x02,0x04,0x08,0x10,0x10}, {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

static const u8 *glyph(char ch) {
    if (ch >= '0' && ch <= '9') return k_font[ch - '0'];
    if (ch >= 'A' && ch <= 'Z') return k_font[10 + ch - 'A'];
    if (ch >= 'a' && ch <= 'z') return k_font[10 + ch - 'a'];
    switch (ch) {
        case '/': return k_font[36];
        case '.': return k_font[37];
        case '%': return k_font[38];
        case ':': return k_font[39];
        case '-': return k_font[40];
        default:  return k_font[41];
    }
}

static int text_w(const char *s, int sc, int track) {
    int n = (int)str_len(s);
    return n ? n * (5 * sc + sc + track) - sc - track : 0;
}

/* 逐行渐变着色（top→bot），用于浮雕标题；top==bot 即单色 */
static void draw_text(u32 *b, int x, int y, int sc, int track, const char *s, u32 top, u32 bot) {
    for (; *s; s++) {
        const u8 *g = glyph(*s);
        for (int r = 0; r < 7; r++) {
            u32 c = cmix(top, bot, r * 256 / 6);
            for (int col = 0; col < 5; col++)
                if (g[r] & (0x10 >> col)) buf_fill(b, x + col * sc, y + r * sc, sc, sc, c);
        }
        x += 5 * sc + sc + track;
    }
}

static void draw_text_sh(u32 *b, int x, int y, int sc, int track, const char *s, u32 c) {
    draw_text(b, x + 1, y + 1, sc, track, s, 0x050607, 0x050607);
    draw_text(b, x, y, sc, track, s, c, c);
}

static char *fmt_u(char *p, u64 v) {
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 23);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}

static char *fmt_mb(char *p, u64 bytes) {
    u64 t = bytes * 10 / (1024 * 1024);
    p = fmt_u(p, t / 10);
    *p++ = '.';
    *p++ = (char)('0' + t % 10);
    *p = 0;
    return p;
}

/* ---------------- 布局 ---------------- */
typedef struct {
    int cx, cy, n, spokes;                       /* 中心为 1/16 像素 */
    int ro, rr, hwr, hwt, hub, bore, axle, rim, spoke_hw, span;
    u64 arc_k, spoke_k;                          /* 弧长系数：arc = |相位| * r * k >> 36 */
    u32 dark, mid, light;
} spl_gear;

static spl_gear g_g1, g_g2;
static int g_gx0, g_gy0, g_gx1, g_gy1;           /* 齿轮动态区 */
static int g_px, g_py, g_pw, g_ph;               /* 面板 */
static int g_tx, g_ty, g_tw, g_th;               /* 进度槽 */
static int g_dx, g_dy;                           /* 七段数码起点 */
static u32 g_mesh_c;
static int g_shoff_x, g_shoff_y;

#define GEAR_N1 18
#define GEAR_N2 11
#define DIGIT_W 16
#define DIGIT_H 30
#define DIGIT_T 4
#define DIGIT_GAP 5
#define READOUT_W (3 * DIGIT_W + 2 * DIGIT_GAP + 6 + 15)

static void gear_setup(spl_gear *g, int cx16, int cy16, int n, int m16) {
    int rp = m16 * n / 2;
    g->cx = cx16; g->cy = cy16; g->n = n;
    g->ro = rp + m16;
    g->rr = rp - m16 * 5 / 4;
    g->hwr = m16 * 115 / 100;
    g->hwt = m16 * 40 / 100;
    g->span = g->ro - g->rr;
    g->arc_k = (1609ULL << 12) / (u64)n;
}

static void spl_layout(void) {
    int W = g_sw, H = g_sh;
    int m = H / 80;
    if (m < 6) m = 6;
    int m16 = m * 16;
    const u32 phi = 65536u - 7282u;              /* 小齿轮在大齿轮右上方 -40° */
    int D = m16 * (GEAR_N1 + GEAR_N2) / 2;
    int dxs = D * icos(phi) / 16384, dys = D * isin(phi) / 16384;
    int ro1 = m16 * GEAR_N1 / 2 + m16, ro2 = m16 * GEAR_N2 / 2 + m16;
    int cx1 = W * 8 - (dxs + ro2 - ro1) / 2;
    int cy1 = H * 16 * 30 / 100 - (dys - ro2 + ro1) / 2;

    gear_setup(&g_g1, cx1, cy1, GEAR_N1, m16);
    g_g1.spokes = 5;
    g_g1.bore = m16 * 13 / 10;
    g_g1.axle = m16 * 95 / 100;
    g_g1.hub = m16 * 28 / 10;
    g_g1.rim = g_g1.rr - m16 * 15 / 10;
    g_g1.spoke_hw = m16 * 80 / 100;
    g_g1.spoke_k = (1609ULL << 12) / 5u;
    g_g1.dark = 0x272C32; g_g1.mid = 0x7A848F; g_g1.light = 0xE6EBF0;

    gear_setup(&g_g2, cx1 + dxs, cy1 + dys, GEAR_N2, m16);
    g_g2.spokes = 0;
    g_g2.bore = m16;
    g_g2.axle = m16 * 70 / 100;
    g_g2.hub = m16 * 2;
    g_g2.rim = 0;
    g_g2.spoke_hw = 0;
    g_g2.dark = 0x3E2C12; g_g2.mid = 0xA77F3C; g_g2.light = 0xF7E3A6;

    /* 啮合相位：rot1 = φ 时大齿轮齿顶正对小齿轮的齿槽 */
    g_mesh_c = phi + 32768u - 32768u / GEAR_N2 + (u32)((u64)phi * GEAR_N1 / GEAR_N2);

    g_shoff_x = m16 / 2;
    g_shoff_y = m16 * 9 / 10;
    int pad = 14;
    g_gx0 = imin(g_g1.cx - g_g1.ro, g_g2.cx - g_g2.ro) / 16 - pad;
    g_gx1 = imax(g_g1.cx + g_g1.ro, g_g2.cx + g_g2.ro) / 16 + g_shoff_x / 16 + pad;
    g_gy0 = imin(g_g1.cy - g_g1.ro, g_g2.cy - g_g2.ro) / 16 - pad;
    g_gy1 = imax(g_g1.cy + g_g1.ro, g_g2.cy + g_g2.ro) / 16 + g_shoff_y / 16 + pad;
    if (g_gx0 < 0) g_gx0 = 0;
    if (g_gy0 < 0) g_gy0 = 0;
    if (g_gx1 > W) g_gx1 = W;
    if (g_gy1 > H) g_gy1 = H;

    g_pw = W * 55 / 100;
    if (g_pw > 760) g_pw = 760;
    if (g_pw < 420) g_pw = imin(W - 20, 420);
    g_ph = 124;
    g_px = (W - g_pw) / 2;
    g_py = H * 64 / 100;
    if (g_py + g_ph > H - 4) g_py = H - 4 - g_ph;
    g_tx = g_px + 20;
    g_ty = g_py + 54;
    g_tw = g_pw - 40;
    g_th = 24;
    g_dx = g_px + g_pw - 24 - READOUT_W;
    g_dy = g_py + 12;
}

/* ---------------- 静态底板 ---------------- */
static void spl_build_static(void) {
    int W = g_sw, H = g_sh;
    u32 *b = g_spl_static;

    for (int y = 0; y < H; y++) {
        u32 base = cmix(0x2D3239, 0x111316, y * 256 / H);
        int row = (int)(hash32((u32)y * 2654435761u) & 7) - 4;
        int vy = 2 * y - H;
        int vy2 = (int)((s64)vy * vy * 128 / ((s64)H * H));
        for (int x = 0; x < W; x++) {
            int vx = 2 * x - W;
            int vx2 = (int)((s64)vx * vx * 128 / ((s64)W * W));
            int n = (int)(hash32((u32)x * 73856093u ^ (u32)y * 19349663u) & 3) - 1 + row;
            u32 c = cadd(base, n);
            b[y * W + x] = cscale(c, 256 - (vx2 + vy2) * 100 / 256);
        }
    }

    /* 上下两道钢板接缝 + 四角螺丝 */
    int yt = H * 7 / 100, yb = H * 93 / 100;
    buf_fill(b, 0, yt, W, 2, 0x07080A);
    buf_fill(b, 0, yt + 2, W, 1, 0x3B424A);
    buf_fill(b, 0, yb, W, 2, 0x07080A);
    buf_fill(b, 0, yb + 2, W, 1, 0x30363D);
    int sy0 = yt / 2, sy1 = yb + (H - yb) / 2;
    draw_rivet(b, 34, sy0, 9, 1);
    draw_rivet(b, W - 34, sy0, 9, 1);
    draw_rivet(b, 34, sy1, 9, 1);
    draw_rivet(b, W - 34, sy1, 9, 1);

    /* 浮雕标题 */
    int sc = H / 110;
    if (sc < 4) sc = 4;
    const char *title = "DESHAB";
    int tw = text_w(title, sc, sc);
    int tx = (W - tw) / 2, ty = H * 50 / 100;
    int so = sc / 2 + 1;
    draw_text(b, tx + so, ty + so, sc, sc, title, 0x040506, 0x040506);
    draw_text(b, tx - 1, ty - 1, sc, sc, title, 0x9AA4AE, 0x59626C);
    draw_text(b, tx, ty, sc, sc, title, 0xF0F3F6, 0x7D8792);

    const char *sub = "UTSM BOOT SEQUENCE";
    int sw = text_w(sub, 2, 4);
    int sx = (W - sw) / 2, sy = ty + 7 * sc + 18;
    draw_text_sh(b, sx, sy, 2, 4, sub, 0x8893A0);
    buf_fill(b, sx - 84, sy + 6, 64, 1, 0x4C5560);
    buf_fill(b, sx - 84, sy + 7, 64, 1, 0x08090B);
    buf_fill(b, sx + sw + 20, sy + 6, 64, 1, 0x4C5560);
    buf_fill(b, sx + sw + 20, sy + 7, 64, 1, 0x08090B);

    const char *foot = "DRR // UTSM // DKM // SAS-R0-PCQ";
    draw_text_sh(b, (W - text_w(foot, 2, 2)) / 2, yb + (H - yb) / 2 - 7, 2, 2, foot, 0x47505A);

    /* 仪表面板：落影 + 黑边 + 渐变钢板 + 上沿高光 */
    int x0 = g_px, y0 = g_py, x1 = g_px + g_pw, y1 = g_py + g_ph;
    for (int y = y0 - 16; y < y1 + 24; y++) {
        if (y < 0 || y >= H) continue;
        for (int x = x0 - 20; x < x1 + 20; x++) {
            if (x < 0 || x >= W) continue;
            u32 *p = &b[y * W + x];
            int ssd = rrect_sd16(x, y, x0 - 4, y0 + 2, x1 + 4, y1 + 10, 18);
            if (ssd < 256) {
                int sh = ssd < -256 ? 150 : (256 - ssd) * 150 / 512;
                *p = cscale(*p, 256 - sh);
            }
            int sd = rrect_sd16(x, y, x0, y0, x1, y1, 12);
            int cv = cov16(-sd);
            if (!cv) continue;
            u32 fill = cmix(0x323840, 0x1A1D22, (y - y0) * 256 / g_ph);
            fill = cadd(fill, (int)(hash32((u32)x * 2246822519u ^ (u32)y * 3266489917u) & 3) - 1);
            if (sd > -36) fill = cmix(fill, y < y0 + g_ph / 2 ? 0x5C6570 : 0x0D0F12, 150);
            u32 c = cmix(0x050608, fill, cov16(-sd - 16));
            *p = cmix(*p, c, cv);
        }
    }
    draw_rivet(b, x0 + 12, y0 + 12, 4, 0);
    draw_rivet(b, x1 - 13, y0 + 12, 4, 0);
    draw_rivet(b, x0 + 12, y1 - 13, 4, 0);
    draw_rivet(b, x1 - 13, y1 - 13, 4, 0);

    /* 七段数码窗 */
    {
        int rx0 = g_dx - 9, ry0 = g_dy - 5, rx1 = g_dx + READOUT_W + 9, ry1 = g_dy + DIGIT_H + 5;
        for (int y = ry0 - 2; y < ry1 + 2; y++)
            for (int x = rx0 - 2; x < rx1 + 2; x++) {
                int sd = rrect_sd16(x, y, rx0, ry0, rx1, ry1, 5);
                u32 *p = &b[y * W + x];
                if (sd >= 0 && sd < 20 && y > (ry0 + ry1) / 2) { *p = cmix(*p, 0x3E4650, 160); continue; }
                int cv = cov16(-sd);
                if (!cv) continue;
                u32 c = sd > -28 && y < (ry0 + ry1) / 2 ? 0x030405 : 0x07090C;
                *p = cmix(*p, c, cv);
            }
    }

    /* 进度槽：凹陷 + 下沿反光 + 刻度 */
    for (int y = g_ty - 3; y < g_ty + g_th + 3; y++)
        for (int x = g_tx - 3; x < g_tx + g_tw + 3; x++) {
            int sd = rrect_sd16(x, y, g_tx, g_ty, g_tx + g_tw, g_ty + g_th, 5);
            u32 *p = &b[y * W + x];
            if (sd >= 0 && sd < 20 && y > g_ty + g_th / 2) { *p = cmix(*p, 0x404853, 170); continue; }
            int cv = cov16(-sd);
            if (!cv) continue;
            u32 c = (y < g_ty + 3) ? 0x030405 : 0x090B0E;
            *p = cmix(*p, c, cv);
        }
    for (int k = 0; k <= 10; k++) {
        int x = g_tx + 4 + k * (g_tw - 9) / 10;
        int major = (k % 5) == 0;
        buf_fill(b, x, g_ty + g_th + 6, major ? 2 : 1, major ? 8 : 4, 0x5A646F);
        buf_fill(b, x + 1, g_ty + g_th + 6, 1, major ? 8 : 4, 0x0A0B0D);
    }
}

/* ---------------- 齿轮 ----------------
 * 极坐标（半径 1/16 像素 + 角度）与圆形落影都与转角无关：建底板时算一次，
 * 落影直接烙进静态底板，极坐标存表。每帧只剩齿形/辐条的整数判定。 */
static u32 *g_polar[2];           /* 齿轮动态区逐像素 (r16 << 16) | angle；~0 = 区外 */

static int gear_shadow(const spl_gear *g, int x, int y) {
    int dx = x * 16 + 8 - g->cx - g_shoff_x, dy = y * 16 + 8 - g->cy - g_shoff_y;
    int rs = (g->rr + g->ro) / 2;
    u32 d2 = (u32)(dx * dx + dy * dy);
    u32 lim = (u32)(rs + 192);
    if (d2 > lim * lim) return 0;
    int d = (int)isqrt32(d2) - rs;
    if (d < -192) return 150;
    return (192 - d) * 150 / 384;
}

static void gear_bake(void) {
    int gw = g_gx1 - g_gx0, gh = g_gy1 - g_gy0;
    usize n = (usize)gw * (usize)gh;
    g_polar[0] = (u32 *)alloc_pages(n * 4u);
    g_polar[1] = (u32 *)alloc_pages(n * 4u);
    const spl_gear *gs[2] = { &g_g1, &g_g2 };
    for (int y = g_gy0; y < g_gy1; y++)
        for (int x = g_gx0; x < g_gx1; x++) {
            u32 *p = &g_spl_static[(u64)y * g_sw + x];
            int s = imax(gear_shadow(&g_g1, x, y), gear_shadow(&g_g2, x, y));
            if (s) *p = cscale(*p, 256 - s);
            usize i = (usize)(y - g_gy0) * (usize)gw + (usize)(x - g_gx0);
            for (int k = 0; k < 2; k++) {
                if (!g_polar[k]) continue;
                const spl_gear *g = gs[k];
                int dx = x * 16 + 8 - g->cx, dy = y * 16 + 8 - g->cy;
                u32 d2 = (u32)(dx * dx + dy * dy);
                u32 lim = (u32)(g->ro + 16);
                g_polar[k][i] = d2 > lim * lim ? 0xFFFFFFFFu
                              : (isqrt32(d2) << 16) | atan2u(dy, dx);
            }
        }
}

static int gear_pixel(const spl_gear *g, u32 pol, u32 rot, u32 *out) {
    int r = (int)(pol >> 16);
    u32 a = pol & 0xFFFFu;
    int light = icos(a - 40960u);             /* 法线朝左上 = 受光 */

    if (r < g->axle + 8) {
        int L = 120 + light * 70 / 16384 * r / (g->axle ? g->axle : 1);
        if (r > g->axle - 20) L -= 45;
        *out = metal(0x1E2226, 0x5E6770, 0xB8C0C8, L);
        return cov16(g->axle - r);
    }

    int sph = (short)(unsigned short)((a - rot) * (u32)g->n);
    if (sph < 0) sph = -sph;
    int arc = (int)(((u64)(u32)sph * (u32)r * g->arc_k) >> 36);
    int rt = r - g->rr;
    rt = rt < 0 ? 0 : rt > g->span ? g->span : rt;
    int hw = g->hwr + (g->hwt - g->hwr) * rt / g->span;
    int d = imax(g->rr - r, imin(hw - arc, g->ro - r));
    int d_bore = r - g->bore;
    if (d_bore < d) d = d_bore;
    int cv = cov16(d);
    if (!cv) return 0;

    int L = 138 + icos(2u * a + 16384u) * 52 / 16384;
    L += (int)(hash32((u32)(r >> 3) * 2654435761u + (u32)g->n) & 15) - 8;
    if (d < 48) L += (48 - d) * light / 16384 * 85 / 48;
    if (d_bore < 40) L -= (40 - d_bore) * light / 16384 * 70 / 40;

    if (g->spokes) {
        int sps = (short)(unsigned short)((a - rot) * (u32)g->spokes);
        if (sps < 0) sps = -sps;
        int arc_s = (int)(((u64)(u32)sps * (u32)r * g->spoke_k) >> 36);
        int dh = imin(imin(r - g->hub, g->rim - r), arc_s - g->spoke_hw);
        int hole = cov16(dh);
        cv = cv * (256 - hole) / 256;
        if (!cv) return 0;
        if (dh > -44) L -= (44 + imin(dh, 0)) * 2;
        if (r < g->hub + 8) L += 14;
    } else {
        int dg = iabs(r - g->hub);
        if (dg < 22) L -= (22 - dg) * 4;
        else if (r < g->hub) L += 12;
        int dr = iabs(r - (g->rr - 20));
        if (dr < 14) L -= (14 - dr) * 3;
    }
    *out = metal(g->dark, g->mid, g->light, L);
    return cv;
}

static void spl_draw_gears(u64 ms) {
    u32 rot1_total = (u32)((ms * 65536u / 9000u) % (65536u * GEAR_N2));
    u32 rot1 = rot1_total & 0xFFFFu;
    u32 rot2 = g_mesh_c - (u32)((u64)rot1_total * GEAR_N1 / GEAR_N2);
    int gw = g_gx1 - g_gx0;
    if (!g_polar[0] || !g_polar[1]) return;
    for (int y = g_gy0; y < g_gy1; y++) {
        const u32 *src = g_spl_static + (u64)y * g_sw;
        u32 *dst = g_spl_back + (u64)y * g_sw;
        const u32 *p1 = g_polar[0] + (usize)(y - g_gy0) * (usize)gw - g_gx0;
        const u32 *p2 = g_polar[1] + (usize)(y - g_gy0) * (usize)gw - g_gx0;
        for (int x = g_gx0; x < g_gx1; x++) {
            u32 c = src[x];
            u32 gc;
            int cv;
            if (p1[x] != 0xFFFFFFFFu && (cv = gear_pixel(&g_g1, p1[x], rot1, &gc)) != 0) c = cmix(c, gc, cv);
            if (p2[x] != 0xFFFFFFFFu && (cv = gear_pixel(&g_g2, p2[x], rot2, &gc)) != 0) c = cmix(c, gc, cv);
            dst[x] = c;
        }
    }
}
/* ---------------- 面板动态部分 ---------------- */
static const u8 k_seg7[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

static void seg7(u32 *b, int x, int y, u8 mask, u32 col) {
    const int w = DIGIT_W, h = DIGIT_H, t = DIGIT_T, hh = DIGIT_H / 2;
    if (mask & 0x01) buf_fill(b, x + t, y, w - 2 * t, t, col);
    if (mask & 0x02) buf_fill(b, x + w - t, y + t, t, hh - t - t / 2, col);
    if (mask & 0x04) buf_fill(b, x + w - t, y + hh + t / 2, t, hh - t - t / 2, col);
    if (mask & 0x08) buf_fill(b, x + t, y + h - t, w - 2 * t, t, col);
    if (mask & 0x10) buf_fill(b, x, y + hh + t / 2, t, hh - t - t / 2, col);
    if (mask & 0x20) buf_fill(b, x, y + t, t, hh - t - t / 2, col);
    if (mask & 0x40) buf_fill(b, x + t, y + hh - t / 2, w - 2 * t, t, col);
}

static void draw_disk(u32 *b, int cx, int cy, int R16, u32 col, int alpha) {
    int R = R16 / 16 + 2;
    for (int y = cy - R; y <= cy + R; y++)
        for (int x = cx - R; x <= cx + R; x++) {
            if (x < 0 || y < 0 || x >= g_sw || y >= g_sh) continue;
            int dx = x * 16 + 8 - cx * 16 - 8, dy = y * 16 + 8 - cy * 16 - 8;
            int cv = cov16(R16 - (int)isqrt32((u32)(dx * dx + dy * dy)));
            if (cv) { u32 *p = &b[y * g_sw + x]; *p = cmix(*p, col, cv * alpha / 256); }
        }
}

static void spl_draw_panel(u64 ms) {
    u32 *b = g_spl_back;
    for (int y = g_py; y < g_py + g_ph; y++)
        memcpy(b + (u64)y * g_sw + g_px, g_spl_static + (u64)y * g_sw + g_px, (usize)g_pw * 4);

    int done = g_disp_q >= 10000;
    u32 led = g_spl_failed ? 0xFF4A3A : done ? 0x3DFF8A : 0xFFB23A;
    int on = g_spl_failed || done || ((ms / 360) & 1);
    int lx = g_px + 26, ly = g_py + 27;
    draw_disk(b, lx, ly, 7 * 16, 0x050608, 256);
    if (on) draw_disk(b, lx, ly, 12 * 16, led, 40);
    draw_disk(b, lx, ly, 5 * 16, on ? led : cscale(led, 70), 256);
    draw_disk(b, lx - 2, ly - 2, 2 * 16, 0xFFFFFF, on ? 150 : 40);

    draw_text_sh(b, g_px + 42, g_py + 20, 2, 0, g_stage, g_spl_failed ? 0xFF8A7A : 0xD6DEE6);

    /* 七段百分比 */
    u32 pct = g_disp_q / 100;
    u32 lit = g_spl_failed ? 0xFF5544 : 0x46E6FF;
    u32 dig[3] = { pct / 100, (pct / 10) % 10, pct % 10 };
    for (int i = 0; i < 3; i++) {
        int x = g_dx + i * (DIGIT_W + DIGIT_GAP);
        seg7(b, x, g_dy, 0x7F, 0x111C24);
        int show = (i == 2) || (i == 1 && pct >= 10) || (i == 0 && pct >= 100);
        if (show) seg7(b, x, g_dy, k_seg7[dig[i]], lit);
    }
    draw_text(b, g_dx + 3 * DIGIT_W + 2 * DIGIT_GAP + 6, g_dy + DIGIT_H - 21, 3, 0, "%", lit, lit);

    /* 40 段进度条 */
    int ix = g_tx + 4, iw = g_tw - 8, sy = g_ty + 5, shh = g_th - 9;
    int fill256 = (int)((u64)g_disp_q * 40u * 256u / 10000u);
    int litn = fill256 / 256;
    int sweep = (int)((ms % 1700) * (u64)(litn + 10) / 1700) - 5;
    for (int i = 0; i < 40; i++) {
        int x0 = ix + i * iw / 40, x1 = ix + (i + 1) * iw / 40 - 2;
        int f = fill256 - i * 256;
        f = f < 0 ? 0 : f > 256 ? 256 : f;
        u32 lc = g_spl_failed ? 0xE0483C : cmix(0x1C8CFF, 0x35F2D2, i * 256 / 39);
        if (!g_spl_failed && f) {
            int dd = iabs(i - sweep);
            if (dd < 4) lc = cadd(lc, (4 - dd) * 18 * f / 256);
        }
        u32 c = cmix(0x15191E, lc, f);
        buf_fill(b, x0, sy, x1 - x0, shh, c);
        buf_fill(b, x0, sy, x1 - x0, shh * 2 / 5, cadd(c, f ? 26 * f / 256 + 4 : 6));
        buf_fill(b, x0, sy + shh - 1, x1 - x0, 1, cscale(c, 170));
    }
    if (litn < 40 && fill256 > 0 && !g_spl_failed) {
        int hx = ix + (int)((u64)fill256 * (u32)iw / (40u * 256u));
        buf_blend(b, hx - 6, sy - 3, 12, shh + 6, 0x9AF8FF, 60);
    }

    /* 读数：文件数 + 字节数 */
    char l[48], r[48], *p;
    str_copy(l, "FILES ", sizeof(l));
    p = l + str_len(l);
    p = fmt_u(p, g_files_done); *p++ = '/'; fmt_u(p, g_files_total);
    p = fmt_mb(r, g_bytes_loaded); *p++ = ' '; *p++ = '/'; *p++ = ' ';
    p = fmt_mb(p, g_bytes_total); str_copy(p, " MB", 4);
    int ry = g_py + g_ph - 28;
    draw_text_sh(b, g_px + 22, ry, 2, 0, l, 0x8A96A2);
    draw_text_sh(b, g_px + g_pw - 22 - text_w(r, 2, 0), ry, 2, 0, r, 0x8A96A2);
}

/* ---------------- 输出到 GOP ---------------- */
static void spl_blit(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_sw) x1 = g_sw;
    if (y1 > g_sh) y1 = g_sh;
    int direct = g_fb_rsh == 16 && g_fb_gsh == 8 && g_fb_bsh == 0;
    for (int y = y0; y < y1; y++) {
        u32 *dst = (u32 *)(g_fb + (u64)y * g_fb_pitch);
        const u32 *src = g_spl_back + (u64)y * g_sw;
        if (direct) {
            for (int x = x0; x < x1; x++) dst[x] = src[x];
        } else {
            for (int x = x0; x < x1; x++) {
                u32 c = src[x];
                dst[x] = (((c >> 16) & 0xFF) << g_fb_rsh) | (((c >> 8) & 0xFF) << g_fb_gsh) | ((c & 0xFF) << g_fb_bsh);
            }
        }
    }
}

static void spl_update_disp(u64 ms) {
    u64 dt = ms - g_last_ms;
    g_last_ms = ms;
    u32 target = g_phase_q;
    if (g_bytes_total) {
        u32 bq = (u32)(g_bytes_loaded * 9000u / g_bytes_total);
        if (bq > target) target = bq;
    }
    if (target > 10000) target = 10000;
    if (g_disp_q >= target) return;
    if (dt > 250) dt = 250;
    u32 diff = target - g_disp_q;
    u32 step = (u32)((u64)diff * dt / 180u);
    if (step < 12) step = 12;
    if (step > diff) step = diff;
    g_disp_q += step;
}

static u64 g_frame_cost;          /* 最近一帧耗时（TSC） */

/* 动画只占装载时间的约 1/4：间隔 = max(33ms, 3 × 本帧耗时) */
static void spl_frame(void) {
    if (!g_spl_on) return;
    u64 t0 = spl_rdtsc();
    u64 ms = spl_ms();
    spl_update_disp(ms);
    spl_draw_gears(ms);
    spl_draw_panel(ms);
    spl_blit(g_gx0, g_gy0, g_gx1, g_gy1);
    spl_blit(g_px, g_py, g_px + g_pw, g_py + g_ph);
    u64 t1 = spl_rdtsc();
    if (!g_frame_cost) log_hex2("[boot] splash frame us=", (t1 - t0) * 1000u / g_tsc_ms);
    g_frame_cost = t1 - t0;
    u64 gap = g_tsc_ms * 33u;
    if (g_frame_cost * 3u > gap) gap = g_frame_cost * 3u;
    g_next_frame = t1 + gap;
}

/* fs 读取每个分块后调用：最多约 30 帧/秒，不额外 Stall */
static void anim_pump(void) {
    if (!g_spl_on) return;
    if (spl_rdtsc() < g_next_frame) return;
    spl_frame();
}

static void splash_stage(const char *label) {
    g_stage = label;
    if (g_spl_on) spl_frame();
}

static void splash_phase(u32 q, const char *label) {
    if (q > g_phase_q) g_phase_q = q;
    splash_stage(label);
}

static void splash_begin(void) {
    if (!g_fb || !g_fb_w || !g_fb_h) return;
    g_sw = (int)g_fb_w;
    g_sh = (int)g_fb_h;
    usize bytes = (usize)g_sw * (usize)g_sh * 4u;
    g_spl_static = (u32 *)alloc_pages(bytes);
    g_spl_back = (u32 *)alloc_pages(bytes);
    if (!g_spl_static || !g_spl_back) {
        log2("[boot] WARN: splash buffers unavailable (no boot animation)", 0);
        return;
    }
    sin_init();
    g_tsc_ms = spl_tsc_calibrate();
    if (!g_tsc_ms) g_tsc_ms = 1;
    g_tsc0 = spl_rdtsc();
    log_hex2("[boot] splash tsc/ms=", g_tsc_ms);

    spl_layout();
    spl_build_static();
    gear_bake();
    memcpy(g_spl_back, g_spl_static, bytes);
    g_spl_on = 1;
    spl_draw_gears(0);
    spl_draw_panel(0);
    spl_blit(0, 0, g_sw, g_sh);
    g_next_frame = spl_rdtsc() + g_tsc_ms * 33u;
}

#include "init_banner.h"

static void spl_put(int x, int y, u32 rgb) {
    if (!g_fb || x < 0 || y < 0 || (u64)x >= g_fb_w || (u64)y >= g_fb_h) return;
    u32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    ((u32 *)(g_fb + (u64)y * g_fb_pitch))[x] = (r << g_fb_rsh) | (g << g_fb_gsh) | (b << g_fb_bsh);
}

/* 与内核 boot_status 同一套字和同一圈点，交接时画面不跳。 */
static void spl_status(void) {
    static const signed char cs[12] = { 16, 14, 8, 0, -8, -14, -16, -14, -8, 0, 8, 14 };
    static const signed char sn[12] = { 0, 8, 14, 16, 14, 8, 0, -8, -14, -16, -14, -8 };
    static const u8 br[12] = { 255, 210, 160, 110, 70, 44, 30, 24, 20, 18, 16, 14 };
    int x0 = (int)g_fb_w / 2 - INIT_BANNER_W / 2;
    int y0 = (int)g_fb_h / 2 - (INIT_BANNER_H + 56) / 2;
    if (y0 < 0) y0 = 0;
    for (int y = 0; y < INIT_BANNER_H; y++) {
        const unsigned char *row = g_init_banner_a + (u32)y * INIT_BANNER_W;
        for (int x = 0; x < INIT_BANNER_W; x++) {
            unsigned char a = row[x];
            if (!a) continue;
            u32 r = 2u + 253u * a / 255u;
            u32 g = 255u * a / 255u;
            u32 b = 47u + 208u * a / 255u;
            spl_put(x0 + x, y0 + y, (r << 16) | (g << 8) | b);
        }
    }
    int sx = (int)g_fb_w / 2, sy = y0 + INIT_BANNER_H + 36;
    for (int age = 11; age >= 0; age--) {
        int i = (12 - age) % 12;
        int cx = sx + cs[i], cy = sy + sn[i];
        u32 col = ((2u + 253u * br[age] / 255u) << 16) | ((255u * br[age] / 255u) << 8) | (47u + 208u * br[age] / 255u);
        for (int dy = -3; dy <= 3; dy++)
            for (int dx = -3; dx <= 3; dx++)
                if (dx * dx + dy * dy <= 10) spl_put(cx + dx, cy + dy, col);
    }
}

static void splash_finish(void) {
    if (!g_spl_on) return;
    splash_phase(10000, "READY");
    while (g_disp_q < 10000 || spl_ms() < 1400) {
        spl_wait_ms(16);
        spl_frame();
    }
    for (int i = 0; i < 12; i++) { spl_wait_ms(16); spl_frame(); }

    /* 整屏渐隐到会话 Logo 底色 #02002F */
    int direct = g_fb_rsh == 16 && g_fb_gsh == 8 && g_fb_bsh == 0;
    for (int i = 1; i <= 16; i++) {
        int t = (int)smooth256((u32)i * 256u / 16u);
        for (int y = 0; y < g_sh; y++) {
            u32 *dst = (u32 *)(g_fb + (u64)y * g_fb_pitch);
            const u32 *src = g_spl_back + (u64)y * g_sw;
            for (int x = 0; x < g_sw; x++) {
                u32 c = cmix(src[x], 0x02002F, t);
                dst[x] = direct ? c : (((c >> 16) & 0xFF) << g_fb_rsh) | (((c >> 8) & 0xFF) << g_fb_gsh) | ((c & 0xFF) << g_fb_bsh);
            }
        }
        spl_wait_ms(14);
    }
    /* 深蓝屏正中：提示文字 + 转圈。内核起来后接着转。 */
    spl_status();
    g_spl_on = 0;
}

static void splash_fail(void) {
    if (!g_spl_on) return;
    g_spl_failed = 1;
    g_stage = "BOOT FAILURE";
    spl_frame();
}
