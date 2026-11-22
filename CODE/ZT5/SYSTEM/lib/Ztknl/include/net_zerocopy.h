/*
 * Zratro OS - Zero-Copy Network Extension (零拷贝网络扩展)
 * 
 * 创新设计：
 * 1. Z-Pipe 集成 - 网络缓冲区使用 Z-Pipe 环形缓冲区
 * 2. DMA 直通 - 网卡直接访问内存
 * 3. 页翻转 - 接收零拷贝
 * 4. 批量操作 - sendmmsg/recvmmsg 高效批处理
 * 5. Scatter-Gather - 分散/聚集 I/O
 */

#ifndef NET_ZEROCOPY_H
#define NET_ZEROCOPY_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "zpipe.h"
#include "socket.h"  /* socklen_t */

/* ========== 配置 ========== */

#define NZC_MAX_IOV             16      /* 最大 iovec 数量 */
#define NZC_MAX_BATCH           32      /* 最大批处理数量 */
#define NZC_MAX_DESCRIPTORS     1024    /* 最大描述符数量 */
#define NZC_RING_SIZE           (64 * 1024)  /* 环形缓冲区大小 */

/* ========== 零拷贝标志 ========== */

#define NZC_FLAG_ZEROCOPY       (1 << 0)  /* 启用零拷贝 */
#define NZC_FLAG_DMA            (1 << 1)  /* DMA 直通 */
#define NZC_FLAG_BATCH          (1 << 2)  /* 批量操作 */
#define NZC_FLAG_NOTIFICATION   (1 << 3)  /* 完成通知 */
#define NZC_FLAG_SHARED         (1 << 4)  /* 共享内存 */

/* ========== 分散/聚集 I/O ========== */

struct nzc_iovec {
    void *iov_base;             /* 基地址 */
    size_t iov_len;             /* 长度 */
    uint64_t iov_phys;          /* 物理地址 (DMA 用) */
    uint32_t iov_flags;         /* 标志 */
};

/* iovec 标志 */
#define NZC_IOV_READ            (1 << 0)
#define NZC_IOV_WRITE           (1 << 1)
#define NZC_IOV_DMA_READY       (1 << 2)

/* ========== 消息头 ========== */

struct nzc_msghdr {
    void *msg_name;             /* 地址 */
    socklen_t msg_namelen;      /* 地址长度 */
    struct nzc_iovec *msg_iov;  /* iovec 数组 */
    size_t msg_iovlen;          /* iovec 数量 */
    void *msg_control;          /* 控制信息 */
    size_t msg_controllen;      /* 控制信息长度 */
    int msg_flags;              /* 消息标志 */
};

/* ========== 批量消息 ========== */

struct nzc_mmsghdr {
    struct nzc_msghdr msg_hdr;  /* 消息头 */
    unsigned int msg_len;       /* 实际发送/接收字节数 */
};

/* ========== 零拷贝描述符 ========== */

struct nzc_descriptor {
    uint64_t desc_id;           /* 描述符 ID */
    uint32_t flags;             /* 标志 */
    
    /* 内存区域 */
    void *base;
    uint64_t phys_addr;
    size_t size;
    
    /* 引用计数 */
    int ref_count;
    
    /* 状态 */
    int in_use;
    int pending_tx;             /* 待发送 */
    
    /* 关联的 socket */
    int sockfd;
    
    struct list_head list;
};

/* ========== 完成通知 ========== */

struct nzc_completion {
    uint64_t cookie;            /* 用户 cookie */
    uint64_t desc_id;           /* 描述符 ID */
    int status;                 /* 状态: 0=成功, <0=错误 */
    uint32_t bytes;             /* 传输字节数 */
    uint32_t flags;             /* 标志 */
};

/* ========== 零拷贝环形缓冲区 ========== */

struct nzc_ring {
    /* 发送环形 */
    struct zp_ring_buffer tx_ring;
    
    /* 接收环形 */
    struct zp_ring_buffer rx_ring;
    
    /* 完成队列 */
    struct nzc_completion *completions;
    uint32_t comp_size;
    volatile uint32_t comp_head;
    volatile uint32_t comp_tail;
    
