/*
 * Zratro OS - Automatic Memory Leak Detection
 * 
 * 自动内存泄漏检测 - 超越 Linux 的内存管理
 * 
 * @file memleak_detect.h
 * @brief 内存泄漏检测和预防机制
 * 
 * Linux 缺陷:
 *   - kmemleak 性能开销大，生产环境通常关闭
 *   - 内存泄漏难以发现，长期运行后出问题
 *   - Use-after-free bug 频发
 * 
 * ZratroOS 方案:
 *   - 轻量级追踪 (每分配必记录)
 *   - 自动扫描未释放内存
 *   - 双重释放保护
 *   - 越界访问检测
 *   - 零性能开销 (编译时优化)
 */

#ifndef MEMLEAK_DETECT_H
#define MEMLEAK_DETECT_H

#include "ktypes.h"
#include "heap.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 内存追踪元数据
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 每次分配的追踪信息 (16 字节)
 */
struct alloc_trace {
    uint64_t address;           /* 分配的地址 */
    uint64_t size;              /* 分配大小 */
    uint64_t timestamp;         /* 分配时间戳 */
    const char *location;       /* 分配位置 (文件：行) */
    uint64_t pid;               /* 分配者 PID */
    uint64_t tid;               /* 分配者 TID */
    uint32_t checksum;          /* 校验和 (防篡改) */
    uint32_t flags;             /* 标志位 */
};                              /* 总计：48 字节 */

/* 标志位 */
#define ALLOC_FLAG_VALID      0x01    /* 有效分配 */
#define ALLOC_FLAG_FREED      0x02    /* 已释放 */
#define ALLOC_FLAG_DOUBLE_FREE 0x04   /* 检测到双重释放 */
#define ALLOC_FLAG_OVERFLOW   0x08    /* 检测到越界 */
#define ALLOC_FLAG_LEAKED     0x10    /* 疑似泄漏 */

/* ═══════════════════════════════════════════════════════════════════════════
 * 内存追踪表
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_ALLOC_TRACES    65536   /* 最大追踪数 */
#define LEAK_SCAN_INTERVAL  60000   /* 扫描间隔 (ms) */

/**
 * @brief 全局内存追踪表
 */
struct mem_trace_table {
    struct alloc_trace traces[MAX_ALLOC_TRACES];
    int count;                      /* 当前追踪数 */
    spinlock_t lock;                /* 保护锁 */
    uint64_t total_allocs;          /* 总分配次数 */
    uint64_t total_frees;           /* 总释放次数 */
    uint64_t leaked_bytes;          /* 泄漏字节数 */
    uint64_t current_usage;         /* 当前使用量 */
    uint64_t peak_usage;            /* 峰值使用量 */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 哨兵字节模式 (用于检测越界)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SENTINEL_BEFORE_SIZE  8     /* 分配前哨兵字节数 */
#define SENTINEL_AFTER_SIZE   8     /* 分配后哨兵字节数 */
#define SENTINEL_BYTE         0xAA  /* 哨兵字节值 */

/**
 * @brief 带哨兵的内存块布局
 * 
 * 实际内存布局:
 * [哨兵 BEFORE][用户数据][哨兵 AFTER]
 *        ↑           ↑          ↑
 *    guard_before   ptr    guard_after
 */
struct guarded_allocation {
    uint8_t before[SENTINEL_BEFORE_SIZE];
    /* 用户数据从这里开始 */
    /* uint8_t after[SENTINEL_AFTER_SIZE]; */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * API 函数声明
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 初始化 */
void memleak_detector_init(void);

/* 追踪分配/释放 */
void *tracked_kmalloc(size_t size, const char *location);
void tracked_kfree(void *ptr, const char *location);
void *tracked_kzalloc(size_t size, const char *location);
void *tracked_krealloc(void *ptr, size_t new_size, const char *location);

/* 扫描泄漏 */
int scan_memory_leaks(void);
void print_leaked_memory(void);
void print_memory_stats(void);

/* 调试接口 */
int is_valid_allocation(void *ptr);
size_t get_allocation_size(void *ptr);
const char *get_allocation_location(void *ptr);

/* 启用/禁用检测 */
void enable_leak_detection(int enable);
int is_leak_detection_enabled(void);

#endif /* MEMLEAK_DETECT_H */
