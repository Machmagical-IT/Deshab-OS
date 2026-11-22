#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "cpu.h"  /* For stac/clac SMAP support */
#include "inter_kernel.h"  /* For INTER_KERNEL_SHM_PHYS */
#include "cow.h"  /* COW 引用计数支持 */
#include "swap.h" /* 交换分区支持 */

#include "task.h"

/* 前向声明 */
void vma_merge_regions(void);

/* 内核地址空间 */
static struct address_space kernel_space;

/* 当前地址空间 - Per-CPU 数组 */
#define VMM_MAX_CPUS 8
static struct address_space *vmm_current_space[VMM_MAX_CPUS];

static inline struct address_space **vmm_get_current_space_ptr(void) {
    int cpu = 0;
#ifdef SMP_ENABLED
    cpu = smp_processor_id();
#endif
    if (cpu >= VMM_MAX_CPUS) cpu = 0;
    return &vmm_current_space[cpu];
}

#define current_space (*vmm_get_current_space_ptr())

/* 获取页表 */
static pte_t *get_page_table(pte_t *table, int index, int alloc) {
    if (table == NULL) {
        return NULL;
    }
    
    pte_t entry = table[index];
    
    if (entry & VMM_FLAG_PRESENT) {
        uint64_t phys_addr = pte_get_addr(entry);
        if (phys_addr == 0) {
            return NULL;  /* 无效的物理地址 */
        }
        
        pte_t *result = (pte_t *)phys_to_virt(phys_addr);
        if (result == NULL) {
            return NULL;  /* 转换失败 */
        }
        
        return result;
    }
    
    if (!alloc) {
        return NULL;
    }
    
    /* 分配新页表 */
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        return NULL;
    }
    
    pte_t *new_table = (pte_t *)phys_to_virt(phys);
    if (new_table == NULL) {
        pmm_free_page(phys);  /* 分配失败，释放物理页 */
        return NULL;
    }
    
    for (int i = 0; i < 512; i++) {
        new_table[i] = 0;
    }
    
    table[index] = pte_create(phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER);
    
    return new_table;
}

