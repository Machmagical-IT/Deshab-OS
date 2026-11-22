/*
 * Zratro OS - Z-Ring (原创异步 I/O 系统)
 * 
 * 创新设计：
 * 1. 智能环形缓冲 - 自适应批处理
 * 2. 零拷贝数据通道 - 直接内存映射
 * 3. 事件驱动调度 - 优先级感知
 * 4. 协程式回调链 - 无栈协程
 * 5. 向量化 I/O - 单次调用多操作
 */

#ifndef ZRING_H
#define ZRING_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* 文件偏移类型 */
typedef int64_t off_t;

/* ========== Z-Ring 配置 ========== */

#define ZR_RING_SIZE          4096        /* 默认环形缓冲大小 */
#define ZR_MAX_IOV            16          /* 最大 I/O 向量数 */
#define ZR_MAX_PRIORITY       8           /* 优先级级别数 */
#define ZR_COMPLETION_EVENT   1           /* 完成事件 */
#define ZR_ERROR_EVENT        2           /* 错误事件 */

/* ========== I/O 操作类型 ========== */

typedef enum {
    ZR_OP_READ = 1,              /* 读取 */
    ZR_OP_WRITE,                 /* 写入 */
    ZR_OP_READV,                 /* 向量读取 */
    ZR_OP_WRITEV,                /* 向量写入 */
    ZR_OP_POLL,                  /* 轮询 */
    ZR_OP_ACCEPT,                /* 接受连接 */
    ZR_OP_CONNECT,               /* 连接 */
    ZR_OP_SEND,                  /* 发送 */
    ZR_OP_RECV,                  /* 接收 */
    ZR_OP_SENDFILE,              /* 零拷贝发送文件 */
    ZR_OP_SPLICE,                /* 管道拼接 */
    ZR_OP_TIMEOUT,               /* 超时 */
    ZR_OP_CANCEL,                /* 取消 */
    ZR_OP_LINK_TIMEOUT,          /* 链接超时 */
    ZR_OP_FALLOCATE,             /* 预分配 */
    ZR_OP_FSYNC,                 /* 同步 */
    ZR_OP_STATX,                 /* 获取文件状态 */
    ZR_OP_OPENAT,                /* 打开文件 */
    ZR_OP_CLOSE,                 /* 关闭文件 */
    
    /* Zratro 原创增强: 零拷贝操作 */
    ZR_OP_SEND_ZC,               /* 零拷贝发送 */
    ZR_OP_RECV_ZC,               /* 零拷贝接收 */
    ZR_OP_SENDMSG_ZC,            /* 零拷贝发送消息 */
    ZR_OP_PROVIDE_BUFFERS,       /* 提供缓冲区 */
    ZR_OP_REMOVE_BUFFERS,        /* 移除缓冲区 */
} zr_op_type_t;

/* ========== I/O 向量 ========== */

struct zr_iovec {
    void *iov_base;              /* 缓冲区基地址 */
    size_t iov_len;              /* 缓冲区长度 */
};

/* ========== 请求描述符 ========== */

struct zr_sqe {
    /* 基本信息 */
    uint8_t  op;                 /* 操作类型 */
    uint8_t  flags;              /* 标志 */
    uint16_t prio;               /* 优先级 */
    uint32_t fd;                 /* 文件描述符 */
    
    /* 联合数据 */
    union {
        struct {
            uint64_t addr;       /* 缓冲区地址 */
            uint32_t len;        /* 数据长度 */
            uint32_t off;        /* 偏移 */
        } rw;
        
        struct {
            uint64_t iovec_addr; /* I/O 向量地址 */
            uint32_t iov_cnt;    /* 向量数 */
            uint32_t off;        /* 偏移 */
        } rwv;
        
        struct {
            uint64_t addr;       /* sockaddr 地址 */
            uint32_t len;        /* 地址长度 */
        } sockaddr;
        
        struct {
            uint32_t mode;       /* 模式 */
            uint32_t flags;      /* 标志 */
        } open;
        
        struct {
            uint32_t in_fd;      /* 输入 fd */
            uint32_t out_fd;     /* 输出 fd */
            uint64_t len;        /* 长度 */
            uint64_t off_in;     /* 输入偏移 */
            uint64_t off_out;    /* 输出偏移 */
        } splice;
        
        /* Zratro 原创增强: 零拷贝操作 */
        struct {
            uint64_t buf_idx;    /* 注册缓冲区索引 */
            uint32_t buf_off;    /* 缓冲区内偏移 */
            uint32_t len;        /* 长度 */
            uint32_t flags;      /* 零拷贝标志 */
            uint32_t reserved;
        } zc;
        
