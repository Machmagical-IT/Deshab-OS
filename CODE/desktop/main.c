/* main.c — Deshab Desktop Manager 组合根（M0 模块化拆分后）
 *
 * 原 4610 行单文件已拆分为 13 个模块（见 desktop.h 头注释）。
 * 本文件保留：dsk_entry 入口、服务指针探测（lxc/pe/block）、boot 屏、
 * 主事件循环。功能与拆分前完全一致（零功能变更）。
 *
 * 原 "Winux-Kate 结构完整复刻" 布局说明见 pages.c 头注释；
 * M2-M4 将按 Win11 Fluent 桌面重构计划逐页替换。
 */
#include "ascii_font.h"  /* 顺序敏感：先于 desktop.h（g_ascii 导出） */
#include "../DKM/dkm_shared.h"  /* P4e: DKM 驱动 ABI（driver_init/desc/kernel_api/bus） */
#include "desktop.h"
#include "zhfont.h"       /* M1: 中文位图字库（dbf 加载 + 中英混排渲染） */
#include "wallpaper.h"   /* M2: 壁纸背景层缓存 */
#include "textures.h"    /* M2: 启动背景/开机 logo/开始菜单 logo 纹理 */
#include "glass.h"       /* 壁纸毛玻璃缓存 */
#include "aqua_theme.h"  /* 顶栏高度（拖拽上界） */

/* ---- 全局状态（渲染上下文 / 服务指针，各模块 extern） ---- */
du_context g_fb;
u64 g_fb_addr, g_fb_w, g_fb_h, g_fb_pitch;
const void *g_kernel_api;
u8 *g_real_fb = 0;                     /* 真实 framebuffer 地址 */
desktop_block_read_fn  g_block_read;
desktop_block_write_fn g_block_write;
const dsk_boot_context *g_boot_ctx = 0;

/* ---- 实机多盘：fat32_io.h 把块设备索引写死为 0 ----
 * 实机上块设备 0 常是内置 NVMe/SATA 而非启动 U 盘；fat32.drv 经跨设备
 * 探测后把实际索引公布在 srv.fs.fat32.volume_info。这里包一层把 index
 * 重定向过去，否则按路径读取（字库/壁纸）全部失败 → DESK-E04。 */
static desktop_block_read_fn  g_block_read_raw;
static desktop_block_write_fn g_block_write_raw;
static u32 g_fat_dev;

static int dt_block_read(u32 index, u64 lba, u32 count, void *buffer) {
    (void)index;
    return g_block_read_raw ? g_block_read_raw(g_fat_dev, lba, count, buffer) : -1;
}
static int dt_block_write(u32 index, u64 lba, u32 count, const void *buffer) {
    (void)index;
    return g_block_write_raw ? g_block_write_raw(g_fat_dev, lba, count, buffer) : -1;
}

/* ---- 引导器预载的桌面资产（boot module cmdline "asset:*"） ----
 * 实机上块设备 0 常是内置盘，且部分 USB 存储在长传输时不稳定；字库/
 * 壁纸/贴图由引导器直接送入内存，按路径读取时优先命中预载数据。 */
typedef struct {
    u64 revision;
    u64 module_count;
    void **modules;
} dt_mod_rsp;
typedef struct {
    u64 revision;
    void *address;
    u64 size;
    char *path;
    char *cmdline;
    u32 media_type;
    u32 unused;
    u32 tftp_ip;
    u32 tftp_port;
    u32 partition_index;
    u32 mbr_disk_id;
    u8  gpt_disk_uuid[16];
    u8  gpt_part_uuid[16];
    u8  part_uuid[16];
} dt_mod_file;

static int dt_str_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int dt_str_ends(const char *s, const char *suf) {
    u64 ls = 0, lf = 0;
    while (s && s[ls]) ls++;
    while (suf && suf[lf]) lf++;
    if (lf > ls) return 0;
    return dt_str_eq(s + (ls - lf), suf);
}

static const void *dt_boot_asset(const char *tag, u32 *out_size) {
    const struct dkm_kernel_api *api = (const struct dkm_kernel_api *)g_kernel_api;
    if (!api) return 0;
    const dt_mod_rsp *rsp = (const dt_mod_rsp *)api->boot_modules_response;
    if (!rsp || !rsp->modules) return 0;
    for (u64 i = 0; i < rsp->module_count; i++) {
        const dt_mod_file *f = (const dt_mod_file *)rsp->modules[i];
        if (!f || !f->cmdline || !f->address) continue;
        if (dt_str_eq(f->cmdline, tag)) {
            if (out_size) *out_size = (u32)f->size;
            return f->address;
        }
    }
    return 0;
}

/* 按路径读取：命中预载资产则内存拷贝，否则回落到 FAT32 服务。 */
static int dt_read_to(const char *path, u8 *dst, u32 cap, u32 *out_size) {
    const void *src = 0;
    u32 sz = 0;
    if (path) {
        if (dt_str_eq(path, "system/font/simhei_16.dbf")) src = dt_boot_asset("asset:font16", &sz);
        else if (dt_str_eq(path, "system/font/simhei_24.dbf")) src = dt_boot_asset("asset:font24", &sz);
        else if (dt_str_ends(path, "wallpaper.rgba")) src = dt_boot_asset("asset:wallpaper", &sz);
        else if (dt_str_ends(path, "back.rgba")) src = dt_boot_asset("asset:backrgba", &sz);
        else if (dt_str_ends(path, "bootlogo.rgba")) src = dt_boot_asset("asset:bootlogo", &sz);
        else if (dt_str_ends(path, "startMenuLogo.rgba")) src = dt_boot_asset("asset:smlogo", &sz);
    }
    if (src) {
        if (sz > cap) return -4;
        const u8 *s = (const u8 *)src;
        for (u32 i = 0; i < sz; i++) dst[i] = s[i];
        if (out_size) *out_size = sz;
        return 0;
    }
    return f32_read_path_lfn_to_w(path, dst, cap, out_size);
}