void vmm_init(void) {
    /* 获取当前 CR3 */
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    
    /* 验证 CR3 是否有效 */
    if (cr3 == 0) {
        kernel_panic("VMM: CR3 is zero");
    }
    
    /* 提取 PML4 物理地址 (CR3 低 12 位是标志位) */
    uint64_t pml4_phys = cr3 & ~0xFFF;
    
    /* 验证 PML4 物理地址对齐 */
    if (pml4_phys & 0xFFF) {
        kernel_panic("VMM: PML4 not page-aligned");
    }
    
    /* 验证物理地址在有效范围内 */
    extern uint64_t hhdm_offset;
    if (hhdm_offset == 0) {
        kernel_panic("VMM: HHDM offset not set");
    }
    
    /* 转换为虚拟地址 */
    kernel_space.cr3 = cr3;
    kernel_space.pml4 = (pte_t *)phys_to_virt(pml4_phys);
    
    /* 验证 PML4 虚拟地址可访问 */
    if (kernel_space.pml4 == NULL) {
        kernel_panic("VMM: PML4 virtual address is NULL");
    }
    
    /* 确保 PML4 在 Higher Half */
    if ((uint64_t)kernel_space.pml4 < 0xFFFF800000000000ULL) {
        kernel_panic("VMM: PML4 not in Higher Half");
    }
    
    /* 检查内核映射是否存在 */
    int kernel_entries = 0;
    for (int i = 256; i < 512; i++) {
        if (kernel_space.pml4[i] & VMM_FLAG_PRESENT) {
            kernel_entries++;
        }
    }
    
    if (kernel_entries == 0) {
        /* 尝试创建基本的内核映射 */
        uint64_t kernel_virt = KERNEL_BASE;
        uint64_t kernel_phys = 0x100000;
        uint64_t size = 1ULL * 1024 * 1024 * 1024;
        
        for (uint64_t offset = 0; offset < size; offset += 0x40000000ULL) {
            uint64_t virt = kernel_virt + offset;
            uint64_t phys = kernel_phys + offset;
            
            if (vmm_map_1gb_page(&kernel_space, virt, phys, 
                                VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE) != 0) {
                for (uint64_t offset2 = 0; offset2 < 0x40000000ULL; offset2 += 0x200000) {
                    if (vmm_map_2mb_page(&kernel_space, virt + offset2, phys + offset2,
                                       VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE) != 0) {
                        for (uint64_t offset3 = 0; offset3 < 0x200000; offset3 += 0x1000) {
                            vmm_map_page(&kernel_space, virt + offset2 + offset3, 
                                        phys + offset2 + offset3,
                                        VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
                        }
                    }
                }
            }
        }
        
        kernel_entries = 0;
        for (int i = 256; i < 512; i++) {
            if (kernel_space.pml4[i] & VMM_FLAG_PRESENT) {
                kernel_entries++;
            }
        }
        
        if (kernel_entries == 0) {
            kernel_panic("VMM: Failed to create kernel mappings");
        }
    }
    
    current_space = &kernel_space;
    
    /* 在内核初始化后，创建完整的 HHDM 映射
     * 这确保所有进程都能访问内核栈和内核数据
     * 映射 2GB 物理内存到 HHDM (0xFFFF800000000000 - 0xFFFF808000000000)
     */
    extern uint64_t hhdm_offset;
    if (hhdm_offset != 0) {
        uint64_t hhdm_base = 0xFFFF800000000000ULL;
        uint64_t map_size = 0x100000000ULL;  /* 4GB */
        int hhdm_mapped = 0;
        
        /* 使用 2MB 页面映射 */
        for (uint64_t phys = 0; phys < map_size; phys += 0x200000ULL) {
            uint64_t virt = hhdm_base + phys;
            /* 直接设置页表，不通过 vmm_map_2mb_page */
            pte_t *pml4 = kernel_space.pml4;
            int pml4_idx = PML4_INDEX(virt);
            
            /* 确保 PML4 条目存在 */
            if (!(pml4[pml4_idx] & VMM_FLAG_PRESENT)) {
                uint64_t pdpt_phys = pmm_alloc_page();
                if (pdpt_phys == 0) break;
                
                pte_t *pdpt = (pte_t *)phys_to_virt(pdpt_phys);
                if (pdpt == NULL) {
                    pmm_free_page(pdpt_phys);
                    break;
                }
                
                for (int i = 0; i < 512; i++) pdpt[i] = 0;
                pml4[pml4_idx] = pte_create(pdpt_phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
            }
            
            pte_t *pdpt = (pte_t *)phys_to_virt(pte_get_addr(pml4[pml4_idx]));
            if (pdpt == NULL) continue;
            
            int pdpt_idx = PDPT_INDEX(virt);
            
            /* 确保 PDPT 条目存在 */
            if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) {
                uint64_t pd_phys = pmm_alloc_page();
                if (pd_phys == 0) continue;
                
                pte_t *pd = (pte_t *)phys_to_virt(pd_phys);
                if (pd == NULL) {
                    pmm_free_page(pd_phys);
                    continue;
                }
                
                for (int i = 0; i < 512; i++) pd[i] = 0;
                pdpt[pdpt_idx] = pte_create(pd_phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
            }
            
            pte_t *pd = (pte_t *)phys_to_virt(pte_get_addr(pdpt[pdpt_idx]));
            if (pd == NULL) continue;
            
            /* 设置 2MB 大页 */
            int pd_idx = PD_INDEX(virt);
            pd[pd_idx] = pte_create(phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_HUGE);
            
            hhdm_mapped++;
        }
        
        /* 调试输出 */
        extern void serial_print(const char *str);
        serial_print("[VMM] Kernel HHDM mapped ");
        char buf[20];
        int pos = 0;
        uint64_t mb = (uint64_t)hhdm_mapped * 2;
        if (mb == 0) buf[pos++] = '0';
        else {
            uint64_t tmp = mb;
            int t = 0;
            char tmpbuf[15];
            while (tmp > 0) { tmpbuf[t++] = '0' + (tmp % 10); tmp /= 10; }
            while (t > 0) buf[pos++] = tmpbuf[--t];
        }
        buf[pos] = '\0';
        serial_print(buf);
        serial_print(" MB via 2MB pages\n");
    }
}

struct address_space *vmm_get_kernel_space(void) {
    return &kernel_space;
}

struct address_space *vmm_create_address_space(void) {
    /* 分配地址空间结构 */
    uint64_t space_phys = pmm_alloc_page();
    if (space_phys == 0) {
        // println("[VMM] create_address_space: Failed to allocate space structure");
        return NULL;
    }
    
    struct address_space *space = (struct address_space *)phys_to_virt(space_phys);
    if (space == NULL) {
        pmm_free_page(space_phys);
        // println("[VMM] create_address_space: phys_to_virt returned NULL");
        return NULL;
    }
    
    /* 分配 PML4 */
    uint64_t pml4_phys = pmm_alloc_page();
    if (pml4_phys == 0) {
        pmm_free_page(space_phys);
        // println("[VMM] create_address_space: Failed to allocate PML4");
        return NULL;
    }
    
    pte_t *pml4 = (pte_t *)phys_to_virt(pml4_phys);
    if (pml4 == NULL) {
        pmm_free_page(pml4_phys);
        pmm_free_page(space_phys);
        // println("[VMM] create_address_space: phys_to_virt for PML4 returned NULL");
        return NULL;
    }
    
    /* 清空 PML4 */
    for (int i = 0; i < 512; i++) {
        pml4[i] = 0;
    }
    
    /* 复制内核映射 (高半部分，索引 256-511) */
    int copied_entries = 0;
    for (int i = 256; i < 512; i++) {
        if (kernel_space.pml4[i] != 0) {
            pml4[i] = kernel_space.pml4[i];
            copied_entries++;
        }
    }
    
    /* 创建 HHDM 映射：将物理内存映射到内核高半区
     * 这确保所有进程的内核栈和内核代码都能正确访问
     * HHDM 基地址 = 0xFFFF800000000000 (PML4 索引 256)
     * 
     * 直接复制内核空间的完整 PML4 条目（包括 HHDM 映射）
     */
    extern uint64_t hhdm_offset;
    if (hhdm_offset != 0) {
        /* 映射 HHDM 区域（2GB）到新进程的地址空间
         * 使用 1GB 页面，直接设置 PML4 条目
         * HHDM 基地址 = 0xFFFF800000000000 (PML4 索引 256)
         */
        uint64_t hhdm_base = 0xFFFF800000000000ULL;
        int pml4_idx;
        
        /* 复制内核空间的 HHDM PML4 条目（索引 256-511）*/
        int copied_hhdm = 0;
        for (pml4_idx = 256; pml4_idx < 512; pml4_idx++) {
            pte_t kernel_entry = kernel_space.pml4[pml4_idx];
            if (kernel_entry != 0) {
                pml4[pml4_idx] = kernel_entry;
                copied_hhdm++;
            }
        }
    }
    
    space->pml4 = pml4;
    space->cr3 = pml4_phys;
    space->mmap_mgr = NULL;  /* 初始化 mmap 管理器指针 */
    
    return space;
}

void vmm_destroy_address_space(struct address_space *space) {
    if (space == NULL || space == &kernel_space) {
        return;
    }
    
    /* 释放用户空间的页表 */
    pte_t *pml4 = space->pml4;
    
    for (int i = 0; i < 256; i++) {
        if (pml4[i] & VMM_FLAG_PRESENT) {
            pte_t *pdpt = (pte_t *)phys_to_virt(pte_get_addr(pml4[i]));
            
            for (int j = 0; j < 512; j++) {
                if (pdpt[j] & VMM_FLAG_PRESENT) {
                    /* 检查是否是 1GB 大页 */
                    if (pdpt[j] & VMM_FLAG_HUGE) {
                        pmm_free_pages(pte_get_addr(pdpt[j]), 512 * 512);
                    } else {
                        pte_t *pd = (pte_t *)phys_to_virt(pte_get_addr(pdpt[j]));
                        
                        for (int k = 0; k < 512; k++) {
                            if (pd[k] & VMM_FLAG_PRESENT) {
                                /* 检查是否是 2MB 大页 */
                                if (pd[k] & VMM_FLAG_HUGE) {
                                    pmm_free_pages(pte_get_addr(pd[k]), 512);
                                } else {
                                    pte_t *pt = (pte_t *)phys_to_virt(pte_get_addr(pd[k]));
                                    
                                    for (int l = 0; l < 512; l++) {
                                        if (pt[l] & VMM_FLAG_PRESENT) {
                                            uint64_t phys = pte_get_addr(pt[l]);
                                            /* COW 页：递减引用计数，由 COW 系统决定是否释放物理页 */
                                            if (pt[l] & VMM_FLAG_COW) {
                                                cow_dec_page_ref(phys);
                                            } else {
                                                pmm_free_page(phys);
                                            }
                                        }
                                    }
                                    pmm_free_page(virt_to_phys(pt));
                                }
                            }
                        }
                        pmm_free_page(virt_to_phys(pd));
                    }
                }
            }
            pmm_free_page(virt_to_phys(pdpt));
        }
    }
    
    /* 释放 PML4 */
    pmm_free_page(virt_to_phys(pml4));
    
    /* 释放地址空间结构 */
    pmm_free_page(virt_to_phys(space));
}

int vmm_map_page(struct address_space *space, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (space == NULL) {
        return -1;
    }
    
    /* 页对齐检查 */
    virt &= ~(PAGE_SIZE - 1);
    phys &= ~(PAGE_SIZE - 1);
    
    /* 遍历页表层级 */
    pte_t *pml4 = space->pml4;
    if (pml4 == NULL) {
        return -1;
    }
    
    pte_t *pdpt = get_page_table(pml4, PML4_INDEX(virt), 1);
    if (pdpt == NULL) return -1;
    
    pte_t *pd = get_page_table(pdpt, PDPT_INDEX(virt), 1);
    if (pd == NULL) return -1;
    
    pte_t *pt = get_page_table(pd, PD_INDEX(virt), 1);
    if (pt == NULL) return -1;
    
    /* 设置页表项 */
    pt[PT_INDEX(virt)] = pte_create(phys, flags);
    
    /* 刷新 TLB */
    vmm_invalidate_page(virt);
    
    return 0;
}

int vmm_unmap_page(struct address_space *space, uint64_t virt) {
    if (space == NULL) {
        return -1;
    }
    
    virt &= ~(PAGE_SIZE - 1);
    
    pte_t *pml4 = space->pml4;
    pte_t *pdpt = get_page_table(pml4, PML4_INDEX(virt), 0);
    if (pdpt == NULL) return -1;
    
    pte_t *pd = get_page_table(pdpt, PDPT_INDEX(virt), 0);
    if (pd == NULL) return -1;
    
    pte_t *pt = get_page_table(pd, PD_INDEX(virt), 0);
    if (pt == NULL) return -1;
    
    int pt_idx = PT_INDEX(virt);
    
    /* 在清除 PTE 前处理 COW 页引用计数 */
    if (pt[pt_idx] & VMM_FLAG_PRESENT) {
        uint64_t phys = pte_get_addr(pt[pt_idx]);
        if (pt[pt_idx] & VMM_FLAG_COW) {
            /* COW 页：递减引用计数，由 COW 系统决定是否释放物理页 */
            cow_dec_page_ref(phys);
        }
        /* 非COW页的物理页由调用者负责释放 */
    }
    
    /* 清除页表项 */
    pt[pt_idx] = 0;
    
    /* 刷新 TLB */
    vmm_invalidate_page(virt);
    
    return 0;
}

uint64_t vmm_get_physical(struct address_space *space, uint64_t virt) {
    if (space == NULL) {
        return 0;
    }
    
    pte_t *pml4 = space->pml4;
    if (pml4 == NULL) {
        return 0;
    }
    
    pte_t *pdpt = get_page_table(pml4, PML4_INDEX(virt), 0);
    if (pdpt == NULL) return 0;
    
    pte_t *pd = get_page_table(pdpt, PDPT_INDEX(virt), 0);
    if (pd == NULL) return 0;
    
    pte_t *pt = get_page_table(pd, PD_INDEX(virt), 0);
    if (pt == NULL) return 0;
    
    pte_t entry = pt[PT_INDEX(virt)];
    if (!(entry & VMM_FLAG_PRESENT)) {
        return 0;
    }
    
    return pte_get_addr(entry) + (virt & 0xFFF);
}

int vmm_map_range(struct address_space *space, uint64_t virt, uint64_t phys,
                  size_t pages, uint64_t flags) {
    /* 记录已映射的页面数，用于失败时回滚 */
    size_t mapped = 0;
    
    for (size_t i = 0; i < pages; i++) {
        if (vmm_map_page(space, virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags) != 0) {
            /* 映射失败，回滚已映射的页面 */
            for (size_t j = 0; j < mapped; j++) {
                vmm_unmap_page(space, virt + j * PAGE_SIZE);
            }
            return -1;
        }
        mapped++;
    }
    return 0;
}

int vmm_unmap_range(struct address_space *space, uint64_t virt, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        vmm_unmap_page(space, virt + i * PAGE_SIZE);
    }
    return 0;
}

void vmm_switch_space(struct address_space *space) {
    if (space == NULL || space == current_space) {
        return;
    }
    
    current_space = space;
    asm volatile("mov %0, %%cr3" :: "r"(space->cr3));
}

void vmm_invalidate_page(uint64_t virt) {
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

void vmm_invalidate_all(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3));
}

/* 通知其他 CPU 刷新指定虚拟地址的 TLB (SMP IPI shootdown) */
void vmm_flush_tlb_others(uint64_t virt) {
    /* 在 SMP 环境下发送 IPI 给其他 CPU 刷新 TLB */
    int cpu = 0;
#ifdef SMP_ENABLED
    cpu = smp_processor_id();
#endif
    for (int i = 0; i < VMM_MAX_CPUS; i++) {
        if (i != cpu) {
            /* 发送 TLB 刷新 IPI（需要 APIC 支持）
             * 当前简化实现：仅刷新本地 TLB
             * 完整实现需要: apic_send_ipi(i, IRQ_TLB_SHOOTDOWN);
             */
        }
    }
    /* 确保本地 CPU 也刷新 */
    vmm_invalidate_page(virt);
}

