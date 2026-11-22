/*
 * Zratro OS - Copy-On-Write (COW) Memory Management Implementation
 * 
 * 写时复制内存管理
 */

#include "cow.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"

/* 全局 COW 管理器 */
struct cow_manager cow_mgr;

/* 页引用缓存 */
#define REF_CACHE_SIZE      4096
static struct page_ref ref_cache_pool[REF_CACHE_SIZE];

/* ========== 初始化 ========== */

int cow_init(void) {
    // println("[COW] Initializing Copy-On-Write subsystem...");
    
    /* 初始化哈希表 */
    for (int i = 0; i < COW_HASH_SIZE; i++) {
        list_init(&cow_mgr.page_hash[i]);
        cow_mgr.hash_lock[i] = SPINLOCK_INIT;
    }
    
    /* 初始化引用缓存 */
    cow_mgr.ref_cache = ref_cache_pool;
    cow_mgr.ref_cache_size = REF_CACHE_SIZE;
    cow_mgr.ref_cache_used = 0;
    
    /* 初始化统计 */
    cow_mgr.total_shared_pages = 0;
    cow_mgr.total_private_pages = 0;
    cow_mgr.cow_faults = 0;
    cow_mgr.cow_breaks = 0;
    
    // println("[COW] COW subsystem initialized");
    return 0;
}

/* ========== 页引用管理 ========== */

/* 分配页引用结构 */
static struct page_ref *alloc_page_ref(uint64_t phys_addr) {
    struct page_ref *ref = NULL;
    
    /* 从缓存分配 */
    if (cow_mgr.ref_cache_used < cow_mgr.ref_cache_size) {
        ref = &cow_mgr.ref_cache[cow_mgr.ref_cache_used++];
    } else {
        /* 动态分配 */
        ref = kmalloc(sizeof(*ref));
    }
    
    if (ref == NULL) return NULL;
    
    ref->phys_addr = phys_addr;
    ref->ref_count = 1;
    ref->lock = SPINLOCK_INIT;
    ref->flags = 0;
    list_init(&ref->list);
    
    return ref;
}

/* 获取页引用 */
struct page_ref *cow_get_page_ref(uint64_t phys_addr) {
    uint32_t hash = cow_hash_addr(phys_addr);
    
    spin_lock(&cow_mgr.hash_lock[hash]);
    
    /* 查找哈希表 */
    struct page_ref *ref;
    list_for_each_entry(ref, &cow_mgr.page_hash[hash], list) {
        if (ref->phys_addr == phys_addr) {
            spin_unlock(&cow_mgr.hash_lock[hash]);
            return ref;
        }
    }
    
    spin_unlock(&cow_mgr.hash_lock[hash]);
    return NULL;
}

/* 增加页引用计数 */
int cow_inc_page_ref(uint64_t phys_addr) {
    struct page_ref *ref = cow_get_page_ref(phys_addr);
    
    if (ref == NULL) {
        /* 创建新的引用 */
        ref = alloc_page_ref(phys_addr);
        if (ref == NULL) return -1;
        
        uint32_t hash = cow_hash_addr(phys_addr);
        spin_lock(&cow_mgr.hash_lock[hash]);
        list_add(&ref->list, &cow_mgr.page_hash[hash]);
        spin_unlock(&cow_mgr.hash_lock[hash]);
        
        cow_mgr.total_shared_pages++;
    } else {
        spin_lock(&ref->lock);
        ref->ref_count++;
        spin_unlock(&ref->lock);
    }
    
    return 0;
}

