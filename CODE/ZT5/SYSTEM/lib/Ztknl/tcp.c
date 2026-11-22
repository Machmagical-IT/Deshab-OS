/*
 * Zratro OS - TCP Protocol Implementation
 */

#include "tcp.h"
#include "net.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"
#include "ntdll.h"  /* for va_list */

/* stdarg 宏定义 */
#define va_start(v, l)  __builtin_va_start(v, l)
#define va_end(v)       __builtin_va_end(v)
#define va_arg(v, l)    __builtin_va_arg(v, l)

/* 环形缓冲区 */
struct ring_buffer {
    uint8_t *data;
    size_t size;
    size_t head;
    size_t tail;
};

/* TCP PCB 池 */
#define MAX_TCP_PCBS    256
static struct tcp_pcb tcp_pcb_pool[MAX_TCP_PCBS];
static struct tcp_pcb *tcp_pcb_free_list = NULL;
static struct tcp_pcb *tcp_pcb_active_list = NULL;
static volatile int tcp_lock = 0;

/* TCP 端口分配 */
static uint16_t next_ephemeral_port = 49152;
static uint8_t port_used[65536 / 8];

/* 统计 */
static uint64_t tcp_in_segs = 0;
static uint64_t tcp_out_segs = 0;
static uint64_t tcp_retrans_segs = 0;
static uint64_t tcp_in_errors = 0;

/* 前向声明 */
struct tcp_pcb *tcp_find_pcb(uint32_t local_addr, uint16_t local_port,
                              uint32_t remote_addr, uint16_t remote_port);
void tcp_send_empty_ack(struct tcp_pcb *pcb);
void tcp_abort(struct tcp_pcb *pcb);
size_t ringbuf_pending(struct ring_buffer *rb);
const char *tcp_state_name(tcp_state_t state);
int snprintf(char *str, size_t size, const char *format, ...);

/* ========== 环形缓冲区 ========== */

static struct ring_buffer *ringbuf_create(size_t size) {
    struct ring_buffer *rb = kmalloc(sizeof(*rb));
    if (!rb) return NULL;
    rb->data = kmalloc(size);
    if (rb->data == NULL) {
        kfree(rb);
        return NULL;
    }
    
    rb->size = size;
    rb->head = 0;
    rb->tail = 0;
    return rb;
}

static void ringbuf_destroy(struct ring_buffer *rb) {
    if (rb) {
        kfree(rb->data);
        kfree(rb);
    }
}

static size_t ringbuf_write(struct ring_buffer *rb, const void *data, size_t len) {
    if (rb == NULL || data == NULL || rb->data == NULL) return 0;
    
    /* 防止整数溢出 */
    size_t used = rb->head - rb->tail;
    if (used > rb->size) {
        /* 缓冲区状态损坏，记录错误并重置 */
        println("[TCP] WARNING: Ring buffer corruption detected (write)");
        print("  [TCP] used=");
        print_dec(used);
        print(", size=");
        print_dec(rb->size);
        println(", resetting...");
        rb->head = rb->tail = 0;
        return 0;
    }
    
    size_t available = rb->size - used;
    if (len > available) len = available;
    if (len == 0) return 0;
    
    /* 优化：批量复制而非逐字节 */
    size_t pos = rb->head % rb->size;
    size_t first_chunk = rb->size - pos;
    
    if (len <= first_chunk) {
        /* 单次复制 */
        memcpy(rb->data + pos, data, len);
    } else {
        /* 分两次复制（环形缓冲区绕回） */
        memcpy(rb->data + pos, data, first_chunk);
        memcpy(rb->data, (const uint8_t *)data + first_chunk, len - first_chunk);
    }
    
    rb->head += len;
    return len;
}

static size_t ringbuf_read(struct ring_buffer *rb, void *data, size_t len) {
    if (rb == NULL || data == NULL || rb->data == NULL) return 0;
    
    /* 防止整数溢出 */
    size_t available = rb->head - rb->tail;
    if (available > rb->size) {
        /* 缓冲区状态损坏，记录错误并重置 */
        println("[TCP] WARNING: Ring buffer corruption detected (read)");
        print("  [TCP] available=");
        print_dec(available);
        print(", size=");
        print_dec(rb->size);
        println(", resetting...");
        rb->head = rb->tail = 0;
        return 0;
    }
    
    if (len > available) len = available;
    if (len == 0) return 0;
    
    /* 优化：批量复制 */
    size_t pos = rb->tail % rb->size;
    size_t first_chunk = rb->size - pos;
    
    if (len <= first_chunk) {
        memcpy(data, rb->data + pos, len);
    } else {
        memcpy(data, rb->data + pos, first_chunk);
        memcpy((uint8_t *)data + first_chunk, rb->data, len - first_chunk);
    }
    
    rb->tail += len;
    return len;
}

static size_t ringbuf_peek(struct ring_buffer *rb, void *data, size_t len) {
    size_t available = rb->head - rb->tail;
    if (len > available) len = available;
    
    size_t old_tail = rb->tail;
    size_t read = ringbuf_read(rb, data, len);
    rb->tail = old_tail;
    
    return read;
}

/* ========== 辅助函数 ========== */

static uint16_t allocate_port(void) {
    for (int i = 0; i < 16384; i++) {
        uint16_t port = next_ephemeral_port++;
        if (next_ephemeral_port >= 65535) {
            next_ephemeral_port = 49152;
        }
        
        int idx = port / 8;
        int bit = port % 8;
        if (!(port_used[idx] & (1 << bit))) {
            port_used[idx] |= (1 << bit);
            return port;
        }
    }
    return 0;
}

static void free_port(uint16_t port) {
    int idx = port / 8;
    int bit = port % 8;
    port_used[idx] &= ~(1 << bit);
}

/* 生成初始序列号 */
static uint32_t generate_isn(void) {
    /* 简化的 ISN 生成 (实际应该使用密码学安全的随机数) */
    static uint32_t isn_counter = 0;
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    return (uint32_t)(tsc ^ (tsc >> 32) ^ isn_counter++);
}

