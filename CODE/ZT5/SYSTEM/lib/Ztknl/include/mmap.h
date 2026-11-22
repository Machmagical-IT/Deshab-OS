/*
 * Zratro OS - Memory Mapping (mmap) Support
 * 
 * 内存映射子系统，支持文件映射和匿名映射
 */

#ifndef MMAP_H
#define MMAP_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "vmm.h"

/* 文件偏移类型 */
typedef int64_t off_t;

/* ========== mmap 标志 ========== */

/* 保护标志 */
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

/* 映射标志 */
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_GROWSDOWN   0x0100
#define MAP_DENYWRITE   0x0800
#define MAP_EXECUTABLE  0x1000
#define MAP_LOCKED      0x2000
#define MAP_NORESERVE   0x4000
#define MAP_POPULATE    0x8000
#define MAP_NONBLOCK    0x10000
#define MAP_STACK       0x20000
#define MAP_HUGETLB     0x40000

/* 解除映射标志 */
#define MCL_CURRENT     1
#define MCL_FUTURE      2

/* 同步标志 */
#define MS_ASYNC        1
#define MS_INVALIDATE   2
#define MS_SYNC         4

/* madvise 建议 */
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_REMOVE     9
#define MADV_DONTFORK   10
#define MADV_DOFORK     11
#define MADV_MERGEABLE  12
#define MADV_UNMERGEABLE 13
#define MADV_HUGEPAGE   14
#define MADV_NOHUGEPAGE 15

/* ========== VMA (虚拟内存区域) ========== */

struct vm_area_struct {
    /* 地址范围 */
    uint64_t vm_start;          /* 起始地址 */
    uint64_t vm_end;            /* 结束地址 */
    
    /* 标志 */
    uint64_t vm_flags;          /* VMA 标志 (VM_READ, VM_WRITE 等) */
    
    /* 所属地址空间 */
    struct address_space *vm_mm;
    
    /* 文件映射 */
    struct file *vm_file;       /* 映射的文件 */
    uint64_t vm_pgoff;          /* 文件偏移 (页为单位) */
    
    /* 操作 */
    struct vm_operations *vm_ops;
    
    /* 私有数据 */
    void *vm_private_data;
    
    /* 链表和红黑树 */
    struct list_head vm_list;   /* VMA 链表 */
    struct rb_node vm_rb;       /* 红黑树节点 */
    
    /* 共享映射 */
    struct vm_region *vm_region; /* 共享区域 */
    
    /* 统计 */
    uint64_t vm_page_prot;      /* 页保护标志 */
};

/* VMA 操作 */
struct vm_operations {
    void (*open)(struct vm_area_struct *vma);
    void (*close)(struct vm_area_struct *vma);
    int (*fault)(struct vm_area_struct *vma, uint64_t address);
    int (*page_mkwrite)(struct vm_area_struct *vma, uint64_t address);
    void (*access)(struct vm_area_struct *vma, uint64_t address, void *buf, size_t len, int write);
};

/* VMA 标志 */
#define VM_READ         0x00000001  /* 可读 */
#define VM_WRITE        0x00000002  /* 可写 */
#define VM_EXEC         0x00000004  /* 可执行 */
#define VM_SHARED       0x00000008  /* 共享映射 */
#define VM_MAYREAD      0x00000010  /* 可能可读 */
#define VM_MAYWRITE     0x00000020  /* 可能可写 */
#define VM_MAYEXEC      0x00000040  /* 可能可执行 */
#define VM_MAYSHARE     0x00000080  /* 可能共享 */
#define VM_GROWSDOWN    0x00000100  /* 向下增长 (栈) */
#define VM_UFFD_MISSING 0x00000200  /* userfaultfd 缺失页 */
#define VM_PFNMAP       0x00000400  /* 页帧号映射 */
#define VM_UFFD_WP      0x00001000  /* userfaultfd 写保护 */
#define VM_LOCKED       0x00002000  /* 锁定在内存 */
#define VM_IO           0x00004000  /* I/O 映射 */
#define VM_SEQ_READ     0x00008000  /* 顺序读 */
#define VM_RAND_READ    0x00010000  /* 随机读 */
#define VM_DONTCOPY     0x00020000  /* fork 时不复制 */
#define VM_DONTEXPAND   0x00040000  /* 不可扩展 */
#define VM_LOCKONFAULT  0x00080000  /* 缺页时锁定 */
#define VM_ACCOUNT      0x00100000  /* 是否计费 */
#define VM_NORESERVE    0x00200000  /* 不预留交换空间 */
#define VM_HUGETLB      0x00400000  /* 大页 */
#define VM_SYNC         0x00800000  /* 同步写 */
#define VM_ARCH_1       0x01000000  /* 架构特定 */
#define VM_WIPEONFORK   0x02000000  /* fork 后清零 */
#define VM_DONTDUMP     0x04000000  /* 不包含在 core dump */