        struct {
            uint64_t addr;       /* 缓冲区地址数组 */
            uint32_t len;        /* 每个缓冲区长度 */
            uint32_t count;      /* 缓冲区数量 */
            uint32_t bgid;       /* 缓冲区组 ID */
            uint32_t flags;
        } provide_buf;
        
        struct {
            uint64_t timeout;    /* 超时 (纳秒) */
            uint32_t events;     /* 事件 */
        } poll;
        
        uint64_t padding[3];     /* 填充对齐 */
    };
    
    /* 用户数据 (回调用) */
    uint64_t user_data;
    
    /* 链式操作 */
    uint16_t link_next;          /* 链接下一个请求索引 */
    uint16_t link_prev;          /* 链接上一个请求索引 */
    
    /* 依赖标记 */
    uint32_t depends_on;         /* 依赖的请求 ID */
    
    /* 调度相关 */
    uint64_t submit_time;        /* 提交时间 */
    uint64_t deadline;           /* 截止时间 */
    uint64_t runtime;            /* 运行时间 */
    
    /* 重试相关 */
    uint32_t retry_count;        /* 重试次数 */
    uint32_t reserved;           /* 保留对齐 */
} __attribute__((packed));

/* 请求标志 */
#define ZR_SQE_FLAG_FIXED_FILE  (1 << 0)   /* 固定文件描述符 */
#define ZR_SQE_FLAG_IO_DRAIN    (1 << 1)   /* 排空操作 */
#define ZR_SQE_FLAG_IO_LINK     (1 << 2)   /* 链式操作 */
#define ZR_SQE_FLAG_IO_HARDLINK (1 << 3)   /* 硬链接 */
#define ZR_SQE_FLAG_ASYNC       (1 << 4)   /* 强制异步 */
#define ZR_SQE_FLAG_BUFFER      (1 << 5)   /* 选择缓冲区 */
#define ZR_SQE_FLAG_DEADLINE    (1 << 6)   /* 有截止时间 */
#define ZR_SQE_FLAG_RETRY       (1 << 7)   /* 允许重试 */

/* ========== 完成事件 ========== */

struct zr_cqe {
    uint64_t user_data;          /* 用户数据 */
    int64_t  result;             /* 结果 */
    uint32_t flags;              /* 标志 */
    uint32_t error;              /* 错误码 */
    uint64_t timestamp;          /* 完成时间戳 */
} __attribute__((packed));

/* 完成标志 */
#define ZR_CQE_FLAG_DONE      (1 << 0)
#define ZR_CQE_FLAG_ERROR     (1 << 1)
#define ZR_CQE_FLAG_TIMEOUT   (1 << 2)
#define ZR_CQE_FLAG_CANCELLED (1 << 3)
#define ZR_CQE_FLAG_LINKED    (1 << 4)

/* ========== 环形缓冲区 ========== */

struct zr_ring {
    /* 提交队列 (SQ) */
    uint32_t *sq_head;           /* SQ 头指针 */
    uint32_t *sq_tail;           /* SQ 尾指针 */
    struct zr_sqe *sqes;         /* SQ 条目数组 */
    uint32_t sq_size;            /* SQ 大小 */
    uint32_t sq_mask;            /* SQ 掩码 */
    
    /* 完成队列 (CQ) */
    uint32_t *cq_head;           /* CQ 头指针 */
    uint32_t *cq_tail;           /* CQ 尾指针 */
    struct zr_cqe *cqes;         /* CQ 条目数组 */
    uint32_t cq_size;            /* CQ 大小 */
    uint32_t cq_mask;            /* CQ 掩码 */
    
    /* 标志 */
    uint32_t *flags;
    uint32_t *features;
    
    /* 统计 */
    uint64_t submitted;          /* 已提交 */
    uint64_t completed;          /* 已完成 */
    uint64_t errors;             /* 错误数 */
};

/* 环形标志 */
#define ZR_FLAG_ENABLED     (1 << 0)
#define ZR_FLAG_NEED_WAKEUP (1 << 1)
#define ZR_FLAG_SQ_THREADS  (1 << 2)

/* ========== 零拷贝缓冲区 ========== */

struct zr_buffer {
    uint64_t addr;               /* 地址 */
    uint32_t len;                /* 长度 */
    uint32_t index;              /* 索引 */
    int ref_count;               /* 引用计数 */
    struct list_head list;       /* 链表节点 */
};

/* 缓冲池 */
struct zr_buffer_pool {
    struct list_head free_list;  /* 空闲列表 */
    struct list_head used_list;  /* 使用列表 */
    uint32_t total;              /* 总数 */
    uint32_t free_count;         /* 空闲数 */
    spinlock_t lock;
};

