/*
 * ZTS Adaptation Layer - Linux Kernel API to ZTS API Mapping
 *
 * This header provides compatibility macros and functions that map
 * Linux kernel APIs to the ZTS API table, allowing Linux source code
 * to be compiled as loadable .zts modules for ZratroOS.
 *
 * Usage:
 *   #include "zts_adapt.h"
 *
 *   // Then use Linux kernel APIs normally:
 *   void *p = kmalloc(1024);
 *   printk("Hello from ZTS module: %p\n", p);
 *   kfree(p);
 */

#ifndef ZTS_ADAPT_H
#define ZTS_ADAPT_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* ============================================================
 * ZTS API Table Structure (must match zts_algorithm.h)
 * ============================================================ */
struct zts_api_table {
    uint32_t api_version;

    /* 内存管理 */
    void *(*alloc)(size_t size);
    void (*free)(void *ptr);
    void *(*realloc)(void *ptr, size_t size);

    /* 日志 */
    void (*log_print)(const char *level, const char *fmt, ...);

    /* 时间 */
    uint64_t (*time_now)(void);

    /* VFS 操作 (可选) */
    int (*vfs_open)(const char *path, uint32_t mode);
    int (*vfs_close)(int fd);
    int (*vfs_read)(int fd, void *buf, size_t count);
    int (*vfs_write)(int fd, const void *buf, size_t count);

    /* 网络 (可选) */
    int (*net_send)(const void *data, size_t len);
    int (*net_recv)(void *data, size_t len);

    /* 字符串操作 */
    int (*strcmp)(const char *s1, const char *s2);
    void *(*__memcpy)(void *dst, const void *src, size_t n);
    void *(*__memset)(void *s, int c, size_t n);
};

/* Helper macros that avoid macro recursion */
#define __zts_memcpy(dst, src, n)   zts_api->__memcpy(dst, src, n)
#define __zts_memset(s, c, n)       zts_api->__memset(s, c, n)

/* ============================================================
 * API pointer (defined by ZTS_MODULE_INIT macro)
 * Each module will define this variable via the macro
 * ============================================================ */

/* Module must define this variable */
#ifndef ZTS_API_VAR_DEFINED
#define ZTS_API_VAR_DEFINED
struct zts_api_table *zts_api = NULL;
#endif

/* ============================================================
 * Core Memory Management
 * ============================================================ */
#define kmalloc(size)           zts_api->alloc(size)
#define kzalloc(size)           ({ void *p = zts_api->alloc(size); \
                                   __zts_memset(p, 0, size); p; })
#define kfree(ptr)              zts_api->free(ptr)
#define krealloc(ptr, size)     zts_api->realloc(ptr, size)
#define vmalloc(size)           zts_api->alloc(size)
#define vfree(ptr)              zts_api->free(ptr)
#define __get_free_page(gfp)    zts_api->alloc(4096)
#define free_page(addr)         zts_api->free((void*)addr)

/* ============================================================
 * String Operations
 * ============================================================ */
#define strcmp(s1, s2)          zts_api->strcmp(s1, s2)
#define memcpy(dst, src, n)     __zts_memcpy(dst, src, n)
#define memset(s, c, n)         __zts_memset(s, c, n)
#define memmove(dst, src, n)    zts_api->memmove(dst, src, n)
#define memcmp(s1, s2, n)       zts_api->memcmp(s1, s2, n)

/* String functions - need inline implementations */
static inline size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return p - s;
}

static inline char *strcpy(char *dst, const char *src) {
    char *p = dst;
    while ((*p++ = *src++));
    return dst;
}

static inline char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
        if (!src[i]) break;
    }
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

static inline int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!s1[i] || !s2[i] || s1[i] != s2[i])
            return s1[i] - s2[i];
    }
    return 0;
}

/* ============================================================
 * Logging
 * ============================================================ */
#define printk(fmt, ...)        zts_api->log_print("KERN", fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)       zts_api->log_print("INFO", fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)        zts_api->log_print("ERR", fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)       zts_api->log_print("WARN", fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)      zts_api->log_print("DBG", fmt, ##__VA_ARGS__)