/* 序列号比较 */
static inline int seq_before(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static inline int seq_after(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

/* ========== TCP PCB 管理 ========== */

int tcp_init(void) {
    println("[TCP] Initializing TCP protocol...");
    
    /* 初始化 PCB 池 */
    for (int i = 0; i < MAX_TCP_PCBS; i++) {
        tcp_pcb_pool[i].state = TCP_CLOSED;
        tcp_pcb_pool[i].next = tcp_pcb_free_list;
        tcp_pcb_free_list = &tcp_pcb_pool[i];
    }
    
    /* 清空端口表 */
    for (int i = 0; i < sizeof(port_used); i++) {
        port_used[i] = 0;
    }
    
    println("[TCP] Initialized successfully");
    return 0;
}

struct tcp_pcb *tcp_new(void) {
    while (__sync_lock_test_and_set(&tcp_lock, 1)) {
        asm volatile("pause");
    }
    
    struct tcp_pcb *pcb = tcp_pcb_free_list;
    if (pcb) {
        tcp_pcb_free_list = pcb->next;
        pcb->next = tcp_pcb_active_list;
        tcp_pcb_active_list = pcb;
    }
    
    __sync_lock_release(&tcp_lock);
    
    if (pcb) {
        /* 初始化 PCB */
        pcb->state = TCP_CLOSED;
        pcb->flags = 0;
        pcb->local_ip = 0;
        pcb->remote_ip = 0;
        pcb->local_port = 0;
        pcb->remote_port = 0;
        
        pcb->iss = generate_isn();
        pcb->snd_una = pcb->iss;
        pcb->snd_nxt = pcb->iss + 1;
        pcb->snd_max = pcb->iss;
        pcb->snd_wnd = TCP_WINDOW_DEFAULT;
        pcb->snd_wscale = 0;
        
        pcb->rcv_nxt = 0;
        pcb->rcv_wnd = TCP_WINDOW_DEFAULT;
        pcb->rcv_wscale = 0;
        pcb->irs = 0;
        
        pcb->srtt = TCP_RTT_BASE * 8;
        pcb->rttvar = TCP_RTT_BASE * 4;
        pcb->rto = TCP_RTO_INITIAL;
        
        pcb->cwnd = 10 * TCP_MSS_DEFAULT;  /* 初始窗口 10 MSS (RFC 6928) */
        pcb->ssthresh = TCP_WINDOW_MAX;
        pcb->dupacks = 0;
        pcb->cc_state = TCP_CC_SLOW_START;
        pcb->cc_algo = TCP_CC_RENO;
        pcb->recover = 0;
        pcb->cubic.w_max = 0;
        pcb->cubic.k = 0;
        pcb->cubic.epoch_start = 0;
        pcb->cubic.origin_point = 0;
        pcb->cubic.tcp_cwnd = 0;
        pcb->cubic.beta = 717;
        
        pcb->mss = TCP_MSS_DEFAULT;
        pcb->wscale_requested = 0;
        
        pcb->sack_count = 0;
        pcb->sack_permitted = 0;
        pcb->nodelay = 0;
        pcb->keep_enabled = 0;
        pcb->ooseq = NULL;
        pcb->ooseq_count = 0;
        
        pcb->snd_buf = ringbuf_create(64 * 1024);  /* 64KB 发送缓冲 */
        pcb->rcv_buf = ringbuf_create(64 * 1024);  /* 64KB 接收缓冲 */
        
        pcb->on_connect = NULL;
        pcb->on_receive = NULL;
        pcb->on_sent = NULL;
        pcb->on_error = NULL;
        pcb->user_data = NULL;
        
        pcb->retry_count = 0;
        pcb->retry_time = 0;
    }
    
    return pcb;
}

void tcp_free(struct tcp_pcb *pcb) {
    if (pcb == NULL) return;
    
    /* 释放乱序队列 */
    tcp_ooseq_free(pcb);
    
    /* 释放缓冲区 */
    ringbuf_destroy(pcb->snd_buf);
    ringbuf_destroy(pcb->rcv_buf);
    
    /* 释放端口 */
    if (pcb->local_port) {
        free_port(pcb->local_port);
    }
    
    /* 从活动列表移除 */
    while (__sync_lock_test_and_set(&tcp_lock, 1)) {
        asm volatile("pause");
    }
    
    struct tcp_pcb **pp = &tcp_pcb_active_list;
    while (*pp && *pp != pcb) {
        pp = &(*pp)->next;
    }
    if (*pp) {
        *pp = pcb->next;
    }
    
    /* 放回空闲列表 */
    pcb->next = tcp_pcb_free_list;
    tcp_pcb_free_list = pcb;
    
    __sync_lock_release(&tcp_lock);
}

int tcp_bind(struct tcp_pcb *pcb, uint32_t addr, uint16_t port) {
    if (pcb == NULL) return -1;
    
    if (port == 0) {
        port = allocate_port();
        if (port == 0) return -1;
    }
    
    pcb->local_ip = addr;
    pcb->local_port = port;
    
    return 0;
}

int tcp_listen(struct tcp_pcb *pcb, int backlog) {
    if (pcb == NULL) return -1;
    
    pcb->state = TCP_LISTEN;
    pcb->snd_wnd = backlog > 0 ? backlog : 1;
    
    return 0;
}

int tcp_connect(struct tcp_pcb *pcb, uint32_t addr, uint16_t port) {
    if (pcb == NULL) return -1;
    
    /* 分配本地端口 */
    if (pcb->local_port == 0) {
        pcb->local_port = allocate_port();
        if (pcb->local_port == 0) return -1;
    }
    
    pcb->remote_ip = addr;
    pcb->remote_port = port;
    pcb->state = TCP_SYN_SENT;
    
    /* 发送 SYN */
    if (tcp_send_syn(pcb) != 0) {
        pcb->state = TCP_CLOSED;
        return -1;
    }
    
    /* 设置重传定时器 */
    pcb->retry_time = get_tick_count() + pcb->rto;
    pcb->retry_count = TCP_SYN_RETRIES;
    
    return 0;
}

int tcp_close(struct tcp_pcb *pcb) {
    if (pcb == NULL) return -1;
    
    switch (pcb->state) {
        case TCP_CLOSED:
        case TCP_LISTEN:
            tcp_free(pcb);
            return 0;
            
        case TCP_SYN_SENT:
            pcb->state = TCP_CLOSED;
            tcp_free(pcb);
            return 0;
            
        case TCP_ESTABLISHED:
            pcb->state = TCP_FIN_WAIT_1;
            tcp_send_fin(pcb);
            return 0;
            
        case TCP_CLOSE_WAIT:
            pcb->state = TCP_LAST_ACK;
            tcp_send_fin(pcb);
            return 0;
            
        default:
            return -1;
    }
}

/* ========== 数据传输 ========== */

int tcp_send(struct tcp_pcb *pcb, const void *data, size_t len) {
    if (pcb == NULL || pcb->state != TCP_ESTABLISHED) {
        return -1;
    }
    
    /* 写入发送缓冲区 */
    size_t written = ringbuf_write(pcb->snd_buf, data, len);
    
    /* 触发发送 */
    tcp_output(pcb);
    
    return written;
}

int tcp_recv(struct tcp_pcb *pcb, void *buf, size_t len) {
    if (pcb == NULL) return -1;
    
    return ringbuf_read(pcb->rcv_buf, buf, len);
}

/* ========== TCP 输出 ========== */

/* 构建 TCP 段 */
static int tcp_build_segment(struct tcp_pcb *pcb, uint8_t flags,
                               void *data, size_t len,
                               struct net_buf *buf) {
    struct nic_device *nic = net_get_nic_by_index(0);
    if (nic == NULL) return -1;
    
    /* 分配网络缓冲区 */
    buf->len = sizeof(struct eth_hdr) + sizeof(struct ip_hdr) +
               sizeof(struct tcp_hdr) + len;
    
    /* 构建 TCP 头 */
    struct tcp_hdr *tcp = (struct tcp_hdr *)
        (buf->data + sizeof(struct eth_hdr) + sizeof(struct ip_hdr));
    
    tcp->source = pcb->local_port;
    tcp->dest = pcb->remote_port;
    tcp->seq = pcb->snd_nxt;
    tcp->ack_seq = pcb->rcv_nxt;
    tcp->doff = sizeof(struct tcp_hdr) / 4;
    tcp->fin = (flags & TCP_FIN) ? 1 : 0;
    tcp->syn = (flags & TCP_SYN) ? 1 : 0;
    tcp->rst = (flags & TCP_RST) ? 1 : 0;
    tcp->psh = (flags & TCP_PSH) ? 1 : 0;
    tcp->ack = (flags & TCP_ACK) ? 1 : 0;
    tcp->urg = (flags & TCP_URG) ? 1 : 0;
    tcp->window = pcb->rcv_wnd >> pcb->rcv_wscale;
    tcp->check = 0;
    tcp->urg_ptr = 0;
    
    /* 添加选项 (SYN 时添加 MSS + WScale + SACK-Permitted) */
    if (flags & TCP_SYN) {
        uint8_t *opt = (uint8_t *)(tcp + 1);
        int opt_len = 0;
        
        /* MSS 选项 (4 字节) */
        opt[0] = TCP_OPT_MSS;
        opt[1] = 4;
        opt[2] = (pcb->mss >> 8) & 0xFF;
        opt[3] = pcb->mss & 0xFF;
        opt_len += 4;
        
        /* 窗口缩放选项 (3 字节 + 1 NOP 对齐) */
        opt[4] = TCP_OPT_NOP;      /* 对齐 */
        opt[5] = TCP_OPT_WSCALE;
        opt[6] = 3;
        opt[7] = pcb->rcv_wscale;  /* 接收端窗口缩放值 */
        opt_len += 4;
        
        /* SACK-Permitted 选项 (2 字节 + 2 NOP 对齐) */
        opt[8] = TCP_OPT_NOP;      /* 对齐 */
        opt[9] = TCP_OPT_NOP;      /* 对齐 */
        opt[10] = TCP_OPT_SACKOK;
        opt[11] = 2;
        opt_len += 4;
        
        pcb->sack_permitted = 1;
        pcb->wscale_requested = 1;
        
        tcp->doff = (sizeof(struct tcp_hdr) + opt_len) / 4;
    }
    
    /* 复制数据 */
    if (data && len > 0) {
        memcpy((uint8_t *)tcp + (tcp->doff * 4), data, len);
    }
    
    /* 计算 IP 伪头部 */
    struct ip_hdr ip;
    ip.saddr = pcb->local_ip;
    ip.daddr = pcb->remote_ip;
    ip.protocol = 6;  /* TCP */
    
    /* 计算校验和 */
    tcp->check = tcp_checksum(&ip, tcp, data, len);
    
    /* 发送 IP 包 */
    return ip_send(pcb->remote_ip, 6, buf->data + sizeof(struct eth_hdr),
                   sizeof(struct ip_hdr) + sizeof(struct tcp_hdr) + len);
}

int tcp_output(struct tcp_pcb *pcb) {
    if (pcb == NULL) return -1;
    
    /* 计算可发送的数据量 */
    uint32_t win = pcb->snd_wnd < pcb->cwnd ? pcb->snd_wnd : pcb->cwnd;
    uint32_t in_flight = pcb->snd_nxt - pcb->snd_una;
    uint32_t usable = win > in_flight ? win - in_flight : 0;
    
    /* 获取待发送数据 */
    size_t avail = pcb->snd_buf->head - pcb->snd_buf->tail;
    size_t send_len = avail < usable ? avail : usable;
    send_len = send_len < pcb->mss ? send_len : pcb->mss;
    
    if (send_len == 0) {
        /* 没有数据，检查是否需要发送 ACK */
        if (pcb->flags & TCP_FLAG_ACK_NOW) {
            struct net_buf buf;
            tcp_build_segment(pcb, TCP_ACK, NULL, 0, &buf);
            pcb->flags &= ~TCP_FLAG_ACK_NOW;
            tcp_out_segs++;
        }
        return 0;
    }
    
    /* Nagle 算法检查 */
    if (!tcp_nagle_check(pcb)) {
        /* Nagle 不允许发送，记录等待时间 */
        if (pcb->nagle_time == 0) {
            pcb->nagle_time = get_tick_count();
        }
        /* 但如果有 ACK 需要发送，仍然发送 ACK */
        if (pcb->flags & TCP_FLAG_ACK_NOW) {
            struct net_buf buf;
            tcp_build_segment(pcb, TCP_ACK, NULL, 0, &buf);
            pcb->flags &= ~TCP_FLAG_ACK_NOW;
            tcp_out_segs++;
        }
        return 0;
    }
    
    /* Nagle 允许发送，重置等待时间 */
    pcb->nagle_time = 0;
    
    /* 发送数据段 */
    uint8_t data[pcb->mss];
    ringbuf_peek(pcb->snd_buf, data, send_len);
    
    struct net_buf buf;
    int ret = tcp_build_segment(pcb, TCP_ACK | TCP_PSH, data, send_len, &buf);
    
    if (ret == 0) {
        pcb->snd_nxt += send_len;
        if (seq_after(pcb->snd_nxt, pcb->snd_max)) {
            pcb->snd_max = pcb->snd_nxt;
        }
        
        /* 开始 RTT 测量 */
        if (pcb->rtt_seq == 0) {
            tcp_rtt_start(pcb, pcb->snd_nxt - send_len);
        }
        
        tcp_out_segs++;
    }
    
    return ret;
}

int tcp_send_syn(struct tcp_pcb *pcb) {
    struct net_buf buf;
    int ret = tcp_build_segment(pcb, TCP_SYN, NULL, 0, &buf);
    
    if (ret == 0) {
        pcb->snd_nxt = pcb->iss + 1;
        pcb->snd_max = pcb->snd_nxt;
        tcp_out_segs++;
    }
    
    return ret;
}

int tcp_send_fin(struct tcp_pcb *pcb) {
    struct net_buf buf;
    uint8_t flags = TCP_FIN | TCP_ACK;
    
    int ret = tcp_build_segment(pcb, flags, NULL, 0, &buf);
    
    if (ret == 0) {
        pcb->snd_nxt++;
        tcp_out_segs++;
    }
    
    return ret;
}

void tcp_send_ack(struct tcp_pcb *pcb) {
    pcb->flags |= TCP_FLAG_ACK_NOW;
    tcp_output(pcb);
}

void tcp_send_reset(struct tcp_pcb *pcb, uint32_t seq) {
    struct net_buf buf;
    tcp_build_segment(pcb, TCP_RST, NULL, 0, &buf);
    tcp_out_segs++;
}

/* ========== TCP 输入 ========== */

void tcp_input(struct net_buf *buf, struct ip_hdr *ip) {
    struct tcp_hdr *tcp = (struct tcp_hdr *)
        ((uint8_t *)ip + (ip->ihl * 4));
    
    tcp_in_segs++;
    
    /* 验证校验和 */
    if (tcp_checksum(ip, tcp, (uint8_t *)tcp + (tcp->doff * 4),
                     buf->len - ((uint8_t *)tcp - buf->data) - (tcp->doff * 4)) != 0) {
        tcp_in_errors++;
        return;
    }
    
    /* 查找 PCB */
    struct tcp_pcb *pcb = NULL;
    
    while (__sync_lock_test_and_set(&tcp_lock, 1)) {
        asm volatile("pause");
    }
    
    for (struct tcp_pcb *p = tcp_pcb_active_list; p; p = p->next) {
        if (p->remote_port == tcp->source &&
            p->local_port == tcp->dest &&
            p->remote_ip == ip->saddr &&
            p->local_ip == ip->daddr) {
            pcb = p;
            break;
        }
        
        /* 监听状态的 PCB */
        if (p->state == TCP_LISTEN && p->local_port == tcp->dest) {
            pcb = p;
        }
    }
    
    __sync_lock_release(&tcp_lock);
    
    if (pcb == NULL) {
        /* 发送 RST */
        return;
    }
    
    /* 处理 TCP 状态机 */
    uint8_t flags = (tcp->fin ? TCP_FIN : 0) | (tcp->syn ? TCP_SYN : 0) |
                    (tcp->rst ? TCP_RST : 0) | (tcp->psh ? TCP_PSH : 0) |
                    (tcp->ack ? TCP_ACK : 0) | (tcp->urg ? TCP_URG : 0);
    uint32_t seq = ntohl(tcp->seq);
    uint32_t ack = ntohl(tcp->ack_seq);
    uint16_t win = ntohs(tcp->window);
    /* 应用窗口缩放: 接收到的窗口值需要按发送端缩放因子展开 */
    uint32_t scaled_win = tcp_wscale_expand(win, pcb->snd_wscale);
    
    switch (pcb->state) {
        case TCP_LISTEN:
            if (flags & TCP_SYN) {
                /* 新连接请求 */
                pcb->irs = seq;
                pcb->rcv_nxt = seq + 1;
                pcb->remote_ip = ip->saddr;
                pcb->remote_port = tcp->source;
                pcb->snd_wnd = scaled_win;
                
                /* 解析选项 */
                int opt_len = (tcp->doff * 4) - sizeof(struct tcp_hdr);
                if (opt_len > 0) {
                    tcp_parse_options(pcb, (uint8_t *)(tcp + 1), opt_len);
                }
                
                /* 发送 SYN-ACK */
                pcb->state = TCP_SYN_RECEIVED;
                struct net_buf rsp;
                tcp_build_segment(pcb, TCP_SYN | TCP_ACK, NULL, 0, &rsp);
                tcp_out_segs++;
            }
            break;
            
        case TCP_SYN_SENT:
            if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                /* SYN-ACK 响应 */
                if (ack != pcb->snd_nxt) {
                    tcp_send_reset(pcb, ack);
                    return;
                }
                
                pcb->irs = seq;
                pcb->rcv_nxt = seq + 1;
                pcb->snd_una = ack;
                pcb->snd_wnd = scaled_win;
                
                /* 发送 ACK */
                pcb->state = TCP_ESTABLISHED;
                tcp_send_ack(pcb);
                
                /* 通知应用层 */
                if (pcb->on_connect) {
                    pcb->on_connect(pcb);
                }
            }
            break;
            
        case TCP_ESTABLISHED:
            if (flags & TCP_RST) {
                pcb->state = TCP_CLOSED;
                if (pcb->on_error) {
                    pcb->on_error(pcb, -1);
                }
                break;
            }
            
            if (flags & TCP_FIN) {
                pcb->rcv_nxt++;
                pcb->state = TCP_CLOSE_WAIT;
                tcp_send_ack(pcb);
                
                if (pcb->on_receive) {
                    pcb->on_receive(pcb, NULL, 0);  /* EOF */
                }
                break;
            }
            
            /* 处理数据 */
            {
                int data_offset = tcp->doff * 4;
                uint8_t *data = (uint8_t *)tcp + data_offset;
                size_t data_len = buf->len - ((uint8_t *)tcp - buf->data) - data_offset;
                
                if (data_len > 0) {
                    if (seq == pcb->rcv_nxt) {
                        /* 按序到达，写入接收缓冲区 */
                        ringbuf_write(pcb->rcv_buf, data, data_len);
                        pcb->rcv_nxt += data_len;
                        
                        /* 检查乱序队列中是否有后续数据 */
                        tcp_ooseq_dequeue(pcb);
                        
                        /* 更新 SACK 块 */
                        tcp_sack_update(pcb);
                        
                        tcp_send_ack(pcb);
                        
                        if (pcb->on_receive) {
                            pcb->on_receive(pcb, data, data_len);
                        }
                    } else if (seq_after(seq, pcb->rcv_nxt)) {
                        /* 乱序到达，插入重排队列 */
                        tcp_ooseq_insert(pcb, seq, data, data_len);
                        
                        /* 更新 SACK 块以反映乱序数据 */
                        tcp_sack_update(pcb);
                        
                        /* 立即发送 ACK，携带 SACK 选项 */
                        pcb->flags |= TCP_FLAG_ACK_NOW;
                    }
                    /* seq_before(seq, pcb->rcv_nxt): 重复数据，忽略 */
                    
                    /* 更新最后活动时间 */
                    pcb->last_activity = get_tick_count();
                    pcb->keep_probes = 0;
                }
            }
            
            /* 处理 ACK */
            if (flags & TCP_ACK) {
                /* 更新发送窗口 (使用缩放后的窗口值) */
                if (seq_after(ack, pcb->snd_wl2) ||
                    (ack == pcb->snd_wl2 && seq_after(seq, pcb->snd_wl1))) {
                    pcb->snd_wnd = scaled_win;
                    pcb->snd_wl1 = seq;
                    pcb->snd_wl2 = ack;
                }
                
                if (seq_after(ack, pcb->snd_una)) {
                    /* 新数据被确认 */
                    uint32_t acked = ack - pcb->snd_una;
                    
                    /* 从发送缓冲区移除 */
                    ringbuf_read(pcb->snd_buf, NULL, acked);
                    
                    pcb->snd_una = ack;
                    pcb->dupacks = 0;
                    
                    /* RTT 测量 */
                    if (pcb->rtt_seq && !seq_before(ack, pcb->rtt_seq)) {
                        tcp_rtt_stop(pcb, ack);
                    }
                    
                    /* 拥塞控制 - 统一入口 */
                    tcp_cc_on_ack(pcb, acked);
                    
                    /* Nagle: 有新 ACK 确认，可能可以发送积压数据 */
                    if (!pcb->nodelay && pcb->nagle_time > 0) {
                        tcp_output(pcb);
                    }
                } else if (ack == pcb->snd_una) {
                    /* 重复 ACK - 交给拥塞控制处理 */
                    tcp_cc_on_ack(pcb, 0);
                }
            }
            break;
            
        case TCP_FIN_WAIT_1:
            if ((flags & (TCP_FIN | TCP_ACK)) == (TCP_FIN | TCP_ACK)) {
                pcb->rcv_nxt++;
                pcb->state = TCP_TIME_WAIT;
                pcb->retry_time = get_tick_count() + TCP_TIME_WAIT_TIMEOUT;
                tcp_send_ack(pcb);
            } else if (flags & TCP_ACK) {
                pcb->state = TCP_FIN_WAIT_2;
            }
            break;
            
        case TCP_FIN_WAIT_2:
            if (flags & TCP_FIN) {
                pcb->rcv_nxt++;
                pcb->state = TCP_TIME_WAIT;
                pcb->retry_time = get_tick_count() + TCP_TIME_WAIT_TIMEOUT;
                tcp_send_ack(pcb);
            }
            break;
            
        case TCP_TIME_WAIT:
            tcp_send_ack(pcb);
            break;
            
        case TCP_LAST_ACK:
            if (flags & TCP_ACK) {
                pcb->state = TCP_CLOSED;
                tcp_free(pcb);
            }
            break;
            
        default:
            break;
    }
}

/* ========== 拥塞控制 (统一实现: Reno/Cubic) ========== */

/* 初始化拥塞控制 */
void tcp_cc_init(struct tcp_pcb *pcb) {
    pcb->cwnd = 10 * pcb->mss;   /* 初始窗口 10 MSS (RFC 6928) */
    pcb->ssthresh = TCP_WINDOW_MAX;
    pcb->dupacks = 0;
    pcb->recover = 0;
    pcb->cc_state = TCP_CC_SLOW_START;
    pcb->cc_algo = TCP_CC_RENO;
    
    /* CUBIC 参数初始化 */
    pcb->cubic.w_max = 0;
    pcb->cubic.k = 0;
    pcb->cubic.epoch_start = 0;
    pcb->cubic.origin_point = 0;
    pcb->cubic.tcp_cwnd = 0;
    pcb->cubic.beta = 717;    /* 0.7 * 1024 ≈ 717 */
}

/* 设置拥塞控制算法 */
int tcp_set_cc_algo(struct tcp_pcb *pcb, int algo) {
    if (pcb == NULL) return -1;
    if (algo != TCP_CC_RENO && algo != TCP_CC_CUBIC) return -1;
    
    pcb->cc_algo = algo;
    /* 重置 CUBIC 参数 */
    if (algo == TCP_CC_CUBIC) {
        pcb->cubic.epoch_start = 0;
        pcb->cubic.w_max = 0;
    }
    return 0;
}

/* Reno 慢启动 */
static void tcp_cc_reno_slow_start(struct tcp_pcb *pcb, uint32_t acked) {
    pcb->cwnd += acked;
    if (pcb->cwnd >= pcb->ssthresh) {
        pcb->cwnd = pcb->ssthresh;
        pcb->cc_state = TCP_CC_CONGESTION_AVOIDANCE;
    }
}

/* Reno 拥塞避免 */
static void tcp_cc_reno_congestion_avoidance(struct tcp_pcb *pcb, uint32_t acked) {
    /* 每个 RTT 增加 1 MSS */
    uint32_t incr = (uint32_t)((uint64_t)pcb->mss * acked / pcb->cwnd);
    if (incr == 0) incr = 1;
    pcb->cwnd += incr;
}

/* CUBIC 拥塞避免 (RFC 8312) */
static void tcp_cc_cubic_congestion_avoidance(struct tcp_pcb *pcb, uint32_t acked) {
    uint64_t now = get_tick_count();
    
    if (pcb->cubic.epoch_start == 0) {
        /* 首次进入拥塞避免，初始化 CUBIC 纪元 */
        pcb->cubic.epoch_start = now;
        if (pcb->cubic.w_max == 0) {
            pcb->cubic.w_max = pcb->cwnd;
        }
        pcb->cubic.origin_point = pcb->cubic.w_max;
        
        /* 计算 K = (w_max * beta / C) ^ (1/3) */
        /* C = 0.4, beta = 0.7, 简化计算 */
        uint64_t w_max_mss = pcb->cubic.w_max / pcb->mss;
        if (w_max_mss == 0) w_max_mss = 1;
        /* K = (w_max * (1-beta) / C) ^ (1/3) */
        /* 简化: K ≈ (w_max_mss * 0.75) 的立方根 */
        uint64_t k_cubed = w_max_mss * 3 / 4;
        pcb->cubic.k = 0;
        while (pcb->cubic.k * pcb->cubic.k * pcb->cubic.k < k_cubed * 10) {
            pcb->cubic.k++;
        }
        pcb->cubic.k = pcb->cubic.k * 100;  /* 转换为 ms 单位 */
    }
    
    /* 计算自纪元开始的时间 (秒 * 1000 = ms) */
    uint64_t t = (now - pcb->cubic.epoch_start);
    
    /* W_cubic(t) = C * (t - K)^3 + w_max */
    /* C = 0.4, 简化计算 */
    int64_t dt = (int64_t)(t) - (int64_t)(pcb->cubic.k);
    int64_t dt_sec = dt / 1000;  /* 转为秒 */
    int64_t dt_cubed = dt_sec * dt_sec * dt_sec;
    uint64_t w_cubic = (uint64_t)(dt_cubed * 4 / 10) + pcb->cubic.origin_point / pcb->mss;
    
    /* TCP 友好性: W_tcp(t) = w_max * beta + 3 * beta / (2 - beta) * t / RTT */
    uint64_t rtt_ms = pcb->srtt >> 3;
    if (rtt_ms > 0) {
        uint64_t w_tcp = (pcb->cubic.w_max * 717 / 1024) / pcb->mss +
                         3 * 717 / (1024 - 717) * t / rtt_ms;
        if (w_tcp > w_cubic) {
            w_cubic = w_tcp;
        }
    }
    
    /* 确保至少为 1 MSS */
    if (w_cubic < 1) w_cubic = 1;
    
    uint32_t target_cwnd = (uint32_t)(w_cubic * pcb->mss);
    if (target_cwnd > pcb->cwnd) {
        uint32_t incr = target_cwnd - pcb->cwnd;
        if (incr > acked) incr = acked;
        pcb->cwnd += incr;
    }
}

/* 快速重传 (Reno/Cubic 共用) */
static void tcp_cc_fast_retransmit(struct tcp_pcb *pcb) {
    /* CUBIC: 在减少前记录窗口最大值 */
    if (pcb->cc_algo == TCP_CC_CUBIC) {
        pcb->cubic.w_max = pcb->cwnd;
        pcb->cubic.epoch_start = 0;  /* 重置纪元 */
    }
    
    /* 设置 ssthresh = cwnd * beta */
    if (pcb->cc_algo == TCP_CC_CUBIC) {
        pcb->ssthresh = (uint32_t)((uint64_t)pcb->cwnd * pcb->cubic.beta / 1024);
    } else {
        pcb->ssthresh = pcb->cwnd / 2;
    }
    if (pcb->ssthresh < 2 * pcb->mss) {
        pcb->ssthresh = 2 * pcb->mss;
    }
    
    /* 快速恢复: cwnd = ssthresh + 3*MSS (3 个重复 ACK 已离开网络) */
    pcb->cwnd = pcb->ssthresh + 3 * pcb->mss;
    pcb->cc_state = TCP_CC_FAST_RECOVERY;
    pcb->recover = pcb->snd_nxt;
    
    /* 执行重传 (优先使用 SACK 选择性重传) */
    if (pcb->sack_permitted && pcb->sack_count > 0) {
        tcp_retransmit_sack(pcb);
    } else {
        tcp_retransmit(pcb);
    }
}

/* 收到 ACK 时的拥塞控制处理 (统一入口) */
void tcp_cc_on_ack(struct tcp_pcb *pcb, uint32_t acked) {
    if (acked == 0) {
        /* 重复 ACK */
        pcb->dupacks++;
        
        if (pcb->dupacks == 3) {
            /* 触发快速重传 */
            if (pcb->cc_state != TCP_CC_FAST_RECOVERY) {
                tcp_cc_fast_retransmit(pcb);
            }
        } else if (pcb->dupacks > 3 && pcb->cc_state == TCP_CC_FAST_RECOVERY) {
            /* 快速恢复中每收到一个重复 ACK，cwnd 增加 1 MSS */
            pcb->cwnd += pcb->mss;
        }
        return;
    }
    
    /* 新数据确认 */
    uint32_t ack_seq = pcb->snd_una + acked;
    
    if (pcb->cc_state == TCP_CC_FAST_RECOVERY) {
        /* 快速恢复中收到新 ACK */
        if (seq_before(ack_seq, pcb->recover)) {
            /* 部分 ACK: 重传下一个未确认段 */
            pcb->cwnd -= pcb->mss;  /* 取消之前重复 ACK 的增量 */
            /* 重传下一个丢失段 */
            if (pcb->sack_permitted && pcb->sack_count > 0) {
                tcp_retransmit_sack(pcb);
            } else {
                tcp_retransmit(pcb);
            }
        } else {
            /* 完全恢复: cwnd 回到 ssthresh */
            pcb->cwnd = pcb->ssthresh;
            pcb->cc_state = TCP_CC_CONGESTION_AVOIDANCE;
            pcb->dupacks = 0;
            
            /* CUBIC: 重置纪元 */
            if (pcb->cc_algo == TCP_CC_CUBIC) {
                pcb->cubic.epoch_start = 0;
            }
            return;
        }
        pcb->dupacks = 0;
        return;
    }
    
    pcb->dupacks = 0;
    
    switch (pcb->cc_state) {
        case TCP_CC_SLOW_START:
            if (pcb->cc_algo == TCP_CC_CUBIC) {
                tcp_cc_reno_slow_start(pcb, acked);  /* CUBIC 慢启动与 Reno 相同 */
            } else {
                tcp_cc_reno_slow_start(pcb, acked);
            }
            break;
            
        case TCP_CC_CONGESTION_AVOIDANCE:
            if (pcb->cc_algo == TCP_CC_CUBIC) {
                tcp_cc_cubic_congestion_avoidance(pcb, acked);
            } else {
                tcp_cc_reno_congestion_avoidance(pcb, acked);
            }
            break;
            
        case TCP_CC_DISORDER:
            /* 乱序状态恢复 */
            pcb->cc_state = TCP_CC_CONGESTION_AVOIDANCE;
            break;
            
        default:
            break;
    }
}

/* 超时处理 */
void tcp_cc_timeout(struct tcp_pcb *pcb) {
    /* CUBIC: 记录窗口最大值 */
    if (pcb->cc_algo == TCP_CC_CUBIC) {
        pcb->cubic.w_max = pcb->cwnd;
        pcb->cubic.epoch_start = 0;
    }
    
    pcb->ssthresh = pcb->cwnd / 2;
    if (pcb->ssthresh < 2 * pcb->mss) {
        pcb->ssthresh = 2 * pcb->mss;
    }
    pcb->cwnd = 1 * pcb->mss;
    pcb->cc_state = TCP_CC_SLOW_START;
    pcb->dupacks = 0;
}

/* ========== RTT 估算 ========== */

void tcp_rtt_start(struct tcp_pcb *pcb, uint32_t seq) {
    pcb->rtt_seq = seq;
    pcb->rtt_start = get_tick_count();
}

void tcp_rtt_stop(struct tcp_pcb *pcb, uint32_t ack) {
    if (pcb->rtt_seq == 0 || seq_before(ack, pcb->rtt_seq)) {
        return;
    }
    
    uint64_t now = get_tick_count();
    uint32_t rtt = (uint32_t)(now - pcb->rtt_start);
    
    tcp_rtt_update(pcb, rtt);
    pcb->rtt_seq = 0;
}

void tcp_rtt_update(struct tcp_pcb *pcb, uint32_t rtt) {
    /* RFC 6298 算法 */
    if (pcb->srtt == 0) {
        pcb->srtt = rtt * 8;
        pcb->rttvar = rtt * 4;
    } else {
        int32_t delta = rtt * 8 - pcb->srtt;
        pcb->srtt += delta / 8;
        
        if (delta < 0) delta = -delta;
        pcb->rttvar = (3 * pcb->rttvar + delta) / 4;
    }
    
    pcb->rto = (pcb->srtt >> 3) + pcb->rttvar;
    
    if (pcb->rto < TCP_RTO_MIN) pcb->rto = TCP_RTO_MIN;
    if (pcb->rto > TCP_RTO_MAX) pcb->rto = TCP_RTO_MAX;
}

/* ========== 重传 ========== */

void tcp_retransmit(struct tcp_pcb *pcb) {
    /* 从发送缓冲区重新发送数据 */
    uint8_t data[pcb->mss];
    size_t len = ringbuf_peek(pcb->snd_buf, data, pcb->mss);
    
    if (len > 0) {
        struct net_buf buf;
        tcp_build_segment(pcb, TCP_ACK | TCP_PSH, data, len, &buf);
        tcp_retrans_segs++;
    }
}

/* SACK 驱动的选择性重传: 根据 SACK 块跳过已确认的数据段 */
void tcp_retransmit_sack(struct tcp_pcb *pcb) {
    if (pcb == NULL || pcb->snd_buf == NULL) return;
    
    /* 如果没有 SACK 块，退回普通重传 */
    if (pcb->sack_count == 0) {
        tcp_retransmit(pcb);
        return;
    }
    
    /* 计算发送缓冲区中未确认数据的起始偏移 */
    uint32_t in_flight = pcb->snd_nxt - pcb->snd_una;
    if (in_flight == 0) return;
    
    /* 遍历发送缓冲区，跳过 SACK 块覆盖的段 */
    size_t buf_offset = 0;
    size_t buf_avail = ringbuf_pending(pcb->snd_buf);
    uint32_t seg_seq = pcb->snd_una;
    
    while (buf_offset < buf_avail && buf_offset < in_flight) {
        size_t seg_len = pcb->mss;
        if (buf_offset + seg_len > buf_avail) {
            seg_len = buf_avail - buf_offset;
        }
        if (buf_offset + seg_len > in_flight) {
            seg_len = in_flight - buf_offset;
        }
        
        uint32_t seg_end = seg_seq + seg_len;
        
        /* 检查此段是否被 SACK 块覆盖 */
        int sacked = 0;
        for (int i = 0; i < pcb->sack_count; i++) {
            if (seq_before(seg_seq, pcb->sack_blocks[i].end) &&
                seq_after(seg_end, pcb->sack_blocks[i].start)) {
                /* 段被 SACK 覆盖，跳过 */
                sacked = 1;
                break;
            }
        }
        
        if (!sacked) {
            /* 此段未被 SACK 确认，重传它 */
            uint8_t data[pcb->mss];
            /* 从发送缓冲区偏移位置读取数据 */
            size_t old_tail = pcb->snd_buf->tail;
            pcb->snd_buf->tail += buf_offset;
            size_t read_len = ringbuf_peek(pcb->snd_buf, data, seg_len);
            pcb->snd_buf->tail = old_tail;
            
            if (read_len > 0) {
                struct net_buf buf;
                /* 使用原始序列号发送 */
                uint32_t old_nxt = pcb->snd_nxt;
                pcb->snd_nxt = seg_seq;
                tcp_build_segment(pcb, TCP_ACK | TCP_PSH, data, read_len, &buf);
                pcb->snd_nxt = old_nxt;
                tcp_retrans_segs++;
            }
            break;  /* 每次只重传一个段 */
        }
        
        buf_offset += seg_len;
        seg_seq += seg_len;
    }
}

/* ========== 定时器 ========== */

void tcp_timer(void) {
    uint64_t now = get_tick_count();
    
    for (struct tcp_pcb *pcb = tcp_pcb_active_list; pcb; pcb = pcb->next) {
        /* 检查重传超时 */
        if (pcb->retry_time && now >= pcb->retry_time) {
            if (pcb->retry_count > 0) {
                pcb->retry_count--;
                
                /* 超时重传 */
                tcp_cc_timeout(pcb);
                
                if (pcb->state == TCP_SYN_SENT) {
                    tcp_send_syn(pcb);
                } else {
                    tcp_retransmit(pcb);
                }
                
                /* 指数退避 */
                pcb->rto *= 2;
                if (pcb->rto > TCP_RTO_MAX) {
                    pcb->rto = TCP_RTO_MAX;
                }
                
                pcb->retry_time = now + pcb->rto;
            } else {
                /* 超时，关闭连接 */
                pcb->state = TCP_CLOSED;
                if (pcb->on_error) {
                    pcb->on_error(pcb, -1);
                }
            }
        }
        
        /* TIME_WAIT 超时 */
        if (pcb->state == TCP_TIME_WAIT) {
            if (now >= pcb->retry_time) {
                pcb->state = TCP_CLOSED;
                tcp_free(pcb);
            }
        }
        
        /* Keep-Alive 定时器 */
        tcp_keepalive_timer(pcb);
        
        /* Nagle 超时检查 */
        if (!pcb->nodelay && pcb->nagle_time > 0 && 
            pcb->state == TCP_ESTABLISHED) {
            if (now - pcb->nagle_time >= TCP_NAGLE_DELAY) {
                /* Nagle 延迟超时，尝试发送 */
                tcp_output(pcb);
            }
        }
    }
}

/* ========== 校验和 ========== */

uint16_t tcp_checksum(struct ip_hdr *ip, struct tcp_hdr *tcp,
                       const void *data, size_t len) {
    uint32_t sum = 0;
    
    /* 伪头部 */
    sum += (ip->saddr >> 16) & 0xFFFF;
    sum += ip->saddr & 0xFFFF;
    sum += (ip->daddr >> 16) & 0xFFFF;
    sum += ip->daddr & 0xFFFF;
    sum += 6;  /* TCP 协议号 */
    sum += sizeof(struct tcp_hdr) + len;
    
    /* TCP 头 */
    uint16_t *ptr = (uint16_t *)tcp;
    for (size_t i = 0; i < sizeof(struct tcp_hdr); i += 2) {
        sum += *ptr++;
    }
    
    /* 数据 */
    ptr = (uint16_t *)data;
    size_t words = len / 2;
    for (size_t i = 0; i < words; i++) {
        sum += *ptr++;
    }
    
    /* 奇数字节 */
    if (len & 1) {
        sum += *(uint8_t *)ptr;
    }
    
    /* 折叠 */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return ~sum & 0xFFFF;
}

/* ========== 选项解析 ========== */

void tcp_parse_options(struct tcp_pcb *pcb, uint8_t *options, int len) {
    int i = 0;
    
    while (i < len) {
        uint8_t kind = options[i];
        
        if (kind == TCP_OPT_EOL) break;
        if (kind == TCP_OPT_NOP) {
            i++;
            continue;
        }
        
        if (i + 1 >= len) break;
        
        uint8_t opt_len = options[i + 1];
        if (opt_len == 0 || i + opt_len > len) break;
        
        switch (kind) {
            case TCP_OPT_MSS:
                if (opt_len >= 4) {
                    uint16_t mss = (options[i + 2] << 8) | options[i + 3];
                    if (mss < pcb->mss) {
                        pcb->mss = mss;
                    }
                }
                break;
                
            case TCP_OPT_WSCALE:
                if (opt_len >= 3) {
                    pcb->snd_wscale = options[i + 2];
                    if (pcb->snd_wscale > 14) {
                        pcb->snd_wscale = 14;  /* RFC 1323 限制 */
                    }
                }
                break;
                
            case TCP_OPT_SACKOK:
                pcb->sack_permitted = 1;
                break;
                
            case TCP_OPT_SACK:
                /* SACK 块在 tcp_input 中单独处理 */
                tcp_process_sack(pcb, &options[i], opt_len);
                break;
                
            case TCP_OPT_TIMESTAMP:
                /* 时间戳选项 - 暂不实现，跳过 */
                break;
        }
        
        i += opt_len;
    }
}

/* ========== 统计 ========== */

void tcp_stats(void) {
    println("=== TCP Statistics ===");
    print("In Segments: ");
    print_dec(tcp_in_segs);
    println("");
    print("Out Segments: ");
    print_dec(tcp_out_segs);
    println("");
    print("Retransmitted: ");
    print_dec(tcp_retrans_segs);
    println("");
    print("In Errors: ");
    print_dec(tcp_in_errors);
    println("");
}

/* ========== TCP 乱序重排队列 ========== */

/* 将乱序段插入队列 (按序列号排序) */
void tcp_ooseq_insert(struct tcp_pcb *pcb, uint32_t seq, const void *data, size_t len) {
    if (pcb == NULL || data == NULL || len == 0) return;
    
    /* 检查队列是否已满 */
    if (pcb->ooseq_count >= TCP_OOQ_MAX_SEGS) {
        /* 丢弃最旧的段 */
        struct tcp_ooseg *oldest = pcb->ooseq;
        if (oldest) {
            pcb->ooseq = oldest->next;
            kfree(oldest->data);
            kfree(oldest);
            pcb->ooseq_count--;
        }
    }
    
    /* 分配新段 */
    struct tcp_ooseg *seg = kmalloc(sizeof(*seg));
    if (seg == NULL) return;
    
    seg->data = kmalloc(len);
    if (seg->data == NULL) {
        kfree(seg);
        return;
    }
    
    seg->seq = seq;
    seg->len = len;
    memcpy(seg->data, data, len);
    seg->next = NULL;
    
    /* 按序列号插入到正确位置 */
    struct tcp_ooseg **pp = &pcb->ooseq;
    while (*pp && seq_before((*pp)->seq, seq)) {
        pp = &(*pp)->next;
    }
    
    /* 检查是否与已有段重叠 */
    if (*pp) {
        /* 与后一个段重叠处理 */
        uint32_t seg_end = seq + len;
        if (seq_after(seg_end, (*pp)->seq)) {
            /* 有重叠，截断当前段 */
            uint32_t overlap = seg_end - (*pp)->seq;
            if (overlap >= len) {
                /* 完全被覆盖，无需插入 */
                kfree(seg->data);
                kfree(seg);
                return;
            }
            seg->len -= overlap;
        }
    }
    
    /* 检查与前一个段的重叠 */
    if (pp != &pcb->ooseq) {
        struct tcp_ooseg *prev = pcb->ooseq;
        while (prev->next != *pp) prev = prev->next;
        uint32_t prev_end = prev->seq + prev->len;
        if (seq_after(prev_end, seq)) {
            /* 前一个段覆盖了当前段的开头 */
            uint32_t overlap = prev_end - seq;
            if (overlap >= len) {
                /* 完全被覆盖 */
                kfree(seg->data);
                kfree(seg);
                return;
            }
            seg->seq += overlap;
            seg->len -= overlap;
            memmove(seg->data, seg->data + overlap, seg->len);
        }
    }
    
    seg->next = *pp;
    *pp = seg;
    pcb->ooseq_count++;
}

/* 将有序数据从队列中取出到接收缓冲区 */
void tcp_ooseq_dequeue(struct tcp_pcb *pcb) {
    if (pcb == NULL) return;
    
    while (pcb->ooseq && pcb->ooseq->seq == pcb->rcv_nxt) {
        struct tcp_ooseg *seg = pcb->ooseq;
        
        /* 写入接收缓冲区 */
        ringbuf_write(pcb->rcv_buf, seg->data, seg->len);
        pcb->rcv_nxt += seg->len;
        
        /* 移除并释放 */
        pcb->ooseq = seg->next;
        kfree(seg->data);
        kfree(seg);
        pcb->ooseq_count--;
    }
}

/* 释放乱序队列 */
void tcp_ooseq_free(struct tcp_pcb *pcb) {
    if (pcb == NULL) return;
    
    struct tcp_ooseg *seg = pcb->ooseq;
    while (seg) {
        struct tcp_ooseg *next = seg->next;
        kfree(seg->data);
        kfree(seg);
        seg = next;
    }
    pcb->ooseq = NULL;
    pcb->ooseq_count = 0;
}

/* ========== TCP SACK 处理 ========== */

/* 添加 SACK 块 */
static int tcp_sack_add_block(struct tcp_pcb *pcb, uint32_t start, uint32_t end) {
    if (pcb->sack_count >= 4) return -1;
    
    pcb->sack_blocks[pcb->sack_count].start = start;
    pcb->sack_blocks[pcb->sack_count].end = end;
    pcb->sack_count++;
    return 0;
}

/* 处理收到的 SACK 选项 */
void tcp_process_sack(struct tcp_pcb *pcb, uint8_t *options, int len) {
    int i = 0;
    
    while (i < len) {
        uint8_t kind = options[i];
        
        if (kind == TCP_OPT_EOL) break;
        if (kind == TCP_OPT_NOP) {
            i++;
            continue;
        }
        
        if (i + 1 >= len) break;
        uint8_t opt_len = options[i + 1];
        if (opt_len == 0 || i + opt_len > len) break;
        
        /* SACK 选项类型 = 5 */
        if (kind == TCP_OPT_SACK && opt_len >= 8) {
            int num_blocks = (opt_len - 2) / 8;
            
            for (int j = 0; j < num_blocks && j < 4; j++) {
                uint8_t *block = &options[i + 2 + j * 8];
                uint32_t start = (block[0] << 24) | (block[1] << 16) | 
                                 (block[2] << 8) | block[3];
                uint32_t end = (block[4] << 24) | (block[5] << 16) | 
                               (block[6] << 8) | block[7];
                
                tcp_sack_add_block(pcb, start, end);
            }
        }
        
        i += opt_len;
    }
}

/* 构建 SACK 选项 */
int tcp_build_sack_option(struct tcp_pcb *pcb, uint8_t *options) {
    if (pcb->sack_count == 0) return 0;
    
    int len = 2 + pcb->sack_count * 8;
    options[0] = TCP_OPT_SACK;
    options[1] = len;
    
    for (int i = 0; i < pcb->sack_count; i++) {
        uint8_t *block = &options[2 + i * 8];
        block[0] = (pcb->sack_blocks[i].start >> 24) & 0xFF;
        block[1] = (pcb->sack_blocks[i].start >> 16) & 0xFF;
        block[2] = (pcb->sack_blocks[i].start >> 8) & 0xFF;
        block[3] = pcb->sack_blocks[i].start & 0xFF;
        block[4] = (pcb->sack_blocks[i].end >> 24) & 0xFF;
        block[5] = (pcb->sack_blocks[i].end >> 16) & 0xFF;
        block[6] = (pcb->sack_blocks[i].end >> 8) & 0xFF;
        block[7] = pcb->sack_blocks[i].end & 0xFF;
    }
    
    return len;
}

/* 更新 SACK 块 (收到新数据时，根据乱序队列更新) */
void tcp_sack_update(struct tcp_pcb *pcb) {
    if (pcb == NULL) return;
    
    /* 清除旧 SACK 块 */
    pcb->sack_count = 0;
    
    /* 根据乱序队列生成新的 SACK 块 */
    struct tcp_ooseg *seg = pcb->ooseq;
    while (seg && pcb->sack_count < 4) {
        pcb->sack_blocks[pcb->sack_count].start = seg->seq;
        pcb->sack_blocks[pcb->sack_count].end = seg->seq + seg->len;
        pcb->sack_count++;
        seg = seg->next;
    }
}

/* 清除 SACK 块 */
void tcp_sack_clear(struct tcp_pcb *pcb) {
    if (pcb != NULL) {
        pcb->sack_count = 0;
    }
}

/* ========== TCP 窗口缩放 ========== */

/* 协商窗口缩放 (SYN 时调用) */
void tcp_wscale_negotiate(struct tcp_pcb *pcb, int is_syn) {
    if (pcb == NULL) return;
    
    if (is_syn) {
        /* 请求窗口缩放，发送端缩放值根据接收缓冲区计算 */
        pcb->wscale_requested = 1;
        pcb->rcv_wscale = 0;
        
        /* 计算接收端需要的缩放值 */
        uint32_t rcv_buf = 64 * 1024;  /* 默认 64KB 接收缓冲 */
        while (rcv_buf > 65535 && pcb->rcv_wscale < 14) {
            rcv_buf >>= 1;
            pcb->rcv_wscale++;
        }
    }
}

/* 应用窗口缩放 */
uint32_t tcp_wscale_expand(uint16_t window, uint8_t wscale) {
    return (uint32_t)window << wscale;
}

/* ========== TCP Keep-Alive ========== */

/* 设置 Keep-Alive 参数 */
int tcp_set_keepalive(struct tcp_pcb *pcb, int idle, int interval, int count) {
    if (pcb == NULL) return -1;
    
    pcb->keep_idle = idle > 0 ? idle * 1000 : TCP_KEEPALIVE_IDLE_DEFAULT;
    pcb->keep_interval = interval > 0 ? interval * 1000 : TCP_KEEPALIVE_INTERVAL_DEFAULT;
    pcb->keep_count = count > 0 ? count : TCP_KEEPALIVE_COUNT_DEFAULT;
    pcb->keep_enabled = 1;
    pcb->keep_probes = 0;
    
    return 0;
}

/* Keep-Alive 定时器处理 */
void tcp_keepalive_timer(struct tcp_pcb *pcb) {
    if (pcb == NULL || !pcb->keep_enabled) return;
    if (pcb->state != TCP_ESTABLISHED) return;
    
    uint64_t now = get_tick_count();
    
    /* 检查是否需要发送 Keep-Alive 探测 */
    if (now - pcb->last_activity > (uint64_t)pcb->keep_idle) {
        if (pcb->keep_probes == 0) {
            /* 首次探测 */
            tcp_send_empty_ack(pcb);
            pcb->keep_probes = 1;
            pcb->last_keep_probe = now;
        } else if (now - pcb->last_keep_probe > (uint64_t)pcb->keep_interval) {
            if (pcb->keep_probes < pcb->keep_count) {
                /* 发送 Keep-Alive 探测 */
                tcp_send_empty_ack(pcb);
                pcb->keep_probes++;
                pcb->last_keep_probe = now;
            } else {
                /* 超过最大探测次数，关闭连接 */
                tcp_abort(pcb);
            }
        }
    }
}

/* ========== TCP Nagle 算法 ========== */

/* 检查 Nagle 算法是否允许发送 */
int tcp_nagle_check(struct tcp_pcb *pcb) {
    if (pcb == NULL) return 0;
    
    /* Nagle 算法禁用时总是可以发送 */
    if (pcb->nodelay) return 1;
    
    /* 有未确认数据时，等待 */
    if (pcb->snd_una != pcb->snd_nxt) return 0;
    
    /* 数据量达到 MSS 时可以发送 */
    size_t pending = ringbuf_pending(pcb->snd_buf);
    if (pending >= pcb->mss) return 1;
    
    /* Nagle 延迟超时后也可以发送 */
    if (pcb->nagle_time > 0) {
        uint64_t now = get_tick_count();
        if (now - pcb->nagle_time >= TCP_NAGLE_DELAY) {
            return 1;
        }
    }
    
    return 0;
}

/* 设置 TCP_NODELAY */
int tcp_set_nodelay(struct tcp_pcb *pcb, int nodelay) {
    if (pcb == NULL) return -1;
    
    pcb->nodelay = nodelay;
    if (nodelay) {
        /* 禁用 Nagle 时，立即触发发送 */
        tcp_output(pcb);
    }
    return 0;
}

/* ========== TCP 零拷贝优化 ========== */

/* 散布/聚集 I/O */
struct iovec {
    void *iov_base;
    size_t iov_len;
};

/* 零拷贝发送 */
int tcp_sendmsg(struct tcp_pcb *pcb, const struct iovec *iov, int iovcnt) {
    if (pcb == NULL || iov == NULL || iovcnt <= 0) return -1;
    
    int total_sent = 0;
    
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_base == NULL || iov[i].iov_len == 0) continue;
        
        int sent = tcp_send(pcb, iov[i].iov_base, iov[i].iov_len);
        if (sent < 0) break;
        
        total_sent += sent;
    }
    
    return total_sent;
}

