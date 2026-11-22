#include "cpu.h"
#include "ktypes.h"
#include "console.h"
#include "string.h"

/* TSC 频率 */
static uint64_t tsc_frequency = 0;

/* 使用 CPUID 获取 TSC 频率 (如果支持) */
static uint64_t get_tsc_frequency_from_cpuid(void) {
    uint32_t eax, ebx, ecx, edx;
    
    /* 检查是否支持 CPUID leaf 0x15 */
    asm volatile("cpuid" 
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(0));
    
    if (eax < 0x15) {
        return 0;  /* 不支持 */
    }
    
    /* CPUID leaf 0x15: TSC/Core Crystal Clock Information */
    asm volatile("cpuid" 
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(0x15));
    
    uint32_t denominator = eax;
    uint32_t numerator = ebx;
    uint32_t core_freq = ecx;  /* Core Crystal Clock Frequency in Hz */
    
    if (denominator == 0 || numerator == 0 || core_freq == 0) {
        return 0;
    }
    
    return (uint64_t)core_freq * numerator / denominator;
}

/* PIT 端口定义 */
#define PIT_CHANNEL0    0x40
#define PIT_CHANNEL2    0x42
#define PIT_COMMAND     0x43
#define PIT_FREQ        1193182ULL  /* PIT 时钟频率 */

/* inb/outb 已在 cpu.h 中定义 */

/* 使用 PIT 精确校准 TSC */
static uint64_t calibrate_tsc(void) {
    uint64_t start, end;
    
    /* 首先检查 PIT 是否可用 */
    /* 在 UEFI 环境下，PIT 端口可能返回 0xFF */
    uint8_t test_port = inb(0x61);
    if ((test_port & 0xF0) == 0xF0) {
        /* PIT 可能不可用，返回默认值 */
        // println("    PIT not available, using default TSC frequency");
        return 3000000000ULL;  /* 默认 3 GHz */
    }
    
    /* 禁用中断以获得更精确的测量 */
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags));
    
    /* 配置 PIT Channel 2: 模式 0 (中断终端计数), 二进制计数 */
    /* 先禁用 PC 扬声器 (channel 2 gate) */
    uint8_t port_b = inb(0x61);
    outb(0x61, port_b & 0x0C);  /* 清除 bit 0 和 1 */
    
    /* 设置 PIT Channel 2 为模式 0，低字节后高字节 */
    outb(PIT_COMMAND, 0xB2);  /* 10 110 010: channel 2, mode 0, lobyte/hibyte */
    
    /* 设置计数值为 11932 (约 10ms: 1193182 / 100 = 11931.82) */
    uint16_t count = 11932;
    outb(PIT_CHANNEL2, count & 0xFF);         /* 低字节 */
    outb(PIT_CHANNEL2, (count >> 8) & 0xFF);  /* 高字节 */
    
    /* 启用 Channel 2 gate (speaker) */
    port_b = inb(0x61);
    outb(0x61, (port_b & 0x0C) | 0x01);  /* 设置 bit 0 */
    
    /* 读取起始 TSC */
    start = rdtsc();
    
    /* 等待 PIT 计数完成 (bit 5 of port 0x61) */
    /* 添加超时保护 */
    int timeout = 10000000;  /* 约 1 秒超时 */
    while (!(inb(0x61) & 0x20)) {
        asm volatile("pause");
        if (--timeout <= 0) {
            /* PIT 超时，恢复中断并返回默认值 */
            asm volatile("push %0; popfq" :: "r"(flags));
            // println("    PIT calibration timeout, using default");
            return 3000000000ULL;  /* 默认 3 GHz */
        }
    }
    
    /* 读取结束 TSC */
    end = rdtsc();
    
    /* 恢复中断状态 */
    asm volatile("push %0; popfq" :: "r"(flags));
    
    /* 计算实际的 TSC 频率 */
    /* 等待时间 = 11932 / 1193182 秒 ≈ 10ms */
    uint64_t elapsed = end - start;
    uint64_t freq = (elapsed * PIT_FREQ) / count;
    
    /* 验证频率是否在合理范围内 (100 MHz - 10 GHz) */
    if (freq < 100000000ULL || freq > 10000000000ULL) {
        // print("    Warning: Unusual TSC frequency ");
        // print_dec(freq / 1000000);
        // println(" MHz, using default");
        return 3000000000ULL;  /* 默认 3 GHz */
    }
    
    return freq;
}

uint64_t cpu_get_tsc_frequency(void) {
    return tsc_frequency;
}

void cpu_init(void) {
    /* 尝试从 CPUID 获取 TSC 频率 */
    tsc_frequency = get_tsc_frequency_from_cpuid();
    
    if (tsc_frequency == 0) {
        /* 使用校准方法 */
        tsc_frequency = calibrate_tsc();
    }
    
    // print("    TSC Frequency: ");
    // print_dec(tsc_frequency / 1000000);
    // println(" MHz");
}

