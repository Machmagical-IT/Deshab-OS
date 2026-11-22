/*
 * Zratro OS - Unified Performance Analysis Extension (统一性能分析扩展)
 * 
 * 整合所有子系统的性能分析：
 * 1. CPU/内存/IO 统计
 * 2. 调度器分析
 * 3. 网络协议栈
 * 4. 文件系统
 * 5. 新子系统 (PMEM, Z-Pipe, Capability, Self-Healing, Coroutine, Heterog)
 */

#ifndef PERF_ANALYSIS_H
#define PERF_ANALYSIS_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== 配置 ========== */

#define PA_MAX_SUBSYSTEMS       32
#define PA_MAX_METRICS          256
#define PA_MAX_SAMPLES          4096
#define PA_MAX_LATENCY_BUCKETS  32
#define PA_MAX_NAME_LEN         32

/* ========== 子系统 ID ========== */

typedef enum {
    PA_SUBSYS_NONE = 0,
    PA_SUBSYS_SCHEDULER,
    PA_SUBSYS_MEMORY,
    PA_SUBSYS_FS,
    PA_SUBSYS_NETWORK,
    PA_SUBSYS_IPC,
    PA_SUBSYS_PMEM,
    PA_SUBSYS_ZPIPE,
    PA_SUBSYS_CAPABILITY,
    PA_SUBSYS_SELFHEAL,
    PA_SUBSYS_COROUTINE,
    PA_SUBSYS_HETEROG,
    PA_SUBSYS_REALTIME,
    PA_SUBSYS_NETZC,
    PA_SUBSYS_UTX,
    PA_SUBSYS_MAX
} pa_subsys_id_t;

/* ========== 指标类型 ========== */

typedef enum {
    PA_METRIC_COUNTER = 0,      /* 累计计数 */
    PA_METRIC_GAUGE,            /* 当前值 */
    PA_METRIC_HISTOGRAM,        /* 直方图 */
    PA_METRIC_SUMMARY,          /* 摘要 */
    PA_METRIC_RATE,             /* 速率 */
} pa_metric_type_t;

/* ========== 延迟桶 ========== */

struct pa_latency_bucket {
    uint64_t threshold;         /* 阈值 */
    uint64_t count;             /* 计数 */
    uint64_t total;             /* 总和 */
};

/* ========== 直方图 ========== */

struct pa_histogram {
    uint64_t min;
    uint64_t max;
    uint64_t sum;
    uint64_t count;
    uint64_t buckets[PA_MAX_LATENCY_BUCKETS];
    uint64_t bucket_bounds[PA_MAX_LATENCY_BUCKETS];
};

/* ========== 指标描述符 ========== */

struct pa_metric {
    uint32_t metric_id;
    char name[PA_MAX_NAME_LEN];
    pa_subsys_id_t subsystem;
    pa_metric_type_t type;
    uint32_t flags;
    
    /* 值 */
    uint64_t value;
    uint64_t last_value;
    uint64_t update_time;
    
    /* 直方图 */
    struct pa_histogram histogram;
    
    /* 采样 */
    uint64_t sample_count;
    uint64_t sample_sum;
    
    struct list_head list;
};

/* 指标标志 */
#define PA_METRIC_FLAG_ENABLED      (1 << 0)
#define PA_METRIC_FLAG_AGGREGATE    (1 << 1)
#define PA_METRIC_FLAG_RATE         (1 << 2)

/* ========== 子系统统计 ========== */

struct pa_subsys_stats {
    pa_subsys_id_t id;
    char name[PA_MAX_NAME_LEN];
    
    /* 指标列表 */
    struct list_head metrics;
    uint32_t metric_count;
    
    /* 总体统计 */
    uint64_t total_operations;
    uint64_t total_errors;
    uint64_t total_time;
    
    /* 利用率 */
    uint64_t busy_time;
    uint64_t idle_time;
    
    struct list_head list;
};

/* ========== CPU 分析 ========== */

struct pa_cpu_stats {
    int cpu_id;
    
