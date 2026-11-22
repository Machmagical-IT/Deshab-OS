/*
 * Zratro OS - Z-Alloc Implementation
 * 
 * 原创高性能内存分配器实现
 */

#include "zalloc.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"
#include "pmm.h"

/* ========== 伙伴系统实现 ========== */

/* 初始化区域 */
static int zone_init(struct za_zone *zone, uint64_t start, uint64_t size) {
    zone->start = start;
    zone->end = start + size;
    zone->size = size;
    zone->free_size = size;
    
    for (int i = 0; i < ZA_NUM_ORDERS; i++) {
        INIT_LIST_HEAD(&zone->free_lists[i]);
        zone->free_counts[i] = 0;
    }
    
    /* 分配位图 */
    uint64_t total_blocks = size / ZA_PAGE_SIZE;
    zone->bitmap_size = (total_blocks + 63) / 64;
    zone->bitmap = kmalloc(zone->bitmap_size * sizeof(uint64_t));
    
    if (zone->bitmap == NULL) {
        return -1;
    }
    
    /* 清零位图 */
    for (uint32_t i = 0; i < zone->bitmap_size; i++) {
        zone->bitmap[i] = 0;
    }
    
    zone->lock = SPINLOCK_INIT;
    
    /* 添加初始空闲块 */
    uint32_t order = ZA_MAX_ORDER;
    uint64_t block_size = ZA_PAGE_SIZE << order;
    uint64_t addr = start;
    
    while (addr + block_size <= start + size) {
        struct za_block *block = kmalloc(sizeof(struct za_block));
        if (block == NULL) break;
        
        block->addr = addr;
        block->order = order;
        block->flags = ZA_BLOCK_FREE;
        
        list_add(&block->list, &zone->free_lists[order]);
        zone->free_counts[order]++;
        
        addr += block_size;
    }
    
    return 0;
}

/* 分裂块 */
static void split_block(struct za_zone *zone, struct za_block *block) {
    if (block->order == 0) return;
    
    block->order--;
    block->flags = ZA_BLOCK_SPLIT;
    
    /* 创建伙伴块 */
    struct za_block *buddy = kmalloc(sizeof(struct za_block));
    if (buddy == NULL) return;
    
    buddy->addr = block->addr + (ZA_PAGE_SIZE << block->order);
    buddy->order = block->order;
    buddy->flags = ZA_BLOCK_FREE;
    
    /* 添加到空闲链表 */
    list_add(&buddy->list, &zone->free_lists[buddy->order]);
    zone->free_counts[buddy->order]++;
}

/* 合并块 */
static struct za_block *merge_blocks(struct za_zone *zone, 
                                     struct za_block *block, 
                                     struct za_block *buddy) {
    /* 从空闲链表移除 */
    list_del(&buddy->list);
    zone->free_counts[buddy->order]--;
    
    /* 更新块 */
    if (buddy->addr < block->addr) {
        block->addr = buddy->addr;
    }
    block->order++;
    
    kfree(buddy);
    
    return block;
}

/* 查找伙伴块 */
static struct za_block *find_buddy(struct za_zone *zone, 
                                    struct za_block *block) {
    uint64_t buddy_addr = za_buddy_addr(block->addr, block->order);
    
    /* 在对应阶的空闲链表中查找 */
    struct za_block *b;
    list_for_each_entry(b, &zone->free_lists[block->order], list) {
        if (b->addr == buddy_addr && b->flags == ZA_BLOCK_FREE) {
            return b;
        }
    }
    
    return NULL;
}

/* 伙伴分配 */
static void *buddy_alloc(struct za_zone *zone, uint32_t order) {
    spin_lock(&zone->lock);
    
    /* 从请求的阶开始查找 */
    for (uint32_t o = order; o < ZA_NUM_ORDERS; o++) {
        if (!list_empty(&zone->free_lists[o])) {
            struct za_block *block = list_first_entry(
                &zone->free_lists[o], struct za_block, list);
            
            list_del(&block->list);
            zone->free_counts[o]--;
            
            /* 分裂直到所需大小 */
            while (block->order > order) {
                split_block(zone, block);
            }
            
            block->flags = ZA_BLOCK_USED;
            zone->free_size -= ZA_PAGE_SIZE << order;
            
            spin_unlock(&zone->lock);
            return (void *)block->addr;
        }
    }
    
    spin_unlock(&zone->lock);
    return NULL;
}

/* 伙伴释放 */
static void buddy_free(struct za_zone *zone, uint64_t addr, uint32_t order) {
    spin_lock(&zone->lock);
    
    struct za_block *block = kmalloc(sizeof(struct za_block));
    if (block == NULL) {
        spin_unlock(&zone->lock);
        return;
    }
    
    block->addr = addr;
    block->order = order;
    block->flags = ZA_BLOCK_FREE;
    
    zone->free_size += ZA_PAGE_SIZE << order;
    
    /* 尝试合并 */
    while (block->order < ZA_MAX_ORDER) {
        struct za_block *buddy = find_buddy(zone, block);
        if (buddy == NULL) break;
        
        block = merge_blocks(zone, block, buddy);
    }
    
    /* 添加到空闲链表 */
    list_add(&block->list, &zone->free_lists[block->order]);
    zone->free_counts[block->order]++;
    
    spin_unlock(&zone->lock);
}

/* ========== 线程本地缓存实现 ========== */

static void thread_cache_init(struct za_thread_cache *cache, int cpu_id) {
    for (int i = 0; i < ZA_NUM_SIZE_CLASSES; i++) {
        cache->counts[i] = 0;
        cache->limits[i] = 32;  /* 默认限制 */
        
        for (int j = 0; j < ZA_CACHE_SIZE; j++) {
            cache->slots[i][j].ptr = NULL;
            cache->slots[i][j].size = 0;
            cache->slots[i][j].flags = 0;
        }
    }
    
    cache->hits = 0;
    cache->misses = 0;
    cache->evictions = 0;
    cache->cpu_id = cpu_id;
}

/* 从缓存分配 */
static void *cache_alloc(struct za_thread_cache *cache, uint32_t size_class) {
    if (cache->counts[size_class] == 0) {
        return NULL;
    }
    
    cache->counts[size_class]--;
    void *ptr = cache->slots[size_class][cache->counts[size_class]].ptr;
    cache->hits++;
    
    return ptr;
}

/* 归还到缓存 */
static int cache_free(struct za_thread_cache *cache, void *ptr, 
                      uint32_t size_class, uint32_t size) {
    if (cache->counts[size_class] >= cache->limits[size_class]) {
        cache->evictions++;
        return -1;  /* 缓存已满 */
    }
    
    cache->slots[size_class][cache->counts[size_class]].ptr = ptr;
    cache->slots[size_class][cache->counts[size_class]].size = size;
    cache->counts[size_class]++;
    
    return 0;
}

