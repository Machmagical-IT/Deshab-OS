#include "apic.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "console.h"
#include "cpu.h"
#include "task.h"

/* APIC 基地址 (虚拟地址) */
static uint64_t apic_base = 0;

/* I/O APIC 基地址 (虚拟地址) */
static uint64_t ioapic_base = 0;

/* 映射 MMIO 区域 (禁用缓存) */
static int map_mmio_region(uint64_t phys, uint64_t virt, size_t pages) {
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | 
                     VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITETHROUGH |
                     VMM_FLAG_NX;  /* MMIO 区域不可执行 */
    
    for (size_t i = 0; i < pages; i++) {
        int ret = vmm_map_page(vmm_get_kernel_space(), virt + i * PAGE_SIZE, 
                               phys + i * PAGE_SIZE, flags);
        if (ret != 0) {
            // print("    [ERROR] Failed to map MMIO page ");
            // print_dec(i);
            // println("");
            return -1;
        }
    }
    
    /* 刷新 TLB */
    for (size_t i = 0; i < pages; i++) {
        vmm_invalidate_page(virt + i * PAGE_SIZE);
    }
    
    return 0;
}

/* APIC 定时器频率 */
static uint32_t timer_frequency = 0;
static uint32_t bus_frequency = 0;

/* 读取 APIC 寄存器 */
uint32_t apic_read(uint32_t reg) {
    return *(volatile uint32_t *)(apic_base + reg);
}

/* 写入 APIC 寄存器 */
void apic_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(apic_base + reg) = value;
}

/* 发送 EOI */
void apic_eoi(void) {
    apic_write(APIC_EOI, 0);
}

/* 获取 APIC ID */
uint32_t apic_get_id(void) {
    return apic_read(APIC_ID) >> 24;
}

/* 设置任务优先级 */
void apic_set_tpr(uint32_t priority) {
    apic_write(APIC_TPR, priority & 0xFF);
}

/* 获取 APIC 基地址 */
uint64_t apic_get_base(void) {
    return apic_base;
}

/* 从 MSR 获取 APIC 基地址 */
static uint64_t apic_get_base_from_msr(void) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B));  /* IA32_APIC_BASE */
    uint64_t msr = ((uint64_t)hi << 32) | lo;
    return (msr & 0x000FFFFFFFFFF000ULL);  /* APIC base 物理地址 (位 12-51) */
}

/* 检查 APIC 是否可用 */
static int apic_available(void) {
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" 
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(1));
    return (edx >> 9) & 1;  /* APIC 位 */
}

void apic_init(void) {
    if (!apic_available()) {
        // println("[APIC] Error: APIC not available!");
        return;
    }
    
    /* 获取 APIC 基地址 */
    uint64_t phys_base = apic_get_base_from_msr();
    
    // print("    APIC physical base: 0x");
    // print_hex(phys_base);
    // println("");
    
    /* 使用 HHDM 偏移转换地址 */
    apic_base = (uint64_t)phys_to_virt(phys_base);
    
    // print("    APIC virtual base: 0x");
    // print_hex(apic_base);
    // println("");
    
    /* 手动映射 APIC MMIO 区域 (1 页，禁用缓存) */
    /* Limine HHDM 可能没有正确映射 MMIO 区域的缓存属性 */
    if (map_mmio_region(phys_base, apic_base, 1) != 0) {
        // println("[APIC] Error: Failed to map APIC MMIO region!");
        return;
    }
    
    // print("    APIC ID: ");
    // print_dec(apic_get_id());
    // println("");
    
    /* 启用 APIC */
    /* 设置 SVR: 启用 APIC + 伪中断向量 */
    uint32_t svr = apic_read(APIC_SVR);
    svr |= APIC_SVR_ENABLE;
    svr = (svr & ~0xFF) | APIC_SPURIOUS_VECTOR;  /* 设置伪中断向量 (必须是 255) */
    apic_write(APIC_SVR, svr);
    
    /* 设置任务优先级为 0 (接受所有中断) */
    apic_set_tpr(0);
    
    /* 清除错误状态 */
    apic_write(APIC_ESR, 0);
    apic_read(APIC_ESR);
    
    /* 设置错误中断向量 */
    apic_write(APIC_LVT_ERROR, APIC_ERROR_VECTOR);
}

/* 初始化本地 CPU 的 APIC (用于 AP 启动) */
void apic_init_local(void) {
    /* 设置 SVR 启用 APIC */
    apic_write(APIC_SVR, APIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR);
    
    /* 设置任务优先级为 0 */
    apic_write(APIC_TPR, 0);
    
    /* 清除错误状态 */
    apic_write(APIC_ESR, 0);
    apic_read(APIC_ESR);
    
    /* 设置错误中断向量 */
    apic_write(APIC_LVT_ERROR, APIC_ERROR_VECTOR);
}

