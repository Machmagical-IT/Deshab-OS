#ifndef APIC_H
#define APIC_H

#include <stdint.h>

/* Local APIC 寄存器偏移 */
#define APIC_ID             0x0020  /* Local APIC ID */
#define APIC_VERSION        0x0030  /* Local APIC Version */
#define APIC_TPR            0x0080  /* Task Priority Register */
#define APIC_APR            0x0090  /* Arbitration Priority Register */
#define APIC_PPR            0x00A0  /* Processor Priority Register */
#define APIC_EOI            0x00B0  /* End of Interrupt */
#define APIC_RRD            0x00C0  /* Remote Read Register */
#define APIC_LDR            0x00D0  /* Logical Destination Register */
#define APIC_DFR            0x00E0  /* Destination Format Register */
#define APIC_SVR            0x00F0  /* Spurious Interrupt Vector */
#define APIC_ISR            0x0100  /* In-Service Register (开始) */
#define APIC_TMR            0x0180  /* Trigger Mode Register */
#define APIC_IRR            0x0200  /* Interrupt Request Register */
#define APIC_ESR            0x0280  /* Error Status Register */
#define APIC_ICR_LOW        0x0300  /* Interrupt Command Register (低 32 位) */
#define APIC_ICR_HIGH       0x0310  /* Interrupt Command Register (高 32 位) */
#define APIC_LVT_TIMER      0x0320  /* LVT Timer Register */
#define APIC_LVT_THERMAL    0x0330  /* LVT Thermal Sensor Register */
#define APIC_LVT_PMC        0x0340  /* LVT Performance Monitor Counter */
#define APIC_LVT_LINT0      0x0350  /* LVT LINT0 Register */
#define APIC_LVT_LINT1      0x0360  /* LVT LINT1 Register */
#define APIC_LVT_ERROR      0x0370  /* LVT Error Register */
#define APIC_TIMER_ICR      0x0380  /* Timer Initial Count Register */
#define APIC_TIMER_CCR      0x0390  /* Timer Current Count Register */
#define APIC_TIMER_DCR      0x03E0  /* Timer Divide Configuration Register */

/* APIC SVR 位 */
#define APIC_SVR_ENABLE     (1 << 8)

/* APIC Timer 模式 */
#define APIC_TIMER_MODE_ONE_SHOT    0x00000000
#define APIC_TIMER_MODE_PERIODIC    0x00020000
#define APIC_TIMER_MODE_TSC         0x00040000

/* APIC Timer 分频 */
#define APIC_TIMER_DIV_1    0x0B    /* 除以 1 */
#define APIC_TIMER_DIV_2    0x00    /* 除以 2 */
#define APIC_TIMER_DIV_4    0x01    /* 除以 4 */
#define APIC_TIMER_DIV_8    0x02    /* 除以 8 */
#define APIC_TIMER_DIV_16   0x03    /* 除以 16 */
#define APIC_TIMER_DIV_32   0x08    /* 除以 32 */
#define APIC_TIMER_DIV_64   0x09    /* 除以 64 */
#define APIC_TIMER_DIV_128  0x0A    /* 除以 128 */

/* APIC LVT 位 */
#define APIC_LVT_MASKED     (1 << 16)
#define APIC_LVT_VECTOR     0x000000FF

/* APIC 中断向量 (避免与 IRQ 32-47 冲突) */
#define APIC_TIMER_VECTOR       48
#define APIC_ERROR_VECTOR       49
#define APIC_SPURIOUS_VECTOR    255  /* 必须是 255，这是 APIC 规范 */

/* 默认 APIC 基地址 (需通过 MMIO 访问) */
#define APIC_DEFAULT_BASE   0xFEE00000UL

/* I/O APIC 基地址 */
#define IOAPIC_BASE         0xFEC00000UL

/* I/O APIC 寄存器 */
#define IOAPIC_REG_SEL      0x00    /* 寄存器选择 */
#define IOAPIC_REG_WIN      0x10    /* 数据窗口 */
#define IOAPIC_REG_VER      0x01    /* 版本 */

/* 初始化 Local APIC */
void apic_init(void);

/* 初始化本地 CPU 的 APIC (用于 AP) */
void apic_init_local(void);

/* 获取 APIC 基地址 */
uint64_t apic_get_base(void);

