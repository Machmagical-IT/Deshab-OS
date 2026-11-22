/*
 * Zratro OS - Hard Real-Time Scheduler (硬实时调度器)
 * 
 * 创新设计：
 * 1. EDF (Earliest Deadline First) 调度
 * 2. 优先级继承协议 (Priority Inheritance)
 * 3. CPU 亲和性绑定
 * 4. 周期性任务支持
 * 5. 截止时间监控
 */

#ifndef REALTIME_H
#define REALTIME_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "task.h"

/* ========== 实时配置 ========== */

#define RT_MAX_TASKS            64          /* 最大实时任务数 */
#define RT_MAX_CPUS             8           /* 最大 CPU 数 */
#define RT_MAX_PERIODIC         32          /* 最大周期性任务数 */
#define RT_MAX_NAME_LEN         32          /* 名字最大长度 */
#define RT_MAX_PI_CHAIN         8           /* 最大优先级继承链深度 */

/* ========== 调度策略 ========== */

typedef enum {
    RT_SCHED_OTHER = 0,         /* 普通调度 */
    RT_SCHED_FIFO,              /* FIFO 实时 */
    RT_SCHED_RR,                /* Round Robin 实时 */
    RT_SCHED_EDF,               /* EDF 调度 */
    RT_SCHED_DEADLINE,          /* 截止时间调度 */
    RT_SCHED_SPORADIC,          /* 离散调度 */
} rt_sched_policy_t;

/* ========== 优先级范围 ========== */

#define RT_PRIO_MIN             1           /* 最高优先级 */
#define RT_PRIO_MAX             99          /* 最低实时优先级 */
#define RT_PRIO_DEFAULT         50

/* ========== 实时任务参数 ========== */

struct rt_params {
    rt_sched_policy_t policy;   /* 调度策略 */
    int priority;               /* 静态优先级 */
    
    /* EDF/Deadline 参数 */
    uint64_t runtime;           /* 运行时间 */
    uint64_t deadline;          /* 相对截止时间 */
    uint64_t period;            /* 周期 */
    
    /* 时间配额 (RR) */
    uint64_t time_slice;        /* 时间片 */
    uint64_t time_remaining;    /* 剩余时间 */
    
    /* CPU 亲和性 */
    uint64_t cpu_mask;          /* CPU 位掩码 */
    int cpu_bound;              /* 绑定到特定 CPU */
    
    /* 截止时间 */
    uint64_t next_deadline;     /* 下一个绝对截止时间 */
    uint64_t next_release;      /* 下一个释放时间 */
    
    /* 统计 */
    uint64_t deadlines_met;     /* 满足截止时间次数 */
    uint64_t deadlines_missed;  /* 错过截止时间次数 */
    uint64_t total_runtime;     /* 总运行时间 */
    uint64_t max_runtime;       /* 最大运行时间 */
    uint64_t context_switches;  /* 上下文切换次数 */
};

/* ========== 优先级继承 ========== */

struct rt_pi_node {
    uint64_t task_id;           /* 任务 ID */
    int original_prio;          /* 原始优先级 */
    int boosted_prio;           /* 提升后优先级 */
    
    /* 等待的资源 */
    void *waiting_for;          /* 等待的锁 */
    struct rt_pi_node *blocked_by;  /* 被谁阻塞 */
    
    /* 阻塞链 */
    struct list_head blockers;  /* 被此任务阻塞的任务 */
    struct list_head pi_list;   /* PI 链表 */
    
    int chain_depth;            /* 继承链深度 */
};

/* ========== 实时任务描述符 ========== */

struct rt_task {
    uint64_t task_id;           /* 任务 ID */
    char name[RT_MAX_NAME_LEN]; /* 任务名 */
    int active;                 /* 是否活跃 */
    
    /* 调度参数 */
    struct rt_params params;
    
    /* 优先级继承 */
    struct rt_pi_node pi_node;
    
    /* 周期性任务 */
    int is_periodic;
    uint64_t last_release;      /* 上次释放时间 */
    uint64_t executions;        /* 执行次数 */
    
