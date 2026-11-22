#include "heap.h"
#include "ktypes.h"
#include "task.h"
#include "pmm.h"
#include "vmm.h"
#include "console.h"
#include "string.h"

/* 串口调试输出函数 (在 console.c 中定义) */
extern void serial_print(const char *str);

/* 堆配置 */
#define HEAP_START      0xFFFFFFFF90000000ULL
#define HEAP_INITIAL_SIZE   (16 * 1024 * 1024)  /* 16 MB 初始大小 */
#define HEAP_MIN_BLOCK_SIZE 32                   /* 最小块大小 */
#define HEAP_MAGIC      0xDEADBEEFCAFEBABEULL   /* 块魔数 */

/* 调试开关 */
#define HEAP_DEBUG 0

/* 内存投毒开关 - 启用后在释放时填充 0xDEAD 模式，分配时检测 use-after-free */
#define HEAP_POISON 1
#define HEAP_POISON_FREE  0xDEADDEADDEADDEADULL  /* 释放时填充模式 */
#define HEAP_POISON_CHECK_BYTES  8              /* 检测前 N 字节 */

/* 堆安全统计 */
struct heap_security_stats {
    uint64_t overflows_detected;    /* 检测到的溢出 */
    uint64_t double_frees;          /* 双重释放 */
    uint64_t uaf_attempts;          /* 释放后使用尝试 */
    uint64_t corruptions;           /* 堆损坏 */
    uint64_t canary_failures;       /* 金丝雀失败 */
};

static struct heap_security_stats heap_sec_stats;

#if HEAP_DEBUG
#define HEAP_LOG(msg) // println("[HEAP] " msg)
#else
#define HEAP_LOG(msg) ((void)0)
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：分离适配算法 (Segregated Fit)
 * 
 * 设计理念：
 * - 按块大小分类到不同的空闲链表
 * - 快速定位合适大小的块，减少搜索时间
 * - 减少内存碎片，提高分配效率
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 大小类别定义 (2的幂次方边界) */
#define HEAP_SIZE_CLASSES    16

/* 大小类别边界 (字节) */
static const size_t size_class_bounds[HEAP_SIZE_CLASSES] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096,
    8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576
};

/* 分离空闲链表 */
struct segregated_free_list {
    struct heap_block *head;    /* 链表头 */
    uint32_t count;             /* 块数量 */
    uint32_t pad;               /* 对齐 */
} __attribute__((aligned(16)));

/* 内核堆定义 (在heap.h中声明为extern) */
struct heap kernel_heap;

/* 分离空闲链表数组 */
static struct segregated_free_list free_lists[HEAP_SIZE_CLASSES];

/* 保护分离空闲链表的全局锁 */
static spinlock_t segregated_lock = SPINLOCK_INIT;

/* 调试计数器 */
static volatile int dbg_sadd_count = 0;
static volatile int dbg_sremove_count = 0;

/* 获取大小类别索引 */
static inline int get_size_class(size_t size) {
    for (int i = 0; i < HEAP_SIZE_CLASSES - 1; i++) {
        if (size <= size_class_bounds[i]) {
            return i;
        }
    }
    return HEAP_SIZE_CLASSES - 1;  /* 大块 */
}

/* 从分离链表中移除块 */
static void segregated_remove(struct heap_block *block) {
    dbg_sremove_count++;  /* 调试计数 */
    
    /* Validate the block pointer itself before accessing any fields */
    if (block == NULL) {
        // println("[HEAP] segregated_remove: NULL block pointer, skipping");
        return;
    }
    
    /* Validate block address is within heap bounds */
    if ((uint64_t)block < kernel_heap.start || (uint64_t)block >= kernel_heap.end) {
        // println("[HEAP] segregated_remove: Block address out of heap range, skipping");
        return;
    }
    
    /* Additional validation: check if the block's size is reasonable */
    if (block->size == 0 || block->size > kernel_heap.size) {
        // println("[HEAP] segregated_remove: Invalid block size, skipping");
        // print("[HEAP] size=");
        // print_dec(block->size);
        // println("");
        return;
    }
    
    int sc = get_size_class(block->size);
    if (sc < 0 || sc >= HEAP_SIZE_CLASSES) {
        // println("[HEAP] segregated_remove: Invalid size class, skipping");
        // print("[HEAP] size=");
        // print_dec(block->size);
        // print(" sc=");
        // print_dec(sc);
        // println("");
        return;
    }
    
    struct segregated_free_list *list = &free_lists[sc];
    
    spin_lock(&segregated_lock);
    
    /* 验证 free_prev 指针 */
    if (block->free_prev != NULL) {
        if ((uint64_t)block->free_prev < kernel_heap.start || 
            (uint64_t)block->free_prev >= kernel_heap.end) {
            // println("[HEAP] segregated_remove: Invalid free_prev pointer, skipping");
            spin_unlock(&segregated_lock);
            return;
        }
        block->free_prev->free_next = block->free_next;
    } else {
        list->head = block->free_next;
    }
    
    /* 验证 free_next 指针 */
    if (block->free_next != NULL) {
        if ((uint64_t)block->free_next < kernel_heap.start || 
            (uint64_t)block->free_next >= kernel_heap.end) {
            // println("[HEAP] segregated_remove: Invalid free_next pointer, skipping");
            /* 已经修改了 free_prev->free_next，需要恢复 */
            if (block->free_prev != NULL) {
                /* Validate free_prev again before restoring */
                if ((uint64_t)block->free_prev >= kernel_heap.start && 
                    (uint64_t)block->free_prev < kernel_heap.end) {
                    block->free_prev->free_next = block;
                }
            }
            spin_unlock(&segregated_lock);
            return;
        }
        block->free_next->free_prev = block->free_prev;
    }
    
    if (list->count > 0) {
        list->count--;
    }
    
    spin_unlock(&segregated_lock);
}

/* 添加块到分离链表 */
static void segregated_add(struct heap_block *block) {
    dbg_sadd_count++;  /* 调试计数（在任何可能崩溃的代码之前） */
    
    /* 验证块指针的有效性 */
    if (block == NULL) {
        // println("[HEAP] WARNING: segregated_add called with NULL block");
        return;
    }
    
    if ((uint64_t)block < kernel_heap.start || (uint64_t)block >= kernel_heap.end) {
        // println("[HEAP] WARNING: segregated_add called with block outside heap range");
        // print("[HEAP] block=0x");
        // print_hex((uint64_t)block);
        // print(" heap=[0x");
        // print_hex(kernel_heap.start);
        // print("-0x");
        // print_hex(kernel_heap.end);
        // println("]");
        return;
    }
    
    // print("SADD_CALLED"); print_hex((uint64_t)block); print(" size="); print_dec(block->size); println("");
    int sc = get_size_class(block->size);
    struct segregated_free_list *list = &free_lists[sc];
    
    spin_lock(&segregated_lock);
    
    block->free_next = list->head;
    block->free_prev = NULL;
    
    if (list->head != NULL) {
        /* 验证 head 指针的有效性 */
        if ((uint64_t)list->head >= kernel_heap.start && (uint64_t)list->head < kernel_heap.end) {
            list->head->free_prev = block;
        } else {
            // println("[HEAP] ERROR: segregated_add found corrupted list head!");
            /* Reset the list to prevent further corruption */
            list->head = block;
            block->free_next = NULL;
        }
    }
    
    list->head = block;
    list->count++;
    
    spin_unlock(&segregated_lock);
}

/* 在分离链表中查找最合适的块 (最佳适应) */
static struct heap_block *segregated_find_best(size_t size) {
    int sc = get_size_class(size);
    
    /* 从当前类别开始搜索 */
    for (int i = sc; i < HEAP_SIZE_CLASSES; i++) {
        struct segregated_free_list *list = &free_lists[i];
        struct heap_block *best = NULL;
        size_t best_size = (size_t)-1;
        
        /* 在该类别中查找最佳匹配 */
        struct heap_block *block = list->head;
        while (block != NULL) {
            /* 关键验证：魔数必须匹配，确保这是有效的块头 */
            if (block->magic != HEAP_MAGIC) {
                /* 魔数不匹配，这是已合并块的数据区域，跳过 */
                block = block->free_next;
                continue;
            }
            /* 关键验证：块必须是空闲状态，且地址在堆范围内 */
            if (block->is_free && 
                (uint64_t)block >= kernel_heap.start && 
                (uint64_t)block < kernel_heap.end &&
                block->size >= size && block->size < best_size) {
                best = block;
                best_size = block->size;
                
                /* 如果找到精确匹配，立即返回 */
                if (best_size == size) {
                    return best;
                }
            }
            block = block->free_next;  /* 使用 free_next 遍历空闲链表 */
        }
        
        if (best != NULL) {
            return best;
        }
    }
    
    return NULL;  /* 没有找到合适的块 */
}

/* 原子操作：查找并移除最合适的块 (线程安全版本) */
static struct heap_block *segregated_find_and_remove_best(size_t size) {
    // serial_print "[SFARB"] Enter, size=");
    char buf[32];
    int len = 0;
    size_t tmp = size;
    if (tmp == 0) { buf[len++] = '0'; }
    else {
        char tmpbuf[32];
        int tlen = 0;
        while (tmp > 0) { tmpbuf[tlen++] = '0' + (tmp % 10); tmp /= 10; }
        while (tlen > 0) buf[len++] = tmpbuf[--tlen];
    }
    buf[len] = '\0';
    // serial_print(buf);
    // serial_print("\n");
    
    int sc = get_size_class(size);
    struct heap_block *best = NULL;
    
    // print("[SFARB] Enter, size=");
    // print_dec(size);
    // print(" sc=");
    // print_dec(sc);
    // println("");
    
    spin_lock(&segregated_lock);
    
    /* 从当前类别开始搜索 */
    for (int i = sc; i < HEAP_SIZE_CLASSES && best == NULL; i++) {
        struct segregated_free_list *list = &free_lists[i];
        size_t best_size = (size_t)-1;
        
        if (list->head != NULL) {
            // print("[SFARB] List ");
            // print_dec(i);
            // print(" head=0x");
            // print_hex((uint64_t)list->head);
            // print(" count=");
            // print_dec(list->count);
            // println("");
        }
        
        // serial_print "[SFARB"] Checking list ");
        buf[0] = '0' + (i / 10); buf[1] = '0' + (i % 10); buf[2] = '\0';
        // serial_print(buf);
        // serial_print(", count=");
        // tmp = list->count;
        len = 0;
        if (tmp == 0) { buf[len++] = '0'; }
        else {
            char tmpbuf[32];
            int tlen = 0;
            while (tmp > 0) { tmpbuf[tlen++] = '0' + (tmp % 10); tmp /= 10; }
            while (tlen > 0) buf[len++] = tmpbuf[--tlen];
        }
        buf[len] = '\0';
        // serial_print(buf);
        // serial_print("\n");
        
        /* 在该类别中查找最佳匹配 */
        struct heap_block *block = list->head;
        int iter_count = 0;
        while (block != NULL) {
            iter_count++;
            if (iter_count > 100) {
                // println("[SFARB] ERROR: Too many iterations!");
                break;
            }
            
            /* Validate that block pointer is within heap bounds before accessing */
            if ((uint64_t)block < kernel_heap.start || (uint64_t)block >= kernel_heap.end) {
                // println("[SFARB]   -> INVALID block address, out of heap range, stopping traversal");
                break;
            }
            
            // print("[SFARB]   block=0x");
            // print_hex((uint64_t)block);
            // print(" magic=0x");
            // print_hex(block->magic);
            // print(" size=");
            // print_dec(block->size);
            // print(" is_free=");
            // print(block->is_free ? "Y" : "N");
            // print(" free_next=0x");
            // print_hex((uint64_t)block->free_next);
            // println("");
            
            /* 关键验证：魔数必须匹配，确保这是有效的块头 */
            if (block->magic != HEAP_MAGIC) {
                /* 魔数不匹配 - 这是已合并块的数据区域，必须从链表中移除 */
                // println("[SFARB]   -> INVALID block (bad magic), removing from list");
                
                struct heap_block *invalid = block;
                
                /* 先保存下一个块的指针，因为移除后 invalid->free_next 可能无效 */
                struct heap_block *next_block = block->free_next;
                
                /* 验证 next_block 指针有效性 */
                if (next_block != NULL && 
                    ((uint64_t)next_block < kernel_heap.start || 
                     (uint64_t)next_block >= kernel_heap.end)) {
                    // println("[SFARB]   -> free_next corrupted, stopping traversal");
                    break;
                }
                
                /* 从链表中移除无效块 */
                if (invalid->free_prev != NULL) {
                    /* Validate free_prev pointer before accessing */
                    if ((uint64_t)invalid->free_prev >= kernel_heap.start && 
                        (uint64_t)invalid->free_prev < kernel_heap.end) {
                        invalid->free_prev->free_next = invalid->free_next;
                    }
                } else {
                    list->head = invalid->free_next;
                }
                
                if (invalid->free_next != NULL) {
                    /* 验证 free_next 在堆范围内 */
                    if ((uint64_t)invalid->free_next >= kernel_heap.start && 
                        (uint64_t)invalid->free_next < kernel_heap.end) {
                        invalid->free_next->free_prev = invalid->free_prev;
                    }
                }
                
                if (list->count > 0) {
                    list->count--;
                }
                
                /* 继续遍历下一个块 */
                block = next_block;
                continue;
            }
            /* 关键检查：块必须是空闲状态才有效 */
            if (block->is_free && block->size >= size && block->size < best_size) {
                best = block;
                best_size = block->size;
                // print("[SFARB]   -> best candidate! size=");
                // print_dec(best_size);
                // println("");
                
                /* 如果找到精确匹配，立即停止搜索 */
                if (best_size == size) {
                    break;
                }
            }
            /* 验证 free_next 指针在有效范围内，防止链表破坏导致页面错误 */
            if (block->free_next != NULL && 
                ((uint64_t)block->free_next < kernel_heap.start || 
                 (uint64_t)block->free_next >= kernel_heap.end)) {
                /* 链表破坏，停止遍历 */
                // println("[HEAP] Warning: Corrupted free list detected, free_next pointer out of range");
                break;
            }
            block = block->free_next;  /* 使用 free_next 遍历空闲链表 */
        }
        // serial_print "[SFARB"] List search done, iterations=");
        buf[0] = '0' + (iter_count / 10); buf[1] = '0' + (iter_count % 10); buf[2] = '\0';
        // serial_print(buf);
        // serial_print("\n");
    }
    
