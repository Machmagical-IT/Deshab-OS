#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

/* 页帧大小 */
#ifndef PAGE_SIZE
#define PAGE_SIZE       4096UL
#endif
#ifndef PAGE_SHIFT
#define PAGE_SHIFT      12
#endif

/* 内存类型 */
#define MEMORY_TYPE_USABLE          0
#define MEMORY_TYPE_RESERVED        1
#define MEMORY_TYPE_ACPI_RECLAIM    2
#define MEMORY_TYPE_ACPI_NVS        3
#define MEMORY_TYPE_UNUSABLE        4
#define MEMORY_TYPE_FRAMEBUFFER     5

/* 内存分配标志 */
#define PMM_ALLOC_ZERO      0x01    /* 清零页面 */
#define PMM_ALLOC_DMA       0x02    /* DMA 可用 (低 16MB) */

/* 前向声明 - 使用 limine.h 中的结构体 */
struct limine_memmap_response;
struct limine_hhdm_response;

/* 物理页帧分配器 */

/* 初始化物理内存管理器 */
void pmm_init(struct limine_memmap_response *memmap, struct limine_hhdm_response *hhdm);

/* 分配一个物理页，返回物理地址 */
uint64_t pmm_alloc_page(void);

/* 分配连续的多个物理页 */
uint64_t pmm_alloc_pages(size_t count);

/* 分配并清零页面 */
uint64_t pmm_alloc_zero_pages(size_t count);

/* 释放一个物理页 */
void pmm_free_page(uint64_t phys);

/* 释放连续的多个物理页 */
void pmm_free_pages(uint64_t phys, size_t count);

/* 获取总物理内存大小 */
uint64_t pmm_get_total_memory(void);

/* 获取可用物理内存大小 */
uint64_t pmm_get_free_memory(void);

/* 获取已使用物理内存大小 */
uint64_t pmm_get_used_memory(void);

/* 获取 HHDM 偏移 */
uint64_t pmm_get_hhdm_offset(void);

/* 检查物理地址是否有效 */
int pmm_is_valid_phys(uint64_t phys);

/* 检查物理地址是否已分配 */
int pmm_is_allocated(uint64_t phys);

/* 打印内存统计信息 */
void pmm_stats(void);

/* 物理地址到虚拟地址转换 */
/* 安全增强版本：添加 HHDM offset 有效性检查 */
static inline void *phys_to_virt(uint64_t phys) {
    extern uint64_t hhdm_offset;
    
    /* 安全检查：HHDM offset 必须有效 */
    if (hhdm_offset == 0) {
        /* 在早期初始化阶段，HHDM offset 可能为零
         * 此时应该使用 Limine 提供的直接映射 */
        return NULL;
    }
    
    /* 安全检查：物理地址应该对齐且在合理范围内 */
    /* 最大支持 64TB 物理内存 (2^46 bytes) */
    if (phys > 0x400000000000ULL) {
        return NULL;
    }
    
    return (void *)(phys + hhdm_offset);
}

/* 虚拟地址到物理地址转换 */
static inline uint64_t virt_to_phys(void *virt) {
    extern uint64_t hhdm_offset;
    
    /* 安全检查：HHDM offset 必须有效 */
    if (hhdm_offset == 0) {
        return 0;
    }
    
    /* 检查虚拟地址是否在 HHDM 区域内 */
    uint64_t virt_addr = (uint64_t)virt;
    
    /* HHDM 虚拟地址应该大于等于 HHDM offset */
    if (virt_addr < hhdm_offset) {
        return 0;  /* 不在 HHDM 区域内 */
    }
    
    uint64_t result = virt_addr - hhdm_offset;

    /* 结果应该在合理的物理地址范围内 (与 phys_to_virt 一致，最大 64TB) */
    if (result > 0x400000000000ULL) {
        return 0;  /* 不在 HHDM 区域内 */
    }
    
    return result;
}