/* Kernel log levels */
#define KERN_EMERG    "KERN_EMERG"
#define KERN_ALERT    "KERN_ALERT"
#define KERN_CRIT     "KERN_CRIT"
#define KERN_ERR      "KERN_ERR"
#define KERN_WARNING  "KERN_WARNING"
#define KERN_NOTICE   "KERN_NOTICE"
#define KERN_INFO     "KERN_INFO"
#define KERN_DEBUG    "KERN_DEBUG"

/* ============================================================
 * Time Functions
 * ============================================================ */
#define get_jiffies_64()        zts_api->time_now()
#define jiffies                 (*(uint64_t*)zts_api->time_now())
#define HZ                      1000
#define msecs_to_jiffies(ms)    (ms)
#define jiffies_to_msecs(j)     (j)

/* ============================================================
 * Basic Types
 * ============================================================ */
typedef int8_t      s8;
typedef int16_t     s16;
typedef int32_t     s32;
typedef int64_t     s64;
typedef uint8_t     u8;
typedef uint16_t    u16;
typedef uint32_t    u32;
typedef uint64_t    u64;

/* Boolean */
#define bool    _Bool
#define true    1
#define false   0

/* Error codes */
#define EPERM           1
#define ENOENT          2
#define ESRCH           3
#define EINTR           4
#define EIO             5
#define ENXIO           6
#define E2BIG           7
#define ENOEXEC         8
#define EBADF           9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EBUSY           16
#define EEXIST          17
#define ENODEV          19
#define EINVAL          22
#define ENOSPC          28
#define EPIPE           32
#define EOPNOTSUPP      95
#define ETIMEDOUT       110
#define ECONNREFUSED    111

#define IS_ERR(ptr)         ((unsigned long)(ptr) > (unsigned long)-1000L)
#define PTR_ERR(ptr)        ((long)(ptr))
#define ERR_PTR(err)        ((void*)(long)(err))

/* ============================================================
 * Container Of & List Operations
 * ============================================================ */
#ifndef offsetof
#define offsetof(TYPE, MEMBER)  ((size_t) &((TYPE *)0)->MEMBER)
#endif
#define container_of(ptr, type, member) ({                      \
    const typeof(((type *)0)->member) * __mptr = (ptr);        \
    (type *)((char *)__mptr - offsetof(type, member)); })

/* Linux-style linked list */
struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *new_node,
                              struct list_head *prev,
                              struct list_head *next) {
    next->prev = new_node;
    new_node->next = next;
    new_node->prev = prev;
    prev->next = new_node;
}

static inline void list_add(struct list_head *new_node, struct list_head *head) {
    __list_add(new_node, head, head->next);
}

static inline void list_add_tail(struct list_head *new_node, struct list_head *head) {
    __list_add(new_node, head->prev, head);
}