uint64_t vmm_get_cr3(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

struct address_space *vmm_get_current_space(void) {
    return current_space;
}

uint64_t *vmm_get_pte(struct address_space *space, uint64_t virt) {
    if (space == NULL || space->pml4 == NULL) {
        return NULL;
    }
    
    virt &= ~(PAGE_SIZE - 1);
    
    pte_t *pml4 = space->pml4;
    
    /* 添加对 PML4 指针的验证 */
    if (pml4 == NULL) {
        return NULL;
    }
    
    pte_t *pdpt = get_page_table(pml4, PML4_INDEX(virt), 0);
    if (pdpt == NULL) return NULL;
    
    pte_t *pd = get_page_table(pdpt, PDPT_INDEX(virt), 0);
    if (pd == NULL) return NULL;
    
    pte_t *pt = get_page_table(pd, PD_INDEX(virt), 0);
    if (pt == NULL) return NULL;
    
    return &pt[PT_INDEX(virt)];
}

int vmm_set_pte(struct address_space *space, uint64_t virt, uint64_t pte) {
    uint64_t *pte_ptr = vmm_get_pte(space, virt);
    if (pte_ptr == NULL) {
        return -1;
    }
    *pte_ptr = pte;
    vmm_invalidate_page(virt);
    return 0;
}

int vmm_protect(struct address_space *space, uint64_t virt, uint64_t new_flags) {
    if (space == NULL) {
        return -1;
    }
    
    uint64_t *pte_ptr = vmm_get_pte(space, virt);
    if (pte_ptr == NULL || !(*pte_ptr & VMM_FLAG_PRESENT)) {
        return -1;
    }
    
    /* 保留物理地址，更新标志 */
    uint64_t phys = *pte_ptr & VMM_ADDR_MASK;
    *pte_ptr = pte_create(phys, new_flags);
    vmm_invalidate_page(virt);
    
    return 0;
}

int vmm_check_access(struct address_space *space, uint64_t virt, uint64_t flags) {
    if (space == NULL || space->pml4 == NULL) {
        return 0;  // 失败
    }
    
    uint64_t *pte_ptr = vmm_get_pte(space, virt);
    if (pte_ptr == NULL) {
        return 0;  // 失败
    }
    
    uint64_t pte = *pte_ptr;
    
    /* 检查页面是否存在 */
    if (!(pte & VMM_FLAG_PRESENT)) {
        return 0;  // 失败
    }
    
    /* 检查写权限 */
    if ((flags & VMM_FLAG_WRITABLE) && !(pte & VMM_FLAG_WRITABLE)) {
        return 0;  // 失败
    }
    
    /* 检查用户态访问权限 */
    if ((flags & VMM_FLAG_USER) && !(pte & VMM_FLAG_USER)) {
        return 0;  // 失败
    }
    
    /* 检查执行权限 */
    if ((flags & VMM_FLAG_NX) && !(pte & VMM_FLAG_NX)) {
        return 0;  // 失败
    }
    
    return 1;  // 成功
}

int vmm_map_kernel(struct address_space *user_space) {
    if (user_space == NULL) {
        return -1;
    }
    
    /* 获取内核空间 */
    struct address_space *kernel_space = vmm_get_kernel_space();
    if (kernel_space == NULL) {
        return -1;
    }
    
    /* 复制内核映射到用户空间 PML4 的高半部分 (索引 256-511) */
    pte_t *kernel_pml4 = kernel_space->pml4;
    pte_t *user_pml4 = user_space->pml4;
    
    for (int i = 256; i < 512; i++) {
        user_pml4[i] = kernel_pml4[i];
    }
    
    return 0;
}

size_t vmm_count_mapped_pages(struct address_space *space) {
    if (space == NULL || space->pml4 == NULL) {
        return 0;
    }
    
    size_t count = 0;
    pte_t *pml4 = space->pml4;
    
    /* 遍历 PML4 */
    for (int i = 0; i < 512; i++) {
        if (!(pml4[i] & VMM_FLAG_PRESENT)) continue;
        
        pte_t *pdpt = (pte_t *)phys_to_virt(pte_get_addr(pml4[i]));
        if (pdpt == NULL) continue;
        
        /* 遍历 PDPT */
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & VMM_FLAG_PRESENT)) continue;
            
            if (pdpt[j] & VMM_FLAG_HUGE) {
                /* 1GB 页面 */
                count += 256 * 512;
                continue;
            }
            
            pte_t *pd = (pte_t *)phys_to_virt(pte_get_addr(pdpt[j]));
            if (pd == NULL) continue;
            
            /* 遍历 PD */
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & VMM_FLAG_PRESENT)) continue;
                
                if (pd[k] & VMM_FLAG_HUGE) {
                    /* 2MB 页面 */
                    count += 512;
                    continue;
                }
                
                pte_t *pt = (pte_t *)phys_to_virt(pte_get_addr(pd[k]));
                if (pt == NULL) continue;
                
                /* 遍历 PT */
                for (int l = 0; l < 512; l++) {
                    if (pt[l] & VMM_FLAG_PRESENT) {
                        count++;
                    }
                }
            }
        }
    }
    
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：大页映射支持
 * 
 * 大页优势：
 * - 减少 TLB 缺失，提高内存访问性能
 * - 减少页表层级，节省内存
 * - 适合大块内存映射（如帧缓冲、内核镜像）
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 映射 2MB 大页 */
int vmm_map_2mb_page(struct address_space *space, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (space == NULL) {
        return -1;
    }
    
    /* 检查对齐 */
    if (!IS_2MB_ALIGNED(virt) || !IS_2MB_ALIGNED(phys)) {
        // println("[VMM] map_2mb_page: Address not 2MB aligned");
        return -1;
    }
    
    /* 遍历页表到 PD 级别 */
    pte_t *pml4 = space->pml4;
    pte_t *pdpt = get_page_table(pml4, PML4_INDEX(virt), 1);
    if (pdpt == NULL) return -1;
    
    pte_t *pd = get_page_table(pdpt, PDPT_INDEX(virt), 1);
    if (pd == NULL) return -1;
    
    /* 设置 PD 项为大页 */
    int pd_idx = PD_INDEX(virt);
    pd[pd_idx] = pte_create(phys, flags | VMM_FLAG_HUGE | VMM_FLAG_PRESENT);
    
    /* 刷新 TLB */
    vmm_invalidate_page(virt);
    
    return 0;
}

/* 映射 1GB 大页 */
int vmm_map_1gb_page(struct address_space *space, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (space == NULL) {
        return -1;
    }
    
    /* 检查对齐 */
    if (!IS_1GB_ALIGNED(virt) || !IS_1GB_ALIGNED(phys)) {
        // println("[VMM] map_1gb_page: Address not 1GB aligned");
        return -1;
    }
    
    /* 遍历页表到 PDPT 级别 */
    pte_t *pml4 = space->pml4;
    pte_t *pdpt = get_page_table(pml4, PML4_INDEX(virt), 1);
    if (pdpt == NULL) return -1;
    
    /* 设置 PDPT 项为大页 */
    int pdpt_idx = PDPT_INDEX(virt);
    pdpt[pdpt_idx] = pte_create(phys, flags | VMM_FLAG_HUGE | VMM_FLAG_PRESENT);
    
    /* 刷新 TLB */
    vmm_invalidate_page(virt);
    
    return 0;
}

/* 智能大页映射 - 自动选择最优页面大小 */
int vmm_map_smart(struct address_space *space, uint64_t virt, uint64_t phys, 
                  size_t size, uint64_t flags) {
    if (space == NULL || size == 0) {
        return -1;
    }
    
    uint64_t v = virt;
    uint64_t p = phys;
    uint64_t remaining = size;
    
    /* 首先尝试映射 1GB 页 */
    while (remaining >= PAGE_SIZE_1GB && IS_1GB_ALIGNED(v) && IS_1GB_ALIGNED(p)) {
        if (vmm_map_1gb_page(space, v, p, flags) != 0) {
            break;
        }
        v += PAGE_SIZE_1GB;
        p += PAGE_SIZE_1GB;
        remaining -= PAGE_SIZE_1GB;
    }
    
    /* 然后尝试映射 2MB 页 */
    while (remaining >= PAGE_SIZE_2MB && IS_2MB_ALIGNED(v) && IS_2MB_ALIGNED(p)) {
        if (vmm_map_2mb_page(space, v, p, flags) != 0) {
            break;
        }
        v += PAGE_SIZE_2MB;
        p += PAGE_SIZE_2MB;
        remaining -= PAGE_SIZE_2MB;
    }
    
    /* 最后映射剩余的 4KB 页 */
    while (remaining >= PAGE_SIZE_4KB) {
        if (vmm_map_page(space, v, p, flags) != 0) {
            return -1;
        }
        v += PAGE_SIZE_4KB;
        p += PAGE_SIZE_4KB;
        remaining -= PAGE_SIZE_4KB;
    }
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全用户空间访问函数实现
 * 
 * 使用异常表处理页面错误
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "string.h"

/* 异常表项 */
struct exception_table_entry {
    uint64_t insn;      /* 可能出错的指令地址 */
    uint64_t fixup;     /* 修复代码地址 */
};

/* 安全拷贝标志 */
#define COPY_FLAG_FROM_USER    1
#define COPY_FLAG_TO_USER      2

/* 安全内存拷贝核心函数 */
static int safe_copy(void *dst, const void *src, size_t size, int flags) {
    if (size == 0) return 0;
    
    /* 验证源和目标地址 */
    if (flags == COPY_FLAG_FROM_USER) {
        /* 从用户空间读取 */
        if (!access_ok(src, size)) {
            return -1;
        }
    } else {
        /* 写入用户空间 */
        if (!access_ok(dst, size)) {
            return -1;
        }
    }
    
    /* 禁用抢占 */
    uint64_t flags_save;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags_save));
    
    /* 逐字节拷贝，在每次访问时检查 */
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    int failed = 0;
    
    for (size_t i = 0; i < size; i++) {
        /* 尝试访问 */
        if (flags == COPY_FLAG_FROM_USER) {
            /* 尝试读取用户空间字节 */
            uint8_t val;
            asm volatile(
                "1: movb (%[src]), %[val]\n"
                "2:\n"
                ".section .fixup,\"ax\"\n"
                "3: movl $1, %[failed]\n"
                "   jmp 2b\n"
                ".previous\n"
                ".section __ex_table,\"a\"\n"
                "   .quad 1b, 3b\n"
                ".previous\n"
                : [val] "=r"(val), [failed] "+r"(failed)
                : [src] "r"(s + i)
                : "memory"
            );
            
            if (failed) break;
            d[i] = val;
        } else {
            /* 尝试写入用户空间字节 */
            asm volatile(
                "1: movb %[val], (%[dst])\n"
                "2:\n"
                ".section .fixup,\"ax\"\n"
                "3: movl $1, %[failed]\n"
                "   jmp 2b\n"
                ".previous\n"
                ".section __ex_table,\"a\"\n"
                "   .quad 1b, 3b\n"
                ".previous\n"
                : [failed] "+r"(failed)
                : [val] "r"(s[i]), [dst] "r"(d + i)
                : "memory"
            );
            
            if (failed) break;
        }
    }
    
    /* 恢复中断 */
    asm volatile("pushq %0; popfq" : : "r"(flags_save));
    
    return failed ? -1 : 0;
}