/* 发送 IPI (处理器间中断) */
void apic_send_ipi(uint32_t apic_id, uint32_t vector, uint32_t mode) {
    /* 设置目标 APIC ID */
    apic_write(APIC_ICR_HIGH, apic_id << 24);
    
    /* 设置向量号和模式 */
    uint32_t icr_low = vector | mode;
    apic_write(APIC_ICR_LOW, icr_low);
    
    /* 等待发送完成 */
    while (apic_read(APIC_ICR_LOW) & (1 << 12)) {
        asm volatile("pause");
    }
}

/* 校准 APIC 定时器 */
static uint32_t calibrate_timer(void) {
    /* 使用 TSC 作为参考来校准 APIC 定时器 */
    /* 在校准期间禁用中断，避免测量偏差 */

    uint64_t rflags;
    asm volatile("pushfq; popq %0; cli" : "=r"(rflags));

    /* 设置定时器为单次模式，初始计数值为最大值 */
    apic_write(APIC_TIMER_DCR, APIC_TIMER_DIV_16);
    apic_write(APIC_TIMER_ICR, 0xFFFFFFFF);
    apic_write(APIC_LVT_TIMER, APIC_TIMER_MODE_ONE_SHOT);

    /* 使用 TSC 作为近似参考 */
    uint64_t tsc_start, tsc_end;
    uint64_t tsc_freq = cpu_get_tsc_frequency();

    asm volatile("rdtsc" : "=a"(((uint32_t*)&tsc_start)[0]), "=d"(((uint32_t*)&tsc_start)[1]));

    /* 等待约 10ms */
    uint64_t wait = tsc_freq / 100;  /* 10ms */
    do {
        asm volatile("rdtsc" : "=a"(((uint32_t*)&tsc_end)[0]), "=d"(((uint32_t*)&tsc_end)[1]));
    } while ((tsc_end - tsc_start) < wait);

    /* 读取剩余计数值 */
    uint32_t remaining = apic_read(APIC_TIMER_CCR);
    uint32_t elapsed = 0xFFFFFFFF - remaining;

    /* 计算总线频率 */
    /* elapsed = (bus_freq / divider) * time */
    /* 10ms elapsed count with divider 16 */
    bus_frequency = (elapsed * 16 * 100);  /* * 100 because we waited 10ms = 1/100 sec */

    /* 恢复中断状态 */
    if (rflags & 0x200) {
        asm volatile("sti");
    }

    return bus_frequency;
}

void apic_timer_init(uint32_t frequency) {
    // println("[APIC] Initializing APIC timer...");
    
    /* 防止除零 - frequency 检查 */
    if (frequency == 0) {
        // println("[APIC] Error: Invalid frequency (0)");
        frequency = 1000;  /* 默认 1000 Hz */
    }
    
    /* 频率上限检查 */
    if (frequency > 1000000) {
        // println("[APIC] Warning: Frequency too high, capping at 1 MHz");
        frequency = 1000000;
    }
    
    /* 校准总线频率 */
    if (bus_frequency == 0) {
        calibrate_timer();
    }
    
    /* 如果校准失败，使用默认值 */
    if (bus_frequency == 0) {
        // println("[APIC] Warning: Timer calibration failed, using default frequency");
        bus_frequency = 100000000;  /* 默认 100 MHz */
    }
    
    timer_frequency = frequency;
    
    /* 计算初始计数值 - 完整的安全检查 */
    uint32_t divider = 16;
    uint32_t divisor = divider * frequency;
    
    /* 检查乘法溢出 */
    if (divisor < divider || divisor < frequency) {
        // println("[APIC] Warning: divider * frequency overflow, using safe value");
        divisor = 16000;  /* 16 * 1000 安全默认值 */
    }
    
    /* 检查除数有效性 */
    if (divisor == 0) {
        // println("[APIC] Error: Division by zero avoided");
        divisor = 16000;  /* 安全默认值 */
    }
    
    /* 检查 bus_frequency 有效性 */
    if (bus_frequency == 0) {
        // println("[APIC] Error: Bus frequency is zero");
        bus_frequency = 100000000;  /* 强制设置默认值 */
    }
    
    uint32_t count = bus_frequency / divisor;
    
    /* 确保计数值有效 */
    if (count == 0) {
        count = 1;  /* 最小计数值 */
        // println("[APIC] Warning: Timer count was 0, set to 1");
    }
    
    /* 计数值上限检查 */
    if (count > 0x7FFFFFFF) {
        count = 0x7FFFFFFF;  /* 避免可能的溢出 */
        // println("[APIC] Warning: Timer count capped at max");
    }
    
    // print("  Bus frequency: ");
    // print_dec(bus_frequency / 1000000);
    // println(" MHz");
    // print("  Timer frequency: ");
    // print_dec(frequency);
    // println(" Hz");
    // print("  Initial count: ");
    // print_dec(count);
    // println("");
    
    /* 设置分频 */
    apic_write(APIC_TIMER_DCR, APIC_TIMER_DIV_16);
    
    /* 设置初始计数 */
    apic_write(APIC_TIMER_ICR, count);
    
    /* 设置定时器模式为周期性，并设置中断向量 */
    apic_write(APIC_LVT_TIMER, APIC_TIMER_VECTOR | APIC_TIMER_MODE_PERIODIC);
    
    // println("[APIC] Timer initialized successfully");
}

