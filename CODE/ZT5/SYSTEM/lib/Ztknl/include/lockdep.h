/*
 * Zratro OS - Lock Dependency and Deadlock Prevention
 * 
 * 锁层级系统 - 防止死锁
 * 
 * 规则：
 * 1. 锁必须按照层级从低到高的顺序获取
 * 2. 不能反向获取锁（会导致死锁）
 * 3. 同层级锁按地址顺序获取
 */

#ifndef _LOCKDEP_H
#define _LOCKDEP_H

#include "ktypes.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 锁层级定义
 * 
 * 层级越高，越先获取
 * 数字越大，层级越高
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 全局锁层级 (最高优先级) */
#define LOCK_LEVEL_GLOBAL       100     /* 全局系统锁 */

/* 子系统锁层级 */
#define LOCK_LEVEL_SCHEDULER    90      /* 调度器锁 */
#define LOCK_LEVEL_TASK         85      /* 任务管理锁 */
#define LOCK_LEVEL_SIGNAL       80      /* 信号处理锁 */
#define LOCK_LEVEL_SMP          75      /* SMP 锁 */

/* 内存管理锁层级 */
#define LOCK_LEVEL_VMM          70      /* 虚拟内存锁 */
#define LOCK_LEVEL_PMM          65      /* 物理内存锁 */
#define LOCK_LEVEL_HEAP         60      /* 堆锁 */
#define LOCK_LEVEL_SLAB         55      /* Slab 锁 */
#define LOCK_LEVEL_ZALLOC       50      /* Z-Alloc 锁 */

/* 文件系统锁层级 */
#define LOCK_LEVEL_VFS          45      /* VFS 锁 */
#define LOCK_LEVEL_FS           40      /* 文件系统锁 (FAT32/EXT4) */
#define LOCK_LEVEL_JOURNAL      35      /* 日志锁 */
#define LOCK_LEVEL_ZRING        30      /* Z-Ring 锁 */

/* 设备锁层级 */
#define LOCK_LEVEL_PCI          25      /* PCI 锁 */
#define LOCK_LEVEL_AHCI         20      /* AHCI 锁 */
#define LOCK_LEVEL_DISK         15      /* 磁盘锁 */

/* 网络锁层级 */
#define LOCK_LEVEL_NET          12      /* 网络协议栈锁 */
#define LOCK_LEVEL_SOCKET       10      /* 套接字锁 */

/* 图形锁层级 */
#define LOCK_LEVEL_DRM          8       /* DRM 锁 */
#define LOCK_LEVEL_FB           5       /* 帧缓冲锁 */

/* 其他锁层级 */
#define LOCK_LEVEL_DRIVER       3       /* 驱动锁 */
#define LOCK_LEVEL_MISC         1       /* 杂项锁 */
#define LOCK_LEVEL_NONE         0       /* 无层级 */

/* ═══════════════════════════════════════════════════════════════════════════
 * 带调试信息的自旋锁
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef LOCKDEP_ENABLE

/* 带调试信息的自旋锁结构 */
struct lockdep_spinlock {
    volatile int locked;           /* 锁状态 */
    int level;                     /* 锁层级 */
    const char *name;              /* 锁名称 */
    const char *file;              /* 获取锁的文件 */
    int line;                      /* 获取锁的行号 */
    void *owner;                   /* 锁持有者 (CPU 或任务) */
    uint64_t acquire_time;         /* 获取时间 */
};

/* 当前 CPU 的锁状态 */
struct lockdep_cpu_state {
    int current_level;             /* 当前持有的最高层级 */
    int lock_count;                /* 持有锁的数量 */
    struct lockdep_spinlock *held_locks[16]; /* 持有的锁 */
};

/* 声明全局 CPU 锁状态 */
extern struct lockdep_cpu_state lockdep_cpu_states[];

/* 获取当前 CPU 的锁状态 */
static inline struct lockdep_cpu_state *lockdep_get_cpu_state(void) {
    /* 简化：使用 CPU 0 */
    return &lockdep_cpu_states[0];
}

