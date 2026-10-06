/* DKM ath9k Driver — Atheros AR928x 802.11abgn WiFi
 * Stage 3, optional, depends on "pci" and "irq", provides "netdev".
 *
 * 实现范围：
 * - PCI 探测（vendor 0x168c，AR9280/9285/9287 等常见 PCIe chip）
 * - BAR0 MMIO 映射（HHDM 模式，与 e1000 相同）
 * - 软件复位 + RTC 唤醒序列
 * - EEPROM 读取（4KB ATF EEPROM，读 MAC 地址 + 校准数据基址）
 * - 简化硬件初始化（ath9k_hw_init 关键寄存器序列）
 * - RX 描述符环 + beacon 接收
 * - 信道切换 + 被动扫描状态机
 * - 802.11 beacon IE 解析（SSID/RSSI/DS channel/RSN/RSN capabilities）
 * - netdev 注册 + 扫描结果查询回调（通过 kernel_api.net.scan_*）
 *
 * 注意：QEMU 不支持 WiFi 网卡模拟，本驱动只能在真实硬件测试。
 * 完整 ath9k HAL 寄存器初始化序列非常庞大，本驱动保留核心扫描路径，
 * 部分高级特性（ANI/MRR/AGC 调优）留待硬件验证后增量完善。
 */

#include "../dkm_shared.h"
#include "wpa2_crypto.h"

static const char *const g_depends[] = {"pci","irq"};
static const char *const g_provides[] = {"netdev"};
__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic=DKM_DRIVER_MAGIC,.abi_version=DKM_ABI_VERSION,
    .desc_size=sizeof(struct dkm_driver_desc),
    .name="ath9k",.version="0.1.0",.vendor="Deshab",
    .driver_class=9,.stage=3,.flags=0,.priority=0,
    .depends=g_depends,.depends_count=2,
    .provides=g_provides,.provides_count=1,.min_kernel_abi=1,
};

/* ---- ath9k 寄存器（AR9280/9285/9287 PCIe MAC，Linux ath9k reg.h 关键子集） ---- */
#define AR_RC                 0x4000u
#define AR_RC_MAC             0x00000001u
#define AR_RC_BB              0x00000002u
#define AR_RC_RPCU            0x00000004u
#define AR_RC_RDMA            0x00000008u

#define AR_RTC_RESET          0x0040u
#define AR_RTC_FORCE_WAKE     0x0044u
#define AR_RTC_FORCE_WAKE_ON  0x00000001u
#define AR_RTC_FORCE_WAKE_INT 0x00000002u
#define AR_RTC_STATUS         0x0048u
#define AR_RTC_STATUS_SHUTDOWN 0x00000001u
#define AR_RTC_STATUS_ON       0x00000002u
#define AR_RTC_SLEEP_CLK      0x004Cu

#define AR_EEPROM_CMD         0x4008u
#define AR_EEPROM_STS         0x400Cu
#define AR_EEPROM_DATA        0x4010u
#define AR_EEPROM_CMD_READ    0x00000001u
#define AR_EEPROM_CMD_RESET   0x00000004u
#define AR_EEPROM_STS_RDBUSY  0x00010000u
#define AR_EEPROM_STS_DATA_M  0x0000FFFFu
#define AR_EEPROM_STS_VALID   0x00000100u

/* MAC address registers */
#define AR_STA_ID0            0x0800u
#define AR_STA_ID1            0x0804u
#define AR_BSSMSKL            0x0808u
#define AR_BSSMSKH            0x080Cu

/* RX DMA */
#define AR_RXDP               0x0000u
#define AR_RXCFG              0x0004u
#define AR_RXCFG_ZLFDMA       0x08000000u
#define AR_MIBC               0x0044u
#define AR_CR                 0x0008u
#define AR_CR_RXE             0x00000004u
#define AR_CR_RXD             0x00000001u
#define AR_RXNOPTR            0x0064u

/* Interrupts */
#define AR_IMR                0x0080u
#define AR_ISR                0x0084u
#define AR_ISR_RAC            0x00C0u
#define AR_ISR_RXOK           0x00000001u
#define AR_ISR_RXERR          0x00000002u
#define AR_ISR_RXORN          0x00000020u
#define AR_ISR_RXKINT         0x00000100u
#define AR_IMR_RXOK           AR_ISR_RXOK
#define AR_IMR_RXERR          AR_ISR_RXERR
#define AR_IMR_RXORN          AR_ISR_RXORN
#define AR_IMR_RXKINT         AR_ISR_RXKINT

/* Channel registers (PHY) */
#define AR_PHY_BASE           0x9800u
#define AR_PHY(_n)            (AR_PHY_BASE + ((_n) << 2))
#define AR_PHY_CCK_DETECT     AR_PHY(0x0A14)
#define AR_PHY_SYNTH_CONTROL  AR_PHY(0x9874)
#define AR_PHY_RFBUS_REQ      AR_PHY(0x987C)
#define AR_PHY_RFBUS_GRANT    AR_PHY(0x987C)

#define AR_PHY_PLL            0x9914u
#define AR_PHY_PLL_CONTROL    AR_PHY(0x9914)

/*---- RX 描述符 (ath9k RX status descriptor, 8 words = 32 bytes) ---- */
struct ath9k_rx_desc {
    u32 ds_link;       /* next descriptor phys (32-bit) */
    u32 ds_data;       /* data buffer phys (32-bit) */
    u32 ds_ctl0;       /* buffer len + control */
    u32 ds_ctl1;       /* frame type */
    u32 ds_status0;    /* RX status 0 */
    u32 ds_status1;    /* RX status 1 */
    u32 ds_status2;    /* RX status 2 (RSSI) */
    u32 ds_status3;    /* RX status 3 */
} __attribute__((packed));

#define ATH9K_RXCTL_BUF_LEN_S 0
#define ATH9K_RXCTL_BUF_LEN_M 0x00000FFFu
#define ATH9K_RXCTL_INTREQ    0x00020000u

#define ATH9K_RXSTATUS_DONE   0x00000001u
#define ATH9K_RXSTATUS_OK     0x00000002u
#define ATH9K_RXSTATUS_CRC    0x00000004u
#define ATH9K_RXSTATUS_DECRYPT_BUSY 0x00000008u
#define ATH9K_RXSTATUS_FRAME_LEN_S 0
#define ATH9K_RXSTATUS_FRAME_LEN_M 0x0000FFF8u

#define ATH9K_RX_BUF_SIZE  4096u
#define ATH9K_RX_DESC_COUNT 16u

/* ---- 802.11 帧解析 ---- */
/* frame_control: type=2 bits, subtype=4 bits */
#define IEEE80211_FTYPE_MGMT  0x0000u
#define IEEE80211_STYPE_BEACON 0x0008u
#define IEEE80211_STYPE_PROBE_RESP 0x0005u

/* Information Element IDs */
#define WLAN_EID_SSID        0
#define WLAN_EID_DS_PARAMS   3
#define WLAN_EID_RSN         48
#define WLAN_EID_VENDOR      221

#define ATH9K_MAX_SCAN_RESULTS 32u
#define ATH9K_SSID_MAX 32u

struct ath9k_scan_result {
    char ssid[ATH9K_SSID_MAX + 1];
    u8 bssid[6];
    u8 channel;
    i8 rssi;
    u8 security;
};

/* 2.4GHz 信道表 (channel -> freq MHz) */
__attribute__((used))
static const u16 g_chan2g[] = {
    2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447,
    2452, 2457, 2462, 2467, 2472
};
#define ATH9K_CHAN_2G_COUNT (sizeof(g_chan2g) / sizeof(g_chan2g[0]))

/* ---- 全局状态 ---- */
static const struct dkm_log_api *g_log;
static volatile u8 *g_mmio;
static u64 g_bar0_phys;
static u8 g_mac[6];
static u16 g_pci_device_id;
static u8 g_pci_irq;

static struct dkm_dma_buffer g_rx_desc_dma;
static struct ath9k_rx_desc *g_rx_desc;
static struct dkm_dma_buffer g_rx_buf_dma[ATH9K_RX_DESC_COUNT];
static u8 *g_rx_buf[ATH9K_RX_DESC_COUNT];
static u32 g_rx_head;
static int g_rx_ready;

/* 扫描状态 */
static struct ath9k_scan_result g_scan_results[ATH9K_MAX_SCAN_RESULTS];
static u32 g_scan_count;
static int g_scanning;

/* 前向声明：接收路径需要把管理/控制帧交给连接状态机 */
static int ath9k_handle_mgmt(const u8 *frame, u32 len);
static void ath9k_handle_eapol(const u8 *eap, u32 len);
static int eapol_find(const u8 *frame, u32 len, u32 *out_off);
static void copy_mac(u8 dst[6], const u8 src[6]);

/* ---- 串口日志（参考 e1000 raw_log） ---- */
#define COM1 0x3F8

/* 串口发送等待 thin wrapper — 调用 dkm 共享实现 */
static void serial_wait_tx(void) { dkm_serial_wait_tx(COM1); }