/* 读取 APIC 寄存器 */
uint32_t apic_read(uint32_t reg);

/* 写入 APIC 寄存器 */
void apic_write(uint32_t reg, uint32_t value);

/* 发送 EOI */
void apic_eoi(void);

/* 获取 APIC ID */
uint32_t apic_get_id(void);

/* 设置任务优先级 */
void apic_set_tpr(uint32_t priority);

/* 发送 IPI (处理器间中断) */
void apic_send_ipi(uint32_t apic_id, uint32_t vector, uint32_t mode);

/* 初始化 APIC 定时器 */
void apic_timer_init(uint32_t frequency);

/* 设置定时器频率 */
void apic_timer_set_frequency(uint32_t frequency);

/* 启动定时器 */
void apic_timer_start(void);

/* 停止定时器 */
void apic_timer_stop(void);

/* 启动定时器 (带频率参数) */
void apic_start_timer(uint32_t frequency);

/* 获取定时器频率 (总线频率) */
uint32_t apic_timer_get_bus_frequency(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * MSI / MSI-X 支持
 * ═══════════════════════════════════════════════════════════════════════════ */

/* MSI 能力结构偏移 (PCI 配置空间) */
#define MSI_CAP_ID             0x05    /* MSI 能力 ID */
#define MSI_CTRL_OFFSET        0x02    /* MSI 控制寄存器偏移 (相对能力基址) */
#define MSI_ADDR_OFFSET        0x04    /* MSI 地址寄存器偏移 */
#define MSI_DATA_32_OFFSET     0x08    /* MSI 数据寄存器偏移 (32位) */
#define MSI_DATA_64_OFFSET     0x0C    /* MSI 数据寄存器偏移 (64位) */
#define MSI_MASK_32_OFFSET     0x0C    /* MSI 屏蔽位偏移 (32位) */
#define MSI_MASK_64_OFFSET     0x10    /* MSI 屏蔽位偏移 (64位) */

/* MSI 控制寄存器位 */
#define MSI_CTRL_ENABLE        (1 << 0)    /* 启用 MSI */
#define MSI_CTRL_MULTI_CAP     0x000E      /* 多消息能力 (只读) */
#define MSI_CTRL_MULTI_ENABLE  0x0070      /* 多消息启用 */
#define MSI_CTRL_64BIT         (1 << 7)    /* 64 位地址能力 */
#define MSI_CTRL_PER_VECTOR    (1 << 8)    /* 逐向量屏蔽能力 */

/* MSI-X 能力结构偏移 */
#define MSIX_CAP_ID            0x11    /* MSI-X 能力 ID */
#define MSIX_CTRL_OFFSET       0x02    /* MSI-X 控制寄存器偏移 */
#define MSIX_TABLE_OFFSET      0x04    /* MSI-X 表偏移/BIR */
#define MSIX_PBA_OFFSET        0x08    /* MSI-X PBA 偏移/BIR */

/* MSI-X 控制寄存器位 */
#define MSIX_CTRL_ENABLE       (1 << 15)   /* 启用 MSI-X */
#define MSIX_CTRL_FUNC_MASK    (1 << 14)   /* 函数级屏蔽 */
#define MSIX_CTRL_TABLE_SIZE   0x07FF      /* 表大小 (只读) */

/* MSI-X 表项结构 */
struct msix_table_entry {
    uint32_t msg_addr_low;     /* 消息地址低 32 位 */
    uint32_t msg_addr_high;    /* 消息地址高 32 位 */
    uint32_t msg_data;         /* 消息数据 */
    uint32_t vector_ctrl;      /* 向量控制 (bit 0 = 屏蔽) */
} __attribute__((packed));

/* MSI-X 配置 */
struct msix_config {
    uint64_t table_base;       /* 表的虚拟地址 */
    uint64_t pba_base;         /* PBA 的虚拟地址 */
    uint16_t table_size;       /* 表项数 */
    uint16_t cap_offset;       /* PCI 能力偏移 */
    uint8_t  enabled;          /* 是否启用 */
    uint8_t  reserved[3];
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 中断亲和性与节流
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 中断亲和性配置 */
struct irq_affinity {
    uint8_t  irq;              /* IRQ 号 */
    uint8_t  target_cpu;       /* 目标 CPU (APIC ID) */
    uint8_t  current_cpu;      /* 当前 CPU (APIC ID) */
    uint8_t  flags;            /* 标志 */
};

#define IRQ_AFFINITY_STRICT     (1 << 0)   /* 严格亲和性 (不自动迁移) */

/* IRQ 节流配置 */
struct irq_throttle_config {
    uint32_t max_rate;         /* 每秒最大中断数 */
    uint32_t current_rate;     /* 当前速率 */
    uint64_t window_start;     /* 时间窗口起始 (tick) */
    uint32_t window_count;     /* 当前窗口内中断计数 */
    uint8_t  enabled;          /* 是否启用 */
    uint8_t  reserved[3];
};

/* 每个 IRQ 的统计信息 */
struct irq_stats_entry {
    uint64_t count;            /* 中断总数 */
    uint64_t total_time_ns;    /* 处理总时间 (纳秒) */
    uint64_t max_time_ns;      /* 单次最长处理时间 */
    uint64_t last_timestamp;   /* 最后一次中断时间戳 */
    uint32_t dropped;          /* 因节流丢弃的中断数 */
};

#define MAX_IRQ_STATS          256

/* ═══════════════════════════════════════════════════════════════════════════
 * PCI 配置空间访问 (MSI/MSI-X 需要)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* PCI 配置空间访问函数类型 */
typedef uint32_t (*pci_read_config_t)(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
typedef void (*pci_write_config_t)(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value);

/* 注册 PCI 配置空间访问函数 */
void apic_register_pci_access(pci_read_config_t read_fn, pci_write_config_t write_fn);

/* ═══════════════════════════════════════════════════════════════════════════
 * MSI / MSI-X 函数声明
 * ═══════════════════════════════════════════════════════════════════════════ */

/* MSI 设置: 为 PCI 设备配置 MSI 中断 */
int apic_setup_msi(uint8_t bus, uint8_t dev, uint8_t func,
                   uint8_t vector, uint8_t apic_id, int nvec);

/* MSI 禁用 */
int apic_disable_msi(uint8_t bus, uint8_t dev, uint8_t func);

/* MSI-X 设置: 为 PCI 设备配置 MSI-X 中断 */
int apic_setup_msix(uint8_t bus, uint8_t dev, uint8_t func,
                    struct msix_config *config);

/* MSI-X 禁用 */
int apic_disable_msix(uint8_t bus, uint8_t dev, uint8_t func,
                      struct msix_config *config);

/* MSI-X 向量屏蔽/取消屏蔽 */
void apic_msix_mask_vector(struct msix_config *config, uint16_t entry);
void apic_msix_unmask_vector(struct msix_config *config, uint16_t entry);

/* MSI-X 获取待处理向量 */
int apic_msix_is_pending(struct msix_config *config, uint16_t entry);

/* ═══════════════════════════════════════════════════════════════════════════
 * 中断亲和性函数声明
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 设置 IRQ 亲和性 (绑定到指定 CPU) */
int apic_set_irq_affinity(uint8_t irq, uint8_t target_cpu);

/* 获取 IRQ 亲和性 */
int apic_get_irq_affinity(uint8_t irq, uint8_t *target_cpu);

/* ═══════════════════════════════════════════════════════════════════════════
 * IRQ 节流函数声明
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 设置 IRQ 节流 */
int apic_set_irq_throttle(uint8_t irq, uint32_t max_rate_per_sec);

/* 检查 IRQ 是否应该处理 (节流检查) */
int apic_irq_throttle_check(uint8_t irq);

/* ═══════════════════════════════════════════════════════════════════════════
 * 中断统计函数声明
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 记录 IRQ 处理开始 (返回起始 TSC) */
uint64_t apic_irq_stats_begin(uint8_t irq);

/* 记录 IRQ 处理结束 */
void apic_irq_stats_end(uint8_t irq, uint64_t start_tsc);

/* 获取 IRQ 统计信息 */
int apic_get_irq_stats(uint8_t irq, struct irq_stats_entry *stats);

/* 初始化 I/O APIC */
void ioapic_init(void);

/* 设置 I/O APIC 中断重定向 */
void ioapic_set_irq(uint8_t irq, uint8_t vector, uint32_t apic_id);

#endif