/* 刷新缓存 */
static void cache_flush(struct za_thread_cache *cache, struct za_zone *zone) {
    for (int i = 0; i < ZA_NUM_SIZE_CLASSES; i++) {
        for (uint32_t j = 0; j < cache->counts[i]; j++) {
            void *ptr = cache->slots[i][j].ptr;
            uint32_t size = cache->slots[i][j].size;
            
            /* 释放到伙伴系统 */
            uint32_t order = za_order_for_size(size);
            buddy_free(zone, (uint64_t)ptr, order);
        }
        cache->counts[i] = 0;
    }
}

/* ========== Slab 实现 ========== */

static struct za_slab *slab_create(struct za_slab_cache *cache) {
    /* 分配 slab 页面 */
    void *base = kmalloc(ZA_PAGE_SIZE);
    if (base == NULL) return NULL;
    
    struct za_slab *slab = kmalloc(sizeof(struct za_slab));
    if (slab == NULL) {
        kfree(base);
        return NULL;
    }
    
    slab->base = base;
    slab->obj_size = cache->obj_size;
    slab->obj_count = ZA_PAGE_SIZE / cache->obj_size;
    slab->free_count = slab->obj_count;
    
    /* 分配空闲位图 */
    slab->map_size = (slab->obj_count + 7) / 8;
    slab->free_map = kmalloc(slab->map_size);
    
    if (slab->free_map == NULL) {
        kfree(slab);
        kfree(base);
        return NULL;
    }
    
    /* 所有对象标记为空闲 */
    for (uint32_t i = 0; i < slab->map_size; i++) {
        slab->free_map[i] = 0;
    }
    
    slab->cache = cache;
    
    return slab;
}

static void *slab_alloc_obj(struct za_slab *slab) {
    if (slab->free_count == 0) return NULL;
    
    /* 查找空闲对象 */
    for (uint32_t i = 0; i < slab->obj_count; i++) {
        uint32_t byte = i / 8;
        uint32_t bit = i % 8;
        
        if (!(slab->free_map[byte] & (1 << bit))) {
            /* 标记为已用 */
            slab->free_map[byte] |= (1 << bit);
            slab->free_count--;
            
            return (void *)((uint64_t)slab->base + i * slab->obj_size);
        }
    }
    
    return NULL;
}

static void slab_free_obj(struct za_slab *slab, void *obj) {
    uint32_t offset = (uint64_t)obj - (uint64_t)slab->base;
    uint32_t index = offset / slab->obj_size;
    
    uint32_t byte = index / 8;
    uint32_t bit = index % 8;
    
    /* 标记为空闲 */
    slab->free_map[byte] &= ~(1 << bit);
    slab->free_count++;
}

/* ========== 主 API 实现 ========== */

int zalloc_init(struct zalloc *za, uint64_t mem_start, uint64_t mem_size) {
    /* 分配区域 */
    za->zones = kmalloc(sizeof(struct za_zone));
    if (za->zones == NULL) return -1;
    
    za->zone_count = 1;
    
    if (zone_init(&za->zones[0], mem_start, mem_size) != 0) {
        kfree(za->zones);
        return -1;
    }
    
    /* 初始化缓存池 */
    za->cache_pool.caches = kmalloc(sizeof(struct za_thread_cache) * ZA_NUM_CACHES);
    if (za->cache_pool.caches == NULL) {
        kfree(za->zones);
        return -1;
    }
    
    za->cache_pool.count = ZA_NUM_CACHES;
    za->cache_pool.lock = SPINLOCK_INIT;
    
    for (int i = 0; i < ZA_NUM_CACHES; i++) {
        thread_cache_init(&za->cache_pool.caches[i], i);
    }
    
    /* 初始化 Slab 缓存 */
    for (int i = 0; i < ZA_NUM_SIZE_CLASSES; i++) {
        za->slab_caches[i].obj_size = za_size_classes[i];
        za->slab_caches[i].flags = 0;
        za->slab_caches[i].slabs_total = 0;
        za->slab_caches[i].slabs_active = 0;
        za->slab_caches[i].lock = SPINLOCK_INIT;
        
        INIT_LIST_HEAD(&za->slab_caches[i].full);
        INIT_LIST_HEAD(&za->slab_caches[i].partial);
        INIT_LIST_HEAD(&za->slab_caches[i].empty);
    }
    
    /* 初始化防碎片整理 */
    za->defrag.scan_interval = 10000000;  /* 10ms */
    za->defrag.last_scan = 0;
    za->defrag.fragmentation = 0;
    za->defrag.threshold = 50;
    za->defrag.pages_moved = 0;
    za->defrag.blocks_merged = 0;
    
    /* 统计 */
    za->total_allocs = 0;
    za->total_frees = 0;
    za->total_bytes_alloc = 0;
    za->total_bytes_free = 0;
    za->current_usage = 0;
    za->peak_usage = 0;
    
    za->lock = SPINLOCK_INIT;
    
    println("[ZALLOC] Initialized");
    return 0;
}

void zalloc_destroy(struct zalloc *za) {
    if (za == NULL) return;
    
    /* 刷新所有缓存 */
    for (int i = 0; i < za->cache_pool.count; i++) {
        cache_flush(&za->cache_pool.caches[i], &za->zones[0]);
    }
    
    kfree(za->cache_pool.caches);
    kfree(za->zones[0].bitmap);
    kfree(za->zones);
}

/* 获取线程缓存 */
struct za_thread_cache *za_get_thread_cache(struct zalloc *za) {
    int cpu = smp_processor_id();
    if (cpu >= 0 && cpu < (int)za->cache_pool.count) {
        return &za->cache_pool.caches[cpu];
    }
    return NULL;
}

/* 分配函数 */
void *zalloc_alloc(struct zalloc *za, size_t size) {
    if (za == NULL || size == 0) return NULL;
    
    /* 对齐大小 */
    size = za_align_up(size, ZA_ALIGN);
    
    /* 检查小对象 */
    if (size <= ZA_MAX_SMALL) {
        uint32_t sc = za_get_size_class(size);
        
        /* 尝试从线程缓存分配 */
        struct za_thread_cache *cache = za_get_thread_cache(za);
        if (cache != NULL) {
            void *ptr = cache_alloc(cache, sc);
            if (ptr != NULL) {
                za->total_allocs++;
                za->total_bytes_alloc += size;
                za->current_usage += size;
                if (za->current_usage > za->peak_usage) {
                    za->peak_usage = za->current_usage;
                }
                return ptr;
            }
        }
        
        /* 从 Slab 分配 */
        struct za_slab_cache *slab_cache = &za->slab_caches[sc];
        spin_lock(&slab_cache->lock);
        
        /* 查找部分 slab */
        if (!list_empty(&slab_cache->partial)) {
            struct za_slab *slab = list_first_entry(
                &slab_cache->partial, struct za_slab, list);
            
            void *ptr = slab_alloc_obj(slab);
            
            if (slab->free_count == 0) {
                list_del(&slab->list);
                list_add(&slab->list, &slab_cache->full);
            }
            
            spin_unlock(&slab_cache->lock);
            
            if (ptr != NULL) {
                za->total_allocs++;
                za->total_bytes_alloc += size;
                za->current_usage += size;
                if (za->current_usage > za->peak_usage) {
                    za->peak_usage = za->current_usage;
                }
            }
            
            return ptr;
        }
        
        /* 创建新 slab */
        struct za_slab *slab = slab_create(slab_cache);
        if (slab == NULL) {
            spin_unlock(&slab_cache->lock);
            return NULL;
        }
        
        void *ptr = slab_alloc_obj(slab);
        list_add(&slab->list, &slab_cache->partial);
        slab_cache->slabs_total++;
        slab_cache->slabs_active++;
        
        spin_unlock(&slab_cache->lock);
        
        if (ptr != NULL) {
            za->total_allocs++;
            za->total_bytes_alloc += size;
            za->current_usage += size;
            if (za->current_usage > za->peak_usage) {
                za->peak_usage = za->current_usage;
            }
        }
        
        return ptr;
    }
    
    /* 大对象：使用伙伴系统 */
    uint32_t order = za_order_for_size(size);
    void *ptr = buddy_alloc(&za->zones[0], order);
    
    if (ptr != NULL) {
        za->total_allocs++;
        za->total_bytes_alloc += size;
        za->current_usage += size;
        if (za->current_usage > za->peak_usage) {
            za->peak_usage = za->current_usage;
        }
    }
    
    return ptr;
}

