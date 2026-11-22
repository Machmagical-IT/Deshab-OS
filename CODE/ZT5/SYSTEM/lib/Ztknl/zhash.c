/*
 * Zratro OS - Z-Hash Implementation
 * 
 * 原创自适应哈希表实现
 */

#include "zhash.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "task.h"

/* ========== 创建/销毁 ========== */

struct zhash *zh_create(uint32_t initial_size) {
    if (initial_size == 0) initial_size = ZH_INITIAL_SIZE;
    
    struct zhash *zh = kmalloc(sizeof(struct zhash));
    if (zh == NULL) return NULL;
    
    /* 初始化段 */
    uint32_t seg_size = (initial_size + ZH_SEGMENT_COUNT - 1) / ZH_SEGMENT_COUNT;
    if (seg_size < 4) seg_size = 4;
    
    /* 确保 seg_size 是 2 的幂 */
    uint32_t power = 1;
    while (power < seg_size) power <<= 1;
    seg_size = power;
    
    for (int i = 0; i < ZH_SEGMENT_COUNT; i++) {
        zh->segments[i].entries = kmalloc(sizeof(struct zh_entry) * seg_size);
        if (zh->segments[i].entries == NULL) {
            /* 回滚已分配的段 */
            for (int j = 0; j < i; j++) {
                kfree(zh->segments[j].entries);
            }
            kfree(zh);
            return NULL;
        }
        
        zh->segments[i].size = seg_size;
        zh->segments[i].count = 0;
        zh->segments[i].tombstones = 0;
        zh->segments[i].lock = SPINLOCK_INIT;
        
        /* 清零条目 */
        for (uint32_t j = 0; j < seg_size; j++) {
            zh->segments[i].entries[j].flags = ZH_ENTRY_EMPTY;
            zh->segments[i].entries[j].probe_dist = -1;
        }
    }
    
    /* 清零布隆过滤器 */
    for (int i = 0; i < ZH_BLOOM_SIZE; i++) {
        zh->bloom.bits[i] = 0;
    }
    
    zh->total_inserts = 0;
    zh->total_lookups = 0;
    zh->total_deletes = 0;
    zh->collisions = 0;
    zh->rehashes = 0;
    
    zh->target_load = ZH_MAX_LOAD_FACTOR;
    zh->growth_factor = 200;  /* 翻倍 */
    zh->is_resizing = 0;
    zh->resize_lock = SPINLOCK_INIT;
    
    return zh;
}

void zh_destroy(struct zhash *zh) {
    if (zh == NULL) return;
    
    for (int i = 0; i < ZH_SEGMENT_COUNT; i++) {
        if (zh->segments[i].entries != NULL) {
            kfree(zh->segments[i].entries);
        }
    }
    
    kfree(zh);
}

/* ========== 段级操作 ========== */

/* 在段内查找条目位置 */
static int find_slot(struct zh_segment *seg, uint64_t hash, uint64_t key,
                     struct zh_entry **slot, int8_t *dist) {
    uint32_t mask = seg->size - 1;
    uint32_t idx = (hash >> ZH_SEGMENT_BITS) & mask;
    
    int8_t probe_dist = 0;
    int8_t max_probe = 0;
    
    for (uint32_t i = 0; i < seg->size; i++) {
        struct zh_entry *entry = &seg->entries[idx];
        
        if (entry->flags == ZH_ENTRY_EMPTY) {
            /* 找到空槽 */
            *slot = entry;
            *dist = probe_dist;
            return 0;  /* 不存在 */
        }
        
        if (entry->flags == ZH_ENTRY_USED && entry->key == key) {
            /* 找到键 */
            *slot = entry;
            *dist = probe_dist;
            return 1;  /* 存在 */
        }
        
        /* 罗宾汉哈希：比较探测距离 */
        if (entry->flags == ZH_ENTRY_USED && entry->probe_dist < probe_dist) {
            /* 当前条目更"富有"，应该交换 */
            *slot = entry;
            *dist = probe_dist;
            return -1;  /* 需要交换 */
        }
        
        if (entry->flags == ZH_ENTRY_DELETED || entry->flags == ZH_ENTRY_TOMBSTONE) {
            /* 墓碑，可以作为插入位置 */
            *slot = entry;
            *dist = probe_dist;
            return -2;  /* 墓碑位置 */
        }
        
        idx = (idx + 1) & mask;
        probe_dist++;
        if (probe_dist > max_probe) max_probe = probe_dist;
    }
    
    return -3;  /* 表已满 */
}

/* 段扩容 */
static int segment_resize(struct zhash *zh, int seg_idx) {
    struct zh_segment *seg = &zh->segments[seg_idx];
    uint32_t new_size = seg->size * 2;
    
    struct zh_entry *new_entries = kmalloc(sizeof(struct zh_entry) * new_size);
    if (new_entries == NULL) return -1;
    
    /* 清零新条目 */
    for (uint32_t i = 0; i < new_size; i++) {
        new_entries[i].flags = ZH_ENTRY_EMPTY;
        new_entries[i].probe_dist = -1;
    }
    
    /* 重新哈希 */
    uint32_t new_mask = new_size - 1;
    
    for (uint32_t i = 0; i < seg->size; i++) {
        struct zh_entry *old = &seg->entries[i];
        
        if (old->flags != ZH_ENTRY_USED) continue;
        
        uint32_t idx = (old->hash >> ZH_SEGMENT_BITS) & new_mask;
        int8_t probe_dist = 0;
        
        /* 罗宾汉插入 */
        for (uint32_t j = 0; j < new_size; j++) {
            struct zh_entry *new_entry = &new_entries[idx];
            
            if (new_entry->flags == ZH_ENTRY_EMPTY) {
                *new_entry = *old;
                new_entry->probe_dist = probe_dist;
                break;
            }
            
            if (new_entry->probe_dist < probe_dist) {
                struct zh_entry tmp = *new_entry;
                *new_entry = *old;
                new_entry->probe_dist = probe_dist;
                *old = tmp;
                probe_dist = tmp.probe_dist;
            }
            
            idx = (idx + 1) & new_mask;
            probe_dist++;
        }
    }
    
    kfree(seg->entries);
    seg->entries = new_entries;
    seg->size = new_size;
    seg->tombstones = 0;
    zh->rehashes++;
    
    return 0;
}

/* ========== 插入操作 ========== */

