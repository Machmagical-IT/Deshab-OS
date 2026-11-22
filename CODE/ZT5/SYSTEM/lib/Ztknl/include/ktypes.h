/*
 * ╔═══════════════════════════════════════════════════════════════════════════╗
 * ║  ███████╗██████╗  ██████╗ ███████╗ █████╗  ██████╗ ██████╗ ███████╗     ║
 * ║  ╚══███╔╝██╔══██╗██╔═══██╗██╔════╝██╔══██╗██╔════╝██╔═══██╗██╔════╝     ║
 * ║    ███╔╝ ██████╔╝██║   ██║███████╗███████║██║     ██║   ██║█████╗       ║
 * ║   ███╔╝  ██╔══██╗██║   ██║╚════██║██╔══██║██║     ██║   ██║██╔══╝       ║
 * ║  ███████╗██║  ██║╚██████╔╝███████║██║  ██║╚██████╗╚██████╔╝███████╗     ║
 * ║  ╚══════╝╚═╝  ╚═╝ ╚═════╝ ╚══════╝╚═╝  ╚═╝ ╚═════╝ ╚═════╝ ╚══════╝     ║
 * ╠═══════════════════════════════════════════════════════════════════════════╣
 * ║  Zratro OS Kernel - Infrastructure Types                                  ║
 * ║  Version: 1.0.0 | Build: 2026.03 | Arch: x86-64 (Higher Half)            ║
 * ║  Copyright (c) 2026 Zratro Project. All rights reserved.                  ║
 * ╚═══════════════════════════════════════════════════════════════════════════╝
 */

#ifndef KTYPES_H
#define KTYPES_H

#include <stdint.h>
#include <stddef.h>

/* ========== 内核自定义类型（避免与系统头文件冲突）========== */

/* 有符号大小类型 */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int64_t ssize_t;
#endif

/* PID/UID/GID 类型 */
#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int32_t pid_t;
#endif
#ifndef _UID_T_DEFINED
#define _UID_T_DEFINED
typedef int32_t uid_t;
#endif
#ifndef _GID_T_DEFINED
#define _GID_T_DEFINED
typedef int32_t gid_t;
#endif

/* 偏移量类型 */
#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
typedef int64_t off_t;
#endif

/* 时间类型 */
#ifndef _TIME_T_DEFINED
#define _TIME_T_DEFINED
typedef int64_t time_t;
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * ZRATRO UNIQUE STYLE SYSTEM (ZUSS)
 * 独特的代码风格系统，提供一致的命名约定和代码模式
 * ═══════════════════════════════════════════════════════════════════════════ */

/*─── 版本标识 ───*/
#define ZRATRO_VERSION_MAJOR    1
#define ZRATRO_VERSION_MINOR    0
#define ZRATRO_VERSION_PATCH    0
#define ZRATRO_VERSION_STRING   "1.0.0"
#define ZRATRO_CODENAME         "Aurora"        /* 极光 - 独特代号 */
#define ZRATRO_BUILD_DATE       __DATE__
#define ZRATRO_BUILD_TIME       __TIME__

/*─── 独特命名宏 ───*/
#define Z_(prefix, name)        prefix##_##name          /* Zratro 连接宏 */
#define Z_EXTERN                extern                    /* 外部声明标记 */
#define Z_STATIC                static                    /* 静态声明标记 */
#define Z_INLINE                static inline             /* 内联函数标记 */
#define Z_API                   __attribute__((visibility("default")))
#define Z_EXPORT                __attribute__((visibility("default")))

/*─── 独特类型前缀 ───*/
/* 使用 z_ 前缀表示 Zratro 原生类型 */
typedef uint8_t     z_u8;
typedef uint16_t    z_u16;
typedef uint32_t    z_u32;
typedef uint64_t    z_u64;
typedef int8_t      z_i8;
typedef int16_t     z_i16;
typedef int32_t     z_i32;
typedef int64_t     z_i64;
typedef size_t      z_size;
typedef intptr_t    z_ptr;
typedef uintptr_t   z_uptr;

/*─── 独特指针标记 ───*/
#define Z_NULL                  ((void *)0)                        /* 统一空指针 */
#define Z_USER_NULL             ((void *)0)                       /* 用户空指针 */
#define Z_KERNEL_BASE           ((void *)0xFFFFFFFF80000000ULL)   /* 内核基址 */
#define Z_IS_KERNEL_PTR(p)      ((z_uptr)(p) >= 0xFFFFFFFF80000000ULL)
#define Z_IS_USER_PTR(p)        ((z_uptr)(p) < 0x0000800000000000ULL)

/*─── 独特布尔风格 ───*/
typedef enum {
    Z_FALSE = 0,
    Z_TRUE = 1
} z_bool;

#define z_true                 Z_TRUE
#define z_false                Z_FALSE

/*─── 独特日志风格 ───*/
#define Z_LOG(level, fmt, ...)  __z_log_print(level, "[ZRATRO] " fmt, ##__VA_ARGS__)
#define Z_TRACE(fmt, ...)       Z_LOG(0, "[TRACE] " fmt, ##__VA_ARGS__)
#define Z_DEBUG(fmt, ...)       Z_LOG(1, "[DEBUG] " fmt, ##__VA_ARGS__)
#define Z_INFO(fmt, ...)        Z_LOG(2, "[INFO] " fmt, ##__VA_ARGS__)
#define Z_WARN(fmt, ...)        Z_LOG(3, "[WARN] " fmt, ##__VA_ARGS__)
#define Z_ERROR(fmt, ...)       Z_LOG(4, "[ERROR] " fmt, ##__VA_ARGS__)
#define Z_FATAL(fmt, ...)       do { Z_LOG(5, "[FATAL] " fmt, ##__VA_ARGS__); __z_halt(); } while(0)

/*─── 独特代码块标记 ───*/
#define Z_BEGIN_DECLS           extern "C" {   /* C++ 兼容开始 */
#define Z_END_DECLS             }              /* C++ 兼容结束 */

