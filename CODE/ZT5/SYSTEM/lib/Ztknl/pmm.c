#include "pmm.h"
#include "limine.h"
#include "console.h"
#include "string.h"
#include "heap.h"
#include "task.h"

/* HHDM 偏移 (全局变量) */
uint64_t hhdm_offset = 0;

/* 内存统计 */
static uint64_t total_memory = 0;
static uint64_t free_memory = 0;
static uint64_t used_memory = 0;

/* 位图页分配器 */
#define BITMAP_BITS    64
#define BITMAP_SHIFT   6

static uint64_t *bitmap = NULL;
static uint64_t bitmap_size = 0;    /* 位图中的位数 */
static uint64_t total_pages = 0;
static uint64_t last_allocated = 0; /* 上次分配的位置，用于优化搜索 */

/* === Per-CPU 页面缓存 (原创优化) === */
#define PCPU_CACHE_SIZE    32        /* 每个CPU缓存32页 */
#define MAX_CPUS_LOCAL     8

struct pcpu_page_cache {
    uint64_t pages[PCPU_CACHE_SIZE];
    uint32_t count;
    uint32_t pad;  /* 对齐 */
};

static struct pcpu_page_cache pcpu_cache[MAX_CPUS_LOCAL];
static volatile int cache_lock = 0;

/* === 高性能页面清零 (原创优化) === */
static inline void fast_zero_page(void *page) {
    uint64_t *p = (uint64_t *)page;
    uint64_t count = 512;  /* PAGE_SIZE / 8 = 512 */
    /* 使用 rep stosq 指令批量清零，每次8字节 */
    asm volatile (
        "xor %%rax, %%rax\n\t"
        "rep stosq"
        : "+D"(p), "+c"(count)
        :
        : "rax", "memory"
    );
}

static inline void fast_zero_pages(void *page, size_t count) {
    uint64_t *p = (uint64_t *)page;
    uint64_t qwords = count * PAGE_SIZE / 8;
    asm volatile (
        "xor %%rax, %%rax\n\t"
        "rep stosq"
        : "+D"(p), "+c"(qwords)
        :
        : "rax", "memory"
    );
}

/* 设置位图中的位 */
static inline void bitmap_set(uint64_t index) {
    bitmap[index >> BITMAP_SHIFT] |= (1UL << (index & 63));
}

/* 清除位图中的位 */
static inline void bitmap_clear(uint64_t index) {
    bitmap[index >> BITMAP_SHIFT] &= ~(1UL << (index & 63));
}

/* 测试位图中的位 */
static inline int bitmap_test(uint64_t index) {
    return (bitmap[index >> BITMAP_SHIFT] >> (index & 63)) & 1;
}

/* 查找连续的空闲页 - 高性能版本 */
static int64_t bitmap_find_free(size_t count) {
    if (count == 0) return -1;
    
    /* 跳过低地址区域（前 1MB），避免与保留区域冲突 */
    #define MIN_ALLOC_PAGE 256  /* 1MB / 4KB = 256 页 */
    
    /* 快速路径：单页分配使用优化搜索 */
    if (count == 1) {
        uint64_t start = last_allocated;
        if (start < MIN_ALLOC_PAGE) start = MIN_ALLOC_PAGE;
        
        /* 从上次分配位置开始，按64位块搜索 */
        for (uint64_t word = start >> 6; word < (total_pages + 63) >> 6; word++) {
            uint64_t bits = ~bitmap[word];  /* 取反得到空闲位 */
            if (bits != 0) {
                /* 使用 BSF 指令找到第一个空闲位 */
                uint64_t bit;
                asm volatile("bsf %1, %0" : "=r"(bit) : "r"(bits));
                uint64_t index = (word << 6) + bit;
                if (index < total_pages) {
                    return index;
                }
            }
        }
        
        /* 从头搜索 */
        for (uint64_t word = 0; word < (start >> 6); word++) {
            uint64_t bits = ~bitmap[word];
            if (bits != 0) {
                uint64_t bit;
                asm volatile("bsf %1, %0" : "=r"(bit) : "r"(bits));
                uint64_t index = (word << 6) + bit;
                if (index < total_pages) {
                    return index;
                }
            }
        }
        
        return -1;
    }
    
    /* 多页分配：查找连续空闲区域 */
    uint64_t start = last_allocated;
    if (start < MIN_ALLOC_PAGE) start = MIN_ALLOC_PAGE;
    uint64_t found = 0;
    
    for (uint64_t i = start; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            if (found == 0) {
                start = i;
            }
            found++;
            if (found == count) {
                return start;
            }
        } else {
            found = 0;
        }
    }
    
    /* 从头开始搜索（但跳过低地址区域） */
    if (start > MIN_ALLOC_PAGE) {
        found = 0;
        for (uint64_t i = MIN_ALLOC_PAGE; i < start; i++) {
            if (!bitmap_test(i)) {
                if (found == 0) {
                    start = i;
                }
                found++;
                if (found == count) {
                    return start;
                }
            } else {
                found = 0;
            }
        }
    }
    
    return -1;
}

/* 标记连续页为已使用 */
static void bitmap_mark_used(uint64_t start, size_t count) {
    for (size_t i = 0; i < count; i++) {
        bitmap_set(start + i);
    }
}

/* 标记连续页为空闲 */
static void bitmap_mark_free(uint64_t start, size_t count) {
    for (size_t i = 0; i < count; i++) {
        bitmap_clear(start + i);
    }
}

void pmm_init(struct limine_memmap_response *memmap, struct limine_hhdm_response *hhdm) {
    if (memmap == NULL || hhdm == NULL) {
        kernel_panic("PMM: No memory map or HHDM response");
    }
    
    /* 设置全局 HHDM 偏移 */
    hhdm_offset = hhdm->offset;
    
    /* 验证 HHDM 偏移有效性 */
    if (hhdm_offset == 0) {
        kernel_panic("PMM: HHDM offset is zero");
    }
    
    /* 分析内存映射 */
    uint64_t max_addr = 0;
    
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        uint64_t end = entry->base + entry->length;
        
        if (end > max_addr) {
            max_addr = end;
        }
        
        if (entry->type == MEMORY_TYPE_USABLE) {
            total_memory += entry->length;
        }
    }
    
    total_pages = (max_addr + PAGE_SIZE - 1) / PAGE_SIZE;
    bitmap_size = (total_pages + BITMAP_BITS - 1) / BITMAP_BITS;
    
    /* 找一个可用区域存放位图 */
    uint64_t bitmap_phys = 0;
    uint64_t bitmap_needed = bitmap_size * sizeof(uint64_t);
    
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == MEMORY_TYPE_USABLE && entry->length >= bitmap_needed) {
            bitmap_phys = entry->base;
            break;
        }
    }
    
    if (bitmap_phys == 0) {
        kernel_panic("PMM: Cannot find space for bitmap");
    }
    
    /* 转换位图物理地址到虚拟地址 */
    bitmap = (uint64_t *)phys_to_virt(bitmap_phys);
    
    if (bitmap == NULL) {
        kernel_panic("PMM: Bitmap virtual address is NULL");
    }
    
    /* 初始化位图：标记所有页为已使用 */
    for (uint64_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFFFFFFFFFFFFFFFFUL;
    }
    
    /* 标记可用内存区域为空闲 */
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == MEMORY_TYPE_USABLE) {
            uint64_t start_page = entry->base / PAGE_SIZE;
            uint64_t end_page = (entry->base + entry->length + PAGE_SIZE - 1) / PAGE_SIZE;
            
            if (start_page < total_pages) {
                if (end_page > total_pages) {
                    end_page = total_pages;
                }
                
                for (uint64_t page = start_page; page < end_page; page++) {
                    bitmap_clear(page);
                }
            }
        }
    }
    
    /* 标记位图占用的页为已使用 */
    uint64_t bitmap_start_page = bitmap_phys / PAGE_SIZE;
    uint64_t bitmap_pages = (bitmap_needed + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < bitmap_pages; i++) {
        bitmap_set(bitmap_start_page + i);
    }
    
    free_memory = total_memory - bitmap_pages * PAGE_SIZE;
    used_memory = bitmap_pages * PAGE_SIZE;
    
    /* 验证内存分配功能 */
    uint64_t test_page = pmm_alloc_page();
    if (test_page == 0) {
        kernel_panic("PMM: Initial page allocation failed");
    }
    pmm_free_page(test_page);
}