/* 带对齐的分配 */
void *zalloc_alloc_aligned(struct zalloc *za, size_t size, size_t align) {
    if (za == NULL || size == 0) return NULL;
    
    /* 确保对齐是 2 的幂 */
    if ((align & (align - 1)) != 0) {
        align = ZA_ALIGN;
    }
    
    size = za_align_up(size, align);
    
    /* 使用伙伴系统分配大块 */
    uint32_t order = za_order_for_size(size);
    void *ptr = buddy_alloc(&za->zones[0], order);
    
    if (ptr != NULL && za_is_aligned(ptr, align)) {
        za->total_allocs++;
        za->total_bytes_alloc += size;
        return ptr;
    }
    
    /* 如果不对齐，重新分配更大的块 */
    if (ptr != NULL) {
        buddy_free(&za->zones[0], (uint64_t)ptr, order);
    }
    
    /* 分配带对齐的块 */
    order++;
    ptr = buddy_alloc(&za->zones[0], order);
    
    if (ptr != NULL) {
        za->total_allocs++;
        za->total_bytes_alloc += size;
    }
    
    return ptr;
}

/* 带标志的分配 */
void *zalloc_alloc_flags(struct zalloc *za, size_t size, uint32_t flags) {
    void *ptr = zalloc_alloc(za, size);
    
    if (ptr != NULL && (flags & ZA_FLAG_ZERO)) {
        /* 清零 */
        for (size_t i = 0; i < size; i++) {
            ((uint8_t *)ptr)[i] = 0;
        }
    }
    
    return ptr;
}

/* 释放函数 */
void zalloc_free(struct zalloc *za, void *ptr) {
    if (za == NULL || ptr == NULL) return;
    
    /* 简化实现：尝试放回缓存或使用伙伴系统 */
    /* 实际实现需要追踪分配大小 */
    
    za->total_frees++;
    za->current_usage -= 16;  /* 简化：假设最小块 */
    
    /* 尝试放回缓存 */
    struct za_thread_cache *cache = za_get_thread_cache(za);
    if (cache != NULL) {
        /* 找到合适的大小类 */
        for (int i = 0; i < ZA_NUM_SIZE_CLASSES; i++) {
            if (cache_free(cache, ptr, i, za_size_classes[i]) == 0) {
                return;
            }
        }
    }
    
    /* 放回伙伴系统 */
    buddy_free(&za->zones[0], (uint64_t)ptr, 0);
}

/* 重分配 */
void *zalloc_realloc(struct zalloc *za, void *ptr, size_t new_size) {
    if (za == NULL) return NULL;
    
    if (ptr == NULL) {
        return zalloc_alloc(za, new_size);
    }
    
    if (new_size == 0) {
        zalloc_free(za, ptr);
        return NULL;
    }
    
    /* 简化实现：分配新块并复制 */
    void *new_ptr = zalloc_alloc(za, new_size);
    if (new_ptr == NULL) return NULL;
    
    /* 复制数据 (无法知道原大小，使用 new_size) */
    for (size_t i = 0; i < new_size; i++) {
        ((uint8_t *)new_ptr)[i] = ((uint8_t *)ptr)[i];
    }
    
    zalloc_free(za, ptr);
    return new_ptr;
}

/* ========== 统计函数 ========== */

uint64_t zalloc_usage(struct zalloc *za) {
    return za->current_usage;
}

uint64_t zalloc_free_space(struct zalloc *za) {
    uint64_t total = 0;
    for (int i = 0; i < za->zone_count; i++) {
        total += za->zones[i].free_size;
    }
    return total;
}

void zalloc_stats(struct zalloc *za, uint64_t *allocs, uint64_t *frees,
                  uint64_t *usage, uint64_t *peak) {
    if (allocs) *allocs = za->total_allocs;
    if (frees) *frees = za->total_frees;
    if (usage) *usage = za->current_usage;
    if (peak) *peak = za->peak_usage;
}

/* ========== 高级特性 ========== */

/* 防碎片整理 */
void zalloc_defrag_run(struct zalloc *za) {
    /* 更新碎片度 */
    uint64_t total = 0;
    uint64_t fragmented = 0;
    
    for (int i = 0; i < za->zone_count; i++) {
        struct za_zone *zone = &za->zones[i];
        
        /* 检查低阶空闲块 */
        for (int o = 0; o < ZA_MAX_ORDER; o++) {
            total += zone->free_counts[o];
            if (o < ZA_MAX_ORDER / 2) {
                fragmented += zone->free_counts[o];
            }
        }
    }
    
    if (total > 0) {
        za->defrag.fragmentation = (fragmented * 100) / total;
    }
    
    /* 如果碎片度高于阈值，尝试合并 */
    if (za->defrag.fragmentation > za->defrag.threshold) {
        /* 合并低阶块 */
        for (int i = 0; i < za->zone_count; i++) {
            struct za_zone *zone = &za->zones[i];
            
            spin_lock(&zone->lock);
            
            for (int o = 0; o < ZA_MAX_ORDER - 1; o++) {
                while (zone->free_counts[o] >= 2) {
                    /* 尝试合并两个块 */
                    struct za_block *b1 = list_first_entry(
                        &zone->free_lists[o], struct za_block, list);
                    struct za_block *b2 = find_buddy(zone, b1);
                    
                    if (b2 != NULL) {
                        b1 = merge_blocks(zone, b1, b2);
                        list_add(&b1->list, &zone->free_lists[b1->order]);
                        zone->free_counts[b1->order]++;
                        za->defrag.blocks_merged++;
                    } else {
                        break;
                    }
                }
            }
            
            spin_unlock(&zone->lock);
        }
    }
    
    za->defrag.last_scan = get_tick_count();
}