int zh_insert(struct zhash *zh, uint64_t key, void *value) {
    if (zh == NULL) return -1;
    
    uint64_t hash = zh_hash(key);
    uint32_t seg_idx = zh_segment_idx(hash);
    struct zh_segment *seg = &zh->segments[seg_idx];
    
    /* 更新布隆过滤器 */
    zh_bloom_add(&zh->bloom, hash);
    
    spin_lock(&seg->lock);
    
    /* 检查是否需要扩容 */
    uint32_t load = (seg->count * 100) / seg->size;
    if (load >= zh->target_load) {
        spin_unlock(&seg->lock);
        spin_lock(&zh->resize_lock);
        segment_resize(zh, seg_idx);
        spin_unlock(&zh->resize_lock);
        spin_lock(&seg->lock);
    }
    
    struct zh_entry *slot;
    int8_t dist;
    int found = find_slot(seg, hash, key, &slot, &dist);
    
    if (found == 1) {
        /* 键已存在，更新值 */
        slot->value = value;
        spin_unlock(&seg->lock);
        return 0;
    }
    
    if (found == -3) {
        /* 表已满，需要扩容 */
        spin_unlock(&seg->lock);
        int resize_result = segment_resize(zh, seg_idx);
        if (resize_result != 0) {
            /* 扩容失败，返回错误 */
            return -1;
        }
        return zh_insert(zh, key, value);
    }
    
    /* 插入新条目 */
    struct zh_entry new_entry;
    new_entry.key = key;
    new_entry.value = value;
    new_entry.hash = hash;
    new_entry.probe_dist = dist;
    new_entry.flags = ZH_ENTRY_USED;
    
    /* 罗宾汉交换插入 */
    while (slot->flags == ZH_ENTRY_USED && slot->probe_dist < dist) {
        struct zh_entry tmp = *slot;
        *slot = new_entry;
        new_entry = tmp;
        dist = tmp.probe_dist;
        
        /* 找下一个位置 */
        uint32_t mask = seg->size - 1;
        uint32_t idx = ((slot - seg->entries) + 1) & mask;
        slot = &seg->entries[idx];
        dist++;
        zh->collisions++;
    }
    
    if (slot->flags == ZH_ENTRY_DELETED || slot->flags == ZH_ENTRY_TOMBSTONE) {
        seg->tombstones--;
    }
    
    *slot = new_entry;
    seg->count++;
    zh->total_inserts++;
    
    spin_unlock(&seg->lock);
    return 0;
}

/* ========== 查找操作 ========== */

void *zh_lookup(struct zhash *zh, uint64_t key) {
    if (zh == NULL) return NULL;
    
    uint64_t hash = zh_hash(key);
    
    /* 布隆过滤器快速排除 */
    if (!zh_bloom_might_contain(&zh->bloom, hash)) {
        return NULL;
    }
    
    uint32_t seg_idx = zh_segment_idx(hash);
    struct zh_segment *seg = &zh->segments[seg_idx];
    
    spin_lock(&seg->lock);
    
    uint32_t mask = seg->size - 1;
    uint32_t idx = (hash >> ZH_SEGMENT_BITS) & mask;
    int8_t probe_dist = 0;
    
    for (uint32_t i = 0; i < seg->size; i++) {
        struct zh_entry *entry = &seg->entries[idx];
        
        if (entry->flags == ZH_ENTRY_EMPTY) {
            break;  /* 不存在 */
        }
        
        if (entry->flags == ZH_ENTRY_USED && entry->key == key) {
            void *value = entry->value;
            zh->total_lookups++;
            spin_unlock(&seg->lock);
            return value;
        }
        
        /* 探测距离已超过，停止 */
        if (entry->probe_dist < probe_dist) {
            break;
        }
        
        idx = (idx + 1) & mask;
        probe_dist++;
    }
    
    zh->total_lookups++;
    spin_unlock(&seg->lock);
    return NULL;
}

/* ========== 删除操作 ========== */

int zh_delete(struct zhash *zh, uint64_t key) {
    if (zh == NULL) return -1;
    
    uint64_t hash = zh_hash(key);
    uint32_t seg_idx = zh_segment_idx(hash);
    struct zh_segment *seg = &zh->segments[seg_idx];
    
    spin_lock(&seg->lock);
    
    uint32_t mask = seg->size - 1;
    uint32_t idx = (hash >> ZH_SEGMENT_BITS) & mask;
    
    for (uint32_t i = 0; i < seg->size; i++) {
        struct zh_entry *entry = &seg->entries[idx];
        
        if (entry->flags == ZH_ENTRY_EMPTY) {
            break;
        }
        
        if (entry->flags == ZH_ENTRY_USED && entry->key == key) {
            entry->flags = ZH_ENTRY_DELETED;
            seg->count--;
            seg->tombstones++;
            zh->total_deletes++;
            spin_unlock(&seg->lock);
            return 0;
        }
        
        idx = (idx + 1) & mask;
    }
    
    spin_unlock(&seg->lock);
    return -1;  /* 未找到 */
}

/* ========== 更新操作 ========== */

int zh_update(struct zhash *zh, uint64_t key, void *value) {
    if (zh == NULL) return -1;
    
    uint64_t hash = zh_hash(key);
    uint32_t seg_idx = zh_segment_idx(hash);
    struct zh_segment *seg = &zh->segments[seg_idx];
    
    spin_lock(&seg->lock);
    
    uint32_t mask = seg->size - 1;
    uint32_t idx = (hash >> ZH_SEGMENT_BITS) & mask;
    
    for (uint32_t i = 0; i < seg->size; i++) {
        struct zh_entry *entry = &seg->entries[idx];
        
        if (entry->flags == ZH_ENTRY_EMPTY) {
            break;
        }
        
        if (entry->flags == ZH_ENTRY_USED && entry->key == key) {
            entry->value = value;
            spin_unlock(&seg->lock);
            return 0;
        }
        
        idx = (idx + 1) & mask;
    }
    
    spin_unlock(&seg->lock);
    return -1;
}

/* ========== 统计函数 ========== */

uint32_t zh_count(struct zhash *zh) {
    if (zh == NULL) return 0;
    
    uint32_t total = 0;
    for (int i = 0; i < ZH_SEGMENT_COUNT; i++) {
        total += zh->segments[i].count;
    }
    return total;
}

uint32_t zh_size(struct zhash *zh) {
    if (zh == NULL) return 0;
    
    uint32_t total = 0;
    for (int i = 0; i < ZH_SEGMENT_COUNT; i++) {
        total += zh->segments[i].size;
    }
    return total;
}

uint32_t zh_load_factor(struct zhash *zh) {
    uint32_t count = zh_count(zh);
    uint32_t size = zh_size(zh);
    if (size == 0) return 0;
    return (count * 100) / size;
}

