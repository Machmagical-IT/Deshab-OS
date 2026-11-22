/* dkm_input.h — 业务模块的输入总线客户端（P6b「敲门」）
 *
 * 消费者经 srv.input.kbd.poll / srv.input.mouse.poll 取输入事件，
 * 不再直读 PS/2 端口 0x60/0x64（IRQ1 投递后 handler 会读走 0x60，
 * 端口轮询必然饿死）。
 *
 * header-only：每个编译单元持有自己的 kbd 预取缓冲与鼠标快照基线。
 * - kbd：ps2kbd.drv 的 128 槽 ring，poll 排空至多 cap 个 Set-1 扫描码。
 * - mouse：ps2mouse.drv 的只读状态快照；step 把绝对坐标转成增量，
 *   保持各消费者自己的光标坐标/边界语义不变。
 *
 * 缺席策略（登记于 docs/STATUS.md probe 白名单）：
 * - kbd 缺席 = 交互模块无法工作 → 消费方 panic INPUT-E01（零降级）。
 * - mouse 缺席 = 键盘仍可完成操作 → 消费方 warn + 禁用鼠标（INPUT-E02）。
 */
#ifndef DKM_INPUT_H
#define DKM_INPUT_H

#include "dkm_shared.h"

/* 与 ps2mouse.drv 的 mouse_state_t 逐字段一致（消费契约 ABI v1）。 */
typedef struct dkm_input_mouse {
    u32 magic;              /* DKM_INPUT_MOUSE_MAGIC */
    u32 version;
    i32 x;
    i32 y;
    u32 buttons;            /* bit0=左 bit1=右 bit2=中 bit7=绝对坐标 */
    u32 packet_count;
    u32 overflow_count;
    i32 max_x;
    i32 max_y;
} dkm_input_mouse;

#define DKM_INPUT_MOUSE_MAGIC 0x4D4F5553u /* "MOUS" */
#define DKM_INPUT_MOUSE_ABS   0x80u      /* buttons 位：x/y 跟随宿主指针（vmmouse） */

typedef struct dkm_input {
    const struct dkm_kernel_api *api;
    int (*kbd_poll)(u8 *out, u32 cap);
    int (*mouse_poll)(dkm_input_mouse *out);
    u8  kbuf[64];
    u32 kn;
    u32 ki;
    u32 mouse_seen;          /* 上一次 packet_count */
    i32 mouse_x;             /* 上一次绝对坐标（增量基线） */
    i32 mouse_y;
    u8  mouse_valid;
    u8  mouse_abs;           /* 1 = mouse_x/y 是绝对位置，消费者应直接采用 */
} dkm_input;

/* 0 = kbd 就绪；-1 = kbd 服务缺席（鼠标是否就绪见 in->mouse_poll）。 */
static __inline__ int dkm_input_open(dkm_input *in,
                                     const struct dkm_kernel_api *api) {
    void *fn = 0;
    if (!in) return -1;
    in->api = api;
    in->kbd_poll = 0;
    in->mouse_poll = 0;
    in->kn = 0;
    in->ki = 0;
    in->mouse_seen = 0;
    in->mouse_valid = 0;
    in->mouse_abs = 0;
    if (!api || !api->bus) return -1;
    if (api->bus->probe("srv.input.kbd.poll", &fn, 0) != 0 || !fn) return -1;
    in->kbd_poll = (int (*)(u8 *, u32))fn;
    fn = 0;
    if (api->bus->probe("srv.input.mouse.poll", &fn, 0) == 0 && fn) {
        in->mouse_poll = (int (*)(dkm_input_mouse *))fn;
        /* 记录初始快照，第一次 step 只产生按钮/位置基线 */
        dkm_input_mouse m;
        if (in->mouse_poll(&m) == 0 && m.magic == DKM_INPUT_MOUSE_MAGIC) {
            in->mouse_seen = m.packet_count;
            in->mouse_x = m.x;
            in->mouse_y = m.y;
            in->mouse_valid = 1;
        }
    }
    return 0;
}

/* 取一个扫描码；0 = 当前无事件。 */
static __inline__ int dkm_input_kbd(dkm_input *in, u8 *sc) {
    if (!in || !in->kbd_poll) return 0;
    if (in->ki >= in->kn) {
        int n = in->kbd_poll(in->kbuf, sizeof(in->kbuf));
        in->ki = 0;
        in->kn = n > 0 ? (u32)n : 0;
        if (!in->kn) return 0;
    }
    *sc = in->kbuf[in->ki++];
    return 1;
}

/* 有新鼠标数据包时返回 1，并给出相对上一次的增量与当前按钮位。
 * 注意：dx/dy 只是"驱动坐标的变化量"（VSCode 转发等场景需要）；光标定位
 * 应直接使用 in->mouse_x / in->mouse_y（驱动绝对坐标，唯一真值）——
 * 相对包模式下驱动从 fb 中心起步并自行钳位，消费方再把差分累加到自己的
 * 起点会因原点不同而永远到不了屏幕下半/右半。 */
static __inline__ int dkm_input_mouse_step(dkm_input *in,
                                           int *dx, int *dy, u32 *buttons) {
    dkm_input_mouse m;
    if (!in || !in->mouse_poll) return 0;
    if (in->mouse_poll(&m) != 0 || m.magic != DKM_INPUT_MOUSE_MAGIC) return 0;
    if (!in->mouse_valid) {
        in->mouse_valid = 1;
        in->mouse_seen = m.packet_count;
        in->mouse_x = m.x;
        in->mouse_y = m.y;
        return 0;
    }
    if (m.packet_count == in->mouse_seen) return 0;
    in->mouse_seen = m.packet_count;
    if (dx) *dx = (int)(m.x - in->mouse_x);
    if (dy) *dy = (int)(m.y - in->mouse_y);
    if (buttons) *buttons = m.buttons & 0x07u;
    in->mouse_abs = (m.buttons & DKM_INPUT_MOUSE_ABS) ? 1 : 0;
    in->mouse_x = m.x;
    in->mouse_y = m.y;
    return 1;
}

#endif /* DKM_INPUT_H */