/* 零拷贝接收 */
int tcp_recvmsg(struct tcp_pcb *pcb, const struct iovec *iov, int iovcnt) {
    if (pcb == NULL || iov == NULL || iovcnt <= 0) return -1;
    
    int total_recv = 0;
    
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_base == NULL || iov[i].iov_len == 0) continue;
        
        int recvd = tcp_recv(pcb, iov[i].iov_base, iov[i].iov_len);
        if (recvd < 0) break;
        
        total_recv += recvd;
    }
    
    return total_recv;
}

/* ========== TCP 快速路径优化 ========== */

/* TCP 快速路径缓存 */
struct tcp_fast_cache {
    uint32_t local_addr;
    uint32_t remote_addr;
    uint16_t local_port;
    uint16_t remote_port;
    struct tcp_pcb *pcb;
    uint64_t last_use;
    int valid;
};

#define TCP_FAST_CACHE_SIZE     64
static struct tcp_fast_cache fast_cache[TCP_FAST_CACHE_SIZE];

/* 快速路径查找 */
struct tcp_pcb *tcp_fast_lookup(uint32_t local_addr, uint16_t local_port,
                                 uint32_t remote_addr, uint16_t remote_port) {
    /* 检查快速缓存 */
    for (int i = 0; i < TCP_FAST_CACHE_SIZE; i++) {
        struct tcp_fast_cache *fc = &fast_cache[i];
        
        if (fc->valid &&
            fc->local_addr == local_addr &&
            fc->local_port == local_port &&
            fc->remote_addr == remote_addr &&
            fc->remote_port == remote_port) {
            fc->last_use = get_tick_count();
            return fc->pcb;
        }
    }
    
    /* 缓存未命中，常规查找 */
    struct tcp_pcb *pcb = tcp_find_pcb(local_addr, local_port, 
                                        remote_addr, remote_port);
    if (pcb == NULL) return NULL;
    
    /* 更新缓存 */
    int oldest = 0;
    uint64_t oldest_time = fast_cache[0].last_use;
    
    for (int i = 1; i < TCP_FAST_CACHE_SIZE; i++) {
        if (!fast_cache[i].valid) {
            oldest = i;
            break;
        }
        if (fast_cache[i].last_use < oldest_time) {
            oldest_time = fast_cache[i].last_use;
            oldest = i;
        }
    }
    
    fast_cache[oldest].local_addr = local_addr;
    fast_cache[oldest].local_port = local_port;
    fast_cache[oldest].remote_addr = remote_addr;
    fast_cache[oldest].remote_port = remote_port;
    fast_cache[oldest].pcb = pcb;
    fast_cache[oldest].last_use = get_tick_count();
    fast_cache[oldest].valid = 1;
    
    return pcb;
}