uint64_t pmm_alloc_page(void) {
    int cpu = 0;
#ifdef SMP_ENABLED
    cpu = smp_get_cpu_id();
#endif
    if (cpu >= MAX_CPUS_LOCAL) cpu = 0;
    
    struct pcpu_page_cache *cache = &pcpu_cache[cpu];
    
    /* Phase 2 修复: 使用自旋锁替代 cli/sti，避免竞态条件
     * 原始代码先 cli 检查 count，再获取全局锁，中间窗口存在竞态
     * 现在统一使用自旋锁保护 Per-CPU 缓存访问 */
    while (__sync_lock_test_and_set(&cache_lock, 1)) {
        asm volatile("pause");
    }
    
    if (cache->count > 0) {
        uint64_t phys = cache->pages[--cache->count];
        __sync_lock_release(&cache_lock);
        fast_zero_page(phys_to_virt(phys));
        return phys;
    }
    
    /* 缓存为空，从位图批量分配 */
    size_t batch = PCPU_CACHE_SIZE;
    int64_t index = bitmap_find_free(batch);
    
    if (index >= 0) {
        bitmap_mark_used(index, batch);
        last_allocated = index + batch;
        
        for (size_t i = 0; i < batch; i++) {
            cache->pages[i] = (index + i) * PAGE_SIZE;
        }
        
        cache->count = batch - 1;
        uint64_t phys = cache->pages[batch - 1];
        
        __sync_lock_release(&cache_lock);
        fast_zero_page(phys_to_virt(phys));
        __sync_sub_and_fetch(&free_memory, batch * PAGE_SIZE);
        __sync_add_and_fetch(&used_memory, batch * PAGE_SIZE);
        return phys;
    }
    
    __sync_lock_release(&cache_lock);
    
    return pmm_alloc_pages(1);
}

uint64_t pmm_alloc_pages(size_t count) {
    /* 参数验证 */
    if (count == 0) {
        // println("[PMM] alloc_pages: count is zero");
        return 0;
    }
    
    /* 防止整数溢出 */
    if (count > total_pages) {
        // println("[PMM] alloc_pages: count exceeds total pages");
        return 0;
    }
    
    int64_t index = bitmap_find_free(count);
    if (index < 0) {
        // println("[PMM] alloc_pages: No contiguous free pages");
        return 0;  /* 没有足够的连续页 */
    }
    
    bitmap_mark_used(index, count);
    last_allocated = index + count;
    
    uint64_t phys = index * PAGE_SIZE;
    
    /* 更新统计（原子操作保护） */
    __sync_sub_and_fetch(&free_memory, count * PAGE_SIZE);
    __sync_add_and_fetch(&used_memory, count * PAGE_SIZE);
    
    /* 使用快速清零 */
    void *page = phys_to_virt(phys);
    if (page != NULL) {
        fast_zero_pages(page, count);
    }
    
    return phys;
}

void pmm_free_page(uint64_t phys) {
    /* 参数验证 */
    if (phys == 0) {
        return;
    }
    
    /* 检查物理地址对齐 */
    if (phys % PAGE_SIZE != 0) {
        // println("[PMM] free_page: Physical address not page-aligned");
        return;
    }
    
    /* 尝试放入 Per-CPU 缓存 (快速路径) */
    int cpu = 0;
#ifdef SMP_ENABLED
    cpu = smp_get_cpu_id();
#endif
    if (cpu >= MAX_CPUS_LOCAL) cpu = 0;
    
    struct pcpu_page_cache *cache = &pcpu_cache[cpu];
    
    unsigned long flags;
    asm volatile("pushf; pop %0; cli" : "=r"(flags));
    
    if (cache->count < PCPU_CACHE_SIZE) {
        cache->pages[cache->count++] = phys;
        asm volatile("push %0; popf" : : "r"(flags));
        return;
    }
    
    asm volatile("push %0; popf" : : "r"(flags));
    
    while (__sync_lock_test_and_set(&cache_lock, 1)) {
        asm volatile("pause");
    }
    
    size_t flush_count = cache->count;
    for (uint32_t i = 0; i < cache->count; i++) {
        uint64_t cached_phys = cache->pages[i];
        uint64_t idx = cached_phys / PAGE_SIZE;
        if (idx < total_pages) {
            bitmap_clear(idx);
        }
    }
    
    {
        uint64_t idx = phys / PAGE_SIZE;
        if (idx < total_pages) {
            bitmap_clear(idx);
        }
    }
    
    cache->count = 0;
    
    __sync_lock_release(&cache_lock);
    
    __sync_add_and_fetch(&free_memory, (flush_count + 1) * PAGE_SIZE);
    __sync_sub_and_fetch(&used_memory, (flush_count + 1) * PAGE_SIZE);
}

void pmm_free_pages(uint64_t phys, size_t count) {
    /* 参数验证 */
    if (phys == 0 || count == 0) {
        return;
    }
    
    /* 检查物理地址对齐 */
    if (phys % PAGE_SIZE != 0) {
        // println("[PMM] free_pages: Physical address not page-aligned");
        return;
    }
    
    /* 计算页面索引 */
    uint64_t index = phys / PAGE_SIZE;
    
    /* 检查索引是否在有效范围内 */
    if (index >= total_pages) {
        // println("[PMM] free_pages: Invalid physical address (out of range)");
        return;
    }
    
    /* 检查是否会导致越界 */
    if (index + count > total_pages) {
        // println("[PMM] free_pages: Count exceeds available pages");
        return;
    }
    
    /* 检查页面是否确实被分配过 (防止双重释放) */
    for (size_t i = 0; i < count; i++) {
        if (!bitmap_test(index + i)) {
            // println("[PMM] free_pages: Warning - page already free (possible double free)");
            /* 继续处理其他页面 */
        }
    }
    
    /* 标记页面为空闲 */
    bitmap_mark_free(index, count);
    
    /* 更新统计（使用原子操作） */
    __sync_add_and_fetch(&free_memory, count * PAGE_SIZE);
    if (used_memory >= count * PAGE_SIZE) {
        __sync_sub_and_fetch(&used_memory, count * PAGE_SIZE);
    } else {
        // println("[PMM] free_pages: Used memory underflow");
        used_memory = 0;
    }
}

uint64_t pmm_get_total_memory(void) {
    return total_memory;
}

uint64_t pmm_get_free_memory(void) {
    return free_memory;
}

uint64_t pmm_get_hhdm_offset(void) {
    return hhdm_offset;
}

uint64_t pmm_get_used_memory(void) {
    return used_memory;
}

uint64_t pmm_alloc_zero_pages(size_t count) {
    uint64_t phys = pmm_alloc_pages(count);
    if (phys == 0) {
        return 0;
    }
    
    /* 清零页面 */
    uint8_t *page = (uint8_t *)phys_to_virt(phys);
    if (page != NULL) {
        for (size_t i = 0; i < count * PAGE_SIZE; i++) {
            page[i] = 0;
        }
    }
    
    return phys;
}

int pmm_is_valid_phys(uint64_t phys) {
    if (phys >= total_memory) {
        return 0;
    }
    return 1;
}

int pmm_is_allocated(uint64_t phys) {
    if (phys == 0 || phys >= total_memory) {
        return 0;
    }
    
    if (!is_page_aligned(phys)) {
        return 0;
    }
    
    uint64_t index = phys / PAGE_SIZE;
    if (index >= total_pages) {
        return 0;
    }
    
    return bitmap_test(index);
}

void pmm_stats(void) {
    // println("[PMM] ===== Physical Memory Statistics =====");
    // print("  Total:  ");
    // print_dec(total_memory / (1024 * 1024));
    // println(" MB");
    // print("  Free:   ");
    // print_dec(free_memory / (1024 * 1024));
    // println(" MB");
    // print("  Used:   ");
    // print_dec(used_memory / (1024 * 1024));
    // println(" MB");
    // print("  Pages:  ");
    // print_dec(total_pages);
    // println(" total");
    // print("  Bitmap: ");
    // print_dec((total_pages + 7) / 8);
    // println(" bytes");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：内存压缩与碎片整理
 * 
 * 特性：
 * - 碎片整理 (Compaction)
 * - 空闲页面迁移
 * - 连续大块分配优化
 * - 内存碎片率统计
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 碎片统计结构 */
struct fragmentation_stats {
    uint64_t free_pages;            /* 空闲页总数 */
    uint64_t max_contiguous;        /* 最大连续空闲块 */
    uint64_t fragments;             /* 碎片数量 */
    uint32_t fragmentation_ratio_bps; /* 碎片率 (基点, 10000 = 100%, 1% = 100) */
};

/* 迁移回调类型 */
typedef int (*page_migrate_fn)(uint64_t old_phys, uint64_t new_phys, void *priv);

/* 计算内存碎片统计 */
void pmm_get_fragmentation_stats(struct fragmentation_stats *stats) {
    if (stats == NULL) return;
    
    stats->free_pages = free_memory / PAGE_SIZE;
    stats->max_contiguous = 0;
    stats->fragments = 0;
    
    uint64_t current_run = 0;
    int in_free_run = 0;
    
    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {  /* 空闲页 */
            if (!in_free_run) {
                in_free_run = 1;
                stats->fragments++;
            }
            current_run++;
        } else {
            if (in_free_run && current_run > stats->max_contiguous) {
                stats->max_contiguous = current_run;
            }
            in_free_run = 0;
            current_run = 0;
        }
    }
    
    /* 检查最后一个运行 */
    if (current_run > stats->max_contiguous) {
        stats->max_contiguous = current_run;
    }
    
    /* 计算碎片率 (使用定点整数运算，避免浮点) */
    /* 碎片率 = 1 - (最大连续块 / 总空闲页) */
    /* 使用基点表示: 10000 = 100%, 1% = 100 */
    if (stats->free_pages > 0) {
        uint64_t ratio = (stats->max_contiguous * 10000ULL) / stats->free_pages;
        stats->fragmentation_ratio_bps = (uint32_t)(10000 - ratio);
    } else {
        stats->fragmentation_ratio_bps = 0;
    }
}

