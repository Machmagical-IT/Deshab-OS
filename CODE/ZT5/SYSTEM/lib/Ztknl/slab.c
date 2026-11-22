/*
 * Zratro OS - Slab Allocator Implementation
 */

#include "slab.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"

/* 页移位 */
#define PAGE_SHIFT      12

/* 对齐宏 */
#define ALIGN_UP(x, align)     (((x) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(x, align)   ((x) & ~((align) - 1))

/* 缓存链表 */
static LIST_HEAD(slab_caches);
static volatile int slab_lock = 0;

/* kmalloc 缓存大小 */
static const size_t kmalloc_sizes[] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
};
#define KMALLOC_CACHES  (sizeof(kmalloc_sizes) / sizeof(kmalloc_sizes[0]))

struct slab_cache *kmalloc_caches[KMALLOC_CACHES];

/* Per-CPU 缓存默认配置 */
#define CPU_CACHE_MIN     4
#define CPU_CACHE_MAX     128
#define CPU_CACHE_BATCH   8

/* ========== 辅助函数 ========== */

/* 计算每个 slab 的对象数 */
static void calculate_slab_params(struct slab_cache *cache) {
    size_t size = ALIGN_UP(cache->obj_size, cache->align);
    size_t slab_size = PAGE_SIZE;
    
    /* 确保每个 slab 至少有 SLAB_MIN_OBJECTS 个对象 */
    while (slab_size / size < SLAB_MIN_OBJECTS && 
           slab_size < (PAGE_SIZE << SLAB_ORDER_MAX)) {
        slab_size *= 2;
    }
    
    /* 计算对象数 */
    cache->objs_per_slab = (slab_size - sizeof(struct slab)) / size;
    
    /* 计算颜色范围 - 使用更多颜色以改善缓存分布 */
    cache->color_max = (slab_size - cache->objs_per_slab * size - sizeof(struct slab)) 
                       / cache->align;
    /* 增强着色：使用更大的颜色范围上限以获得更好的缓存行分布 */
    if (cache->color_max > SLAB_COLOR_MAX * 2) {
        cache->color_max = SLAB_COLOR_MAX * 2;
    }
    /* 确保至少有 1 种颜色（即使空间有限） */
    if (cache->color_max == 0 && slab_size > cache->objs_per_slab * size + sizeof(struct slab) + cache->align) {
        cache->color_max = 1;
    }
    
    cache->color_next = 0;
}

/* 分配 slab */
static struct slab *slab_create(struct slab_cache *cache) {
    size_t size = ALIGN_UP(cache->obj_size, cache->align);
    size_t slab_size = PAGE_SIZE;
    
    /* 计算需要的页数 */
    size_t pages = (cache->objs_per_slab * size + sizeof(struct slab) + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;
    
    /* 分配物理页 */
    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == 0) {
        return NULL;
    }
    
    /* 映射到内核地址空间 */
    static volatile uint64_t next_slab_addr = 0xFFFFFFFF90300000ULL;
    /* 使用原子操作获取并增加地址，避免 SMP 竞争 */
    uint64_t virt = __sync_fetch_and_add((volatile uint64_t *)&next_slab_addr, pages * PAGE_SIZE);
    
    if (vmm_map_range(vmm_get_kernel_space(), virt, phys, pages,
                      VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE) != 0) {
        pmm_free_pages(phys, pages);
        return NULL;
    }
    
    /* 清零 */
    memset((void *)virt, 0, pages * PAGE_SIZE);
    
    /* 设置 slab 描述符 */
    struct slab *slab = (struct slab *)(virt + pages * PAGE_SIZE - sizeof(struct slab));
    slab->freelist = NULL;
    slab->inuse = 0;
    slab->total = cache->objs_per_slab;
    slab->color = cache->color_next;
    slab->cache = cache;
    
    /* 更新下一个颜色 */
    cache->color_next = (cache->color_next + 1) % (cache->color_max + 1);
    
    /* 初始化空闲链表 */
    void *obj_start = (void *)(virt + slab->color * cache->align);
    for (uint32_t i = 0; i < cache->objs_per_slab; i++) {
        void *obj = (char *)obj_start + i * size;
        
        /* 调用构造函数 */
        if (cache->ctor) {
            cache->ctor(obj);
        }
        
        /* 加入空闲链表 */
        *(void **)obj = slab->freelist;
        slab->freelist = obj;
    }
    
    /* 更新统计 */
    cache->nr_slabs++;
    cache->nr_objs += slab->total;
    cache->nr_free += slab->total;
    
    return slab;
}

/* 销毁 slab */
static void slab_destroy(struct slab *slab) {
    struct slab_cache *cache = slab->cache;
    
    /* 调用析构函数 */
    if (cache->dtor) {
        void *obj = slab->freelist;
        while (obj) {
            cache->dtor(obj);
            obj = *(void **)obj;
        }
    }
    
    /* 计算地址范围 */
    size_t size = ALIGN_UP(cache->obj_size, cache->align);
    size_t pages = (cache->objs_per_slab * size + sizeof(struct slab) + PAGE_SIZE - 1) / PAGE_SIZE;
    
    uint64_t virt = (uint64_t)slab + sizeof(struct slab) - pages * PAGE_SIZE;
    uint64_t phys = vmm_get_physical(vmm_get_kernel_space(), virt);
    
    /* 解除映射 */
    vmm_unmap_range(vmm_get_kernel_space(), virt, pages);
    
    /* 释放物理页 */
    if (phys != 0) {
        pmm_free_pages(phys, pages);
    }
    
    /* 更新统计 - 添加边界检查防止下溢 */
    uint32_t free_count = 0;
    if (slab->total >= slab->inuse) {
        free_count = slab->total - slab->inuse;
    }
    
    cache->nr_slabs--;
    
    if (cache->nr_objs >= slab->total) {
        cache->nr_objs -= slab->total;
    } else {
        cache->nr_objs = 0;
    }
    
    if (cache->nr_free >= free_count) {
        cache->nr_free -= free_count;
    } else {
        cache->nr_free = 0;
    }
}