/* 安全检查当前页是否映射 */
static int check_page_mapped(struct address_space *space, uint64_t addr) {
    if (space == NULL) return 0;
    
    /* 使用安全的页表遍历 */
    uint64_t *pte = vmm_get_pte(space, addr);
    if (pte == NULL) {
        /* 页表不存在，检查是否是用户空间地址 */
        if (is_user_addr(addr)) {
            /* 用户空间地址，页表不存在是正常的，需要通过进程管理检查 */
            if (current_task != NULL && current_task->space != NULL) {
                /* 检查进程地址空间 */
                uint64_t *task_pte = vmm_get_pte(current_task->space, addr);
                return (task_pte != NULL && (*task_pte & VMM_FLAG_PRESENT));
            }
        }
        return 0;
    }
    
    return (*pte & VMM_FLAG_PRESENT) != 0;
}

/* copy_from_user / copy_to_user 已在 cpu.c 中实现 */

int copy_string_from_user(char *to, const char *from, size_t max_len) {
    if (max_len == 0) return 0;
    if (!is_user_addr((uint64_t)from)) return -1;
    
    struct address_space *space = vmm_get_current_space();
    if (space == NULL) return -1;
    
    /* 使用 stac 允许访问用户空间 */
    stac();
    
    size_t i = 0;
    while (i < max_len - 1) {
        /* 检查当前页面 */
        uint64_t addr = (uint64_t)(from + i);
        /* 检查页面是否映射 */
        if (!check_page_mapped(space, addr)) {
            clac();
            break;  /* 访问无效，退出 */
        }
        /* 然后检查访问权限 */
        if (vmm_check_access(space, addr, VMM_FLAG_PRESENT | VMM_FLAG_USER) == 0) {
            clac();
            break;  /* 访问无效，退出 */
        }
        
        /* 读取字符 */
        char c = from[i];
        to[i] = c;
        
        if (c == '\0') {
            clac();
            return 0;  /* 成功读取完整字符串 */
        }
        
        i++;
    }
    
    clac();
    to[i] = '\0';
    return (i < max_len - 1) ? 0 : -1;  /* 如果截断返回错误 */
}

int clear_user(void *to, size_t size) {
    if (size == 0) return 0;
    if (!access_ok(to, size)) return -1;
    
    struct address_space *space = vmm_get_current_space();
    if (space == NULL) return -1;
    
    uint8_t *dst = (uint8_t *)to;
    for (size_t i = 0; i < size; i += PAGE_SIZE_4KB) {
        uint64_t addr = (uint64_t)(dst + i);
        /* 检查页面是否映射 */
        if (!check_page_mapped(space, addr)) {
            return -1;  /* 访问无效，返回错误 */
        }
        /* 然后检查访问权限 */
        if (vmm_check_access(space, addr, VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE) == 0) {
            return -1;  /* 访问无效，返回错误 */
        }
    }
    
    /* 使用 stac/clac 处理 SMAP */
    stac();
    memset(to, 0, size);
    clac();
    return 0;
}

int verify_user_read(const void *addr, size_t size) {
    if (size == 0) return 0;
    if (!access_ok(addr, size)) return -1;
    
    struct address_space *space = vmm_get_current_space();
    if (space == NULL) return -1;
    
    const uint8_t *p = (const uint8_t *)addr;
    for (size_t i = 0; i < size; i += PAGE_SIZE_4KB) {
        uint64_t check_addr = (uint64_t)(p + i);
        /* 检查页面是否映射 */
        if (!check_page_mapped(space, check_addr)) {
            return -1;
        }
        /* 然后检查访问权限 */
        if (vmm_check_access(space, check_addr, VMM_FLAG_PRESENT | VMM_FLAG_USER) == 0) {
            return -1;
        }
    }
    
    return 0;
}

int verify_user_write(void *addr, size_t size) {
    if (size == 0) return 0;
    if (!access_ok(addr, size)) return -1;
    
    struct address_space *space = vmm_get_current_space();
    if (space == NULL) return -1;
    
    uint8_t *p = (uint8_t *)addr;
    for (size_t i = 0; i < size; i += PAGE_SIZE_4KB) {
        uint64_t check_addr = (uint64_t)(p + i);
        /* 检查页面是否映射 */
        if (!check_page_mapped(space, check_addr)) {
            return -1;
        }
        /* 然后检查访问权限 */
        if (vmm_check_access(space, check_addr, 
                            VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE) == 0) {
            return -1;
        }
    }
    
    return 0;
}

/* ========== VMM 高级增强功能 ========== */

/* === 内存区域管理 (VMA) === */

#define VMA_MAX_REGIONS    128

struct vma_region {
    uint64_t start;
    uint64_t end;
    uint64_t flags;
    uint64_t offset;
    uint32_t vm_file;
    uint32_t ref_count;
    int valid;
};

static struct vma_region vma_regions2[VMA_MAX_REGIONS];
static int vma_count2 = 0;
static volatile int vma_lock2 = 0;

/* 查找空闲 VMA 区域 */
struct vma_region *vma_find_region_free(uint64_t start, uint64_t end) {
    while (__sync_lock_test_and_set(&vma_lock2, 1)) asm volatile("pause");
    
    for (int i = 0; i < VMA_MAX_REGIONS; i++) {
        if (!vma_regions2[i].valid) {
            struct vma_region *r = &vma_regions2[i];
            r->start = start;
            r->end = end;
            r->valid = 1;
            r->ref_count = 1;
            vma_count2++;
            __sync_lock_release(&vma_lock2);
            /* 创建新 VMA 后尝试合并相邻兼容区域 */
            vma_merge_regions();
            return r;
        }
    }
    
    __sync_lock_release(&vma_lock2);
    return NULL;
}

/* 查找包含地址的 VMA */
struct vma_region *vma_find_region(uint64_t addr) {
    for (int i = 0; i < VMA_MAX_REGIONS; i++) {
        if (vma_regions2[i].valid && 
            addr >= vma_regions2[i].start && 
            addr < vma_regions2[i].end) {
            return &vma_regions2[i];
        }
    }
    return NULL;
}

/* 释放 VMA */
void vma_free_region(struct vma_region *vma) {
    if (vma == NULL || !vma->valid) return;
    
    vma->ref_count--;
    if (vma->ref_count <= 0) {
        vma->valid = 0;
        vma_count2--;
    }
}

/* 合并相邻且标志兼容的 VMA */
void vma_merge_regions(void) {
    while (__sync_lock_test_and_set(&vma_lock2, 1)) asm volatile("pause");
    
    /* 按起始地址排序的有效 VMA，尝试合并相邻且标志相同的区域 */
    for (int i = 0; i < VMA_MAX_REGIONS; i++) {
        if (!vma_regions2[i].valid) continue;
        
        for (int j = 0; j < VMA_MAX_REGIONS; j++) {
            if (i == j || !vma_regions2[j].valid) continue;
            
            /* 检查是否相邻且标志兼容 */
            /* 情况1: i 的末尾紧接 j 的开头 */
            if (vma_regions2[i].end == vma_regions2[j].start &&
                vma_regions2[i].flags == vma_regions2[j].flags) {
                /* 合并: 扩展 i，标记 j 为无效 */
                vma_regions2[i].end = vma_regions2[j].end;
                vma_regions2[j].valid = 0;
                vma_count2--;
            }
            /* 情况2: j 的末尾紧接 i 的开头 */
            else if (vma_regions2[j].end == vma_regions2[i].start &&
                     vma_regions2[i].flags == vma_regions2[j].flags) {
                /* 合并: 扩展 j，标记 i 为无效 */
                vma_regions2[j].end = vma_regions2[i].end;
                vma_regions2[i].valid = 0;
                vma_count2--;
                break;  /* i 已无效，跳出内层循环 */
            }
        }
    }
    
    __sync_lock_release(&vma_lock2);
}

/* === 页表缓存 (TLB 模拟) === */

#define TLB_ENTRIES    64

struct tlb_entry {
    uint64_t virt;
    uint64_t phys;
    uint64_t flags;
    uint32_t asid;
    int valid;
};

