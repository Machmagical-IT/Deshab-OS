/*
 * Zratro OS - 交换分区与页面回收实现
 *
 * 实现交换分区管理、LRU 页面回收、kswapd 守护线程和内存压力通知
 */

#include "swap.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "task.h"

/* ========== 全局状态 ========== */

/* 交换设备数组 */
static struct swap_device swap_devices[SWAP_MAX_DEVICES];
static int swap_device_count = 0;
static volatile int swap_device_lock = 0;

/* LRU 链表 */
static struct lru_list swap_lru;

/* LRU 页面缓存 (避免频繁 kmalloc) */
#define LRU_PAGE_CACHE_SIZE  1024
static struct lru_page lru_page_cache[LRU_PAGE_CACHE_SIZE];
static int lru_page_cache_used = 0;
static volatile int lru_page_cache_lock = 0;

/* kswapd 控制 */
static struct kswapd_control kswapd_ctrl;

/* 内存压力通知回调 */
static struct pressure_notifier pressure_callbacks[SWAP_MAX_CALLBACKS];
static volatile int pressure_lock = 0;

/* 统计 */
static struct swap_stats swap_statistics;

/* ========== 辅助函数 ========== */

/* 从缓存分配 LRU 页面结构 */
static struct lru_page *lru_page_alloc(void) {
    while (__sync_lock_test_and_set(&lru_page_cache_lock, 1)) {
        asm volatile("pause");
    }
    
    struct lru_page *page = NULL;
    
    /* 从缓存中查找空闲项 */
    for (int i = 0; i < LRU_PAGE_CACHE_SIZE; i++) {
        if (lru_page_cache[i].phys_addr == 0 && lru_page_cache[i].ref_count == 0) {
            page = &lru_page_cache[i];
            lru_page_cache_used++;
            break;
        }
    }
    
    __sync_lock_release(&lru_page_cache_lock);
    
    /* 缓存不足时从堆分配 */
    if (page == NULL) {
        page = (struct lru_page *)kmalloc(sizeof(struct lru_page));
    }
    
    if (page != NULL) {
        memset(page, 0, sizeof(struct lru_page));
        INIT_LIST_HEAD(&page->list);
    }
    
    return page;
}

/* 释放 LRU 页面结构 */
static void lru_page_free(struct lru_page *page) {
    if (page == NULL) return;
    
    /* 检查是否在缓存范围内 */
    if (page >= lru_page_cache && page < lru_page_cache + LRU_PAGE_CACHE_SIZE) {
        while (__sync_lock_test_and_set(&lru_page_cache_lock, 1)) {
            asm volatile("pause");
        }
        page->phys_addr = 0;
        page->ref_count = 0;
        page->flags = 0;
        if (lru_page_cache_used > 0) lru_page_cache_used--;
        __sync_lock_release(&lru_page_cache_lock);
    } else {
        kfree(page);
    }
}

/* 在 LRU 链表中查找页面 */
static struct lru_page *lru_find_page(uint64_t phys_addr) {
    struct list_head *pos;
    
    /* 先搜索活跃链表 */
    list_for_each(pos, &swap_lru.active) {
        struct lru_page *page = list_entry(pos, struct lru_page, list);
        if (page->phys_addr == phys_addr) return page;
    }
    
    /* 再搜索不活跃链表 */
    list_for_each(pos, &swap_lru.inactive) {
        struct lru_page *page = list_entry(pos, struct lru_page, list);
        if (page->phys_addr == phys_addr) return page;
    }
    
    return NULL;
}

/* ========== 初始化 ========== */

int swap_init(void) {
    /* 初始化交换设备数组 */
    for (int i = 0; i < SWAP_MAX_DEVICES; i++) {
        swap_devices[i].enabled = 0;
        swap_devices[i].swap_map = NULL;
    }
    swap_device_count = 0;
    
    /* 初始化 LRU 链表 */
    INIT_LIST_HEAD(&swap_lru.active);
    INIT_LIST_HEAD(&swap_lru.inactive);
    swap_lru.active_count = 0;
    swap_lru.inactive_count = 0;
    swap_lru.lock = 0;
    
    /* 初始化 LRU 页面缓存 */
    memset(lru_page_cache, 0, sizeof(lru_page_cache));
    lru_page_cache_used = 0;
    
    /* 初始化 kswapd 控制 */
    memset(&kswapd_ctrl, 0, sizeof(kswapd_ctrl));
    kswapd_ctrl.lock = 0;
    
    /* 初始化压力通知回调 */
    for (int i = 0; i < SWAP_MAX_CALLBACKS; i++) {
        pressure_callbacks[i].active = 0;
        pressure_callbacks[i].callback = NULL;
    }
    
    /* 初始化统计 */
    memset(&swap_statistics, 0, sizeof(swap_statistics));
    
    return 0;
}