void apic_timer_set_frequency(uint32_t frequency) {
    timer_frequency = frequency;
    
    if (bus_frequency == 0) {
        calibrate_timer();
    }
    
    uint32_t count = bus_frequency / (16 * frequency);
    apic_write(APIC_TIMER_ICR, count);
}

void apic_timer_start(void) {
    uint32_t lvt = apic_read(APIC_LVT_TIMER);
    lvt &= ~APIC_LVT_MASKED;
    apic_write(APIC_LVT_TIMER, lvt);
}

void apic_timer_stop(void) {
    uint32_t lvt = apic_read(APIC_LVT_TIMER);
    lvt |= APIC_LVT_MASKED;
    apic_write(APIC_LVT_TIMER, lvt);
}

uint32_t apic_timer_get_bus_frequency(void) {
    return bus_frequency;
}

void apic_start_timer(uint32_t frequency) {
    apic_timer_init(frequency);
    apic_timer_start();
}

/* I/O APIC 支持 */

static uint32_t ioapic_read(uint8_t reg) {
    *(volatile uint32_t *)(ioapic_base + IOAPIC_REG_SEL) = reg;
    return *(volatile uint32_t *)(ioapic_base + IOAPIC_REG_WIN);
}

static void ioapic_write(uint8_t reg, uint32_t value) {
    *(volatile uint32_t *)(ioapic_base + IOAPIC_REG_SEL) = reg;
    *(volatile uint32_t *)(ioapic_base + IOAPIC_REG_WIN) = value;
}

void ioapic_init(void) {
    // println("[IOAPIC] Initializing I/O APIC...");
    
    ioapic_base = (uint64_t)phys_to_virt(IOAPIC_BASE);
    
    /* 手动映射 I/O APIC MMIO 区域 (1 页，禁用缓存) */
    if (map_mmio_region(IOAPIC_BASE, ioapic_base, 1) != 0) {
        // println("[IOAPIC] Error: Failed to map I/O APIC MMIO region!");
        return;
    }
    
    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    uint8_t max_irq = (ver >> 16) & 0xFF;
    
    // print("  I/O APIC version: ");
    // print_dec(ver & 0xFF);
    // println("");
    // print("  Max IRQs: ");
    // print_dec(max_irq + 1);
    // println("");
    
    /* 屏蔽所有 IRQ */
    for (int i = 0; i <= max_irq; i++) {
        ioapic_write(0x10 + i * 2, APIC_LVT_MASKED | (IRQ_BASE + i));
        ioapic_write(0x11 + i * 2, 0);
    }
    
    // println("[IOAPIC] Initialized successfully");
}

void ioapic_set_irq(uint8_t irq, uint8_t vector, uint32_t apic_id) {
    /* 设置 I/O 重定向表项 */
    /* 低 32 位: vector + delivery mode + destination mode + polarity + trigger + mask */
    /* 高 32 位: destination (APIC ID) */
    
    uint32_t low = vector | (0 << 8) |   /* delivery mode: Fixed */
                   (0 << 11) |            /* destination mode: Physical */
                   (0 << 13) |            /* polarity: Active High */
                   (0 << 15) |            /* trigger mode: Edge */
                   (0 << 16);             /* unmasked */
    
    ioapic_write(0x10 + irq * 2, low);
    ioapic_write(0x11 + irq * 2, apic_id << 24);
}

/* ========== APIC 高级增强功能 ========== */

/* === 中断路由优化 === */

/* IRQ 平衡状态 */
struct irq_balance {
    uint8_t irq;
    uint8_t current_cpu;
    uint8_t target_cpu;
    uint32_t count;
    uint64_t last_balance;
};

#define MAX_IRQ_BALANCE     24
static struct irq_balance irq_balance_table[MAX_IRQ_BALANCE];
static int irq_balance_count = 0;

/* 记录 IRQ 处理 */
void apic_record_irq(uint8_t irq) {
    for (int i = 0; i < irq_balance_count; i++) {
        if (irq_balance_table[i].irq == irq) {
            irq_balance_table[i].count++;
            return;
        }
    }
    
    /* 新 IRQ */
    if (irq_balance_count < MAX_IRQ_BALANCE) {
        irq_balance_table[irq_balance_count].irq = irq;
        irq_balance_table[irq_balance_count].count = 1;
        irq_balance_table[irq_balance_count].current_cpu = 0;
        irq_balance_table[irq_balance_count].target_cpu = 0;
        irq_balance_count++;
    }
}

/* IRQ 平衡 */
void apic_balance_irqs(int cpu_count) {
    if (cpu_count <= 1) return;
    
    for (int i = 0; i < irq_balance_count; i++) {
        struct irq_balance *ib = &irq_balance_table[i];
        
        /* 计算目标 CPU (轮询) */
        int target = (ib->current_cpu + 1) % cpu_count;
        
        if (target != ib->current_cpu) {
            ib->target_cpu = target;
            /* 迁移 IRQ */
            ioapic_set_irq(ib->irq, IRQ_BASE + ib->irq, target);
            ib->current_cpu = target;
            ib->count = 0;
        }
    }
}

