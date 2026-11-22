/* DKM PS/2 Keyboard Driver — IRQ1 scan code ring + srv.input.kbd.poll
 * Stage 3, optional, depends on "irq", provides "keyboard" + "input.kbd".
 *
 * P6 万物互联：从"ISR 里直接回显"升级为事件环形缓冲。IRQ1 handler 把原始
 * scan code 推入 ring；消费者（desktop/firstInit/login/shell/cmd）经服务
 * 总线 srv.input.kbd.poll 排空事件，取代直接轮询端口 0x60（后者在 IRQ1
 * 投递后会与 handler 抢读 0x60 而饿死）。
 *
 * 安全不变式（与 ps2mouse 同）：handler 已注册进内核 IRQ 表，但 IRQ1 的
 * 实际投递取决于 PIC/IF。当前启动期 IF=0，handler 不会触发，端口轮询照常；
 * P6b 解屏蔽 + IF=1 后，消费者必须改用本服务。
 */

#include <stdint.h>

#include "../dkm_shared.h"
#include "../dkm_instr.h"

DKM_STAT_DECL(ps2kbd_irq_count);

static const char *const g_depends[] = {"irq"};
static const char *const g_provides[] = {"keyboard", "input.kbd"};

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic=DKM_DRIVER_MAGIC, .abi_version=DKM_ABI_VERSION,
    .desc_size=sizeof(struct dkm_driver_desc),
    .name="ps2kbd", .version="0.2.0", .vendor="Deshab",
    .driver_class=10, .stage=3, .flags=0, .priority=0,
    .depends=g_depends, .depends_count=1,
    .provides=g_provides, .provides_count=2,
    .min_kernel_abi=1,
};

static const struct dkm_log_api *g_log;
static const struct dkm_kernel_api *g_api;

/* ---- 事件环形缓冲（IRQ1 写，消费者读；单生产者单消费者无锁） ---- */
#define KBD_RING_SZ 128
static volatile u8  g_ring[KBD_RING_SZ];
static volatile u32 g_ring_head;   /* 写指针（ISR） */
static volatile u32 g_ring_tail;   /* 读指针（消费者 poll） */

static inline u8  inb(u16 p) { u8 v; __asm__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

static int kbd_handler(u8 irq) {
    (void)irq;
    u8 sc = inb(0x60);
    u32 next = (g_ring_head + 1) % KBD_RING_SZ;
    if (next != g_ring_tail) {          /* 未满才入队（满则丢弃，避免覆盖未读） */
        g_ring[g_ring_head] = sc;
        g_ring_head = next;
    }
    DKM_STAT_INC(ps2kbd_irq_count);
    return 0; /* let kernel send EOI */
}

/* ---- 服务：srv.input.kbd.push —— 其他输入源（USB HID）把 Scan Set-1
 *      扫描码写入同一环形缓冲。与 IRQ1 handler 共用同一生产者语义：
 *      单生产者单消费者；USB 泵在消费者上下文运行，因此不会与 IRQ1
 *      并发（同一时刻只有一方在跑）。 ---- */
int kbd_push(const u8 *sc, u32 n) {
    if (!sc || !n) return 0;
    u32 pushed = 0;
    for (u32 i = 0; i < n; i++) {
        u32 next = (g_ring_head + 1) % KBD_RING_SZ;
        if (next == g_ring_tail) break;      /* 满：丢弃多余（与 IRQ1 同策略） */
        g_ring[g_ring_head] = sc[i];
        g_ring_head = next;
        pushed++;
    }
    return (int)pushed;
}

/* ---- USB HID 泵：srv.input.usb.pump（xhci.drv 可选注册）。
 * 消费者每次 poll 前先给 USB 一次机会把报告转成扫描码，避免依赖
 * 调度器任务与轮询线程。服务缺席 = 无 USB 键盘，直接跳过。
 * 首次 poll 时解析并缓存，之后零总线查找开销。 ---- */
static void (*g_usb_pump)(void);
static int g_usb_pump_resolved;

static void kbd_pump_usb(void) {
    if (!g_usb_pump_resolved) {
        g_usb_pump_resolved = 1;
        void *fn = 0;
        if (g_api && g_api->bus &&
            g_api->bus->probe("srv.input.usb.pump", &fn, 0) == 0 && fn) {
            g_usb_pump = (void (*)(void))fn;
        }
    }
    if (g_usb_pump) g_usb_pump();
}

/* ---- 服务：srv.input.kbd.poll —— 排空至多 cap 个 scan code，返回个数 ---- */
int kbd_poll(u8 *out, u32 cap) {
    if (!out || cap == 0) return 0;
    kbd_pump_usb();
    u32 n = 0;
    while (n < cap && g_ring_tail != g_ring_head) {
        out[n++] = g_ring[g_ring_tail];
        g_ring_tail = (g_ring_tail + 1) % KBD_RING_SZ;
    }
    return (int)n;
}

/* 便捷：snapshot 结构（消费者一次取一批，避免多次总线调用） */
typedef struct { u32 count; u8 sc[64]; } kbd_snapshot_t;
static kbd_snapshot_t g_kbd_snap;
int kbd_snapshot(void) {
    int n = kbd_poll(g_kbd_snap.sc, sizeof(g_kbd_snap.sc));
    g_kbd_snap.count = (u32)(n < 0 ? 0 : n);
    return n;
}
const void *kbd_snapshot_get(void) { return &g_kbd_snap; }

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;
    if (!api||!api->log) return -1;
    g_log=api->log;
    g_api=api;
    dkm_instr_init(api);

    if (!api->irq_register) {
        g_log->warn("[kbd] irq_register not available");
    } else if (api->irq_register(1, (void*)(uintptr_t)kbd_handler) != 0) {
        g_log->error("[kbd] failed to register IRQ1");
        return -1;
    } else {
        g_log->info("[kbd] IRQ1 registered (PS/2 scan code ring)");
    }

    /* P6a：注册键盘事件服务（消费者经总线读取，取代端口轮询） */
    if (api->bus) {
        api->bus->register_service("srv.input.kbd.poll", 1, (void*)kbd_poll);
        api->bus->register_service("srv.input.kbd.snapshot", 1, (void*)kbd_snapshot_get);
        /* USB HID（xhci.drv）经本服务把 HID 报告翻译成 Set-1 扫描码注入，
         * 与 IRQ1 共用同一 ring —— 所有消费者无需区分键鼠来源。 */
        api->bus->register_service("srv.input.kbd.push", 1, (void*)kbd_push);
        g_log->info("[kbd] srv.input.kbd.poll / .push registered");
    }

    g_log->info("[kbd] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle; return 0;
}