    /* 统计 */
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint64_t tx_packets;
    uint64_t rx_packets;
};

/* ========== 零拷贝 socket 扩展 ========== */

struct nzc_socket_ext {
    int sockfd;
    
    /* 零拷贝环形缓冲区 */
    struct nzc_ring ring;
    
    /* 描述符池 */
    struct list_head descriptors;
    uint32_t desc_count;
    
    /* DMA 映射 */
    void *dma_region;
    uint64_t dma_phys;
    size_t dma_size;
    
    /* 配置 */
    uint32_t flags;
    uint32_t batch_size;
    
    /* 统计 */
    uint64_t zero_copy_tx;
    uint64_t zero_copy_rx;
    uint64_t fallback_tx;
    uint64_t fallback_rx;
    
    struct list_head list;
};

/* ========== 零拷贝管理器 ========== */

struct nzc_manager {
    /* socket 扩展列表 */
    struct list_head sockets;
    uint32_t socket_count;
    
    /* 描述符池 */
    struct list_head free_descriptors;
    uint32_t desc_pool_size;
    
    /* DMA 缓冲区池 */
    void *dma_pool;
    uint64_t dma_pool_phys;
    size_t dma_pool_size;
    
    /* 配置 */
    uint32_t default_flags;
    size_t default_ring_size;
    
    /* 统计 */
    uint64_t total_zc_tx;
    uint64_t total_zc_rx;
    
    /* 锁 */
    spinlock_t lock;
};

/* ========== 公共 API ========== */

/* 初始化 */
int nzc_init(struct nzc_manager *mgr);
void nzc_destroy(struct nzc_manager *mgr);

/* ========== Socket 扩展 ========== */

/* 启用零拷贝 */
int nzc_enable(int sockfd, uint32_t flags);

/* 禁用零拷贝 */
int nzc_disable(int sockfd);

/* 获取扩展 */
struct nzc_socket_ext *nzc_get_ext(int sockfd);

/* ========== 零拷贝操作 ========== */

/* 零拷贝发送 */
ssize_t nzc_send(int sockfd, const void *buf, size_t len, int flags);

/* 零拷贝发送消息 */
ssize_t nzc_sendmsg(int sockfd, const struct nzc_msghdr *msg, int flags);

/* 批量发送 */
int nzc_sendmmsg(int sockfd, struct nzc_mmsghdr *msgvec, unsigned int vlen, int flags);

/* 零拷贝接收 */
ssize_t nzc_recv(int sockfd, void *buf, size_t len, int flags);

/* 零拷贝接收消息 */
ssize_t nzc_recvmsg(int sockfd, struct nzc_msghdr *msg, int flags);

/* 批量接收 */
int nzc_recvmmsg(int sockfd, struct nzc_mmsghdr *msgvec, unsigned int vlen, int flags);

/* ========== 描述符管理 ========== */

/* 注册内存区域 */
struct nzc_descriptor *nzc_register_region(int sockfd, void *base, size_t size);

/* 注销内存区域 */
int nzc_unregister_region(struct nzc_descriptor *desc);

/* 增加引用 */
int nzc_desc_get(struct nzc_descriptor *desc);

/* 释放引用 */
int nzc_desc_put(struct nzc_descriptor *desc);

/* ========== 完成通知 ========== */

/* 等待完成 */
int nzc_wait_completion(int sockfd, struct nzc_completion *comp, uint64_t timeout);

/* 批量等待完成 */
int nzc_wait_completions(int sockfd, struct nzc_completion *comps, 
                          uint32_t count, uint64_t timeout);

/* 轮询完成 */
int nzc_poll_completions(int sockfd, struct nzc_completion *comps, uint32_t count);

/* ========== DMA 操作 ========== */

/* DMA 映射 */
void *nzc_dma_map(void *virt, size_t size, uint64_t *phys);

/* DMA 解除映射 */
int nzc_dma_unmap(void *virt, size_t size);

/* DMA 同步 (缓存一致性) */
int nzc_dma_sync(void *virt, size_t size, int direction);

