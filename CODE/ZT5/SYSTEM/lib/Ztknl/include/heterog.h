/*
 * Zratro OS - Heterogeneous Computing Scheduler (异构计算统一调度)
 * 
 * 创新设计：
 * 1. 统一资源抽象 - CPU/GPU/FPGA/NPU 统一视图
 * 2. 自动任务映射 - 智能选择最优硬件
 * 3. 统一内存模型 - 异构内存池管理
 * 4. 任务依赖图 - DAG 调度
 * 5. 性能模型 - 硬件特性感知
 */

#ifndef HETEROG_H
#define HETEROG_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== 异构配置 ========== */

#define HET_MAX_DEVICES         16          /* 最大设备数 */
#define HET_MAX_QUEUES          64          /* 最大队列数 */
#define HET_MAX_TASKS           1024        /* 最大任务数 */
#define HET_MAX_NAME_LEN        32          /* 名字最大长度 */
#define HET_MAX_DEPENDENCIES    16          /* 最大依赖数 */

/* ========== 设备类型 ========== */

typedef enum {
    HET_DEV_NONE = 0,
    HET_DEV_CPU,                /* CPU 核心 */
    HET_DEV_GPU,                /* GPU */
    HET_DEV_FPGA,               /* FPGA */
    HET_DEV_NPU,                /* 神经网络处理器 */
    HET_DEV_TPU,                /* 张量处理器 */
    HET_DEV_DSP,                /* 数字信号处理器 */
    HET_DEV_ACCEL,              /* 通用加速器 */
    HET_DEV_MAX
} het_dev_type_t;

/* ========== 设备能力标志 ========== */

#define HET_CAP_COMPUTE         (1ULL << 0)  /* 计算能力 */
#define HET_CAP_GRAPHICS        (1ULL << 1)  /* 图形能力 */
#define HET_CAP_MATRIX          (1ULL << 2)  /* 矩阵运算 */
#define HET_CAP_CONVOLUTION     (1ULL << 3)  /* 卷积运算 */
#define HET_CAP_TENSOR          (1ULL << 4)  /* 张量运算 */
#define HET_CAP_VECTOR          (1ULL << 5)  /* 向量运算 */
#define HET_CAP_INT8            (1ULL << 6)  /* INT8 支持 */
#define HET_CAP_FP16            (1ULL << 7)  /* FP16 支持 */
#define HET_CAP_FP32            (1ULL << 8)  /* FP32 支持 */
#define HET_CAP_FP64            (1ULL << 9)  /* FP64 支持 */
#define HET_CAP_INT4            (1ULL << 10) /* INT4 支持 */
#define HET_CAP_BF16            (1ULL << 11) /* BF16 支持 */
#define HET_CAP_UNIFIED_MEM     (1ULL << 12) /* 统一内存 */
#define HET_CAP_VIRTUALIZATION  (1ULL << 13) /* 虚拟化 */

/* ========== 内存类型 ========== */

typedef enum {
    HET_MEM_SYSTEM = 0,         /* 系统内存 */
    HET_MEM_DEVICE,             /* 设备内存 */
    HET_MEM_UNIFIED,            /* 统一内存 */
    HET_MEM_REGISTER,           /* 寄存器文件 */
    HET_MEM_SHARED,             /* 共享内存 */
} het_mem_type_t;

/* ========== 内存区域 ========== */

struct het_memory_region {
    uint64_t id;
    void *host_ptr;             /* 主机地址 */
    uint64_t device_ptr;        /* 设备地址 */
    size_t size;                /* 大小 */
    het_mem_type_t type;        /* 类型 */
    uint32_t flags;             /* 标志 */
    
    /* 所有权 */
    uint32_t owner_device;
    uint32_t ref_count;
    
    struct list_head list;
};

/* 内存标志 */
#define HET_MEM_FLAG_READ       (1 << 0)
#define HET_MEM_FLAG_WRITE      (1 << 1)
#define HET_MEM_FLAG_EXEC       (1 << 2)
#define HET_MEM_FLAG_CACHED     (1 << 3)
#define HET_MEM_FLAG_COHERENT   (1 << 4)

/* ========== 设备描述符 ========== */

struct het_device {
    uint32_t device_id;         /* 设备 ID */
    char name[HET_MAX_NAME_LEN];/* 设备名 */
    het_dev_type_t type;        /* 设备类型 */
    uint32_t vendor_id;         /* 厂商 ID */
    uint32_t model_id;          /* 型号 ID */
    
    /* 能力 */
    uint64_t capabilities;      /* 能力位图 */
    uint64_t compute_units;     /* 计算单元数 */
    uint64_t clock_freq;        /* 时钟频率 (Hz) */
    
