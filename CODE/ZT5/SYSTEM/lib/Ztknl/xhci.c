/*
 * Zratro OS - XHCI (USB 3.0) 控制器驱动
 *
 * eXtensible Host Controller Interface - USB 3.0 规范
 */

#include "usb.h"
#include "console.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "cpu.h"

/* ========== XHCI 寄存器定义 ========== */

/* 能力寄存器 */
#define XHCI_CAPLENGTH      0x00    /* 能力寄存器长度 */
#define XHCI_HCIVERSION     0x02    /* 接口版本号 */
#define XHCI_HCSPARAMS1     0x04    /* 结构参数 1 */
#define XHCI_HCSPARAMS2     0x08    /* 结构参数 2 */
#define XHCI_HCSPARAMS3     0x0C    /* 结构参数 3 */
#define XHCI_HCCPARAMS1     0x10    /* 能力参数 1 */
#define XHCI_DBOFF          0x14    /* Doorbell 偏移 */
#define XHCI_RTSOFF         0x18    /* 运行时寄存器偏移 */
#define XHCI_HCCPARAMS2     0x1C    /* 能力参数 2 */

/* HCSPARAMS1 字段 */
#define XHCI_MAX_SLOTS_MASK     0xFF
#define XHCI_MAX_INTRS_SHIFT    8
#define XHCI_MAX_PORTS_SHIFT    24

/* 操作寄存器 */
#define XHCI_USBCMD          0x00   /* USB 命令 */
#define XHCI_USBSTS          0x04   /* USB 状态 */
#define XHCI_PAGESIZE        0x08   /* 页面大小 */
#define XHCI_DNCTRL          0x14   /* 设备通知控制 */
#define XHCI_CRCR            0x18   /* 命令环控制 */
#define XHCI_DCBAAP          0x30   /* 设备上下文基地址 */
#define XHCI_CONFIG          0x38   /* 配置 */

/* USB 命令寄存器位 */
#define XHCI_CMD_RS          (1 << 0)   /* Run/Stop */
#define XHCI_CMD_HCRST       (1 << 1)   /* Host Controller Reset */
#define XHCI_CMD_INTE        (1 << 2)   /* Interrupt Enable */
#define XHCI_CMD_HSEE        (1 << 3)   /* Host System Error Enable */
#define XHCI_CMD_LHCRST      (1 << 7)   /* Light HC Reset */
#define XHCI_CMD_CSS         (1 << 8)   /* Controller Save State */
#define XHCI_CMD_CRS         (1 << 9)   /* Controller Restore State */
#define XHCI_CMD_EWE         (1 << 10)  /* Enable Wrap Event */
#define XHCI_CMD_EU3S        (1 << 11)  /* Enable U3 MFINDEX Stop */

/* USB 状态寄存器位 */
#define XHCI_STS_HCH         (1 << 0)   /* HC Halted */
#define XHCI_STS_HSE         (1 << 2)   /* Host System Error */
#define XHCI_STS_EINT        (1 << 3)   /* Event Interrupt */
#define XHCI_STS_PCD         (1 << 4)   /* Port Change Detect */
#define XHCI_STS_SSS         (1 << 8)   /* Save State Status */
#define XHCI_STS_RSS         (1 << 9)   /* Restore State Status */
#define XHCI_STS_SRE         (1 << 10)  /* Save/Restore Error */
#define XHCI_STS_CNR         (1 << 11)  /* Controller Not Ready */
#define XHCI_STS_HCE         (1 << 12)  /* HC Error */

/* 端口寄存器集 (每个端口 0x10 字节) */
#define XHCI_PORTSC          0x00      /* 端口状态和控制 */
#define XHCI_PORTPMSC        0x04      /* 端口电源管理 */
#define XHCI_PORTLI          0x08      /* 端口链路信息 */
#define XHCI_PORTRTS         0x0C      /* 端口运行时状态 */

