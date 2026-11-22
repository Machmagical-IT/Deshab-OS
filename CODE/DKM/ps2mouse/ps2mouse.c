/* DKM PS/2 Mouse Driver — 完整初始化 + IRQ12 包解码 + 共享状态（经服务总线暴露）
 * Stage 4, optional, depends on [], provides "input.mouse"。
 *
 * P3 万物互联：把 DSK 调度的独立 ELF 模块 mouseInit（母本 CODE/mouse/main.c）
 * 归位为常驻 DKM 输入驱动。初始化完成即返回，状态经 srv.input.mouse.poll 暴露，
 * 不再随 DSK 镜像复用被覆盖。
 *
 * ==================== 初始化命令序列 ====================
 *  1. ps2_drain() 冲刷控制器残留数据；
 *  2. 0x20 读配置字节 → 清 bit5（启用 AUX 时钟）、置 bit1（启用 AUX IRQ）、
 *     置 bit6（Set2→Set1 翻译）→ 0x60 写回；
 *  3. 0xA8 启用 AUX 端口；
 *  4. 0xFF 复位鼠标 → 再 drain 最多 8 字节（ACK 0xFA / 自检 0xAA / ID 0x00）；
 *  5. 0xF6 Set Defaults → 读 ACK；
 *  6. 0xF3 + 100 设置采样率 → 两次 ACK；
 *  7. 0xE8 + 2 设置分辨率（4 counts/mm）→ 两次 ACK；
 *  8. 0xF4 启用数据报告 → 读 ACK。
 *  全程 ps2_wait_write/ps2_wait_read 100000 次超时保护，防死锁。
 *  最高准则（PS/2 初始化准则，非降级）：任何单步失败只记日志继续执行，
 *  绝不阻塞或崩溃启动流程。
 *
 * ==================== IRQ12 包解码 ====================
 *  经 kernel_api.irq_register 注册 IRQ12 handler。标准 3 字节包：
 *    byte0: bit7 Y溢出 bit6 X溢出 bit5 Y符号 bit4 X符号 bit3 恒1(同步位)
 *           bit2 中键 bit1 右键 bit0 左键
 *    byte1: X 位移低 8 位；byte2: Y 位移低 8 位
 *  解码规则：首字节无同步位 → 丢弃并重新同步；溢出位置位 → 丢包计 overflow；
 *  dx/dy 按 9 位有符号合成（符号位扩展）；PS/2 Y 向上为正、屏幕 Y 向下为正，
 *  故 y -= dy；坐标累计后按 framebuffer 尺寸钳位到 [0,max_x/max_y]。
 *  handler 读 0x60 前必查状态寄存器 bit5（AUX 位）：键盘数据一律不读，
 *  留在输出缓冲中交给键盘路径，避免吞掉键盘扫描码。
 *
 * ==================== handler 安全不变式（照搬母本） ====================
 *  1) PIC remap 后从片 PIC 屏蔽字为 0xFF，IRQ12 保持屏蔽，本驱动不解屏蔽；
 *  2) 启动/DSK 阶段 IF=0，无任何模块 sti；
 *  => handler 已注册进内核 g_irq_handlers[12]，但在 P6 常驻调度解屏蔽之前
 *     永远不会被调用，不形成野指针执行风险。本 DKM 驱动常驻内存，
 *     因此不存在母本「镜像被后续模块覆盖」的问题——注册点长期有效。
 *  P6 解屏蔽 IRQ12 后，desktop 等消费者必须改为读取本驱动的共享状态，
 *  而非轮询端口（handler 读走 0x60 后轮询者将读不到数据）。
 *
 * ==================== mouse_state_t 消费契约 ====================
 *  g_mouse_state 为导出全局符号，其布局即 ABI（version 字段标识版本）。
 *  消费者先校验 magic == MOUSE_STATE_MAGIC 与 version，再读 x/y/buttons 及
 *  计数器；或经服务总线 srv.input.mouse.poll 调用 mouse_poll_state(out)
 *  取得只读快照（返回 0 成功；-1 出参为空；-2 magic 未初始化）。
 */