/* ========== 交换设备管理 ========== */

int swap_register(uint64_t device_id, uint64_t total_pages, uint64_t priority) {
    while (__sync_lock_test_and_set(&swap_device_lock, 1)) {
        asm volatile("pause");
    }
    
    if (swap_device_count >= SWAP_MAX_DEVICES) {
        __sync_lock_release(&swap_device_lock);
        return -1;
    }
    
    struct swap_device *sd = &swap_devices[swap_device_count];
    sd->device_id = device_id;
    sd->total_pages = total_pages;
    sd->used_pages = 0;
    sd->priority = priority;
    sd->enabled = 1;
    
    /* 分配交换位图 */
    sd->swap_map = (uint8_t *)kmalloc(sizeof(uint8_t) * total_pages);
    if (sd->swap_map == NULL) {
        __sync_lock_release(&swap_device_lock);
        return -1;
    }
    memset(sd->swap_map, 0, sizeof(uint8_t) * total_pages);
    
    swap_device_count++;
    
    __sync_lock_release(&swap_device_lock);
    return 0;
}

int swap_out_page(uint64_t phys_addr, uint64_t *swap_offset) {
    while (__sync_lock_test_and_set(&swap_device_lock, 1)) {
        asm volatile("pause");
    }
    
    /* 找到优先级最高且有空闲空间的交换设备 */
    struct swap_device *best = NULL;
    uint64_t best_prio = 0;
    
    for (int i = 0; i < swap_device_count; i++) {
        if (swap_devices[i].enabled && 
            swap_devices[i].used_pages < swap_devices[i].total_pages &&
            swap_devices[i].priority > best_prio) {
            best = &swap_devices[i];
            best_prio = swap_devices[i].priority;
        }
    }
    
    if (best == NULL) {
        __sync_lock_release(&swap_device_lock);
        return -1;
    }
    
    /* 查找空闲槽位 */
    uint64_t offset = 0;
    for (uint64_t i = 0; i < best->total_pages; i++) {
        if (best->swap_map[i] == 0) {
            offset = i;
            break;
        }
    }
    
    /* 标记槽位为已使用 */
    best->swap_map[offset] = 1;
    best->used_pages++;
    
    /* 计算 swap_offset: 设备索引 << 48 | 页偏移 */
    int dev_idx = (int)(best - swap_devices);
    *swap_offset = ((uint64_t)dev_idx << 48) | offset;
    
    /* 复制页面数据到交换空间 (简化实现：记录映射关系) */
    /* 实际实现需要将页面写入块设备 */
    
    swap_statistics.swap_outs++;
    
    __sync_lock_release(&swap_device_lock);
    return 0;
}

int swap_in_page(uint64_t swap_offset, uint64_t phys_addr) {
    while (__sync_lock_test_and_set(&swap_device_lock, 1)) {
        asm volatile("pause");
    }
    
    /* 解析 swap_offset */
    int dev_idx = (int)(swap_offset >> 48);
    uint64_t page_offset = swap_offset & 0xFFFFFFFFFFFFULL;
    
    if (dev_idx < 0 || dev_idx >= swap_device_count) {
        __sync_lock_release(&swap_device_lock);
        return -1;
    }
    
    struct swap_device *sd = &swap_devices[dev_idx];
    
    if (!sd->enabled || page_offset >= sd->total_pages) {
        __sync_lock_release(&swap_device_lock);
        return -1;
    }
    
    if (sd->swap_map[page_offset] == 0) {
        /* 该槽位未被占用，无法换入 */
        __sync_lock_release(&swap_device_lock);
        return -1;
    }
    
    /* 从交换空间读取页面数据到物理页 (简化实现) */
    /* 实际实现需要从块设备读取数据 */
    
    /* 释放交换槽位 */
    sd->swap_map[page_offset] = 0;
    if (sd->used_pages > 0) sd->used_pages--;
    
    swap_statistics.swap_ins++;
    
    __sync_lock_release(&swap_device_lock);
    return 0;
}