    /* 内存 */
    uint64_t global_mem_size;   /* 全局内存大小 */
    uint64_t local_mem_size;    /* 本地内存大小 */
    uint64_t const_mem_size;    /* 常量内存大小 */
    
    /* 性能模型 */
    struct {
        uint64_t gflops;        /* GFLOPS */
        uint64_t bandwidth;     /* 内存带宽 (MB/s) */
        uint64_t latency;       /* 延迟 */
        double efficiency;      /* 效率因子 */
    } perf;
    
    /* 队列 */
    struct list_head queues;    /* 命令队列 */
    uint32_t queue_count;
    
    /* 状态 */
    int is_available;
    int is_busy;
    uint64_t load;              /* 当前负载 */
    
    /* 驱动 */
    void *driver_data;          /* 驱动私有数据 */
    
    struct list_head list;
};

/* ========== 命令队列 ========== */

struct het_queue {
    uint32_t queue_id;
    uint32_t device_id;
    char name[HET_MAX_NAME_LEN];
    
    /* 队列类型 */
    uint32_t priority;
    uint32_t flags;
    
    /* 任务链表 */
    struct list_head pending;   /* 待执行 */
    struct list_head running;   /* 执行中 */
    struct list_head completed; /* 已完成 */
    
    uint32_t pending_count;
    uint32_t running_count;
    
    /* 统计 */
    uint64_t total_tasks;
    uint64_t total_time;
    
    struct list_head list;          /* 设备队列链表 */
    struct list_head global_list;   /* 全局队列链表 */
};

/* 队列标志 */
#define HET_QUEUE_FLAG_IN_ORDER    (1 << 0)   /* 顺序执行 */
#define HET_QUEUE_FLAG_OUT_ORDER   (1 << 1)   /* 乱序执行 */

/* ========== 任务类型 ========== */

typedef enum {
    HET_TASK_KERNEL = 0,        /* 内核任务 */
    HET_TASK_MEMCPY,            /* 内存拷贝 */
    HET_TASK_MEMFILL,           /* 内存填充 */
    HET_TASK_MEMSET,            /* 内存设置 */
    HET_TASK_BARRIER,           /* 屏障 */
    HET_TASK_COMPUTE,           /* 通用计算 */
    HET_TASK_MATRIX,            /* 矩阵运算 */
    HET_TASK_TENSOR,            /* 张量运算 */
    HET_TASK_CONV,              /* 卷积运算 */
} het_task_type_t;

/* ========== 任务状态 ========== */

typedef enum {
    HET_TASK_CREATED = 0,
    HET_TASK_SUBMITTED,
    HET_TASK_READY,
    HET_TASK_RUNNING,
    HET_TASK_COMPLETED,
    HET_TASK_FAILED,
    HET_TASK_CANCELLED,
} het_task_state_t;

/* ========== 计算网格 ========== */

struct het_ndrange {
    uint32_t dimensions;
    uint64_t global_size[3];    /* 全局大小 */
    uint64_t local_size[3];     /* 本地大小 (工作组) */
    uint64_t global_offset[3];  /* 全局偏移 */
};

/* ========== 任务描述符 ========== */

struct het_task {
    uint64_t task_id;
    char name[HET_MAX_NAME_LEN];
    het_task_type_t type;
    het_task_state_t state;
    
    /* 目标设备 */
    uint32_t device_id;
    uint32_t queue_id;
    
    /* 内核信息 */
    void *kernel_handle;
    struct het_ndrange ndrange;
    
    /* 参数 */
    void *args;
    size_t args_size;
    
    /* 内存引用 */
    struct het_memory_region *mem_regions[16];
    uint32_t mem_count;
    
    /* 依赖 */
    uint64_t dependencies[HET_MAX_DEPENDENCIES];
    uint32_t dep_count;
    uint32_t dep_satisfied;
    
    /* 事件 */
    uint64_t event_id;
    
    /* 时间 */
    uint64_t submit_time;
    uint64_t start_time;
    uint64_t end_time;
    
    /* 结果 */
    int result;
    uint64_t ret_value;
    
    struct list_head list;
};

/* ========== 事件 ========== */

struct het_event {
    uint64_t event_id;
    uint32_t ref_count;
    int completed;
    int status;
    
    /* 关联任务 */
    uint64_t task_id;
    
    /* 回调 */
    void (*callback)(struct het_event *event, void *data);
    void *callback_data;
    
    /* 等待队列 */
    struct list_head waiters;
    
    struct list_head list;
};

/* ========== 性能模型 ========== */