/* 清除快速缓存 */
void tcp_fast_cache_flush(void) {
    for (int i = 0; i < TCP_FAST_CACHE_SIZE; i++) {
        fast_cache[i].valid = 0;
    }
}

/* ========== TCP 性能调优 ========== */

/* 设置发送缓冲区大小 */
int tcp_set_snd_buffer(struct tcp_pcb *pcb, size_t size) {
    if (pcb == NULL || size < 4096) return -1;
    
    struct ring_buffer *new_buf = ringbuf_create(size);
    if (new_buf == NULL) return -1;
    
    /* 复制旧数据 */
    if (pcb->snd_buf != NULL) {
        /* TODO: 复制数据到新缓冲区 */
        ringbuf_destroy(pcb->snd_buf);
    }
    
    pcb->snd_buf = new_buf;
    return 0;
}

/* 设置接收缓冲区大小 */
int tcp_set_rcv_buffer(struct tcp_pcb *pcb, size_t size) {
    if (pcb == NULL || size < 4096) return -1;
    
    struct ring_buffer *new_buf = ringbuf_create(size);
    if (new_buf == NULL) return -1;
    
    if (pcb->rcv_buf != NULL) {
        ringbuf_destroy(pcb->rcv_buf);
    }
    
    pcb->rcv_buf = new_buf;
    return 0;
}

