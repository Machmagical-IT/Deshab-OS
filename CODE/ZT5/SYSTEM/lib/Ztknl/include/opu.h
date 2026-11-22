/*
 * Zratro OS - OPU (Open Processing Unit) Support
 * 
 * 用于GPU、NPU、FPGA等异构处理单元的支持
 */

#ifndef OPU_H
#define OPU_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== OPU 本地数据结构 ========== */

struct opu_local {
    uint32_t opu_id;                    /* OPU ID */
    uint32_t device_id;                 /* 硬件设备ID */
    int state;                          /* 状态 */
    uint64_t stack_canary;              /* 栈金丝雀 */
    
    struct thread *current_thread;      /* 当前线程 */
    struct thread *idle_thread;         /* 空闲线程 */
    
    uint64_t schedule_count;            /* 调度计数 */
    uint64_t irq_count;                 /* 中断计数 */
    uint64_t softirq_pending;           /* 待处理软中断 */
    
    int interrupt_depth;                /* 中断深度 */
    int spinlock_count;                 /* 自旋锁计数 */
    int preempt_count;                  /* 抢占计数 */
    
    uint64_t context_switches;          /* 上下文切换 */
    uint64_t syscalls;                  /* 系统调用 */
    uint64_t page_faults;               /* 页面错误 */
    
    /* OPU 特定字段 */
    uint64_t *command_queue;            /* 命令队列 */
    uint32_t queue_head;                /* 队列头 */
    uint32_t queue_tail;                /* 队列尾 */
    uint32_t queue_size;                /* 队列大小 */
    
    /* 互斥锁保护共享资源 */
    struct mutex queue_mutex;
    spinlock_t state_lock;
    
    /* 性能计数器 */
    uint64_t compute_units_used;        /* 使用的计算单元 */
    uint64_t memory_bandwidth;          /* 内存带宽使用 */
    uint64_t total_compute_time;        /* 总计算时间 */
};

/* OPU 状态 */
#define OPU_STATE_OFFLINE       0
#define OPU_STATE_ONLINE        1
#define OPU_STATE_RUNNING       2
#define OPU_STATE_SUSPENDED     3
#define OPU_STATE_ERROR         4
#define OPU_STATE_HALTED        5

/* ========== OPU 配置 ========== */

#define MAX_OPUS                16          /* 最大OPU数量 */
#define OPU_QUEUE_SIZE          1024        /* 命令队列大小 */

/* ========== 全局 OPU 数据 ========== */

extern struct opu_local opu_data[MAX_OPUS];
extern int opu_count;

/* ========== 公共 API ========== */

/* 初始化 OPU 子系统 */
int opu_init(void);

/* 获取当前 OPU */
static inline struct opu_local *this_opu(void) {
    /* 在 OPU 上运行时，通过特殊寄存器或其他机制获取当前 OPU */
    /* 暂时返回 opu_data[0] 作为示例 */
    return &opu_data[0];
}

/* 注册 OPU 设备 */
int opu_register_device(uint32_t device_id, uint32_t opu_type);

/* 分配 OPU 任务 */
int opu_submit_task(uint32_t opu_id, void *task_desc);

/* 等待 OPU 任务完成 */
int opu_wait_task(uint32_t opu_id, uint64_t task_id);

/* 获取 OPU 状态 */
int opu_get_state(uint32_t opu_id);

/* 设置 OPU 状态 */
void opu_set_state(uint32_t opu_id, int state);

/* OPU 内存管理 */
void *opu_alloc_memory(uint32_t opu_id, size_t size, uint32_t flags);
int opu_free_memory(uint32_t opu_id, void *ptr);

#endif /* OPU_H */