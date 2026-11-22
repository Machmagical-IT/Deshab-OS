/*
 * Zratro OS - ACPI 驱动
 *
 * 功能: RSDP/RSDT/XSDT 表解析, MADT 解析, 电源状态管理, 电池监控
 */

#include "acpi.h"
#include "console.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "cpu.h"

/* ========== 全局设备实例 ========== */

static struct acpi_device acpi_dev;

/* ========== 物理内存访问辅助 ========== */

static inline void *acpi_map_table(uint64_t phys) {
    return (void *)phys_to_virt(phys);
}

/* ========== 校验和验证 ========== */

int acpi_verify_checksum(const void *table, size_t len) {
    const uint8_t *p = (const uint8_t *)table;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += p[i];
    }
    return sum == 0 ? 0 : -1;
}

/* ========== RSDP 查找 ========== */

/* 在指定区域搜索 RSDP */
static int acpi_scan_for_rsdp(uint64_t start, uint64_t end) {
    for (uint64_t addr = start; addr < end; addr += 16) {
        const char *sig = (const char *)phys_to_virt(addr);
        if (sig[0] == 'R' && sig[1] == 'S' && sig[2] == 'D' &&
            sig[3] == ' ' && sig[4] == 'P' && sig[5] == 'T' &&
            sig[6] == 'R' && sig[7] == ' ') {
            /* 找到签名，验证校验和 */
            struct acpi_rsdp *rsdp = (struct acpi_rsdp *)phys_to_virt(addr);
            size_t check_len = (rsdp->revision >= 2) ? rsdp->length : 20;

            if (acpi_verify_checksum(rsdp, check_len) == 0) {
                acpi_dev.rsdp_phys = addr;
                memcpy(&acpi_dev.rsdp, rsdp, sizeof(struct acpi_rsdp));
                return 0;
            }
        }
    }
    return -1;
}

int acpi_find_rsdp(void) {
    /* 方法1: 搜索 EBDA (扩展 BIOS 数据区) */
    /* EBDA 地址存储在 0x40E */
    uint16_t ebda_seg = *(volatile uint16_t *)phys_to_virt(0x40E);
    uint64_t ebda_addr = (uint64_t)ebda_seg << 4;
    if (ebda_addr != 0 && acpi_scan_for_rsdp(ebda_addr, ebda_addr + 0x400) == 0) {
        return 0;
    }

    /* 方法2: 搜索 BIOS ROM 区域 (0xE0000 - 0xFFFFF) */
    if (acpi_scan_for_rsdp(0xE0000, 0x100000) == 0) {
        return 0;
    }

    printk("acpi: 未找到 RSDP");
    return -1;
}

/* ========== 表查找 ========== */

struct acpi_table_header *acpi_find_table(const char *signature) {
    if (acpi_dev.use_xsdt && acpi_dev.xsdt_phys != 0) {
        struct acpi_xsdt *xsdt = (struct acpi_xsdt *)acpi_map_table(acpi_dev.xsdt_phys);
        uint32_t count = (xsdt->header.length - sizeof(struct acpi_table_header)) / 8;

        for (uint32_t i = 0; i < count; i++) {
            struct acpi_table_header *hdr =
                (struct acpi_table_header *)acpi_map_table(xsdt->entries[i]);
            if (memcmp(hdr->signature, signature, 4) == 0) {
                return hdr;
            }
        }
    } else if (acpi_dev.rsdt_phys != 0) {
        struct acpi_rsdt *rsdt = (struct acpi_rsdt *)acpi_map_table(acpi_dev.rsdt_phys);
        uint32_t count = (rsdt->header.length - sizeof(struct acpi_table_header)) / 4;

        for (uint32_t i = 0; i < count; i++) {
            struct acpi_table_header *hdr =
                (struct acpi_table_header *)acpi_map_table(rsdt->entries[i]);
            if (memcmp(hdr->signature, signature, 4) == 0) {
                return hdr;
            }
        }
    }
    return NULL;
}

/* ========== MADT 解析 ========== */