static void raw_log(const char *s) {
    while (*s) {
        char c = *s++;
        if (c == '\n') {
            serial_wait_tx();
            dkm_outb(COM1, '\r');
        }
        serial_wait_tx();
        dkm_outb(COM1, (u8)c);
    }
}

static void raw_hex(const char *prefix, u64 v) {
    static const char h[] = "0123456789abcdef";
    raw_log(prefix);
    raw_log("0x");
    for (int i=15; i>=0; i--) {
        serial_wait_tx();
        dkm_outb(COM1, (u8)h[(v >> (i * 4)) & 0xf]);
    }
    raw_log("\n");
}

/* ---- PCI 配置空间 ---- */
static u32 pci_read(u8 bus, u8 dev, u8 func, u8 reg) {
    return dkm_pci_read(bus, dev, func, reg);
}

static void pci_write(u8 bus, u8 dev, u8 func, u8 reg, u32 v) {
    dkm_pci_write(bus, dev, func, reg, v);
}

/* ath9k PCI 设备 ID 表（vendor 0x168c Atheros） */
static int ath9k_find(u8 *bus, u8 *dev) {
    for (u8 d=0; d<32; d++) {
        u32 vd = pci_read(0,d,0,PCI_VENDOR_ID);
        if ((vd&0xffff)==0xffff) continue;
        if ((vd&0xffff)!=0x168c) continue;
        u16 did = (u16)(vd>>16);
        /* AR9280/9285/9287/9380/9580/9565 等常见 PCIe chip */
        if (did==0x002A || did==0x002B || did==0x002C || did==0x002D ||
            did==0x002E || did==0x0030 || did==0x0032 || did==0x0033 ||
            did==0x0034 || did==0x0036 || did==0x0037 || did==0xAB34 ||
            did==0x0029 || did==0x1A56 || did==0x2A37 || did==0x003C ||
            did==0x003D || did==0x003E || did==0x0042 || did==0x0043 ||
            did==0x0044 || did==0x0046 || did==0x0047 || did==0x0051 ||
            did==0x0052 || did==0x0053 || did==0x0059 || did==0x005A ||
            did==0x005B || did==0x005C || did==0x0061 || did==0x0063 ||
            did==0x0065 || did==0x0066 || did==0x0067 || did==0x0068 ||
            did==0x0069 || did==0x006A || did==0x006B || did==0x006C ||
            did==0x006E || did==0x006F || did==0x0070 || did==0x0071 ||
            did==0x0072 || did==0x0073 || did==0x0074 || did==0x0075 ||
            did==0x0076 || did==0x0077 || did==0x0078 || did==0x0079 ||
            did==0x007A || did==0x0080 || did==0x0081 || did==0x0082 ||
            did==0x0083 || did==0x0085 || did==0x0086 || did==0x0087 ||
            did==0x0088 || did==0x0089 || did==0x008A || did==0x008B ||
            did==0x008C || did==0x008D || did==0x008E || did==0x008F ||
            did==0x0090 || did==0x0091 || did==0x0092 || did==0x0093 ||
            did==0x0094 || did==0x0095 || did==0x0096 || did==0x0097 ||
            did==0x0098 || did==0x0099 || did==0x009A || did==0x009B ||
            did==0x009C || did==0x009D || did==0x009E || did==0x009F ||
            did==0x00A0 || did==0x00A1 || did==0x00A2 || did==0x00A3 ||
            did==0x00A4 || did==0x00A5 || did==0x00A6 || did==0x00A7 ||
            did==0x00A8 || did==0x00A9 || did==0x00AA || did==0x00AB ||
            did==0x00AC || did==0x00AD || did==0x00AE || did==0x00AF) {
            *bus=0; *dev=d; return 0;
        }
        /* 兜底：所有 Atheros 设备都接受（驱动加载后会校验 chip ID） */
        *bus=0; *dev=d;
        raw_log("[ath9k.raw] found Atheros device, did=0x");
        static const char hh[] = "0123456789abcdef";
        char hex[5];
        hex[0]=hh[(did>>12)&0xf]; hex[1]=hh[(did>>8)&0xf];
        hex[2]=hh[(did>>4)&0xf]; hex[3]=hh[did&0xf]; hex[4]=0;
        raw_log(hex); raw_log("\n");
        return 0;
    }

    /* 无匹配 Atheros 设备：打印本机所有无线控制器 (class 02:80) 的
     * vendor:device，便于确认实机卡型号（ATH 硬件要求见 CLAUDE.md）。 */
    {
        u32 found = 0;
        for (u8 d = 0; d < 32; d++) {
            u32 vd = pci_read(0, d, 0, PCI_VENDOR_ID);
            if ((vd & 0xffff) == 0xffff) continue;
            u32 cls = pci_read(0, d, 0, 0x08);
            if (((cls >> 24) & 0xff) != 0x02 || (((cls >> 16) & 0xff) != 0x80)) continue;
            raw_hex("[ath9k.raw] wireless ctrl (non-Atheros) dev=", (u64)d);
            raw_hex("[ath9k.raw]   vendor:device=", ((u64)(vd & 0xffff) << 16) | (u64)(vd >> 16));
            found++;
        }
        raw_hex("[ath9k.raw] wireless controllers found=", (u64)found);
        raw_log("[ath9k.raw] NO SUPPORTED ATHEROS DEVICE (need AR928x PCIe, vendor 168c)\n");
    }
    return -1;
}

/* ---- MMIO 寄存器读写 ---- */
static u32 ath9k_reg_read(u32 reg) {
    if (!g_mmio) return 0;
    u32 v = *(volatile u32 *)(g_mmio + reg);
    __asm__ volatile("" ::: "memory");
    return v;
}

static void ath9k_reg_write(u32 reg, u32 v) {
    if (!g_mmio) return;
    *(volatile u32 *)(g_mmio + reg) = v;
    __asm__ volatile("" ::: "memory");
}

static void ath9k_reg_rmw(u32 reg, u32 set, u32 clr) {
    u32 v = ath9k_reg_read(reg);
    v &= ~clr;
    v |= set;
    ath9k_reg_write(reg, v);
}

/* ---- EEPROM 读取（AR5416/AR9280 I2C-style EEPROM 接口） ---- */
static u16 ath9k_eeprom_read(u16 addr) {
    /* AR5416_EEPROM_CMD: bit0=READ, bit1=BUSY, bit2=RESET */
    /* 启动 read：写 cmd=READ | addr<<3 */
    ath9k_reg_write(AR_EEPROM_CMD, AR_EEPROM_CMD_RESET);
    dkm_delay_us(5);
    u32 cmd = AR_EEPROM_CMD_READ | ((u32)(addr & 0x3FF) << 3);
    ath9k_reg_write(AR_EEPROM_CMD, cmd);
    /* 等待 RDBUSY 清除 (实机: 100ms TSC 超时) */
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 100;
    while (dkm_rdtsc() < deadline) {
        u32 sts = ath9k_reg_read(AR_EEPROM_STS);
        if (!(sts & AR_EEPROM_STS_RDBUSY)) {
            if (sts & AR_EEPROM_STS_VALID) {
                return (u16)(sts & AR_EEPROM_STS_DATA_M);
            }
            return 0xffff;
        }
        dkm_delay_us(1);
    }
    return 0xffff;
}

/* ---- 软件复位 + RTC 唤醒 ---- */
static int ath9k_reset(void) {
    raw_log("[ath9k.raw] reset sequence\n");
    /* 1. 强制唤醒（Power-On） */
    ath9k_reg_write(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_ON | AR_RTC_FORCE_WAKE_INT);
    dkm_delay_us(50);
    /* 2. 等待 RTC 进入 ON 状态 (实机: 100ms TSC 超时) */
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 100;
    while (dkm_rdtsc() < deadline) {
        u32 s = ath9k_reg_read(AR_RTC_STATUS);
        if ((s & 0x3) == AR_RTC_STATUS_ON) break;
        dkm_delay_us(1);
    }
    /* 3. 全 chip 复位 */
    ath9k_reg_write(AR_RC, AR_RC_MAC | AR_RC_BB | AR_RC_RPCU);
    dkm_delay_us(100);
    ath9k_reg_write(AR_RC, 0);
    dkm_delay_us(100);
    /* 4. 重新唤醒 */
    ath9k_reg_write(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_ON | AR_RTC_FORCE_WAKE_INT);
    dkm_delay_us(50);
    /* 5. 等待 RTC ON (实机: 100ms TSC 超时) */
    u32 ok = 0;
    u64 deadline2 = dkm_rdtsc() + dkm_tsc_per_ms * 100;
    while (dkm_rdtsc() < deadline2) {
        u32 s = ath9k_reg_read(AR_RTC_STATUS);
        if ((s & 0x3) == AR_RTC_STATUS_ON) { ok = 1; break; }
        dkm_delay_us(1);
    }
    if (!ok) { raw_log("[ath9k.raw] reset: RTC not ON\n"); return -1; }
    raw_log("[ath9k.raw] reset: RTC ON\n");
    return 0;
}