/* ========== 缓存管理 ========== */

int slab_init(void) {
    println("[SLAB] Initializing slab allocator...");
    
    /* 初始化全局锁 */
    slab_lock = 0;
    
    /* 创建 kmalloc 缓存 */
    for (size_t i = 0; i < KMALLOC_CACHES; i++) {
        char name[16];
        /* 简单的数字转字符串 */
        int size = kmalloc_sizes[i];
        int j = 0;
        char temp[16];
        int k = 0;
        while (size > 0) {
            temp[k++] = '0' + (size % 10);
            size /= 10;
        }
        name[0] = 'k';
        name[1] = 'm';
        name[2] = 'a';
        name[3] = 'l';
        name[4] = 'l';
        name[5] = 'o';
        name[6] = 'c';
        name[7] = '-';
        while (k > 0) {
            name[8 + j++] = temp[--k];
        }
        name[8 + j] = '\0';
        
        kmalloc_caches[i] = slab_cache_create(name, kmalloc_sizes[i], 
                                               kmalloc_sizes[i], 0, NULL, NULL);
        if (kmalloc_caches[i] == NULL) {
            print("[SLAB] Warning: Failed to create kmalloc-");
            print_dec(kmalloc_sizes[i]);
            println("");
        }
    }
    
    println("[SLAB] Initialized successfully");
    return 0;
}

struct slab_cache *slab_cache_create(const char *name, size_t size,
                                      size_t align, int flags,
                                      void (*ctor)(void *obj),
                                      void (*dtor)(void *obj)) {
    /* 从堆分配缓存结构 */
    struct slab_cache *cache = (struct slab_cache *)kmalloc(sizeof(*cache));
    if (cache == NULL) {
        return NULL;
    }
    
    /* 初始化 */
    cache->name = name;
    cache->obj_size = size;
    cache->align = align > 8 ? align : 8;
    cache->flags = flags;
    cache->ctor = ctor;
    cache->dtor = dtor;
    
    INIT_LIST_HEAD(&cache->slabs_full);
    INIT_LIST_HEAD(&cache->slabs_partial);
    INIT_LIST_HEAD(&cache->slabs_free);
    
    cache->nr_slabs = 0;
    cache->nr_partial = 0;
    cache->nr_objs = 0;
    cache->nr_free = 0;
    
    cache->lock = 0;
    
    /* 计算 slab 参数 */
    calculate_slab_params(cache);
    
    /* 初始化 Per-CPU 缓存 */
    for (int i = 0; i < MAX_CPUS; i++) {
        struct cpu_cache *cc = &cache->cpu_cache[i];
        
        /* 分配缓存数组 */
        cc->freelist = (void **)kmalloc(sizeof(void *) * CPU_CACHE_MAX);
        if (cc->freelist == NULL) {
            /* 回滚 */
            for (int j = 0; j < i; j++) {
                kfree(cache->cpu_cache[j].freelist);
            }
            kfree(cache);
            return NULL;
        }
        
        cc->avail = 0;
        cc->limit = CPU_CACHE_MAX;
        cc->batchcount = CPU_CACHE_BATCH;
        cc->alloc_count = 0;
        cc->free_count = 0;
    }
    
    /* 加入全局链表 */
    while (__sync_lock_test_and_set(&slab_lock, 1)) {
        asm volatile("pause");
    }
    list_add_tail(&cache->cache_list, &slab_caches);
    __sync_lock_release(&slab_lock);
    
    return cache;
}

void slab_cache_destroy(struct slab_cache *cache) {
    if (cache == NULL) return;
    
    /* 从全局链表移除 */
    while (__sync_lock_test_and_set(&slab_lock, 1)) {
        asm volatile("pause");
    }
    list_del(&cache->cache_list);
    __sync_lock_release(&slab_lock);
    
    /* 销毁所有 slab */
    struct list_head *pos, *n;
    
    list_for_each_safe(pos, n, &cache->slabs_full) {
        struct slab *slab = list_entry(pos, struct slab, list);
        list_del(&slab->list);
        slab_destroy(slab);
    }
    
    list_for_each_safe(pos, n, &cache->slabs_partial) {
        struct slab *slab = list_entry(pos, struct slab, list);
        list_del(&slab->list);
        slab_destroy(slab);
    }
    
    list_for_each_safe(pos, n, &cache->slabs_free) {
        struct slab *slab = list_entry(pos, struct slab, list);
        list_del(&slab->list);
        slab_destroy(slab);
    }
    
    /* 释放 Per-CPU 缓存 */
    for (int i = 0; i < MAX_CPUS; i++) {
        if (cache->cpu_cache[i].freelist) {
            kfree(cache->cpu_cache[i].freelist);
        }
    }
    
    kfree(cache);
}

/* ========== 对象分配 ========== */

/* 从 slab 分配对象 */
static void *slab_alloc_from_slab(struct slab *slab) {
    if (slab->freelist == NULL) {
        return NULL;
    }
    
    void *obj = slab->freelist;
    slab->freelist = *(void **)obj;
    slab->inuse++;
    
    slab->cache->nr_free--;
    
    return obj;
}

/* 向 slab 释放对象 */
static void slab_free_to_slab(struct slab *slab, void *obj) {
    *(void **)obj = slab->freelist;
    slab->freelist = obj;
    slab->inuse--;
    
    slab->cache->nr_free++;
}