/* 检查锁层级是否有效 */
static inline int lockdep_check_level(int level) {
    struct lockdep_cpu_state *state = lockdep_get_cpu_state();
    
    /* 新锁层级必须 >= 当前层级 */
    if (level < state->current_level) {
        /* 潜在死锁风险！ */
        return -1;  /* 错误：违反层级规则 */
    }
    
    return 0;  /* OK */
}

/* 带层级检查的自旋锁初始化 */
#define spin_lock_init_level(lock, lvl) \
    do { \
        (lock)->locked = 0; \
        (lock)->level = (lvl); \
        (lock)->name = #lock; \
        (lock)->file = NULL; \
        (lock)->line = 0; \
        (lock)->owner = NULL; \
    } while (0)

/* 带调试的锁获取 */
static inline void lockdep_spin_lock(struct lockdep_spinlock *lock,
                                     const char *file, int line) {
    struct lockdep_cpu_state *state = lockdep_get_cpu_state();
    
    /* 检查层级 */
    if (lock->level < state->current_level) {
        /* 死锁风险！记录警告 */
        /* TODO: 调用死锁检测处理程序 */
    }
    
    /* 自旋等待 */
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        asm volatile("pause");
    }
    
    /* 记录锁信息 */
    lock->file = file;
    lock->line = line;
    lock->owner = (void *)1;  /* TODO: 使用实际的 CPU/任务 ID */
    lock->acquire_time = 0;   /* TODO: 使用实际时间 */
    
    /* 更新 CPU 状态 */
    if (lock->level > state->current_level) {
        state->current_level = lock->level;
    }
    if (state->lock_count < 16) {
        state->held_locks[state->lock_count++] = lock;
    }
}

/* 带调试的锁释放 */
static inline void lockdep_spin_unlock(struct lockdep_spinlock *lock) {
    struct lockdep_cpu_state *state = lockdep_get_cpu_state();
    
    /* 清除锁信息 */
    lock->file = NULL;
    lock->line = 0;
    lock->owner = NULL;
    
    /* 更新 CPU 状态 */
    for (int i = 0; i < state->lock_count; i++) {
        if (state->held_locks[i] == lock) {
            /* 移除锁 */
            for (int j = i; j < state->lock_count - 1; j++) {
                state->held_locks[j] = state->held_locks[j + 1];
            }
            state->lock_count--;
            break;
        }
    }
    
    /* 重新计算当前层级 */
    state->current_level = LOCK_LEVEL_NONE;
    for (int i = 0; i < state->lock_count; i++) {
        if (state->held_locks[i]->level > state->current_level) {
            state->current_level = state->held_locks[i]->level;
        }
    }
    
    __sync_lock_release(&lock->locked);
}

/* 宏：带调试信息的锁获取 */
#define spin_lock_dep(lock) \
    lockdep_spin_lock((lock), __FILE__, __LINE__)

#define spin_unlock_dep(lock) \
    lockdep_spin_unlock(lock)

/* 使用带层级的锁 */
typedef struct lockdep_spinlock spinlock_dep_t;

#define SPINLOCK_DEP_INIT_LEVEL(level) \
    { 0, level, NULL, NULL, 0, NULL, 0 }

#define SPINLOCK_DEP_INIT \
    SPINLOCK_DEP_INIT_LEVEL(LOCK_LEVEL_MISC)

#else /* !LOCKDEP_ENABLE */

/* 不启用锁调试时，使用原始自旋锁 */
typedef volatile int spinlock_dep_t;
#define spin_lock_init_level(lock, lvl) (*(lock) = 0)
#define spin_lock_dep(lock) spin_lock(lock)
#define spin_unlock_dep(lock) spin_unlock(lock)
#define SPINLOCK_DEP_INIT 0

#endif /* LOCKDEP_ENABLE */

/* ═══════════════════════════════════════════════════════════════════════════
 * 双锁安全获取宏
 * 
 * 用于安全地获取两个锁，避免死锁
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 安全获取两个自旋锁（按地址排序） */
#define double_spin_lock(l1, l2) \
    do { \
        spinlock_t *__l1 = (l1); \
        spinlock_t *__l2 = (l2); \
        if (__l1 < __l2) { \
            spin_lock(__l1); \
            spin_lock(__l2); \
        } else if (__l1 > __l2) { \
            spin_lock(__l2); \
            spin_lock(__l1); \
        } else { \
            spin_lock(__l1); \
        } \
    } while (0)