/* === APIC 性能统计 === */

struct apic_perf_stats {
    uint64_t timer_interrupts;
    uint64_t spurious_interrupts;
    uint64_t ipi_sent;
    uint64_t ipi_received;
    uint64_t errors;
};

static struct apic_perf_stats apic_perf;

/* 记录定时器中断 */
void apic_record_timer_int(void) {
    apic_perf.timer_interrupts++;
}

/* 记录伪中断 */
void apic_record_spurious(void) {
    apic_perf.spurious_interrupts++;
}

/* 记录 IPI 发送 */
void apic_record_ipi_sent(void) {
    apic_perf.ipi_sent++;
}

/* 记录 IPI 接收 */
void apic_record_ipi_recv(void) {
    apic_perf.ipi_received++;
}

/* 记录错误 */
void apic_record_error(void) {
    apic_perf.errors++;
}

/* 获取统计 */
void apic_get_perf_stats(struct apic_perf_stats *stats) {
    *stats = apic_perf;
}

/* === 高级定时器功能 === */

/* 定时器模式 */
#define APIC_TIMER_ONESHOT      0
#define APIC_TIMER_PERIODIC     1
#define APIC_TIMER_DEADLINE     2

/* 设置定时器模式 */
void apic_timer_set_mode(int mode) {
    uint32_t lvt = apic_read(APIC_LVT_TIMER);
    lvt &= ~(3 << 17);  /* 清除模式位 */
    lvt |= (mode << 17);
    apic_write(APIC_LVT_TIMER, lvt);
}

/* 设置定时器分频 */
void apic_timer_set_divider(int div) {
    uint32_t config = 0;
    switch (div) {
        case 1:  config = 0x0B; break;
        case 2:  config = 0x00; break;
        case 4:  config = 0x01; break;
        case 8:  config = 0x02; break;
        case 16: config = 0x03; break;
        case 32: config = 0x08; break;
        case 64: config = 0x09; break;
        case 128: config = 0x0A; break;
        default: config = 0x03; break;  /* 默认 16 */
    }
    apic_write(APIC_TIMER_DCR, config);
}

/* TSC-Deadline 模式支持 */
int apic_supports_tsc_deadline(void) {
    uint32_t a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    return (c & (1 << 24)) != 0;  /* TSC-Deadline bit */
}

/* 使用 TSC-Deadline 定时器 */
void apic_set_tsc_deadline(uint64_t tsc) {
    /* 设置 TSC deadline MSR */
    asm volatile("wrmsr" :: "a"((uint32_t)tsc), "d"((uint32_t)(tsc >> 32)), "c"(0x6E0));
    
    /* 启用 TSC-deadline 模式 */
    uint32_t lvt = apic_read(APIC_LVT_TIMER);
    lvt &= ~(3 << 17);
    lvt |= (2 << 17);  /* TSC-deadline mode */
    lvt &= ~APIC_LVT_MASKED;
    apic_write(APIC_LVT_TIMER, lvt);
}

/* === 中断优先级管理 === */

/* 设置任务优先级 */
void apic_set_task_priority(uint8_t priority) {
    apic_write(APIC_TPR, priority & 0xFF);
}

/* 获取任务优先级 */
uint8_t apic_get_tpr(void) {
    return apic_read(APIC_TPR) & 0xFF;
}

/* 设置处理器优先级 */
void apic_set_ppr(uint8_t priority) {
    /* PPR 是只读的，由 TPR 和当前 ISR 决定 */
    (void)priority;
}

/* 获取处理器优先级 */
uint8_t apic_get_ppr(void) {
    return apic_read(APIC_PPR) & 0xFF;
}

/* === 中断控制器诊断 === */

/* 检查 APIC 版本 */
uint32_t apic_get_version(void) {
    return apic_read(APIC_VERSION) & 0xFF;
}

/* 获取最大 LVT 条目数 */
uint32_t apic_get_max_lvt(void) {
    return (apic_read(APIC_VERSION) >> 16) & 0xFF;
}

/* 检查 APIC 状态 */
int apic_check_status(void) {
    uint32_t id = apic_read(APIC_ID);
    (void)id;  /* ID 保留用于未来扩展 */
    uint32_t ver = apic_read(APIC_VERSION);
    
    if (ver == 0 || ver == 0xFFFFFFFF) {
        return -1;  /* APIC 未响应 */
    }
    
    return 0;
}

