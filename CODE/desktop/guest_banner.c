/* guest_banner.c — 右下角 Guest 装载进度横幅。
 *
 * 数据源：kern.guest.progress（AP guest_runner 写入的 32 位共享表）。
 * 纯展示，不拦截鼠标；g_fluent=0 或服务缺席时不绘制。 */
#include "desktop.h"
#include "aqua_draw.h"
#include "aqua_theme.h"
#include "zhfont.h"
#include "../DKM/dkm_shared.h"
#include "../UTSM/include/utsm/guest_progress.h"

static const utsm_guest_progress_table *g_prog;
static int g_probed;
static u64 g_last_sample;
static u64 g_done_tsc;
static int g_visible;
static u32 g_cache_en[GUEST_SLOT_COUNT];
static u32 g_cache_ph[GUEST_SLOT_COUNT];
static u32 g_cache_pc[GUEST_SLOT_COUNT];
static int g_cached;

static const char *phase_text(u32 phase, u32 percent, u32 detail) {
    switch (phase) {
    case GUEST_PHASE_PREP:
        if (detail == 1 && percent >= 50) return "已准备，待启动";
        return "装载镜像";
    case GUEST_PHASE_LAUNCH: return "启动虚拟机";
    case GUEST_PHASE_KERNEL: return "内核启动";
    case GUEST_PHASE_DAEMON: return "用户态就绪";
    case GUEST_PHASE_POST:   return "同步组件";
    case GUEST_PHASE_READY:  return "已就绪";
    case GUEST_PHASE_FAILED: return "失败";
    default: return "";
    }
}

static int all_done(void) {
    int any = 0;
    for (u32 i = 0; i < GUEST_SLOT_COUNT; i++) {
        const utsm_guest_progress *p = &g_prog->slot[i];
        if (!p->enabled) continue;
        any = 1;
        if (p->phase != GUEST_PHASE_READY) return 0;
    }
    return any;
}

int guest_banner_tick(void) {
    if (!g_probed) {
        const struct dkm_kernel_api *api =
            (const struct dkm_kernel_api *)g_kernel_api;
        if (api && api->bus) {
            void *fn = 0;
            if (api->bus->probe("kern.guest.progress", &fn, 0) == 0 && fn) {
                g_prog = ((const utsm_guest_progress_table *(*)(void))fn)();
            }
        }
        g_probed = 1;
    }
    if (!g_prog || g_prog->magic != GUEST_PROGRESS_MAGIC) {
        g_visible = 0;
        return 0;
    }

    /* 100ms 采样节流 */
    if (g_tsc_per_sec) {
        u64 now = rdtsc();
        if (g_last_sample && now - g_last_sample < g_tsc_per_sec / 10u) return 0;
        g_last_sample = now;
    }

    int changed = !g_cached;
    for (u32 i = 0; i < GUEST_SLOT_COUNT; i++) {
        const utsm_guest_progress *p = &g_prog->slot[i];
        if (g_cache_en[i] != p->enabled || g_cache_ph[i] != p->phase ||
            g_cache_pc[i] != p->percent) {
            changed = 1;
            g_cache_en[i] = p->enabled;
            g_cache_ph[i] = p->phase;
            g_cache_pc[i] = p->percent;
        }
    }
    g_cached = 1;

    int done = all_done();
    if (done) {
        if (!g_done_tsc) g_done_tsc = rdtsc();
        if (g_tsc_per_sec && rdtsc() - g_done_tsc > g_tsc_per_sec * 6u) {
            if (g_visible) { g_visible = 0; changed = 1; }
        } else {
            if (!g_visible) { g_visible = 1; changed = 1; }
        }
    } else {
        g_done_tsc = 0;
        if (!g_visible) { g_visible = 1; changed = 1; }
    }
    return changed;
}

int guest_banner_visible(void) {
    return g_visible && g_prog && g_prog->magic == GUEST_PROGRESS_MAGIC;
}

static void put_uint(char *b, int *p, u32 v) {
    char tmp[12];
    int n = 0;
    if (v > 100) v = 100;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10u); v /= 10u; }
    while (n) b[(*p)++] = tmp[--n];
}

void guest_banner_draw(void) {
    if (!g_fluent || !guest_banner_visible()) return;

    int rows = 0;
    for (u32 i = 0; i < GUEST_SLOT_COUNT; i++) {
        if (g_prog->slot[i].enabled) rows++;
    }
    if (!rows) return;

    int W = (int)g_fb_w;
    int w = 420;
    if (w > W - 32) w = W - 32;
    if (w < 220) return;
    int row_h = 44;
    int h = 12 + rows * row_h + (rows - 1) * 6 + 12;
    int x = W - w - 16;
    int y = (int)g_fb_h - AQUA_DOCK_BOTTOM - AQUA_DOCK_PANEL_H - h - 14;
    if (y < AQUA_MENUBAR_H + 8) y = AQUA_MENUBAR_H + 8;

    aqua_panel(&g_fb, x, y, w, h, 12);

    int ry = y + 12;
    for (u32 i = 0; i < GUEST_SLOT_COUNT; i++) {
        const utsm_guest_progress *p = &g_prog->slot[i];
        if (!p->enabled) continue;
        const char *name = (i == GUEST_SLOT_LINUX) ? "Linux" : "Windows";
        const char *ph = phase_text(p->phase, p->percent, p->detail);

        zh_draw_string(&g_fb, name, x + 12, ry + 2, 0xFF1E2430u, 0);
        int nw = zh_string_width(name, 0);
        zh_draw_string(&g_fb, ph, x + 12 + nw + 10, ry + 2, 0xFF5A6472u, 0);

        char pct[12];
        int pl = 0;
        if (p->percent != GUEST_PROGRESS_PCT_UNKNOWN) {
            put_uint(pct, &pl, p->percent);
            pct[pl++] = '%';
            pct[pl] = 0;
            int pw = zh_string_width(pct, 0);
            zh_draw_string(&g_fb, pct, x + w - 12 - pw, ry + 2, 0xFF1E2430u, 0);
        }

        int bx = x + 12, bw = w - 24;
        int by = ry + 28;
        aqua_fill_round_alpha(&g_fb, bx, by, bw, 4, 2, 0xFF8A97A8u, 90);
        u32 pc = p->percent;
        if (pc != GUEST_PROGRESS_PCT_UNKNOWN && pc > 0) {
            if (pc > 100) pc = 100;
            int fw = (int)((u64)bw * pc / 100u);
            if (fw < 4) fw = 4;
            aqua_fill_round_alpha(&g_fb, bx, by, fw, 4, 2,
                                  (p->phase == GUEST_PHASE_FAILED)
                                      ? 0xFFE05252u : 0xFF3AA8F0u, 255);
        }
        ry += row_h + 6;
    }
}