int acpi_parse_madt(void) {
    struct acpi_table_header *hdr = acpi_find_table(ACPI_SIG_MADT);
    if (hdr == NULL) {
        printk("acpi: 未找到 MADT 表");
        return -1;
    }

    struct acpi_madt *madt = (struct acpi_madt *)hdr;
    acpi_dev.madt = madt;
    acpi_dev.lapic_addr = madt->lapic_addr;

    printk("acpi: MADT 地址=0x%08X LAPIC基址=0x%08X 标志=0x%08X",
           (uint32_t)acpi_dev.madt, madt->lapic_addr, madt->flags);

    /* 解析 MADT 条目 */
    uint8_t *ptr = (uint8_t *)(madt + 1);
    uint8_t *end = (uint8_t *)madt + madt->header.length;
    acpi_dev.cpu_count = 0;
    acpi_dev.ioapic_count = 0;
    acpi_dev.override_count = 0;

    while (ptr < end) {
        uint8_t type = ptr[0];
        uint8_t len = ptr[1];

        if (len == 0) break;  /* 防止无限循环 */

        switch (type) {
        case ACPI_MADT_TYPE_LAPIC: {
            struct acpi_madt_lapic *lapic = (struct acpi_madt_lapic *)ptr;
            if (acpi_dev.cpu_count < 256) {
                acpi_dev.cpus[acpi_dev.cpu_count].processor_id = lapic->processor_id;
                acpi_dev.cpus[acpi_dev.cpu_count].apic_id = lapic->apic_id;
                acpi_dev.cpus[acpi_dev.cpu_count].enabled = (lapic->flags & 1) != 0;
                printk("acpi: CPU %u: APIC ID=%u %s",
                       acpi_dev.cpu_count, lapic->apic_id,
                       (lapic->flags & 1) ? "启用" : "禁用");
                acpi_dev.cpu_count++;
            }
            break;
        }

        case ACPI_MADT_TYPE_IOAPIC: {
            struct acpi_madt_ioapic *ioapic = (struct acpi_madt_ioapic *)ptr;
            if (acpi_dev.ioapic_count < 8) {
                acpi_dev.ioapics[acpi_dev.ioapic_count].id = ioapic->ioapic_id;
                acpi_dev.ioapics[acpi_dev.ioapic_count].addr = ioapic->ioapic_addr;
                acpi_dev.ioapics[acpi_dev.ioapic_count].gsi_base = ioapic->gsi_base;
                printk("acpi: I/O APIC %u: 地址=0x%08X GSI基址=%u",
                       ioapic->ioapic_id, ioapic->ioapic_addr, ioapic->gsi_base);
                acpi_dev.ioapic_count++;
            }
            break;
        }

        case ACPI_MADT_TYPE_OVERRIDE: {
            struct acpi_madt_override *ovr = (struct acpi_madt_override *)ptr;
            if (acpi_dev.override_count < 16) {
                acpi_dev.overrides[acpi_dev.override_count] = *ovr;
                printk("acpi: IRQ 覆盖: BUS=%u SRC=%u GSI=%u 标志=0x%04X",
                       ovr->bus, ovr->source, ovr->gsi, ovr->flags);
                acpi_dev.override_count++;
            }
            break;
        }

        default:
            break;
        }

        ptr += len;
    }

    printk("acpi: MADT 解析完成: %d CPU, %d I/O APIC, %d IRQ 覆盖",
           acpi_dev.cpu_count, acpi_dev.ioapic_count, acpi_dev.override_count);
    return 0;
}

/* ========== FADT 解析 ========== */

int acpi_parse_fadt(void) {
    struct acpi_table_header *hdr = acpi_find_table(ACPI_SIG_FADT);
    if (hdr == NULL) {
        printk("acpi: 未找到 FADT 表");
        return -1;
    }

    struct acpi_fadt *fadt = (struct acpi_fadt *)hdr;
    acpi_dev.fadt = fadt;

    /* 提取电源管理信息 */
    acpi_dev.sci_irq = fadt->sci_int;
    acpi_dev.pm1a_evt = fadt->pm1a_evt_blk;
    acpi_dev.pm1a_cnt = fadt->pm1a_cnt_blk;
    acpi_dev.pm_tmr = fadt->pm_tmr_blk;

    printk("acpi: FADT: SCI IRQ=%u PM1a事件=0x%08X PM1a控制=0x%08X PM定时器=0x%08X",
           fadt->sci_int, fadt->pm1a_evt_blk, fadt->pm1a_cnt_blk, fadt->pm_tmr_blk);
    printk("acpi: FADT: SMI命令=0x%08X ACPI使能=0x%02X ACPI禁用=0x%02X",
           fadt->smi_cmd, fadt->acpi_enable, fadt->acpi_disable);

    return 0;
}