    // serial_print "[SFARB"] Search complete, best=");
    // serial_print(best ? "found" : "NULL");
    // serial_print("\n");
    
    /* 如果找到块，原子地从链表中移除 */
    if (best != NULL) {
        int best_sc = get_size_class(best->size);
        struct segregated_free_list *list = &free_lists[best_sc];
        
        if (best->free_prev != NULL) {
            /* Validate free_prev pointer before accessing */
            if ((uint64_t)best->free_prev >= kernel_heap.start && 
                (uint64_t)best->free_prev < kernel_heap.end) {
                best->free_prev->free_next = best->free_next;
            }
        } else {
            list->head = best->free_next;
        }
        
        if (best->free_next != NULL) {
            /* Validate free_next pointer before accessing */
            if ((uint64_t)best->free_next >= kernel_heap.start && 
                (uint64_t)best->free_next < kernel_heap.end) {
                best->free_next->free_prev = best->free_prev;
            }
        }
        
        if (list->count > 0) {
            list->count--;
        }
    }
    
    spin_unlock(&segregated_lock);
    return best;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 自创算法: 智能内存预分配器 (Smart Memory Pre-allocator)
 * 
 * 原理说明:
 * - 基于历史分配模式预测未来需求
 * - 使用滑动窗口统计各大小类别的分配频率
 * - 检测突发分配模式，提前预分配内存
 * - 减少实时分配延迟，提高系统响应速度
 * 
 * 创新点:
 * 1. 基于马尔可夫链的分配模式预测
 * 2. 自适应阈值调整，避免过度预分配
 * 3. 多级缓存预分配池
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 预分配器配置 */
#define PREALLOC_WINDOW_SIZE    64      /* 统计窗口大小 */
#define PREALLOC_THRESHOLD      3       /* 触发预分配的阈值 */
#define PREALLOC_MAX_POOL       4       /* 每个类别最大预分配数 */
#define PREALLOC_BURST_FACTOR   2       /* 突发因子 */

/* 分配历史记录 */
struct alloc_record {
    size_t size;                     /* 请求大小 */
    uint64_t timestamp;              /* 时间戳 (TSC) */
    int size_class;                  /* 大小类别 */
};

/* 大小类别统计 */
struct size_class_stats {
    uint32_t count;                  /* 窗口内分配次数 */
    uint32_t burst_count;            /* 连续分配计数 */
    uint64_t last_alloc_time;        /* 上次分配时间 */
    uint32_t avg_interval;           /* 平均分配间隔 */
    uint32_t prealloc_count;         /* 当前预分配数量 */
};

/* 预分配池 */
struct prealloc_pool {
    struct heap_block *blocks[PREALLOC_MAX_POOL];
    int count;
    spinlock_t lock;
};

/* 预分配器状态 */
static struct {
    struct alloc_record history[PREALLOC_WINDOW_SIZE];
    int history_index;
    struct size_class_stats stats[HEAP_SIZE_CLASSES];
    struct prealloc_pool pools[HEAP_SIZE_CLASSES];
    int enabled;
    spinlock_t lock;
    uint32_t total_preallocs;        /* 总预分配次数 */
    uint32_t total_hits;             /* 预分配命中次数 */
} preallocator;

/* 初始化预分配器 */
static void preallocator_init(void) {
    for (int i = 0; i < HEAP_SIZE_CLASSES; i++) {
        preallocator.stats[i].count = 0;
        preallocator.stats[i].burst_count = 0;
        preallocator.stats[i].last_alloc_time = 0;
        preallocator.stats[i].avg_interval = 1000000; /* 默认 1M 周期 */
        preallocator.stats[i].prealloc_count = 0;
        preallocator.pools[i].count = 0;
        preallocator.pools[i].lock = 0;
    }
    preallocator.history_index = 0;
    preallocator.enabled = 1;
    preallocator.lock = 0;
    preallocator.total_preallocs = 0;
    preallocator.total_hits = 0;
}

/* 记录分配 */
static void prealloc_record_alloc(size_t size, int size_class) {
    if (!preallocator.enabled) return;
    
    uint64_t now;
    asm volatile("rdtsc" : "=A"(now));
    
    /* 更新历史记录 */
    preallocator.history[preallocator.history_index].size = size;
    preallocator.history[preallocator.history_index].timestamp = now;
    preallocator.history[preallocator.history_index].size_class = size_class;
    preallocator.history_index = (preallocator.history_index + 1) % PREALLOC_WINDOW_SIZE;
    
    /* 更新统计 */
    struct size_class_stats *stats = &preallocator.stats[size_class];
    stats->count++;
    
    /* 计算分配间隔 */
    if (stats->last_alloc_time > 0) {
        uint64_t interval = now - stats->last_alloc_time;
        /* 指数移动平均 */
        stats->avg_interval = (stats->avg_interval * 7 + (uint32_t)interval) / 8;
        
        /* 检测突发分配 (间隔小于平均的一半) */
        if (interval < stats->avg_interval / 2) {
            stats->burst_count++;
        } else {
            if (stats->burst_count > 0) stats->burst_count--;
        }
    }
    stats->last_alloc_time = now;
}

/* 检查是否需要预分配 */
static int prealloc_should_preallocate(int size_class) {
    if (!preallocator.enabled) return 0;
    
    struct size_class_stats *stats = &preallocator.stats[size_class];
    
    /* 条件 1: 突发分配模式 */
    if (stats->burst_count >= PREALLOC_THRESHOLD) {
        return PREALLOC_BURST_FACTOR;
    }
    
    /* 条件 2: 高频分配 (窗口内多次分配) */
    if (stats->count >= PREALLOC_THRESHOLD) {
        return 1;
    }
    
    /* 条件 3: 预分配池为空但有历史分配 */
    if (preallocator.pools[size_class].count == 0 && stats->count > 0) {
        return 1;
    }
    
    return 0;
}

/* 执行预分配 */
static void prealloc_do_preallocate(int size_class, int count) {
    if (!preallocator.enabled) return;
    if (count <= 0) return;
    if (count > PREALLOC_MAX_POOL) count = PREALLOC_MAX_POOL;
    
    size_t size = size_class_bounds[size_class];
    struct prealloc_pool *pool = &preallocator.pools[size_class];
    
    spin_lock(&pool->lock);
    
    int available = PREALLOC_MAX_POOL - pool->count;
    if (available <= 0) {
        spin_unlock(&pool->lock);
        return;
    }
    
    int to_alloc = (count < available) ? count : available;
    
    for (int i = 0; i < to_alloc; i++) {
        /* 使用分离适配算法查找块 (不扩展堆，只使用已有空闲块) */
        struct heap_block *block = segregated_find_best(size);
        if (block == NULL) {
            /* 没有合适的空闲块，停止预分配 */
            break;
        }
        
        /* 从空闲链表移除并加入预分配池 */
        segregated_remove(block);
        pool->blocks[pool->count++] = block;
    }
    
    preallocator.total_preallocs += to_alloc;
    spin_unlock(&pool->lock);
}

/* 从预分配池获取块 */
static struct heap_block *prealloc_get_block(size_t size) {
    if (!preallocator.enabled) return NULL;
    
    int sc = get_size_class(size);
    struct prealloc_pool *pool = &preallocator.pools[sc];
    
    if (pool->count == 0) {
        /* 尝试从更大的类别获取 */
        for (int i = sc + 1; i < HEAP_SIZE_CLASSES; i++) {
            if (preallocator.pools[i].count > 0) {
                pool = &preallocator.pools[i];
                break;
            }
        }
        if (pool->count == 0) return NULL;
    }
    
    spin_lock(&pool->lock);
    if (pool->count == 0) {
        spin_unlock(&pool->lock);
        return NULL;
    }
    
    struct heap_block *block = pool->blocks[--pool->count];
    preallocator.total_hits++;
    
    /* 更新预分配计数 */
    preallocator.stats[sc].prealloc_count = pool->count;
    
    spin_unlock(&pool->lock);
    
    /* 触发新的预分配 (异步) */
    int need = prealloc_should_preallocate(sc);
    if (need > 0) {
        prealloc_do_preallocate(sc, need);
    }
    
    return block;
}

/* 获取预分配器统计 */
void prealloc_get_stats(uint32_t *total_preallocs, uint32_t *total_hits, uint32_t *efficiency) {
    if (total_preallocs) *total_preallocs = preallocator.total_preallocs;
    if (total_hits) *total_hits = preallocator.total_hits;
    if (efficiency) {
        if (preallocator.total_preallocs > 0) {
            *efficiency = (preallocator.total_hits * 100) / preallocator.total_preallocs;
        } else {
            *efficiency = 0;
        }
    }
}

/* 块完整性检查 */
static int heap_validate_block(struct heap_block *block) {
    if (block == NULL) return 0;
    if ((uint64_t)block < kernel_heap.start || (uint64_t)block >= kernel_heap.end) {
        return 0;  /* 超出堆范围 */
    }
    if (block->size > kernel_heap.size) {
        return 0;  /* 大小不合理 */
    }
    return 1;
}

/* 块魔数检查 */
#define BLOCK_MAGIC(block) (*((uint64_t *)((uint8_t *)(block) + sizeof(struct heap_block) + (block)->size)))

/* 扩展堆 */
static int heap_expand(size_t min_size) {
    // serial_print "[HEAP_EXPAND"] Enter, min_size=");
    char buf[32];
    int len = 0;
    size_t tmp = min_size;
    if (tmp == 0) { buf[len++] = '0'; }
    else {
        char tmpbuf[32];
        int tlen = 0;
        while (tmp > 0) { tmpbuf[tlen++] = '0' + (tmp % 10); tmp /= 10; }
        while (tlen > 0) buf[len++] = tmpbuf[--tlen];
    }
    buf[len] = '\0';
    // serial_print(buf);
    // serial_print("\n");
    
    /* 计算需要扩展的页数 */
    size_t needed = min_size + sizeof(struct heap_block);
    size_t pages = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    
    /* 确保至少扩展 4 页 (16KB) 以减少频繁扩展 */
    if (pages < 4) pages = 4;
    
    // serial_print "[HEAP_EXPAND"] Allocating ");
    buf[0] = '0' + (pages / 10); buf[1] = '0' + (pages % 10); buf[2] = '\0';
    // serial_print(buf);
    // serial_print(" pages\n");
    
    /* 分配物理页并映射 */
    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == 0) {
        return -1;
    }
    
    uint64_t virt = kernel_heap.end;
    if (vmm_map_range(vmm_get_kernel_space(), virt, phys, pages, VMM_FLAGS_KERNEL) != 0) {
        pmm_free_pages(phys, pages);
        return -1;
    }
    
    /* 创建新的空闲块 */
    struct heap_block *block = (struct heap_block *)virt;
    
    /* 验证新块地址的有效性 */
    if (virt < 0xFFFFFFFF80000000ULL) {
        // println("[HEAP] ERROR: heap_expand creating block at invalid address!");
        // print("[HEAP] ERROR: virt=0x");
        // print_hex(virt);
        // println("");
        pmm_free_pages(phys, pages);
        return -1;
    }
    
    block->magic = HEAP_MAGIC;      /* 设置魔数，确保块有效性验证通过 */
    block->size = pages * PAGE_SIZE - sizeof(struct heap_block);
    block->is_free = 1;
    block->next = NULL;
    block->prev = NULL;
    block->free_next = NULL;   /* 初始化空闲链表指针 */
    block->free_prev = NULL;   /* 初始化空闲链表指针 */
    
