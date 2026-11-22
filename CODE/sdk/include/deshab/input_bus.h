/* deshab/input_bus.h - 输入服务总线客户端（P6b「敲门」的用户态版本）
 *
 * 全屏 ELF 工具（diskinfo/browser/editor/fileman/settings/sysinfo）不得再
 * 直接轮询 PS/2 端口 0x60/0x64：P6b 之后 ps2kbd.drv/ps2mouse.drv 常驻，
 * 其 IRQ1/IRQ12 handler 已把 0x60 的字节读走，端口轮询必然饿死
 * （鼠标不动 / Esc 失灵，且残留半包会让退出后光标错乱）。
 *
 * 本头与内核侧 DKM/dkm_input.h 语义一致，header-only、无静态缓冲（每 TU
 * 持有自己的预取/基线），经 kernel_api.bus->probe 取：
 *   srv.input.kbd.poll   -> int (*)(u8 *out, u32 cap)         返回扫描码个数
 *   srv.input.mouse.poll -> int (*)(dsb_mouse_state *out)     0=成功
 *
 * 缺席策略（与 dkm_input.h 相同）：
 *   - kbd 缺席：open 返回 -1（工具应 message + 退出，不再碰端口）。
 *   - mouse 缺席：kbd 仍可用，鼠标轮询 no-op。
 */
#ifndef DESHAB_INPUT_BUS_H
#define DESHAB_INPUT_BUS_H

#include "types.h"
#include "kernel_api.h"
#include "cursor.h"
#include "boot_context.h"

/* 与 ps2mouse.drv 的 mouse_state_t 逐字段一致（消费契约 ABI v1）。 */
#define DSB_MOUSE_STATE_MAGIC 0x4D4F5553u /* "MOUS" */

typedef struct dsb_mouse_state {
    u32 magic;
    u32 version;
    i32 x;
    i32 y;
    u32 buttons;          /* bit0=左 bit1=右 bit2=中 */
    u32 packet_count;
    u32 overflow_count;
    i32 max_x;
    i32 max_y;
} dsb_mouse_state;

typedef struct dsb_input {
    const dkm_kernel_api *api;
    int (*kbd_poll)(u8 *out, u32 cap);
    int (*mouse_poll)(dsb_mouse_state *out);
    u8  kbuf[64];
    u32 kn;
    u32 ki;
    u32 mouse_seen;       /* 上一次 packet_count */
    i32 mouse_x;          /* 上一次绝对坐标（增量基线） */
    i32 mouse_y;
    u8  mouse_valid;
} dsb_input;

/* 0 = kbd 就绪；-1 = kbd 服务缺席（调用方应放弃端口轮询并退出）。 */
static inline int dsb_input_open(dsb_input *in, const dkm_kernel_api *api) {
    void *fn = 0;
    if (!in) return -1;
    in->api = api;
    in->kbd_poll = 0;
    in->mouse_poll = 0;
    in->kn = 0;
    in->ki = 0;
    in->mouse_seen = 0;
    in->mouse_valid = 0;
    if (!api || !api->bus) return -1;
    if (!api->bus->probe || api->bus->probe("srv.input.kbd.poll", &fn, 0) != 0 || !fn)
        return -1;
    in->kbd_poll = (int (*)(u8 *, u32))fn;
    fn = 0;
    if (api->bus->probe("srv.input.mouse.poll", &fn, 0) == 0 && fn) {
        in->mouse_poll = (int (*)(dsb_mouse_state *))fn;
        dsb_mouse_state m;
        if (in->mouse_poll(&m) == 0 && m.magic == DSB_MOUSE_STATE_MAGIC) {
            in->mouse_seen = m.packet_count;
            in->mouse_x = m.x;
            in->mouse_y = m.y;
            in->mouse_valid = 1;
        }
    }
    return 0;
}

/* 取一个扫描码；返回 1 = 取到，0 = 当前无事件。 */
static inline int dsb_input_kbd(dsb_input *in, u8 *sc) {
    if (g_dsb_pump) g_dsb_pump();
    if (g_dsb_esc) { g_dsb_esc = 0; if (sc) *sc = 1; return 1; }
    if (!in || !in->kbd_poll) return 0;
    if (in->ki >= in->kn) {
        int n = in->kbd_poll(in->kbuf, (u32)sizeof(in->kbuf));
        in->ki = 0;
        in->kn = n > 0 ? (u32)n : 0;
        if (!in->kn) return 0;
    }
    *sc = in->kbuf[in->ki++];
    return 1;
}

/* 有新鼠标包时返回 1，并把光标坐标 + 按钮更新到一个 dsb_cursor。
 * 返回 0 表示无新包（或鼠标服务缺席）。
 *
 * 直接用驱动快照的绝对坐标（驱动已按 framebuffer 尺寸钳位），不用增量
 * 累加——工具光标初值在屏幕中心，与驱动绝对基线的起点未必一致，增量
 * 累加会漂移。首次调用只建基线（返回 0）。 */
static inline int dsb_input_mouse(dsb_input *in, dsb_cursor *c,
                                  i64 fb_w, i64 fb_h) {
    dsb_mouse_state m;
    int x, y, btn, changed;
    if (g_dsb_pump) g_dsb_pump();
    if (!in || !in->mouse_poll || !c) return 0;
    if (in->mouse_poll(&m) != 0 || m.magic != DSB_MOUSE_STATE_MAGIC) return 0;
    x = (int)m.x - g_dsb_ox;
    y = (int)m.y - g_dsb_oy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (fb_w > 0 && x >= (int)fb_w) x = (int)fb_w - 1;
    if (fb_h > 0 && y >= (int)fb_h) y = (int)fb_h - 1;
    btn = (int)(m.buttons & 0x01u);
    if (!in->mouse_valid) {
        in->mouse_valid = 1;
        in->mouse_seen = m.packet_count;
        in->mouse_x = m.x;
        in->mouse_y = m.y;
        c->mx = x;
        c->my = y;
        c->btn = btn;
        return 0;
    }
    if (g_dsb_hosted && btn && g_dsb_close_w > 0 &&
        m.x >= g_dsb_close_x && m.x < g_dsb_close_x + g_dsb_close_w &&
        m.y >= g_dsb_close_y && m.y < g_dsb_close_y + g_dsb_close_h)
        g_dsb_esc = 1;
    changed = (m.packet_count != in->mouse_seen) || (btn != c->btn) ||
              (x != c->mx) || (y != c->my) || g_dsb_esc;
    in->mouse_seen = m.packet_count;
    in->mouse_x = m.x;
    in->mouse_y = m.y;
    c->mx = x;
    c->my = y;
    c->btn = btn;
    return changed ? 1 : 0;
}

#endif /* DESHAB_INPUT_BUS_H */