/* 打印 APIC 信息 */
void apic_print_info(void) {
    // println("╔══════════════════════════════════════════╗");
    // println("║          APIC Information                ║");
    // println("╚══════════════════════════════════════════╝");
    
    // println("");
    // println("=== Local APIC ===");
    // print("APIC ID: "); print_hex(apic_read(APIC_ID) >> 24); println("");
    // print("版本: "); print_dec(apic_get_version()); println("");
    // print("最大 LVT: "); print_dec(apic_get_max_lvt()); println("");
    
    uint32_t svr = apic_read(APIC_SVR);
    // print("软件使能: "); println((svr & 0x100) ? "是" : "否");
    
    // println("");
    // println("=== 定时器 ===");
    // print("总线频率: "); print_dec(bus_frequency / 1000000); println(" MHz");
    // print("当前计数: "); print_dec(apic_read(APIC_TIMER_CCR)); println("");
    
    // println("");
    // println("=== 性能统计 ===");
    // print("定时器中断: "); print_dec(apic_perf.timer_interrupts); println("");
    // print("伪中断: "); print_dec(apic_perf.spurious_interrupts); println("");
    // print("IPI 发送: "); print_dec(apic_perf.ipi_sent); println("");
    // print("IPI 接收: "); print_dec(apic_perf.ipi_received); println("");
    // print("错误: "); print_dec(apic_perf.errors); println("");
}

/* === I/O APIC 增强 === */

/* 屏蔽 IRQ */
void ioapic_mask_irq(uint8_t irq) {
    uint32_t low = ioapic_read(0x10 + irq * 2);
    low |= (1 << 16);  /* 设置屏蔽位 */
    ioapic_write(0x10 + irq * 2, low);
}

/* 取消屏蔽 IRQ */
void ioapic_unmask_irq(uint8_t irq) {
    uint32_t low = ioapic_read(0x10 + irq * 2);
    low &= ~(1 << 16);  /* 清除屏蔽位 */
    ioapic_write(0x10 + irq * 2, low);
}

/* 设置 IRQ 触发模式 */
void ioapic_set_trigger(uint8_t irq, int level) {
    uint32_t low = ioapic_read(0x10 + irq * 2);
    if (level) {
        low |= (1 << 15);  /* Level trigger */
    } else {
        low &= ~(1 << 15);  /* Edge trigger */
    }
    ioapic_write(0x10 + irq * 2, low);
}

/* 设置 IRQ 极性 */
void ioapic_set_polarity(uint8_t irq, int active_low) {
    uint32_t low = ioapic_read(0x10 + irq * 2);
    if (active_low) {
        low |= (1 << 13);
    } else {
        low &= ~(1 << 13);
    }
    ioapic_write(0x10 + irq * 2, low);
}