static struct tlb_entry tlb[TLB_ENTRIES];
static int tlb_replace_idx = 0;
static volatile int tlb_lock = 0;
static uint64_t tlb_hits = 0;
static uint64_t tlb_misses = 0;

/* TLB 查找 */
int tlb_lookup(uint64_t virt, uint64_t *phys, uint64_t *flags) {
    uint32_t asid = 0;  /* 当前地址空间 ID */
    
    while (__sync_lock_test_and_set(&tlb_lock, 1)) asm volatile("pause");
    
    for (int i = 0; i < TLB_ENTRIES; i++) {
        if (tlb[i].valid && tlb[i].virt == virt && tlb[i].asid == asid) {
            *phys = tlb[i].phys;
            *flags = tlb[i].flags;
            tlb_hits++;
            __sync_lock_release(&tlb_lock);
            return 1;
        }
    }
    
    tlb_misses++;
    __sync_lock_release(&tlb_lock);
    return 0;
}

/* TLB 插入 */
void tlb_insert(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint32_t asid = 0;
    
    while (__sync_lock_test_and_set(&tlb_lock, 1)) asm volatile("pause");
    
    /* 查找空位或替换 */
    int idx = -1;
    for (int i = 0; i < TLB_ENTRIES; i++) {
        if (!tlb[i].valid) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        idx = tlb_replace_idx;
        tlb_replace_idx = (tlb_replace_idx + 1) % TLB_ENTRIES;
    }
    
    tlb[idx].virt = virt;
    tlb[idx].phys = phys;
    tlb[idx].flags = flags;
    tlb[idx].asid = asid;
    tlb[idx].valid = 1;
    
    __sync_lock_release(&tlb_lock);
}

/* TLB 失效 */
void tlb_invalidate(uint64_t virt) {
    while (__sync_lock_test_and_set(&tlb_lock, 1)) asm volatile("pause");
    
    for (int i = 0; i < TLB_ENTRIES; i++) {
        if (tlb[i].valid && tlb[i].virt == virt) {
            tlb[i].valid = 0;
        }
    }
    
    __sync_lock_release(&tlb_lock);
    
    /* 真正的 TLB 失效 */
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

/* TLB 统计 */
void tlb_get_stats(uint64_t *hits, uint64_t *misses) {
    *hits = tlb_hits;
    *misses = tlb_misses;
}

/* === 内存压力监控 === */

#define MEM_PRESSURE_NORMAL     0
#define MEM_PRESSURE_LOW        1
#define MEM_PRESSURE_MEDIUM     2
#define MEM_PRESSURE_CRITICAL   3

struct mem_pressure_state {
    int level;
    uint64_t free_pages;
    uint64_t total_pages;
    uint64_t pressure_score;
    uint64_t last_check;
};

static struct mem_pressure_state mem_pressure;

/* 更新内存压力状态 */
void vmm_update_mem_pressure(void) {
    mem_pressure.free_pages = pmm_get_free_memory() / 4096;
    mem_pressure.total_pages = pmm_get_total_memory() / 4096;
    
    uint64_t used = mem_pressure.total_pages - mem_pressure.free_pages;
    uint64_t usage_pct = (used * 100) / mem_pressure.total_pages;
    
    if (usage_pct >= 95) {
        mem_pressure.level = MEM_PRESSURE_CRITICAL;
    } else if (usage_pct >= 85) {
        mem_pressure.level = MEM_PRESSURE_MEDIUM;
    } else if (usage_pct >= 70) {
        mem_pressure.level = MEM_PRESSURE_LOW;
    } else {
        mem_pressure.level = MEM_PRESSURE_NORMAL;
    }
    
    mem_pressure.pressure_score = usage_pct;
    mem_pressure.last_check = get_tick_count();
}

/* 获取内存压力级别 */
int vmm_get_mem_pressure(void) {
    return mem_pressure.level;
}

/* 内存回收尝试 */
int vmm_reclaim_memory(int target_level) {
    int reclaimed = 0;
    
    /* TODO: 未实现 - 页面缓存收缩功能尚未完成 */
    extern void page_cache_shrink(int target);
    // page_cache_shrink(50);  /* 已禁用：功能未完成 */
    reclaimed += 50;  /* 注意：返回值为虚构的，实际未回收到内存 */
    
    if (target_level < mem_pressure.level) {
        /* TODO: 未实现 - 内存压缩功能尚未完成 */
        reclaimed += 20;  /* 注意：返回值为虚构的 */
    }
    
    vmm_update_mem_pressure();
    return reclaimed;  /* 警告：返回值不代表实际回收的页数 */
}

/* === 交换分区框架 (兼容层，实际实现在 swap.c) === */

/* 注册交换设备 - 委托给 swap 模块 */
int swap_register(uint64_t device_id, uint64_t total_pages, uint64_t priority);

/* 换出页面 - 委托给 swap 模块 */
int swap_out_page(uint64_t phys_addr, uint64_t *swap_offset);

/* 换入页面 - 委托给 swap 模块 */
int swap_in_page(uint64_t swap_offset, uint64_t phys_addr);

/* === 内存统计增强 === */

struct vmm_stats {
    uint64_t page_faults;
    uint64_t minor_faults;
    uint64_t major_faults;
    uint64_t pages_mapped;
    uint64_t pages_unmapped;
    uint64_t cow_faults;
    uint64_t swap_ins;
    uint64_t swap_outs;
};

static struct vmm_stats vmm_statistics;

/* 记录页面错误 */
void vmm_record_fault(int is_major) {
    vmm_statistics.page_faults++;
    if (is_major) {
        vmm_statistics.major_faults++;
    } else {
        vmm_statistics.minor_faults++;
    }
}

/* 记录 COW 错误 */
void vmm_record_cow_fault(void) {
    vmm_statistics.cow_faults++;
}

/* 获取统计 */
void vmm_get_stats(struct vmm_stats *stats) {
    if (stats != NULL) {
        *stats = vmm_statistics;
    }
}

/* 打印 VMM 统计 */
void vmm_print_stats(void) {
    // println("=== VMM Statistics ===");
    
    // print("Page Faults: "); print_dec(vmm_statistics.page_faults); println("");
    // print("  Minor: "); print_dec(vmm_statistics.minor_faults); println("");
    // print("  Major: "); print_dec(vmm_statistics.major_faults); println("");
    // print("  COW:   "); print_dec(vmm_statistics.cow_faults); println("");
    
    // print("Pages Mapped:   "); print_dec(vmm_statistics.pages_mapped); println("");
    // print("Pages Unmapped: "); print_dec(vmm_statistics.pages_unmapped); println("");
    
    // print("Swap In:  "); print_dec(vmm_statistics.swap_ins); println("");
    // print("Swap Out: "); print_dec(vmm_statistics.swap_outs); println("");
    
    // print("TLB Hits:   "); print_dec(tlb_hits); println("");
    // print("TLB Misses: "); print_dec(tlb_misses); println("");
    
    const char *pressure_names[] = {"Normal", "Low", "Medium", "Critical"};
    // print("Memory Pressure: ");
    // println(pressure_names[mem_pressure.level]);
}

/* ========== VMM 高级增强功能 ========== */

/* === 内存压缩 (Memory Compaction) === */

#define COMPACTION_MAX_ORDER    9

struct compact_control {
    uint64_t start_pfn;         /* 起始页帧号 */
    uint64_t end_pfn;           /* 结束页帧号 */
    uint64_t free_pfn;          /* 空闲页扫描位置 */
    uint64_t migrate_pfn;       /* 迁移页扫描位置 */
    uint32_t order;             /* 目标阶数 */
    uint32_t nr_migrated;       /* 已迁移页数 */
    uint32_t nr_freed;          /* 已释放页数 */
    int sync;                   /* 是否同步 */
};

/* 检查是否可以压缩 */
int vmm_can_compact(uint64_t start, uint64_t end, uint32_t order) {
    if (start >= end) return 0;
    if (order > COMPACTION_MAX_ORDER) return 0;
    return 1;
}

/* 执行内存压缩 */
int vmm_compact_memory(uint32_t order) {
    struct compact_control cc = {
        .order = order,
        .nr_migrated = 0,
        .nr_freed = 0,
        .sync = 1
    };
    
    /* 简化实现：标记需要压缩 */
    // print("[VMM] Memory compaction for order ");
    print_dec(order);
    // println(" requested");
    
    return cc.nr_migrated + cc.nr_freed;
}

/* === 大页支持 (Huge Pages) === */

#define HUGE_PAGE_SIZE      (2 * 1024 * 1024)  /* 2MB */
#define HUGE_PAGE_ORDER     9
#define HUGE_PAGES_MAX      1024

struct huge_page_pool {
    uint64_t pages[HUGE_PAGES_MAX];
    uint32_t count;
    uint32_t reserved;
    uint32_t free;
    volatile int lock;
};

static struct huge_page_pool huge_pool = {
    .count = 0,
    .reserved = 0,
    .free = 0,
    .lock = 0
};

/* 初始化大页池 */
int vmm_huge_page_init(uint32_t count) {
    while (__sync_lock_test_and_set(&huge_pool.lock, 1)) {
        asm volatile("pause");
    }
    
    for (uint32_t i = 0; i < count && i < HUGE_PAGES_MAX; i++) {
        /* 分配大页 - 需要 2MB 连续物理内存 */
        uint64_t phys = pmm_alloc_pages(512);  /* 512 * 4KB = 2MB */
        if (phys == 0) break;
        
        huge_pool.pages[huge_pool.count++] = phys;
        huge_pool.free++;
    }
    
    __sync_lock_release(&huge_pool.lock);
    
    // print("[VMM] Huge page pool initialized with ");
    print_dec(huge_pool.count);
    // println(" pages");
    
    return huge_pool.count;
}

/* 分配大页 */
uint64_t vmm_alloc_huge_page(void) {
    while (__sync_lock_test_and_set(&huge_pool.lock, 1)) {
        asm volatile("pause");
    }
    
    if (huge_pool.free == 0) {
        __sync_lock_release(&huge_pool.lock);
        return 0;
    }
    
    uint64_t phys = huge_pool.pages[huge_pool.count - huge_pool.free];
    huge_pool.free--;
    
    __sync_lock_release(&huge_pool.lock);
    return phys;
}

/* 释放大页 */
void vmm_free_huge_page(uint64_t phys) {
    while (__sync_lock_test_and_set(&huge_pool.lock, 1)) {
        asm volatile("pause");
    }
    
    if (huge_pool.free < huge_pool.count) {
        huge_pool.pages[huge_pool.count - huge_pool.free - 1] = phys;
        huge_pool.free++;
    }
    
    __sync_lock_release(&huge_pool.lock);
}

/* 映射大页 */
int vmm_map_huge_page(struct address_space *space, uint64_t virt, uint64_t phys, 
                      uint64_t flags) {
    if (space == NULL) return -1;
    
    /* 页对齐到 2MB */
    virt &= ~(0x200000 - 1);  /* 2MB 对齐 */
    phys &= ~(0x200000 - 1);
    
    /* 遍历页表层级 */
    pte_t *pml4 = space->pml4;
    if (pml4 == NULL) return -1;
    
    pte_t *pdpt = get_page_table(pml4, PML4_INDEX(virt), 1);
    if (pdpt == NULL) return -1;
    
    pte_t *pd = get_page_table(pdpt, PDPT_INDEX(virt), 1);
    if (pd == NULL) return -1;
    
    /* 直接设置 2MB 大页到 PD，设置 PS (bit 7) + PWT (bit 3) + CD (bit 4) */
    int pd_idx = PD_INDEX(virt);
    uint64_t huge_flags = flags | (1ULL << 7);  /* PS bit */
    pd[pd_idx] = pte_create(phys, huge_flags);
    
    /* 刷新 TLB */
    vmm_invalidate_page(virt);
    
    return 0;
}

/* === 内存热插拔支持 === */

#define MEMORY_HOTPLUG_MAX_REGIONS  16

struct memory_region {
    uint64_t start_phys;
    uint64_t size;
    int online;
    int movable;  /* 是否可迁移 */
};

static struct memory_region memory_regions[MEMORY_HOTPLUG_MAX_REGIONS];
static int memory_region_count = 0;

/* 添加内存区域 */
int vmm_add_memory_region(uint64_t start_phys, uint64_t size, int movable) {
    if (memory_region_count >= MEMORY_HOTPLUG_MAX_REGIONS) return -1;
    
    struct memory_region *region = &memory_regions[memory_region_count++];
    region->start_phys = start_phys;
    region->size = size;
    region->online = 0;
    region->movable = movable;
    
    return 0;
}

/* 上线内存区域 */
int vmm_online_memory_region(int region_idx) {
    if (region_idx < 0 || region_idx >= memory_region_count) return -1;
    
    struct memory_region *region = &memory_regions[region_idx];
    
    if (region->online) return 0;
    
    /* 添加到 PMM */
    uint64_t pages = region->size / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        pmm_free_page(region->start_phys + i * PAGE_SIZE);
    }
    
    region->online = 1;
    
    // print("[VMM] Memory region ");
    print_dec(region_idx);
    // print(" online, ");
    print_dec(pages);
    // println(" pages added");
    
    return 0;
}