/*─── 独特作用域守卫 ───*/
#define Z_SCOPE_EXIT(action) \
    __attribute__((cleanup(__z_scope_cleanup_##action))) z_i32 __z_scope_var_##action
#define Z_DEFER(name, code) \
    static void __z_scope_cleanup_##name(z_i32 *z_unused) { (void)z_unused; code; }

/*─── 独特模块标记 ───*/
#define Z_MODULE(name)          static const char __z_module_name[] = #name
#define Z_AUTHOR(name)          static const char __z_author_name[] = name
#define Z_LICENSE(type)         static const char __z_license_type[] = #type
#define Z_DESCRIPTION(text)     static const char __z_description[] = text

/*─── 独特函数属性 ───*/
#define Z_COLD                  __attribute__((cold))
#define Z_HOT                   __attribute__((hot))
#define Z_NORETURN              __attribute__((noreturn))
#define Z_MALLOC                __attribute__((malloc))
#define Z_PRINTF(a, b)          __attribute__((format(printf, a, b)))
#define Z_NONNULL(...)          __attribute__((nonnull(__VA_ARGS__)))
#define Z_PURE                  __attribute__((pure))
#define Z_CONST                 __attribute__((const))
#define Z_UNUSED                __attribute__((unused))
#define Z_USED                  __attribute__((used))
#define Z_PACKED                __attribute__((packed))
#define Z_ALIGNED(x)            __attribute__((aligned(x)))
#define Z_WEAK                  __attribute__((weak))
#define Z_ALIAS(name)           __attribute__((alias(#name)))
#define Z_CONSTRUCTOR           __attribute__((constructor))
#define Z_DESTRUCTOR            __attribute__((destructor))

/*─── 独特循环宏 ───*/
#define Z_FOREACH(var, array)   for (size_t __z_i = 0, var = array[0]; \
                                     __z_i < ARRAY_SIZE(array); \
                                     var = array[++__z_i])
#define Z_FOREACH_SAFE(var, array, n) \
                                for (size_t __z_i = 0; __z_i < (n); ++__z_i) \
                                    if (var = array[__z_i], 1)
#define Z_REPEAT(n)             for (size_t __z_rep = 0; __z_rep < (n); ++__z_rep)
#define Z_FOREVER               for (;;)

/*─── 独特交换/比较宏 ───*/
#define Z_SWAP(a, b)            do { typeof(a) __z_tmp = (a); (a) = (b); (b) = __z_tmp; } while(0)
#define Z_MIN(a, b)             ({ typeof(a) __z_a = (a); typeof(b) __z_b = (b); __z_a < __z_b ? __z_a : __z_b; })
#define Z_MAX(a, b)             ({ typeof(a) __z_a = (a); typeof(b) __z_b = (b); __z_a > __z_b ? __z_a : __z_b; })
#define Z_CLAMP(x, lo, hi)      Z_MIN(Z_MAX(x, lo), hi)
#define Z_ABS(x)                ({ typeof(x) __z_x = (x); __z_x < 0 ? -__z_x : __z_x; })
#define Z_SIGN(x)               ({ typeof(x) __z_x = (x); (__z_x > 0) - (__z_x < 0); })

/*─── 独特位操作风格 ───*/
#define Z_SET_BIT(val, bit)     ((val) |= (1ULL << (bit)))
#define Z_CLEAR_BIT(val, bit)   ((val) &= ~(1ULL << (bit)))
#define Z_TEST_BIT(val, bit)    (!!((val) & (1ULL << (bit))))
#define Z_TOGGLE_BIT(val, bit)  ((val) ^= (1ULL << (bit)))
#define Z_MASK(bits)            ((1ULL << (bits)) - 1)
#define Z_ALIGN_UP(x, align)    (((x) + (align) - 1) & ~((align) - 1))
#define Z_ALIGN_DOWN(x, align)  ((x) & ~((align) - 1))

/*─── 独特字符串哈希 ───*/
#define Z_HASH_INIT             0x811c9dc5U     /* FNV-1a 初始值 */
#define Z_HASH_PRIME            0x01000193U     /* FNV-1a 质数 */
#define Z_HASH_STR(str)         __z_hash_str(str, sizeof(str) - 1)
#define Z_HASH_UPDATE(h, byte)  ((h) = ((h) ^ (byte)) * Z_HASH_PRIME)

/*─── 独特容器宏 ───*/
#define Z_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define Z_ENTRY(ptr, type, member) \
    Z_CONTAINER_OF(ptr, type, member)

/*─── 独特链表风格 (红黑配色) ───*/
#define Z_LIST_HEAD(name)       struct list_head name = { &(name), &(name) }
#define Z_INIT_LIST_HEAD(ptr)   do { (ptr)->next = (ptr); (ptr)->prev = (ptr); } while(0)

/* ═══════════════════════════════════════════════════════════════════════════
 * 基础内核类型 (保持兼容)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 基本常量 */
#ifndef INT_MAX
#define INT_MAX     0x7FFFFFFF
#endif

#ifndef UINT_MAX
#define UINT_MAX    0xFFFFFFFFU
#endif

/* 自旋锁 */
typedef volatile int spinlock_t;
#define SPINLOCK_INIT      0
#define spinlock_init(l)   (*(l) = 0)
#define spin_lock_init(l)  (*(l) = 0)  /* 兼容别名 */

static inline void spin_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        asm volatile("pause");
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    __sync_lock_release(lock);
}

/* 双重自旋锁 - 按地址顺序获取两个锁，避免死锁 */
static inline void double_spin_lock(spinlock_t *lock1, spinlock_t *lock2) {
    /* 按地址顺序获取锁，确保所有线程以相同顺序获取锁 */
    if (lock1 < lock2) {
        spin_lock(lock1);
        spin_lock(lock2);
    } else if (lock1 > lock2) {
        spin_lock(lock2);
        spin_lock(lock1);
    } else {
        /* 同一个锁，只需获取一次 */
        spin_lock(lock1);
    }
}

/* 双重自旋解锁 - 按相反顺序释放 */
static inline void double_spin_unlock(spinlock_t *lock1, spinlock_t *lock2) {
    if (lock1 != lock2) {
        spin_unlock(lock1);
        spin_unlock(lock2);
    } else {
        spin_unlock(lock1);
    }
}

/* 带中断保护的自旋锁 */
static inline unsigned long spin_lock_irqsave(spinlock_t *lock) {
    unsigned long flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags));
    spin_lock(lock);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags) {
    spin_unlock(lock);
    asm volatile("pushq %0; popfq" : : "r"(flags) : "memory");
}

