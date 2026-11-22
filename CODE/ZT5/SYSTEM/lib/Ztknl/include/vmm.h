#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include "cpu.h"

/* 页表层级 */
#define PT_LEVEL    0   /* Page Table (4KB pages) */
#define PD_LEVEL    1   /* Page Directory */
#define PDPT_LEVEL  2   /* Page Directory Pointer Table */
#define PML4_LEVEL  3   /* Page Map Level 4 */

/* 页表项标志位 */
#define VMM_FLAG_PRESENT      (1ULL << 0)
#define VMM_FLAG_WRITABLE     (1ULL << 1)
#define VMM_FLAG_USER         (1ULL << 2)
#define VMM_FLAG_WRITETHROUGH (1ULL << 3)
#define VMM_FLAG_CACHE_DISABLE (1ULL << 4)
#define VMM_FLAG_ACCESSED     (1ULL << 5)
#define VMM_FLAG_DIRTY        (1ULL << 6)
#define VMM_FLAG_HUGE         (1ULL << 7)   /* 2MB 或 1GB 页 */
#define VMM_FLAG_GLOBAL       (1ULL << 8)
#define VMM_FLAG_COW          (1ULL << 9)   /* Copy-on-Write 标记 */
#define VMM_FLAG_NX           (1ULL << 63)  /* No Execute */
#define VMM_FLAG_NO_EXECUTE   VMM_FLAG_NX   /* Alias */
#define VMM_FLAG_LOCKED       (1ULL << 10)  /* 锁定页面（不可换出） */
#define VMM_FLAG_SHARED       (1ULL << 11)  /* 共享页面 */

/* ═══ 原创优化：大页支持 ═══ */
#define PAGE_SIZE_4KB     4096ULL
#define PAGE_SIZE_2MB     (2ULL * 1024 * 1024)
#define PAGE_SIZE_1GB     (1ULL * 1024 * 1024 * 1024)

/* 大页对齐检查 */
#define IS_4KB_ALIGNED(addr)  (((addr) & 0xFFF) == 0)
#define IS_2MB_ALIGNED(addr)  (((addr) & 0x1FFFFF) == 0)
#define IS_1GB_ALIGNED(addr)  (((addr) & 0x3FFFFFFF) == 0)

/* 便捷标志组合 */
#define VMM_FLAGS_KERNEL      (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE)
#define VMM_FLAGS_KERNEL_RO   (VMM_FLAG_PRESENT)
#define VMM_FLAGS_USER        (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER)
#define VMM_FLAGS_USER_RO     (VMM_FLAG_PRESENT | VMM_FLAG_USER)
#define VMM_FLAGS_KERNEL_NX   (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_NX)
#define VMM_FLAGS_USER_NX     (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER | VMM_FLAG_NX)
#define VMM_FLAGS_KERNEL_RO_NX (VMM_FLAG_PRESENT | VMM_FLAG_NX)
#define VMM_FLAGS_MMIO        (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | \
                               VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITETHROUGH | VMM_FLAG_NX)

/* 页表项掩码 */
#define VMM_ADDR_MASK    0x000FFFFFFFFFF000ULL
#define VMM_FLAGS_MASK   0xFFF0000000000FFFULL

/* 虚拟地址范围 */
#define KERNEL_BASE      0xFFFFFFFF80000000ULL
#define KERNEL_END       0xFFFFFFFFFFFFFFFFULL
#define USER_BASE        0x0000000000400000ULL
#define USER_END         0x00007FFFFFFFFFFFULL

/* 地址转换宏 */
#define PML4_INDEX(addr) (((addr) >> 39) & 0x1FF)
#define PDPT_INDEX(addr) (((addr) >> 30) & 0x1FF)
#define PD_INDEX(addr)   (((addr) >> 21) & 0x1FF)
#define PT_INDEX(addr)   (((addr) >> 12) & 0x1FF)

/* 页表结构 */
typedef uint64_t pte_t;

/* 页表项操作 (内联函数) */
static inline uint64_t pte_get_addr(pte_t pte) {
    return pte & VMM_ADDR_MASK;
}

static inline uint64_t pte_get_flags(pte_t pte) {
    return pte & ~VMM_ADDR_MASK;
}

static inline pte_t pte_create(uint64_t phys, uint64_t flags) {
    return (phys & VMM_ADDR_MASK) | (flags & ~VMM_ADDR_MASK);
}

static inline int pte_is_present(pte_t pte) {
    return (pte & VMM_FLAG_PRESENT) != 0;
}

static inline int pte_is_writable(pte_t pte) {
    return (pte & VMM_FLAG_WRITABLE) != 0;
}

static inline int pte_is_user(pte_t pte) {
    return (pte & VMM_FLAG_USER) != 0;
}

static inline int pte_is_executable(pte_t pte) {
    return (pte & VMM_FLAG_NX) == 0;
}

/* 检查地址是否在内核空间 */
static inline int is_kernel_addr(uint64_t addr) {
    return addr >= KERNEL_BASE;
}

/* 检查地址是否在用户空间 */
static inline int is_user_addr(uint64_t addr) {
    return addr >= USER_BASE && addr <= USER_END;
}

/* 检查地址是否有效 */
static inline int is_valid_addr(uint64_t addr) {
    return is_kernel_addr(addr) || is_user_addr(addr);
}

/* 前向声明 */
struct mmap_manager;

/* 地址空间结构 */
struct address_space {
    pte_t *pml4;            /* PML4 物理地址 */
    uint64_t cr3;           /* CR3 寄存器值 */
    struct mmap_manager *mmap_mgr; /* 内存映射管理器 */
    uint64_t ref_count;     /* 引用计数 */
};

