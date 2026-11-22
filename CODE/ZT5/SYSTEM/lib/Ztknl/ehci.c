/*
 * Zratro OS - EHCI (USB 2.0) 控制器驱动
 *
 * Enhanced Host Controller Interface - USB 2.0 规范
 */

#include "usb.h"
#include "console.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "cpu.h"

/* ========== 内部辅助函数 ========== */

static void delay_ms(int ms) {
    uint64_t tsc_start, tsc_end;
    uint64_t tsc_freq = cpu_get_tsc_frequency();
    if (tsc_freq == 0) tsc_freq = 1000000000ULL;  /* 默认 1GHz */
    
    asm volatile("rdtsc" : "=A"(tsc_start));
    uint64_t wait = (tsc_freq * ms) / 1000;
    do {
        asm volatile("rdtsc" : "=A"(tsc_end));
    } while ((tsc_end - tsc_start) < wait);
}

/* 辅助函数：获取物理地址（支持HHDM和堆地址） */
static uint64_t ehci_virt_to_phys(void *virt) {
    uint64_t virt_addr = (uint64_t)virt;

    /* 首先尝试HHDM偏移转换 */
    extern uint64_t hhdm_offset;
    if (hhdm_offset != 0 && virt_addr >= hhdm_offset) {
        uint64_t phys = virt_addr - hhdm_offset;
        /* 检查结果是否合理（<4GB用于DMA） */
        if (phys < 0x100000000ULL) {
            return phys;
        }
    }

    /* 否则使用页表查询 */
    return vmm_get_physical(vmm_get_kernel_space(), virt_addr);
}

/* ========== EHCI 寄存器定义 ========== */

/* 能力寄存器 */
#define EHCI_CAPLENGTH      0x00    /* 能力寄存器长度 */
#define EHCI_HCIVERSION     0x02    /* 接口版本号 */
#define EHCI_HCSPARAMS      0x04    /* 结构参数 */
#define EHCI_HCCPARAMS      0x08    /* 能力参数 */

/* HCSPARAMS 字段 */
#define EHCI_N_PORTS(p)     ((p) & 0xF)
#define EHCI_N_PTT(p)       (((p) >> 20) & 0xF)

/* 操作寄存器 */
#define EHCI_USBCMD         0x00    /* USB 命令 */
#define EHCI_USBSTS         0x04    /* USB 状态 */
#define EHCI_USBINTR        0x08    /* USB 中断使能 */
#define EHCI_FRINDEX        0x0C    /* 帧索引 */
#define EHCI_CTRLDSSEG      0x10    /* 控制数据结构段 */
#define EHCI_PERIODICLIST   0x14    /* 周期帧列表基地址 */
#define EHCI_ASYNCLIST      0x18    /* 异步列表基地址 */
#define EHCI_CONFIGFLAG     0x40    /* 配置标志 */
#define EHCI_PORTSC_BASE    0x44    /* 端口状态/控制 */

/* USB 命令寄存器位 */
#define EHCI_CMD_RUNSTOP        (1 << 0)    /* Run/Stop */
#define EHCI_CMD_HCRESET        (1 << 1)    /* HC Reset */
#define EHCI_CMD_FLSZ_MASK      (3 << 2)    /* Frame List Size */
#define EHCI_CMD_FLSZ_1024      (0 << 2)
#define EHCI_CMD_FLSZ_512       (1 << 2)
#define EHCI_CMD_FLSZ_256       (2 << 2)
#define EHCI_CMD_PSEN           (1 << 4)    /* Periodic Schedule Enable */
#define EHCI_CMD_ASEN           (1 << 5)    /* Async Schedule Enable */
#define EHCI_CMD_IAAD           (1 << 6)    /* Interrupt on Async Advance Doorbell */
#define EHCI_CMD_LHCR           (1 << 7)    /* Light Host Controller Reset */
#define EHCI_CMD_ASPMC          (3 << 8)    /* Async Schedule Park Mode Count */
#define EHCI_CMD_ASPME          (1 << 11)   /* Async Schedule Park Mode Enable */
#define EHCI_CMD_ITC            (0xFF << 16) /* Interrupt Threshold Control */

/* USB 状态寄存器位 */
#define EHCI_STS_USBINT         (1 << 0)    /* USB Interrupt */
#define EHCI_STS_USBERRINT      (1 << 1)    /* USB Error Interrupt */
#define EHCI_STS_PCD            (1 << 2)    /* Port Change Detect */
#define EHCI_STS_FLR            (1 << 3)    /* Frame List Rollover */
#define EHCI_STS_HSE            (1 << 4)    /* Host System Error */
#define EHCI_STS_IAA            (1 << 5)    /* Interrupt on Async Advance */
#define EHCI_STS_HCH            (1 << 12)   /* HC Halted */
#define EHCI_STS_RECL           (1 << 13)   /* Reclamation */
#define EHCI_STS_PSS            (1 << 14)   /* Periodic Schedule Status */
#define EHCI_STS_ASS            (1 << 15)   /* Async Schedule Status */

/* 端口状态/控制寄存器位 */
#define EHCI_PORT_CCS           (1 << 0)    /* Current Connect Status */
#define EHCI_PORT_CSC           (1 << 1)    /* Connect Status Change */
#define EHCI_PORT_PE            (1 << 2)    /* Port Enable */
#define EHCI_PORT_PEC           (1 << 3)    /* Port Enable/Disable Change */
#define EHCI_PORT_OCA           (1 << 4)    /* Over-current Active */
#define EHCI_PORT_OCC           (1 << 5)    /* Over-current Change */
#define EHCI_PORT_FPR           (1 << 6)    /* Force Port Resume */
#define EHCI_PORT_SUSP          (1 << 7)    /* Suspend */
#define EHCI_PORT_RESET         (1 << 8)    /* Port Reset */
#define EHCI_PORT_LPM           (1 << 9)    /* LPM (Link Power Management) */
#define EHCI_PORT_PP            (1 << 12)   /* Port Power */
#define EHCI_PORT_SPEED_MASK    (3 << 10)   /* Line Status */
#define EHCI_PORT_SPEED_FULL    0           /* SE0 - Full Speed */
#define EHCI_PORT_SPEED_KSTATE  (1 << 10)   /* K-state (Low Speed before reset) */
#define EHCI_PORT_SPEED_HIGH    (2 << 10)   /* High Speed (after reset handshake) */
#define EHCI_PORT_SPEED_JSTATE  (3 << 10)   /* J-state */
#define EHCI_PORT_OWNER         (1 << 13)   /* Port Owner (to companion) */
#define EHCI_PORT_PIC           (3 << 14)   /* Port Indicator Control */
#define EHCI_PORT_WKCNNT_E      (1 << 20)   /* Wake on Connect Enable */
#define EHCI_PORT_WKDSCNNT_E    (1 << 21)   /* Wake on Disconnect Enable */
#define EHCI_PORT_WKOC_E        (1 << 22)   /* Wake on Over-current Enable */

