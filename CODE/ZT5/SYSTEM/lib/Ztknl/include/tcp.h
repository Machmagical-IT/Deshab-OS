/*
 * Zratro OS - TCP Protocol Implementation
 *
 * 完整的 TCP 状态机和拥塞控制
 */

#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include <stddef.h>
#include "net.h"      /* TCP 标志、IP/TCP 头部结构统一定义在 net.h */
#include "socket.h"

/* TCP 标志 - 已在 net.h 中统一定义 (TCP_FIN, TCP_SYN, TCP_RST 等) */

/* TCP 选项 */
#define TCP_OPT_EOL     0
#define TCP_OPT_NOP     1
#define TCP_OPT_MSS     2
#define TCP_OPT_WSCALE  3
#define TCP_OPT_SACKOK  4
#define TCP_OPT_SACK    5
#define TCP_OPT_TIMESTAMP   8

/* TCP 状态 */
typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
} tcp_state_t;

/* TCP 连接标志 */
#define TCP_FLAG_ACK_DELAYED    (1 << 0)
#define TCP_FLAG_ACK_NOW        (1 << 1)
#define TCP_FLAG_PSH            (1 << 2)
#define TCP_FLAG_FIN_PENDING    (1 << 3)
#define TCP_FLAG_TX_QUEUED      (1 << 4)

/* TCP 配置 */
#define TCP_MSS_DEFAULT     1460
#define TCP_MSS_MIN         536
#define TCP_WINDOW_DEFAULT  65535
#define TCP_WINDOW_MAX      1073741824UL
#define TCP_RTT_BASE        1000        /* 基础 RTT (ms) */
#define TCP_RTO_MIN         200         /* 最小 RTO (ms) */
#define TCP_RTO_MAX         120000      /* 最大 RTO (ms) */
#define TCP_RTO_INITIAL     1000        /* 初始 RTO (ms) */
#define TCP_TIME_WAIT_TIMEOUT 60000     /* TIME_WAIT 超时 60秒 (2*MSL) */
#define TCP_SYN_RETRIES     5
#define TCP_SYNACK_RETRIES  5
#define TCP_FIN_RETRIES     5
#define TCP_TIMEWAIT_DELAY  60000       /* TIME_WAIT 持续时间 (ms) */

/* Keep-Alive 默认配置 (RFC 1122) */
#define TCP_KEEPALIVE_IDLE_DEFAULT    7200000   /* 2小时 (ms) */
#define TCP_KEEPALIVE_INTERVAL_DEFAULT 75000    /* 75秒 (ms) */
#define TCP_KEEPALIVE_COUNT_DEFAULT   9         /* 探测次数 */

/* Nagle 算法延迟 */
#define TCP_NAGLE_DELAY    200         /* Nagle 延迟 200ms */

/* 乱序重排队列最大段数 */
#define TCP_OOQ_MAX_SEGS   64

/* 拥塞控制算法类型 */
#define TCP_CC_RENO        0           /* TCP Reno */
#define TCP_CC_CUBIC       1           /* TCP CUBIC */

/* TCP 选项最大长度 */
#define TCP_OPT_MAX_LEN    40

/* IP/以太网头部 - 使用 net.h 中的统一定义 (struct ip_hdr / struct eth_hdr) */
/* 兼容别名已在 net.h 中定义: ip_header_t = struct ip_hdr */

/* TCP 头部 - 使用 net.h 中的统一定义 (struct tcp_hdr) */
/* 兼容别名: */
typedef struct tcp_hdr tcp_header_t;

/* TCP 选项 */
struct tcp_option {
    uint8_t kind;
    uint8_t length;
    uint8_t data[];
} __attribute__((packed));

/* 乱序重排队列段 */
struct tcp_ooseg {
    uint32_t seq;           /* 起始序列号 */
    uint32_t len;           /* 数据长度 */
    uint8_t *data;          /* 数据指针 */
    struct tcp_ooseg *next; /* 下一个段 */
};

/* 拥塞控制状态 */
typedef enum {
    TCP_CC_SLOW_START = 0,
    TCP_CC_CONGESTION_AVOIDANCE,
    TCP_CC_FAST_RECOVERY,
    TCP_CC_DISORDER
} tcp_cc_state_t;

/* CUBIC 拥塞控制参数 */
struct tcp_cubic_params {
    uint64_t w_max;         /* 窗口最大值 (CUBIC) */
    uint64_t k;             /* K 值 (CUBIC) */
    uint64_t epoch_start;   /* 纪元开始时间 (ms) */
    uint64_t origin_point;  /* 原点 (CUBIC) */
    uint32_t tcp_cwnd;      /* TCP 友好窗口 */
    int32_t  beta;          /* 乘法减少因子 * 1024 */
};

/* TCP 控制块 */
struct tcp_pcb {
    /* 连接信息 */
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;
    
    /* 状态 */
    tcp_state_t state;
    uint32_t flags;
    