/* 自旋锁尝试锁定 */
static inline int spin_trylock(spinlock_t *lock) {
    return __sync_lock_test_and_set(lock, 1) == 0;
}

/* ========== 自创算法: 自适应退避自旋锁 (Adaptive Backoff Spinlock) ==========
 * 
 * 原理说明:
 * - 低竞争时：快速重试 (pause 指令)，最小化延迟
 * - 中等竞争：线性退避，逐步增加等待时间
 * - 高竞争时：指数退避，减少总线争用和缓存行颠簸
 * 
 * 创新点:
 * 1. 动态检测竞争程度，自动选择最优策略
 * 2. 基于历史等待次数的自适应学习
 * 3. 避免传统固定退避策略的效率损失
 */
typedef struct {
    volatile int locked;
    volatile uint32_t wait_count;     /* 总等待次数统计 */
    volatile uint32_t success_fast;   /* 快速路径成功次数 */
    volatile uint32_t success_backoff;/* 退避路径成功次数 */
} adaptive_spinlock_t;

#define ADAPTIVE_SPINLOCK_INIT  { 0, 0, 0, 0 }
#define adaptive_spinlock_init(l) do { \
    (l)->locked = 0; \
    (l)->wait_count = 0; \
    (l)->success_fast = 0; \
    (l)->success_backoff = 0; \
} while(0)

/* 内联函数: 精确延迟 (基于 TSC) */
static inline void adaptive_delay(uint64_t cycles) {
    if (cycles == 0) return;
    uint64_t start;
    asm volatile("rdtsc" : "=A"(start));
    uint64_t end = start + cycles;
    while (1) {
        uint64_t now;
        asm volatile("rdtsc" : "=A"(now));
        if (now >= end) break;
        asm volatile("pause");
    }
}

/* 自适应获取锁 */
static inline void adaptive_spin_lock(adaptive_spinlock_t *lock) {
    uint32_t spins = 0;
    uint32_t backoff = 1;
    const uint32_t MAX_BACKOFF = 16384;  /* 最大退避周期 */
    const uint32_t FAST_THRESHOLD = 100; /* 快速路径阈值 */
    
    while (1) {
        /* 尝试快速获取 (无竞争情况) */
        if (__sync_lock_test_and_set(&lock->locked, 1) == 0) {
            /* 成功获取锁 */
            __sync_fetch_and_add(&lock->wait_count, spins);
            if (spins < FAST_THRESHOLD) {
                __sync_fetch_and_add(&lock->success_fast, 1);
            } else {
                __sync_fetch_and_add(&lock->success_backoff, 1);
            }
            return;
        }
        
        spins++;
        
        /* 自适应策略选择 */
        if (spins < FAST_THRESHOLD) {
            /* 低竞争: 快速重试 */
            for (uint32_t i = 0; i < 8; i++) {
                asm volatile("pause");
            }
        } else if (spins < FAST_THRESHOLD * 4) {
            /* 中等竞争: 小幅退避 */
            adaptive_delay(backoff * 10);
            if (backoff < 64) backoff++;
        } else {
            /* 高竞争: 指数退避 */
            adaptive_delay(backoff);
            backoff = backoff * 2;
            if (backoff > MAX_BACKOFF) backoff = MAX_BACKOFF;
        }
    }
}

/* 自适应释放锁 */
static inline void adaptive_spin_unlock(adaptive_spinlock_t *lock) {
    __sync_lock_release(&lock->locked);
}

/* 带中断保护的自适应锁 */
static inline unsigned long adaptive_spin_lock_irqsave(adaptive_spinlock_t *lock) {
    unsigned long flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags));
    adaptive_spin_lock(lock);
    return flags;
}

static inline void adaptive_spin_unlock_irqrestore(adaptive_spinlock_t *lock, unsigned long flags) {
    adaptive_spin_unlock(lock);
    asm volatile("pushq %0; popfq" : : "r"(flags) : "memory");
}

/* 获取锁统计信息 (用于性能分析) */
static inline void adaptive_spinlock_stats(adaptive_spinlock_t *lock, 
                                           uint32_t *total, 
                                           uint32_t *fast, 
                                           uint32_t *backoff) {
    if (total) *total = lock->wait_count;
    if (fast) *fast = lock->success_fast;
    if (backoff) *backoff = lock->success_backoff;
}

/* ========== Ticket Lock (公平自旋锁) ========== */

/* 票据锁结构 */
typedef struct {
    volatile uint16_t next_ticket;    /* 下一个票据号 */
    volatile uint16_t serving;        /* 当前服务的票据号 */
} ticketlock_t;

#define TICKETLOCK_INIT    { 0, 0 }
#define ticketlock_init(l) do { (l)->next_ticket = 0; (l)->serving = 0; } while(0)

/* 获取票据锁 */
static inline void ticket_lock(ticketlock_t *lock) {
    /* 获取票据 */
    uint16_t my_ticket = __sync_fetch_and_add(&lock->next_ticket, 1);
    
    /* 等待轮到自己 */
    while (lock->serving != my_ticket) {
        asm volatile("pause");
    }
}

/* 释放票据锁 */
static inline void ticket_unlock(ticketlock_t *lock) {
    /* 增加服务号，唤醒下一个等待者 */
    __sync_add_and_fetch(&lock->serving, 1);
}

/* 尝试获取票据锁 */
static inline int ticket_trylock(ticketlock_t *lock) {
    uint16_t next = lock->next_ticket;
    if (lock->serving != next) {
        return 0;  /* 已被锁定 */
    }
    /* 尝试原子地增加 next_ticket */
    if (__sync_bool_compare_and_swap(&lock->next_ticket, next, next + 1)) {
        /* 成功获取票据，等待轮到自己 */
        while (lock->serving != next) {
            asm volatile("pause");
        }
        return 1;
    }
    return 0;
}

/* 带中断保护的票据锁 */
static inline unsigned long ticket_lock_irqsave(ticketlock_t *lock) {
    unsigned long flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags));
    ticket_lock(lock);
    return flags;
}

static inline void ticket_unlock_irqrestore(ticketlock_t *lock, unsigned long flags) {
    ticket_unlock(lock);
    asm volatile("pushq %0; popfq" : : "r"(flags) : "memory");
}