/* 定点数精度 (避免浮点) */
#define HET_FIXED_SCALE 1000

struct het_perf_model {
    /* 设备权重 (定点数，精度 1000) */
    int64_t compute_weight;      /* 计算权重 */
    int64_t memory_weight;       /* 内存权重 */
    int64_t latency_weight;      /* 延迟权重 */
    
    /* 任务特征 */
    uint64_t flops_estimate;    /* FLOPS 估算 */
    uint64_t bytes_estimate;    /* 数据传输估算 */
    
    /* 选择得分 (定点数) */
    int64_t scores[HET_MAX_DEVICES];
};

/* ========== 异构调度器 ========== */

struct het_scheduler {
    /* 设备列表 */
    struct list_head devices;
    uint32_t device_count;
    
    /* 队列列表 */
    struct list_head queues;
    uint32_t queue_count;
    
    /* 任务管理 */
    struct list_head all_tasks;
    uint64_t next_task_id;
    
    /* 事件管理 */
    struct list_head events;
    uint64_t next_event_id;
    
    /* 内存管理 */
    struct list_head memory_regions;
    uint64_t next_mem_id;
    
    /* 调度策略 */
    uint32_t policy;
    uint32_t flags;
    
    /* 性能模型 */
    struct het_perf_model perf_model;
    
    /* 统计 */
    uint64_t total_tasks;
    uint64_t completed_tasks;
    uint64_t failed_tasks;
    uint64_t total_bytes;
    
    /* 锁 */
    spinlock_t lock;
};

/* 调度策略 */
#define HET_POLICY_ROUND_ROBIN   0   /* 轮询 */
#define HET_POLICY_LOAD_BALANCE  1   /* 负载均衡 */
#define HET_POLICY_PERF_MODEL    2   /* 性能模型 */
#define HET_POLICY_AFFINITY      3   /* 亲和性 */

/* 调度标志 */
#define HET_FLAG_AUTO_MIGRATE    (1 << 0)  /* 自动数据迁移 */
#define HET_FLAG_UNIFIED_MEM     (1 << 1)  /* 统一内存 */
#define HET_FLAG_PROFILE         (1 << 2)  /* 性能分析 */

/* ========== 公共 API ========== */

/* 初始化/销毁 */
int het_init(struct het_scheduler *hs);
void het_destroy(struct het_scheduler *hs);

/* ========== 设备管理 ========== */

/* 注册设备 */
int het_register_device(struct het_scheduler *hs, struct het_device *dev);

/* 注销设备 */
int het_unregister_device(struct het_scheduler *hs, uint32_t device_id);

/* 获取设备 */
struct het_device *het_get_device(struct het_scheduler *hs, uint32_t device_id);

/* 查询设备能力 */
int het_query_capability(struct het_device *dev, uint64_t cap);

/* 枚举设备 */
int het_enumerate_devices(struct het_scheduler *hs, het_dev_type_t type,
                          uint32_t *devices, uint32_t *count);

/* ========== 队列管理 ========== */

/* 创建队列 */
struct het_queue *het_create_queue(struct het_scheduler *hs,
                                    uint32_t device_id,
                                    uint32_t priority,
                                    uint32_t flags);

/* 销毁队列 */
int het_destroy_queue(struct het_scheduler *hs, uint32_t queue_id);

/* 刷新队列 */
int het_flush_queue(struct het_queue *queue);

/* ========== 内存管理 ========== */

/* 分配内存 */
struct het_memory_region *het_alloc_memory(struct het_scheduler *hs,
                                            uint32_t device_id,
                                            size_t size,
                                            het_mem_type_t type,
                                            uint32_t flags);

/* 释放内存 */
int het_free_memory(struct het_scheduler *hs, struct het_memory_region *region);

/* 内存拷贝 */
int het_memcpy(struct het_scheduler *hs,
               struct het_memory_region *dst, size_t dst_offset,
               struct het_memory_region *src, size_t src_offset,
               size_t size,
               uint64_t *event);

/* 内存填充 */
int het_memfill(struct het_scheduler *hs,
                struct het_memory_region *dst, size_t offset,
                uint32_t pattern, size_t size,
                uint64_t *event);

/* 获取主机指针 */
void *het_map_memory(struct het_memory_region *region);

/* 解除映射 */
int het_unmap_memory(struct het_memory_region *region);

/* ========== 任务管理 ========== */

/* 创建任务 */
struct het_task *het_create_task(struct het_scheduler *hs,
                                  het_task_type_t type,
                                  const char *name);

/* 销毁任务 */
int het_destroy_task(struct het_task *task);