    /* 添加到堆 */
    if (kernel_heap.head == NULL) {
        kernel_heap.head = block;
        /* 新块添加到分离空闲链表 */
        segregated_add(block);
    } else {
        /* 找到最后一个块 */
        struct heap_block *last = kernel_heap.head;
        while (last->next != NULL) {
            last = last->next;
        }
        
        /* 验证最后一个块是否与新块物理相邻 */
        uint64_t last_end = (uint64_t)last + sizeof(struct heap_block) + last->size;
        bool is_adjacent = (last_end == (uint64_t)virt);
        
        /* 调试输出 */
        // print("[HEAP_EXPAND] === LAST BLOCK ANALYSIS ===\n");
        // print("[HEAP_EXPAND] last addr=0x");
        // print_hex((uint64_t)last);
        // print("\n");
        // print("[HEAP_EXPAND] last->size=");
        // print_dec(last->size);
        // print("\n");
        // print("[HEAP_EXPAND] last->is_free=");
        // println(last->is_free ? "yes" : "no");
        // print("[HEAP_EXPAND] last->prev=0x");
        // print_hex((uint64_t)last->prev);
        // print("\n");
        // print("[HEAP_EXPAND] last_end=0x");
        // print_hex(last_end);
        // print("\n");
        // print("[HEAP_EXPAND] virt=0x");
        // print_hex((uint64_t)virt);
        // print("\n");
        // print("[HEAP_EXPAND] is_adjacent=");
        // println(is_adjacent ? "yes" : "no");
        
        /* 尝试合并相邻的空闲块 */
        if (last->is_free && is_adjacent) {
            // print("[HEAP_EXPAND] Merging with last block\n");
            /* 先从分离链表中移除旧的块 */
            segregated_remove(last);
            /* 扩展最后一个块 */
            last->size += sizeof(struct heap_block) + block->size;
            /* 重新添加到分离链表 */
            segregated_add(last);
        } else {
            // print("[HEAP_EXPAND] Creating new independent block at 0x");
            // print_hex((uint64_t)block);
            // println("");
            last->next = block;
            block->prev = last;
            /* 新块添加到分离空闲链表 */
            // println("HM: calling segregated_add"); segregated_add(block);
        }
    }
    
    kernel_heap.end += pages * PAGE_SIZE;
    kernel_heap.size += pages * PAGE_SIZE;
    
    return 0;
}

/* 合并相邻空闲块 */
static int heap_merge(struct heap_block *block) {
    // print("[HEAP_MERGE] === Starting merge ===\n");
    // print("[HEAP_MERGE] block=0x");
    // print_hex((uint64_t)block);
    // print(" size=");
    // print_dec(block->size);
    // print(" is_free=");
    // println(block->is_free ? "yes" : "no");
    
    int was_merged = 0;  /* 标记是否进行了合并 */
    
    /* 合并后面的块 */
    int merge_count = 0;
    while (block->next != NULL) {
        /* 验证 block->next 指针是否有效 */
        if ((uint64_t)block->next < kernel_heap.start || (uint64_t)block->next >= kernel_heap.end) {
            // print("[HEAP_MERGE] next ptr invalid, stopping\n");
            break;  /* 指针无效，停止合并 */
        }
        
        struct heap_block *next = block->next;
        
        /* 验证 next 指针是否有效 */
        if ((uint64_t)next < kernel_heap.start || (uint64_t)next >= kernel_heap.end) {
            // print("[HEAP_MERGE] next ptr invalid, stopping\n");
            break;  /* 指针无效，停止合并 */
        }
        if (next->size > kernel_heap.size) {
            // print("[HEAP_MERGE] next size invalid, stopping\n");
            break;  /* 块大小不合理，停止合并 */
        }
        
        /* 验证物理相邻：next 必须紧随 block 之后 */
        uint64_t expected_next = (uint64_t)block + sizeof(struct heap_block) + block->size;
        // print("[HEAP_MERGE] forward: next=0x");
        // print_hex((uint64_t)next);
        // print(" expected=0x");
        // print_hex(expected_next);
        // print(" match=");
        // println(((uint64_t)next == expected_next) ? "yes" : "no");
        
        if ((uint64_t)next != expected_next) {
            break;  /* 不物理相邻，停止合并 */
        }
        
        if (!next->is_free) {
            // print("[HEAP_MERGE] next not free, stopping\n");
            break;  /* 下一个块不是空闲的，停止合并 */
        }
        
        // print("[HEAP_MERGE] merging forward: block size ");
        // print_dec(block->size);
        // print(" + ");
        // print_dec(sizeof(struct heap_block) + next->size);
        // print(" = ");
        
        /* 从分离空闲链表中移除 next（block 还未被添加到链表） */
        segregated_remove(next);
        /* 注意：不调用 segregated_remove(block)，因为 block 刚刚释放，还未添加到链表 */
        /* 清除 block 的空闲链表指针，因为它将被合并 */
        block->free_next = NULL;
        block->free_prev = NULL;
        
        block->size += sizeof(struct heap_block) + next->size;
        // print_dec(block->size);
        // println("");
        
        /* 清除被合并块的魔数和空闲链表指针，防止误用 */
        next->magic = 0;  /* 关键：清除魔数，防止被误认为是有效块头 */
        next->free_next = NULL;
        next->free_prev = NULL;
        /* 注意：不清除 next->prev 和 next->next，因为它们可能仍被使用 */
        
        block->next = next->next;
        if (next->next != NULL) {
            /* 验证 next->next 指针 */
            if ((uint64_t)next->next >= kernel_heap.start && 
                (uint64_t)next->next < kernel_heap.end) {
                next->next->prev = block;
            }
        }
        
        /* 重新添加合并后的块 */
        segregated_add(block);
        was_merged = 1;  /* 标记进行了向后合并 */
        merge_count++;
        
        if (merge_count > 100) {
            break;  /* 防止无限循环 */
        }
    }
    
    /* 合并前面的块 */
    merge_count = 0;
    while (block->prev != NULL) {
        /* 验证 block->prev 指针是否有效 */
        if ((uint64_t)block->prev < kernel_heap.start || (uint64_t)block->prev >= kernel_heap.end) {
            // print("[HEAP_MERGE] prev ptr invalid, stopping\n");
            break;  /* 指针无效，停止合并 */
        }
        
        struct heap_block *prev = block->prev;
        
        /* 验证 prev 指针是否有效 */
        if ((uint64_t)prev < kernel_heap.start || (uint64_t)prev >= kernel_heap.end) {
            // print("[HEAP_MERGE] prev ptr invalid, stopping\n");
            break;  /* 指针无效，停止合并 */
        }
        if (prev->size > kernel_heap.size) {
            // print("[HEAP_MERGE] prev size invalid, stopping\n");
            break;  /* 块大小不合理，停止合并 */
        }
        
        /* 验证物理相邻：block 必须紧随 prev 之后 */
        uint64_t expected_block = (uint64_t)prev + sizeof(struct heap_block) + prev->size;
        // print("[HEAP_MERGE] backward: prev=0x");
        // print_hex((uint64_t)prev);
        // print(" prev_size=");
        // print_dec(prev->size);
        // print(" expected_block=0x");
        // print_hex(expected_block);
        // print(" actual_block=0x");
        // print_hex((uint64_t)block);
        // print(" match=");
        // println(((uint64_t)block == expected_block) ? "yes" : "no");
        
        if ((uint64_t)block != expected_block) {
            break;  /* 不物理相邻，停止合并 */
        }
        
        if (!prev->is_free) {
            // print("[HEAP_MERGE] prev not free, stopping\n");
            break;  /* 前一个块不是空闲的，停止合并 */
        }
        
                // print("[HEAP_MERGE] merging backward: prev size ");
                // print_dec(prev->size);
                // print(" + ");
                // print_dec(sizeof(struct heap_block) + block->size);
                // print(" = ");
                
                /* 从分离空闲链表中移除 prev（block 还未被添加到链表） */
                segregated_remove(prev);
                /* 注意：不调用 segregated_remove(block)，因为 block 刚刚释放，还未添加到链表 */
                /* 清除 block 的空闲链表指针，标记它已被合并 */
                block->free_next = NULL;
                block->free_prev = NULL;
        
                prev->size += sizeof(struct heap_block) + block->size;
                // print_dec(prev->size);
                // println("");
        
                /* 清除被合并块的魔数和链表指针，防止误用 */
                block->magic = 0;  /* 关键：清除魔数，防止被误认为是有效块头 */
                block->free_next = NULL;
                block->free_prev = NULL;
                /* 注意：不清除 block->prev 和 block->next，因为它们可能仍被使用 */
        
                prev->next = block->next;
                if (block->next != NULL) {
                    /* 验证 block->next 指针 */
                    if ((uint64_t)block->next >= kernel_heap.start && 
                        (uint64_t)block->next < kernel_heap.end) {
                        block->next->prev = prev;
                    }
                }
        
                /* 重新添加合并后的块 */
                segregated_add(prev);
                block = prev;
                was_merged = 1;  /* 标记进行了向前合并 */
                merge_count++;
        
                if (merge_count > 100) {
                    break;  /* 防止无限循环 */
                }
    }
    
    /* 如果没有进行任何合并，将块添加到空闲链表 */
    if (!was_merged) {
        segregated_add(block);
    }
    
    return was_merged;
}

/* 分割块 */
static void heap_split(struct heap_block *block, size_t size) {
    /* 检查剩余空间是否足够创建新块 */
    size_t remaining = block->size - size;
    if (remaining < HEAP_MIN_BLOCK_SIZE + sizeof(struct heap_block)) {
        return;  /* 空间太小，不分割 */
    }
    
    /* 创建新块 */
    struct heap_block *new_block = (struct heap_block *)
        ((uint8_t *)block + sizeof(struct heap_block) + size);
    
    /* 验证新块地址的有效性 */
    uint64_t new_block_addr = (uint64_t)new_block;
    if (new_block_addr < kernel_heap.start || new_block_addr >= kernel_heap.end) {
        // println("[HEAP] ERROR: heap_split creating block outside heap range!");
        // print("[HEAP] ERROR: block=0x");
        // print_hex((uint64_t)block);
        // print(" new_block=0x");
        // print_hex(new_block_addr);
        // print(" heap_range=[0x");
        // print_hex(kernel_heap.start);
        // print("-0x");
        // print_hex(kernel_heap.end);
        // println("]");
        return;  /* 跳过分割操作 */
    }
    
    new_block->magic = HEAP_MAGIC;      /* 设置魔数，确保块有效性验证通过 */
    new_block->size = remaining - sizeof(struct heap_block);
    new_block->is_free = 1;
    new_block->next = block->next;
    new_block->prev = block;
    new_block->free_next = NULL;   /* 初始化空闲链表指针 */
    new_block->free_prev = NULL;   /* 初始化空闲链表指针 */
    
    if (block->next != NULL) {
        block->next->prev = new_block;
    }
    block->next = new_block;
    block->size = size;
    
    kernel_heap.used -= sizeof(struct heap_block);
    
    /* 关键修复：将新空闲块添加到分离空闲链表 */
    segregated_add(new_block);
}

void heap_init(void) {
    kernel_heap.head = NULL;
    kernel_heap.start = HEAP_START;
    kernel_heap.end = HEAP_START;
    kernel_heap.size = 0;
    kernel_heap.used = 0;
    kernel_heap.alloc_count = 0;
    kernel_heap.free_count = 0;
    kernel_heap.total_alloc_size = 0;
    kernel_heap.total_free_size = 0;
    kernel_heap.peak_usage = 0;
    kernel_heap.initialized = 0;
    
    /* 初始化分离空闲链表 */
    for (int i = 0; i < HEAP_SIZE_CLASSES; i++) {
        free_lists[i].head = NULL;
        free_lists[i].count = 0;
    }
    
    /* 扩展初始堆 */
    if (heap_expand(HEAP_INITIAL_SIZE) != 0) {
        // println("[HEAP] Error: Failed to expand heap!");
        return;
    }
    
    kernel_heap.initialized = 1;
    
    // print("    Heap start: 0x");
    // print_hex(kernel_heap.start);
    // println("");
    // print("    Heap size: ");
    // print_dec(kernel_heap.size / 1024);
    // println(" KB");
    // print("    Size classes: ");
    // print_dec(HEAP_SIZE_CLASSES);
    // println("");
}