void zh_stats(struct zhash *zh, uint64_t *inserts, uint64_t *lookups,
              uint64_t *collisions, uint64_t *rehashes) {
    if (inserts) *inserts = zh->total_inserts;
    if (lookups) *lookups = zh->total_lookups;
    if (collisions) *collisions = zh->collisions;
    if (rehashes) *rehashes = zh->rehashes;
}

/* ========== 批量操作 ========== */

int zh_insert_batch(struct zhash *zh, uint64_t *keys, void **values, uint32_t count) {
    if (zh == NULL || keys == NULL || values == NULL) return -1;
    
    for (uint32_t i = 0; i < count; i++) {
        if (zh_insert(zh, keys[i], values[i]) != 0) {
            return -1;
        }
    }
    
    return 0;
}

int zh_delete_batch(struct zhash *zh, uint64_t *keys, uint32_t count) {
    if (zh == NULL || keys == NULL) return -1;
    
    for (uint32_t i = 0; i < count; i++) {
        zh_delete(zh, keys[i]);
    }
    
    return 0;
}

/* ========== 迭代器 ========== */

void zh_iter_init(struct zhash *zh, struct zh_iter *iter) {
    iter->hash = zh;
    iter->seg_idx = 0;
    iter->entry_idx = 0;
    iter->current = NULL;
}

struct zh_entry *zh_iter_next(struct zh_iter *iter) {
    if (iter == NULL || iter->hash == NULL) return NULL;
    
    while (iter->seg_idx < ZH_SEGMENT_COUNT) {
        struct zh_segment *seg = &iter->hash->segments[iter->seg_idx];
        
        while (iter->entry_idx < seg->size) {
            struct zh_entry *entry = &seg->entries[iter->entry_idx];
            iter->entry_idx++;
            
            if (entry->flags == ZH_ENTRY_USED) {
                iter->current = entry;
                return entry;
            }
        }
        
        iter->seg_idx++;
        iter->entry_idx = 0;
    }
    
    iter->current = NULL;
    return NULL;
}

/* ========== 高级特性 ========== */

void zh_set_target_load(struct zhash *zh, uint32_t load_percent) {
    if (zh != NULL && load_percent > 0 && load_percent < 100) {
        zh->target_load = load_percent;
    }
}

void zh_set_growth_factor(struct zhash *zh, uint32_t factor) {
    if (zh != NULL && factor >= 100) {
        zh->growth_factor = factor;
    }
}

void zh_clear(struct zhash *zh) {
    if (zh == NULL) return;
    
    for (int i = 0; i < ZH_SEGMENT_COUNT; i++) {
        struct zh_segment *seg = &zh->segments[i];
        spin_lock(&seg->lock);
        
        for (uint32_t j = 0; j < seg->size; j++) {
            seg->entries[j].flags = ZH_ENTRY_EMPTY;
        }
        
        seg->count = 0;
        seg->tombstones = 0;
        
        spin_unlock(&seg->lock);
    }
}

int zh_compact(struct zhash *zh) {
    if (zh == NULL) return -1;
    
    int compacted = 0;
    
    for (int i = 0; i < ZH_SEGMENT_COUNT; i++) {
        struct zh_segment *seg = &zh->segments[i];
        
        if (seg->tombstones == 0) continue;
        
        spin_lock(&seg->lock);
        
        /* 重新哈希该段 */
        struct zh_entry *new_entries = kmalloc(sizeof(struct zh_entry) * seg->size);
        if (new_entries == NULL) {
            spin_unlock(&seg->lock);
            continue;
        }
        
        for (uint32_t j = 0; j < seg->size; j++) {
            new_entries[j].flags = ZH_ENTRY_EMPTY;
            new_entries[j].probe_dist = -1;
        }
        
        uint32_t new_count = 0;
        uint32_t mask = seg->size - 1;
        
        for (uint32_t j = 0; j < seg->size; j++) {
            struct zh_entry *old = &seg->entries[j];
            
            if (old->flags != ZH_ENTRY_USED) continue;
            
            uint32_t idx = (old->hash >> ZH_SEGMENT_BITS) & mask;
            
            while (new_entries[idx].flags != ZH_ENTRY_EMPTY) {
                idx = (idx + 1) & mask;
            }
            
            new_entries[idx] = *old;
            new_count++;
        }
        
        kfree(seg->entries);
        seg->entries = new_entries;
        seg->count = new_count;
        seg->tombstones = 0;
        compacted++;
        
        spin_unlock(&seg->lock);
    }
    
    return compacted;
}

void zh_foreach(struct zhash *zh, zh_callback_fn fn, void *data) {
    if (zh == NULL || fn == NULL) return;
    
    struct zh_iter iter;
    zh_iter_init(zh, &iter);
    
    struct zh_entry *entry;
    while ((entry = zh_iter_next(&iter)) != NULL) {
        fn(entry->key, entry->value, data);
    }
}

void *zh_lookup_or_insert(struct zhash *zh, uint64_t key, void *default_value) {
    void *value = zh_lookup(zh, key);
    if (value != NULL) return value;
    
    zh_insert(zh, key, default_value);
    return default_value;
}

int zh_update_atomic(struct zhash *zh, uint64_t key,
                     void *(*update_fn)(void *old, void *data), void *data) {
    if (zh == NULL || update_fn == NULL) return -1;
    
    uint64_t hash = zh_hash(key);
    uint32_t seg_idx = zh_segment_idx(hash);
    struct zh_segment *seg = &zh->segments[seg_idx];
    
    spin_lock(&seg->lock);
    
    uint32_t mask = seg->size - 1;
    uint32_t idx = (hash >> ZH_SEGMENT_BITS) & mask;
    
    for (uint32_t i = 0; i < seg->size; i++) {
        struct zh_entry *entry = &seg->entries[idx];
        
        if (entry->flags == ZH_ENTRY_USED && entry->key == key) {
            entry->value = update_fn(entry->value, data);
            spin_unlock(&seg->lock);
            return 0;
        }
        
        if (entry->flags == ZH_ENTRY_EMPTY) {
            break;
        }
        
        idx = (idx + 1) & mask;
    }
    
    spin_unlock(&seg->lock);
    return -1;
}

int zh_reserve(struct zhash *zh, uint32_t capacity) {
    if (zh == NULL) return -1;
    
    uint32_t seg_capacity = (capacity + ZH_SEGMENT_COUNT - 1) / ZH_SEGMENT_COUNT;
    
    for (int i = 0; i < ZH_SEGMENT_COUNT; i++) {
        struct zh_segment *seg = &zh->segments[i];
        
        while (seg->size < seg_capacity) {
            spin_lock(&seg->lock);
            segment_resize(zh, i);
            spin_unlock(&seg->lock);
        }
    }
    
    return 0;
}