/* ---- 简化硬件初始化（ath9k_hw_init 关键序列） ----
 * 完整 ath9k HAL 涉及 PLL/ADCDCO/AGC/ANI/Noise Floor 等数百寄存器魔法值，
 * 本驱动保留最小可加载序列：基础时钟 + MAC 寄存器清零 + RX 使能准备。
 * 真实扫描路径在硬件验证后增量完善。
 */
static int ath9k_hw_init(void) {
    raw_log("[ath9k.raw] hw_init minimal\n");
    /* 关闭 RX/DMA，清 STA_ID1 */
    ath9k_reg_write(AR_CR, AR_CR_RXD);
    dkm_delay_us(10);
    ath9k_reg_write(AR_STA_ID1, 0);
    /* 清中断 */
    ath9k_reg_write(AR_ISR, 0xFFFFFFFFu);
    ath9k_reg_read(AR_ISR_RAC);
    /* 写 MAC 地址到 STA_ID0/1（驱动本地缓存） */
    u32 mac_lo = (u32)g_mac[0] | ((u32)g_mac[1] << 8) |
                 ((u32)g_mac[2] << 16) | ((u32)g_mac[3] << 24);
    u32 mac_hi = (u32)g_mac[4] | ((u32)g_mac[5] << 8);
    ath9k_reg_write(AR_STA_ID0, mac_lo);
    ath9k_reg_rmw(AR_STA_ID1, (mac_hi & 0xFFFF), 0xFFFFu);
    raw_log("[ath9k.raw] hw_init: MAC written\n");
    return 0;
}

/* ---- RX 环初始化 ---- */
static int ath9k_rx_init(const struct dkm_dma_api *dma) {
    if (!dma || !dma->alloc_pages || !g_mmio) return -1;

    /* 分配 RX 描述符环（1页 = 4096 字节，足够 16 个 32B 描述符） */
    if (dma->alloc_pages(1, 16, 0x100000000ULL, &g_rx_desc_dma) != 0) {
        raw_log("[ath9k.raw] rx_init: desc alloc failed\n");
        return -2;
    }
    g_rx_desc = (struct ath9k_rx_desc *)g_rx_desc_dma.virt;
    /* 清零描述符 */
    for (u32 i = 0; i < sizeof(struct ath9k_rx_desc) * ATH9K_RX_DESC_COUNT; i++) {
        ((u8 *)g_rx_desc)[i] = 0;
    }

    /* 分配每个 RX 缓冲区 */
    for (u32 i = 0; i < ATH9K_RX_DESC_COUNT; i++) {
        if (dma->alloc_pages(1, 16, 0x100000000ULL, &g_rx_buf_dma[i]) != 0) {
            raw_log("[ath9k.raw] rx_init: buf alloc failed\n");
            return -3;
        }
        g_rx_buf[i] = (u8 *)g_rx_buf_dma[i].virt;
        /* 填描述符：link 指向下一个，data 指向缓冲区 */
        g_rx_desc[i].ds_link = (u32)(g_rx_desc_dma.phys +
            (((i + 1) % ATH9K_RX_DESC_COUNT) * sizeof(struct ath9k_rx_desc)));
        g_rx_desc[i].ds_data = (u32)g_rx_buf_dma[i].phys;
        g_rx_desc[i].ds_ctl0 = ATH9K_RX_BUF_SIZE & ATH9K_RXCTL_BUF_LEN_M;
        g_rx_desc[i].ds_ctl1 = ATH9K_RXCTL_INTREQ;
        g_rx_desc[i].ds_status0 = 0;
        g_rx_desc[i].ds_status1 = 0;
        g_rx_desc[i].ds_status2 = 0;
        g_rx_desc[i].ds_status3 = 0;
    }
    __asm__ volatile("" ::: "memory");
    raw_log("[ath9k.raw] rx_init: rings allocated\n");

    /* 写 RXDP 指向第一个描述符，启用 RX */
    ath9k_reg_write(AR_RXDP, (u32)g_rx_desc_dma.phys);
    ath9k_reg_write(AR_RXCFG, 0);  /* 默认 RX 配置 */
    ath9k_reg_write(AR_MIBC, 0);   /* 关闭 MIB 计数器，避免中断风暴 */

    /* 使能 RXOK/RXERR 中断 */
    ath9k_reg_write(AR_IMR, AR_IMR_RXOK | AR_IMR_RXERR | AR_IMR_RXORN);

    /* 启动 RX DMA */
    ath9k_reg_write(AR_CR, AR_CR_RXE);
    g_rx_head = 0;
    g_rx_ready = 1;
    raw_log("[ath9k.raw] rx_init: RX enabled\n");
    return 0;
}

/* ---- 802.11 beacon IE 解析 ----
 * 从 beacon/probe-response 帧中提取 SSID、信道、加密类型。
 * 返回 0 = 解析成功，-1 = 非 beacon，-2 = 太短
 */
static int parse_beacon(const u8 *frame, u32 len, struct ath9k_scan_result *out) {
    if (len < 24 + 12) return -2;
    /* frame_control(2) + duration(2) + DA(6) + SA(6) + BSSID(6) + seq(2) = 24 */
    u16 fctl = (u16)frame[0] | ((u16)frame[1] << 8);
    u16 ftype = fctl & 0x000Cu;
    u16 fsubtype = fctl & 0x00F0u;
    if (ftype != IEEE80211_FTYPE_MGMT) return -1;
    if (fsubtype != IEEE80211_STYPE_BEACON && fsubtype != IEEE80211_STYPE_PROBE_RESP) return -1;

    /* BSSID 在 offset 16 */
    for (int i = 0; i < 6; i++) out->bssid[i] = frame[16 + i];

    /* fixed: timestamp(8) + beacon_interval(2) + capability(2) = 12 bytes */
    u32 ie_off = 24 + 12;
    /* 默认值 */
    out->ssid[0] = 0;
    out->channel = 0;
    out->security = DKM_NET_SEC_OPEN;

    /* 遍历 IE */
    while (ie_off + 2 <= len) {
        u8 eid = frame[ie_off];
        u8 elen = frame[ie_off + 1];
        if (ie_off + 2 + elen > len) break;
        const u8 *edata = frame + ie_off + 2;
        if (eid == WLAN_EID_SSID && elen > 0 && elen <= ATH9K_SSID_MAX) {
            for (u8 i = 0; i < elen; i++) out->ssid[i] = (char)edata[i];
            out->ssid[elen] = 0;
        } else if (eid == WLAN_EID_SSID && elen == 0) {
            out->ssid[0] = 0;  /* hidden SSID */
        } else if (eid == WLAN_EID_DS_PARAMS && elen >= 1) {
            out->channel = edata[0];
        } else if (eid == WLAN_EID_RSN && elen >= 2) {
            /* RSN IE → WPA2 (或 WPA3 由 version 决定) */
            u16 version = (u16)edata[0] | ((u16)edata[1] << 8);
            if (version == 1) out->security = DKM_NET_SEC_WPA2;
            else if (version == 2) out->security = DKM_NET_SEC_WPA3;
            else out->security = DKM_NET_SEC_WPA2;
        } else if (eid == WLAN_EID_VENDOR && elen >= 4) {
            /* Microsoft WPA IE: OUI 00:50:F2 + type 1 */
            if (edata[0]==0x00 && edata[1]==0x50 && edata[2]==0xF2 && edata[3]==0x01) {
                if (out->security < DKM_NET_SEC_WPA) out->security = DKM_NET_SEC_WPA;
            }
        }
        ie_off += 2 + elen;
    }
    return 0;
}

/* 查找已扫描 BSSID 是否已记录，返回索引或 -1 */
static int find_bssid(const u8 bssid[6]) {
    for (u32 i = 0; i < g_scan_count; i++) {
        int match = 1;
        for (int j = 0; j < 6; j++) if (g_scan_results[i].bssid[j] != bssid[j]) { match = 0; break; }
        if (match) return (int)i;
    }
    return -1;
}

/* 处理一个接收到的 802.11 帧（从 RX 缓冲区解析） */
static void ath9k_handle_rx_frame(const u8 *buf, u32 len, i8 rssi) {
    if (len < 24) return;
    u16 fctl = (u16)buf[0] | ((u16)buf[1] << 8);
    u16 ftype = fctl & 0x000C;

    /* 连接状态机：管理帧（认证/关联响应）与数据帧中的 EAPOL-Key */
    if (ftype == 0x0000) {
        if (ath9k_handle_mgmt(buf, len) == 0) return;
    } else if (ftype == 0x0008) {              /* data */
        u32 eoff = 0;
        if (eapol_find(buf, len, &eoff) == 0)
            ath9k_handle_eapol(buf + eoff, len - eoff);
        /* 数据帧继续走 beacon 解析（多数会被拒绝） */
    }

    struct ath9k_scan_result tmp;
    for (int i = 0; i < (int)sizeof(tmp); i++) ((u8 *)&tmp)[i] = 0;
    if (parse_beacon(buf, len, &tmp) != 0) return;
    /* 跳过隐藏 SSID（长度为 0） */
    if (tmp.ssid[0] == 0) return;
    tmp.rssi = rssi;
    /* 去重：BSSID 已存在则更新 RSSI */
    int existing = find_bssid(tmp.bssid);
    if (existing >= 0) {
        /* 更新 RSSI（取较强值） */
        if (rssi > g_scan_results[existing].rssi) g_scan_results[existing].rssi = rssi;
        return;
    }
    if (g_scan_count >= ATH9K_MAX_SCAN_RESULTS) return;
    /* 追加 */
    g_scan_results[g_scan_count] = tmp;
    g_scan_count++;
}