/* 获取 I/O APIC IRQ 状态 */
uint32_t ioapic_get_irq_status(uint8_t irq) {
    return ioapic_read(0x10 + irq * 2);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSI (Message Signaled Interrupts) 支持
 * 通过写入 PCI 配置空间中的 MSI 能力寄存器来路由中断到 APIC
 * ═══════════════════════════════════════════════════════════════════════════ */

/* PCI 配置空间访问函数指针 */
static pci_read_config_t pci_read_fn = NULL;
static pci_write_config_t pci_write_fn = NULL;

/* 注册 PCI 配置空间访问函数 */
void apic_register_pci_access(pci_read_config_t read_fn, pci_write_config_t write_fn) {
    pci_read_fn = read_fn;
    pci_write_fn = write_fn;
}

/* 在 PCI 能力链中查找指定能力 ID */
static int pci_find_capability(uint8_t bus, uint8_t dev, uint8_t func, uint8_t cap_id) {
    if (!pci_read_fn) return -1;

    /* 检查能力列表是否可用 (状态寄存器 bit 4) */
    uint32_t status = pci_read_fn(bus, dev, func, 0x06);
    if (!(status & (1 << 20))) {
        return -1;  /* 无能力列表 */
    }

    /* 遍历能力链 */
    uint8_t offset = (uint8_t)(pci_read_fn(bus, dev, func, 0x34) & 0xFC);
    while (offset != 0) {
        uint32_t cap = pci_read_fn(bus, dev, func, offset);
        if ((cap & 0xFF) == cap_id) {
            return offset;
        }
        offset = (uint8_t)((cap >> 8) & 0xFC);
    }

    return -1;  /* 未找到 */
}

/* MSI 设置: 为 PCI 设备配置 MSI 中断
 *
 * @param bus     PCI 总线号
 * @param dev     PCI 设备号
 * @param func    PCI 功能号
 * @param vector  中断向量号
 * @param apic_id 目标 APIC ID
 * @param nvec    请求的向量数 (1/2/4/8/16/32, 2 的幂)
 * @return 0=成功, -1=失败
 */
int apic_setup_msi(uint8_t bus, uint8_t dev, uint8_t func,
                   uint8_t vector, uint8_t apic_id, int nvec) {
    if (!pci_read_fn || !pci_write_fn) return -1;

    /* 查找 MSI 能力 */
    int cap_off = pci_find_capability(bus, dev, func, MSI_CAP_ID);
    if (cap_off < 0) return -1;

    /* 读取 MSI 控制寄存器 */
    uint16_t ctrl = (uint16_t)pci_read_fn(bus, dev, func, cap_off + MSI_CTRL_OFFSET);

    /* 屏蔽 MSI (配置期间) */
    ctrl &= ~MSI_CTRL_ENABLE;
    pci_write_fn(bus, dev, func, cap_off + MSI_CTRL_OFFSET, ctrl);

    /* 计算多消息编码 (log2(nvec) << 4) */
    int encode = 0;
    int tmp = nvec;
    while (tmp > 1) { encode++; tmp >>= 1; }

    /* 检查请求的向量数是否超出能力 */
    int max_enc = (ctrl & MSI_CTRL_MULTI_CAP) >> 1;
    if (encode > max_enc) {
        encode = max_enc;
        nvec = 1 << encode;
    }

    /* 写入 MSI 地址寄存器
     * 格式: [63:20] 基地址 0xFEE, [19:12] 目标 APIC ID, [3:0] 红定向提示等 */
    uint32_t addr_low = 0xFEE00000UL | ((uint32_t)apic_id << 12);
    uint32_t addr_high = 0;
    pci_write_fn(bus, dev, func, cap_off + MSI_ADDR_OFFSET, addr_low);

    /* 如果支持 64 位地址，写入高位 */
    if (ctrl & MSI_CTRL_64BIT) {
        pci_write_fn(bus, dev, func, cap_off + MSI_ADDR_OFFSET + 4, addr_high);
    }

    /* 写入 MSI 数据寄存器
     * 格式: [15:8] 传送模式 (000=固定), [7:0] 向量号 */
    uint16_t data = (uint16_t)vector;  /* 固定传送模式, 边沿触发 */
    int data_off = (ctrl & MSI_CTRL_64BIT) ? MSI_DATA_64_OFFSET : MSI_DATA_32_OFFSET;
    pci_write_fn(bus, dev, func, cap_off + data_off, (uint32_t)data);

    /* 设置多消息启用并启用 MSI */
    ctrl &= ~MSI_CTRL_MULTI_ENABLE;
    ctrl |= (encode << 4) & MSI_CTRL_MULTI_ENABLE;
    ctrl |= MSI_CTRL_ENABLE;
    pci_write_fn(bus, dev, func, cap_off + MSI_CTRL_OFFSET, ctrl);

    return 0;
}

/* MSI 禁用 */
int apic_disable_msi(uint8_t bus, uint8_t dev, uint8_t func) {
    if (!pci_read_fn || !pci_write_fn) return -1;

    int cap_off = pci_find_capability(bus, dev, func, MSI_CAP_ID);
    if (cap_off < 0) return -1;

    /* 清除启用位 */
    uint16_t ctrl = (uint16_t)pci_read_fn(bus, dev, func, cap_off + MSI_CTRL_OFFSET);
    ctrl &= ~MSI_CTRL_ENABLE;
    pci_write_fn(bus, dev, func, cap_off + MSI_CTRL_OFFSET, ctrl);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSI-X 支持
 * MSI-X 使用内存中的表/PBA 结构，支持逐向量屏蔽
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 从 PCI 能力的 BIR 字段解析基址和 BAR 索引 */
static void msix_parse_bar_offset(uint32_t bir_field, uint64_t *base, uint8_t *bar_idx) {
    *bar_idx = (uint8_t)(bir_field & 0x07);
    *base = (uint64_t)(bir_field & ~0x07ULL);
}

/* MSI-X 设置: 为 PCI 设备配置 MSI-X 中断
 *
 * @param bus    PCI 总线号
 * @param dev    PCI 设备号
 * @param func   PCI 功能号
 * @param config MSI-X 配置结构 (调用者提供, 函数填充)
 * @return 0=成功, -1=失败
 */
int apic_setup_msix(uint8_t bus, uint8_t dev, uint8_t func,
                    struct msix_config *config) {
    if (!pci_read_fn || !pci_write_fn || !config) return -1;

    /* 查找 MSI-X 能力 */
    int cap_off = pci_find_capability(bus, dev, func, MSIX_CAP_ID);
    if (cap_off < 0) return -1;

    config->cap_offset = (uint16_t)cap_off;

    /* 读取控制寄存器获取表大小 */
    uint16_t ctrl = (uint16_t)pci_read_fn(bus, dev, func, cap_off + MSIX_CTRL_OFFSET);
    config->table_size = (ctrl & MSIX_CTRL_TABLE_SIZE) + 1;  /* 表大小 = 值 + 1 */

    /* 读取表和 PBA 的偏移/BIR */
    uint32_t table_bir = pci_read_fn(bus, dev, func, cap_off + MSIX_TABLE_OFFSET);
    uint32_t pba_bir = pci_read_fn(bus, dev, func, cap_off + MSIX_PBA_OFFSET);

    /* 解析表和 PBA 的物理地址
     * 实际地址 = BAR 基地址 + 偏移
     * 这里简化处理: 假设调用者已映射好地址 */
    uint64_t table_offset, pba_offset;
    uint8_t table_bar, pba_bar;
    msix_parse_bar_offset(table_bir, &table_offset, &table_bar);
    msix_parse_bar_offset(pba_bir, &pba_offset, &pba_bar);

    /* 注意: 实际映射需要读取 BAR[table_bar] 获取基址，然后加上 offset
     * 这里仅记录偏移，映射由调用者完成 */
    config->table_base = table_offset;
    config->pba_base = pba_offset;

    /* 屏蔽所有向量 (配置期间) */
    ctrl |= MSIX_CTRL_FUNC_MASK;
    pci_write_fn(bus, dev, func, cap_off + MSIX_CTRL_OFFSET, ctrl);

    /* 如果表已映射，屏蔽所有表项 */
    if (config->table_base) {
        volatile struct msix_table_entry *table =
            (volatile struct msix_table_entry *)config->table_base;
        for (uint16_t i = 0; i < config->table_size; i++) {
            table[i].vector_ctrl |= 0x01;  /* bit 0 = 屏蔽 */
        }
    }

    /* 启用 MSI-X */
    ctrl |= MSIX_CTRL_ENABLE;
    ctrl &= ~MSIX_CTRL_FUNC_MASK;  /* 清除函数级屏蔽 */
    pci_write_fn(bus, dev, func, cap_off + MSIX_CTRL_OFFSET, ctrl);

    config->enabled = 1;
    return 0;
}

/* MSI-X 禁用 */
int apic_disable_msix(uint8_t bus, uint8_t dev, uint8_t func,
                      struct msix_config *config) {
    if (!pci_read_fn || !pci_write_fn) return -1;

    int cap_off = (config && config->cap_offset) ?
                  config->cap_offset :
                  pci_find_capability(bus, dev, func, MSIX_CAP_ID);
    if (cap_off < 0) return -1;

    /* 屏蔽所有向量 */
    if (config && config->table_base) {
        volatile struct msix_table_entry *table =
            (volatile struct msix_table_entry *)config->table_base;
        for (uint16_t i = 0; i < config->table_size; i++) {
            table[i].vector_ctrl |= 0x01;
        }
    }

    /* 禁用 MSI-X */
    uint16_t ctrl = (uint16_t)pci_read_fn(bus, dev, func, cap_off + MSIX_CTRL_OFFSET);
    ctrl &= ~MSIX_CTRL_ENABLE;
    ctrl |= MSIX_CTRL_FUNC_MASK;
    pci_write_fn(bus, dev, func, cap_off + MSIX_CTRL_OFFSET, ctrl);

    if (config) config->enabled = 0;
    return 0;
}

/* MSI-X 向量屏蔽 */
void apic_msix_mask_vector(struct msix_config *config, uint16_t entry) {
    if (!config || !config->table_base || entry >= config->table_size) return;

    volatile struct msix_table_entry *table =
        (volatile struct msix_table_entry *)config->table_base;
    table[entry].vector_ctrl |= 0x01;
}

/* MSI-X 向量取消屏蔽 */
void apic_msix_unmask_vector(struct msix_config *config, uint16_t entry) {
    if (!config || !config->table_base || entry >= config->table_size) return;

    volatile struct msix_table_entry *table =
        (volatile struct msix_table_entry *)config->table_base;
    table[entry].vector_ctrl &= ~0x01U;
}

/* MSI-X 检查向量是否待处理 */
int apic_msix_is_pending(struct msix_config *config, uint16_t entry) {
    if (!config || !config->pba_base || entry >= config->table_size) return 0;

    /* PBA 是位图: 每个 MSI-X 向量对应一位 */
    volatile uint32_t *pba = (volatile uint32_t *)config->pba_base;
    uint32_t word = entry / 32;
    uint32_t bit = entry % 32;
    return (pba[word] & (1U << bit)) != 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 中断亲和性 - 绑定中断到指定 CPU
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 亲和性表 */
static struct irq_affinity irq_affinity_table[MAX_IRQ_BALANCE];
static int irq_affinity_count = 0;

/* 设置 IRQ 亲和性 */
int apic_set_irq_affinity(uint8_t irq, uint8_t target_cpu) {
    /* 查找现有条目 */
    for (int i = 0; i < irq_affinity_count; i++) {
        if (irq_affinity_table[i].irq == irq) {
            irq_affinity_table[i].target_cpu = target_cpu;
            /* 通过 I/O APIC 重定向到目标 CPU */
            ioapic_set_irq(irq, IRQ_BASE + irq, target_cpu);
            irq_affinity_table[i].current_cpu = target_cpu;
            return 0;
        }
    }

    /* 新增条目 */
    if (irq_affinity_count < MAX_IRQ_BALANCE) {
        irq_affinity_table[irq_affinity_count].irq = irq;
        irq_affinity_table[irq_affinity_count].target_cpu = target_cpu;
        irq_affinity_table[irq_affinity_count].current_cpu = target_cpu;
        irq_affinity_table[irq_affinity_count].flags = 0;
        ioapic_set_irq(irq, IRQ_BASE + irq, target_cpu);
        irq_affinity_count++;
        return 0;
    }

    return -1;
}

/* 获取 IRQ 亲和性 */
int apic_get_irq_affinity(uint8_t irq, uint8_t *target_cpu) {
    if (!target_cpu) return -1;

    for (int i = 0; i < irq_affinity_count; i++) {
        if (irq_affinity_table[i].irq == irq) {
            *target_cpu = irq_affinity_table[i].current_cpu;
            return 0;
        }
    }

    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * IRQ 节流 - 限制风暴设备的中断速率
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 节流表 */
static struct irq_throttle_config irq_throttle_table[MAX_IRQ_STATS];
static int irq_throttle_count = 0;

/* 设置 IRQ 节流 */
int apic_set_irq_throttle(uint8_t irq, uint32_t max_rate_per_sec) {
    /* 查找现有条目 */
    for (int i = 0; i < irq_throttle_count; i++) {
        if (irq_throttle_table[i].max_rate > 0 &&
            (uint8_t)(irq_throttle_table[i].window_start & 0xFF) == irq) {
            irq_throttle_table[i].max_rate = max_rate_per_sec;
            irq_throttle_table[i].enabled = (max_rate_per_sec > 0) ? 1 : 0;
            irq_throttle_table[i].window_count = 0;
            return 0;
        }
    }

    /* 新增条目 */
    if (irq_throttle_count < MAX_IRQ_STATS) {
        irq_throttle_table[irq_throttle_count].max_rate = max_rate_per_sec;
        irq_throttle_table[irq_throttle_count].current_rate = 0;
        irq_throttle_table[irq_throttle_count].window_start = get_tick_count();
        irq_throttle_table[irq_throttle_count].window_count = 0;
        irq_throttle_table[irq_throttle_count].enabled = (max_rate_per_sec > 0) ? 1 : 0;
        irq_throttle_count++;
        return 0;
    }

    return -1;
}

/* 检查 IRQ 是否应该处理 (节流检查)
 * 返回: 1=应处理, 0=应丢弃
 */
int apic_irq_throttle_check(uint8_t irq) {
    /* 查找节流配置 */
    for (int i = 0; i < irq_throttle_count; i++) {
        /* 使用 window_start 的低字节作为 IRQ 标识 (简化) */
        if (!irq_throttle_table[i].enabled) continue;

        /* 通过遍历匹配 - 这里用简单方式查找 */
    }

    /* 使用更直接的方式: 遍历所有节流项 */
    for (int i = 0; i < irq_throttle_count; i++) {
        if (!irq_throttle_table[i].enabled) continue;

        uint64_t now = get_tick_count();

        /* 检查时间窗口是否过期 (1秒 = 1000 tick) */
        if (now - irq_throttle_table[i].window_start >= 1000) {
            irq_throttle_table[i].window_start = now;
            irq_throttle_table[i].window_count = 0;
            irq_throttle_table[i].current_rate = 0;
        }

        /* 检查是否超过限制 */
        if (irq_throttle_table[i].window_count >= irq_throttle_table[i].max_rate) {
            irq_throttle_table[i].current_rate = irq_throttle_table[i].window_count;
            return 0;  /* 丢弃中断 */
        }

        irq_throttle_table[i].window_count++;
    }

    return 1;  /* 允许处理 */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 中断统计 - 每个 IRQ 的计数、处理时间
 * ═══════════════════════════════════════════════════════════════════════════ */

/* IRQ 统计表 */
static struct irq_stats_entry irq_stats_table[MAX_IRQ_STATS];
static spinlock_t irq_stats_lock = SPINLOCK_INIT;

/* 记录 IRQ 处理开始 (返回起始 TSC) */
uint64_t apic_irq_stats_begin(uint8_t irq) {
    if ((int)irq >= MAX_IRQ_STATS) return 0;

    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));

    spin_lock(&irq_stats_lock);
    irq_stats_table[irq].count++;
    irq_stats_table[irq].last_timestamp = tsc;
    spin_unlock(&irq_stats_lock);

    return tsc;
}

/* 记录 IRQ 处理结束 */
void apic_irq_stats_end(uint8_t irq, uint64_t start_tsc) {
    if ((int)irq >= MAX_IRQ_STATS) return;

    uint64_t end_tsc;
    asm volatile("rdtsc" : "=A"(end_tsc));

    /* 将 TSC 周期转换为近似纳秒 (假设 TSC 频率已知) */
    uint64_t cycles = end_tsc - start_tsc;
    uint64_t tsc_freq = cpu_get_tsc_frequency();
    uint64_t time_ns = 0;
    if (tsc_freq > 0) {
        time_ns = (cycles * 1000000000ULL) / tsc_freq;
    }

    spin_lock(&irq_stats_lock);
    irq_stats_table[irq].total_time_ns += time_ns;
    if (time_ns > irq_stats_table[irq].max_time_ns) {
        irq_stats_table[irq].max_time_ns = time_ns;
    }
    spin_unlock(&irq_stats_lock);
}

/* 获取 IRQ 统计信息 */
int apic_get_irq_stats(uint8_t irq, struct irq_stats_entry *stats) {
    if (!stats || (int)irq >= MAX_IRQ_STATS) return -1;

    spin_lock(&irq_stats_lock);
    *stats = irq_stats_table[irq];
    spin_unlock(&irq_stats_lock);

    return 0;
}
