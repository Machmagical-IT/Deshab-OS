/*
 * Zratro OS - Slab Allocator
 * 
 * 高效的内核对象缓存分配器
 */

#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>
#include <stddef.h>
#include "smp.h"

/* Slab 配置 */
#define SLAB_MIN_SIZE           8       /* 最小对象大小 */
#define SLAB_MAX_SIZE           8192    /* 最大对象大小 */
#define SLAB_ORDER_MAX          3       /* 最大阶数 */
#define SLAB_MIN_OBJECTS        4       /* 每个 slab 最少对象数 */

/* Slab 颜色 (用于缓存行对齐) */
#define SLAB_COLOR_MAX          16

/* Slab 标志 */
#define SLAB_FLAG_NONE          0
#define SLAB_FLAG_DMA           (1 << 0)    /* DMA 可用内存 */
#define SLAB_FLAG_RECLAIM       (1 << 1)    /* 可回收 */
#define SLAB_FLAG_PERCPU        (1 << 2)    /* Per-CPU 缓存 */
#define SLAB_FLAG_ATOMIC        (1 << 3)    /* 原子分配 */

/* 对象标志 */
#define SLAB_OBJ_FREE           0
#define SLAB_OBJ_USED           1

/* Slab 描述符 */
struct slab {
    struct list_head list;       /* 链入 cache 的 slabs 链表 */
    void *freelist;              /* 空闲对象链表 */
    int inuse;                   /* 已使用对象数 */
    int total;                   /* 总对象数 */
    uint16_t color;              /* 颜色偏移 */
    struct slab_cache *cache;    /* 所属缓存 */
};

/* Per-CPU 缓存 */
struct cpu_cache {
    void **freelist;             /* 空闲对象数组 */
    int avail;                   /* 可用对象数 */
    int limit;                   /* 最大对象数 */
    int batchcount;              /* 批量转移数 */
    uint64_t alloc_count;
    uint64_t free_count;
};

/* Slab 缓存 */
struct slab_cache {
    /* 基本信息 */
    const char *name;            /* 缓存名称 */
    size_t obj_size;             /* 对象大小 */
    size_t align;                /* 对齐要求 */
    int flags;                   /* 标志 */
    
    /* Slab 管理 */
    struct list_head slabs_full;     /* 完全使用的 slabs */
    struct list_head slabs_partial;  /* 部分使用的 slabs */
    struct list_head slabs_free;     /* 空闲 slabs */
    
    /* 统计 */
    uint32_t nr_slabs;           /* slab 数量 */
    uint32_t nr_partial;         /* 部分 slab 数量 */
    uint32_t nr_objs;            /* 总对象数 */
    uint32_t nr_free;            /* 空闲对象数 */
    uint32_t objs_per_slab;      /* 每 slab 对象数 */
    
    /* 颜色 */
    uint16_t color_next;         /* 下一个颜色 */
    uint16_t color_max;          /* 最大颜色 */
    
    /* Per-CPU 缓存 */
    struct cpu_cache cpu_cache[MAX_CPUS];
    
    /* 锁 */
    volatile int lock;
    
    /* 构造/析构函数 */
    void (*ctor)(void *obj);
    void (*dtor)(void *obj);
    
    /* 链表 */
    struct list_head cache_list;
};

/* ========== 全局缓存 ========== */

/* 常用对象缓存 */
extern struct slab_cache *kmalloc_caches[];

/* 缓存索引计算 */
static inline int kmalloc_index(size_t size) {
    if (size <= 8) return 0;
    if (size <= 16) return 1;
    if (size <= 32) return 2;
    if (size <= 64) return 3;
    if (size <= 128) return 4;
    if (size <= 256) return 5;
    if (size <= 512) return 6;
    if (size <= 1024) return 7;
    if (size <= 2048) return 8;
    if (size <= 4096) return 9;
    if (size <= 8192) return 10;
    return -1;  /* 太大，使用 vmalloc */
}

/* ========== 初始化 ========== */

/* 初始化 slab 分配器 */
int slab_init(void);

/* ========== 缓存管理 ========== */

/* 创建缓存 */
struct slab_cache *slab_cache_create(const char *name, size_t size,
                                      size_t align, int flags,
                                      void (*ctor)(void *obj),
                                      void (*dtor)(void *obj));

/* 销毁缓存 */
void slab_cache_destroy(struct slab_cache *cache);

/* 调整缓存大小 */
int slab_cache_shrink(struct slab_cache *cache);

/* ========== 对象分配 ========== */

/* 从缓存分配对象 */
void *slab_alloc(struct slab_cache *cache, int flags);

/* 释放对象到缓存 */
void slab_free(struct slab_cache *cache, void *obj);

/* Per-CPU 快速分配 */
static inline void *slab_alloc_fast(struct slab_cache *cache, int flags) {
    int cpu = smp_processor_id();
    struct cpu_cache *cc = &cache->cpu_cache[cpu];
    
    if (cc->avail > 0) {
        cc->avail--;
        cc->alloc_count++;
        return cc->freelist[cc->avail];
    }
    
    return slab_alloc(cache, flags);
}

/* Per-CPU 快速释放 */
static inline void slab_free_fast(struct slab_cache *cache, void *obj) {
    int cpu = smp_processor_id();
    struct cpu_cache *cc = &cache->cpu_cache[cpu];
    
    if (cc->avail < cc->limit) {
        cc->freelist[cc->avail++] = obj;
        cc->free_count++;
        return;
    }
    
    slab_free(cache, obj);
}

/* ========== kmalloc 接口 ========== */

/* 内核内存分配 */
void *kmalloc(size_t size);

/* 内核内存释放 */
void kfree(void *ptr);

/* 重新分配 */
void *krealloc(void *ptr, size_t new_size);

/* 分配并清零 */
void *kzalloc(size_t size);

/* 分配对齐内存 */
void *kmalloc_align(size_t size, size_t align);

/* ========== 统计 ========== */

/* 打印缓存统计 */
void slab_cache_stats(struct slab_cache *cache);

/* 打印所有缓存统计 */
void slab_stats_all(void);

/* 获取缓存信息 */
struct slab_cache *slab_cache_get(const char *name);

/* ========== 内联分配 (小对象) ========== */

/* 快速 kmalloc (内联路径) */
static inline void *kmalloc_small(size_t size, int flags) {
    int idx = kmalloc_index(size);
    if (idx < 0 || kmalloc_caches[idx] == NULL) {
        return NULL;
    }
    return slab_alloc_fast(kmalloc_caches[idx], flags);
}

/* 快速 kfree */
static inline void kfree_small(void *ptr, size_t size) {
    int idx = kmalloc_index(size);
    if (idx >= 0 && kmalloc_caches[idx] != NULL) {
        slab_free_fast(kmalloc_caches[idx], ptr);
    }
}

#endif /* SLAB_H */
