/* session.drv — 会话排序器（P5 万物互联：DSK 退役后的编排者）
 *
 * 母本 CODE/dsk/main.c 的 dsk_spinner_and_check / dsk_check_firstinit /
 * dsk_run_login / dsk_run_desktop / dsk_schedule_netman / milestone_write。
 *
 * DSK 退役后，UTSM 加载完全部 manifest 驱动（stage0-4，含本模块）后调用
 * srv.session.run；本模块经服务总线依次拉起：
 *   netman → (firstInit 向导 | 跳过) → (login | 跳过) → desktop / cmd
 * 所有跨模块调用都走 dkm_bus（srv.*），配置读走 kern.config.get，
 * 文件读写走 srv.fs.fat32.*，不再依赖任何 dsk_boot_context/reserved[]。
 *
 * 与母本的差异（登记于 STATUS）：
 *   - 启动 splash 只显示 machmagic（TEXTURES/panic/machmagic.png 嵌入）。
 *   - 会话选择：FUCK session=native/cmd 直选；session=ask / kde 暂回落 native
 *     （KDE guest 会话路径依赖 Linux guest + IPC，留待后续）。
 */

#include <stdint.h>
#include "../dkm_shared.h"

/* ---- 服务签名（经总线取） ---- */
typedef int  (*run_fn)(void);
typedef int  (*wr_root_fn)(const char *, const uint8_t *, uint32_t);
typedef int  (*rd_path_fn)(const char *, uint8_t **, uint32_t *);
typedef const char *(*cfg_get_fn)(const char *, const char *);
typedef const int   *(*auth_get_fn)(void);

static const struct dkm_log_api *g_log;
static const struct dkm_kernel_api *g_api;
static uint64_t g_fb_addr, g_fb_w, g_fb_h, g_fb_p;
static uint64_t g_tsc_per_ms;

#define COM1 0x3F8
static void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }

/* ---------------------------------------------------------------
 * 服务获取（缺席返回 0）
 * --------------------------------------------------------------- */
static void *svc(const char *name) {
    void *fn = 0;
    if (!g_api || !g_api->bus) return 0;
    if (g_api->bus->probe(name, &fn, 0) != 0) return 0;
    return fn;
}

/* FUCK 配置读：kern.config.get(section,key) 返回字符串或 NULL */
static const char *cfg_str(const char *sec, const char *key) {
    cfg_get_fn get = (cfg_get_fn)svc("kern.config.get");
    return get ? get(sec, key) : 0;
}
static int cfg_bool(const char *sec, const char *key, int def) {
    const char *v = cfg_str(sec, key);
    if (!v) return def;
    return (v[0] == '1') ? 1 : 0;
}

/* 里程碑：写 BOOT0N.TXT 到 FAT32 根（经 srv.fs.fat32.write_root_file） */
static void milestone(int num, const char *desc) {
    if (num < 1 || num > 9) return;
    wr_root_fn wr = (wr_root_fn)svc("srv.fs.fat32.write_root_file");
    if (!wr) return;
    char name11[11];
    name11[0]='B'; name11[1]='O'; name11[2]='O'; name11[3]='T';
    name11[4]=(char)('0' + num / 10); name11[5]=(char)('0' + num % 10);
    name11[6]=' '; name11[7]=' '; name11[8]='T'; name11[9]='X'; name11[10]='T';
    uint8_t buf[160]; uint32_t len = 0;
    buf[len++]='O'; buf[len++]='K'; buf[len++]='\n';
    while (*desc && len < 150) buf[len++] = (uint8_t)*desc++;
    buf[len++] = '\n';
    if (wr(name11, buf, len) == 0) {
        g_log->info("[session] milestone BOOT_0");
        char d[2]; d[0] = (char)('0' + num); d[1] = 0;
        g_log->info(d);
    }
}

/* framebuffer：开机只画 machmagic。
 * Logo 自带一圈中心偏亮的径向光晕（中心约 #07014F，边缘约 #030132），
 * 底色若是纯色，方形边缘会露出接缝。这里把同一条光晕曲线画满屏幕，
 * Logo 四边再羽化，两者在边界处颜色一致。每帧每个像素只写一次，
 * 不先清屏再画，避免中途露出底色造成闪烁。 */
#include "machmagic_logo_data.c"