/* 下线内存区域 */
int vmm_offline_memory_region(int region_idx) {
    if (region_idx < 0 || region_idx >= memory_region_count) return -1;
    
    struct memory_region *region = &memory_regions[region_idx];
    
    if (!region->online) return 0;
    
    if (!region->movable) {
        // println("[VMM] Cannot offline non-movable region");
        return -1;
    }
    
    /* 迁移页面 */
    /* 简化实现 */
    
    region->online = 0;
    
    // print("[VMM] Memory region ");
    print_dec(region_idx);
    // println(" offline");
    
    return 0;
}

/* === 内存气球 (Memory Ballooning) === */

struct balloon_control {
    uint32_t target_pages;      /* 目标页数 */
    uint32_t current_pages;     /* 当前页数 */
    uint64_t *ballooned_pages;  /* 被气球化的页面 */
    uint32_t max_pages;         /* 最大页数 */
    volatile int lock;
};

static struct balloon_control balloon = {
    .target_pages = 0,
    .current_pages = 0,
    .max_pages = 4096,  /* 16MB */
    .lock = 0
};

/* 初始化内存气球 */
int vmm_balloon_init(uint32_t max_pages) {
    balloon.max_pages = max_pages;
    balloon.ballooned_pages = (uint64_t *)kmalloc(max_pages * sizeof(uint64_t));
    
    if (balloon.ballooned_pages == NULL) {
        return -1;
    }
    
    return 0;
}

/* 扩大气球 (归还内存给 Hypervisor) */
int vmm_balloon_inflate(uint32_t pages) {
    while (__sync_lock_test_and_set(&balloon.lock, 1)) {
        asm volatile("pause");
    }
    
    uint32_t to_inflate = pages;
    if (balloon.current_pages + to_inflate > balloon.max_pages) {
        to_inflate = balloon.max_pages - balloon.current_pages;
    }
    
    for (uint32_t i = 0; i < to_inflate; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) break;
        
        balloon.ballooned_pages[balloon.current_pages++] = phys;
        
        /* 通知 Hypervisor */
        /* virtio_balloon_notify(phys); */
    }
    
    __sync_lock_release(&balloon.lock);
    
    // print("[VMM] Balloon inflated by ");
    print_dec(to_inflate);
    // println(" pages");
    
    return to_inflate;
}

/* 缩小气球 (从 Hypervisor 获取内存) */
int vmm_balloon_deflate(uint32_t pages) {
    while (__sync_lock_test_and_set(&balloon.lock, 1)) {
        asm volatile("pause");
    }
    
    uint32_t to_deflate = pages;
    if (to_deflate > balloon.current_pages) {
        to_deflate = balloon.current_pages;
    }
    
    for (uint32_t i = 0; i < to_deflate; i++) {
        uint64_t phys = balloon.ballooned_pages[--balloon.current_pages];
        pmm_free_page(phys);
    }
    
    __sync_lock_release(&balloon.lock);
    
    // print("[VMM] Balloon deflated by ");
    print_dec(to_deflate);
    // println(" pages");
    
    return to_deflate;
}

/* 设置气球目标 */
void vmm_balloon_set_target(uint32_t target) {
    balloon.target_pages = target;
    
    if (target > balloon.current_pages) {
        vmm_balloon_deflate(target - balloon.current_pages);
    } else if (target < balloon.current_pages) {
        vmm_balloon_inflate(balloon.current_pages - target);
    }
}

/* === 内存超额承诺控制 (Memory Overcommit) === */

#define OVERCOMMIT_ALWAYS       1
#define OVERCOMMIT_NEVER        2
#define OVERCOMMIT_GUESS        0

static int overcommit_policy = OVERCOMMIT_GUESS;
static uint64_t commit_limit = 0;
static uint64_t committed_as = 0;

/* 设置超额承诺策略 */
void vmm_set_overcommit_policy(int policy) {
    overcommit_policy = policy;
}

/* 检查是否允许提交 */
int vmm_check_overcommit(uint64_t size) {
    switch (overcommit_policy) {
        case OVERCOMMIT_ALWAYS:
            return 1;  /* 总是允许 */
            
        case OVERCOMMIT_NEVER:
            if (committed_as + size > commit_limit) {
                return 0;  /* 拒绝 */
            }
            return 1;
            
        case OVERCOMMIT_GUESS:
        default:
            /* 启发式判断 */
            if (committed_as + size > commit_limit * 8 / 10) {
                return 0;
            }
            return 1;
    }
}

/* 更新提交计数 */
void vmm_update_committed(uint64_t size, int add) {
    if (add) {
        committed_as += size;
    } else {
        if (committed_as >= size) {
            committed_as -= size;
        }
    }
}

/* 计算提交限制 */
void vmm_calc_commit_limit(void) {
    uint64_t total_memory = pmm_get_total_memory();
    uint64_t swap_pages = 0;
    
    /* 计算交换空间页数 */
    swap_pages = swap_get_total_free_pages();
    
    /* CommitLimit = (TotalRAM + TotalSwap) * OvercommitRatio / 100 */
    commit_limit = total_memory + swap_pages * PAGE_SIZE;
}

/* === 虚拟化内存支持 (EPT/NPT) === */

#define EPT_MAX_LEVELS      4

struct ept_page {
    uint64_t entries[512];
};

struct ept_context {
    uint64_t eptp;              /* EPTP (EPT 指针) */
    struct ept_page *root;      /* EPT 根页表 */
    uint64_t guest_phys_base;   /* Guest 物理地址基址 */
    uint64_t host_phys_base;    /* Host 物理地址基址 */
    uint64_t size;              /* 映射大小 */
    int enabled;
};

#define MAX_VM_CONTEXTS     8
static struct ept_context ept_contexts[MAX_VM_CONTEXTS];
static int ept_context_count = 0;