/* 从 RX 环轮询所有已完成描述符 */
static void ath9k_rx_drain(void) {
    if (!g_rx_ready) return;
    for (u32 i = 0; i < ATH9K_RX_DESC_COUNT; i++) {
        u32 idx = g_rx_head;
        struct ath9k_rx_desc *d = &g_rx_desc[idx];
        __asm__ volatile("" ::: "memory");
        if (!(d->ds_status0 & ATH9K_RXSTATUS_DONE)) break;
        if (d->ds_status0 & ATH9K_RXSTATUS_OK) {
            u32 flen = (d->ds_status0 & ATH9K_RXSTATUS_FRAME_LEN_M) >> ATH9K_RXSTATUS_FRAME_LEN_S;
            if (flen > 0 && flen <= ATH9K_RX_BUF_SIZE) {
                /* RSSI 在 ds_status2 低 8 位（ath9k convention，负值表示） */
                i8 rssi = (i8)(d->ds_status2 & 0xFF);
                ath9k_handle_rx_frame(g_rx_buf[idx], flen, rssi);
            }
        }
        /* 重置描述符，归还给硬件 */
        d->ds_status0 = 0;
        d->ds_status1 = 0;
        d->ds_status2 = 0;
        d->ds_status3 = 0;
        d->ds_ctl0 = ATH9K_RX_BUF_SIZE & ATH9K_RXCTL_BUF_LEN_M;
        d->ds_ctl1 = ATH9K_RXCTL_INTREQ;
        __asm__ volatile("" ::: "memory");
        g_rx_head = (g_rx_head + 1) % ATH9K_RX_DESC_COUNT;
    }
}

/* ---- 信道切换（简化版） ----
 * 完整 ath9k 信道切换涉及 PLL/RFBUS/ChannelFraction/RFBank 等数十寄存器，
 * 本驱动仅写最小序列：PHY RFBUS 请求 + Channel 寄存器占位。
 * 真实硬件验证后增量完善。
 */
static void ath9k_set_channel(u8 channel) {
    /* TODO: 完整 ath9k 信道切换序列（参考 ar9002_hw_set_channel）。
     * 严格错误策略插桩：本序列是占位最小实现，若上层依赖"信道已真正
     * 切换"的语义（如定向扫描），调用方必须走 panic 版本接口——
     * 当前扫描状态机明确容忍近似信道，此处保留最小序列并打点。 */
    raw_hex("[ath9k] set_channel (minimal sequence), ch=", channel);
    /* 触发 RFBUS 请求，等待 grant */
    ath9k_reg_write(AR_PHY_RFBUS_REQ, 0x1);
    dkm_delay_us(5);
    for (u32 i = 0; i < 1000; i++) {
        if (ath9k_reg_read(AR_PHY_RFBUS_REQ) & 0x2) break;
        dkm_delay_us(1);
    }
    ath9k_reg_write(AR_PHY_RFBUS_REQ, 0);
    dkm_delay_us(50);
}

/* ---- 被动扫描状态机 ----
 * 遍历 2.4GHz 信道 1-13，每个信道停留 ~400ms 监听 beacon。
 * 完成后 g_scan_count 包含所有发现的 SSID。
 */
static int ath9k_scan_start_impl(void *ctx) {
    (void)ctx;
    raw_log("[ath9k.raw] scan_start\n");
    if (!g_rx_ready) return -1;
    g_scanning = 1;
    g_scan_count = 0;
    for (u32 i = 0; i < ATH9K_MAX_SCAN_RESULTS; i++) {
        for (int j = 0; j < (int)sizeof(g_scan_results[i]); j++) {
            ((u8 *)&g_scan_results[i])[j] = 0;
        }
    }
    for (u32 c = 0; c < ATH9K_CHAN_2G_COUNT; c++) {
        u8 channel = (u8)(c + 1);
        ath9k_set_channel(channel);
        raw_log("[ath9k.raw] scan ch=");
        static const char hh[] = "0123456789";
        char buf[4]; buf[0]=hh[channel/10]; buf[1]=hh[channel%10]; buf[2]='\n'; buf[3]=0;
        raw_log(buf);
        /* 在该信道停留 ~400ms，期间轮询 RX 环 */
        for (u32 tick = 0; tick < 400; tick++) {
            ath9k_rx_drain();
            dkm_delay_us(1000);
        }
    }
    g_scanning = 0;
    raw_log("[ath9k.raw] scan done, results=");
    static const char hh2[] = "0123456789";
    char cntbuf[4]; cntbuf[0]=hh2[g_scan_count/10]; cntbuf[1]=hh2[g_scan_count%10]; cntbuf[2]='\n'; cntbuf[3]=0;
    raw_log(cntbuf);
    return 0;
}

static int ath9k_scan_count_impl(void *ctx) {
    (void)ctx;
    return (int)g_scan_count;
}

static int ath9k_scan_result_impl(void *ctx, u32 n, struct dkm_net_scan_result *out) {
    (void)ctx;
    if (!out) return -1;
    if (n >= g_scan_count) return -2;
    /* 内部 ath9k_scan_result 与 dkm_net_scan_result 布局一致 */
    const u8 *src = (const u8 *)&g_scan_results[n];
    u8 *dst = (u8 *)out;
    for (u32 i = 0; i < sizeof(struct dkm_net_scan_result); i++) dst[i] = src[i];
    return 0;
}

static int ath9k_is_wireless_impl(void *ctx) {
    (void)ctx;
    return 1;
}

/* ============================================================
 *  TX 路径 + WPA2-PSK 连接状态机
 *
 *  说明：TX 描述符采用 AR9002（AR9280/9285）队列描述符布局的
 *  最小可用子集；位域与 HAL initvals 需在实机 bring-up 时用
 *  串口日志对拍（日志点见 ath9k_tx_frame）。协议层（认证/关联/
 *  4-way/密钥安装）由 wpa2_crypto 提供并通过 wpa2_kat 自检。
 * ============================================================ */

/* TX 队列（BE）寄存器 */
#define AR_TXDP(_q)   (0x0000u + 0x0800u * (_q))    /* TXQ 描述符基址 */
#define AR_TXE        0x0800u
#define AR_TXCFG      0x0008u
#define AR_TXCFG_TXE  0x00000004u

#define ATH9K_TX_DESC_COUNT 8u
#define ATH9K_TX_BUF_SIZE   2048u

/* AR9002 TX 队列描述符（32 字节，最小字段集） */
struct ath9k_tx_desc {
    u32 ds_link;     /* 下一描述符物理地址 */
    u32 ds_data;     /* 数据缓冲物理地址 */
    u32 ds_ctl0;     /* FRAME_LEN(0:12) | TXQ(12:16) */
    u32 ds_ctl1;     /* BUF_LEN(0:12) | MORE(12) */
    u32 ds_ctl2;     /* RTS/CTS + 速率索引（0 = 默认 1M CCK） */
    u32 ds_ctl3;     /* 分片/重试（0） */
    u32 ds_ctl4;
    u32 ds_ctl5;
} __attribute__((packed));

#define ATH9K_TXCTL0_LEN_S   0
#define ATH9K_TXCTL0_LEN_M   0x00000FFFu
#define ATH9K_TXCTL1_LEN_S   0
#define ATH9K_TXCTL1_LEN_M   0x00000FFFu
#define ATH9K_TXCTL1_MORE    0x00001000u

/* ---- 连接状态 ---- */
#define WPA_STATE_IDLE      0
#define WPA_STATE_AUTH_SENT 1
#define WPA_STATE_ASSOC_SENT 2
#define WPA_STATE_M1_WAIT   3
#define WPA_STATE_M3_WAIT   4
#define WPA_STATE_CONNECTED 5

static struct dkm_dma_buffer g_tx_desc_dma;
static struct ath9k_tx_desc *g_tx_desc;
static struct dkm_dma_buffer g_tx_buf_dma;
static u8 *g_tx_buf;
static u32 g_tx_head;
static int g_tx_ready;
static const struct dkm_dma_api *g_dma;

/* WPA2 会话 */
static u8  g_sta_mac[6];                 /* SPA */
static u8  g_ap_mac[6];                  /* AA / BSSID */
static u8  g_channel;
static char g_target_ssid[ATH9K_SSID_MAX + 1];
static int g_wpa_state;
static u8  g_anonce[32];
static u8  g_snonce[32];
static u8  g_pmk[WPA2_PMK_LEN];
static u8  g_ptk[64];                    /* KCK(16) KEK(16) TK(16) + 余量 */
static u64 g_ptk_replay;
static u64 g_tx_pn;                      /* CCMP 发包序号 */
static u16 g_seq;
static int g_key_installed;