uint32_t zalloc_fragmentation(struct zalloc *za) {
    return za->defrag.fragmentation;
}

void zalloc_set_cache_limit(struct zalloc *za, uint32_t size_class, uint32_t limit) {
    if (za != NULL && size_class < ZA_NUM_SIZE_CLASSES) {
        for (int i = 0; i < za->cache_pool.count; i++) {
            za->cache_pool.caches[i].limits[size_class] = limit;
        }
    }
}

void zalloc_set_defrag_threshold(struct zalloc *za, uint32_t threshold) {
    if (za != NULL) {
        za->defrag.threshold = threshold;
    }
}

void za_flush_thread_cache(struct za_thread_cache *cache) {
    /* 简化：不实际释放，因为需要 zone */
    for (int i = 0; i < ZA_NUM_SIZE_CLASSES; i++) {
        cache->counts[i] = 0;
    }
    cache->evictions = 0;
}

/* Slab API */
struct za_slab_cache *za_slab_create(struct zalloc *za, size_t obj_size, uint32_t flags) {
    /* 查找合适的缓存 */
    uint32_t sc = za_get_size_class(obj_size);
    if (sc < ZA_NUM_SIZE_CLASSES) {
        za->slab_caches[sc].flags = flags;
        return &za->slab_caches[sc];
    }
    return NULL;
}

void za_slab_destroy(struct zalloc *za, struct za_slab_cache *cache) {
    (void)za;
    (void)cache;
}

void *za_slab_alloc(struct za_slab_cache *cache) {
    spin_lock(&cache->lock);
    
    if (!list_empty(&cache->partial)) {
        struct za_slab *slab = list_first_entry(&cache->partial, struct za_slab, list);
        void *ptr = slab_alloc_obj(slab);
        
        if (slab->free_count == 0) {
            list_del(&slab->list);
            list_add(&slab->list, &cache->full);
        }
        
        spin_unlock(&cache->lock);
        return ptr;
    }
    
    spin_unlock(&cache->lock);
    return NULL;
}

void za_slab_free(struct za_slab_cache *cache, void *obj) {
    spin_lock(&cache->lock);
    
    /* 在所有 slab 中查找 */
    struct za_slab *slab;
    list_for_each_entry(slab, &cache->full, list) {
        if ((uint64_t)obj >= (uint64_t)slab->base &&
            (uint64_t)obj < (uint64_t)slab->base + ZA_PAGE_SIZE) {
            slab_free_obj(slab, obj);
            
            if (slab->free_count == 1) {
                list_del(&slab->list);
                list_add(&slab->list, &cache->partial);
            }
            
            spin_unlock(&cache->lock);
            return;
        }
    }
    
    list_for_each_entry(slab, &cache->partial, list) {
        if ((uint64_t)obj >= (uint64_t)slab->base &&
            (uint64_t)obj < (uint64_t)slab->base + ZA_PAGE_SIZE) {
            slab_free_obj(slab, obj);
            spin_unlock(&cache->lock);
            return;
        }
    }
    
    spin_unlock(&cache->lock);
}

/* 批量分配 */
int zalloc_alloc_batch(struct zalloc *za, size_t size, void **ptrs, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        ptrs[i] = zalloc_alloc(za, size);
        if (ptrs[i] == NULL) {
            /* 回滚 */
            for (uint32_t j = 0; j < i; j++) {
                zalloc_free(za, ptrs[j]);
            }
            return -1;
        }
    }
    return 0;
}

void zalloc_free_batch(struct zalloc *za, void **ptrs, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        zalloc_free(za, ptrs[i]);
    }
}

/* ========== 内存池实现 ========== */

/* 内存池结构 */
struct za_mempool {
    char name[32];
    size_t obj_size;
    uint32_t min_objs;
    uint32_t max_objs;
    uint32_t curr_objs;
    void **elements;
    uint32_t capacity;
    spinlock_t lock;
    void *(*alloc_fn)(size_t);
    void (*free_fn)(void *);
};

#define ZA_MAX_MEMPOOLS    16

static struct za_mempool mempools[ZA_MAX_MEMPOOLS];
static uint32_t mempool_count = 0;
static spinlock_t mempool_lock = SPINLOCK_INIT;

/* 创建内存池 */
struct za_mempool *za_mempool_create(const char *name, size_t obj_size,
                                     uint32_t min_objs, uint32_t max_objs,
                                     void *(*alloc_fn)(size_t),
                                     void (*free_fn)(void *)) {
    spin_lock(&mempool_lock);
    
    if (mempool_count >= ZA_MAX_MEMPOOLS) {
        spin_unlock(&mempool_lock);
        return NULL;
    }
    
    struct za_mempool *pool = &mempools[mempool_count];
    
    /* 复制名称 */
    for (int i = 0; i < 31 && name[i]; i++) {
        pool->name[i] = name[i];
    }
    pool->name[31] = '\0';
    
    pool->obj_size = obj_size;
    pool->min_objs = min_objs;
    pool->max_objs = max_objs;
    pool->curr_objs = 0;
    pool->capacity = max_objs;
    pool->alloc_fn = alloc_fn ? alloc_fn : kmalloc;
    pool->free_fn = free_fn ? free_fn : kfree;
    pool->lock = SPINLOCK_INIT;
    
    /* 分配元素数组 */
    pool->elements = kmalloc(sizeof(void *) * max_objs);
    if (pool->elements == NULL) {
        spin_unlock(&mempool_lock);
        return NULL;
    }
    
    /* 预分配最小数量的对象 */
    for (uint32_t i = 0; i < min_objs; i++) {
        pool->elements[pool->curr_objs++] = pool->alloc_fn(obj_size);
    }
    
    mempool_count++;
    spin_unlock(&mempool_lock);
    
    return pool;
}

/* 从内存池分配 */
void *za_mempool_alloc(struct za_mempool *pool) {
    if (pool == NULL) return NULL;
    
    spin_lock(&pool->lock);
    
    void *obj;
    
    if (pool->curr_objs > 0) {
        /* 从池中取出 */
        obj = pool->elements[--pool->curr_objs];
    } else if (pool->curr_objs < pool->max_objs) {
        /* 动态分配新对象 */
        obj = pool->alloc_fn(pool->obj_size);
    } else {
        obj = NULL;
    }
    
    spin_unlock(&pool->lock);
    return obj;
}

/* 归还到内存池 */
void za_mempool_free(struct za_mempool *pool, void *obj) {
    if (pool == NULL || obj == NULL) return;
    
    spin_lock(&pool->lock);
    
    if (pool->curr_objs < pool->capacity) {
        /* 放回池中 */
        pool->elements[pool->curr_objs++] = obj;
    } else {
        /* 超过容量，释放 */
        pool->free_fn(obj);
    }
    
    spin_unlock(&pool->lock);
}