/* 检查票据锁是否被锁定 */
static inline int ticket_is_locked(ticketlock_t *lock) {
    return lock->next_ticket != lock->serving;
}

/* 获取等待者数量 */
static inline int ticket_waiters(ticketlock_t *lock) {
    return lock->next_ticket - lock->serving;
}

/* 互斥锁 */
struct mutex {
    volatile int locked;
    volatile int count;
    void *owner;
};

static inline void mutex_init(struct mutex *m) {
    m->locked = 0;
    m->count = 0;
    m->owner = NULL;
}

static inline void mutex_lock(struct mutex *m) {
    while (__sync_lock_test_and_set(&m->locked, 1)) {
        asm volatile("pause");
    }
    m->owner = __builtin_return_address(0);
}

static inline void mutex_unlock(struct mutex *m) {
    m->owner = NULL;
    __sync_lock_release(&m->locked);
}

/* 互斥锁尝试锁定 */
static inline int mutex_trylock(struct mutex *m) {
    if (__sync_lock_test_and_set(&m->locked, 1) == 0) {
        m->owner = __builtin_return_address(0);
        return 1;
    }
    return 0;
}

/* 引用计数 */
struct kref {
    volatile int count;
};

static inline void kref_init(struct kref *kref) {
    kref->count = 1;
}

static inline void kref_get(struct kref *kref) {
    __sync_add_and_fetch(&kref->count, 1);
}

/* 安全的引用计数获取，防止溢出 */
static inline int kref_get_safe(struct kref *kref) {
    int old, new;
    do {
        old = kref->count;
        if (old <= 0 || old >= INT_MAX - 1) {
            return 0;  /* 无效或溢出 */
        }
        new = old + 1;
    } while (!__sync_bool_compare_and_swap(&kref->count, old, new));
    return 1;
}

static inline int kref_put(struct kref *kref) {
    return __sync_sub_and_fetch(&kref->count, 1) == 0;
}

/* 获取当前引用计数 */
static inline int kref_read(struct kref *kref) {
    return kref->count;
}

/* 链表 */
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)
#define INIT_LIST_HEAD(ptr) do { (ptr)->next = (ptr); (ptr)->prev = (ptr); } while (0)

static inline void list_init(struct list_head *head) {
    head->next = head;
    head->prev = head;
}

static inline void list_add(struct list_head *new, struct list_head *head) {
    head->next->prev = new;
    new->next = head->next;
    new->prev = head;
    head->next = new;
}

static inline void list_add_tail(struct list_head *new, struct list_head *head) {
    head->prev->next = new;
    new->prev = head->prev;
    new->next = head;
    head->prev = new;
}

static inline void list_del(struct list_head *entry) {
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
}

static inline int list_empty(struct list_head *head) {
    return head->next == head;
}

/* 判断链表是否为空（带锁保护，用于并发场景） */
static inline int list_empty_careful(struct list_head *head) {
    return head->next == head && head->prev == head;
}

/* 安全删除链表节点（先删除再初始化） */
static inline void list_del_init(struct list_head *entry) {
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    INIT_LIST_HEAD(entry);
}

/* 将节点从一个链表移动到另一个链表头部 */
static inline void list_move(struct list_head *list, struct list_head *head) {
    list_del(list);
    list_add(list, head);
}

/* 将节点从一个链表移动到另一个链表尾部 */
static inline void list_move_tail(struct list_head *list, struct list_head *head) {
    list_del(list);
    list_add_tail(list, head);
}

static inline void list_splice(struct list_head *list, struct list_head *head) {
    if (list_empty(list)) return;
    
    list->next->prev = head;
    list->prev->next = head->next;
    head->next->prev = list->prev;
    head->next = list->next;
}

/* 安全合并链表（初始化源链表） */
static inline void list_splice_init(struct list_head *list, struct list_head *head) {
    if (list_empty(list)) return;
    
    list->next->prev = head;
    list->prev->next = head->next;
    head->next->prev = list->prev;
    head->next = list->next;
    INIT_LIST_HEAD(list);
}

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)

#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)

#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); \
        pos = n, n = pos->next)