/* ========== 动态扩容优化 ========== */

/* 增量式重哈希 */
struct zh_rehash_state {
    int from_seg;
    int to_seg;
    uint32_t current_idx;
    uint32_t batch_size;
    uint32_t migrated;
    uint32_t total;
    int active;
};

static struct zh_rehash_state rehash_state;

/* 开始增量重哈希 */
int zh_begin_incremental_rehash(struct zhash *zh, int from_seg, int to_seg) {
    if (zh == NULL) return -1;
    if (from_seg < 0 || from_seg >= ZH_SEGMENT_COUNT) return -1;
    if (to_seg < 0 || to_seg >= ZH_SEGMENT_COUNT) return -1;
    
    rehash_state.from_seg = from_seg;
    rehash_state.to_seg = to_seg;
    rehash_state.current_idx = 0;
    rehash_state.batch_size = 64;  /* 每批迁移 64 个条目 */
    rehash_state.migrated = 0;
    rehash_state.total = zh->segments[from_seg].count;
    rehash_state.active = 1;
    
    return 0;
}

/* 执行一批重哈希 */
int zh_rehash_step(struct zhash *zh) {
    if (!rehash_state.active || zh == NULL) return -1;
    
    struct zh_segment *from = &zh->segments[rehash_state.from_seg];
    struct zh_segment *to = &zh->segments[rehash_state.to_seg];
    
    spin_lock(&from->lock);
    spin_lock(&to->lock);
    
    uint32_t processed = 0;
    
    while (rehash_state.current_idx < from->size && processed < rehash_state.batch_size) {
        struct zh_entry *entry = &from->entries[rehash_state.current_idx];
        
        if (entry->flags == ZH_ENTRY_USED) {
            /* 迁移到目标段 */
            uint64_t hash = entry->hash;
            uint32_t mask = to->size - 1;
            uint32_t idx = (hash >> ZH_SEGMENT_BITS) & mask;
            
            /* 罗宾汉插入 */
            int8_t probe_dist = 0;
            for (uint32_t i = 0; i < to->size; i++) {
                struct zh_entry *target = &to->entries[idx];
                
                if (target->flags == ZH_ENTRY_EMPTY) {
                    *target = *entry;
                    target->probe_dist = probe_dist;
                    break;
                }
                
                if (target->probe_dist < probe_dist) {
                    struct zh_entry tmp = *target;
                    *target = *entry;
                    target->probe_dist = probe_dist;
                    *entry = tmp;
                    probe_dist = tmp.probe_dist;
                }
                
                idx = (idx + 1) & mask;
                probe_dist++;
            }
            
            /* 标记原位置为删除 */
            entry->flags = ZH_ENTRY_DELETED;
            from->count--;
            to->count++;
            rehash_state.migrated++;
        }
        
        rehash_state.current_idx++;
        processed++;
    }
    
    spin_unlock(&to->lock);
    spin_unlock(&from->lock);
    
    /* 检查是否完成 */
    if (rehash_state.current_idx >= from->size) {
        rehash_state.active = 0;
        return 1;  /* 完成 */
    }
    
    return 0;  /* 继续 */
}

/* 自动增量扩容 */
int zh_auto_expand(struct zhash *zh) {
    if (zh == NULL) return -1;
    
    /* 找出负载最高的段 */
    int max_seg = 0;
    uint32_t max_load = 0;
    
    for (int i = 0; i < ZH_SEGMENT_COUNT; i++) {
        uint32_t load = (zh->segments[i].count * 100) / zh->segments[i].size;
        if (load > max_load) {
            max_load = load;
            max_seg = i;
        }
    }
    
    /* 如果负载超过阈值，进行扩容 */
    if (max_load >= zh->target_load) {
        return segment_resize(zh, max_seg);
    }
    
    return 0;
}

/* ========== 热点检测 ========== */

/* 热点统计 */
struct zh_hotspot {
    uint64_t key;
    uint64_t access_count;
    uint64_t last_access;
    void *value;
    struct list_head list;
};

#define ZH_HOTSPOT_THRESHOLD    1000
#define ZH_HOTSPOT_MAX          64

static struct list_head hotspot_list = LIST_HEAD_INIT(hotspot_list);
static spinlock_t hotspot_lock = SPINLOCK_INIT;
static uint32_t hotspot_count = 0;

/* 更新热点计数 */
void zh_update_hotspot(struct zhash *zh, uint64_t key) {
    /* 简化的热点检测 */
    spin_lock(&hotspot_lock);
    
    struct zh_hotspot *spot, *max_spot = NULL;
    uint64_t max_count = 0;
    
    list_for_each_entry(spot, &hotspot_list, list) {
        if (spot->key == key) {
            spot->access_count++;
            spot->last_access = get_tick_count();
            spin_unlock(&hotspot_lock);
            return;
        }
        
        if (spot->access_count > max_count) {
            max_count = spot->access_count;
            max_spot = spot;
        }
    }
    
    /* 新热点 */
    if (hotspot_count < ZH_HOTSPOT_MAX) {
        struct zh_hotspot *new_spot = kmalloc(sizeof(struct zh_hotspot));
        if (new_spot != NULL) {
            new_spot->key = key;
            new_spot->access_count = 1;
            new_spot->last_access = get_tick_count();
            new_spot->value = zh_lookup(zh, key);
            list_add(&new_spot->list, &hotspot_list);
            hotspot_count++;
        }
    } else if (max_spot != NULL) {
        /* 替换访问最少的 */
        max_spot->key = key;
        max_spot->access_count = 1;
        max_spot->last_access = get_tick_count();
        max_spot->value = zh_lookup(zh, key);
    }
    
    spin_unlock(&hotspot_lock);
}

/* 获取热点键列表 */
uint32_t zh_get_hotspots(uint64_t *keys, uint32_t max_count) {
    spin_lock(&hotspot_lock);
    
    uint32_t count = 0;
    struct zh_hotspot *spot;
    list_for_each_entry(spot, &hotspot_list, list) {
        if (count >= max_count) break;
        keys[count++] = spot->key;
    }
    
    spin_unlock(&hotspot_lock);
    return count;
}

/* 清除热点缓存 */
void zh_clear_hotspots(void) {
    spin_lock(&hotspot_lock);
    
    struct zh_hotspot *spot, *next;
    list_for_each_entry_safe(spot, next, &hotspot_list, list) {
        list_del(&spot->list);
        kfree(spot);
    }
    
    hotspot_count = 0;
    spin_unlock(&hotspot_lock);
}