/* 桌面外壳开关（FUCK [desktop] fluent=，缺省 1）。
 * 1 = macOS Aqua 外壳（壁纸 + 顶栏 + Dock + Launchpad）；0 = 旧 KATE 桌面。 */
int g_fluent = 1;
int g_boot_reveal = 256;
static int g_motion_selftest = 0;

/* VSCode Phase 2: Linux guest 输入转发。
 * g_lxc_svc 由 dsk_entry() 从 boot context reserved[5] 读取；
 * g_input_forward_enabled 在 IDE 标签 attach/detach 时联动置位，
 * 开启后 PS/2 鼠标键盘事件经 virtio-input 注入 Linux guest（VSCode 运行其中）。 */
const linux_compat_service *g_lxc_svc = 0;
int g_input_forward_enabled = 0;

/* P5d: PE 兼容层服务指针（dsk_loader 在 reserved[4] 写入，magic 校验）。 */
const pe_service *g_pe_svc = 0;

static int g_booted = 0;
static int g_serial_test = 0;

/* ============================================================
 *  M2: FUCK 配置解析（[desktop] fluent=）
 *  复用 dsk 的 FAT32 路径读取（f32_read_path_lfn_w），INI 最小解析。
 * ============================================================ */

static int cfg_key(const char *line, u32 len, const char *key,
                   const char **val, u32 *vlen) {
    u32 i = 0;
    while (key[i]) {
        if (i >= len || line[i] != key[i]) return 0;
        i++;
    }
    *val = line + i;
    *vlen = len - i;
    return 1;
}

static void load_fuck_config(void) {
    u8 *data = 0;
    u32 size = 0;
    if (f32_read_path_lfn_w("system/deshab64/FUCK", &data, &size) != 0) {
        slog("[cfg] FUCK not found, defaults");
        return;
    }
    /* [desktop] 的 fluent=/motion_selftest=；[prank] 交给 prank_config_line。
     * 旧匹配器只认 [dsk] 且要求行长 >= 9，[desktop] 从未命中。两条都认。 */
    int in_dsk = 0, in_desktop = 0, in_prank = 0;
    u32 pos = 0;
    while (pos < size) {
        u32 eol = pos;
        while (eol < size && data[eol] != '\n' && data[eol] != '\r') eol++;
        u32 len = eol - pos;
        const char *line = (const char *)data + pos;
        if (len > 0 && line[0] != '#' && line[0] != ';') {
            if (len > 1 && line[0] == '[') {
                in_dsk = (len >= 9 && line[1] == 'd' && line[2] == 's' &&
                          line[3] == 'k' && line[4] == ']');
                in_desktop = (len == 9 && line[1] == 'd' && line[2] == 'e' &&
                              line[3] == 's' && line[4] == 'k' && line[5] == 't' &&
                              line[6] == 'o' && line[7] == 'p' && line[8] == ']');
                in_prank = (len >= 7 && line[1] == 'p' && line[2] == 'r' &&
                            line[3] == 'a' && line[4] == 'n' && line[5] == 'k' &&
                            line[6] == ']');
            } else if (in_prank) {
                prank_config_line(line, (int)len);
            } else if (in_dsk || in_desktop) {
                const char *val = 0;
                u32 vl = 0;
                if (cfg_key(line, len, "fluent=", &val, &vl)) {
                    int v = (vl > 0 && val[0] == '0') ? 0 : 1;
                    if (g_fluent != v) {
                        g_fluent = v;
                        slog(v ? "[cfg] fluent=1 (Aqua desktop)" :
                                 "[cfg] fluent=0 (KATE fallback)");
                    }
                } else if (cfg_key(line, len, "motion_selftest=", &val, &vl)) {
                    g_motion_selftest = (vl > 0 && val[0] == '1');
                }
            }
        }
        pos = eol;
        while (pos < size && (data[pos] == '\n' || data[pos] == '\r')) pos++;
    }
}

/* ============================================================
 *  动效推进（主循环与 PE pump 每帧调用）
 *  dt 取真实 TSC 间隔，钳到 1..32ms；Dock / 顶栏先按鼠标更新目标。
 *  返回 1 = 本帧仍有轨道活动（需要重绘）。
 * ============================================================ */
static u64 g_motion_tsc = 0;

int desktop_motion_step(void) {
    int dt = 16, act = 0;
    if (g_tsc_per_sec) {
        u64 now = rdtsc();
        if (g_motion_tsc) {
            u64 ms = (now - g_motion_tsc) * 1000u / g_tsc_per_sec;
            dt = ms < 1 ? 1 : ms > 32 ? 32 : (int)ms;
        }
        g_motion_tsc = now;
    }
    if (g_fluent) {
        menubar_pointer(g_mouse_x, g_mouse_y);
        dock_pointer(g_mouse_x, g_mouse_y);
    }
    act |= motion_tick(dt);
    if (g_fluent) {
        act |= dock_tick(dt);
        act |= launchpad_tick(dt);
    }
    wm_anim_settle();
    return act;
}

/* ============================================================
 *  登录 → 桌面 crossfade（渐隐渐显）
 *  登录最后一帧已在真实 fb 上；桌面首帧已由 redraw_all() 绘入
 *  sprite buffer。复用 WALLPAPER_STAGING（wallpaper_init 后已空闲）
 *  保存登录帧，按 alpha 混合 16 步直写真实 fb。
 * ============================================================ */

