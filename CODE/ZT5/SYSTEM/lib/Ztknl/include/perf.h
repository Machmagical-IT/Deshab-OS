/*
 * Zratro OS - Performance Counters and Debug Interface
 * 
 * 统一的性能监控和调试接口
 */

#ifndef _PERF_H
#define _PERF_H

#include "ktypes.h"

/* 性能计数器类型 */
enum perf_counter_type {
    /* 调度器 */
    PERF_SCHED_CONTEXT_SWITCHES,
    PERF_SCHED_TASK_CREATES,
    PERF_SCHED_TASK_DELETES,
    PERF_SCHED_PREEMPTIONS,
    PERF_SCHED_LOAD_BALANCE,
    
    /* 内存管理 */
    PERF_MEM_PAGE_FAULTS,
    PERF_MEM_PAGE_ALLOC,
    PERF_MEM_PAGE_FREE,
    PERF_MEM_CACHE_HITS,
    PERF_MEM_CACHE_MISSES,
    PERF_MEM_SLAB_ALLOC,
    PERF_MEM_SLAB_FREE,
    
    /* 文件系统 */
    PERF_FS_READS,
    PERF_FS_WRITES,
    PERF_FS_CACHE_HITS,
    PERF_FS_CACHE_MISSES,
    
    /* 网络 */
    PERF_NET_PACKETS_IN,
    PERF_NET_PACKETS_OUT,
    PERF_NET_BYTES_IN,
    PERF_NET_BYTES_OUT,
    PERF_NET_TCP_CONNECTIONS,
    PERF_NET_TCP_RETRANSMITS,
    
    /* IPC */
    PERF_IPC_MESSAGES,
    PERF_IPC_SIGNALS,
    
    /* RCU */
    PERF_RCU_GP_COUNT,
    PERF_RCU_CALLBACKS,
    
    /* IO */
    PERF_IO_REQUESTS,
    PERF_IO_BYTES,
    
    /* 计数器数量 */
    PERF_COUNTER_COUNT
};

/* 性能事件 */
struct perf_event {
    uint64_t timestamp;
    uint32_t type;
    uint32_t cpu;
    uint64_t value;
    uint64_t aux[2];
};

/* 性能采样 */
struct perf_sample {
    uint64_t count;
    uint64_t total;
    uint64_t min;
    uint64_t max;
    uint64_t last;
};

/* 性能计数器 */
struct perf_counter {
    uint64_t value;
    uint64_t last_value;
    uint64_t rate_per_sec;      /* 每秒变化率 */
    uint32_t flags;
    char name[32];
};

/* 调试级别 */
typedef enum {
    DEBUG_LEVEL_NONE = 0,
    DEBUG_LEVEL_ERROR = 1,
    DEBUG_LEVEL_WARN = 2,
    DEBUG_LEVEL_INFO = 3,
    DEBUG_LEVEL_DEBUG = 4,
    DEBUG_LEVEL_TRACE = 5
} debug_level_t;

/* 调试标志 */
#define DEBUG_FLAG_ENABLED      (1 << 0)
#define DEBUG_FLAG_TRACE        (1 << 1)
#define DEBUG_FLAG_PROFILE      (1 << 2)
#define DEBUG_FLAG_STATS        (1 << 3)

/* 性能统计结构 */
struct perf_stats {
    /* CPU 统计 */
    uint64_t cpu_idle_time;
    uint64_t cpu_busy_time;
    uint64_t cpu_user_time;
    uint64_t cpu_kernel_time;
    uint64_t cpu_irq_time;
    
    /* 内存统计 */
    uint64_t mem_total;
    uint64_t mem_used;
    uint64_t mem_cached;
    uint64_t mem_free;
    uint64_t mem_fragmentation;
    
    /* 调度统计 */
    uint64_t sched_switches;
    uint64_t sched_latency_avg;
    uint64_t sched_latency_max;
    uint64_t runnable_tasks;
    
    /* IO 统计 */
    uint64_t io_reads;
    uint64_t io_writes;
    uint64_t io_read_bytes;
    uint64_t io_write_bytes;
    uint64_t io_queue_depth;
    
