/*
 * Zratro OS - EPT (Extended Page Tables) Management
 * 
 * 实现扩展页表用于内存虚拟化
 * 将客户机物理地址 (GPA) 映射到主机物理地址 (HPA)
 */

#include "ept.h"
#include "vmm.h"
#include "pmm.h"
#include "console.h"
#include "string.h"

/* ========== EPT 全局变量 ========== */

static uint64_t *ept_root = NULL;        /* EPT PML4 根页面 */
static uint64_t ept_root_phys = 0;       /* EPT PML4 物理地址 */
static int ept_initialized = 0;

/* ========== 工具函数 ========== */

static inline uint64_t ept_create_entry(uint64_t phys, uint64_t flags) {
    return (phys & 0x000FFFFFFFFFF000ULL) | flags;
}

static inline uint64_t ept_get_addr(uint64_t entry) {
    return entry & 0x000FFFFFFFFFF000ULL;
}

/* ========== EPT 初始化 ========== */

int ept_init(void) {
    if (ept_initialized) {
        return 0;
    }
    
    print("[EPT] Initializing EPT...\n");
    
    /* 分配 PML4 页面 */
    ept_root = (uint64_t *)pmm_alloc_page();
    if (!ept_root) {
        print("[EPT] Failed to allocate EPT PML4\n");
        return -1;
    }
    
    ept_root_phys = (uint64_t)ept_root;  /* 假设直接映射 */
    
    /* 清零 PML4 */
    memset(ept_root, 0, 4096);
    
    ept_initialized = 1;
    print("[EPT] EPT initialized at 0x");
    char buf[17];
    uint64_t addr = ept_root_phys;
    for (int i = 15; i >= 0; i--) {
        buf[i] = "0123456789ABCDEF"[addr & 0xF];
        addr >>= 4;
    }
    buf[16] = '\0';
    println(buf);
    
    return 0;
}

/* ========== EPT 页表遍历 ========== */

static uint64_t *ept_get_pdpt(uint64_t gpa, int alloc) {
    uint32_t pml4_idx = (gpa >> 39) & 0x1FF;
    uint64_t pml4_entry = ept_root[pml4_idx];
    
    if (!(pml4_entry & EPT_READ_ACCESS)) {
        if (!alloc) return NULL;
        
        /* 分配 PDPT */
        uint64_t *pdpt = (uint64_t *)pmm_alloc_page();
        if (!pdpt) return NULL;
        
        memset(pdpt, 0, 4096);
        ept_root[pml4_idx] = ept_create_entry((uint64_t)pdpt, 
                                               EPT_READ_ACCESS | 
                                               EPT_WRITE_ACCESS | 
                                               EPT_EXECUTE_ACCESS);
        return pdpt;
    }
    
    return (uint64_t *)ept_get_addr(pml4_entry);
}

static uint64_t *ept_get_pd(uint64_t gpa, int alloc) {
    uint32_t pdpt_idx = (gpa >> 30) & 0x1FF;
    uint64_t *pdpt = ept_get_pdpt(gpa, alloc);
    
    if (!pdpt) return NULL;
    
    uint64_t pdpt_entry = pdpt[pdpt_idx];
    
    if (!(pdpt_entry & EPT_READ_ACCESS)) {
        if (!alloc) return NULL;
        
        /* 分配 PD */
        uint64_t *pd = (uint64_t *)pmm_alloc_page();
        if (!pd) return NULL;
        
        memset(pd, 0, 4096);
        pdpt[pdpt_idx] = ept_create_entry((uint64_t)pd,
                                           EPT_READ_ACCESS |
                                           EPT_WRITE_ACCESS |
                                           EPT_EXECUTE_ACCESS);
        return pd;
    }
    
    return (uint64_t *)ept_get_addr(pdpt_entry);
}

