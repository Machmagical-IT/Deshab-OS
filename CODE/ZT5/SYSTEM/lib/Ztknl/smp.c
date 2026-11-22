/*
 * Zratro OS - SMP Implementation
 */

#include "smp.h"
#include "apic.h"
#include "pmm.h"
#include "vmm.h"
#include "console.h"
#include "string.h"
#include "gdt.h"
#include "idt.h"
#include "cpu.h"
#include "heap.h"

/* CPU 数据 */
__attribute__((aligned(4096))) struct cpu_local cpu_data[MAX_CPUS];
int smp_cpu_count = 1;

/* AP 启动信息 (低地址，16位可访问)
 * 物理地址 0x5000 用于 AP trampoline (实模式可访问)
 * BSP 内核需要通过 HHDM 虚拟地址访问
 */
#define SMP_BOOT_INFO_PHYS  0x5000
static volatile struct smp_boot_info *boot_info = NULL;  /* 初始化时设置 */

/* AP 启动入口声明 */
extern void ap_start(void);

/* ACPI MADT 解析相关 */
struct madt_entry_header {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct madt_processor {
    struct madt_entry_header header;
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed));

struct madt_ioapic {
    struct madt_entry_header header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t address;
    uint32_t gsi_base;
} __attribute__((packed));

struct madt_interrupt_override {
    struct madt_entry_header header;
    uint8_t bus;        /* 0 = ISA */
    uint8_t source_irq; /* ISA IRQ number */
    uint32_t gsi;       /* Global System Interrupt */
    uint16_t flags;     /* Polarity and trigger mode */
} __attribute__((packed));

/* APIC 列表 */
static uint32_t apic_ids[MAX_CPUS];
static int apic_count = 0;

/* ISA IRQ to GSI remapping (from ACPI Interrupt Source Override) */
static uint32_t irq_to_gsi[16] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

/* Get GSI for an ISA IRQ, accounting for ACPI overrides */
uint32_t irq_get_gsi(uint8_t isa_irq) {
    if (isa_irq < 16) return irq_to_gsi[isa_irq];
    return isa_irq;
}

/* IPI 调用数据 */
static struct smp_call_data call_data[MAX_CPUS];
static volatile int call_data_lock = 0;

/* ========== ACPI MADT 解析 ========== */

/* RSDP 搜索区域 - 使用静态常量避免栈破坏问题 */
static const uint64_t rsdp_search_areas[2] = {
    0x000E0000ULL,  /* BIOS ROM 区域起始 */
    0x000F0000ULL   /* BIOS ROM 区域后半 */
};

/* 确保 BIOS ROM 区域映射到 HHDM */
static void map_bios_region(void) {
    /* BIOS ROM 区域: 0xE0000 - 0x100000 (128KB) */
    /* Limine HHDM 可能没有映射这个保留区域 */
    
    extern uint64_t hhdm_offset;
    struct address_space *space = vmm_get_kernel_space();
    
    if (space == NULL) {
        // println("  [SMP-ERROR] Kernel address space is NULL!");
        return;
    }
    
    /* 检查 0xE0000 是否已映射 */
    uint64_t test_virt = 0xE0000 + hhdm_offset;
    uint64_t phys = vmm_get_physical(space, test_virt);
    
    if (phys != 0) {
        /* 已映射，无需操作 */
        // print("  [SMP-DEBUG] BIOS ROM region already mapped at 0x");
        // print_hex(test_virt);
        // println("");
        return;
    }
    
    // println("  [SMP-DEBUG] BIOS ROM region not mapped, mapping now...");
    
    /* 映射 BIOS ROM 区域 (0xE0000 - 0x100000) */
    /* 使用 4 个 4KB 页面覆盖主要区域 */
    uint64_t bios_start = 0xE0000;
    uint64_t bios_pages = 32;  /* 32 pages = 128KB */
    
    /* 检查映射是否成功 */
    int mapped = 0;
    for (uint64_t i = 0; i < bios_pages; i++) {
        uint64_t phys_addr = bios_start + i * PAGE_SIZE;
        uint64_t virt_addr = phys_addr + hhdm_offset;
        
        /* 使用 MMIO 标志：禁用缓存，因为 BIOS ROM 是 ROM */
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITETHROUGH | VMM_FLAG_CACHE_DISABLE;
        
        if (vmm_map_page(space, virt_addr, phys_addr, flags) == 0) {
            mapped++;
        }
    }
    
    // print("  [SMP-DEBUG] Mapped ");
    // print_dec(mapped);
    // print("/");
    // print_dec(bios_pages);
    // println(" BIOS ROM pages");
}

/* 简化的 RSDP 查找 */
static uint8_t *find_rsdp(void) {
    /* 获取 HHDM 偏移 */
    extern uint64_t hhdm_offset;
    
    // println("");
    // println("  [SMP-DEBUG] ========== find_rsdp START ==========");
    // print("  [SMP-DEBUG] HHDM offset = 0x");
    // print_hex(hhdm_offset);
    // println("");
    
    /* 验证 HHDM 偏移有效性 */
    if (hhdm_offset == 0) {
        // println("  [SMP-ERROR] HHDM offset is ZERO! Cannot continue!");
        // println("  [SMP-ERROR] This means pmm_init() was not called or failed!");
        return NULL;
    }
    
    if (hhdm_offset < 0xFFFF800000000000ULL) {
        // println("  [SMP-ERROR] HHDM offset too low! Invalid value!");
        // print("  [SMP-ERROR] Expected >= 0xFFFF800000000000, got 0x");
        // print_hex(hhdm_offset);
        // println("");
        return NULL;
    }
    
    /* 在访问 BIOS ROM 之前，确保该区域已映射 */
    map_bios_region();
    
    /* 验证搜索区域数组完整性 */
    // print("  [SMP-DEBUG] rsdp_search_areas array at 0x");
    // print_hex((uint64_t)rsdp_search_areas);
    // println("");
    // print("  [SMP-DEBUG] rsdp_search_areas[0] = 0x");
    // print_hex(rsdp_search_areas[0]);
    // println(" (expected 0xE0000)");
    // print("  [SMP-DEBUG] rsdp_search_areas[1] = 0x");
    // print_hex(rsdp_search_areas[1]);
    // println(" (expected 0xF0000)");
    
    /* 检查数组值是否被破坏 */
    if (rsdp_search_areas[0] != 0x000E0000ULL || rsdp_search_areas[1] != 0x000F0000ULL) {
        // println("  [SMP-ERROR] rsdp_search_areas array corrupted!");
        // println("  [SMP-ERROR] This indicates a serious memory corruption issue!");
        return NULL;
    }
    
    for (int i = 0; i < 2; i++) {
        /* 转换物理地址到虚拟地址 */
        uint64_t phys_addr = rsdp_search_areas[i];
        uint64_t virt_addr = phys_addr + hhdm_offset;
        
        // print("  [SMP-DEBUG] Area ");
        // print_dec(i);
        // print(": phys 0x");
        // print_hex(phys_addr);
        // println("");
        
        // print("  [SMP-DEBUG]      + hhdm_offset 0x");
        // print_hex(hhdm_offset);
        // println("");
        
        // print("  [SMP-DEBUG]      = virt 0x");
        // print_hex(virt_addr);
        // println("");
        
        uint8_t *p = (uint8_t *)virt_addr;
        
        /* 安全检查：确保虚拟地址在有效范围内 */
        if (virt_addr < 0xFFFF800000000000ULL) {
            // println("  [SMP-WARN] Virtual address too low, skipping");
            continue;
        }
        
        // print("  [SMP-DEBUG]      Reading from virt address...");
        
        /* 尝试安全读取第一个字节 */
        /* 注意：如果这里 Page Fault，说明 HHDM 映射有问题 */
        volatile uint8_t test_byte = p[0];
        // print(" first byte = 0x");
        // print_hex(test_byte);
        // println(" OK");
        
        for (int j = 0; j < 0x10000; j += 16) {
            if (p[j] == 'R' && p[j+1] == 'S' && p[j+2] == 'D' && p[j+3] == ' ' &&
                p[j+4] == 'P' && p[j+5] == 'T' && p[j+6] == 'R') {
                // print("  [SMP-DEBUG] RSDP found at virt 0x");
                // print_hex((uint64_t)&p[j]);
                // println("");
                return &p[j];
            }
        }
    }
    // println("  [SMP-WARN] RSDP not found in standard areas");
    return NULL;
}

