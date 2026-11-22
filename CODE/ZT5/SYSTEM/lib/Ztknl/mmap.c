/*
 * Zratro OS - Memory Mapping (mmap) Implementation
 * 
 * 内存映射子系统实现
 */

#include "mmap.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "cow.h"
#include "vfs.h"
#include "smp.h"
#include "cfs.h"

/* ========== 地址空间布局 ========== */

/* 用户空间布局 (x86-64) */
#define MMAP_MIN_ADDR       0x10000
#define MMAP_MAX_ADDR       0x7FFFFFFFFFFFULL
#define MMAP_BASE           0x100000000ULL   /* 4GB */
#define STACK_TOP           0x7FFFFFFFFFFFULL
#define STACK_SIZE          (8 * 1024 * 1024)  /* 8MB */

/* ========== VMA 管理 ========== */

/* 创建 VMA */
struct vm_area_struct *vma_create(uint64_t start, uint64_t end, uint64_t flags) {
    struct vm_area_struct *vma = kmalloc(sizeof(*vma));
    if (vma == NULL) return NULL;
    
    vma->vm_start = start;
    vma->vm_end = end;
    vma->vm_flags = flags;
    vma->vm_mm = NULL;
    vma->vm_file = NULL;
    vma->vm_pgoff = 0;
    vma->vm_ops = NULL;
    vma->vm_private_data = NULL;
    vma->vm_region = NULL;
    vma->vm_page_prot = VMM_FLAG_PRESENT;
    
    if (flags & VM_READ)  vma->vm_page_prot |= VMM_FLAG_USER;
    if (flags & VM_WRITE) vma->vm_page_prot |= VMM_FLAG_WRITABLE;
    if (!(flags & VM_EXEC)) vma->vm_page_prot |= VMM_FLAG_NO_EXECUTE;
    
    list_init(&vma->vm_list);
    
    return vma;
}

/* 销毁 VMA */
void vma_destroy(struct vm_area_struct *vma) {
    if (vma == NULL) return;
    
    /* 取消映射 */
    if (vma->vm_mm != NULL) {
        uint64_t addr;
        for (addr = vma->vm_start; addr < vma->vm_end; addr += 4096) {
            vmm_unmap_page(vma->vm_mm, addr);
        }
    }
    
    /* 释放文件引用 */
    if (vma->vm_file != NULL) {
        /* vfs_close(vma->vm_file); */
    }
    
    kfree(vma);
}

/* 查找 VMA */
struct vm_area_struct *vma_find(struct address_space *mm, uint64_t addr) {
    if (mm == NULL) return NULL;
    
    struct mmap_manager *mgr = mm->mmap_mgr;
    if (mgr == NULL) return NULL;
    
    /* 红黑树查找 */
    struct rb_node *node = mgr->vma_tree.rb_node;
    
    while (node != NULL) {
        struct vm_area_struct *vma = container_of(node, struct vm_area_struct, vm_rb);
        
        if (addr < vma->vm_start) {
            node = node->left;
        } else if (addr >= vma->vm_end) {
            node = node->right;
        } else {
            return vma;
        }
    }
    
    return NULL;
}

/* 插入 VMA */
int vma_insert(struct address_space *mm, struct vm_area_struct *vma) {
    if (mm == NULL || vma == NULL) return -1;
    
    struct mmap_manager *mgr = mm->mmap_mgr;
    if (mgr == NULL) return -1;
    
    vma->vm_mm = mm;
    
    /* 插入红黑树 */
    struct rb_node **link = &mgr->vma_tree.rb_node;
    struct rb_node *parent = NULL;
    
    while (*link != NULL) {
        struct vm_area_struct *tmp = container_of(*link, struct vm_area_struct, vm_rb);
        parent = *link;
        
        if (vma->vm_end <= tmp->vm_start) {
            link = &(*link)->left;
        } else if (vma->vm_start >= tmp->vm_end) {
            link = &(*link)->right;
        } else {
            /* 重叠 */
            return -1;
        }
    }
    
    vma->vm_rb.parent = parent;
    *link = &vma->vm_rb;
    
    /* 插入链表 */
    list_add(&vma->vm_list, &mgr->vma_list);
    mgr->vma_count++;
    
    /* 更新统计 */
    mgr->total_vm += (vma->vm_end - vma->vm_start) / 4096;
    if (vma->vm_flags & VM_LOCKED) {
        mgr->locked_vm += (vma->vm_end - vma->vm_start) / 4096;
    }
    if (vma->vm_flags & VM_EXEC) {
        mgr->exec_vm += (vma->vm_end - vma->vm_start) / 4096;
    }
    if (vma->vm_flags & VM_GROWSDOWN) {
        mgr->stack_vm += (vma->vm_end - vma->vm_start) / 4096;
    }
    
    return 0;
}