/* 销毁内存池 */
void za_mempool_destroy(struct za_mempool *pool) {
    if (pool == NULL) return;
    
    spin_lock(&pool->lock);
    
    /* 释放所有对象 */
    for (uint32_t i = 0; i < pool->curr_objs; i++) {
        pool->free_fn(pool->elements[i]);
    }
    
    kfree(pool->elements);
    pool->elements = NULL;
    pool->curr_objs = 0;
    
    spin_unlock(&pool->lock);
}

/* ========== 对象缓存 (kmem_cache 风格) ========== */

/* 对象缓存结构 */
struct za_objcache {
    char name[32];
    size_t obj_size;
    size_t align;
    uint32_t flags;
    
    struct za_slab_cache slab_cache;
    
    void (*ctor)(void *);           /* 构造函数 */
    void (*dtor)(void *);           /* 析构函数 */
    
    /* 统计 */
    uint64_t allocs;
    uint64_t frees;
    uint32_t active_objs;
};

#define ZA_MAX_OBJCACHES    32

static struct za_objcache objcaches[ZA_MAX_OBJCACHES];
static uint32_t objcache_count = 0;
static spinlock_t objcache_lock = SPINLOCK_INIT;

/* 创建对象缓存 */
struct za_objcache *za_objcache_create(const char *name, size_t obj_size,
                                       size_t align, uint32_t flags,
                                       void (*ctor)(void *),
                                       void (*dtor)(void *)) {
    spin_lock(&objcache_lock);
    
    if (objcache_count >= ZA_MAX_OBJCACHES) {
        spin_unlock(&objcache_lock);
        return NULL;
    }
    
    struct za_objcache *cache = &objcaches[objcache_count];
    
    for (int i = 0; i < 31 && name[i]; i++) {
        cache->name[i] = name[i];
    }
    cache->name[31] = '\0';
    
    cache->obj_size = obj_size;
    cache->align = align ? align : ZA_ALIGN;
    cache->flags = flags;
    cache->ctor = ctor;
    cache->dtor = dtor;
    cache->allocs = 0;
    cache->frees = 0;
    cache->active_objs = 0;
    
    /* 初始化 slab 缓存 */
    cache->slab_cache.obj_size = obj_size;
    cache->slab_cache.flags = flags;
    cache->slab_cache.slabs_total = 0;
    cache->slab_cache.slabs_active = 0;
    cache->slab_cache.lock = SPINLOCK_INIT;
    INIT_LIST_HEAD(&cache->slab_cache.full);
    INIT_LIST_HEAD(&cache->slab_cache.partial);
    INIT_LIST_HEAD(&cache->slab_cache.empty);
    
    objcache_count++;
    spin_unlock(&objcache_lock);
    
    return cache;
}

/* 从对象缓存分配 */
void *za_objcache_alloc(struct za_objcache *cache) {
    if (cache == NULL) return NULL;
    
    void *obj = za_slab_alloc(&cache->slab_cache);
    
    if (obj != NULL) {
        cache->allocs++;
        cache->active_objs++;
        
        /* 调用构造函数 */
        if (cache->ctor != NULL) {
            cache->ctor(obj);
        }
    }
    
    return obj;
}

/* 归还到对象缓存 */
void za_objcache_free(struct za_objcache *cache, void *obj) {
    if (cache == NULL || obj == NULL) return;
    
    /* 调用析构函数 */
    if (cache->dtor != NULL) {
        cache->dtor(obj);
    }
    
    za_slab_free(&cache->slab_cache, obj);
    
    cache->frees++;
    cache->active_objs--;
}

/* 销毁对象缓存 */
void za_objcache_destroy(struct za_objcache *cache) {
    if (cache == NULL) return;
    
    /* 释放所有 slab */
    struct za_slab *slab, *next;
    
    list_for_each_entry_safe(slab, next, &cache->slab_cache.full, list) {
        kfree(slab->free_map);
        kfree(slab->base);
        kfree(slab);
    }
    
    list_for_each_entry_safe(slab, next, &cache->slab_cache.partial, list) {
        kfree(slab->free_map);
        kfree(slab->base);
        kfree(slab);
    }
    
    list_for_each_entry_safe(slab, next, &cache->slab_cache.empty, list) {
        kfree(slab->free_map);
        kfree(slab->base);
        kfree(slab);
    }
}

/* 收缩对象缓存 */
int za_objcache_shrink(struct za_objcache *cache) {
    if (cache == NULL) return 0;
    
    int freed = 0;
    
    spin_lock(&cache->slab_cache.lock);
    
    struct za_slab *slab, *next;
    list_for_each_entry_safe(slab, next, &cache->slab_cache.empty, list) {
        list_del(&slab->list);
        kfree(slab->free_map);
        kfree(slab->base);
        kfree(slab);
        cache->slab_cache.slabs_total--;
        freed++;
    }
    
    spin_unlock(&cache->slab_cache.lock);
    
    return freed;
}

/* ========== NUMA 感知分配 ========== */

/* NUMA 节点信息 */
struct za_numa_node {
    uint32_t node_id;
    uint64_t mem_start;
    uint64_t mem_size;
    uint64_t free_size;
    struct za_zone *zones;
    uint32_t zone_count;
    struct za_thread_cache *caches;
    uint32_t cache_count;
};

#define ZA_MAX_NUMA_NODES    8

static struct za_numa_node numa_nodes[ZA_MAX_NUMA_NODES];
static uint32_t numa_node_count = 0;

/* 设置 NUMA 节点 */
int za_numa_add_node(uint32_t node_id, uint64_t mem_start, uint64_t mem_size) {
    if (numa_node_count >= ZA_MAX_NUMA_NODES) return -1;
    
    struct za_numa_node *node = &numa_nodes[numa_node_count];
    node->node_id = node_id;
    node->mem_start = mem_start;
    node->mem_size = mem_size;
    node->free_size = mem_size;
    node->zone_count = 0;
    node->zones = NULL;
    node->caches = NULL;
    node->cache_count = 0;
    
    numa_node_count++;
    return 0;
}

/* NUMA 感知分配 */
void *za_alloc_numa(struct zalloc *za, size_t size, uint32_t node_id) {
    if (node_id >= numa_node_count) {
        node_id = 0;  /* 回退到节点 0 */
    }
    
    struct za_numa_node *node = &numa_nodes[node_id];
    
    /* 在指定节点上分配 */
    if (node->zones != NULL) {
        uint32_t order = za_order_for_size(size);
        return buddy_alloc(node->zones, order);
    }
    
    /* 回退到默认分配 */
    return zalloc_alloc(za, size);
}

/* 获取当前 NUMA 节点 */
uint32_t za_current_numa_node(void) {
    /* 简化实现：假设 CPU 和节点一一对应 */
    return smp_processor_id() % numa_node_count;
}

/* ========== 内存压缩 ========== */

/* 压缩迁移类型 */
#define ZA_MIGRATE_UNMOVABLE    0
#define ZA_MIGRATE_MOVABLE      1
#define ZA_MIGRATE_RECLAIMABLE  2

/* 可移动页列表 */
struct za_movable_page {
    uint64_t addr;
    uint32_t size;
    uint32_t type;
    struct list_head list;
};