/* 简化的 ACPI 表查找 */
static void *find_acpi_table(const char *signature) {
    // print("  [SMP-DEBUG] find_acpi_table('");
    // print(signature);
    // print("')...");
    
    uint8_t *rsdp = find_rsdp();
    if (rsdp == NULL) {
        // println(" RSDP not found");
        return NULL;
    }
    
    /* 获取 RSDT/XSDT 地址 */
    uint64_t rsdt_addr;
    int is_xsdt = 0;
    
    /* 检查 ACPI 版本 (Revision 字段在偏移 15) */
    if (rsdp[15] >= 2) {
        /* ACPI 2.0+: XSDT */
        rsdt_addr = *(uint64_t *)(rsdp + 24);
        is_xsdt = 1;
        // print("  [SMP-DEBUG] ACPI 2.0+ XSDT at phys 0x");
    } else {
        /* ACPI 1.0: RSDT */
        rsdt_addr = *(uint32_t *)(rsdp + 16);
        // print("  [SMP-DEBUG] ACPI 1.0 RSDT at phys 0x");
    }
    // print_hex(rsdt_addr);
    // println("");
    
    /* 确保 RSDT/XSDT 页面已映射 */
    extern uint64_t hhdm_offset;
    struct address_space *kernel_space = vmm_get_kernel_space();
    if (kernel_space != NULL) {
        uint64_t rsdt_page = rsdt_addr & ~(4096 - 1);
        uint64_t rsdt_virt_page = rsdt_page + hhdm_offset;
        
        /* 检查页面是否已映射 */
        if (vmm_get_physical(kernel_space, rsdt_virt_page) == 0) {
            /* 映射 RSDT/XSDT 页面 */
            if (vmm_map_page(kernel_space, rsdt_virt_page, rsdt_page, 
                           VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER) != 0) {
                // println("  [SMP-ERROR] Failed to map RSDT/XSDT page!");
                return NULL;
            }
        }
    }
    
    /* 转换 RSDT/XSDT 物理地址到虚拟地址 */
    uint64_t rsdt_virt = rsdt_addr + hhdm_offset;
    // print("  [SMP-DEBUG] RSDT/XSDT virt addr: 0x");
    // print_hex(rsdt_virt);
    // println("");
    
    uint8_t *rsdt = (uint8_t *)rsdt_virt;
    if (rsdt == NULL) {
        // println("  [SMP-ERROR] rsdt is NULL");
        return NULL;
    }
    
    /* 检查 RSDT/XSDT 签名 */
    if (rsdt[0] != 'R' || rsdt[1] != 'S' || rsdt[2] != 'D' || rsdt[3] != 'T') {
        /* 可能是 XSDT，签名是 XSDT */
        if (rsdt[0] != 'X' || rsdt[1] != 'S' || rsdt[2] != 'D' || rsdt[3] != 'T') {
            // println("  [SMP-ERROR] Invalid RSDT/XSDT signature");
            return NULL;
        }
    }
    
    /* 遍历 RSDT/XSDT 条目 */
    int entry_size = is_xsdt ? 8 : 4;
    int entry_count = (*(uint32_t *)(rsdt + 4) - 36) / entry_size;
    
    // print("  [SMP-DEBUG] RSDT/XSDT has ");
    // print_dec(entry_count);
    // println(" entries");
    
    for (int i = 0; i < entry_count; i++) {
        uint64_t table_phys;
        if (entry_size == 8) {
            table_phys = *(uint64_t *)(rsdt + 36 + i * 8);
        } else {
            table_phys = *(uint32_t *)(rsdt + 36 + i * 4);
        }
        
        /* 确保 ACPI 表页面已映射 */
        if (kernel_space != NULL) {
            uint64_t table_page = table_phys & ~(4096 - 1);
            uint64_t table_virt_page = table_page + hhdm_offset;
            
            /* 检查页面是否已映射 */
            if (vmm_get_physical(kernel_space, table_virt_page) == 0) {
                /* 映射 ACPI 表页面 */
                if (vmm_map_page(kernel_space, table_virt_page, table_page, 
                               VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER) != 0) {
                    continue; /* 跳过无法映射的条目 */
                }
            }
        }
        
        /* 转换 ACPI 表物理地址到虚拟地址 */
        uint64_t table_virt = table_phys + hhdm_offset;
        uint8_t *table = (uint8_t *)table_virt;
        
        /* 检查签名匹配 */
        if (table[0] == signature[0] && table[1] == signature[1] &&
            table[2] == signature[2] && table[3] == signature[3]) {
            // print("  [SMP-DEBUG] Found '");
            // print(signature);
            // print("' at phys 0x");
            // print_hex(table_phys);
            // print(" virt 0x");
            // print_hex(table_virt);
            // println("");
            return table;
        }
    }
    
    // print("  [SMP-WARN] Table '");
    // print(signature);
    // println("' not found");
    return NULL;
}

