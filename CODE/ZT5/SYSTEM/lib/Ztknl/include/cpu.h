#ifndef CPU_H
#define CPU_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * CPU 特性标志位
 * ═══════════════════════════════════════════════════════════════════════════ */

/* CR4 控制寄存器位定义 */
#define CR4_VME        (1 << 0)    /* 虚拟 8086 模式扩展 */
#define CR4_PVI        (1 << 1)    /* 保护模式虚拟中断 */
#define CR4_TSD        (1 << 2)    /* 时间戳禁用 */
#define CR4_DE         (1 << 3)    /* 调试扩展 */
#define CR4_PSE        (1 << 4)    /* 页面大小扩展 */
#define CR4_PAE        (1 << 5)    /* 物理地址扩展 */
#define CR4_MCE        (1 << 6)    /* 机器检查异常 */
#define CR4_PGE        (1 << 7)    /* 页面全局启用 */
#define CR4_PCE        (1 << 8)    /* 性能计数器启用 */
#define CR4_OSFXSR     (1 << 9)    /* FXSAVE/FXRSTOR 支持 */
#define CR4_OSXMMEXCPT (1 << 10)   /* SIMD 浮点异常支持 */
#define CR4_UMIP       (1 << 11)   /* 用户模式指令防护 */
#define CR4_LA57       (1 << 12)   /* 5 级分页 */
#define CR4_VMXE       (1 << 13)   /* VMX 启用 */
#define CR4_SMXE       (1 << 14)   /* SMX 启用 */
#define CR4_FSGSBASE   (1 << 16)   /* FS/GS BASE 指令启用 */
#define CR4_PCIDE      (1 << 17)   /* PCID 启用 */
#define CR4_OSXSAVE    (1 << 18)   /* XSAVE 启用 */
#define CR4_SMEP       (1 << 20)   /* 管理员模式执行保护 */
#define CR4_SMAP       (1 << 21)   /* 管理员模式访问保护 */
#define CR4_PKE        (1 << 22)   /* 保护密钥启用 */

/* CPUID 特性标志 */
#define CPUID_FEATURE_SMEP    (1 << 7)    /* EBX, leaf 7, subleaf 0 */
#define CPUID_FEATURE_SMAP    (1 << 20)   /* EBX, leaf 7, subleaf 0 */
#define CPUID_FEATURE_FSGSBASE (1 << 0)   /* EBX, leaf 7, subleaf 0 */
#define CPUID_FEATURE_UMIP    (1 << 2)    /* ECX, leaf 7, subleaf 0 */

/* EFER MSR 位定义 */
#define MSR_EFER             0xC0000080
#define EFER_SCE             (1 << 0)    /* 系统调用扩展 */
#define EFER_LME             (1 << 8)    /* 长模式启用 */
#define EFER_LMA             (1 << 10)   /* 长模式激活 */
#define EFER_NXE             (1 << 11)   /* 不可执行位启用 */
#define EFER_SVME            (1 << 12)   /* SVM 启用 */
#define EFER_LMSLE           (1 << 13)   /* 长模式段限制启用 */
#define EFER_FFXSR           (1 << 14)   /* 快速 FXSAVE/FXRSTOR */
#define EFER_TCE             (1 << 15)   /* 翻译缓存扩展 */

/* 缓存行大小 */
#define CACHE_LINE_SIZE      64

/* ═══════════════════════════════════════════════════════════════════════════
 * 内联函数
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 禁用中断 */
static inline void cli(void) {
    asm volatile("cli");
}

/* 启用中断 */
static inline void sti(void) {
    asm volatile("sti");
}

/* 暂停 CPU */
static inline void hlt(void) {
    asm volatile("hlt");
}

