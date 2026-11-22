/*
 * Zratro OS - 交换分区与页面回收模块
 *
 * 实现交换分区管理、LRU 页面回收、kswapd 守护线程和内存压力通知
 */

#ifndef SWAP_H
#define SWAP_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== 交换分区配置 ========== */

#define SWAP_MAX_DEVICES        4       /* 最大交换设备数 */
#define SWAP_CLUSTER_SIZE       256     /* 交换簇大小 (页数) */
#define SWAP_MAP_SIZE           4096    /* 交换位图大小 */

/* ========== LRU 配置 ========== */

#define LRU_ACTIVE_RATIO       2       /* active/inactive 比率阈值 */
#define LRU_SCAN_BATCH         32      /* 每次扫描批量大页 */
#define LRU_MAX_PAGES          65536   /* LRU 链表最大页数 */

/* ========== kswapd 配置 ========== */

#define KSWAPD_WAKEUP_WATERMARK  128   /* 唤醒 kswapd 的低水位 (页数) */
#define KSWAPD_HIGH_WATERMARK    256   /* kswapd 目标高水位 (页数) */
#define KSWAPD_SCAN_INTERVAL     1000  /* kswapd 扫描间隔 (ticks) */

/* ========== 内存压力级别 ========== */

#define SWAP_PRESSURE_NONE      0
#define SWAP_PRESSURE_LOW       1
#define SWAP_PRESSURE_MEDIUM    2
#define SWAP_PRESSURE_HIGH      3
#define SWAP_PRESSURE_CRITICAL  4

/* ========== 交换设备结构 ========== */

struct swap_device {
    uint64_t device_id;        /* 设备标识 */
    uint64_t total_pages;      /* 总页数 */
    uint64_t used_pages;       /* 已用页数 */
    uint64_t priority;         /* 优先级 (越高越优先) */
    uint8_t *swap_map;         /* 交换位图 (每页引用计数) */
    int enabled;               /* 是否启用 */
};

/* ========== LRU 页面结构 ========== */

struct lru_page {
    uint64_t phys_addr;        /* 物理地址 */
    uint64_t virt_addr;        /* 虚拟地址 (0 表示未知) */
    uint32_t flags;            /* 页面标志 */
    uint32_t ref_count;        /* 引用计数 */
    uint64_t last_access;      /* 最后访问时间 (ticks) */
    struct list_head list;     /* LRU 链表节点 */
};

/* LRU 页面标志 */
#define LRU_PAGE_ACTIVE     (1 << 0)  /* 在活跃链表中 */
#define LRU_PAGE_REFERENCED (1 << 1)  /* 被访问过 */
#define LRU_PAGE_DIRTY      (1 << 2)  /* 脏页 */
#define LRU_PAGE_UNEVICTABLE (1 << 3) /* 不可换出 */

/* ========== LRU 链表结构 ========== */

struct lru_list {
    struct list_head active;    /* 活跃链表 */
    struct list_head inactive;  /* 不活跃链表 */
    uint32_t active_count;     /* 活跃页数 */
    uint32_t inactive_count;   /* 不活跃页数 */
    volatile int lock;         /* 自旋锁 */
};

/* ========== kswapd 守护线程控制 ========== */

struct kswapd_control {
    int running;                /* 是否运行中 */
    int should_wake;            /* 是否需要唤醒 */
    uint64_t last_scan;         /* 上次扫描时间 */
    uint64_t pages_scanned;     /* 已扫描页数 */
    uint64_t pages_reclaimed;   /* 已回收页数 */
    uint64_t pages_swapped_out; /* 已换出页数 */
    uint64_t wake_count;        /* 唤醒次数 */
    volatile int lock;          /* 自旋锁 */
};

/* ========== 内存压力通知回调 ========== */

#define SWAP_MAX_CALLBACKS  8

typedef void (*pressure_callback_t)(int level, uint64_t free_pages);

struct pressure_notifier {
    pressure_callback_t callback;   /* 回调函数 */
    int min_level;                  /* 最低触发级别 */
    int active;                     /* 是否激活 */
};

/* ========== 交换分区统计 ========== */

struct swap_stats {
    uint64_t swap_outs;             /* 换出次数 */
    uint64_t swap_ins;              /* 换入次数 */
    uint64_t pages_reclaimed;       /* 回收页数 */
    uint64_t pages_scanned;         /* 扫描页数 */
    uint64_t kswapd_wakeups;        /* kswapd 唤醒次数 */
    uint64_t pressure_notifications;/* 压力通知次数 */
};

/* ========== 函数原型 ========== */

/* 初始化交换模块 */
int swap_init(void);

/* 注册交换设备 */
int swap_register(uint64_t device_id, uint64_t total_pages, uint64_t priority);

/* 换出页面 */
int swap_out_page(uint64_t phys_addr, uint64_t *swap_offset);

/* 换入页面 */
int swap_in_page(uint64_t swap_offset, uint64_t phys_addr);

/* LRU 操作 */
void swap_lru_add_page(uint64_t phys_addr, uint64_t virt_addr, uint32_t flags);
void swap_lru_del_page(uint64_t phys_addr);
void swap_lru_mark_accessed(uint64_t phys_addr);
uint32_t swap_lru_reclaim(uint32_t target_pages);

/* kswapd 守护线程 */
void swap_kswapd_init(void);
void swap_kswapd_wake(void);
void swap_kswapd_run(void);

/* 内存压力通知 */
int swap_register_pressure_callback(pressure_callback_t callback, int min_level);
void swap_unregister_pressure_callback(pressure_callback_t callback);
void swap_notify_pressure(int level, uint64_t free_pages);

/* 获取统计 */
void swap_get_stats(struct swap_stats *stats);
int swap_get_pressure_level(void);

/* 获取交换设备信息 (供 VMM 使用) */
int swap_get_device_count(void);
struct swap_device *swap_get_device(int index);
uint64_t swap_get_total_free_pages(void);

#endif /* SWAP_H */