/* 端口状态位 */
#define XHCI_PORT_CCS        (1 << 0)   /* Current Connect Status */
#define XHCI_PORT_PED        (1 << 1)   /* Port Enabled/Disabled */
#define XHCI_PORT_OCA        (1 << 3)   /* Over-current Active */
#define XHCI_PORT_PR         (1 << 4)   /* Port Reset */
#define XHCI_PORT_PLS_SHIFT  5          /* Port Link State */
#define XHCI_PORT_PLS_MASK   0xF
#define XHCI_PORT_PP         (1 << 9)   /* Port Power */
#define XHCI_PORT_SPEED_SHIFT 10        /* Port Speed */
#define XHCI_PORT_SPEED_MASK 0xF
#define XHCI_PORT_PIC_SHIFT  14         /* Port Indicator Control */
#define XHCI_PORT_LWS        (1 << 16)  /* Port Link State Write Strobe */
#define XHCI_PORT_CSC        (1 << 17)  /* Connect Status Change */
#define XHCI_PORT_PEC        (1 << 18)  /* Port Enable/Disable Change */
#define XHCI_PORT_WRC        (1 << 19)  /* Warm Port Reset Change */
#define XHCI_PORT_OCC        (1 << 20)  /* Over-current Change */
#define XHCI_PORT_PRC        (1 << 21)  /* Port Reset Change */
#define XHCI_PORT_PLC        (1 << 22)  /* Port Link State Change */
#define XHCI_PORT_CEC        (1 << 23)  /* Port Config Error Change */
#define XHCI_PORT_WCE        (1 << 25)  /* Wake on Connect Enable */
#define XHCI_PORT_WDE        (1 << 26)  /* Wake on Disconnect Enable */
#define XHCI_PORT_WOE        (1 << 27)  /* Wake on Over-current Enable */
#define XHCI_PORT_DR         (1 << 30)  /* Device Removable */
#define XHCI_PORT_WPR        (1 << 31)  /* Warm Port Reset */

/* 端口链路状态 */
#define XHCI_PLS_U0          0          /* U0 - On */
#define XHCI_PLS_U1          1          /* U1 */
#define XHCI_PLS_U2          2          /* U2 */
#define XHCI_PLS_U3          3          /* U3 - Suspend */
#define XHCI_PLS_DISABLED    4          /* Disabled */
#define XHCI_PLS_RX_DETECT   5          /* RxDetect */
#define XHCI_PLS_INACTIVE    6          /* Inactive */
#define XHCI_PLS_POLLING     7          /* Polling */
#define XHCI_PLS_RECOVERY    8          /* Recovery */
#define XHCI_PLS_HOT_RESET   9          /* Hot Reset */
#define XHCI_PLS_COMP_MODE   10         /* Compliance Mode */
#define XHCI_PLS_TEST_MODE   11         /* Test Mode */
#define XHCI_PLS_RESUME      15         /* Resume */

/* 运行时寄存器 */
#define XHCI_MFINDEX         0x00       /* Microframe Index */
#define XHCI_IR0             0x20       /* 中断寄存器集 0 */

/* 中断寄存器 (每集 0x20 字节) */
#define XHCI_IMAN            0x00       /* Interrupt Management */
#define XHCI_IMOD            0x04       /* Interrupt Moderation */
#define XHCI_ERSTSZ          0x08       /* Event Ring Segment Table Size */
#define XHCI_ERSTBA          0x10       /* Event Ring Segment Table Base */
#define XHCI_ERDP            0x18       /* Event Ring Dequeue Pointer */

/* Doorbell 寄存器 */
#define XHCI_DB_TARGET_SHIFT 0
#define XHCI_DB_SID_SHIFT    16

/* ========== TRB (Transfer Request Block) 定义 ========== */

/* TRB 类型 */
#define TRB_TYPE_NORMAL           1
#define TRB_TYPE_SETUP_STAGE      2
#define TRB_TYPE_DATA_STAGE       3
#define TRB_TYPE_STATUS_STAGE     4
#define TRB_TYPE_ISOCH            5
#define TRB_TYPE_LINK             6
#define TRB_TYPE_EVENT_DATA       7
#define TRB_TYPE_NOOP             8
#define TRB_TYPE_ENABLE_SLOT      9
#define TRB_TYPE_DISABLE_SLOT    10
#define TRB_TYPE_ADDRESS_DEVICE  11
#define TRB_TYPE_CONFIG_EP       12
#define TRB_TYPE_EVALUATE_CTX    13
#define TRB_TYPE_RESET_EP        14
#define TRB_TYPE_STOP_EP         15
#define TRB_TYPE_SET_TR_DEQ      16
#define TRB_TYPE_RESET_DEVICE    17
#define TRB_TYPE_FORCE_EVENT     18
#define TRB_TYPE_NEGOTIATE_BW    19
#define TRB_TYPE_SET_LATENCY_TOL 20
#define TRB_TYPE_GET_PORT_BW     21
#define TRB_TYPE_FORCE_HEADER    22
#define TRB_TYPE_NOOP_CMD        23