    /* 当前状态 */
    uint64_t start_time;        /* 本次开始时间 */
    uint64_t accrued_time;      /* 累积运行时间 */
    
    struct list_head list;      /* 任务链表 */
    struct list_head run_list;  /* 运行队列链表 */
};

/* ========== EDF 优先级队列 ========== */

struct rt_edf_queue {
    struct list_head tasks;      /* 按截止时间排序 */
    uint32_t count;
    uint64_t earliest_deadline;  /* 最早截止时间 */
};

/* ========== 截止时间监控 ========== */

struct rt_deadline_monitor {
    uint64_t check_interval;     /* 检查间隔 */
    uint64_t last_check;         /* 上次检查时间 */
    
    /* 统计 */
    uint64_t total_misses;       /* 总错过次数 */
    uint64_t recent_misses;      /* 最近错过次数 */
    uint64_t max_consecutive;    /* 最大连续错过 */
    uint64_t consecutive_misses; /* 当前连续错过 */
    
    /* 告警阈值 */
    uint64_t miss_threshold;     /* 告警阈值 */
    int callback_enabled;
    void (*miss_callback)(struct rt_task *task, uint64_t deadline);
};

/* ========== CPU 实时调度状态 ========== */

struct rt_cpu_state {
    int cpu_id;
    int has_rt_task;             /* 是否运行实时任务 */
    
    /* 当前任务 */
    struct rt_task *current;
    
    /* 就绪队列 */
    struct list_head ready[100]; /* 按优先级分区 */
    uint32_t ready_count;
    
    /* EDF 队列 */
    struct rt_edf_queue edf_queue;
    
    /* 截止时间最近的 */
    uint64_t nearest_deadline;
    
    /* 统计 */
    uint64_t rt_time;            /* 实时任务运行时间 */
    uint64_t other_time;         /* 普通任务运行时间 */
};

/* ========== 实时调度器 ========== */

struct rt_scheduler {
    /* 任务管理 */
    struct list_head tasks;      /* 所有实时任务 */
    uint32_t task_count;
    
    /* CPU 状态 */
    struct rt_cpu_state cpu_states[RT_MAX_CPUS];
    uint32_t cpu_count;
    
    /* 周期性任务 */
    struct list_head periodic_tasks;
    uint32_t periodic_count;
    
    /* 截止时间监控 */
    struct rt_deadline_monitor monitor;
    
    /* 配置 */
    uint64_t tick_period;        /* 时钟周期 */
    int strict_mode;             /* 严格模式 */
    
    /* 统计 */
    uint64_t total_switches;
    uint64_t total_preemptions;
    uint64_t total_pi_inherit;
    
    /* 锁 */
    spinlock_t lock;
};

/* ========== 公共 API ========== */

/* 初始化 */
int rt_init(struct rt_scheduler *rts);
void rt_destroy(struct rt_scheduler *rts);

/* ========== 任务管理 ========== */

/* 创建实时任务 */
struct rt_task *rt_create_task(struct rt_scheduler *rts,
                                uint64_t task_id,
                                const char *name);

/* 销毁实时任务 */
int rt_destroy_task(struct rt_scheduler *rts, struct rt_task *task);

/* 设置调度参数 */
int rt_set_params(struct rt_task *task, struct rt_params *params);

/* 获取调度参数 */
int rt_get_params(struct rt_task *task, struct rt_params *params);

/* 绑定 CPU */
int rt_set_cpu_affinity(struct rt_task *task, int cpu);

/* ========== 调度操作 ========== */

/* 选择下一个任务 */
struct rt_task *rt_select_next(struct rt_scheduler *rts, int cpu);

/* 任务就绪 */
int rt_task_ready(struct rt_scheduler *rts, struct rt_task *task);

/* 任务阻塞 */
int rt_task_block(struct rt_scheduler *rts, struct rt_task *task);

/* 任务完成 */
int rt_task_complete(struct rt_scheduler *rts, struct rt_task *task);

/* 时间片用尽 */
int rt_task_expire(struct rt_scheduler *rts, struct rt_task *task);

/* ========== EDF 调度 ========== */