/* 检查地址是否页对齐 */
static inline int is_page_aligned(uint64_t addr) {
    return (addr & (PAGE_SIZE - 1)) == 0;
}

/* 向上对齐到页边界 */
static inline uint64_t page_align_up(uint64_t addr) {
    return (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

/* 向下对齐到页边界 */
static inline uint64_t page_align_down(uint64_t addr) {
    return addr & ~(PAGE_SIZE - 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Phase 2 优化：Buddy 分配器、Per-CPU 页帧缓存 (PCP)、水印机制
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Buddy 系统最大阶数 (2^10 = 1024 页 = 4MB) */
#define BUDDY_MAX_ORDER     10

/* Per-CPU 页帧缓存 (PCP) 配置 */
#define PCP_BATCH           8       /* 批量转移数量 */
#define PCP_HIGH_DEFAULT    64      /* PCP 高水位 */
#define PCP_LOW_DEFAULT     16      /* PCP 低水位 */

/* PMM 水印级别 */
typedef enum {
    PMM_WATERMARK_MIN = 0,     /* 最低水位 - 紧急回收 */
    PMM_WATERMARK_LOW,         /* 低水位 - 开始后台回收 */
    PMM_WATERMARK_HIGH,        /* 高水位 - 回收目标 */
    PMM_WATERMARK_COUNT
} pmm_watermark_level_t;

/* Buddy 系统空闲链表节点 */
struct buddy_free_block {
    uint64_t page_index;           /* 起始页帧号 */
    struct buddy_free_block *next; /* 同阶空闲链表中的下一个 */
};

/* Per-CPU 页帧缓存 (类似 Linux per-cpu pagevec) */
struct pcp_cache {
    uint64_t pages[PCP_HIGH_DEFAULT];  /* 缓存的物理页地址 */
    int32_t count;                      /* 当前缓存页数 */
    int32_t high;                       /* 高水位 (超过则批量释放) */
    int32_t low;                        /* 低水位 (低于则批量补充) */
    int32_t batch;                      /* 批量转移数量 */
    uint64_t alloc_count;               /* 分配计数 */
    uint64_t free_count;                /* 释放计数 */
    volatile int lock;                  /* 自旋锁 */
};

/* PMM 水印配置 */
struct pmm_watermarks {
    uint64_t min_pages;    /* 最低水位页数 */
    uint64_t low_pages;    /* 低水位页数 */
    uint64_t high_pages;   /* 高水位页数 */
};

/* 初始化 Buddy 分配器 (在 pmm_init 之后调用) */
void pmm_buddy_init(void);

/* Buddy 分配: 分配 2^order 个连续页 */
uint64_t pmm_buddy_alloc(int order);

/* Buddy 释放: 释放 2^order 个连续页 */
void pmm_buddy_free(uint64_t phys, int order);

/* 初始化 Per-CPU 页帧缓存 */
void pmm_pcp_init(void);

/* 从 PCP 分配单页 */
uint64_t pmm_pcp_alloc(void);

/* 释放单页到 PCP */
void pmm_pcp_free(uint64_t phys);

/* 批量补充 PCP */
void pmm_pcp_refill(int cpu);

/* 批量排空 PCP */
void pmm_pcp_drain(int cpu);

/* 设置 PMM 水印 */
void pmm_set_watermarks(uint64_t min, uint64_t low, uint64_t high);

/* 获取 PMM 水印 */
struct pmm_watermarks pmm_get_watermarks(void);

/* 检查是否低于指定水位 */
int pmm_below_watermark(pmm_watermark_level_t level);

/* 触发水印回调 (当低于低水位时唤醒回收) */
void pmm_check_watermarks(void);

/* 注册水印回调 */
typedef void (*pmm_watermark_callback_t)(pmm_watermark_level_t level);
int pmm_register_watermark_callback(pmm_watermark_callback_t callback);

#endif