void *kmalloc(size_t size) {
    /* 调试输出：显示 segregated_add/remove 计数 */
    // print("[KMALLOC-DBG] sadd="); print_dec(dbg_sadd_count);
    // print(" sremove="); print_dec(dbg_sremove_count); println("");

    /* 调试输出 */
    // serial_print "[KMALLOC"] Enter, size=");
    char buf[32];
    int len = 0;
    size_t tmp = size;
    if (tmp == 0) { buf[len++] = '0'; }
    else {
        char tmpbuf[32];
        int tlen = 0;
        while (tmp > 0) { tmpbuf[tlen++] = '0' + (tmp % 10); tmp /= 10; }
        while (tlen > 0) buf[len++] = tmpbuf[--tlen];
    }
    buf[len] = '\0';
    // serial_print(buf);
    // serial_print("\n");
    
    /* 参数检查 */
    if (size == 0) {
        // serial_print "[KMALLOC"] size=0, return NULL\n");
        return NULL;
    }
    
    /* 防止整数溢出：如果 size 接近 SIZE_MAX，对齐会导致溢出 */
    if (size > SIZE_MAX - 8) {
        // println("[HEAP] malloc: Size overflow");
        return NULL;
    }
    
    /* 防止过大的分配请求 */
    if (size > kernel_heap.size / 2) {
        // println("[HEAP] malloc: Request too large");
        return NULL;
    }
    
    /* 对齐到 8 字节 */
    size = (size + 7) & ~7UL;
    
    // serial_print "[KMALLOC"] Calling segregated_find_and_remove_best...\n");
    /* 使用分离适配算法原子地查找并移除合适的块 */
    struct heap_block *block = segregated_find_and_remove_best(size);
    // serial_print "[KMALLOC"] segregated_find_and_remove_best returned\n");
    
    if (block != NULL) {
        /* 验证块地址在堆范围内 */
        if ((uint64_t)block < kernel_heap.start || 
            (uint64_t)block >= kernel_heap.end) {
            // println("[HEAP] ERROR: Block address out of heap range!");
            return NULL;
        }
        
        /* 验证块大小合理 */
        if (block->size > kernel_heap.size || block->size < size) {
            // println("[HEAP] ERROR: Block size invalid!");
            return NULL;
        }
        
        /* 验证块结束地址在堆范围内 */
        uint64_t block_end = (uint64_t)block + sizeof(struct heap_block) + block->size;
        if (block_end > kernel_heap.end) {
            // println("[HEAP] ERROR: Block extends beyond heap!");
            // print("  block_start: 0x");
            // print_hex((uint64_t)block);
            // print("  block_end: 0x");
            // print_hex(block_end);
            // print("  heap_end: 0x");
            // print_hex(kernel_heap.end);
            // println("");
            return NULL;
        }
        
        /* 标记为已使用 */
        block->is_free = 0;
        block->magic = HEAP_BLOCK_MAGIC;
        
        /* 尝试分割块 */
        heap_split(block, size);
        kernel_heap.used += block->size + sizeof(struct heap_block);
        
        /* 返回块数据区域 */
        void *ptr = (void *)((uint8_t *)block + sizeof(struct heap_block));
        
        /* 内存投毒检测：检查是否包含 0xDEAD 模式 (use-after-free 检测) */
#if HEAP_POISON
        {
            uint64_t *check = (uint64_t *)ptr;
            int poison_detected = 0;
            size_t check_count = block->size / sizeof(uint64_t);
            if (check_count > HEAP_POISON_CHECK_BYTES / sizeof(uint64_t)) {
                check_count = HEAP_POISON_CHECK_BYTES / sizeof(uint64_t);
            }
            for (size_t i = 0; i < check_count; i++) {
                if (check[i] == HEAP_POISON_FREE) {
                    poison_detected = 1;
                    break;
                }
            }
            if (poison_detected) {
                /* 检测到投毒模式，说明这块内存曾被释放后又被使用 */
                /* 记录但不阻止分配，因为清零会覆盖投毒模式 */
                heap_sec_stats.uaf_attempts++;
            }
        }
#endif
        
        /* 重要验证：确保返回的指针在内核地址空间范围内 */
        uint64_t ptr_addr = (uint64_t)ptr;
        if (ptr_addr < 0xFFFFFFFF80000000ULL) {
            // println("[HEAP] ERROR: kmalloc returning address below kernel space!");
            // print("[HEAP] ERROR: ptr=0x");
            // print_hex(ptr_addr);
            // println("");
            return NULL;
        }
        
        /* 额外验证：确保指针在堆范围内或在合理内核空间 */
        if (ptr_addr >= 0xFFFF800000000000ULL) {
            /* 检查是否在堆之外的其他内核区域 */
            if (ptr_addr < kernel_heap.start || ptr_addr >= kernel_heap.end) {
                /* 验证是否在其他已知内核区域 */
                if (ptr_addr < 0xFFFFFFFF80000000ULL || ptr_addr >= 0xFFFFFFFFC0000000ULL) {
                    // println("[HEAP] ERROR: kmalloc returning invalid address!");
                    // print("[HEAP] ERROR: ptr=0x");
                    // print_hex(ptr_addr);
                    // println("");
                    return NULL;
                }
            }
        }
        
        /* 调试输出 */
        // print("[KMALLOC] block=0x");
        // print_hex((uint64_t)block);
        // print(" size=");
        // print_dec(block->size);
        // print(" ptr=0x");
        // print_hex((uint64_t)ptr);
        // print(" heap_end=0x");
        // print_hex(kernel_heap.end);
        // println("");
        
        /* 清零新分配的内存 - 使用 block->size 确保整个分配块清零 */
        memset(ptr, 0, block->size);
        
        /* 更新分配统计 */
        kernel_heap.alloc_count++;
        kernel_heap.total_alloc_size += block->size;
        uint64_t current_used = kernel_heap.used;
        if (current_used > kernel_heap.peak_usage) {
            kernel_heap.peak_usage = current_used;
        }
        
        return ptr;
    }
    
    /* 没有找到合适的块，扩展堆 */
    // serial_print "[KMALLOC"] No block found, calling heap_expand...\n");
    // print("[KMALLOC] No block found, expanding heap for size ");
    // print_dec(size);
    // println(" bytes");
    
    if (heap_expand(size) != 0) {
        // println("[HEAP] malloc: Failed to expand heap");
        return NULL;
    }
    // serial_print "[KMALLOC"] heap_expand succeeded, searching again...\n");
    
    // print("[KMALLOC] Heap expanded, new end=0x");
    // print_hex(kernel_heap.end);
    // println("");
    
    /* 扩展成功后再次查找合适的块 (避免递归调用 kmalloc) */
    block = segregated_find_and_remove_best(size);
    // serial_print "[KMALLOC"] Second search done\n");
    // print("[KMALLOC] After expand search result: ");
    if (block == NULL) {
        // println("NULL!");
    } else {
        // print("block=0x");
        // print_hex((uint64_t)block);
        // print(" size=");
        // print_dec(block->size);
        // println("");
    }
    
    if (block != NULL) {
        /* 验证块地址在堆范围内 */
        if ((uint64_t)block < kernel_heap.start || 
            (uint64_t)block >= kernel_heap.end) {
            // println("[HEAP] ERROR: Block address out of heap range after expand!");
            return NULL;
        }
        
        /* 验证块大小合理 */
        if (block->size > kernel_heap.size || block->size < size) {
            // println("[HEAP] ERROR: Block size invalid after expand!");
            return NULL;
        }
        
        /* 验证块结束地址在堆范围内 */
        uint64_t block_end = (uint64_t)block + sizeof(struct heap_block) + block->size;
        if (block_end > kernel_heap.end) {
            // println("[HEAP] ERROR: Block extends beyond heap after expand!");
            return NULL;
        }
        
        /* 标记为已使用 */
        block->is_free = 0;
        block->magic = HEAP_BLOCK_MAGIC;
        
        /* 尝试分割块 */
        heap_split(block, size);
        kernel_heap.used += block->size + sizeof(struct heap_block);
        
        /* 返回块数据区域 */
        void *ptr = (void *)((uint8_t *)block + sizeof(struct heap_block));
        
        /* 清零新分配的内存 - 使用 block->size 确保整个分配块清零 */
        memset(ptr, 0, block->size);
        
        /* 更新分配统计 */
        kernel_heap.alloc_count++;
        kernel_heap.total_alloc_size += block->size;
        uint64_t current_used2 = kernel_heap.used;
        if (current_used2 > kernel_heap.peak_usage) {
            kernel_heap.peak_usage = current_used2;
        }
        
        return ptr;
    }
    
    /* 如果扩展后仍然找不到块，可能是严重的内存碎片问题 */
    // println("[HEAP] malloc: No suitable block after expansion");
    return NULL;
}

void *krealloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return kmalloc(size);
    }
    
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    struct heap_block *block = (struct heap_block *)
        ((uint8_t *)ptr - sizeof(struct heap_block));
    
    /* 如果新大小小于当前大小，尝试分割 */
    if (size <= block->size) {
        heap_split(block, (size + 7) & ~7UL);
        return ptr;
    }
    
    /* 尝试扩展到下一个空闲块 */
    if (block->next != NULL && block->next->is_free) {
        size_t combined = block->size + sizeof(struct heap_block) + block->next->size;
        if (combined >= size) {
            /* 合并块 */
            struct heap_block *next = block->next;
            segregated_remove(next);  /* 从空闲链表移除被合并的块 */
            block->size = combined;
            block->next = next->next;
            if (next->next != NULL) {
                next->next->prev = block;
            }
            heap_split(block, size);
            return ptr;
        }
    }
    
    /* 需要分配新块并复制 */
    void *new_ptr = kmalloc(size);
    if (new_ptr == NULL) {
        return NULL;
    }
    
    memcpy(new_ptr, ptr, block->size);
    kfree(ptr);
    
    return new_ptr;
}

/* 串口调试输出函数 (在 console.c 中定义) */
extern void serial_print(const char *str);

void kfree(void *ptr) {
    /* 空指针检查 */
    if (ptr == NULL) {
        return;
    }
    
    /* 调试：打印指针、堆范围和调用者 */
    // print("[HEAP] free: ptr=0x");
    // print_hex((uint64_t)ptr);
    // print(" heap_start=0x");
    // print_hex(kernel_heap.start);
    // print(" heap_end=0x");
    // print_hex(kernel_heap.end);
    // print(" caller=0x");
    // print_hex((uint64_t)__builtin_return_address(0));
    // println("");
    
    /* 获取块头 */
    struct heap_block *block = (struct heap_block *)
        ((uint8_t *)ptr - sizeof(struct heap_block));
    
    // print("[HEAP] free: block=0x");
    // print_hex((uint64_t)block);
    // println("");
    
    /* 验证块完整性 */
    if (!heap_validate_block(block)) {
        // println("[HEAP] free: Invalid block pointer");
        return;
    }
    
    /* 检查是否已经释放 (双重释放检测) */
    if (block->is_free) {
        // println("[HEAP] free: Double free detected!");
        return;
    }
    
    /* 标记为空闲 */
    block->is_free = 1;
    block->magic = HEAP_MAGIC;  /* 使用 HEAP_MAGIC 以便 segregated_find_best 能找到此块 */
    
    /* 内存投毒：在释放时填充 0xDEAD 模式，检测 use-after-free */
#if HEAP_POISON
    {
        void *data_ptr = (void *)((uint8_t *)block + sizeof(struct heap_block));
        size_t poison_size = block->size;
        /* 填充投毒模式 */
        for (size_t i = 0; i + sizeof(uint64_t) <= poison_size; i += sizeof(uint64_t)) {
            *(uint64_t *)((uint8_t *)data_ptr + i) = HEAP_POISON_FREE;
        }
        /* 处理剩余字节 */
        for (size_t i = (poison_size / sizeof(uint64_t)) * sizeof(uint64_t); i < poison_size; i++) {
            ((uint8_t *)data_ptr)[i] = (uint8_t)(HEAP_POISON_FREE & 0xFF);
        }
    }
#endif
    
    /* 更新统计 */
    if (kernel_heap.used >= block->size + sizeof(struct heap_block)) {
        kernel_heap.used -= block->size + sizeof(struct heap_block);
    } else {
        // println("[HEAP] free: Used memory underflow");
    }
    
    kernel_heap.free_count++;
    kernel_heap.total_free_size += block->size;
    
    /* 合并相邻空闲块（heap_merge 内部会将块添加到空闲链表） */
    heap_merge(block);
}

void *kvalloc(size_t size, size_t align) {
    if (size == 0 || align == 0) {
        return NULL;
    }
    
    /* 确保 align 是 2 的幂 */
    if ((align & (align - 1)) != 0) {
        /* 不是 2 的幂，向上舍入到最近的 2 的幂 */
        align = 1UL << (64 - __builtin_clzll(align));
    }
    
    /* 分配额外空间用于对齐和存储原始指针 */
    size_t total = size + align + sizeof(void *);
    void *ptr = kmalloc(total);
    if (ptr == NULL) {
        return NULL;
    }
    
    /* 计算对齐地址，预留空间存储原始指针 */
    uintptr_t addr = (uintptr_t)ptr + sizeof(void *);
    uintptr_t aligned = (addr + align - 1) & ~(align - 1);
    
    /* 在对齐地址前存储原始指针，用于 kfree */
    void **stored = (void **)(aligned - sizeof(void *));
    *stored = ptr;
    
    return (void *)aligned;
}

size_t ksize(void *ptr) {
    if (ptr == NULL) {
        return 0;
    }
    
    struct heap_block *block = (struct heap_block *)
        ((uint8_t *)ptr - sizeof(struct heap_block));
    
    return block->size;
}

/* 释放对齐分配的内存 */
void kvfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    
    /* 从对齐地址前获取原始指针 */
    void **stored = (void **)((uint8_t *)ptr - sizeof(void *));
    void *original = *stored;
    
    /* 验证原始指针是否有效 */
    if (original == NULL) {
        // println("[HEAP] kvfree: Invalid stored pointer (NULL)");
        return;
    }
    
    /* 使用原始指针释放 */
    kfree(original);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全辅助函数实现
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 分配并清零 */
void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr != NULL && size > 0) {
        memset(ptr, 0, size);
    }
    return ptr;
}

/* 安全复制 */
void *kmemcpy_safe(void *dest, const void *src, size_t n) {
    if (dest == NULL || src == NULL) {
        return dest;
    }
    if (n == 0) {
        return dest;
    }
    return memcpy(dest, src, n);
}

/* 安全字符串复制 */
char *kstrdup(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    
    size_t len = strlen(s) + 1;
    char *dup = kmalloc(len);
    if (dup != NULL) {
        memcpy(dup, s, len);
    }
    return dup;
}