static uint64_t *ept_get_pt(uint64_t gpa, int alloc) {
    uint32_t pd_idx = (gpa >> 21) & 0x1FF;
    uint64_t *pd = ept_get_pd(gpa, alloc);
    
    if (!pd) return NULL;
    
    uint64_t pd_entry = pd[pd_idx];
    
    /* 检查是否是 2MB 大页 */
    if (pd_entry & (1 << 7)) {
        return NULL;  /* 大页，不返回 PT */
    }
    
    if (!(pd_entry & EPT_READ_ACCESS)) {
        if (!alloc) return NULL;
        
        /* 分配 PT */
        uint64_t *pt = (uint64_t *)pmm_alloc_page();
        if (!pt) return NULL;
        
        memset(pt, 0, 4096);
        pd[pd_idx] = ept_create_entry((uint64_t)pt,
                                       EPT_READ_ACCESS |
                                       EPT_WRITE_ACCESS |
                                       EPT_EXECUTE_ACCESS);
        return pt;
    }
    
    return (uint64_t *)ept_get_addr(pd_entry);
}

/* ========== EPT 映射函数 ========== */

int ept_map_page(uint64_t gpa, uint64_t hpa, uint64_t flags) {
    if (!ept_initialized) {
        return -1;
    }
    
    uint64_t *pt = ept_get_pt(gpa, 1);
    if (!pt) {
        print("[EPT] Failed to get PT for GPA 0x");
        char buf[17];
        for (int i = 15; i >= 0; i--) {
            buf[i] = "0123456789ABCDEF"[gpa & 0xF];
            gpa >>= 4;
        }
        buf[16] = '\0';
        println(buf);
        return -1;
    }
    
    uint32_t pt_idx = (gpa >> 12) & 0x1FF;
    pt[pt_idx] = ept_create_entry(hpa, flags);
    
    /* 刷新 EPT */
    asm volatile("invlpg (%0)" : : "r"(gpa) : "memory");
    
    return 0;
}

int ept_map_range(uint64_t gpa, uint64_t hpa, size_t pages, uint64_t flags) {
    for (size_t i = 0; i < pages; i++) {
        if (ept_map_page(gpa + i * 4096, hpa + i * 4096, flags) != 0) {
            return -1;
        }
    }
    return 0;
}

int ept_unmap_page(uint64_t gpa) {
    if (!ept_initialized) {
        return -1;
    }
    
    uint64_t *pt = ept_get_pt(gpa, 0);
    if (!pt) {
        return -1;
    }
    
    uint32_t pt_idx = (gpa >> 12) & 0x1FF;
    pt[pt_idx] = 0;
    
    asm volatile("invlpg (%0)" : : "r"(gpa) : "memory");
    
    return 0;
}

/* ========== EPT 大页支持 ========== */

int ept_map_2mb_page(uint64_t gpa, uint64_t hpa, uint64_t flags) {
    if (!ept_initialized) {
        return -1;
    }
    
    /* 确保 2MB 对齐 */
    gpa &= ~0x1FFFFFULL;
    hpa &= ~0x1FFFFFULL;
    
    uint64_t *pd = ept_get_pd(gpa, 1);
    if (!pd) {
        return -1;
    }
    
    uint32_t pd_idx = (gpa >> 21) & 0x1FF;
    
    /* 创建 2MB 大页条目 */
    pd[pd_idx] = ept_create_entry(hpa, flags | (1 << 7));  /* Bit 7 = 大页标志 */
    
    return 0;
}

int ept_map_1gb_page(uint64_t gpa, uint64_t hpa, uint64_t flags) {
    if (!ept_initialized) {
        return -1;
    }
    
    /* 确保 1GB 对齐 */
    gpa &= ~0x3FFFFFFFULL;
    hpa &= ~0x3FFFFFFFULL;
    
    uint64_t *pdpt = ept_get_pdpt(gpa, 1);
    if (!pdpt) {
        return -1;
    }
    
    uint32_t pdpt_idx = (gpa >> 30) & 0x1FF;
    
    /* 创建 1GB 大页条目 */
    pdpt[pdpt_idx] = ept_create_entry(hpa, flags | (1 << 7));  /* Bit 7 = 大页标志 */
    
    return 0;
}

/* ========== EPT 权限管理 ========== */