/* 事件 TRB 类型 */
#define TRB_TYPE_TRANSFER_EVENT   32
#define TRB_TYPE_CMD_COMPLETION   33
#define TRB_TYPE_PORT_STATUS      34
#define TRB_TYPE_BANDWIDTH_EVENT  35
#define TRB_TYPE_DOORBELL_EVENT   36
#define TRB_TYPE_HC_EVENT         37
#define TRB_TYPE_DEVICE_NOTIFY    38
#define TRB_TYPE_MFINDEX_WRAP     39

/* TRB 标志 */
#define TRB_CYCLE       (1 << 0)
#define TRB_TOGGLE      (1 << 1)
#define TRB_ISP         (1 << 2)
#define TRB_IOC         (1 << 5)
#define TRB_IDT         (1 << 6)
#define TRB_BEI         (1 << 9)
#define TRB_CHAIN       (1 << 4)

/* TRB 结构 (16 bytes) */
struct xhci_trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed, aligned(16)));

/* TRB 控制字段 */
#define TRB_TYPE(trb)       (((trb)->control >> 10) & 0x3F)
#define TRB_TYPE_SET(type)  ((type) << 10)

/* ========== XHCI 环结构 ========== */

/* 事件环段表项 */
struct xhci_erst_entry {
    uint64_t segment_addr;
    uint32_t segment_size;
    uint32_t reserved;
} __attribute__((packed, aligned(16)));

/* 传输环 */
struct xhci_ring {
    struct xhci_trb *trbs;
    uint32_t size;
    uint32_t enqueue;
    uint32_t dequeue;
    uint32_t cycle;
    uint64_t dma_addr;
};

/* ========== XHCI 设备上下文 ========== */

/* 输入上下文 */
struct xhci_input_ctx {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t reserved[6];
} __attribute__((packed));

/* 插槽上下文 */
struct xhci_slot_ctx {
    uint32_t dev_info;      /* Route String, Speed, MTT, etc. */
    uint32_t dev_info2;     /* Max Exit Latency, Root Hub Port */
    uint32_t tt_info;       /* TT Hub Slot ID, TT Port Number */
    uint32_t dev_state;     /* Device Address, Slot State */
    uint32_t reserved[4];
} __attribute__((packed));

/* 端点上下文 */
struct xhci_ep_ctx {
    uint32_t ep_info;       /* EP State, Mult, Max PStreams */
    uint32_t ep_info2;      /* Transfer Type, Max Packet, etc. */
    uint64_t deq_ptr;       /* Dequeue Pointer */
    uint32_t tx_info;       /* Average TRB Length, Max ESIT */
    uint32_t reserved[3];
} __attribute__((packed));

/* 设备上下文 (32 endpoints) */
struct xhci_device_ctx {
    struct xhci_slot_ctx slot;
    struct xhci_ep_ctx eps[31];
} __attribute__((packed, aligned(64)));

/* ========== XHCI 控制器私有数据 ========== */

struct xhci_priv {
    uint64_t mmio_base;
    uint64_t db_offset;
    uint64_t rt_offset;
    
    uint32_t page_size;
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t max_intrs;
    
    /* 命令环 */
    struct xhci_ring cmd_ring;
    
    /* 事件环 */
    struct xhci_ring event_ring;
    struct xhci_erst_entry *erst;
    uint64_t erst_dma;
    
    /* 设备上下文数组 */
    struct xhci_device_ctx **dev_ctxs;
    uint64_t dcbaa_dma;
    
    /* 已分配的地址 */
    uint32_t slot_bitmap;
    uint8_t address_map[128];
};

/* ========== 内联函数 ========== */

static void delay_ms(int ms) {
    uint64_t tsc_start, tsc_end;
    uint64_t tsc_freq = cpu_get_tsc_frequency();
    if (tsc_freq == 0) tsc_freq = 1000000000ULL;
    
    asm volatile("rdtsc" : "=A"(tsc_start));
    uint64_t wait = (tsc_freq * ms) / 1000;
    do {
        asm volatile("rdtsc" : "=A"(tsc_end));
    } while ((tsc_end - tsc_start) < wait);
}

/* 查找最后设置的位 */
static inline int ffs(uint32_t x) {
    if (x == 0) return 0;
    int r = 1;
    while (!(x & 1)) {
        x >>= 1;
        r++;
    }
    return r - 1;
}

static inline uint8_t xhci_read8(uint64_t addr) {
    return *(volatile uint8_t *)addr;
}

static inline uint32_t xhci_read32(uint64_t addr) {
    return *(volatile uint32_t *)addr;
}