#define XFADE_ADDR   WALLPAPER_STAGING_ADDR   /* 8MB 区，init 后空闲 */
#define XFADE_STEPS  28

static void busy_delay(u32 cycles) {
    for (volatile u32 i = 0; i < cycles; i++) __asm__("pause");
}

/* t 为 0..256。两端慢、中间快。 */
static int ease_smooth(int t) {
    u64 u = (u64)t;
    if (u > 256) u = 256;
    return (int)(u * u * (768u - 2u * u) / 65536u);
}

static void wait_ms(u32 ms) {
    if (!ms) return;
    if (!g_tsc_per_sec) { busy_delay(ms * 8000u); return; }
    u64 end = rdtsc() + (g_tsc_per_sec / 1000u) * (u64)ms;
    while (rdtsc() < end) __asm__ volatile("pause");
}

static void crossfade_login_to_desktop(void) {
    int W = (int)g_fb_w, H = (int)g_fb_h;
    u32 *src = (u32 *)XFADE_ADDR;          /* 登录帧 */
    const u32 *dst = (const u32 *)g_fb.fb; /* 桌面帧（sprite buffer） */
    u32 *out = (u32 *)g_real_fb;
    u64 n = (u64)W * (u64)H;

    /* 1. 保存登录最后一帧 */
    {
        u64 pitch_u32 = g_fb_pitch / 4;
        for (u64 y = 0; y < g_fb_h; y++) {
            const u32 *s = (const u32 *)((const u8 *)g_real_fb + y * g_fb_pitch);
            u32 *d = (u32 *)((u8 *)src + y * (u64)W * 4);
            for (u64 x = 0; x < g_fb_w; x++) d[x] = s[x];
        }
        (void)pitch_u32;
    }

    /* 2. 桌面首帧绘入 sprite buffer（由调用方 redraw_all() 完成后进入本函数） */

    /* 3. alpha 混合直写真实 fb：out = src*(1-a) + dst*a */
    for (int step = 1; step <= XFADE_STEPS; step++) {
        u32 a = (u32)ease_smooth(step * 256 / XFADE_STEPS);
        u32 ia = 256 - a;
        for (u64 i = 0; i < n; i++) {
            u32 s = src[i], d = dst[i];
            u32 sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
            u32 dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            out[i] = 0xFF000000u |
                     (((sr * ia + dr * a) >> 8) << 16) |
                     (((sg * ia + dg * a) >> 8) << 8) |
                     ((sb * ia + db * a) >> 8);
        }
        wait_ms(28);
    }
}

/* 壁纸就位之后：图标淡入，顶栏自上、Dock 自下。 */
static void desktop_reveal(void) {
    int step;
    if (!g_fluent) {
        g_boot_reveal = 256;
        redraw_all();
        flip_buffer();
        return;
    }
    for (step = 1; step <= 22; step++) {
        g_boot_reveal = ease_smooth(step * 256 / 22);
        if (g_boot_reveal < 1) g_boot_reveal = 1;
        redraw_all();
        flip_buffer();
        wait_ms(32);
    }
    g_boot_reveal = 256;
    redraw_all();
    flip_buffer();
}

/* ============================================================
 *  严格错误策略插桩（D 系列）：错误不再降级，整屏 fatal + 错误码。
 *  每个插桩点唯一 symbol（DESK-E<nn>）。
 * ============================================================ */

void desktop_fatal(const char *sym) {
    /* 严格错误策略：串口留档 + 深红 fatal 屏（ASCII 18px 直写真实 fb）+ 停机。
     * dev 与 release 都不走备用路径。 */
    slog(sym);
    if (g_real_fb && g_fb_w && g_fb_h) {
        u32 *fb = (u32 *)g_real_fb;
        u64 pitch_u32 = g_fb_pitch / 4;
        for (u64 y = 0; y < g_fb_h; y++)
            for (u64 x = 0; x < g_fb_w; x++)
                fb[y * pitch_u32 + x] = 0xFF3A0A0Au;   /* 深红 */
        /* 居中绘制 symbol（ASCII 字符走 11x18 位图，其余跳过） */
        int len = 0;
        while (sym[len]) len++;
        int cw = 11, ch = 18;
        int x0 = (int)(g_fb_w - (u64)len * cw) / 2;
        int y0 = (int)g_fb_h / 2 - ch / 2;
        for (int i = 0; i < len; i++) {
            unsigned char c = (unsigned char)sym[i];
            if (c < 32 || c > 126) continue;
            const unsigned char *g = g_ascii[c - 32];
            for (int py = 0; py < ch; py++) {
                u32 *row = fb + (u64)(y0 + py) * pitch_u32;
                for (int px = 0; px < cw; px++) {
                    if (g[py * cw + px])
                        row[x0 + i * cw + px] = 0xFFF2F2F2u;   /* 白 */
                }
            }
        }
    }
    for (;;)
#ifdef DESHAB_HOST
        host_fatal_halt(sym);
#else
        __asm__("cli; hlt");
#endif
}

/* ============================================================
 *  主入口
 * ============================================================ */

/* P4e 万物互联：DKM 驱动入口。driver_init 从 api/bus 解析服务并注册
 * srv.desktop.run；desktop_run 执行桌面主循环（永不返回，同母本）。 */