/* 移除 VMA */
void vma_remove(struct vm_area_struct *vma) {
    if (vma == NULL || vma->vm_mm == NULL) return;
    
    struct mmap_manager *mgr = vma->vm_mm->mmap_mgr;
    if (mgr == NULL) return;
    
    /* 从红黑树删除 */
    rb_erase(&mgr->vma_tree, &vma->vm_rb);
    
    /* 从链表删除 */
    list_del(&vma->vm_list);
    mgr->vma_count--;
    
    /* 更新统计 */
    mgr->total_vm -= (vma->vm_end - vma->vm_start) / 4096;
}

/* 合并相邻 VMA */
int vma_merge(struct address_space *mm, struct vm_area_struct *prev, uint64_t start,
              uint64_t end, uint64_t flags, struct file *file, uint64_t pgoff) {
    /* 检查是否可以与前一个 VMA 合并 */
    if (prev != NULL && prev->vm_end == start) {
        /* 检查标志和文件是否一致 */
        if (prev->vm_flags == flags && prev->vm_file == file) {
            if (file == NULL || prev->vm_pgoff + (prev->vm_end - prev->vm_start) / 4096 == pgoff) {
                prev->vm_end = end;
                return 0;
            }
        }
    }
    
    /* 检查是否可以与后一个 VMA 合并 */
    struct vm_area_struct *next = vma_find(mm, end);
    if (next != NULL && next->vm_start == end) {
        if (next->vm_flags == flags && next->vm_file == file) {
            if (file == NULL || pgoff + (end - start) / 4096 == next->vm_pgoff) {
                next->vm_start = start;
                next->vm_pgoff = pgoff;
                return 0;
            }
        }
    }
    
    return -1;  /* 无法合并 */
}

/* ========== 地址分配 ========== */

/* 查找空闲区域 */
uint64_t mmap_find_vma(struct address_space *mm, uint64_t addr, size_t len) {
    if (mm == NULL || len == 0) return 0;
    
    struct mmap_manager *mgr = mm->mmap_mgr;
    if (mgr == NULL) return 0;
    
    /* 对齐 */
    addr = (addr + 0xFFFULL) & ~0xFFFULL;
    len = (len + 0xFFFULL) & ~0xFFFULL;
    
    /* 检查范围 */
    if (addr < MMAP_MIN_ADDR) addr = MMAP_MIN_ADDR;
    if (addr + len > MMAP_MAX_ADDR) return 0;
    
    /* 从缓存位置开始查找 */
    uint64_t start = addr;
    if (start < mgr->free_area_cache) {
        start = mgr->free_area_cache;
    }
    
    /* 线性扫描查找空闲区域 */
    struct vm_area_struct *vma;
    uint64_t end = start + len;
    
    list_for_each_entry(vma, &mgr->vma_list, vm_list) {
        if (vma->vm_start >= end) {
            /* 找到足够的间隙 */
            mgr->free_area_cache = start;
            return start;
        }
        
        if (vma->vm_end > start) {
            /* 重叠，跳过 */
            start = vma->vm_end;
            end = start + len;
        }
    }
    
    /* 检查是否超出范围 */
    if (end > MMAP_MAX_ADDR) {
        return 0;
    }
    
    mgr->free_area_cache = start;
    return start;
}

/* ========== mmap 系统调用 ========== */