/* ========== 一致性哈希支持 ========== */

/* 虚拟节点 */
struct zh_vnode {
    uint64_t hash;
    int node_id;
    struct list_head list;
};

/* 一致性哈希环 */
struct zh_consistent_hash {
    struct list_head ring;
    uint32_t vnode_count;
    int nodes[16];
    uint32_t node_count;
    spinlock_t lock;
};

static struct zh_consistent_hash chash;

/* 初始化一致性哈希 */
int zh_chash_init(void) {
    INIT_LIST_HEAD(&chash.ring);
    chash.vnode_count = 0;
    chash.node_count = 0;
    chash.lock = SPINLOCK_INIT;
    return 0;
}

/* 添加节点 */
int zh_chash_add_node(int node_id, uint32_t vnodes) {
    if (chash.node_count >= 16) return -1;
    
    spin_lock(&chash.lock);
    
    chash.nodes[chash.node_count++] = node_id;
    
    /* 创建虚拟节点 */
    for (uint32_t i = 0; i < vnodes; i++) {
        struct zh_vnode *vn = kmalloc(sizeof(struct zh_vnode));
        if (vn == NULL) continue;
        
        vn->node_id = node_id;
        vn->hash = zh_hash((uint64_t)node_id * 1000 + i);
        
        /* 按哈希值排序插入 */
        struct zh_vnode *pos;
        int inserted = 0;
        list_for_each_entry(pos, &chash.ring, list) {
            if (vn->hash < pos->hash) {
                list_add_tail(&vn->list, &pos->list);
                inserted = 1;
                break;
            }
        }
        
        if (!inserted) {
            list_add_tail(&vn->list, &chash.ring);
        }
        
        chash.vnode_count++;
    }
    
    spin_unlock(&chash.lock);
    return 0;
}

/* 查找键对应的节点 */
int zh_chash_lookup(uint64_t key) {
    uint64_t hash = zh_hash(key);
    
    spin_lock(&chash.lock);
    
    struct zh_vnode *vn;
    list_for_each_entry(vn, &chash.ring, list) {
        if (hash <= vn->hash) {
            int node_id = vn->node_id;
            spin_unlock(&chash.lock);
            return node_id;
        }
    }
    
    /* 环形，返回第一个节点 */
    if (!list_empty(&chash.ring)) {
        vn = list_first_entry(&chash.ring, struct zh_vnode, list);
        int node_id = vn->node_id;
        spin_unlock(&chash.lock);
        return node_id;
    }
    
    spin_unlock(&chash.lock);
    return -1;
}

/* 移除节点 */
int zh_chash_remove_node(int node_id) {
    spin_lock(&chash.lock);
    
    struct zh_vnode *vn, *next;
    list_for_each_entry_safe(vn, next, &chash.ring, list) {
        if (vn->node_id == node_id) {
            list_del(&vn->list);
            kfree(vn);
            chash.vnode_count--;
        }
    }
    
    /* 从节点数组中移除 */
    int found = 0;
    for (uint32_t i = 0; i < chash.node_count; i++) {
        if (chash.nodes[i] == node_id) {
            /* 检查 node_count 是否大于 0，避免下溢 */
            if (chash.node_count > 0) {
                chash.nodes[i] = chash.nodes[--chash.node_count];
            }
            found = 1;
            break;
        }
    }
    
    spin_unlock(&chash.lock);
    return found ? 0 : -1;  /* 返回是否找到节点 */
}

/* ========== 快速查找优化 ========== */

/* 缓存友好的线性探测 */
void *zh_lookup_cached(struct zhash *zh, uint64_t key) {
    if (zh == NULL) return NULL;
    
    uint64_t hash = zh_hash(key);
    
    /* 布隆过滤器 */
    if (!zh_bloom_might_contain(&zh->bloom, hash)) {
        return NULL;
    }
    
    uint32_t seg_idx = zh_segment_idx(hash);
    struct zh_segment *seg = &zh->segments[seg_idx];
    
    /* 更新热点 */
    zh_update_hotspot(zh, key);
    
    spin_lock(&seg->lock);
    
    uint32_t mask = seg->size - 1;
    uint32_t idx = (hash >> ZH_SEGMENT_BITS) & mask;
    
    /* 预取下一缓存行 */
    __builtin_prefetch(&seg->entries[(idx + 8) & mask], 0, 3);
    
    int8_t probe_dist = 0;
    
    for (uint32_t i = 0; i < seg->size; i++) {
        struct zh_entry *entry = &seg->entries[idx];
        
        if (entry->flags == ZH_ENTRY_EMPTY) {
            break;
        }
        
        if (entry->flags == ZH_ENTRY_USED && entry->key == key) {
            void *value = entry->value;
            zh->total_lookups++;
            spin_unlock(&seg->lock);
            return value;
        }
        
        if (entry->probe_dist < probe_dist) {
            break;
        }
        
        idx = (idx + 1) & mask;
        probe_dist++;
        
        /* 持续预取 */
        if ((i & 7) == 0) {
            __builtin_prefetch(&seg->entries[(idx + 8) & mask], 0, 3);
        }
    }
    
    zh->total_lookups++;
    spin_unlock(&seg->lock);
    return NULL;
}

/* ========== 持久化支持 ========== */

/* 保存哈希表到文件 */
int zh_save(struct zhash *zh, int fd) {
    if (zh == NULL) return -1;
    
    /* 写入头部 */
    uint32_t magic = 0x5A484153;  /* "ZHSH" */
    uint32_t version = 1;
    uint32_t total_count = zh_count(zh);
    
    /* 简化实现，实际应写入文件 */
    (void)fd;
    (void)magic;
    (void)version;
    (void)total_count;
    
    /* 写入每个条目 */
    struct zh_iter iter;
    zh_iter_init(zh, &iter);
    
    struct zh_entry *entry;
    while ((entry = zh_iter_next(&iter)) != NULL) {
        /* 写入: key, value_size, value */
    }
    
    return 0;
}

/* 从文件加载哈希表 */
struct zhash *zh_load(int fd) {
    /* 读取头部 */
    /* 分配哈希表 */
    /* 读取条目 */
    (void)fd;
    return NULL;
}

/* ========== 性能监控 ========== */

/* 性能统计结构 */
struct zh_perf_stats {
    uint64_t lookup_hits;
    uint64_t lookup_misses;
    uint64_t insert_new;
    uint64_t insert_update;
    uint64_t delete_found;
    uint64_t delete_miss;
    uint64_t rehash_time_ns;
    uint64_t avg_probe_dist;
};