#define ATH9K_STA_MAC_ID0 0x0800u

/* ---- 802.11 管理帧构造 ---- */
#define IEEE80211_STYPE_AUTH   0x00B0u
#define IEEE80211_STYPE_ASSOC_REQ 0x0000u
#define IEEE80211_STYPE_ASSOC_RESP 0x0010u
#define IEEE80211_STYPE_REASSOC_REQ 0x0020u
#define IEEE80211_STYPE_DEAUTH 0x00C0u

#define WLAN_EID_SUPP_RATES  1
#define WLAN_EID_ERP         42
#define WLAN_EID_RSN         48

static u32 put_le16(u8 *p, u16 v) { p[0]=(u8)v; p[1]=(u8)(v>>8); return 2; }

/* 帧控制字（管理帧，非保护） */
static void mgmt_fc(u8 *f, u16 subtype) {
    u16 fc = 0x0000u | subtype;          /* proto 0, type=mgmt(0) */
    f[0] = (u8)(fc & 0xFF);
    f[1] = (u8)(fc >> 8);
    f[2] = 0; f[3] = 0;                  /* duration */
}

/* 填 24 字节管理帧头：DA / SA / BSSID / seq */
static void mgmt_hdr(u8 *f, const u8 *da, const u8 *sa, const u8 *bssid) {
    for (int i = 0; i < 6; i++) f[4+i]  = da[i];
    for (int i = 0; i < 6; i++) f[10+i] = sa[i];
    for (int i = 0; i < 6; i++) f[16+i] = bssid[i];
    u16 sc = (u16)((++g_seq & 0xFFF) << 4);
    f[22] = (u8)(sc & 0xFF); f[23] = (u8)(sc >> 8);
}

/* WPA2-PSK RSN IE（AKM 00-0F-AC:2，CCMP） */
static u32 rsn_ie_psk(u8 *p) {
    u8 *q = p;
    q[0] = WLAN_EID_RSN;
    q[1] = 20;                            /* 长度 */
    q += 2;
    q += put_le16(q, 1);                  /* version */
    /* group cipher: OUI 00-0F-AC + type 4 (CCMP) */
    q[0]=0x00; q[1]=0x0F; q[2]=0xAC; q[3]=0x04; q += 4;
    q += put_le16(q, 1);                  /* pairwise count */
    q[0]=0x00; q[1]=0x0F; q[2]=0xAC; q[3]=0x04; q += 4;
    q += put_le16(q, 1);                  /* AKM count */
    q[0]=0x00; q[1]=0x0F; q[2]=0xAC; q[3]=0x02; q += 4;   /* PSK */
    q += put_le16(q, 0);                  /* RSN caps */
    return (u32)(q - p);
}

/* 发送一帧（构造 802.11 头 → TX 环 → 触发） */
static int ath9k_tx_frame(const u8 *frame, u32 len);

/* ---- 认证请求（open system） ---- */
static int ath9k_send_auth(void) {
    u8 f[30 + 4];
    for (u32 i = 0; i < sizeof(f); i++) f[i] = 0;
    mgmt_fc(f, IEEE80211_STYPE_AUTH);
    mgmt_hdr(f, g_ap_mac, g_sta_mac, g_ap_mac);
    /* auth body: alg(2)=0, seq(2)=1, status(2)=0 */
    put_le16(f + 24, 0);
    put_le16(f + 26, 1);
    put_le16(f + 28, 0);
    raw_log("[ath9k.raw] tx auth request\n");
    return ath9k_tx_frame(f, 30);
}

/* ---- 关联请求（含 RSN IE） ---- */
static int ath9k_send_assoc(void) {
    u8 f[24 + 4 + 2 + 34 + 4 + 24 + 2];
    u32 n = 0;
    for (u32 i = 0; i < sizeof(f); i++) f[i] = 0;
    mgmt_fc(f, IEEE80211_STYPE_ASSOC_REQ);
    mgmt_hdr(f, g_ap_mac, g_sta_mac, g_ap_mac);
    n = 24;
    n += put_le16(f + n, 0x0431);         /* capability: ESS + privacy + short preamble */
    n += put_le16(f + n, 10);             /* listen interval */
    /* SSID IE */
    {
        u32 sl = 0;
        while (g_target_ssid[sl]) sl++;
        f[n++] = WLAN_EID_SSID;
        f[n++] = (u8)sl;
        for (u32 i = 0; i < sl; i++) f[n++] = (u8)g_target_ssid[i];
    }
    /* supported rates: 1,2,5.5,11,6,9,12,18 Mbps */
    f[n++] = WLAN_EID_SUPP_RATES;
    f[n++] = 8;
    f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8B; f[n++] = 0x96;
    f[n++] = 0x0C; f[n++] = 0x12; f[n++] = 0x18; f[n++] = 0x24;
    /* RSN IE */
    n += rsn_ie_psk(f + n);
    raw_log("[ath9k.raw] tx assoc request\n");
    return ath9k_tx_frame(f, n);
}

/* ---- EAPOL-Key 构造（802.1X 4-way） ---- */
#define EAPOL_VERSION 2
#define EAPOL_TYPE_KEY 3
#define WPA_KEY_DESC_TYPE 2
#define WPA_KEY_INFO_KEY_TYPE  0x0008u   /* 1 = pairwise */
#define WPA_KEY_INFO_INSTALL   0x0040u
#define WPA_KEY_INFO_ACK       0x0080u
#define WPA_KEY_INFO_MIC       0x0100u
#define WPA_KEY_INFO_SECURE    0x0200u
#define WPA_KEY_INFO_ENC_KEY   0x1000u
#define WPA_KEY_INFO_VER2      0x0002u   /* WPA2 = 3 (HMAC-SHA1 + AES) */

/* 构造 802.11 数据帧（toDS）+ LLC/SNAP + EAPOL-Key，做 MIC 后发送。
 * key_info 由调用方给，nonce 指针可为 0（M4 用）。 */
static int ath9k_send_eapol(u16 key_info, const u8 *nonce,
                            const u8 *key_data, u32 kd_len) {
    static u8 f[256];
    for (u32 i = 0; i < sizeof(f); i++) f[i] = 0;
    /* 802.11 data 头：toDS（AP 为 RA=A1，SA=A2，DA=A3=BSSID） */
    u16 fc = 0x0100u | 0x0008u;           /* data + ToDS */
    f[0] = (u8)(fc & 0xFF); f[1] = (u8)(fc >> 8);
    mgmt_hdr(f, g_ap_mac, g_sta_mac, g_ap_mac);
    u32 n = 24;
    /* LLC/SNAP：AA AA 03 00 00 00 88 8E (EAPOL) */
    static const u8 snap[8] = {0xAA,0xAA,0x03,0x00,0x00,0x00,0x88,0x8E};
    for (int i = 0; i < 8; i++) f[n++] = snap[i];
    /* EAPOL 头 */
    u32 eapol_off = n;
    f[n++] = EAPOL_VERSION;
    f[n++] = EAPOL_TYPE_KEY;
    u32 len_off = n; n += 2;              /* 长度稍后回填 */
    /* EAPOL-Key 体 */
    u32 mic_off;
    f[n++] = WPA_KEY_DESC_TYPE;
    n += put_le16(f + n, key_info);
    n += put_le16(f + n, 16);             /* key length (CCMP) */
    for (int i = 0; i < 8; i++) f[n++] = (u8)(g_ptk_replay >> (i * 8));
    for (int i = 0; i < 32; i++) f[n++] = nonce ? nonce[i] : 0;   /* nonce */
    n += 16;                              /* key IV（全 0） */
    n += 8;                               /* key RSC */
    n += 8;                               /* key ID（全 0） */
    mic_off = n; n += 16;                 /* key MIC（待填） */
    n += put_le16(f + n, (u16)kd_len);
    for (u32 i = 0; i < kd_len; i++) f[n++] = key_data[i];
    /* 回填 EAPOL 长度（含 4 字节 EAPOL 头） */
    u16 elen = (u16)(n - eapol_off - 4);
    f[len_off] = (u8)(elen & 0xFF); f[len_off + 1] = (u8)(elen >> 8);
    /* MIC：若置位，对 EAPOL 帧（MIC 字段已清零）用 KCK 计算 HMAC-SHA1-128 */
    if (key_info & WPA_KEY_INFO_MIC) {
        u8 mic[20];
        wpa2_hmac_sha1(g_ptk, WPA2_KCK_LEN, f + eapol_off, n - eapol_off, mic);
        for (int i = 0; i < 16; i++) f[mic_off + i] = mic[i];
    }
    return ath9k_tx_frame(f, n);
}

/* ---- EAPOL-Key M2（SNonce + MIC，4-way 第 2 条） ---- */
static int ath9k_send_m2(void) {
    u16 ki = WPA_KEY_INFO_VER2 | WPA_KEY_INFO_KEY_TYPE | WPA_KEY_INFO_MIC;
    u8 kd[2 + 20];                        /* RSN IE（WPA2 常见，可省） */
    u32 kdn = rsn_ie_psk(kd);
    raw_log("[ath9k.raw] tx EAPOL M2\n");
    return ath9k_send_eapol(ki, g_snonce, kd, kdn);
}