static inline void xhci_write32(uint64_t addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}

static inline uint64_t xhci_read64(uint64_t addr) {
    return *(volatile uint64_t *)addr;
}

static inline void xhci_write64(uint64_t addr, uint64_t val) {
    *(volatile uint64_t *)addr = val;
}

/* ========== 环操作 ========== */

/* 初始化环 */
static int xhci_ring_init(struct xhci_ring *ring, int size) {
    ring->trbs = kmalloc(sizeof(struct xhci_trb) * size);
    if (!ring->trbs) return -1;

    memset(ring->trbs, 0, sizeof(struct xhci_trb) * size);
    ring->size = size;
    ring->enqueue = 0;
    ring->dequeue = 0;
    ring->cycle = 1;

    /* 获取物理地址供 DMA 使用 */
    ring->dma_addr = virt_to_phys(ring->trbs);

    return 0;
}

/* 获取下一个 TRB 入队位置 */
static struct xhci_trb *xhci_ring_next(struct xhci_ring *ring) {
    struct xhci_trb *trb = &ring->trbs[ring->enqueue];

    ring->enqueue = (ring->enqueue + 1) % ring->size;

    /* Link TRB - 环回时翻转 Cycle 位 */
    if (ring->enqueue == 0) {
        ring->cycle ^= 1;
    }

    return trb;
}

/* 获取当前入队 TRB 的物理地址 (用于设置数据指针) */
static uint64_t xhci_ring_enqueue_phys(struct xhci_ring *ring) {
    return ring->dma_addr + ring->enqueue * sizeof(struct xhci_trb);
}

/* 等待事件环中的命令完成事件 */
static int xhci_wait_for_event(struct xhci_priv *priv, uint32_t expected_type, int timeout_ms) {
    uint64_t rt_base = priv->mmio_base + priv->rt_offset;
    uint64_t erdp = xhci_read64(rt_base + XHCI_IR0 + XHCI_ERDP);

    while (timeout_ms > 0) {
        /* 检查事件环中是否有新事件 */
        struct xhci_trb *event = &priv->event_ring.trbs[priv->event_ring.dequeue];

        /* 检查 Cycle 位是否匹配 */
        if ((event->control & TRB_CYCLE) == priv->event_ring.cycle) {
            uint32_t evt_type = TRB_TYPE(event);

            if (evt_type == expected_type) {
                /* 处理完成事件 */
                uint8_t completion_code = (event->status >> 24) & 0xFF;

                /* 推进事件环 dequeue 指针 */
                priv->event_ring.dequeue = (priv->event_ring.dequeue + 1) % priv->event_ring.size;
                if (priv->event_ring.dequeue == 0) {
                    priv->event_ring.cycle ^= 1;
                }

                /* 更新 ERDP 寄存器 */
                uint64_t new_erdp = priv->event_ring.dma_addr +
                                    priv->event_ring.dequeue * sizeof(struct xhci_trb);
                xhci_write64(rt_base + XHCI_IR0 + XHCI_ERDP, new_erdp | 1);  /* EHB=1 清除中断 */

                if (completion_code == 1) {
                    /* Success */
                    return 0;
                } else {
                    /* 完成码非成功 */
                    return -1;
                }
            }

            /* 其他事件类型 - 跳过并继续 */
            if (evt_type == TRB_TYPE_PORT_STATUS) {
                /* 端口状态变化事件 - 记录但不处理 */
            }

            priv->event_ring.dequeue = (priv->event_ring.dequeue + 1) % priv->event_ring.size;
            if (priv->event_ring.dequeue == 0) {
                priv->event_ring.cycle ^= 1;
            }

            /* 更新 ERDP */
            uint64_t new_erdp = priv->event_ring.dma_addr +
                                priv->event_ring.dequeue * sizeof(struct xhci_trb);
            xhci_write64(rt_base + XHCI_IR0 + XHCI_ERDP, new_erdp | 1);
        }

        delay_ms(1);
        timeout_ms--;
    }

    return -1;  /* 超时 */
}