/* ========== 共享内存区域 ========== */

struct vm_region {
    uint64_t vm_start;
    uint64_t vm_end;
    uint64_t vm_flags;
    struct file *vm_file;
    uint64_t vm_pgoff;
    
    /* 引用计数 */
    atomic_t ref_count;
    
    /* 物理页 */
    uint64_t *pages;
    size_t page_count;
    
    spinlock_t lock;
    struct list_head list;
};

/* ========== mmap 管理器 ========== */

struct mmap_manager {
    /* VMA 红黑树 */
    struct rb_root vma_tree;
    
    /* VMA 链表 */
    struct list_head vma_list;
    uint32_t vma_count;
    
    /* 地址空间 */
    struct address_space *mm;
    
    /* 统计 */
    uint64_t total_vm;          /* 总虚拟页数 */
    uint64_t locked_vm;         /* 锁定页数 */
    uint64_t shared_vm;         /* 共享页数 */
    uint64_t exec_vm;           /* 可执行页数 */
    uint64_t stack_vm;          /* 栈页数 */
    
    /* 地址分配 */
    uint64_t mmap_base;         /* mmap 基地址 */
    uint64_t mmap_end;          /* mmap 结束地址 */
    uint64_t free_area_cache;   /* 空闲区域缓存 */
    
    /* 锁 */
    spinlock_t lock;
};

/* ========== 函数原型 ========== */

/* 初始化 */
int mmap_init(struct address_space *mm);
void mmap_exit(struct address_space *mm);

/* mmap 系统调用 */
void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int sys_munmap(void *addr, size_t length);

/* 内部实现 */
int do_munmap(struct address_space *mm, uint64_t start, size_t length);
void *do_mmap(struct address_space *mm, void *addr, size_t length, int prot, 
              int flags, int fd, off_t offset);

int sys_mprotect(void *addr, size_t len, int prot);
int sys_msync(void *addr, size_t length, int flags);
void *sys_mremap(void *old_address, size_t old_size, size_t new_size, int flags, void *new_address);
int sys_madvise(void *addr, size_t length, int advice);
int sys_mincore(void *addr, size_t length, unsigned char *vec);
int sys_mlock(const void *addr, size_t len);
int sys_munlock(const void *addr, size_t len);

/* VMA 操作 */
struct vm_area_struct *vma_create(uint64_t start, uint64_t end, uint64_t flags);
void vma_destroy(struct vm_area_struct *vma);
int vma_insert(struct address_space *mm, struct vm_area_struct *vma);
void vma_remove(struct vm_area_struct *vma);
struct vm_area_struct *vma_find(struct address_space *mm, uint64_t addr);
struct vm_area_struct *vma_find_intersect(struct address_space *mm, uint64_t start, uint64_t end);

/* VMA 合并 */
int vma_merge(struct address_space *mm, struct vm_area_struct *prev, uint64_t start,
              uint64_t end, uint64_t flags, struct file *file, uint64_t pgoff);

/* 地址分配 */
uint64_t mmap_find_vma(struct address_space *mm, uint64_t addr, size_t len);
uint64_t mmap_find_vma_unmapped(struct address_space *mm, uint64_t addr, size_t len);

/* 页错误处理 */
int mmap_handle_fault(struct vm_area_struct *vma, uint64_t address, uint32_t error_code);

/* 文件映射 */
int mmap_file(struct file *file, struct vm_area_struct *vma);
int munmap_file(struct vm_area_struct *vma);

/* 匿名映射 */
int mmap_anon(struct vm_area_struct *vma);

/* 共享映射 */
int mmap_shared(struct vm_area_struct *vma);

/* 辅助函数 */
static inline uint64_t vma_size(struct vm_area_struct *vma) {
    return vma->vm_end - vma->vm_start;
}

static inline int vma_contains(struct vm_area_struct *vma, uint64_t addr) {
    return addr >= vma->vm_start && addr < vma->vm_end;
}

static inline int vma_is_anonymous(struct vm_area_struct *vma) {
    return vma->vm_file == NULL;
}

/* 保护标志转换 */
static inline uint64_t prot_to_vm_flags(int prot) {
    uint64_t flags = 0;
    if (prot & PROT_READ)  flags |= VM_READ | VM_MAYREAD;
    if (prot & PROT_WRITE) flags |= VM_WRITE | VM_MAYWRITE;
    if (prot & PROT_EXEC)  flags |= VM_EXEC | VM_MAYEXEC;
    return flags;
}

static inline int vm_flags_to_prot(uint64_t vm_flags) {
    int prot = PROT_NONE;
    if (vm_flags & VM_READ)  prot |= PROT_READ;
    if (vm_flags & VM_WRITE) prot |= PROT_WRITE;
    if (vm_flags & VM_EXEC)  prot |= PROT_EXEC;
    return prot;
}

#endif /* MMAP_H */