    /* 时间分布 */
    uint64_t user_time;
    uint64_t system_time;
    uint64_t idle_time;
    uint64_t iowait_time;
    uint64_t irq_time;
    
    /* 调度统计 */
    uint64_t context_switches;
    uint64_t migrations;
    uint64_t preemptions;
    
    /* 负载 */
    uint64_t load_avg_1;        /* 1分钟负载 */
    uint64_t load_avg_5;        /* 5分钟负载 */
    uint64_t load_avg_15;       /* 15分钟负载 */
    
    /* 频率 */
    uint64_t current_freq;
    uint64_t max_freq;
    uint64_t min_freq;
};

/* ========== 内存分析 ========== */

struct pa_memory_stats {
    /* 容量 */
    uint64_t total;
    uint64_t used;
    uint64_t free;
    uint64_t cached;
    uint64_t buffers;
    
    /* 分配 */
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t alloc_bytes;
    uint64_t free_bytes;
    
    /* 页错误 */
    uint64_t page_faults;
    uint64_t major_faults;
    uint64_t minor_faults;
    
    /* 缓存命中率 */
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cache_hit_rate;    /* 千分比 */
    
    /* 堆统计 */
    uint64_t heap_size;
    uint64_t heap_used;
    uint64_t heap_fragmentation;
};

/* ========== IO 分析 ========== */

struct pa_io_stats {
    /* 读写 */
    uint64_t read_ops;
    uint64_t write_ops;
    uint64_t read_bytes;
    uint64_t write_bytes;
    
    /* 延迟 */
    uint64_t read_latency_us;
    uint64_t write_latency_us;
    
    /* 队列 */
    uint64_t queue_depth;
    uint64_t queue_wait_time;
    
    /* 错误 */
    uint64_t read_errors;
    uint64_t write_errors;
};

/* ========== 网络分析 ========== */

struct pa_network_stats {
    /* 包统计 */
    uint64_t packets_in;
    uint64_t packets_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
    
    /* 错误 */
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    
    /* TCP */
    uint64_t tcp_connections;
    uint64_t tcp_active;
    uint64_t tcp_passive;
    uint64_t tcp_retransmits;
    
    /* 零拷贝统计 */
    uint64_t zc_tx;
    uint64_t zc_rx;
    uint64_t zc_fallback;
};

/* ========== 调度器分析 ========== */

struct pa_scheduler_stats {
    /* 任务统计 */
    uint64_t tasks_created;
    uint64_t tasks_destroyed;
    uint64_t tasks_running;
    uint64_t tasks_blocked;
    
    /* 上下文切换 */
    uint64_t context_switches;
    uint64_t voluntary_switches;
    uint64_t involuntary_switches;
    
    /* 负载均衡 */
    uint64_t load_balance_count;
    uint64_t migrations;
    
    /* 实时任务 */
    uint64_t rt_tasks;
    uint64_t rt_deadlines_met;
    uint64_t rt_deadlines_missed;
    
    /* CFS */
    uint64_t cfs_tasks;
    uint64_t cfs_latency;
};

/* ========== 火焰图数据 ========== */

struct pa_flame_node {
    uint64_t ip;                /* 指令指针 */
    uint32_t count;             /* 采样计数 */
    uint32_t self_count;        /* 自身计数 */
    struct pa_flame_node *parent;
    struct pa_flame_node *children[16];
    uint32_t child_count;
};

struct pa_flame_graph {
    struct pa_flame_node *root;
    uint32_t total_samples;
    uint32_t depth;
    uint32_t node_count;
};

/* ========== 性能分析管理器 ========== */

struct pa_manager {
    /* 子系统统计 */
    struct list_head subsystems;
    uint32_t subsystem_count;
    
    /* CPU 统计 */
    struct pa_cpu_stats cpu_stats[8];
    uint32_t cpu_count;
    
    /* 内存统计 */
    struct pa_memory_stats memory_stats;
    
    /* IO 统计 */
    struct pa_io_stats io_stats;
    