/* ========== LRU 页面回收 ========== */

void swap_lru_add_page(uint64_t phys_addr, uint64_t virt_addr, uint32_t flags) {
    while (__sync_lock_test_and_set(&swap_lru.lock, 1)) {
        asm volatile("pause");
    }
    
    /* 检查是否已在链表中 */
    struct lru_page *existing = lru_find_page(phys_addr);
    if (existing != NULL) {
        /* 已存在，更新信息 */
        existing->last_access = get_tick_count();
        if (flags & LRU_PAGE_REFERENCED) {
            existing->flags |= LRU_PAGE_REFERENCED;
        }
        __sync_lock_release(&swap_lru.lock);
        return;
    }
    
    /* 分配新的 LRU 页面结构 */
    struct lru_page *page = lru_page_alloc();
    if (page == NULL) {
        __sync_lock_release(&swap_lru.lock);
        return;
    }
    
    page->phys_addr = phys_addr;
    page->virt_addr = virt_addr;
    page->flags = flags;
    page->ref_count = 1;
    page->last_access = get_tick_count();
    
    /* 新页面加入不活跃链表尾部 */
    list_add_tail(&page->list, &swap_lru.inactive);
    swap_lru.inactive_count++;
    
    __sync_lock_release(&swap_lru.lock);
}

void swap_lru_del_page(uint64_t phys_addr) {
    while (__sync_lock_test_and_set(&swap_lru.lock, 1)) {
        asm volatile("pause");
    }
    
    struct lru_page *page = lru_find_page(phys_addr);
    if (page == NULL) {
        __sync_lock_release(&swap_lru.lock);
        return;
    }
    
    list_del(&page->list);
    
    if (page->flags & LRU_PAGE_ACTIVE) {
        swap_lru.active_count--;
    } else {
        swap_lru.inactive_count--;
    }
    
    lru_page_free(page);
    
    __sync_lock_release(&swap_lru.lock);
}

void swap_lru_mark_accessed(uint64_t phys_addr) {
    while (__sync_lock_test_and_set(&swap_lru.lock, 1)) {
        asm volatile("pause");
    }
    
    struct lru_page *page = lru_find_page(phys_addr);
    if (page == NULL) {
        __sync_lock_release(&swap_lru.lock);
        return;
    }
    
    page->last_access = get_tick_count();
    page->flags |= LRU_PAGE_REFERENCED;
    
    /* 如果在不活跃链表中被再次访问，提升到活跃链表 */
    if (!(page->flags & LRU_PAGE_ACTIVE)) {
        list_del(&page->list);
        swap_lru.inactive_count--;
        
        page->flags |= LRU_PAGE_ACTIVE;
        list_add_tail(&page->list, &swap_lru.active);
        swap_lru.active_count++;
    } else {
        /* 已在活跃链表，移到尾部 (最近使用) */
        list_del(&page->list);
        list_add_tail(&page->list, &swap_lru.active);
    }
    
    __sync_lock_release(&swap_lru.lock);
}