    /* 序列号 */
    uint32_t snd_una;        /* 最早未确认 */
    uint32_t snd_nxt;        /* 下一个发送 */
    uint32_t snd_max;        /* 已发送的最大序列号 */
    uint32_t snd_wnd;        /* 发送窗口 */
    uint32_t snd_up;         /* 紧急指针 */
    uint32_t snd_wl1;        /* 窗口更新 seq */
    uint32_t snd_wl2;        /* 窗口更新 ack */
    uint32_t iss;            /* 初始序列号 */
    
    /* 接收序列 */
    uint32_t rcv_nxt;        /* 期望接收 */
    uint32_t rcv_wnd;        /* 接收窗口 */
    uint32_t rcv_up;         /* 紧急指针 */
    uint32_t irs;            /* 初始接收序列号 */
    
    /* RTT 估算 */
    uint32_t rtt_seq;        /* RTT 测量序列号 */
    uint64_t rtt_start;      /* RTT 测量开始时间 */
    uint32_t srtt;           /* 平滑 RTT (ms * 8) */
    uint32_t rttvar;         /* RTT 方差 (ms * 4) */
    uint32_t rto;            /* 重传超时 (ms) */
    
    /* 拥塞控制 - 统一实现 */
    uint32_t cwnd;           /* 拥塞窗口 */
    uint32_t ssthresh;       /* 慢启动阈值 */
    uint32_t recover;        /* 恢复序列号 (快速恢复用) */
    uint8_t  dupacks;        /* 重复 ACK 计数 */
    tcp_cc_state_t cc_state; /* 拥塞控制状态 */
    uint8_t  cc_algo;        /* 拥塞控制算法 (TCP_CC_RENO/CUBIC) */
    struct tcp_cubic_params cubic; /* CUBIC 参数 */
    
    /* MSS 和窗口缩放 */
    uint16_t mss;            /* 最大段大小 */
    uint8_t  snd_wscale;     /* 发送窗口缩放 */
    uint8_t  rcv_wscale;     /* 接收窗口缩放 */
    uint8_t  wscale_requested; /* 是否请求了窗口缩放 */
    
    /* SACK (选择性确认) */
    uint8_t  sack_count;     /* SACK 块数量 */
    uint8_t  sack_permitted; /* SACK 允许标志 */
    uint8_t  nodelay;        /* Nagle 禁用标志 */
    uint8_t  keep_enabled;   /* Keep-Alive 启用标志 */
    struct {
        uint32_t start;
        uint32_t end;
    } sack_blocks[4];        /* SACK 块 */
    
    /* 乱序重排队列 */
    struct tcp_ooseg *ooseq; /* 乱序段链表头 */
    uint32_t ooseq_count;    /* 乱序段数量 */
    
    /* 缓冲区 */
    struct ring_buffer *snd_buf;   /* 发送缓冲区 */
    struct ring_buffer *rcv_buf;   /* 接收缓冲区 */
    
    /* 回调函数 */
    void (*on_connect)(struct tcp_pcb *pcb);
    void (*on_receive)(struct tcp_pcb *pcb, void *data, size_t len);
    void (*on_sent)(struct tcp_pcb *pcb, uint16_t len);
    void (*on_error)(struct tcp_pcb *pcb, int err);
    
    void *user_data;
    
    /* 定时器 */
    uint64_t retry_time;     /* 重传时间 */
    int retry_count;         /* 重传次数 */
    uint64_t keepalive_time; /* 保活时间 */
    uint64_t last_activity;  /* 最后活动时间 */
    uint64_t last_keep_probe;/* 最后 Keep-Alive 探测时间 */
    int keep_idle;           /* Keep-Idle 时间 (ms) */
    int keep_interval;       /* Keep-Interval (ms) */
    int keep_count;          /* Keep-Count */
    int keep_probes;         /* Keep-Probes 计数 */
    
    /* Nagle 算法 */
    uint64_t nagle_time;     /* Nagle 等待开始时间 */
    
    /* 链表 */
    struct tcp_pcb *next;
};

/* TCP 伪头部 (用于校验和计算) */
struct tcp_pseudo_header {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  reserved;
    uint8_t  protocol;
    uint16_t length;
} __attribute__((packed));

/* ========== TCP 控制块管理 ========== */

/* 创建 TCP PCB */
struct tcp_pcb *tcp_new(void);

/* 绑定 TCP PCB */
int tcp_bind(struct tcp_pcb *pcb, uint32_t addr, uint16_t port);

/* 监听 */
int tcp_listen(struct tcp_pcb *pcb, int backlog);

/* 连接 */
int tcp_connect(struct tcp_pcb *pcb, uint32_t addr, uint16_t port);

/* 接受连接 */
struct tcp_pcb *tcp_accept(struct tcp_pcb *pcb);

/* 关闭 */
int tcp_close(struct tcp_pcb *pcb);

