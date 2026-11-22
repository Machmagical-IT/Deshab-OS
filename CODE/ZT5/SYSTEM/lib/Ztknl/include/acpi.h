/*
 * Zratro OS - ACPI 驱动
 *
 * 功能: RSDP/RSDT/XSDT 表解析, MADT 解析, 电源状态管理, 电池监控
 */

#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== ACPI 签名 ========== */
#define ACPI_SIG_RSDP       "RSD PTR "
#define ACPI_SIG_RSDT       "RSDT"
#define ACPI_SIG_XSDT       "XSDT"
#define ACPI_SIG_MADT       "APIC"
#define ACPI_SIG_FADT       "FACP"
#define ACPI_SIG_DSDT       "DSDT"
#define ACPI_SIG_SSDT       "SSDT"
#define ACPI_SIG_HPET       "HPET"
#define ACPI_SIG_MCFG       "MCFG"
#define ACPI_SIG_BGRT       "BGRT"

/* ========== ACPI 表头 ========== */
struct acpi_table_header {
    char signature[4];           /* 表签名 */
    uint32_t length;             /* 表长度 */
    uint8_t revision;            /* 修订号 */
    uint8_t checksum;            /* 校验和 */
    char oem_id[6];              /* OEM ID */
    char oem_table_id[8];        /* OEM 表 ID */
    uint32_t oem_revision;       /* OEM 修订号 */
    char asl_compiler_id[4];     /* ASL 编译器 ID */
    uint32_t asl_compiler_rev;   /* ASL 编译器修订号 */
} __attribute__((packed));

/* ========== RSDP 结构 ========== */
struct acpi_rsdp {
    char signature[8];           /* "RSD PTR " */
    uint8_t checksum;            /* 校验和 */
    char oem_id[6];              /* OEM ID */
    uint8_t revision;            /* 修订号 (0=1.0, 2=2.0+) */
    uint32_t rsdt_address;       /* RSDT 物理地址 (32位) */
    /* 以下字段仅 ACPI 2.0+ */
    uint32_t length;             /* RSDP 长度 */
    uint64_t xsdt_address;       /* XSDT 物理地址 (64位) */
    uint8_t extended_checksum;   /* 扩展校验和 */
    uint8_t reserved[3];         /* 保留 */
} __attribute__((packed));

/* ========== RSDT/XSDT 结构 ========== */
struct acpi_rsdt {
    struct acpi_table_header header;
    uint32_t entries[];          /* 32位物理地址数组 */
} __attribute__((packed));

struct acpi_xsdt {
    struct acpi_table_header header;
    uint64_t entries[];          /* 64位物理地址数组 */
} __attribute__((packed));

/* ========== MADT (APIC) 结构 ========== */
#define ACPI_MADT_TYPE_LAPIC        0   /* Local APIC */
#define ACPI_MADT_TYPE_IOAPIC       1   /* I/O APIC */
#define ACPI_MADT_TYPE_OVERRIDE     2   /* 中断源覆盖 */
#define ACPI_MADT_TYPE_NMI          3   /* NMI 源 */
#define ACPI_MADT_TYPE_LAPIC_NMI    4   /* Local APIC NMI */
#define ACPI_MADT_TYPE_LAPIC_ADDR   5   /* Local APIC 地址覆盖 */
#define ACPI_MADT_TYPE_IOSAPIC      6   /* I/O SAPIC */

struct acpi_madt {
    struct acpi_table_header header;
    uint32_t lapic_addr;         /* Local APIC 基址 */
    uint32_t flags;              /* 标志 (bit0=PCAT_COMPAT) */
} __attribute__((packed));

struct acpi_madt_lapic {
    uint8_t type;                /* 0 */
    uint8_t length;              /* 8 */
    uint8_t processor_id;        /* ACPI 处理器 ID */
    uint8_t apic_id;             /* Local APIC ID */
    uint32_t flags;              /* bit0=使能 */
} __attribute__((packed));

struct acpi_madt_ioapic {
    uint8_t type;                /* 1 */
    uint8_t length;              /* 12 */
    uint8_t ioapic_id;           /* I/O APIC ID */
    uint8_t reserved;
    uint32_t ioapic_addr;        /* I/O APIC 基址 */
    uint32_t gsi_base;           /* 全局系统中断基址 */
} __attribute__((packed));

struct acpi_madt_override {
    uint8_t type;                /* 2 */
    uint8_t length;              /* 10 */
    uint8_t bus;                 /* 总线 (0=ISA) */
    uint8_t source;              /* IRQ 源 */
    uint32_t gsi;                /* 全局系统中断 */
    uint16_t flags;              /* 极性/触发模式 */
} __attribute__((packed));