    /* 网络统计 */
    uint64_t net_rx_packets;
    uint64_t net_tx_packets;
    uint64_t net_rx_bytes;
    uint64_t net_tx_bytes;
    uint64_t net_errors;
};

/* 全局性能状态 */
struct perf_state {
    struct perf_counter counters[PERF_COUNTER_COUNT];
    struct perf_sample samples[PERF_COUNTER_COUNT];
    struct perf_event *event_buffer;
    uint32_t event_count;
    uint32_t event_capacity;
    uint32_t event_head;
    uint32_t event_tail;
    
    debug_level_t debug_level;
    uint32_t debug_flags;
    char debug_filter[64];
    
    uint64_t start_time;
    uint64_t last_update;
    
    spinlock_t lock;
};

/* ========== 初始化 ========== */

/* 初始化性能监控 */
int perf_init(void);

/* 关闭性能监控 */
void perf_shutdown(void);

/* ========== 计数器操作 ========== */

/* 增加计数器 */
void perf_inc(enum perf_counter_type type);
void perf_inc_by(enum perf_counter_type type, uint64_t value);

/* 读取计数器 */
uint64_t perf_read(enum perf_counter_type type);

/* 重置计数器 */
void perf_reset(enum perf_counter_type type);
void perf_reset_all(void);

/* 更新速率 */
void perf_update_rates(void);

/* ========== 采样 ========== */

/* 开始采样 */
void perf_sample_begin(enum perf_counter_type type);

/* 结束采样 */
uint64_t perf_sample_end(enum perf_counter_type type);

/* 记录采样值 */
void perf_record_sample(enum perf_counter_type type, uint64_t value);

/* 获取采样统计 */
void perf_get_sample_stats(enum perf_counter_type type, struct perf_sample *stats);

/* ========== 事件记录 ========== */

/* 记录事件 */
void perf_event_record(enum perf_counter_type type, uint64_t value);
void perf_event_record_ex(enum perf_counter_type type, uint64_t value,
                          uint64_t aux1, uint64_t aux2);

/* 获取事件 */
int perf_event_get(struct perf_event *event);

/* 清空事件缓冲 */
void perf_event_clear(void);

/* ========== 调试接口 ========== */

/* 设置调试级别 */
void debug_set_level(debug_level_t level);

/* 获取调试级别 */
debug_level_t debug_get_level(void);

/* 设置调试标志 */
void debug_set_flags(uint32_t flags);
void debug_clear_flags(uint32_t flags);

/* 设置调试过滤器 */
void debug_set_filter(const char *filter);

/* 调试打印 */
void debug_print(debug_level_t level, const char *fmt, ...);