/* 解析 MADT 获取 CPU 列表 */
static int parse_madt(void) {
    uint8_t *madt = (uint8_t *)find_acpi_table("APIC");
    if (madt == NULL) {
        apic_ids[0] = apic_get_id();
        apic_count = 1;
        return 0;
    }
    
    /* MADT 头部: 44 字节 */
    int length = *(uint32_t *)(madt + 4);
    int offset = 44;
    
    while (offset < length) {
        struct madt_entry_header *entry = (struct madt_entry_header *)(madt + offset);
        if (entry->length == 0) break;
        
        if (entry->type == 0) {
            /* Processor Local APIC */
            if (apic_count < MAX_CPUS) {
                struct madt_processor *proc = (struct madt_processor *)entry;
                if (proc->flags & 1) {
                    apic_ids[apic_count++] = proc->apic_id;
                }
            }
        } else if (entry->type == 2) {
            /* Interrupt Source Override */
            struct madt_interrupt_override *iso = (struct madt_interrupt_override *)entry;
            if (iso->bus == 0 && iso->source_irq < 16) {
                irq_to_gsi[iso->source_irq] = iso->gsi;
                printk("[ACPI] IRQ override: ISA IRQ %d -> GSI %d\n", iso->source_irq, iso->gsi);
            }
        }
        
        offset += entry->length;
    }
    
    return apic_count;
}

/* ========== 延迟函数 ========== */

static void delay_us(uint64_t us) {
    uint64_t tsc_start, tsc_end;
    uint64_t tsc_freq = cpu_get_tsc_frequency();
    if (tsc_freq == 0) tsc_freq = 2000000000ULL;  /* 默认 2GHz */
    
    asm volatile("rdtsc" : "=A"(tsc_start));
    tsc_end = tsc_start + (us * tsc_freq / 1000000);
    
    while (1) {
        uint64_t tsc_now;
        asm volatile("rdtsc" : "=A"(tsc_now));
        if (tsc_now >= tsc_end) break;
    }
}

static void delay_ms(uint64_t ms) {
    delay_us(ms * 1000);
}

/* ========== AP 启动代码复制 ========== */

/* 将 AP 启动代码复制到低地址 */
static void copy_trampoline(void) {
    /* AP 启动代码需要放在 16 位可访问的地址 */
    /* 简化实现：使用预设的启动代码 */
    
    /* 在 0x5000 处设置简单的跳转代码 */
    extern uint8_t ap_trampoline_start[];
    extern uint8_t ap_trampoline_end[];
    
    /* 如果有汇编启动代码，复制它 */
    /* 这里我们使用 C 函数地址作为入口 */
}

/* ========== BSP 初始化 ========== */