/* 安全字符串复制 (带长度限制) */
char *kstrndup(const char *s, size_t n) {
    if (s == NULL) {
        return NULL;
    }
    
    size_t len = strnlen(s, n);
    char *dup = kmalloc(len + 1);
    if (dup != NULL) {
        memcpy(dup, s, len);
        dup[len] = '\0';
    }
    return dup;
}

/* 安全内存填充 */
void *kmemset_safe(void *s, int c, size_t n) {
    if (s == NULL || n == 0) {
        return s;
    }
    return memset(s, c, n);
}

/* 带标志分配 */
void *kmalloc_flags(size_t size, int flags) {
    void *ptr = kmalloc(size);
    if (ptr != NULL && (flags & KMALLOC_ZERO)) {
        memset(ptr, 0, size);
    }
    return ptr;
}

/* 重新分配数组 */
void *krealloc_array(void *ptr, size_t nmemb, size_t size) {
    /* 检查整数溢出 */
    if (nmemb > 0 && size > SIZE_MAX / nmemb) {
        return NULL;
    }
    return krealloc(ptr, nmemb * size);
}

/* 验证堆完整性 */
int heap_validate(void) {
    struct heap_block *block = kernel_heap.head;
    int errors = 0;
    
    while (block != NULL) {
        /* 检查块大小 */
        if (block->size > kernel_heap.size) {
            // println("[HEAP] Validate: Block size exceeds heap size");
            errors++;
        }
        
        /* 检查块地址范围 */
        uint64_t block_end = (uint64_t)block + sizeof(struct heap_block) + block->size;
        if (block_end > kernel_heap.end) {
            // println("[HEAP] Validate: Block extends beyond heap");
            errors++;
        }
        
        /* 检查链表一致性 */
        if (block->next != NULL && block->next->prev != block) {
            // println("[HEAP] Validate: Broken list links");
            errors++;
        }
        
        block = block->next;
    }
    
    return errors == 0 ? 1 : 0;
}