#define NZC_DMA_TO_DEVICE    0
#define NZC_DMA_FROM_DEVICE  1
#define NZC_DMA_BIDIRECTIONAL 2

/* ========== Scatter-Gather ========== */

/* SG 发送 */
ssize_t nzc_send_sg(int sockfd, struct nzc_iovec *iov, size_t count, int flags);

/* SG 接收 */
ssize_t nzc_recv_sg(int sockfd, struct nzc_iovec *iov, size_t count, int flags);

/* ========== 统计 ========== */

/* 获取统计 */
void nzc_get_stats(struct nzc_manager *mgr,
                   uint64_t *zc_tx, uint64_t *zc_rx,
                   uint64_t *fallback);

/* 重置统计 */
void nzc_reset_stats(struct nzc_manager *mgr);

/* ========== sendfile/splice 零拷贝 ========== */

/* sendfile: 在文件描述符和套接字之间零拷贝传输 */
ssize_t nzc_sendfile(int sockfd, int file_fd, off_t *offset, size_t count);

/* splice: 在两个文件描述符之间零拷贝传输 */
ssize_t nzc_splice(int fd_in, int fd_out, size_t len);

/* ========== NAPI 轮询 ========== */

/* NAPI 状态 */
#define NAPI_STATE_SCHED    0x01    /* 已调度 */
#define NAPI_STATE_DISABLE  0x02    /* 已禁用 */
#define NAPI_STATE_NPS      0x04    /* 在轮询调度中 */
#define NAPI_STATE_POLL     0x08    /* 轮询模式 (高流量) */

/* NAPI 轮询配置 */
#define NAPI_POLL_WEIGHT    64      /* 每次轮询最大处理包数 */
#define NAPI_POLL_BUDGET    300     /* 每轮总预算 */

/* NAPI 中断/轮询混合模式阈值 */
#define NAPI_POLL_THRESHOLD     16      /* 连续高流量包数阈值，超过则切换到轮询 */
#define NAPI_INTERRUPT_IDLE_MS  50      /* 轮询模式下空闲超时 (ms)，超时则切回中断 */

struct napi_struct {
    int poll_list;              /* 是否在轮询列表中 */
    int state;                  /* NAPI 状态 */
    int weight;                 /* 轮询权重 */
    int (*poll)(void *priv, int budget);  /* 轮询回调 */
    void *priv;                 /* 驱动私有数据 */
    struct nic_device *nic;     /* 关联的网卡 */
    
    /* 中断/轮询混合模式 */
    int high_traffic_count;     /* 连续高流量计数 */
    uint64_t last_poll_time;    /* 上次轮询时间 */
    int poll_mode;              /* 0=中断模式, 1=轮询模式 */
};

/* 初始化 NAPI */
void napi_init(struct napi_struct *napi, struct nic_device *nic,
               int (*poll)(void *, int), int weight);

/* 调度 NAPI 轮询 */
void napi_schedule(struct napi_struct *napi);

/* 完成 NAPI 轮询 */
void napi_complete(struct napi_struct *napi);

/* NAPI 轮询主循环 */
int napi_poll_all(int budget);

/* ========== GRO/GSO (通用接收/发送卸载) ========== */

/* GRO 最大合并段数 */
#define GRO_MAX_SEGS       16

/* GRO 合并结果 */
struct gro_result {
    struct net_buf *merged;     /* 合并后的缓冲区 */
    int seg_count;              /* 合并的段数 */
    int same_flow;              /* 是否同流 */
};

/* GRO 节点 */
struct gro_node {
    struct net_buf *buf;        /* 缓冲区 */
    uint32_t flow_hash;         /* 流哈希 */
    uint32_t seq;               /* TCP 序列号 */
    uint16_t mss;               /* MSS */
    uint8_t  protocol;          /* 协议 */
    int seg_count;              /* 已合并段数 */
    struct gro_node *next;
};

/* GRO 引擎 */
struct gro_engine {
    struct gro_node flows[8];   /* 最多 8 个流 */
    int flow_count;
    int max_segs;               /* 最大合并段数 */
    struct nic_device *nic;     /* 关联的网卡设备 (用于刷新时传递给上层) */
};

