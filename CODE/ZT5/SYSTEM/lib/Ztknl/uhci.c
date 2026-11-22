/*
 * Zratro OS - UHCI (USB 1.1) 控制器驱动
 *
 * Universal Host Controller Interface - USB 1.1 规范
 */

#include "usb.h"
#include "console.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "cpu.h"

/* 辅助函数：获取物理地址（支持HHDM和堆地址） */
static uint64_t uhci_virt_to_phys(void *virt) {
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

/* ========== UHCI 寄存器定义 ========== */

/* I/O 寄存器偏移 */
#define UHCI_USBCMD         0x00    /* USB 命令 */
#define UHCI_USBSTS         0x02    /* USB 状态 */
#define UHCI_USBINTR        0x04    /* USB 中断使能 */
#define UHCI_FRNUM          0x06    /* 帧号 */
#define UHCI_FLBASEADD      0x08    /* 帧列表基地址 */
#define UHCI_SOFMOD         0x0C    /* SOF 修改值 */
#define UHCI_PORTSC1        0x10    /* 端口 1 状态/控制 */
#define UHCI_PORTSC2        0x12    /* 端口 2 状态/控制 */

/* USB 命令寄存器位 */
#define UHCI_CMD_RS         (1 << 0)    /* Run/Stop */
#define UHCI_CMD_HCRESET    (1 << 1)    /* Host Controller Reset */
#define UHCI_CMD_GRESET     (1 << 2)    /* Global Reset */
#define UHCI_CMD_EGSM       (1 << 3)    /* Enter Global Suspend Mode */
#define UHCI_CMD_FGR        (1 << 4)    /* Force Global Resume */
#define UHCI_CMD_SWDBG      (1 << 5)    /* Software Debug */
#define UHCI_CMD_CF         (1 << 6)    /* Configure Flag */
#define UHCI_CMD_MAXP       (1 << 7)    /* Max Packet (64 bytes) */

/* USB 状态寄存器位 */
#define UHCI_STS_USBINT     (1 << 0)    /* USB Interrupt */
#define UHCI_STS_USBERR     (1 << 1)    /* USB Error Interrupt */
#define UHCI_STS_RESUME     (1 << 2)    /* Resume Detect */
#define UHCI_STS_HSE        (1 << 3)    /* Host System Error */
#define UHCI_STS_HCPE       (1 << 4)    /* Host Controller Process Error */
#define UHCI_STS_HCH        (1 << 5)    /* HC Halted */

/* USB 中断使能寄存器位 */
#define UHCI_INTR_TIMEOUT   (1 << 0)    /* Timeout/CRC Interrupt */
#define UHCI_INTR_RESUME    (1 << 1)    /* Resume Interrupt */
#define UHCI_INTR_IOC       (1 << 2)    /* Interrupt on Complete */
#define UHCI_INTR_SP        (1 << 3)    /* Short Packet Interrupt */

/* 端口状态/控制寄存器位 */
#define UHCI_PORT_CCS       (1 << 0)    /* Current Connect Status */
#define UHCI_PORT_CSC       (1 << 1)    /* Connect Status Change */
#define UHCI_PORT_PED       (1 << 2)    /* Port Enable/Disable */
#define UHCI_PORT_PEDC      (1 << 3)    /* Port Enable/Disable Change */
#define UHCI_PORT_LSDA      (1 << 8)    /* Low Speed Device Attached */
#define UHCI_PORT_PR        (1 << 9)    /* Port Reset */
#define UHCI_PORT_SUSP      (1 << 12)   /* Suspend */
#define UHCI_PORT_RD        (1 << 13)   /* Resume Detect */
#define UHCI_PORT_LPBK      (1 << 14)   /* Loopback */
#define UHCI_PORT_RWC       (UHCI_PORT_CSC | UHCI_PORT_PEDC)

/* ========== UHCI 数据结构定义 ========== */

/* Transfer Descriptor (TD) */
struct uhci_td {
    uint32_t link;              /* Link Pointer */
    uint32_t status;            /* Status */
    uint32_t token;             /* Token */
    uint32_t buffer;            /* Buffer Pointer */
} __attribute__((packed, aligned(16)));

/* TD Link 字段 */
#define UHCI_TD_LINK_DEPTH_FIRST  (1 << 2)   /* Depth-first traversal bit */
#define UHCI_TD_LINK_QH           (1 << 1)   /* Points to a QH */
#define UHCI_TD_LINK_T            (1 << 0)   /* Terminate */

/* TD Status 字段 */
#define UHCI_TD_STATUS_ACTUAL_LEN_SHIFT 0
#define UHCI_TD_STATUS_ACTUAL_LEN_MASK   0x7FF
#define UHCI_TD_STATUS_BITSTUFF         (1 << 17)
#define UHCI_TD_STATUS_CRCTO            (1 << 18)
#define UHCI_TD_STATUS_NAK              (1 << 19)
#define UHCI_TD_STATUS_BABBLE           (1 << 20)
#define UHCI_TD_STATUS_DBERR            (1 << 21)
#define UHCI_TD_STATUS_STALLED          (1 << 22)
#define UHCI_TD_STATUS_ACTIVE           (1 << 23)
#define UHCI_TD_STATUS_IOC              (1 << 24)
#define UHCI_TD_STATUS_IOS              (1 << 25)
#define UHCI_TD_STATUS_LS               (1 << 26)
#define UHCI_TD_STATUS_ERR              (1 << 27)
#define UHCI_TD_STATUS_SPD              (1 << 29)

/* TD Token 字段 */
#define UHCI_TD_TOKEN_PID_OUT           0xE1
#define UHCI_TD_TOKEN_PID_IN            0x69
#define UHCI_TD_TOKEN_PID_SETUP         0x2D
#define UHCI_TD_TOKEN_DEVADDR_SHIFT     8
#define UHCI_TD_TOKEN_EP_SHIFT          15
#define UHCI_TD_TOKEN_DATA_TOGGLE       (1 << 19)
#define UHCI_TD_TOKEN_MAXLEN_SHIFT      21

/* Queue Head (QH) */
struct uhci_qh {
    uint32_t link;              /* Link Pointer */
    uint32_t element;           /* Queue Element Link Pointer */
} __attribute__((packed, aligned(16)));

/* ========== UHCI 控制器私有数据 ========== */

struct uhci_priv {
    uint32_t io_base;
    
    /* 帧列表 */
    uint32_t *frame_list;
    uint64_t frame_list_phys;
    
    /* 控制传输队列 - 永久QH */
    struct uhci_qh *ctrl_qh;
    struct uhci_qh *fs_ctrl_qh;  /* Full Speed控制QH */
    struct uhci_qh *ls_ctrl_qh;  /* Low Speed控制QH */
    struct uhci_qh *bulk_qh;     /* 批量传输QH */
    struct uhci_qh *intr_qh;     /* 中断传输QH */
    
    /* TD池 - 预分配的TD */
    struct uhci_td *td_pool[8];
    int td_pool_used;
    
    /* 已分配的地址 */
    uint8_t address_map[128];
    int next_address;
};

/* ========== 内联函数 ========== */

static inline void uhci_outw(uint16_t port, uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t uhci_inw(uint16_t port) {
    uint16_t val;
    asm volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void uhci_outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t uhci_inb(uint16_t port) {
    uint8_t val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void uhci_outl(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t uhci_inl(uint16_t port) {
    uint32_t val;
    asm volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ========== 端口操作 ========== */

static int uhci_port_reset(struct usb_controller *ctrl, int port) {
    struct uhci_priv *priv = ctrl->private_data;
    uint16_t portsc = priv->io_base + UHCI_PORTSC1 + (port * 2);
    
    uint16_t val = uhci_inw(portsc);
    
    print("[UHCI] Port ");
    print_dec(port);
    print(" initial status: 0x");
    print_hex(val);
    println("");
    
    /* Decode status */
    print("[UHCI] Port ");
    print_dec(port);
    print(" status bits: CCS=");
    print_dec((val >> 0) & 1);
    print(" CSC=");
    print_dec((val >> 1) & 1);
    print(" PED=");
    print_dec((val >> 2) & 1);
    print(" LSDA=");
    print_dec((val >> 8) & 1);
    print(" PR=");
    print_dec((val >> 9) & 1);
    println("");
    
    /* 检查是否已连接 */
    if (!(val & UHCI_PORT_CCS)) {
        print("[UHCI] Port ");
        print_dec(port);
        println(" not connected");
        return -1;
    }
    
    /* 清除状态变化 */
    val |= UHCI_PORT_RWC;
    uhci_outw(portsc, val);
    
    /* 发起端口复位 */
    val = uhci_inw(portsc);
    val |= UHCI_PORT_PR;
    uhci_outw(portsc, val);
    
    /* 等待复位完成 (增加等待时间) */
    delay_ms(50);
    
    /* 清除复位位 */
    val = uhci_inw(portsc);
    val &= ~UHCI_PORT_PR;
    uhci_outw(portsc, val);
    
    /* 等待端口使能 (增加等待时间) */
    delay_ms(100);
    
    val = uhci_inw(portsc);
    
    print("[UHCI] Port ");
    print_dec(port);
    print(" after reset status: 0x");
    print_hex(val);
    println("");
    
    /* 清除状态变化 */
    val |= UHCI_PORT_RWC;
    uhci_outw(portsc, val);
    
    /* 如果端口未使能，尝试强制使能 */
    if (!(val & UHCI_PORT_PED)) {
        print("[UHCI] Port ");
        print_dec(port);
        println(" manually enabling...");
        /* 尝试直接设置PED位（虽然规范不建议，但QEMU可能需要） */
        val = uhci_inw(portsc);
        val |= UHCI_PORT_PED;
        /* 清除变化标志 */
        val |= UHCI_PORT_RWC;
        uhci_outw(portsc, val);
        delay_ms(100);
        val = uhci_inw(portsc);
        print("[UHCI] Port ");
        print_dec(port);
        print(" after manual enable: 0x");
        print_hex(val);
        println("");
    }
    
    if (!(val & UHCI_PORT_PED)) {
        print("[UHCI] Port ");
        print_dec(port);
        println(" not enabled after reset");
    }
    
    return (val & UHCI_PORT_PED) ? 0 : -1;
}

static int uhci_port_enable(struct usb_controller *ctrl, int port) {
    struct uhci_priv *priv = ctrl->private_data;
    uint16_t portsc = priv->io_base + UHCI_PORTSC1 + (port * 2);
    
    uint16_t val = uhci_inw(portsc);
    
    /* 使能端口 */
    if (!(val & UHCI_PORT_PED)) {
        val |= UHCI_PORT_PED;
        uhci_outw(portsc, val);
        delay_ms(5);
        val = uhci_inw(portsc);
    }
    
    return (val & UHCI_PORT_PED) ? 0 : -1;
}

static usb_speed_t uhci_port_get_speed(struct usb_controller *ctrl, int port) {
    struct uhci_priv *priv = ctrl->private_data;
    uint16_t portsc = priv->io_base + UHCI_PORTSC1 + (port * 2);
    
    uint16_t val = uhci_inw(portsc);
    
    if (val & UHCI_PORT_LSDA) {
        return USB_SPEED_LOW;
    }
    return USB_SPEED_FULL;
}

static int uhci_port_status(struct usb_controller *ctrl, int port) {
    struct uhci_priv *priv = ctrl->private_data;
    uint16_t portsc = priv->io_base + UHCI_PORTSC1 + (port * 2);
    
    uint16_t val = uhci_inw(portsc);
    
    return (val & UHCI_PORT_CCS) ? 1 : 0;
}

/* ========== 地址分配 ========== */

static int uhci_alloc_address(struct usb_controller *ctrl) {
    struct uhci_priv *priv = ctrl->private_data;
    
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

static void uhci_free_address(struct usb_controller *ctrl, int addr) {
    struct uhci_priv *priv = ctrl->private_data;
    
    if (addr > 0 && addr < 128) {
        priv->address_map[addr] = 0;
        if (addr < priv->next_address) {
            priv->next_address = addr;
        }
    }
}

/* ========== TD/QH 创建 ========== */

static struct uhci_td *uhci_create_td(void) {
    struct uhci_td *td = kmalloc(sizeof(struct uhci_td));
    if (!td) return NULL;
    
    memset(td, 0, sizeof(*td));
    td->link = UHCI_TD_LINK_T;
    
    print("[UHCI_TD] Created at virt=0x");
    print_hex((uint64_t)td);
    println("");
    
    return td;
}

static struct uhci_qh *uhci_create_qh(void) {
    struct uhci_qh *qh = kmalloc(sizeof(struct uhci_qh));
    if (!qh) return NULL;
    
    memset(qh, 0, sizeof(*qh));
    qh->link = UHCI_TD_LINK_T;
    qh->element = UHCI_TD_LINK_T;
    
    print("[UHCI_QH] Created at virt=0x");
    print_hex((uint64_t)qh);
    println("");
    
    return qh;
}

/* ========== 控制传输 ========== */

static int uhci_submit_control(struct usb_device *dev,
                               uint8_t request_type, uint8_t request,
                               uint16_t value, uint16_t index,
                               void *data, uint16_t length) {
    struct uhci_priv *priv = dev->controller->private_data;
    int is_low_speed = (dev->speed == USB_SPEED_LOW);
    int max_packet = is_low_speed ? 8 : 64;
    
    /* Debug: print device info */
    print("[UHCI_CTRL] submit_control: dev->addr=");
    print_dec(dev->address);
    print(" speed=");
    print_dec(dev->speed);
    print(" is_ls=");
    print_dec(is_low_speed);
    print(" req_type=0x");
    print_hex(request_type);
    print(" req=0x");
    print_hex(request);
    print(" value=0x");
    print_hex(value);
    print(" len=");
    print_dec(length);
    println("");
    
    /* 选择正确的控制QH（根据设备速度） */
    struct uhci_qh *ctrl_qh = is_low_speed ? priv->ls_ctrl_qh : priv->fs_ctrl_qh;
    priv->ctrl_qh = ctrl_qh;
    
    print("[UHCI_CTRL] Using ctrl_qh: ");
    print(is_low_speed ? "ls_ctrl" : "fs_ctrl");
    println("");
    
    /* Setup TD */
    struct uhci_td *setup_td = uhci_create_td();
    if (!setup_td) {
        return -1;
    }
    
    /* Setup packet - 使用堆分配确保DMA可访问 */
    uint8_t *setup_pkt = kmalloc(8);
    if (!setup_pkt) {
        kfree(setup_td);
        return -1;
    }
    
    /* 填充Setup包 (USB小端序) */
    setup_pkt[0] = request_type;      /* bmRequestType */
    setup_pkt[1] = request;           /* bRequest */
    setup_pkt[2] = value & 0xFF;      /* wValue low */
    setup_pkt[3] = (value >> 8) & 0xFF; /* wValue high */
    setup_pkt[4] = index & 0xFF;      /* wIndex low */
    setup_pkt[5] = (index >> 8) & 0xFF; /* wIndex high */
    setup_pkt[6] = length & 0xFF;     /* wLength low */
    setup_pkt[7] = (length >> 8) & 0xFF; /* wLength high */
    
    setup_td->status = UHCI_TD_STATUS_ACTIVE |
                       (is_low_speed ? UHCI_TD_STATUS_LS : 0) |
                       (3 << 27);  /* 3 error retries (bits 27-28) = 0x18000000 */
    
    print("[UHCI_CTRL] setup_td->status set to: 0x");
    print_hex(setup_td->status);
    print(" expected: 0x");
    uint32_t expected = UHCI_TD_STATUS_ACTIVE | (is_low_speed ? UHCI_TD_STATUS_LS : 0) | (3 << 27);
    print_hex(expected);
    println("");
    
    setup_td->token = UHCI_TD_TOKEN_PID_SETUP |
                      (dev->address << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
                      (0 << UHCI_TD_TOKEN_EP_SHIFT) |  /* 端点0 */
                      (((uint32_t)0x7) << UHCI_TD_TOKEN_MAXLEN_SHIFT); /* 8字节 */
    
    setup_td->buffer = (uint32_t)uhci_virt_to_phys(setup_pkt);
    
    print("[UHCI_CTRL] setup_pkt virt=0x");
    print_hex((uint64_t)setup_pkt);
    uint64_t setup_pkt_phys = uhci_virt_to_phys(setup_pkt);
    print(" phys=0x");
    print_hex(setup_pkt_phys);
    println("");
    
    /* Data TD */
    struct uhci_td *data_td = NULL;
    uint8_t *data_buffer = NULL;
    if (length > 0 && data) {
        print("[UHCI_CTRL] Creating Data TD: length=");
        print_dec(length);
        println("");
        data_td = uhci_create_td();
        if (!data_td) {
            kfree(setup_pkt);
            kfree(setup_td);
            return -1;
        }
        
        /* 数据缓冲区 - 使用堆分配 */
        data_buffer = kmalloc(length);
        if (!data_buffer) {
            kfree(data_td);
            kfree(setup_pkt);
            kfree(setup_td);
            return -1;
        }
        
        int pid = (request_type & USB_DIR_IN) ? 
                  UHCI_TD_TOKEN_PID_IN : UHCI_TD_TOKEN_PID_OUT;
        
        uint16_t max_len_raw = ((length - 1) & 0x7FF);
        uint32_t max_len = ((uint32_t)((length - 1) & 0x7FF)) << UHCI_TD_TOKEN_MAXLEN_SHIFT;
        
        print("[UHCI_CTRL] Data TD: length=");
        print_dec(length);
        print(" max_len_raw=");
        print_dec(max_len_raw);
        print(" max_len_shifted=0x");
        print_hex(max_len);
        println("");
        
        data_td->status = UHCI_TD_STATUS_ACTIVE |
                          UHCI_TD_STATUS_SPD |
                          (is_low_speed ? UHCI_TD_STATUS_LS : 0) |
                          (3 << 27);
        
        data_td->token = pid |
                         (dev->address << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
                         (0 << UHCI_TD_TOKEN_EP_SHIFT) |  /* 端点0 */
                         UHCI_TD_TOKEN_DATA_TOGGLE |
                         max_len;
        
        data_td->buffer = (uint32_t)uhci_virt_to_phys(data_buffer);
    }
    
    /* Status TD */
    struct uhci_td *status_td = uhci_create_td();
    if (!status_td) {
        if (data_td) kfree(data_td);
        kfree(setup_pkt);
        kfree(setup_td);
        return -1;
    }
    
    int status_pid = (request_type & USB_DIR_IN) ? 
                     UHCI_TD_TOKEN_PID_OUT : UHCI_TD_TOKEN_PID_IN;
    
    status_td->status = UHCI_TD_STATUS_ACTIVE | UHCI_TD_STATUS_IOC |
                        (is_low_speed ? UHCI_TD_STATUS_LS : 0) |
                        (1 << 27);  /* 1 error retry for status stage */
    
    status_td->token = status_pid |
                       (dev->address << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
                       (0 << UHCI_TD_TOKEN_EP_SHIFT) |  /* 端点0 */
                       UHCI_TD_TOKEN_DATA_TOGGLE |  /* Status阶段使用DATA1 */
                       (((uint32_t)0x7FF) << UHCI_TD_TOKEN_MAXLEN_SHIFT); /* 零长度包 */
    
    status_td->buffer = 0;
    
    /* 预先计算所有物理地址 */
    uint64_t setup_td_phys = uhci_virt_to_phys(setup_td);
    print("[UHCI_CTRL] setup_td virt=0x");
    print_hex((uint64_t)setup_td);
    print(" phys=0x");
    print_hex(setup_td_phys);
    println("");
    
    uint64_t data_td_phys = data_td ? uhci_virt_to_phys(data_td) : 0;
    if (data_td) {
        print("[UHCI_CTRL] data_td virt=0x");
        print_hex((uint64_t)data_td);
        print(" phys=0x");
        print_hex(data_td_phys);
        println("");
    }
    
    uint64_t status_td_phys = uhci_virt_to_phys(status_td);
    
    /* 链接 TD 链
     * 根据UHCI规范和Linux实现：
     * - TD链中间的TD：只设置下一个TD的物理地址（不设置标志位）
     * - 最后一个TD：设置Terminate位
     * - 不使用Depth First位，因为控制传输需要breadth-first遍历
     */
    if (data_td) {
        /* Setup TD -> Data TD -> Status TD */
        setup_td->link = (uint32_t)data_td_phys;  /* 指向Data TD */
        data_td->link = (uint32_t)status_td_phys; /* 指向Status TD */
    } else {
        /* Setup TD -> Status TD (无数据传输) */
        setup_td->link = (uint32_t)status_td_phys;
    }
    
    /* Status TD是最后一个，link保持Terminate (UHCI_TD_LINK_T = 0x0001) */
    
    /* 使用TD链一次性提交整个控制传输（参考OSDev和SerenityOS） */
    /* 关键：TD链必须正确链接，QEMU会自动处理三个阶段 */
    
    /* 链接TD链 - 使用Depth First位（bit 2 = 1） */
    /* Depth First确保UHCI先处理完整个TD链，再回到QH调度 */
    if (data_td) {
        /* Setup TD -> Data TD -> Status TD */
        setup_td->link = (uint32_t)data_td_phys | 0x4;  /* Depth First bit */
        data_td->link = (uint32_t)status_td_phys | 0x4; /* Depth First bit */
    } else {
        /* Setup TD -> Status TD (无数据传输) */
        setup_td->link = (uint32_t)status_td_phys | 0x4;
    }
    
    /* Status TD是最后一个，link保持Terminate (0x0001) */
    
    /* 将TD链添加到永久控制QH */
    uint32_t ctrl_qh_phys = (uint32_t)uhci_virt_to_phys(priv->ctrl_qh);
    
    print("[UHCI_CTRL] Frame[0]=0x");
    print_hex(priv->frame_list[0]);
    print(" ctrl_qh_phys=0x");
    print_hex(ctrl_qh_phys);
    println("");
    
    print("[UHCI_CTRL] TD chain: setup->link=0x");
    print_hex(setup_td->link);
    print(" data->link=0x");
    if (data_td) print_hex(data_td->link);
    else print(0);
    print(" status->link=0x");
    print_hex(status_td->link);
    println("");
    
    /* 一次性设置QH element指向Setup TD */
    priv->ctrl_qh->element = (uint32_t)setup_td_phys;
    
    print("[UHCI_CTRL] Submitting complete TD chain to QH");
    println("");
    
    /* 等待整个控制传输完成 */
    int timeout = 3000;  /* 3秒超时 */
    int iter_count = 0;
    while (timeout-- > 0) {
        if (!(status_td->status & UHCI_TD_STATUS_ACTIVE)) {
            break;
        }
        /* 每500ms打印状态 */
        if (iter_count++ % 500 == 0) {
            print("[UHCI_CTRL] Waiting... Setup=0x");
            print_hex(setup_td->status);
            if (data_td) {
                print(" Data=0x");
                print_hex(data_td->status);
            }
            print(" Status=0x");
            print_hex(status_td->status);
            println("");
        }
        delay_ms(1);
    }
    
    print("[UHCI_CTRL] Transfer complete, timeout=");
    print_dec(timeout);
    println("");
    print("[UHCI_CTRL] Setup TD status=0x");
    print_hex(setup_td->status);
    println("");
    if (data_td) {
        print("[UHCI_CTRL] Data TD status=0x");
        print_hex(data_td->status);
        println("");
        print("[UHCI_CTRL] Data TD actual_len=");
        print_dec(data_td->status & UHCI_TD_STATUS_ACTUAL_LEN_MASK);
        println("");
    }
    print("[UHCI_CTRL] Status TD status=0x");
    print_hex(status_td->status);
    println("");
    print("[UHCI_CTRL] QH element=0x");
    print_hex(priv->ctrl_qh->element);
    println("");
    
    int ret = 0;
    if (status_td->status & UHCI_TD_STATUS_STALLED) {
        print("[UHCI_CTRL] Transfer STALLED");
        println("");
        ret = -1;
    } else if (timeout <= 0) {
        print("[UHCI_CTRL] Transfer TIMEOUT");
        println("");
        ret = -2;
    } else {
        print("[UHCI_CTRL] Transfer OK");
        println("");
        /* 如果是IN传输，复制数据回原缓冲区 */
        if ((request_type & USB_DIR_IN) && data && data_buffer && length > 0) {
            memcpy(data, data_buffer, length);
        }
    }
    
    /* 打印QH element最终值 */
    print("[UHCI_CTRL] After wait - QH element=0x");
    print_hex(priv->ctrl_qh->element);
    println("");
    
    /* 重置控制QH */
    priv->ctrl_qh->element = UHCI_TD_LINK_T;
    
    /* 清理 */
    kfree(setup_pkt);
    if (data_buffer) kfree(data_buffer);
    kfree(setup_td);
    if (data_td) kfree(data_td);
    kfree(status_td);
    
    return ret;
}

/* ========== 批量传输 ========== */

static int uhci_submit_bulk(struct usb_device *dev, int endpoint,
                            void *data, int length, int direction) {
    struct uhci_priv *priv = dev->controller->private_data;
    int is_low_speed = (dev->speed == USB_SPEED_LOW);
    int max_packet = is_low_speed ? 8 : 64;
    
    /* 创建 QH */
    struct uhci_qh *qh = uhci_create_qh();
    if (!qh) return -1;
    
    /* 创建 TD */
    struct uhci_td *td = uhci_create_td();
    if (!td) {
        kfree(qh);
        return -1;
    }
    
    int pid = direction ? UHCI_TD_TOKEN_PID_IN : UHCI_TD_TOKEN_PID_OUT;
    
    td->status = UHCI_TD_STATUS_ACTIVE | UHCI_TD_STATUS_IOC |
                 UHCI_TD_STATUS_ERR |
                 (is_low_speed ? UHCI_TD_STATUS_LS : 0) |
                 (3 << 27);
    
    uint16_t max_len = (((max_packet - 1) & 0x7FF) << UHCI_TD_TOKEN_MAXLEN_SHIFT);
    
    td->token = pid |
                (dev->address << UHCI_TD_TOKEN_DEVADDR_SHIFT) |
                (endpoint << UHCI_TD_TOKEN_EP_SHIFT) |
                max_len;
    
    td->buffer = (uint32_t)uhci_virt_to_phys(data);
    
    /* 使用物理地址链接 QH 和 TD */
    uint64_t td_phys = uhci_virt_to_phys(td);
    uint64_t qh_phys = uhci_virt_to_phys(qh);
    qh->element = (uint32_t)td_phys;
    
    /* 加入帧列表 - 使用物理地址 */
    uint32_t old_entry = priv->frame_list[0];
    priv->frame_list[0] = (uint32_t)qh_phys | UHCI_TD_LINK_QH;
    
    /* 等待完成 */
    int timeout = 1000;
    while (timeout-- > 0) {
        if (!(td->status & UHCI_TD_STATUS_ACTIVE)) {
            break;
        }
        delay_ms(1);
    }
    
    int ret = length;
    if (td->status & UHCI_TD_STATUS_STALLED) {
        ret = -1;
    } else if (timeout <= 0) {
        ret = -2;
    } else {
        /* 计算实际传输长度 */
        int actual = (td->status >> 0) & 0x7FF;
        ret = length - actual + 1;
    }
    
    /* 恢复帧列表 */
    priv->frame_list[0] = old_entry;
    
    /* 清理 */
    kfree(td);
    kfree(qh);
    
    return ret;
}

/* ========== 中断传输 ========== */

static int uhci_submit_interrupt(struct usb_device *dev, int endpoint,
                                 void *data, int length) {
    /* 中断传输使用周期帧 */
    return uhci_submit_bulk(dev, endpoint, data, length, 1);
}

/* ========== UHCI 控制器操作 ========== */

static const struct usb_controller_ops uhci_ops = {
    .port_reset = uhci_port_reset,
    .port_enable = uhci_port_enable,
    .port_get_speed = uhci_port_get_speed,
    .port_status = uhci_port_status,
    .submit_control = uhci_submit_control,
    .submit_bulk = uhci_submit_bulk,
    .submit_interrupt = uhci_submit_interrupt,
    .alloc_address = uhci_alloc_address,
    .free_address = uhci_free_address,
};

/* ========== UHCI 初始化 ========== */

static int uhci_reset(struct uhci_priv *priv) {
    /* 全局复位 */
    uhci_outw(priv->io_base + UHCI_USBCMD, UHCI_CMD_GRESET);
    delay_ms(15);
    uhci_outw(priv->io_base + UHCI_USBCMD, 0);
    delay_ms(5);
    
    /* 主机控制器复位 */
    uhci_outw(priv->io_base + UHCI_USBCMD, UHCI_CMD_HCRESET);
    
    int timeout = 100;
    while (timeout-- > 0) {
        uint16_t cmd = uhci_inw(priv->io_base + UHCI_USBCMD);
        if (!(cmd & UHCI_CMD_HCRESET)) break;
        delay_ms(1);
    }
    
    return (timeout > 0) ? 0 : -1;
}

int uhci_init(struct usb_controller *ctrl) {
    struct uhci_priv *priv;
    
    print("[UHCI] Initializing controller at I/O 0x");
    print_hex(ctrl->io_base);
    println("");
    
    /* 分配私有数据 */
    priv = kmalloc(sizeof(struct uhci_priv));
    if (!priv) {
        println("[UHCI] Failed to allocate private data");
        return -1;
    }
    
    memset(priv, 0, sizeof(*priv));
    priv->io_base = ctrl->io_base;
    priv->next_address = 1;
    ctrl->private_data = priv;
    
    /* 复位控制器 */
    if (uhci_reset(priv) < 0) {
        println("[UHCI] Reset failed");
        kfree(priv);
        return -1;
    }
    
    /* 分配帧列表 (1024 项，4KB) - 必须 4KB 对齐 */
    /* 使用页对齐分配 */
    uint64_t fl_phys_addr = pmm_alloc_pages(1);  /* 分配1页 = 4KB */
    if (fl_phys_addr == 0) {
        kfree(priv);
        return -1;
    }
    
    /* 映射到虚拟地址 - 使用HHDM直接映射 */
    extern uint64_t hhdm_offset;
    priv->frame_list = (uint32_t *)(fl_phys_addr + hhdm_offset);
    
    print("[UHCI] Frame list physical addr=0x");
    print_hex(fl_phys_addr);
    println("");
    
    print("[UHCI] Frame list virtual addr=0x");
    print_hex((uint64_t)priv->frame_list);
    println("");
    priv->frame_list_phys = fl_phys_addr;
    
    /* Debug: print HHDM offset */
    extern uint64_t hhdm_offset;
    print("[UHCI] HHDM offset=0x");
    print_hex(hhdm_offset);
    println("");
    
    /* 初始化帧列表 - 所有项设置为终止 */
    for (int i = 0; i < 1024; i++) {
        priv->frame_list[i] = UHCI_TD_LINK_T;  /* Terminate */
    }
    
    /* 创建完整的QH调度链 - 参考SerenityOS实现 */
    /* 创建调度锚点QH（也是中断QH） */
    priv->intr_qh = uhci_create_qh();
    if (!priv->intr_qh) {
        kfree(priv->frame_list);
        kfree(priv);
        return -1;
    }
    
    /* 创建Low Speed控制QH */
    priv->ls_ctrl_qh = uhci_create_qh();
    if (!priv->ls_ctrl_qh) {
        kfree(priv->frame_list);
        kfree(priv);
        return -1;
    }
    
    /* 创建Full Speed控制QH */
    priv->fs_ctrl_qh = uhci_create_qh();
    if (!priv->fs_ctrl_qh) {
        kfree(priv->frame_list);
        kfree(priv);
        return -1;
    }
    
    /* 创建批量传输QH */
    priv->bulk_qh = uhci_create_qh();
    if (!priv->bulk_qh) {
        kfree(priv->frame_list);
        kfree(priv);
        return -1;
    }
    
    /* 设置控制QH为fs_ctrl_qh（用于控制传输） */
    priv->ctrl_qh = priv->fs_ctrl_qh;
    
    /* 链接QH形成调度链：intr -> ls_ctrl -> fs_ctrl -> bulk -> intr（循环） */
    uint32_t ls_ctrl_qh_phys = (uint32_t)uhci_virt_to_phys(priv->ls_ctrl_qh);
    uint32_t fs_ctrl_qh_phys = (uint32_t)uhci_virt_to_phys(priv->fs_ctrl_qh);
    uint32_t bulk_qh_phys = (uint32_t)uhci_virt_to_phys(priv->bulk_qh);
    uint32_t intr_qh_phys = (uint32_t)uhci_virt_to_phys(priv->intr_qh);
    
    /* intr_qh -> ls_ctrl_qh */
    priv->intr_qh->link = ls_ctrl_qh_phys | UHCI_TD_LINK_QH;
    priv->intr_qh->element = UHCI_TD_LINK_T;
    
    /* ls_ctrl_qh -> fs_ctrl_qh */
    priv->ls_ctrl_qh->link = fs_ctrl_qh_phys | UHCI_TD_LINK_QH;
    priv->ls_ctrl_qh->element = UHCI_TD_LINK_T;
    
    /* fs_ctrl_qh -> bulk_qh */
    priv->fs_ctrl_qh->link = bulk_qh_phys | UHCI_TD_LINK_QH;
    priv->fs_ctrl_qh->element = UHCI_TD_LINK_T;
    
    /* bulk_qh -> fs_ctrl_qh（形成循环，PIIX4 QEMU工作区）*/
    /* 注意：这与SerenityOS不同，他们循环回intr_qh */
    /* 但为了避免控制器碰到终止点，我们循环回fs_ctrl_qh */
    priv->bulk_qh->link = fs_ctrl_qh_phys | UHCI_TD_LINK_QH;
    priv->bulk_qh->element = UHCI_TD_LINK_T;
    
    /* 将所有帧列表项指向intr_qh（调度链入口） */
    for (int i = 0; i < 1024; i++) {
        priv->frame_list[i] = intr_qh_phys | UHCI_TD_LINK_QH;
    }
    
    print("[UHCI] QH schedule chain created:");
    print(" intr=0x");
    print_hex(intr_qh_phys);
    print(" ls_ctrl=0x");
    print_hex(ls_ctrl_qh_phys);
    print(" fs_ctrl=0x");
    print_hex(fs_ctrl_qh_phys);
    print(" bulk=0x");
    print_hex(bulk_qh_phys);
    println("");
    
    /* 设置帧列表基地址 - 使用已保存的物理地址 */
    print("[UHCI] Setting frame list base address to 0x");
    print_hex(priv->frame_list_phys);
    println("");
    uhci_outl(priv->io_base + UHCI_FLBASEADD, (uint32_t)priv->frame_list_phys);
    
    /* 设置 SOF 修改值 */
    uhci_outb(priv->io_base + UHCI_SOFMOD, 64);
    
    /* 清除状态 */
    uhci_outw(priv->io_base + UHCI_USBSTS, 0xFFFF);
    
    /* 设置配置标志 */
    uhci_outw(priv->io_base + UHCI_USBCMD, UHCI_CMD_CF);
    delay_ms(1);
    
    /* 使能中断 */
    uhci_outw(priv->io_base + UHCI_USBINTR, 
              UHCI_INTR_TIMEOUT | UHCI_INTR_RESUME | UHCI_INTR_IOC);
    
    /* 启动控制器 */
    uhci_outw(priv->io_base + UHCI_USBCMD, UHCI_CMD_CF | UHCI_CMD_RS | UHCI_CMD_MAXP);
    
    /* 等待运行 */
    int timeout = 100;
    while (timeout-- > 0) {
        uint16_t sts = uhci_inw(priv->io_base + UHCI_USBSTS);
        if (!(sts & UHCI_STS_HCH)) break;
        delay_ms(1);
    }
    
    if (timeout <= 0) {
        println("[UHCI] Failed to start");
        kfree(priv->frame_list);
        kfree(priv);
        return -1;
    }
    
    /* 设置控制器信息 */
    ctrl->ops = &uhci_ops;
    ctrl->num_ports = 2;  /* UHCI 通常有 2 个端口 */
    ctrl->num_devices = 0;
    ctrl->initialized = 1;
    
    println("[UHCI] Controller initialized successfully");
    
    return 0;
}
