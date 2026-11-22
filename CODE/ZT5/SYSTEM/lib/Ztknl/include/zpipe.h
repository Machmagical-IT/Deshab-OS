/*
 * Zratro OS - Z-Pipe (零拷贝管道子系统)
 * 
 * 创新设计：
 * 1. DMA 直通 - 数据不经过 CPU 拷贝
 * 2. 共享内存页 - 进程间零拷贝通信
 * 3. 环形缓冲区 - 无锁生产者-消费者模式
 * 4. 事件通知 - 高效唤醒机制
 * 5. 背压控制 - 防止生产者过载
 */

#ifndef ZPIPE_H
#define ZPIPE_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== Z-Pipe 配置 ========== */

#define ZP_DEFAULT_SIZE        (64 * 1024)  /* 默认 64KB */
#define ZP_MAX_SIZE            (16 * 1024 * 1024)  /* 最大 16MB */
#define ZP_MIN_SIZE            4096          /* 最小 4KB */
#define ZP_MAX_READERS         16            /* 最大读者数 */
#define ZP_MAX_WRITERS         16            /* 最大写者数 */
#define ZP_MAX_CHANNELS        64            /* 最大通道数 */

/* ========== Z-Pipe 状态 ========== */

typedef enum {
    ZP_STATE_EMPTY = 0,          /* 空闲 */
    ZP_STATE_ACTIVE,             /* 活跃 */
    ZP_STATE_DRAINING,           /* 排空中 */
    ZP_STATE_CLOSED,             /* 已关闭 */
} zp_state_t;

/* ========== 环形缓冲区 ========== */

struct zp_ring_buffer {
    uint8_t *data;               /* 数据缓冲区 */
    uint64_t phys_addr;          /* 物理地址 (DMA 用) */
    uint32_t size;               /* 大小 */
    uint32_t mask;               /* 掩码 (size - 1, size 必须是 2 的幂) */
    
    /* 无锁指针 (使用原子操作) */
    volatile uint64_t head;      /* 读指针 */
    volatile uint64_t tail;      /* 写指针 */
    
    /* 统计 */
    uint64_t bytes_written;
    uint64_t bytes_read;
    uint64_t overflows;          /* 溢出次数 */
};

/* ========== DMA 描述符 ========== */

struct zp_dma_desc {
    uint64_t src_phys;           /* 源物理地址 */
    uint64_t dst_phys;           /* 目标物理地址 */
    uint32_t size;               /* 大小 */
    uint32_t flags;              /* 标志 */
    uint64_t completion_addr;    /* 完成通知地址 */
};

/* DMA 标志 */
#define ZP_DMA_FLAG_WRITE    (1 << 0)   /* 写操作 (设备到内存) */
#define ZP_DMA_FLAG_READ     (1 << 1)   /* 读操作 (内存到设备) */
#define ZP_DMA_FLAG_INTR     (1 << 2)   /* 完成中断 */
#define ZP_DMA_FLAG_CHAIN    (1 << 3)   /* 链式描述符 */

/* ========== 共享内存通道 ========== */

struct zp_channel {
    uint32_t channel_id;         /* 通道 ID */
    char name[32];               /* 通道名称 */
    zp_state_t state;            /* 状态 */
    uint32_t flags;              /* 标志 */
    
    /* 环形缓冲区 */
    struct zp_ring_buffer ring;
    
    /* 访问控制 */
    pid_t owner_pid;             /* 所有者 PID */
    pid_t readers[ZP_MAX_READERS]; /* 读者 PID */
    pid_t writers[ZP_MAX_WRITERS]; /* 写者 PID */
    uint32_t reader_count;
    uint32_t writer_count;
    
    /* 事件通知 */
    struct {
        uint32_t read_ready;     /* 可读事件 */
        uint32_t write_ready;    /* 可写事件 */
        uint32_t error;          /* 错误事件 */
    } events;
    
    /* 背压控制 */
    uint32_t high_watermark;     /* 高水位 (字节) */
    uint32_t low_watermark;      /* 低水位 (字节) */
    volatile int blocked;        /* 阻塞状态 */
    