#define SPLASH_BASE_R 2u
#define SPLASH_BASE_G 0u
#define SPLASH_BASE_B 47u

static uint16_t g_glow_f[256];   /* 光晕强度 f(t^2)，Q8；t = 距中心 / Logo 半边长 */

static uint32_t isqrt_u64(uint64_t v) {
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* 下标 i 对应 t^2 = i * 2.1 / 256；f = (1 - t / 1.45)^2，与 Logo 实测光晕吻合 */
static void glow_init(void) {
    for (int i = 0; i < 256; i++) {
        uint64_t t2_q16 = (uint64_t)i * 137626u / 256u;
        uint32_t t_q8 = isqrt_u64(t2_q16);
        int32_t k = 256 - (int32_t)(t_q8 * 256u / 371u);
        if (k < 0) k = 0;
        g_glow_f[i] = (uint16_t)((uint32_t)(k * k) / 256u);
    }
}

static void fb_fill_solid(uint32_t c) {
    if (!g_fb_addr) return;
    for (uint64_t y = 0; y < g_fb_h; y++) {
        uint32_t *line = (uint32_t *)((uint8_t *)(uint64_t)g_fb_addr + y * g_fb_p);
        for (uint64_t x = 0; x < g_fb_w; x++) line[x] = c;
    }
}

static inline uint32_t smooth256(uint32_t t) {
    if (t >= 256) return 256;
    return t * t * (768u - 2u * t) / 65536u;
}

/* 一帧：光晕底 + 双线性缩放的 Logo。alpha/glow 为 0..256，size = Logo 显示边长。 */
static void splash_frame(const uint8_t *pix, int w, int h, uint32_t alpha, uint32_t glow, int size) {
    if (!g_fb_addr || w <= 1 || h <= 1 || size < 8) return;
    int64_t W = (int64_t)g_fb_w, H = (int64_t)g_fb_h;
    int64_t cx = W / 2, cy = H / 2;
    int64_t R = size / 2;
    int64_t reach = R * 29 / 20 + 2;
    int64_t x0 = cx - reach, x1 = cx + reach, y0 = cy - reach, y1 = cy + reach;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    int dw = size, dh = (int)((int64_t)size * h / w);
    int64_t lx0 = cx - dw / 2, ly0 = cy - dh / 2;
    int feather = dw / 9;
    if (feather < 1) feather = 1;
    uint64_t den = (uint64_t)R * (uint64_t)R * 21u;
    if (!den) den = 1;

    for (int64_t y = y0; y < y1; y++) {
        uint32_t *line = (uint32_t *)((uint8_t *)(uint64_t)g_fb_addr + (uint64_t)y * g_fb_p);
        int64_t dy = y - cy;
        int ly = (int)(y - ly0);
        int inrow = ly >= 0 && ly < dh;
        int sy0 = 0, sy1 = 0;
        uint32_t fy = 0;
        int ey = 0;
        if (inrow) {
            uint32_t syq = (uint32_t)((uint64_t)ly * (uint64_t)(h - 1) * 256u / (uint64_t)(dh > 1 ? dh - 1 : 1));
            sy0 = (int)(syq >> 8);
            fy = syq & 255u;
            sy1 = sy0 + 1 < h ? sy0 + 1 : sy0;
            ey = ly < dh - 1 - ly ? ly : dh - 1 - ly;
        }
        const uint8_t *row0 = pix + (uint64_t)sy0 * (uint64_t)w * 4u;
        const uint8_t *row1 = pix + (uint64_t)sy1 * (uint64_t)w * 4u;
        for (int64_t x = x0; x < x1; x++) {
            int64_t dx = x - cx;
            uint64_t d2 = (uint64_t)(dx * dx + dy * dy);
            uint64_t idx = d2 * 2560u / den;
            uint32_t f = idx < 256 ? (uint32_t)g_glow_f[idx] * glow / 256u : 0;
            uint32_t r = SPLASH_BASE_R + 5u * f / 256u;
            uint32_t g = SPLASH_BASE_G + 1u * f / 256u;
            uint32_t b = SPLASH_BASE_B + 33u * f / 256u;
            int lx = (int)(x - lx0);
            if (inrow && alpha && lx >= 0 && lx < dw) {
                uint32_t sxq = (uint32_t)((uint64_t)lx * (uint64_t)(w - 1) * 256u / (uint64_t)(dw > 1 ? dw - 1 : 1));
                int sx0 = (int)(sxq >> 8);
                uint32_t fx = sxq & 255u;
                int sx1 = sx0 + 1 < w ? sx0 + 1 : sx0;
                const uint8_t *p00 = row0 + sx0 * 4, *p01 = row0 + sx1 * 4;
                const uint8_t *p10 = row1 + sx0 * 4, *p11 = row1 + sx1 * 4;
                uint32_t c[4];
                for (int k = 0; k < 4; k++) {
                    uint32_t top = p00[k] * (256u - fx) + p01[k] * fx;
                    uint32_t bot = p10[k] * (256u - fx) + p11[k] * fx;
                    c[k] = (top * (256u - fy) + bot * fy) >> 16;
                }
                int ex = lx < dw - 1 - lx ? lx : dw - 1 - lx;
                int e = ex < ey ? ex : ey;
                uint32_t m = e >= feather ? 256u : smooth256((uint32_t)e * 256u / (uint32_t)feather);
                uint32_t a = c[3] * alpha / 256u * m / 256u;
                if (a > 255) a = 255;
                uint32_t na = 256u - a;
                r = (r * na + c[0] * a) >> 8;
                g = (g * na + c[1] * a) >> 8;
                b = (b * na + c[2] * a) >> 8;
            }
            line[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

static void delay_ms(uint32_t ms);   /* 前向声明（定义在下方） */
static void wait_until(uint64_t t) { while (rdtsc() < t) __asm__ volatile("pause"); }

static void splash_image(const uint8_t *pix, int w, int h, uint32_t hold_ms, const char *name) {
    int size = (int)(g_fb_h * 78u / 100u);
    if (size > w) size = w;
    if ((uint64_t)size > g_fb_w * 9u / 10u) size = (int)(g_fb_w * 9u / 10u);
    uint64_t frame = g_tsc_per_ms ? g_tsc_per_ms * 30u : 0;
    glow_init();
    fb_fill_solid(0xFF000000u | (SPLASH_BASE_R << 16) | (SPLASH_BASE_G << 8) | SPLASH_BASE_B);

    /* 光晕与 Logo 一起从底色里浮出来，尺寸 0.94 → 1.0 */
    for (int i = 1; i <= 24; i++) {
        uint64_t next = rdtsc() + frame;
        uint32_t t = smooth256((uint32_t)i * 256u / 24u);
        splash_frame(pix, w, h, t, t, size * (240 + (int)(16u * t / 256u)) / 256);
        if (frame) wait_until(next); else delay_ms(30);
    }
    g_log->info("[session] splash: showing");
    g_log->info(name);
    if (hold_ms > 1200) delay_ms(hold_ms - 1200);
    else if (hold_ms) delay_ms(hold_ms);
    /* 交给桌面之前收回同一底色，桌面再从这里交叉淡入 */
    for (int i = 1; i <= 18; i++) {
        uint64_t next = rdtsc() + frame;
        uint32_t t = 256u - smooth256((uint32_t)i * 256u / 18u);
        splash_frame(pix, w, h, t, t, size * (256 + (int)(8u * (256u - t) / 256u)) / 256);
        if (frame) wait_until(next); else delay_ms(30);
    }
}

static void delay_ms(uint32_t ms) {
    if (!g_tsc_per_ms) { for (volatile uint32_t i = 0; i < 100000 * ms; i++) __asm__("pause"); return; }
    uint64_t t = g_tsc_per_ms * ms, s = rdtsc();
    while (rdtsc() - s < t) __asm__("pause");
}

static void tsc_cal(void) {
    outb(0x43, 0x30); outb(0x40, 0x7c); outb(0x40, 0x2e);
    uint64_t s = rdtsc(); uint16_t prev = 0; uint64_t loops = 0;
    for (;;) { outb(0x43, 0x00); uint16_t c = (uint16_t)inb(0x40) | ((uint16_t)inb(0x40) << 8);
        if (c > prev && loops > 10) break; prev = c; loops++; }
    g_tsc_per_ms = (rdtsc() - s) / 10;
}

/* ---------------------------------------------------------------
 * firstInit.txt 检查：返回 1 = 首次启动；dev_mode 由第二行决定
 * --------------------------------------------------------------- */
static int check_firstinit(int *is_first, int *dev_mode_out) {
    *dev_mode_out = 0; *is_first = 1;
    rd_path_fn rd = (rd_path_fn)svc("srv.fs.fat32.read_path");
    uint8_t *data = 0; uint32_t size = 0;
    if (!rd) { g_log->warn("[session] no fs service; assume first boot"); return 0; }
    if (rd("firstInit.txt", &data, &size) != 0 &&
        rd("FIRSTINTXT", &data, &size) != 0 &&
        rd("system/user/use/firstInit.txt", &data, &size) != 0 &&
        rd("user/use/firstInit.txt", &data, &size) != 0) {
        g_log->info("[session] firstInit.txt not found, assuming first boot");
        return 0;
    }
    if (!data || size == 0) return 0;
    if (data[0] == '0') { g_log->info("[session] firstInit=0 (first boot)"); *is_first = 1; }
    else { g_log->info("[session] firstInit!=0"); *is_first = 0; }

    /* 出厂镜像/全新 U 盘：firstInit.txt 可能已存在但 USER.CONF 尚未生成。
     * 此时必须回落到向导，否则 login 会 LOGIN-E03 直接 panic。 */
    if (!*is_first) {
        uint8_t *uc = 0; uint32_t ucs = 0;
        if (rd("USER    CON", &uc, &ucs) != 0 || !uc || ucs == 0) {
            g_log->info("[session] USER.CONF missing -> first boot");
            *is_first = 1;
        }
    }
    uint32_t i = 0;
    while (i < size && data[i] != '\n') i++;
    if (i < size) { i++; if (i < size && data[i] == '1') { *dev_mode_out = 1; g_log->info("[session] dev_mode=1"); } }
    return 0;
}

/* 拉起 srv.<name>（缺席返回 0） */
static int run_svc(const char *name) {
    run_fn f = (run_fn)svc(name);
    if (!f) return 0;
    g_log->info("[session] invoking ");
    g_log->info(name);
    return f();
}

/* ---------------------------------------------------------------
 * 会话编排
 * --------------------------------------------------------------- */
static int session_run(void) {
    __asm__ volatile("cli");
    g_log->info("[session] run");

    /* P6b 真 IRQ 输入：IRQ1/IRQ12 解屏蔽（kern.irq.unmask）后 IF=1，
     * 各 app 模块经 srv.input.kbd.poll / srv.input.mouse.poll 消费事件。
     * apic_route=1 时 PIC IMR 已由 IOAPIC 接管，legacy 线在 apic_route_legacy
     * 下已放开；此调用在 PIC 路径上同样成立（双路径同一入口）。 */
    {
        void *fn = 0;
        if (!g_api || !g_api->bus ||
            g_api->bus->probe("kern.irq.unmask", &fn, 0) != 0 || !fn) {
            g_log->error("[session] kern.irq.unmask missing");
            g_log->panic("INPUT-E03 IRQ UNMASK SERVICE MISSING");
            return 0;   /* fallback：无真 IRQ，交由上层按会话失败处理 */
        }
        void (*unmask)(uint8_t) = (void (*)(uint8_t))fn;
        unmask(1);
        unmask(12);
        g_log->info("[session] IRQ1/IRQ12 unmasked via kern.irq.unmask");
        __asm__ volatile("sti" ::: "memory");
        g_log->info("[session] IF=1 (true IRQ input armed)");
    }

    g_fb_addr = (uint64_t)g_api->fb_address;
    g_fb_w = g_api->fb_width; g_fb_h = g_api->fb_height; g_fb_p = g_api->fb_pitch;
    tsc_cal();

    /* FUCK 配置（kern.config.get） */
    int show_logo      = cfg_bool("dsk", "show_logo", 1);
    int skip_login     = cfg_bool("dsk", "skip_login", 0);
    int skip_firstinit = cfg_bool("dsk", "skip_firstinit", 0);
    int load_netman    = cfg_bool("dsk", "netman", 1);
    int dev_mode       = cfg_bool("boot", "dev_mode", 0);
    const char *def_shell = cfg_str("dsk", "default_shell");
    const char *session_sel = cfg_str("dsk", "session");

    milestone(2, "block API ready");
    milestone(3, "FUCK config loaded");

    /* 开机画面只显示 machmagic */
    if (show_logo && g_fb_addr) {
        splash_image(g_mm_logo_rgba, g_mm_logo_w, g_mm_logo_h, 3000, "machmagic.png");
        g_log->info("[session] splash sequence done");
    } else {
        g_log->info("[session] logo skipped by FUCK config");
    }

    int is_first = 0, fi_dev_mode = 0;
    check_firstinit(&is_first, &fi_dev_mode);
    if (fi_dev_mode) dev_mode = 1;
    milestone(4, "firstInit check done");

    /* 首次启动且未跳过 → 向导（firstinit.drv 自写 user.conf + flip firstInit.txt） */
    if (is_first && !skip_firstinit) {
        if (!svc("srv.firstinit.run")) {
            g_log->panic("SESSION-E01 FIRSTINIT SERVICE MISSING");
            return 0;
        }
        run_svc("srv.firstinit.run");
        milestone(5, "firstinit done");
    } else {
        milestone(5, "firstinit skipped");
    }

    /* netman（每次启动都拉起，进桌面前拿到 IP） */
    if (load_netman) run_svc("srv.netman.run");
    milestone(7, "netman done");

    /* dev_mode：跑 shell 自动测试命令序列（母本 DSK dev_mode 行为） */
    if (dev_mode) {
        g_log->info("[session] dev_mode -> shell auto-test");
        run_svc("srv.shell.run");
    }

    /* 登录：首次启动向导后强制重登；常规启动按 skip_login */
    if (!skip_login && !skip_firstinit) {
        if (!svc("srv.login.run")) {
            g_log->panic("SESSION-E02 LOGIN SERVICE MISSING");
            return 0;
        }
        run_svc("srv.login.run");
        int ok = 0;
        auth_get_fn aget = (auth_get_fn)svc("srv.auth.result");
        if (aget) { const int *r = aget(); if (r && *r == 1) ok = 1; }
        if (!ok) {
            g_log->panic("SESSION-E03 LOGIN REQUIRED");
            return 0;
        }
        g_log->info("[session] login success");
    } else {
        g_log->info("[session] login skipped");
    }

    /* 会话选择：FUCK default_shell=cmd → cmd；否则桌面 */
    if (def_shell && def_shell[0] == 'c' && def_shell[1] == 'm' && def_shell[2] == 'd') {
        milestone(6, "cmd loading");
        if (!svc("srv.cmd.run")) { g_log->panic("SESSION-E04 CMD SERVICE MISSING"); return 0; }
        g_log->info("[session] session=cmd");
        run_svc("srv.cmd.run");
    }

    milestone(6, "desktop loading");
    (void)session_sel;   /* native/kde：kde 暂回落 native（P5 简化） */
    if (!svc("srv.desktop.run")) { g_log->panic("SESSION-E05 DESKTOP SERVICE MISSING"); return 0; }
    g_log->info("[session] session=native");

    /* P6b：桌面任务化——先让 desktop.drv 把自己变成调度器任务，
     * session 随即返回，bootstrap 转 idle（内核 main 负责 idle 循环）。
     * 调度器不可用（spawn 返回 0）时回退到直接执行（永不返回）。 */
    if (svc("srv.desktop.spawn") && run_svc("srv.desktop.spawn") == 1) {
        g_log->info("[session] desktop spawned as task");
        return 1;
    }
    g_log->warn("[session] desktop spawn unavailable; running inline");
    run_svc("srv.desktop.run");   /* 永不返回 */
    return 0;
}

/* ---------------------------------------------------------------
 * 驱动描述符 + 入口
 * --------------------------------------------------------------- */
static const char *const g_provides[] = { "session" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic = DKM_DRIVER_MAGIC, .abi_version = DKM_ABI_VERSION,
    .desc_size = sizeof(struct dkm_driver_desc),
    .name = "session", .version = "0.1.0", .vendor = "Deshab",
    .driver_class = DKM_CLASS_MISC, .stage = 4,
    .flags = DKM_F_NO_UNLOAD, .priority = 0,
    .depends = NULL, .depends_count = 0,
    .provides = g_provides, .provides_count = 1,
    .min_kernel_abi = 1,
};

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api, struct dkm_driver_handle *handle) {
    (void)handle;
    if (!api || !api->log) return -1;
    g_log = api->log;
    g_api = api;
    if (api->bus) {
        api->bus->register_service("srv.session.run", 1, (void *)session_run);
        g_log->info("[session] srv.session.run registered");
    }
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle; return 0;
}