int smp_init(void) {
    // println("");
    // println("  [SMP-DEBUG] ===== SMP Initialization Start =====");
    // println("[SMP] Initializing multi-processor support...");
    
    /* ===== 步骤 0: 验证 HHDM 偏移 ===== */
    // println("  [SMP-DEBUG] --- Step 0: Verify HHDM offset ---");
    extern uint64_t hhdm_offset;
    
    // print("  [SMP-DEBUG] hhdm_offset = 0x");
    // print_hex(hhdm_offset);
    // println("");
    
    if (hhdm_offset == 0) {
        // println("  [SMP-ERROR] hhdm_offset is ZERO!");
        // println("  [SMP-ERROR] pmm_init() may not have been called properly.");
        // println("  [SMP-WARN] Continuing with single CPU mode.");
        apic_ids[0] = apic_get_id();
        apic_count = 1;
        return 1;
    }
    
    if (hhdm_offset < 0xFFFF800000000000ULL) {
        // println("  [SMP-ERROR] hhdm_offset is INVALID (too low)!");
        // println("  [SMP-WARN] Continuing with single CPU mode.");
        apic_ids[0] = apic_get_id();
        apic_count = 1;
        return 1;
    }
    
    // println("  [SMP-DEBUG] HHDM offset is valid (OK)");
    
    /* ===== 步骤 1: 验证 cpu_data 数组 ===== */
    // println("  [SMP-DEBUG] --- Step 1: Verify cpu_data array ---");
    
    // print("  [SMP-DEBUG] cpu_data array address: 0x");
    // print_hex((uint64_t)cpu_data);
    // println("");
    
    // print("  [SMP-DEBUG] cpu_data[0] address: 0x");
    // print_hex((uint64_t)&cpu_data[0]);
    // println("");
    
    // print("  [SMP-DEBUG] sizeof(cpu_local): ");
    // print_dec(sizeof(struct cpu_local));
    // println(" bytes");
    
    // print("  [SMP-DEBUG] MAX_CPUS: ");
    // print_dec(MAX_CPUS);
    // println("");
    
    // print("  [SMP-DEBUG] cpu_data array total size: ");
    // print_dec(sizeof(cpu_data));
    // println(" bytes");
    
    /* 验证 cpu_data 在 Higher Half 区域 */
    if ((uint64_t)cpu_data < 0xFFFF800000000000ULL) {
        // print("  [SMP-WARN] cpu_data is NOT in Higher Half! Address: 0x");
        // print_hex((uint64_t)cpu_data);
        // println("");
    } else {
        // print("  [SMP-DEBUG] cpu_data is in Higher Half region (OK)");
        // println("");
    }
    
    /* ===== 步骤 2: 初始化 CPU 数据 ===== */
    // println("  [SMP-DEBUG] --- Step 2: Initialize cpu_data array ---");
    
    /* 使用 TSC 初始化栈 canary */
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    uint64_t canary = tsc ^ (tsc >> 32) ^ 0xDEADBEEFCAFEBABEULL;
    
    // print("  [SMP-DEBUG] Initializing cpu_data[0]...");
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_data[i].cpu_id = i;
        cpu_data[i].apic_id = 0;
        cpu_data[i].state = CPU_STATE_OFFLINE;
        /* 每个 CPU 使用不同的 canary (基于 CPU ID 和 TSC) */
        cpu_data[i].stack_canary = canary ^ ((uint64_t)i * 0x9E3779B97F4A7C15ULL);
        cpu_data[i].current_thread = NULL;
        cpu_data[i].idle_thread = NULL;
        cpu_data[i].schedule_count = 0;
        cpu_data[i].irq_count = 0;
        cpu_data[i].softirq_pending = 0;
        cpu_data[i].interrupt_depth = 0;
        cpu_data[i].spinlock_count = 0;
        cpu_data[i].preempt_count = 0;
        cpu_data[i].context_switches = 0;
        cpu_data[i].syscalls = 0;
        cpu_data[i].page_faults = 0;
    }
    
    /* 重新获取当前的 canary 值（由 kernel_main 设置）并更新 BSP 的 canary */
    uint64_t current_canary;
    asm volatile("mov %%gs:0x28, %0" : "=r"(current_canary));
    cpu_data[0].stack_canary = current_canary;
    
    /* 同步全局 __stack_chk_guard 与 cpu_data[0] 的 canary
     * 这确保 -fstack-protector-strong 使用的 canary 值与 GS:0x28 一致 */
    extern uint64_t __stack_chk_guard;
    __stack_chk_guard = current_canary;
    
    // print("  [SMP-DEBUG] BSP stack_canary (preserved): 0x");
    // print_hex(cpu_data[0].stack_canary);
    // println("");
    // println(" done");
    
    /* 验证写入是否成功 */
    // print("  [SMP-DEBUG] Verifying cpu_data[0] write...");
    if (cpu_data[0].cpu_id == 0) {
        // print(" OK (cpu_id=");
        // print_dec(cpu_data[0].cpu_id);
        // print(")");
    } else {
        // print(" FAILED (cpu_id=");
        // print_dec(cpu_data[0].cpu_id);
        // print(", expected 0)");
    }
    // println("");
    
    /* ===== 步骤 3: 初始化 boot_info ===== */
    // println("  [SMP-DEBUG] --- Step 3: Initialize boot_info ---");
    
    // print("  [SMP-DEBUG] SMP_BOOT_INFO_PHYS = 0x");
    // print_hex(SMP_BOOT_INFO_PHYS);
    // println(" (physical address, 16-bit accessible)");
    
    /* 转换为虚拟地址 */
    boot_info = (volatile struct smp_boot_info *)phys_to_virt(SMP_BOOT_INFO_PHYS);
    
    // print("  [SMP-DEBUG] boot_info virtual address: 0x");
    // print_hex((uint64_t)boot_info);
    // println("");
    
    /* 验证 boot_info 可访问 */
    // print("  [SMP-DEBUG] Verifying boot_info accessibility...");
    if (boot_info == NULL) {
        // println(" FAILED - NULL pointer!");
        for (;;) asm volatile("hlt");
    }
    
    /* 测试读写 */
    boot_info->ready = 0;
    boot_info->error = 0;
    boot_info->cpu_id = 0xFF;
    if (boot_info->cpu_id == 0xFF) {
        // print(" OK (read/write test passed)");
    } else {
        // print(" FAILED (write test failed)");
    }
    // println("");
    boot_info->cpu_id = 0;  /* 重置 */
    
    /* ===== 步骤 4: 解析 MADT ===== */
    // println("  [SMP-DEBUG] --- Step 4: Parse MADT ---");
    
    // print("  [SMP-DEBUG] Calling parse_madt()...");
    parse_madt();
    // println(" done");
    
    // print("  [SMP-DEBUG] CPUs detected: ");
    // print_dec(apic_count);
    // println("");
    
    /* 显示检测到的 APIC ID */
    for (int i = 0; i < apic_count && i < 4; i++) {
        // print("  [SMP-DEBUG]   APIC ID[");
        // print_dec(i);
        // print("]: ");
        // print_dec(apic_ids[i]);
        // println("");
    }
    
    /* ===== 步骤 5: 设置 BSP 数据 ===== */
    // println("  [SMP-DEBUG] --- Step 5: Setup BSP data ---");
    
    cpu_data[0].apic_id = apic_get_id();
    cpu_data[0].state = CPU_STATE_RUNNING;
    
    // print("  [SMP-DEBUG] BSP APIC ID: ");
    // print_dec(cpu_data[0].apic_id);
    // println("");
    
    // print("  [SMP-DEBUG] BSP state: RUNNING (");
    // print_dec(CPU_STATE_RUNNING);
    // println(")");
    
    /* ===== 步骤 6: 设置 GS 段基址 ===== */
    // println("  [SMP-DEBUG] --- Step 6: Set GS base register ---");
    
    // print("  [SMP-DEBUG] Target GS base: 0x");
    // print_hex((uint64_t)&cpu_data[0]);
    // println("");
    
    /* 读取当前 GS base */
    uint64_t old_gs_base;
    uint32_t low, high;
    asm volatile("mov $0xC0000101, %%ecx; rdmsr" : "=a"(low), "=d"(high));
    old_gs_base = ((uint64_t)high << 32) | low;
    
    // print("  [SMP-DEBUG] Current GS base (before): 0x");
    // print_hex(old_gs_base);
    // println("");
    
    /* 设置新的 GS base */
    // print("  [SMP-DEBUG] Calling set_cpu_local()...");
    set_cpu_local(&cpu_data[0]);
    // println(" done");
    
    /* 验证 GS base 是否设置成功 */
    uint64_t new_gs_base;
    asm volatile("mov $0xC0000101, %%ecx; rdmsr" : "=a"(low), "=d"(high));
    new_gs_base = ((uint64_t)high << 32) | low;
    
    // print("  [SMP-DEBUG] New GS base (after): 0x");
    // print_hex(new_gs_base);
    // println("");
    
    /* 比较期望值和实际值 */
    uint64_t expected_gs = (uint64_t)&cpu_data[0];
    if (new_gs_base != expected_gs) {
        // print("  [SMP-ERROR] GS base mismatch!");
        // print(" Expected: 0x");
        // print_hex(expected_gs);
        // print(" Got: 0x");
        // print_hex(new_gs_base);
        // println("");
        
        // println("  [SMP-ERROR] This will cause this_cpu() to return wrong address!");
        for (;;) asm volatile("hlt");
    } else {
        // print("  [SMP-DEBUG] GS base set correctly! (verified)");
        // println("");
    }
    
    /* ===== 步骤 7: 测试 this_cpu() 宏 ===== */
    // println("  [SMP-DEBUG] --- Step 7: Test this_cpu() macro ---");
    
    // print("  [SMP-DEBUG] Calling this_cpu()...");
    struct cpu_local *cpu_ptr = this_cpu();
    // print(" returned: 0x");
    // print_hex((uint64_t)cpu_ptr);
    // println("");
    
    /* 详细验证 */
    if (cpu_ptr == NULL) {
        // println("  [SMP-ERROR] this_cpu() returned NULL!");
        // println("  [SMP-ERROR] This indicates GS base is not set correctly!");
        for (;;) asm volatile("hlt");
    }
    
    if (cpu_ptr != &cpu_data[0]) {
        // print("  [SMP-ERROR] this_cpu() returned wrong address!");
        // print(" Expected: 0x");
        // print_hex((uint64_t)&cpu_data[0]);
        // print(" Got: 0x");
        // print_hex((uint64_t)cpu_ptr);
        // println("");
        for (;;) asm volatile("hlt");
    }
    
    // print("  [SMP-DEBUG] this_cpu()->cpu_id = ");
    // print_dec(cpu_ptr->cpu_id);
    // println("");
    
    // print("  [SMP-DEBUG] this_cpu()->apic_id = ");
    // print_dec(cpu_ptr->apic_id);
    // println("");
    
    // print("  [SMP-DEBUG] this_cpu()->state = ");
    // print_dec(cpu_ptr->state);
    // println("");
    
    /* 最终验证 */
    if (cpu_ptr->cpu_id != 0 || cpu_ptr->apic_id != cpu_data[0].apic_id) {
        // println("  [SMP-ERROR] this_cpu() data mismatch!");
        for (;;) asm volatile("hlt");
    }
    
    // println("  [SMP-DEBUG] this_cpu() test PASSED!");
    
    /* ===== 完成 ===== */
    // println("  [SMP-DEBUG] --- SMP Init Complete ---");
    
    // print("[SMP] BSP (CPU 0) APIC ID: ");
    // print_dec(cpu_data[0].apic_id);
    // println("");
    
    // println("  [SMP-DEBUG] ===== SMP Initialization Complete =====");
    // println("");
    
    return 0;
}