int ept_set_page_access(uint64_t gpa, uint64_t access) {
    if (!ept_initialized) {
        return -1;
    }
    
    uint64_t *pt = ept_get_pt(gpa, 0);
    if (!pt) {
        return -1;
    }
    
    uint32_t pt_idx = (gpa >> 12) & 0x1FF;
    uint64_t entry = pt[pt_idx];
    
    /* 清除旧权限，设置新权限 */
    entry &= ~(EPT_READ_ACCESS | EPT_WRITE_ACCESS | EPT_EXECUTE_ACCESS);
    entry |= (access & (EPT_READ_ACCESS | EPT_WRITE_ACCESS | EPT_EXECUTE_ACCESS));
    
    pt[pt_idx] = entry;
    
    return 0;
}

int ept_get_page_access(uint64_t gpa) {
    if (!ept_initialized) {
        return 0;
    }
    
    uint64_t *pt = ept_get_pt(gpa, 0);
    if (!pt) {
        return 0;
    }
    
    uint32_t pt_idx = (gpa >> 12) & 0x1FF;
    uint64_t entry = pt[pt_idx];
    
    return entry & (EPT_READ_ACCESS | EPT_WRITE_ACCESS | EPT_EXECUTE_ACCESS);
}

/* ========== EPT 信息获取 ========== */

uint64_t ept_get_root_phys(void) {
    return ept_root_phys;
}

uint64_t ept_construct_eptp(void) {
    /* 构造 EPTP (EPT Pointer) */
    /* Bits 2:0  = Memory Type (6 = WB) */
    /* Bits 5:3  = Page Walk Length (3 = 4 levels) */
    /* Bit 6     = Enable Accessed/Dirty flag (0 = disabled) */
    /* Bits 11:7 = Reserved */
    /* Bits 51:12 = EPT PML4 Address */
    
    uint64_t eptp = EPT_MEMORY_TYPE_WB;          /* Write-Back */
    eptp |= (3ULL << 3);                         /* 4-level page walk */
    eptp |= (ept_root_phys & 0x000FFFFFFFFFF000ULL);
    
    return eptp;
}

/* ========== EPT 统计 ========== */

void ept_get_stats(struct ept_stats *stats) {
    if (!stats || !ept_initialized) return;
    
    memset(stats, 0, sizeof(*stats));
    
    /* 遍历 PML4 统计 */
    for (int i = 0; i < 512; i++) {
        if (ept_root[i] & EPT_READ_ACCESS) {
            stats->pdpt_count++;
            
            uint64_t *pdpt = (uint64_t *)ept_get_addr(ept_root[i]);
            for (int j = 0; j < 512; j++) {
                if (pdpt[j] & EPT_READ_ACCESS) {
                    stats->pd_count++;
                    
                    uint64_t *pd = (uint64_t *)ept_get_addr(pdpt[j]);
                    for (int k = 0; k < 512; k++) {
                        if (pd[k] & EPT_READ_ACCESS) {
                            if (pd[k] & (1 << 7)) {
                                /* 2MB 大页 */
                                stats->large_2mb_pages++;
                            } else {
                                stats->pt_count++;
                                
                                uint64_t *pt = (uint64_t *)ept_get_addr(pd[k]);
                                for (int l = 0; l < 512; l++) {
                                    if (pt[l] & EPT_READ_ACCESS) {
                                        stats->small_4kb_pages++;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    stats->total_mapped_bytes = 
        (stats->small_4kb_pages * 4096) +
        (stats->large_2mb_pages * 2 * 1024 * 1024);
}

void ept_print_stats(void) {
    struct ept_stats stats;
    ept_get_stats(&stats);
    
    print("[EPT] Statistics:\n");
    print("  PDPTs: ");
    print_hex(stats.pdpt_count);
    println("");
    print("  PDs: ");
    print_hex(stats.pd_count);
    println("");
    print("  PTs: ");
    print_hex(stats.pt_count);
    println("");
    print("  4KB pages: ");
    print_hex(stats.small_4kb_pages);
    println("");
    print("  2MB pages: ");
    print_hex(stats.large_2mb_pages);
    println("");
    print("  Total mapped: ");
    print_hex(stats.total_mapped_bytes);
    println(" bytes");
}
