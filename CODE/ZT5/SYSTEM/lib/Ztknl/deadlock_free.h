/*
 * Zratro OS - Deadlock-Free Kernel Design
 * 
 * 零死锁内核 - 超越 Linux 的锁管理
 * 
 * @file deadlock_free.h
 * @brief 死锁预防和检测机制
 * 
 * Linux 缺陷:
 *   - 每年都有新的死锁 bug
 *   - 调试困难，复现成本高
 *   - 锁层级复杂，容易违反
 * 
 * ZratroOS 方案:
 *   - 编译时锁依赖检查
 *   - 运行时死锁检测
 *   - 自动超时释放
 *   - 锁持有时间监控
 */

#ifndef DEADLOCK_FREE_H
#define DEADLOCK_FREE_H

#include "ktypes.h"
#include "spinlock.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 智能自旋锁 (带死锁检测)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 增强型自旋锁结构
 * 
 * 相比 Linux spinlock 的改进:
 *   - 记录持有者信息
 *   - 超时自动释放
 *   - 调用栈追踪
 *   - 锁依赖图检查
 */
struct smart_spinlock {
    spinlock_t lock;              /* 基础锁 */
    
    uint64_t owner_pid;           /* 持有者 PID */
    uint64_t owner_tid;           /* 持有者 TID */
    uint64_t acquire_time;        /* 获取时间戳 */
    const char *acquire_location; /* 获取位置 (文件：行) */
    
    uint64_t timeout_ticks;       /* 超时时间 (ticks), 0=无限制 */
    uint32_t recursion_count;     /* 递归计数 (支持递归锁) */
    
    uint64_t contention_count;    /* 竞争次数统计 */
    uint64_t hold_time_total;     /* 总持有时间 */
    uint64_t hold_time_max;       /* 最大持有时间 */
    
    uint32_t flags;               /* 锁标志 */
    uint32_t lock_class;          /* 锁类别 (用于依赖检查) */
};

/* 锁标志 */
#define SMART_LOCK_RECURSIVE   0x01    /* 递归锁 */
#define SMART_LOCK_TIMEOUT     0x02    /* 启用超时 */
#define SMART_LOCK_DEBUG       0x04    /* 调试模式 */
#define SMART_LOCK_DEADLOCKED  0x80    /* 检测到死锁 */

/* 默认超时时间 (100ms) */
#define SMART_LOCK_DEFAULT_TIMEOUT  100

/* ═══════════════════════════════════════════════════════════════════════════
 * 锁依赖图 (Lock Dependency Graph)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_LOCK_CLASSES    256
#define MAX_LOCK_DEPS       1024

/**
 * @brief 锁类别 (用于死锁检测)
 */
struct lock_class {
    const char *name;         /* 锁类别名称 */
    uint32_t class_id;        /* 类别 ID */
    uint32_t dep_count;       /* 依赖数量 */
    uint32_t deps[MAX_LOCK_CLASSES];  /* 依赖列表 */
};

/**
 * @brief 全局锁依赖图
 */
struct lock_dep_graph {
    struct lock_class classes[MAX_LOCK_CLASSES];
    int class_count;
    spinlock_t graph_lock;    /* 保护依赖图 */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * API 函数声明
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 初始化 */
void smart_lock_init(void);
void smart_spin_lock_init(struct smart_spinlock *lock, const char *name, 
                          uint32_t lock_class, uint64_t timeout_ticks);

/* 锁操作 */
int smart_spin_lock(struct smart_spinlock *lock, const char *location);
int smart_spin_lock_irqsave(struct smart_spinlock *lock, uint64_t *flags,
                            const char *location);
void smart_spin_unlock(struct smart_spinlock *lock);
void smart_spin_unlock_irqrestore(struct smart_spinlock *lock, uint64_t flags);

/* 超时控制 */
int smart_spin_lock_timeout(struct smart_spinlock *lock, uint64_t timeout_ticks,
                            const char *location);

/* 调试接口 */
int smart_lock_is_locked(struct smart_spinlock *lock);
uint64_t smart_lock_hold_time(struct smart_spinlock *lock);
void smart_lock_dump_stats(struct smart_spinlock *lock);
void smart_lock_dump_all(void);

/* 死锁检测 */
int detect_deadlock(void);
void print_lock_chain(struct smart_spinlock *lock);

/* 锁依赖注册 */
uint32_t register_lock_class(const char *name);
void add_lock_dependency(uint32_t from_class, uint32_t to_class);
int check_lock_order(uint32_t current_class, uint32_t new_class);

#endif /* DEADLOCK_FREE_H */
