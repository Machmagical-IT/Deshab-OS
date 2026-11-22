/* 引导器收尾后的深蓝色屏幕。圆点由独立的内核任务绘制：优先级最高，
 * 每 16ms 被调度器唤醒画一帧再睡下。启动主流程再忙（读盘、加载驱动）
 * 也会被抢占，转圈不会跟着停。会话 Logo 之前 kern.boot.dismiss 关掉它。 */
#include <utsm/boot_status.h>
#include <utsm/types.h>
#include <utsm/pit.h>
#include <utsm/dkm.h>
#include <utsm/sched_ext.h>
#include <utsm/panic.h>
#include <utsm/log.h>
#include "../arch/x86_64/limine.h"

#include "../../boot/init_banner.h"

extern volatile struct limine_framebuffer_request g_fb_request;

static u8 *g_fb;
static u64 g_w, g_h, g_pitch;
static u32 g_rsh, g_gsh, g_bsh;
static int g_on;
static int g_sx, g_sy;
static u32 g_phase;
static u64 g_tsc0, g_step;
static int g_owned;   /* 1 = 转圈任务独占绘制，日志路径不再碰屏幕 */

static const i8 k_cos12[12] = { 16, 14, 8, 0, -8, -14, -16, -14, -8, 0, 8, 14 };
static const i8 k_sin12[12] = { 0, 8, 14, 16, 14, 8, 0, -8, -14, -16, -14, -8 };
/* 头亮、尾暗，一圈 12 点 */
static const u8 k_br[12] = { 255, 210, 160, 110, 70, 44, 30, 24, 20, 18, 16, 14 };

static inline u64 bs_rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static inline u32 bs_pack(u32 rgb) {
    u32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (r << g_rsh) | (g << g_gsh) | (b << g_bsh);
}

static void bs_fill(u32 rgb) {
    u32 c = bs_pack(rgb);
    for (u64 y = 0; y < g_h; y++) {
        u32 *line = (u32 *)(g_fb + y * g_pitch);
        for (u64 x = 0; x < g_w; x++) line[x] = c;
    }
}

static void bs_banner(void) {
    int x0 = (int)g_w / 2 - INIT_BANNER_W / 2;
    int y0 = (int)g_h / 2 - (INIT_BANNER_H + 56) / 2;
    if (y0 < 0) y0 = 0;
    for (int y = 0; y < INIT_BANNER_H; y++) {
        int fy = y0 + y;
        if (fy < 0 || (u64)fy >= g_h) continue;
        u32 *line = (u32 *)(g_fb + (u64)fy * g_pitch);
        const unsigned char *row = g_init_banner_a + (u32)y * INIT_BANNER_W;
        for (int x = 0; x < INIT_BANNER_W; x++) {
            int fx = x0 + x;
            unsigned char a = row[x];
            if (!a || fx < 0 || (u64)fx >= g_w) continue;
            u32 r = 2u + 253u * a / 255u;
            u32 g = 255u * a / 255u;
            u32 b = 47u + 208u * a / 255u;
            line[fx] = bs_pack((r << 16) | (g << 8) | b);
        }
    }
    g_sx = (int)g_w / 2;
    g_sy = y0 + INIT_BANNER_H + 36;
}

static void bs_spinner(u32 phase) {
    int head = (int)(phase % 12u);
    int R = 22;
    int y0 = g_sy - R, y1 = g_sy + R, x0 = g_sx - R, x1 = g_sx + R;
    u32 bg = bs_pack(0x02002F);
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || (u64)y >= g_h) continue;
        u32 *line = (u32 *)(g_fb + (u64)y * g_pitch);
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || (u64)x >= g_w) continue;
            line[x] = bg;
        }
    }
    for (int age = 11; age >= 0; age--) {
        int i = (head - age + 12) % 12;
        int cx = g_sx + (int)k_cos12[i];
        int cy = g_sy + (int)k_sin12[i];
        int br = k_br[age];
        u32 col = bs_pack(((2u + 253u * (u32)br / 255u) << 16) |
                          ((255u * (u32)br / 255u) << 8) |
                          (47u + 208u * (u32)br / 255u));
        for (int dy = -3; dy <= 3; dy++) {
            int y = cy + dy;
            if (y < 0 || (u64)y >= g_h) continue;
            u32 *line = (u32 *)(g_fb + (u64)y * g_pitch);
            for (int dx = -3; dx <= 3; dx++) {
                if (dx * dx + dy * dy > 10) continue;
                int x = cx + dx;
                if (x < 0 || (u64)x >= g_w) continue;
                line[x] = col;
            }
        }
    }
}

