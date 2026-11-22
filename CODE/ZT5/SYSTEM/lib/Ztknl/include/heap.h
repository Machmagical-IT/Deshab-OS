/*
 * Zratro OS - Safe Kernel Heap Allocator
 * 
 * 增强容错性的内核堆分配器
 */

#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * 堆内存块结构
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 块魔数 - 用于完整性验证 */
#define HEAP_BLOCK_MAGIC    0xDEADBEEFCAFEBABEULL
#define HEAP_BLOCK_FREE     0xFEEDFACEDEADC0DEULL

/* 堆内存块头部 */
struct heap_block {
    uint64_t magic;               /* 魔数验证 */
    uint64_t size;                /* 块大小 (不含头部) */
    uint32_t is_free;             /* 是否空闲 (使用32位避免对齐问题) */
    uint32_t guard1;              /* 前保护字 */
    struct heap_block *next;      /* 下一个物理相邻块 */
    struct heap_block *prev;      /* 上一个物理相邻块 */
    struct heap_block *free_next; /* 空闲链表中的下一个块 */
    struct heap_block *free_prev; /* 空闲链表中的上一个块 */
    uint32_t guard2;              /* 后保护字 */
    uint32_t reserved;            /* 保留对齐 */
} __attribute__((aligned(16)));

/* 堆管理器 */
struct heap {
    struct heap_block *head;    /* 第一个块 */
    uint64_t start;             /* 堆起始地址 */
    uint64_t end;               /* 堆结束地址 */
    uint64_t size;              /* 堆总大小 */
    uint64_t used;              /* 已使用大小 */
    uint64_t alloc_count;       /* 分配次数 */
    uint64_t free_count;        /* 释放次数 */
    uint64_t total_alloc_size;  /* 累计分配字节数 */
    uint64_t total_free_size;   /* 累计释放字节数 */
    uint64_t peak_usage;        /* 峰值使用量 */
    volatile int lock;          /* 自旋锁 */
    int initialized;            /* 初始化标志 */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 核心分配函数
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 初始化内核堆 */
void heap_init(void);

/* 检查堆是否已初始化 */
static inline int heap_is_initialized(void) {
    extern struct heap kernel_heap;
    return kernel_heap.initialized;
}

/* 分配内存 */
void *kmalloc(size_t size);

/* 分配并清零内存 */
void *kzalloc(size_t size);

/* 分配内存 (带标志) */
#define KMALLOC_ZERO    0x01    /* 清零 */
#define KMALLOC_ATOMIC  0x02    /* 原子分配 (不睡眠) */
#define KMALLOC_NOWARN  0x04    /* 不打印警告 */

void *kmalloc_flags(size_t size, int flags);

/* 重新分配内存 */
void *krealloc(void *ptr, size_t size);

/* 重新分配并清零 */
void *krealloc_array(void *ptr, size_t nmemb, size_t size);

/* 释放内存 */
void kfree(void *ptr);

/* 安全释放 - 清零指针 */
static inline void kfree_safe(void *ptr_ptr) {
    if (ptr_ptr != NULL) {
        void **pp = (void **)ptr_ptr;
        if (*pp != NULL) {
            kfree(*pp);
            *pp = NULL;
        }
    }
}

/* 分配对齐内存 */
void *kvalloc(size_t size, size_t align);

/* 获取已分配块的大小 */
size_t ksize(void *ptr);

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全分配宏
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 安全分配 - 失败时返回 NULL 并清零内存 */
#define KMALLOC_SAFE(size, type) \
    ((type *)kzalloc(sizeof(type) * (size)))

/* 安全数组分配 */
#define KMALLOC_ARRAY(n, type) \
    ((type *)kmalloc((n) * sizeof(type)))

/* 安全数组分配并清零 */
#define KZALLOC_ARRAY(n, type) \
    ((type *)kzalloc((n) * sizeof(type)))

/* 安全释放宏 - 设置指针为 NULL */
#define KFREE_SAFE(pp) do { \
    if ((pp) != NULL && *(pp) != NULL) { \
        kfree(*(pp)); \
        *(pp) = NULL; \
    } \
} while (0)

/* ═══════════════════════════════════════════════════════════════════════════
 * 内存调试和统计
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 打印堆统计信息 */
void heap_stats(void);

/* 验证堆完整性 */
int heap_validate(void);

/* 检查指针是否在堆范围内 */
static inline int heap_contains(const void *ptr) {
    extern struct heap kernel_heap;
    uint64_t addr = (uint64_t)ptr;
    return addr >= kernel_heap.start && addr < kernel_heap.end;
}

/* 获取堆使用信息 */
static inline uint64_t heap_get_used(void) {
    extern struct heap kernel_heap;
    return kernel_heap.used;
}

static inline uint64_t heap_get_size(void) {
    extern struct heap kernel_heap;
    return kernel_heap.size;
}

static inline uint64_t heap_get_free(void) {
    extern struct heap kernel_heap;
    return kernel_heap.size - kernel_heap.used;
}

/* 获取堆分配统计 */
static inline uint64_t heap_get_total_alloc_size(void) {
    extern struct heap kernel_heap;
    return kernel_heap.total_alloc_size;
}

static inline uint64_t heap_get_total_free_size(void) {
    extern struct heap kernel_heap;
    return kernel_heap.total_free_size;
}

static inline uint64_t heap_get_peak_usage(void) {
    extern struct heap kernel_heap;
    return kernel_heap.peak_usage;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 容错辅助函数
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 安全复制 - 检查源和目标 */
void *kmemcpy_safe(void *dest, const void *src, size_t n);

/* 安全字符串复制到新分配内存 */
char *kstrdup(const char *s);

/* 安全字符串复制 (带长度限制) */
char *kstrndup(const char *s, size_t n);

/* 安全内存填充 */
void *kmemset_safe(void *s, int c, size_t n);

#endif /* HEAP_H */
