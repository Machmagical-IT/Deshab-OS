/*
 * Zratro OS - Advanced Page Fault Handling and Prevention Header
 */

#ifndef PAGE_FAULT_H
#define PAGE_FAULT_H

#include <stdint.h>
#include <stddef.h>
#include "isr.h"  /* 包含interrupt_frame和相关定义 */
#include "task.h" /* 包含struct thread和struct process定义 */

/* 页错误类型枚举 */
typedef enum {

/* 缺页预取方向 */
#define PF_PREFETCH_FORWARD    1    /* 前向预取 */
#define PF_PREFETCH_BACKWARD   2    /* 后向预取 */

    PF_TYPE_UNKNOWN = 0,
    PF_TYPE_NOT_PRESENT,
    PF_TYPE_PROTECTION,
    PF_TYPE_STACK_OVERFLOW,
    PF_TYPE_USER_ACCESS,
    PF_TYPE_DOUBLE_FAULT,
    PF_TYPE_ACCESS_VIOLATION,
    PF_TYPE_COW,                /* COW 写时复制缺页 */
    PF_TYPE_DEMAND_PAGING,      /* 按需分页缺页 */
    PF_TYPE_GUARD_PAGE          /* 保护页访问 */
} pf_type_t;

/* 页错误结果枚举 */
typedef enum {
    PF_HANDLED_FAILED = 0,    /* 处理失败 */
    PF_HANDLED_SUCCESS,       /* 处理成功 */
    PF_HANDLED_FATAL,         /* 致命错误，需要终止程序 */
    PF_SKIPPED                /* 跳过指令并继续执行 */
} pf_result_t;

/* 页错误统计信息 */
struct page_fault_stats {
    uint64_t total_faults;              /* 总页错误数 */
    uint64_t unhandled_faults;          /* 未处理错误数 */
    uint64_t handled_faults;            /* 已处理错误数 */
    uint64_t demand_paging_faults;      /* 按需分页错误 */
    uint64_t invalid_access_faults;     /* 无效访问错误 */
    uint64_t stack_faults;              /* 栈错误 */
    uint64_t user_faults;               /* 用户错误 */
    uint64_t kernel_faults;             /* 内核错误 */
    uint64_t cow_faults;                /* COW错误 */
    uint64_t recovery_attempts;         /* 恢复尝试 */
    uint64_t recovery_successes;        /* 恢复成功 */
    uint64_t skipped_faults;            /* 跳过的错误数 */
    uint64_t skipped_instructions;      /* 跳过的指令数 */
    uint64_t cow_breaks;                /* COW 断开 (实际复制) 次数 */
    uint64_t prefetch_hits;             /* 预取命中次数 */
    uint64_t prefetch_attempts;         /* 预取尝试次数 */
    uint64_t guard_page_faults;         /* 保护页错误次数 */
};

/* 页错误预防配置 */
struct page_fault_prevention {
    int enable_bounds_checking;         /* 启用边界检查 */
    int enable_stack_protection;        /* 启用栈保护 */
    int enable_kernel_guard;            /* 启用内核保护 */
    int enable_user_isolation;          /* 启用用户隔离 */
    size_t max_stack_growth;            /* 最大栈增长 */
    size_t guard_page_size;             /* 保护页大小 */
    int enable_access_monitoring;       /* 启用访问监控 */
};

/* 页错误信息结构 */
struct page_fault_info {
    uint64_t fault_addr;                /* 错误地址 */
    uint64_t error_code;                /* 错误代码 */
    uint64_t rip;                       /* 指令指针 */
    pf_type_t type;                     /* 错误类型 */
    int is_kernel_mode;                 /* 是否内核模式 */
};

/* 函数声明 */
pf_result_t handle_page_fault(struct interrupt_frame *frame, uint64_t fault_addr);
pf_result_t handle_kernel_page_fault(struct interrupt_frame *frame, uint64_t fault_addr);
pf_result_t handle_user_page_fault(struct interrupt_frame *frame, uint64_t fault_addr);

/* 统计函数 */
void pf_stats_init(void);
void pf_stats_record_fault(pf_type_t type);
struct page_fault_stats *pf_get_stats(void);

/* 预防函数 */
void pf_prevention_init(void);
pf_type_t pf_classify_error(uint64_t fault_addr, uint64_t error_code);
int pf_is_kernel_address(uint64_t addr);
int pf_is_user_address(uint64_t addr);
int pf_is_stack_address(uint64_t addr, struct thread *thread);
int pf_prevent_stack_overflow(uint64_t fault_addr, struct thread *thread);
int pf_prevent_invalid_access(uint64_t fault_addr, uint64_t error_code);
int pf_prevent_double_fault(void);

/* 辅助函数 */
int pf_safe_terminate_process(struct process *proc, const char *reason);
void pf_debug_info(struct page_fault_info *info);
void pf_dump_context(struct interrupt_frame *frame, uint64_t fault_addr);
void pf_try_recovery(struct page_fault_info *info);

/* 跳过指令相关函数 */
int pf_can_skip_fault(uint64_t fault_addr, uint64_t rip, uint64_t error_code);
int pf_skip_instruction(struct interrupt_frame *frame);
uint64_t pf_get_instruction_length(uint64_t rip);

/* ═══════════════════════════════════════════════════════════════════════════
 * COW 缺页处理增强
 * ═══════════════════════════════════════════════════════════════════════════ */

/* COW 缺页处理: 检测对只读 COW 页的写入，分配新页，复制数据，更新 PTE
 *
 * @param fault_addr 缺页地址
 * @param error_code 页错误码
 * @return 0=成功处理, -1=非 COW 页或处理失败
 */
int pf_handle_cow_fault(uint64_t fault_addr, uint64_t error_code);

/* ═══════════════════════════════════════════════════════════════════════════
 * 页错误预取
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 页错误预取: 当缺页发生时，预取相邻页面
 *
 * @param fault_addr 缺页地址
 * @param error_code 页错误码
 * @param prefetch_pages 预取页数 (通常 1-4)
 */
void pf_prefetch_adjacent(uint64_t fault_addr, uint64_t error_code, int prefetch_pages);

/* ═══════════════════════════════════════════════════════════════════════════
 * 页错误详细统计
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 记录按类型统计的页错误 */
void pf_stats_record_typed(pf_type_t type, int handled);

/* 获取按类型的页错误计数 */
uint64_t pf_stats_get_count(pf_type_t type);

/* 重置页错误统计 */
void pf_stats_reset(void);

#endif /* PAGE_FAULT_H */