/* 通用 TRB 入队函数 - 处理环回和 Link TRB */
static int xhci_ring_enqueue_trb(struct xhci_ring *ring, uint64_t parameter,
                                   uint32_t status, uint32_t control) {
    /* 检查环是否已满 */
    uint32_t next_enq = (ring->enqueue + 1) % ring->size;
    if (next_enq == ring->dequeue) {
        return -1;  /* 环满 */
    }

    struct xhci_trb *trb = &ring->trbs[ring->enqueue];
    trb->parameter = parameter;
    trb->status = status;
    trb->control = control | (ring->cycle ? TRB_CYCLE : 0);

    ring->enqueue = next_enq;

    /* 如果到达环末尾，添加 Link TRB 并翻转 Cycle */
    if (ring->enqueue == 0) {
        struct xhci_trb *link = &ring->trbs[ring->size - 1];
        link->parameter = ring->dma_addr;
        link->status = 0;
        link->control = TRB_TYPE_SET(TRB_TYPE_LINK) | (ring->cycle ? TRB_CYCLE : 0) | TRB_TOGGLE;
        ring->cycle ^= 1;
    }

    return 0;
}

/* ========== 端口操作 ========== */

static int xhci_port_reset(struct usb_controller *ctrl, int port) {
    struct xhci_priv *priv = ctrl->private_data;
    uint64_t portsc = priv->mmio_base + 0x400 + (port * 0x10) + XHCI_PORTSC;
    
    uint32_t val = xhci_read32(portsc);
    
    /* 检查是否已连接 */
    if (!(val & XHCI_PORT_CCS)) {
        return -1;
    }
    
    /* 检查端口电源 */
    if (!(val & XHCI_PORT_PP)) {
        /* 开启端口电源 */
        val |= XHCI_PORT_PP;
        xhci_write32(portsc, val);
        delay_ms(20);
    }
    
    /* 发起端口复位 */
    val = xhci_read32(portsc);
    val |= XHCI_PORT_PR | XHCI_PORT_CSC | XHCI_PORT_PEC | XHCI_PORT_PRC;
    xhci_write32(portsc, val);
    
    /* 等待复位完成 */
    int timeout = 500;
    while (timeout-- > 0) {
        val = xhci_read32(portsc);
        if (!(val & XHCI_PORT_PR) && (val & XHCI_PORT_PED)) {
            /* 复位完成，端口已使能 */
            return 0;
        }
        delay_ms(1);
    }
    
    return -1;
}

static int xhci_port_enable(struct usb_controller *ctrl, int port) {
    /* XHCI 端口在复位后自动使能 */
    struct xhci_priv *priv = ctrl->private_data;
    uint64_t portsc = priv->mmio_base + 0x400 + (port * 0x10) + XHCI_PORTSC;
    
    uint32_t val = xhci_read32(portsc);
    return (val & XHCI_PORT_PED) ? 0 : -1;
}

static usb_speed_t xhci_port_get_speed(struct usb_controller *ctrl, int port) {
    struct xhci_priv *priv = ctrl->private_data;
    uint64_t portsc = priv->mmio_base + 0x400 + (port * 0x10) + XHCI_PORTSC;
    
    uint32_t val = xhci_read32(portsc);
    uint32_t speed = (val >> XHCI_PORT_SPEED_SHIFT) & XHCI_PORT_SPEED_MASK;
    
    switch (speed) {
        case 1: return USB_SPEED_FULL;
        case 2: return USB_SPEED_LOW;
        case 3: return USB_SPEED_HIGH;
        case 4: return USB_SPEED_SUPER;
        default: return USB_SPEED_FULL;
    }
}

static int xhci_port_status(struct usb_controller *ctrl, int port) {
    struct xhci_priv *priv = ctrl->private_data;
    uint64_t portsc = priv->mmio_base + 0x400 + (port * 0x10) + XHCI_PORTSC;
    
    uint32_t val = xhci_read32(portsc);
    
    /* 返回连接状态 */
    return (val & XHCI_PORT_CCS) ? 1 : 0;
}

/* ========== 地址分配 ========== */

static int xhci_alloc_address(struct usb_controller *ctrl) {
    struct xhci_priv *priv = ctrl->private_data;
    
    /* 查找空闲插槽 */
    for (int i = 1; i <= priv->max_slots && i < 128; i++) {
        if (!(priv->slot_bitmap & (1 << i))) {
            priv->slot_bitmap |= (1 << i);
            return i;
        }
    }
    
    return -1;
}

static void xhci_free_address(struct usb_controller *ctrl, int addr) {
    struct xhci_priv *priv = ctrl->private_data;
    
    if (addr > 0 && addr < 128) {
        priv->slot_bitmap &= ~(1 << addr);
    }
}

/* ========== 控制传输 ========== */