/* 减少页引用计数 */
int cow_dec_page_ref(uint64_t phys_addr) {
    uint32_t hash = cow_hash_addr(phys_addr);

    /* Always acquire hash_lock before ref->lock to prevent ABBA deadlock
     * with cow_inc_page_ref which also acquires hash_lock first. */
    spin_lock(&cow_mgr.hash_lock[hash]);

    struct page_ref *ref = NULL;
    list_for_each_entry(ref, &cow_mgr.page_hash[hash], list) {
        if (ref->phys_addr == phys_addr) {
            break;
        }
    }
    if (ref == NULL || ref->phys_addr != phys_addr) {
        spin_unlock(&cow_mgr.hash_lock[hash]);
        return -1;
    }

    spin_lock(&ref->lock);
    int count = --ref->ref_count;

    if (count <= 0) {
        /* 引用计数归零，从哈希表移除 (hash_lock already held) */
        list_del(&ref->list);

        /* 记录需要释放的信息 */
        uint64_t phys_to_free = ref->phys_addr;
        int from_cache = (ref >= cow_mgr.ref_cache &&
                          ref < cow_mgr.ref_cache + cow_mgr.ref_cache_size);

        /* 标记为无效，防止其他线程访问 */
        ref->phys_addr = 0;
        ref->ref_count = 0;

        /* 释放锁 */
        spin_unlock(&ref->lock);
        spin_unlock(&cow_mgr.hash_lock[hash]);

        /* 安全地释放物理页 (此时 ref 已标记为无效) */
        pmm_free_page(phys_to_free);

        /* 如果不是来自缓存，释放内存 */
        if (!from_cache) {
            kfree(ref);
        }

        if (cow_mgr.total_shared_pages > 0) {
            cow_mgr.total_shared_pages--;
        }
    } else {
        /* 引用计数未归零，正常释放锁 */
        spin_unlock(&ref->lock);
        spin_unlock(&cow_mgr.hash_lock[hash]);
    }

    return 0;
}

/* ========== COW 区域管理 ========== */

/* 创建 COW 区域 */
struct cow_region *cow_create_region(struct address_space *space,
                                      uint64_t start, uint64_t size) {
    struct cow_region *region = kmalloc(sizeof(*region));
    if (region == NULL) return NULL;
    
    region->start = cow_page_align_down(start);
    region->end = cow_page_align(start + size);
    region->size = region->end - region->start;
    region->page_count = region->size / 4096;
    region->space = space;
    
    /* 分配页引用数组 */
    region->pages = kmalloc(sizeof(struct page_ref *) * region->page_count);
    if (region->pages == NULL) {
        kfree(region);
        return NULL;
    }
    
    region->orig_ptes = kmalloc(sizeof(uint64_t) * region->page_count);
    if (region->orig_ptes == NULL) {
        kfree(region->pages);
        kfree(region);
        return NULL;
    }
    
    memset(region->pages, 0, sizeof(struct page_ref *) * region->page_count);
    memset(region->orig_ptes, 0, sizeof(uint64_t) * region->page_count);
    
    list_init(&region->list);
    
    return region;
}

/* 销毁 COW 区域 */
void cow_destroy_region(struct cow_region *region) {
    if (region == NULL) return;
    
    /* 减少所有页的引用计数 */
    for (size_t i = 0; i < region->page_count; i++) {
        if (region->pages[i] != NULL) {
            cow_dec_page_ref(region->pages[i]->phys_addr);
        }
    }
    
    kfree(region->pages);
    kfree(region->orig_ptes);
    kfree(region);
}

/* ========== 地址空间复制 ========== */