/* 设置内核参数 */
int het_set_kernel_args(struct het_task *task, void *args, size_t size);

/* 设置计算网格 */
int het_set_ndrange(struct het_task *task, struct het_ndrange *ndrange);

/* 添加依赖 */
int het_add_dependency(struct het_task *task, uint64_t event_id);

/* 添加内存区域 */
int het_add_memory_region(struct het_task *task, struct het_memory_region *region);

/* 提交任务 */
int het_submit_task(struct het_scheduler *hs,
                    struct het_task *task,
                    uint32_t queue_id,
                    uint64_t *event);

/* ========== 事件管理 ========== */

/* 等待事件 */
int het_wait_event(struct het_scheduler *hs, uint64_t event_id, uint64_t timeout);

/* 等待多个事件 */
int het_wait_events(struct het_scheduler *hs, uint64_t *events, uint32_t count,
                    uint64_t timeout);

/* 设置回调 */
int het_set_callback(struct het_scheduler *hs, uint64_t event_id,
                     void (*callback)(struct het_event *, void *),
                     void *data);

/* 查询事件状态 */
int het_query_event(struct het_scheduler *hs, uint64_t event_id);

/* ========== 自动设备选择 ========== */

/* 选择最优设备 */
uint32_t het_select_device(struct het_scheduler *hs,
                            struct het_perf_model *model);

/* 估算执行时间 */
uint64_t het_estimate_time(struct het_device *dev,
                            struct het_perf_model *model);

/* 计算设备得分 (返回定点数，精度 1000) */
int64_t het_compute_score(struct het_device *dev,
                          struct het_perf_model *model);

/* ========== 便捷 API ========== */

/* 简单计算任务 */
int het_compute(struct het_scheduler *hs,
                uint64_t flops,
                void *input, size_t input_size,
                void *output, size_t output_size,
                uint64_t *event);

/* 矩阵乘法 */
int het_matmul(struct het_scheduler *hs,
               void *A, void *B, void *C,
               uint32_t M, uint32_t N, uint32_t K,
               uint64_t *event);

/* 卷积运算 */
int het_conv2d(struct het_scheduler *hs,
               void *input, void *kernel, void *output,
               uint32_t in_h, uint32_t in_w, uint32_t in_c,
               uint32_t k_h, uint32_t k_w, uint32_t k_c,
               uint64_t *event);

/* ========== 统计和调试 ========== */

/* 获取统计 */
void het_get_stats(struct het_scheduler *hs,
                   uint64_t *tasks, uint64_t *completed,
                   uint64_t *bytes);

/* 打印设备列表 */
void het_print_devices(struct het_scheduler *hs);

/* 打印任务状态 */
void het_print_tasks(struct het_scheduler *hs);

/* ========== 内联辅助 ========== */

/* 设备类型字符串 */
static inline const char *het_dev_type_name(het_dev_type_t type) {
    switch (type) {
        case HET_DEV_CPU:   return "CPU";
        case HET_DEV_GPU:   return "GPU";
        case HET_DEV_FPGA:  return "FPGA";
        case HET_DEV_NPU:   return "NPU";
        case HET_DEV_TPU:   return "TPU";
        case HET_DEV_DSP:   return "DSP";
        case HET_DEV_ACCEL: return "Accelerator";
        default:            return "Unknown";
    }
}

/* 任务类型字符串 */
static inline const char *het_task_type_name(het_task_type_t type) {
    switch (type) {
        case HET_TASK_KERNEL:  return "kernel";
        case HET_TASK_MEMCPY:  return "memcpy";
        case HET_TASK_MEMFILL: return "memfill";
        case HET_TASK_MEMSET:  return "memset";
        case HET_TASK_BARRIER: return "barrier";
        case HET_TASK_COMPUTE: return "compute";
        case HET_TASK_MATRIX:  return "matrix";
        case HET_TASK_TENSOR:  return "tensor";
        case HET_TASK_CONV:    return "conv";
        default:               return "unknown";
    }
}

/* 任务状态字符串 */
static inline const char *het_task_state_name(het_task_state_t state) {
    switch (state) {
        case HET_TASK_CREATED:   return "created";
        case HET_TASK_SUBMITTED: return "submitted";
        case HET_TASK_READY:     return "ready";
        case HET_TASK_RUNNING:   return "running";
        case HET_TASK_COMPLETED: return "completed";
        case HET_TASK_FAILED:    return "failed";
        case HET_TASK_CANCELLED: return "cancelled";
        default:                 return "unknown";
    }
}

/* 全局调度器访问 */
struct het_scheduler *het_get_global(void);
int het_global_init(void);

#endif /* HETEROG_H */