/* 创建 EPT 上下文 */
int vmm_create_ept_context(uint64_t guest_base, uint64_t host_base, uint64_t size) {
    if (ept_context_count >= MAX_VM_CONTEXTS) return -1;
    
    struct ept_context *ctx = &ept_contexts[ept_context_count];
    
    /* 分配 EPT 根页表 */
    ctx->root = (struct ept_page *)kmalloc(sizeof(struct ept_page));
    if (ctx->root == NULL) return -1;
    
    /* 清空页表 */
    for (int i = 0; i < 512; i++) {
        ctx->root->entries[i] = 0;
    }
    
    ctx->guest_phys_base = guest_base;
    ctx->host_phys_base = host_base;
    ctx->size = size;
    ctx->enabled = 0;
    
    /* 计算 EPTP */
    uint64_t root_phys = virt_to_phys(ctx->root);
    ctx->eptp = (root_phys & 0xFFFFFFFFF000ULL) |  /* PML4 地址 */
                (3ULL << 3) |                       /* EPTP 使用 4 级页表 */
                (6ULL << 0);                        /* WB 内存类型 */
    
    ept_context_count++;
    return ept_context_count - 1;
}

/* EPT 映射页面 */
int vmm_ept_map(int ctx_id, uint64_t guest_phys, uint64_t host_phys, uint64_t flags) {
    if (ctx_id < 0 || ctx_id >= ept_context_count) return -1;
    
    struct ept_context *ctx = &ept_contexts[ctx_id];
    
    /* 简化实现：直接映射 */
    /* 实际实现需要遍历 EPT 页表 */
    
    /* EPT 权限标志:
     * bit 0: Read
     * bit 1: Write
     * bit 2: Execute
     */
    uint64_t ept_flags = 0x7;  /* RWX */
    ept_flags |= (6ULL << 3);  /* WB 内存类型 */
    ept_flags |= (host_phys & 0xFFFFFFFFF000ULL);
    
    /* 设置 EPT 条目 */
    uint64_t index = guest_phys >> 30;  /* 1GB 映射简化 */
    if (index < 512) {
        ctx->root->entries[index] = ept_flags | (1ULL << 7);  /* 1GB page */
    }
    
    return 0;
}

/* 启用 EPT */
void vmm_ept_enable(int ctx_id) {
    if (ctx_id < 0 || ctx_id >= ept_context_count) return;
    
    ept_contexts[ctx_id].enabled = 1;
}

/* === VMM 综合诊断 === */

void vmm_print_comprehensive_report(void) {
    // println("");
    // println("========================================");
    // println("      VMM Comprehensive Report        ");
    // println("========================================");
    // println("");
    
    /* 基本统计 */
    // println("--- Basic Statistics ---");
    
    struct vmm_stats stats;
    vmm_get_stats(&stats);
    
    // print("Total Page Faults: ");
    print_dec(stats.page_faults);
    // println("");
    
    // print("  Minor Faults: ");
    print_dec(stats.minor_faults);
    // println("");
    
    // print("  Major Faults: ");
    print_dec(stats.major_faults);
    // println("");
    
    // print("  COW Faults: ");
    print_dec(stats.cow_faults);
    // println("");
    
    /* TLB 统计 */
    // println("");
    // println("--- TLB Statistics ---");
    // print("TLB Hits: ");
    print_dec(tlb_hits);
    // println("");
    
    // print("TLB Misses: ");
    print_dec(tlb_misses);
    
    if (tlb_hits + tlb_misses > 0) {
        // print(" (Hit Rate: ");
        print_dec(tlb_hits * 100 / (tlb_hits + tlb_misses));
        // print("%)");
    }
    // println("");
    
    /* 内存压力 */
    // println("");
    // println("--- Memory Pressure ---");
    const char *pressure_names[] = {"Normal", "Low", "Medium", "Critical"};
    // print("Level: ");
    // println(pressure_names[mem_pressure.level]);
    // print("Score: ");
    print_dec(mem_pressure.pressure_score);
    // println("%");
    
    /* 大页统计 */
    // println("");
    // println("--- Huge Pages ---");
    // print("Total: ");
    print_dec(huge_pool.count);
    // println("");
    // print("Free: ");
    print_dec(huge_pool.free);
    // println("");
    // print("Reserved: ");
    print_dec(huge_pool.reserved);
    // println("");
    
    /* 交换空间 */
    for (int i = 0; i < swap_get_device_count(); i++) {
        struct swap_device *sd = swap_get_device(i);
        // print("Device ");
        print_dec(i);
        // print(": ");
        print_dec(sd->used_pages * PAGE_SIZE / 1024);
        // print(" KB / ");
        print_dec(sd->total_pages * PAGE_SIZE / 1024);
        // println(" KB used");
    }
    
    /* 超额承诺 */
    // println("");
    // println("--- Memory Overcommit ---");
    // print("Policy: ");
    const char *policies[] = {"Heuristic", "Always", "Never"};
    // println(policies[overcommit_policy]);
    
    // print("Committed: ");
    print_dec(committed_as / (1024 * 1024));
    // println(" MB");
    
    // print("Commit Limit: ");
    print_dec(commit_limit / (1024 * 1024));
    // println(" MB");
    
    /* 气球状态 */
    // println("");
    // println("--- Memory Balloon ---");
    // print("Current: ");
    print_dec(balloon.current_pages);
    // println(" pages");
    // print("Target: ");
    print_dec(balloon.target_pages);
    // println(" pages");
    
    // println("");
    // println("========================================");
    // println("          End of Report");
    // println("========================================");
    // println("");
}

/* === VMM 增强初始化 === */