static struct list_head movable_pages = LIST_HEAD_INIT(movable_pages);
static spinlock_t movable_lock = SPINLOCK_INIT;

/* 注册可移动页 */
int za_register_movable(uint64_t addr, uint32_t size, uint32_t type) {
    struct za_movable_page *page = kmalloc(sizeof(struct za_movable_page));
    if (page == NULL) return -1;
    
    page->addr = addr;
    page->size = size;
    page->type = type;
    
    spin_lock(&movable_lock);
    list_add(&page->list, &movable_pages);
    spin_unlock(&movable_lock);
    
    return 0;
}

/* 压缩内存 */
uint32_t za_compact_memory(struct zalloc *za) {
    uint32_t pages_moved = 0;
    
    spin_lock(&movable_lock);
    
    struct za_movable_page *page;
    list_for_each_entry(page, &movable_pages, list) {
        if (page->type == ZA_MIGRATE_MOVABLE) {
            /* 尝试移动到更紧凑的位置 */
            /* 实际实现需要复制数据并更新指针 */
            pages_moved++;
        }
    }
    
    spin_unlock(&movable_lock);
    
    /* 合并伙伴块 */
    for (int i = 0; i < za->zone_count; i++) {
        struct za_zone *zone = &za->zones[i];
        spin_lock(&zone->lock);
        
        for (int o = 0; o < ZA_MAX_ORDER - 1; o++) {
            while (zone->free_counts[o] >= 2) {
                struct za_block *b1 = list_first_entry(
                    &zone->free_lists[o], struct za_block, list);
                struct za_block *b2 = find_buddy(zone, b1);
                
                if (b2 != NULL) {
                    b1 = merge_blocks(zone, b1, b2);
                    list_add(&b1->list, &zone->free_lists[b1->order]);
                    zone->free_counts[b1->order]++;
                } else {
                    break;
                }
            }
        }
        
        spin_unlock(&zone->lock);
    }
    
    return pages_moved;
}

/* ========== 调试和诊断 ========== */

/* 内存分配追踪 */
struct za_alloc_trace {
    void *ptr;
    size_t size;
    uint64_t timestamp;
    uint32_t cpu;
    char tag[16];
    struct list_head list;
};

#define ZA_TRACE_MAX    1024

static struct list_head trace_list = LIST_HEAD_INIT(trace_list);
static spinlock_t trace_lock = SPINLOCK_INIT;
static uint32_t trace_count = 0;
static int trace_enabled = 0;

/* 启用追踪 */
void za_trace_enable(int enable) {
    trace_enabled = enable;
}

/* 记录分配 */
void za_trace_alloc(void *ptr, size_t size, const char *tag) {
    if (!trace_enabled || trace_count >= ZA_TRACE_MAX) return;
    
    struct za_alloc_trace *trace = kmalloc(sizeof(struct za_alloc_trace));
    if (trace == NULL) return;
    
    trace->ptr = ptr;
    trace->size = size;
    trace->timestamp = get_tick_count();
    trace->cpu = smp_processor_id();
    
    for (int i = 0; i < 15 && tag[i]; i++) {
        trace->tag[i] = tag[i];
    }
    trace->tag[15] = '\0';
    
    spin_lock(&trace_lock);
    list_add(&trace->list, &trace_list);
    trace_count++;
    spin_unlock(&trace_lock);
}

/* 记录释放 */
void za_trace_free(void *ptr) {
    if (!trace_enabled) return;
    
    spin_lock(&trace_lock);
    
    struct za_alloc_trace *trace, *next;
    list_for_each_entry_safe(trace, next, &trace_list, list) {
        if (trace->ptr == ptr) {
            list_del(&trace->list);
            kfree(trace);
            trace_count--;
            break;
        }
    }
    
    spin_unlock(&trace_lock);
}

/* 检测内存泄漏 */
uint32_t za_detect_leaks(void) {
    return trace_count;
}

/* 打印分配追踪 */
void za_print_trace(void) {
    spin_lock(&trace_lock);
    
    println("[ZALLOC] Allocation trace:");
    
    struct za_alloc_trace *trace;
    list_for_each_entry(trace, &trace_list, list) {
        print("  ptr=");
        print_hex((uint64_t)trace->ptr);
        print(" size=");
        print_dec(trace->size);
        print(" cpu=");
        print_dec(trace->cpu);
        print(" tag=");
        print(trace->tag);
        println("");
    }
    
    spin_unlock(&trace_lock);
}

/* 内存完整性检查 */
int za_check_integrity(struct zalloc *za) {
    if (za == NULL) return -1;
    
    int errors = 0;
    
    for (int i = 0; i < za->zone_count; i++) {
        struct za_zone *zone = &za->zones[i];
        
        spin_lock(&zone->lock);
        
        /* 检查空闲链表一致性 */
        for (int o = 0; o < ZA_NUM_ORDERS; o++) {
            uint32_t count = 0;
            struct za_block *block;
            list_for_each_entry(block, &zone->free_lists[o], list) {
                count++;
                
                /* 验证块地址在区域内 */
                if (block->addr < zone->start || block->addr >= zone->end) {
                    errors++;
                }
                
                /* 验证块标记为空闲 */
                if (block->flags != ZA_BLOCK_FREE) {
                    errors++;
                }
            }
            
            if (count != zone->free_counts[o]) {
                errors++;
            }
        }
        
        spin_unlock(&zone->lock);
    }
    
    return errors;
}

/* 打印分配器状态 */
void za_print_status(struct zalloc *za) {
    if (za == NULL) return;
    
    println("[ZALLOC] Status:");
    print("  Zones: ");
    print_dec(za->zone_count);
    println("");
    print("  Total allocs: ");
    print_dec(za->total_allocs);
    println("");
    print("  Total frees: ");
    print_dec(za->total_frees);
    println("");
    print("  Current usage: ");
    print_dec(za->current_usage);
    println(" bytes");
    print("  Peak usage: ");
    print_dec(za->peak_usage);
    println(" bytes");
    print("  Fragmentation: ");
    print_dec(za->defrag.fragmentation);
    println("%");
    
    for (int i = 0; i < za->zone_count; i++) {
        struct za_zone *zone = &za->zones[i];
        print("  Zone ");
        print_dec(i);
        print(": free=");
        print_dec(zone->free_size);
        print(" / ");
        print_dec(zone->size);
        println("");
        
        for (int o = 0; o < ZA_NUM_ORDERS; o++) {
            if (zone->free_counts[o] > 0) {
                print("    Order ");
                print_dec(o);
                print(": ");
                print_dec(zone->free_counts[o]);
                print(" blocks (");
                print_dec(ZA_PAGE_SIZE << o);
                println(" bytes each)");
            }
        }
    }
}

/* ========== 高级分配策略 ========== */