static const struct dkm_kernel_api *g_dsk_api;
static int g_desktop_kbd_logged;
static struct dsk_boot_context g_compat_ctx;   /* 供 launcher 加载外部 ELF 应用的兼容上下文 */
static int desktop_run(void);                   /* 前向声明 */
static int desktop_spawn(void);                 /* P6b：任务化入口 */

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api, struct dkm_driver_handle *handle) {
    (void)handle;
    if (!api || !api->log) return -1;
    g_dsk_api = api;
    g_kernel_api = (const void *)api;

    g_fb_addr = (u64)api->fb_address;
    g_fb_w = api->fb_width;
    g_fb_h = api->fb_height;
    g_fb_pitch = api->fb_pitch;

    /* block API（P4e：api->block 直读，不再 +0xA8 裸偏移）。
     * 原始指针保存在 *_raw；对外统一暴露重定向到启动卷设备的包装。 */
    g_block_read_raw  = (api->block && api->block->read)  ? (desktop_block_read_fn)api->block->read  : 0;
    g_block_write_raw = (api->block && api->block->write) ? (desktop_block_write_fn)api->block->write : 0;
    g_block_read  = g_block_read_raw ? dt_block_read  : 0;
    g_block_write = g_block_write_raw ? dt_block_write : 0;

    /* 启动卷设备索引（fat32.drv 已跨设备探测完成；缺席时保持 0）。 */
    g_fat_dev = 0;
    {
        void *fn = 0;
        if (api->bus && api->bus->probe("srv.fs.fat32.volume_info", &fn, 0) == 0 && fn) {
            const struct dkm_fat32_volume_info *(*get)(void) =
                (const struct dkm_fat32_volume_info *(*)(void))fn;
            const struct dkm_fat32_volume_info *vi = get();
            if (vi && vi->valid) {
                g_fat_dev = vi->dev;
                slog_num("[desktop] fat32 volume dev=", (int)vi->dev);
            }
        }
    }

    /* P4e 过渡：为 launcher 加载外部 ELF 应用构造兼容 boot-context（子应用仍按
     * dsk_boot_context ABI 读）。从 api 填字段；reserved[] 留空。 */
    {
        u64 *r = g_compat_ctx.reserved;
        for (u32 i = 0; i < 8; i++) r[i] = 0;
        g_compat_ctx.magic = 0x44534B31424F4F54ULL;
        g_compat_ctx.framebuffer_address = (u64)api->fb_address;
        g_compat_ctx.framebuffer_width = api->fb_width;
        g_compat_ctx.framebuffer_height = api->fb_height;
        g_compat_ctx.framebuffer_pitch = api->fb_pitch;
        g_compat_ctx.dkm_kernel_api = (u64)api;
        g_boot_ctx = &g_compat_ctx;
    }

    if (api->bus) {
        api->bus->register_service("srv.desktop.run", 1, (void *)desktop_run);
        api->bus->register_service("srv.desktop.spawn", 1, (void *)desktop_spawn);
        api->log->info("[desktop] srv.desktop.run registered");
    }
    return 0;
}

/* ===================================================================
 *  电源：任务栏/开始菜单的电源钮
 *
 *  经总线调用 acpi.drv 注册的 srv.acpi.poweroff / srv.acpi.reboot
 *  （母本 SYSTEMCOPY/OpenXJ380 driver/power.cpp 的 S5 + reset_reg 路径）。
 *  服务缺席（无 ACPI 表 / FUCK [drivers] acpi=0）→ 退回 g_quit 退出桌面，
 *  由 session 收尾（与改造前行为一致）。
 * =================================================================== */
int desktop_poweroff(void) {
    int (*fn)(void) = 0;
    if (g_dsk_api && g_dsk_api->bus &&
        g_dsk_api->bus->probe("srv.acpi.poweroff", (void **)&fn, 0) == 0 && fn) {
        slog("[power] srv.acpi.poweroff");
        fn();                      /* 成功不返回 */
        slog("[power] poweroff returned");
    }
    slog("[power] ACPI poweroff unavailable; quitting desktop");
    g_quit = 1;
    return -1;
}

int desktop_reboot(void) {
    int (*fn)(void) = 0;
    if (g_dsk_api && g_dsk_api->bus &&
        g_dsk_api->bus->probe("srv.acpi.reboot", (void **)&fn, 0) == 0 && fn) {
        slog("[power] srv.acpi.reboot");
        fn();                      /* 成功不返回 */
        slog("[power] reboot returned");
    }
    slog("[power] ACPI reboot unavailable; falling back to 0xCF9");
#ifdef DESHAB_HOST
    host_demo_quit();
    return -1;
#else
    outb(0x64, 0xFE);
    outb(0xCF9, 0x06);
    for (;;) __asm__ volatile("hlt");
    return -1;
#endif
}

/* P6b：把桌面主循环变成调度器任务。
 * 返回 1 = 任务已创建（session 可返回，bootstrap 转 idle）；
 * 返回 0 = 调度器不可用，调用方应回退到 srv.desktop.run 直接执行。 */
static void desktop_task_entry(void *arg) {
    (void)arg;
    desktop_run();
}

static int desktop_spawn(void) {
    const struct dkm_kernel_api *api = g_dsk_api;
    if (!api || !api->sched || !api->sched->task_create) {
        api->log->warn("[desktop] no sched api; spawn unavailable");
        return 0;
    }
    /* 0xFFFFFFFF = 无进程关联的纯内核任务（sched_ext.h 约定） */
    int slot = api->sched->task_create(0xFFFFFFFFu, 20, desktop_task_entry, 0);
    if (slot < 0) {
        api->log->error("[desktop] task_create failed");
        return 0;
    }
    slog_num("[desktop] task slot=", slot);
    return 1;
}