/* 启用 CPU 安全特性 */
void cpu_enable_security_features(void) {
    // println("[CPU] Enabling security features...");
    
    /* 启用 NX 位 (不可执行) */
    cpu_enable_nx();
    // println("    NX (No-Execute): Enabled");
    
    /* 启用全局页 */
    cpu_enable_pge();
    // println("    PGE (Global Pages): Enabled");
    
    /* 检查并启用 SMEP */
    if (cpu_has_smep()) {
        cpu_enable_smep();
        // println("    SMEP (Supervisor Execution Protection): Enabled");
    } else {
        // println("    SMEP: Not supported");
    }
    
    /* 检查并启用 SMAP */
    if (cpu_has_smap()) {
        cpu_enable_smap();
        // println("    SMAP (Supervisor Access Protection): Enabled");
    } else {
        // println("    SMAP: Not supported");
    }
    
    /* 检查并启用 UMIP */
    if (cpu_has_umip()) {
        cpu_enable_umip();
        // println("    UMIP (User Mode Instruction Protection): Enabled");
    } else {
        // println("    UMIP: Not supported");
    }
    
    // println("[CPU] Security features initialized");
}

/* 打印 CPU 特性信息 */
void cpu_print_features(void) {
    // println("[CPU] Feature Detection:");
    
    /* SMEP */
    // print("    SMEP: ");
    println(cpu_has_smep() ? "Supported" : "Not supported");
    
    /* SMAP */
    // print("    SMAP: ");
    println(cpu_has_smap() ? "Supported" : "Not supported");
    
    /* UMIP */
    // print("    UMIP: ");
    println(cpu_has_umip() ? "Supported" : "Not supported");
    
    /* FSGSBASE */
    // print("    FSGSBASE: ");
    println(cpu_has_fsgsbase() ? "Supported" : "Not supported");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全的用户空间访问函数
 * copy_from_user / copy_to_user: 在 SMAP 保护下安全地访问用户空间内存
 *
 * 使用 stac/clac 包裹访问，并使用异常表处理缺页恢复
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 从用户空间安全复制数据到内核 */
int copy_from_user(void *kernel_dst, const void *user_src, unsigned long size) {
    if (!kernel_dst || !user_src || size == 0) return -1;

    /* 验证用户空间地址范围 */
    if ((uint64_t)user_src >= 0x800000000000ULL) {
        return -1;  /* 不是用户空间地址 */
    }

    /* 临时允许内核访问用户空间 */
    stac();

    /* 逐字节复制，使用异常表处理缺页 */
    unsigned long copied = 0;
    uint8_t *dst = (uint8_t *)kernel_dst;
    const uint8_t *src = (const uint8_t *)user_src;

    while (copied < size) {
        /* 使用异常表安全读取 */
        int ok = 1;
        uint8_t byte = 0;

        __asm__ volatile (
            "1: movb (%[src]), %[byte]\n\t"
            "jmp 2f\n\t"
            ".section .text.fixup, \"ax\"\n\t"
            "3: movl $0, %[ok]\n\t"
            "jmp 2f\n\t"
            ".previous\n\t"
            ".section __ex_table, \"a\"\n\t"
            ".quad 1b, 3b\n\t"
            ".previous\n\t"
            "2:"
            : [byte] "=r" (byte), [ok] "+r" (ok)
            : [src] "r" (src + copied)
            : "memory"
        );

        if (!ok) {
            break;  /* 访问失败 */
        }

        dst[copied] = byte;
        copied++;
    }

    /* 恢复 SMAP 保护 */
    clac();

    /* 记录审计 (如果启用) */
    cpu_audit_user_access((uint64_t)user_src, 0, 1);

    return (copied == size) ? 0 : -1;
}

/* 从内核安全复制数据到用户空间 */
int copy_to_user(void *user_dst, const void *kernel_src, unsigned long size) {
    if (!user_dst || !kernel_src || size == 0) return -1;

    /* 验证用户空间地址范围 */
    if ((uint64_t)user_dst >= 0x800000000000ULL) {
        return -1;  /* 不是用户空间地址 */
    }

    /* 临时允许内核访问用户空间 */
    stac();

    /* 逐字节复制，使用异常表处理缺页 */
    unsigned long copied = 0;
    uint8_t *dst = (uint8_t *)user_dst;
    const uint8_t *src = (const uint8_t *)kernel_src;

    while (copied < size) {
        /* 使用异常表安全写入 */
        int ok = 1;

        __asm__ volatile (
            "1: movb %[byte], (%[dst])\n\t"
            "jmp 2f\n\t"
            ".section .text.fixup, \"ax\"\n\t"
            "3: movl $0, %[ok]\n\t"
            "jmp 2f\n\t"
            ".previous\n\t"
            ".section __ex_table, \"a\"\n\t"
            ".quad 1b, 3b\n\t"
            ".previous\n\t"
            "2:"
            : [ok] "+r" (ok)
            : [byte] "r" (src[copied]), [dst] "r" (dst + copied)
            : "memory"
        );

        if (!ok) {
            break;  /* 写入失败 */
        }

        copied++;
    }

    /* 恢复 SMAP 保护 */
    clac();

    /* 记录审计 (如果启用) */
    cpu_audit_user_access((uint64_t)user_dst, 1, 1);

    return (copied == size) ? 0 : -1;
}

/* 安全地从用户空间读取一个 64 位值 */
int get_user_long(uint64_t *out, const uint64_t *user_ptr) {
    if (!out || !user_ptr) return -1;
    if ((uint64_t)user_ptr >= 0x800000000000ULL) return -1;

    stac();

    int ok = 1;
    uint64_t val = 0;

    __asm__ volatile (
        "1: movq (%[src]), %[val]\n\t"
        "jmp 2f\n\t"
        ".section .text.fixup, \"ax\"\n\t"
        "3: movl $0, %[ok]\n\t"
        "jmp 2f\n\t"
        ".previous\n\t"
        ".section __ex_table, \"a\"\n\t"
        ".quad 1b, 3b\n\t"
        ".previous\n\t"
        "2:"
        : [val] "=r" (val), [ok] "+r" (ok)
        : [src] "r" (user_ptr)
        : "memory"
    );

    clac();

    if (!ok) return -1;

    *out = val;
    return 0;
}

/* 安全地向用户空间写入一个 64 位值 */
int put_user_long(uint64_t *user_ptr, uint64_t val) {
    if (!user_ptr) return -1;
    if ((uint64_t)user_ptr >= 0x800000000000ULL) return -1;

    stac();

    int ok = 1;

    __asm__ volatile (
        "1: movq %[val], (%[dst])\n\t"
        "jmp 2f\n\t"
        ".section .text.fixup, \"ax\"\n\t"
        "3: movl $0, %[ok]\n\t"
        "jmp 2f\n\t"
        ".previous\n\t"
        ".section __ex_table, \"a\"\n\t"
        ".quad 1b, 3b\n\t"
        ".previous\n\t"
        "2:"
        : [ok] "+r" (ok)
        : [val] "r" (val), [dst] "r" (user_ptr)
        : "memory"
    );

    clac();

    return ok ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 用户空间访问审计
 * 记录意外的内核对用户内存的访问，用于安全分析
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 审计日志缓冲区 */
static struct user_access_audit_entry user_access_audit_buffer[USER_ACCESS_AUDIT_SIZE];
static uint32_t user_access_audit_index = 0;
static int user_access_audit_enabled = 0;
static spinlock_t user_access_audit_lock = SPINLOCK_INIT;

/* 记录用户空间访问审计事件 */
void cpu_audit_user_access(uint64_t user_addr, int access_type, int expected) {
    if (!user_access_audit_enabled) return;

    spin_lock(&user_access_audit_lock);

    uint32_t idx = user_access_audit_index % USER_ACCESS_AUDIT_SIZE;
    user_access_audit_buffer[idx].timestamp = rdtsc();
    user_access_audit_buffer[idx].kernel_rip = 0;  /* 调用者可以设置 */
    user_access_audit_buffer[idx].user_addr = user_addr;
    user_access_audit_buffer[idx].access_type = access_type;
    user_access_audit_buffer[idx].expected = expected;

    user_access_audit_index++;

    spin_unlock(&user_access_audit_lock);
}

/* 获取用户空间访问审计日志 */
int cpu_get_user_access_audit(struct user_access_audit_entry *entries, int max_count) {
    if (!entries || max_count <= 0) return -1;

    spin_lock(&user_access_audit_lock);

    uint32_t count = user_access_audit_index < USER_ACCESS_AUDIT_SIZE ?
                     user_access_audit_index : USER_ACCESS_AUDIT_SIZE;
    if ((int)count > max_count) count = (uint32_t)max_count;

    uint32_t start = user_access_audit_index < USER_ACCESS_AUDIT_SIZE ?
                     0 : user_access_audit_index % USER_ACCESS_AUDIT_SIZE;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t src_idx = (start + i) % USER_ACCESS_AUDIT_SIZE;
        entries[i] = user_access_audit_buffer[src_idx];
    }

    spin_unlock(&user_access_audit_lock);
    return (int)count;
}

/* 启用/禁用用户空间访问审计 */
void cpu_set_user_access_audit_enabled(int enabled) {
    spin_lock(&user_access_audit_lock);
    user_access_audit_enabled = enabled;
    if (enabled) {
        user_access_audit_index = 0;
    }
    spin_unlock(&user_access_audit_lock);
}

/* 检查用户空间访问审计是否启用 */
int cpu_is_user_access_audit_enabled(void) {
    return user_access_audit_enabled;
}