/* ========== AP 启动 ========== */

int smp_start_cpu(uint32_t apic_id) {
    if (smp_cpu_count >= MAX_CPUS) {
        return -1;
    }
    
    /* 跳过 BSP */
    if (apic_id == cpu_data[0].apic_id) {
        return 0;
    }
    
    int cpu_id = smp_cpu_count;
    
    // print("[SMP] Starting CPU ");
    // print_dec(cpu_id);
    // print(" (APIC ID: ");
    // print_dec(apic_id);
    // println(")...");
    
    /* 设置 CPU 数据 */
    struct cpu_local *cpu = &cpu_data[cpu_id];
    cpu->cpu_id = cpu_id;
    cpu->apic_id = apic_id;
    cpu->state = CPU_STATE_STARTING;
    
    /* 分配内核栈 (64KB) */
    size_t stack_pages = 16;
    uint64_t stack_phys = pmm_alloc_pages(stack_pages);
    if (stack_phys == 0) {
        // println("[SMP] Failed to allocate stack");
        return -1;
    }
    
    /* 映射栈到内核地址空间 */
    static uint64_t next_stack_addr = 0xFFFFFFFF90200000ULL;
    uint64_t stack_virt = next_stack_addr;
    next_stack_addr += stack_pages * 4096;
    
    if (vmm_map_range(vmm_get_kernel_space(), stack_virt, stack_phys, 
                      stack_pages, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE) != 0) {
        pmm_free_pages(stack_phys, stack_pages);
        // println("[SMP] Failed to map stack");
        return -1;
    }
    
    /* 设置启动信息 */
    boot_info->kernel_entry = (uint64_t)ap_start;
    boot_info->pml4 = vmm_get_kernel_space()->cr3;
    boot_info->stack = stack_virt + stack_pages * 4096;  /* 栈顶 */
    boot_info->cpu_local = (uint64_t)cpu;
    boot_info->ready = 0;
    boot_info->error = 0;
    boot_info->cpu_id = cpu_id;
    boot_info->apic_id = apic_id;
    
    /* 发送 INIT IPI */
    apic_send_ipi(apic_id, 0x500, 0x500);  /* INIT */
    delay_ms(10);
    
    /* 发送 STARTUP IPI (两次) */
    apic_send_ipi(apic_id, 0x500, 0x605);  /* STARTUP at 0x5000 */
    delay_ms(1);
    apic_send_ipi(apic_id, 0x500, 0x605);
    delay_ms(1);
    
    /* 等待 AP 就绪 (最多 5 秒) */
    int timeout = 5000;
    while (!boot_info->ready && timeout-- > 0) {
        delay_ms(1);
    }
    
    if (boot_info->ready) {
        cpu->state = CPU_STATE_RUNNING;
        smp_cpu_count++;
        // println("[SMP] CPU started successfully");
        return 0;
    }
    
    // println("[SMP] CPU start timeout");
    cpu->state = CPU_STATE_OFFLINE;
    return -1;
}

int smp_start_aps(void) {
    // println("[SMP] Starting application processors...");
    
    /* 启动所有 AP */
    for (int i = 0; i < apic_count && smp_cpu_count < MAX_CPUS; i++) {
        if (apic_ids[i] != cpu_data[0].apic_id) {
            smp_start_cpu(apic_ids[i]);
        }
    }
    
    // print("[SMP] Total CPUs online: ");
    // print_dec(smp_cpu_count);
    // println("");
    
    return smp_cpu_count;
}

/* ========== IPI 操作 ========== */

void smp_send_ipi(uint32_t apic_id, uint8_t vector) {
    apic_send_ipi(apic_id, vector, 0);
}

void smp_broadcast_ipi(uint8_t vector) {
    /* 发送给所有 CPU (除自己外) */
    apic_send_ipi(0xFFFFFFFF, vector, 0);  /* 广播 */
}

void smp_send_reschedule(int cpu_id) {
    if (cpu_id >= 0 && cpu_id < smp_cpu_count) {
        uint32_t apic_id = cpu_data[cpu_id].apic_id;
        if (apic_id != cpu_data[0].apic_id) {
            smp_send_ipi(apic_id, IPI_RESCHEDULE);
        }
    }
}

/* ========== 函数调用 ========== */

/* IPI 重新调度处理程序 */
void smp_reschedule_handler(struct interrupt_frame *frame) {
    (void)frame;
    
    /* 设置需要调度标志 */
    struct cpu_local *cpu = this_cpu();
    if (cpu->current_thread) {
        cpu->current_thread->flags |= THREAD_FLAG_NEED_RESCHED;
    }
    
    apic_eoi();
}

/* IPI TLB 刷新处理程序 */
void smp_tlb_shootdown_handler(struct interrupt_frame *frame) {
    (void)frame;
    
    /* 刷新整个 TLB */
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    
    apic_eoi();
}

/* IPI 内存屏障处理程序 */
void smp_mem_barrier_handler(struct interrupt_frame *frame) {
    (void)frame;
    
    /* 执行内存屏障 */
    asm volatile("mfence" ::: "memory");
    
    apic_eoi();
}

/* IPI 处理函数 */
void smp_call_function_interrupt(struct interrupt_frame *frame) {
    (void)frame;
    
    int cpu_id = smp_processor_id();
    struct smp_call_data *data = &call_data[cpu_id];
    
    if (data->func != NULL) {
        data->func(data->arg);
    }
    
    /* 标记完成 */
    data->done = 1;
    
    apic_eoi();
}