void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    struct address_space *mm = vmm_get_current_space();
    if (mm == NULL) return (void *)-1;
    
    struct mmap_manager *mgr = mm->mmap_mgr;
    if (mgr == NULL) return (void *)-1;
    
    /* 参数检查 */
    if (length == 0) return (void *)-1;
    
    length = (length + 0xFFFULL) & ~0xFFFULL;
    offset = (offset + 0xFFFULL) & ~0xFFFULL;
    
    /* 转换保护标志 */
    uint64_t vm_flags = prot_to_vm_flags(prot);
    
    /* 检查是否为共享映射 */
    if (flags & MAP_SHARED) {
        vm_flags |= VM_SHARED | VM_MAYSHARE;
    }
    
    /* 查找地址 */
    uint64_t vaddr;
    if (flags & MAP_FIXED) {
        vaddr = (uint64_t)addr;
        /* 取消现有映射 */
        do_munmap(mm, vaddr, length);
    } else {
        vaddr = mmap_find_vma(mm, (uint64_t)addr, length);
        if (vaddr == 0) return (void *)-1;
    }
    
    /* 创建 VMA */
    struct vm_area_struct *vma = vma_create(vaddr, vaddr + length, vm_flags);
    if (vma == NULL) return (void *)-1;
    
    vma->vm_pgoff = offset / 4096;
    
    /* 处理文件映射 */
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        struct file *file = NULL;  /* TODO: vfs_get_file(fd) */
        if (file == NULL) {
            kfree(vma);
            return (void *)-1;
        }
        vma->vm_file = file;
        
        /* 文件映射 */
        if (mmap_file(file, vma) != 0) {
            kfree(vma);
            return (void *)-1;
        }
    } else {
        /* 匿名映射 */
        if (mmap_anon(vma) != 0) {
            kfree(vma);
            return (void *)-1;
        }
    }
    
    /* 插入 VMA */
    if (vma_insert(mm, vma) != 0) {
        vma_destroy(vma);
        return (void *)-1;
    }
    
    return (void *)vaddr;
}

/* 内部 munmap 实现 */
int do_munmap(struct address_space *mm, uint64_t start, size_t length) {
    if (mm == NULL) return -1;
    
    /* 对齐 */
    start = start & ~0xFFFULL;
    length = (length + 0xFFFULL) & ~0xFFFULL;
    uint64_t end = start + length;
    
    /* 查找涉及的 VMA */
    struct vm_area_struct *vma;
    int unmapped = 0;
    
    while ((vma = vma_find(mm, start)) != NULL && vma->vm_start < end) {
        uint64_t vma_start = vma->vm_start;
        uint64_t vma_end = vma->vm_end;
        
        /* 计算实际取消映射的范围 */
        uint64_t unmap_start = (start > vma_start) ? start : vma_start;
        uint64_t unmap_end = (end < vma_end) ? end : vma_end;
        
        /* 取消页面映射 */
        for (uint64_t addr = unmap_start; addr < unmap_end; addr += 4096) {
            /* 获取物理页 */
            uint64_t phys = vmm_get_physical(mm, addr);
            if (phys != 0) {
                /* 取消映射 */
                vmm_unmap_page(mm, addr);
                /* 如果不是共享映射，释放物理页 */
                if (!(vma->vm_flags & VM_SHARED)) {
                    pmm_free_page(phys);
                }
            }
        }
        
        /* 处理 VMA 分割 */
        if (vma_start < start) {
            /* 前部保留 */
            vma->vm_end = start;
        } else if (vma_end > end) {
            /* 后部保留 */
            vma->vm_start = end;
            vma->vm_pgoff += (end - vma_start) / 4096;
        } else {
            /* 整个 VMA 被移除 */
            vma_remove(vma);
            vma_destroy(vma);
        }
        
        unmapped++;
    }
    
    return unmapped > 0 ? 0 : -1;
}

/* munmap 系统调用 */
int sys_munmap(void *addr, size_t length) {
    struct address_space *mm = vmm_get_current_space();
    if (mm == NULL) return -1;
    
    uint64_t start = (uint64_t)addr;
    
    /* 对齐检查 */
    if (start & 0xFFF) return -1;
    
    length = (length + 0xFFFULL) & ~0xFFFULL;
    uint64_t end = start + length;
    
    /* 查找并删除涉及的 VMA */
    struct vm_area_struct *vma;
    while ((vma = vma_find(mm, start)) != NULL && vma->vm_start < end) {
        if (vma->vm_start < start) {
            /* 分割 VMA (前部) */
            struct vm_area_struct *front = vma_create(vma->vm_start, start, vma->vm_flags);
            if (front != NULL) {
                vma_insert(mm, front);
            }
        }
        
        if (vma->vm_end > end) {
            /* 分割 VMA (后部) */
            struct vm_area_struct *back = vma_create(end, vma->vm_end, vma->vm_flags);
            if (back != NULL) {
                back->vm_file = vma->vm_file;
                back->vm_pgoff = vma->vm_pgoff + (end - vma->vm_start) / 4096;
                vma_insert(mm, back);
            }
        }
        
        vma_remove(vma);
        vma_destroy(vma);
    }
    
    return 0;
}