#include <stdint.h>

#include "../dkm_shared.h"

/* ---- PS/2 端口与状态位 ---- */
#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64
#define PS2_ST_OBF  0x01    /* 输出缓冲满 */
#define PS2_ST_IBF  0x02    /* 输入缓冲满 */
#define PS2_ST_AUX  0x20    /* 输出数据来自 AUX（鼠标） */
#define PS2_TIMEOUT 100000u /* 读写等待超时次数，防死锁 */

/* ---- 控制器 / 鼠标命令 ---- */
#define PS2_CMD_READ_CFG   0x20   /* 读配置字节 */
#define PS2_CMD_WRITE_CFG  0x60   /* 写配置字节 */
#define PS2_CMD_ENABLE_AUX 0xA8   /* 启用 AUX 端口 */
#define PS2_CMD_TO_MOUSE   0xD4   /* 下一字节发往鼠标 */
#define MOUSE_CMD_RESET    0xFF
#define MOUSE_CMD_DEFAULTS 0xF6
#define MOUSE_CMD_RATE     0xF3   /* 设置采样率，跟一个字节参数 */
#define MOUSE_CMD_RESOL    0xE8   /* 设置分辨率，跟一个字节参数 */
#define MOUSE_CMD_ENABLE   0xF4   /* 启用数据报告 */
#define MOUSE_ACK          0xFA

#define IRQ_MOUSE 12

#define MOUSE_STATE_MAGIC   0x4D4F5553u /* "MOUS" */
#define MOUSE_STATE_VERSION 1u
#define MOUSE_BTN_ABS       0x80u       /* buttons 位：坐标来自绝对指针（vmmouse） */

/* ---- VMware 后门绝对指针（QEMU vmport + vmmouse，协议同 Linux vmmouse.c） ---- */
#define VMW_MAGIC              0x564D5868u
#define VMW_PORT               0x5658u
#define VMW_CMD_GETVERSION     10u
#define VMW_CMD_ABS_DATA       39u
#define VMW_CMD_ABS_STATUS     40u
#define VMW_CMD_ABS_COMMAND    41u
#define VMMOUSE_ENABLE         0x45414552u
#define VMMOUSE_REQ_ABSOLUTE   0x53424152u
#define VMMOUSE_VERSION_ID     0x3442554au
#define VMMOUSE_ERROR          0xFFFF0000u
#define VMMOUSE_RELATIVE_PKT   0x00010000u
#define VMMOUSE_LEFT           0x20u
#define VMMOUSE_RIGHT          0x10u
#define VMMOUSE_MIDDLE         0x08u

/* 无 framebuffer 信息时的兜底边界 */
#define MOUSE_FB_DEFAULT_W 1024
#define MOUSE_FB_DEFAULT_H 768

/* 鼠标共享状态（布局即 ABI，见文件顶部「消费契约」） */
typedef struct mouse_state {
    u32 magic;                  /* MOUSE_STATE_MAGIC，消费者校验用 */
    u32 version;                /* 布局版本，当前为 1 */
    volatile i32 x;             /* 光标 X（屏幕边界钳位后） */
    volatile i32 y;             /* 光标 Y */
    volatile u32 buttons;       /* bit0=左键 bit1=右键 bit2=中键 bit7=绝对坐标 */
    volatile u32 packet_count;  /* 成功解码包数 */
    volatile u32 overflow_count;/* 溢出 / 失步丢弃计数 */
    i32 max_x;                  /* 钳位右边界 = fb_width - 1 */
    i32 max_y;                  /* 钳位下边界 = fb_height - 1 */
} mouse_state_t;

__attribute__((visibility("default")))
mouse_state_t g_mouse_state;