    /* 统计 */
    uint64_t total_bytes;
    uint64_t total_msgs;
    
    /* 锁 */
    spinlock_t lock;
    
    /* 链表 */
    struct list_head list;
};

/* 通道标志 */
#define ZP_CH_FLAG_DMA      (1 << 0)   /* DMA 模式 */
#define ZP_CH_FLAG_NONBLOCK (1 << 1)   /* 非阻塞 */
#define ZP_CH_FLAG_EXCLUSIVE (1 << 2)  /* 独占模式 */

/* ========== 零拷贝 I/O 向量 ========== */

struct zp_iov {
    uint64_t phys_addr;          /* 物理地址 */
    uint32_t size;               /* 大小 */
    uint32_t flags;              /* 标志 */
};

/* 零拷贝消息 */
struct zp_msg {
    struct zp_iov *iovs;         /* I/O 向量数组 */
    uint32_t iov_count;          /* 向量数 */
    uint32_t total_size;         /* 总大小 */
    uint64_t timestamp;          /* 时间戳 */
    uint32_t flags;              /* 标志 */
};

/* 消息标志 */
#define ZP_MSG_FLAG_MORE    (1 << 0)   /* 更多数据待发送 */
#define ZP_MSG_FLAG_DMA     (1 << 1)   /* DMA 传输 */
#define ZP_MSG_FLAG_PEEK    (1 << 2)   /* 窥视 (不移除) */

/* ========== DMA 引擎 ========== */

struct zp_dma_engine {
    /* DMA 描述符环 */
    struct zp_dma_desc *desc_ring;
    uint32_t desc_count;
    volatile uint32_t desc_head;
    volatile uint32_t desc_tail;
    
    /* 完成队列 */
    uint64_t *completion_ring;
    uint32_t completion_count;
    volatile uint32_t completion_head;
    volatile uint32_t completion_tail;
    
    /* 统计 */
    uint64_t transfers;
    uint64_t bytes_transferred;
    uint64_t errors;
    
    /* 锁 */
    spinlock_t lock;
};

/* ========== Z-Pipe 管理器 ========== */

struct zpipe {
    /* 通道管理 */
    struct list_head channels;
    uint32_t channel_count;
    uint32_t next_channel_id;
    
    /* DMA 引擎 */
    struct zp_dma_engine dma;
    int dma_available;
    
    /* 全局共享内存池 */
    void *shared_pool;
    uint64_t shared_pool_phys;
    uint64_t shared_pool_size;
    
    /* 统计 */
    uint64_t total_channels;
    uint64_t total_bytes;
    
    /* 锁 */
    spinlock_t lock;
};

/* ========== 公共 API ========== */

/* 初始化/销毁 */
int zpipe_init(struct zpipe *zp, uint64_t shared_pool_size);
void zpipe_destroy(struct zpipe *zp);

/* 通道管理 */
struct zp_channel *zpipe_create_channel(struct zpipe *zp, const char *name,
                                         uint32_t size, uint32_t flags);
int zpipe_destroy_channel(struct zpipe *zp, uint32_t channel_id);
struct zp_channel *zpipe_find_channel(struct zpipe *zp, const char *name);
struct zp_channel *zpipe_get_channel(struct zpipe *zp, uint32_t channel_id);
void zpipe_put_channel(struct zp_channel *ch);

/* 通道访问 */
int zpipe_channel_add_reader(struct zp_channel *ch, pid_t pid);
int zpipe_channel_add_writer(struct zp_channel *ch, pid_t pid);
int zpipe_channel_remove_reader(struct zp_channel *ch, pid_t pid);
int zpipe_channel_remove_writer(struct zp_channel *ch, pid_t pid);

/* ========== 零拷贝数据传输 ========== */

/* 写入数据 (零拷贝) */
int zpipe_write_zc(struct zp_channel *ch, const void *data, uint32_t size);
int zpipe_writev_zc(struct zp_channel *ch, const struct zp_iov *iovs, uint32_t count);
int zpipe_write_msg(struct zp_channel *ch, struct zp_msg *msg);