/* 低延迟分配 (快速路径) */
void *za_alloc_fast(struct zalloc *za, size_t size) {
    /* 直接从线程缓存分配，无锁 */
    struct za_thread_cache *cache = za_get_thread_cache(za);
    if (cache == NULL) return NULL;
    
    uint32_t sc = za_get_size_class(size);
    if (sc >= ZA_NUM_SIZE_CLASSES) return NULL;
    
    if (cache->counts[sc] > 0) {
        cache->counts[sc]--;
        void *ptr = cache->slots[sc][cache->counts[sc]].ptr;
        cache->hits++;
        return ptr;
    }
    
    cache->misses++;
    return NULL;  /* 快速路径失败，调用者应使用正常分配 */
}

/* 大块连续内存分配 */
void *za_alloc_contiguous(struct zalloc *za, size_t size, uint32_t align_order) {
    if (za == NULL || size == 0) return NULL;
    
    uint32_t order = za_order_for_size(size);
    if (align_order > order) {
        order = align_order;
    }
    
    return buddy_alloc(&za->zones[0], order);
}

/* 带 NUMA 策略的分配 */
void *za_alloc_numa_policy(struct zalloc *za, size_t size, int policy, uint32_t preferred_node) {
    switch (policy) {
    case 0:  /* 默认 */
        return zalloc_alloc(za, size);
    case 1:  /* 指定节点 */
        return za_alloc_numa(za, size, preferred_node);
    case 2:  /* 本地节点 */
        return za_alloc_numa(za, size, za_current_numa_node());
    case 3:  /* 交错 */
        return za_alloc_numa(za, size, za->total_allocs % numa_node_count);
    default:
        return NULL;
    }
}

/* ========== Z-Alloc 高级增强功能 ========== */

/* === 内存压力监控 === */

struct za_pressure_state {
    uint32_t level;             /* 压力级别 0-100 */
    uint32_t threshold_low;     /* 低阈值 */
    uint32_t threshold_high;    /* 高阈值 */
    uint64_t last_check;
    uint64_t reclaim_count;
};

static struct za_pressure_state pressure_state = {0, 20, 80, 0, 0};

/* 检查内存压力 */
uint32_t za_check_pressure(struct zalloc *za) {
    uint64_t total = 0;
    uint64_t free = 0;
    
    for (int i = 0; i < za->zone_count; i++) {
        total += za->zones[i].size;
        free += za->zones[i].free_size;
    }
    
    if (total == 0) return 100;
    
    uint32_t used_pct = (uint32_t)((total - free) * 100 / total);
    pressure_state.level = used_pct;
    pressure_state.last_check = get_tick_count();
    
    return used_pct;
}

/* 压力回调注册 */
#define ZA_MAX_PRESSURE_CB  8
static za_pressure_cb pressure_callbacks[ZA_MAX_PRESSURE_CB];
static int pressure_cb_count = 0;

int za_register_pressure_callback(za_pressure_cb cb) {
    if (pressure_cb_count >= ZA_MAX_PRESSURE_CB) return -1;
    pressure_callbacks[pressure_cb_count++] = cb;
    return 0;
}

/* 触发压力回调 */
void za_trigger_pressure_callbacks(struct zalloc *za, uint32_t level) {
    (void)za;
    for (int i = 0; i < pressure_cb_count; i++) {
        if (pressure_callbacks[i] != NULL) {
            pressure_callbacks[i]((int)level);
        }
    }
}

/* === 内存碎片整理 === */

/* 计算碎片化程度 */
uint32_t za_calc_fragmentation(struct zalloc *za) {
    uint32_t total_free = 0;
    uint32_t largest_free = 0;
    
    for (int i = 0; i < za->zone_count; i++) {
        total_free += za->zones[i].free_size;
        
        /* 找最大连续块 */
        for (int o = ZA_NUM_ORDERS - 1; o >= 0; o--) {
            if (za->zones[i].free_counts[o] > 0) {
                uint32_t block_size = ZA_PAGE_SIZE << o;
                if (block_size > largest_free) {
                    largest_free = block_size;
                }
                break;
            }
        }
    }
    
    if (total_free == 0) return 100;
    
    /* 碎片化 = 1 - 最大块 / 总空闲 */
    return 100 - (largest_free * 100 / total_free);
}

/* 执行碎片整理 */
int za_defragment(struct zalloc *za, int zone_idx) {
    if (za == NULL || zone_idx < 0 || zone_idx >= za->zone_count) return -1;
    
    struct za_zone *zone = &za->zones[zone_idx];
    int merged = 0;
    
    spin_lock(&zone->lock);
    
    /* 尝试合并所有可能的块 */
    for (int o = 0; o < ZA_NUM_ORDERS - 1; o++) {
        struct za_block *block, *next;
        list_for_each_entry_safe(block, next, &zone->free_lists[o], list) {
            struct za_block *buddy = find_buddy(zone, block);
            if (buddy != NULL) {
                block = merge_blocks(zone, block, buddy);
                merged++;
            }
        }
    }
    
    spin_unlock(&zone->lock);
    return merged;
}

/* === 内存预读 === */

struct za_prefetch {
    uint64_t last_addr;
    uint32_t last_size;
    uint32_t hit_count;
    int enabled;
};

static struct za_prefetch prefetch_state = {0, 0, 0, 1};

/* 预读内存 */
void za_prefetch_region(struct zalloc *za, uint64_t addr, uint32_t size) {
    if (!prefetch_state.enabled) return;
    
    /* 预取到缓存 */
    for (uint64_t a = addr; a < addr + size; a += 64) {
        __builtin_prefetch((void *)a, 0, 3);
    }
    
    prefetch_state.last_addr = addr;
    prefetch_state.last_size = size;
}

/* === 对齐分配增强 === */

/* 对齐分配 */
void *za_alloc_aligned(struct zalloc *za, size_t size, size_t alignment) {
    if (za == NULL || size == 0) return NULL;
    if (alignment == 0) alignment = 1;
    
    /* 确保对齐是 2 的幂 */
    size_t align = 1;
    while (align < alignment) align <<= 1;
    
    /* 计算所需阶 */
    size_t total = size + align - 1;
    uint32_t order = za_order_for_size(total);
    
    void *ptr = buddy_alloc(&za->zones[0], order);
    if (ptr == NULL) return NULL;
    
    /* 对齐地址 */
    uint64_t addr = (uint64_t)ptr;
    uint64_t aligned_addr = (addr + align - 1) & ~(align - 1);
    
    return (void *)aligned_addr;
}

/* === 内存统计增强 === */

/* 获取详细统计 */
void za_get_detailed_stats(struct zalloc *za, uint64_t *total_allocs, 
                           uint64_t *total_frees, uint64_t *current_usage,
                           uint64_t *peak_usage, uint32_t *fragmentation) {
    if (total_allocs) *total_allocs = za->total_allocs;
    if (total_frees) *total_frees = za->total_frees;
    if (current_usage) *current_usage = za->current_usage;
    if (peak_usage) *peak_usage = za->peak_usage;
    if (fragmentation) *fragmentation = za_calc_fragmentation(za);
}

/* === 紧急分配 === */