static int xhci_submit_control(struct usb_device *dev,
                               uint8_t request_type, uint8_t request,
                               uint16_t value, uint16_t index,
                               void *data, uint16_t length) {
    struct xhci_priv *priv = dev->controller->private_data;

    /* 分配 DMA 缓冲区用于数据阶段 (确保物理地址可用) */
    uint64_t data_phys = 0;
    void *data_virt = NULL;
    if (length > 0 && data) {
        data_phys = pmm_alloc_page();
        if (!data_phys) return -1;
        data_virt = phys_to_virt(data_phys);
        memset(data_virt, 0, 4096);

        /* 如果是输出方向，先复制数据到 DMA 缓冲区 */
        if (!(request_type & USB_DIR_IN)) {
            memcpy(data_virt, data, length);
        }
    }

    /* 构造 Setup 包数据 (8字节，直接嵌入 TRB) */
    uint64_t setup_data = (uint64_t)request_type |
                          ((uint64_t)request << 8) |
                          ((uint64_t)value << 16) |
                          ((uint64_t)index << 32) |
                          ((uint64_t)length << 48);

    /* Setup Stage TRB */
    uint32_t setup_control = TRB_TYPE_SET(TRB_TYPE_SETUP_STAGE) | TRB_IDT;
    /* 设置 Transfer Size = 8, Direction = IN (bit 16) 由数据方向决定 */
    setup_control |= (3 << 16);  /* DIR=IN (bit 16=1) 表示数据阶段可能为IN */
    xhci_ring_enqueue_trb(&priv->cmd_ring, setup_data, 8, setup_control);

    /* Data Stage TRB (如果有数据) */
    if (length > 0 && data_phys) {
        uint32_t data_control = TRB_TYPE_SET(TRB_TYPE_DATA_STAGE);
        if (request_type & USB_DIR_IN) {
            data_control |= (1 << 16);  /* DIR=IN */
        }
        data_control |= TRB_CHAIN;  /* 链接到 Status Stage */
        xhci_ring_enqueue_trb(&priv->cmd_ring, data_phys, length, data_control);
    }

    /* Status Stage TRB */
    uint32_t status_control = TRB_TYPE_SET(TRB_TYPE_STATUS_STAGE) | TRB_IOC;
    if (length > 0 && !(request_type & USB_DIR_IN)) {
        status_control |= (1 << 16);  /* DIR=IN for status after OUT data */
    }
    xhci_ring_enqueue_trb(&priv->cmd_ring, 0, 0, status_control);

    /* 内存屏障 - 确保所有 TRB 写入对控制器可见 */
    asm volatile("mfence" ::: "memory");

    /* Ring doorbell: Slot ID + Endpoint 0 */
    uint64_t db_addr = priv->mmio_base + priv->db_offset;
    xhci_write32(db_addr, dev->address & 0xFF);

    /* 等待传输完成事件 */
    int result = xhci_wait_for_event(priv, TRB_TYPE_TRANSFER_EVENT, 5000);

    /* 如果是输入方向，从 DMA 缓冲区复制数据到调用者 */
    if (result == 0 && length > 0 && data_phys && data && (request_type & USB_DIR_IN)) {
        memcpy(data, data_virt, length);
    }

    /* 释放 DMA 缓冲区 */
    if (data_phys) {
        pmm_free_page(data_phys);
    }

    return result;
}

/* ========== 批量传输 ========== */

static int xhci_submit_bulk(struct usb_device *dev, int endpoint,
                            void *data, int length, int direction) {
    struct xhci_priv *priv = dev->controller->private_data;

    /* 分配 DMA 缓冲区 */
    uint64_t data_phys = pmm_alloc_page();
    if (!data_phys) return -1;
    void *data_virt = phys_to_virt(data_phys);
    memset(data_virt, 0, 4096);

    /* 如果是输出方向，复制数据到 DMA 缓冲区 */
    if (direction == USB_DIR_OUT && data) {
        memcpy(data_virt, data, length);
    }

    /* Normal TRB */
    uint32_t trb_control = TRB_TYPE_SET(TRB_TYPE_NORMAL) | TRB_IOC;
    xhci_ring_enqueue_trb(&priv->cmd_ring, data_phys, length, trb_control);

    /* 内存屏障 */
    asm volatile("mfence" ::: "memory");

    /* Ring doorbell: Slot ID | Endpoint ID */
    uint64_t db_addr = priv->mmio_base + priv->db_offset;
    uint32_t db_val = (dev->address & 0xFF) | (((endpoint + 1) & 0xFF) << 8);
    xhci_write32(db_addr, db_val);

    /* 等待传输完成 */
    int result = xhci_wait_for_event(priv, TRB_TYPE_TRANSFER_EVENT, 5000);

    /* 如果是输入方向，从 DMA 缓冲区复制数据 */
    if (result == 0 && direction == USB_DIR_IN && data) {
        memcpy(data, data_virt, length);
    }

    pmm_free_page(data_phys);

    return (result == 0) ? length : -1;
}