/* ========== EHCI 数据结构定义 ========== */

/* Queue Element Transfer Descriptor (qTD) */
struct ehci_qtd {
    uint32_t next_qtd;          /* Next qTD Pointer */
    uint32_t alt_next_qtd;      /* Alternate Next qTD Pointer */
    uint32_t token;             /* Token */
    uint32_t buffer[5];         /* Buffer Pointers */
} __attribute__((packed, aligned(32)));

/* qTD Token 字段 */
#define EHCI_QTD_STATUS_MASK        0x000000FC
#define EHCI_QTD_STATUS_ACTIVE      (1 << 7)
#define EHCI_QTD_STATUS_HALTED      (1 << 6)
#define EHCI_QTD_STATUS_DBERR       (1 << 5)
#define EHCI_QTD_STATUS_BABBLE      (1 << 4)
#define EHCI_QTD_STATUS_XACTERR     (1 << 3)
#define EHCI_QTD_STATUS_MISSED      (1 << 2)
#define EHCI_QTD_STATUS_SPLIT       (1 << 1)
#define EHCI_QTD_STATUS_PING        (1 << 0)

#define EHCI_QTD_PID_OUT            (0 << 8)
#define EHCI_QTD_PID_IN             (1 << 8)
#define EHCI_QTD_PID_SETUP          (2 << 8)
#define EHCI_QTD_CERR_SHIFT         10
#define EHCI_QTD_CERR(n)            ((n) << EHCI_QTD_CERR_SHIFT)
#define EHCI_QTD_CPAGE_MASK         (7 << 12)
#define EHCI_QTD_IOC                (1 << 15)
#define EHCI_QTD_TOTAL_BYTES_SHIFT  16
#define EHCI_QTD_TOTAL_BYTES(n)     (((n) & 0x7FFF) << EHCI_QTD_TOTAL_BYTES_SHIFT)
#define EHCI_QTD_DT                 (1 << 31)

/* Queue Head (QH) */
struct ehci_qh {
    uint32_t qh_link;           /* Queue Head Link Pointer */
    uint32_t ep_char;           /* Endpoint Characteristics */
    uint32_t ep_cap;            /* Endpoint Capabilities */
    uint32_t curr_qtd;          /* Current qTD Pointer */
    uint32_t next_qtd;          /* Next qTD Pointer */
    uint32_t alt_qtd;           /* Alternate Next qTD Pointer */
    uint32_t token;             /* Token (overlay) */
    uint32_t buffer[5];         /* Buffer Pointers (overlay) */
} __attribute__((packed, aligned(32)));

/* QH Link 字段 */
#define EHCI_QH_LINK_TYPE_QH    (1 << 1)
#define EHCI_QH_LINK_TYPE_ITD   (0 << 1)
#define EHCI_QH_LINK_TYPE_SITD  (2 << 1)
#define EHCI_QH_LINK_TYPE_FSTN  (3 << 1)
#define EHCI_QH_LINK_T          (1 << 0)    /* Terminate */

/* QH Endpoint Characteristics 字段 */
#define EHCI_QH_EP_DEVADDR_SHIFT    0
#define EHCI_QH_EP_DEVADDR(n)       ((n) << EHCI_QH_EP_DEVADDR_SHIFT)
#define EHCI_QH_EP_NUM_SHIFT        8
#define EHCI_QH_EP_NUM(n)           ((n) << EHCI_QH_EP_NUM_SHIFT)
#define EHCI_QH_EP_SPEED_SHIFT      12
#define EHCI_QH_EP_SPEED(n)         ((n) << EHCI_QH_EP_SPEED_SHIFT)
#define EHCI_QH_EP_SPEED_FULL       0
#define EHCI_QH_EP_SPEED_LOW        1
#define EHCI_QH_EP_SPEED_HIGH       2
#define EHCI_QH_EP_DTC              (1 << 14)
#define EHCI_QH_EP_H                (1 << 15)
#define EHCI_QH_EP_MAXPKT_SHIFT     16
#define EHCI_QH_EP_MAXPKT(n)        ((n) << EHCI_QH_EP_MAXPKT_SHIFT)
#define EHCI_QH_EP_C                (1 << 27)
#define EHCI_QH_EP_RL_SHIFT         28
#define EHCI_QH_EP_RL(n)            ((n) << EHCI_QH_EP_RL_SHIFT)

/* QH Endpoint Capabilities 字段 */
#define EHCI_QH_CAP_MULT_SHIFT      30
#define EHCI_QH_CAP_MULT(n)         ((n) << EHCI_QH_CAP_MULT_SHIFT)
#define EHCI_QH_CAP_HUBPORT_SHIFT   23
#define EHCI_QH_CAP_HUBPORT(n)      ((n) << EHCI_QH_CAP_HUBPORT_SHIFT)
#define EHCI_QH_CAP_HUBADDR_SHIFT   16
#define EHCI_QH_CAP_HUBADDR(n)      ((n) << EHCI_QH_CAP_HUBADDR_SHIFT)
#define EHCI_QH_CAP_CMASK_SHIFT     8
#define EHCI_QH_CAP_CMASK(n)        ((n) << EHCI_QH_CAP_CMASK_SHIFT)
#define EHCI_QH_CAP_SMASK_SHIFT     0
#define EHCI_QH_CAP_SMASK(n)        ((n) << EHCI_QH_CAP_SMASK_SHIFT)
#define EHCI_QH_CAP_CTL             (1 << 15)

/* ========== EHCI 控制器私有数据 ========== */

struct ehci_priv {
    uint64_t mmio_base;
    uint32_t op_offset;
    
    uint32_t num_ports;
    int extended_caps;
    
    /* 异步队列 */
    struct ehci_qh *async_qh;
    uint64_t async_qh_phys;
    
    /* 周期帧列表 */
    uint32_t *periodic_list;
    uint64_t periodic_list_phys;
    
    /* 已分配的地址 */
    uint8_t address_map[128];
    int next_address;
};

/* ========== 内联函数 ========== */

