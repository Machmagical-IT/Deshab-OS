/*
 * Zratro OS - SMP (Symmetric Multi-Processing) Support
 * 
 * 多核 CPU 支持模块
 */

#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "task.h"
#include "idt.h"  /* for struct interrupt_frame */

/* 最大 CPU 数量 */
#define MAX_CPUS        256

/* CPU 掩码类型 */
typedef struct {
    uint64_t bits[MAX_CPUS / 64];
} cpu_mask_t;

/* CPU 状态 */
typedef enum {
    CPU_STATE_OFFLINE = 0,
    CPU_STATE_STARTING,
    CPU_STATE_RUNNING,
    CPU_STATE_HALTED
} cpu_state_t;

/* CPU 缓存行大小 */
#define CACHE_LINE_SIZE 64

/* 每 CPU 数据结构 (通过 GS 段基址访问)
 * 
 * 重要：struct cpu_local 的布局必须保证 stack_canary 在偏移 0x28 (40 字节)
 * 因为 GCC 的 -fstack-protector-strong 会访问 gs:0x28 来读取栈 canary 值
 */
struct cpu_local {
    /* 基本信息 */
    uint32_t cpu_id;              /* 偏移 0x00: CPU 编号 */
    uint32_t apic_id;             /* 偏移 0x04: Local APIC ID */
    cpu_state_t state;            /* 偏移 0x08: CPU 状态 */
    
    /* 栈 canary - 必须在偏移 0x28 (40 字节) */
    /* GCC -fstack-protector-strong 会访问 gs:0x28 读取 canary */
    /* 需要填充 40 - 12 = 28 字节 = 三个 uint64_t + 一个 uint32_t */
    uint32_t __pad_0c;            /* 偏移 0x0C: 填充 (4 字节) */
    uint64_t __pad_10;            /* 偏移 0x10: 填充 (8 字节) */
    uint64_t __pad_18;            /* 偏移 0x18: 填充 (8 字节) */
    uint64_t __pad_20;            /* 偏移 0x20: 填充 (8 字节) */
    uint64_t stack_canary;        /* 偏移 0x28: 栈 canary 值 (gs:0x28) */
    
    /* 调度相关 */
    struct thread *current_thread;  /* 当前运行的线程 */
    struct thread *idle_thread;     /* 空闲线程 */
    uint64_t schedule_count;        /* 调度次数 */
    
    /* 中断相关 */
    uint64_t irq_count;             /* 中断计数 */
    uint64_t softirq_pending;       /* 软中断待处理位图 */
    int interrupt_depth;            /* 中断嵌套深度 */
    
    /* 自旋锁相关 */
    int spinlock_count;             /* 自旋锁计数 (用于禁用抢占) */
    int preempt_count;              /* 抢占计数 */
    
    /* RCU 相关 */
    int rcu_nesting;                /* RCU 读锁嵌套深度 */
    
    /* 时间相关 */
    uint64_t last_tick;             /* 上次 tick 时间 */
    uint64_t steal_time;            /* 被偷走的时间 */
    
    /* 统计 */
    uint64_t context_switches;      /* 上下文切换次数 */
    uint64_t syscalls;              /* 系统调用次数 */
    uint64_t page_faults;           /* 页错误次数 */
    
    /* 缓存行对齐填充 */
    char __pad[36];  /* 固定大小的填充 */
} __attribute__((aligned(CACHE_LINE_SIZE)));

/* 编译时验证 stack_canary 的偏移量 */
_Static_assert(offsetof(struct cpu_local, stack_canary) == 0x28,
    "stack_canary must be at offset 0x28 for gs:0x28 access!");

/* AP 启动信息结构 (传递给 AP 处理器) */
struct smp_boot_info {
    uint64_t kernel_entry;        /* 内核入口地址 */
    uint64_t pml4;                /* 页表基址 */
    uint64_t stack;               /* 内核栈地址 */
    uint64_t cpu_local;           /* CPU 本地数据地址 */
    volatile int ready;           /* AP 就绪标志 */
    volatile int error;           /* 错误码 */
    uint32_t cpu_id;              /* CPU ID */
    uint32_t apic_id;             /* APIC ID */
} __attribute__((packed));

/* IPI 消息类型
 * 注意：向量号必须避开：
 *   - CPU 异常 (0-31)
 *   - IRQ (32-47)
 *   - APIC 定时器/错误/热/性能 (48-53)
 *   - 系统调用 (0x80)
 *   - APIC 伪中断 (255)
 * 使用 0x60-0x6F 范围
 */
typedef enum {
    IPI_RESCHEDULE    = 0x60,    /* 重新调度 */
    IPI_CALL_FUNCTION = 0x61,    /* 函数调用 */
    IPI_CPU_STOP      = 0x62,    /* 停止 CPU */
    IPI_CPU_START     = 0x63,    /* 启动 CPU */
    IPI_TLB_SHOOTDOWN = 0x64,    /* TLB 刷新 */
    IPI_MEM_BARRIER   = 0x65,    /* 内存屏障 */
    /* 双内核通信 IPI (0x70-0x7F 保留) */
    IPI_ZTKNL_TO_AZKNL = 0x70,   /* Ztknl -> Azknl 通知 */
    IPI_AZKNL_TO_ZTKNL = 0x71,   /* Azknl -> Ztknl 通知 */
} ipi_type_t;