/* ========== 中断传输 ========== */

static int xhci_submit_interrupt(struct usb_device *dev, int endpoint,
                                 void *data, int length) {
    struct xhci_priv *priv = dev->controller->private_data;

    /* 分配 DMA 缓冲区 */
    uint64_t data_phys = pmm_alloc_page();
    if (!data_phys) return -1;
    void *data_virt = phys_to_virt(data_phys);
    memset(data_virt, 0, 4096);

    /* 中断传输使用 Normal TRB (XHCI 规范中中断端点与批量端点使用相同的 TRB 类型) */
    uint32_t trb_control = TRB_TYPE_SET(TRB_TYPE_NORMAL) | TRB_IOC | TRB_ISP;
    xhci_ring_enqueue_trb(&priv->cmd_ring, data_phys, length, trb_control);

    /* 内存屏障 */
    asm volatile("mfence" ::: "memory");

    /* Ring doorbell */
    uint64_t db_addr = priv->mmio_base + priv->db_offset;
    uint32_t db_val = (dev->address & 0xFF) | (((endpoint + 1) & 0xFF) << 8);
    xhci_write32(db_addr, db_val);

    /* 等待传输完成 */
    int result = xhci_wait_for_event(priv, TRB_TYPE_TRANSFER_EVENT, 5000);

    /* 复制数据 */
    if (result == 0 && data) {
        memcpy(data, data_virt, length);
    }

    pmm_free_page(data_phys);

    return (result == 0) ? length : -1;
}

/* ========== XHCI 初始化 ========== */

static int xhci_reset(struct xhci_priv *priv) {
    uint64_t op_base = priv->mmio_base + xhci_read8(priv->mmio_base);
    
    /* 发起复位 */
    uint32_t cmd = xhci_read32(op_base + XHCI_USBCMD);
    cmd |= XHCI_CMD_HCRST;
    xhci_write32(op_base + XHCI_USBCMD, cmd);
    
    /* 等待复位完成 */
    int timeout = 1000;
    while (timeout-- > 0) {
        cmd = xhci_read32(op_base + XHCI_USBCMD);
        if (!(cmd & XHCI_CMD_HCRST)) break;
        delay_ms(1);
    }
    
    /* 等待 Controller Not Ready 清除 */
    timeout = 1000;
    while (timeout-- > 0) {
        uint32_t sts = xhci_read32(op_base + XHCI_USBSTS);
        if (!(sts & XHCI_STS_CNR)) break;
        delay_ms(1);
    }
    
    return (timeout > 0) ? 0 : -1;
}

/* 控制器操作 */
static const struct usb_controller_ops xhci_ops = {
    .init = NULL,  /* 在 xhci_init 中完成 */
    .shutdown = NULL,
    .reset = NULL,
    .port_reset = xhci_port_reset,
    .port_enable = xhci_port_enable,
    .port_get_speed = xhci_port_get_speed,
    .port_status = xhci_port_status,
    .submit_control = xhci_submit_control,
    .submit_bulk = xhci_submit_bulk,
    .submit_interrupt = xhci_submit_interrupt,
    .alloc_address = xhci_alloc_address,
    .free_address = xhci_free_address,
};