/* 从不活跃链表尾部回收页面 */
uint32_t swap_lru_reclaim(uint32_t target_pages) {
    uint32_t reclaimed = 0;
    
    while (__sync_lock_test_and_set(&swap_lru.lock, 1)) {
        asm volatile("pause");
    }
    
    /* 优先从活跃链表降级：如果 active/inactive 比率过高 */
    if (swap_lru.active_count > swap_lru.inactive_count * LRU_ACTIVE_RATIO) {
        int to_demote = swap_lru.active_count / LRU_ACTIVE_RATIO;
        struct list_head *pos, *n;
        int demoted = 0;
        
        list_for_each_safe(pos, n, &swap_lru.active) {
            if (demoted >= to_demote) break;
            
            struct lru_page *page = list_entry(pos, struct lru_page, list);
            
            /* 从活跃链表移到不活跃链表 */
            list_del(&page->list);
            swap_lru.active_count--;
            
            page->flags &= ~LRU_PAGE_ACTIVE;
            page->flags &= ~LRU_PAGE_REFERENCED;
            list_add_tail(&page->list, &swap_lru.inactive);
            swap_lru.inactive_count++;
            demoted++;
        }
    }
    
    /* 从不活跃链表尾部选取页面回收 */
    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &swap_lru.inactive) {
        if (reclaimed >= target_pages) break;
        
        struct lru_page *page = list_entry(pos, struct lru_page, list);
        
        /* 跳过不可换出的页面 */
        if (page->flags & LRU_PAGE_UNEVICTABLE) continue;
        
        /* 如果页面被访问过，提升到活跃链表而非回收 */
        if (page->flags & LRU_PAGE_REFERENCED) {
            list_del(&page->list);
            swap_lru.inactive_count--;
            
            page->flags |= LRU_PAGE_ACTIVE;
            page->flags &= ~LRU_PAGE_REFERENCED;
            list_add_tail(&page->list, &swap_lru.active);
            swap_lru.active_count++;
            continue;
        }
        
        /* 尝试换出页面 */
        uint64_t swap_offset;
        if (swap_out_page(page->phys_addr, &swap_offset) == 0) {
            /* 换出成功，释放物理页 */
            pmm_free_page(page->phys_addr);
            
            /* 从 LRU 链表移除 */
            list_del(&page->list);
            swap_lru.inactive_count--;
            lru_page_free(page);
            
            reclaimed++;
            swap_statistics.pages_reclaimed++;
        }
    }
    
    swap_statistics.pages_scanned += swap_lru.inactive_count;
    
    __sync_lock_release(&swap_lru.lock);
    return reclaimed;
}

/* ========== kswapd 守护线程 ========== */

void swap_kswapd_init(void) {
    memset(&kswapd_ctrl, 0, sizeof(kswapd_ctrl));
    kswapd_ctrl.running = 0;
    kswapd_ctrl.should_wake = 0;
    kswapd_ctrl.lock = 0;
}

void swap_kswapd_wake(void) {
    while (__sync_lock_test_and_set(&kswapd_ctrl.lock, 1)) {
        asm volatile("pause");
    }
    
    kswapd_ctrl.should_wake = 1;
    kswapd_ctrl.wake_count++;
    swap_statistics.kswapd_wakeups++;
    
    __sync_lock_release(&kswapd_ctrl.lock);
}

void swap_kswapd_run(void) {
    while (__sync_lock_test_and_set(&kswapd_ctrl.lock, 1)) {
        asm volatile("pause");
    }
    
    kswapd_ctrl.running = 1;
    
    __sync_lock_release(&kswapd_ctrl.lock);
    
    /* kswapd 主循环 */
    while (1) {
        /* 检查是否需要唤醒 */
        int should_run = 0;
        
        while (__sync_lock_test_and_set(&kswapd_ctrl.lock, 1)) {
            asm volatile("pause");
        }
        
        uint64_t now = get_tick_count();
        
        /* 条件1: 被显式唤醒 */
        if (kswapd_ctrl.should_wake) {
            should_run = 1;
            kswapd_ctrl.should_wake = 0;
        }
        
        /* 条件2: 定期扫描 */
        if (now - kswapd_ctrl.last_scan >= KSWAPD_SCAN_INTERVAL) {
            should_run = 1;
        }
        
        /* 条件3: 内存低于低水位 */
        uint64_t free_pages = pmm_get_free_memory() / PAGE_SIZE;
        if (free_pages < KSWAPD_WAKEUP_WATERMARK) {
            should_run = 1;
        }
        
        __sync_lock_release(&kswapd_ctrl.lock);
        
        if (!should_run) {
            /* 短暂休眠 */
            for (volatile int i = 0; i < 10000; i++) {
                asm volatile("pause");
            }
            continue;
        }
        
        /* 执行页面回收 */
        uint64_t free_pages_now = pmm_get_free_memory() / PAGE_SIZE;
        uint32_t target = 0;
        
        if (free_pages_now < KSWAPD_WAKEUP_WATERMARK) {
            /* 内存紧张，积极回收 */
            target = KSWAPD_HIGH_WATERMARK - (uint32_t)free_pages_now;
            if (target > LRU_SCAN_BATCH) target = LRU_SCAN_BATCH;
        } else if (free_pages_now < KSWAPD_HIGH_WATERMARK) {
            /* 内存偏低，温和回收 */
            target = (KSWAPD_HIGH_WATERMARK - (uint32_t)free_pages_now) / 2;
        }
        
        if (target > 0) {
            uint32_t reclaimed = swap_lru_reclaim(target);
            
            while (__sync_lock_test_and_set(&kswapd_ctrl.lock, 1)) {
                asm volatile("pause");
            }
            kswapd_ctrl.pages_reclaimed += reclaimed;
            kswapd_ctrl.pages_scanned += target;
            kswapd_ctrl.last_scan = now;
            __sync_lock_release(&kswapd_ctrl.lock);
            
            /* 检查内存压力并通知 */
            free_pages_now = pmm_get_free_memory() / PAGE_SIZE;
            int level = swap_get_pressure_level();
            swap_notify_pressure(level, free_pages_now);
        }
        
        /* 如果内存充足，降低扫描频率 */
        if (free_pages_now >= KSWAPD_HIGH_WATERMARK) {
            for (volatile int i = 0; i < 100000; i++) {
                asm volatile("pause");
            }
        }
    }
}