/* IPI 调用函数 */
typedef void (*smp_call_func_t)(void *arg);

struct smp_call_data {
    smp_call_func_t func;
    void *arg;
    volatile int done;
    volatile int wait;
};

/* 全局变量 */
extern int smp_cpu_count;
extern struct cpu_local cpu_data[];

/* ========== 初始化 ========== */

/* 初始化 SMP 子系统 */
int smp_init(void);

/* 启动所有 AP */
int smp_start_aps(void);

/* 启动单个 CPU */
int smp_start_cpu(uint32_t apic_id);

/* 停止 CPU */
void smp_stop_cpu(int cpu_id);

/* ========== IPI 操作 ========== */

/* 发送 IPI */
void smp_send_ipi(uint32_t apic_id, uint8_t vector);

/* 发送广播 IPI */
void smp_broadcast_ipi(uint8_t vector);

/* 发送重新调度 IPI */
void smp_send_reschedule(int cpu_id);

/* 在其他 CPU 上执行函数 */
int smp_call_function(int cpu_id, smp_call_func_t func, void *arg, int wait);

/* 在所有 CPU 上执行函数 */
void smp_call_function_all(smp_call_func_t func, void *arg, int wait);

/* ========== TLB 刷新 ========== */

/* 刷新指定地址的 TLB */
void smp_flush_tlb(uint64_t addr);

/* 刷新整个 TLB */
void smp_flush_tlb_all(void);

/* ========== CPU 信息 ========== */

/* 获取当前 CPU 数据 */
static inline struct cpu_local *this_cpu(void) {
    /* 直接从 IA32_GS_BASE MSR 读取段基址 */
    uint64_t gs_base;
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"((uint32_t)0xC0000101));
    gs_base = ((uint64_t)high << 32) | low;
    return (struct cpu_local *)gs_base;
}

/* 获取当前 CPU ID */
static inline uint32_t smp_processor_id(void) {
    return this_cpu()->cpu_id;
}

/* 获取 CPU 数量 */
static inline int smp_get_cpu_count(void) {
    return smp_cpu_count;
}

/* 获取 CPU 数据 */
static inline struct cpu_local *cpu_data_ptr(int cpu_id) {
    if (cpu_id >= 0 && cpu_id < MAX_CPUS) {
        return &cpu_data[cpu_id];
    }
    return NULL;
}

/* ========== 抢占控制 ========== */

/* 禁用抢占 */
static inline void preempt_disable(void) {
    struct cpu_local *cpu = this_cpu();
    cpu->preempt_count++;
}

/* 启用抢占 */
static inline void preempt_enable(void) {
    struct cpu_local *cpu = this_cpu();
    cpu->preempt_count--;
}

/* 检查是否可抢占 */
static inline int preemptible(void) {
    struct cpu_local *cpu = this_cpu();
    return cpu->preempt_count == 0 && cpu->interrupt_depth == 0;
}

/* ========== 中断上下文检测 ========== */

/* 检查是否在中断上下文中 */
static inline int in_interrupt(void) {
    struct cpu_local *cpu = this_cpu();
    return cpu->interrupt_depth > 0;
}

/* 检查是否在硬中断上下文中 */
static inline int in_irq(void) {
    struct cpu_local *cpu = this_cpu();
    return cpu->interrupt_depth > 0 && cpu->spinlock_count > 0;
}

/* 检查是否在软中断上下文中 */
static inline int in_softirq(void) {
    struct cpu_local *cpu = this_cpu();
    return (cpu->softirq_pending & 0xFF) != 0;
}

/* 检查是否在原子上下文中 (不能睡眠) */
static inline int in_atomic(void) {
    struct cpu_local *cpu = this_cpu();
    return cpu->preempt_count > 0 || cpu->spinlock_count > 0 || cpu->interrupt_depth > 0;
}

/* 中断进入 */
static inline void irq_enter(void) {
    struct cpu_local *cpu = this_cpu();
    cpu->interrupt_depth++;
    cpu->irq_count++;
}

/* 中断退出 */
static inline void irq_exit(void) {
    struct cpu_local *cpu = this_cpu();
    if (cpu->interrupt_depth > 0) {
        cpu->interrupt_depth--;
    }
}

/* 检查是否可以睡眠 */
static inline int can_sleep(void) {
    return !in_atomic() && !in_interrupt();
}

/* 断言不在中断上下文 */
#define BUG_ON_IN_IRQ() do { \
    if (in_interrupt()) { \
        println("[BUG] Function called from interrupt context!"); \
        asm volatile("cli; hlt"); \
    } \
} while(0)