/* XHCI 控制器初始化 */
int xhci_init(struct usb_controller *ctrl) {
    struct xhci_priv *priv;
    uint64_t op_base;
    uint32_t hcs_params1;
    uint64_t mmio_phys;
    
    /* 分配私有数据 */
    priv = kmalloc(sizeof(struct xhci_priv));
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
    
    /* XHCI MMIO 区域通常较大，映射 16 页 (64KB) */
    for (int i = 0; i < 16; i++) {
        vmm_map_page(vmm_get_kernel_space(), 
                     priv->mmio_base + i * 4096, 
                     mmio_phys + i * 4096, 
                     mmio_flags);
    }
    
    ctrl->private_data = priv;
    
    /* 读取能力参数 */
    hcs_params1 = xhci_read32(priv->mmio_base + XHCI_HCSPARAMS1);
    priv->max_slots = hcs_params1 & XHCI_MAX_SLOTS_MASK;
    priv->max_intrs = (hcs_params1 >> XHCI_MAX_INTRS_SHIFT) & 0x3FF;
    priv->max_ports = (hcs_params1 >> XHCI_MAX_PORTS_SHIFT) & 0xFF;
    
    /* 读取寄存器偏移 */
    priv->db_offset = xhci_read32(priv->mmio_base + XHCI_DBOFF);
    priv->rt_offset = xhci_read32(priv->mmio_base + XHCI_RTSOFF);
    
    /* 复位控制器 */
    if (xhci_reset(priv) < 0) {
        kfree(priv);
        return -1;
    }
    
    /* 获取页面大小 */
    op_base = priv->mmio_base + xhci_read8(priv->mmio_base);
    uint32_t page_size = xhci_read32(op_base + XHCI_PAGESIZE);
    priv->page_size = 1 << (ffs(page_size) + 11);
    
    /* 初始化命令环 */
    if (xhci_ring_init(&priv->cmd_ring, 256) < 0) {
        kfree(priv);
        return -1;
    }
    
    /* 初始化事件环 */
    if (xhci_ring_init(&priv->event_ring, 256) < 0) {
        kfree(priv->cmd_ring.trbs);
        kfree(priv);
        return -1;
    }
    
    /* 分配 ERST */
    priv->erst = kmalloc(sizeof(struct xhci_erst_entry));
    if (!priv->erst) {
        kfree(priv->event_ring.trbs);
        kfree(priv->cmd_ring.trbs);
        kfree(priv);
        return -1;
    }
    
    priv->erst->segment_addr = priv->event_ring.dma_addr;
    priv->erst->segment_size = 256;
    priv->erst->reserved = 0;
    
    /* 分配设备上下文数组 */
    priv->dev_ctxs = kmalloc(sizeof(void *) * (priv->max_slots + 1));
    if (!priv->dev_ctxs) {
        kfree(priv->erst);
        kfree(priv->event_ring.trbs);
        kfree(priv->cmd_ring.trbs);
        kfree(priv);
        return -1;
    }
    
    memset(priv->dev_ctxs, 0, sizeof(void *) * (priv->max_slots + 1));
    
    /* 设置 CRCR (Command Ring Control) */
    uint64_t crcr = priv->cmd_ring.dma_addr | priv->cmd_ring.cycle;
    xhci_write64(op_base + XHCI_CRCR, crcr);
    
    /* 设置 DCBAAP - 使用物理地址 */
    xhci_write64(op_base + XHCI_DCBAAP, virt_to_phys(priv->dev_ctxs));
    
    /* 设置 Config (Max Slots En) */
    xhci_write32(op_base + XHCI_CONFIG, priv->max_slots);
    
    /* 设置 ERSTSZ */
    uint64_t rt_base = priv->mmio_base + priv->rt_offset;
    xhci_write32(rt_base + XHCI_IR0 + XHCI_ERSTSZ, 1);
    
    /* 设置 ERSTBA - 使用物理地址 */
    xhci_write64(rt_base + XHCI_IR0 + XHCI_ERSTBA, virt_to_phys(priv->erst));
    
    /* 设置 ERDP */
    xhci_write64(rt_base + XHCI_IR0 + XHCI_ERDP, 
                 priv->event_ring.dma_addr | priv->event_ring.cycle);
    
    /* 使能中断 */
    uint32_t iman = xhci_read32(rt_base + XHCI_IR0 + XHCI_IMAN);
    iman |= 1;  /* Enable Interrupt */
    xhci_write32(rt_base + XHCI_IR0 + XHCI_IMAN, iman);
    
    /* 启动控制器 */
    uint32_t cmd = xhci_read32(op_base + XHCI_USBCMD);
    cmd |= XHCI_CMD_RS | XHCI_CMD_INTE;
    xhci_write32(op_base + XHCI_USBCMD, cmd);
    
    /* 等待运行 */
    int timeout = 100;
    while (timeout-- > 0) {
        uint32_t sts = xhci_read32(op_base + XHCI_USBSTS);
        if (!(sts & XHCI_STS_HCH)) break;
        delay_ms(1);
    }
    
    if (timeout <= 0) {
        kfree(priv->dev_ctxs);
        kfree(priv->erst);
        kfree(priv->event_ring.trbs);
        kfree(priv->cmd_ring.trbs);
        kfree(priv);
        return -1;
    }
    
    /* 设置控制器信息 */
    ctrl->ops = &xhci_ops;
    ctrl->num_ports = priv->max_ports;
    ctrl->num_devices = 0;
    ctrl->initialized = 1;
    
    return 0;
}