#define list_for_each_entry(pos, head, member) \
    for (pos = list_entry((head)->next, typeof(*pos), member); \
        &pos->member != (head); \
        pos = list_entry(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = list_entry((head)->next, typeof(*pos), member), \
        n = list_entry(pos->member.next, typeof(*pos), member); \
        &pos->member != (head); \
        pos = n, n = list_entry(n->member.next, typeof(*pos), member))

/* 获取链表长度 */
static inline size_t list_count(struct list_head *head) {
    size_t count = 0;
    struct list_head *pos;
    list_for_each(pos, head) {
        count++;
    }
    return count;
}

/* ID 分配器 */
struct idr {
    uint32_t next_id;
    void **ptrs;
    size_t size;
    spinlock_t lock;  /* 保护并发访问 */
};

static inline void idr_init(struct idr *idr, void **ptrs, size_t size) {
    idr->next_id = 0;
    idr->ptrs = ptrs;
    idr->size = size;
    idr->lock = SPINLOCK_INIT;
    /* 清空指针数组 */
    for (size_t i = 0; i < size; i++) {
        ptrs[i] = NULL;
    }
}

static inline void *idr_find(struct idr *idr, uint32_t id) {
    if (id < idr->size) {
        return idr->ptrs[id];
    }
    return NULL;
}

static inline int idr_alloc(struct idr *idr, void *ptr, uint32_t start, uint32_t end) {
    spin_lock(&idr->lock);
    for (uint32_t id = start; id < end && id < idr->size; id++) {
        if (idr->ptrs[id] == NULL) {
            idr->ptrs[id] = ptr;
            if (id >= idr->next_id) {
                idr->next_id = id + 1;
            }
            spin_unlock(&idr->lock);
            return id;
        }
    }
    spin_unlock(&idr->lock);
    return -1;
}

static inline void idr_remove(struct idr *idr, uint32_t id) {
    if (id < idr->size) {
        spin_lock(&idr->lock);
        idr->ptrs[id] = NULL;
        if (id < idr->next_id) {
            idr->next_id = id;  /* 优化下次搜索 */
        }
        spin_unlock(&idr->lock);
    }
}

/* 获取下一个可用的 ID */
static inline int idr_get_next(struct idr *idr, uint32_t *nextid) {
    spin_lock(&idr->lock);
    for (uint32_t id = idr->next_id; id < idr->size; id++) {
        if (idr->ptrs[id] == NULL) {
            *nextid = id;
            spin_unlock(&idr->lock);
            return 0;
        }
    }
    spin_unlock(&idr->lock);
    return -1;
}

/* 原子操作 */
typedef volatile int atomic_t;
typedef volatile long atomic64_t;

#define atomic_set(v, i)    (*(v) = (i))
#define atomic_read(v)      (*(v))
#define atomic_inc(v)       __sync_add_and_fetch(v, 1)
#define atomic_dec(v)       __sync_sub_and_fetch(v, 1)
#define atomic_add(v, i)    __sync_add_and_fetch(v, i)
#define atomic_sub(v, i)    __sync_sub_and_fetch(v, i)

/* 原子比较交换 */
#define atomic_cmpxchg(v, old, new) \
    __sync_val_compare_and_swap(v, old, new)

/* 原子交换 */
#define atomic_xchg(v, new) \
    __sync_lock_test_and_set(v, new)

/* 64 位原子操作 */
#define atomic64_set(v, i)    (*(v) = (i))
#define atomic64_read(v)      (*(v))
#define atomic64_inc(v)       __sync_add_and_fetch(v, 1)
#define atomic64_dec(v)       __sync_sub_and_fetch(v, 1)
#define atomic64_add(v, i)    __sync_add_and_fetch(v, i)
#define atomic64_sub(v, i)    __sync_sub_and_fetch(v, i)

/* 红黑树节点 */
struct rb_node {
    uint64_t key;               /* 键值 */
    int color;                  /* 颜色: 0=红, 1=黑 */
    struct rb_node *left;
    struct rb_node *right;
    struct rb_node *parent;
};

/* 红黑树根 */
struct rb_root {
    struct rb_node *rb_node;    /* 根节点 */
    struct rb_node *rb_leftmost; /* 最左节点 (最小 key) */
    uint32_t nr_nodes;          /* 节点数 */
};

#define RB_ROOT_INIT { NULL, NULL, 0 }

/* ========== 常用类型定义 ========== */

/* 文件偏移类型 - 已在文件开头通过 _OFF_T_DEFINED 保护定义 */

/* 进程/线程 ID 类型 - 已在文件开头通过 _PID_T_DEFINED 保护定义 */
typedef int32_t tid_t;

/* 时间类型 - 已在文件开头通过 _TIME_T_DEFINED 保护定义 */
typedef int64_t suseconds_t;

/* 模式类型 */
typedef uint32_t mode_t;

/* ========== 内存屏障 (唯一定义) ========== */

#define barrier()           asm volatile("" ::: "memory")
#define mb()                asm volatile("mfence" ::: "memory")
#define rmb()               asm volatile("lfence" ::: "memory")
#define wmb()               asm volatile("sfence" ::: "memory")
#define smp_mb()            mb()
#define smp_rmb()           rmb()
#define smp_wmb()           wmb()

/* 编译器屏障 */
#define READ_ONCE(x)        (*(volatile typeof(x) *)&(x))
#define WRITE_ONCE(x, v)    (*(volatile typeof(x) *)&(x) = (v))

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全算术运算 - 检测整数溢出
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 安全加法 - 返回1表示溢出 */
static inline int safe_add_u64(uint64_t a, uint64_t b, uint64_t *result) {
    if (a > UINT64_MAX - b) {
        return 1;  /* 溢出 */
    }
    *result = a + b;
    return 0;
}

static inline int safe_add_size(size_t a, size_t b, size_t *result) {
    if (a > SIZE_MAX - b) {
        return 1;
    }
    *result = a + b;
    return 0;
}

/* 安全乘法 - 返回1表示溢出 */
static inline int safe_mul_u64(uint64_t a, uint64_t b, uint64_t *result) {
    if (a == 0 || b == 0) {
        *result = 0;
        return 0;
    }
    if (a > UINT64_MAX / b) {
        return 1;  /* 溢出 */
    }
    *result = a * b;
    return 0;
}

static inline int safe_mul_size(size_t a, size_t b, size_t *result) {
    if (a == 0 || b == 0) {
        *result = 0;
        return 0;
    }
    if (a > SIZE_MAX / b) {
        return 1;
    }
    *result = a * b;
    return 0;
}

/* 安全数组大小计算 - 检查溢出 */
static inline int safe_array_size(size_t nmemb, size_t size, size_t *result) {
    if (nmemb == 0 || size == 0) {
        *result = 0;
        return 0;
    }
    return safe_mul_size(nmemb, size, result);
}

/* 安全减法 - 返回1表示下溢 */
static inline int safe_sub_u64(uint64_t a, uint64_t b, uint64_t *result) {
    if (a < b) {
        return 1;  /* 下溢 */
    }
    *result = a - b;
    return 0;
}

/* 检查乘法是否会溢出的宏 */
#define WOULD_OVERFLOW_MUL(a, b) \
    ((a) != 0 && (b) > (typeof(a))-1 / (a))

/* 检查加法是否会溢出的宏 */
#define WOULD_OVERFLOW_ADD(a, b) \
    ((a) > (typeof(a))-1 - (b))

/* ═══════════════════════════════════════════════════════════════════════════
 * 位操作宏
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BIT(n)              (1ULL << (n))
#define BIT_MASK(n)         (BIT(n) - 1)
#define BITS_PER_LONG       64

#define set_bit(nr, addr)   __sync_or_and_fetch(addr, BIT(nr))
#define clear_bit(nr, addr) __sync_and_and_fetch(addr, ~BIT(nr))
#define test_bit(nr, addr)  (!!((*(addr)) & BIT(nr)))
#define change_bit(nr, addr) __sync_xor_and_fetch(addr, BIT(nr))

/* 位域操作 */
#define GENMASK(h, l)       ((BIT((h) - (l) + 1) - 1) << (l))
#define FIELD_GET(mask, val) (((val) & (mask)) / ((mask) & ~((mask) << 1)))
#define FIELD_PREP(mask, val) (((val) * ((mask) & ~((mask) << 1))) & (mask))

/* ========== 对齐宏 ========== */

#define ALIGN(x, a)         (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a)    ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a)    (((x) & ((a) - 1)) == 0)

#define PAGE_SHIFT          12
#ifndef PAGE_SIZE
#define PAGE_SIZE           (1UL << PAGE_SHIFT)
#endif
#define PAGE_MASK           (~(PAGE_SIZE - 1))

/* ========== 编译器属性 ========== */

#define __packed            __attribute__((packed))
#define __aligned(x)        __attribute__((aligned(x)))
#define __section(x)        __attribute__((section(x)))
#define __used              __attribute__((used))
#define __unused            __attribute__((unused))
#define __init              __section(".init.text")
#define __exit              __section(".exit.text")
#define __cold              __attribute__((cold))
#define __hot               __attribute__((hot))
#define __noreturn          __attribute__((noreturn))
#define __pure              __attribute__((pure))
#define __const             __attribute__((const))
#define __malloc            __attribute__((malloc))
#define __printf(a, b)      __attribute__((format(printf, a, b)))
#define __flatten           __attribute__((flatten))
#define __noinline          __attribute__((noinline))
#define __always_inline     __attribute__((always_inline)) inline

/* ========== 安全检查宏 ========== */

#define CHECK_PTR(ptr)      ((ptr) != NULL)
#define CHECK_PTR_SAFE(ptr, ret) do { if (!(ptr)) return (ret); } while (0)
#define CHECK_PTR_VOID(ptr) do { if (!(ptr)) return; } while (0)

/* 安全最小/最大值 */
#ifndef MIN
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#endif
#ifndef CLAMP
#define CLAMP(val, lo, hi)  MIN(MAX(val, lo), hi)
#endif

/* 数组大小 */
#define ARRAY_SIZE(arr)     (sizeof(arr) / sizeof((arr)[0]))

/* ========== 断言和调试 ========== */

#ifdef DEBUG
#define ASSERT(cond) do { \
    if (!(cond)) { \
        println("ASSERT FAILED: " #cond); \
        asm volatile("int $3"); \
    } \
} while (0)
#else
#define ASSERT(cond) do { } while (0)
#endif

#define STATIC_ASSERT(cond) _Static_assert(cond, #cond)

/* ========== 条件调试系统 ========== */

/* 调试级别在 perf.h 中定义 (debug_level_t 枚举) */
/* 这里定义内核默认调试级别 */

#ifndef KERNEL_DEBUG_LEVEL
#define KERNEL_DEBUG_LEVEL   2    /* 默认: DEBUG_LEVEL_WARN */
#endif

/* 子系统调试开关 */
#define DEBUG_SUBSYS_SMP     (1 << 0)
#define DEBUG_SUBSYS_VMM     (1 << 1)
#define DEBUG_SUBSYS_TASK    (1 << 2)
#define DEBUG_SUBSYS_VFS     (1 << 3)
#define DEBUG_SUBSYS_NET     (1 << 4)
#define DEBUG_SUBSYS_SCHED   (1 << 5)
#define DEBUG_SUBSYS_ALL     0xFFFFFFFF

/* 条件打印宏 - 根据调试级别控制输出 */
#if KERNEL_DEBUG_LEVEL >= 5  /* DEBUG_LEVEL_TRACE */
#define DBG_TRACE(fmt, ...)     println(fmt)
#define DBG_TRACE_SMP(fmt, ...) println(fmt)
#else
#define DBG_TRACE(fmt, ...)     ((void)0)
#define DBG_TRACE_SMP(fmt, ...) ((void)0)
#endif

#if KERNEL_DEBUG_LEVEL >= 4  /* DEBUG_LEVEL_DEBUG */
#define DBG_DEBUG(fmt, ...)     println(fmt)
#define DBG_DEBUG_SMP(fmt, ...) println(fmt)
#define DBG_DEBUG_VMM(fmt, ...) println(fmt)
#define DBG_DEBUG_TASK(fmt, ...) println(fmt)
#else
#define DBG_DEBUG(fmt, ...)     ((void)0)
#define DBG_DEBUG_SMP(fmt, ...) ((void)0)
#define DBG_DEBUG_VMM(fmt, ...) ((void)0)
#define DBG_DEBUG_TASK(fmt, ...) ((void)0)
#endif

#if KERNEL_DEBUG_LEVEL >= 3  /* DEBUG_LEVEL_INFO */
#define DBG_INFO(fmt, ...)      println(fmt)
#else
#define DBG_INFO(fmt, ...)      ((void)0)
#endif

#if KERNEL_DEBUG_LEVEL >= 2  /* DEBUG_LEVEL_WARN */
#define DBG_WARN(fmt, ...)      println(fmt)
#else
#define DBG_WARN(fmt, ...)      ((void)0)
#endif

#define DBG_ERROR(fmt, ...)     println(fmt)  /* 错误总是输出 */

/* ========== 常用宏 (唯一定义) ========== */

#define DIV_ROUND_UP(n, d)  (((n) + (d) - 1) / (d))
#define ROUND_UP(x, y)      ((((x) + (y) - 1) / (y)) * (y))
#define ROUND_DOWN(x, y)    (((x) / (y)) * (y))

/* ========== 错误码 ========== */

#define ESUCCESS            0
#define EPERM               1
#define ENOENT              2
#define ESRCH               3
#define EINTR               4
#define EIO                 5
#define ENXIO               6
#define ENOMEM              12
#define EACCES              13
#define EFAULT              14
#define EINVAL              22
#define ENOSPC              28
#define ENOTEMPTY           39

/* ========== 标志位检查 ========== */

#define FLAGS_SET(flags, bit)   ((flags) & (bit))
#define FLAGS_CLEAR(flags, bit) (!((flags) & (bit)))
#define SET_FLAGS(flags, bit)   ((flags) |= (bit))
#define CLEAR_FLAGS(flags, bit) ((flags) &= ~(bit))

/* ========== 高性能内联函数 (原创优化) ========== */

/* 分支预测提示 */
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

/* 预取指令 - 提前加载数据到缓存 */
/* 注意: cpu.h 也有 prefetch 定义，这里使用条件编译避免冲突 */
#ifndef _CPU_PREFETCH_DEFINED
#define _CPU_PREFETCH_DEFINED
static inline void prefetch(const void *addr) {
    asm volatile("prefetcht0 %0" : : "m"(*(const char *)addr));
}
#endif

static inline void prefetch_for_read(const void *addr) {
    asm volatile("prefetcht0 %0" : : "m"(*(const char *)addr));
}

static inline void prefetch_for_write(const void *addr) {
    asm volatile("prefetcht1 %0" : : "m"(*(const char *)addr));
}

/* 预取一个缓存行 (64 字节) */
#define PREFETCH_STRIDE 64
static inline void prefetch_range(const void *addr, size_t len) {
    const char *p = (const char *)addr;
    for (size_t i = 0; i < len; i += PREFETCH_STRIDE) {
        prefetch((const void *)(p + i));
    }
}

/* 快速位操作 - 使用 CPU 指令 */
static inline int ffs64(uint64_t x) {
    if (x == 0) return 0;
    int r;
    asm volatile("bsf %1, %0" : "=r"(r) : "r"(x));
    return r + 1;
}

static inline int fls64(uint64_t x) {
    if (x == 0) return 0;
    int r;
    asm volatile("bsr %1, %0" : "=r"(r) : "r"(x));
    return r + 1;
}

/* 计算前导零 */
static inline int clz64(uint64_t x) {
    if (x == 0) return 64;
    return 64 - fls64(x);
}

/* 计算尾部零 */
static inline int ctz64(uint64_t x) {
    if (x == 0) return 64;
    return ffs64(x) - 1;
}

/* 快速向上取整到 2 的幂 */
static inline uint64_t round_up_pow2(uint64_t x) {
    if (x == 0) return 1;
    return 1ULL << fls64(x - 1);
}

/* 快速向下取整到 2 的幂 */
static inline uint64_t round_down_pow2(uint64_t x) {
    if (x == 0) return 0;
    return 1ULL << (fls64(x) - 1);
}

/* 快速对数 2 */
static inline int log2_uint64(uint64_t x) {
    return fls64(x) - 1;
}

/* 快速判断是否为 2 的幂 */
static inline int is_power_of_2(uint64_t x) {
    return x && !(x & (x - 1));
}

/* 快速内存复制 - 使用 rep movsq */
static inline void fast_memcpy(void *dst, const void *src, size_t n) {
    if (n == 0 || dst == NULL || src == NULL) return;
    uint64_t d = (uint64_t)dst;
    uint64_t s = (uint64_t)src;
    size_t qwords = n / 8;
    size_t bytes = n % 8;
    asm volatile (
        "rep movsq\n\t"
        "mov %4, %%rcx\n\t"
        "rep movsb"
        : "+D"(d), "+S"(s), "+c"(qwords)
        : "m"(*(const char *)src), "r"(bytes)
        : "memory"
    );
}

/* 快速内存清零 - 使用 rep stosq */
static inline void fast_memzero(void *dst, size_t n) {
    if (n == 0 || dst == NULL) return;
    uint64_t d = (uint64_t)dst;
    size_t qwords = n / 8;
    size_t bytes = n % 8;
    asm volatile (
        "xor %%rax, %%rax\n\t"
        "rep stosq\n\t"
        "mov %2, %%rcx\n\t"
        "rep stosb"
        : "+D"(d), "+c"(qwords)
        : "r"(bytes)
        : "rax", "memory"
    );
}

/* 循环展开宏 */
#define UNROLL_2(op)   op; op
#define UNROLL_4(op)   UNROLL_2(op); UNROLL_2(op)
#define UNROLL_8(op)   UNROLL_4(op); UNROLL_4(op)
#define UNROLL_16(op)  UNROLL_8(op); UNROLL_8(op)

/* 编译时字符串哈希 */
#define HASH_STR(s)    hash_str_impl(s, sizeof(s) - 1)
static inline uint32_t hash_str_impl(const char *s, size_t len) {
    uint32_t h = 0;
    for (size_t i = 0; i < len; i++) {
        h = h * 31 + (uint8_t)s[i];
    }
    return h;
}

/* 获取结构体成员偏移 */
#ifndef offsetof
#define offsetof(type, member) \
    ((size_t) &((type *)0)->member)
#endif

/* 编译时类型检查 */
#define typecheck(type, x) \
    ({ type __dummy; \
       typeof(x) __dummy2; \
       (void)(&__dummy == &__dummy2); \
       1; })

/* 编译时计算数组元素数 */
#define BUILD_BUG_ON(cond) ((void)sizeof(char[1 - 2 * !!(cond)]))

/* ═══════════════════════════════════════════════════════════════════════════
 * 错误码系统 - 统一的错误处理
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Zratro 错误码范围: -1 到 -1023 */
#define Z_OK                    0       /* 成功 */
#define Z_ERR_UNKNOWN           (-1)    /* 未知错误 */
#define Z_ERR_INVALID           (-2)    /* 无效参数 */
#define Z_ERR_NULL              (-3)    /* 空指针 */
#define Z_ERR_NOMEM             (-4)    /* 内存不足 */
#define Z_ERR_NOENT             (-5)    /* 条目不存在 */
#define Z_ERR_EXIST             (-6)    /* 已存在 */
#define Z_ERR_ACCESS            (-7)    /* 访问被拒绝 */
#define Z_ERR_BUSY              (-8)    /* 资源繁忙 */
#define Z_ERR_IO                (-9)    /* I/O 错误 */
#define Z_ERR_TIMEOUT           (-10)   /* 超时 */
#define Z_ERR_OVERFLOW          (-11)   /* 溢出 */
#define Z_ERR_RANGE             (-12)   /* 超出范围 */
#define Z_ERR_NOTSUP            (-13)   /* 不支持 */
#define Z_ERR_AGAIN             (-14)   /* 请重试 */
#define Z_ERR_FAULT             (-15)   /* 地址错误 */
#define Z_ERR_CORRUPT           (-16)   /* 数据损坏 */
#define Z_ERR_SHUTDOWN          (-17)   /* 正在关闭 */

/* 检查是否为错误码 */
#define IS_ERROR(err)           ((err) < 0)
#define IS_OK(err)              ((err) >= 0)

/* 错误传播宏 - 简化错误处理代码 */
#define Z_RETURN_IF_ERR(expr) do { \
    int __ret = (expr); \
    if (__ret < 0) return __ret; \
} while (0)

