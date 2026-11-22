/*
 * Zratro OS - EPT (Extended Page Tables) Header
 */

#ifndef EPT_H
#define EPT_H

#include <stdint.h>
#include <stddef.h>

/* EPT 权限标志 */
#define EPT_READ_ACCESS         (1u << 0)
#define EPT_WRITE_ACCESS        (1u << 1)
#define EPT_EXECUTE_ACCESS      (1u << 2)

/* EPT 全访问标志 */
#define EPT_ALL_ACCESS          (EPT_READ_ACCESS | EPT_WRITE_ACCESS | EPT_EXECUTE_ACCESS)

/* EPT 内存类型 */
#define EPT_MEMORY_TYPE_UC      0  /* Uncacheable */
#define EPT_MEMORY_TYPE_WC      1  /* Write-combining */
#define EPT_MEMORY_TYPE_WT      4  /* Write-through */
#define EPT_MEMORY_TYPE_WP      5  /* Write-protect */
#define EPT_MEMORY_TYPE_WB      6  /* Write-back */

/* EPT 页表项标志 */
#define EPT_FLAG_PRESENT        EPT_READ_ACCESS
#define EPT_FLAG_WRITABLE       EPT_WRITE_ACCESS
#define EPT_FLAG_EXECUTABLE     EPT_EXECUTE_ACCESS
#define EPT_FLAG_LARGE_PAGE     (1ULL << 7)
#define EPT_FLAG_ACCESSED       (1ULL << 8)
#define EPT_FLAG_DIRTY          (1ULL << 9)

/* EPT 统计信息 */
struct ept_stats {
    uint32_t pdpt_count;         /* PDPT 数量 */
    uint32_t pd_count;           /* PD 数量 */
    uint32_t pt_count;           /* PT 数量 */
    uint32_t small_4kb_pages;    /* 4KB 页面数量 */
    uint32_t large_2mb_pages;    /* 2MB 页面数量 */
    uint64_t total_mapped_bytes; /* 总映射字节数 */
};

/**
 * 初始化 EPT
 * @return 0 成功，-1 失败
 */
int ept_init(void);

/**
 * 映射单个页面
 * @param gpa 客户机物理地址
 * @param hpa 主机物理地址
 * @param flags 权限标志
 * @return 0 成功，-1 失败
 */
int ept_map_page(uint64_t gpa, uint64_t hpa, uint64_t flags);

/**
 * 映射连续页面范围
 * @param gpa 起始客户机物理地址
 * @param hpa 起始主机物理地址
 * @param pages 页面数量
 * @param flags 权限标志
 * @return 0 成功，-1 失败
 */
int ept_map_range(uint64_t gpa, uint64_t hpa, size_t pages, uint64_t flags);

/**
 * 取消映射页面
 * @param gpa 客户机物理地址
 * @return 0 成功，-1 失败
 */
int ept_unmap_page(uint64_t gpa);

/**
 * 映射 2MB 大页
 * @param gpa 客户机物理地址 (2MB 对齐)
 * @param hpa 主机物理地址 (2MB 对齐)
 * @param flags 权限标志
 * @return 0 成功，-1 失败
 */
int ept_map_2mb_page(uint64_t gpa, uint64_t hpa, uint64_t flags);

/**
 * 映射 1GB 大页
 * @param gpa 客户机物理地址 (1GB 对齐)
 * @param hpa 主机物理地址 (1GB 对齐)
 * @param flags 权限标志
 * @return 0 成功，-1 失败
 */
int ept_map_1gb_page(uint64_t gpa, uint64_t hpa, uint64_t flags);

/**
 * 设置页面访问权限
 * @param gpa 客户机物理地址
 * @param access 访问权限
 * @return 0 成功，-1 失败
 */
int ept_set_page_access(uint64_t gpa, uint64_t access);

/**
 * 获取页面访问权限
 * @param gpa 客户机物理地址
 * @return 访问权限
 */
int ept_get_page_access(uint64_t gpa);

/**
 * 获取 EPT 根页面物理地址
 * @return EPT PML4 物理地址
 */
uint64_t ept_get_root_phys(void);

/**
 * 构造 EPTP 寄存器值
 * @return EPTP 值
 */
uint64_t ept_construct_eptp(void);

/**
 * 获取 EPT 统计信息
 * @param stats 统计信息结构
 */
void ept_get_stats(struct ept_stats *stats);

/**
 * 打印 EPT 统计信息
 */
void ept_print_stats(void);

#endif /* EPT_H */