/* 销毁 */
void tcp_free(struct tcp_pcb *pcb);

/* ========== 数据传输 ========== */

/* 发送数据 */
int tcp_send(struct tcp_pcb *pcb, const void *data, size_t len);

/* 接收数据 */
int tcp_recv(struct tcp_pcb *pcb, void *buf, size_t len);

/* 发送紧急数据 */
int tcp_urgent(struct tcp_pcb *pcb, const void *data, size_t len);

/* ========== TCP 输入处理 ========== */

/* 处理输入的 TCP 段 */
void tcp_input(struct net_buf *buf, struct ip_hdr *ip);

/* 处理 TCP 选项 */
void tcp_parse_options(struct tcp_pcb *pcb, uint8_t *options, int len);

/* ========== TCP 输出处理 ========== */

/* 发送 TCP 段 */
int tcp_output(struct tcp_pcb *pcb);

/* 发送 ACK */
void tcp_send_ack(struct tcp_pcb *pcb);

/* 发送 SYN */
int tcp_send_syn(struct tcp_pcb *pcb);

/* 发送 FIN */
int tcp_send_fin(struct tcp_pcb *pcb);

/* 发送 RST */
void tcp_send_reset(struct tcp_pcb *pcb, uint32_t seq);

/* 重传 */
void tcp_retransmit(struct tcp_pcb *pcb);

/* SACK 驱动的选择性重传 */
void tcp_retransmit_sack(struct tcp_pcb *pcb);

/* ========== 拥塞控制 ========== */

/* 初始化拥塞控制 */
void tcp_cc_init(struct tcp_pcb *pcb);

/* 收到新 ACK 时的拥塞控制处理 (统一入口) */
void tcp_cc_on_ack(struct tcp_pcb *pcb, uint32_t acked);

/* 超时处理 */
void tcp_cc_timeout(struct tcp_pcb *pcb);

/* 设置拥塞控制算法 */
int tcp_set_cc_algo(struct tcp_pcb *pcb, int algo);

/* ========== 乱序重排队列 ========== */

/* 将乱序段插入队列 */
void tcp_ooseq_insert(struct tcp_pcb *pcb, uint32_t seq, const void *data, size_t len);

/* 将有序数据从队列中取出到接收缓冲区 */
void tcp_ooseq_dequeue(struct tcp_pcb *pcb);

/* 释放乱序队列 */
void tcp_ooseq_free(struct tcp_pcb *pcb);

/* ========== SACK 处理 ========== */

/* 处理收到的 SACK 选项 */
void tcp_process_sack(struct tcp_pcb *pcb, uint8_t *options, int len);

/* 构建 SACK 选项 */
int tcp_build_sack_option(struct tcp_pcb *pcb, uint8_t *options);

/* 更新 SACK 块 (收到新数据时) */
void tcp_sack_update(struct tcp_pcb *pcb);

/* 清除 SACK 块 */
void tcp_sack_clear(struct tcp_pcb *pcb);

/* ========== 窗口缩放 ========== */

/* 协商窗口缩放 (SYN 时) */
void tcp_wscale_negotiate(struct tcp_pcb *pcb, int is_syn);

/* 应用窗口缩放 */
uint32_t tcp_wscale_expand(uint16_t window, uint8_t wscale);

/* ========== Keep-Alive ========== */

/* 设置 Keep-Alive 参数 */
int tcp_set_keepalive(struct tcp_pcb *pcb, int idle, int interval, int count);

/* Keep-Alive 定时器处理 */
void tcp_keepalive_timer(struct tcp_pcb *pcb);

/* ========== Nagle 算法 ========== */

/* 检查 Nagle 算法是否允许发送 */
int tcp_nagle_check(struct tcp_pcb *pcb);

/* 设置 TCP_NODELAY */
int tcp_set_nodelay(struct tcp_pcb *pcb, int nodelay);

/* ========== RTT 估算 ========== */

/* 开始 RTT 测量 */
void tcp_rtt_start(struct tcp_pcb *pcb, uint32_t seq);

/* 结束 RTT 测量 */
void tcp_rtt_stop(struct tcp_pcb *pcb, uint32_t ack);

/* 更新 RTO */
void tcp_rtt_update(struct tcp_pcb *pcb, uint32_t rtt);

/* ========== 定时器 ========== */

/* TCP 定时器 (每 200ms 调用) */
void tcp_timer(void);

/* 检查超时 */
void tcp_check_timeouts(void);

/* ========== 统计 ========== */

/* 打印 TCP 统计 */
void tcp_stats(void);

/* 打印 PCB 信息 */
void tcp_pcb_dump(struct tcp_pcb *pcb);

/* ========== 校验和 ========== */

/* 计算 TCP 校验和 */
uint16_t tcp_checksum(struct ip_hdr *ip, struct tcp_hdr *tcp, 
                       const void *data, size_t len);

#endif /* TCP_H */