int smp_call_function(int cpu_id, smp_call_func_t func, void *arg, int wait) {
    if (cpu_id < 0 || cpu_id >= smp_cpu_count || cpu_id == smp_processor_id()) {
        return -1;
    }
    
    /* 等待锁 */
    while (__sync_lock_test_and_set(&call_data_lock, 1)) {
        asm volatile("pause");
    }
    
    struct smp_call_data *data = &call_data[cpu_id];
    data->func = func;
    data->arg = arg;
    data->done = 0;
    data->wait = wait;
    
    /* 发送 IPI */
    smp_send_ipi(cpu_data[cpu_id].apic_id, IPI_CALL_FUNCTION);
    
    /* 等待完成 - 添加超时检测 */
    if (wait) {
        /* 超时计数器：约 1 秒 (假设每次 pause 约 1ns，实际会更长) */
        uint64_t timeout = 1000000000ULL;
        while (!data->done && timeout > 0) {
            asm volatile("pause");
            timeout--;
        }
        
        if (timeout == 0) {
            // print("[SMP] Warning: IPI call to CPU ");
            // print_dec(cpu_id);
            // println(" timed out");
            
            /* 关键修复：先清除参数，再设置 done 标志
             * 这样目标 CPU 即使在检查 done 时发现为 1，
             * 也已经没有有效的 func 可以执行了 */
            data->func = NULL;
            data->arg = NULL;
            __sync_synchronize();  /* 内存屏障，确保写入顺序 */
            data->done = 1;  /* 标记为完成，防止后续干扰 */
            
            __sync_lock_release(&call_data_lock);
            return -2;  /* 超时错误 */
        }
    }
    
    __sync_lock_release(&call_data_lock);
    return 0;
}

void smp_call_function_all(smp_call_func_t func, void *arg, int wait) {
    for (int i = 0; i < smp_cpu_count; i++) {
        if (i != smp_processor_id()) {
            smp_call_function(i, func, arg, wait);
        }
    }
}

/* ========== TLB 刷新 ========== */

static void tlb_flush_one(void *arg) {
    uint64_t addr = (uint64_t)arg;
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

static void tlb_flush_all_local(void *arg) {
    (void)arg;
    asm volatile(
        "mov %%cr3, %%rax\n"
        "mov %%rax, %%cr3\n"
        ::: "rax", "memory");
}

void smp_flush_tlb(uint64_t addr) {
    /* 刷新本地 TLB */
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
    
    /* 通知其他 CPU */
    smp_call_function_all(tlb_flush_one, (void *)addr, 0);
}

void smp_flush_tlb_all(void) {
    /* 刷新本地 TLB */
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    
    /* 通知其他 CPU */
    smp_call_function_all(tlb_flush_all_local, NULL, 1);
}

/* ========== CPU 停止 ========== */

static volatile int cpu_stop_requested = 0;

void smp_stop_cpu(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= smp_cpu_count) {
        return;
    }
    
    cpu_stop_requested = 1;
    smp_send_ipi(cpu_data[cpu_id].apic_id, IPI_CPU_STOP);
}

void smp_cpu_stop_handler(struct interrupt_frame *frame) {
    (void)frame;
    
    int cpu_id = smp_processor_id();
    cpu_data[cpu_id].state = CPU_STATE_HALTED;
    
    apic_eoi();
    
    /* 停止此 CPU */
    while (1) {
        asm volatile("hlt");
    }
}

/* ========== AP 入口 (C 部分) ========== */

/* AP 启动后调用的 C 函数 */
void ap_main(void) {
    /* 从启动信息获取 CPU ID */
    int cpu_id = boot_info->cpu_id;
    struct cpu_local *cpu = &cpu_data[cpu_id];
    
    /* 初始化本地 APIC */
    apic_init_local();
    
    /* 设置 GS 段基址 */
    set_cpu_local(cpu);
    
    /* 初始化 GDT 和 TSS */
    gdt_init_cpu(cpu_id);
    
    /* 初始化 IDT */
    idt_reload();
    
    /* 初始化 Per-CPU 调度器数据 (运行队列、idle 线程等) */
    extern void scheduler_init_cpu(int cpu_id);
    scheduler_init_cpu(cpu_id);
    
    /* 启动本地 APIC 定时器 (与 BSP 相同频率) */
    extern void apic_start_timer(uint32_t frequency);
    apic_start_timer(1000);  /* 1000Hz = 1 tick/ms */
    
    /* 标记 AP 为运行状态 */
    cpu->state = CPU_STATE_RUNNING;
    
    /* 标记就绪 (通知 BSP 此 AP 已完成初始化) */
    boot_info->ready = 1;
    
    /* 启用中断 */
    asm volatile("sti");
    
    /* 进入 idle 调度循环 (替代 while(1){hlt;} 使 AP 参与调度) */
    extern void idle_task(void *arg);
    idle_task(NULL);
    
    /* 正常情况不会到达这里 */
    while (1) {
        asm volatile("hlt");
    }
}

/* ========== 统计信息 ========== */

void smp_print_stats(void) {
    // println("=== SMP Statistics ===");
    
    for (int i = 0; i < smp_cpu_count; i++) {
        struct cpu_local *cpu = &cpu_data[i];
        
        // print("CPU ");
        // print_dec(i);
        // print(": APIC=");
        // print_dec(cpu->apic_id);
        // print(" switches=");
        // print_dec(cpu->context_switches);
        // print(" syscalls=");
        // print_dec(cpu->syscalls);
        // println("");
    }
}

/* ========== CPU 热插拔支持 ========== */

/* CPU 热插拔状态 */
#define CPU_HOTPLUG_OFFLINE     0
#define CPU_HOTPLUG_ONLINING    1
#define CPU_HOTPLUG_ONLINE      2
#define CPU_HOTPLUG_OFFLINING   3

struct cpu_hotplug_state {
    int cpu_id;
    int state;
    int can_offline;
    uint64_t offline_time;
    struct list_head callbacks;
};

static struct cpu_hotplug_state hotplug_states[MAX_CPUS];

/* CPU 热插拔回调 */
struct cpu_notifier {
    int (*callback)(int cpu_id, int event);
    struct list_head list;
    int priority;
};

#define CPU_EVENT_ONLINE_START   0
#define CPU_EVENT_ONLINE_END     1
#define CPU_EVENT_OFFLINE_START  2
#define CPU_EVENT_OFFLINE_END    3

static LIST_HEAD(cpu_notifiers);

/* 注册 CPU 热插拔回调 */
int smp_register_cpu_notifier(int (*callback)(int, int), int priority) {
    struct cpu_notifier *notifier = kmalloc(sizeof(struct cpu_notifier));
    if (notifier == NULL) return -1;
    
    notifier->callback = callback;
    notifier->priority = priority;
    list_add(&notifier->list, &cpu_notifiers);
    
    return 0;
}