/* 获取 TCP 连接信息 */
void tcp_get_info(struct tcp_pcb *pcb, char *buf, size_t len) {
    if (pcb == NULL || buf == NULL || len < 128) return;
    
    int pos = 0;
    pos += snprintf(buf + pos, len - pos, "Local: %d.%d.%d.%d:%d\n",
                    (pcb->local_ip >> 24) & 0xFF,
                    (pcb->local_ip >> 16) & 0xFF,
                    (pcb->local_ip >> 8) & 0xFF,
                    pcb->local_ip & 0xFF,
                    pcb->local_port);
    
    pos += snprintf(buf + pos, len - pos, "Remote: %d.%d.%d.%d:%d\n",
                    (pcb->remote_ip >> 24) & 0xFF,
                    (pcb->remote_ip >> 16) & 0xFF,
                    (pcb->remote_ip >> 8) & 0xFF,
                    pcb->remote_ip & 0xFF,
                    pcb->remote_port);
    
    pos += snprintf(buf + pos, len - pos, "State: %s\n",
                    tcp_state_name(pcb->state));
    
    pos += snprintf(buf + pos, len - pos, "CWND: %u  SSTHRESH: %u\n",
                    pcb->cwnd, pcb->ssthresh);
    
    pos += snprintf(buf + pos, len - pos, "SND_UNA: %u  SND_NXT: %u\n",
                    pcb->snd_una, pcb->snd_nxt);
}