/* 断言可以睡眠 */
#define BUG_ON_CANT_SLEEP() do { \
    if (!can_sleep()) { \
        println("[BUG] Sleeping function called in atomic context!"); \
        asm volatile("cli; hlt"); \
    } \
} while(0)

/* ========== 自旋锁辅助 ========== */

/* 自旋锁获取时禁用抢占 */
static inline void spin_lock_nested(void) {
    struct cpu_local *cpu = this_cpu();
    cpu->spinlock_count++;
}

/* 自旋锁释放时检查抢占 */
static inline void spin_unlock_nested(void) {
    struct cpu_local *cpu = this_cpu();
    cpu->spinlock_count--;
    /* 如果可抢占且需要调度，设置需要调度标志
     * 注意：不使用软中断触发调度，因为可能与 APIC 定时器向量冲突
     * 实际调度会在时钟中断或下一次内核入口时发生
     */
    if (preemptible() && cpu->current_thread && 
        cpu->current_thread->time_slice <= 0) {
        /* 设置需要调度标志，让调度器在下一次机会时调度 */
        cpu->current_thread->flags |= THREAD_FLAG_NEED_RESCHED;
    }
}

/* ========== CPU 本地存储 ========== */

/* 设置 GS 段基址 */
static inline void set_cpu_local(struct cpu_local *cpu) {
    uint64_t addr = (uint64_t)cpu;
    asm volatile("wrmsr" : :
        "c"((uint32_t)0xC0000101),  /* IA32_GS_BASE */
        "a"((uint32_t)addr),
        "d"((uint32_t)(addr >> 32)));
}

/* ========== 内存屏障 ========== */

/* 使用 ktypes.h 中定义的屏障宏 */
#ifndef smp_mb
#define smp_mb()    asm volatile("mfence" ::: "memory")
#endif
#ifndef smp_rmb
#define smp_rmb()   asm volatile("lfence" ::: "memory")
#endif
#ifndef smp_wmb
#define smp_wmb()   asm volatile("sfence" ::: "memory")
#endif

/* 编译器屏障 */
#ifndef barrier
#define barrier()   asm volatile("" ::: "memory")
#endif

/* 原子加 */
static inline int atomic_add_return(int i, volatile int *v) {
    return __sync_add_and_fetch(v, i);
}

/* 原子减 */
static inline int atomic_sub_return(int i, volatile int *v) {
    return __sync_sub_and_fetch(v, i);
}

/* 原子交换 (取消 ktypes.h 宏定义避免冲突) */
#undef atomic_xchg
static inline int atomic_xchg(volatile int *v, int new_val) {
    return __sync_lock_test_and_set(v, new_val);
}

/* 比较并交换 (取消 ktypes.h 宏定义避免冲突) */
#undef atomic_cmpxchg
static inline int atomic_cmpxchg(volatile int *v, int old_val, int new_val) {
    return __sync_val_compare_and_swap(v, old_val, new_val);
}

/* ========== CPU 负载统计 ========== */

/* 获取 CPU 负载 */
uint64_t smp_get_load(int cpu_id);

/* 获取系统总负载 */
uint64_t smp_get_system_load(void);

/* 更新 CPU 负载 */
void smp_update_load(int cpu_id, int idle);

/* ========== CPU 热插拔 ========== */

/* CPU 上线 */
int smp_cpu_online(int cpu_id);

/* CPU 下线 */
int smp_cpu_offline(int cpu_id);

/* ========== CPU 掩码操作 ========== */

void cpumask_set(cpu_mask_t *mask, int cpu);
void cpumask_clear(cpu_mask_t *mask, int cpu);
int cpumask_test(const cpu_mask_t *mask, int cpu);
void cpumask_copy(cpu_mask_t *dst, const cpu_mask_t *src);
int cpumask_weight(const cpu_mask_t *mask);
int cpumask_first(const cpu_mask_t *mask);

/* ========== IPI 广播 ========== */

/* 向所有其他 CPU 发送 IPI */
void smp_broadcast_others(uint8_t vector);

/* 向所有 CPU 发送 IPI */
void smp_broadcast_all(uint8_t vector);

/* 向指定 CPU 掩码发送 IPI */
void smp_broadcast_mask(const cpu_mask_t *mask, uint8_t vector);

/* ========== SMP 调试 ========== */

void smp_dump_state(void);

/* ========== IPI 处理程序 ========== */

/* IPI 重新调度处理程序 */
void smp_reschedule_handler(struct interrupt_frame *frame);

/* IPI 函数调用处理程序 */
void smp_call_function_interrupt(struct interrupt_frame *frame);

/* IPI CPU 停止处理程序 */
void smp_cpu_stop_handler(struct interrupt_frame *frame);

/* IPI TLB 刷新处理程序 */
void smp_tlb_shootdown_handler(struct interrupt_frame *frame);

/* IPI 内存屏障处理程序 */
void smp_mem_barrier_handler(struct interrupt_frame *frame);

#endif /* SMP_H */