/* ========== FADT 结构 ========== */
struct acpi_fadt {
    struct acpi_table_header header;
    uint32_t firmware_ctrl;      /* FACS 地址 */
    uint32_t dsdt;               /* DSDT 地址 */
    uint8_t reserved1;
    uint8_t preferred_pm_profile;/* 电源管理配置 */
    uint16_t sci_int;            /* SCI 中断 */
    uint32_t smi_cmd;            /* SMI 命令端口 */
    uint8_t acpi_enable;         /* ACPI 使能值 */
    uint8_t acpi_disable;        /* ACPI 禁用值 */
    uint8_t s4bios_req;          /* S4 请求值 */
    uint8_t pstate_cnt;          /* P-State 控制值 */
    uint32_t pm1a_evt_blk;       /* PM1a 事件块 */
    uint32_t pm1b_evt_blk;       /* PM1b 事件块 */
    uint32_t pm1a_cnt_blk;       /* PM1a 控制块 */
    uint32_t pm1b_cnt_blk;       /* PM1b 控制块 */
    uint32_t pm2_cnt_blk;        /* PM2 控制块 */
    uint32_t pm_tmr_blk;         /* PM 定时器块 */
    uint32_t gpe0_blk;           /* GPE0 事件块 */
    uint32_t gpe1_blk;           /* GPE1 事件块 */
    uint8_t pm1_evt_len;         /* PM1 事件长度 */
    uint8_t pm1_cnt_len;         /* PM1 控制长度 */
    uint8_t pm2_cnt_len;         /* PM2 控制长度 */
    uint8_t pm_tmr_len;          /* PM 定时器长度 */
    uint8_t gpe0_blk_len;        /* GPE0 长度 */
    uint8_t gpe1_blk_len;        /* GPE1 长度 */
    uint8_t gpe1_base;           /* GPE1 基址 */
    uint8_t cst_cnt;             /* C-State 控制 */
    uint16_t p_lvl2_lat;         /* C2 延迟 */
    uint16_t p_lvl3_lat;         /* C3 延迟 */
    uint16_t flush_size;         /* 刷新大小 */
    uint16_t flush_stride;       /* 刷新步长 */
    uint8_t duty_offset;         /* 占空比偏移 */
    uint8_t duty_width;          /* 占空比宽度 */
    uint8_t day_alrm;            /* 日期闹钟 */
    uint8_t mon_alrm;            /* 月份闹钟 */
    uint8_t century;             /* 世纪寄存器 */
    uint16_t iapc_boot_arch;     /* IA-PC 启动架构 */
    uint8_t reserved2;
    uint32_t flags;              /* FADT 标志 */
    /* ACPI 2.0+ 扩展字段 */
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    uint8_t x_pm1a_evt_blk[12];
    uint8_t x_pm1b_evt_blk[12];
    uint8_t x_pm1a_cnt_blk[12];
    uint8_t x_pm1b_cnt_blk[12];
    uint8_t x_pm2_cnt_blk[12];
    uint8_t x_pm_tmr_blk[12];
    uint8_t x_gpe0_blk[12];
    uint8_t x_gpe1_blk[12];
} __attribute__((packed));

/* ========== 电源状态 ========== */
enum acpi_sleep_state {
    ACPI_STATE_S0 = 0,   /* 工作状态 */
    ACPI_STATE_S1 = 1,   /* 待机 (CPU 停止, 内存刷新) */
    ACPI_STATE_S2 = 2,   /* 低待机 */
    ACPI_STATE_S3 = 3,   /* 睡眠 (挂起到内存) */
    ACPI_STATE_S4 = 4,   /* 休眠 (挂起到磁盘) */
    ACPI_STATE_S5 = 5,   /* 软关机 */
};

/* ========== 电池状态 ========== */
struct acpi_battery_info {
    int present;                 /* 是否存在 */
    int charging;                /* 是否充电中 */
    int discharging;             /* 是否放电中 */
    int critical;                /* 是否严重低电量 */
    uint32_t capacity;           /* 当前容量 (mWh) */
    uint32_t full_capacity;      /* 满容量 (mWh) */
    uint32_t design_capacity;    /* 设计容量 (mWh) */
    uint32_t voltage;            /* 电压 (mV) */
    uint32_t rate;               /* 充放电速率 (mW) */
    uint8_t percent;             /* 电量百分比 */
};

/* ========== ACPI 设备结构 ========== */
struct acpi_device {
    int initialized;                     /* 是否已初始化 */
    int acpi_enabled;                    /* ACPI 是否已启用 */

    /* RSDP 信息 */
    uint64_t rsdp_phys;                  /* RSDP 物理地址 */
    struct acpi_rsdp rsdp;               /* RSDP 副本 */

    /* 表地址 */
    uint64_t rsdt_phys;
    uint64_t xsdt_phys;
    int use_xsdt;                        /* 是否使用 XSDT */
    uint32_t table_count;                /* 表数量 */

    /* 已解析的表 */
    struct acpi_madt *madt;
    struct acpi_fadt *fadt;

    /* MADT 解析结果 */
    uint32_t lapic_addr;                 /* Local APIC 基址 */
    int cpu_count;                       /* CPU 数量 */
    struct {
        uint8_t processor_id;
        uint8_t apic_id;
        int enabled;
    } cpus[256];
    int ioapic_count;
    struct {
        uint8_t id;
        uint32_t addr;
        uint32_t gsi_base;
    } ioapics[8];
    int override_count;
    struct acpi_madt_override overrides[16];

    /* 电源管理 */
    uint16_t sci_irq;                    /* SCI 中断号 */
    uint32_t pm1a_evt;                   /* PM1a 事件块 */
    uint32_t pm1a_cnt;                   /* PM1a 控制块 */
    uint32_t pm_tmr;                     /* PM 定时器块 */

    /* 电池 */
    struct acpi_battery_info battery;

    /* 锁 */
    spinlock_t lock;
};

/* ========== API 函数声明 ========== */

/* 初始化 ACPI */
int acpi_init(void);

/* 查找 RSDP */
int acpi_find_rsdp(void);

/* 验证表校验和 */
int acpi_verify_checksum(const void *table, size_t len);

/* 解析表 */
int acpi_parse_tables(void);
int acpi_parse_madt(void);
int acpi_parse_fadt(void);

/* 查找 ACPI 表 */
struct acpi_table_header *acpi_find_table(const char *signature);

/* 电源管理 */
int acpi_enable(void);
int acpi_disable(void);
int acpi_sleep(enum acpi_sleep_state state);
int acpi_reboot(void);
int acpi_shutdown(void);

/* 电池状态 */
int acpi_battery_update(void);
int acpi_battery_get_info(struct acpi_battery_info *info);

/* 获取设备实例 */
struct acpi_device *acpi_get_device(void);

#endif /* ACPI_H */