/* 触发 CPU 热插拔回调 */
static void notify_cpu_event(int cpu_id, int event) {
    struct cpu_notifier *notifier;
    list_for_each_entry(notifier, &cpu_notifiers, list) {
        if (notifier->callback != NULL) {
            notifier->callback(cpu_id, event);
        }
    }
}

/* CPU 上线 */
int smp_cpu_online(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) return -1;
    if (cpu_id == 0) return -1;  /* BSP 不能热插拔 */
    
    struct cpu_hotplug_state *state = &hotplug_states[cpu_id];
    
    if (state->state == CPU_HOTPLUG_ONLINE) {
        return 0;  /* 已经在线 */
    }
    
    state->state = CPU_HOTPLUG_ONLINING;
    
    /* 通知子系统 */
    notify_cpu_event(cpu_id, CPU_EVENT_ONLINE_START);
    
    /* 启动 CPU */
    /* 这里需要发送 SIPI 启动 AP */
    
    /* 等待 CPU 就绪 */
    /* 简化实现 */
    
    state->state = CPU_HOTPLUG_ONLINE;
    notify_cpu_event(cpu_id, CPU_EVENT_ONLINE_END);
    
    return 0;
}

/* CPU 下线 */
int smp_cpu_offline(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= MAX_CPUS) return -1;
    if (cpu_id == 0) return -1;  /* BSP 不能热插拔 */
    
    struct cpu_hotplug_state *state = &hotplug_states[cpu_id];
    
    if (state->state != CPU_HOTPLUG_ONLINE) {
        return -1;
    }
    
    /* 检查是否可以下线 */
    if (!state->can_offline) {
        return -1;
    }
    
    state->state = CPU_HOTPLUG_OFFLINING;
    
    /* 通知子系统 */
    notify_cpu_event(cpu_id, CPU_EVENT_OFFLINE_START);
    
    /* 迁移此 CPU 上的任务 */
    /* 简化实现 */
    
    /* 发送停止 IPI */
    smp_stop_cpu(cpu_id);
    
    state->state = CPU_HOTPLUG_OFFLINE;
    state->offline_time = get_tick_count();
    notify_cpu_event(cpu_id, CPU_EVENT_OFFLINE_END);
    
    return 0;
}

/* ========== CPU 亲和性掩码 ========== */

/* CPU 掩码操作 */
void cpumask_set(cpu_mask_t *mask, int cpu) {
    if (cpu >= 0 && cpu < MAX_CPUS) {
        mask->bits[cpu / 64] |= (1ULL << (cpu % 64));
    }
}

void cpumask_clear(cpu_mask_t *mask, int cpu) {
    if (cpu >= 0 && cpu < MAX_CPUS) {
        mask->bits[cpu / 64] &= ~(1ULL << (cpu % 64));
    }
}

int cpumask_test(const cpu_mask_t *mask, int cpu) {
    if (cpu >= 0 && cpu < MAX_CPUS) {
        return (mask->bits[cpu / 64] >> (cpu % 64)) & 1;
    }
    return 0;
}

void cpumask_copy(cpu_mask_t *dst, const cpu_mask_t *src) {
    for (int i = 0; i < MAX_CPUS / 64; i++) {
        dst->bits[i] = src->bits[i];
    }
}

int cpumask_weight(const cpu_mask_t *mask) {
    int count = 0;
    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpumask_test(mask, i)) {
            count++;
        }
    }
    return count;
}

int cpumask_first(const cpu_mask_t *mask) {
    for (int i = 0; i < MAX_CPUS; i++) {
        if (cpumask_test(mask, i)) {
            return i;
        }
    }
    return -1;
}

/* ========== IPI 广播优化 ========== */

/* 向所有其他 CPU 发送 IPI */
void smp_broadcast_others(uint8_t vector) {
    int my_id = smp_processor_id();
    
    for (int i = 0; i < smp_cpu_count; i++) {
        if (i != my_id && cpu_data[i].state == CPU_STATE_RUNNING) {
            smp_send_ipi(cpu_data[i].apic_id, vector);
        }
    }
}

/* 向所有 CPU 发送 IPI */
void smp_broadcast_all(uint8_t vector) {
    for (int i = 0; i < smp_cpu_count; i++) {
        if (cpu_data[i].state == CPU_STATE_RUNNING) {
            smp_send_ipi(cpu_data[i].apic_id, vector);
        }
    }
}

/* 向指定 CPU 掩码发送 IPI */
void smp_broadcast_mask(const cpu_mask_t *mask, uint8_t vector) {
    for (int i = 0; i < smp_cpu_count; i++) {
        if (cpumask_test(mask, i) && cpu_data[i].state == CPU_STATE_RUNNING) {
            smp_send_ipi(cpu_data[i].apic_id, vector);
        }
    }
}

/* ========== CPU 负载统计 ========== */

struct cpu_load {
    uint64_t idle_ticks;
    uint64_t total_ticks;
    uint64_t load_avg;      /* 负载平均值 * 100 */
    uint64_t last_update;
};

static struct cpu_load cpu_loads[MAX_CPUS];

/* 更新 CPU 负载 */
void smp_update_load(int cpu_id, int idle) {
    if (cpu_id < 0 || cpu_id >= smp_cpu_count) return;
    
    struct cpu_load *load = &cpu_loads[cpu_id];
    uint64_t now = get_tick_count();
    
    load->total_ticks++;
    if (idle) {
        load->idle_ticks++;
    }
    
    /* 计算负载 (每秒更新一次) */
    if (now - load->last_update >= 1000000) {
        uint64_t active = load->total_ticks - load->idle_ticks;
        if (load->total_ticks > 0) {
            load->load_avg = (active * 100) / load->total_ticks;
        }
        load->last_update = now;
    }
}

/* 获取 CPU 负载 */
uint64_t smp_get_load(int cpu_id) {
    if (cpu_id < 0 || cpu_id >= smp_cpu_count) return 0;
    return cpu_loads[cpu_id].load_avg;
}

/* 获取系统总负载 */
uint64_t smp_get_system_load(void) {
    uint64_t total_load = 0;
    int count = 0;
    
    for (int i = 0; i < smp_cpu_count; i++) {
        if (cpu_data[i].state == CPU_STATE_RUNNING) {
            total_load += cpu_loads[i].load_avg;
            count++;
        }
    }
    
    return count > 0 ? total_load / count : 0;
}

/* ========== SMP 调试 ========== */

