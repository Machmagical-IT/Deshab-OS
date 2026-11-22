/*
 * Zratro OS - RCU (Read-Copy-Update) Mechanism
 * 
 * 高性能读端无锁同步原语
 * 适用于读多写少的场景
 */

#ifndef RCU_H
#define RCU_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "smp.h"

/* ========== RCU 配置 ========== */

#define RCU_GP_TICKS           4          /* 宽限期检查周期 */
#define RCU_FANOUT             32         /* RCU 树扇出 */
#define RCU_FANOUT_LEAF        16         /* 叶子扇出 */

/* RCU 回调批次大小 */
#define RCU_BATCH_SIZE         100

/* ========== RCU 核心数据结构 ========== */

/* RCU 回调结构 (必须最先定义，其他头文件可能需要) */
struct rcu_head {
    struct list_head list;          /* 链表节点 */
    void (*func)(struct rcu_head *head);
};

/* 回调初始化 */
#define RCU_HEAD_INIT(head) { .list = { &(head).list, &(head).list }, .func = NULL }
#define RCU_HEAD(head) struct rcu_head head = RCU_HEAD_INIT(head)
#define INIT_RCU_HEAD(ptr) \
    do { \
        INIT_LIST_HEAD(&(ptr)->list); \
        (ptr)->func = NULL; \
    } while (0)

/* ========== RCU 核心概念 ========== */

/*
 * RCU 工作原理:
 * 1. 读者使用 rcu_read_lock()/rcu_read_unlock() 进入/退出临界区
 * 2. 读者在临界区内不能阻塞
 * 3. 写者使用 rcu_dereference() 读取指针，使用 rcu_assign_pointer() 更新指针
 * 4. 写者调用 synchronize_rcu() 等待所有读者退出临界区
 * 5. 或者使用 call_rcu() 延迟回调来释放旧数据
 */

/* ========== RCU 读端 API ========== */

/* 读锁定 - 禁止抢占 */
static inline void rcu_read_lock(void) {
    /* 增加嵌套计数 */
    struct cpu_local *cpu = this_cpu();
    cpu->rcu_nesting++;
    barrier();  /* 确保编译器不会重排 */
}

/* 读解锁 - 允许抢占 */
static inline void rcu_read_unlock(void) {
    barrier();  /* 确保临界区操作完成 */
    struct cpu_local *cpu = this_cpu();
    cpu->rcu_nesting--;
}

/* 是否在 RCU 临界区 */
static inline int rcu_read_lock_held(void) {
    struct cpu_local *cpu = this_cpu();
    return cpu->rcu_nesting > 0;
}

/* ========== RCU 指针操作 ========== */

/* 读取 RCU 保护指针 (带内存屏障) */
#define rcu_dereference(p) \
    ({ \
        typeof(p) ____p = READ_ONCE(p); \
        smp_rmb(); \
        ____p; \
    })

/* 更新 RCU 保护指针 (带内存屏障) */
#define rcu_assign_pointer(p, v) \
    do { \
        smp_wmb(); \
        WRITE_ONCE(p, v); \
    } while (0)

/* 读取 RCU 保护指针 (锁定版本) */
#define rcu_dereference_protected(p, c) \
    ({ \
        if (debug_rcu_head_enabled) \
            RCU_LOCKDEP_WARN(!(c), "suspicious rcu_dereference_protected() usage"); \
        rcu_dereference_raw(p); \
    })

/* 原始读取 (无屏障) */
#define rcu_dereference_raw(p) \
    READ_ONCE(p)

/* ========== RCU 宽限期 ========== */

/* RCU 宽限期状态 */
struct rcu_state {
    uint64_t gp_seq;                /* 宽限期序列号 */
    uint64_t gp_flags;              /* 宽限期标志 */
    int gp_state;                   /* 宽限期状态 */
    uint64_t gp_start_time;         /* 宽限期开始时间 (用于 Stall 检测) */
    
    /* 统计 */
    uint64_t n_online_cpus;         /* 在线 CPU 数 */
    uint64_t n_grace_periods;       /* 完成的宽限期数 */
    uint64_t n_done_batches;        /* 完成的批次数 */
    
    /* 回调 */
    struct list_head cblist;        /* 回调链表 */
    uint64_t n_cbs;                 /* 回调数 */
    
    /* 锁 */
    spinlock_t lock;
    
    /* 根节点 */
    struct rcu_node *rda;           /* RCU 数据数组 */
    struct rcu_node *level[RCU_FANOUT];
    int n_levels;
};

/* RCU 节点 (树形结构) */
struct rcu_node {
    /* 锁 */
    spinlock_t lock;
    
    /* 宽限期序列 */
    uint64_t gp_seq;
    uint64_t gp_seq_needed;
    
    /* 掩码 */
    uint64_t qsmask;                /* 需要检查的 CPU */
    uint64_t expmask;               /* 加速检查的 CPU */
    
    /* 父子关系 */
    struct rcu_node *parent;
    int level;
    
    /* 叶子节点数据 */
    int grplo;                      /* 组内最低 CPU */
    int grphi;                      /* 组内最高 CPU */
    int grpnum;                     /* 组号 */
    int grpmask;                    /* 组掩码 */
};

/* RCU 数据 */
struct rcu_data {
    /* 当前宽限期 */
    uint64_t gp_seq;                /* 当前 GP 序列 */
    uint64_t gp_seq_needed;         /* 需要的 GP 序列 */
    int gpwrap;                     /* GP 序列是否回绕 */
    int cpu;                        /* CPU ID */
    