/* 补充 Per-CPU 缓存 */
static int refill_cpu_cache(struct slab_cache *cache, int cpu) {
    struct cpu_cache *cc = &cache->cpu_cache[cpu];
    int batch = cc->batchcount;
    
    /* 动态调整 batchcount：根据分配/释放比例调整批量大小 */
    if (cc->alloc_count > 100) {
        uint64_t ratio = cc->alloc_count / (cc->free_count + 1);
        if (ratio > 3 && cc->batchcount < CPU_CACHE_BATCH * 4) {
            /* 分配远多于释放，增大批量以减少 refill 次数 */
            cc->batchcount = cc->batchcount + (cc->batchcount >> 2);
            if (cc->batchcount > CPU_CACHE_MAX) cc->batchcount = CPU_CACHE_MAX;
            batch = cc->batchcount;
        } else if (ratio < 1 && cc->batchcount > CPU_CACHE_BATCH / 2) {
            /* 释放多于分配，减小批量避免缓存浪费 */
            cc->batchcount = cc->batchcount - (cc->batchcount >> 3);
            if (cc->batchcount < CPU_CACHE_MIN) cc->batchcount = CPU_CACHE_MIN;
            batch = cc->batchcount;
        }
    }
    
    while (__sync_lock_test_and_set(&cache->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 从 partial 和 free slabs 获取对象 */
    while (batch > 0) {
        struct slab *slab = NULL;
        
        /* 优先从 partial 分配 */
        if (!list_empty(&cache->slabs_partial)) {
            slab = list_entry(cache->slabs_partial.next, struct slab, list);
        } else if (!list_empty(&cache->slabs_free)) {
            slab = list_entry(cache->slabs_free.next, struct slab, list);
        }
        
        if (slab == NULL) {
            /* 创建新 slab */
            slab = slab_create(cache);
            if (slab == NULL) {
                break;
            }
            list_add(&slab->list, &cache->slabs_partial);
        }
        
        /* 从 slab 分配对象 */
        while (slab->freelist && batch > 0 && cc->avail < cc->limit) {
            void *obj = slab_alloc_from_slab(slab);
            cc->freelist[cc->avail++] = obj;
            batch--;
        }
        
        /* 更新 slab 状态 */
        if (slab->inuse == slab->total) {
            /* 变为 full */
            list_del(&slab->list);
            list_add(&slab->list, &cache->slabs_full);
        } else if (slab->inuse > 0 && 
                   list_entry(slab->list.prev, struct slab, list) != slab) {
            /* 移到 partial */
            list_del(&slab->list);
            list_add(&slab->list, &cache->slabs_partial);
        }
    }
    
    __sync_lock_release(&cache->lock);
    
    return cc->avail;
}

void *slab_alloc(struct slab_cache *cache, int flags) {
    if (cache == NULL) return NULL;
    
    int cpu = smp_processor_id();
    struct cpu_cache *cc = &cache->cpu_cache[cpu];
    
    /* 检查 Per-CPU 缓存 */
    if (cc->avail == 0) {
        if (refill_cpu_cache(cache, cpu) == 0) {
            return NULL;
        }
    }
    
    cc->avail--;
    cc->alloc_count++;
    
    return cc->freelist[cc->avail];
}

/* 获取页阶数 - 提前定义 */
static int get_order(size_t size) {
    int order = 0;
    size_t s = PAGE_SIZE;
    while (s < size) {
        s <<= 1;
        order++;
    }
    return order;
}

/* 检查对象是否在 slab 范围内 */
static struct slab *find_slab_for_obj(struct slab_cache *cache, void *obj) {
    uint64_t obj_addr = (uint64_t)obj;
    
    /* 遍历 partial slabs */
    struct list_head *pos;
    list_for_each(pos, &cache->slabs_partial) {
        struct slab *slab = list_entry(pos, struct slab, list);
        /* slab 描述符在页末尾，计算 slab 起始地址 */
        uint64_t slab_end = (uint64_t)slab + sizeof(struct slab);
        uint64_t slab_start = slab_end - (PAGE_SIZE << get_order(cache->objs_per_slab * cache->obj_size + sizeof(struct slab)));
        
        /* 检查对象是否在此 slab 范围内 */
        if (obj_addr >= slab_start && obj_addr < slab_end - sizeof(struct slab)) {
            return slab;
        }
    }
    
    /* 遍历 full slabs */
    list_for_each(pos, &cache->slabs_full) {
        struct slab *slab = list_entry(pos, struct slab, list);
        uint64_t slab_end = (uint64_t)slab + sizeof(struct slab);
        uint64_t slab_start = slab_end - (PAGE_SIZE << get_order(cache->objs_per_slab * cache->obj_size + sizeof(struct slab)));
        
        if (obj_addr >= slab_start && obj_addr < slab_end - sizeof(struct slab)) {
            return slab;
        }
    }
    
    return NULL;
}

/* 更高效的 slab 查找：使用地址对齐 */
static struct slab *find_slab_by_addr(struct slab_cache *cache, void *obj) {
    /* 假设 slab 以页对齐，我们可以快速定位 slab 描述符 */
    uint64_t addr = (uint64_t)obj;
    
    /* 尝试从页末尾获取 slab 描述符 */
    uint64_t page_addr = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    struct slab *slab = (struct slab *)(page_addr - sizeof(struct slab));
    
    /* 验证 slab 是否有效 */
    if (slab->cache == cache && slab->total > 0 && slab->inuse >= 0) {
        return slab;
    }
    
    /* 对于多页 slab，尝试更大页数 */
    for (int order = 1; order <= SLAB_ORDER_MAX; order++) {
        uint64_t big_page = addr & ~((PAGE_SIZE << order) - 1);
        slab = (struct slab *)(big_page + (PAGE_SIZE << order) - sizeof(struct slab));
        
        if (slab->cache == cache && slab->total > 0 && slab->inuse >= 0) {
            return slab;
        }
    }
    
    /* 回退到遍历查找 */
    return find_slab_for_obj(cache, obj);
}

void slab_free(struct slab_cache *cache, void *obj) {
    if (cache == NULL || obj == NULL) return;
    
    int cpu = smp_processor_id();
    struct cpu_cache *cc = &cache->cpu_cache[cpu];
    
    /* 检查是否可以放入 Per-CPU 缓存 */
    if (cc->avail < cc->limit) {
        cc->freelist[cc->avail++] = obj;
        cc->free_count++;
        return;
    }
    
    /* Per-CPU 缓存已满，刷新部分对象到 slab */
    while (__sync_lock_test_and_set(&cache->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 找到对象所属的 slab */
    struct slab *slab = find_slab_by_addr(cache, obj);
    
    if (slab != NULL) {
        /* 释放对象到 slab */
        slab_free_to_slab(slab, obj);
        
        /* 更新 slab 状态 */
        if (slab->inuse == 0) {
            /* 从 partial 移到 free */
            list_del(&slab->list);
            list_add(&slab->list, &cache->slabs_free);
            cache->nr_partial--;
        } else if (slab->inuse == slab->total - 1) {
            /* 从 full 移到 partial */
            list_del(&slab->list);
            list_add(&slab->list, &cache->slabs_partial);
            cache->nr_partial++;
        }
    } else {
        /* 无法找到所属 slab，刷新 Per-CPU 缓存到 slab */
        int flushed = 0;
        for (int i = 0; i < cc->avail && i < cc->batchcount; i++) {
            struct slab *s = find_slab_by_addr(cache, cc->freelist[i]);
            if (s != NULL) {
                slab_free_to_slab(s, cc->freelist[i]);
                flushed++;
                
                if (s->inuse == 0) {
                    list_del(&s->list);
                    list_add(&s->list, &cache->slabs_free);
                    cache->nr_partial--;
                } else if (s->inuse == s->total - 1) {
                    list_del(&s->list);
                    list_add(&s->list, &cache->slabs_partial);
                    cache->nr_partial++;
                }
            }
        }
        
        /* 移动剩余对象 */
        if (flushed > 0) {
            for (int i = flushed; i < cc->avail; i++) {
                cc->freelist[i - flushed] = cc->freelist[i];
            }
            cc->avail -= flushed;
        }
        
        /* 现在放入对象 */
        if (cc->avail < cc->limit) {
            cc->freelist[cc->avail++] = obj;
            cc->free_count++;
        }
    }
    
    __sync_lock_release(&cache->lock);
}

/* ========== 统计 ========== */

void slab_cache_stats(struct slab_cache *cache) {
    print("Cache: ");
    println(cache->name);
    
    print("  Object size: ");
    print_dec(cache->obj_size);
    println("");
    
    print("  Slabs: ");
    print_dec(cache->nr_slabs);
    print(" (partial: ");
    print_dec(cache->nr_partial);
    println(")");
    
    print("  Objects: ");
    print_dec(cache->nr_objs);
    print(" (free: ");
    print_dec(cache->nr_free);
    println(")");
    
    for (int i = 0; i < smp_cpu_count; i++) {
        struct cpu_cache *cc = &cache->cpu_cache[i];
        print("  CPU ");
        print_dec(i);
        print(": avail=");
        print_dec(cc->avail);
        print(" allocs=");
        print_dec(cc->alloc_count);
        print(" frees=");
        print_dec(cc->free_count);
        println("");
    }
}

void slab_stats_all(void) {
    println("=== Slab Allocator Statistics ===");
    
    struct list_head *pos;
    list_for_each(pos, &slab_caches) {
        struct slab_cache *cache = list_entry(pos, struct slab_cache, cache_list);
        slab_cache_stats(cache);
        println("");
    }
}

/* ========== Slab 增强功能 ========== */

/* 对象预取 - 提前填充 CPU 缓存 */
int slab_prefill_cpu_cache(struct slab_cache *cache, int cpu) {
    if (cache == NULL || cpu < 0 || cpu >= MAX_CPUS) return -1;
    
    struct cpu_cache *cc = &cache->cpu_cache[cpu];
    int needed = cc->limit - cc->avail;
    
    if (needed <= 0) return 0;
    
    /* 批量分配 */
    int batch = needed < cc->batchcount ? needed : cc->batchcount;
    int count = 0;
    
    while (__sync_lock_test_and_set(&cache->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 从空闲 slab 分配 */
    while (!list_empty(&cache->slabs_free) && count < batch) {
        struct slab *slab = list_first_entry(&cache->slabs_free, 
                                              struct slab, list);
        
        while (slab->freelist && count < batch) {
            void *obj = slab->freelist;
            slab->freelist = *(void **)obj;
            slab->inuse++;
            cache->nr_free--;
            
            cc->freelist[cc->avail++] = obj;
            count++;
        }
        
        /* 移动 slab */
        if (slab->inuse == slab->total) {
            list_del(&slab->list);
            list_add(&slab->list, &cache->slabs_full);
        } else if (slab->inuse > 0) {
            list_del(&slab->list);
            list_add(&slab->list, &cache->slabs_partial);
            cache->nr_partial++;
        }
    }
    
    __sync_lock_release(&cache->lock);
    return count;
}

/* Slab 碎片整理 - 回收空闲 slab */
int slab_shrink(struct slab_cache *cache) {
    if (cache == NULL) return 0;
    
    int freed = 0;
    
    while (__sync_lock_test_and_set(&cache->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 释放空闲 slab */
    while (!list_empty(&cache->slabs_free)) {
        struct slab *slab = list_first_entry(&cache->slabs_free, 
                                              struct slab, list);
        list_del(&slab->list);
        slab_destroy(slab);
        freed++;
        
        /* 每次最多释放 8 个 slab */
        if (freed >= 8) break;
    }
    
    __sync_lock_release(&cache->lock);
    return freed;
}

/* 全局碎片整理 */
int slab_shrink_all(void) {
    int total_freed = 0;
    
    struct list_head *pos;
    list_for_each(pos, &slab_caches) {
        struct slab_cache *cache = list_entry(pos, struct slab_cache, cache_list);
        total_freed += slab_shrink(cache);
    }
    
    return total_freed;
}

/* Slab 内存池 */
struct slab_pool {
    struct slab_cache *cache;
    void **prealloc;
    size_t prealloc_size;
    size_t prealloc_used;
    volatile int lock;
};

/* 创建内存池 */
struct slab_pool *slab_pool_create(const char *name, size_t obj_size,
                                    size_t prealloc_count, int flags) {
    struct slab_pool *pool = kmalloc(sizeof(struct slab_pool));
    if (pool == NULL) return NULL;
    
    /* 创建缓存 */
    pool->cache = slab_cache_create(name, obj_size, obj_size, flags, NULL, NULL);
    if (pool->cache == NULL) {
        kfree(pool);
        return NULL;
    }
    
    /* 预分配对象 */
    if (prealloc_count > 0) {
        pool->prealloc = kmalloc(sizeof(void *) * prealloc_count);
        if (pool->prealloc == NULL) {
            slab_cache_destroy(pool->cache);
            kfree(pool);
            return NULL;
        }
        
        pool->prealloc_size = prealloc_count;
        pool->prealloc_used = 0;
        
        for (size_t i = 0; i < prealloc_count; i++) {
            pool->prealloc[i] = slab_alloc(pool->cache, flags);
            if (pool->prealloc[i] == NULL) {
                /* 回滚 */
                for (size_t j = 0; j < i; j++) {
                    slab_free(pool->cache, pool->prealloc[j]);
                }
                kfree(pool->prealloc);
                slab_cache_destroy(pool->cache);
                kfree(pool);
                return NULL;
            }
        }
    } else {
        pool->prealloc = NULL;
        pool->prealloc_size = 0;
        pool->prealloc_used = 0;
    }
    
    pool->lock = 0;
    return pool;
}

/* 从内存池分配 */
void *slab_pool_alloc(struct slab_pool *pool, int flags) {
    if (pool == NULL) return NULL;
    
    while (__sync_lock_test_and_set(&pool->lock, 1)) {
        asm volatile("pause");
    }
    
    void *obj = NULL;
    
    /* 优先使用预分配（prealloc_used 表示可用对象数，从数组尾部取） */
    if (pool->prealloc_used > 0) {
        obj = pool->prealloc[--pool->prealloc_used];
    }
    
    __sync_lock_release(&pool->lock);
    
    /* 否则从缓存分配 */
    if (obj == NULL) {
        obj = slab_alloc(pool->cache, flags);
    }
    
    return obj;
}

/* 检查对象是否在 slab 的对象范围内 */
static int slab_pool_obj_in_range(struct slab_pool *pool, void *obj) {
    if (pool == NULL || obj == NULL || pool->cache == NULL) return 0;
    
    struct slab_cache *cache = pool->cache;
    size_t obj_size = ALIGN_UP(cache->obj_size, cache->align);
    
    /* 遍历所有 slab 检查对象是否在合法范围内 */
    struct list_head *pos;
    
    list_for_each(pos, &cache->slabs_partial) {
        struct slab *slab = list_entry(pos, struct slab, list);
        uint64_t slab_end = (uint64_t)slab + sizeof(struct slab);
        size_t pages = (cache->objs_per_slab * obj_size + sizeof(struct slab) + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t slab_start = slab_end - pages * PAGE_SIZE;
        void *obj_start = (void *)(slab_start + slab->color * cache->align);
        
        for (uint32_t i = 0; i < cache->objs_per_slab; i++) {
            if (obj == (char *)obj_start + i * obj_size) {
                return 1;
            }
        }
    }
    
    list_for_each(pos, &cache->slabs_full) {
        struct slab *slab = list_entry(pos, struct slab, list);
        uint64_t slab_end = (uint64_t)slab + sizeof(struct slab);
        size_t pages = (cache->objs_per_slab * obj_size + sizeof(struct slab) + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t slab_start = slab_end - pages * PAGE_SIZE;
        void *obj_start = (void *)(slab_start + slab->color * cache->align);
        
        for (uint32_t i = 0; i < cache->objs_per_slab; i++) {
            if (obj == (char *)obj_start + i * obj_size) {
                return 1;
            }
        }
    }
    
    return 0;
}

/* 释放到内存池 */
void slab_pool_free(struct slab_pool *pool, void *obj) {
    if (pool == NULL || obj == NULL) return;
    
    /* 验证对象是否在 slab 的合法范围内 */
    if (!slab_pool_obj_in_range(pool, obj)) {
        /* 对象不在任何 slab 的对象范围内，可能是无效指针 */
        return;
    }
    
    while (__sync_lock_test_and_set(&pool->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 尝试放回预分配区（追加到数组末尾） */
    if (pool->prealloc_used < pool->prealloc_size) {
        pool->prealloc[pool->prealloc_used++] = obj;
        __sync_lock_release(&pool->lock);
        return;
    }
    
    __sync_lock_release(&pool->lock);
    
    /* 否则释放到缓存 */
    slab_free(pool->cache, obj);
}

/* 销毁内存池 */
void slab_pool_destroy(struct slab_pool *pool) {
    if (pool == NULL) return;
    
    /* 释放预分配对象（prealloc_used 表示当前可用的对象数） */
    if (pool->prealloc != NULL) {
        for (size_t i = 0; i < pool->prealloc_used; i++) {
            if (pool->prealloc[i] != NULL) {
                slab_free(pool->cache, pool->prealloc[i]);
            }
        }
        kfree(pool->prealloc);
    }
    
    /* 销毁缓存 */
    slab_cache_destroy(pool->cache);
    kfree(pool);
}

/* ========== Slab 调试 ========== */

/* 验证 slab 完整性 */
int slab_validate_cache(struct slab_cache *cache) {
    if (cache == NULL) return -1;
    
    int errors = 0;
    
    while (__sync_lock_test_and_set(&cache->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 检查 partial slab */
    struct list_head *pos;
    list_for_each(pos, &cache->slabs_partial) {
        struct slab *slab = list_entry(pos, struct slab, list);
        if (slab->inuse < 0 || slab->inuse > slab->total) {
            errors++;
        }
        if (slab->cache != cache) {
            errors++;
        }
    }
    
    /* 检查 full slab */
    list_for_each(pos, &cache->slabs_full) {
        struct slab *slab = list_entry(pos, struct slab, list);
        if (slab->inuse != slab->total) {
            errors++;
        }
        if (slab->cache != cache) {
            errors++;
        }
    }
    
    /* 检查 free slab */
    list_for_each(pos, &cache->slabs_free) {
        struct slab *slab = list_entry(pos, struct slab, list);
        if (slab->inuse != 0) {
            errors++;
        }
        if (slab->cache != cache) {
            errors++;
        }
    }
    
    __sync_lock_release(&cache->lock);
    return errors;
}

/* Slab 内存泄漏检测 */
struct slab_leak_info {
    const char *cache_name;
    size_t allocated;
    size_t freed;
    size_t leaked;
};

void slab_check_leaks(void) {
    println("=== Slab Memory Leak Check ===");
    
    struct list_head *pos;
    list_for_each(pos, &slab_caches) {
        struct slab_cache *cache = list_entry(pos, struct slab_cache, cache_list);
        
        size_t allocated = cache->nr_objs;
        size_t free = cache->nr_free;
        size_t in_use = allocated - free;
        
        if (in_use > 0) {
            print("Cache: ");
            print(cache->name);
            print(" - In use: ");
            print_dec(in_use);
            print(" objects (");
            print_dec(in_use * cache->obj_size);
            println(" bytes)");
        }
    }
}

/* Slab 性能调优 */
void slab_tune_cache(struct slab_cache *cache, int batchcount, int limit) {
    if (cache == NULL) return;
    
    for (int i = 0; i < MAX_CPUS; i++) {
        struct cpu_cache *cc = &cache->cpu_cache[i];
        
        if (batchcount > 0) {
            cc->batchcount = batchcount;
        }
        
        if (limit > 0 && limit <= CPU_CACHE_MAX) {
            cc->limit = limit;
        }
    }
}

/* 获取 Slab 内存使用统计 */
size_t slab_get_memory_usage(void) {
    size_t total = 0;
    
    struct list_head *pos;
    list_for_each(pos, &slab_caches) {
        struct slab_cache *cache = list_entry(pos, struct slab_cache, cache_list);
        total += cache->nr_slabs * cache->objs_per_slab * cache->obj_size;
    }
    
    return total;
}

/* 获取 Slab 碎片率 */
int slab_get_fragmentation(void) {
    size_t total_objects = 0;
    size_t free_objects = 0;
    
    struct list_head *pos;
    list_for_each(pos, &slab_caches) {
        struct slab_cache *cache = list_entry(pos, struct slab_cache, cache_list);
        total_objects += cache->nr_objs;
        free_objects += cache->nr_free;
    }
    
    if (total_objects == 0) return 0;
    return (free_objects * 100) / total_objects;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：高级 Slab 增强功能
 * 
 * 特性：
 * - 对象生命周期跟踪
 * - NUMA 感知分配
 * - 动态参数调整
 * - 高级诊断报告
 * ═══════════════════════════════════════════════════════════════════════════ */

/* === 对象生命周期跟踪 === */

#define TRACK_MAX_OBJECTS    4096

struct object_track {
    void *obj;
    uint64_t alloc_time;
    uint64_t last_access;
    uint32_t access_count;
    int cache_idx;
    int active;
};

static struct object_track obj_tracks[TRACK_MAX_OBJECTS];
static uint32_t track_count;
static int tracking_enabled = 0;
static spinlock_t track_lock = SPINLOCK_INIT;

/* 启用对象跟踪 */
void slab_enable_tracking(int enable) {
    tracking_enabled = enable;
    if (enable) {
        track_count = 0;
        for (int i = 0; i < TRACK_MAX_OBJECTS; i++) {
            obj_tracks[i].active = 0;
        }
    }
}

/* 记录对象分配 */
void slab_track_alloc(void *obj, int cache_idx) {
    if (!tracking_enabled || obj == NULL) return;
    
    spin_lock(&track_lock);
    
    for (uint32_t i = 0; i < TRACK_MAX_OBJECTS; i++) {
        if (!obj_tracks[i].active) {
            obj_tracks[i].obj = obj;
            obj_tracks[i].alloc_time = get_tick_count();
            obj_tracks[i].last_access = obj_tracks[i].alloc_time;
            obj_tracks[i].access_count = 0;
            obj_tracks[i].cache_idx = cache_idx;
            obj_tracks[i].active = 1;
            track_count++;
            break;
        }
    }
    
    spin_unlock(&track_lock);
}

/* 记录对象访问 */
void slab_track_access(void *obj) {
    if (!tracking_enabled || obj == NULL) return;
    
    spin_lock(&track_lock);
    
    for (uint32_t i = 0; i < TRACK_MAX_OBJECTS; i++) {
        if (obj_tracks[i].active && obj_tracks[i].obj == obj) {
            obj_tracks[i].last_access = get_tick_count();
            obj_tracks[i].access_count++;
            break;
        }
    }
    
    spin_unlock(&track_lock);
}

/* 记录对象释放 */
void slab_track_free(void *obj) {
    if (!tracking_enabled || obj == NULL) return;
    
    spin_lock(&track_lock);
    
    for (uint32_t i = 0; i < TRACK_MAX_OBJECTS; i++) {
        if (obj_tracks[i].active && obj_tracks[i].obj == obj) {
            obj_tracks[i].active = 0;
            track_count--;
            break;
        }
    }
    
    spin_unlock(&track_lock);
}

/* 获取对象生命周期统计 */
void slab_get_lifecycle_stats(uint64_t *avg_lifetime, uint64_t *max_lifetime) {
    uint64_t total_lifetime = 0;
    uint64_t max_lt = 0;
    uint32_t count = 0;
    
    spin_lock(&track_lock);
    
    for (uint32_t i = 0; i < TRACK_MAX_OBJECTS; i++) {
        if (obj_tracks[i].active) {
            uint64_t lifetime = get_tick_count() - obj_tracks[i].alloc_time;
            total_lifetime += lifetime;
            if (lifetime > max_lt) max_lt = lifetime;
            count++;
        }
    }
    
    spin_unlock(&track_lock);
    
    if (avg_lifetime) *avg_lifetime = count > 0 ? total_lifetime / count : 0;
    if (max_lifetime) *max_lifetime = max_lt;
}

/* === NUMA 感知分配 === */

#define SLAB_MAX_NUMA_NODES    8

struct numa_slab_stats {
    uint64_t local_allocs;
    uint64_t remote_allocs;
    uint64_t migrations;
};

static struct numa_slab_stats numa_stats[SLAB_MAX_NUMA_NODES];
static int numa_preferred_node[SLAB_MAX_NUMA_NODES];

/* 初始化 NUMA */
void slab_numa_init(int node_count) {
    for (int i = 0; i < SLAB_MAX_NUMA_NODES && i < node_count; i++) {
        numa_stats[i].local_allocs = 0;
        numa_stats[i].remote_allocs = 0;
        numa_stats[i].migrations = 0;
        numa_preferred_node[i] = i;
    }
}

/* NUMA 感知分配 */
void *slab_alloc_numa(struct slab_cache *cache, int node, int flags) {
    /* 简化实现：先从本地节点分配 */
    void *obj = slab_alloc(cache, flags);
    
    if (obj != NULL && node >= 0 && node < SLAB_MAX_NUMA_NODES) {
        numa_stats[node].local_allocs++;
    }
    
    return obj;
}

/* 获取 NUMA 统计 */
void slab_get_numa_stats(int node, uint64_t *local, uint64_t *remote, uint64_t *migrations) {
    if (node >= 0 && node < SLAB_MAX_NUMA_NODES) {
        if (local) *local = numa_stats[node].local_allocs;
        if (remote) *remote = numa_stats[node].remote_allocs;
        if (migrations) *migrations = numa_stats[node].migrations;
    }
}

/* === 动态参数调整 === */

/* 根据使用模式调整缓存参数 */
void slab_auto_tune(struct slab_cache *cache) {
    if (cache == NULL) return;
    
    /* 分析每个 CPU 的使用模式 */
    for (int i = 0; i < smp_cpu_count; i++) {
        struct cpu_cache *cc = &cache->cpu_cache[i];
        
        /* 计算命中率 */
        uint64_t total = cc->alloc_count + cc->free_count;
        if (total < 100) continue;  /* 样本太少 */
        
        /* 如果分配多释放少，增加批量大小 */
        if (cc->alloc_count > cc->free_count * 2) {
            if (cc->batchcount < CPU_CACHE_BATCH * 2) {
                cc->batchcount++;
            }
        }
        
        /* 如果释放多分配少，减少批量大小 */
        if (cc->free_count > cc->alloc_count * 2) {
            if (cc->batchcount > CPU_CACHE_BATCH / 2) {
                cc->batchcount--;
            }
        }
    }
}

/* 全局自动调优 */
void slab_auto_tune_all(void) {
    struct list_head *pos;
    list_for_each(pos, &slab_caches) {
        struct slab_cache *cache = list_entry(pos, struct slab_cache, cache_list);
        slab_auto_tune(cache);
    }
}

/* === 高级统计 === */

struct slab_advanced_stats {
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t slabs_created;
    uint64_t slabs_destroyed;
    uint64_t objects_migrated;
};

static struct slab_advanced_stats adv_stats;

/* 记录高级统计 */
void slab_record_alloc_stat(int from_cache) {
    adv_stats.total_allocs++;
    if (from_cache) {
        adv_stats.cache_hits++;
    } else {
        adv_stats.cache_misses++;
    }
}

void slab_record_free_stat(void) {
    adv_stats.total_frees++;
}

void slab_record_slab_create(void) {
    adv_stats.slabs_created++;
}

void slab_record_slab_destroy(void) {
    adv_stats.slabs_destroyed++;
}

/* 获取高级统计 */
void slab_get_advanced_stats(struct slab_advanced_stats *stats) {
    if (stats != NULL) {
        *stats = adv_stats;
    }
}

/* 计算缓存命中率 */
int slab_get_cache_hit_rate(void) {
    uint64_t total = adv_stats.cache_hits + adv_stats.cache_misses;
    if (total == 0) return 0;
    return (adv_stats.cache_hits * 100) / total;
}

/* === Slab 健康检查 === */

struct slab_health {
    int fragmentation_score;    /* 0-100, 越高越差 */
    int efficiency_score;       /* 0-100, 越高越好 */
    int memory_waste;           /* 浪费的字节数 */
    int slab_count;             /* 总 slab 数 */
    int empty_slab_count;       /* 空 slab 数 */
    int recommended_action;     /* 建议操作 */
};

/* 执行健康检查 */
void slab_health_check(struct slab_health *health) {
    if (health == NULL) return;
    
    memset(health, 0, sizeof(*health));
    
    size_t total_objects = 0;
    size_t free_objects = 0;
    size_t total_memory = 0;
    size_t wasted_memory = 0;
    int empty_slabs = 0;
    int total_slabs = 0;
    
    struct list_head *pos;
    list_for_each(pos, &slab_caches) {
        struct slab_cache *cache = list_entry(pos, struct slab_cache, cache_list);
        
        total_objects += cache->nr_objs;
        free_objects += cache->nr_free;
        total_slabs += cache->nr_slabs;
        total_memory += cache->nr_slabs * cache->objs_per_slab * cache->obj_size;
        
        /* 计算空闲 slab */
        struct list_head *slab_pos;
        list_for_each(slab_pos, &cache->slabs_free) {
            empty_slabs++;
        }
        
        /* 计算浪费的内存 */
        wasted_memory += (cache->objs_per_slab * cache->obj_size) - 
                         (cache->nr_objs - cache->nr_free) * cache->obj_size;
    }
    
    /* 计算评分 */
    if (total_objects > 0) {
        health->fragmentation_score = (free_objects * 100) / total_objects;
        health->efficiency_score = 100 - health->fragmentation_score;
    }
    
    health->memory_waste = wasted_memory;
    health->slab_count = total_slabs;
    health->empty_slab_count = empty_slabs;
    
    /* 建议操作 */
    if (health->fragmentation_score > 70) {
        health->recommended_action = 1;  /* 建议收缩 */
    } else if (health->efficiency_score < 30) {
        health->recommended_action = 2;  /* 建议优化 */
    } else {
        health->recommended_action = 0;  /* 正常 */
    }
}

/* === Slab 内存回收 === */

/* 回收空闲内存 */
size_t slab_reclaim_memory(size_t target) {
    size_t reclaimed = 0;
    
    struct list_head *pos;
    list_for_each(pos, &slab_caches) {
        struct slab_cache *cache = list_entry(pos, struct slab_cache, cache_list);
        
        /* 收缩缓存 */
        int freed = slab_shrink(cache);
        reclaimed += freed * cache->objs_per_slab * cache->obj_size;
        
        if (reclaimed >= target) break;
    }
    
    return reclaimed;
}

/* === Slab 预热 === */

/* 预热缓存 */
int slab_warmup(struct slab_cache *cache, int count) {
    if (cache == NULL || count <= 0) return -1;
    
    int warmed = 0;
    
    for (int cpu = 0; cpu < smp_cpu_count; cpu++) {
        int filled = slab_prefill_cpu_cache(cache, cpu);
        warmed += filled;
        
        if (warmed >= count) break;
    }
    
    return warmed;
}

/* === Slab 对象重用 === */

/* 对象缓存重用 */
struct reuse_pool {
    void **objects;
    size_t capacity;
    size_t count;
    volatile int lock;
};

#define MAX_REUSE_POOLS    16
static struct reuse_pool reuse_pools[MAX_REUSE_POOLS];

/* 初始化重用池 */
int slab_init_reuse_pool(int pool_id, size_t capacity) {
    if (pool_id < 0 || pool_id >= MAX_REUSE_POOLS) return -1;
    
    struct reuse_pool *pool = &reuse_pools[pool_id];
    pool->objects = kmalloc(sizeof(void *) * capacity);
    if (pool->objects == NULL) return -1;
    
    pool->capacity = capacity;
    pool->count = 0;
    pool->lock = 0;
    
    return 0;
}

/* 添加对象到重用池 */
int slab_add_reuse(int pool_id, void *obj) {
    if (pool_id < 0 || pool_id >= MAX_REUSE_POOLS || obj == NULL) return -1;
    
    struct reuse_pool *pool = &reuse_pools[pool_id];
    
    while (__sync_lock_test_and_set(&pool->lock, 1)) {
        asm volatile("pause");
    }
    
    int added = 0;
    if (pool->count < pool->capacity) {
        pool->objects[pool->count++] = obj;
        added = 1;
    }
    
    __sync_lock_release(&pool->lock);
    return added;
}

/* 从重用池获取对象 */
void *slab_get_reuse(int pool_id) {
    if (pool_id < 0 || pool_id >= MAX_REUSE_POOLS) return NULL;
    
    struct reuse_pool *pool = &reuse_pools[pool_id];
    
    while (__sync_lock_test_and_set(&pool->lock, 1)) {
        asm volatile("pause");
    }
    
    void *obj = NULL;
    if (pool->count > 0) {
        obj = pool->objects[--pool->count];
    }
    
    __sync_lock_release(&pool->lock);
    return obj;
}

/* === 综合诊断报告 === */

void slab_print_comprehensive_report(void) {
    println("");
    println("╔════════════════════════════════════════════════════════════╗");
    println("║        Zratro OS Slab Allocator Report                     ║");
    println("╚════════════════════════════════════════════════════════════╝");
    
    /* 基本统计 */
    println("");
    println("=== 基本统计 ===");
    
    size_t total_memory = slab_get_memory_usage();
    print("总内存使用: ");
    print_dec(total_memory / 1024);
    println(" KB");
    
    print("碎片率: ");
    print_dec(slab_get_fragmentation());
    println(" %");
    
    print("缓存命中率: ");
    print_dec(slab_get_cache_hit_rate());
    println(" %");
    
    /* 高级统计 */
    println("");
    println("=== 高级统计 ===");
    
    struct slab_advanced_stats adv;
    slab_get_advanced_stats(&adv);
    
    print("总分配次数: ");
    print_dec(adv.total_allocs);
    println("");
    
    print("总释放次数: ");
    print_dec(adv.total_frees);
    println("");
    
    print("Slab 创建次数: ");
    print_dec(adv.slabs_created);
    println("");
    
    print("Slab 销毁次数: ");
    print_dec(adv.slabs_destroyed);
    println("");
    
    /* 健康检查 */
    println("");
    println("=== 健康检查 ===");
    
    struct slab_health health;
    slab_health_check(&health);
    
    print("效率评分: ");
    print_dec(health.efficiency_score);
    println("/100");
    
    print("碎片评分: ");
    print_dec(health.fragmentation_score);
    println("/100");
    
    print("总 Slab 数: ");
    print_dec(health.slab_count);
    println("");
    
    print("空 Slab 数: ");
    print_dec(health.empty_slab_count);
    println("");
    
    /* 对象生命周期 */
    println("");
    println("=== 对象生命周期 ===");
    
    uint64_t avg_lifetime, max_lifetime;
    slab_get_lifecycle_stats(&avg_lifetime, &max_lifetime);
    
    print("平均生命周期: ");
    print_dec(avg_lifetime / 1000000);
    println(" ms");
    
    print("最大生命周期: ");
    print_dec(max_lifetime / 1000000);
    println(" ms");
    
    /* 缓存详情 */
    println("");
    println("=== 缓存详情 ===");
    
    struct list_head *pos;
    int cache_count = 0;
    list_for_each(pos, &slab_caches) {
        struct slab_cache *cache = list_entry(pos, struct slab_cache, cache_list);
        
        print(cache->name);
        print(": ");
        print_dec(cache->nr_objs - cache->nr_free);
        print("/");
        print_dec(cache->nr_objs);
        print(" objects, ");
        print_dec(cache->nr_slabs);
        println(" slabs");
        
        cache_count++;
        if (cache_count >= 8) {
            println("... (更多缓存省略)");
            break;
        }
    }
    
    /* 建议 */
    println("");
    println("=== 优化建议 ===");
    
    if (health.recommended_action == 1) {
        println("- 碎片过高，建议执行 slab_shrink_all()");
    } else if (health.recommended_action == 2) {
        println("- 效率较低，建议检查对象大小配置");
    } else {
        println("- Slab 状态正常");
    }
    
    if (health.empty_slab_count > 5) {
        println("- 存在过多空闲 Slab，可考虑收缩");
    }
    
    println("");
    println("════════════════════════════════════════════════════════════");
}