/* mprotect 系统调用 */
int sys_mprotect(void *addr, size_t len, int prot) {
    struct address_space *mm = vmm_get_current_space();
    if (mm == NULL) return -1;
    
    uint64_t start = (uint64_t)addr;
    if (start & 0xFFF) return -1;
    
    len = (len + 0xFFFULL) & ~0xFFFULL;
    uint64_t end = start + len;
    
    struct vm_area_struct *vma = vma_find(mm, start);
    if (vma == NULL) return -1;
    
    /* 检查整个范围 */
    while (start < end) {
        if (vma == NULL) return -1;
        
        uint64_t new_flags = vma->vm_flags;
        new_flags &= ~(VM_READ | VM_WRITE | VM_EXEC);
        new_flags |= prot_to_vm_flags(prot);
        
        vma->vm_flags = new_flags;
        
        start = vma->vm_end;
        vma = vma_find(mm, start);
    }
    
    return 0;
}

/* msync 系统调用 */
int sys_msync(void *addr, size_t length, int flags) {
    /* TODO: 实现文件同步 */
    (void)addr;
    (void)length;
    (void)flags;
    return 0;
}

/* madvise 系统调用 */
int sys_madvise(void *addr, size_t length, int advice) {
    struct address_space *mm = vmm_get_current_space();
    if (mm == NULL) return -1;
    
    struct vm_area_struct *vma = vma_find(mm, (uint64_t)addr);
    if (vma == NULL) return -1;
    
    switch (advice) {
        case MADV_NORMAL:
            vma->vm_flags &= ~(VM_SEQ_READ | VM_RAND_READ);
            break;
        case MADV_RANDOM:
            vma->vm_flags |= VM_RAND_READ;
            vma->vm_flags &= ~VM_SEQ_READ;
            break;
        case MADV_SEQUENTIAL:
            vma->vm_flags |= VM_SEQ_READ;
            vma->vm_flags &= ~VM_RAND_READ;
            break;
        case MADV_WILLNEED:
            /* TODO: 预读 */
            break;
        case MADV_DONTNEED:
            /* TODO: 释放页面 */
            break;
        default:
            return -1;
    }
    
    return 0;
}

/* ========== 文件映射 ========== */

int mmap_file(struct file *file, struct vm_area_struct *vma) {
    if (file == NULL || vma == NULL) return -1;
    
    /* 设置 VMA 操作 */
    vma->vm_ops = NULL;  /* 使用默认操作 */
    
    /* 获取文件大小 */
    /* TODO: 使用 vfs_stat 获取文件大小 */
    
    /* 按需分配页面 - 延迟分配 */
    /* 页面会在访问时通过缺页异常分配 */
    
    return 0;
}

int munmap_file(struct vm_area_struct *vma) {
    if (vma == NULL) return -1;
    
    /* 刷新脏页到文件 */
    if (vma->vm_file != NULL && (vma->vm_flags & VM_SHARED)) {
        /* 对于共享映射，写回脏页 */
        uint64_t addr;
        for (addr = vma->vm_start; addr < vma->vm_end; addr += 4096) {
            /* TODO: 检查页面是否脏，写回文件 */
        }
    }
    
    return 0;
}

/* ========== 匿名映射 ========== */

int mmap_anon(struct vm_area_struct *vma) {
    if (vma == NULL) return -1;
    
    /* 分配物理页 */
    uint64_t start = vma->vm_start;
    while (start < vma->vm_end) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) return -1;
        
        /* 映射页 */
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (vma->vm_flags & VM_WRITE) flags |= VMM_FLAG_WRITABLE;
        
        if (vmm_map_page(vma->vm_mm, start, phys, flags) != 0) {
            pmm_free_page(phys);
            return -1;
        }
        
        /* 清零 */
        void *virt = phys_to_virt(phys);
        memset(virt, 0, 4096);
        
        start += 4096;
    }
    
    return 0;
}