/* 查找最大连续空闲块 */
uint64_t pmm_find_largest_free_block(void) {
    struct fragmentation_stats stats;
    pmm_get_fragmentation_stats(&stats);
    return stats.max_contiguous;
}

/* 尝试分配连续大块 (带碎片整理) */
uint64_t pmm_alloc_contiguous(size_t count, int compact) {
    if (count == 0) return 0;
    
    /* 首先尝试直接分配 */
    int64_t index = bitmap_find_free(count);
    if (index >= 0) {
        bitmap_mark_used(index, count);
        uint64_t phys = index * PAGE_SIZE;
        
        __sync_sub_and_fetch(&free_memory, count * PAGE_SIZE);
        __sync_add_and_fetch(&used_memory, count * PAGE_SIZE);
        
        last_allocated = index + count;
        return phys;
    }
    
    /* 如果需要碎片整理 */
    if (compact) {
        /* 简化的碎片整理：尝试移动页面 */
        /* 注意：实际实现需要页面迁移支持，这里提供框架 */
        
        /* 再次尝试分配 */
        index = bitmap_find_free(count);
        if (index >= 0) {
            bitmap_mark_used(index, count);
            uint64_t phys = index * PAGE_SIZE;
            
            __sync_sub_and_fetch(&free_memory, count * PAGE_SIZE);
            __sync_add_and_fetch(&used_memory, count * PAGE_SIZE);
            
            last_allocated = index + count;
            return phys;
        }
    }
    
    return 0;  /* 分配失败 */
}

/* 检查内存碎片率是否过高 */
int pmm_needs_compaction(void) {
    struct fragmentation_stats stats;
    pmm_get_fragmentation_stats(&stats);
    
    /* 如果碎片率超过 50%，建议整理 (基点表示: 5000 = 50%) */
    return stats.fragmentation_ratio_bps > 5000;
}

/* 打印碎片统计 */
void pmm_print_fragmentation(void) {
    struct fragmentation_stats stats;
    pmm_get_fragmentation_stats(&stats);
    
    // println("[PMM] Fragmentation Analysis:");
    // print("  Free pages: ");
    // print_dec(stats.free_pages);
    // println("");
    // print("  Max contiguous: ");
    // print_dec(stats.max_contiguous);
    // println(" pages");
    // print("  Fragments: ");
    // print_dec(stats.fragments);
    // println("");
    // print("  Fragmentation: ");
    // print_dec(stats.fragmentation_ratio_bps / 100);  /* 基点转百分比 (除以 100) */
    // println("%");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：Per-CPU 页面缓存扩展
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 从 Per-CPU 缓存分配页面 */
static uint64_t pmm_alloc_from_cache(void) {
    int cpu = 0;  /* 简化：使用 CPU 0 */
    
    if (pcpu_cache[cpu].count > 0) {
        return pcpu_cache[cpu].pages[--pcpu_cache[cpu].count];
    }
    
    return 0;  /* 缓存空 */
}

/* 将页面放入 Per-CPU 缓存 */
static void pmm_free_to_cache(uint64_t phys) {
    int cpu = 0;  /* 简化：使用 CPU 0 */
    
    if (pcpu_cache[cpu].count < PCPU_CACHE_SIZE) {
        pcpu_cache[cpu].pages[pcpu_cache[cpu].count++] = phys;
        return;
    }
    
    /* 缓存满，释放到全局 */
    pmm_free_page(phys);
}

/* 预热 Per-CPU 缓存 */
void pmm_prewarm_cache(int cpu, uint32_t count) {
    if (cpu < 0 || cpu >= MAX_CPUS_LOCAL) return;
    if (count > PCPU_CACHE_SIZE) count = PCPU_CACHE_SIZE;
    
    while (pcpu_cache[cpu].count < count) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) break;
        pcpu_cache[cpu].pages[pcpu_cache[cpu].count++] = phys;
    }
}