void smp_dump_state(void) {
    // println("=== SMP State Dump ===");
    // print("CPU count: ");
    // print_dec(smp_cpu_count);
    // println("");
    
    for (int i = 0; i < smp_cpu_count; i++) {
        struct cpu_local *cpu = &cpu_data[i];
        
        // print("CPU ");
        // print_dec(i);
        // print(": APIC=");
        // print_dec(cpu->apic_id);
        // print(" State=");
        
        switch (cpu->state) {
            case CPU_STATE_OFFLINE: print("OFFLINE"); break;
            case CPU_STATE_STARTING: print("BOOTING"); break;
            case CPU_STATE_RUNNING: print("RUNNING"); break;
            case CPU_STATE_HALTED: print("HALTED"); break;
            default: print("UNKNOWN");
        }
        
        // print(" Load=");
        // print_dec(cpu_loads[i].load_avg);
        // print("%");
        
        // println("");
    }
}

/* ========== 内存屏障增强 ========== */

/* 全局内存屏障 - 发送 IPI 强制所有 CPU 同步 */
void smp_mb_global_impl(void) {
    smp_broadcast_others(IPI_MEM_BARRIER);
    smp_mb();  /* 使用宏定义的屏障 */
}

/* 数据依赖屏障 (x86 自动保证) */
void smp_dmb_impl(void) {
    barrier();  /* 使用编译器屏障 */
}

/* ========== 原子操作增强 ========== */

/* 原子比较并交换 (返回旧值) */
int atomic_cmpxchg_int_impl(volatile int *ptr, int old_val, int new_val) {
    return __sync_val_compare_and_swap(ptr, old_val, new_val);
}

/* 原子加并返回新值 */
int atomic_add_return_impl(volatile int *ptr, int val) {
    return __sync_add_and_fetch(ptr, val);
}

/* 原子减并返回新值 */
int atomic_sub_return_impl(volatile int *ptr, int val) {
    return __sync_sub_and_fetch(ptr, val);
}

/* 原子设置位 */
void atomic_set_bit_impl(volatile unsigned long *addr, int bit) {
    __sync_or_and_fetch(addr, 1UL << bit);
}

/* 原子清除位 */
void atomic_clear_bit_impl(volatile unsigned long *addr, int bit) {
    __sync_and_and_fetch(addr, ~(1UL << bit));
}

/* 原子测试并设置位 */
int atomic_test_and_set_bit(volatile unsigned long *addr, int bit) {
    unsigned long mask = 1UL << bit;
    unsigned long old = __sync_fetch_and_or(addr, mask);
    return (old & mask) != 0;
}

/* ========== SMP 高级增强功能 ========== */

/* === CPU 亲和性管理 === */

/* 设置线程 CPU 亲和性 */
int smp_set_affinity_impl(struct thread *t, uint64_t mask) {
    if (t == NULL) return -1;
    t->cpu_affinity = mask;
    return 0;
}

/* 获取线程 CPU 亲和性 */
uint64_t smp_get_affinity_impl(struct thread *t) {
    if (t == NULL) return 0;
    return t->cpu_affinity;
}

/* 迁移线程到指定 CPU */
int smp_migrate_thread_impl(struct thread *t, int target_cpu) {
    if (t == NULL || target_cpu < 0 || target_cpu >= smp_cpu_count) return -1;
    
    t->cpu_affinity = 1ULL << target_cpu;
    smp_send_ipi(target_cpu, IPI_RESCHEDULE);
    
    return 0;
}

/* === 负载均衡增强 === */

/* 计算负载不平衡 */
int smp_calc_imbalance(void) {
    if (smp_cpu_count <= 1) return 0;
    
    uint64_t total_load = 0;
    for (int i = 0; i < smp_cpu_count; i++) {
        total_load += cpu_loads[i].load_avg;
    }
    
    uint64_t avg_load = total_load / smp_cpu_count;
    uint64_t max_deviation = 0;
    
    for (int i = 0; i < smp_cpu_count; i++) {
        uint64_t deviation = cpu_loads[i].load_avg > avg_load ?
            cpu_loads[i].load_avg - avg_load : avg_load - cpu_loads[i].load_avg;
        if (deviation > max_deviation) {
            max_deviation = deviation;
        }
    }
    
    return (int)max_deviation;
}

/* 执行负载均衡 */
void smp_do_load_balance(void) {
    if (smp_cpu_count <= 1) return;
    
    int busiest = 0, idlest = 0;
    uint64_t max_load = 0, min_load = 100;
    
    for (int i = 0; i < smp_cpu_count; i++) {
        if (cpu_loads[i].load_avg > max_load) {
            max_load = cpu_loads[i].load_avg;
            busiest = i;
        }
        if (cpu_loads[i].load_avg < min_load) {
            min_load = cpu_loads[i].load_avg;
            idlest = i;
        }
    }
    
    if (max_load - min_load > 25) {
        smp_send_ipi(busiest, IPI_RESCHEDULE);
    }
}

/* === 性能监控 === */

struct smp_perf_data {
    uint64_t ipi_count;
    uint64_t migrations;
    uint64_t load_balances;
};

static struct smp_perf_data smp_perf_data;

/* 记录 IPI */
void smp_record_ipi_stat(void) {
    smp_perf_data.ipi_count++;
}

/* 记录迁移 */
void smp_record_migration_stat(void) {
    smp_perf_data.migrations++;
}

/* 记录负载均衡 */
void smp_record_lb_stat(void) {
    smp_perf_data.load_balances++;
}

/* 获取统计 */
void smp_get_perf_data(struct smp_perf_data *stats) {
    *stats = smp_perf_data;
}

/* === 诊断报告 === */

void smp_print_report(void) {
    // println("╔══════════════════════════════════════════╗");
    // println("║          SMP System Report               ║");
    // println("╚══════════════════════════════════════════╝");
    
    // println("");
    // println("=== CPU 状态 ===");
    // print("CPU 数量: "); print_dec(smp_cpu_count); println("");
    
    int active = 0;
    for (int i = 0; i < smp_cpu_count; i++) {
        if (cpu_data[i].state == CPU_STATE_RUNNING) active++;
    }
    // print("活跃 CPU: "); print_dec(active); println("");
    
    // println("");
    // println("=== 负载分布 ===");
    for (int i = 0; i < smp_cpu_count; i++) {
        // print("CPU "); print_dec(i); print(": ");
        // print_dec(cpu_loads[i].load_avg); println("%");
    }
    
    // println("");
    // println("=== 性能统计 ===");
    // print("IPI 发送: "); print_dec(smp_perf_data.ipi_count); println("");
    // print("线程迁移: "); print_dec(smp_perf_data.migrations); println("");
    // print("负载均衡: "); print_dec(smp_perf_data.load_balances); println("");
}