/* ========== 页错误处理 ========== */

int mmap_handle_fault(struct vm_area_struct *vma, uint64_t address, uint32_t error_code) {
    if (vma == NULL) return -1;
    
    /* 检查权限 */
    if ((error_code & 2) && !(vma->vm_flags & VM_WRITE)) {
        return -1;  /* 写保护违规 */
    }
    if (!(error_code & 2) && !(vma->vm_flags & VM_READ)) {
        return -1;  /* 读保护违规 */
    }
    if ((error_code & 16) && !(vma->vm_flags & VM_EXEC)) {
        return -1;  /* 执行保护违规 */
    }
    
    /* 获取页表项一次，供后续检查使用 */
    /* But verify that we're not causing issues by accessing invalid page tables */
    if (vma->vm_mm == NULL) {
        return -1; /* Invalid address space */
    }
    
    uint64_t *pte = vmm_get_pte(vma->vm_mm, address & ~0xFFFULL);
    
    /* 检查是否为 COW 页错误：页面存在但因为写保护而失败 */
    if ((error_code & 2) && pte != NULL && (*pte & VMM_FLAG_PRESENT) && !(*pte & VMM_FLAG_WRITABLE)) {  /* 写操作，页面存在，但不可写 */
        /* 尝试使用 COW 处理 */
        int cow_result = cow_handle_page_fault(address, error_code);
        if (cow_result == 0) {
            return 0;  /* COW 处理成功 */
        }
    }
    
    /* 检查虚拟地址是否已经映射但物理页不存在（按需分页） */
    if (pte != NULL && !(*pte & VMM_FLAG_PRESENT)) {  /* 页表项存在但页面不存在 */
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) return -1;
        
        /* 清零物理页 */
        void *virt = phys_to_virt(phys);
        if (virt != NULL) {
            memset(virt, 0, 4096);
        }
        
        /* 设置页表项，根据 VMA 标志设置权限 */
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (vma->vm_flags & VM_WRITE) flags |= VMM_FLAG_WRITABLE;
        
        *pte = phys | flags;
        vmm_invalidate_page(address & ~0xFFFULL);
        return 0;
    }
    
    /* 分配新页并映射 */
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) return -1;
    
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (vma->vm_flags & VM_WRITE) flags |= VMM_FLAG_WRITABLE;
    
    if (vmm_map_page(vma->vm_mm, address & ~0xFFFULL, phys, flags) != 0) {
        pmm_free_page(phys);
        return -1;
    }
    
    /* 清零 */
    void *virt = phys_to_virt(phys);
    if (virt != NULL) {
        memset(virt, 0, 4096);
    }
    
    return 0;
}

/* ========== 初始化 ========== */

int mmap_init(struct address_space *mm) {
    if (mm == NULL) return -1;
    
    struct mmap_manager *mgr = kmalloc(sizeof(*mgr));
    if (mgr == NULL) return -1;
    
    memset(mgr, 0, sizeof(*mgr));
    
    mgr->vma_tree.rb_node = NULL;
    list_init(&mgr->vma_list);
    mgr->vma_count = 0;
    mgr->mm = mm;
    mgr->mmap_base = MMAP_BASE;
    mgr->mmap_end = MMAP_MAX_ADDR;
    mgr->free_area_cache = MMAP_BASE;
    mgr->lock = SPINLOCK_INIT;
    
    mm->mmap_mgr = mgr;
    
    return 0;
}

void mmap_exit(struct address_space *mm) {
    if (mm == NULL || mm->mmap_mgr == NULL) return;
    
    struct mmap_manager *mgr = mm->mmap_mgr;
    
    /* 释放所有 VMA */
    struct vm_area_struct *vma, *tmp;
    list_for_each_entry_safe(vma, tmp, &mgr->vma_list, vm_list) {
        vma_destroy(vma);
    }
    
    kfree(mgr);
    mm->mmap_mgr = NULL;
}