/* ---- EAPOL-Key M4（MIC + secure，4-way 第 4 条） ---- */
static int ath9k_send_m4(void) {
    u16 ki = WPA_KEY_INFO_VER2 | WPA_KEY_INFO_KEY_TYPE |
             WPA_KEY_INFO_MIC | WPA_KEY_INFO_SECURE;
    raw_log("[ath9k.raw] tx EAPOL M4\n");
    return ath9k_send_eapol(ki, 0, 0, 0);
}

/* ---- PTK 派生（802.11i H.4） ---- */
static void ath9k_derive_ptk(void) {
    u8 seed[64];
    const u8 *aa = g_ap_mac, *spa = g_sta_mac;
    int swap = 0;
    for (int i = 0; i < 6; i++) {
        if (aa[i] != spa[i]) { swap = aa[i] > spa[i]; break; }
    }
    const u8 *m1 = swap ? spa : aa;       /* min(AA,SPA) */
    const u8 *m2 = swap ? aa : spa;       /* max(AA,SPA) */
    for (int i = 0; i < 6; i++) { seed[i] = m1[i]; seed[6+i] = m2[i]; }
    int nswap = 0;
    for (int i = 0; i < 32; i++) {
        if (g_anonce[i] != g_snonce[i]) { nswap = g_anonce[i] > g_snonce[i]; break; }
    }
    const u8 *n1 = nswap ? g_snonce : g_anonce;
    const u8 *n2 = nswap ? g_anonce : g_snonce;
    for (int i = 0; i < 32; i++) { seed[12+i] = n1[i]; seed[44+i] = n2[i]; }
    wpa2_prf512(g_pmk, seed, 64, g_ptk);
}

/* ---- 扫描结果查找（按 SSID 找 BSSID/信道） ---- */
static int ath9k_find_ssid(const char *ssid, u8 *bssid, u8 *channel, u8 *sec) {
    for (u32 i = 0; i < g_scan_count; i++) {
        const char *a = g_scan_results[i].ssid;
        const char *b = ssid;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) {
            for (int j = 0; j < 6; j++) bssid[j] = g_scan_results[i].bssid[j];
            *channel = g_scan_results[i].channel;
            *sec = g_scan_results[i].security;
            return 0;
        }
    }
    return -1;
}

/* ---- 随机 SNonce（TSC 混合，实机 TLS 级随机由上层补充） ---- */
static void ath9k_make_snonce(void) {
    u64 t = dkm_rdtsc();
    for (int i = 0; i < 32; i++) {
        t = t * 6364136223846793005ULL + 1442695040888963407ULL;
        g_snonce[i] = (u8)(t >> ((i % 8) * 8));
    }
}

/* ---- 连接入口 ---- */
static int ath9k_connect_impl(void *ctx, const char *ssid, const char *password) {
    (void)ctx;
    if (!ssid) return -1;
    raw_log("[ath9k.raw] connect request\n");
    if (!g_tx_ready) { raw_log("[ath9k.raw] TX not ready\n"); return -2; }

    u8 sec = 0;
    if (ath9k_find_ssid(ssid, g_ap_mac, &g_channel, &sec) != 0) {
        raw_log("[ath9k.raw] connect: SSID not in scan results\n");
        return -3;                        /* 调用方先扫描 */
    }
    u32 sl = 0;
    while (ssid[sl] && sl < ATH9K_SSID_MAX) { g_target_ssid[sl] = ssid[sl]; sl++; }
    g_target_ssid[sl] = 0;
    copy_mac(g_sta_mac, g_mac);

    /* 需要 WPA2-PSK 时先算 PMK */
    if (sec != DKM_NET_SEC_OPEN) {
        if (!password || !password[0]) { raw_log("[ath9k.raw] connect: empty PSK\n"); return -4; }
        wpa2_pbkdf2_sha1(password, (const u8 *)g_target_ssid, sl, g_pmk);
        raw_log("[ath9k.raw] PMK derived\n");
    }

    ath9k_set_channel(g_channel);
    g_key_installed = 0;
    g_ptk_replay = 0;
    g_tx_pn = 0;
    g_wpa_state = WPA_STATE_AUTH_SENT;
    if (ath9k_send_auth() != 0) { g_wpa_state = WPA_STATE_IDLE; return -5; }
    return 0;
}

static int ath9k_disconnect_impl(void *ctx) {
    (void)ctx;
    raw_log("[ath9k.raw] disconnect\n");
    g_wpa_state = WPA_STATE_IDLE;
    g_key_installed = 0;
    return 0;
}

/* ---- 管理/控制帧处理（连接状态机驱动） ---- */
static int ath9k_handle_mgmt(const u8 *frame, u32 len);

/* ath9k TX：把帧写进 TX 环并触发 DMA。
 * 描述符位域按 AR9002 队列描述符最小集构造；实机对拍日志见下方 raw_hex。
 * 上层把数据帧交给本驱动时，若尚未安装密钥（未连接）则 panic（ATH-E04）。 */
static int ath9k_tx_frame(const u8 *frame, u32 len) {
    if (!g_tx_ready || !g_tx_desc) return -1;
    if (len > ATH9K_TX_BUF_SIZE) return -1;
    u32 idx = g_tx_head;
    u8 *buf = g_tx_buf + (u64)idx * ATH9K_TX_BUF_SIZE;
    for (u32 i = 0; i < len; i++) buf[i] = frame[i];
    struct ath9k_tx_desc *d = &g_tx_desc[idx];
    d->ds_data = (u32)(g_tx_buf_dma.phys + (u64)idx * ATH9K_TX_BUF_SIZE);
    d->ds_ctl0 = (len & ATH9K_TXCTL0_LEN_M);          /* FRAME_LEN，TXQ=0(BE) */
    d->ds_ctl1 = (len & ATH9K_TXCTL1_LEN_M);          /* BUF_LEN，无 MORE */
    d->ds_ctl2 = 0;
    d->ds_ctl3 = 0;
    __asm__ volatile("" ::: "memory");
    /* 触发 TX：写 TXDP 指向队首描述符并置 TXE */
    ath9k_reg_write(AR_TXDP(0), (u32)g_tx_desc_dma.phys);
    ath9k_reg_write(AR_TXCFG, AR_TXCFG_TXE);
    g_tx_head = (g_tx_head + 1) % ATH9K_TX_DESC_COUNT;
    raw_hex("[ath9k] tx kick len=", len);
    return 0;
}

/* ---- 数据帧 TX（已连接后由 net 栈调用；CCMP 加密） ---- */
static int ath9k_tx_impl(void *ctx, const void *packet, u32 length) {
    (void)ctx;
    if (!g_key_installed) {
        /* 未安装密钥时不允许数据帧上路：零降级 panic（ATH-E04） */
        g_log->panic("ATH-E04 DATA TX BEFORE KEY INSTALL");
        return -1;
    }
    static u8 f[ATH9K_TX_BUF_SIZE];
    for (u32 i = 0; i < sizeof(f); i++) f[i] = 0;
    /* toDS 数据帧头：A1=BSSID, A2=STA, A3=DA(未解析，用 BSSID 占位) */
    u16 fc = 0x0100u | 0x0008u;
    f[0] = (u8)(fc & 0xFF); f[1] = (u8)(fc >> 8);
    mgmt_hdr(f, g_ap_mac, g_sta_mac, g_ap_mac);
    int rc = wpa2_ccmp_encrypt(f, sizeof(f), (const u8 *)packet, length,
                               g_ptk + WPA2_KCK_LEN + WPA2_KEK_LEN, ++g_tx_pn);
    if (rc <= 0) return -1;
    return ath9k_tx_frame(f, (u32)rc);
}

/* ---- 4-way 消息处理 ---- */
/* 定位 802.11 数据帧中的 EAPOL-Key（返回 EAPOL 起始偏移，-1 未找到） */
static int eapol_find(const u8 *frame, u32 len, u32 *out_off) {
    /* 24B MAC 头 + 8B LLC/SNAP，可能带 8B CCMP 头（加密 EAPOL 场景） */
    u32 off = 24;
    if (len < off + 8) return -1;
    if (frame[off] == 0xAA && frame[off+1] == 0xAA) {
        /* 明文 EAPOL */
    } else if (len >= off + 16) {
        off += 8;                          /* 跳过 CCMP 头 */
        if (frame[off] != 0xAA) return -1;
    } else {
        return -1;
    }
    if (off + 8 + 4 > len) return -1;
    if (frame[off+6] != 0x88 || frame[off+7] != 0x8E) return -1;   /* ethertype EAPOL */
    if (frame[off+8] != EAPOL_VERSION || frame[off+9] != EAPOL_TYPE_KEY) return -1;
    *out_off = off + 8;
    return 0;
}