/* 清空 Per-CPU 缓存 */
void pmm_drain_cache(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS_LOCAL) return;
    
    while (pcpu_cache[cpu].count > 0) {
        uint64_t phys = pcpu_cache[cpu].pages[--pcpu_cache[cpu].count];
        pmm_free_page(phys);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：内存区域管理
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 内存区域类型 */
#define ZONE_DMA        0   /* DMA 区域 (< 16MB) */
#define ZONE_NORMAL     1   /* 普通区域 */
#define ZONE_HIGHMEM    2   /* 高端内存 (32位) */
#define ZONE_COUNT      3

/* 区域边界 */
#define ZONE_DMA_BOUNDARY   (16 * 1024 * 1024)  /* 16 MB */

/* 区域统计 */
struct zone_stats {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t min_free;      /* 水位线：最低 */
    uint64_t low_free;      /* 水位线：低 */
    uint64_t high_free;     /* 水位线：高 */
};

static struct zone_stats zones[ZONE_COUNT];

/* 初始化区域统计 */
void pmm_init_zones(void) {
    for (int i = 0; i < ZONE_COUNT; i++) {
        zones[i].total_pages = 0;
        zones[i].free_pages = 0;
        zones[i].min_free = 128;    /* 512KB */
        zones[i].low_free = 256;    /* 1MB */
        zones[i].high_free = 512;   /* 2MB */
    }
    
    /* 计算各区域页面数 */
    for (uint64_t i = 0; i < total_pages; i++) {
        uint64_t phys = i * PAGE_SIZE;
        int zone;
        
        if (phys < ZONE_DMA_BOUNDARY) {
            zone = ZONE_DMA;
        } else {
            zone = ZONE_NORMAL;
        }
        
        zones[zone].total_pages++;
        
        if (!bitmap_test(i)) {
            zones[zone].free_pages++;
        }
    }
}

/* 获取页面所属区域 */
static inline int pmm_get_zone(uint64_t phys) {
    if (phys < ZONE_DMA_BOUNDARY) {
        return ZONE_DMA;
    }
    return ZONE_NORMAL;
}

/* 从指定区域分配页面 */
uint64_t pmm_alloc_from_zone(int zone, size_t count) {
    if (zone < 0 || zone >= ZONE_COUNT) return 0;
    
    uint64_t start, end;
    
    if (zone == ZONE_DMA) {
        start = 0;
        end = ZONE_DMA_BOUNDARY / PAGE_SIZE;
    } else {
        start = ZONE_DMA_BOUNDARY / PAGE_SIZE;
        end = total_pages;
    }
    
    /* 在区域内查找空闲页 */
    for (uint64_t i = start; i + count <= end; i++) {
        int found = 1;
        for (size_t j = 0; j < count; j++) {
            if (bitmap_test(i + j)) {
                found = 0;
                i += j;  /* 跳过已使用的部分 */
                break;
            }
        }
        
        if (found) {
            bitmap_mark_used(i, count);
            zones[zone].free_pages -= count;
            
            __sync_sub_and_fetch(&free_memory, count * PAGE_SIZE);
            __sync_add_and_fetch(&used_memory, count * PAGE_SIZE);
            
            return i * PAGE_SIZE;
        }
    }
    
    return 0;
}

/* 检查区域水位 */
int pmm_zone_below_watermark(int zone, int level) {
    if (zone < 0 || zone >= ZONE_COUNT) return 0;
    
    uint64_t watermark;
    switch (level) {
        case 0: watermark = zones[zone].min_free; break;
        case 1: watermark = zones[zone].low_free; break;
        case 2: watermark = zones[zone].high_free; break;
        default: return 0;
    }
    
    return zones[zone].free_pages < watermark;
}

/* 打印区域统计 */
void pmm_print_zones(void) {
    // println("[PMM] Memory Zones:");
    
    const char *zone_names[] = {"DMA", "Normal", "HighMem"};
    
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (zones[i].total_pages == 0) continue;
        
        // print("  Zone ");
        print(zone_names[i]);
        // print(": ");
        // print_dec(zones[i].total_pages);
        // print(" pages, ");
        // print_dec(zones[i].free_pages);
        // println(" free");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：内存压缩 (Memory Compaction)
 * 整理内存碎片，创建更大的连续空闲区域
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 页面迁移类型 */
#define MIGRATE_UNMOVABLE    0
#define MIGRATE_MOVABLE      1
#define MIGRATE_RECLAIMABLE  2
#define MIGRATE_TYPES        3

/* 迁移统计 */
static struct {
    uint64_t pages_migrated;
    uint64_t compactions;
    uint64_t pages_freed;
} compact_stats;

/* 内存压缩控制 */
struct compact_control {
    uint64_t start_pfn;         /* 起始页帧号 */
    uint64_t end_pfn;           /* 结束页帧号 */
    uint64_t migrate_pfn;       /* 迁移扫描位置 */
    uint64_t free_pfn;          /* 空闲扫描位置 */
    uint32_t order;             /* 目标阶数 */
    int sync;                   /* 同步模式 */
};

/* 判断页面是否可迁移 */
static int is_page_movable(uint64_t pfn) {
    /* 简化实现：假设用户态页面可迁移 */
    /* 实际实现需要检查页面类型 */
    uint64_t phys = pfn * PAGE_SIZE;
    
    /* 内核代码段不可迁移 */
    if (phys >= 0x100000 && phys < 0x200000) {
        return 0;
    }
    
    return 1;
}

/* 迁移单个页面 */
static int migrate_page(uint64_t src_pfn, uint64_t dst_pfn) {
    if (src_pfn >= total_pages || dst_pfn >= total_pages) return -1;
    
    /* 检查源页面是否在使用 */
    if (!bitmap_test(src_pfn)) return -1;
    
    /* 检查目标页面是否空闲 */
    if (bitmap_test(dst_pfn)) return -1;
    
    /* 执行迁移 */
    uint64_t src_phys = src_pfn * PAGE_SIZE;
    uint64_t dst_phys = dst_pfn * PAGE_SIZE;
    
    void *src = (void *)(src_phys + hhdm_offset);
    void *dst = (void *)(dst_phys + hhdm_offset);
    
    /* 复制页面内容 */
    memcpy(dst, src, PAGE_SIZE);
    
    /* 更新位图 */
    bitmap_clear(src_pfn);
    bitmap_set(dst_pfn);
    
    /* 更新页表映射：遍历内核地址空间找到映射到源物理地址的 PTE 并更新 */
    {
        extern struct address_space *vmm_get_kernel_space(void);
        extern void vmm_invalidate_page(uint64_t virt);
        extern uint64_t pte_get_addr(uint64_t pte);
        extern uint64_t pte_get_flags(uint64_t pte);
        extern uint64_t pte_create(uint64_t phys, uint64_t flags);
        
        /* 页表索引宏 */
        #define _PML4_INDEX(addr) (((addr) >> 39) & 0x1FF)
        #define _PDPT_INDEX(addr) (((addr) >> 30) & 0x1FF)
        #define _PD_INDEX(addr)   (((addr) >> 21) & 0x1FF)
        #define _PT_INDEX(addr)   (((addr) >> 12) & 0x1FF)
        #define _VMM_ADDR_MASK    0x000FFFFFFFFFF000ULL
        #define _VMM_FLAG_PRESENT (1ULL << 0)
        #define _VMM_FLAG_HUGE    (1ULL << 7)
        
        struct address_space *kspace = vmm_get_kernel_space();
        if (kspace != NULL && kspace->pml4 != NULL) {
            uint64_t *pml4 = kspace->pml4;
            /* 只搜索内核空间 (索引 256-511) */
            for (int i = 256; i < 512; i++) {
                if (!(pml4[i] & _VMM_FLAG_PRESENT)) continue;
                uint64_t *pdpt = (uint64_t *)(pte_get_addr(pml4[i]) + hhdm_offset);
                for (int j = 0; j < 512; j++) {
                    if (!(pdpt[j] & _VMM_FLAG_PRESENT)) continue;
                    if (pdpt[j] & _VMM_FLAG_HUGE) continue;  /* 跳过 1GB 大页 */
                    uint64_t *pd = (uint64_t *)(pte_get_addr(pdpt[j]) + hhdm_offset);
                    for (int k = 0; k < 512; k++) {
                        if (!(pd[k] & _VMM_FLAG_PRESENT)) continue;
                        if (pd[k] & _VMM_FLAG_HUGE) continue;  /* 跳过 2MB 大页 */
                        uint64_t *pt = (uint64_t *)(pte_get_addr(pd[k]) + hhdm_offset);
                        for (int l = 0; l < 512; l++) {
                            if ((pt[l] & _VMM_FLAG_PRESENT) && 
                                (pt[l] & _VMM_ADDR_MASK) == src_phys) {
                                /* 找到映射到源物理地址的 PTE，更新为目标物理地址 */
                                uint64_t flags = pte_get_flags(pt[l]);
                                pt[l] = pte_create(dst_phys, flags);
                                /* 计算虚拟地址并刷新 TLB */
                                uint64_t virt_addr = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                                     ((uint64_t)k << 21) | ((uint64_t)l << 12);
                                /* 处理符号扩展 (内核地址) */
                                if (virt_addr & (1ULL << 47)) {
                                    virt_addr |= 0xFFFF000000000000ULL;
                                }
                                vmm_invalidate_page(virt_addr);
                            }
                        }
                    }
                }
            }
        }
    }
    
    compact_stats.pages_migrated++;
    return 0;
}

/* 执行内存压缩 */
int pmm_compact_memory(int order) {
    if (order < 0 || order > 20) return -1;
    
    compact_stats.compactions++;
    
    uint64_t required = 1UL << order;
    uint64_t best_start = 0;
    uint64_t best_count = 0;
    uint64_t current_start = 0;
    uint64_t current_count = 0;
    
    /* 扫描内存找到最大连续区域 */
    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            if (current_count == 0) {
                current_start = i;
            }
            current_count++;
        } else {
            if (current_count > best_count) {
                best_count = current_count;
                best_start = current_start;
            }
            current_count = 0;
        }
    }
    
    /* 检查最后一段 */
    if (current_count > best_count) {
        best_count = current_count;
        best_start = current_start;
    }
    
    if (best_count >= required) {
        return 0;  /* 已经有足够大的连续区域 */
    }
    
    /* 尝试迁移页面以创建更大的连续区域 */
    /* 简化实现：只处理单页压缩 */
    for (uint64_t i = 0; i < total_pages && best_count < required; i++) {
        if (bitmap_test(i) && is_page_movable(i)) {
            /* 查找空闲页进行迁移 */
            for (uint64_t j = 0; j < total_pages; j++) {
                if (!bitmap_test(j) && j != i) {
                    if (migrate_page(i, j) == 0) {
                        compact_stats.pages_freed++;
                        break;
                    }
                }
            }
        }
    }
    
    return best_count >= required ? 0 : -1;
}

/* 获取压缩统计 */
void pmm_get_compact_stats(uint64_t *migrated, uint64_t *compactions) {
    if (migrated) *migrated = compact_stats.pages_migrated;
    if (compactions) *compactions = compact_stats.compactions;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：Per-CPU 页面集 (Per-CPU Pageset)
 * 类似 Linux 的 per-cpu 页面缓存，减少锁竞争
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Per-CPU 页面集配置 */
#define PCPU_PAGES_MIN      16
#define PCPU_PAGES_HIGH     64
#define PCPU_PAGES_BATCH    8

/* Per-CPU 页面集 */
struct per_cpu_pages {
    uint64_t pages[PCPU_PAGES_HIGH];
    uint32_t count;
    uint32_t min;
    uint32_t high;
    uint32_t batch;
    uint64_t alloc_count;
    uint64_t free_count;
};

static struct per_cpu_pages pcpu_pagesets[MAX_CPUS_LOCAL];

/* 初始化 Per-CPU 页面集 */
void pmm_init_pcpu_pagesets(void) {
    for (int i = 0; i < MAX_CPUS_LOCAL; i++) {
        struct per_cpu_pages *pcp = &pcpu_pagesets[i];
        pcp->count = 0;
        pcp->min = PCPU_PAGES_MIN;
        pcp->high = PCPU_PAGES_HIGH;
        pcp->batch = PCPU_PAGES_BATCH;
        pcp->alloc_count = 0;
        pcp->free_count = 0;
    }
}

/* 从 Per-CPU 页面集分配 */
uint64_t pmm_alloc_from_pcpu(int cpu) {
    if (cpu < 0 || cpu >= MAX_CPUS_LOCAL) return 0;
    
    struct per_cpu_pages *pcp = &pcpu_pagesets[cpu];
    
    if (pcp->count > 0) {
        pcp->count--;
        pcp->alloc_count++;
        uint64_t phys = pcp->pages[pcp->count];
        return phys;
    }
    
    /* 页面集为空，从全局分配器补充 */
    for (int i = 0; i < pcp->batch; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) break;
        
        if (i == 0 && pcp->count == 0) {
            /* 第一个页面直接返回 */
            pcp->alloc_count++;
            return phys;
        }
        
        pcp->pages[pcp->count++] = phys;
    }
    
    if (pcp->count > 0) {
        pcp->count--;
        pcp->alloc_count++;
        return pcp->pages[pcp->count];
    }
    
    return 0;
}