/* 紧急内存池 */
#define ZA_EMERGENCY_POOL_SIZE  (16 * 1024)  /* 16KB */
static uint8_t emergency_pool[ZA_EMERGENCY_POOL_SIZE];
static spinlock_t emergency_lock = SPINLOCK_INIT;
static uint32_t emergency_used = 0;

/* 紧急分配 */
void *za_alloc_emergency(size_t size) {
    if (size > ZA_EMERGENCY_POOL_SIZE) return NULL;
    
    spin_lock(&emergency_lock);
    
    if (emergency_used + size > ZA_EMERGENCY_POOL_SIZE) {
        spin_unlock(&emergency_lock);
        return NULL;
    }
    
    void *ptr = &emergency_pool[emergency_used];
    emergency_used += size;
    
    spin_unlock(&emergency_lock);
    return ptr;
}

/* 重置紧急池 */
void za_reset_emergency_pool(void) {
    spin_lock(&emergency_lock);
    emergency_used = 0;
    spin_unlock(&emergency_lock);
}

/* === 内存池管理 === */

/* 简化的内存池 */
struct za_mem_pool {
    size_t obj_size;
    uint32_t capacity;
    uint32_t used;
    struct list_head free_list;
    spinlock_t lock;
};

/* 创建内存池 */
struct za_mem_pool *za_mem_pool_create(struct zalloc *za, size_t obj_size, uint32_t count) {
    struct za_mem_pool *pool = kmalloc(sizeof(struct za_mem_pool));
    if (pool == NULL) return NULL;
    
    pool->obj_size = obj_size;
    pool->capacity = count;
    pool->used = 0;
    INIT_LIST_HEAD(&pool->free_list);
    pool->lock = SPINLOCK_INIT;
    
    /* 预分配对象 - 使用链表节点 */
    for (uint32_t i = 0; i < count; i++) {
        void *obj = zalloc_alloc(za, obj_size);
        if (obj == NULL) break;
        
        struct list_head *node = kmalloc(sizeof(struct list_head));
        if (node == NULL) {
            zalloc_free(za, obj);
            break;
        }
        
        /* 存储指针 */
        *(void **)node = obj;
        list_add(node, &pool->free_list);
    }
    
    return pool;
}

/* 从池分配 */
void *za_mem_pool_alloc(struct za_mem_pool *pool) {
    if (pool == NULL) return NULL;
    
    spin_lock(&pool->lock);
    
    if (list_empty(&pool->free_list)) {
        spin_unlock(&pool->lock);
        return NULL;
    }
    
    struct list_head *node = pool->free_list.next;
    list_del(node);
    void *ptr = *(void **)node;
    pool->used++;
    
    spin_unlock(&pool->lock);
    kfree(node);
    return ptr;
}

/* 释放到池 */
void za_mem_pool_free(struct za_mem_pool *pool, void *ptr) {
    if (pool == NULL || ptr == NULL) return;
    
    struct list_head *node = kmalloc(sizeof(struct list_head));
    if (node == NULL) return;
    
    *(void **)node = ptr;
    
    spin_lock(&pool->lock);
    list_add(node, &pool->free_list);
    pool->used--;
    spin_unlock(&pool->lock);
}

/* === 内存泄漏检测 === */

struct za_alloc_record {
    uint64_t addr;
    size_t size;
    uint64_t timestamp;
    int valid;
};

#define ZA_MAX_RECORDS  1024
static struct za_alloc_record alloc_records[ZA_MAX_RECORDS];
static int record_idx = 0;
static spinlock_t record_lock = SPINLOCK_INIT;
static int leak_detection_enabled = 0;

/* 记录分配 */
void za_record_alloc(uint64_t addr, size_t size) {
    if (!leak_detection_enabled) return;
    
    spin_lock(&record_lock);
    
    alloc_records[record_idx].addr = addr;
    alloc_records[record_idx].size = size;
    alloc_records[record_idx].timestamp = get_tick_count();
    alloc_records[record_idx].valid = 1;
    
    record_idx = (record_idx + 1) % ZA_MAX_RECORDS;
    
    spin_unlock(&record_lock);
}

/* 记录释放 */
void za_record_free(uint64_t addr) {
    if (!leak_detection_enabled) return;
    
    spin_lock(&record_lock);
    
    for (int i = 0; i < ZA_MAX_RECORDS; i++) {
        if (alloc_records[i].valid && alloc_records[i].addr == addr) {
            alloc_records[i].valid = 0;
            break;
        }
    }
    
    spin_unlock(&record_lock);
}

/* 报告泄漏 */
void za_report_leaks(void) {
    spin_lock(&record_lock);
    
    uint64_t leaked = 0;
    int count = 0;
    
    for (int i = 0; i < ZA_MAX_RECORDS; i++) {
        if (alloc_records[i].valid) {
            leaked += alloc_records[i].size;
            count++;
        }
    }
    
    spin_unlock(&record_lock);
    
    print("[ZALLOC] Leaked: ");
    print_dec(count);
    print(" allocations, ");
    print_dec(leaked);
    println(" bytes");
}

/* 启用泄漏检测 */
void za_enable_leak_detection(int enable) {
    leak_detection_enabled = enable;
}

/* === 完整诊断报告 === */

void za_print_report(struct zalloc *za) {
    if (za == NULL) return;
    
    println("╔══════════════════════════════════════════╗");
    println("║        Z-Alloc Diagnostic Report         ║");
    println("╚══════════════════════════════════════════╝");
    
    println("");
    println("=== 基本统计 ===");
    print("总分配次数: "); print_dec(za->total_allocs); println("");
    print("总释放次数: "); print_dec(za->total_frees); println("");
    print("当前使用: "); print_dec(za->current_usage); println(" bytes");
    print("峰值使用: "); print_dec(za->peak_usage); println(" bytes");
    
    println("");
    println("=== 内存压力 ===");
    uint32_t pressure = za_check_pressure(za);
    print("压力级别: "); print_dec(pressure); println("%");
    print("状态: ");
    if (pressure < 30) println("正常");
    else if (pressure < 60) println("中等");
    else if (pressure < 80) println("高");
    else println("严重");
    
    println("");
    println("=== 碎片化 ===");
    uint32_t frag = za_calc_fragmentation(za);
    print("碎片化程度: "); print_dec(frag); println("%");
    
    println("");
    println("=== 区域状态 ===");
    for (int i = 0; i < za->zone_count; i++) {
        struct za_zone *zone = &za->zones[i];
        print("Zone "); print_dec(i); println(":");
        print("  大小: "); print_dec(zone->size / (1024*1024)); println(" MB");
        print("  空闲: "); print_dec(zone->free_size / (1024*1024)); println(" MB");
        
        /* 计算利用率 */
        uint32_t util = zone->size > 0 ? 
            (uint32_t)((zone->size - zone->free_size) * 100 / zone->size) : 0;
        print("  利用率: "); print_dec(util); println("%");
    }
    
    println("");
    println("=== 线程缓存 ===");
    print("缓存池活跃: ");
    print_dec(za->cache_pool.count);
    println("");
}