/* ========== 智能批处理器 ========== */

struct zr_batch {
    struct zr_sqe *requests[64]; /* 待处理请求 */
    uint32_t count;              /* 请求数 */
    uint32_t priority_sum;       /* 优先级总和 */
    uint64_t earliest_timeout;   /* 最早超时 */
    
    /* 自适应参数 */
    uint32_t optimal_size;       /* 最优批次大小 */
    uint32_t adaptive_threshold; /* 自适应阈值 */
};

/* ========== 事件驱动调度器 ========== */

struct zr_event {
    uint64_t id;                 /* 事件 ID */
    uint32_t type;               /* 类型 */
    uint32_t priority;           /* 优先级 */
    uint64_t timestamp;          /* 时间戳 */
    void *data;                  /* 事件数据 */
    struct list_head list;       /* 链表节点 */
};

/* 优先级队列 */
struct zr_priority_queue {
    struct list_head queues[ZR_MAX_PRIORITY];
    uint32_t counts[ZR_MAX_PRIORITY];
    spinlock_t lock;
};

/* ========== 回调链系统 ========== */

typedef void (*zr_callback_fn)(struct zr_cqe *cqe, void *data);

struct zr_callback {
    zr_callback_fn fn;           /* 回调函数 */
    void *data;                  /* 用户数据 */
    struct zr_callback *next;    /* 链式回调 */
};

/* 回调链 */
struct zr_callback_chain {
    struct zr_callback *head;    /* 链头 */
    struct zr_callback *tail;    /* 链尾 */
    uint32_t count;              /* 回调数 */
    spinlock_t lock;
};

/* ========== Z-Ring 主结构 ========== */

struct zring {
    /* 环形缓冲区 */
    struct zr_ring ring;
    
    /* 缓冲池 */
    struct zr_buffer_pool buf_pool;
    
    /* 批处理器 */
    struct zr_batch batch;
    
    /* 调度器 */
    struct zr_priority_queue pq;
    
    /* 回调链 */
    struct zr_callback_chain cb_chain;
    
    /* 文件描述符表 */
    void **files;                /* 文件指针数组 */
    uint32_t file_count;         /* 文件数 */
    uint32_t file_capacity;      /* 容量 */
    
    /* 工作线程 */
    int worker_count;
    volatile int running;
    
    /* 调度策略 */
    int scheduler_policy;        /* 调度策略 */
    
    /* 统计 */
    uint64_t total_requests;
    uint64_t total_bytes;
    uint64_t avg_latency;        /* 平均延迟 (纳秒) */
    
    /* 锁 */
    spinlock_t lock;
};

/* ========== 公共 API ========== */

/* 初始化/销毁 */
int zring_init(struct zring *zr, uint32_t ring_size, uint32_t buf_count);
void zring_destroy(struct zring *zr);

/* 提交请求 */
int zring_submit(struct zring *zr, struct zr_sqe *sqe);
int zring_submit_batch(struct zring *zr, struct zr_sqe *sqes, uint32_t count);

/* 收取完成 */
int zring_peek_cqe(struct zring *zr, struct zr_cqe **cqe);
int zring_wait_cqe(struct zring *zr, struct zr_cqe **cqe, uint64_t timeout);
int zring_seen_cqe(struct zring *zr, struct zr_cqe *cqe);

/* 批量收取 */
uint32_t zring_batch_cqe(struct zring *zr, struct zr_cqe **cqes, uint32_t count);

/* 构建请求 */
struct zr_sqe *zring_get_sqe(struct zring *zr);
void zring_prep_read(struct zr_sqe *sqe, int fd, void *buf, size_t len, off_t off);
void zring_prep_write(struct zr_sqe *sqe, int fd, const void *buf, size_t len, off_t off);
void zring_prep_readv(struct zr_sqe *sqe, int fd, const struct zr_iovec *iov, int iov_cnt, off_t off);
void zring_prep_writev(struct zr_sqe *sqe, int fd, const struct zr_iovec *iov, int iov_cnt, off_t off);
void zring_prep_poll(struct zr_sqe *sqe, int fd, uint32_t events);
void zring_prep_accept(struct zr_sqe *sqe, int fd, void *addr, uint32_t *addrlen);
void zring_prep_connect(struct zr_sqe *sqe, int fd, const void *addr, uint32_t addrlen);
void zring_prep_send(struct zr_sqe *sqe, int fd, const void *buf, size_t len);
void zring_prep_recv(struct zr_sqe *sqe, int fd, void *buf, size_t len);
void zring_prep_sendfile(struct zr_sqe *sqe, int out_fd, int in_fd, size_t count, off_t *off);
void zring_prep_fsync(struct zr_sqe *sqe, int fd);
void zring_prep_timeout(struct zr_sqe *sqe, uint64_t nsec);