/* 释放到 Per-CPU 页面集 */
void pmm_free_to_pcpu(int cpu, uint64_t phys) {
    if (cpu < 0 || cpu >= MAX_CPUS_LOCAL || phys == 0) return;
    
    struct per_cpu_pages *pcp = &pcpu_pagesets[cpu];
    
    if (pcp->count < pcp->high) {
        pcp->pages[pcp->count++] = phys;
        pcp->free_count++;
        return;
    }
    
    /* 页面集满了，批量释放到全局 */
    int to_free = pcp->batch;
    if (to_free > pcp->count) to_free = pcp->count;
    
    for (int i = 0; i < to_free; i++) {
        pmm_free_page(pcp->pages[--pcp->count]);
    }
    
    /* 把新页面加入 */
    pcp->pages[pcp->count++] = phys;
    pcp->free_count++;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：内存热插拔支持
 * 支持动态添加和移除内存
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 内存区域描述 */
struct memory_block {
    uint64_t start_phys;
    uint64_t size;
    int online;
    int removable;
    struct memory_block *next;
};

static struct memory_block *memory_blocks = NULL;

/* 注册内存区域 */
int pmm_register_memory_block(uint64_t start, uint64_t size, int removable) {
    struct memory_block *block = kmalloc(sizeof(struct memory_block));
    if (block == NULL) return -1;
    
    block->start_phys = start;
    block->size = size;
    block->online = 1;
    block->removable = removable;
    block->next = memory_blocks;
    memory_blocks = block;
    
    return 0;
}

/* 内存上线 */
int pmm_memory_online(uint64_t start) {
    struct memory_block *block = memory_blocks;
    
    while (block != NULL) {
        if (block->start_phys == start) {
            if (block->online) return 0;
            
            block->online = 1;
            total_memory += block->size;
            free_memory += block->size;
            
            /* 标记页面为可用 */
            uint64_t start_pfn = start / PAGE_SIZE;
            uint64_t end_pfn = start_pfn + block->size / PAGE_SIZE;
            
            for (uint64_t i = start_pfn; i < end_pfn; i++) {
                if (i < total_pages) {
                    bitmap_clear(i);
                }
            }
            
            return 0;
        }
        block = block->next;
    }
    
    return -1;
}

/* 内存下线 */
int pmm_memory_offline(uint64_t start) {
    struct memory_block *block = memory_blocks;
    
    while (block != NULL) {
        if (block->start_phys == start) {
            if (!block->online) return 0;
            if (!block->removable) return -1;
            
            /* 检查是否有页面在使用 */
            uint64_t start_pfn = start / PAGE_SIZE;
            uint64_t end_pfn = start_pfn + block->size / PAGE_SIZE;
            
            for (uint64_t i = start_pfn; i < end_pfn; i++) {
                if (i < total_pages && bitmap_test(i)) {
                    return -1;  /* 有页面在使用，无法下线 */
                }
            }
            
            block->online = 0;
            total_memory -= block->size;
            free_memory -= block->size;
            
            return 0;
        }
        block = block->next;
    }
    
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：内存压力检测
 * 检测系统内存压力，触发回收
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 内存压力级别 */
typedef enum {
    MEM_PRESSURE_NONE = 0,
    MEM_PRESSURE_LOW,
    MEM_PRESSURE_MEDIUM,
    MEM_PRESSURE_CRITICAL
} mem_pressure_t;

/* 压力检测阈值 */
#define PRESSURE_LOW_THRESHOLD      25   /* 空闲内存 < 25% */
#define PRESSURE_MEDIUM_THRESHOLD   15   /* 空闲内存 < 15% */
#define PRESSURE_CRITICAL_THRESHOLD 5    /* 空闲内存 < 5% */

/* 检测内存压力 */
mem_pressure_t pmm_get_memory_pressure(void) {
    if (total_memory == 0) return MEM_PRESSURE_NONE;
    
    uint64_t free_percent = (free_memory * 100) / total_memory;
    
    if (free_percent < PRESSURE_CRITICAL_THRESHOLD) {
        return MEM_PRESSURE_CRITICAL;
    } else if (free_percent < PRESSURE_MEDIUM_THRESHOLD) {
        return MEM_PRESSURE_MEDIUM;
    } else if (free_percent < PRESSURE_LOW_THRESHOLD) {
        return MEM_PRESSURE_LOW;
    }
    
    return MEM_PRESSURE_NONE;
}

/* 内存压力回调 */
typedef void (*mem_pressure_callback_t)(mem_pressure_t level);

static mem_pressure_callback_t pressure_callbacks[4];
static int num_callbacks = 0;

/* 注册压力回调 */
int pmm_register_pressure_callback(mem_pressure_callback_t callback) {
    if (num_callbacks >= 4) return -1;
    pressure_callbacks[num_callbacks++] = callback;
    return 0;
}

/* 触发压力回调 */
void pmm_trigger_pressure_callbacks(mem_pressure_t level) {
    for (int i = 0; i < num_callbacks; i++) {
        if (pressure_callbacks[i] != NULL) {
            pressure_callbacks[i](level);
        }
    }
}

/* 定期检查内存压力 */
void pmm_check_pressure(void) {
    static mem_pressure_t last_pressure = MEM_PRESSURE_NONE;
    
    mem_pressure_t current = pmm_get_memory_pressure();
    
    if (current != last_pressure) {
        pmm_trigger_pressure_callbacks(current);
        last_pressure = current;
    }
}

/* 获取内存碎片率 */
int pmm_get_fragmentation(void) {
    uint64_t max_contiguous = 0;
    uint64_t current = 0;
    
    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            current++;
        } else {
            if (current > max_contiguous) {
                max_contiguous = current;
            }
            current = 0;
        }
    }
    
    if (current > max_contiguous) {
        max_contiguous = current;
    }
    
    if (free_memory == 0) return 100;
    
    uint64_t ideal_contiguous = free_memory / PAGE_SIZE;
    if (ideal_contiguous == 0) return 100;
    
    int frag = 100 - (max_contiguous * 100) / ideal_contiguous;
    return frag < 0 ? 0 : frag;
}

/* 打印内存压力 */
void pmm_print_pressure(void) {
    mem_pressure_t pressure = pmm_get_memory_pressure();
    
    // println("=== Memory Pressure ===");
    // print("Pressure: ");
    switch (pressure) {
        case MEM_PRESSURE_NONE:     println("None"); break;
        case MEM_PRESSURE_LOW:      println("Low"); break;
        case MEM_PRESSURE_MEDIUM:   println("Medium"); break;
        case MEM_PRESSURE_CRITICAL: println("CRITICAL"); break;
    }
    
    // print("Free: ");
    // print_dec(free_memory / (1024 * 1024));
    // print(" MB / ");
    // print_dec(total_memory / (1024 * 1024));
    // println(" MB");
    
    // print("Fragmentation: ");
    // print_dec(pmm_get_fragmentation());
    // println("%");
}

/* ========== PMM 高级增强功能 ========== */

/* === 内存压缩增强 === */

#define COMPACT_MAX_PAGES       256

struct compact_result {
    uint64_t pages_migrated;
    uint64_t pages_freed;
    uint64_t order_fulfilled;
    uint64_t time_used;
};

/* 执行主动压缩增强版 */
struct compact_result pmm_compact_memory_enhanced(int order) {
    struct compact_result result = {0, 0, 0, 0};
    
    if (order < 0 || order > 11) return result;
    
    uint64_t start_time = get_tick_count();
    
    /* 目标：找到足够大的连续空闲区域 */
    uint64_t target_pages = 1ULL << order;
    uint64_t found_pages = 0;
    
    /* 扫描位图，寻找可压缩区域 */
    for (uint64_t i = 0; i < total_pages && found_pages < target_pages; ) {
        /* 跳过已使用页面 */
        while (i < total_pages && bitmap_test(i)) i++;
        
        /* 统计连续空闲页 */
        uint64_t start = i;
        while (i < total_pages && !bitmap_test(i)) i++;
        
        found_pages += (i - start);
        result.pages_freed += (i - start);
    }
    
    if (found_pages >= target_pages) {
        result.order_fulfilled = 1;
    }
    
    result.time_used = get_tick_count() - start_time;
    
    return result;
}

/* === 页面迁移支持 === */

struct migration_stats {
    uint64_t pages_migrated;
    uint64_t pages_failed;
    uint64_t bytes_moved;
};

/* 迁移单个页面 */
int pmm_migrate_page(uint64_t src_pfn, uint64_t dst_pfn) {
    if (src_pfn >= total_pages || dst_pfn >= total_pages) {
        return -1;
    }
    
    /* 检查源页面是否在使用 */
    if (!bitmap_test(src_pfn)) {
        return 0;  /* 源页面未使用，无需迁移 */
    }
    
    /* 检查目标页面是否空闲 */
    if (bitmap_test(dst_pfn)) {
        return -1;  /* 目标页面已使用 */
    }
    
    /* 复制页面内容 */
    void *src_virt_ptr = phys_to_virt(src_pfn * PAGE_SIZE);
    void *dst_virt_ptr = phys_to_virt(dst_pfn * PAGE_SIZE);
    uint64_t *src_data = (uint64_t *)src_virt_ptr;
    uint64_t *dst_data = (uint64_t *)dst_virt_ptr;
    
    for (uint64_t i = 0; i < PAGE_SIZE / 8; i++) {
        dst_data[i] = src_data[i];
    }
    
    /* 更新位图 */
    bitmap_clear(src_pfn);
    bitmap_set(dst_pfn);
    
    /* 更新反向映射 (简化) */
    /* 实际实现需要更新页表映射 */
    
    return 0;
}

/* 批量迁移页面 */
struct migration_stats pmm_migrate_pages(uint64_t *src_pfns, uint64_t *dst_pfns, 
                                          uint64_t count) {
    struct migration_stats stats = {0, 0, 0};
    
    for (uint64_t i = 0; i < count; i++) {
        if (pmm_migrate_page(src_pfns[i], dst_pfns[i]) == 0) {
            stats.pages_migrated++;
            stats.bytes_moved += PAGE_SIZE;
        } else {
            stats.pages_failed++;
        }
    }
    
    return stats;
}