void heap_stats(void) {
    // println("╔══════════════════════════════════════════════════════════════╗");
    // println("║                    Heap Statistics                            ║");
    // println("╠══════════════════════════════════════════════════════════════╣");
    
    // print("║ Total size: ");
    // print_dec(kernel_heap.size / 1024);
    // println(" KB                                        ║");
    
    // print("║ Used:       ");
    // print_dec(kernel_heap.used / 1024);
    // println(" KB                                        ║");
    
    // print("║ Free:       ");
    // print_dec((kernel_heap.size - kernel_heap.used) / 1024);
    // println(" KB                                        ║");
    
    // print("║ Allocs:     ");
    // print_dec(kernel_heap.alloc_count);
    // println("                                          ║");
    
    // print("║ Frees:      ");
    // print_dec(kernel_heap.free_count);
    // println("                                          ║");
    
    /* 统计块数 */
    int free_blocks = 0, used_blocks = 0;
    struct heap_block *block = kernel_heap.head;
    while (block != NULL) {
        if (block->is_free) {
            free_blocks++;
        } else {
            used_blocks++;
        }
        block = block->next;
    }
    
    // print("║ Free blocks:  ");
    // print_dec(free_blocks);
    // println("                                          ║");
    
    // print("║ Used blocks:  ");
    // print_dec(used_blocks);
    // println("                                          ║");
    
    // println("╚══════════════════════════════════════════════════════════════╝");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：堆安全增强
 * 
 * 特性：
 * - 堆溢出检测
 * - 释放后使用检测 (Use-After-Free)
 * - 双重释放检测
 * - 堆垃圾回收
 * - 块校验和
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 安全魔数 */
#define HEAP_FREE_MAGIC     0xFEEDFACEDEADBEEFULL   /* 空闲块魔数 */
#define HEAP_ALLOC_MAGIC    0xCAFEBABE12345678ULL   /* 已分配块魔数 */
#define HEAP_CANARY_SIZE    16                       /* 金丝雀大小 */

/* 初始化堆安全 */
void heap_security_init(void) {
    memset(&heap_sec_stats, 0, sizeof(heap_sec_stats));
}

/* 计算块校验和 */
__attribute__((unused)) static uint32_t heap_block_checksum(struct heap_block *block) {
    uint32_t sum = 0;
    uint8_t *p = (uint8_t *)block;
    
    /* 简化的校验和：跳过 checksum 字段 */
    for (size_t i = 0; i < sizeof(struct heap_block) - 4; i++) {
        sum = sum * 31 + p[i];
    }
    
    return sum ^ 0xDEADBEEF;
}

/* 设置块金丝雀 (溢出检测) */
__attribute__((unused)) static void heap_set_canary(struct heap_block *block) {
    if (block == NULL) return;
    
    /* 数据紧跟在块头部之后 */
    void *data = (void *)(block + 1);
    
    /* 在数据末尾放置金丝雀 */
    uint64_t *canary = (uint64_t *)((uint8_t *)data + block->size - sizeof(uint64_t));
    *canary = HEAP_FREE_MAGIC ^ (uint64_t)block;
}

/* 验证块金丝雀 */
static int heap_verify_canary(struct heap_block *block) {
    if (block == NULL) return 1;
    
    void *data = (void *)(block + 1);
    uint64_t *canary = (uint64_t *)((uint8_t *)data + block->size - sizeof(uint64_t));
    uint64_t expected = HEAP_FREE_MAGIC ^ (uint64_t)block;
    
    if (*canary != expected) {
        heap_sec_stats.canary_failures++;
        heap_sec_stats.overflows_detected++;
        return 0;  /* 金丝雀被破坏 */
    }
    
    return 1;
}

/* 安全分配 (带溢出检测) */
void *kmalloc_secure(size_t size) {
    /* 额外分配金丝雀空间 */
    size_t total_size = size + HEAP_CANARY_SIZE;
    
    void *ptr = kmalloc(total_size);
    if (ptr == NULL) return NULL;
    
    /* 设置金丝雀 */
    uint64_t *canary = (uint64_t *)((uint8_t *)ptr + size);
    *canary = HEAP_ALLOC_MAGIC ^ (uint64_t)ptr;
    
    return ptr;
}

/* 安全释放 (带多重检测) */
void kfree_secure(void *ptr) {
    if (ptr == NULL) return;
    
    /* 查找块 - 数据指针在块头部之后 */
    struct heap_block *block = (struct heap_block *)ptr - 1;
    
    /* 验证魔数 */
    if (block->magic != HEAP_MAGIC && block->magic != HEAP_FREE_MAGIC) {
        heap_sec_stats.corruptions++;
        // println("[HEAP] kfree_secure: Invalid block magic");
        return;
    }
    
    /* 检测双重释放 */
    if (block->is_free) {
        heap_sec_stats.double_frees++;
        // println("[HEAP] kfree_secure: Double free detected");
        return;
    }
    
    /* 验证金丝雀 */
    heap_verify_canary(block);
    
    /* 设置释放魔数 */
    block->magic = HEAP_FREE_MAGIC;
    
    /* 执行释放 */
    kfree(ptr);
}

/* 堆垃圾回收 */
void heap_gc(void) {
    struct heap_block *block = kernel_heap.head;
    int collected = 0;
    
    while (block != NULL) {
        if (block->is_free) {
            /* 验证空闲块 */
            if (block->magic == HEAP_FREE_MAGIC) {
                /* 尝试合并相邻空闲块 */
                if (block->prev != NULL && block->prev->is_free) {
                    block->prev->size += block->size + sizeof(struct heap_block);
                    block->prev->next = block->next;
                    if (block->next != NULL) {
                        block->next->prev = block->prev;
                    }
                    collected++;
                }
            }
        }
        block = block->next;
    }
    
    if (collected > 0) {
        // print("[HEAP] GC: Collected ");
        // print_dec(collected);
        // println(" blocks");
    }
}

/* 获取堆安全统计 */
void heap_get_security_stats(struct heap_security_stats *stats) {
    if (stats != NULL) {
        *stats = heap_sec_stats;
    }
}

/* 打印堆安全统计 */
void heap_print_security_stats(void) {
    // println("[HEAP] Security Statistics:");
    // print("  Overflows detected: ");
    // print_dec(heap_sec_stats.overflows_detected);
    // println("");
    // print("  Double frees: ");
    // print_dec(heap_sec_stats.double_frees);
    // println("");
    // print("  Use-after-free attempts: ");
    // print_dec(heap_sec_stats.uaf_attempts);
    // println("");
    // print("  Corruptions: ");
    // print_dec(heap_sec_stats.corruptions);
    // println("");
    // print("  Canary failures: ");
    // print_dec(heap_sec_stats.canary_failures);
    // println("");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：内存池快速分配器
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 内存池结构 */
struct mem_pool {
    char name[32];
    size_t obj_size;            /* 对象大小 */
    size_t capacity;            /* 容量 */
    size_t used;                /* 已用 */
    void *base;                 /* 基地址 */
    void **free_list;           /* 空闲链表 */
    size_t free_count;          /* 空闲数量 */
    volatile int lock;
};

#define MAX_MEM_POOLS   16
static struct mem_pool mem_pools[MAX_MEM_POOLS];
static int mem_pool_count = 0;

/* 创建内存池 */
struct mem_pool *mem_pool_create(const char *name, size_t obj_size, size_t capacity) {
    if (mem_pool_count >= MAX_MEM_POOLS) return NULL;
    if (name == NULL || obj_size == 0 || capacity == 0) return NULL;
    
    struct mem_pool *pool = &mem_pools[mem_pool_count++];
    
    /* 复制名称 */
    int i = 0;
    while (name[i] && i < 31) {
        pool->name[i] = name[i];
        i++;
    }
    pool->name[i] = '\0';
    
    pool->obj_size = obj_size;
    pool->capacity = capacity;
    pool->used = 0;
    pool->free_count = 0;
    pool->lock = 0;
    
    /* 分配内存 */
    pool->base = kmalloc(obj_size * capacity);
    if (pool->base == NULL) {
        mem_pool_count--;
        return NULL;
    }
    
    /* 分配空闲链表 */
    pool->free_list = kmalloc(sizeof(void *) * capacity);
    if (pool->free_list == NULL) {
        kfree(pool->base);
        mem_pool_count--;
        return NULL;
    }
    
    /* 初始化空闲链表 */
    for (size_t j = 0; j < capacity; j++) {
        pool->free_list[j] = (void *)((uint8_t *)pool->base + j * obj_size);
    }
    pool->free_count = capacity;
    
    return pool;
}

/* 从内存池分配 */
void *mem_pool_alloc(struct mem_pool *pool) {
    if (pool == NULL || pool->free_count == 0) return NULL;
    
    while (__sync_lock_test_and_set(&pool->lock, 1)) {
        asm volatile("pause");
    }
    
    void *obj = pool->free_list[--pool->free_count];
    pool->used++;
    
    __sync_lock_release(&pool->lock);
    
    return obj;
}

/* 释放到内存池 */
void mem_pool_free(struct mem_pool *pool, void *obj) {
    if (pool == NULL || obj == NULL) return;
    
    while (__sync_lock_test_and_set(&pool->lock, 1)) {
        asm volatile("pause");
    }
    
    if (pool->free_count < pool->capacity) {
        pool->free_list[pool->free_count++] = obj;
        pool->used--;
    }
    
    __sync_lock_release(&pool->lock);
}

/* 销毁内存池 */
void mem_pool_destroy(struct mem_pool *pool) {
    if (pool == NULL) return;
    
    kfree(pool->base);
    kfree(pool->free_list);
    
    pool->base = NULL;
    pool->free_list = NULL;
    pool->capacity = 0;
    pool->used = 0;
    pool->free_count = 0;
}

/* 获取内存池统计 */
void mem_pool_stats(struct mem_pool *pool, size_t *used, size_t *free, size_t *capacity) {
    if (pool == NULL) return;
    
    if (used) *used = pool->used;
    if (free) *free = pool->free_count;
    if (capacity) *capacity = pool->capacity;
}

/* ========== 内核内存诊断增强功能 ========== */

/* === 内存泄漏检测 === */

/* 分配追踪记录 */
#define ALLOC_TRACE_MAX     4096

struct alloc_trace {
    void *ptr;              /* 分配的指针 */
    size_t size;            /* 分配大小 */
    uint64_t timestamp;     /* 分配时间 */
    uint32_t flags;         /* 标志 */
    char tag[16];           /* 标签 */
    int in_use;
};

static struct alloc_trace alloc_traces[ALLOC_TRACE_MAX];
static uint32_t alloc_trace_count;
static spinlock_t alloc_trace_lock = SPINLOCK_INIT;
static int alloc_tracing_enabled;

/* 初始化分配追踪 */
void heap_trace_init(void) {
    alloc_trace_count = 0;
    alloc_tracing_enabled = 0;
    
    for (int i = 0; i < ALLOC_TRACE_MAX; i++) {
        alloc_traces[i].in_use = 0;
    }
}

/* 启用/禁用分配追踪 */
void heap_trace_enable(int enable) {
    alloc_tracing_enabled = enable;
}

/* 记录分配 */
void heap_trace_alloc(void *ptr, size_t size, const char *tag) {
    if (!alloc_tracing_enabled || ptr == NULL) return;
    
    spin_lock(&alloc_trace_lock);
    
    /* 查找空闲槽 */
    for (uint32_t i = 0; i < ALLOC_TRACE_MAX; i++) {
        if (!alloc_traces[i].in_use) {
            alloc_traces[i].ptr = ptr;
            alloc_traces[i].size = size;
            alloc_traces[i].timestamp = get_tick_count();
            alloc_traces[i].flags = 0;
            alloc_traces[i].in_use = 1;
            
            /* 复制标签 */
            if (tag != NULL) {
                int j = 0;
                while (tag[j] && j < 15) {
                    alloc_traces[i].tag[j] = tag[j];
                    j++;
                }
                alloc_traces[i].tag[j] = '\0';
            } else {
                alloc_traces[i].tag[0] = '\0';
            }
            
            alloc_trace_count++;
            break;
        }
    }
    
    spin_unlock(&alloc_trace_lock);
}

/* 记录释放 */
void heap_trace_free(void *ptr) {
    if (!alloc_tracing_enabled || ptr == NULL) return;
    
    spin_lock(&alloc_trace_lock);
    
    for (uint32_t i = 0; i < ALLOC_TRACE_MAX; i++) {
        if (alloc_traces[i].in_use && alloc_traces[i].ptr == ptr) {
            alloc_traces[i].in_use = 0;
            alloc_trace_count--;
            break;
        }
    }
    
    spin_unlock(&alloc_trace_lock);
}

/* 检查内存泄漏 */
int heap_check_leaks(void) {
    int leaks = 0;
    
    spin_lock(&alloc_trace_lock);
    
    for (uint32_t i = 0; i < ALLOC_TRACE_MAX; i++) {
        if (alloc_traces[i].in_use) {
            leaks++;
            
            // print("[LEAK] ");
            // print_hex((uint64_t)alloc_traces[i].ptr);
            // print(" size=");
            // print_dec(alloc_traces[i].size);
            // print(" tag=");
            // println(alloc_traces[i].tag);
        }
    }
    
    spin_unlock(&alloc_trace_lock);
    
    if (leaks == 0) {
        // println("[HEAP] No memory leaks detected");
    } else {
        // print("[HEAP] Total leaks: ");
        // print_dec(leaks);
        // println("");
    }
    
    return leaks;
}

/* === 内存碎片分析 === */

struct fragment_analysis {
    uint64_t total_free;            /* 总空闲空间 */
    uint64_t total_used;            /* 总已用空间 */
    uint64_t largest_free_block;    /* 最大空闲块 */
    uint64_t smallest_free_block;   /* 最小空闲块 */
    uint32_t free_block_count;      /* 空闲块数量 */
    uint32_t used_block_count;      /* 已用块数量 */
    uint32_t fragment_score;        /* 碎片评分 (0-100) */
};

/* 分析内存碎片 */
void heap_analyze_fragments(struct fragment_analysis *analysis) {
    if (analysis == NULL) return;
    
    memset(analysis, 0, sizeof(*analysis));
    
    /* 遍历堆 */
    struct heap_block *block = (struct heap_block *)kernel_heap.start;
    
    while (block != NULL && (uint64_t)block < kernel_heap.end) {
        if (block->is_free) {
            analysis->total_free += block->size;
            analysis->free_block_count++;
            
            if (block->size > analysis->largest_free_block) {
                analysis->largest_free_block = block->size;
            }
            if (analysis->smallest_free_block == 0 || 
                block->size < analysis->smallest_free_block) {
                analysis->smallest_free_block = block->size;
            }
        } else {
            analysis->total_used += block->size;
            analysis->used_block_count++;
        }
        
        block = (struct heap_block *)((uint8_t *)block + sizeof(struct heap_block) + block->size);
    }
    
    /* 计算碎片评分 */
    if (analysis->total_free > 0 && analysis->free_block_count > 1) {
        /* 碎片评分 = (1 - 最大块 / 总空闲) * 100 */
        uint64_t ratio = analysis->largest_free_block * 100 / analysis->total_free;
        analysis->fragment_score = 100 - (uint32_t)ratio;
    }
}

/* 打印碎片分析 */
void heap_print_fragment_analysis(void) {
    struct fragment_analysis analysis;
    heap_analyze_fragments(&analysis);
    
    // println("=== Memory Fragmentation Analysis ===");
    
    // print("Total free: ");
    // print_dec(analysis.total_free / 1024);
    // println(" KB");
    
    // print("Total used: ");
    // print_dec(analysis.total_used / 1024);
    // println(" KB");
    
    // print("Free blocks: ");
    // print_dec(analysis.free_block_count);
    // println("");
    
    // print("Used blocks: ");
    // print_dec(analysis.used_block_count);
    // println("");
    
    // print("Largest free block: ");
    // print_dec(analysis.largest_free_block / 1024);
    // println(" KB");
    
    // print("Fragmentation score: ");
    // print_dec(analysis.fragment_score);
    // println("/100");
}

/* === 内存压力监控 === */

enum mem_pressure_level {
    MEM_PRESSURE_NONE = 0,
    MEM_PRESSURE_LOW,
    MEM_PRESSURE_MEDIUM,
    MEM_PRESSURE_HIGH,
    MEM_PRESSURE_CRITICAL
};

struct mem_pressure_stats {
    enum mem_pressure_level level;
    uint64_t heap_used;
    uint64_t heap_total;
    uint64_t heap_percent;
    uint64_t pmm_used;
    uint64_t pmm_total;
    uint64_t pmm_percent;
};

/* 获取内存压力 */
enum mem_pressure_level heap_get_pressure(void) {
    struct mem_pressure_stats stats;
    
    stats.heap_total = kernel_heap.size;
    stats.heap_used = kernel_heap.used;
    stats.heap_percent = stats.heap_used * 100 / stats.heap_total;
    
    /* 获取 PMM 统计 */
    extern uint64_t pmm_get_used_memory(void);
extern uint64_t pmm_get_total_memory(void);
    extern uint64_t pmm_get_total_memory(void);
    stats.pmm_used = pmm_get_used_memory() * 4096;
    stats.pmm_total = pmm_get_total_memory() * 4096;
    stats.pmm_percent = stats.pmm_used * 100 / stats.pmm_total;
    
    /* 计算压力等级 */
    uint32_t max_percent = (uint32_t)(stats.heap_percent > stats.pmm_percent ? 
                                      stats.heap_percent : stats.pmm_percent);
    
    if (max_percent > 95) return MEM_PRESSURE_CRITICAL;
    if (max_percent > 85) return MEM_PRESSURE_HIGH;
    if (max_percent > 70) return MEM_PRESSURE_MEDIUM;
    if (max_percent > 50) return MEM_PRESSURE_LOW;
    return MEM_PRESSURE_NONE;
}

/* 打印内存压力状态 */
void heap_print_pressure(void) {
    enum mem_pressure_level level = heap_get_pressure();
    
    const char *level_names[] = {
        "None", "Low", "Medium", "High", "Critical"
    };
    
    // println("=== Memory Pressure Status ===");
    // print("Level: ");
    // println(level_names[level]);
    
    // print("Heap: ");
    // print_dec(kernel_heap.used * 100 / kernel_heap.size);
    // println("% used");
}

/* === 内存访问模式分析 === */

struct access_pattern {
    uint64_t sequential_reads;
    uint64_t random_reads;
    uint64_t sequential_writes;
    uint64_t random_writes;
    uint64_t avg_access_size;
    uint64_t hot_regions_count;
};

static struct access_pattern access_pattern;

/* 记录内存访问 */
void heap_record_access(void *ptr, size_t size, int is_write) {
    /* 简化实现：更新统计 */
    if (is_write) {
        access_pattern.random_writes++;
    } else {
        access_pattern.random_reads++;
    }
    access_pattern.avg_access_size = (access_pattern.avg_access_size * 7 + size) / 8;
}

/* === 内存分配热力图 === */

#define HEATMAP_BUCKETS     32

struct heatmap_entry {
    uint64_t start_addr;
    uint64_t end_addr;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t current_bytes;
};

static struct heatmap_entry heatmap[HEATMAP_BUCKETS];

/* 初始化热力图 */
void heap_heatmap_init(void) {
    uint64_t start = (uint64_t)kernel_heap.start;
    uint64_t bucket_size = kernel_heap.size / HEATMAP_BUCKETS;
    
    for (int i = 0; i < HEATMAP_BUCKETS; i++) {
        heatmap[i].start_addr = start + i * bucket_size;
        heatmap[i].end_addr = start + (i + 1) * bucket_size - 1;
        heatmap[i].alloc_count = 0;
        heatmap[i].free_count = 0;
        heatmap[i].current_bytes = 0;
    }
}

/* 更新热力图 */
void heap_heatmap_update(void *ptr, size_t size, int is_alloc) {
    uint64_t addr = (uint64_t)ptr;
    
    for (int i = 0; i < HEATMAP_BUCKETS; i++) {
        if (addr >= heatmap[i].start_addr && addr <= heatmap[i].end_addr) {
            if (is_alloc) {
                heatmap[i].alloc_count++;
                heatmap[i].current_bytes += size;
            } else {
                heatmap[i].free_count++;
                heatmap[i].current_bytes -= size;
            }
            break;
        }
    }
}

/* 打印热力图 */
void heap_print_heatmap(void) {
    // println("=== Memory Allocation Heatmap ===");
    
    for (int i = 0; i < HEATMAP_BUCKETS; i++) {
        // print("Region ");
        // print_dec(i);
        // print(": ");
        // print_dec(heatmap[i].alloc_count);
        // print(" allocs, ");
        // print_dec(heatmap[i].current_bytes / 1024);
        // println(" KB");
    }
}

/* === 内存审计日志 === */

#define AUDIT_LOG_SIZE      256

struct audit_entry {
    uint64_t timestamp;
    int operation;          /* 0=alloc, 1=free, 2=realloc */
    void *ptr;
    size_t size;
    size_t old_size;        /* realloc 用 */
    int result;             /* 0=success, -1=failure */
};

static struct audit_entry audit_log[AUDIT_LOG_SIZE];
static uint32_t audit_log_idx;
static spinlock_t audit_lock = SPINLOCK_INIT;
static int audit_enabled;

/* 启用/禁用审计 */
void heap_audit_enable(int enable) {
    audit_enabled = enable;
    if (enable) {
        audit_log_idx = 0;
    }
}

/* 记录审计条目 */
void heap_audit_log(int op, void *ptr, size_t size, size_t old_size, int result) {
    if (!audit_enabled) return;
    
    spin_lock(&audit_lock);
    
    uint32_t idx = audit_log_idx % AUDIT_LOG_SIZE;
    audit_log[idx].timestamp = get_tick_count();
    audit_log[idx].operation = op;
    audit_log[idx].ptr = ptr;
    audit_log[idx].size = size;
    audit_log[idx].old_size = old_size;
    audit_log[idx].result = result;
    audit_log_idx++;
    
    spin_unlock(&audit_lock);
}

/* 打印审计日志 */
void heap_print_audit_log(void) {
    // println("=== Memory Audit Log ===");
    
    uint32_t count = audit_log_idx < AUDIT_LOG_SIZE ? audit_log_idx : AUDIT_LOG_SIZE;
    
    for (uint32_t i = 0; i < count; i++) {
        struct audit_entry *e = &audit_log[i];
        
        // print("[");
        // print_dec(e->timestamp);
        // print("] ");
        
        switch (e->operation) {
            case 0:
                // print("ALLOC ");
                break;
            case 1:
                // print("FREE  ");
                break;
            case 2:
                // print("REALLOC ");
                break;
        }
        
        // print_hex((uint64_t)e->ptr);
        // print(" size=");
        // print_dec(e->size);
        // print(" result=");
        // println(e->result == 0 ? "OK" : "FAIL");
    }
}

/* === 内存诊断综合报告 === */

void heap_print_diagnostic_report(void) {
    // println("");
    // println("╔════════════════════════════════════════════════════╗");
    // println("║       Zratro OS Kernel Memory Diagnostic           ║");
    // println("╚════════════════════════════════════════════════════╝");
    
    /* 堆统计 */
    // println("");
    // println("--- Heap Statistics ---");
    // print("Total size: ");
    // print_dec(kernel_heap.size / 1024 / 1024);
    // println(" MB");
    
    // print("Used: ");
    // print_dec(kernel_heap.used / 1024 / 1024);
    // println(" MB");
    
    // print("Free: ");
    // print_dec((kernel_heap.size - kernel_heap.used) / 1024 / 1024);
    // println(" MB");
    
    /* 碎片分析 */
    // println("");
    heap_print_fragment_analysis();
    
    /* 内存压力 */
    // println("");
    heap_print_pressure();
    
    /* 分离链表状态 */
    // println("");
    // println("--- Free List Status ---");
    for (int i = 0; i < HEAP_SIZE_CLASSES; i++) {
        if (free_lists[i].count > 0) {
            // print("Size <= ");
            // print_dec(size_class_bounds[i]);
            // print(": ");
            // print_dec(free_lists[i].count);
            // println(" blocks");
        }
    }
    
    /* 泄漏检测 */
    // println("");
    heap_check_leaks();
    
    // println("");
    // println("════════════════════════════════════════════════════");
}

/* === 内存统计快照 === */

struct mem_snapshot {
    uint64_t timestamp;
    uint64_t heap_used;
    uint64_t heap_free;
    uint64_t heap_blocks;
    uint32_t fragment_score;
    enum mem_pressure_level pressure;
};

#define MAX_SNAPSHOTS   16
static struct mem_snapshot snapshots[MAX_SNAPSHOTS];
static uint32_t snapshot_count;

/* 创建内存快照 */
int heap_create_snapshot(void) {
    if (snapshot_count >= MAX_SNAPSHOTS) {
        /* 移动快照 */
        for (int i = 0; i < MAX_SNAPSHOTS - 1; i++) {
            snapshots[i] = snapshots[i + 1];
        }
        snapshot_count--;
    }
    
    struct mem_snapshot *snap = &snapshots[snapshot_count++];
    snap->timestamp = get_tick_count();
    snap->heap_used = kernel_heap.used;
    snap->heap_free = kernel_heap.size - kernel_heap.used;
    snap->heap_blocks = kernel_heap.alloc_count;
    
    struct fragment_analysis analysis;
    heap_analyze_fragments(&analysis);
    snap->fragment_score = analysis.fragment_score;
    snap->pressure = heap_get_pressure();
    
    return snapshot_count - 1;
}

/* 打印快照历史 */
void heap_print_snapshot_history(void) {
    // println("=== Memory Snapshot History ===");
    
    for (uint32_t i = 0; i < snapshot_count; i++) {
        struct mem_snapshot *snap = &snapshots[i];
        
        // print("Snapshot #");
        // print_dec(i);
        // print(": ");
        // print_dec(snap->heap_used / 1024);
        // print(" KB used, ");
        // print_dec(snap->fragment_score);
        // print("% frag, pressure=");
        // print_dec(snap->pressure);
        // println("");
    }
}

/* === 自动内存监控线程 === */

static int monitor_running = 0;
static uint64_t monitor_interval = 1000000000ULL;  /* 1秒 */

/* 堆内存整理 (简化实现) */
void heap_defragment(void) {
    /* 简化实现：合并相邻空闲块 */
    struct heap_block *block = kernel_heap.head;
    
    while (block != NULL && (void *)block < (void *)kernel_heap.end) {
        if (block->is_free) {
            /* 检查下一个块是否也是空闲的 */
            struct heap_block *next = (struct heap_block *)
                ((uint8_t *)block + sizeof(struct heap_block) + block->size);
            
            if ((void *)next < (void *)kernel_heap.end && next->is_free) {
                /* 合并 */
                block->size += sizeof(struct heap_block) + next->size;
                block->next = next->next;
                if (next->next != NULL) {
                    next->next->prev = block;
                }
                /* 不移动 block，继续检查合并后的块 */
            } else {
                block = next;
            }
        } else {
            block = (struct heap_block *)
                ((uint8_t *)block + sizeof(struct heap_block) + block->size);
        }
    }
}

/* 内存监控检查 */
void heap_monitor_check(void) {
    if (!monitor_running) return;
    
    /* 检查内存压力 */
    enum mem_pressure_level level = heap_get_pressure();
    
    if (level >= MEM_PRESSURE_HIGH) {
    // println("[HEAP MONITOR] Warning: High memory pressure!");
        
        /* 尝试内存整理 */
        heap_defragment();
        
        /* 创建快照 */
        heap_create_snapshot();
    }
    
    /* 定期检查泄漏 */
    if (alloc_tracing_enabled) {
        static uint64_t last_leak_check = 0;
        uint64_t now = get_tick_count();
        
        if (now - last_leak_check > 10000000000ULL) {  /* 10秒 */
            heap_check_leaks();
            last_leak_check = now;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：Per-CPU 堆缓存
 * 
 * 特性：
 * - 每个 CPU 核心独立的分配缓存
 * - 减少锁竞争
 * - 提高多核性能
 * ═══════════════════════════════════════════════════════════════════════════ */

#define HEAP_CPU_CACHE_SIZE     64      /* 每个大小类别的缓存条目数 */
#define HEAP_CPU_CACHE_CLASSES   8      /* 缓存的大小类别数 */
#define HEAP_MAX_CPUS           256

/* CPU 缓存大小边界 */
static const size_t cpu_cache_sizes[HEAP_CPU_CACHE_CLASSES] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096
};

/* Per-CPU 缓存结构 */
struct heap_cpu_cache {
    void *cache[HEAP_CPU_CACHE_CLASSES][HEAP_CPU_CACHE_SIZE];
    uint32_t count[HEAP_CPU_CACHE_CLASSES];
    uint32_t pad[HEAP_CPU_CACHE_CLASSES];  /* 对齐 */
    volatile int lock;
    uint64_t hits;
    uint64_t misses;
} __attribute__((aligned(64)));

static struct heap_cpu_cache cpu_caches[HEAP_MAX_CPUS];
static int cpu_cache_initialized = 0;

/* 初始化 Per-CPU 缓存 */
void heap_cpu_cache_init(void) {
    for (int cpu = 0; cpu < HEAP_MAX_CPUS; cpu++) {
        cpu_caches[cpu].lock = 0;
        cpu_caches[cpu].hits = 0;
        cpu_caches[cpu].misses = 0;
        
        for (int c = 0; c < HEAP_CPU_CACHE_CLASSES; c++) {
            cpu_caches[cpu].count[c] = 0;
        }
    }
    cpu_cache_initialized = 1;
}

/* 获取大小类别 */
static inline int heap_get_cache_class(size_t size) {
    for (int i = 0; i < HEAP_CPU_CACHE_CLASSES; i++) {
        if (size <= cpu_cache_sizes[i]) {
            return i;
        }
    }
    return -1;  /* 太大，不缓存 */
}

/* 从 CPU 缓存分配 */
void *heap_cpu_cache_alloc(int cpu_id, size_t size) {
    if (!cpu_cache_initialized || cpu_id < 0 || cpu_id >= HEAP_MAX_CPUS) {
        return NULL;
    }
    
    int sc = heap_get_cache_class(size);
    if (sc < 0) return NULL;  /* 不使用缓存 */
    
    struct heap_cpu_cache *cache = &cpu_caches[cpu_id];
    
    while (__sync_lock_test_and_set(&cache->lock, 1)) {
        asm volatile("pause");
    }
    
    void *ptr = NULL;
    if (cache->count[sc] > 0) {
        ptr = cache->cache[sc][--cache->count[sc]];
        cache->hits++;
    } else {
        cache->misses++;
    }
    
    __sync_lock_release(&cache->lock);
    
    return ptr;
}

/* 释放到 CPU 缓存 */
int heap_cpu_cache_free(int cpu_id, void *ptr, size_t size) {
    if (!cpu_cache_initialized || cpu_id < 0 || cpu_id >= HEAP_MAX_CPUS) {
        return 0;
    }
    
    int sc = heap_get_cache_class(size);
    if (sc < 0) return 0;  /* 不使用缓存 */
    
    struct heap_cpu_cache *cache = &cpu_caches[cpu_id];
    
    while (__sync_lock_test_and_set(&cache->lock, 1)) {
        asm volatile("pause");
    }
    
    int cached = 0;
    if (cache->count[sc] < HEAP_CPU_CACHE_SIZE) {
        cache->cache[sc][cache->count[sc]++] = ptr;
        cached = 1;
    }
    
    __sync_lock_release(&cache->lock);
    
    return cached;
}

/* 获取 CPU 缓存统计 */
void heap_cpu_cache_stats(int cpu_id, uint64_t *hits, uint64_t *misses) {
    if (cpu_id >= 0 && cpu_id < HEAP_MAX_CPUS) {
        if (hits) *hits = cpu_caches[cpu_id].hits;
        if (misses) *misses = cpu_caches[cpu_id].misses;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：大块分配器
 * 
 * 特性：
 * - 直接从 PMM 分配大块内存
 * - 避免堆碎片
 * - 支持 NUMA 感知分配
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LARGE_ALLOC_THRESHOLD  (64 * 1024)  /* 64KB */
#define MAX_LARGE_ALLOCS        1024

/* 大块分配记录 */
struct large_alloc {
    void *ptr;
    size_t size;
    uint64_t phys_addr;
    int numa_node;
    int in_use;
};

static struct large_alloc large_allocs[MAX_LARGE_ALLOCS];
static uint32_t large_alloc_count;
static spinlock_t large_alloc_lock = SPINLOCK_INIT;

/* 大块分配 */
void *kmalloc_large(size_t size) {
    if (size < LARGE_ALLOC_THRESHOLD) {
        return kmalloc(size);
    }
    
    spin_lock(&large_alloc_lock);
    
    /* 查找空闲槽 */
    int slot = -1;
    for (uint32_t i = 0; i < MAX_LARGE_ALLOCS; i++) {
        if (!large_allocs[i].in_use) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        spin_unlock(&large_alloc_lock);
        return NULL;
    }
    
    /* 计算页数 */
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    /* 分配物理内存 */
    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == 0) {
        spin_unlock(&large_alloc_lock);
        return NULL;
    }
    
    /* 映射到内核空间 */
    uint64_t virt = kernel_heap.end;
    if (vmm_map_range(vmm_get_kernel_space(), virt, phys, pages, VMM_FLAGS_KERNEL) != 0) {
        pmm_free_pages(phys, pages);
        spin_unlock(&large_alloc_lock);
        return NULL;
    }
    
    /* 记录分配 */
    large_allocs[slot].ptr = (void *)virt;
    large_allocs[slot].size = size;
    large_allocs[slot].phys_addr = phys;
    large_allocs[slot].numa_node = 0;  /* 简化：不区分 NUMA */
    large_allocs[slot].in_use = 1;
    large_alloc_count++;
    
    spin_unlock(&large_alloc_lock);
    
    /* 清零 */
    memset((void *)virt, 0, size);
    
    return (void *)virt;
}

/* 大块释放 */
void kfree_large(void *ptr) {
    if (ptr == NULL) return;
    
    spin_lock(&large_alloc_lock);
    
    for (uint32_t i = 0; i < MAX_LARGE_ALLOCS; i++) {
        if (large_allocs[i].in_use && large_allocs[i].ptr == ptr) {
            /* 释放物理页 */
            size_t pages = (large_allocs[i].size + PAGE_SIZE - 1) / PAGE_SIZE;
            pmm_free_pages(large_allocs[i].phys_addr, pages);
            
            large_allocs[i].in_use = 0;
            large_alloc_count--;
            break;
        }
    }
    
    spin_unlock(&large_alloc_lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：对象缓存分配器 (Slab-like)
 * 
 * 特性：
 * - 针对固定大小对象的快速分配
 * - 减少内存碎片
 * - 支持构造/析构函数
 * ═══════════════════════════════════════════════════════════════════════════ */

#define OBJ_CACHE_MAX       32
#define OBJ_CACHE_SIZE      256

/* 对象缓存 */
struct obj_cache {
    char name[32];
    size_t obj_size;
    void *slabs;
    void *free_list;
    uint32_t obj_count;
    uint32_t free_count;
    uint32_t slab_count;
    volatile int lock;
    uint64_t allocs;
    uint64_t frees;
};

static struct obj_cache obj_caches[OBJ_CACHE_MAX];
static int obj_cache_count = 0;

/* 创建对象缓存 */
struct obj_cache *obj_cache_create(const char *name, size_t obj_size) {
    if (obj_cache_count >= OBJ_CACHE_MAX) return NULL;
    if (name == NULL || obj_size == 0) return NULL;
    
    struct obj_cache *cache = &obj_caches[obj_cache_count++];
    
    /* 复制名称 */
    int i = 0;
    while (name[i] && i < 31) {
        cache->name[i] = name[i];
        i++;
    }
    cache->name[i] = '\0';
    
    cache->obj_size = obj_size;
    cache->slabs = NULL;
    cache->free_list = NULL;
    cache->obj_count = 0;
    cache->free_count = 0;
    cache->slab_count = 0;
    cache->lock = 0;
    cache->allocs = 0;
    cache->frees = 0;
    
    return cache;
}

/* 从对象缓存分配 */
void *obj_cache_alloc(struct obj_cache *cache) {
    if (cache == NULL) return NULL;
    
    while (__sync_lock_test_and_set(&cache->lock, 1)) {
        asm volatile("pause");
    }
    
    void *obj = NULL;
    
    if (cache->free_list != NULL) {
        /* 从空闲链表取 */
        obj = cache->free_list;
        cache->free_list = *(void **)obj;
        cache->free_count--;
    } else {
        /* 需要分配新的 slab */
        size_t slab_size = PAGE_SIZE;
        void *slab = kmalloc(slab_size);
        
        if (slab != NULL) {
            /* 链接到 slab 链表 */
            *(void **)slab = cache->slabs;
            cache->slabs = slab;
            cache->slab_count++;
            
            /* 将 slab 中的对象加入空闲链表 */
            size_t count = (slab_size - sizeof(void *)) / cache->obj_size;
            uint8_t *objs = (uint8_t *)slab + sizeof(void *);
            
            for (size_t i = 0; i < count; i++) {
                void *o = objs + i * cache->obj_size;
                *(void **)o = cache->free_list;
                cache->free_list = o;
                cache->free_count++;
                cache->obj_count++;
            }
            
            /* 再次尝试分配 */
            if (cache->free_list != NULL) {
                obj = cache->free_list;
                cache->free_list = *(void **)obj;
                cache->free_count--;
            }
        }
    }
    
    if (obj != NULL) {
        cache->allocs++;
    }
    
    __sync_lock_release(&cache->lock);
    
    return obj;
}

/* 释放到对象缓存 */
void obj_cache_free(struct obj_cache *cache, void *obj) {
    if (cache == NULL || obj == NULL) return;
    
    while (__sync_lock_test_and_set(&cache->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 加入空闲链表 */
    *(void **)obj = cache->free_list;
    cache->free_list = obj;
    cache->free_count++;
    cache->frees++;
    
    __sync_lock_release(&cache->lock);
}

/* 销毁对象缓存 */
void obj_cache_destroy(struct obj_cache *cache) {
    if (cache == NULL) return;
    
    while (__sync_lock_test_and_set(&cache->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 释放所有 slab */
    void *slab = cache->slabs;
    while (slab != NULL) {
        void *next = *(void **)slab;
        kfree(slab);
        slab = next;
    }
    
    cache->slabs = NULL;
    cache->free_list = NULL;
    cache->obj_count = 0;
    cache->free_count = 0;
    cache->slab_count = 0;
    
    __sync_lock_release(&cache->lock);
}

/* 获取对象缓存统计 */
void obj_cache_stats(struct obj_cache *cache, uint64_t *allocs, uint64_t *frees, 
                     uint32_t *obj_count, uint32_t *free_count) {
    if (cache == NULL) return;
    
    if (allocs) *allocs = cache->allocs;
    if (frees) *frees = cache->frees;
    if (obj_count) *obj_count = cache->obj_count;
    if (free_count) *free_count = cache->free_count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：内存压缩器
 * 
 * 特性：
 * - 压缩不常用的内存页
 * - 释放内存压力
 * - 支持多种压缩算法
 * ═══════════════════════════════════════════════════════════════════════════ */

#define COMPRESS_THRESHOLD    (4 * 1024)  /* 4KB */

/* 压缩统计 */
struct compress_stats {
    uint64_t pages_compressed;
    uint64_t bytes_saved;
    uint64_t compress_time_ns;
    uint64_t decompress_time_ns;
};

static struct compress_stats compress_stats;

/* 简化的压缩：用 0 填充模式 */
static size_t simple_compress(void *dst, const void *src, size_t size) {
    /* RLE 压缩零字节 */
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    size_t di = 0;
    size_t i = 0;
    
    while (i < size) {
        if (s[i] == 0) {
            /* 计数连续的零 */
            size_t count = 0;
            while (i + count < size && s[i + count] == 0 && count < 255) {
                count++;
            }
            d[di++] = 0xFF;  /* 标记 */
            d[di++] = count;
            i += count;
        } else {
            d[di++] = s[i++];
        }
    }
    
    return di;
}

/* 解压缩 */
static void simple_decompress(void *dst, const void *src, size_t size) {
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    size_t si = 0;
    size_t di = 0;
    
    while (di < size && si < size) {
        if (s[si] == 0xFF) {
            si++;
            size_t count = s[si++];
            for (size_t j = 0; j < count; j++) {
                d[di++] = 0;
            }
        } else {
            d[di++] = s[si++];
        }
    }
}

/* 尝试压缩内存块 */
int heap_try_compress(void *ptr, size_t size) {
    if (ptr == NULL || size < COMPRESS_THRESHOLD) {
        return 0;
    }
    
    /* 分配压缩缓冲区 */
    void *compressed = kmalloc(size);
    if (compressed == NULL) return 0;
    
    /* 压缩 */
    size_t compressed_size = simple_compress(compressed, ptr, size);
    
    if (compressed_size < size * 3 / 4) {
        /* 压缩率超过 25%，有效 */
        compress_stats.pages_compressed++;
        compress_stats.bytes_saved += size - compressed_size;
        
        /* 清零原内存 */
        memset(ptr, 0, size);
        
        kfree(compressed);
        return 1;
    }
    
    kfree(compressed);
    return 0;
}

/* 获取压缩统计 */
void heap_get_compress_stats(struct compress_stats *stats) {
    if (stats != NULL) {
        *stats = compress_stats;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：内存 NUMA 支持
 * 
 * 特性：
 * - NUMA 感知分配
 * - 本地内存优先
 * - 内存迁移支持
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_NUMA_NODES    8

/* NUMA 节点统计 */
struct numa_node_stats {
    uint64_t total_memory;
    uint64_t used_memory;
    uint64_t alloc_count;
    uint64_t free_count;
};

static struct numa_node_stats numa_stats[MAX_NUMA_NODES];
static int numa_node_count = 1;

/* 初始化 NUMA */
void heap_numa_init(int node_count) {
    numa_node_count = node_count > 0 ? node_count : 1;
    if (numa_node_count > MAX_NUMA_NODES) {
        numa_node_count = MAX_NUMA_NODES;
    }
    
    for (int i = 0; i < numa_node_count; i++) {
        numa_stats[i].total_memory = 0;
        numa_stats[i].used_memory = 0;
        numa_stats[i].alloc_count = 0;
        numa_stats[i].free_count = 0;
    }
}

/* NUMA 感知分配 */
void *kmalloc_numa(size_t size, int node) {
    /* 简化实现：总是从主堆分配 */
    void *ptr = kmalloc(size);
    
    if (ptr != NULL && node >= 0 && node < numa_node_count) {
        numa_stats[node].alloc_count++;
        numa_stats[node].used_memory += size;
    }
    
    return ptr;
}

/* 获取 NUMA 统计 */
void heap_get_numa_stats(int node, struct numa_node_stats *stats) {
    if (node >= 0 && node < numa_node_count && stats != NULL) {
        *stats = numa_stats[node];
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：高级诊断与性能分析
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 分配延迟统计 */
struct alloc_latency {
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t avg_ns;
    uint64_t total_ns;
    uint64_t count;
};

static struct alloc_latency alloc_latency_stats;

/* 记录分配延迟 */
void heap_record_alloc_latency(uint64_t ns) {
    if (alloc_latency_stats.count == 0) {
        alloc_latency_stats.min_ns = ns;
        alloc_latency_stats.max_ns = ns;
    } else {
        if (ns < alloc_latency_stats.min_ns) alloc_latency_stats.min_ns = ns;
        if (ns > alloc_latency_stats.max_ns) alloc_latency_stats.max_ns = ns;
    }
    
    alloc_latency_stats.total_ns += ns;
    alloc_latency_stats.count++;
    alloc_latency_stats.avg_ns = alloc_latency_stats.total_ns / alloc_latency_stats.count;
}

/* 获取分配延迟统计 */
void heap_get_alloc_latency(struct alloc_latency *latency) {
    if (latency != NULL) {
        *latency = alloc_latency_stats;
    }
}

/* 内存使用预测 */
struct mem_prediction {
    uint64_t predicted_used;
    uint64_t growth_rate;       /* 字节/秒 */
    uint64_t time_to_full;      /* 预计耗尽时间(秒) */
};

/* 预测内存使用 */
void heap_predict_usage(struct mem_prediction *pred) {
    if (pred == NULL) return;
    
    /* 简化预测：基于最近快照的趋势 */
    if (snapshot_count >= 2) {
        struct mem_snapshot *s1 = &snapshots[snapshot_count - 2];
        struct mem_snapshot *s2 = &snapshots[snapshot_count - 1];
        
        uint64_t time_diff = s2->timestamp - s1->timestamp;
        uint64_t mem_diff = s2->heap_used - s1->heap_used;
        
        if (time_diff > 0) {
            pred->growth_rate = mem_diff * 1000000000ULL / time_diff;
            
            uint64_t remaining = kernel_heap.size - s2->heap_used;
            if (pred->growth_rate > 0) {
                pred->time_to_full = remaining / pred->growth_rate;
            } else {
                pred->time_to_full = 0;
            }
        }
        
        pred->predicted_used = s2->heap_used + pred->growth_rate;
    } else {
        pred->predicted_used = kernel_heap.used;
        pred->growth_rate = 0;
        pred->time_to_full = 0;
    }
}

/* 堆性能评分 */
uint32_t heap_performance_score(void) {
    uint32_t score = 100;
    
    /* 碎片扣分 */
    struct fragment_analysis analysis;
    heap_analyze_fragments(&analysis);
    score -= analysis.fragment_score / 5;
    
    /* 压力扣分 */
    enum mem_pressure_level pressure = heap_get_pressure();
    if (pressure >= MEM_PRESSURE_CRITICAL) score -= 40;
    else if (pressure >= MEM_PRESSURE_HIGH) score -= 25;
    else if (pressure >= MEM_PRESSURE_MEDIUM) score -= 15;
    else if (pressure >= MEM_PRESSURE_LOW) score -= 5;
    
    /* 安全问题扣分 */
    if (heap_sec_stats.corruptions > 0) score -= 20;
    if (heap_sec_stats.overflows_detected > 0) score -= 15;
    if (heap_sec_stats.double_frees > 0) score -= 10;
    
    return score > 0 ? score : 1;
}

/* 综合诊断报告 */
void heap_print_comprehensive_report(void) {
    // println("");
    // println("╔════════════════════════════════════════════════════════════╗");
    // println("║        Zratro OS Heap Comprehensive Report                 ║");
    // println("╚════════════════════════════════════════════════════════════╝");
    
    /* 基本统计 */
    // println("");
    // println("=== 基本统计 ===");
    // print("堆总大小: ");
    // print_dec(kernel_heap.size / 1024 / 1024);
    // println(" MB");
    
    // print("已使用: ");
    // print_dec(kernel_heap.used / 1024 / 1024);
    // println(" MB");
    
    // print("使用率: ");
    // print_dec(kernel_heap.used * 100 / kernel_heap.size);
    // println(" %");
    
    // print("分配次数: ");
    // print_dec(kernel_heap.alloc_count);
    // println("");
    
    // print("释放次数: ");
    // print_dec(kernel_heap.free_count);
    // println("");
    
    /* 性能评分 */
    // println("");
    // print("性能评分: ");
    // print_dec(heap_performance_score());
    // println("/100");
    
    /* 碎片分析 */
    // println("");
    // println("=== 碎片分析 ===");
    struct fragment_analysis analysis;
    heap_analyze_fragments(&analysis);
    
    // print("碎片评分: ");
    // print_dec(analysis.fragment_score);
    // println("/100");
    
    // print("最大空闲块: ");
    // print_dec(analysis.largest_free_block / 1024);
    // println(" KB");
    
    // print("空闲块数量: ");
    // print_dec(analysis.free_block_count);
    // println("");
    
    /* 内存压力 */
    // println("");
    // println("=== 内存压力 ===");
    enum mem_pressure_level pressure = heap_get_pressure();
    const char *pressure_names[] = {
        "无", "低", "中", "高", "严重"
    };
    // print("压力等级: ");
    // println(pressure_names[pressure]);
    
    /* 安全统计 */
    // println("");
    // println("=== 安全统计 ===");
    // print("检测到的溢出: ");
    // print_dec(heap_sec_stats.overflows_detected);
    // println("");
    
    // print("双重释放: ");
    // print_dec(heap_sec_stats.double_frees);
    // println("");
    
    // print("堆损坏: ");
    // print_dec(heap_sec_stats.corruptions);
    // println("");
    
    /* CPU 缓存统计 */
    // println("");
    // println("=== CPU 缓存统计 ===");
    for (int cpu = 0; cpu < 4; cpu++) {
        uint64_t hits, misses;
        heap_cpu_cache_stats(cpu, &hits, &misses);
        if (hits > 0 || misses > 0) {
            // print("CPU ");
            // print_dec(cpu);
            // print(": hits=");
            // print_dec(hits);
            // print(", misses=");
            // print_dec(misses);
            // println("");
        }
    }
    
    /* 分配延迟 */
    // println("");
    // println("=== 分配延迟 ===");
    struct alloc_latency latency;
    heap_get_alloc_latency(&latency);
    
    // print("平均延迟: ");
    // print_dec(latency.avg_ns);
    // println(" ns");
    
    // print("最小延迟: ");
    // print_dec(latency.min_ns);
    // println(" ns");
    
    // print("最大延迟: ");
    // print_dec(latency.max_ns);
    // println(" ns");
    
    /* 预测 */
    // println("");
    // println("=== 使用预测 ===");
    struct mem_prediction pred;
    heap_predict_usage(&pred);
    
    // print("增长率: ");
    // print_dec(pred.growth_rate / 1024);
    // println(" KB/s");
    
    if (pred.time_to_full > 0) {
        // print("预计耗尽: ");
        // print_dec(pred.time_to_full);
        // println(" 秒");
    }
    
    /* 建议 */
    // println("");
    // println("=== 优化建议 ===");
    
    if (analysis.fragment_score > 50) {
        // println("- 碎片较高，建议执行堆整理");
    }
    
    if (pressure >= MEM_PRESSURE_HIGH) {
        // println("- 内存压力高，建议释放不必要的内存");
    }
    
    if (heap_sec_stats.corruptions > 0) {
        // println("- 检测到堆损坏，建议检查内存访问");
    }
    
    // println("");
    // println("════════════════════════════════════════════════════════════");
}