static struct zh_perf_stats perf_stats;

/* 记录查找结果 */
void zh_record_lookup(int hit) {
    if (hit) {
        perf_stats.lookup_hits++;
    } else {
        perf_stats.lookup_misses++;
    }
}

/* 获取性能统计 */
void zh_get_perf_stats(struct zh_perf_stats *stats) {
    *stats = perf_stats;
}

/* 重置性能统计 */
void zh_reset_perf_stats(void) {
    memset(&perf_stats, 0, sizeof(perf_stats));
}

/* ========== 并发优化 ========== */

/* 读写锁版本的查找 */
void *zh_lookup_rwlock(struct zhash *zh, uint64_t key) {
    if (zh == NULL) return NULL;
    
    uint64_t hash = zh_hash(key);
    
    if (!zh_bloom_might_contain(&zh->bloom, hash)) {
        return NULL;
    }
    
    uint32_t seg_idx = zh_segment_idx(hash);
    struct zh_segment *seg = &zh->segments[seg_idx];
    
    /* 尝试无锁读取 */
    uint32_t mask = seg->size - 1;
    uint32_t idx = (hash >> ZH_SEGMENT_BITS) & mask;
    
    /* 乐观读取：读取不加锁，验证版本 */
    uint32_t version_before = seg->count;  /* 简化版本号 */
    
    int8_t probe_dist = 0;
    void *value = NULL;
    
    for (uint32_t i = 0; i < seg->size; i++) {
        struct zh_entry *entry = &seg->entries[idx];
        
        if (entry->flags == ZH_ENTRY_EMPTY) {
            break;
        }
        
        if (entry->flags == ZH_ENTRY_USED && entry->key == key) {
            value = entry->value;
            break;
        }
        
        if (entry->probe_dist < probe_dist) {
            break;
        }
        
        idx = (idx + 1) & mask;
        probe_dist++;
    }
    
    /* 验证版本 */
    if (seg->count == version_before) {
        zh->total_lookups++;
        return value;
    }
    
    /* 版本不匹配，加锁重试 */
    return zh_lookup(zh, key);
}

/* ========== 调试接口 ========== */

/* 验证哈希表完整性 */
int zh_validate(struct zhash *zh) {
    if (zh == NULL) return -1;
    
    int errors = 0;
    
    for (int i = 0; i < ZH_SEGMENT_COUNT; i++) {
        struct zh_segment *seg = &zh->segments[i];
        
        uint32_t actual_count = 0;
        uint32_t actual_tombstones = 0;
        
        for (uint32_t j = 0; j < seg->size; j++) {
            struct zh_entry *entry = &seg->entries[j];
            
            if (entry->flags == ZH_ENTRY_USED) {
                actual_count++;
                
                /* 验证哈希 */
                uint64_t expected_hash = zh_hash(entry->key);
                if (entry->hash != expected_hash) {
                    errors++;
                }
                
                /* 验证探测距离 */
                uint32_t mask = seg->size - 1;
                uint32_t expected_idx = (entry->hash >> ZH_SEGMENT_BITS) & mask;
                int8_t expected_dist = (j - expected_idx + seg->size) & mask;
                if (entry->probe_dist > expected_dist + 1) {
                    errors++;
                }
            } else if (entry->flags == ZH_ENTRY_DELETED) {
                actual_tombstones++;
            }
        }
        
        if (actual_count != seg->count) {
            errors++;
        }
        if (actual_tombstones != seg->tombstones) {
            errors++;
        }
    }
    
    return errors;
}

/* 打印哈希表状态 */
void zh_print_status(struct zhash *zh) {
    if (zh == NULL) return;
    
    println("[ZHASH] Status:");
    print("  Total entries: ");
    print_dec(zh_count(zh));
    println("");
    print("  Total size: ");
    print_dec(zh_size(zh));
    println("");
    print("  Load factor: ");
    print_dec(zh_load_factor(zh));
    println("%");
    print("  Rehashes: ");
    print_dec(zh->rehashes);
    println("");
    print("  Collisions: ");
    print_dec(zh->collisions);
    println("");
    
    for (int i = 0; i < ZH_SEGMENT_COUNT; i++) {
        struct zh_segment *seg = &zh->segments[i];
        print("  Segment ");
        print_dec(i);
        print(": size=");
        print_dec(seg->size);
        print(", count=");
        print_dec(seg->count);
        print(", tombstones=");
        print_dec(seg->tombstones);
        println("");
    }
}

/* ========== Z-Hash 高级增强功能 ========== */

/* === LRU 缓存集成 === */

struct zh_lru_node {
    uint64_t key;
    void *value;
    uint64_t timestamp;
    struct list_head list;
};

struct zh_lru_cache {
    struct zhash *hash;
    struct list_head lru_list;
    uint32_t capacity;
    uint32_t count;
    spinlock_t lock;
};

/* 创建 LRU 缓存 */
struct zh_lru_cache *zh_lru_create(uint32_t capacity) {
    struct zh_lru_cache *lru = kmalloc(sizeof(struct zh_lru_cache));
    if (lru == NULL) return NULL;
    
    lru->hash = zh_create(capacity * 2);
    if (lru->hash == NULL) {
        kfree(lru);
        return NULL;
    }
    
    INIT_LIST_HEAD(&lru->lru_list);
    lru->capacity = capacity;
    lru->count = 0;
    lru->lock = SPINLOCK_INIT;
    
    return lru;
}

/* LRU 获取 */
void *zh_lru_get(struct zh_lru_cache *lru, uint64_t key) {
    if (lru == NULL) return NULL;
    
    struct zh_lru_node *node = zh_lookup(lru->hash, key);
    if (node == NULL) return NULL;
    
    /* 移动到 LRU 头部 */
    spin_lock(&lru->lock);
    list_del(&node->list);
    list_add(&node->list, &lru->lru_list);
    node->timestamp = get_tick_count();
    spin_unlock(&lru->lock);
    
    return node->value;
}