/* ---------------------------------------------------------------
 * Driver descriptor
 * --------------------------------------------------------------- */
static const char *const g_provides[] = { "input.mouse" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic=DKM_DRIVER_MAGIC, .abi_version=DKM_ABI_VERSION,
    .desc_size=sizeof(struct dkm_driver_desc),
    .name="ps2mouse", .version="0.1.0", .vendor="Deshab",
    .driver_class=DKM_CLASS_INPUT, .stage=4, .flags=DKM_F_NO_UNLOAD, .priority=0,
    .depends=NULL, .depends_count=0,
    .provides=g_provides, .provides_count=1,
    .min_kernel_abi=1,
};

/* 驱动状态：API 指针（IRQ 上下文经 g_api 访问内核服务）+ 日志 */
static const struct dkm_log_api *g_log;
static const struct dkm_kernel_api *g_api;

/* 3 字节包组包缓冲（仅 IRQ handler 使用） */
static u8 g_pkt[3];
static u8 g_pkt_idx;

/* 1 = vmmouse 绝对模式已启用：IRQ12 只作"队列有数据"通知，坐标取后门 */
static int g_vmmouse;

static void vmw_call(u32 cmd, u32 arg, u32 r[4]) {
    u32 a = VMW_MAGIC, b = arg, c = cmd, d = VMW_PORT;
    __asm__ volatile("inl %%dx, %%eax"
                     : "+a"(a), "+b"(b), "+c"(c), "+d"(d)
                     :
                     : "rsi", "rdi", "memory");
    r[0] = a; r[1] = b; r[2] = c; r[3] = d;
}

/* 真机上 0x5658 无设备：eax 读回全 1、ebx 保持入参，探测不通过，继续纯 PS/2。 */
static int vmmouse_enable(void) {
    u32 r[4];
    vmw_call(VMW_CMD_GETVERSION, 0xFFFFFFFFu, r);
    if (r[1] != VMW_MAGIC || r[0] == 0xFFFFFFFFu) return 0;
    vmw_call(VMW_CMD_ABS_COMMAND, VMMOUSE_ENABLE, r);
    vmw_call(VMW_CMD_ABS_STATUS, 0, r);
    if ((r[0] & 0xFFFFu) == 0) return 0;
    vmw_call(VMW_CMD_ABS_DATA, 1, r);
    if (r[0] != VMMOUSE_VERSION_ID) return 0;
    vmw_call(VMW_CMD_ABS_COMMAND, VMMOUSE_REQ_ABSOLUTE, r);
    return 1;
}

/* 后门队列每条 4 个字：按钮/状态、X、Y、Z，坐标 0..0xFFFF 映射到 framebuffer。 */
static void vmmouse_drain(void) {
    for (u32 n = 0; n < 32; n++) {
        u32 r[4];
        vmw_call(VMW_CMD_ABS_STATUS, 0, r);
        if ((r[0] & VMMOUSE_ERROR) == VMMOUSE_ERROR) {
            g_mouse_state.overflow_count++;
            g_vmmouse = vmmouse_enable();
            return;
        }
        if ((r[0] & 0xFFFFu) < 4) return;
        vmw_call(VMW_CMD_ABS_DATA, 4, r);
        if (r[0] & VMMOUSE_RELATIVE_PKT) continue;
        u32 btn = 0;
        if (r[0] & VMMOUSE_LEFT)   btn |= 1u;
        if (r[0] & VMMOUSE_RIGHT)  btn |= 2u;
        if (r[0] & VMMOUSE_MIDDLE) btn |= 4u;
        g_mouse_state.x = (i32)(((u64)(r[1] & 0xFFFFu) * (u64)(g_mouse_state.max_x + 1)) >> 16);
        g_mouse_state.y = (i32)(((u64)(r[2] & 0xFFFFu) * (u64)(g_mouse_state.max_y + 1)) >> 16);
        g_mouse_state.buttons = btn | MOUSE_BTN_ABS;
        g_mouse_state.packet_count++;
    }
}