    /* 回调 */
    struct list_head cblist;        /* 待处理回调 */
    uint64_t n_cbs;                 /* 回调数 */
    struct list_head done_cbs;      /* 已完成回调 */
    uint64_t n_done_cbs;            /* 已完成回调数 */
    
    /* 统计 */
    uint64_t n_cbs_invoked;         /* 调用的回调数 */
    uint64_t n_cbs_orphaned;        /* 孤儿回调数 */
    
    /* 报告 */
    int core_needs_qs;              /* 需要 QS 报告 */
    int cpu_no_qs;                  /* 尚未通过 QS */
    
    /* 节点指针 */
    struct rcu_node *mynode;
};

/* ========== RCU 写端 API ========== */

/* 同步等待宽限期 */
void synchronize_rcu(void);

/* 异步回调 */
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head));

/* 加速同步 */
void synchronize_rcu_expedited(void);

/* ========== SRCU (可睡眠 RCU) ========== */

struct srcu_struct {
    int completed;                  /* 已完成的宽限期 */
    struct srcu_array *per_cpu_ref; /* Per-CPU 引用 */
    spinlock_t lock;
    struct list_head callback_queue;
};

struct srcu_array {
    int lock_count[2];              /* 两个计数器 */
    int unlock_count[2];
};

/* 等待 SRCU 宽限期 */
int srcu_read_lock(struct srcu_struct *sp);
void srcu_read_unlock(struct srcu_struct *sp, int idx);
void synchronize_srcu(struct srcu_struct *sp);

/* ========== SRCU 增强优化 ========== */

/* SRCU 初始化 (分配 per-CPU 引用计数) */
int srcu_init(struct srcu_struct *sp, const char *name);

/* SRCU 清理 */
void srcu_cleanup(struct srcu_struct *sp);

/* SRCU 异步回调 */
void call_srcu(struct srcu_struct *sp, struct rcu_head *head,
               void (*func)(struct rcu_head *head));

/* SRCU 加速同步 (减少等待时间) */
void synchronize_srcu_expedited(struct srcu_struct *sp);

/* 获取 SRCU 状态信息 */
void srcu_get_stats(struct srcu_struct *sp, uint64_t *gp_count,
                    uint64_t *total_reads, uint64_t *pending_reads);

/* ========== RCU 工具函数 ========== */

/* 初始化 RCU */
int rcu_init(void);

/* 处理 RCU 回调 */
void rcu_process_callbacks(void);

/* 宽限期启动 */
void rcu_gp_kthread(void);

/* 检查是否需要 QS */
int rcu_implicit_cpu_qs(struct rcu_data *rdp);

/* CPU 上线/下线 */
int rcu_cpu_online(int cpu);
int rcu_cpu_offline(int cpu);

/* 报告静默状态 */
void rcu_report_qs_rdp(int cpu, struct rcu_data *rdp);

/* ========== 调试支持 ========== */

extern int debug_rcu_head_enabled;

#ifdef CONFIG_DEBUG_RCU_HEAD
void debug_rcu_head_queue(struct rcu_head *head);
void debug_rcu_head_unqueue(struct rcu_head *head);
#define RCU_LOCKDEP_WARN(c, s) \
    do { if (c) println(s); } while (0)
#else
#define debug_rcu_head_queue(head)   do { } while (0)
#define debug_rcu_head_unqueue(head) do { } while (0)
#define RCU_LOCKDEP_WARN(c, s)       do { } while (0)
#endif

/* ========== 高级 API ========== */

/* 使用 RCU 替换指针并同步 */
#define RCU_REPLACE_POINTER(rcu_ptr, new_ptr) \
    do { \
        typeof(rcu_ptr) ___old = rcu_dereference(rcu_ptr); \
        rcu_assign_pointer(rcu_ptr, new_ptr); \
        synchronize_rcu(); \
        ___old; \
    } while (0)

/* 安全释放 RCU 保护的内存 */
#define kfree_rcu(ptr, rcu_head) \
    do { \
        call_rcu(&(ptr)->rcu_head, kfree_rcu_callback); \
    } while (0)

void kfree_rcu_callback(struct rcu_head *head);

/* ========== Grace Period 检测优化 ========== */

/* 批量宽限期完成 (减少 IPI 数量) */
void rcu_batch_gp_complete(void);

/* 加速宽限期检测 (使用更少的 IPI) */
void rcu_accelerate_gp(void);

/* 获取当前宽限期状态信息 */
int rcu_gp_get_info(uint64_t *gp_seq, int *gp_state,
                    uint64_t *gp_duration, uint32_t *pending_cbs);

/* ========== RCU 回调批处理增强 ========== */

/* 回调批处理配置 */
#define RCU_CB_BATCH_THRESHOLD   32    /* 触发批处理的回调数阈值 */
#define RCU_CB_BATCH_MAX_SIZE    256   /* 单批次最大回调数 */

/* 初始化回调批处理 */
void rcu_cb_batch_init(void);

/* 刷新所有待处理的回调批次 */
void rcu_cb_batch_flush_all(void);

/* 获取回调批处理统计 */
void rcu_cb_batch_get_stats(uint64_t *total_batches, uint64_t *total_cbs,
                            uint64_t *avg_batch_size);

/* ========== 内存屏障包装 ========== */

#define READ_ONCE(x) \
    (*(volatile typeof(x) *)&(x))

#define WRITE_ONCE(x, v) \
    do { \
        *(volatile typeof(x) *)&(x) = (v); \
    } while (0)

#endif /* RCU_H */