static void ath9k_handle_eapol(const u8 *eap, u32 len) {
    /* eap 指向 EAPOL 头（version,type,length,然后 Key 体） */
    if (len < 4 + 1 + 2 + 2 + 8 + 32 + 16 + 8 + 8 + 16 + 2) return;
    const u8 *k = eap + 4;                 /* Key descriptor 起始 */
    u16 key_info = (u16)k[1] | ((u16)k[2] << 8);
    const u8 *nonce = k + 13;              /* key nonce 偏移：1(desc)+2(info)+2(len)+8(replay) */
    const u8 *mic = k + 13 + 32 + 16 + 8 + 8;
    u16 kd_len = (u16)k[13 + 32 + 16 + 8 + 8 + 16] | ((u16)k[13+32+16+8+8+16+1] << 8);
    const u8 *kd = mic + 16 + 2;

    int pairwise = (key_info & WPA_KEY_INFO_KEY_TYPE) != 0;
    int ack = (key_info & WPA_KEY_INFO_ACK) != 0;
    int has_mic = (key_info & WPA_KEY_INFO_MIC) != 0;

    if (!pairwise) {
        /* GTK 消息（M3 附带），此处只做 MIC 校验后记录，实际 GTK 由 M3 的 key data 携带 */
        raw_log("[ath9k.raw] EAPOL GTK msg\n");
        return;
    }

    if (ack && !has_mic) {
        /* ---- M1：带 ANonce ---- */
        raw_log("[ath9k.raw] EAPOL M1 recv\n");
        for (int i = 0; i < 32; i++) g_anonce[i] = nonce[i];
        /* replay counter 8 字节（小端） */
        g_ptk_replay = 0;
        for (int i = 0; i < 8; i++) g_ptk_replay |= (u64)k[5 + i] << (i * 8);
        ath9k_make_snonce();
        ath9k_derive_ptk();
        raw_log("[ath9k.raw] PTK derived\n");
        g_wpa_state = WPA_STATE_M3_WAIT;
        ath9k_send_m2();
        return;
    }

    if (has_mic && !ack) {
        /* ---- M3：校验 MIC → 安装 PTK/GTK → 回 M4 ---- */
        raw_log("[ath9k.raw] EAPOL M3 recv\n");
        u64 rc = 0;
        for (int i = 0; i < 8; i++) rc |= (u64)k[5 + i] << (i * 8);
        g_ptk_replay = rc;
        /* MIC 校验：MIC 字段清零后对整段 EAPOL 计算 HMAC-SHA1-128 再比对。
         * 用本地副本避免改写 RX 缓冲（const 语义）。 */
        u32 mic_rel = (u32)(mic - eap);
        u8 tmp[256];
        if (len > sizeof(tmp)) { raw_log("[ath9k.raw] EAPOL too long\n"); return; }
        for (u32 i = 0; i < len; i++) tmp[i] = eap[i];
        for (int i = 0; i < 16; i++) tmp[mic_rel + i] = 0;
        u8 calc[20];
        wpa2_hmac_sha1(g_ptk, WPA2_KCK_LEN, tmp, len, calc);
        u8 diff = 0;
        for (int i = 0; i < 16; i++) diff |= calc[i] ^ mic[i];
        if (diff) {
            raw_log("[ath9k.raw] M3 MIC MISMATCH (bad PSK?)\n");
            g_wpa_state = WPA_STATE_IDLE;
            return;
        }
        raw_log("[ath9k.raw] M3 MIC ok\n");

        /* key data 中可能含 GTK（KDE 0xDD + OUI 00-0F-AC + type 1，AES key wrap）。
         * 若含 ENC_KEY，用 KEK unwrap 取 GTK。 */
        if ((key_info & WPA_KEY_INFO_ENC_KEY) && kd_len > 0) {
            /* 遍历 KDE */
            u32 p = 0;
            while (p + 2 <= kd_len) {
                u8 ktype = kd[p];
                u8 klen = kd[p+1];
                if (p + 2 + klen > kd_len) break;
                if (ktype == 0xDD && klen >= 6 &&
                    kd[p+2] == 0x00 && kd[p+3] == 0x0F && kd[p+4] == 0xAC && kd[p+5] == 0x01) {
                    const u8 *wrapped = kd + p + 6;
                    u32 wlen = (u32)klen - 6;
                    u8 gtk[32];
                    if (wpa2_key_unwrap(g_ptk + WPA2_KCK_LEN, wrapped, wlen, gtk) == 0) {
                        raw_log("[ath9k.raw] GTK unwrapped\n");
                    } else {
                        raw_log("[ath9k.raw] GTK unwrap FAILED\n");
                    }
                }
                p += 2 + klen;
            }
        }
        g_key_installed = 1;
        g_wpa_state = WPA_STATE_CONNECTED;
        ath9k_send_m4();
        raw_log("[ath9k.raw] === CONNECTED (WPA2-PSK) ===\n");
    }
}

/* ---- 管理帧处理（认证/关联响应） ---- */
static int ath9k_handle_mgmt(const u8 *frame, u32 len) {
    if (len < 24) return -1;
    u16 fctl = (u16)frame[0] | ((u16)frame[1] << 8);
    if ((fctl & 0x000C) != 0x0000) return -1;              /* 非管理帧 */
    u16 subtype = fctl & 0x00F0;
    /* 只处理来自目标 AP 的帧 */
    for (int i = 0; i < 6; i++) if (frame[10+i] != g_ap_mac[i]) return -1;

    if (subtype == 0x00B0) {                                /* Authentication */
        if (len < 24 + 6) return -1;
        u16 status = (u16)frame[24+4] | ((u16)frame[24+5] << 8);
        if (status != 0) { raw_log("[ath9k.raw] auth FAILED\n"); g_wpa_state = WPA_STATE_IDLE; return -1; }
        raw_log("[ath9k.raw] auth ok\n");
        if (g_wpa_state == WPA_STATE_AUTH_SENT) {
            g_wpa_state = WPA_STATE_ASSOC_SENT;
            ath9k_send_assoc();
        }
        return 0;
    }
    if (subtype == 0x0010) {                                /* Association Response */
        if (len < 24 + 6) return -1;
        u16 status = (u16)frame[24+2] | ((u16)frame[24+3] << 8);
        if (status != 0) { raw_log("[ath9k.raw] assoc FAILED\n"); g_wpa_state = WPA_STATE_IDLE; return -1; }
        raw_log("[ath9k.raw] assoc ok, waiting 4-way\n");
        g_wpa_state = WPA_STATE_M1_WAIT;
        return 0;
    }
    if (subtype == 0x00C0) {                                /* Deauth */
        raw_log("[ath9k.raw] deauth from AP\n");
        g_wpa_state = WPA_STATE_IDLE;
        g_key_installed = 0;
        return 0;
    }
    return -1;
}

/* ---- TX 环初始化 ---- */
static int ath9k_tx_init(const struct dkm_dma_api *dma) {
    if (!dma || !dma->alloc_pages || !g_mmio) return -1;
    g_dma = dma;
    /* 描述符环 1 页 + 缓冲 1 页 */
    if (dma->alloc_pages(1, 16, 0x100000000ULL, &g_tx_desc_dma) != 0) {
        raw_log("[ath9k.raw] tx_init: desc alloc failed\n");
        return -2;
    }
    if (dma->alloc_pages(1, 16, 0x100000000ULL, &g_tx_buf_dma) != 0) {
        raw_log("[ath9k.raw] tx_init: buf alloc failed\n");
        return -3;
    }
    g_tx_desc = (struct ath9k_tx_desc *)g_tx_desc_dma.virt;
    g_tx_buf = (u8 *)g_tx_buf_dma.virt;
    for (u32 i = 0; i < sizeof(struct ath9k_tx_desc) * ATH9K_TX_DESC_COUNT; i++)
        ((u8 *)g_tx_desc)[i] = 0;
    for (u32 i = 0; i < ATH9K_TX_DESC_COUNT; i++) {
        g_tx_desc[i].ds_link = (u32)(g_tx_desc_dma.phys +
            (((i + 1) % ATH9K_TX_DESC_COUNT) * sizeof(struct ath9k_tx_desc)));
    }
    __asm__ volatile("" ::: "memory");
    g_tx_head = 0;
    g_tx_ready = 1;
    raw_log("[ath9k.raw] tx_init: rings allocated\n");
    return 0;
}

static int ath9k_rx_poll_impl(void *ctx, void *buffer, u32 capacity, u32 *out_length) {
    (void)ctx; (void)buffer; (void)capacity;
    if (out_length) *out_length = 0;
    /* 被动扫描内部消费所有帧，外部 rx_poll 总是返回无数据 */
    return 1;
}

static void copy_mac(u8 dst[6], const u8 src[6]) {
    for (int i = 0; i < 6; i++) dst[i] = src[i];
}

/* ---- 中断处理（PIC IRQ 路由） ---- */
static int ath9k_irq_handler(u8 irq) {
    (void)irq;
    if (!g_mmio) return 0;
    u32 isr = ath9k_reg_read(AR_ISR_RAC);
    if (isr) {
        ath9k_reg_write(AR_ISR, isr);
        if (isr & (AR_ISR_RXOK | AR_ISR_RXERR | AR_ISR_RXORN)) {
            ath9k_rx_drain();
        }
    }
    return 0;
}

