/*
 * Zratro OS - Z-Alloc (原创高性能内存分配器)
 * 
 * 创新设计：
 * 1. 分层伙伴系统 - 减少碎片
 * 2. 线程本地缓存 - 无锁快路径
 * 3. 伙伴位图压缩 - 空间高效
 * 4. 智能预取 - 减少缓存未命中
 * 5. 防碎片整理 - 后台合并
 */

#ifndef ZALLOC_H
#define ZALLOC_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== Z-Alloc 配置 ========== */

#define ZA_MIN_BLOCK_SIZE      16          /* 最小块大小 */
#define ZA_MAX_ORDER           11          /* 最大阶 (16KB) */
#define ZA_NUM_ORDERS          (ZA_MAX_ORDER + 1)
#define ZA_CACHE_SIZE          256         /* 每个缓存的槽位数 */
#define ZA_NUM_CACHES          8           /* 缓存数量 (每大小类) */
#define ZA_MAX_SMALL           256         /* 小对象阈值 */
#define ZA_PAGE_SIZE           4096        /* 页大小 */
#define ZA_ALIGN               16          /* 默认对齐 */

/* ========== 大小类 ========== */

/* 小对象大小类 (字节) */
static const uint16_t za_size_classes[] = {
    16, 24, 32, 48, 64, 96, 128, 192, 256
};
#define ZA_NUM_SIZE_CLASSES    (sizeof(za_size_classes) / sizeof(za_size_classes[0]))

/* ========== 伙伴块结构 ========== */

struct za_block {
    uint64_t addr;              /* 块地址 */
    uint32_t order;             /* 阶数 */
    uint32_t flags;             /* 标志 */
    struct list_head list;      /* 链表节点 */
};

/* 块标志 */
#define ZA_BLOCK_FREE      0
#define ZA_BLOCK_USED      1
#define ZA_BLOCK_SPLIT     2
#define ZA_BLOCK_MERGING   3

/* ========== 伙伴区域 ========== */

struct za_zone {
    uint64_t start;             /* 起始地址 */
    uint64_t end;               /* 结束地址 */
    uint64_t size;              /* 总大小 */
    uint64_t free_size;         /* 空闲大小 */
    
    /* 空闲链表 */
    struct list_head free_lists[ZA_NUM_ORDERS];
    uint32_t free_counts[ZA_NUM_ORDERS];
    
    /* 位图 (用于快速查找伙伴) */
    uint64_t *bitmap;
    uint32_t bitmap_size;
    
    spinlock_t lock;
};

/* 位图操作 */
static inline void za_set_bit(struct za_zone *zone, uint64_t index) {
    zone->bitmap[index / 64] |= (1ULL << (index % 64));
}

static inline void za_clear_bit(struct za_zone *zone, uint64_t index) {
    zone->bitmap[index / 64] &= ~(1ULL << (index % 64));
}

static inline int za_test_bit(struct za_zone *zone, uint64_t index) {
    return (zone->bitmap[index / 64] >> (index % 64)) & 1;
}

/* ========== 线程本地缓存 ========== */

struct za_cache_slot {
    void *ptr;                  /* 内存指针 */
    uint32_t size;              /* 大小 */
    uint32_t flags;             /* 标志 */
};

struct za_thread_cache {
    struct za_cache_slot slots[ZA_NUM_SIZE_CLASSES][ZA_CACHE_SIZE];
    uint32_t counts[ZA_NUM_SIZE_CLASSES];
    uint32_t limits[ZA_NUM_SIZE_CLASSES];
    
    /* 统计 */
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    
    int cpu_id;
    struct list_head list;
};

/* ========== Slab 缓存 ========== */

struct za_slab {
    void *base;                 /* 基地址 */
    uint32_t obj_size;          /* 对象大小 */
    uint32_t obj_count;         /* 对象总数 */
    uint32_t free_count;        /* 空闲对象数 */
    
    uint8_t *free_map;          /* 空闲位图 */
    uint32_t map_size;
    
    struct list_head list;      /* 链表节点 */
    struct za_slab_cache *cache;
};

struct za_slab_cache {
    uint32_t obj_size;          /* 对象大小 */
    uint32_t flags;             /* 标志 */
    
    struct list_head full;      /* 满 slab */
    struct list_head partial;   /* 部分 slab */
    struct list_head empty;     /* 空 slab */
    
    uint32_t slabs_total;
    uint32_t slabs_active;
    
    spinlock_t lock;
};

/* Slab 标志 */
#define ZA_SLAB_FLAG_DMA        (1 << 0)   /* DMA 可用 */
#define ZA_SLAB_FLAG_RECLAIM    (1 << 1)   /* 可回收 */

/* ========== 全局缓存池 ========== */

struct za_cache_pool {
    struct za_thread_cache *caches;
    uint32_t count;
    spinlock_t lock;
};

/* ========== 防碎片整理器 ========== */