#define Z_RETURN_NULL_IF_ERR(expr) do { \
    int __ret = (expr); \
    if (__ret < 0) return NULL; \
} while (0)

#define Z_GOTO_IF_ERR(expr, label) do { \
    int __ret = (expr); \
    if (__ret < 0) goto label; \
} while (0)

/* 条件检查宏 - 失败时返回错误码 */
#define Z_CHECK_NULL(ptr) \
    do { if ((ptr) == NULL) return Z_ERR_NULL; } while (0)

#define Z_CHECK_NULL_RET(ptr, ret) \
    do { if ((ptr) == NULL) return (ret); } while (0)

#define Z_CHECK_NULL_VOID(ptr) \
    do { if ((ptr) == NULL) return; } while (0)

#define Z_CHECK_COND(cond, err) \
    do { if (!(cond)) return (err); } while (0)

#define Z_CHECK_ALLOC(ptr) \
    do { if ((ptr) == NULL) return Z_ERR_NOMEM; } while (0)

/* 资源清理宏 - 确保资源释放 */
#define Z_CLEANUP_PTR(pp, free_func) \
    do { \
        if ((pp) != NULL && *(pp) != NULL) { \
            free_func(*(pp)); \
            *(pp) = NULL; \
        } \
    } while (0)

#define Z_CLEANUP_FD(fd, close_func) \
    do { \
        if ((fd) >= 0) { \
            close_func(fd); \
            (fd) = -1; \
        } \
    } while (0)