/* 初始化虚拟内存管理器 */
void vmm_init(void);

/* 创建新的地址空间 */
struct address_space *vmm_create_address_space(void);

/* 销毁地址空间 */
void vmm_destroy_address_space(struct address_space *space);

/* 获取内核地址空间 */
struct address_space *vmm_get_kernel_space(void);

/* 映射页面 */
int vmm_map_page(struct address_space *space, uint64_t virt, uint64_t phys, uint64_t flags);

/* 取消映射页面 */
int vmm_unmap_page(struct address_space *space, uint64_t virt);

/* 获取物理地址 */
uint64_t vmm_get_physical(struct address_space *space, uint64_t virt);

/* 映射一段内存区域 */
int vmm_map_range(struct address_space *space, uint64_t virt, uint64_t phys, 
                  size_t pages, uint64_t flags);

/* 取消映射一段内存区域 */
int vmm_unmap_range(struct address_space *space, uint64_t virt, size_t pages);

/* 切换地址空间 */
void vmm_switch_space(struct address_space *space);

/* 刷新 TLB */
void vmm_invalidate_page(uint64_t virt);
void vmm_invalidate_all(void);

/* TLB shootdown - 通知其他 CPU 刷新指定虚拟地址的 TLB */
void vmm_flush_tlb_others(uint64_t virt);

/* 获取当前 CR3 */
uint64_t vmm_get_cr3(void);

/* 获取当前地址空间 */
struct address_space *vmm_get_current_space(void);

/* 获取页表项 */
uint64_t *vmm_get_pte(struct address_space *space, uint64_t virt);

/* 设置页表项 */
int vmm_set_pte(struct address_space *space, uint64_t virt, uint64_t pte);

/* 修改页面权限 */
int vmm_protect(struct address_space *space, uint64_t virt, uint64_t new_flags);

/* 检查页面权限 */
int vmm_check_access(struct address_space *space, uint64_t virt, uint64_t flags);

/* 映射内核空间到用户地址空间 */
int vmm_map_kernel(struct address_space *user_space);

/* 统计地址空间内存使用 */
size_t vmm_count_mapped_pages(struct address_space *space);

/* ═══ 原创优化：大页映射函数 ═══ */

/* 映射 2MB 大页 */
int vmm_map_2mb_page(struct address_space *space, uint64_t virt, uint64_t phys, uint64_t flags);

/* 映射 1GB 大页 */
int vmm_map_1gb_page(struct address_space *space, uint64_t virt, uint64_t phys, uint64_t flags);

/* 智能大页映射 - 自动选择最优页面大小 */
int vmm_map_smart(struct address_space *space, uint64_t virt, uint64_t phys, 
                  size_t size, uint64_t flags);

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全用户空间访问函数
 * 
 * 防止用户空间指针攻击
 * - 检查指针是否在用户空间
 * - 使用安全拷贝避免页面错误
 * - 支持 access_ok 验证
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 验证用户空间地址 */
static inline int access_ok(const void *addr, size_t size) {
    uint64_t start = (uint64_t)addr;
    uint64_t end = start + size;
    
    /* 检查溢出 */
    if (end < start) return 0;
    
    /* 检查是否在用户空间 */
    return is_user_addr(start) && (size == 0 || is_user_addr(end - 1));
}

/* 验证用户空间字符串 */
static inline int access_ok_string(const char *str, size_t max_len) {
    if (!is_user_addr((uint64_t)str)) return 0;
    
    if (max_len == 0) return 0;
    
    /* Check if the entire range is within user space bounds */
    if ((uint64_t)str + max_len > USER_END) return 0;
    
    /* Check if the pointer is valid by confirming it's in user space */
    uint64_t start_addr = (uint64_t)str;
    uint64_t end_addr = start_addr + max_len;
    
    if (!is_user_addr(start_addr) || !is_user_addr(end_addr - 1)) return 0;
    
    /* For now, just validate the address range without actually accessing memory */
    /* This prevents page faults during validation */
    return 1;
}

/* 安全从用户空间拷贝 - 声明在 cpu.h 中 */
/* 安全拷贝到用户空间 - 声明在 cpu.h 中 */

/* 安全从用户空间拷贝字符串 */
int copy_string_from_user(char *to, const char *from, size_t max_len);

/* 安全清零用户空间内存 */
int clear_user(void *to, size_t size);

/* 验证用户空间内存可读 */
int verify_user_read(const void *addr, size_t size);

/* 验证用户空间内存可写 */
int verify_user_write(void *addr, size_t size);

/* 安全创建页表项（用于按需分页） */
int vmm_create_pte_if_not_present(struct address_space *space, uint64_t virt, uint64_t flags);

/* ═══════════════════════════════════════════════════════════════════════════
 * 预测性页面预取系统
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 初始化预取系统 */
void prefetch_init(void);

/* 记录页面访问 */
void prefetch_record_access(uint64_t page_addr);

/* 预测下一个要访问的页面 */
int prefetch_predict(uint64_t *predicted_pages, int max_predictions);

/* 执行预取 */
void prefetch_execute(uint64_t page_addr);

/* 验证预测 */
void prefetch_verify(uint64_t page_addr);

/* 获取统计 */
void prefetch_get_stats(uint64_t *made, uint64_t *hit, uint64_t *miss);

/* 打印统计 */
void prefetch_print_stats(void);

/* 双内核状态管理 */
int is_address_mapped(uint64_t addr);
int dual_kernel_is_enabled(void);
void vmm_set_dual_kernel_status(int status);

#endif