struct za_defrag {
    uint64_t scan_interval;     /* 扫描间隔 */
    uint64_t last_scan;         /* 上次扫描时间 */
    uint32_t fragmentation;     /* 碎片度 (%) */
    uint32_t threshold;         /* 整理阈值 */
    
    /* 统计 */
    uint64_t pages_moved;
    uint64_t blocks_merged;
};

/* ========== Z-Alloc 主结构 ========== */

struct zalloc {
    /* 伙伴区域 */
    struct za_zone *zones;
    uint32_t zone_count;
    
    /* 线程本地缓存池 */
    struct za_cache_pool cache_pool;
    
    /* Slab 缓存 */
    struct za_slab_cache slab_caches[ZA_NUM_SIZE_CLASSES];
    
    /* 防碎片整理 */
    struct za_defrag defrag;
    
    /* 统计 */
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t total_bytes_alloc;
    uint64_t total_bytes_free;
    uint64_t current_usage;
    uint64_t peak_usage;
    
    /* 锁 */
    spinlock_t lock;
};

/* ========== 公共 API ========== */

/* 初始化/销毁 */
int zalloc_init(struct zalloc *za, uint64_t mem_start, uint64_t mem_size);
void zalloc_destroy(struct zalloc *za);

/* 基本分配 */
void *zalloc_alloc(struct zalloc *za, size_t size);
void *zalloc_alloc_aligned(struct zalloc *za, size_t size, size_t align);
void zalloc_free(struct zalloc *za, void *ptr);

/* 带 flags 的分配 */
#define ZA_FLAG_ZERO    (1 << 0)   /* 清零 */
#define ZA_FLAG_DMA     (1 << 1)   /* DMA 可用 */
#define ZA_FLAG_ATOMIC  (1 << 2)   /* 原子分配 */

void *zalloc_alloc_flags(struct zalloc *za, size_t size, uint32_t flags);

/* 批量分配 */
int zalloc_alloc_batch(struct zalloc *za, size_t size, void **ptrs, uint32_t count);
void zalloc_free_batch(struct zalloc *za, void **ptrs, uint32_t count);

/* 重分配 */
void *zalloc_realloc(struct zalloc *za, void *ptr, size_t new_size);

/* 统计 */
uint64_t zalloc_usage(struct zalloc *za);
uint64_t zalloc_free_space(struct zalloc *za);
void zalloc_stats(struct zalloc *za, uint64_t *allocs, uint64_t *frees, 
                  uint64_t *usage, uint64_t *peak);

/* ========== Slab API ========== */

struct za_slab_cache *za_slab_create(struct zalloc *za, size_t obj_size, uint32_t flags);
void za_slab_destroy(struct zalloc *za, struct za_slab_cache *cache);
void *za_slab_alloc(struct za_slab_cache *cache);
void za_slab_free(struct za_slab_cache *cache, void *obj);

/* ========== 高级特性 ========== */

/* 防碎片整理 */
void zalloc_defrag_run(struct zalloc *za);
uint32_t zalloc_fragmentation(struct zalloc *za);

/* 线程本地缓存 */
struct za_thread_cache *za_get_thread_cache(struct zalloc *za);
void za_flush_thread_cache(struct za_thread_cache *cache);

/* 内存压力回调 */
typedef void (*za_pressure_cb)(int pressure);
void zalloc_set_pressure_callback(struct zalloc *za, za_pressure_cb cb);

/* 调优参数 */
void zalloc_set_cache_limit(struct zalloc *za, uint32_t size_class, uint32_t limit);
void zalloc_set_defrag_threshold(struct zalloc *za, uint32_t threshold);

/* ========== 内联辅助 ========== */

/* 快速大小类查找 */
static inline uint32_t za_get_size_class(size_t size) {
    if (size <= 16) return 0;
    if (size <= 24) return 1;
    if (size <= 32) return 2;
    if (size <= 48) return 3;
    if (size <= 64) return 4;
    if (size <= 96) return 5;
    if (size <= 128) return 6;
    if (size <= 192) return 7;
    if (size <= 256) return 8;
    return 9;  /* 大对象，使用伙伴系统 */
}

/* 计算伙伴地址 */
static inline uint64_t za_buddy_addr(uint64_t addr, uint32_t order) {
    return addr ^ (ZA_PAGE_SIZE << order);
}

/* 对齐向上 */
static inline size_t za_align_up(size_t size, size_t align) {
    return (size + align - 1) & ~(align - 1);
}

/* 检查对齐 */
static inline int za_is_aligned(void *ptr, size_t align) {
    return ((uint64_t)ptr & (align - 1)) == 0;
}

/* 阶数计算 */
static inline uint32_t za_order_for_size(size_t size) {
    size_t pages = (size + ZA_PAGE_SIZE - 1) / ZA_PAGE_SIZE;
    uint32_t order = 0;
    while ((1U << order) < pages && order < ZA_MAX_ORDER) {
        order++;
    }
    return order;
}

#endif /* ZALLOC_H */