/* 调试宏 */
#define DEBUG_ERROR(fmt, ...) \
    debug_print(DEBUG_LEVEL_ERROR, "[ERROR] " fmt, ##__VA_ARGS__)

#define DEBUG_WARN(fmt, ...) \
    debug_print(DEBUG_LEVEL_WARN, "[WARN] " fmt, ##__VA_ARGS__)

#define DEBUG_INFO(fmt, ...) \
    debug_print(DEBUG_LEVEL_INFO, "[INFO] " fmt, ##__VA_ARGS__)

#define DEBUG_DEBUG(fmt, ...) \
    debug_print(DEBUG_LEVEL_DEBUG, "[DEBUG] " fmt, ##__VA_ARGS__)

#define DEBUG_TRACE(fmt, ...) \
    debug_print(DEBUG_LEVEL_TRACE, "[TRACE] " fmt, ##__VA_ARGS__)

/* ========== 统计接口 ========== */

/* 获取性能统计 */
void perf_get_stats(struct perf_stats *stats);

/* 更新性能统计 */
void perf_update_stats(void);

/* 打印性能报告 */
void perf_print_report(void);

/* 打印计数器 */
void perf_print_counters(void);

/* ========== 剖析接口 ========== */

/* 开始剖析 */
void perf_profile_start(const char *name);

/* 结束剖析 */
uint64_t perf_profile_end(const char *name);

/* 剖析点 */
#define PERF_PROFILE_POINT(name) \
    do { \
        static uint64_t __start_time; \
        __start_time = get_tick_count()

#define PERF_PROFILE_END() \
        uint64_t __elapsed = get_tick_count() - __start_time; \
        perf_record_sample(PERF_COUNTER_COUNT, __elapsed); \
    } while (0)

/* ========== 追踪接口 ========== */

/* 追踪点 */
struct trace_point {
    const char *name;
    uint64_t count;
    uint64_t total_time;
    uint64_t min_time;
    uint64_t max_time;
};

/* 定义追踪点 */
#define DEFINE_TRACE_POINT(name) \
    static struct trace_point __trace_##name = { #name, 0, 0, ~0ULL, 0 }

/* 追踪点入口 */
#define TRACE_ENTER(name) \
    uint64_t __trace_enter_##name = get_tick_count()

/* 追踪点退出 */
#define TRACE_EXIT(name) \
    do { \
        uint64_t __elapsed = get_tick_count() - __trace_enter_##name; \
        __trace_##name.count++; \
        __trace_##name.total_time += __elapsed; \
        if (__elapsed < __trace_##name.min_time) \
            __trace_##name.min_time = __elapsed; \
        if (__elapsed > __trace_##name.max_time) \
            __trace_##name.max_time = __elapsed; \
    } while (0)

/* 获取追踪点信息 */
void trace_get_info(const char *name, struct trace_point *info);

/* ========== 硬件性能计数器 ========== */

/* 硬件计数器类型 */
enum hw_perf_type {
    HW_PERF_CPU_CYCLES,
    HW_PERF_INSTRUCTIONS,
    HW_PERF_CACHE_REFERENCES,
    HW_PERF_CACHE_MISSES,
    HW_PERF_BRANCH_INSTRUCTIONS,
    HW_PERF_BRANCH_MISSES,
    HW_PERF_COUNT
};

/* 启用硬件计数器 */
int hw_perf_enable(enum hw_perf_type type);

/* 禁用硬件计数器 */
void hw_perf_disable(enum hw_perf_type type);

/* 读取硬件计数器 */
uint64_t hw_perf_read(enum hw_perf_type type);

/* 重置硬件计数器 */
void hw_perf_reset(enum hw_perf_type type);

/* ========== PMU 事件采样 ========== */

/* PMU 事件类型 */
enum pmu_event_type {
    PMU_EVENT_CPU_CYCLES = 0,         /* CPU 周期 */
    PMU_EVENT_INSTRUCTIONS,            /* 指令数 */
    PMU_EVENT_CACHE_REFILLS,           /* 缓存重填 */
    PMU_EVENT_CACHE_MISSES,            /* 缓存未命中 */
    PMU_EVENT_BRANCH_MISPREDICT,       /* 分支预测错误 */
    PMU_EVENT_BUS_ACCESS,              /* 总线访问 */
    PMU_EVENT_L1D_CACHE_REFILL,        /* L1 数据缓存重填 */
    PMU_EVENT_L1I_CACHE_REFILL,        /* L1 指令缓存重填 */
    PMU_EVENT_LL_CACHE_REFILL,         /* 末级缓存重填 */
    PMU_EVENT_COUNT
};

/* PMU 采样配置 */
struct pmu_sample_config {
    enum pmu_event_type event;         /* 采样事件 */
    uint64_t period;                   /* 采样周期 (每 N 个事件采样一次) */
    uint32_t max_samples;              /* 最大采样数 */
    int kernel_only;                   /* 仅内核空间 */
    int user_only;                     /* 仅用户空间 */
};

/* PMU 采样记录 */
struct pmu_sample_record {
    uint64_t ip;                       /* 指令指针 */
    uint64_t timestamp;                /* 时间戳 */
    uint32_t cpu;                      /* CPU ID */
    uint32_t pid;                      /* 进程 ID */
    uint64_t period;                   /* 采样周期 */
    uint64_t stack[16];               /* 调用栈 (最多 16 层) */
    uint32_t stack_depth;              /* 调用栈深度 */
};

/* 初始化 PMU 采样 */
int perf_pmu_init(void);

/* 启动 PMU 采样 */
int perf_pmu_start(struct pmu_sample_config *config);

/* 停止 PMU 采样 */
int perf_pmu_stop(void);

/* 读取 PMU 采样记录 */
int perf_pmu_read_samples(struct pmu_sample_record *records, int max_count);

/* 获取 PMU 采样统计 */
void perf_pmu_get_stats(uint64_t *total_samples, uint64_t *total_period,
                        uint64_t *lost_samples);

/* ========== 火焰图生成 ========== */

/* 火焰图折叠栈数据格式 (用于 flamegraph.pl 等工具) */
#define FLAME_FOLDED_MAX    8192   /* 最大折叠栈条目数 */
#define FLAME_STACK_MAX     64     /* 最大栈深度 */

/* 折叠栈条目 */
struct flame_folded_entry {
    char stack_str[256];             /* 折叠栈字符串 (func1;func2;func3) */
    uint64_t count;                  /* 采样计数 */
};

/* 生成火焰图折叠栈数据 */
int perf_flame_generate_folded(struct flame_folded_entry *entries,
                                int max_entries, int *actual_count);

/* 输出火焰图数据到文本格式 */
int perf_flame_output_folded_text(char *buf, size_t buf_size);

/* 清除火焰图采样数据 */
void perf_flame_clear_data(void);

/* ========== ftrace 函数追踪 ========== */

/* ftrace 追踪记录 */
struct ftrace_record {
    uint64_t ip;                       /* 函数地址 */
    uint64_t parent_ip;                /* 调用者地址 */
    uint64_t timestamp;                /* 时间戳 */
    uint32_t cpu;                      /* CPU ID */
    uint32_t flags;                    /* 标志 */
};

/* ftrace 函数追踪点 */
struct ftrace_point {
    uint64_t addr;                     /* 函数地址 */
    uint8_t original_code[16];         /* 原始指令 (NOP 或函数开头) */
    uint32_t original_size;            /* 原始指令大小 */
    int enabled;                       /* 是否启用 */
    char name[64];                     /* 函数名 */
};

/* ftrace 配置 */
#define FTRACE_MAX_POINTS       256
#define FTRACE_BUFFER_SIZE      4096

/* 初始化 ftrace */
int perf_ftrace_init(void);

/* 注册 ftrace 追踪点 */
int perf_ftrace_register(uint64_t func_addr, const char *name);

/* 启用 ftrace 追踪点 */
int perf_ftrace_enable(uint64_t func_addr);

/* 禁用 ftrace 追踪点 */
int perf_ftrace_disable(uint64_t func_addr);

/* 启用所有追踪点 */
int perf_ftrace_enable_all(void);

/* 禁用所有追踪点 */
int perf_ftrace_disable_all(void);

/* 读取 ftrace 记录 */
int perf_ftrace_read(struct ftrace_record *records, int max_count);

/* ftrace 入口回调 (由被追踪函数调用) */
void perf_ftrace_caller(uint64_t ip, uint64_t parent_ip);

/* 获取 ftrace 统计 */
void perf_ftrace_get_stats(uint64_t *total_calls, uint64_t *total_time,
                           uint32_t *active_points);

/* ========== 系统健康监控 ========== */

/* 健康指标 */
struct health_metrics {
    uint32_t cpu_load;          /* CPU 负载百分比 */
    uint32_t mem_usage;         /* 内存使用百分比 */
    uint32_t io_wait;           /* IO 等待百分比 */
    uint32_t swap_usage;        /* 交换使用百分比 */
    uint32_t net_usage;         /* 网络带宽使用 */
    uint32_t errors;            /* 错误计数 */
    uint32_t warnings;          /* 警告计数 */
    uint32_t uptime_seconds;    /* 运行时间 */
};

/* 获取健康指标 */
void health_get_metrics(struct health_metrics *metrics);

/* 健康检查 */
int health_check(void);

/* 打印健康报告 */
void health_print_report(void);

/* ========== 快速内联函数 ========== */

/* 快速计数器增加 */
static inline void perf_inc_fast(enum perf_counter_type type) {
    perf_inc(type);
}

/* 快速读取 */
static inline uint64_t perf_read_fast(enum perf_counter_type type) {
    return perf_read(type);
}

#endif /* _PERF_H */