static inline uint8_t ehci_read8(uint64_t addr) {
    return *(volatile uint8_t *)addr;
}

static inline uint32_t ehci_read32(uint64_t addr) {
    return *(volatile uint32_t *)addr;
}

static inline void ehci_write32(uint64_t addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}

/* ========== 端口操作 ========== */

static int ehci_port_reset(struct usb_controller *ctrl, int port) {
    struct ehci_priv *priv = ctrl->private_data;
    uint64_t portsc = priv->mmio_base + priv->op_offset + EHCI_PORTSC_BASE + (port * 4);
    
    uint32_t val = ehci_read32(portsc);
    
    print("[EHCI] Port ");
    print_dec(port);
    print(" initial status: 0x");
    print_hex(val);
    println("");
    
    /* 检查是否已连接 */
    if (!(val & EHCI_PORT_CCS)) {
        print("[EHCI] Port ");
        print_dec(port);
        println(" - no device connected");
        return -1;
    }
    
    /* 检查设备速度（复位前通过Line Status判断） */
    uint32_t speed = val & EHCI_PORT_SPEED_MASK;
    print("[EHCI] Port ");
    print_dec(port);
    print(" line_status: 0x");
    print_hex(speed);
    println("");

    /* 只有K-state (bit 10 = 1) 表示低速设备，需要转交给同伴控制器
     * SE0 (0x00) = 全速设备，可以由EHCI通过事务翻译器处理 */
    if (speed == EHCI_PORT_SPEED_KSTATE) {
        print("[EHCI] Low-speed device detected (K-state), handing off to companion...");
        println("");
        
        /* 设置端口归属为同伴控制器 */
        val |= EHCI_PORT_OWNER;
        ehci_write32(portsc, val);
        
        delay_ms(10);
        val = ehci_read32(portsc);
        print("[EHCI] After handoff, port status: 0x");
        print_hex(val);
        println("");
        
        return -1;  /* 不支持低速设备，由同伴控制器处理 */
    }
    
    /* 开启端口电源 */
    if (!(val & EHCI_PORT_PP)) {
        print("[EHCI] Enabling port power...");
        println("");
        val |= EHCI_PORT_PP;
        ehci_write32(portsc, val);
        delay_ms(20);
        val = ehci_read32(portsc);
        print("[EHCI] After power enable: 0x");
        print_hex(val);
        println("");
    }
    
    /* 清除状态变化位 */
    val = ehci_read32(portsc);
    val |= EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC;
    ehci_write32(portsc, val);
    
    /* 发起端口复位 */
    print("[EHCI] Starting port reset...");
    println("");
    val = ehci_read32(portsc);
    /* 先清除变化位再设置复位 */
    val |= EHCI_PORT_CSC | EHCI_PORT_PEC;
    val |= EHCI_PORT_RESET;
    ehci_write32(portsc, val);

    /* 等待复位信号生效（EHCI规范：至少50ms） */
    delay_ms(60);

    /* 手动清除PORT_RESET位（QEMU需要驱动清除此位） */
    val = ehci_read32(portsc);
    val &= ~EHCI_PORT_RESET;
    ehci_write32(portsc, val);

    /* 等待端口使能 */
    int reset_timeout = 200;
    while (reset_timeout-- > 0) {
        val = ehci_read32(portsc);
        if (val & EHCI_PORT_PE) {
            break;
        }
        if (!(val & EHCI_PORT_CCS)) {
            print("[EHCI] Device disconnected during reset");
            println("");
            return -1;
        }
        delay_ms(1);
    }

    val = ehci_read32(portsc);
    print("[EHCI] After reset, port status: 0x");
    print_hex(val);
    println("");

    if (!(val & EHCI_PORT_PE)) {
        print("[EHCI] Port ");
        print_dec(port);
        println(" failed to enable after reset");
        return -1;
    }

    /* 检查PORT_RESET是否已清除 */
    if (val & EHCI_PORT_RESET) {
        print("[EHCI] Port reset bit still set, forcing clear...");
        println("");
        val &= ~EHCI_PORT_RESET;
        ehci_write32(portsc, val);
    }

    /* 清除状态变化位 */
    val = ehci_read32(portsc);
    val |= EHCI_PORT_CSC | EHCI_PORT_PEC;
    ehci_write32(portsc, val);

    /* 打印最终速度信息 */
    uint32_t final_speed = val & EHCI_PORT_SPEED_MASK;
    print("[EHCI] Port ");
    print_dec(port);
    print(" speed after reset: 0x");
    print_hex(final_speed);
    println("");

    print("[EHCI] Port ");
    print_dec(port);
    println(" reset successful");

    return 0;
}

static int ehci_port_enable(struct usb_controller *ctrl, int port) {
    struct ehci_priv *priv = ctrl->private_data;
    uint64_t portsc = priv->mmio_base + priv->op_offset + EHCI_PORTSC_BASE + (port * 4);
    
    uint32_t val = ehci_read32(portsc);
    return (val & EHCI_PORT_PE) ? 0 : -1;
}

static usb_speed_t ehci_port_get_speed(struct usb_controller *ctrl, int port) {
    struct ehci_priv *priv = ctrl->private_data;
    uint64_t portsc = priv->mmio_base + priv->op_offset + EHCI_PORTSC_BASE + (port * 4);

    uint32_t val = ehci_read32(portsc);

    if (!(val & EHCI_PORT_PE)) {
        return USB_SPEED_FULL;  /* 端口未使能 */
    }

    /* Line Status位指示设备速度：
     * 00 = Full Speed (SE0)
     * 01 = Low Speed (K-state) - 复位前
     * 10 = High Speed (复位后握手成功) */
    uint32_t line_status = val & EHCI_PORT_SPEED_MASK;
    switch (line_status) {
        case EHCI_PORT_SPEED_KSTATE:
            return USB_SPEED_LOW;
        case EHCI_PORT_SPEED_HIGH:
            return USB_SPEED_HIGH;
        case EHCI_PORT_SPEED_FULL:
        default:
            return USB_SPEED_FULL;
    }
}

static int ehci_port_status(struct usb_controller *ctrl, int port) {
    struct ehci_priv *priv = ctrl->private_data;
    uint64_t portsc = priv->mmio_base + priv->op_offset + EHCI_PORTSC_BASE + (port * 4);
    
    uint32_t val = ehci_read32(portsc);
    
    return (val & EHCI_PORT_CCS) ? 1 : 0;
}

/* ========== 地址分配 ========== */

