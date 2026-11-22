/* DKM ACPI Driver - RSDP scan / RSDT parse / table enumeration
 * Stage 0, optional, provides "acpi" + "acpi.power".
 * Scans BIOS memory for RSDP, then walks RSDT/XSDT to list all ACPI tables.
 *
 * 电源路径（母本 SYSTEMCOPY/OpenXJ380 driver/power.cpp，Apache-2.0）：
 *   FADT → DSDT `_S5_` AML 包解析 → PM1a/PM1b_CNT 写 SLP_TYP|SLP_EN 关机；
 *   FADT reset_reg (ACPI 2.0+ GAS) → 复位；两者失败依次走 QEMU/VMware 端口
 *   与键盘控制器/CF9 复位兜底。服务：srv.acpi.poweroff / srv.acpi.reboot。
 */

#include "../dkm_shared.h"

/* ---------------------------------------------------------------
 * ACPI structures
 * --------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    char     signature[8];
    u8       checksum;
    char     oem_id[6];
    u8       revision;
    u32      rsdt_addr;
} acpi_rsdp_v1;

typedef struct __attribute__((packed)) {
    /* RSDP v1 fields */
    char     signature[8];
    u8       checksum;
    char     oem_id[6];
    u8       revision;
    u32      rsdt_addr;
    /* RSDP v2+ fields */
    u32      length;
    u64      xsdt_addr;
    u8       ext_checksum;
    u8       reserved[3];
} acpi_rsdp_v2;

typedef struct __attribute__((packed)) {
    char     signature[4];
    u32      length;
    u8       revision;
    u8       checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    u32      oem_revision;
    u32      creator_id;
    u32      creator_revision;
} acpi_sdt_header;

/* ACPI Generic Address Structure (GAS) — HPET 等表使用 */
typedef struct __attribute__((packed)) {
    u8  space_id;     /* 0=SystemMemory, 1=SystemIO, 2=PCIConfig, ... */
    u8  bit_width;
    u8  bit_offset;
    u8  access_size;  /* 0=undefined, 1=byte, 2=word, 3=dword, 4=qword */
    u64 address;
} acpi_generic_address;

/* FADT (Fixed ACPI Description Table, 签名字 "FACP")。
 * 字段顺序严格按 ACPI 6.x §5.2.9，reserved 字段名保留以便核对偏移。 */
typedef struct __attribute__((packed)) {
    acpi_sdt_header hdr;
    u32 firmware_ctrl;
    u32 dsdt;
    u8  reserved0;
    u8  preferred_pm_profile;
    u16 sci_int;
    u32 smi_cmd;
    u8  acpi_enable;
    u8  acpi_disable;
    u8  s4bios_req;
    u8  pstate_cnt;
    u32 pm1a_evt_blk;
    u32 pm1b_evt_blk;
    u32 pm1a_cnt_blk;
    u32 pm1b_cnt_blk;
    u32 pm2_cnt_blk;
    u32 pm_tmr_blk;
    u32 gpe0_blk;
    u32 gpe1_blk;
    u8  pm1_evt_len;
    u8  pm1_cnt_len;
    u8  pm2_cnt_len;
    u8  pm_tmr_len;
    u8  gpe0_blk_len;
    u8  gpe1_blk_len;
    u8  gpe1_base;
    u8  cst_cnt;
    u16 p_lvl2_lat;
    u16 p_lvl3_lat;
    u16 flush_size;
    u16 flush_stride;
    u8  duty_offset;
    u8  duty_width;
    u8  day_alrm;
    u8  mon_alrm;
    u8  century;
    u16 iapc_boot_arch;
    u8  reserved1;
    u32 flags;
    acpi_generic_address reset_reg;
    u8  reset_value;
    u8  reserved2[3];
    u64 x_firmware_ctrl;
    u64 x_dsdt;
} acpi_fadt;

#define ACPI_SLP_EN          (1u << 13)
#define ACPI_GAS_SYSTEM_MEM  0
#define ACPI_GAS_SYSTEM_IO   1

static const acpi_fadt *g_fadt;
static u16 g_slp_typa, g_slp_typb;
static int g_power_ready;   /* 1 = _S5_ 解析成功，S5 可用 */