/* ========== 辅助函数实现 ========== */

/* 查找 TCP PCB */
struct tcp_pcb *tcp_find_pcb(uint32_t local_addr, uint16_t local_port,
                              uint32_t remote_addr, uint16_t remote_port) {
    struct tcp_pcb *pcb = tcp_pcb_active_list;
    
    while (pcb != NULL) {
        if (pcb->local_ip == local_addr &&
            pcb->local_port == local_port &&
            pcb->remote_ip == remote_addr &&
            pcb->remote_port == remote_port) {
            return pcb;
        }
        pcb = pcb->next;
    }
    
    return NULL;
}

/* 发送空 ACK */
void tcp_send_empty_ack(struct tcp_pcb *pcb) {
    if (pcb == NULL) return;
    tcp_send_ack(pcb);
}

/* 中止 TCP 连接 */
void tcp_abort(struct tcp_pcb *pcb) {
    if (pcb == NULL) return;
    
    /* 发送 RST */
    tcp_send_reset(pcb, pcb->snd_nxt);
    
    /* 清理 */
    pcb->state = TCP_CLOSED;
    tcp_free(pcb);
}

/* 获取环形缓冲区待处理数据量 */
size_t ringbuf_pending(struct ring_buffer *rb) {
    if (rb == NULL) return 0;
    return rb->head - rb->tail;
}

/* 获取 TCP 状态名称 */
const char *tcp_state_name(tcp_state_t state) {
    switch (state) {
        case TCP_CLOSED:       return "CLOSED";
        case TCP_LISTEN:       return "LISTEN";
        case TCP_SYN_SENT:     return "SYN_SENT";
        case TCP_SYN_RECEIVED: return "SYN_RECEIVED";
        case TCP_ESTABLISHED:  return "ESTABLISHED";
        case TCP_FIN_WAIT_1:   return "FIN_WAIT_1";
        case TCP_FIN_WAIT_2:   return "FIN_WAIT_2";
        case TCP_CLOSE_WAIT:   return "CLOSE_WAIT";
        case TCP_CLOSING:      return "CLOSING";
        case TCP_LAST_ACK:     return "LAST_ACK";
        case TCP_TIME_WAIT:    return "TIME_WAIT";
        default:               return "UNKNOWN";
    }
}

/* 简化的 snprintf 实现 */
int snprintf(char *str, size_t size, const char *format, ...) {
    if (str == NULL || size == 0) return 0;
    
    va_list args;
    va_start(args, format);
    
    int written = 0;
    const char *p = format;
    
    while (*p && written < (int)(size - 1)) {
        if (*p == '%' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'd': {
                    int val = va_arg(args, int);
                    if (val < 0) {
                        str[written++] = '-';
                        val = -val;
                    }
                    /* 简单的整数转字符串 */
                    char temp[16];
                    int i = 0;
                    if (val == 0) {
                        temp[i++] = '0';
                    } else {
                        while (val > 0) {
                            temp[i++] = '0' + (val % 10);
                            val /= 10;
                        }
                    }
                    while (i > 0 && written < (int)(size - 1)) {
                        str[written++] = temp[--i];
                    }
                    break;
                }
                case 'u': {
                    unsigned int val = va_arg(args, unsigned int);
                    char temp[16];
                    int i = 0;
                    if (val == 0) {
                        temp[i++] = '0';
                    } else {
                        while (val > 0) {
                            temp[i++] = '0' + (val % 10);
                            val /= 10;
                        }
                    }
                    while (i > 0 && written < (int)(size - 1)) {
                        str[written++] = temp[--i];
                    }
                    break;
                }
                case 's': {
                    const char *s = va_arg(args, const char *);
                    while (*s && written < (int)(size - 1)) {
                        str[written++] = *s++;
                    }
                    break;
                }
                case '%':
                    str[written++] = '%';
                    break;
                                default:
                                    str[written++] = '%';
                                    if (written < (int)(size - 1)) {
                                        str[written++] = *p;
                                    }
                                    break;
                            }
                        } else {
                            str[written++] = *p;
                        }
                        p++;
                    }
                    
                    str[written] = '\0';
                    va_end(args);
                    return written;
                }
                
                /* ========== TCP 高级增强功能 ========== */
                
                /* === TCP 统计增强 === */
                
                struct tcp_stats_detail {
                    uint64_t active_opens;
                    uint64_t passive_opens;
                    uint64_t attempt_fails;
                    uint64_t estab_resets;
                    uint64_t checksum_errors;
                    uint64_t listen_drops;
                    uint64_t accept_overflows;
                };
                
                static struct tcp_stats_detail tcp_stats_full;
                
                /* 记录 TCP 事件 */
                void tcp_record_event(int event) {
                    switch (event) {
                        case 0: tcp_stats_full.active_opens++; break;
                        case 1: tcp_stats_full.passive_opens++; break;
                        case 2: tcp_stats_full.attempt_fails++; break;
                        case 3: tcp_stats_full.estab_resets++; break;
                        case 4: tcp_stats_full.checksum_errors++; break;
                        case 5: tcp_stats_full.listen_drops++; break;
                    }
                }
                
                /* 打印 TCP 统计 */
                void tcp_print_stats_full(void) {
                    println("=== TCP Statistics ===");
                    print("Active Opens:    "); print_dec(tcp_stats_full.active_opens); println("");
                    print("Passive Opens:   "); print_dec(tcp_stats_full.passive_opens); println("");
                    print("Attempt Fails:   "); print_dec(tcp_stats_full.attempt_fails); println("");
                    print("In Segments:     "); print_dec(tcp_in_segs); println("");
                    print("Out Segments:    "); print_dec(tcp_out_segs); println("");
                    print("Retransmits:     "); print_dec(tcp_retrans_segs); println("");
                    print("Checksum Errors: "); print_dec(tcp_stats_full.checksum_errors); println("");
                }

/* ========== TCP 高级增强功能 ========== */

/* === TCP 零拷贝传输 === */

/* 零拷贝发送缓冲区描述符 */
struct tcp_zc_buf {
    void *base;              /* 数据基址 */
    size_t len;              /* 数据长度 */
    size_t offset;           /* 当前偏移 */
    void (*completion)(void *base, size_t len, void *user);  /* 完成回调 */
    void *user_data;         /* 用户数据 */
    int in_use;
};

#define TCP_ZC_MAX_BUFS     16

static struct tcp_zc_buf zc_bufs[TCP_ZC_MAX_BUFS];
static volatile int zc_lk = 0;

/* 初始化零拷贝系统 */
void tcp_zc_init(void) {
    for (int i = 0; i < TCP_ZC_MAX_BUFS; i++) {
        zc_bufs[i].in_use = 0;
    }
}