static inline void list_del(struct list_head *entry) {
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
    entry->next = NULL;
    entry->prev = NULL;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_next_entry(pos, member) list_entry((pos)->member.next, typeof(*(pos)), member)
#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)
#define list_for_each_entry(pos, head, member) \
    for (pos = list_first_entry(head, typeof(*pos), member); \
         &pos->member != (head); \
         pos = list_next_entry(pos, member))

/* ============================================================
 * Spin Locks (Simplified - ZratroOS doesn't have full SMP yet)
 * ============================================================ */
typedef volatile int spinlock_t;
#define DEFINE_SPINLOCK(x)  spinlock_t x = 0
#define spin_lock_init(lock)    (*(lock) = 0)

static inline void spin_lock(spinlock_t *lock) {
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {
        while (*lock) { /* busy wait */ }
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

static inline void spin_lock_bh(spinlock_t *lock) { spin_lock(lock); }
static inline void spin_unlock_bh(spinlock_t *lock) { spin_unlock(lock); }
static inline void spin_lock_irqsave(spinlock_t *lock, unsigned long flags) { 
    (void)flags; spin_lock(lock); 
}
static inline void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags) { 
    (void)flags; spin_unlock(lock); 
}

/* ============================================================
 * Mutexes (Simplified)
 * ============================================================ */
struct mutex {
    volatile int locked;
};

#define DEFINE_MUTEX(name)  struct mutex name = { .locked = 0 }
#define mutex_init(mtx)     ((mtx)->locked = 0)

static inline void mutex_lock(struct mutex *mtx) {
    while (__atomic_exchange_n(&mtx->locked, 1, __ATOMIC_ACQUIRE)) {
        while (mtx->locked) { /* busy wait */ }
    }
}

static inline void mutex_unlock(struct mutex *mtx) {
    __atomic_store_n(&mtx->locked, 0, __ATOMIC_RELEASE);
}

/* ============================================================
 * Atomic Operations
 * ============================================================ */
typedef struct {
    volatile int counter;
} atomic_t;

#define ATOMIC_INIT(i)  { (i) }
#define atomic_set(v, i)    ((v)->counter = (i))
#define atomic_read(v)      ((v)->counter)

static inline void atomic_add(int i, atomic_t *v) {
    __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline void atomic_sub(int i, atomic_t *v) {
    __atomic_fetch_sub(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int atomic_inc_and_test(atomic_t *v) {
    return __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST) == 0;
}

static inline int atomic_dec_and_test(atomic_t *v) {
    return __atomic_sub_fetch(&v->counter, 1, __ATOMIC_SEQ_CST) == 0;
}

static inline void atomic_inc(atomic_t *v) { atomic_add(1, v); }
static inline void atomic_dec(atomic_t *v) { atomic_sub(1, v); }

/* ============================================================
 * VFS Operations (if available in ZTS)
 * ============================================================ */
#define vfs_open(path, mode)    (zts_api->vfs_open ? zts_api->vfs_open(path, mode) : -1)
#define vfs_close(fd)           (zts_api->vfs_close ? zts_api->vfs_close(fd) : -1)
#define vfs_read(fd, buf, len)  (zts_api->vfs_read ? zts_api->vfs_read(fd, buf, len) : -1)
#define vfs_write(fd, buf, len) (zts_api->vfs_write ? zts_api->vfs_write(fd, buf, len) : -1)

/* File open modes */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_APPEND    02000

/* ============================================================
 * Network Operations (if available in ZTS)
 * ============================================================ */
#define net_send(data, len)     (zts_api->net_send ? zts_api->net_send(data, len) : -1)
#define net_recv(data, len)     (zts_api->net_recv ? zts_api->net_recv(data, len) : -1)

/* ============================================================
 * Work Queues (Simplified - execute immediately)
 * ============================================================ */
struct work_struct {
    void (*func)(void *);
    void *data;
};

#define INIT_WORK(work, func)   do { \
    (work)->func = func; \
    (work)->data = NULL; \
} while (0)

static inline void schedule_work(struct work_struct *work) {
    if (work->func) work->func(work->data);
}

/* ============================================================
 * Timers (Simplified)
 * ============================================================ */
struct timer_list {
    void (*function)(void *);
    void *data;
    uint64_t expires;
};

#define timer_setup(timer, cb, flags)   do { \
    (timer)->function = cb; \
    (timer)->data = NULL; \
} while (0)

static inline void mod_timer(struct timer_list *timer, uint64_t expires) {
    timer->expires = expires;
}

static inline void del_timer(struct timer_list *timer) {
    timer->expires = 0;
}

/* ============================================================
 * Memory Barriers
 * ============================================================ */
#define barrier()           __asm__ __volatile__("" ::: "memory")
#define smp_mb()            __sync_synchronize()
#define smp_rmb()           __sync_synchronize()
#define smp_wmb()           __sync_synchronize()
#define mb()                __sync_synchronize()
#define rmb()               __sync_synchronize()
#define wmb()               __sync_synchronize()

/* ============================================================
 * CPU Operations
 * ============================================================ */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#define cpu_relax()         barrier()
#define smp_processor_id()  0
#define num_online_cpus()   1
#define get_cpu()           0
#define put_cpu()           do {} while(0)

/* ============================================================
 * Module Declaration Macros (for ZTS modules)
 * ============================================================ */

/* Initialize module with ZTS API table */
#define ZTS_MODULE_INIT(fn) \
    int zts_module_init(struct zts_api_table *api) { \
        zts_api = api; \
        return fn(); \
    }

/* Module execute entry point */
#define ZTS_MODULE_EXEC(fn) \
    int zts_module_exec(void *input, void *output, void *ctx) { \
        return fn(input, output, ctx); \
    }

/* Module cleanup entry point */
#define ZTS_MODULE_CLEANUP(fn) \
    void zts_module_cleanup(void) { \
        fn(); \
    }

/* ============================================================
 * Inline Helper Functions
 * ============================================================ */

/* min/max macros */
#define min(x, y) ({ \
    typeof(x) _min1 = (x); \
    typeof(y) _min2 = (y); \
    (void)(&_min1 == &_min2); \
    _min1 < _min2 ? _min1 : _min2; })