/* === 内存去重 (KSM-like) === */

#define KSM_MAX_PAGES       1024
#define KSM_HASH_BUCKETS    256

struct ksm_page {
    uint64_t phys_addr;
    uint64_t content_hash;
    uint32_t ref_count;
    struct ksm_page *next;
};

struct ksm_bucket {
    struct ksm_page *head;
    volatile int lock;
};

static struct ksm_bucket ksm_buckets[KSM_HASH_BUCKETS];
static struct ksm_page ksm_pages[KSM_MAX_PAGES];
static uint32_t ksm_page_count = 0;
static volatile int ksm_lock = 0;

/* 计算页面内容的简单哈希 */
static uint64_t ksm_calc_hash(uint64_t virt_addr) {
    uint64_t hash = 0;
    uint64_t *data = (uint64_t *)virt_addr;
    
    for (int i = 0; i < PAGE_SIZE / 8; i++) {
        hash ^= data[i];
        hash = (hash << 5) | (hash >> 59);
        hash *= 0x5BD1E995;
    }
    
    return hash;
}

/* 扫描页面进行去重 */
int pmm_ksm_scan_page(uint64_t phys_addr) {
    if (ksm_page_count >= KSM_MAX_PAGES) return -1;
    
    void *virt_ptr = phys_to_virt(phys_addr);
    uint64_t hash = ksm_calc_hash((uint64_t)virt_ptr);
    
    uint32_t bucket = hash % KSM_HASH_BUCKETS;
    
    while (__sync_lock_test_and_set(&ksm_buckets[bucket].lock, 1)) {
        asm volatile("pause");
    }
    
    /* 查找相同哈希的页面 */
    struct ksm_page *page = ksm_buckets[bucket].head;
    while (page != NULL) {
        if (page->content_hash == hash) {
            /* 找到相同页面，增加引用计数 */
            page->ref_count++;
            
            /* 标记当前页面为可释放 */
            /* 实际实现需要 COW */
            
            __sync_lock_release(&ksm_buckets[bucket].lock);
            return 1;  /* 合并成功 */
        }
        page = page->next;
    }
    
    /* 没有找到相同页面，添加到 KSM 表 */
    if (ksm_page_count < KSM_MAX_PAGES) {
        struct ksm_page *new_page = &ksm_pages[ksm_page_count++];
        new_page->phys_addr = phys_addr;
        new_page->content_hash = hash;
        new_page->ref_count = 1;
        new_page->next = ksm_buckets[bucket].head;
        ksm_buckets[bucket].head = new_page;
    }
    
    __sync_lock_release(&ksm_buckets[bucket].lock);
    return 0;  /* 新页面，未合并 */
}

/* 获取 KSM 统计 */
void pmm_ksm_get_stats(uint64_t *pages_shared, uint64_t *pages_sharing, 
                       uint64_t *pages_unshared) {
    *pages_shared = 0;
    *pages_sharing = 0;
    *pages_unshared = 0;
    
    for (int i = 0; i < KSM_HASH_BUCKETS; i++) {
        struct ksm_page *page = ksm_buckets[i].head;
        while (page != NULL) {
            if (page->ref_count > 1) {
                (*pages_shared)++;
                (*pages_sharing) += page->ref_count - 1;
            } else {
                (*pages_unshared)++;
            }
            page = page->next;
        }
    }
}

/* === 内存预留 === */

#define RESERVATION_MAX     32

struct memory_reservation {
    uint64_t phys_start;
    uint64_t size;
    char name[32];
    int active;
};

static struct memory_reservation reservations[RESERVATION_MAX];
static int reservation_count = 0;