/* 复制地址空间 (用于 fork) */
int cow_copy_address_space(struct address_space *dst, struct address_space *src) {
    if (dst == NULL || src == NULL) return -1;
    
    /* 复制页表结构 */
    /* 这需要遍历源地址空间的页表，并将所有可写页标记为只读和 COW */
    
    /* 遍历 PML4 */
    for (int pml4_idx = 0; pml4_idx < 512; pml4_idx++) {
        uint64_t *src_pml4 = src->pml4;
        if (!(src_pml4[pml4_idx] & VMM_FLAG_PRESENT)) continue;
        
        /* 分配目标 PML4 项 */
        uint64_t *dst_pml4 = dst->pml4;
        
        /* 获取 PDPT */
        uint64_t src_pdpt_phys = src_pml4[pml4_idx] & ~0xFFFULL;
        uint64_t *src_pdpt = (uint64_t *)phys_to_virt(src_pdpt_phys);
        
        /* 为目标分配 PDPT */
        uint64_t dst_pdpt_phys = pmm_alloc_page();
        if (dst_pdpt_phys == 0) return -1;
        uint64_t *dst_pdpt = (uint64_t *)phys_to_virt(dst_pdpt_phys);
        memset(dst_pdpt, 0, 4096);
        
        dst_pml4[pml4_idx] = dst_pdpt_phys | (src_pml4[pml4_idx] & 0xFFFULL);
        
        /* 遍历 PDPT */
        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(src_pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) continue;
            
            /* 获取 PD */
            uint64_t src_pd_phys = src_pdpt[pdpt_idx] & ~0xFFFULL;
            uint64_t *src_pd = (uint64_t *)phys_to_virt(src_pd_phys);
            
            /* 为目标分配 PD */
            uint64_t dst_pd_phys = pmm_alloc_page();
            if (dst_pd_phys == 0) return -1;
            uint64_t *dst_pd = (uint64_t *)phys_to_virt(dst_pd_phys);
            memset(dst_pd, 0, 4096);
            
            dst_pdpt[pdpt_idx] = dst_pd_phys | (src_pdpt[pdpt_idx] & 0xFFFULL);
            
            /* 遍历 PD */
            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(src_pd[pd_idx] & VMM_FLAG_PRESENT)) continue;
                
                /* 获取 PT */
                uint64_t src_pt_phys = src_pd[pd_idx] & ~0xFFFULL;
                uint64_t *src_pt = (uint64_t *)phys_to_virt(src_pt_phys);
                
                /* 为目标分配 PT */
                uint64_t dst_pt_phys = pmm_alloc_page();
                if (dst_pt_phys == 0) return -1;
                uint64_t *dst_pt = (uint64_t *)phys_to_virt(dst_pt_phys);
                
                dst_pd[pd_idx] = dst_pt_phys | (src_pd[pd_idx] & 0xFFFULL);
                
                /* 遍历 PT */
                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    uint64_t src_pte = src_pt[pt_idx];
                    
                    if (!(src_pte & VMM_FLAG_PRESENT)) {
                        dst_pt[pt_idx] = 0;
                        continue;
                    }
                    
                    /* 检查是否为可写页 */
                    if (src_pte & VMM_FLAG_WRITABLE) {
                        /* 标记为 COW */
                        uint64_t phys = src_pte & ~0xFFFULL;
                        
                        /* 增加引用计数 */
                        cow_inc_page_ref(phys);
                        
                        /* 设置为只读，添加 COW 标记 */
                        src_pte &= ~VMM_FLAG_WRITABLE;
                        src_pte |= VMM_FLAG_COW;  /* 假设有 COW 标志位 */
                        
                        /* 更新源页表 */
                        src_pt[pt_idx] = src_pte;
                        
                        /* 复制到目标 */
                        dst_pt[pt_idx] = src_pte;
                    } else {
                        /* 只读页直接复制 */
                        dst_pt[pt_idx] = src_pte;
                    }
                }
            }
        }
    }
    
    /* 刷新 TLB */
    vmm_invalidate_all();
    
    return 0;
}

/* ========== COW 缺页处理 ========== */

/* 处理 COW 缺页 */
int cow_handle_page_fault(uint64_t fault_addr, uint64_t error_code) {
    /* 检查是否为写缺页 - error_code 的位 1 表示写操作 */
    if (!(error_code & (1 << 1))) {  /* 检查是否为写操作 */
        return -1;  /* 不是写缺页，不处理 */
    }
    
    struct address_space *space = vmm_get_current_space();
    if (space == NULL) return -1;
    
    /* 对齐到页 */
    uint64_t page_addr = cow_page_align_down(fault_addr);
    
    /* 获取页表项 */
    uint64_t *pte = vmm_get_pte(space, page_addr);
    if (pte == NULL) return -1;
    
    uint64_t pte_val = *pte;
    
    /* 检查是否为 COW 页 (存在、不可写) */
    if (!(pte_val & VMM_FLAG_PRESENT)) return -1;
    if (pte_val & VMM_FLAG_WRITABLE) return -1;  /* 已经可写 */
    
    uint64_t phys = pte_val & ~0xFFFULL;
    
    /* 获取页引用 */
    struct page_ref *ref = cow_get_page_ref(phys);
    
    if (ref == NULL || ref->ref_count <= 1) {
        /* 只有一个引用或没有引用，直接设置为可写 */
        *pte |= VMM_FLAG_WRITABLE;
        vmm_invalidate_page(page_addr);
        return 0;
    }
    
    /* 需要复制页面 */
    cow_mgr.cow_faults++;
    
    /* 分配新物理页 */
    uint64_t new_phys = pmm_alloc_page();
    if (new_phys == 0) return -1;
    
    /* 清零新页 */
    void *dst = phys_to_virt(new_phys);
    if (dst != NULL) {
        memset(dst, 0, 4096);
    }
    
    /* 复制数据 */
    void *src = phys_to_virt(phys);
    if (src != NULL) {
        memcpy(dst, src, 4096);
    }
    
    /* 更新页表 */
    *pte = new_phys | (pte_val & 0xFFFULL) | VMM_FLAG_WRITABLE;
    
    /* 减少原页引用 */
    cow_dec_page_ref(phys);
    
    /* 刷新 TLB */
    vmm_invalidate_page(page_addr);
    
    cow_mgr.cow_breaks++;
    cow_mgr.total_private_pages++;
    
    return 0;
}