/* 零拷贝缓冲区 */
int zring_register_buffer(struct zring *zr, void *buf, size_t len);
int zring_unregister_buffer(struct zring *zr, uint32_t index);
struct zr_buffer *zring_get_buffer(struct zring *zr, uint32_t index);

/* 文件注册 */
int zring_register_file(struct zring *zr, int fd, uint32_t *index);
int zring_unregister_file(struct zring *zr, uint32_t index);

/* 回调注册 */
int zring_set_callback(struct zring *zr, uint64_t user_data, zr_callback_fn fn, void *data);
int zring_add_callback(struct zring *zr, uint64_t user_data, zr_callback_fn fn, void *data);

/* 链式操作 */
int zring_link_sqes(struct zring *zr, struct zr_sqe *sqe1, struct zr_sqe *sqe2);

/* 处理循环 */
int zring_process(struct zring *zr);
int zring_process_batch(struct zring *zr, uint32_t count);
int zring_flush(struct zring *zr);

/* 统计 */
void zring_get_stats(struct zring *zr, uint64_t *requests, 
                     uint64_t *bytes, uint64_t *latency);

/* ========== 高级特性 ========== */

/* 自适应批处理 */
void zring_adaptive_batch_tune(struct zring *zr, uint64_t avg_latency);

/* 优先级感知调度 */
void zring_set_priority(struct zr_sqe *sqe, uint16_t prio);

/* 超时管理 */
int zring_cancel(struct zring *zr, uint64_t user_data);
int zring_cancel_all(struct zring *zr);

/* 零拷贝发送 */
int zring_zero_copy_send(struct zring *zr, int out_fd, int in_fd, 
                         size_t count, off_t *off_in, off_t *off_out);

/* ========== Zratro 原创增强: 零拷贝 API ========== */

/* 零拷贝发送 (使用注册缓冲区) */
void zring_prep_send_zc(struct zr_sqe *sqe, int fd, uint32_t buf_idx, 
                        uint32_t buf_off, uint32_t len);

/* 零拷贝接收 (使用注册缓冲区) */
void zring_prep_recv_zc(struct zr_sqe *sqe, int fd, uint32_t buf_idx,
                        uint32_t buf_off, uint32_t len);

/* 提供缓冲区给内核 */
void zring_prep_provide_buffers(struct zr_sqe *sqe, void *addr, 
                                 uint32_t len, uint32_t count, uint32_t bgid);

/* 移除缓冲区 */
void zring_prep_remove_buffers(struct zr_sqe *sqe, void *addr, 
                                uint32_t count, uint32_t bgid);

/* 零拷贝 sendfile (DMA 直通) */
int zring_sendfile_dma(struct zring *zr, int out_fd, int in_fd,
                       size_t count, off_t *off_in);

/* 注册持久缓冲区 (用于零拷贝) */
int zring_register_persistent_buffer(struct zring *zr, void *buf, 
                                      size_t len, uint32_t *idx);

/* 获取缓冲区物理地址 (DMA 用) */
uint64_t zring_get_buffer_phys(struct zring *zr, uint32_t idx);

/* ========== 内联辅助函数 ========== */

/* 环形缓冲区操作 */
static inline uint32_t zr_ring_next(struct zr_ring *ring, uint32_t idx, uint32_t mask) {
    return (idx + 1) & mask;
}

static inline int zr_ring_empty(struct zr_ring *ring) {
    return *ring->sq_head == *ring->sq_tail;
}

static inline uint32_t zr_ring_pending(struct zr_ring *ring) {
    return *ring->sq_tail - *ring->sq_head;
}

static inline uint32_t zr_ring_available(struct zr_ring *ring) {
    return ring->sq_size - zr_ring_pending(ring);
}

/* SQE 辅助 */
static inline void zr_sqe_set_data(struct zr_sqe *sqe, void *data) {
    sqe->user_data = (uint64_t)data;
}

static inline void *zr_sqe_get_data(struct zr_sqe *sqe) {
    return (void *)sqe->user_data;
}

/* CQE 辅助 */
static inline int64_t zr_cqe_result(struct zr_cqe *cqe) {
    return cqe->result;
}

static inline int zr_cqe_error(struct zr_cqe *cqe) {
    return cqe->flags & ZR_CQE_FLAG_ERROR ? (int)cqe->error : 0;
}

#endif /* ZRING_H */