/* 插入 EDF 队列 */
int rt_edf_insert(struct rt_edf_queue *queue, struct rt_task *task);

/* 从 EDF 队列移除 */
int rt_edf_remove(struct rt_edf_queue *queue, struct rt_task *task);

/* 获取最早截止时间任务 */
struct rt_task *rt_edf_peek(struct rt_edf_queue *queue);

/* ========== 优先级继承 ========== */

/* 开始优先级继承 */
int rt_pi_begin(struct rt_scheduler *rts, struct rt_task *task, void *lock);

/* 结束优先级继承 */
int rt_pi_end(struct rt_scheduler *rts, struct rt_task *task, void *lock);

/* 更新优先级链 */
int rt_pi_update_chain(struct rt_scheduler *rts, struct rt_pi_node *node);

/* ========== 主调度器优先级继承 (struct thread 集成) ========== */

/* 提升线程优先级 (当高优先级线程阻塞在低优先级线程持有的锁上时调用) */
int rt_pi_boost(struct thread *waiter, struct thread *holder, void *lock);

/* 恢复线程优先级 (锁释放时调用) */
int rt_pi_unboost(struct thread *holder, void *lock);

/* 获取线程的有效实时优先级 (考虑 PI 提升) */
static inline int rt_thread_effective_prio(struct thread *t) {
    if (t == NULL) return 0;
    if (t->boosted_priority > 0) {
        return t->boosted_priority;
    }
    return t->rt_priority;
}

/* ========== 周期性任务 ========== */

/* 注册周期性任务 */
int rt_register_periodic(struct rt_scheduler *rts, struct rt_task *task);

/* 注销周期性任务 */
int rt_unregister_periodic(struct rt_scheduler *rts, struct rt_task *task);

/* 处理周期性释放 */
int rt_handle_periodic(struct rt_scheduler *rts, uint64_t now);

/* ========== 截止时间监控 ========== */

/* 检查截止时间 */
int rt_check_deadlines(struct rt_scheduler *rts, uint64_t now);

/* 设置错过回调 */
int rt_set_miss_callback(struct rt_scheduler *rts,
                         void (*callback)(struct rt_task *, uint64_t));

/* 获取统计 */
void rt_get_deadline_stats(struct rt_scheduler *rts,
                            uint64_t *met, uint64_t *missed);

/* ========== 工具函数 ========== */

/* 计算截止时间紧迫度 */
int64_t rt_deadline_urgency(struct rt_task *task, uint64_t now);

/* 检查是否可调度 */
int rt_check_schedulability(struct rt_scheduler *rts);

/* 计算利用率 */
uint64_t rt_calc_utilization(struct rt_scheduler *rts);

/* ========== 时钟钩子 ========== */

/* 实时时钟中断处理 */
void rt_tick_handler(struct rt_scheduler *rts, uint64_t now);

/* ========== 统计 ========== */

/* 获取统计 */
void rt_get_stats(struct rt_scheduler *rts,
                  uint64_t *switches, uint64_t *preemptions,
                  uint64_t *pi_inherits);

/* 打印任务状态 */
void rt_print_tasks(struct rt_scheduler *rts);

/* ========== 内联辅助 ========== */

/* 检查是否为实时任务 */
static inline int rt_is_realtime(struct rt_task *task) {
    if (task == NULL) return 0;
    return task->params.policy != RT_SCHED_OTHER;
}

/* 获取有效优先级 */
static inline int rt_effective_prio(struct rt_task *task) {
    if (task == NULL) return RT_PRIO_MAX;
    if (task->pi_node.boosted_prio > 0) {
        return task->pi_node.boosted_prio;
    }
    return task->params.priority;
}

/* 比较截止时间 */
static inline int rt_compare_deadline(struct rt_task *a, struct rt_task *b) {
    if (a == NULL) return 1;
    if (b == NULL) return -1;
    if (a->params.next_deadline < b->params.next_deadline) return -1;
    if (a->params.next_deadline > b->params.next_deadline) return 1;
    return 0;
}

/* 全局调度器 */
struct rt_scheduler *rt_get_global(void);
int rt_global_init(void);

#endif /* REALTIME_H */
