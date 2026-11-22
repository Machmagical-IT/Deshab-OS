/*
 * Zratro OS - CFS (Completely Fair Scheduler)
 * 
 * Linux 风格的完全公平调度器
 * 使用红黑树实现 O(log n) 的调度效率
 */

#ifndef CFS_H
#define CFS_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "task.h"

/* ========== CFS 配置 ========== */

/* 时间精度 (纳秒) */
#define NSEC_PER_SEC        1000000000ULL
#define NSEC_PER_MSEC       1000000ULL
#define NSEC_PER_USEC       1000ULL

/* CFS 默认参数 */
#define CFS_DEFAULT_SLICE   7500000ULL   /* 默认时间片 7.5ms */
#define CFS_MIN_SLICE       750000ULL    /* 最小时间片 0.75ms */
#define CFS_MAX_SLICE       100000000ULL /* 最大时间片 100ms */

/* 调度延迟 (目标调度周期) */
#define CFS_SCHED_LATENCY   6000000ULL   /* 6ms */
#define CFS_MIN_GRANULARITY 750000ULL    /* 最小粒度 0.75ms */
#define CFS_WAKEUP_GRANULARITY 1000000ULL /* 唤醒抢占粒度 1ms */

/* nice 值相关 */
#define NICE_0_LOAD         1024         /* nice=0 的权重 */
#define NICE_0_SHIFT        10           /* 权重移位 */

/* nice 值权重表 (与 Linux 相同) */
static const uint32_t sched_prio_to_weight[40] = {
 /* -20 */     88761,     71755,     56483,     46273,     36291,
 /* -15 */     29154,     23254,     18705,     14949,     11916,
 /* -10 */      9548,      7620,      6100,      4904,      3906,
 /*  -5 */      3121,      2501,      1991,      1586,      1277,
 /*   0 */      1024,       820,       655,       526,       423,
 /*   5 */       335,       272,       215,       172,       137,
 /*  10 */       110,        87,        70,        56,        45,
 /*  15 */        36,        29,        23,        18,        15,
};

/* nice 值到权重的逆 (用于除法优化, 与 Linux 内核一致) */
static const uint32_t sched_prio_to_wmult[40] = {
 /* -20 */     48388,     59856,     76040,     92818,    118348,
 /* -15 */    147320,    184698,    229616,    287308,    360424,
 /* -10 */    450936,    562980,    704864,    877104,   1085440,
 /*  -5 */   1354816,   1690896,   2101376,   2631488,   3290752,
 /*   0 */   4194304,   5244160,   6545408,   8164800,  10194432,
 /*   5 */  12719104,  15862656,  19849728,  24807680,  31127296,
 /*  10 */  39012864,  49073664,  61855744,  78273024,  98906112,
 /*  15 */ 125739008, 159219712, 201694592, 255395328, 322321024,
};

/* ========== CFS 运行队列 ========== */

struct cfs_rq {
    /* ---- 热路径 (每次 tick/schedule 都访问) ---- */

    /* 红黑树 */
    struct rb_root tasks_timeline;  /* 按虚拟时间排序的红黑树 */

    /* 当前运行的实体 */
    struct sched_entity *curr;
    struct sched_entity *next;      /* 下一个预选 */
    struct sched_entity *last;      /* 上一个运行 */
    struct sched_entity *skip;      /* 跳过调度 */

    /* 负载权重 (slice 计算热路径) */
    uint64_t load_weight;           /* 总负载权重 */
    uint64_t runnable_weight;       /* 可运行权重 */
    uint64_t load_weight_recip;     /* load_weight 逆权重 (用于快速除法) */
    uint32_t load_weight_shift;     /* 逆权重移位量 */

    /* 运行计数 */
    uint32_t nr_running;            /* 运行进程数 */
    uint32_t h_nr_running;          /* 层次化运行数 */

    /* 最小虚拟时间 */
    uint64_t min_vruntime;          /* 队列最小虚拟时间 */

    /* 调度延迟 (slice 计算用) */
    uint64_t sched_latency;         /* 目标调度延迟 */
    uint64_t min_granularity;       /* 最小调度粒度 */

    /* 锁 */
    spinlock_t lock;

    /* ---- 冷路径 (统计/初始化) ---- */

    /* 时间相关 */
    uint64_t exec_clock;            /* 执行时钟 */
    uint64_t clock;                 /* 队列时钟 */
    uint64_t clock_task;            /* 任务时钟 (不含 IRQ 时间) */

    /* 所属 CPU */
    int cpu;
} __attribute__((aligned(64)));

/* ========== CFS 调度类 ========== */

extern struct sched_class cfs_sched_class;

/* ========== 函数原型 ========== */

/* 初始化 */
void cfs_init(void);
void cfs_rq_init(struct cfs_rq *cfs_rq, int cpu);

/* 调度操作 */
void cfs_enqueue_task(struct cfs_rq *cfs_rq, struct sched_entity *se);
void cfs_dequeue_task(struct cfs_rq *cfs_rq, struct sched_entity *se);
struct sched_entity *cfs_pick_next_task(struct cfs_rq *cfs_rq);
void cfs_put_prev_task(struct cfs_rq *cfs_rq, struct sched_entity *se);
void cfs_task_tick(struct cfs_rq *cfs_rq, struct sched_entity *se);

/* 时间片计算 */
uint64_t cfs_calc_slice(struct cfs_rq *cfs_rq, struct sched_entity *se);
uint64_t __calc_delta(uint64_t delta_exec, uint32_t wmult_idx);

/* 虚拟时间 */
void cfs_update_vruntime(struct cfs_rq *cfs_rq, struct sched_entity *se, uint64_t delta_exec);

/* 红黑树操作 */
void rb_insert(struct rb_root *root, struct rb_node *node);
void rb_erase(struct rb_root *root, struct rb_node *node);
struct rb_node *rb_first(struct rb_root *root);
struct rb_node *rb_next(struct rb_node *node);

/* 工具函数 */
static inline uint64_t cfs_time_to_vruntime(uint64_t delta, uint32_t wmult_idx) {
    return __calc_delta(delta, wmult_idx);
}

/* 从 nice 值计算 wmult_idx */
static inline uint32_t sched_wmult_idx(int nice) {
    int idx = nice + 20;
    if (idx < 0) idx = 0;
    if (idx > 39) idx = 39;
    return (uint32_t)idx;
}

static inline uint64_t cfs_vruntime_to_time(uint64_t vruntime, uint32_t weight) {
    return (vruntime * weight) / NICE_0_LOAD;
}

/* nice 值转换 */
static inline int prio_to_nice(int prio) {
    return prio - 120;
}

static inline int nice_to_prio(int nice) {
    return nice + 120;
}

static inline uint32_t sched_weight(int nice) {
    int idx = nice + 20;
    if (idx < 0) idx = 0;
    if (idx > 39) idx = 39;
    return sched_prio_to_weight[idx];
}

/* 检查是否需要抢占 */
int cfs_check_preempt_wakeup(struct cfs_rq *cfs_rq, struct sched_entity *curr,
                              struct sched_entity *se);

/* 负载均衡 */
void cfs_load_balance(struct cfs_rq *src, struct cfs_rq *dst);

#endif /* CFS_H */