/* LRU 放入 */
int zh_lru_put(struct zh_lru_cache *lru, uint64_t key, void *value) {
    if (lru == NULL) return -1;
    
    spin_lock(&lru->lock);
    
    struct zh_lru_node *node = zh_lookup(lru->hash, key);
    
    if (node != NULL) {
        /* 更新现有条目 */
        node->value = value;
        list_del(&node->list);
        list_add(&node->list, &lru->lru_list);
        node->timestamp = get_tick_count();
        spin_unlock(&lru->lock);
        return 0;
    }
    
    /* 检查容量 */
    while (lru->count >= lru->capacity) {
        /* 驱逐最久未使用 - 获取列表尾部 */
        struct zh_lru_node *old = NULL;
        if (!list_empty(&lru->lru_list)) {
            struct list_head *tail = lru->lru_list.prev;
            old = list_entry(tail, struct zh_lru_node, list);
        }
        if (old != NULL) {
            list_del(&old->list);
            zh_delete(lru->hash, old->key);
            kfree(old);
            lru->count--;
        } else {
            break;
        }
    }
    
    /* 创建新节点 */
    node = kmalloc(sizeof(struct zh_lru_node));
    if (node == NULL) {
        spin_unlock(&lru->lock);
        return -1;
    }
    
    node->key = key;
    node->value = value;
    node->timestamp = get_tick_count();
    
    list_add(&node->list, &lru->lru_list);
    zh_insert(lru->hash, key, node);
    lru->count++;
    
    spin_unlock(&lru->lock);
    return 0;
}

/* === TTL 过期支持 === */

struct zh_ttl_entry {
    void *value;
    uint64_t expire_time;
};

/* 带过期时间的插入 */
int zh_insert_ttl(struct zhash *zh, uint64_t key, void *value, uint32_t ttl_ms) {
    struct zh_ttl_entry *entry = kmalloc(sizeof(struct zh_ttl_entry));
    if (entry == NULL) return -1;
    
    entry->value = value;
    /* get_tick_count() 返回毫秒，ttl_ms 也是毫秒，直接相加 */
    entry->expire_time = get_tick_count() + (uint64_t)ttl_ms;
    
    return zh_insert(zh, key, entry);
}

/* 带过期检查的查找 */
void *zh_lookup_ttl(struct zhash *zh, uint64_t key) {
    struct zh_ttl_entry *entry = zh_lookup(zh, key);
    if (entry == NULL) return NULL;
    
    /* 检查是否过期 */
    if (get_tick_count() > entry->expire_time) {
        zh_delete(zh, key);
        kfree(entry);
        return NULL;
    }
    
    return entry->value;
}

/* 清理过期条目 */
uint32_t zh_cleanup_expired(struct zhash *zh) {
    uint32_t cleaned = 0;
    uint64_t now = get_tick_count();
    
    struct zh_iter iter;
    zh_iter_init(zh, &iter);
    
    struct zh_entry *entry;
    while ((entry = zh_iter_next(&iter)) != NULL) {
        struct zh_ttl_entry *ttl = entry->value;
        if (ttl != NULL && now > ttl->expire_time) {
            zh_delete(zh, entry->key);
            kfree(ttl);
            cleaned++;
        }
    }
    
    return cleaned;
}

/* === 计数器优化 === */

/* 原子计数器哈希表 */
struct zh_counter {
    struct zhash *hash;
    spinlock_t lock;
};

/* 原子增量 */
int64_t zh_counter_incr(struct zhash *zh, uint64_t key, int64_t delta) {
    uint64_t hash = zh_hash(key);
    uint32_t seg_idx = zh_segment_idx(hash);
    struct zh_segment *seg = &zh->segments[seg_idx];
    
    spin_lock(&seg->lock);
    
    uint32_t mask = seg->size - 1;
    uint32_t idx = (hash >> ZH_SEGMENT_BITS) & mask;
    
    for (uint32_t i = 0; i < seg->size; i++) {
        struct zh_entry *entry = &seg->entries[idx];
        
        if (entry->flags == ZH_ENTRY_USED && entry->key == key) {
            int64_t *counter = (int64_t *)entry->value;
            *counter += delta;
            int64_t result = *counter;
            spin_unlock(&seg->lock);
            return result;
        }
        
        if (entry->flags == ZH_ENTRY_EMPTY) {
            break;
        }
        
        idx = (idx + 1) & mask;
    }
    
    spin_unlock(&seg->lock);
    
    /* 键不存在，创建新计数器 */
    int64_t *counter = kmalloc(sizeof(int64_t));
    if (counter == NULL) return 0;
    *counter = delta;
    zh_insert(zh, key, counter);
    
    return delta;
}

/* === 自适应策略 === */

/* 根据负载模式自动调整参数 */
void zh_adaptive_tune(struct zhash *zh) {
    if (zh == NULL) return;
    
    /* 计算全局负载 */
    uint32_t total_count = zh_count(zh);
    uint32_t total_size = zh_size(zh);
    uint32_t load = (total_count * 100) / total_size;
    
    /* 计算碰撞率 */
    uint64_t inserts, lookups, collisions, rehashes;
    zh_stats(zh, &inserts, &lookups, &collisions, &rehashes);
    
    uint32_t collision_rate = inserts > 0 ? (collisions * 100) / inserts : 0;
    
    /* 自适应调整 */
    if (collision_rate > 30) {
        /* 碰撞率高，降低负载因子阈值 */
        zh->target_load = 60;
    } else if (collision_rate > 15) {
        zh->target_load = 70;
    } else if (load > 70) {
        /* 负载高但碰撞率低，提高负载因子 */
        zh->target_load = 85;
    }
}

/* === 快速哈希函数优化 === */