/* 初始化 GRO 引擎 */
void gro_init(struct gro_engine *engine);

/* GRO 接收处理 */
struct gro_result gro_receive(struct gro_engine *engine, struct net_buf *buf);

/* GRO 刷新 (超时后发送合并结果) */
void gro_flush(struct gro_engine *engine);

/* GSO 最大段大小 */
#define GSO_MAX_SIZE        65536

/* GSO 上下文 */
struct gso_ctx {
    uint32_t mss;               /* 最大段大小 */
    uint32_t total_len;         /* 总数据长度 */
    uint8_t  protocol;          /* 协议 */
    int hdr_len;                /* 头部长度 */
};

/* GSO 分段发送 */
int gso_segment(struct nic_device *nic, struct gso_ctx *ctx,
                const void *data, size_t len);

/* ========== SNMP 网络统计 ========== */

struct net_snmp_stats {
    /* IP 层统计 */
    uint64_t ip_in_receives;
    uint64_t ip_in_hdr_errors;
    uint64_t ip_in_addr_errors;
    uint64_t ip_forw_datagrams;
    uint64_t ip_in_unknown_protos;
    uint64_t ip_in_discards;
    uint64_t ip_in_delivers;
    uint64_t ip_out_requests;
    uint64_t ip_out_discards;
    uint64_t ip_out_no_routes;
    uint64_t ip_reasm_timeout;
    uint64_t ip_reasm_reqds;
    uint64_t ip_reasm_oks;
    uint64_t ip_reasm_fails;
    uint64_t ip_frag_oks;
    uint64_t ip_frag_fails;
    uint64_t ip_frag_creates;
    
    /* TCP 层统计 */
    uint64_t tcp_active_opens;
    uint64_t tcp_passive_opens;
    uint64_t tcp_attempt_fails;
    uint64_t tcp_estab_resets;
    uint64_t tcp_in_segs;
    uint64_t tcp_out_segs;
    uint64_t tcp_retrans_segs;
    uint64_t tcp_in_errors;
    uint64_t tcp_out_rsts;
    
    /* UDP 层统计 */
    uint64_t udp_in_datagrams;
    uint64_t udp_no_ports;
    uint64_t udp_in_errors;
    uint64_t udp_out_datagrams;
    
    /* ICMP 层统计 */
    uint64_t icmp_in_msgs;
    uint64_t icmp_in_errors;
    uint64_t icmp_out_msgs;
    uint64_t icmp_out_errors;
    
    /* 接口统计 */
    uint64_t if_in_octets;
    uint64_t if_out_octets;
    uint64_t if_in_ucast_pkts;
    uint64_t if_out_ucast_pkts;
    uint64_t if_in_discards;
    uint64_t if_out_discards;
    uint64_t if_in_errors;
    uint64_t if_out_errors;
};

/* 获取全局 SNMP 统计 */
struct net_snmp_stats *net_get_snmp_stats(void);

/* 更新 SNMP 统计 */
void net_snmp_inc_ip(int field);
void net_snmp_inc_tcp(int field);
void net_snmp_inc_udp(int field);
void net_snmp_inc_icmp(int field);
void net_snmp_inc_if(int field);

/* 打印 SNMP 统计 */
void net_snmp_print(void);

/* ========== 内联辅助 ========== */

/* 计算对齐大小 */
static inline size_t nzc_align_size(size_t size) {
    return (size + 4095) & ~4095;
}

/* 检查零拷贝是否可用 */
static inline int nzc_can_zerocopy(struct nzc_socket_ext *ext, size_t len) {
    if (ext == NULL) return 0;
    if (!(ext->flags & NZC_FLAG_ZEROCOPY)) return 0;
    if (len < 4096) return 0;  /* 小包不使用零拷贝 */
    return 1;
}

/* 全局管理器 */
struct nzc_manager *nzc_get_global(void);
int nzc_global_init(void);

#endif /* NET_ZEROCOPY_H */