static u64 bs_calibrate(void) {
    pit_oneshot_ms(10);
    u64 t0 = bs_rdtsc();
    while (!pit_oneshot_expired()) {
        if (bs_rdtsc() - t0 > 20000000000ULL) break;
    }
    u64 dt = bs_rdtsc() - t0;
    pit_quiet();
    /* 约 50ms 走一格；校准失败时按 2GHz 估算 */
    if (dt < 1000000ULL) dt = 100000000ULL;
    return dt * 5u;
}

void boot_status_start(void) {
    if (!g_fb_request.response || g_fb_request.response->framebuffer_count == 0) return;
    struct limine_framebuffer *fb = g_fb_request.response->framebuffers[0];
    if (!fb || !fb->address || fb->width < 320 || fb->height < 200 || fb->bpp != 32) return;
    g_fb = (u8 *)fb->address;
    g_w = fb->width;
    g_h = fb->height;
    g_pitch = fb->pitch ? fb->pitch : fb->width * 4;
    g_rsh = fb->red_mask_shift;
    g_gsh = fb->green_mask_shift;
    g_bsh = fb->blue_mask_shift;
    if (g_rsh == 0 && g_gsh == 0 && g_bsh == 0) { g_rsh = 16; g_gsh = 8; g_bsh = 0; }
    g_step = bs_calibrate();
    g_tsc0 = bs_rdtsc();
    g_phase = 0;
    /* 引导器已经涂过同一块深蓝并写过同一句字时不再整屏重填，避免闪一下。 */
    {
        u32 bg = bs_pack(0x02002F);
        if (((u32 *)g_fb)[0] != bg) bs_fill(0x02002F);
    }
    bs_banner();
    bs_spinner(0);
    g_on = 1;
}

static void bs_advance(void) {
    if (!g_on || !g_step) return;
    u32 phase = (u32)((bs_rdtsc() - g_tsc0) / g_step);
    if (phase == g_phase) return;
    g_phase = phase;
    bs_spinner(phase);
}

/* 任务接手之前，日志路径先顶着转。接手后这里直接返回，避免和任务抢同一块像素。 */
void boot_status_pump(void) {
    if (g_owned) return;
    bs_advance();
}

static void spinner_task(void *arg) {
    (void)arg;
    for (;;) {
        if (!g_on) utsm_sched_task_exit();
        bs_advance();
        utsm_sched_sleep_ms(16);
    }
}

void boot_status_spawn(void) {
    if (!g_on) return;
    int slot = utsm_task_create(0xFFFFFFFFu, 0, spinner_task, 0);
    if (slot < 0) {
        panic_full("SAS-E06 SPINNER TASK FAILED",
                   "boot_status_spawn: could not create the spinner task", 0);
        return;
    }
    g_owned = 1;
    /* 打开抢占。之后每次 tick 都会把这个最高优先级任务换上来画一帧。
     * 主流程里的长循环不再能把转圈卡住。 */
    utsm_sched_enable(0);
    log_info("[boot] spinner task running");
}

void boot_status_dismiss(void) {
    g_on = 0;
}

void boot_status_register(void) {
    const dkm_kernel_api *api = dkm_get_kernel_api();
    if (!api || !api->bus) return;
    api->bus->register_service("kern.boot.dismiss", 1, (void *)boot_status_dismiss);
}