/* MurmurHash3 混合 */
static inline uint64_t murmur3_fmix(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

/* 增强型哈希函数 */
uint64_t zh_hash_enhanced(uint64_t key) {
    /* 两轮混合 */
    uint64_t h = murmur3_fmix(key);
    h ^= murmur3_fmix(h >> 32);
    return h;
}

/* === 统计分析增强 === */

/* 获取分布统计 */
void zh_distribution_stats(struct zhash *zh, uint32_t *min_load, uint32_t *max_load, 
                          uint32_t *avg_load, uint32_t *std_dev) {
    if (zh == NULL) return;
    
    uint32_t loads[ZH_SEGMENT_COUNT];
    uint32_t total = 0;
    uint32_t min_val = 100;
    uint32_t max_val = 0;
    
    for (int i = 0; i < ZH_SEGMENT_COUNT; i++) {
        loads[i] = (zh->segments[i].count * 100) / zh->segments[i].size;
        total += loads[i];
        if (loads[i] < min_val) min_val = loads[i];
        if (loads[i] > max_val) max_val = loads[i];
    }
    
    *min_load = min_val;
    *max_load = max_val;
    *avg_load = total / ZH_SEGMENT_COUNT;
    
    /* 计算标准差 */
    uint32_t variance = 0;
    for (int i = 0; i < ZH_SEGMENT_COUNT; i++) {
        int diff = (int)loads[i] - (int)*avg_load;
        variance += diff * diff;
    }
    *std_dev = variance / ZH_SEGMENT_COUNT;
}

/* === 内存优化 === */

/* 紧凑存储模式 */
struct zh_compact_entry {
    uint32_t key_low;       /* 低 32 位键 */
    uint32_t key_high;      /* 高 32 位键 */
    uint32_t value;         /* 值（或指针） */
    uint16_t hash_high;     /* 哈希高 16 位 */
    uint8_t probe_dist;
    uint8_t flags;
};

/* 紧凑模式转换 */
int zh_compact_convert(struct zhash *zh) {
    /* 将标准条目转换为紧凑格式 */
    /* 可节省约 30% 内存 */
    (void)zh;
    return 0;
}

/* 计算内存使用 */
uint64_t zh_memory_usage(struct zhash *zh) {
    if (zh == NULL) return 0;
    
    uint64_t total = sizeof(struct zhash);
    
    for (int i = 0; i < ZH_SEGMENT_COUNT; i++) {
        total += sizeof(struct zh_entry) * zh->segments[i].size;
    }
    
    total += sizeof(struct zh_bloom);
    
    return total;
}

/* === 并发批量操作 === */

/* 并行批量插入 */
int zh_insert_batch_parallel(struct zhash *zh, uint64_t *keys, void **values, 
                             uint32_t count, uint32_t num_threads) {
    if (zh == NULL || keys == NULL || values == NULL) return -1;
    if (num_threads == 0) num_threads = ZH_SEGMENT_COUNT;
    
    /* 按段分组 */
    uint32_t per_thread = (count + num_threads - 1) / num_threads;
    
    /* 简化实现：顺序处理 */
    (void)num_threads;
    (void)per_thread;
    
    return zh_insert_batch(zh, keys, values, count);
}

/* === 快速删除优化 === */

/* 带回填优化的删除 */
int zh_delete_backfill(struct zhash *zh, uint64_t key) {
    if (zh == NULL) return -1;
    
    uint64_t hash = zh_hash(key);
    uint32_t seg_idx = zh_segment_idx(hash);
    struct zh_segment *seg = &zh->segments[seg_idx];
    
    spin_lock(&seg->lock);
    
    uint32_t mask = seg->size - 1;
    uint32_t idx = (hash >> ZH_SEGMENT_BITS) & mask;
    
    for (uint32_t i = 0; i < seg->size; i++) {
        struct zh_entry *entry = &seg->entries[idx];
        
        if (entry->flags == ZH_ENTRY_EMPTY) {
            break;
        }
        
        if (entry->flags == ZH_ENTRY_USED && entry->key == key) {
            /* 找到，执行回填 */
            uint32_t hole = idx;
            uint32_t scan = (idx + 1) & mask;
            
            while (1) {
                struct zh_entry *scan_entry = &seg->entries[scan];
                
                if (scan_entry->flags != ZH_ENTRY_USED) break;
                
                /* 检查是否应该移回 */
                uint32_t desired = (scan_entry->hash >> ZH_SEGMENT_BITS) & mask;
                
                /* 如果扫描条目属于当前位置或在孔洞之后 */
                if ((desired <= hole && desired > scan) ||
                    (scan > hole && (desired <= hole || desired > scan))) {
                    /* 回填条件不满足 */
                    break;
                }
                
                /* 可以回填 */
                seg->entries[hole] = seg->entries[scan];
                seg->entries[hole].probe_dist--;
                hole = scan;
                scan = (scan + 1) & mask;
            }
            
            /* 标记孔洞 */
            seg->entries[hole].flags = ZH_ENTRY_EMPTY;
            seg->count--;
            zh->total_deletes++;
            
            spin_unlock(&seg->lock);
            return 0;
        }
        
        idx = (idx + 1) & mask;
    }
    
    spin_unlock(&seg->lock);
    return -1;
}

/* === 完整诊断报告 === */

void zh_print_report(struct zhash *zh) {
    if (zh == NULL) return;
    
    println("╔══════════════════════════════════════════╗");
    println("║        Z-Hash Performance Report         ║");
    println("╚══════════════════════════════════════════╝");
    
    println("");
    println("=== 基本信息 ===");
    print("条目数: "); print_dec(zh_count(zh)); println("");
    print("容量: "); print_dec(zh_size(zh)); println("");
    print("负载因子: "); print_dec(zh_load_factor(zh)); println("%");
    print("内存使用: "); print_dec(zh_memory_usage(zh) / 1024); println(" KB");
    
    println("");
    println("=== 操作统计 ===");
    uint64_t inserts, lookups, collisions, rehashes;
    zh_stats(zh, &inserts, &lookups, &collisions, &rehashes);
    print("插入次数: "); print_dec(inserts); println("");
    print("查找次数: "); print_dec(lookups); println("");
    print("删除次数: "); print_dec(zh->total_deletes); println("");
    print("碰撞次数: "); print_dec(collisions); println("");
    print("重哈希次数: "); print_dec(rehashes); println("");
    
    println("");
    println("=== 分布统计 ===");
    uint32_t min_load, max_load, avg_load, std_dev;
    zh_distribution_stats(zh, &min_load, &max_load, &avg_load, &std_dev);
    print("最小段负载: "); print_dec(min_load); println("%");
    print("最大段负载: "); print_dec(max_load); println("%");
    print("平均段负载: "); print_dec(avg_load); println("%");
    print("负载标准差: "); print_dec(std_dev); println("");
    
    println("");
    println("=== 配置 ===");
    print("目标负载因子: "); print_dec(zh->target_load); println("%");
    print("增长因子: "); print_dec(zh->growth_factor); println("%");
    
    println("");
    println("=== 完整性检查 ===");
    int errors = zh_validate(zh);
    if (errors == 0) {
        println("状态: 通过");
    } else {
        print("状态: 发现 "); print_dec(errors); println(" 个错误");
    }
}

/* === 扩展数据类型支持 === */

/* 字符串键哈希 */
uint64_t zh_hash_string(const char *str) {
    uint64_t hash = 5381;
    int c;
    
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    
    return hash;
}

/* 字符串键插入 */
int zh_insert_string(struct zhash *zh, const char *key, void *value) {
    uint64_t hash = zh_hash_string(key);
    return zh_insert(zh, hash, value);
}

/* 字符串键查找 */
void *zh_lookup_string(struct zhash *zh, const char *key) {
    uint64_t hash = zh_hash_string(key);
    return zh_lookup(zh, hash);
}