/* 注册零拷贝缓冲区 */
int tcp_zc_register(void *base, size_t len, 
                     void (*completion)(void *, size_t, void *), void *user) {
    while (__sync_lock_test_and_set(&zc_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < TCP_ZC_MAX_BUFS; i++) {
        if (!zc_bufs[i].in_use) {
            zc_bufs[i].base = base;
            zc_bufs[i].len = len;
            zc_bufs[i].offset = 0;
            zc_bufs[i].completion = completion;
            zc_bufs[i].user_data = user;
            zc_bufs[i].in_use = 1;
            
            __sync_lock_release(&zc_lk);
            return i;
        }
    }
    
    __sync_lock_release(&zc_lk);
    return -1;
}

/* 零拷贝发送 */
size_t tcp_zc_send(struct tcp_pcb *pcb, int zc_id, size_t offset, size_t len) {
    if (pcb == NULL || zc_id < 0 || zc_id >= TCP_ZC_MAX_BUFS) {
        return 0;
    }
    
    while (__sync_lock_test_and_set(&zc_lk, 1)) {
        asm volatile("pause");
    }
    
    if (!zc_bufs[zc_id].in_use) {
        __sync_lock_release(&zc_lk);
        return 0;
    }
    
    struct tcp_zc_buf *zb = &zc_bufs[zc_id];
    
    /* 计算可发送范围 */
    if (offset >= zb->len) {
        __sync_lock_release(&zc_lk);
        return 0;
    }
    
    size_t avail = zb->len - offset;
    if (len > avail) len = avail;
    
    /* 直接发送 (无需复制到发送缓冲区) */
    uint8_t *data = (uint8_t *)zb->base + offset;
    struct net_buf buf;
    
    if (tcp_build_segment(pcb, TCP_ACK | TCP_PSH, data, len, &buf) == 0) {
        pcb->snd_nxt += len;
        zb->offset = offset + len;
        
        if (zb->offset >= zb->len && zb->completion) {
            zb->completion(zb->base, zb->len, zb->user_data);
            zb->in_use = 0;
        }
        
        __sync_lock_release(&zc_lk);
        return len;
    }
    
    __sync_lock_release(&zc_lk);
    return 0;
}

/* 取消零拷贝缓冲区 */
void tcp_zc_cancel(int zc_id) {
    if (zc_id < 0 || zc_id >= TCP_ZC_MAX_BUFS) return;
    
    while (__sync_lock_test_and_set(&zc_lk, 1)) {
        asm volatile("pause");
    }
    
    zc_bufs[zc_id].in_use = 0;
    
    __sync_lock_release(&zc_lk);
}

/* === TCP 快速打开 (TFO) === */

#define TFO_COOKIE_MAX          16
#define TFO_PENDING_MAX         8

struct tfo_cookie {
    uint32_t remote_ip;
    uint16_t remote_port;
    uint8_t cookie[8];
    uint64_t create_time;
    int valid;
};

static struct tfo_cookie tfo_cookies[TFO_COOKIE_MAX];
static volatile int tfo_lk = 0;

/* 初始化 TFO */
void tcp_tfo_init(void) {
    for (int i = 0; i < TFO_COOKIE_MAX; i++) {
        tfo_cookies[i].valid = 0;
    }
}

/* 生成 TFO Cookie */
static void tfo_gen_cookie(uint32_t remote_ip, uint8_t *cookie) {
    /* 简化的 Cookie 生成 (实际应使用加密算法) */
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    
    uint32_t hash = remote_ip ^ (uint32_t)tsc;
    for (int i = 0; i < 8; i++) {
        cookie[i] = (hash >> (i * 4)) & 0xFF;
        hash = hash * 1103515245 + 12345;
    }
}

/* 查找 TFO Cookie */
int tcp_tfo_find_cookie(uint32_t remote_ip, uint16_t remote_port, uint8_t *cookie) {
    while (__sync_lock_test_and_set(&tfo_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < TFO_COOKIE_MAX; i++) {
        if (tfo_cookies[i].valid && 
            tfo_cookies[i].remote_ip == remote_ip &&
            tfo_cookies[i].remote_port == remote_port) {
            if (cookie) {
                for (int j = 0; j < 8; j++) {
                    cookie[j] = tfo_cookies[i].cookie[j];
                }
            }
            __sync_lock_release(&tfo_lk);
            return 1;
        }
    }
    
    __sync_lock_release(&tfo_lk);
    return 0;
}

/* 保存 TFO Cookie */
void tcp_tfo_save_cookie(uint32_t remote_ip, uint16_t remote_port, const uint8_t *cookie) {
    while (__sync_lock_test_and_set(&tfo_lk, 1)) {
        asm volatile("pause");
    }
    
    /* 查找现有条目或空闲槽 */
    int slot = -1;
    for (int i = 0; i < TFO_COOKIE_MAX; i++) {
        if (tfo_cookies[i].valid &&
            tfo_cookies[i].remote_ip == remote_ip &&
            tfo_cookies[i].remote_port == remote_port) {
            slot = i;
            break;
        }
        if (!tfo_cookies[i].valid && slot < 0) {
            slot = i;
        }
    }
    
    if (slot >= 0) {
        tfo_cookies[slot].remote_ip = remote_ip;
        tfo_cookies[slot].remote_port = remote_port;
        for (int i = 0; i < 8; i++) {
            tfo_cookies[slot].cookie[i] = cookie[i];
        }
        tfo_cookies[slot].create_time = get_tick_count();
        tfo_cookies[slot].valid = 1;
    }
    
    __sync_lock_release(&tfo_lk);
}

/* TFO 连接 (携带数据) */
int tcp_tfo_connect(struct tcp_pcb *pcb, uint32_t addr, uint16_t port,
                     const void *data, size_t len) {
    if (pcb == NULL) return -1;
    
    /* 分配本地端口 */
    if (pcb->local_port == 0) {
        pcb->local_port = allocate_port();
        if (pcb->local_port == 0) return -1;
    }
    
    pcb->remote_ip = addr;
    pcb->remote_port = port;
    
    /* 检查是否有缓存的 Cookie */
    uint8_t cookie[8];
    int has_cookie = tcp_tfo_find_cookie(addr, port, cookie);
    
    if (!has_cookie) {
        /* 第一次连接：生成 Cookie 并发送 SYN */
        tfo_gen_cookie(addr, cookie);
    }
    
    pcb->state = TCP_SYN_SENT;
    
    /* 构建 SYN + Cookie + 数据包 */
    struct net_buf buf;
    
    /* 这里简化处理：发送 SYN 并携带数据 */
    if (tcp_build_segment(pcb, TCP_SYN, NULL, 0, &buf) == 0) {
        pcb->snd_nxt = pcb->iss + 1;
        pcb->snd_max = pcb->snd_nxt;
        tcp_out_segs++;
        
        /* 如果有 Cookie 且有数据，同时发送数据 */
        if (has_cookie && data && len > 0) {
            tcp_send(pcb, data, len);
        }
    }
    
    pcb->retry_time = get_tick_count() + pcb->rto;
    pcb->retry_count = TCP_SYN_RETRIES;
    
    return 0;
}

/* === TCP 加权公平队列 (WFQ) === */

#define WFQ_MAX_FLOWS       32

struct wfq_flow {
    struct tcp_pcb *pcb;
    uint32_t weight;
    uint64_t bytes_sent;
    uint64_t virtual_time;
    int active;
};

static struct wfq_flow wfq_flows[WFQ_MAX_FLOWS];
static uint64_t wfq_virtual_time = 0;
static volatile int wfq_lk = 0;

/* 初始化 WFQ */
void tcp_wfq_init(void) {
    for (int i = 0; i < WFQ_MAX_FLOWS; i++) {
        wfq_flows[i].active = 0;
    }
}

/* 添加流到 WFQ */
int tcp_wfq_add_flow(struct tcp_pcb *pcb, uint32_t weight) {
    if (pcb == NULL) return -1;
    
    while (__sync_lock_test_and_set(&wfq_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < WFQ_MAX_FLOWS; i++) {
        if (!wfq_flows[i].active) {
            wfq_flows[i].pcb = pcb;
            wfq_flows[i].weight = weight > 0 ? weight : 1;
            wfq_flows[i].bytes_sent = 0;
            wfq_flows[i].virtual_time = wfq_virtual_time;
            wfq_flows[i].active = 1;
            
            __sync_lock_release(&wfq_lk);
            return i;
        }
    }
    
    __sync_lock_release(&wfq_lk);
    return -1;
}

/* WFQ 调度：选择下一个发送的流 */
struct tcp_pcb *tcp_wfq_schedule(void) {
    struct tcp_pcb *selected = NULL;
    uint64_t min_vt = ~0ULL;
    int min_idx = -1;
    
    while (__sync_lock_test_and_set(&wfq_lk, 1)) {
        asm volatile("pause");
    }
    
    uint32_t total_weight = 0;
    for (int i = 0; i < WFQ_MAX_FLOWS; i++) {
        if (wfq_flows[i].active) {
            total_weight += wfq_flows[i].weight;
        }
    }
    
    if (total_weight == 0) {
        __sync_lock_release(&wfq_lk);
        return NULL;
    }
    
    /* 选择虚拟时间最小的流 */
    for (int i = 0; i < WFQ_MAX_FLOWS; i++) {
        if (wfq_flows[i].active && wfq_flows[i].virtual_time < min_vt) {
            min_vt = wfq_flows[i].virtual_time;
            min_idx = i;
        }
    }
    
    if (min_idx >= 0) {
        selected = wfq_flows[min_idx].pcb;
        /* 更新虚拟时间 */
        wfq_flows[min_idx].bytes_sent += 1;
        wfq_flows[min_idx].virtual_time += 
            (WFQ_MAX_FLOWS * 1000) / wfq_flows[min_idx].weight;
        wfq_virtual_time = wfq_flows[min_idx].virtual_time;
    }
    
    __sync_lock_release(&wfq_lk);
    return selected;
}

/* 移除 WFQ 流 */
void tcp_wfq_remove_flow(struct tcp_pcb *pcb) {
    if (pcb == NULL) return;
    
    while (__sync_lock_test_and_set(&wfq_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < WFQ_MAX_FLOWS; i++) {
        if (wfq_flows[i].active && wfq_flows[i].pcb == pcb) {
            wfq_flows[i].active = 0;
            break;
        }
    }
    
    __sync_lock_release(&wfq_lk);
}

/* === TCP 丢包恢复增强 === */

/* 前向纠错 (FEC) 数据块 */
struct tcp_fec_block {
    uint32_t seq_start;
    uint32_t seq_end;
    uint8_t *parity_data;
    size_t parity_len;
    int valid;
};

#define TCP_FEC_MAX_BLOCKS      8

static struct tcp_fec_block fec_blocks[TCP_FEC_MAX_BLOCKS];
static volatile int fec_lk = 0;

/* 初始化 FEC */
void tcp_fec_init(void) {
    for (int i = 0; i < TCP_FEC_MAX_BLOCKS; i++) {
        fec_blocks[i].valid = 0;
    }
}

/* 生成 FEC 奇偶校验数据 */
void tcp_fec_generate(struct tcp_pcb *pcb, uint32_t seq_start, uint32_t seq_end) {
    if (pcb == NULL) return;
    
    while (__sync_lock_test_and_set(&fec_lk, 1)) {
        asm volatile("pause");
    }
    
    /* 查找空闲块 */
    int slot = -1;
    for (int i = 0; i < TCP_FEC_MAX_BLOCKS; i++) {
        if (!fec_blocks[i].valid) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        __sync_lock_release(&fec_lk);
        return;
    }
    
    /* 简化的 XOR 奇偶校验生成 */
    size_t block_size = (seq_end - seq_start) * pcb->mss;
    fec_blocks[slot].parity_data = kmalloc(block_size);
    
    if (fec_blocks[slot].parity_data) {
        for (size_t i = 0; i < block_size; i++) {
            fec_blocks[slot].parity_data[i] = 0;
        }
        
        /* XOR 所有数据块 */
        for (uint32_t seq = seq_start; seq < seq_end; seq++) {
            /* 这里简化：实际需要从缓冲区获取数据 */
        }
        
        fec_blocks[slot].seq_start = seq_start;
        fec_blocks[slot].seq_end = seq_end;
        fec_blocks[slot].parity_len = block_size;
        fec_blocks[slot].valid = 1;
    }
    
    __sync_lock_release(&fec_lk);
}

/* 使用 FEC 恢复丢失数据 */
int tcp_fec_recover(struct tcp_pcb *pcb, uint32_t lost_seq) {
    while (__sync_lock_test_and_set(&fec_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < TCP_FEC_MAX_BLOCKS; i++) {
        if (fec_blocks[i].valid &&
            lost_seq >= fec_blocks[i].seq_start &&
            lost_seq < fec_blocks[i].seq_end) {
            
            /* 检查是否有足够的其他块来恢复 */
            uint32_t received = 0;
            for (uint32_t seq = fec_blocks[i].seq_start; 
                 seq < fec_blocks[i].seq_end; seq++) {
                if (seq != lost_seq) {
                    /* 检查是否已接收 */
                    received++;
                }
            }
            
            if (received >= fec_blocks[i].seq_end - fec_blocks[i].seq_start - 1) {
                /* 可以恢复 */
                /* 简化：实际需要 XOR 运算恢复 */
                __sync_lock_release(&fec_lk);
                return 1;
            }
        }
    }
    
    __sync_lock_release(&fec_lk);
    return 0;
}

/* RACK (Recent ACKnowledgment) 丢包检测 */
int tcp_rack_detect_loss(struct tcp_pcb *pcb, uint32_t seq) {
    if (pcb == NULL) return 0;
    
    /* RACK 算法：如果收到更高序号的 ACK，且时间差超过阈值，
       则认为较早的段丢失 */
    static uint32_t rack_rtt = 0;
    static uint32_t rack_end_seq = 0;
    static uint64_t rack_xmit_time = 0;
    
    if (seq_after(seq, rack_end_seq)) {
        rack_end_seq = seq;
        rack_xmit_time = get_tick_count();
    }
    
    uint64_t now = get_tick_count();
    uint64_t elapsed = now - rack_xmit_time;
    
    /* 如果超过 1/4 RTT，认为丢失 */
    if (elapsed > (pcb->srtt >> 2)) {
        return 1;
    }
    
    return 0;
}

/* === TCP 性能诊断 === */

struct tcp_perf_diag {
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    uint64_t segs_sent;
    uint64_t segs_recv;
    uint64_t retrans_total;
    uint64_t dupacks_total;
    uint64_t rtt_min;
    uint64_t rtt_max;
    uint64_t rtt_avg;
    uint32_t cwnd_max;
    uint32_t cwnd_min;
    uint64_t bandwidth_estimate;
    int sample_count;
};

static struct tcp_perf_diag tcp_diag;

/* 更新诊断数据 */
void tcp_diag_update(struct tcp_pcb *pcb) {
    if (pcb == NULL) return;
    
    tcp_diag.sample_count++;
    
    /* 更新 RTT 统计 */
    uint64_t rtt = pcb->srtt >> 3;
    if (tcp_diag.sample_count == 1 || rtt < tcp_diag.rtt_min) {
        tcp_diag.rtt_min = rtt;
    }
    if (rtt > tcp_diag.rtt_max) {
        tcp_diag.rtt_max = rtt;
    }
    tcp_diag.rtt_avg = (tcp_diag.rtt_avg * 7 + rtt) / 8;
    
    /* 更新 CWND 统计 */
    if (pcb->cwnd > tcp_diag.cwnd_max) {
        tcp_diag.cwnd_max = pcb->cwnd;
    }
    if (tcp_diag.sample_count == 1 || pcb->cwnd < tcp_diag.cwnd_min) {
        tcp_diag.cwnd_min = pcb->cwnd;
    }
    
    /* 估计带宽 */
    if (rtt > 0) {
        tcp_diag.bandwidth_estimate = 
            (uint64_t)pcb->cwnd * 8 * 1000 / rtt;  /* bits/sec */
    }
}

/* 打印诊断报告 */
void tcp_diag_print_report(void) {
    println("");
    println("========================================");
    println("       TCP Performance Diagnostic       ");
    println("========================================");
    println("");
    
    println("--- Traffic Statistics ---");
    print("Bytes Sent: ");
    print_dec(tcp_diag.bytes_sent);
    println("");
    
    print("Bytes Received: ");
    print_dec(tcp_diag.bytes_recv);
    println("");
    
    print("Segments Sent: ");
    print_dec(tcp_diag.segs_sent);
    println("");
    
    print("Segments Received: ");
    print_dec(tcp_diag.segs_recv);
    println("");
    
    println("");
    println("--- Retransmission ---");
    print("Total Retransmissions: ");
    print_dec(tcp_diag.retrans_total);
    println("");
    
    print("Duplicate ACKs: ");
    print_dec(tcp_diag.dupacks_total);
    println("");
    
    println("");
    println("--- RTT Analysis ---");
    print("RTT Min: ");
    print_dec(tcp_diag.rtt_min);
    println(" ms");
    
    print("RTT Max: ");
    print_dec(tcp_diag.rtt_max);
    println(" ms");
    
    print("RTT Avg: ");
    print_dec(tcp_diag.rtt_avg);
    println(" ms");
    
    println("");
    println("--- Congestion Window ---");
    print("CWND Min: ");
    print_dec(tcp_diag.cwnd_min);
    println(" bytes");
    
    print("CWND Max: ");
    print_dec(tcp_diag.cwnd_max);
    println(" bytes");
    
    println("");
    println("--- Bandwidth Estimate ---");
    print("Estimated: ");
    print_dec(tcp_diag.bandwidth_estimate / 1000);
    println(" Kbps");
    
    println("");
    println("========================================");
    println("          End of Diagnostic");
    println("========================================");
    println("");
}

/* === TCP 流量整形 === */

#define TSHAPER_MAX_FLOWS       16
#define TSHAPER_BUCKET_SIZE     65536
#define TSHAPER_REFILL_RATE     1000000  /* 1 Mbps */

struct tshaper_flow {
    struct tcp_pcb *pcb;
    uint64_t bucket_tokens;
    uint64_t max_tokens;
    uint64_t refill_rate;    /* tokens per second */
    uint64_t last_refill;
    int active;
};

static struct tshaper_flow tshaper_flows[TSHAPER_MAX_FLOWS];
static volatile int tshaper_lk = 0;

/* 初始化流量整形器 */
void tcp_tshaper_init(void) {
    for (int i = 0; i < TSHAPER_MAX_FLOWS; i++) {
        tshaper_flows[i].active = 0;
    }
}

/* 添加流到整形器 */
int tcp_tshaper_add_flow(struct tcp_pcb *pcb, uint64_t rate_bps) {
    if (pcb == NULL) return -1;
    
    while (__sync_lock_test_and_set(&tshaper_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < TSHAPER_MAX_FLOWS; i++) {
        if (!tshaper_flows[i].active) {
            tshaper_flows[i].pcb = pcb;
            tshaper_flows[i].bucket_tokens = TSHAPER_BUCKET_SIZE;
            tshaper_flows[i].max_tokens = TSHAPER_BUCKET_SIZE;
            tshaper_flows[i].refill_rate = rate_bps / 8;  /* bytes/sec */
            tshaper_flows[i].last_refill = get_tick_count();
            tshaper_flows[i].active = 1;
            
            __sync_lock_release(&tshaper_lk);
            return i;
        }
    }
    
    __sync_lock_release(&tshaper_lk);
    return -1;
}

/* 检查是否可以发送 */
int tcp_tshaper_can_send(struct tcp_pcb *pcb, size_t bytes) {
    if (pcb == NULL) return 1;
    
    while (__sync_lock_test_and_set(&tshaper_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < TSHAPER_MAX_FLOWS; i++) {
        if (tshaper_flows[i].active && tshaper_flows[i].pcb == pcb) {
            /* 补充令牌 */
            uint64_t now = get_tick_count();
            uint64_t elapsed = now - tshaper_flows[i].last_refill;
            
            /* elapsed 是纳秒，转换为秒并计算令牌 */
            uint64_t new_tokens = (tshaper_flows[i].refill_rate * elapsed) / 1000000000ULL;
            tshaper_flows[i].bucket_tokens += new_tokens;
            
            if (tshaper_flows[i].bucket_tokens > tshaper_flows[i].max_tokens) {
                tshaper_flows[i].bucket_tokens = tshaper_flows[i].max_tokens;
            }
            
            tshaper_flows[i].last_refill = now;
            
            /* 检查令牌是否足够 */
            if (tshaper_flows[i].bucket_tokens >= bytes) {
                tshaper_flows[i].bucket_tokens -= bytes;
                __sync_lock_release(&tshaper_lk);
                return 1;
            }
            
            __sync_lock_release(&tshaper_lk);
            return 0;
        }
    }
    
    __sync_lock_release(&tshaper_lk);
    return 1;  /* 未找到流，允许发送 */
}

/* 移除整形器流 */
void tcp_tshaper_remove_flow(struct tcp_pcb *pcb) {
    if (pcb == NULL) return;
    
    while (__sync_lock_test_and_set(&tshaper_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < TSHAPER_MAX_FLOWS; i++) {
        if (tshaper_flows[i].active && tshaper_flows[i].pcb == pcb) {
            tshaper_flows[i].active = 0;
            break;
        }
    }
    
    __sync_lock_release(&tshaper_lk);
}

/* === TCP 综合初始化增强 === */

void tcp_init_enhanced(void) {
    /* 初始化基础 TCP */
    tcp_init();
    
    /* 初始化高级功能 */
    tcp_zc_init();
    tcp_tfo_init();
    tcp_wfq_init();
    tcp_fec_init();
    tcp_tshaper_init();
    
    /* 初始化诊断数据 */
    tcp_diag.sample_count = 0;
    tcp_diag.rtt_min = ~0ULL;
    tcp_diag.rtt_max = 0;
    tcp_diag.rtt_avg = 0;
    tcp_diag.cwnd_max = 0;
    tcp_diag.cwnd_min = ~0U;
    tcp_diag.bandwidth_estimate = 0;
    
    println("[TCP] Enhanced TCP initialized");
}

/* === TCP 综合状态报告 === */

void tcp_print_comprehensive_report(void) {
    println("");
    println("========================================");
    println("       TCP Comprehensive Status         ");
    println("========================================");
    println("");
    
    /* 基础统计 */
    tcp_stats();
    
    /* 活动连接 */
    int active_count = 0;
    for (struct tcp_pcb *p = tcp_pcb_active_list; p; p = p->next) {
        active_count++;
    }
    
    print("Active Connections: ");
    print_dec(active_count);
    println("");
    
    /* ZC 状态 */
    int zc_active = 0;
    for (int i = 0; i < TCP_ZC_MAX_BUFS; i++) {
        if (zc_bufs[i].in_use) zc_active++;
    }
    print("Zero-Copy Buffers: ");
    print_dec(zc_active);
    print("/");
    print_dec(TCP_ZC_MAX_BUFS);
    println("");
    
    /* TFO 状态 */
    int tfo_count = 0;
    for (int i = 0; i < TFO_COOKIE_MAX; i++) {
        if (tfo_cookies[i].valid) tfo_count++;
    }
    print("TFO Cookies: ");
    print_dec(tfo_count);
    println("");
    
    /* WFQ 状态 */
    int wfq_count = 0;
    for (int i = 0; i < WFQ_MAX_FLOWS; i++) {
        if (wfq_flows[i].active) wfq_count++;
    }
    print("WFQ Active Flows: ");
    print_dec(wfq_count);
    println("");
    
    /* 整形器状态 */
    int shaper_count = 0;
    for (int i = 0; i < TSHAPER_MAX_FLOWS; i++) {
        if (tshaper_flows[i].active) shaper_count++;
    }
    print("Traffic Shaped Flows: ");
    print_dec(shaper_count);
    println("");
    
    /* 性能诊断 */
    tcp_diag_print_report();
}