static int desktop_run(void) {
    const struct dkm_kernel_api *api = g_dsk_api;
    slog("boot");

    /* P6b：输入经服务总线（IRQ1/IRQ12 已由 session.drv 解屏蔽）。 */
    if (dt_input_init() != 0) {
        api->log->error("[desktop] srv.input.kbd.poll missing");
        api->log->panic("INPUT-E01 KBD POLL SERVICE MISSING");
        return 0;
    }
#ifndef DESHAB_HOST
    __asm__ volatile("sti" ::: "memory");
#endif
    slog("input: kbd+mouse via service bus");

    /* lxc / pe 服务（P4e：经服务总线 kern.lxc.service / kern.pe.service，
     * 不再 reserved[4]/[5]）。缺席即降级（lxc→本地 editor，pe→报错）。 */
    {
        void *fn = 0;
        if (api && api->bus && api->bus->probe("kern.lxc.service", &fn, 0) == 0 && fn) {
            const linux_compat_service *(*get)(void) = (const linux_compat_service *(*)(void))fn;
            const linux_compat_service *lxc = get();
            if (lxc && lxc->magic == LINUX_COMPAT_MAGIC) g_lxc_svc = lxc;
        }
        if (g_lxc_svc) slog("lxc service available");
        else { g_lxc_svc = 0; slog("lxc service unavailable (IDE will use local editor)"); }

        fn = 0;
        if (api && api->bus && api->bus->probe("kern.pe.service", &fn, 0) == 0 && fn) {
            const pe_service *(*pget)(void) = (const pe_service *(*)(void))fn;
            const pe_service *pe = pget();
            if (pe && pe->magic == PE_SERVICE_MAGIC) g_pe_svc = pe;
        }
        if (g_pe_svc) slog("pe service available");
        else { g_pe_svc = 0; slog("pe service unavailable"); }

        /* 串口键鼠：FUCK [desktop] serial_test=1。不绑 dev_mode，避免先被 shell 自测挡住。 */
        fn = 0;
        if (api && api->bus && api->bus->probe("kern.config.get", &fn, 0) == 0 && fn) {
            const char *(*get)(const char *, const char *) = (const char *(*)(const char *, const char *))fn;
            const char *st = get("desktop", "serial_test");
            if (st && st[0] == '1') {
                g_serial_test = 1;
                slog("serial test on: !M x y | !C | !R | !K hh | !T text");
            }
        }


        /* P4e 过渡补齐：外部 ELF 子应用（browser 等）仍按旧 ABI 读
         * g_compat_ctx.reserved[4]=pe / [5]=lxc（desktop_init 里 reserved[]
         * 被清零）。这里把刚经总线拿到的服务回填，否则子应用永远看不到
         * lxc/pe —— browser 就因此 "lxc unavailable" 只能走 host 回退。 */
        g_compat_ctx.reserved[4] = (u64)g_pe_svc;
        g_compat_ctx.reserved[5] = (u64)g_lxc_svc;
    }

    /* 双缓冲只有 8MiB（SPRITE_BUF_ADDR 0x7000000 → PE_SURFACE_ADDR 0x7800000）。
     * 引导器已把 GOP 模式限制在预算内；这里再做硬校验，越界即 DESK-E05，
     * 绝不带病渲染（越界会踩到字库/PE surface，表现为整屏错位）。 */
    if (g_fb_h * g_fb_pitch > (8u * 1024u * 1024u)) {
        slog_num("[desktop] fb bytes over sprite budget=", g_fb_h * g_fb_pitch);
        desktop_fatal("DESK-E05 SPRITE BUF OVERFLOW");
    }

    du_context_init(&g_fb, g_fb_addr, g_fb_w, g_fb_h, g_fb_pitch);
    /* 双缓冲：保存真实 fb，切换到 sprite buffer 渲染 */
    g_real_fb = (u8 *)g_fb_addr;
    g_fb.fb = (du_u32 *)SPRITE_BUF_ADDR;
    f32_io_init(g_block_read, g_block_write);

    /* M1: 中文位图字库加载（system/font/simhei_16.dbf → 0x8000000）。
     * 失败阻塞启动。 */
    {
        int zrc = zhfont_init(dt_read_to);
        if (zrc == 0) {
            slog("[zhfont] dbf loaded (16px ok)");
        } else {
            slog_num("[zhfont] dbf load failed rc=", zrc);
            desktop_fatal("DESK-E04 FONT PACK MISSING");
            /* fallback：中文字形缺失时 zhfont 画空框占位，ASCII 正常 */
        }
    }

    /* M2: FUCK 配置（[desktop] fluent=）→ 壁纸初始化（Fluent 路径需要） */
    load_fuck_config();
    slog(g_fluent ? "[cfg] shell=aqua" : "[cfg] shell=kate");
    if (g_prank_enabled) {
        slog(g_prank_boot_demo > 0 ? "[cfg] prank enabled (boot demo on)"
                                   : "[cfg] prank enabled");
    }
    if (g_fluent) {
        int wrc = wallpaper_init(dt_read_to, (int)g_fb_w, (int)g_fb_h);
        if (wrc == 0) {
            int grc;
            slog("[wallpaper] loaded + scaled");
            grc = glass_init((int)g_fb_w, (int)g_fb_h);
            if (grc != 0) slog_num("[glass] blur failed rc=", grc);
        } else {
            slog_num("[wallpaper] load failed rc=", wrc);
            /* 严格错误策略：材质包（壁纸）缺失不降级，fatal 屏 + 错误码 */
            desktop_fatal("DESK-E02 TEXTURE PACK MISSING");
            g_fluent = 0;   /* fallback：回落经典（非 Fluent）桌面 */
        }
        /* 顶栏 Apple 菜单 logo（textures/startMenuLogo.rgba → 0x8700000） */
        if (g_fluent && taskbar_init(dt_read_to) == 0)
            slog("[menubar] logo loaded");
        else if (g_fluent) {
            /* 严格错误策略：材质包（开始菜单 logo）缺失不降级 */
            desktop_fatal("DESK-E03 STARTMENU LOGO MISSING");
            g_fluent = 0;   /* fallback：回落经典（非 Fluent）桌面 */
        }
        if (g_fluent) g_page = 3;   /* Fluent 默认进 DESKTOP 页 */
    }

    tsc_calibrate();
    if (g_motion_selftest) {
        motion_selftest();
        slog(glass_ready() ? "[glass] blur ok" : "[glass] blur skipped");
    }
    g_rtc_boot_sec = rtc_day_sec();
    g_last_sec = g_rtc_boot_sec;

    ps2_mouse_init();

    register_apps();
    setup_desktop_icons();

    /* 工作区 0 */
    kstrcpy(g_ws[0].name, "WS-1", 16);
    g_ws[0].page = 1;
    g_ws_count = 1;
    g_ws_cur = 0;

    /* DASHBOARD 面板共享状态 */
    {
        bash_state *t1 = (bash_state *)ADDR_TERM1;
        bash_reset(t1, 640, 280);
        bash_puts(t1, "Deshab Bash v0.1 - TERM-01\nType help for commands\n\n", KS_TEXT_PRIMARY);

        bash_state *t2 = (bash_state *)ADDR_TERM2;
        bash_reset(t2, 640, 280);
        bash_puts(t2, "Deshab Bash v0.1 - TERM-02\nType help for commands\n\n", KS_TEXT_PRIMARY);

        editor_reset((editor_state *)ADDR_EDDASH,
            "Welcome to Deshab Editor\n"
            "\n"
            "- Double-click a file in FILES panel to open it here\n"
            "- SAVE button / Ctrl+S writes back to FAT32 disk\n"
            "- IDE page hosts this same editor full-screen\n");
    }

    files_refresh();

    /* 先只出壁纸，从上一画面（Logo 或登录）缓入，再让顶栏 / Dock / 图标入场 */
    g_boot_reveal = 0;
    redraw_all();
    crossfade_login_to_desktop();
    desktop_reveal();
    g_booted = 1;

    slog("desktop ready");
    /* 整蛊系统：开机进桌面后按 FUCK [prank] boot_demo_sec 串烧演示一遍 */
    prank_boot_demo_start();
    desktop_launch_pending_native();

    /* 主事件循环 */
    /* P8.1: dev 自动 VSCode 启动测试--无需 GUI 交互即可验证 launch 链路 */
    static int g_autotest_state = 0;
    static u64 g_autotest_tsc = 0;
    static int g_hover_key = 0;

    while (!g_quit) {
        int need_redraw = 0;
        int moved = 0;

        /* Guest 进度横幅：100ms 采样，有变化才重绘 */
        if (guest_banner_tick()) need_redraw = 1;

        /* 整蛊系统：推进调度；可视类整蛊播放期间强制全屏重绘 */
        prank_tick();
        if (prank_wants_redraw()) need_redraw = 1;

        /* P8.1: 自动启动测试（先 ping guest，不可用直接报告，不浪费 3s 等待） */
        /* 后台装载：guest 可能还没 READY，不能一次 ping 失败就放弃。 */
        static u64 g_autotest_try_tsc = 0;
        if (g_tsc_per_sec && g_autotest_state == 0) {
            g_autotest_tsc = rdtsc();
            g_autotest_try_tsc = 0;
            g_autotest_state = 1;
        }
        if (g_autotest_state == 1 && g_tsc_per_sec) {
            u64 now = rdtsc();
            if (now - g_autotest_tsc >= (u64)g_tsc_per_sec * 90) {
                slog("[autotest] SKIP: Linux guest not ready after 90s");
                g_autotest_state = 3;
            } else if (now >= g_autotest_try_tsc) {
                g_autotest_try_tsc = now + g_tsc_per_sec;
                if (lxc_guest_ping() == 0) {
                    slog("[autotest] guest OK, waiting 90s before vscode_launch");
                    g_autotest_tsc = now;
                    g_autotest_state = 4;
                }
            }
        }
        if (g_autotest_state == 4 && g_tsc_per_sec) {
            u64 elapsed = rdtsc() - g_autotest_tsc;
            if (elapsed >= (u64)g_tsc_per_sec * 90) {
                slog("[autotest] triggering vscode_launch");
                g_autotest_state = 2;
                g_autotest_tsc = rdtsc();
                vscode_launch();
            }
        }
        if (g_autotest_state == 2 && g_tsc_per_sec) {
            u64 elapsed = rdtsc() - g_autotest_tsc;
            /* 15s 后报告结果 */
            if (elapsed >= (u64)g_tsc_per_sec * 15) {
                if (g_vscode_state == 2) {
                    slog("[autotest] SUCCESS: scanout online, VSCode running");
                } else if (g_vscode_state == 1) {
                    slog("[autotest] TIMEOUT: still launching");
                    slog_num("[autotest] phase=", g_launch_phase);
                } else {
                    slog("[autotest] FAIL: vscode_state=0");
                    slog_num("[autotest] phase=", g_launch_phase);
                }
                g_autotest_state = 3;
            }
        }

        /* 鼠标：排空所有数据包 */
        while (ps2_mouse_poll()) moved = 1;

        /* dev 串口测试（一行一条，以 ! 开头，CR/LF 结束）：
         *   !M x y   光标移到绝对坐标
         *   !C / !R  左键 / 右键单击
         *   !K hh    注入 Set-1 扫描码（两位十六进制，如 1C=Enter）
         *   !T text  把字母数字打成扫描码
         * 非 ! 的单字节仍走下面的启动通道。 */
        {
            static char line[80];
            static int llen, armed;
            unsigned char ch = 0;
            int have = 0;
            if (inb(COM1 + 5) & 1) { ch = inb(COM1); have = 1; }
            if (have && g_serial_test && (armed || ch == '!')) {
                have = 0;
                if (!armed) { armed = 1; llen = 0; }
                else if (ch == '\n' || ch == '\r') {
                line[llen] = 0;
                armed = 0;
                if (line[0] == 'M') {
                    int x = 0, y = 0, i = 1, sign = 1;
                    while (line[i] == ' ') i++;
                    if (line[i] == '-') { sign = -1; i++; }
                    while (line[i] >= '0' && line[i] <= '9') { x = x * 10 + line[i] - '0'; i++; }
                    x *= sign; sign = 1;
                    while (line[i] == ' ') i++;
                    if (line[i] == '-') { sign = -1; i++; }
                    while (line[i] >= '0' && line[i] <= '9') { y = y * 10 + line[i] - '0'; i++; }
                    y *= sign;
                    if (x < 0) x = 0;
                    if (y < 0) y = 0;
                    if (g_fb_w && x >= (int)g_fb_w) x = (int)g_fb_w - 1;
                    if (g_fb_h && y >= (int)g_fb_h) y = (int)g_fb_h - 1;
                    g_mouse_x = x; g_mouse_y = y;
                    slog("serial !M");
                    need_redraw = 1;
                } else if (line[0] == 'C') {
                    g_left_pressed = 1;
                    g_left_released = 1;
                    slog("serial !C");
                } else if (line[0] == 'R') {
                    g_right_pressed = 1;
                    slog("serial !R");
                } else if (line[0] == 'K') {
                    int i = 1, v = 0, n = 0;
                    while (line[i] == ' ') i++;
                    while (n < 2 && line[i]) {
                        char c = line[i++];
                        int d = (c >= '0' && c <= '9') ? c - '0' :
                                (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                                (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                        if (d < 0) break;
                        v = (v << 4) | d; n++;
                    }
                    if (n) { dt_kbd_inject((u8)v); slog("serial !K"); }
                } else if (line[0] == 'T') {
                    static const char alpha[] = "qwertyuiopasdfghjklzxcvbnm";
                    static const u8 alpha_sc[] = {
                        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,
                        0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,
                        0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32
                    };
                    int i = 1;
                    while (line[i] == ' ') i++;
                    for (; line[i]; i++) {
                        unsigned char c = (unsigned char)line[i];
                        int shift = 0;
                        u8 sc = 0;
                        if (c >= 'A' && c <= 'Z') { shift = 1; c = (unsigned char)(c + 32); }
                        if (c >= 'a' && c <= 'z') {
                            for (int k = 0; k < 26; k++) if (alpha[k] == c) { sc = alpha_sc[k]; break; }
                        } else if (c >= '1' && c <= '9') sc = (u8)(0x02 + c - '1');
                        else if (c == '0') sc = 0x0B;
                        else if (c == ' ') sc = 0x39;
                        else if (c == '-') sc = 0x0C;
                        else if (c == '=') sc = 0x0D;
                        if (!sc) continue;
                        if (shift) dt_kbd_inject(0x2A);
                        dt_kbd_inject(sc);
                        dt_kbd_inject((u8)(sc | 0x80));
                        if (shift) dt_kbd_inject(0xAA);
                    }
                    slog("serial !T");
                }
                llen = 0;
            } else if (llen < 79) {
                line[llen++] = (char)ch;
            }
            }
            if (have) {
            if (ch == '2') {
                slog("serial 2: launch KDE");
                launch_app_by_name("kde");
                need_redraw = 1;
            } else if (ch == '3') {
                slog("serial 3: launchpad toggle");
                launchpad_toggle();
                need_redraw = 1;
            } else if (ch == '4') {
                slog("serial 4: control center toggle");
                taskbar_cc_toggle();
                need_redraw = 1;
            } else if (ch == '5') {
                slog("serial 5: launch sysinfo.exe");
                if (launch_app_by_name("sysinfo")) need_redraw = 1;
            } else if (ch == '6') {
                slog("serial 6: launch clock.exe");
                if (launch_app_by_name("clock")) need_redraw = 1;
            } else if (ch == '7') {
                slog("serial 7: launch diskinfo.elf");
                if (launch_app_by_name("diskinfo")) need_redraw = 1;
            } else if (ch == '8') {
                slog("serial 8: close overlays");
                launchpad_close();
                menubar_close_menus();
                taskbar_cards_close();
                need_redraw = 1;
            } else if (ch >= 'a' && ch <= 'f') {
                static const char *names[6] = {
                    "shell", "cmd", "browser", "settings", "proedit", "editor"
                };
                slog("serial a-f: launch by name");
                launch_app_by_name(names[ch - 'a']);
                need_redraw = 1;
            }
            }
        }

        if (g_left_pressed) {
            g_left_pressed = 0;
            if (on_left_press()) need_redraw = 1;
        }
        if (g_right_pressed) {
            g_right_pressed = 0;
            if (on_right_press()) need_redraw = 1;
        }
        if (g_left_released) {
            g_left_released = 0;
            if (g_dragging || g_drag_slider) {
                g_dragging = 0;
                g_drag_win = -1;
                g_drag_slider = 0;
                need_redraw = 1;
            }
        }

        /* 窗口拖拽 */
        if (moved && g_dragging && g_drag_win >= 0) {
            desktop_window *w = win_find(g_drag_win);
            if (w) {
                w->x = g_mouse_x - g_drag_off_x;
                w->y = g_mouse_y - g_drag_off_y;
                {
                    int top = g_fluent ? AQUA_MENUBAR_H : KATE_TOPBAR_H;
                    if (w->y < top) w->y = top;
                }
                w->zoomed = 0;
                if (w->x < 0 - w->w + 40) w->x = 0 - w->w + 40;
                if (w->x > (int)g_fb_w - 40) w->x = (int)g_fb_w - 40;
                if (w->y > (int)g_fb_h - KATE_TASKBAR_H - 20)
                    w->y = (int)g_fb_h - KATE_TASKBAR_H - 20;
                win_present_sync(w);
            }
            need_redraw = 1;
        }

        /* 滑块拖拽 */
        if (moved && g_drag_slider == 1) { slider_set(g_vol_rect, &g_vol, g_mouse_x); need_redraw = 1; }
        if (moved && g_drag_slider == 2) { slider_set(g_bri_rect, &g_bright, g_mouse_x); need_redraw = 1; }

        /* 键盘：排空（srv.input.kbd.poll ← ps2kbd.drv IRQ1 ring） */
        {
            u8 sc;
            while (dt_kbd_poll(&sc)) {
                if (!g_desktop_kbd_logged) {
                    g_desktop_kbd_logged = 1;
                    slog("kbd event via srv.input.kbd.poll");
                }
                /* 整蛊"打字乱码"：按键可打印码被随机重映射（特殊键/释放码放行） */
                if (handle_key(prank_filter_key(sc))) need_redraw = 1;
            }
        }

        /* RTC 秒 tick：DASHBOARD 状态栏时钟/UP 计时刷新 */
        {
            int sec = rtc_day_sec();
            if (sec != g_last_sec) {
                g_last_sec = sec;
                if (g_page == 1 && !g_taskview) need_redraw = 1;
            }
        }

        /* guest 里的 X/VSCode 只有在 resume 时才跑。挂上 IDE 后每帧切几片。 */
        if (g_ide_attached && g_lxc_svc && g_lxc_svc->run_slice) {
            for (int s = 0; s < 4; s++) (void)g_lxc_svc->run_slice();
        }

        /* VSCode Phase 3/7: IDE attached 时轮询 guest scanout dirty，
         * 有新帧则触发重绘以 blit 到 framebuffer。查询结果缓存进 g_so
         * （查询清零 UTSM 侧 dirty + dirty rect），blit 据此局部拷贝。 */
        if (g_page == 2 && g_ide_attached && g_input_forward_enabled &&
            g_lxc_svc && g_lxc_svc->gpu_get_scanout_info) {
            if (g_lxc_svc->gpu_get_scanout_info(&g_so) == 0) {
                g_so_valid = 1;
                if (g_so.dirty) need_redraw = 1;
            }
        }

        /* P7.9: FPS 统计（每秒更新，基于 TSC） */
        if (g_tsc_per_sec && g_prof_fps_tsc) {
            u64 elapsed = rdtsc() - g_prof_fps_tsc;
            if (elapsed >= g_tsc_per_sec) {
                g_prof_fps = (int)(g_prof_frames * g_tsc_per_sec / elapsed);
                g_prof_frames = 0;
                g_prof_fps_tsc = rdtsc();
                if (g_page == 2) need_redraw = 1;  /* 刷新叠加显示 */
            }
        } else if (g_tsc_per_sec) {
            g_prof_fps_tsc = rdtsc();
        }
        if (need_redraw && g_page == 2) g_prof_frames++;

        if (desktop_motion_step()) need_redraw = 1;
        if (moved && g_fluent) {
            int hk = aqua_hover_key();
            if (hk != g_hover_key) { g_hover_key = hk; need_redraw = 1; }
        }

        if (need_redraw) {
            /* 脏矩形渲染：合并脏矩形，只 flip 变化区域 */
            if (g_full_redraw || g_dirty_count == 0) {
                /* 全屏重绘 */
                redraw_all();
                flip_buffer();
            } else {
                /* 合并脏矩形 */
                int dx, dy, dw, dh;
                merge_dirty_rects(&dx, &dy, &dw, &dh);

                /* 重绘并 flip 脏区域 */
                if (dw > 0 && dh > 0) {
                    /* 清除脏矩形标记 */
                    clear_dirty_rects();

                    /* 重绘整个屏幕（简化：暂不支持局部重绘） */
                    redraw_all();

                    /* Flip 合并后的脏区域 */
                    flip_rect(dx, dy, dw, dh);
                }
            }
        } else if (moved) {
            /* 仅光标移动：局部 flip（双缓冲 → 无闪烁） */
            int old_x = g_cursor_old_x, old_y = g_cursor_old_y;
            cursor_restore_bg();
            flip_rect(old_x, old_y, CURSOR_SIZE, CURSOR_SIZE);
            cursor_save_bg(g_mouse_x, g_mouse_y);
            cursor_draw(g_mouse_x, g_mouse_y);
            flip_rect(g_mouse_x, g_mouse_y, CURSOR_SIZE, CURSOR_SIZE);
        }

        /* P6b：桌面是长驻任务，每帧让出 1ms——否则忙等会吃掉整个时间片。
         * 无调度器 API 时退回 pause 忙等（旧行为）。 */
        if (api->sched && api->sched->sleep_ms) api->sched->sleep_ms(1);
        else __asm__("pause");
    }   /* for(;;) 桌面主循环：永不返回 */

    slog("exit");
    return 0;   /* 不可达 */
}

/* ===================================================================
 *  P4e: DKM 驱动描述符 + 退出
 * =================================================================== */
static const char *const g_provides[] = { "desktop" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic = DKM_DRIVER_MAGIC, .abi_version = DKM_ABI_VERSION,
    .desc_size = sizeof(struct dkm_driver_desc),
    .name = "desktop", .version = "0.1.0", .vendor = "Deshab",
    .driver_class = DKM_CLASS_MISC, .stage = 4,
    .flags = DKM_F_NO_UNLOAD, .priority = 0,
    .depends = NULL, .depends_count = 0,
    .provides = g_provides, .provides_count = 1,
    .min_kernel_abi = 1,
};

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle; return 0;
}