/* 错误码转字符串 */
static inline const char *z_strerror(int err) {
    switch (err) {
        case Z_OK:              return "Success";
        case Z_ERR_UNKNOWN:     return "Unknown error";
        case Z_ERR_INVALID:     return "Invalid argument";
        case Z_ERR_NULL:        return "Null pointer";
        case Z_ERR_NOMEM:       return "Out of memory";
        case Z_ERR_NOENT:       return "No such entry";
        case Z_ERR_EXIST:       return "Already exists";
        case Z_ERR_ACCESS:      return "Access denied";
        case Z_ERR_BUSY:        return "Resource busy";
        case Z_ERR_IO:          return "I/O error";
        case Z_ERR_TIMEOUT:     return "Timeout";
        case Z_ERR_OVERFLOW:    return "Overflow";
        case Z_ERR_RANGE:       return "Out of range";
        case Z_ERR_NOTSUP:      return "Not supported";
        case Z_ERR_AGAIN:       return "Try again";
        case Z_ERR_FAULT:       return "Bad address";
        case Z_ERR_CORRUPT:     return "Data corrupted";
        case Z_ERR_SHUTDOWN:    return "Shutting down";
        default:                return "Unknown error code";
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 断言增强
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 运行时断言 */
#ifdef DEBUG
#define Z_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        println("[ZASSERT] " msg); \
        println("  Condition: " #cond); \
        println("  File: " __FILE__); \
        println("  Line: " Z_STRINGIFY(__LINE__)); \
        asm volatile("int $3"); \
    } \
} while (0)
#else
#define Z_ASSERT(cond, msg) ((void)0)
#endif

/* 编译时断言 */
#define Z_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

/* 字符串化 */
#define Z_STRINGIFY(x) #x
#define Z_TOSTRING(x) Z_STRINGIFY(x)

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全算术运算 (防止整数溢出)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 安全乘法检查 */
static inline int safe_mul_check(size_t a, size_t b, size_t *result) {
    if (a == 0 || b == 0) {
        *result = 0;
        return 1;
    }
    if (a > SIZE_MAX / b) {
        return 0;  /* 溢出 */
    }
    *result = a * b;
    return 1;
}

/* 安全加法检查 */
static inline int safe_add_check(size_t a, size_t b, size_t *result) {
    if (a > SIZE_MAX - b) {
        return 0;  /* 溢出 */
    }
    *result = a + b;
    return 1;
}

/* 安全乘法宏 - 返回 0 表示溢出 */
#define SAFE_MUL(a, b, result)  safe_mul_check((a), (b), (result))
#define SAFE_ADD(a, b, result)  safe_add_check((a), (b), (result))

/* 带溢出检查的分配大小计算 */
static inline size_t safe_alloc_size(size_t nmemb, size_t size) {
    size_t total;
    if (!safe_mul_check(nmemb, size, &total)) {
        return 0;  /* 溢出 */
    }
    return total;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全字符串操作
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 安全字符串复制 - 保证终止符 */
static inline char *strncpy_safe(char *dest, const char *src, size_t dest_size) {
    if (dest == NULL || src == NULL || dest_size == 0) {
        return dest;
    }
    
    size_t i;
    for (i = 0; i < dest_size - 1 && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
    
    return dest;
}

/* 安全字符串连接 */
static inline char *strncat_safe(char *dest, const char *src, size_t dest_size) {
    if (dest == NULL || src == NULL || dest_size == 0) {
        return dest;
    }
    
    size_t dest_len = 0;
    while (dest_len < dest_size && dest[dest_len] != '\0') {
        dest_len++;
    }
    
    if (dest_len >= dest_size - 1) {
        return dest;  /* 目标已满 */
    }
    
    size_t i;
    for (i = 0; dest_len + i < dest_size - 1 && src[i] != '\0'; i++) {
        dest[dest_len + i] = src[i];
    }
    dest[dest_len + i] = '\0';
    
    return dest;
}

/* 安全内存复制 (带重叠检查) */
static inline void *memcpy_safe(void *dest, const void *src, size_t n) {
    if (dest == NULL || src == NULL || n == 0) {
        return dest;
    }
    
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    
    /* 检查重叠 */
    if ((s < d && s + n > d) || (d < s && d + n > s)) {
        /* 有重叠，使用反向复制 */
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    }
    
    return dest;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 额外容器操作宏 (带类型检查版本)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 类型安全的交换 - 需要 type 参数 */
#define SWAP_TYPED(a, b, type) do { \
    type __tmp = (a); \
    (a) = (b); \
    (b) = __tmp; \
} while (0)

/* 掩码操作 */
#define MASK_LOW(n)         ((1ULL << (n)) - 1)
#define MASK_HIGH(n)        (~MASK_LOW(n))
#define EXTRACT_BITS(x, hi, lo) (((x) >> (lo)) & MASK_LOW((hi) - (lo) + 1))

#endif /* KTYPES_H */