/* 标记页面为 COW */
int cow_mark_page_cow(struct address_space *space, uint64_t virt_addr) {
    uint64_t *pte = vmm_get_pte(space, virt_addr);
    if (pte == NULL) return -1;
    
    uint64_t pte_val = *pte;
    if (!(pte_val & VMM_FLAG_PRESENT)) return -1;
    
    uint64_t phys = pte_val & ~0xFFFULL;
    
    /* 增加引用计数 */
    cow_inc_page_ref(phys);
    
    /* 设置为只读 */
    pte_val &= ~VMM_FLAG_WRITABLE;
    *pte = pte_val;
    
    vmm_invalidate_page(virt_addr);
    
    return 0;
}

/* 检查是否为 COW 页 */
int cow_is_cow_page(struct address_space *space, uint64_t virt_addr) {
    uint64_t *pte = vmm_get_pte(space, virt_addr);
    if (pte == NULL) return 0;
    
    uint64_t pte_val = *pte;
    
    /* 存在但不可写，且有引用 */
    if ((pte_val & VMM_FLAG_PRESENT) && !(pte_val & VMM_FLAG_WRITABLE)) {
        uint64_t phys = pte_val & ~0xFFFULL;
        struct page_ref *ref = cow_get_page_ref(phys);
        return (ref != NULL && ref->ref_count > 0);
    }
    
    return 0;
}

/* 断开 COW (复制页面) */
int cow_break_page(uint64_t virt_addr, struct page_ref *ref) {
    if (ref == NULL) return -1;
    
    struct address_space *space = vmm_get_current_space();
    if (space == NULL) return -1;
    
    uint64_t *pte = vmm_get_pte(space, virt_addr);
    if (pte == NULL) return -1;
    
    uint64_t old_phys = ref->phys_addr;
    
    /* 分配新页 */
    uint64_t new_phys = pmm_alloc_page();
    if (new_phys == 0) return -1;
    
    /* 复制数据 */
    void *src = phys_to_virt(old_phys);
    void *dst = phys_to_virt(new_phys);
    memcpy(dst, src, 4096);
    
    /* 更新页表 */
    uint64_t pte_val = *pte;
    pte_val = (pte_val & ~0xFFFFFFFFF000ULL) | new_phys;
    pte_val |= VMM_FLAG_WRITABLE;
    *pte = pte_val;
    
    /* 减少旧引用 */
    cow_dec_page_ref(old_phys);
    
    vmm_invalidate_page(virt_addr);
    
    return 0;
}

/* 打印 COW 统计 */
void cow_print_stats(void) {
    // println("[COW] Statistics:");
    // print("  Shared pages: ");
    // print_dec(cow_mgr.total_shared_pages);
    // println("");
    // print("  Private pages: ");
    // print_dec(cow_mgr.total_private_pages);
    // println("");
    // print("  COW faults: ");
    // print_dec(cow_mgr.cow_faults);
    // println("");
    // print("  COW breaks: ");
    // print_dec(cow_mgr.cow_breaks);
    // println("");
}