#define max(x, y) ({ \
    typeof(x) _max1 = (x); \
    typeof(y) _max2 = (y); \
    (void)(&_max1 == &_max2); \
    _max1 > _max2 ? _max1 : _max2; })

#define min_t(type, x, y) ({ \
    type __min1 = (x); \
    type __min2 = (y); \
    __min1 < __min2 ? __min1 : __min2; })

#define max_t(type, x, y) ({ \
    type __max1 = (x); \
    type __max2 = (y); \
    __max1 > __max2 ? __max1 : __max2; })

/* Clamp */
#define clamp(val, lo, hi) min_t(typeof(val), max_t(typeof(val), val, lo), hi)

/* Round up/down */
#define ALIGN(x, a)     (((x) + ((a) - 1)) & ~((a) - 1))
#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define PAGE_ALIGN(addr) ALIGN(addr, PAGE_SIZE)

/* Array size */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Likely/unlikely */
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

/* Division with rounding */
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

/* Bit operations */
#define BIT(n)          (1UL << (n))
#define BITS_PER_BYTE   8
#define BITS_TO_LONGS(nr) DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))

/* Return helpers */
#define return_if(cond) do { if (cond) return; } while(0)
#define return_val_if(cond, val) do { if (cond) return val; } while(0)

/* Suppress unused variable warnings */
#define __maybe_unused  __attribute__((unused))
#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

/* Section attributes */
#define __init          __attribute__((section(".init.text")))
#define __exit          __attribute__((section(".exit.text")))
#define __initdata      __attribute__((section(".init.data")))
#define __initconst     __attribute__((section(".init.rodata")))

/* Alignment */
#define __aligned(x)    __attribute__((aligned(x)))
#define __packed        __attribute__((packed))

/* Endianness conversion */
#define cpu_to_le16(x)  (x)
#define cpu_to_le32(x)  (x)
#define cpu_to_le64(x)  (x)
#define le16_to_cpu(x)  (x)
#define le32_to_cpu(x)  (x)
#define le64_to_cpu(x)  (x)
#define cpu_to_be16(x)  __builtin_bswap16(x)
#define cpu_to_be32(x)  __builtin_bswap32(x)
#define cpu_to_be64(x)  __builtin_bswap64(x)
#define be16_to_cpu(x)  __builtin_bswap16(x)
#define be32_to_cpu(x)  __builtin_bswap32(x)
#define be64_to_cpu(x)  __builtin_bswap64(x)

/* Checksum helpers */
static inline uint16_t csum_partial(const void *buf, int len, uint16_t sum) {
    const uint16_t *data = (const uint16_t *)buf;
    uint32_t s = sum;
    
    while (len > 1) {
        s += *data++;
        len -= 2;
    }
    if (len > 0) s += *(uint8_t *)data;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return ~s;
}

static inline uint16_t csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum;
}

#endif /* ZTS_ADAPT_H */