/* ========== 表解析 ========== */

int acpi_parse_tables(void) {
    /* 解析 RSDT/XSDT */
    if (acpi_dev.rsdp.revision >= 2 && acpi_dev.rsdp.xsdt_address != 0) {
        acpi_dev.xsdt_phys = acpi_dev.rsdp.xsdt_address;
        acpi_dev.use_xsdt = 1;

        struct acpi_xsdt *xsdt = (struct acpi_xsdt *)acpi_map_table(acpi_dev.xsdt_phys);
        acpi_dev.table_count = (xsdt->header.length - sizeof(struct acpi_table_header)) / 8;

        /* 验证 XSDT 校验和 */
        if (acpi_verify_checksum(xsdt, xsdt->header.length) != 0) {
            printk("acpi: XSDT 校验和错误");
            acpi_dev.use_xsdt = 0;
        } else {
            printk("acpi: 使用 XSDT (%u 个表)", acpi_dev.table_count);
        }
    }

    if (!acpi_dev.use_xsdt) {
        acpi_dev.rsdt_phys = acpi_dev.rsdp.rsdt_address;

        struct acpi_rsdt *rsdt = (struct acpi_rsdt *)acpi_map_table(acpi_dev.rsdt_phys);
        acpi_dev.table_count = (rsdt->header.length - sizeof(struct acpi_table_header)) / 4;

        if (acpi_verify_checksum(rsdt, rsdt->header.length) != 0) {
            printk("acpi: RSDT 校验和错误");
            return -1;
        }

        printk("acpi: 使用 RSDT (%u 个表)", acpi_dev.table_count);
    }

    /* 列出所有表 */
    printk("acpi: 已发现的 ACPI 表:");
    for (uint32_t i = 0; i < acpi_dev.table_count; i++) {
        uint64_t entry_addr;
        if (acpi_dev.use_xsdt) {
            struct acpi_xsdt *xsdt = (struct acpi_xsdt *)acpi_map_table(acpi_dev.xsdt_phys);
            entry_addr = xsdt->entries[i];
        } else {
            struct acpi_rsdt *rsdt = (struct acpi_rsdt *)acpi_map_table(acpi_dev.rsdt_phys);
            entry_addr = rsdt->entries[i];
        }

        struct acpi_table_header *hdr = (struct acpi_table_header *)acpi_map_table(entry_addr);
        printk("acpi:   [%.4s] 地址=0x%016llX 长度=%u 修订=%u OEM=%.6s",
               hdr->signature, entry_addr, hdr->length, hdr->revision, hdr->oem_id);
    }

    /* 解析关键表 */
    acpi_parse_madt();
    acpi_parse_fadt();

    return 0;
}

/* ========== 电源管理 ========== */