static int ehci_alloc_address(struct usb_controller *ctrl) {
    struct ehci_priv *priv = ctrl->private_data;
    
    for (int i = priv->next_address; i < 128; i++) {
        if (!priv->address_map[i]) {
            priv->address_map[i] = 1;
            priv->next_address = i + 1;
            return i;
        }
    }
    
    for (int i = 1; i < priv->next_address; i++) {
        if (!priv->address_map[i]) {
            priv->address_map[i] = 1;
            priv->next_address = i + 1;
            return i;
        }
    }
    
    return -1;
}

static void ehci_free_address(struct usb_controller *ctrl, int addr) {
    struct ehci_priv *priv = ctrl->private_data;
    
    if (addr > 0 && addr < 128) {
        priv->address_map[addr] = 0;
        if (addr < priv->next_address) {
            priv->next_address = addr;
        }
    }
}

/* ========== 传输实现 ========== */

/* 创建 qTD (32字节对齐，DMA要求) */
static struct ehci_qtd *ehci_create_qtd(void *data, int len, int pid, int ioc) {
    struct ehci_qtd *qtd = kvalloc(sizeof(struct ehci_qtd), 32);
    if (!qtd) return NULL;

    memset(qtd, 0, sizeof(*qtd));

    /* pid已经包含移位（EHCI_QTD_PID_*宏已<<8），直接使用 */
    qtd->token = EHCI_QTD_STATUS_ACTIVE |
                 EHCI_QTD_CERR(3) |
                 pid |
                 EHCI_QTD_TOTAL_BYTES(len);

    if (ioc) qtd->token |= EHCI_QTD_IOC;

    if (data && len > 0) {
        uint64_t phys_addr = ehci_virt_to_phys(data);
        /* buffer[0]: 完整物理地址（含页内偏移） */
        qtd->buffer[0] = (uint32_t)phys_addr;
        /* buffer[1..4]: 后续页的页对齐物理地址 */
        uint64_t next_page = (phys_addr + 0x1000) & ~0xFFFULL;
        int remaining = len - (int)(next_page - phys_addr);
        for (int i = 1; i < 5 && remaining > 0; i++) {
            qtd->buffer[i] = (uint32_t)next_page;
            next_page += 0x1000;
            remaining -= 0x1000;
        }
    }

    qtd->next_qtd = 1;  /* Terminate */
    qtd->alt_next_qtd = 1;

    return qtd;
}

/* 创建带data toggle的qTD（用于中断传输） */
static struct ehci_qtd *ehci_create_qtd_dt(void *data, int len, int pid, int ioc, int data_toggle) {
    struct ehci_qtd *qtd = ehci_create_qtd(data, len, pid, ioc);
    if (!qtd) return NULL;
    if (data_toggle) qtd->token |= EHCI_QTD_DT;
    return qtd;
}

