/*
 * Zratro OS - Z-Hash (原创自适应哈希表)
 * 
 * 创新设计：
 * 1. 分段锁设计 - 高并发无竞争
 * 2. 自适应扩容 - 动态调整负载因子
 * 3. 布隆过滤器优化 - 快速排除不存在键
 * 4. 缓存行对齐 - 减少伪共享
 * 5. 罗宾汉哈希 - 减少探测长度
 */

#ifndef ZHASH_H
#define ZHASH_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== Z-Hash 配置 ========== */

#define ZH_INITIAL_SIZE       64          /* 初始容量 */
#define ZH_MAX_LOAD_FACTOR    75          /* 最大负载因子 (%) */
#define ZH_MIN_LOAD_FACTOR    25          /* 最小负载因子 (%) */
#define ZH_SEGMENT_BITS       6           /* 分段数位数 (64段) */
#define ZH_SEGMENT_COUNT      (1 << ZH_SEGMENT_BITS)
#define ZH_SEGMENT_MASK       (ZH_SEGMENT_COUNT - 1)
#define ZH_BLOOM_SIZE         8           /* 布隆过滤器字节数 */
#define ZH_CACHE_LINE         64          /* 缓存行大小 */

/* ========== 哈希条目 ========== */

struct zh_entry {
    uint64_t key;               /* 键 */
    void *value;                /* 值 */
    uint64_t hash;              /* 缓存的哈希值 */
    int8_t probe_dist;          /* 探测距离 (罗宾汉哈希) */
    uint8_t flags;              /* 标志 */
    uint8_t reserved[6];        /* 保留对齐 */
} __attribute__((packed, aligned(16)));

/* 条目标志 */
#define ZH_ENTRY_EMPTY      0
#define ZH_ENTRY_USED       1
#define ZH_ENTRY_DELETED    2
#define ZH_ENTRY_TOMBSTONE  3

/* ========== 哈希段 ========== */

struct zh_segment {
    struct zh_entry *entries;   /* 条目数组 */
    uint32_t size;              /* 段大小 */
    uint32_t count;             /* 已用条目数 */
    uint32_t tombstones;        /* 墓碑数量 */
    spinlock_t lock;            /* 段级锁 */
    char __pad[ZH_CACHE_LINE - sizeof(spinlock_t) - 3 * sizeof(uint32_t) - sizeof(void *)];
} __attribute__((aligned(ZH_CACHE_LINE)));

/* ========== 布隆过滤器 ========== */

struct zh_bloom {
    uint8_t bits[ZH_BLOOM_SIZE];
};

static inline void zh_bloom_add(struct zh_bloom *bloom, uint64_t hash) {
    /* 使用哈希的多个位设置布隆过滤器 */
    for (int i = 0; i < 4; i++) {
        int bit = (hash >> (i * 6)) & 63;
        bloom->bits[bit / 8] |= (1 << (bit % 8));
    }
}

static inline int zh_bloom_might_contain(struct zh_bloom *bloom, uint64_t hash) {
    /* 检查所有对应位是否设置 */
    for (int i = 0; i < 4; i++) {
        int bit = (hash >> (i * 6)) & 63;
        if (!(bloom->bits[bit / 8] & (1 << (bit % 8)))) {
            return 0;  /* 确定不包含 */
        }
    }
    return 1;  /* 可能包含 */
}

/* ========== Z-Hash 主结构 ========== */

struct zhash {
    struct zh_segment segments[ZH_SEGMENT_COUNT];
    struct zh_bloom bloom;
    
    /* 统计 */
    uint64_t total_inserts;
    uint64_t total_lookups;
    uint64_t total_deletes;
    uint64_t collisions;
    uint64_t rehashes;
    
    /* 自适应参数 */
    uint32_t target_load;       /* 目标负载因子 */
    uint32_t growth_factor;     /* 增长因子 (百分比) */
    
    /* 状态 */
    int is_resizing;
    spinlock_t resize_lock;
};

/* ========== 迭代器 ========== */

struct zh_iter {
    struct zhash *hash;
    uint32_t seg_idx;
    uint32_t entry_idx;
    struct zh_entry *current;
};

/* ========== 公共 API ========== */

/* 创建/销毁 */
struct zhash *zh_create(uint32_t initial_size);
void zh_destroy(struct zhash *zh);

/* 基本操作 */
int zh_insert(struct zhash *zh, uint64_t key, void *value);
void *zh_lookup(struct zhash *zh, uint64_t key);
int zh_delete(struct zhash *zh, uint64_t key);
int zh_update(struct zhash *zh, uint64_t key, void *value);

/* 批量操作 */
int zh_insert_batch(struct zhash *zh, uint64_t *keys, void **values, uint32_t count);
int zh_delete_batch(struct zhash *zh, uint64_t *keys, uint32_t count);

/* 统计 */
uint32_t zh_count(struct zhash *zh);
uint32_t zh_size(struct zhash *zh);
uint32_t zh_load_factor(struct zhash *zh);
void zh_stats(struct zhash *zh, uint64_t *inserts, uint64_t *lookups, 
              uint64_t *collisions, uint64_t *rehashes);

/* 迭代 */
void zh_iter_init(struct zhash *zh, struct zh_iter *iter);
struct zh_entry *zh_iter_next(struct zh_iter *iter);

/* 调优 */
void zh_set_target_load(struct zhash *zh, uint32_t load_percent);
void zh_set_growth_factor(struct zhash *zh, uint32_t factor);

/* 哈希函数 */
static inline uint64_t zh_hash(uint64_t key) {
    /* MurmurHash3 终结器 */
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return key;
}

/* 从段索引计算哈希 */
static inline uint32_t zh_segment_idx(uint64_t hash) {
    return hash & ZH_SEGMENT_MASK;
}

/* ========== 高级特性 ========== */

/* 并发接口 */
int zh_insert_locked(struct zhash *zh, uint64_t key, void *value, int lock_segment);
void *zh_lookup_locked(struct zhash *zh, uint64_t key, int lock_segment);

/* 预留容量 */
int zh_reserve(struct zhash *zh, uint32_t capacity);

/* 清空 */
void zh_clear(struct zhash *zh);

/* 压缩 */
int zh_compact(struct zhash *zh);

/* 带回调操作 */
typedef void (*zh_callback_fn)(uint64_t key, void *value, void *data);
void zh_foreach(struct zhash *zh, zh_callback_fn fn, void *data);

/* 查找或插入 */
void *zh_lookup_or_insert(struct zhash *zh, uint64_t key, void *default_value);

/* 原子更新 */
int zh_update_atomic(struct zhash *zh, uint64_t key, 
                     void *(*update_fn)(void *old, void *data), void *data);

#endif /* ZHASH_H */
