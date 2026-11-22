/*
 * Zratro OS - User Transaction API (UTX)
 * 
 * 原创用户态事务系统
 * 基于 Z-Journal 内核事务引擎，为用户态程序提供：
 * - ACID 事务保证
 * - 快照隔离
 * - 崩溃恢复
 */

#ifndef UTX_H
#define UTX_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== 配置 ========== */

#define UTX_MAX_PER_PROCESS    16      /* 每进程最大事务数 */
#define UTX_MAX_OPS            256     /* 单事务最大操作数 */
#define UTX_MAX_DATA_SIZE      (64 * 1024)  /* 单操作最大数据 64KB */
#define UTX_DEFAULT_TIMEOUT    5000    /* 默认超时 5 秒 */

/* ========== 事务标志 ========== */

#define UTX_FLAG_READONLY      (1 << 0)   /* 只读事务 */
#define UTX_FLAG_SYNC          (1 << 1)   /* 同步提交 */
#define UTX_FLAG_NESTED        (1 << 2)   /* 嵌套事务 */
#define UTX_FLAG_NOWAIT        (1 << 3)   /* 不等待锁 */
#define UTX_FLAG_PRIORITY      (1 << 4)   /* 高优先级事务 */

/* ========== 事务状态 ========== */

typedef enum {
    UTX_STATE_INVALID = 0,
    UTX_STATE_ACTIVE,           /* 活跃中 */
    UTX_STATE_PREPARING,        /* 准备提交 */
    UTX_STATE_COMMITTING,       /* 提交中 */
    UTX_STATE_COMMITTED,        /* 已提交 */
    UTX_STATE_ABORTING,         /* 中止中 */
    UTX_STATE_ABORTED,          /* 已中止 */
    UTX_STATE_TIMEOUT           /* 超时 */
} utx_state_t;

/* ========== 操作类型 ========== */

typedef enum {
    UTX_OP_NONE = 0,
    UTX_OP_WRITE,               /* 写文件 */
    UTX_OP_READ,                /* 读文件 */
    UTX_OP_MEM_WRITE,           /* 内存写入 */
    UTX_OP_ATOMIC_ADD,          /* 原子加 */
    UTX_OP_ATOMIC_CAS,          /* 原子比较交换 */
    UTX_OP_BARRIER              /* 内存屏障 */
} utx_op_type_t;

/* ========== 用户态事务句柄 ========== */

/* 用户可见的事务句柄 (用于系统调用返回) */
struct utx_handle_user {
    uint64_t tx_id;             /* 事务 ID */
    uint32_t state;             /* 状态 (utx_state_t) */
    uint32_t flags;             /* 标志 */
    uint32_t op_count;          /* 操作计数 */
    uint32_t error_code;        /* 错误码 */
    uint64_t start_time;        /* 开始时间 */
    uint64_t timeout;           /* 超时时间 */
};

/* ========== 操作记录 ========== */

struct utx_op {
    utx_op_type_t type;         /* 操作类型 */
    uint32_t flags;             /* 标志 */
    int fd;                     /* 文件描述符 (文件操作) */
    uint64_t offset;            /* 偏移 */
    uint64_t size;              /* 大小 */
    union {
        void *buf;              /* 缓冲区指针 */
        uint64_t value;         /* 原子操作值 */
        uint64_t compare;       /* CAS 比较值 */
    };
    struct list_head list;
};

/* ========== 内核态事务上下文 ========== */

/* 内核事务描述符 */
struct utx_context {
    uint64_t tx_id;             /* 事务 ID (全局唯一) */
    utx_state_t state;          /* 状态 */
    uint32_t flags;             /* 标志 */
    
    /* 所属进程 */
    uint32_t pid;
    
    /* 操作队列 */
    struct list_head ops;       /* 操作链表 */
    uint32_t op_count;          /* 操作数 */
    
    /* 冲突检测 */
    uint64_t read_set[16];      /* 读集合 */
    uint64_t write_set[16];     /* 写集合 */
    int read_count;
    int write_count;
    
    /* 时间管理 */
    uint64_t start_time;
    uint64_t timeout;
    