static inline void acpi_outw(uint16_t port, uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t acpi_inw(uint16_t port) {
    uint16_t val;
    asm volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void acpi_outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

int acpi_enable(void) {
    if (acpi_dev.fadt == NULL) return -1;

    /* 通过 SMI 命令启用 ACPI */
    if (acpi_dev.fadt->smi_cmd != 0 && acpi_dev.fadt->acpi_enable != 0) {
        acpi_outb(acpi_dev.fadt->smi_cmd, acpi_dev.fadt->acpi_enable);

        /* 等待 ACPI 启用 */
        for (int i = 0; i < 1000; i++) {
            if (acpi_inw(acpi_dev.pm1a_cnt) & 0x0001) {
                acpi_dev.acpi_enabled = 1;
                printk("acpi: ACPI 已启用");
                return 0;
            }
        }
        printk("acpi: ACPI 启用超时");
        return -1;
    }

    /* 某些系统 ACPI 默认启用 */
    acpi_dev.acpi_enabled = 1;
    printk("acpi: ACPI 默认已启用");
    return 0;
}

int acpi_disable(void) {
    if (acpi_dev.fadt == NULL) return -1;

    if (acpi_dev.fadt->smi_cmd != 0 && acpi_dev.fadt->acpi_disable != 0) {
        acpi_outb(acpi_dev.fadt->smi_cmd, acpi_dev.fadt->acpi_disable);
        acpi_dev.acpi_enabled = 0;
        printk("acpi: ACPI 已禁用");
    }
    return 0;
}

int acpi_sleep(enum acpi_sleep_state state) {
    if (!acpi_dev.acpi_enabled) return -1;

    /* 设置睡眠类型 (SLP_TYP) 在 PM1a_CNT */
    uint16_t pm1a_cnt = acpi_inw(acpi_dev.pm1a_cnt);
    pm1a_cnt &= ~0x1C00;  /* 清除 SLP_TYP */
    pm1a_cnt |= ((uint16_t)state & 0x07) << 10;
    pm1a_cnt |= 0x2000;   /* 设置 SLP_EN */
    acpi_outw(acpi_dev.pm1a_cnt, pm1a_cnt);

    /* 等待进入睡眠 */
    for (volatile int i = 0; i < 1000000; i++);
    return 0;
}

int acpi_reboot(void) {
    /* 使用 ACPI 重置寄存器 (如果可用) */
    if (acpi_dev.fadt != NULL) {
        /* FADT reset register (ACPI 2.0+) */
        uint8_t reset_reg_space = acpi_dev.fadt->x_pm1a_cnt_blk[0];

        if (reset_reg_space == 1) {
            /* I/O 空间 */
            uint16_t reset_addr = *(uint16_t *)&acpi_dev.fadt->x_pm1a_cnt_blk[4];
            acpi_outb(reset_addr, acpi_dev.fadt->acpi_disable);
        }
    }

    /* 回退: 键盘控制器复位 */
    uint8_t val = 0x02;
    while (val & 0x02) {
        val = inb(0x64);
    }
    outb(0x64, 0xFE);  /* 脉冲复位线 */

    return 0;
}

int acpi_shutdown(void) {
    return acpi_sleep(ACPI_STATE_S5);
}

/* ========== 电池状态 ========== */

int acpi_battery_update(void) {
    /* TODO: 完整的电池状态需要 AML 解释器
     * 这里提供框架，实际实现需要:
     * 1. 在 DSDT/SSDT 中查找电池设备 (_HID = PNP0C0A)
     * 2. 执行 _STA, _BIF, _BST 方法
     * 3. 解析返回的包数据
     */
    acpi_dev.battery.present = 0;
    return 0;
}

int acpi_battery_get_info(struct acpi_battery_info *info) {
    if (info == NULL) return Z_ERR_NULL;
    *info = acpi_dev.battery;
    return 0;
}

/* ========== 初始化 ========== */

int acpi_init(void) {
    printk("acpi: 初始化 ACPI 子系统...");

    memset(&acpi_dev, 0, sizeof(acpi_dev));
    spinlock_init(&acpi_dev.lock);

    /* 查找 RSDP */
    if (acpi_find_rsdp() != 0) {
        printk("acpi: RSDP 未找到, ACPI 不可用");
        return -1;
    }

    printk("acpi: RSDP 物理地址=0x%016llX 修订=%u OEM=%.6s",
           acpi_dev.rsdp_phys, acpi_dev.rsdp.revision, acpi_dev.rsdp.oem_id);

    /* 解析 ACPI 表 */
    if (acpi_parse_tables() != 0) {
        printk("acpi: 表解析失败");
        return -1;
    }

    /* 启用 ACPI */
    acpi_enable();

    acpi_dev.initialized = 1;
    printk("acpi: ACPI 子系统初始化完成");
    return 0;
}

struct acpi_device *acpi_get_device(void) {
    return &acpi_dev;
}
