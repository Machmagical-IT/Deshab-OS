/*
 * Zratro OS - Copy-On-Write (COW) Memory Management
 * 
 * 写时复制内存管理，用于 fork() 等操作的高效内存共享
 */

#ifndef COW_H
#define COW_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "vmm.h"

/* ========== COW 配置 ========== */

#define COW_MAGIC           0x434F574D      /* "COWM" */

/* COW 页状态 */
#define COW_PAGE_SHARED     0               /* 共享页 (只读) */
#define COW_PAGE_PRIVATE    1               /* 私有页 (已复制) */
#define COW_PAGE_COW        2               /* COW 标记页 */

/* ========== 引用计数页结构 ========== */

/* 物理页引用计数 */
struct page_ref {
    uint64_t phys_addr;         /* 物理地址 */
    volatile int ref_count;     /* 引用计数 */
    spinlock_t lock;            /* 保护锁 */
    uint32_t flags;             /* 标志 */
    struct list_head list;      /* 链表 */
};

/* ========== COW 区域 ========== */

struct cow_region {
    uint64_t start;             /* 起始虚拟地址 */
    uint64_t end;               /* 结束虚拟地址 */
    uint64_t size;              /* 大小 */
    
    /* 页引用数组 */
    struct page_ref **pages;    /* 每页的引用 */
    size_t page_count;          /* 页数 */
    
    /* 原始页表项 (用于恢复) */
    uint64_t *orig_ptes;        /* 原始 PTE 值 */
    
    /* 所属地址空间 */
    struct address_space *space;
    
    /* 链表 */
    struct list_head list;
};

/* ========== COW 管理器 ========== */

struct cow_manager {
    /* 页引用哈希表 */
    #define COW_HASH_BITS    10
    #define COW_HASH_SIZE    (1 << COW_HASH_BITS)
    struct list_head page_hash[COW_HASH_SIZE];
    spinlock_t hash_lock[COW_HASH_SIZE];
    
    /* 统计 */
    uint64_t total_shared_pages;    /* 总共享页数 */
    uint64_t total_private_pages;   /* 总私有页数 */
    uint64_t cow_faults;            /* COW 缺页次数 */
    uint64_t cow_breaks;            /* COW 断开次数 */
    
    /* 页引用缓存 */
    struct page_ref *ref_cache;
    size_t ref_cache_size;
    size_t ref_cache_used;
};

/* ========== 全局 COW 管理器 ========== */

extern struct cow_manager cow_mgr;

/* ========== 函数原型 ========== */

/* 初始化 */
int cow_init(void);

/* 创建 COW 区域 */
struct cow_region *cow_create_region(struct address_space *space,
                                      uint64_t start, uint64_t size);

/* 销毁 COW 区域 */
void cow_destroy_region(struct cow_region *region);

/* 复制地址空间 (用于 fork) */
int cow_copy_address_space(struct address_space *dst, struct address_space *src);

/* 处理 COW 缺页 */
int cow_handle_page_fault(uint64_t fault_addr, uint64_t error_code);

/* 标记页面为 COW */
int cow_mark_page_cow(struct address_space *space, uint64_t virt_addr);

/* 获取/增加页引用 */
struct page_ref *cow_get_page_ref(uint64_t phys_addr);
void cow_put_page_ref(struct page_ref *ref);
int cow_inc_page_ref(uint64_t phys_addr);
int cow_dec_page_ref(uint64_t phys_addr);

/* 断开 COW (复制页面) */
int cow_break_page(uint64_t virt_addr, struct page_ref *ref);

/* 获取引用计数 */
static inline int cow_get_refcount(uint64_t phys_addr) {
    struct page_ref *ref = cow_get_page_ref(phys_addr);
    if (ref == NULL) return 0;
    int count = ref->ref_count;
    return count;
}

/* 检查是否为 COW 页 */
int cow_is_cow_page(struct address_space *space, uint64_t virt_addr);

/* ========== 缺页错误码 ========== */

#define PF_ERROR_PRESENT    (1 << 0)    /* 页存在 */
#define PF_ERROR_WRITE      (1 << 1)    /* 写操作 */
#define PF_ERROR_USER       (1 << 2)    /* 用户态 */
#define PF_ERROR_RSVD       (1 << 3)    /* 保留位 */
#define PF_ERROR_FETCH      (1 << 4)    /* 指令获取 */

/* ========== 辅助函数 ========== */

/* 哈希函数 */
static inline uint32_t cow_hash_addr(uint64_t phys_addr) {
    return (phys_addr >> 12) & (COW_HASH_SIZE - 1);
}

/* 对齐到页 */
static inline uint64_t cow_page_align(uint64_t addr) {
    return (addr + 0xFFF) & ~0xFFFULL;
}

static inline uint64_t cow_page_align_down(uint64_t addr) {
    return addr & ~0xFFFULL;
}

#endif /* COW_H */