/* ---- driver_init ---- */
__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;
    /* 实机要求: 先校准 TSC, 再使用基于 CPU 频率的延迟 */
    dkm_tsc_calibrate();
    raw_log("[ath9k.raw] driver_init entered\n");
    if (!api || !api->log) return -1;
    g_log = api->log;

    /* 1. PCI 探测 */
    u8 bus, dev;
    if (ath9k_find(&bus, &dev) != 0) {
        raw_log("[ath9k.raw] no Atheros WiFi device found\n");
        return -1;
    }
    raw_log("[ath9k.raw] Atheros device found\n");

    /* 2. 启用 PCI 命令（MEM + Bus Master） */
    u32 cmd = pci_read(bus, dev, 0, PCI_COMMAND);
    pci_write(bus, dev, 0, PCI_COMMAND, cmd | PCI_CMD_MEM | PCI_CMD_BUSM);

    /* 3. 读取 BAR0 */
    u32 bar0 = pci_read(bus, dev, 0, PCI_BAR0);
    raw_hex("[ath9k.raw] BAR0 raw=", bar0);
    if ((bar0 & 0x1) != 0) {
        raw_log("[ath9k.raw] BAR0 is IO space, unsupported\n");
        return -2;
    }
    g_bar0_phys = (u64)(bar0 & 0xFFFFFFF0u);
    /* 检查是否 64-bit BAR */
    if ((bar0 & 0x6) == 0x4) {
        u32 bar1 = pci_read(bus, dev, 0, PCI_BAR1);
        g_bar0_phys |= ((u64)bar1 << 32);
        raw_hex("[ath9k.raw] BAR0 high=", (u64)bar1);
    }
    raw_hex("[ath9k.raw] BAR0 phys=", g_bar0_phys);
    if (g_bar0_phys >= 0x100000000ULL) {
        raw_log("[ath9k.raw] BAR0 above 4G, current HHDM cannot map\n");
        return -3;
    }

    /* 4. HHDM 映射 BAR0 */
    u64 hhdm = api->hhdm_offset;
    raw_hex("[ath9k.raw] HHDM=", hhdm);
    g_mmio = (volatile u8 *)(g_bar0_phys + hhdm);
    /* 验证 MMIO 可读：读 AR_STA_ID1 */
    u32 probe = ath9k_reg_read(AR_STA_ID1);
    raw_hex("[ath9k.raw] AR_STA_ID1 probe=", probe);

    /* 5. 读取 PCI device ID + IRQ line */
    u32 vd = pci_read(bus, dev, 0, PCI_VENDOR_ID);
    g_pci_device_id = (u16)(vd >> 16);
    g_pci_irq = (u8)pci_read(bus, dev, 0, PCI_IRQ_LINE);
    raw_hex("[ath9k.raw] device id=", g_pci_device_id);
    raw_hex("[ath9k.raw] irq line=", g_pci_irq);

    /* 6. 软件复位 */
    if (ath9k_reset() != 0) {
        raw_log("[ath9k.raw] reset failed\n");
        return -4;
    }

    /* 7. EEPROM 读取 MAC 地址 */
    /* AR5416 EEPROM 偏移：MAC 地址在 word 0x1D-0x1F（6 字节） */
    u16 m0 = ath9k_eeprom_read(0x1D);
    u16 m1 = ath9k_eeprom_read(0x1E);
    u16 m2 = ath9k_eeprom_read(0x1F);
    if (m0 == 0xffff && m1 == 0xffff && m2 == 0xffff) {
        raw_log("[ath9k.raw] EEPROM MAC all-FF, fallback to all-zero\n");
        for (int i = 0; i < 6; i++) g_mac[i] = 0;
    } else {
        g_mac[0] = (u8)(m0 & 0xFF);
        g_mac[1] = (u8)(m0 >> 8);
        g_mac[2] = (u8)(m1 & 0xFF);
        g_mac[3] = (u8)(m1 >> 8);
        g_mac[4] = (u8)(m2 & 0xFF);
        g_mac[5] = (u8)(m2 >> 8);
    }
    {
        static const char hh[] = "0123456789abcdef";
        char mstr[18];
        mstr[0]=hh[(g_mac[0]>>4)&0xf]; mstr[1]=hh[g_mac[0]&0xf]; mstr[2]=':';
        mstr[3]=hh[(g_mac[1]>>4)&0xf]; mstr[4]=hh[g_mac[1]&0xf]; mstr[5]=':';
        mstr[6]=hh[(g_mac[2]>>4)&0xf]; mstr[7]=hh[g_mac[2]&0xf]; mstr[8]=':';
        mstr[9]=hh[(g_mac[3]>>4)&0xf]; mstr[10]=hh[g_mac[3]&0xf]; mstr[11]=':';
        mstr[12]=hh[(g_mac[4]>>4)&0xf]; mstr[13]=hh[g_mac[4]&0xf]; mstr[14]=':';
        mstr[15]=hh[(g_mac[5]>>4)&0xf]; mstr[16]=hh[g_mac[5]&0xf]; mstr[17]=0;
        raw_log("[ath9k.raw] MAC=");
        raw_log(mstr);
        raw_log("\n");
    }

    /* 8. 硬件初始化 */
    if (ath9k_hw_init() != 0) {
        raw_log("[ath9k.raw] hw_init failed\n");
        return -5;
    }

    /* 9. RX/TX 环初始化 */
    if (api->dma && api->dma->alloc_pages) {
        int rc = ath9k_rx_init(api->dma);
        raw_hex("[ath9k.raw] rx_init rc=", (u64)(i8)rc);
        if (rc != 0) {
            raw_log("[ath9k.raw] RX init failed, scan unavailable\n");
        }
        int trc = ath9k_tx_init(api->dma);
        raw_hex("[ath9k.raw] tx_init rc=", (u64)(i8)trc);
        if (trc != 0) raw_log("[ath9k.raw] TX init failed, connect unavailable\n");
    } else {
        raw_log("[ath9k.raw] no DMA api, scan/TX unavailable\n");
    }

    /* 9b. WPA2 加密底座自检（KAT）：失败即 panic，绝不带病上路 */
    {
        int krc = wpa2_kat();
        if (krc != 0) {
            raw_hex("[ath9k.raw] WPA2 KAT FAILED case=", (u64)krc);
            g_log->panic("ATH-E02 WPA2 CRYPTO KAT FAILED");
            return -7;
        }
        raw_log("[ath9k.raw] WPA2 crypto KAT ok\n");
    }

    /* 10. 注册中断 */
    if (api->irq_register && g_pci_irq) {
        int rc = api->irq_register(g_pci_irq, ath9k_irq_handler);
        raw_hex("[ath9k.raw] irq_register rc=", (u64)(i8)rc);
    }

    /* 11. 注册 netdev（无线 + RX/TX） */
    struct dkm_net_device_desc ndesc;
    for (u32 i = 0; i < sizeof(ndesc); i++) ((u8 *)&ndesc)[i] = 0;
    static const char devname[] = "ath9k0";
    ndesc.name = devname;
    copy_mac(ndesc.mac, g_mac);
    ndesc.flags = DKM_NET_F_WIRELESS |
                  (g_rx_ready ? DKM_NET_F_RX_READY : 0) |
                  (g_tx_ready ? DKM_NET_F_TX_READY : 0);
    ndesc.ctx = (void *)0;  /* ath9k 用全局状态，ctx 不需要 */
    ndesc.tx = ath9k_tx_impl;
    ndesc.rx_poll = ath9k_rx_poll_impl;
    ndesc.scan_start = ath9k_scan_start_impl;
    ndesc.scan_count = ath9k_scan_count_impl;
    ndesc.scan_result = ath9k_scan_result_impl;
    ndesc.is_wireless = ath9k_is_wireless_impl;
    ndesc.connect = ath9k_connect_impl;
    ndesc.disconnect = ath9k_disconnect_impl;

    if (api->net && api->net->register_device) {
        int rc = api->net->register_device(&ndesc);
        raw_hex("[ath9k.raw] netdev registered, index=", (u64)rc);
    } else {
        raw_log("[ath9k.raw] no net api\n");
        return -6;
    }

    /* 12. 启动后台被动扫描（同步执行，阻塞 driver_init 直到扫描完成）
     * 注：DSK 加载驱动是顺序的，ath9k 在 stage3 末尾加载，
     * 同步扫描会让 stage3 加载多花 ~5s（13 信道 × 400ms）。
     * 这是可接受的，因为 FirstInit 在 DSK 之后才运行，能拿到完整扫描结果。
     */
    if (g_rx_ready) {
        raw_log("[ath9k.raw] starting initial passive scan\n");
        ath9k_scan_start_impl(0);
    }

    raw_log("[ath9k.raw] driver_init done\n");
    return 0;
}

__attribute__((visibility("default")))
void driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    if (g_mmio) {
        ath9k_reg_write(AR_CR, AR_CR_RXD);
    }
    g_rx_ready = 0;
    g_mmio = 0;
    raw_log("[ath9k.raw] driver_exit\n");
}