    /* 嵌套事务支持 */
    uint64_t parent_tx_id;
    int nest_level;
    
    /* 错误信息 */
    int error_code;
    
    /* 引用计数 - 用于防止 TOCTOU 问题 */
    int ref_count;
    
    /* 链表 - 需要两个独立的链表节点 */
    struct list_head proc_list; /* 进程事务链表节点 */
    struct list_head global_list; /* 全局事务链表节点 */
    struct list_head list;      /* 兼容旧代码，指向 global_list */
    
    /* 自旋锁 */
    spinlock_t lock;
};

/* ========== 进程事务表 ========== */

struct utx_process_table {
    struct list_head transactions;  /* 事务链表 */
    uint32_t count;                 /* 事务数 */
    uint64_t next_tx_id;            /* 下一个事务 ID */
    spinlock_t lock;
};

/* ========== 快照结构 ========== */

struct utx_snapshot {
    uint64_t snapshot_id;       /* 快照 ID */
    uint64_t version;           /* 版本号 */
    uint64_t timestamp;         /* 时间戳 */
    uint32_t ref_count;         /* 引用计数 */
    struct list_head list;
};

/* ========== 统计信息 ========== */

struct utx_stats {
    uint64_t total_tx;          /* 总事务数 */
    uint64_t committed_tx;      /* 已提交 */
    uint64_t aborted_tx;        /* 已中止 */
    uint64_t timeout_tx;        /* 超时事务 */
    uint64_t total_ops;         /* 总操作数 */
    uint64_t conflicts;         /* 冲突次数 */
};

/* ========== 公共 API ========== */

/* 初始化 */
int utx_init(void);

/* 清理 */
void utx_cleanup(void);

/* 获取进程事务表 */
struct utx_process_table *utx_get_process_table(uint32_t pid);

/* 创建事务表 */
struct utx_process_table *utx_create_process_table(void);

/* 销毁事务表 */
void utx_destroy_process_table(struct utx_process_table *table);

/* ========== 事务操作 ========== */

/* 开始事务 (内核实现) */
int utx_begin_kernel(uint32_t pid, uint32_t flags, struct utx_handle_user *handle);

/* 提交事务 */
int utx_commit_kernel(uint64_t tx_id);

/* 中止事务 */
int utx_abort_kernel(uint64_t tx_id);

/* 添加写操作 */
int utx_add_write_kernel(uint64_t tx_id, int fd, uint64_t offset, 
                         const void *data, size_t size);

/* 添加读操作 */
int utx_add_read_kernel(uint64_t tx_id, int fd, uint64_t offset,
                        void *buf, size_t size);

/* 查询事务状态 */
int utx_get_status_kernel(uint64_t tx_id, struct utx_handle_user *handle);

/* 创建快照 */
int utx_snapshot_create_kernel(uint32_t pid, uint64_t *snapshot_id);

/* ========== 冲突检测 ========== */

/* 检查两个事务是否可并行提交 */
int utx_can_parallel_commit(struct utx_context *t1, struct utx_context *t2);

/* 检测读写冲突 */
int utx_detect_conflict(struct utx_context *tx, uint64_t block_no, int is_write);

/* ========== 超时管理 ========== */

/* 检查超时事务 */
void utx_check_timeouts(void);

/* 设置事务超时 */
int utx_set_timeout(uint64_t tx_id, uint64_t timeout_ms);

/* ========== 统计 ========== */

void utx_get_stats(struct utx_stats *stats);

/* ========== 辅助函数 ========== */

/* 验证用户空间句柄指针 */
static inline int utx_handle_valid(const struct utx_handle_user *handle) {
    return handle != NULL && handle->tx_id != 0;
}

/* 事务是否活跃 */
static inline int utx_is_active(const struct utx_context *ctx) {
    return ctx != NULL && ctx->state == UTX_STATE_ACTIVE;
}

/* 事务是否可提交 */
static inline int utx_can_commit(const struct utx_context *ctx) {
    return ctx != NULL && 
           (ctx->state == UTX_STATE_ACTIVE || ctx->state == UTX_STATE_PREPARING);
}

#endif /* UTX_H */