static int ehci_submit_control(struct usb_device *dev,
                               uint8_t request_type, uint8_t request,
                               uint16_t value, uint16_t index,
                               void *data, uint16_t length) {
    struct ehci_priv *priv = dev->controller->private_data;

    print("[EHCI_CTRL] dev->addr="); print_dec(dev->address);
    print(" speed="); print_dec(dev->speed);
    print(" req=0x"); print_hex(request);
    print(" len="); print_dec(length);
    println("");

    /* 创建 QH (32字节对齐) */
    struct ehci_qh *qh = kvalloc(sizeof(struct ehci_qh), 32);
    if (!qh) return -1;

    memset(qh, 0, sizeof(*qh));

    /* 根据设备速度设置QH参数 */
    int ehci_speed;
    switch (dev->speed) {
        case USB_SPEED_LOW:  ehci_speed = EHCI_QH_EP_SPEED_LOW;  break;
        case USB_SPEED_FULL: ehci_speed = EHCI_QH_EP_SPEED_FULL; break;
        case USB_SPEED_HIGH: ehci_speed = EHCI_QH_EP_SPEED_HIGH; break;
        default:             ehci_speed = EHCI_QH_EP_SPEED_FULL; break;
    }

    int max_pkt = (dev->speed == USB_SPEED_LOW) ? 8 : 64;

    qh->ep_char = EHCI_QH_EP_DEVADDR(dev->address) |
                  EHCI_QH_EP_NUM(0) |
                  EHCI_QH_EP_SPEED(ehci_speed) |
                  EHCI_QH_EP_MAXPKT(max_pkt) |
                  EHCI_QH_EP_DTC;

    qh->ep_cap = EHCI_QH_CAP_MULT(1);

    /* 全速/低速设备在EHCI上需要分割事务 */
    if (dev->speed != USB_SPEED_HIGH) {
        qh->ep_char |= EHCI_QH_EP_C;  /* 控制端点分割事务标志 */
        qh->ep_cap |= EHCI_QH_CAP_HUBADDR(0) | EHCI_QH_CAP_HUBPORT(dev->port) |
                      EHCI_QH_CAP_CMASK(0x1C) | EHCI_QH_CAP_SMASK(0x01);
    }

    /* QH链接 - 使用物理地址 */
    qh->qh_link = (uint32_t)priv->async_qh_phys | EHCI_QH_LINK_TYPE_QH;

    /* Setup包 - 堆分配确保DMA可访问 */
    uint8_t *setup_pkt = kmalloc(8);
    if (!setup_pkt) {
        kfree(qh);
        return -1;
    }
    setup_pkt[0] = request_type;
    setup_pkt[1] = request;
    setup_pkt[2] = value & 0xFF;
    setup_pkt[3] = (value >> 8) & 0xFF;
    setup_pkt[4] = index & 0xFF;
    setup_pkt[5] = (index >> 8) & 0xFF;
    setup_pkt[6] = length & 0xFF;
    setup_pkt[7] = (length >> 8) & 0xFF;

    print("[EHCI_CTRL] setup_pkt virt=0x"); print_hex((uint64_t)setup_pkt);
    print(" phys=0x"); print_hex(ehci_virt_to_phys(setup_pkt));
    println("");

    /* 创建 Setup qTD */
    struct ehci_qtd *setup_qtd = ehci_create_qtd(setup_pkt, 8, EHCI_QTD_PID_SETUP, 0);
    if (!setup_qtd) {
        kfree(setup_pkt);
        kfree(qh);
        return -1;
    }

    /* IN传输需要DMA安全缓冲区 */
    uint8_t *dma_data_buf = data;
    int need_copy_back = 0;
    if (length > 0 && data && (request_type & USB_DIR_IN)) {
        dma_data_buf = kmalloc(length);
        if (!dma_data_buf) {
            kfree(setup_qtd);
            kfree(setup_pkt);
            kfree(qh);
            return -1;
        }
        need_copy_back = 1;
    }

    /* 创建 Data qTD */
    struct ehci_qtd *data_qtd = NULL;
    if (length > 0 && dma_data_buf) {
        int pid = (request_type & USB_DIR_IN) ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT;
        data_qtd = ehci_create_qtd(dma_data_buf, length, pid, 0);
        if (!data_qtd) {
            if (need_copy_back) kfree(dma_data_buf);
            kfree(setup_qtd);
            kfree(setup_pkt);
            kfree(qh);
            return -1;
        }
        setup_qtd->next_qtd = (uint32_t)ehci_virt_to_phys(data_qtd);
    }

    /* 创建 Status qTD */
    int status_pid = (request_type & USB_DIR_IN) ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN;
    struct ehci_qtd *status_qtd = ehci_create_qtd(NULL, 0, status_pid, 1);
    if (!status_qtd) {
        if (data_qtd) kfree(data_qtd);
        if (need_copy_back) kfree(dma_data_buf);
        kfree(setup_qtd);
        kfree(setup_pkt);
        kfree(qh);
        return -1;
    }

    if (data_qtd) {
        data_qtd->next_qtd = (uint32_t)ehci_virt_to_phys(status_qtd);
    } else {
        setup_qtd->next_qtd = (uint32_t)ehci_virt_to_phys(status_qtd);
    }

    /* 链接 QH 和 qTD - 使用物理地址 */
    qh->next_qtd = (uint32_t)ehci_virt_to_phys(setup_qtd);

    print("[EHCI_CTRL] qh phys=0x"); print_hex(ehci_virt_to_phys(qh));
    print(" setup_qtd phys=0x"); print_hex(ehci_virt_to_phys(setup_qtd));
    if (data_qtd) { print(" data_qtd phys=0x"); print_hex(ehci_virt_to_phys(data_qtd)); }
    print(" status_qtd phys=0x"); print_hex(ehci_virt_to_phys(status_qtd));
    println("");

    print("[EHCI_CTRL] qh->ep_char=0x"); print_hex(qh->ep_char);
    print(" qh->next_qtd=0x"); print_hex(qh->next_qtd);
    println("");

    /* 将 QH 加入异步队列 */
    struct ehci_qh *async_head = priv->async_qh;
    qh->qh_link = async_head->qh_link;
    async_head->qh_link = (uint32_t)ehci_virt_to_phys(qh) | EHCI_QH_LINK_TYPE_QH;

    /* 启动异步队列 */
    uint64_t op_base = priv->mmio_base + priv->op_offset;

    /* 清除USBSTS中所有挂起的中断状态位（前次传输可能遗留IAA等） */
    uint32_t old_sts = ehci_read32(op_base + EHCI_USBSTS);
    if (old_sts & 0x3F) {
        ehci_write32(op_base + EHCI_USBSTS, old_sts);
    }

    uint32_t cmd = ehci_read32(op_base + EHCI_USBCMD);
    cmd |= EHCI_CMD_ASEN;
    ehci_write32(op_base + EHCI_USBCMD, cmd);

    /* 响IAAD门铃，确保HC刷新async schedule缓存并看到新QH */
    ehci_write32(op_base + EHCI_USBCMD,
                 ehci_read32(op_base + EHCI_USBCMD) | EHCI_CMD_IAAD);
    int doorbell_timeout = 10;
    while (doorbell_timeout-- > 0) {
        if (ehci_read32(op_base + EHCI_USBSTS) & EHCI_STS_IAA) {
            ehci_write32(op_base + EHCI_USBSTS, EHCI_STS_IAA);
            break;
        }
        delay_ms(1);
    }
    if (doorbell_timeout <= 0) {
        print("[EHCI_CTRL] IAAD doorbell TIMEOUT before transfer!");
        println("");
    }

    /* 清除IAAD可能留下的其他状态位 */
    old_sts = ehci_read32(op_base + EHCI_USBSTS);
    if (old_sts & 0x3F) {
        ehci_write32(op_base + EHCI_USBSTS, old_sts);
    }

    /* 检查异步队列状态 */
    uint32_t sts = ehci_read32(op_base + EHCI_USBSTS);
    print("[EHCI_CTRL] USBSTS=0x"); print_hex(sts);
    print(" USBCMD=0x"); print_hex(ehci_read32(op_base + EHCI_USBCMD));
    print(" ASYNCLIST=0x"); print_hex(ehci_read32(op_base + EHCI_ASYNCLIST));
    print(" async_head->qh_link=0x"); print_hex(async_head->qh_link);
    println("");

    /* 等待完成 */
    int timeout = 500;
    int iter = 0;
    while (timeout-- > 0) {
        if (!(status_qtd->token & EHCI_QTD_STATUS_ACTIVE)) {
            break;
        }
        if (iter++ % 100 == 0) {
            print("[EHCI_CTRL] Waiting... setup=0x"); print_hex(setup_qtd->token);
            if (data_qtd) { print(" data=0x"); print_hex(data_qtd->token); }
            print(" status=0x"); print_hex(status_qtd->token);
            print(" QH_overlay=0x"); print_hex(qh->token);
            print(" QH_curr=0x"); print_hex(qh->curr_qtd);
            print(" USBSTS=0x"); print_hex(ehci_read32(op_base + EHCI_USBSTS));
            println("");
        }
        delay_ms(1);
    }

    int ret = 0;
    if (status_qtd->token & EHCI_QTD_STATUS_HALTED) {
        print("[EHCI_CTRL] Transfer HALTED");
        println("");
        ret = -1;
    } else if (timeout <= 0) {
        print("[EHCI_CTRL] Transfer TIMEOUT");
        println("");
        ret = -2;
    } else {
        print("[EHCI_CTRL] Transfer OK");
        println("");
        /* IN传输复制数据回原缓冲区 */
        if (need_copy_back && ret == 0) {
            memcpy(data, dma_data_buf, length);
        }
    }

    print("[EHCI_CTRL] setup_qtd token=0x"); print_hex(setup_qtd->token);
    if (data_qtd) { print(" data_qtd token=0x"); print_hex(data_qtd->token); }
    print(" status_qtd token=0x"); print_hex(status_qtd->token);
    println("");

    /* 从异步队列移除QH */
    async_head->qh_link = qh->qh_link;

    /* IAAD门铃：确保HC已卸载QH再释放 */
    ehci_write32(op_base + EHCI_USBCMD,
                 ehci_read32(op_base + EHCI_USBCMD) | EHCI_CMD_IAAD);
    int db_timeout = 10;
    while (db_timeout-- > 0) {
        if (ehci_read32(op_base + EHCI_USBSTS) & EHCI_STS_IAA) {
            ehci_write32(op_base + EHCI_USBSTS, EHCI_STS_IAA);
            break;
        }
        delay_ms(1);
    }

    /* 清理 */
    kfree(setup_pkt);
    if (need_copy_back) kfree(dma_data_buf);
    kfree(qh);
    kfree(setup_qtd);
    if (data_qtd) kfree(data_qtd);
    kfree(status_qtd);

    return ret;
}