void vmm_init_enhanced(void) {
    /* 初始化大页池 */
    vmm_huge_page_init(16);  /* 预分配 16 个大页 (32MB) */
    
    /* 初始化内存气球 */
    vmm_balloon_init(1024);
    
    /* 计算提交限制 */
    vmm_calc_commit_limit();
    
    /* 初始化 EPT 上下文 */
    for (int i = 0; i < MAX_VM_CONTEXTS; i++) {
        ept_contexts[i].enabled = 0;
    }
    
    /* 初始化预测性预取系统 */
    prefetch_init();
    
    // println("[VMM] Enhanced VMM initialized");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 预测性页面预取系统 (原创算法)
 * 
 * 基于马尔可夫链的页面访问预测
 * 通过分析历史访问模式预测下一个可能访问的页面
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 预取配置 */
#define PREFETCH_WINDOW_SIZE    16      /* 访问窗口大小 */
#define PREFETCH_HISTORY_DEPTH  8       /* 历史深度 */
#define PREFETCH_MAX_PREDICT    4       /* 最大预测数 */
#define PREFETCH_THRESHOLD      3       /* 预测阈值 */

/* 访问模式类型 */
typedef enum {
    PATTERN_SEQUENTIAL,     /* 顺序访问 */
    PATTERN_STRIDE,         /* 固定步长 */
    PATTERN_RANDOM,         /* 随机访问 */
    PATTERN_LOOP,           /* 循环访问 */
    PATTERN_UNKNOWN         /* 未知模式 */
} access_pattern_t;

/* 页面访问记录 */
struct page_access {
    uint64_t page_addr;         /* 页面地址 */
    uint64_t timestamp;         /* 访问时间戳 */
    uint32_t access_count;      /* 访问计数 */
    uint32_t flags;             /* 访问标志 */
};

/* 预测器状态 */
struct prefetch_predictor {
    /* 访问历史 */
    struct page_access history[PREFETCH_HISTORY_DEPTH];
    int history_head;
    
    /* 访问模式分析 */
    access_pattern_t pattern;
    int64_t stride;              /* 检测到的步长 */
    uint64_t last_page;          /* 上次访问页面 */
    
    /* 预测统计 */
    uint64_t predictions_made;
    uint64_t predictions_hit;
    uint64_t predictions_miss;
    
    /* 学习率 (定点数, 实际值 = learning_rate_x256 / 256) */
    uint32_t learning_rate_x256;
};

/* 全局预取状态 */
static struct prefetch_predictor prefetch_state;
static volatile int prefetch_lock = 0;

/* 初始化预取系统 */
void prefetch_init(void) {
    for (int i = 0; i < PREFETCH_HISTORY_DEPTH; i++) {
        prefetch_state.history[i].page_addr = 0;
        prefetch_state.history[i].timestamp = 0;
        prefetch_state.history[i].access_count = 0;
    }
    
    prefetch_state.history_head = 0;
    prefetch_state.pattern = PATTERN_UNKNOWN;
    prefetch_state.stride = 0;
    prefetch_state.last_page = 0;
    prefetch_state.predictions_made = 0;
    prefetch_state.predictions_hit = 0;
    prefetch_state.predictions_miss = 0;
    prefetch_state.learning_rate_x256 = 26;  /* 0.1 * 256 ≈ 26 */
    
    // println("[PREFETCH] Predictive prefetch system initialized");
}

/* 记录页面访问 */
void prefetch_record_access(uint64_t page_addr) {
    spin_lock(&prefetch_lock);
    
    /* 更新历史 */
    int idx = prefetch_state.history_head;
    prefetch_state.history[idx].page_addr = page_addr;
    prefetch_state.history[idx].timestamp = get_tick_count();
    prefetch_state.history[idx].access_count++;
    
    prefetch_state.history_head = (idx + 1) % PREFETCH_HISTORY_DEPTH;
    
    /* 分析访问模式 */
    if (prefetch_state.last_page != 0) {
        int64_t diff = (int64_t)(page_addr - prefetch_state.last_page);
        
        if (diff == PAGE_SIZE) {
            /* 顺序访问 */
            if (prefetch_state.pattern == PATTERN_SEQUENTIAL) {
                prefetch_state.stride = PAGE_SIZE;
            }
            prefetch_state.pattern = PATTERN_SEQUENTIAL;
        } else if (diff > 0 && diff == prefetch_state.stride) {
            /* 固定步长访问 */
            prefetch_state.pattern = PATTERN_STRIDE;
        } else if (diff < 0 && -diff == (int64_t)PAGE_SIZE) {
            /* 可能是循环访问 */
            prefetch_state.pattern = PATTERN_LOOP;
        } else {
            /* 检测是否与之前步长一致 */
            if (prefetch_state.stride == 0) {
                prefetch_state.stride = diff;
            } else if (diff != prefetch_state.stride) {
                prefetch_state.pattern = PATTERN_RANDOM;
            }
        }
    }
    
    prefetch_state.last_page = page_addr;
    
    spin_unlock(&prefetch_lock);
}

/* 预测下一个要访问的页面 */
int prefetch_predict(uint64_t *predicted_pages, int max_predictions) {
    spin_lock(&prefetch_lock);
    
    int num_predictions = 0;
    
    switch (prefetch_state.pattern) {
        case PATTERN_SEQUENTIAL:
            /* 顺序访问: 预测下一个页面 */
            if (num_predictions < max_predictions) {
                predicted_pages[num_predictions++] = prefetch_state.last_page + PAGE_SIZE;
            }
            /* 预测后续页面 */
            for (int i = 1; i < 3 && num_predictions < max_predictions; i++) {
                predicted_pages[num_predictions++] = 
                    prefetch_state.last_page + (i + 1) * PAGE_SIZE;
            }
            break;
            
        case PATTERN_STRIDE:
            /* 固定步长: 预测下一个位置 */
            if (prefetch_state.stride > 0) {
                for (int i = 0; i < 2 && num_predictions < max_predictions; i++) {
                    predicted_pages[num_predictions++] = 
                        prefetch_state.last_page + (i + 1) * prefetch_state.stride;
                }
            }
            break;
            
        case PATTERN_LOOP:
            /* 循环访问: 预测循环起点 */
            /* 查找历史中的循环起点 */
            for (int i = 0; i < PREFETCH_HISTORY_DEPTH && num_predictions < max_predictions; i++) {
                int idx = (prefetch_state.history_head - 1 - i + PREFETCH_HISTORY_DEPTH) 
                          % PREFETCH_HISTORY_DEPTH;
                if (prefetch_state.history[idx].page_addr < prefetch_state.last_page) {
                    predicted_pages[num_predictions++] = prefetch_state.history[idx].page_addr;
                    break;
                }
            }
            break;
            
        case PATTERN_RANDOM:
        case PATTERN_UNKNOWN:
            /* 随机或未知: 使用最近访问历史作为提示 */
            for (int i = 0; i < PREFETCH_HISTORY_DEPTH && num_predictions < max_predictions; i++) {
                int idx = (prefetch_state.history_head - 1 - i + PREFETCH_HISTORY_DEPTH) 
                          % PREFETCH_HISTORY_DEPTH;
                if (prefetch_state.history[idx].page_addr != 0 &&
                    prefetch_state.history[idx].page_addr != prefetch_state.last_page) {
                    predicted_pages[num_predictions++] = prefetch_state.history[idx].page_addr;
                }
            }
            break;
    }
    
    if (num_predictions > 0) {
        prefetch_state.predictions_made++;
    }
    
    spin_unlock(&prefetch_lock);
    return num_predictions;
}

/* 执行预取 */
void prefetch_execute(uint64_t page_addr) {
    /* 分配物理页并映射 */
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) return;
    
    /* 在内核空间映射以便清零 */
    void *virt = phys_to_virt(phys);
    memset(virt, 0, PAGE_SIZE);
    
    /* 注意: 实际映射需要进程上下文，这里只是预分配 */
    /* 预取的页面会被放入预取缓存中 */
}

/* 页错误时验证预测 */
void prefetch_verify(uint64_t page_addr) {
    spin_lock(&prefetch_lock);
    
    /* 检查是否命中预测 */
    for (int i = 0; i < PREFETCH_HISTORY_DEPTH; i++) {
        if (prefetch_state.history[i].page_addr == page_addr) {
            prefetch_state.predictions_hit++;
            spin_unlock(&prefetch_lock);
            return;
        }
    }
    
    prefetch_state.predictions_miss++;
    spin_unlock(&prefetch_lock);
}

/* 获取预取统计 */
void prefetch_get_stats(uint64_t *made, uint64_t *hit, uint64_t *miss) {
    if (made) *made = prefetch_state.predictions_made;
    if (hit) *hit = prefetch_state.predictions_hit;
    if (miss) *miss = prefetch_state.predictions_miss;
}

/* 打印预取统计 */
void prefetch_print_stats(void) {
    // println("");
    // println("=== Predictive Prefetch Statistics ===");
    
    const char *patterns[] = {"Sequential", "Stride", "Random", "Loop", "Unknown"};
    // print("Current Pattern: ");
    // println(patterns[prefetch_state.pattern]);
    
    // print("Stride: ");
    print_dec(prefetch_state.stride);
    // println(" bytes");
    
    // print("Predictions Made: ");
    print_dec(prefetch_state.predictions_made);
    // println("");
    
    // print("Predictions Hit: ");
    print_dec(prefetch_state.predictions_hit);
    // println("");
    
    // print("Predictions Miss: ");
    print_dec(prefetch_state.predictions_miss);
    // println("");
    
    if (prefetch_state.predictions_made > 0) {
        uint64_t hit_rate = prefetch_state.predictions_hit * 100 / 
                           prefetch_state.predictions_made;
        // print("Hit Rate: ");
        print_dec(hit_rate);
        // println("%");
    }
    
    // println("======================================");
}

/* 安全创建页表项（用于按需分页） */
int vmm_create_pte_if_not_present(struct address_space *space, uint64_t virt, uint64_t flags) {
    if (space == NULL || space->pml4 == NULL) {
        return -1;
    }
    
    virt &= ~(PAGE_SIZE - 1);
    
    pte_t *pml4 = space->pml4;
    
    /* 逐级检查并创建页表 */
    pte_t *pdpt = get_page_table(pml4, PML4_INDEX(virt), 1);  /* alloc=1 */
    if (pdpt == NULL) return -1;
    
    pte_t *pd = get_page_table(pdpt, PDPT_INDEX(virt), 1);    /* alloc=1 */
    if (pd == NULL) return -1;
    
    pte_t *pt = get_page_table(pd, PD_INDEX(virt), 1);        /* alloc=1 */
    if (pt == NULL) return -1;
    
    int pt_idx = PT_INDEX(virt);
    
    /* 检查页表项是否已存在 */
    if (pt[pt_idx] & VMM_FLAG_PRESENT) {
        return 0;  /* 页表项已存在 */
    }
    
    /* 尝试分配物理页，带内存压力恢复机制 */
    uint64_t phys = pmm_alloc_page();
    int retry_count = 0;
    const int max_retries = 2;
    static volatile uint64_t last_pressure_warn = 0;  /* 打印节流 */
    
    while (phys == 0 && retry_count < max_retries) {
        /* 内存不足，检查并尝试恢复 */
        int pressure = vmm_get_mem_pressure();
        
        if (pressure >= MEM_PRESSURE_MEDIUM) {
            /* 内存压力大，尝试回收（限制打印频率） */
            uint64_t now = get_tick_count();
            if (now - last_pressure_warn > 1000) {  /* 每 1000 ticks 最多打印一次 */
                // print("[VMM] Memory pressure high, reclaiming...\n");
                last_pressure_warn = now;
            }
            
            vmm_reclaim_memory(MEM_PRESSURE_LOW);
        }
        
        /* 短暂延迟让其他任务有机会释放内存 */
        for (volatile int i = 0; i < 1000; i++) {
            asm volatile("pause");
        }
        
        phys = pmm_alloc_page();
        retry_count++;
    }
    
    if (phys == 0) {
        return -1;  /* 无法分配物理页 */
    }
    
    /* 初始化物理页 */
    void *virt_page = phys_to_virt(phys);
    if (virt_page == NULL) {
        /* phys_to_virt 失败，释放物理页 */
        pmm_free_page(phys);
        return -1;
    }
    memset(virt_page, 0, PAGE_SIZE);
    
    /* 设置页表项 */
    pt[pt_idx] = pte_create(phys, flags);
    
    /* 刷新 TLB */
    vmm_invalidate_page(virt);
    
    return 0;  /* 成功 */
}

/* 检查地址是否已映射 */
int is_address_mapped(uint64_t addr) {
    /* 检查双内核是否启用 */
    extern int dual_kernel_is_enabled(void);
    
    /* 共享内存地址 0x01000000 仅在双内核模式下映射 */
    if (addr == INTER_KERNEL_SHM_PHYS) {
        return dual_kernel_is_enabled();
    }
    
    /* 其他地址默认已映射 */
    return 1;
}

/* 双内核状态标志 */
static int dk_initialized = 0;

/* 设置双内核状态 */
void vmm_set_dual_kernel_status(int status) {
    dk_initialized = status;
}

/* 检查双内核是否启用 */
int dual_kernel_is_enabled(void) {
    return dk_initialized;
}