/* 安全释放两个自旋锁 */
#define double_spin_unlock(l1, l2) \
    do { \
        spinlock_t *__l1 = (l1); \
        spinlock_t *__l2 = (l2); \
        if (__l1 < __l2) { \
            spin_unlock(__l2); \
            spin_unlock(__l1); \
        } else if (__l1 > __l2) { \
            spin_unlock(__l1); \
            spin_unlock(__l2); \
        } else { \
            spin_unlock(__l1); \
        } \
    } while (0)

/* ═══════════════════════════════════════════════════════════════════════════
 * 读写锁 (Read-Copy-Update 风格)
 * ═══════════════════════════════════════════════════════════════════════════ */

struct rwlock {
    volatile int readers;     /* 读者计数 */
    volatile int writer;      /* 写者标志 */
    spinlock_t lock;          /* 内部锁 */
};

#define RWLOCK_INIT { 0, 0, SPINLOCK_INIT }

static inline void rwlock_init(struct rwlock *rw) {
    rw->readers = 0;
    rw->writer = 0;
    rw->lock = SPINLOCK_INIT;
}

/* 获取读锁 */
static inline void read_lock(struct rwlock *rw) {
    spin_lock(&rw->lock);
    rw->readers++;
    spin_unlock(&rw->lock);
}

/* 释放读锁 */
static inline void read_unlock(struct rwlock *rw) {
    spin_lock(&rw->lock);
    rw->readers--;
    spin_unlock(&rw->lock);
}

/* 获取写锁 */
static inline void write_lock(struct rwlock *rw) {
    spin_lock(&rw->lock);
    while (rw->readers > 0) {
        spin_unlock(&rw->lock);
        asm volatile("pause");
        spin_lock(&rw->lock);
    }
    rw->writer = 1;
    spin_unlock(&rw->lock);
}

/* 释放写锁 */
static inline void write_unlock(struct rwlock *rw) {
    spin_lock(&rw->lock);
    rw->writer = 0;
    spin_unlock(&rw->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原子操作增强 (atomic_t 结构版本)
 * 注意：ktypes.h 中已有针对 volatile int 的 atomic_* 宏
 * 这里的 atomic_t 是结构体封装版本，用于类型安全
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 原子变量结构体 */
typedef struct {
    volatile long value;
} atomic_long_t;

#define ATOMIC_LONG_INIT(v) { (v) }

static inline long atomic_long_read(atomic_long_t *a) {
    return a->value;
}

static inline void atomic_long_set(atomic_long_t *a, long v) {
    a->value = v;
}

static inline long atomic_long_add(atomic_long_t *a, long v) {
    return __sync_add_and_fetch(&a->value, v);
}

static inline long atomic_long_sub(atomic_long_t *a, long v) {
    return __sync_sub_and_fetch(&a->value, v);
}

static inline long atomic_long_inc(atomic_long_t *a) {
    return __sync_add_and_fetch(&a->value, 1);
}

static inline long atomic_long_dec(atomic_long_t *a) {
    return __sync_sub_and_fetch(&a->value, 1);
}

static inline int atomic_long_cmpxchg(atomic_long_t *a, long old, long new) {
    return __sync_bool_compare_and_swap(&a->value, old, new);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 引用计数
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef atomic_long_t refcount_t;

#define REFCOUNT_INIT(n) ATOMIC_LONG_INIT(n)

static inline void refcount_set(refcount_t *r, int n) {
    atomic_long_set(r, n);
}

static inline int refcount_read(refcount_t *r) {
    return atomic_long_read(r);
}

static inline void refcount_inc(refcount_t *r) {
    atomic_long_inc(r);
}

/* 返回 1 如果引用计数变为 0 */
static inline int refcount_dec_and_test(refcount_t *r) {
    return atomic_long_dec(r) == 0;
}

#endif /* _LOCKDEP_H */