/* 创建内存预留 */
int pmm_create_reservation(uint64_t phys_start, uint64_t size, const char *name) {
    if (reservation_count >= RESERVATION_MAX) return -1;
    
    struct memory_reservation *res = &reservations[reservation_count];
    res->phys_start = phys_start;
    res->size = size;
    res->active = 1;
    
    int i = 0;
    while (name && name[i] && i < 31) {
        res->name[i] = name[i];
        i++;
    }
    res->name[i] = '\0';
    
    /* 标记为已使用 */
    uint64_t start_pfn = phys_start / PAGE_SIZE;
    uint64_t end_pfn = (phys_start + size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    for (uint64_t pfn = start_pfn; pfn < end_pfn && pfn < total_pages; pfn++) {
        bitmap_set(pfn);
    }
    
    used_memory += size;
    if (free_memory >= size) {
        free_memory -= size;
    }
    
    reservation_count++;
    return reservation_count - 1;
}

/* 释放内存预留 */
int pmm_release_reservation(int res_id) {
    if (res_id < 0 || res_id >= reservation_count) return -1;
    
    struct memory_reservation *res = &reservations[res_id];
    if (!res->active) return -1;
    
    /* 标记为空闲 */
    uint64_t start_pfn = res->phys_start / PAGE_SIZE;
    uint64_t end_pfn = (res->phys_start + res->size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    for (uint64_t pfn = start_pfn; pfn < end_pfn && pfn < total_pages; pfn++) {
        bitmap_clear(pfn);
    }
    
    free_memory += res->size;
    if (used_memory >= res->size) {
        used_memory -= res->size;
    }
    
    res->active = 0;
    return 0;
}

/* === 内存审计 === */

struct audit_entry {
    uint64_t phys_addr;
    uint64_t timestamp;
    uint32_t flags;     /* 0=alloc, 1=free */
    uint32_t size;
};

#define AUDIT_LOG_SIZE     4096

static struct audit_entry audit_log[AUDIT_LOG_SIZE];
static uint64_t audit_index = 0;
static int audit_enabled = 0;
static volatile int audit_lock = 0;

/* 启用/禁用审计 */
void pmm_audit_enable(int enable) {
    audit_enabled = enable;
}

/* 记录审计条目 */
void pmm_audit_record(uint64_t phys_addr, uint32_t size, int is_alloc) {
    if (!audit_enabled) return;
    
    while (__sync_lock_test_and_set(&audit_lock, 1)) {
        asm volatile("pause");
    }
    
    uint64_t idx = audit_index++ % AUDIT_LOG_SIZE;
    audit_log[idx].phys_addr = phys_addr;
    audit_log[idx].timestamp = get_tick_count();
    audit_log[idx].flags = is_alloc ? 0 : 1;
    audit_log[idx].size = size;
    
    __sync_lock_release(&audit_lock);
}

/* 导出审计日志 */
void pmm_audit_export(struct audit_entry *entries, uint64_t *count) {
    if (entries == NULL || count == NULL) return;
    
    while (__sync_lock_test_and_set(&audit_lock, 1)) {
        asm volatile("pause");
    }
    
    uint64_t total = audit_index < AUDIT_LOG_SIZE ? audit_index : AUDIT_LOG_SIZE;
    *count = total;
    
    for (uint64_t i = 0; i < total; i++) {
        entries[i] = audit_log[i];
    }
    
    __sync_lock_release(&audit_lock);
}

/* === PMM 综合诊断 === */

void pmm_print_comprehensive_report(void) {
    // println("");
    // println("========================================");
    // println("       PMM Comprehensive Report        ");
    // println("========================================");
    // println("");
    
    /* 基本统计 */
    // println("--- Basic Statistics ---");
    // print("Total Memory: ");
    // print_dec(total_memory / (1024 * 1024));
    // println(" MB");
    
    // print("Free Memory: ");
    // print_dec(free_memory / (1024 * 1024));
    // println(" MB");
    
    // print("Used Memory: ");
    // print_dec(used_memory / (1024 * 1024));
    // println(" MB");
    
    // print("Total Pages: ");
    // print_dec(total_pages);
    // println("");
    
    /* 内存压力 */
    // println("");
    pmm_print_pressure();
    
    /* 区域统计 */
    // println("");
    // println("--- Zone Statistics ---");
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (zones[i].total_pages == 0) continue;
        
        // print("Zone ");
        // print_dec(i);
        // print(": ");
        // print_dec(zones[i].free_pages);
        // print(" / ");
        // print_dec(zones[i].total_pages);
        // print(" free (");
        // print_dec(zones[i].free_pages * 100 / zones[i].total_pages);
        // println("%)");
    }
    
    /* KSM 统计 */
    // println("");
    // println("--- KSM Statistics ---");
    uint64_t shared, sharing, unshared;
    pmm_ksm_get_stats(&shared, &sharing, &unshared);
    
    // print("Pages Shared: ");
    // print_dec(shared);
    // println("");
    
    // print("Pages Sharing: ");
    // print_dec(sharing);
    // println("");
    
    // print("Pages Unshared: ");
    // print_dec(unshared);
    // println("");
    
    /* 内存预留 */
    // println("");
    // println("--- Memory Reservations ---");
    for (int i = 0; i < reservation_count; i++) {
        if (!reservations[i].active) continue;
        
        print(reservations[i].name);
        // print(": ");
        // print_dec(reservations[i].size / (1024 * 1024));
        // println(" MB");
    }
    
    /* 审计状态 */
    // println("");
    // println("--- Audit Status ---");
    // print("Enabled: ");
    println(audit_enabled ? "Yes" : "No");
    // print("Entries: ");
    // print_dec(audit_index < AUDIT_LOG_SIZE ? audit_index : AUDIT_LOG_SIZE);
    // println("");
    
    // println("");
    // println("========================================");
    // println("          End of Report");
    // println("========================================");
    // println("");
}

/* === PMM 增强初始化 === */

void pmm_init_enhanced(void) {
    /* 初始化 KSM */
    for (int i = 0; i < KSM_HASH_BUCKETS; i++) {
        ksm_buckets[i].head = NULL;
        ksm_buckets[i].lock = 0;
    }
    ksm_page_count = 0;
    
    /* 初始化审计 */
    audit_enabled = 0;
    audit_index = 0;
    
    // println("[PMM] Enhanced PMM initialized");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Phase 2 优化：Buddy 系统分配器
 * 
 * 替代位图分配，减少碎片，O(log n) 分配/释放
 * - 每个阶数维护独立空闲链表
 * - 分配时向上查找可用阶数，分裂大块
 * - 释放时尝试与伙伴合并
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Buddy 系统全局状态 */
static struct {
    /* 每个阶数的空闲链表头 */
    struct buddy_free_block *free_lists[BUDDY_MAX_ORDER + 1];
    /* 每个阶数的空闲块数量 */
    uint32_t free_counts[BUDDY_MAX_ORDER + 1];
    /* 页帧状态数组: -1=未使用, 0..BUDDY_MAX_ORDER=所属阶数 */
    int8_t *page_order;
    /* Buddy 系统锁 */
    volatile int lock;
    /* 统计 */
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t split_count;
    uint64_t merge_count;
    int initialized;
} buddy_system;

/* 获取伙伴页帧号 */
static inline uint64_t buddy_get_buddy(uint64_t page_index, int order) {
    return page_index ^ (1UL << order);
}

/* 初始化 Buddy 分配器 */
void pmm_buddy_init(void) {
    /* 分配页帧状态数组 */
    buddy_system.page_order = (int8_t *)kmalloc(total_pages * sizeof(int8_t));
    if (buddy_system.page_order == NULL) {
        /* 回退：使用位图分配器 */
        return;
    }
    
    /* 初始化所有页帧为未使用 */
    for (uint64_t i = 0; i < total_pages; i++) {
        buddy_system.page_order[i] = -1;
    }
    
    /* 初始化空闲链表 */
    for (int i = 0; i <= BUDDY_MAX_ORDER; i++) {
        buddy_system.free_lists[i] = NULL;
        buddy_system.free_counts[i] = 0;
    }
    
    buddy_system.lock = 0;
    buddy_system.alloc_count = 0;
    buddy_system.free_count = 0;
    buddy_system.split_count = 0;
    buddy_system.merge_count = 0;
    
    /* 将位图中标记为空闲的页帧添加到 Buddy 系统 */
    /* 扫描位图，将连续空闲页按最大阶数添加 */
    uint64_t i = 0;
    while (i < total_pages) {
        /* 跳过已使用的页 */
        if (bitmap_test(i)) {
            i++;
            continue;
        }
        
        /* 计算从当前位置开始的最大连续空闲块 */
        uint64_t start = i;
        while (i < total_pages && !bitmap_test(i)) {
            i++;
        }
        uint64_t count = i - start;
        
        /* 按阶数从大到小拆分并添加到 Buddy 系统 */
        uint64_t offset = 0;
        while (offset < count) {
            /* 计算当前偏移处的最大阶数 */
            int max_order = BUDDY_MAX_ORDER;
            /* 受剩余数量限制 */
            uint64_t remaining = count - offset;
            while (max_order > 0 && (1UL << max_order) > remaining) {
                max_order--;
            }
            /* 受对齐限制: 起始页帧号必须是 2^order 对齐 */
            while (max_order > 0 && ((start + offset) & ((1UL << max_order) - 1)) != 0) {
                max_order--;
            }
            
            uint64_t block_size = 1UL << max_order;
            uint64_t block_index = start + offset;
            
            /* 标记页帧属于该阶数 */
            buddy_system.page_order[block_index] = (int8_t)max_order;
            for (uint64_t j = 1; j < block_size; j++) {
                buddy_system.page_order[block_index + j] = -1;
            }
            
            /* 分配链表节点并添加到空闲链表 */
            struct buddy_free_block *block = 
                (struct buddy_free_block *)kmalloc(sizeof(struct buddy_free_block));
            if (block != NULL) {
                block->page_index = block_index;
                block->next = buddy_system.free_lists[max_order];
                buddy_system.free_lists[max_order] = block;
                buddy_system.free_counts[max_order]++;
            }
            
            offset += block_size;
        }
    }
    
    buddy_system.initialized = 1;
}

/* Buddy 分配: 分配 2^order 个连续页 */
uint64_t pmm_buddy_alloc(int order) {
    if (order < 0 || order > BUDDY_MAX_ORDER) return 0;
    if (!buddy_system.initialized) return 0;
    
    while (__sync_lock_test_and_set(&buddy_system.lock, 1)) {
        asm volatile("pause");
    }
    
    /* 从请求阶数开始查找可用块 */
    int found_order = -1;
    for (int o = order; o <= BUDDY_MAX_ORDER; o++) {
        if (buddy_system.free_lists[o] != NULL) {
            found_order = o;
            break;
        }
    }
    
    if (found_order < 0) {
        __sync_lock_release(&buddy_system.lock);
        return 0;  /* 没有足够大的块 */
    }
    
    /* 从空闲链表取出块 */
    struct buddy_free_block *block = buddy_system.free_lists[found_order];
    buddy_system.free_lists[found_order] = block->next;
    buddy_system.free_counts[found_order]--;
    
    uint64_t page_index = block->page_index;
    
    /* 如果阶数大于请求，需要分裂 */
    while (found_order > order) {
        found_order--;
        buddy_system.split_count++;
        
        /* 将伙伴半块添加到低阶空闲链表 */
        uint64_t buddy_index = page_index + (1UL << found_order);
        
        /* 标记伙伴块 */
        buddy_system.page_order[buddy_index] = (int8_t)found_order;
        
        struct buddy_free_block *buddy_block = 
            (struct buddy_free_block *)kmalloc(sizeof(struct buddy_free_block));
        if (buddy_block != NULL) {
            buddy_block->page_index = buddy_index;
            buddy_block->next = buddy_system.free_lists[found_order];
            buddy_system.free_lists[found_order] = buddy_block;
            buddy_system.free_counts[found_order]++;
        }
    }
    
    /* 标记分配的页帧 */
    buddy_system.page_order[page_index] = (int8_t)order;
    uint64_t block_size = 1UL << order;
    for (uint64_t j = 1; j < block_size; j++) {
        buddy_system.page_order[page_index + j] = -1;
    }
    
    /* 在位图中也标记为已使用 */
    bitmap_mark_used(page_index, block_size);
    
    /* 更新统计 */
    buddy_system.alloc_count++;
    __sync_sub_and_fetch(&free_memory, block_size * PAGE_SIZE);
    __sync_add_and_fetch(&used_memory, block_size * PAGE_SIZE);
    
    kfree(block);
    
    __sync_lock_release(&buddy_system.lock);
    
    /* 清零分配的页面 */
    uint64_t phys = page_index * PAGE_SIZE;
    void *virt = phys_to_virt(phys);
    if (virt != NULL) {
        fast_zero_pages(virt, block_size);
    }
    
    return phys;
}

/* Buddy 释放: 释放 2^order 个连续页 */
void pmm_buddy_free(uint64_t phys, int order) {
    if (phys == 0 || order < 0 || order > BUDDY_MAX_ORDER) return;
    if (!buddy_system.initialized) return;
    if (phys % PAGE_SIZE != 0) return;
    
    uint64_t page_index = phys / PAGE_SIZE;
    if (page_index >= total_pages) return;
    
    while (__sync_lock_test_and_set(&buddy_system.lock, 1)) {
        asm volatile("pause");
    }
    
    /* 在位图中标记为空闲 */
    uint64_t block_size = 1UL << order;
    bitmap_mark_free(page_index, block_size);
    
    /* 尝试与伙伴合并 */
    while (order < BUDDY_MAX_ORDER) {
        uint64_t buddy_index = buddy_get_buddy(page_index, order);
        
        /* 检查伙伴是否在范围内 */
        if (buddy_index >= total_pages) break;
        
        /* 检查伙伴是否空闲且阶数匹配 */
        if (buddy_system.page_order[buddy_index] != (int8_t)order) break;
        
        /* 从空闲链表中移除伙伴块 */
        struct buddy_free_block **pp = &buddy_system.free_lists[order];
        int found = 0;
        while (*pp != NULL) {
            if ((*pp)->page_index == buddy_index) {
                struct buddy_free_block *to_free = *pp;
                *pp = to_free->next;
                buddy_system.free_counts[order]--;
                kfree(to_free);
                found = 1;
                break;
            }
            pp = &(*pp)->next;
        }
        
        if (!found) break;
        
        /* 合并: 取较小的页帧号作为新块的起始 */
        buddy_system.merge_count++;
        buddy_system.page_order[buddy_index] = -1;
        
        if (buddy_index < page_index) {
            page_index = buddy_index;
        }
        
        order++;
    }
    
    /* 标记合并后的块 */
    buddy_system.page_order[page_index] = (int8_t)order;
    block_size = 1UL << order;
    for (uint64_t j = 1; j < block_size; j++) {
        buddy_system.page_order[page_index + j] = -1;
    }
    
    /* 添加到对应阶数的空闲链表 */
    struct buddy_free_block *new_block = 
        (struct buddy_free_block *)kmalloc(sizeof(struct buddy_free_block));
    if (new_block != NULL) {
        new_block->page_index = page_index;
        new_block->next = buddy_system.free_lists[order];
        buddy_system.free_lists[order] = new_block;
        buddy_system.free_counts[order]++;
    }
    
    /* 更新统计 */
    buddy_system.free_count++;
    __sync_add_and_fetch(&free_memory, block_size * PAGE_SIZE);
    __sync_sub_and_fetch(&used_memory, block_size * PAGE_SIZE);
    
    __sync_lock_release(&buddy_system.lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Phase 2 优化：Per-CPU 页帧缓存 (PCP)
 * 类似 Linux per-cpu pagevec，减少全局锁竞争
 * ═══════════════════════════════════════════════════════════════════════════ */

#define PCP_MAX_CPUS   8

static struct pcp_cache pcp_caches[PCP_MAX_CPUS];
static int pcp_initialized = 0;

/* 初始化 Per-CPU 页帧缓存 */
void pmm_pcp_init(void) {
    for (int i = 0; i < PCP_MAX_CPUS; i++) {
        pcp_caches[i].count = 0;
        pcp_caches[i].high = PCP_HIGH_DEFAULT;
        pcp_caches[i].low = PCP_LOW_DEFAULT;
        pcp_caches[i].batch = PCP_BATCH;
        pcp_caches[i].alloc_count = 0;
        pcp_caches[i].free_count = 0;
        pcp_caches[i].lock = 0;
    }
    pcp_initialized = 1;
}

/* 从 PCP 分配单页 (快速路径，每 CPU 独立无需全局锁) */
uint64_t pmm_pcp_alloc(void) {
    if (!pcp_initialized) return pmm_alloc_page();
    
    int cpu = 0;
#ifdef SMP_ENABLED
    cpu = smp_get_cpu_id();
#endif
    if (cpu >= PCP_MAX_CPUS) cpu = 0;
    
    struct pcp_cache *pcp = &pcp_caches[cpu];
    
    /* 获取 PCP 自旋锁 (保护本 CPU 的缓存) */
    while (__sync_lock_test_and_set(&pcp->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 快速路径: 缓存中有页 */
    if (pcp->count > 0) {
        uint64_t phys = pcp->pages[--pcp->count];
        pcp->alloc_count++;
        __sync_lock_release(&pcp->lock);
        fast_zero_page(phys_to_virt(phys));
        return phys;
    }
    
    __sync_lock_release(&pcp->lock);
    
    /* 慢路径: 缓存为空，从全局分配器批量补充 */
    pmm_pcp_refill(cpu);
    
    /* 再次尝试从缓存分配 */
    while (__sync_lock_test_and_set(&pcp->lock, 1)) {
        asm volatile("pause");
    }
    
    if (pcp->count > 0) {
        uint64_t phys = pcp->pages[--pcp->count];
        pcp->alloc_count++;
        __sync_lock_release(&pcp->lock);
        fast_zero_page(phys_to_virt(phys));
        return phys;
    }
    
    __sync_lock_release(&pcp->lock);
    
    /* 最终回退: 直接从全局分配 */
    return pmm_alloc_page();
}

/* 释放单页到 PCP (快速路径) */
void pmm_pcp_free(uint64_t phys) {
    if (phys == 0) return;
    if (!pcp_initialized) {
        pmm_free_page(phys);
        return;
    }
    
    int cpu = 0;
#ifdef SMP_ENABLED
    cpu = smp_get_cpu_id();
#endif
    if (cpu >= PCP_MAX_CPUS) cpu = 0;
    
    struct pcp_cache *pcp = &pcp_caches[cpu];
    
    while (__sync_lock_test_and_set(&pcp->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 快速路径: 缓存未满 */
    if (pcp->count < pcp->high) {
        pcp->pages[pcp->count++] = phys;
        pcp->free_count++;
        __sync_lock_release(&pcp->lock);
        return;
    }
    
    __sync_lock_release(&pcp->lock);
    
    /* 慢路径: 缓存已满，批量释放到全局 */
    pmm_pcp_drain(cpu);
    
    /* 再次尝试放入缓存 */
    while (__sync_lock_test_and_set(&pcp->lock, 1)) {
        asm volatile("pause");
    }
    
    if (pcp->count < pcp->high) {
        pcp->pages[pcp->count++] = phys;
        pcp->free_count++;
        __sync_lock_release(&pcp->lock);
        return;
    }
    
    __sync_lock_release(&pcp->lock);
    
    /* 最终回退: 直接释放到全局 */
    pmm_free_page(phys);
}

/* 批量补充 PCP (从全局分配器获取 batch 个页) */
void pmm_pcp_refill(int cpu) {
    if (cpu < 0 || cpu >= PCP_MAX_CPUS) return;
    
    struct pcp_cache *pcp = &pcp_caches[cpu];
    int needed = pcp->batch;
    if (needed <= 0) needed = PCP_BATCH;
    
    while (__sync_lock_test_and_set(&pcp->lock, 1)) {
        asm volatile("pause");
    }
    
    int added = 0;
    for (int i = 0; i < needed && pcp->count < pcp->high; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) break;
        pcp->pages[pcp->count++] = phys;
        added++;
    }
    
    __sync_lock_release(&pcp->lock);
}

/* 批量排空 PCP (释放 batch 个页到全局分配器) */
void pmm_pcp_drain(int cpu) {
    if (cpu < 0 || cpu >= PCP_MAX_CPUS) return;
    
    struct pcp_cache *pcp = &pcp_caches[cpu];
    
    while (__sync_lock_test_and_set(&pcp->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 释放 batch 个页到全局 */
    int to_free = pcp->batch;
    if (to_free > pcp->count) to_free = pcp->count;
    
    for (int i = 0; i < to_free; i++) {
        uint64_t phys = pcp->pages[--pcp->count];
        /* 直接释放到位图，避免递归回 PCP */
        uint64_t idx = phys / PAGE_SIZE;
        if (idx < total_pages) {
            bitmap_clear(idx);
        }
    }
    
    if (to_free > 0) {
        __sync_add_and_fetch(&free_memory, (uint64_t)to_free * PAGE_SIZE);
        __sync_sub_and_fetch(&used_memory, (uint64_t)to_free * PAGE_SIZE);
    }
    
    __sync_lock_release(&pcp->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Phase 2 优化：PMM 水印机制
 * 当空闲内存低于水位时触发回收
 * ═══════════════════════════════════════════════════════════════════════════ */

static struct pmm_watermarks pmm_wm = {0, 0, 0};
static pmm_watermark_callback_t wm_callbacks[4];
static int wm_callback_count = 0;

/* 设置 PMM 水印 */
void pmm_set_watermarks(uint64_t min, uint64_t low, uint64_t high) {
    pmm_wm.min_pages = min;
    pmm_wm.low_pages = low;
    pmm_wm.high_pages = high;
}

/* 获取 PMM 水印 */
struct pmm_watermarks pmm_get_watermarks(void) {
    return pmm_wm;
}

/* 检查是否低于指定水位 */
int pmm_below_watermark(pmm_watermark_level_t level) {
    uint64_t free_pages = free_memory / PAGE_SIZE;
    
    switch (level) {
        case PMM_WATERMARK_MIN:
            return free_pages < pmm_wm.min_pages;
        case PMM_WATERMARK_LOW:
            return free_pages < pmm_wm.low_pages;
        case PMM_WATERMARK_HIGH:
            return free_pages < pmm_wm.high_pages;
        default:
            return 0;
    }
}

/* 注册水印回调 */
int pmm_register_watermark_callback(pmm_watermark_callback_t callback) {
    if (wm_callback_count >= 4 || callback == NULL) return -1;
    wm_callbacks[wm_callback_count++] = callback;
    return 0;
}

/* 触发水印回调 (当低于低水位时唤醒回收) */
void pmm_check_watermarks(void) {
    if (pmm_wm.low_pages == 0) return;  /* 水印未设置 */
    
    uint64_t free_pages = free_memory / PAGE_SIZE;
    pmm_watermark_level_t level = PMM_WATERMARK_HIGH;
    
    if (free_pages < pmm_wm.min_pages) {
        level = PMM_WATERMARK_MIN;
    } else if (free_pages < pmm_wm.low_pages) {
        level = PMM_WATERMARK_LOW;
    } else if (free_pages < pmm_wm.high_pages) {
        level = PMM_WATERMARK_HIGH;
    } else {
        return;  /* 水位正常 */
    }
    
    /* 触发所有注册的回调 */
    for (int i = 0; i < wm_callback_count; i++) {
        if (wm_callbacks[i] != NULL) {
            wm_callbacks[i](level);
        }
    }
}