/* 读取数据 (零拷贝) */
int zpipe_read_zc(struct zp_channel *ch, void *data, uint32_t size);
int zpipe_readv_zc(struct zp_channel *ch, struct zp_iov *iovs, uint32_t *count);
int zpipe_read_msg(struct zp_channel *ch, struct zp_msg *msg);

/* 窥视数据 (不移除) */
int zpipe_peek_zc(struct zp_channel *ch, void *data, uint32_t size);

/* 获取可读/可写空间 */
uint32_t zpipe_readable(struct zp_channel *ch);
uint32_t zpipe_writable(struct zp_channel *ch);

/* ========== DMA 直通 ========== */

/* DMA 传输 (设备到通道) */
int zpipe_dma_from_device(struct zp_channel *ch, uint64_t device_addr,
                           uint32_t size, uint64_t completion);

/* DMA 传输 (通道到设备) */
int zpipe_dma_to_device(struct zp_channel *ch, uint64_t device_addr,
                         uint32_t size, uint64_t completion);

/* DMA 传输 (通道到通道) */
int zpipe_dma_channel_to_channel(struct zp_channel *src, struct zp_channel *dst,
                                  uint32_t size);

/* 等待 DMA 完成 */
int zpipe_dma_wait_completion(struct zpipe *zp, uint64_t timeout);

/* ========== 事件通知 ========== */

/* 等待可读 */
int zpipe_wait_readable(struct zp_channel *ch, uint64_t timeout);

/* 等待可写 */
int zpipe_wait_writable(struct zp_channel *ch, uint64_t timeout);

/* 通知事件 */
void zpipe_notify_read(struct zp_channel *ch);
void zpipe_notify_write(struct zp_channel *ch);
void zpipe_notify_error(struct zp_channel *ch, int error);

/* ========== 背压控制 ========== */

/* 设置水位 */
int zpipe_set_watermarks(struct zp_channel *ch, uint32_t high, uint32_t low);

/* 检查阻塞状态 */
int zpipe_is_blocked(struct zp_channel *ch);

/* 冲刷缓冲区 */
int zpipe_flush(struct zp_channel *ch);

/* ========== 统计和调试 ========== */

/* 获取统计 */
void zpipe_get_stats(struct zpipe *zp, uint64_t *channels, 
                     uint64_t *bytes, uint64_t *transfers);

/* 获取通道统计 */
void zpipe_channel_stats(struct zp_channel *ch, uint64_t *written,
                          uint64_t *read, uint64_t *msgs);

/* ========== 全局管理器 ========== */

/* 获取全局管理器 */
struct zpipe *zpipe_get_global(void);

/* 初始化全局管理器 */
int zpipe_global_init(uint64_t shared_pool_size);

/* ========== 内联辅助函数 ========== */

/* 环形缓冲区操作 (无锁) */
static inline uint32_t zp_ring_free(struct zp_ring_buffer *ring) {
    return ring->size - (ring->tail - ring->head);
}

static inline uint32_t zp_ring_used(struct zp_ring_buffer *ring) {
    return ring->tail - ring->head;
}

static inline int zp_ring_empty(struct zp_ring_buffer *ring) {
    return ring->head == ring->tail;
}

static inline int zp_ring_full(struct zp_ring_buffer *ring) {
    return (ring->tail - ring->head) >= ring->size;
}

/* 环形缓冲区索引计算 */
static inline uint32_t zp_ring_index(struct zp_ring_buffer *ring, uint64_t pos) {
    return pos & ring->mask;
}

/* 获取物理地址 (DMA 用) */
static inline uint64_t zp_ring_phys(struct zp_ring_buffer *ring, uint64_t pos) {
    return ring->phys_addr + zp_ring_index(ring, pos);
}

/* 获取虚拟地址 */
static inline void *zp_ring_virt(struct zp_ring_buffer *ring, uint64_t pos) {
    return ring->data + zp_ring_index(ring, pos);
}

#endif /* ZPIPE_H */