static int ehci_submit_bulk(struct usb_device *dev, int endpoint,
                            void *data, int length, int direction) {
    struct ehci_priv *priv = dev->controller->private_data;

    /* 创建 QH (32字节对齐) */
    struct ehci_qh *qh = kvalloc(sizeof(struct ehci_qh), 32);
    if (!qh) return -1;

    memset(qh, 0, sizeof(*qh));

    /* 根据设备速度设置QH参数 */
    int ehci_speed;
    switch (dev->speed) {
        case USB_SPEED_LOW:  ehci_speed = EHCI_QH_EP_SPEED_LOW;  break;
        case USB_SPEED_FULL: ehci_speed = EHCI_QH_EP_SPEED_FULL; break;
        case USB_SPEED_HIGH: ehci_speed = EHCI_QH_EP_SPEED_HIGH; break;
        default:             ehci_speed = EHCI_QH_EP_SPEED_FULL; break;
    }

    int max_pkt = (dev->speed == USB_SPEED_HIGH) ? 512 : 64;

    qh->ep_char = EHCI_QH_EP_DEVADDR(dev->address) |
                  EHCI_QH_EP_NUM(endpoint) |
                  EHCI_QH_EP_SPEED(ehci_speed) |
                  EHCI_QH_EP_MAXPKT(max_pkt);

    qh->ep_cap = EHCI_QH_CAP_MULT(1);

    /* 全速/低速设备需要分割事务 */
    if (dev->speed != USB_SPEED_HIGH) {
        qh->ep_char |= EHCI_QH_EP_C;
        qh->ep_cap |= EHCI_QH_CAP_HUBADDR(0) | EHCI_QH_CAP_HUBPORT(dev->port) |
                      EHCI_QH_CAP_CMASK(0x1C) | EHCI_QH_CAP_SMASK(0x01);
    }

    /* 创建 qTD */
    int pid = direction ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT;
    struct ehci_qtd *qtd = ehci_create_qtd(data, length, pid, 1);
    if (!qtd) {
        kfree(qh);
        return -1;
    }

    qh->next_qtd = (uint32_t)ehci_virt_to_phys(qtd);

    /* 加入异步队列 */
    struct ehci_qh *async_head = priv->async_qh;
    qh->qh_link = async_head->qh_link;
    async_head->qh_link = (uint32_t)ehci_virt_to_phys(qh) | EHCI_QH_LINK_TYPE_QH;

    /* 启动异步队列 */
    uint64_t op_base = priv->mmio_base + priv->op_offset;

    /* 清除USBSTS中所有挂起的中断状态位 */
    uint32_t old_sts = ehci_read32(op_base + EHCI_USBSTS);
    if (old_sts & 0x3F) {
        ehci_write32(op_base + EHCI_USBSTS, old_sts);
    }

    uint32_t cmd = ehci_read32(op_base + EHCI_USBCMD);
    cmd |= EHCI_CMD_ASEN;
    ehci_write32(op_base + EHCI_USBCMD, cmd);

    /* IAAD门铃：确保HC看到新加入的QH */
    ehci_write32(op_base + EHCI_USBCMD,
                 ehci_read32(op_base + EHCI_USBCMD) | EHCI_CMD_IAAD);
    int doorbell_timeout = 10;
    while (doorbell_timeout-- > 0) {
        if (ehci_read32(op_base + EHCI_USBSTS) & EHCI_STS_IAA) {
            ehci_write32(op_base + EHCI_USBSTS, EHCI_STS_IAA);
            break;
        }
        delay_ms(1);
    }

    /* 清除IAAD后可能的状态位 */
    old_sts = ehci_read32(op_base + EHCI_USBSTS);
    if (old_sts & 0x3F) {
        ehci_write32(op_base + EHCI_USBSTS, old_sts);
    }

    /* 检查异步队列状态 */
    uint32_t sts = ehci_read32(op_base + EHCI_USBSTS);
    print("[EHCI_BULK] USBSTS=0x"); print_hex(sts);
    print(" USBCMD=0x"); print_hex(ehci_read32(op_base + EHCI_USBCMD));
    println("");

    /* 等待完成 */
    int timeout = 1000;
    while (timeout-- > 0) {
        if (!(qtd->token & EHCI_QTD_STATUS_ACTIVE)) {
            break;
        }
        delay_ms(1);
    }

    int ret = length;
    if (qtd->token & (EHCI_QTD_STATUS_HALTED | EHCI_QTD_STATUS_DBERR |
                       EHCI_QTD_STATUS_BABBLE | EHCI_QTD_STATUS_XACTERR)) {
        ret = -1;
    } else if (timeout <= 0) {
        ret = -2;
    }

    /* 从异步队列移除QH */
    async_head->qh_link = qh->qh_link;

    /* IAAD门铃 */
    ehci_write32(op_base + EHCI_USBCMD,
                 ehci_read32(op_base + EHCI_USBCMD) | EHCI_CMD_IAAD);
    int db_timeout = 10;
    while (db_timeout-- > 0) {
        if (ehci_read32(op_base + EHCI_USBSTS) & EHCI_STS_IAA) {
            ehci_write32(op_base + EHCI_USBSTS, EHCI_STS_IAA);
            break;
        }
        delay_ms(1);
    }

    kfree(qh);
    kfree(qtd);

    return ret;
}