/* ========== 内存压力通知 ========== */

int swap_register_pressure_callback(pressure_callback_t callback, int min_level) {
    if (callback == NULL) return -1;
    
    while (__sync_lock_test_and_set(&pressure_lock, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < SWAP_MAX_CALLBACKS; i++) {
        if (!pressure_callbacks[i].active) {
            pressure_callbacks[i].callback = callback;
            pressure_callbacks[i].min_level = min_level;
            pressure_callbacks[i].active = 1;
            __sync_lock_release(&pressure_lock);
            return 0;
        }
    }
    
    __sync_lock_release(&pressure_lock);
    return -1;  /* 没有空闲槽位 */
}

void swap_unregister_pressure_callback(pressure_callback_t callback) {
    if (callback == NULL) return;
    
    while (__sync_lock_test_and_set(&pressure_lock, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < SWAP_MAX_CALLBACKS; i++) {
        if (pressure_callbacks[i].active && pressure_callbacks[i].callback == callback) {
            pressure_callbacks[i].active = 0;
            pressure_callbacks[i].callback = NULL;
            break;
        }
    }
    
    __sync_lock_release(&pressure_lock);
}

void swap_notify_pressure(int level, uint64_t free_pages) {
    while (__sync_lock_test_and_set(&pressure_lock, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < SWAP_MAX_CALLBACKS; i++) {
        if (pressure_callbacks[i].active && 
            pressure_callbacks[i].callback != NULL &&
            level >= pressure_callbacks[i].min_level) {
            /* 调用回调函数 */
            pressure_callbacks[i].callback(level, free_pages);
        }
    }
    
    swap_statistics.pressure_notifications++;
    
    __sync_lock_release(&pressure_lock);
}

/* ========== 统计与查询 ========== */

int swap_get_pressure_level(void) {
    uint64_t total = pmm_get_total_memory();
    uint64_t free = pmm_get_free_memory();
    
    if (total == 0) return SWAP_PRESSURE_CRITICAL;
    
    uint64_t usage_pct = ((total - free) * 100) / total;
    
    if (usage_pct >= 97) return SWAP_PRESSURE_CRITICAL;
    if (usage_pct >= 90) return SWAP_PRESSURE_HIGH;
    if (usage_pct >= 80) return SWAP_PRESSURE_MEDIUM;
    if (usage_pct >= 65) return SWAP_PRESSURE_LOW;
    return SWAP_PRESSURE_NONE;
}

void swap_get_stats(struct swap_stats *stats) {
    if (stats != NULL) {
        *stats = swap_statistics;
    }
}

/* 获取交换设备数量 */
int swap_get_device_count(void) {
    return swap_device_count;
}

/* 获取交换设备指针 */
struct swap_device *swap_get_device(int index) {
    if (index < 0 || index >= swap_device_count) return NULL;
    return &swap_devices[index];
}

/* 获取所有交换设备的空闲页总数 */
uint64_t swap_get_total_free_pages(void) {
    uint64_t free_pages = 0;
    for (int i = 0; i < swap_device_count; i++) {
        if (swap_devices[i].enabled) {
            free_pages += swap_devices[i].total_pages - swap_devices[i].used_pages;
        }
    }
    return free_pages;
}