/* ---- 十六进制日志辅助（g_log 经 kernel_api->log） ---- */
static void log_hex(const char *prefix, u64 v) {
    static const char h[] = "0123456789abcdef";
    char b[19];
    u32 i = 0;
    b[i++] = '0'; b[i++] = 'x';
    for (int j = 15; j >= 0; j--) b[i++] = h[(v >> (j * 4)) & 0xf];
    b[i] = 0;
    g_log->info(prefix);
    g_log->info(b);
}

/* ---- PS/2 基础等待与冲刷（全程超时保护） ---- */
static int ps2_wait_write(void) {
    for (u32 t = 0; t < PS2_TIMEOUT; t++) {
        if (!(dkm_inb(PS2_STATUS) & PS2_ST_IBF)) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}
static int ps2_wait_read(void) {
    for (u32 t = 0; t < PS2_TIMEOUT; t++) {
        if (dkm_inb(PS2_STATUS) & PS2_ST_OBF) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}
static void ps2_drain(void) {
    for (int i = 0; i < 16; i++) {
        if (!(dkm_inb(PS2_STATUS) & PS2_ST_OBF)) break;
        dkm_inb(PS2_DATA);
    }
}
static int ps2_cmd(u8 cmd) {
    if (ps2_wait_write() != 0) return -1;
    dkm_outb(PS2_CMD, cmd);
    return 0;
}

/* ---- 鼠标命令：0xD4 前缀 + 命令字节 + 等 ACK(0xFA)，返回 0 成功 ---- */
static int mouse_cmd(u8 cmd) {
    if (ps2_cmd(PS2_CMD_TO_MOUSE) != 0) return -1;
    if (ps2_wait_write() != 0) return -2;
    dkm_outb(PS2_DATA, cmd);
    if (ps2_wait_read() != 0) return -3;
    return dkm_inb(PS2_DATA) == MOUSE_ACK ? 0 : -4;
}
/* 带单字节参数的鼠标命令（0xF3 采样率 / 0xE8 分辨率），命令与参数各等一次 ACK */
static int mouse_cmd_param(u8 cmd, u8 param) {
    int rc = mouse_cmd(cmd);
    if (rc != 0) return rc;
    if (ps2_cmd(PS2_CMD_TO_MOUSE) != 0) return -5;
    if (ps2_wait_write() != 0) return -6;
    dkm_outb(PS2_DATA, param);
    if (ps2_wait_read() != 0) return -7;
    return dkm_inb(PS2_DATA) == MOUSE_ACK ? 0 : -8;
}

/* ---- IRQ12 中断处理：读数据前必查 AUX 位，键盘数据一律不读。
 *     返回 0 = 由内核发送 EOI（UTSM idt.c irq_handler_t 约定）。 ---- */
static int mouse_irq12_handler(u8 irq) {
    (void)irq;
    for (u32 n = 0; n < 8; n++) {              /* 有界读取，防异常硬件下 IRQ 内死循环 */
        u8 st = dkm_inb(PS2_STATUS);
        if (!(st & PS2_ST_OBF)) break;         /* 无数据 */
        if (!(st & PS2_ST_AUX)) break;         /* 键盘数据：留给键盘路径 */
        u8 data = dkm_inb(PS2_DATA);

        if (g_pkt_idx == 0 && !(data & 0x08)) {/* 首字节缺同步位：丢字节重新同步 */
            g_mouse_state.overflow_count++;
            continue;
        }
        g_pkt[g_pkt_idx++] = data;
        if (g_pkt_idx < 3) continue;
        g_pkt_idx = 0;

        u8 f = g_pkt[0];
        if (g_vmmouse) continue;               /* QEMU 伪包（dx=1），只当通知 */
        if (f & 0xC0) {                        /* X/Y 溢出位：丢包 */
            g_mouse_state.overflow_count++;
            continue;
        }
        /* 9 位有符号位移合成：byte0 bit4/bit5 为 X/Y 符号位，扩展进第 9 位 */
        i32 dx = (i32)g_pkt[1] - ((f & 0x10) ? 256 : 0);
        i32 dy = (i32)g_pkt[2] - ((f & 0x20) ? 256 : 0);
        i32 nx = g_mouse_state.x + dx;
        i32 ny = g_mouse_state.y - dy;         /* PS/2 Y 向上为正，屏幕 Y 向下为正 */
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > g_mouse_state.max_x) nx = g_mouse_state.max_x;
        if (ny > g_mouse_state.max_y) ny = g_mouse_state.max_y;
        g_mouse_state.x = nx;
        g_mouse_state.y = ny;
        g_mouse_state.buttons = f & 0x07u;
        g_mouse_state.packet_count++;
    }
    if (g_vmmouse) vmmouse_drain();
    return 0;
}

/* ---- 只读查询接口：把当前状态快照拷贝给调用者，返回 0 成功。
 *      经服务总线 srv.input.mouse.poll 暴露（签名保持母本一致）。 ---- */
__attribute__((visibility("default")))
int mouse_poll_state(mouse_state_t *out) {
    if (!out) return -1;
    if (g_mouse_state.magic != MOUSE_STATE_MAGIC) return -2;
    out->magic          = g_mouse_state.magic;
    out->version        = g_mouse_state.version;
    out->x              = g_mouse_state.x;
    out->y              = g_mouse_state.y;
    out->buttons        = g_mouse_state.buttons;
    out->packet_count   = g_mouse_state.packet_count;
    out->overflow_count = g_mouse_state.overflow_count;
    out->max_x          = g_mouse_state.max_x;
    out->max_y          = g_mouse_state.max_y;
    return 0;
}

static void mouse_state_reset(u64 fb_w, u64 fb_h) {
    if (!fb_w) fb_w = MOUSE_FB_DEFAULT_W;
    if (!fb_h) fb_h = MOUSE_FB_DEFAULT_H;
    g_mouse_state.magic = MOUSE_STATE_MAGIC;
    g_mouse_state.version = MOUSE_STATE_VERSION;
    g_mouse_state.max_x = (i32)fb_w - 1;
    g_mouse_state.max_y = (i32)fb_h - 1;
    g_mouse_state.x = (i32)fb_w / 2;
    g_mouse_state.y = (i32)fb_h / 2;
    g_mouse_state.buttons = 0;
    g_mouse_state.packet_count = 0;
    g_mouse_state.overflow_count = 0;
    g_pkt_idx = 0;
}

/* ---------------------------------------------------------------
 * Driver entry
 * --------------------------------------------------------------- */
__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;
    if (!api || !api->log) return -1;
    g_log = api->log;
    g_api = api;
    dkm_tsc_calibrate();

    g_log->info("[ps2mouse] boot");

    /* 1. 冲刷残留数据 */
    ps2_drain();

    /* 2. 配置字节：bit5=0（启用 AUX 时钟） bit1=1（启用 AUX IRQ） bit6=1（翻译） */
    {
        int cfg_ok = 0;
        u8 cfg = 0;
        if (ps2_cmd(PS2_CMD_READ_CFG) == 0 && ps2_wait_read() == 0) {
            cfg = dkm_inb(PS2_DATA);
            cfg &= (u8)~0x20u;
            cfg |= 0x02u | 0x40u;
            if (ps2_cmd(PS2_CMD_WRITE_CFG) == 0 && ps2_wait_write() == 0) {
                dkm_outb(PS2_DATA, cfg);
                cfg_ok = 1;
            }
        }
        if (cfg_ok) log_hex("[ps2mouse] cfg byte=", cfg);
        else g_log->warn("[ps2mouse] cfg byte access failed, continue");
    }

    /* 3. 启用 AUX 端口 */
    if (ps2_cmd(PS2_CMD_ENABLE_AUX) != 0)
        g_log->warn("[ps2mouse] enable AUX timeout, continue");

    /* 4. 复位鼠标：0xFF → 再 drain 最多 8 字节（ACK / 自检 0xAA / ID 0x00） */
    {
        int rc = mouse_cmd(MOUSE_CMD_RESET);
        if (rc != 0) log_hex("[ps2mouse] reset rc=", (u64)(i64)rc);
        for (int i = 0; i < 8; i++) {
            if (ps2_wait_read() != 0) break;
            dkm_inb(PS2_DATA);
        }
    }

    /* 5-8. Set Defaults → 采样率 100 → 分辨率 2 → 启用数据报告，逐步验 ACK */
    {
        int rc;
        rc = mouse_cmd(MOUSE_CMD_DEFAULTS);
        if (rc != 0) log_hex("[ps2mouse] set defaults rc=", (u64)(i64)rc);
        rc = mouse_cmd_param(MOUSE_CMD_RATE, 100);
        if (rc != 0) log_hex("[ps2mouse] sample rate rc=", (u64)(i64)rc);
        rc = mouse_cmd_param(MOUSE_CMD_RESOL, 2);
        if (rc != 0) log_hex("[ps2mouse] resolution rc=", (u64)(i64)rc);
        rc = mouse_cmd(MOUSE_CMD_ENABLE);
        if (rc != 0) log_hex("[ps2mouse] enable reporting rc=", (u64)(i64)rc);
        else g_log->info("[ps2mouse] data reporting enabled");
    }

    /* 共享状态初始化：边界取自 kernel_api 的 framebuffer 尺寸。
     * 0 表示无 framebuffer 信息，用 1024x768 兜底并 warn。 */
    if (!g_api->fb_width || !g_api->fb_height)
        g_log->warn("[ps2mouse] no framebuffer dims; using default 1024x768");
    mouse_state_reset(g_api->fb_width, g_api->fb_height);

    /* 虚拟机绝对指针：QEMU 窗口无需抓取鼠标即可直接点击。
     * 双路径回退：探测不到（真机 / 无 vmport）即保持 PS/2 相对包。 */
    g_vmmouse = vmmouse_enable();
    if (g_vmmouse) {
        g_mouse_state.buttons = MOUSE_BTN_ABS;
        g_log->info("[ps2mouse] vmmouse absolute pointer enabled");
    } else {
        g_log->info("[ps2mouse] vmmouse absent; PS/2 relative packets");
    }

    /* 注册 IRQ12 handler：仅注册，按安全不变式不解屏蔽 PIC、不 sti
     * （见文件顶部「handler 安全不变式」）。 */
    if (g_api->irq_register) {
        int rc = g_api->irq_register(IRQ_MOUSE, (void *)mouse_irq12_handler);
        if (rc == 0) {
            g_log->info("[ps2mouse] IRQ12 handler registered (PIC line kept masked)");
        } else {
            /* 驱动契约（provides input.mouse）要求安装 IRQ12 handler，
             * 安装失败 = 内核 IRQ 子系统异常，属真正致命（非 PS/2 单步失败）。
             * 继续路径：状态仍导出，poll 读取仍然可用。 */
            g_log->panic("PS2M-E01 IRQ12 REGISTRATION FAILED");
        }
    } else {
        g_log->warn("[ps2mouse] irq_register unavailable, IRQ12 not registered");
    }

    /* 经服务总线暴露状态快照（srv.* 命名空间）。 */
    if (g_api->bus) {
        g_api->bus->register_service("srv.input.mouse.poll", 1, (void *)mouse_poll_state);
        g_log->info("[ps2mouse] srv.input.mouse.poll registered");
    } else {
        g_log->warn("[ps2mouse] no service bus; srv.input.mouse.poll not registered");
    }

    g_log->info("[ps2mouse] init complete");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle; return 0;
}