static int ehci_submit_interrupt(struct usb_device *dev, int endpoint,
                                 void *data, int length) {
    struct ehci_priv *priv = dev->controller->private_data;

    /* 创建 QH (32字节对齐) */
    struct ehci_qh *qh = kvalloc(sizeof(struct ehci_qh), 32);
    if (!qh) return -1;
    memset(qh, 0, sizeof(*qh));

    /* 根据设备速度设置QH参数 */
    int ehci_speed;
    switch (dev->speed) {
        case USB_SPEED_LOW:  ehci_speed = EHCI_QH_EP_SPEED_LOW;  break;
        case USB_SPEED_FULL: ehci_speed = EHCI_QH_EP_SPEED_FULL; break;
        case USB_SPEED_HIGH: ehci_speed = EHCI_QH_EP_SPEED_HIGH; break;
        default:             ehci_speed = EHCI_QH_EP_SPEED_FULL; break;
    }

    /* 中断端点的max_packet_size从设备端点描述符获取 */
    int max_pkt = 8;
    for (int i = 0; i < dev->num_interfaces; i++) {
        struct usb_interface *iface = &dev->interfaces[i];
        for (int j = 0; j < iface->num_endpoints; j++) {
            if (iface->endpoints[j].address == endpoint) {
                max_pkt = iface->endpoints[j].max_packet_size;
                if (max_pkt == 0) max_pkt = 8;
                break;
            }
        }
    }

    qh->ep_char = EHCI_QH_EP_DEVADDR(dev->address) |
                  EHCI_QH_EP_NUM(endpoint & 0x0F) |
                  EHCI_QH_EP_SPEED(ehci_speed) |
                  EHCI_QH_EP_MAXPKT(max_pkt) |
                  EHCI_QH_EP_DTC;

    qh->qh_link = 1;  /* Terminate — 不要跟随任何链接 */
    qh->ep_cap = EHCI_QH_CAP_MULT(1);

    /* 全速/低速设备需要分割事务 */
    if (dev->speed != USB_SPEED_HIGH) {
        qh->ep_cap |= EHCI_QH_CAP_HUBADDR(0) | EHCI_QH_CAP_HUBPORT(dev->port) |
                      EHCI_QH_CAP_CMASK(0x1C) | EHCI_QH_CAP_SMASK(0x01);
    }

    /* 分配DMA安全缓冲区 */
    uint8_t *dma_buf = kmalloc(length);
    if (!dma_buf) {
        kfree(qh);
        return -1;
    }
    memset(dma_buf, 0, length);

    /* 创建 IN qTD */
    static int intr_data_toggle = 0;
    struct ehci_qtd *qtd = ehci_create_qtd_dt(dma_buf, length, EHCI_QTD_PID_IN, 1, intr_data_toggle);
    if (!qtd) {
        kfree(dma_buf);
        kfree(qh);
        return -1;
    }

    qh->curr_qtd = 0;  /* 无当前qTD - HC从next_qtd获取 */
    qh->next_qtd = (uint32_t)ehci_virt_to_phys(qtd);
    qh->alt_qtd = 1;  /* Terminate alternate */
    /* 复制qTD的token到overlay区域，保持ACTIVE位 */
    qh->token = qtd->token;

    /* 将QH加入周期帧列表 */
    uint32_t qh_phys = (uint32_t)ehci_virt_to_phys(qh);
    /* 帧列表条目: T=0, Type=01(QH), 指针 */
    uint32_t frame_entry = (qh_phys & ~0x1F) | EHCI_QH_LINK_TYPE_QH;  /* T=0, Type=QH */

    /* 在帧列表中放入QH - 每隔1帧（每帧都放，确保1ms内被调度） */
    static uint32_t saved_framelist_entries[16];
    for (int i = 0; i < 16; i++) {
        int idx = i * 64;  /* 1024/16 = 64, 每隔64帧放一个 */
        if (idx >= 1024) break;
        saved_framelist_entries[i] = priv->periodic_list[idx];
        priv->periodic_list[idx] = frame_entry;
    }

    /* 启用周期调度 */
    uint64_t op_base = priv->mmio_base + priv->op_offset;
    uint32_t cmd = ehci_read32(op_base + EHCI_USBCMD);
    if (!(cmd & EHCI_CMD_PSEN)) {
        cmd |= EHCI_CMD_PSEN;
        ehci_write32(op_base + EHCI_USBCMD, cmd);
    }

    /* 等待传输完成 - 中断端点每1ms轮询一次 */
    int timeout = 10;  /* 短超时：用于轮询模式，避免阻塞太久 */
    while (timeout-- > 0) {
        if (!(qtd->token & EHCI_QTD_STATUS_ACTIVE)) {
            break;
        }
        (void)ehci_read32(op_base + EHCI_USBSTS);
        delay_ms(1);
    }

    int ret = length;
    if (qtd->token & (EHCI_QTD_STATUS_HALTED | EHCI_QTD_STATUS_DBERR |
                       EHCI_QTD_STATUS_BABBLE | EHCI_QTD_STATUS_XACTERR)) {
        ret = -1;
        intr_data_toggle = 0;
        print("[EHCI_INTR] HALTED token=0x"); print_hex((uint32_t)qtd->token); println("");
    } else if (timeout <= 0) {
        ret = -2;
        print("[EHCI_INTR] TIMEOUT token=0x"); print_hex((uint32_t)qtd->token);
        print(" PFLentry=0x"); print_hex(priv->periodic_list[0]);
        print(" qh_phys=0x"); print_hex(qh_phys);
        print(" frame_entry=0x"); print_hex(frame_entry);
        print(" PERIODICLIST=0x"); print_hex(ehci_read32(op_base + EHCI_PERIODICLIST));
        print(" USBSTS=0x"); print_hex(ehci_read32(op_base + EHCI_USBSTS));
        print(" USBCMD=0x"); print_hex(ehci_read32(op_base + EHCI_USBCMD));
        println("");
    } else {
        memcpy(data, dma_buf, length);
        int total_bytes = (qtd->token >> EHCI_QTD_TOTAL_BYTES_SHIFT) & 0x7FFF;
        ret = length - total_bytes;
        if (ret > 0) intr_data_toggle ^= 1;
        print("[EHCI_INTR] OK ret="); print_dec(ret); println("");
    }

    /* 从周期帧列表移除QH */
    for (int i = 0; i < 16; i++) {
        int idx = i * 64;
        if (idx >= 1024) break;
        priv->periodic_list[idx] = saved_framelist_entries[i];
    }

    kfree(dma_buf);
    kfree(qh);
    kfree(qtd);

    return ret;
}

/* ========== EHCI 控制器操作 ========== */

static const struct usb_controller_ops ehci_ops = {
    .port_reset = ehci_port_reset,
    .port_enable = ehci_port_enable,
    .port_get_speed = ehci_port_get_speed,
    .port_status = ehci_port_status,
    .submit_control = ehci_submit_control,
    .submit_bulk = ehci_submit_bulk,
    .submit_interrupt = ehci_submit_interrupt,
    .alloc_address = ehci_alloc_address,
    .free_address = ehci_free_address,
};