/* 读取 CR0 */
static inline uint64_t read_cr0(void) {
    uint64_t value;
    asm volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

/* 写入 CR0 */
static inline void write_cr0(uint64_t value) {
    asm volatile("mov %0, %%cr0" :: "r"(value));
}

/* 读取 CR2 */
static inline uint64_t read_cr2(void) {
    uint64_t value;
    asm volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

/* 读取 CR3 */
static inline uint64_t read_cr3(void) {
    uint64_t value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

/* 写入 CR3 */
static inline void write_cr3(uint64_t value) {
    asm volatile("mov %0, %%cr3" :: "r"(value));
}

/* 读取 CR4 */
static inline uint64_t read_cr4(void) {
    uint64_t value;
    asm volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

/* 写入 CR4 */
static inline void write_cr4(uint64_t value) {
    asm volatile("mov %0, %%cr4" :: "r"(value));
}

/* 读取 MSR */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

/* 写入 MSR */
static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    asm volatile("wrmsr" :: "a"(low), "d"(high), "c"(msr));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SMAP 安全访问函数
 * 当 SMAP 启用时，内核需要使用 stac/clac 来临时允许访问用户空间
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 设置 AC 标志 (Alignment Check)，临时允许内核访问用户空间 */
static inline void stac(void) {
    /* 只有在 SMAP 启用时才执行 stac 指令 */
    if ((read_cr4() & CR4_SMAP) != 0) {
        asm volatile("stac" ::: "memory");
    }
}

/* 清除 AC 标志，恢复 SMAP 保护 */
static inline void clac(void) {
    /* 只有在 SMAP 启用时才执行 clac 指令 */
    if ((read_cr4() & CR4_SMAP) != 0) {
        asm volatile("clac" ::: "memory");
    }
}

/* 检查 SMAP 是否启用 */
static inline int cpu_smap_enabled(void) {
    return (read_cr4() & CR4_SMAP) != 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全的用户空间访问函数
 * copy_from_user / copy_to_user: 在 SMAP 保护下安全地访问用户空间内存
 * 使用 stac/clac 包裹访问，并处理缺页恢复
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 从用户空间安全复制数据到内核
 *
 * 在 SMAP 启用时，使用 stac/clac 临时允许访问用户空间。
 * 如果访问失败 (缺页)，返回已复制的字节数。
 *
 * @param kernel_dst 内核目标地址
 * @param user_src 用户空间源地址
 * @param size 要复制的字节数
 * @return 未复制的字节数 (0=全部成功)
 */
int copy_from_user(void *kernel_dst, const void *user_src, unsigned long size);

/**
 * @brief 从内核安全复制数据到用户空间
 *
 * 在 SMAP 启用时，使用 stac/clac 临时允许访问用户空间。
 * 如果访问失败 (缺页)，返回已复制的字节数。
 *
 * @param user_dst 用户空间目标地址
 * @param kernel_src 内核源地址
 * @param size 要复制的字节数
 * @return 未复制的字节数 (0=全部成功)
 */
int copy_to_user(void *user_dst, const void *kernel_src, unsigned long size);

/**
 * @brief 安全地从用户空间读取一个值
 *
 * @param user_ptr 用户空间地址
 * @param out 输出值
 * @return 0=成功, -1=失败
 */
int get_user_long(uint64_t *out, const uint64_t *user_ptr);

/**
 * @brief 安全地向用户空间写入一个值
 *
 * @param user_ptr 用户空间地址
 * @param val 要写入的值
 * @return 0=成功, -1=失败
 */
int put_user_long(uint64_t *user_ptr, uint64_t val);

/* ═══════════════════════════════════════════════════════════════════════════
 * 用户空间访问审计
 * 记录意外的内核对用户内存的访问
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 访问审计条目 */
struct user_access_audit_entry {
    uint64_t timestamp;        /* 时间戳 */
    uint64_t kernel_rip;       /* 内核访问点 RIP */
    uint64_t user_addr;        /* 被访问的用户空间地址 */
    int access_type;           /* 0=读, 1=写 */
    int expected;              /* 0=意外访问, 1=预期访问 (stac/clac) */
};

/* 审计日志配置 */
#define USER_ACCESS_AUDIT_SIZE  256

/* 记录用户空间访问审计事件 */
void cpu_audit_user_access(uint64_t user_addr, int access_type, int expected);

/* 获取用户空间访问审计日志 */
int cpu_get_user_access_audit(struct user_access_audit_entry *entries, int max_count);

/* 启用/禁用用户空间访问审计 */
void cpu_set_user_access_audit_enabled(int enabled);

/* 检查用户空间访问审计是否启用 */
int cpu_is_user_access_audit_enabled(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * I/O 端口访问函数
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %b0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    asm volatile("inb %w1, %b0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outw(uint16_t port, uint16_t val) {
    asm volatile("outw %w0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    asm volatile("inw %w1, %w0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    asm volatile("inl %w1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* 读取 TSC */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* 获取 TSC 频率 */
uint64_t cpu_get_tsc_frequency(void);

/* CPU 信息初始化 */
void cpu_init(void);

/* 便捷函数 */
static inline void cpu_enable_interrupts(void) {
    sti();
}

static inline void cpu_disable_interrupts(void) {
    cli();
}

static inline void cpu_halt(void) {
    hlt();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全特性函数
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 启用 SMEP (Supervisor Mode Execution Prevention) */
static inline void cpu_enable_smep(void) {
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_SMEP;
    write_cr4(cr4);
}

/* 启用 SMAP (Supervisor Mode Access Prevention) */
static inline void cpu_enable_smap(void) {
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_SMAP;
    write_cr4(cr4);
}

/* 禁用 SMAP (用于访问用户空间内存) */
static inline void cpu_disable_smap(void) {
    uint64_t cr4 = read_cr4();
    cr4 &= ~CR4_SMAP;
    write_cr4(cr4);
}

/* 启用 NX 位 (不可执行) */
static inline void cpu_enable_nx(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_NXE;
    wrmsr(MSR_EFER, efer);
}

/* 启用全局页 */
static inline void cpu_enable_pge(void) {
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_PGE;
    write_cr4(cr4);
}

/* 启用 UMIP (用户模式指令防护) */
static inline void cpu_enable_umip(void) {
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_UMIP;
    write_cr4(cr4);
}

/* STAC - 设置 AC 标志 (允许访问用户空间) */
static inline void cpu_stac(void) {
    asm volatile("stac" ::: "memory");
}

/* CLAC - 清除 AC 标志 (禁止访问用户空间) */
static inline void cpu_clac(void) {
    asm volatile("clac" ::: "memory");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CPUID 函数
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 执行 CPUID */
static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx) {
    asm volatile("cpuid"
                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                 : "a"(leaf), "c"(subleaf));
}

/* 检查 CPUID leaf 是否支持 */
static inline int cpuid_leaf_supported(uint32_t leaf) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    return eax >= leaf;
}

/* 检查 SMEP 支持 */
static inline int cpu_has_smep(void) {
    if (!cpuid_leaf_supported(7)) return 0;
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    return (ebx & CPUID_FEATURE_SMEP) != 0;
}

/* 检查 SMAP 支持 */
static inline int cpu_has_smap(void) {
    if (!cpuid_leaf_supported(7)) return 0;
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    return (ebx & CPUID_FEATURE_SMAP) != 0;
}

/* 检查 UMIP 支持 */
static inline int cpu_has_umip(void) {
    if (!cpuid_leaf_supported(7)) return 0;
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    return (ecx & CPUID_FEATURE_UMIP) != 0;
}

/* 检查 FSGSBASE 支持 */
static inline int cpu_has_fsgsbase(void) {
    if (!cpuid_leaf_supported(7)) return 0;
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    return (ebx & CPUID_FEATURE_FSGSBASE) != 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 缓存优化
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 预取数据 */
#ifndef _CPU_PREFETCH_DEFINED
#define _CPU_PREFETCH_DEFINED
static inline void prefetch(void *addr) {
    asm volatile("prefetcht0 %0" :: "m"(*(char *)addr));
}
#endif

/* 预取到 L1 */
static inline void prefetch_l1(void *addr) {
    asm volatile("prefetcht0 %0" :: "m"(*(char *)addr));
}

/* 预取到 L2 */
static inline void prefetch_l2(void *addr) {
    asm volatile("prefetcht1 %0" :: "m"(*(char *)addr));
}

/* 内存屏障 */
static inline void memory_barrier(void) {
    asm volatile("" ::: "memory");
}

/* 读内存屏障 */
static inline void read_barrier(void) {
    asm volatile("lfence" ::: "memory");
}

/* 写内存屏障 */
static inline void write_barrier(void) {
    asm volatile("sfence" ::: "memory");
}

/* 全内存屏障 */
static inline void full_barrier(void) {
    asm volatile("mfence" ::: "memory");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 缓存行对齐宏
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 强制缓存行对齐 */
#define __aligned_cache __attribute__((aligned(CACHE_LINE_SIZE)))

/* 填充到缓存行大小 */
#define CACHE_LINE_PAD(name) char __cache_pad_##name[CACHE_LINE_SIZE]

/* 避免伪共享的结构体包装 */
#define DEFINE_ALIGNED_TYPE(name, type) \
    struct name { \
        type value; \
        char __pad[CACHE_LINE_SIZE - sizeof(type)]; \
    } __aligned_cache

/* ═══════════════════════════════════════════════════════════════════════════
 * 初始化函数
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 初始化 CPU 安全特性 */
void cpu_enable_security_features(void);

/* 打印 CPU 特性信息 */
void cpu_print_features(void);

#endif