/* ---------------------------------------------------------------
 * Driver descriptor
 * --------------------------------------------------------------- */
static const char *const g_provides[] = { "acpi" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "acpi",
    .version        = "0.1.0",
    .vendor         = "Deshab",
    .driver_class   = 1,   /* DKM_CLASS_PLATFORM */
    .stage          = 0,
    .flags          = 0,   /* optional */
    .priority       = 0,
    .depends        = NULL,
    .depends_count  = 0,
    .provides       = g_provides,
    .provides_count = 1,
    .min_kernel_abi = 1,
    .feature_bits   = 0,
    .reserved0      = 0,
    .reserved1      = 0
};

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */
static const struct dkm_log_api *g_log;
static const struct dkm_kernel_api *g_api;

/* ACPI 表 / GAS 地址是物理地址。有 HHDM 走 HHDM，否则退回引导器
 * 建立的恒等映射（低 4GB 表一定可见）。 */
static u64 acpi_map(u64 phys) {
    if (!phys) return 0;
    if (g_api && g_api->hhdm_offset) return g_api->hhdm_offset + phys;
    return phys;
}

static int memneq(const char *a, const char *b, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (a[i] != b[i]) return 1;
    }
    return 0;
}

static u8 checksum8(const u8 *data, u32 len) {
    u8 sum = 0;
    for (u32 i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

static void log_hex(const char *prefix, u64 val) {
    static const char hex[] = "0123456789abcdef";
    char buf[32];
    u32 pos = 0;
    while (pos < 30) { buf[pos] = 0; pos++; }
    pos = 0;
    buf[pos++] = '0';
    buf[pos++] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[pos++] = hex[(val >> (i * 4)) & 0xf];
    }
    buf[pos] = 0;

    /* manual concat via serial_write-like approach: pass prefix then buf */
    g_log->info(prefix);
    g_log->info(buf);
}

/* ---------------------------------------------------------------
 * RSDP scan
 * --------------------------------------------------------------- */
static const acpi_rsdp_v2 *acpi_find_rsdp(const void *limine_rsdp) {
    /* If Limine gave us the RSDP, use it directly */
    if (limine_rsdp) {
        const acpi_rsdp_v2 *rsdp = (const acpi_rsdp_v2 *)limine_rsdp;
        if (memneq(rsdp->signature, "RSD PTR ", 8) == 0) {
            if (checksum8((const u8 *)rsdp, 20) == 0) {
                return rsdp;
            }
        }
    }
    /* EBDA: extended BIOS data area, base at 0x40E */
    {
        u16 ebda_seg = *(const volatile u16 *)(uintptr_t)0x40E;
        u64 ebda_base = (u64)ebda_seg << 4;
        if (ebda_base >= 0x80000ULL && ebda_base < 0x100000ULL) {
            for (u64 off = 0; off < 1024; off += 16) {
                const acpi_rsdp_v2 *rsdp = (const acpi_rsdp_v2 *)(uintptr_t)(ebda_base + off);
                if (off + sizeof(acpi_rsdp_v1) > 1024) break;
                if (memneq(rsdp->signature, "RSD PTR ", 8) == 0) {
                    if (checksum8((const u8 *)rsdp, 20) == 0) {
                        return rsdp;
                    }
                }
            }
        }
    }

    /* BIOS read-only area: 0x000E0000 - 0x000FFFFF */
    for (u64 addr = 0x000E0000ULL; addr < 0x00100000ULL; addr += 16) {
        const acpi_rsdp_v2 *rsdp = (const acpi_rsdp_v2 *)(uintptr_t)addr;
        if (memneq(rsdp->signature, "RSD PTR ", 8) == 0) {
            if (checksum8((const u8 *)rsdp, 20) == 0) {
                return rsdp;
            }
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------
 * MCFG (PCIe ECAM Memory-mapped Configuration) 解析
 *
 * **QEMU 无法测试但真机必需**: PCIe 配置空间通过 ECAM 访问,
 * MCFG 表提供 ECAM 基址和总线范围。没有 MCFG 解析,
 * 真机无法访问 Bus 0 以外的 PCIe 设备配置空间。
 * QEMU 的 PCI 设备都在 Bus 0, 使用传统 CF8/CFC 端口即可。
 * --------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    u64 base_address;       /* ECAM 基址 (4K 对齐, 256MB per bus) */
    u16 pci_segment_group;  /* PCI 段组号 (通常为 0) */
    u8  start_bus_number;   /* 起始总线号 */
    u8  end_bus_number;     /* 结束总线号 */
    u32 reserved;
} mcfg_entry;

typedef struct __attribute__((packed)) {
    acpi_sdt_header hdr;
    u64 reserved;           /* 保留, 通常为 0 */
    /* mcfg_entry entries[] 紧跟 */
} mcfg_table;

static void acpi_parse_mcfg(const acpi_sdt_header *hdr) {
    if (!hdr || hdr->length < sizeof(mcfg_table)) return;

    const mcfg_table *mcfg = (const mcfg_table *)hdr;
    u32 entry_size = sizeof(mcfg_entry);
    u32 entries_len = mcfg->hdr.length - sizeof(mcfg_table);
    u32 count = entries_len / entry_size;

    g_log->info("[acpi] MCFG entries:");
    log_hex("[acpi]   count=", count);

    for (u32 i = 0; i < count; i++) {
        const mcfg_entry *e = (const mcfg_entry *)((const u8 *)mcfg + sizeof(mcfg_table) + i * entry_size);
        log_hex("[acpi]   MCFG base=", e->base_address);
        log_hex("[acpi]   MCFG seg=", e->pci_segment_group);
        log_hex("[acpi]   MCFG bus_start=", e->start_bus_number);
        log_hex("[acpi]   MCFG bus_end=", e->end_bus_number);

        /* 验证 ECAM 基址合理性 */
        if (e->base_address == 0 || e->base_address == 0xFFFFFFFFFFFFFFFFULL) {
            g_log->warn("[acpi]   MCFG entry has invalid base address");
            continue;
        }
        /* 真机: ECAM 基址通常在 0xB0000000 - 0xBFFFFFFF (3G-4G 范围),
         * 但也可能在 4G 以上 (特别是大内存系统) */
        if (e->base_address >= 0x100000000ULL) {
            g_log->info("[acpi]   MCFG base above 4G — requires MMIO mapping");
        }
    }
}

/* ---------------------------------------------------------------
 * HPET (High Precision Event Timer) 解析
 *
 * 真机需要 HPET 替代 PIT 作为高精度定时器源。
 * QEMU 默认也提供 HPET, 但当前内核使用 PIT 即可。
 * HPET 信息留作后续高精度定时器实现的参考。
 * --------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    u8  hardware_rev_id;
    u8  comparator_count : 5;
    u8  counter_size : 1;       /* 0=32-bit, 1=64-bit */
    u8  reserved0 : 1;
    u8  legacy_replacement : 1; /* 1=LegacyReplacement IRQ routing available */
    u16 pci_vendor_id;
    acpi_generic_address address;  /* HPET 寄存器基址 */
    u8  hpet_number;
    u16 minimum_tick;
    u8  page_protection;
} hpet_entry_data;

/* HPET 表不含子条目, 只有 1 个 data block */
typedef struct __attribute__((packed)) {
    acpi_sdt_header hdr;
    u32 event_timer_block_id;  /* 与 hpet_entry_data 的前 4 字节相同 */
    acpi_generic_address base_address;
    u8  hpet_number;
    u16 minimum_tick;
    u8  page_protection;
} hpet_table;

static void acpi_parse_hpet(const acpi_sdt_header *hdr) {
    if (!hdr || hdr->length < sizeof(hpet_table)) return;

    const hpet_table *hpet = (const hpet_table *)hdr;

    u32 block_id = hpet->event_timer_block_id;
    u8  comp_count = (block_id >> 8) & 0x1f;
    u8  count_size = (block_id >> 13) & 1;
    u8  legacy     = (block_id >> 15) & 1;
    u8  rev_id     = block_id & 0xff;
    u16 vendor_id  = (block_id >> 16) & 0xffff;

    g_log->info("[acpi] HPET info:");
    log_hex("[acpi]   rev_id=", rev_id);
    log_hex("[acpi]   comparators=", comp_count);
    log_hex("[acpi]   counter_size=", count_size ? 64 : 32);
    log_hex("[acpi]   legacy_irq=", legacy);
    log_hex("[acpi]   vendor_id=", vendor_id);
    log_hex("[acpi]   min_tick=", hpet->minimum_tick);

    /* HPET 地址空间类型 */
    if (hpet->base_address.space_id == 0) {
        /* System Memory — MMIO 访问 */
        u64 addr = hpet->base_address.address;
        log_hex("[acpi]   HPET MMIO addr=", addr);
        if (addr >= 0x100000000ULL) {
            g_log->info("[acpi]   HPET base above 4G — requires MMIO mapping");
        }
    } else if (hpet->base_address.space_id == 1) {
        /* System I/O — 端口 I/O */
        log_hex("[acpi]   HPET I/O port=", hpet->base_address.address);
    }
}

/* ---------------------------------------------------------------
 * MADT (APIC) 解析 — 为 apic 驱动提供数据
 * --------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    acpi_sdt_header hdr;
    u32 lapic_addr;
    u32 flags;
} acpi_madt;

static void acpi_parse_madt(const acpi_sdt_header *hdr) {
    if (!hdr || hdr->length < sizeof(acpi_madt)) return;
    const acpi_madt *madt = (const acpi_madt *)hdr;

    g_log->info("[acpi] MADT detail:");
    log_hex("[acpi]   LAPIC addr=", madt->lapic_addr);
    log_hex("[acpi]   flags=", madt->flags);
    if (madt->flags & 1) {
        g_log->info("[acpi]   PCAT_COMPAT=1 (8259 PIC present)");
    }

    /* 枚举 MADT 子条目做汇总统计 */
    const u8 *base = (const u8 *)madt;
    u32 off = sizeof(acpi_madt);
    u32 lapic_count = 0, ioapic_count = 0, iso_count = 0, nmi_count = 0;

    while (off + 2 <= madt->hdr.length) {
        const u8 *entry = base + off;
        u8 type = entry[0];
        u8 len = entry[1];
        if (len < 2 || off + len > madt->hdr.length) break;

        if (type == 0) lapic_count++;
        else if (type == 1) ioapic_count++;
        else if (type == 2) iso_count++;
        else if (type == 3 || type == 4) nmi_count++;

        off += len;
    }

    log_hex("[acpi]   LAPIC entries=", lapic_count);
    log_hex("[acpi]   IOAPIC entries=", ioapic_count);
    log_hex("[acpi]   ISO entries=", iso_count);
    log_hex("[acpi]   NMI entries=", nmi_count);
}

/* ---------------------------------------------------------------
 * Table enumeration
 * --------------------------------------------------------------- */
static void acpi_power_scan_fadt(const acpi_sdt_header *hdr);

static void acpi_enumerate_rsdt(const acpi_sdt_header *rsdt, u32 entry_size) {
    u32 entry_count = (rsdt->length - sizeof(acpi_sdt_header)) / entry_size;
    const u8 *entries = (const u8 *)rsdt + sizeof(acpi_sdt_header);

    g_log->info("[acpi] tables found");
    for (u32 i = 0; i < entry_count; i++) {
        u64 addr;
        if (entry_size == 4) {
            addr = (u64)((const u32 *)entries)[i];
        } else {
            addr = ((const u64 *)entries)[i];
        }
        if (addr == 0) continue;

        const acpi_sdt_header *hdr = (const acpi_sdt_header *)(uintptr_t)acpi_map(addr);
        char sig[5];
        sig[0] = hdr->signature[0];
        sig[1] = hdr->signature[1];
        sig[2] = hdr->signature[2];
        sig[3] = hdr->signature[3];
        sig[4] = 0;

        g_log->info("[acpi]   ");
        g_log->info(sig);
        log_hex("[acpi]   addr=", addr);
        log_hex("[acpi]   len =", (u64)hdr->length);

        /* 对关键表做详细解析 */
        if (sig[0]=='A' && sig[1]=='P' && sig[2]=='I' && sig[3]=='C') {
            acpi_parse_madt(hdr);
        } else if (sig[0]=='M' && sig[1]=='C' && sig[2]=='F' && sig[3]=='G') {
            acpi_parse_mcfg(hdr);
        } else if (sig[0]=='H' && sig[1]=='P' && sig[2]=='E' && sig[3]=='T') {
            acpi_parse_hpet(hdr);
        } else if (sig[0]=='F' && sig[1]=='A' && sig[2]=='C' && sig[3]=='P') {
            acpi_power_scan_fadt(hdr);
        }
    }
}

/* ================================================================
 *  电源：FADT / DSDT `_S5_` → S5 关机 + ACPI 复位
 *
 *  母本 SYSTEMCOPY/OpenXJ380/driver/power.cpp（Apache-2.0）。
 *  这里补上 DSDT 的 HHDM 换算与日志化，语义保持一致：
 *    - `_S5_` 包缺失 → 不启用 S5，关机退回虚拟机端口兜底；
 *    - 复位优先 FADT reset_reg (ACPI 2.0+)，否则键盘控制器/CF9/三击。
 * ================================================================ */

/* AML 整数项解码（ZeroOp/OneOp/ByteOp/WordOp/DWordOp/QWordOp）。 */
static int acpi_aml_read_integer(const u8 **cursor, const u8 *end, u8 *value) {
    if (*cursor >= end) return 0;
    const u8 opcode = *(*cursor)++;
    switch (opcode) {
    case 0x00: *value = 0; return 1;                       /* ZeroOp */
    case 0x01: *value = 1; return 1;                       /* OneOp */
    case 0x0A:                                             /* BytePrefix */
        if (*cursor >= end) return 0;
        *value = *(*cursor)++;
        return 1;
    case 0x0B:                                             /* WordPrefix */
        if (end - *cursor < 2) return 0;
        *value = (*cursor)[0];
        *cursor += 2;
        return 1;
    case 0x0C:                                             /* DWordPrefix */
    case 0x0E:                                             /* QWordPrefix */
        if (*cursor >= end) return 0;
        *value = (*cursor)[0];
        *cursor += (opcode == 0x0C) ? 4 : 8;
        return 1;
    default:
        return 0;
    }
}

/* 在 DSDT AML 中查找 `_S5_` 包并取出 SLP_TYPa/SLP_TYPb（各左移 10 位）。 */
static int acpi_get_s5_sleep_types(const acpi_fadt *fadt, u16 *slp_typa, u16 *slp_typb) {
    if (!fadt || !slp_typa || !slp_typb) return 0;

    const u64 dsdt_phys = fadt->x_dsdt ? fadt->x_dsdt : fadt->dsdt;
    if (!dsdt_phys) return 0;

    const acpi_sdt_header *dsdt = (const acpi_sdt_header *)(uintptr_t)acpi_map(dsdt_phys);
    if (!dsdt || memneq(dsdt->signature, "DSDT", 4) != 0 ||
        dsdt->length <= sizeof(acpi_sdt_header)) {
        return 0;
    }

    const u8 *aml = (const u8 *)(dsdt + 1);
    const u8 *end = ((const u8 *)dsdt) + dsdt->length;

    for (const u8 *cursor = aml; cursor + 4 < end; ++cursor) {
        if (memneq((const char *)cursor, "_S5_", 4) != 0) continue;
        /* 只认 NameOp 定义的 `_S5_`（08 5F 53 35 5F 或 08 5C 5F 53 35 5F） */
        if (!((cursor > aml && cursor[-1] == 0x08) ||
              (cursor > aml + 1 && cursor[-2] == 0x08 && cursor[-1] == '\\'))) {
            continue;
        }

        const u8 *pkg = cursor + 4;
        if (pkg >= end || *pkg != 0x12) continue;            /* PackageOp */
        ++pkg;
        if (pkg >= end) continue;
        pkg += 1 + ((*pkg >> 6) & 0x3);                      /* 跳过 PkgLength */
        if (pkg >= end) continue;
        ++pkg;                                               /* NumElements */

        u8 a = 0, b = 0;
        if (!acpi_aml_read_integer(&pkg, end, &a)) continue;
        if (!acpi_aml_read_integer(&pkg, end, &b)) continue;

        *slp_typa = ((u16)a) << 10;
        *slp_typb = ((u16)b) << 10;
        return 1;
    }
    return 0;
}

static void acpi_write_pm_control(u32 port, u8 width, u16 value) {
    if (port == 0 || width == 0) return;
    if (width >= 2) dkm_outw((u16)port, value);
    else dkm_outb((u16)port, (u8)value);
}

static void acpi_write_gas_u8(const acpi_generic_address *gas, u8 value) {
    if (!gas || gas->address == 0) return;

    u8 access = gas->access_size;
    if (access == 0) {
        if (gas->bit_width <= 8) access = 1;
        else if (gas->bit_width <= 16) access = 2;
        else access = 3;
    }

    if (gas->space_id == ACPI_GAS_SYSTEM_IO) {
        if (access <= 1) dkm_outb((u16)gas->address, value);
        else if (access == 2) dkm_outw((u16)gas->address, (u16)value);
        else __asm__ volatile ("outl %0, %1" :: "a"((u32)value), "Nd"((u16)gas->address));
        return;
    }
    if (gas->space_id == ACPI_GAS_SYSTEM_MEM) {
        u64 va = acpi_map(gas->address);
        if (!va) return;
        if (access <= 1) *(volatile u8 *)(uintptr_t)va = value;
        else if (access == 2) *(volatile u16 *)(uintptr_t)va = (u16)value;
        else *(volatile u32 *)(uintptr_t)va = (u32)value;
    }
}

/* FACP 表出现时调用：保存 FADT 并解析 `_S5_`。 */
static void acpi_power_scan_fadt(const acpi_sdt_header *hdr) {
    if (!hdr || hdr->length < sizeof(acpi_fadt)) {
        g_log->warn("[acpi] FADT too short; S5 unavailable");
        return;
    }
    g_fadt = (const acpi_fadt *)hdr;

    log_hex("[acpi] FADT pm1a_cnt=", g_fadt->pm1a_cnt_blk);
    log_hex("[acpi] FADT pm1b_cnt=", g_fadt->pm1b_cnt_blk);
    log_hex("[acpi] FADT pm1_cnt_len=", g_fadt->pm1_cnt_len);

    if (acpi_get_s5_sleep_types(g_fadt, &g_slp_typa, &g_slp_typb)) {
        g_power_ready = 1;
        log_hex("[acpi] S5 SLP_TYPa=", g_slp_typa);
        log_hex("[acpi] S5 SLP_TYPb=", g_slp_typb);
        g_log->info("[acpi] ACPI S5 poweroff available");
    } else {
        g_log->warn("[acpi] _S5_ not found; poweroff falls back to VM ports");
    }
}

/* ---- 服务：srv.acpi.poweroff ---- 成功不返回；失败返回 -1 供调用方兜底 ---- */
static int acpi_poweroff(void) {
    /* 虚拟机端口兜底（无论 ACPI 是否可用都先试，因为这些端口无副作用） */
    if (!g_power_ready || !g_fadt) {
        g_log->warn("[acpi] S5 unavailable; trying VM poweroff ports");
        dkm_outw(0x604, 0x2000);     /* QEMU PIIX4/AHCI PM1a */
        dkm_outw(0xB004, 0x2000);    /* QEMU q35 ACPI PM1a */
        dkm_outw(0x4004, 0x3400);    /* VirtualBox / VMware */
        return -1;
    }

    if (g_fadt->smi_cmd != 0 && g_fadt->acpi_enable != 0) {
        dkm_outb((u16)g_fadt->smi_cmd, g_fadt->acpi_enable);
        for (volatile int i = 0; i < 100000; i++);
    }

    g_log->info("[acpi] entering S5");
    acpi_write_pm_control(g_fadt->pm1a_cnt_blk, g_fadt->pm1_cnt_len,
                          (u16)(g_slp_typa | ACPI_SLP_EN));
    if (g_fadt->pm1b_cnt_blk != 0) {
        acpi_write_pm_control(g_fadt->pm1b_cnt_blk, g_fadt->pm1_cnt_len,
                              (u16)(g_slp_typb | ACPI_SLP_EN));
    }

    /* 走到这里说明 S5 没生效（或固件不允许）。退回 VM 端口后停住。 */
    dkm_outw(0x604, 0x2000);
    dkm_outw(0xB004, 0x2000);
    dkm_outw(0x4004, 0x3400);
    for (;;) __asm__ volatile ("hlt");
    return 0;
}

/* ---- 服务：srv.acpi.reboot ---- 成功不返回；失败返回 -1 ---- */
static int acpi_reboot(void) {
    if (g_fadt && g_fadt->reset_reg.address != 0) {
        log_hex("[acpi] reset_reg=", g_fadt->reset_reg.address);
        log_hex("[acpi] reset_value=", g_fadt->reset_value);
        if (g_fadt->smi_cmd != 0 && g_fadt->acpi_enable != 0) {
            dkm_outb((u16)g_fadt->smi_cmd, g_fadt->acpi_enable);
            for (volatile int i = 0; i < 100000; i++);
        }
        acpi_write_gas_u8(&g_fadt->reset_reg, g_fadt->reset_value);
        for (volatile int i = 0; i < 1000000; i++);
    }

    /* 键盘控制器脉冲复位 */
    for (int i = 0; i < 100000; ++i) {
        if ((dkm_inb(0x64) & 0x02) == 0) break;
        __asm__ volatile ("pause");
    }
    dkm_outb(0x64, 0xFE);

    /* 芯片组复位控制端口 */
    dkm_outb(0xCF9, 0x02);
    for (volatile int i = 0; i < 100000; i++);
    dkm_outb(0xCF9, 0x06);

    /* 最后手段：空 IDT + int3 触发三击复位 */
    {
        struct __attribute__((packed)) { u16 limit; u64 base; } null_idtr = { 0, 0 };
        __asm__ volatile ("lidt %0" :: "m"(null_idtr));
        __asm__ volatile ("int3");
    }
    for (;;) __asm__ volatile ("hlt");
    return -1;
}

/* ---------------------------------------------------------------
 * Driver entry points
 * --------------------------------------------------------------- */
__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;

    if (!api || !api->log) {
        return -1;
    }
    g_log = api->log;
    g_api = api;

    g_log->info("[acpi] scanning for RSDP");

    const acpi_rsdp_v2 *rsdp = acpi_find_rsdp(api->rsdp_address);
    if (!rsdp) {
        g_log->error("[acpi] RSDP not found");
        return 0;  /* optional driver, don't fail boot */
    }

    g_log->info("[acpi] RSDP found");
    log_hex("[acpi] revision =", (u64)rsdp->revision);
    g_log->info("[acpi] oem_id");
    {
        char oem[7];
        for (int i = 0; i < 6; i++) oem[i] = rsdp->oem_id[i];
        oem[6] = 0;
        g_log->info(oem);
    }

    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        g_log->info("[acpi] using XSDT");
        const acpi_sdt_header *xsdt = (const acpi_sdt_header *)(uintptr_t)acpi_map(rsdp->xsdt_addr);
        acpi_enumerate_rsdt(xsdt, 8);
    } else if (rsdp->rsdt_addr) {
        g_log->info("[acpi] using RSDT");
        const acpi_sdt_header *rsdt = (const acpi_sdt_header *)(uintptr_t)acpi_map(rsdp->rsdt_addr);
        acpi_enumerate_rsdt(rsdt, 4);
    } else {
        g_log->warn("[acpi] no RSDT/XSDT address");
    }

    /* 电源服务：桌面电源钮 / shell poweroff / SESSION 退出共用。 */
    if (api->bus && api->bus->register_service) {
        api->bus->register_service("srv.acpi.poweroff", 1, (void *)acpi_poweroff);
        api->bus->register_service("srv.acpi.reboot", 1, (void *)acpi_reboot);
        g_log->info("[acpi] srv.acpi.poweroff / srv.acpi.reboot registered");
    } else {
        g_log->warn("[acpi] bus unavailable; power services not registered");
    }

    g_log->info("[acpi] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