/* ========== EHCI 初始化 ========== */

static int ehci_reset(struct ehci_priv *priv) {
    uint64_t op_base = priv->mmio_base + priv->op_offset;
    
    /* 发起复位 */
    ehci_write32(op_base + EHCI_USBCMD, EHCI_CMD_HCRESET);
    
    /* 等待复位完成 */
    int timeout = 1000;
    while (timeout-- > 0) {
        uint32_t cmd = ehci_read32(op_base + EHCI_USBCMD);
        if (!(cmd & EHCI_CMD_HCRESET)) break;
        delay_ms(1);
    }
    
    return (timeout > 0) ? 0 : -1;
}

int ehci_init(struct usb_controller *ctrl) {
    struct ehci_priv *priv;
    uint32_t hcs_params;
    uint64_t op_base;
    uint64_t mmio_phys;
    
    /* 分配私有数据 */
    priv = kmalloc(sizeof(struct ehci_priv));
    if (!priv) {
        return -1;
    }
    
    memset(priv, 0, sizeof(*priv));
    
    /* 保存物理地址并映射 MMIO 区域 */
    mmio_phys = ctrl->mmio_base;
    
    /* 检查物理地址是否有效 */
    if (mmio_phys == 0) {
        kfree(priv);
        return -1;
    }
    
    /* 将 MMIO 物理地址映射到内核虚拟地址空间 */
    priv->mmio_base = (uint64_t)phys_to_virt(mmio_phys);
    
    if (priv->mmio_base == 0) {
        kfree(priv);
        return -1;
    }
    
    /* 确保 MMIO 区域被正确映射 (禁用缓存) */
    extern struct address_space *vmm_get_kernel_space(void);
    extern int vmm_map_page(struct address_space *space, uint64_t virt, uint64_t phys, uint64_t flags);
    
    uint64_t mmio_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | 
                          VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITETHROUGH |
                          VMM_FLAG_NX;
    
    /* EHCI MMIO 区域通常为 4KB-64KB，先映射一页 */
    vmm_map_page(vmm_get_kernel_space(), priv->mmio_base, mmio_phys, mmio_flags);
    
    priv->op_offset = ehci_read8(priv->mmio_base + EHCI_CAPLENGTH);
    priv->next_address = 1;
    ctrl->private_data = priv;

    print("[EHCI] MMIO phys=0x"); print_hex(mmio_phys);
    print(" virt=0x"); print_hex(priv->mmio_base);
    print(" op_offset="); print_dec(priv->op_offset);
    println("");

    /* 读取能力参数 */
    hcs_params = ehci_read32(priv->mmio_base + EHCI_HCSPARAMS);
    priv->num_ports = EHCI_N_PORTS(hcs_params);

    print("[EHCI] num_ports="); print_dec(priv->num_ports);
    print(" HCSPARAMS=0x"); print_hex(hcs_params);
    println("");
    
    /* 复位控制器 */
    if (ehci_reset(priv) < 0) {
        kfree(priv);
        return -1;
    }
    
    op_base = priv->mmio_base + priv->op_offset;
    
    /* 创建异步队列头 (32字节对齐) */
    priv->async_qh = kvalloc(sizeof(struct ehci_qh), 32);
    if (!priv->async_qh) {
        kfree(priv);
        return -1;
    }

    memset(priv->async_qh, 0, sizeof(*priv->async_qh));
    priv->async_qh_phys = ehci_virt_to_phys(priv->async_qh);
    priv->async_qh->qh_link = (uint32_t)priv->async_qh_phys | EHCI_QH_LINK_TYPE_QH;
    priv->async_qh->ep_char = EHCI_QH_EP_H;

    print("[EHCI] async_qh virt=0x"); print_hex((uint64_t)priv->async_qh);
    print(" phys=0x"); print_hex(priv->async_qh_phys);
    println("");

    /* 创建周期帧列表 - 使用物理页确保页对齐 (EHCI要求4K对齐) */
    uint64_t pl_phys = pmm_alloc_pages(1);  /* 1页 = 4KB = 1024 * 4字节 */
    if (pl_phys == 0) {
        kfree(priv->async_qh);
        kfree(priv);
        return -1;
    }

    /* 通过HHDM映射到虚拟地址 */
    extern uint64_t hhdm_offset;
    priv->periodic_list = (uint32_t *)(pl_phys + hhdm_offset);
    priv->periodic_list_phys = pl_phys;

    print("[EHCI] periodic_list phys=0x"); print_hex(pl_phys);
    print(" virt=0x"); print_hex((uint64_t)priv->periodic_list);
    println("");

    for (int i = 0; i < 1024; i++) {
        priv->periodic_list[i] = 1;  /* Terminate */
    }

    /* 设置寄存器 - 使用物理地址 */
    ehci_write32(op_base + EHCI_ASYNCLIST, (uint32_t)priv->async_qh_phys);
    ehci_write32(op_base + EHCI_PERIODICLIST, (uint32_t)priv->periodic_list_phys);
    ehci_write32(op_base + EHCI_CTRLDSSEG, 0);
    
    /* 设置配置标志 (路由到 EHCI) */
    ehci_write32(op_base + EHCI_CONFIGFLAG, 1);
    
    /* 启动控制器 */
    uint32_t cmd = EHCI_CMD_RUNSTOP | EHCI_CMD_FLSZ_1024 | (8 << 16);
    ehci_write32(op_base + EHCI_USBCMD, cmd);
    
    /* 使能中断 */
    ehci_write32(op_base + EHCI_USBINTR, 0x1F);
    
    /* 等待运行 */
    int timeout = 100;
    while (timeout-- > 0) {
        uint32_t sts = ehci_read32(op_base + EHCI_USBSTS);
        if (!(sts & EHCI_STS_HCH)) break;
        delay_ms(1);
    }
    
    if (timeout <= 0) {
        println("[EHCI] Failed to start controller");
        /* periodic_list通过pmm分配，无需kfree */
        kfree(priv->async_qh);
        kfree(priv);
        return -1;
    }

    print("[EHCI] Controller initialized successfully, ");
    print_dec(priv->num_ports);
    println(" ports");
    
    /* 设置控制器信息 */
    ctrl->ops = &ehci_ops;
    ctrl->num_ports = priv->num_ports;
    ctrl->num_devices = 0;
    ctrl->initialized = 1;
    
    return 0;
}