    /* 网络统计 */
    struct pa_network_stats network_stats;
    
    /* 调度器统计 */
    struct pa_scheduler_stats scheduler_stats;
    
    /* 火焰图 */
    struct pa_flame_graph flame_graph;
    
    /* 采样配置 */
    uint64_t sample_interval;   /* 采样间隔 */
    uint64_t sample_count;      /* 采样计数 */
    int profiling_enabled;
    
    /* 时间 */
    uint64_t start_time;
    uint64_t last_update;
    
    /* 锁 */
    spinlock_t lock;
};

/* ========== 公共 API ========== */

/* 初始化 */
int pa_init(struct pa_manager *pa);
void pa_destroy(struct pa_manager *pa);

/* ========== 子系统注册 ========== */

/* 注册子系统 */
struct pa_subsys_stats *pa_register_subsystem(struct pa_manager *pa,
                                               pa_subsys_id_t id,
                                               const char *name);

/* 注销子系统 */
int pa_unregister_subsystem(struct pa_manager *pa, pa_subsys_id_t id);

/* ========== 指标管理 ========== */

/* 注册指标 */
struct pa_metric *pa_register_metric(struct pa_manager *pa,
                                      pa_subsys_id_t subsystem,
                                      const char *name,
                                      pa_metric_type_t type);

/* 更新指标 */
int pa_update_metric(struct pa_manager *pa, uint32_t metric_id, uint64_t value);

/* 增加指标 */
int pa_increment_metric(struct pa_manager *pa, uint32_t metric_id, uint64_t delta);

/* 记录采样 */
int pa_record_sample(struct pa_manager *pa, uint32_t metric_id, uint64_t value);

/* ========== 统计更新 ========== */

/* 更新 CPU 统计 */
int pa_update_cpu_stats(struct pa_manager *pa, int cpu);

/* 更新内存统计 */
int pa_update_memory_stats(struct pa_manager *pa);

/* 更新 IO 统计 */
int pa_update_io_stats(struct pa_manager *pa);

/* 更新网络统计 */
int pa_update_network_stats(struct pa_manager *pa);

/* 更新调度器统计 */
int pa_update_scheduler_stats(struct pa_manager *pa);

/* ========== 汇总接口 ========== */

/* 收集所有子系统统计 */
int pa_collect_all(struct pa_manager *pa);

/* 计算利用率 */
uint64_t pa_calc_utilization(struct pa_manager *pa, pa_subsys_id_t subsystem);

/* 计算吞吐量 */
uint64_t pa_calc_throughput(struct pa_manager *pa, pa_subsys_id_t subsystem);

/* 计算延迟百分位 */
uint64_t pa_calc_latency_percentile(struct pa_manager *pa,
                                     pa_subsys_id_t subsystem,
                                     uint32_t percentile);

/* ========== 火焰图 ========== */

/* 开始性能分析 */
int pa_start_profiling(struct pa_manager *pa, uint64_t interval);

/* 停止性能分析 */
int pa_stop_profiling(struct pa_manager *pa);

/* 采样 */
int pa_take_sample(struct pa_manager *pa);

/* 获取火焰图 */
struct pa_flame_graph *pa_get_flame_graph(struct pa_manager *pa);

/* ========== 报告生成 ========== */

/* 生成报告 */
int pa_generate_report(struct pa_manager *pa, char *buf, size_t size);

/* 打印摘要 */
void pa_print_summary(struct pa_manager *pa);

/* 打印子系统详情 */
void pa_print_subsystem(struct pa_manager *pa, pa_subsys_id_t id);

/* ========== 便捷宏 ========== */

#define PA_METRIC_INC(pa, id)  pa_increment_metric(pa, id, 1)
#define PA_METRIC_ADD(pa, id, val) pa_increment_metric(pa, id, val)
#define PA_METRIC_SET(pa, id, val) pa_update_metric(pa, id, val)

/* 全局管理器 */
struct pa_manager *pa_get_global(void);
int pa_global_init(void);

#endif /* PERF_ANALYSIS_H */
