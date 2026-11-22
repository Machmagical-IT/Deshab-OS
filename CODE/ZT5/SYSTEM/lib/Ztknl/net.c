#include "net.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "ktypes.h"
#include "task.h"
#include "tcp.h"
#include "dhcp.h"

/* 前向声明 */
void net_tx_batch_flush(void);
static void udp_input(struct nic_device *nic, struct net_buf *buf,
                      struct ip_hdr *ip, uint8_t *udp_data, size_t udp_len);

/* 网卡表 */
#define MAX_NICS    8
static struct nic_device *nic_table[MAX_NICS];
static int nic_count = 0;

/* ARP 缓存 */
#define ARP_CACHE_SIZE  64
struct arp_entry {
    uint32_t ip;
    struct mac_addr mac;
    uint64_t time;
    int valid;
};
static struct arp_entry arp_cache[ARP_CACHE_SIZE];

/* 网络缓冲区池 */
#define NET_BUF_POOL_SIZE   64
static struct net_buf net_buf_pool[NET_BUF_POOL_SIZE];
static int net_buf_pool_used[NET_BUF_POOL_SIZE];
static spinlock_t net_buf_pool_lock = SPINLOCK_INIT;

/* 初始化网络子系统 */
int net_init(void) {
    /* 清空网卡表 */
    for (int i = 0; i < MAX_NICS; i++) {
        nic_table[i] = NULL;
    }
    nic_count = 0;
    
    /* 清空 ARP 缓存 */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_cache[i].valid = 0;
    }
    
    /* 初始化缓冲区池 */
    for (int i = 0; i < NET_BUF_POOL_SIZE; i++) {
        net_buf_pool_used[i] = 0;
    }
    
    /* 初始化环回接口 */
    loopback_init();
    
    return 0;
}

/* 注册网卡 */
int net_register_nic(struct nic_device *nic) {
    if (nic == NULL || nic_count >= MAX_NICS) {
        return -1;
    }
    
    nic_table[nic_count] = nic;
    nic->id = nic_count;
    nic_count++;
    
    // print("[NET] Registered NIC: ");
    // println(nic->name);
    
    return 0;
}

/* 注销网卡 */
int net_unregister_nic(struct nic_device *nic) {
    if (nic == NULL) {
        return -1;
    }
    
    for (int i = 0; i < nic_count; i++) {
        if (nic_table[i] == nic) {
            nic_table[i] = NULL;
            return 0;
        }
    }
    
    return -1;
}

/* 获取网卡 */
struct nic_device *net_get_nic(const char *name) {
    for (int i = 0; i < nic_count; i++) {
        if (nic_table[i] != NULL) {
            int j = 0;
            while (name[j] && nic_table[i]->name[j] && 
                   name[j] == nic_table[i]->name[j]) {
                j++;
            }
            if (name[j] == '\0' && nic_table[i]->name[j] == '\0') {
                return nic_table[i];
            }
        }
    }
    return NULL;
}

struct nic_device *net_get_nic_by_index(int index) {
    if (index >= 0 && index < nic_count) {
        return nic_table[index];
    }
    return NULL;
}

/* Get the first non-loopback NIC (for DHCP/autoconnect) */
struct nic_device *net_get_first_nic(void) {
    for (int i = 0; i < nic_count; i++) {
        struct nic_device *nic = nic_table[i];
        if (nic == NULL) continue;
        /* Skip loopback */
        if (nic->name[0] == 'l' && nic->name[1] == 'o') continue;
        if (nic->state == NIC_STATE_DOWN || nic->state == NIC_STATE_UP || nic->state == NIC_STATE_RUNNING) {
            return nic;
        }
    }
    return NULL;
}

int net_get_nic_count(void) {
    return nic_count;
}

/* 分配网络缓冲区 */
struct net_buf *net_buf_alloc(void) {
    spin_lock(&net_buf_pool_lock);
    for (int i = 0; i < NET_BUF_POOL_SIZE; i++) {
        if (!net_buf_pool_used[i]) {
            net_buf_pool_used[i] = 1;
            net_buf_pool[i].len = 0;
            net_buf_pool[i].next = NULL;
            spin_unlock(&net_buf_pool_lock);
            return &net_buf_pool[i];
        }
    }
    spin_unlock(&net_buf_pool_lock);
    return NULL;
}

/* 释放网络缓冲区 */
void net_buf_free(struct net_buf *buf) {
    if (buf == NULL) {
        return;
    }
    
    int index = buf - net_buf_pool;
    if (index >= 0 && index < NET_BUF_POOL_SIZE) {
        spin_lock(&net_buf_pool_lock);
        net_buf_pool_used[index] = 0;
        spin_unlock(&net_buf_pool_lock);
    }
}

/* IP 校验和 */
uint16_t ip_checksum(const struct ip_hdr *ip) {
    return net_checksum(ip, ip->ihl * 4);
}

/* 发送数据包 */
int net_send_packet(struct nic_device *nic, struct net_buf *buf) {
    if (nic == NULL || buf == NULL || nic->ops == NULL || nic->ops->send == NULL) {
        return -1;
    }
    
    int ret = nic->ops->send(nic->priv, buf);
    
    if (ret == 0) {
        nic->tx_packets++;
        nic->tx_bytes += buf->len;
    } else {
        nic->tx_errors++;
    }
    
    return ret;
}

/* 接收数据包 */
struct net_buf *net_recv_packet(struct nic_device *nic) {
    if (nic == NULL || nic->ops == NULL || nic->ops->recv == NULL) {
        return NULL;
    }
    
    struct net_buf *buf = nic->ops->recv(nic->priv);
    
    if (buf != NULL) {
        nic->rx_packets++;
        nic->rx_bytes += buf->len;
        
        /* 处理数据包 */
        net_handle_ethernet(nic, buf);
    }
    
    return buf;
}

/* 轮询所有网卡 (用于 ARP 等待) */
void net_poll(void) {
    for (int i = 0; i < nic_count; i++) {
        struct nic_device *nic = nic_table[i];
        if (nic == NULL || nic->ops == NULL || nic->ops->recv == NULL) continue;
        
        struct net_buf *buf = nic->ops->recv(nic->priv);
        if (buf != NULL) {
            nic->rx_packets++;
            nic->rx_bytes += buf->len;
            net_handle_ethernet(nic, buf);
        }
    }
}

/* 处理以太网帧 */
void net_handle_ethernet(struct nic_device *nic, struct net_buf *buf) {
    if (buf->len < sizeof(struct eth_hdr)) {
        net_buf_free(buf);
        return;
    }
    
    struct eth_hdr *eth = (struct eth_hdr *)buf->data;
    uint16_t proto = (eth->h_proto >> 8) | (eth->h_proto << 8);  /* 网络字节序转换 */
    
    /* 检查目标 MAC (广播/本机/组播) */
    /* 简化: 接收所有帧 */
    
    /* 根据协议分发 */
    switch (proto) {
        case ETH_P_IP:
            net_handle_ip(nic, buf);
            break;
        case ETH_P_ARP:
            net_handle_arp(nic, buf);
            break;
        case ETH_P_IPV6:
            /* IPv6 处理 (TODO) */
            net_buf_free(buf);
            break;
        default:
            net_buf_free(buf);
            break;
    }
}

/* 处理 IP 数据包 */
void net_handle_ip(struct nic_device *nic, struct net_buf *buf) {
    if (buf->len < sizeof(struct eth_hdr) + sizeof(struct ip_hdr)) {
        net_buf_free(buf);
        return;
    }
    
    struct eth_hdr *eth = (struct eth_hdr *)buf->data;
    struct ip_hdr *ip = (struct ip_hdr *)(eth + 1);
    
    /* 校验 IP 头 */
    if (ip->version != 4) {
        net_buf_free(buf);
        return;
    }
    
    /* 检查目标 IP */
    if (ip->daddr != nic->ip_addr && ip->daddr != 0xFFFFFFFF) {
        /* 不是发给我们的 */
        net_buf_free(buf);
        return;
    }
    
    /* 根据协议分发 */
    switch (ip->protocol) {
        case IPPROTO_TCP:
            net_handle_tcp(nic, buf);
            break;
        case IPPROTO_UDP:
            net_handle_udp(nic, buf);
            break;
        case IPPROTO_ICMP:
            net_handle_icmp(nic, buf);
            break;
        default:
            net_buf_free(buf);
            break;
    }
}

/* 处理 TCP 数据包 */
void net_handle_tcp(struct nic_device *nic, struct net_buf *buf) {
    struct eth_hdr *eth = (struct eth_hdr *)buf->data;
    struct ip_hdr *ip = (struct ip_hdr *)(eth + 1);

    /* 验证 IP 头长度 */
    if (ip->ihl < 5) {
        net_buf_free(buf);
        return;
    }

    /* 调用 TCP 输入处理 */
    tcp_input(buf, ip);

    net_buf_free(buf);
}

/* 处理 UDP 数据包 */
void net_handle_udp(struct nic_device *nic, struct net_buf *buf) {
    struct eth_hdr *eth = (struct eth_hdr *)buf->data;
    struct ip_hdr *ip = (struct ip_hdr *)(eth + 1);

    /* 验证 IP 头长度 */
    if (ip->ihl < 5) {
        net_buf_free(buf);
        return;
    }

    size_t ip_hdr_len = ip->ihl * 4;
    uint16_t ip_total_len = ntohs(ip->tot_len);
    if (ip_total_len < ip_hdr_len) {
        net_buf_free(buf);
        return;
    }
    size_t udp_len = ip_total_len - ip_hdr_len;
    uint8_t *udp_data = (uint8_t *)ip + ip_hdr_len;

    /* 调用 UDP 输入处理 */
    udp_input(nic, buf, ip, udp_data, udp_len);

    net_buf_free(buf);
}

/* 处理 ICMP 数据包 */
void net_handle_icmp(struct nic_device *nic, struct net_buf *buf) {
    struct eth_hdr *eth = (struct eth_hdr *)buf->data;
    struct ip_hdr *ip = (struct ip_hdr *)(eth + 1);
    size_t ip_hdr_len = ip->ihl * 4;
    struct icmp_hdr *icmp = (struct icmp_hdr *)((uint8_t *)ip + ip_hdr_len);
    
    // print("[NET] ICMP packet: type=");
    // print_hex(icmp->type);
    // println("");
    
    /* 处理 Ping 请求 */
    if (icmp->type == 8) {  /* Echo Request */
        /* 构造 Echo Reply */
        icmp->type = 0;  /* Echo Reply */
        icmp->checksum = 0;
        icmp->checksum = net_checksum(icmp, buf->len - sizeof(struct eth_hdr) - ip_hdr_len);
        
        /* 交换 IP 地址 */
        uint32_t tmp = ip->saddr;
        ip->saddr = ip->daddr;
        ip->daddr = tmp;
        
        /* 交换 MAC 地址 */
        struct mac_addr mac_tmp = eth->h_source;
        eth->h_source = eth->h_dest;
        eth->h_dest = mac_tmp;
        
        /* 重新计算 IP 校验和 */
        ip->check = 0;
        ip->check = net_checksum(ip, sizeof(struct ip_hdr));
        
        /* 发送回复 */
        net_send_packet(nic, buf);
        return;
    }
    
    net_buf_free(buf);
}

/* 处理 ARP 数据包 */
void net_handle_arp(struct nic_device *nic, struct net_buf *buf) {
    if (buf->len < sizeof(struct eth_hdr) + sizeof(struct arp_hdr)) {
        net_buf_free(buf);
        return;
    }
    
    struct eth_hdr *eth = (struct eth_hdr *)buf->data;
    struct arp_hdr *arp = (struct arp_hdr *)(eth + 1);
    
    /* 检查是否为 ARP 请求 */
    uint16_t oper = (arp->oper >> 8) | (arp->oper << 8);
    
    if (oper == 1) {  /* ARP Request */
        /* 检查目标 IP 是否是我们 */
        if (arp->tpa == nic->ip_addr) {
            /* 发送 ARP Reply */
            arp->oper = (2 << 8) | (2 >> 8);  /* Reply */
            arp->tha = arp->sha;
            arp->sha = nic->mac;
            uint32_t tmp = arp->tpa;
            arp->tpa = arp->spa;
            arp->spa = tmp;
            
            /* 交换以太网地址 */
            eth->h_dest = eth->h_source;
            eth->h_source = nic->mac;
            
            net_send_packet(nic, buf);
            return;
        }
    } else if (oper == 2) {  /* ARP Reply */
        /* 添加到 ARP 缓存 */
        arp_add_entry(arp->spa, &arp->sha);
    }
    
    net_buf_free(buf);
}

/* 校验和计算 */
uint16_t net_checksum(const void *data, size_t len) {
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;
    
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    
    if (len > 0) {
        sum += *(const uint8_t *)ptr;
    }
    
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return ~sum;
}

/* IP 地址转换 */
uint32_t net_aton(const char *cp) {
    uint32_t addr = 0;
    int val = 0;
    
    while (*cp) {
        if (*cp >= '0' && *cp <= '9') {
            val = val * 10 + (*cp - '0');
        } else if (*cp == '.') {
            addr = (addr << 8) | val;
            val = 0;
        }
        cp++;
    }
    addr = (addr << 8) | val;
    
    return addr;
}

char *net_ntoa(uint32_t addr) {
    static char buf[16];
    int i = 0;
    
    for (int j = 0; j < 4; j++) {
        uint8_t byte = (addr >> (24 - j * 8)) & 0xFF;
        if (byte >= 100) {
            buf[i++] = '0' + byte / 100;
            byte %= 100;
        }
        if (byte >= 10) {
            buf[i++] = '0' + byte / 10;
            byte %= 10;
        }
        buf[i++] = '0' + byte;
        if (j < 3) {
            buf[i++] = '.';
        }
    }
    buf[i] = '\0';
    
    return buf;
}

void net_mac_to_str(const struct mac_addr *mac, char *str) {
    static const char hex[] = "0123456789ABCDEF";
    
    for (int i = 0; i < ETH_ALEN; i++) {
        str[i * 3] = hex[(mac->addr[i] >> 4) & 0xF];
        str[i * 3 + 1] = hex[mac->addr[i] & 0xF];
        str[i * 3 + 2] = (i < ETH_ALEN - 1) ? ':' : '\0';
    }
}

/* ARP 缓存操作 */
int arp_resolve(struct nic_device *nic, uint32_t ip, struct mac_addr *mac) {
    /* 广播地址直接返回广播 MAC，不需要 ARP */
    if (ip == 0xFFFFFFFF || ip == 0) {
        for (int i = 0; i < ETH_ALEN; i++) {
            mac->addr[i] = 0xFF;
        }
        return 0;
    }
    
    /* 查找缓存 */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            *mac = arp_cache[i].mac;
            return 0;
        }
    }
    
    /* 发送 ARP 请求 */
    struct net_buf *buf = net_buf_alloc();
    if (buf == NULL) {
        return -1;
    }
    
    struct eth_hdr *eth = (struct eth_hdr *)buf->data;
    struct arp_hdr *arp = (struct arp_hdr *)(eth + 1);
    
    /* 广播地址 */
    for (int i = 0; i < ETH_ALEN; i++) {
        eth->h_dest.addr[i] = 0xFF;
    }
    eth->h_source = nic->mac;
    eth->h_proto = htons(ETH_P_ARP);
    
    /* ARP 请求 */
    arp->htype = 0x0100;
    arp->ptype = 0x0008;
    arp->hlen = ETH_ALEN;
    arp->plen = 4;
    arp->oper = 0x0100;  /* Request */
    arp->sha = nic->mac;
    arp->spa = nic->ip_addr;
    for (int i = 0; i < ETH_ALEN; i++) {
        arp->tha.addr[i] = 0;
    }
    arp->tpa = ip;
    
    buf->len = sizeof(struct eth_hdr) + sizeof(struct arp_hdr);
    
    net_send_packet(nic, buf);
    
    /* 简单等待 ARP 回复 (轮询) */
    extern void net_poll(void);
    for (int i = 0; i < 1000; i++) {
        net_poll();
        /* 检查缓存是否更新 */
        for (int j = 0; j < ARP_CACHE_SIZE; j++) {
            if (arp_cache[j].valid && arp_cache[j].ip == ip) {
                *mac = arp_cache[j].mac;
                return 0;
            }
        }
        /* 短暂延迟 */
        for (volatile int k = 0; k < 10000; k++);
    }
    
    return -1;  /* ARP 超时 */
}

void arp_add_entry(uint32_t ip, struct mac_addr *mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            arp_cache[i].mac = *mac;
            arp_cache[i].valid = 1;
            return;
        }
    }
}

void arp_remove_entry(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            arp_cache[i].valid = 0;
            return;
        }
    }
}

/* 环回接口 */
static struct nic_device loopback_nic;

static int loopback_send(void *priv, struct net_buf *buf) {
    (void)priv;
    
    /* 环回: 将数据包放入接收队列 */
    /* 简化实现: 直接处理 */
    loopback_nic.rx_packets++;
    loopback_nic.rx_bytes += buf->len;
    
    /* 处理 IP 数据包 */
    net_handle_ip(&loopback_nic, buf);
    
    return 0;
}

int loopback_init(void) {
    /* 设置环回接口 */
    loopback_nic.name[0] = 'l';
    loopback_nic.name[1] = 'o';
    loopback_nic.name[2] = '\0';
    
    loopback_nic.state = NIC_STATE_UP;
    loopback_nic.ip_addr = 0x7F000001;  /* 127.0.0.1 */
    loopback_nic.netmask = 0xFF000000;  /* 255.0.0.0 */
    
    /* 操作函数 */
    static struct nic_ops loopback_ops = {
        .send = loopback_send,
    };
    loopback_nic.ops = &loopback_ops;
    
    net_register_nic(&loopback_nic);
    
    return 0;
}

/* ========== IP 发送 ========== */

/* 获取第一个非环回网卡的 IP 地址 */
static uint32_t get_primary_nic_ip(void) {
    for (int i = 0; i < nic_count; i++) {
        struct nic_device *nic = nic_table[i];
        if (nic == NULL) continue;
        /* Skip loopback (name starts with "lo") */
        if (nic->name[0] == 'l' && nic->name[1] == 'o') continue;
        if (nic->ip_addr != 0) {
            return nic->ip_addr;
        }
    }
    return 0;
}

/* 获取第一个非环回网卡 */
static struct nic_device *get_primary_nic(void) {
    for (int i = 0; i < nic_count; i++) {
        struct nic_device *nic = nic_table[i];
        if (nic == NULL) continue;
        /* Skip loopback */
        if (nic->name[0] == 'l' && nic->name[1] == 'o') continue;
        if (nic->state == NIC_STATE_RUNNING || nic->state == NIC_STATE_UP) {
            return nic;
        }
    }
    return NULL;
}

int ip_send(uint32_t dst_ip, uint8_t protocol, const void *data, size_t len) {
    struct nic_device *nic = get_primary_nic();
    if (nic == NULL) return -1;

    /* 解析目标 MAC 地址 (ARP) */
    struct mac_addr dst_mac;

    /* 特殊处理广播地址 - 直接使用广播 MAC，不检查子网 */
    if (dst_ip == 0xFFFFFFFF || dst_ip == 0) {
        /* 广播地址不需要 ARP 和网关检查 */
        for (int i = 0; i < ETH_ALEN; i++) {
            dst_mac.addr[i] = 0xFF;
        }
    } else {
        /* 检查是否在同一子网 */
        uint32_t netmask = nic->netmask ? nic->netmask : 0xFFFFFF00;
        uint32_t gateway = nic->gateway;
        uint32_t actual_dst = dst_ip;

        /* 只有当 NIC 已有 IP 时才检查子网 */
        if (nic->ip_addr != 0 && (dst_ip & netmask) != (nic->ip_addr & netmask)) {
            /* 目标在不同子网，发送到网关 */
            if (gateway == 0 || gateway == INADDR_ANY) {
                return -1;  /* 没有网关配置 */
            }
            actual_dst = gateway;
        }

        if (arp_resolve(nic, actual_dst, &dst_mac) < 0) {
            return -1;  /* ARP 解析失败 */
        }
    }

    /* 检查载荷是否超过以太网帧容量 */
    size_t frame_payload = ETH_FRAME_LEN - sizeof(struct eth_hdr) - sizeof(struct ip_hdr);
    if (len > frame_payload) return -1;

    /* 分配缓冲区 */
    struct net_buf *buf = net_buf_alloc();
    if (buf == NULL) return -1;

    /* 构造以太网头 */
    struct eth_hdr *eth = (struct eth_hdr *)buf->data;
    mac_copy(&eth->h_dest, &dst_mac);
    mac_copy(&eth->h_source, &nic->mac);
    eth->h_proto = htons(ETH_P_IP);

    /* 构造 IP 头 */
    struct ip_hdr *ip = (struct ip_hdr *)(buf->data + sizeof(struct eth_hdr));
    ip->version = 4;
    ip->ihl = 5;
    ip->tos = 0;
    ip->tot_len = htons(sizeof(struct ip_hdr) + len);
    ip->id = 0;
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->check = 0;
    ip->saddr = nic->ip_addr ? nic->ip_addr : get_primary_nic_ip();
    ip->daddr = dst_ip;
    ip->check = ip_checksum(ip);

    /* 复制数据 */
    memcpy(buf->data + sizeof(struct eth_hdr) + sizeof(struct ip_hdr), data, len);
    buf->len = sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + len;

    /* 发送 */
    int ret = net_send_packet(nic, buf);
    net_buf_free(buf);

    return ret;
}

/* ========== UDP 实现 ========== */

struct udp_pcb *udp_new(void) {
    static struct udp_pcb udp_pcb_pool[16];
    static int next_pcb = 0;

    if (next_pcb >= 16) return NULL;

    struct udp_pcb *pcb = &udp_pcb_pool[next_pcb++];
    memset(pcb, 0, sizeof(*pcb));
    return pcb;
}

int udp_bind(struct udp_pcb *pcb, uint32_t addr, uint16_t port) {
    pcb->local_ip = addr;
    pcb->local_port = port;
    return 0;
}

void udp_free(struct udp_pcb *pcb) {
    if (pcb) {
        pcb->local_port = 0;
        pcb->local_ip = 0;
    }
}

int udp_sendto(struct udp_pcb *pcb, const void *data, size_t len,
               uint32_t dst_ip, uint16_t dst_port) {
    struct nic_device *nic = get_primary_nic();
    if (nic == NULL) return -1;

    /* 构造 UDP 头 */
    struct udp_hdr udp;
    udp.source = htons(pcb ? pcb->local_port : 0);
    udp.dest = htons(dst_port);
    udp.len = htons(sizeof(struct udp_hdr) + len);
    udp.check = 0;  /* Optional in IPv4 */

    /* 构造 UDP+数据缓冲区 */
    size_t total_len = sizeof(struct udp_hdr) + len;
    uint8_t *buf = kmalloc(total_len);
    if (!buf) return -1;

    memcpy(buf, &udp, sizeof(struct udp_hdr));
    memcpy(buf + sizeof(struct udp_hdr), data, len);

    /* 通过 IP 层发送 */
    int ret = ip_send(dst_ip, IPPROTO_UDP, buf, total_len);
    kfree(buf);

    return ret;
}

/* UDP 接收处理 */
static void udp_input(struct nic_device *nic, struct net_buf *buf,
                      struct ip_hdr *ip, uint8_t *udp_data, size_t udp_len) {
    if (udp_len < sizeof(struct udp_hdr)) return;

    struct udp_hdr *udp = (struct udp_hdr *)udp_data;
    uint16_t src_port = ntohs(udp->source);
    uint16_t dst_port = ntohs(udp->dest);
    uint16_t data_len = ntohs(udp->len) - sizeof(struct udp_hdr);
    uint8_t *payload = udp_data + sizeof(struct udp_hdr);

    /* 检查是否是 DHCP 响应 (BOOTP reply) */
    if (dst_port == 68 && src_port == 67) {
        /* DHCP 输入 - 调用 bridge 函数 */
        extern void net_dhcp_input(struct nic_device *nic, const void *data, size_t len);
        net_dhcp_input(nic, payload, data_len);
        return;
    }

    /* 检查是否是 NTP 响应 (SNTP) */
    if (src_port == 123) {
        extern void sntp_handle_response(const uint8_t *data, size_t len);
        sntp_handle_response(payload, data_len);
        return;
    }

    (void)buf;
    (void)ip;
}

/* 打印网络统计信息 */
void net_stats(void) {
    // println("=== Network Statistics ===");
    // print("  Registered NICs: ");
    // print_dec(nic_count);
    // println("");
    
    uint64_t total_tx = 0, total_rx = 0;
    uint64_t total_tx_err = 0, total_rx_err = 0;
    
    for (int i = 0; i < nic_count; i++) {
        struct nic_device *nic = nic_table[i];
        if (nic == NULL) continue;
        
        // print("  NIC ");
        // print_dec(i);
        // print(": ");
        // println(nic->name);
        
        // print("    TX: ");
        // print_dec(nic->tx_packets);
        // print(" packets, ");
        // print_dec(nic->tx_bytes);
        // println(" bytes");
        
        // print("    RX: ");
        // print_dec(nic->rx_packets);
        // print(" packets, ");
        // print_dec(nic->rx_bytes);
        // println(" bytes");
        
        total_tx += nic->tx_packets;
        total_rx += nic->rx_packets;
        total_tx_err += nic->tx_errors;
        total_rx_err += nic->rx_errors;
    }
    
    // println("  --- Totals ---");
    // print("  TX: ");
    // print_dec(total_tx);
    // print(" packets, Errors: ");
    // print_dec(total_tx_err);
    // println("");
    // print("  RX: ");
    // print_dec(total_rx);
    // print(" packets, Errors: ");
    // print_dec(total_rx_err);
    // println("");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：网络包聚合与批量处理
 * 
 * 特性：
 * - 批量发送 (减少中断)
 * - 包聚合 (合并小包)
 * - NAPI 风格轮询
 * - 零拷贝优化
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 批量发送配置 */
#define NET_TX_BATCH_SIZE       32
#define NET_TX_BATCH_TIMEOUT    100000ULL   /* 100us */

/* 包聚合配置 */
#define NET_COALESCE_MIN_PKTS   2
#define NET_COALESCE_MAX_SIZE   (ETH_MTU - 64)

/* NAPI 配置 */
#define NAPI_POLL_WEIGHT        64
#define NAPI_POLL_BUDGET        16

/* 批量发送队列 */
struct net_tx_batch {
    struct net_buf *bufs[NET_TX_BATCH_SIZE];
    uint32_t count;
    uint64_t deadline;
    struct nic_device *nic;
};

static struct net_tx_batch tx_batch;
static volatile int tx_batch_lock = 0;

/* NAPI 状态 */
struct napi_state {
    int enabled;
    int polling;
    uint32_t poll_count;
    uint64_t last_poll;
};

static struct napi_state napi;

/* 批量发送初始化 */
static void net_tx_batch_init(void) {
    tx_batch.count = 0;
    tx_batch.deadline = 0;
    tx_batch.nic = NULL;
    tx_batch_lock = 0;
    
    napi.enabled = 0;
    napi.polling = 0;
    napi.poll_count = 0;
    napi.last_poll = 0;
}

/* 添加到批量发送队列 */
int net_tx_batch_add(struct nic_device *nic, struct net_buf *buf) {
    if (buf == NULL) return -1;
    
    /* 获取锁 */
    while (__sync_lock_test_and_set(&tx_batch_lock, 1)) {
        asm volatile("pause");
    }
    
    /* 初始化批次 */
    if (tx_batch.count == 0) {
        tx_batch.deadline = get_tick_count() + NET_TX_BATCH_TIMEOUT;
        tx_batch.nic = nic;
    }
    
    /* 添加到批次 */
    tx_batch.bufs[tx_batch.count++] = buf;
    
    /* 检查是否需要立即发送 */
    int need_flush = (tx_batch.count >= NET_TX_BATCH_SIZE) ||
                     (get_tick_count() >= tx_batch.deadline) ||
                     (tx_batch.nic != nic);
    
    __sync_lock_release(&tx_batch_lock);
    
    if (need_flush) {
        net_tx_batch_flush();
    }
    
    return 0;
}

/* 刷新批量发送队列 */
void net_tx_batch_flush(void) {
    while (__sync_lock_test_and_set(&tx_batch_lock, 1)) {
        asm volatile("pause");
    }
    
    if (tx_batch.count == 0 || tx_batch.nic == NULL) {
        __sync_lock_release(&tx_batch_lock);
        return;
    }
    
    struct nic_device *nic = tx_batch.nic;
    uint32_t count = tx_batch.count;
    struct net_buf *bufs[NET_TX_BATCH_SIZE];
    
    /* 复制缓冲区指针 */
    for (uint32_t i = 0; i < count; i++) {
        bufs[i] = tx_batch.bufs[i];
    }
    
    /* 重置批次 */
    tx_batch.count = 0;
    tx_batch.nic = NULL;
    
    __sync_lock_release(&tx_batch_lock);
    
    /* 批量发送 */
    for (uint32_t i = 0; i < count; i++) {
        net_send_packet(nic, bufs[i]);
    }
}

/* NAPI 轮询接收 */
int napi_poll(struct nic_device *nic, int budget) {
    if (nic == NULL) return 0;
    
    int received = 0;
    
    while (received < budget) {
        struct net_buf *buf = net_recv_packet(nic);
        if (buf == NULL) break;
        
        /* 处理数据包 */
        net_handle_ethernet(nic, buf);
        received++;
    }
    
    napi.poll_count += received;
    napi.last_poll = get_tick_count();
    
    return received;
}

/* 启用 NAPI */
void napi_enable(void) {
    napi.enabled = 1;
    napi.polling = 0;
}

/* 禁用 NAPI */
void napi_disable(void) {
    napi.enabled = 0;
    napi.polling = 0;
}

/* 检查 NAPI 状态 */
int napi_is_enabled(void) {
    return napi.enabled;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：零拷贝网络 I/O
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 零拷贝缓冲区描述符 */
struct zc_buf_desc {
    void *data;             /* 数据指针 */
    size_t len;             /* 数据长度 */
    void *priv;             /* 私有数据 */
    void (*release)(void *priv);  /* 释放回调 */
};

/* 零拷贝发送队列 */
#define ZC_TX_QUEUE_SIZE    64

struct zc_tx_queue {
    struct zc_buf_desc descs[ZC_TX_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    volatile int lock;
};

static struct zc_tx_queue zc_txq;

/* 初始化零拷贝队列 */
static void zc_tx_queue_init(void) {
    zc_txq.head = 0;
    zc_txq.tail = 0;
    zc_txq.count = 0;
    zc_txq.lock = 0;
}

/* 零拷贝发送 */
int net_send_zc(struct nic_device *nic, struct zc_buf_desc *desc) {
    if (nic == NULL || desc == NULL || desc->data == NULL) {
        return -1;
    }
    
    /* 获取锁 */
    while (__sync_lock_test_and_set(&zc_txq.lock, 1)) {
        asm volatile("pause");
    }
    
    if (zc_txq.count >= ZC_TX_QUEUE_SIZE) {
        __sync_lock_release(&zc_txq.lock);
        return -1;  /* 队列满 */
    }
    
    zc_txq.descs[zc_txq.tail] = *desc;
    zc_txq.tail = (zc_txq.tail + 1) % ZC_TX_QUEUE_SIZE;
    zc_txq.count++;
    
    __sync_lock_release(&zc_txq.lock);
    
    /* TODO: 未实现真正的零拷贝 - 当前为简化复制实现 */
    /* 注意：此函数名为"零拷贝"但实际执行了 memcpy，待后续完善 */
    struct net_buf buf;
    memcpy(buf.data, desc->data, desc->len > ETH_FRAME_LEN ? ETH_FRAME_LEN : desc->len);
    buf.len = desc->len;
    
    int ret = net_send_packet(nic, &buf);
    
    /* 调用释放回调 */
    if (desc->release) {
        desc->release(desc->priv);
    }
    
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：网络包聚合 (Packet Coalescing)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 聚合包结构 */
struct coalesced_packet {
    struct eth_hdr eth;
    uint16_t inner_count;           /* 内部包数量 */
    uint16_t total_len;             /* 总长度 */
    uint8_t data[];                 /* 可变长度数据 */
};

/* 尝试聚合包 */
int net_coalesce_packets(struct net_buf **bufs, uint32_t count, 
                         struct net_buf *out_buf) {
    if (bufs == NULL || count < NET_COALESCE_MIN_PKTS || out_buf == NULL) {
        return -1;
    }
    
    /* 计算总长度 */
    size_t total_len = 0;
    for (uint32_t i = 0; i < count; i++) {
        total_len += bufs[i]->len;
    }
    
    /* 检查是否可以聚合 */
    if (total_len > NET_COALESCE_MAX_SIZE) {
        return -1;  /* 太大，不能聚合 */
    }
    if (offsetof(struct coalesced_packet, data) + total_len > ETH_FRAME_LEN) {
        return -1;  /* 超出帧容量 */
    }
    
    /* 检查是否同一目标 */
    struct eth_hdr *first_eth = (struct eth_hdr *)bufs[0]->data;
    for (uint32_t i = 1; i < count; i++) {
        struct eth_hdr *eth = (struct eth_hdr *)bufs[i]->data;
        if (!mac_equals(&first_eth->h_dest, &eth->h_dest)) {
            return -1;  /* 目标不同，不能聚合 */
        }
    }
    
    /* 聚合数据包 */
    struct coalesced_packet *cp = (struct coalesced_packet *)out_buf->data;
    mac_copy(&cp->eth.h_dest, &first_eth->h_dest);
    mac_copy(&cp->eth.h_source, &first_eth->h_source);
    cp->eth.h_proto = 0xFFFF;  /* 自定义聚合协议 */
    cp->inner_count = count;
    cp->total_len = total_len;
    
    /* 复制数据 */
    size_t offset = 0;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(cp->data + offset, bufs[i]->data, bufs[i]->len);
        offset += bufs[i]->len;
    }
    
    out_buf->len = offsetof(struct coalesced_packet, data) + total_len;
    
    return 0;
}

/* 解聚合包 */
int net_decoalesce_packet(struct net_buf *buf, 
                          void (*handler)(struct net_buf *)) {
    if (buf == NULL || handler == NULL) {
        return -1;
    }
    
    struct coalesced_packet *cp = (struct coalesced_packet *)buf->data;
    
    /* 检查是否为聚合包 */
    if (cp->eth.h_proto != 0xFFFF) {
        return -1;  /* 不是聚合包 */
    }
    
    /* 解析内部包 */
    size_t offset = 0;
    struct net_buf inner_buf;

    for (uint16_t i = 0; i < cp->inner_count; i++) {
        if (offset + sizeof(struct eth_hdr) > cp->total_len) break;
        struct eth_hdr *inner_eth = (struct eth_hdr *)(cp->data + offset);
        /* Use total_len as boundary; each inner frame is at least eth_hdr sized */
        size_t remaining = cp->total_len - offset;
        size_t inner_len = remaining;  /* Last packet gets all remaining */
        if (i < cp->inner_count - 1) {
            /* For non-last packets, use IP total length to determine size */
            if (remaining > sizeof(struct eth_hdr) + sizeof(struct ip_hdr)) {
                struct ip_hdr *inner_ip = (struct ip_hdr *)(inner_eth + 1);
                size_t ip_total = ntohs(inner_ip->tot_len);
                inner_len = sizeof(struct eth_hdr) + ip_total;
            }
        }
        if (inner_len > remaining) inner_len = remaining;
        if (inner_len > ETH_FRAME_LEN) inner_len = ETH_FRAME_LEN;

        /* 复制到缓冲区 */
        memcpy(inner_buf.data, cp->data + offset, inner_len);
        inner_buf.len = inner_len;
        
        /* 处理内部包 */
        handler(&inner_buf);
        
        offset += inner_len;
    }
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 网络性能监控
 * ═══════════════════════════════════════════════════════════════════════════ */

struct net_perf_stats {
    uint64_t tx_batch_count;     /* 批量发送次数 */
    uint64_t tx_batch_pkts;      /* 批量发送包数 */
    uint64_t coalesce_count;     /* 聚合次数 */
    uint64_t coalesce_pkts;      /* 聚合包数 */
    uint64_t napi_polls;         /* NAPI 轮询次数 */
    uint64_t zc_sends;           /* 零拷贝发送次数 */
};

static struct net_perf_stats net_perf;

void net_perf_init(void) {
    memset(&net_perf, 0, sizeof(net_perf));
    net_tx_batch_init();
    zc_tx_queue_init();
}

void net_perf_report(void) {
    // println("[NET] Performance Statistics:");
    // print("  TX batches: ");
    // print_dec(net_perf.tx_batch_count);
    // print(" (");
    // print_dec(net_perf.tx_batch_pkts);
    // println(" pkts)");
    // print("  Coalesced: ");
    // print_dec(net_perf.coalesce_count);
    // print(" (");
    // print_dec(net_perf.coalesce_pkts);
    // println(" pkts)");
    // print("  NAPI polls: ");
    // print_dec(net_perf.napi_polls);
    // println("");
    // print("  Zero-copy TX: ");
    // print_dec(net_perf.zc_sends);
    // println("");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：网络流量控制 (Traffic Control)
 * 类似 Linux TC 的流量整形和队列管理
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 队列规则类型 */
enum qdisc_type {
    QDISC_FIFO = 0,         /* 先进先出 */
    QDISC_PRIO,             /* 优先级队列 */
    QDISC_TBF,              /* 令牌桶 */
    QDISC_HTB,              /* 分层令牌桶 */
    QDISC_SFQ               /* 随机公平队列 */
};

/* 队列规则 */
struct qdisc {
    enum qdisc_type type;
    uint32_t limit;             /* 队列限制 */
    uint32_t packets;           /* 当前包数 */
    uint64_t bytes;             /* 当前字节数 */
    uint64_t dropped;           /* 丢包计数 */
    uint64_t overlimits;        /* 超限计数 */
    
    /* 令牌桶参数 */
    uint64_t rate;              /* 速率 (bytes/s) */
    uint64_t burst;             /* 突发大小 */
    uint64_t tokens;            /* 当前令牌 */
    uint64_t last_update;
    
    /* 队列 */
    struct net_buf *head;
    struct net_buf *tail;
    
    struct qdisc_ops *ops;
};

/* 队列操作 */
struct qdisc_ops {
    int (*enqueue)(struct qdisc *q, struct net_buf *buf);
    struct net_buf *(*dequeue)(struct qdisc *q);
    void (*reset)(struct qdisc *q);
};

/* 默认队列 */
static struct qdisc default_qdisc;

/* FIFO 入队 */
static int qdisc_fifo_enqueue(struct qdisc *q, struct net_buf *buf) {
    if (q->packets >= q->limit) {
        q->dropped++;
        return -1;
    }
    
    buf->next = NULL;
    if (q->tail == NULL) {
        q->head = q->tail = buf;
    } else {
        q->tail->next = buf;
        q->tail = buf;
    }
    
    q->packets++;
    q->bytes += buf->len;
    return 0;
}

/* FIFO 出队 */
static struct net_buf *qdisc_fifo_dequeue(struct qdisc *q) {
    if (q->head == NULL) return NULL;
    
    struct net_buf *buf = q->head;
    q->head = buf->next;
    if (q->head == NULL) q->tail = NULL;
    
    q->packets--;
    q->bytes -= buf->len;
    buf->next = NULL;
    return buf;
}

/* 令牌桶入队 */
static int qdisc_tbf_enqueue(struct qdisc *q, struct net_buf *buf) {
    /* 更新令牌 */
    uint64_t now = get_tick_count();
    uint64_t elapsed = now - q->last_update;
    q->tokens += (elapsed * q->rate) / 1000000000ULL;
    if (q->tokens > q->burst) q->tokens = q->burst;
    q->last_update = now;
    
    /* 检查令牌 */
    if (buf->len > q->tokens) {
        q->overlimits++;
        return -1;
    }
    
    return qdisc_fifo_enqueue(q, buf);
}

/* 初始化队列规则 */
void qdisc_init(struct qdisc *q, enum qdisc_type type, uint32_t limit) {
    memset(q, 0, sizeof(*q));
    q->type = type;
    q->limit = limit > 0 ? limit : 1000;
    q->last_update = get_tick_count();
}

/* 初始化默认队列 */
void net_tc_init(void) {
    qdisc_init(&default_qdisc, QDISC_TBF, 10000);
    default_qdisc.rate = 1000000000ULL;     /* 1 Gbps */
    default_qdisc.burst = 65536;            /* 64KB burst */
    default_qdisc.tokens = default_qdisc.burst;
}

/* 入队数据包 */
int net_tc_enqueue(struct net_buf *buf) {
    return qdisc_fifo_enqueue(&default_qdisc, buf);
}

/* 出队数据包 */
struct net_buf *net_tc_dequeue(void) {
    return qdisc_fifo_dequeue(&default_qdisc);
}

/* 获取队列统计 */
void net_tc_get_stats(uint64_t *packets, uint64_t *dropped, uint64_t *overlimits) {
    if (packets) *packets = default_qdisc.packets;
    if (dropped) *dropped = default_qdisc.dropped;
    if (overlimits) *overlimits = default_qdisc.overlimits;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：网络 QoS (Quality of Service)
 * 服务质量保证
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 服务类别 */
#define TC_CLASS_BEST_EFFORT    0   /* 尽力而为 */
#define TC_CLASS_ASSURED        1   /* 保证转发 */
#define TC_CLASS_EXPEDITED      2   /* 加速转发 */
#define TC_CLASS_CONTROL        3   /* 控制流量 */

/* QoS 配置 */
struct qos_config {
    uint8_t dscp;               /* DSCP 标记 */
    uint8_t priority;           /* 优先级 */
    uint64_t min_rate;          /* 最小保证速率 */
    uint64_t max_rate;          /* 最大速率 */
    uint64_t burst;             /* 突发大小 */
    uint32_t latency_us;        /* 最大延迟 (微秒) */
    uint32_t jitter_us;         /* 最大抖动 */
};

/* QoS 类 */
struct qos_class {
    struct qos_config config;
    uint64_t bytes_sent;
    uint64_t packets_sent;
    uint64_t bytes_dropped;
};

#define MAX_QOS_CLASSES     8
static struct qos_class qos_classes[MAX_QOS_CLASSES];

/* 初始化 QoS */
void net_qos_init(void) {
    /* 默认配置 */
    qos_classes[TC_CLASS_BEST_EFFORT].config.priority = 0;
    qos_classes[TC_CLASS_BEST_EFFORT].config.max_rate = 1000000000ULL;
    
    qos_classes[TC_CLASS_ASSURED].config.priority = 5;
    qos_classes[TC_CLASS_ASSURED].config.min_rate = 100000000ULL;
    qos_classes[TC_CLASS_ASSURED].config.max_rate = 500000000ULL;
    
    qos_classes[TC_CLASS_EXPEDITED].config.priority = 7;
    qos_classes[TC_CLASS_EXPEDITED].config.min_rate = 200000000ULL;
    qos_classes[TC_CLASS_EXPEDITED].config.latency_us = 1000;
    
    qos_classes[TC_CLASS_CONTROL].config.priority = 10;
    qos_classes[TC_CLASS_CONTROL].config.max_rate = 10000000ULL;
}

/* 分类数据包 */
int net_qos_classify(struct net_buf *buf) {
    if (buf == NULL) return TC_CLASS_BEST_EFFORT;
    
    /* 简化实现：根据端口分类 */
    struct ip_hdr *ip = (struct ip_hdr *)(buf->data + sizeof(struct eth_hdr));
    if (ip->protocol == 6) {  /* TCP */
        struct tcp_hdr *tcp = (struct tcp_hdr *)((uint8_t *)ip + (ip->ihl << 2));
        uint16_t dst_port = (tcp->dest >> 8) | (tcp->dest << 8);  /* ntohs */

        /* SSH, DNS 等控制流量 */
        if (dst_port == 22 || dst_port == 53) {
            return TC_CLASS_CONTROL;
        }
        /* 实时应用 */
        if (dst_port >= 5000 && dst_port <= 5999) {
            return TC_CLASS_EXPEDITED;
        }
    }
    
    return TC_CLASS_BEST_EFFORT;
}

/* 应用 QoS 策略 */
int net_qos_apply(struct net_buf *buf, int class_id) {
    if (class_id < 0 || class_id >= MAX_QOS_CLASSES) return -1;
    
    struct qos_class *qc = &qos_classes[class_id];
    qc->packets_sent++;
    qc->bytes_sent += buf->len;
    
    /* 设置优先级标记 */
    buf->flags = qc->config.priority;
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：网络诊断和调试
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 网络诊断信息 */
struct net_diagnosis {
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    uint64_t collisions;
    uint64_t crc_errors;
    uint64_t frame_errors;
    uint64_t fifo_errors;
    uint64_t carrier_lost;
};

static struct net_diagnosis net_diag;

/* 记录网络错误 */
void net_record_error(int type) {
    switch (type) {
        case 0: net_diag.rx_errors++; break;
        case 1: net_diag.tx_errors++; break;
        case 2: net_diag.rx_dropped++; break;
        case 3: net_diag.tx_dropped++; break;
        case 4: net_diag.collisions++; break;
        case 5: net_diag.crc_errors++; break;
        case 6: net_diag.frame_errors++; break;
        case 7: net_diag.fifo_errors++; break;
        case 8: net_diag.carrier_lost++; break;
    }
}

/* 获取诊断信息 */
void net_get_diagnosis(struct net_diagnosis *diag) {
    if (diag) *diag = net_diag;
}

/* 网络健康检查 */
int net_health_check(void) {
    /* 检查错误率 */
    uint64_t total_errors = net_diag.rx_errors + net_diag.tx_errors +
                            net_diag.crc_errors + net_diag.frame_errors;
    
    if (total_errors > 1000) {
        // println("[NET] WARNING: High error rate detected");
        return -1;
    }
    
    /* 检查丢包率 */
    uint64_t total_dropped = net_diag.rx_dropped + net_diag.tx_dropped;
    if (total_dropped > 10000) {
        // println("[NET] WARNING: High packet drop rate");
        return -1;
    }
    
    return 0;
}

/* 打印网络诊断 */
void net_print_diagnosis(void) {
    // println("=== Network Diagnosis ===");
    // print("RX Errors: ");
    // print_dec(net_diag.rx_errors);
    // print("  TX Errors: ");
    // print_dec(net_diag.tx_errors);
    // println("");
    
    // print("RX Dropped: ");
    // print_dec(net_diag.rx_dropped);
    // print("  TX Dropped: ");
    // print_dec(net_diag.tx_dropped);
    // println("");
    
    // print("Collisions: ");
    // print_dec(net_diag.collisions);
    // print("  CRC Errors: ");
    // print_dec(net_diag.crc_errors);
    // println("");
    
    // print("Frame Errors: ");
    // print_dec(net_diag.frame_errors);
    // print("  FIFO Errors: ");
    // print_dec(net_diag.fifo_errors);
    // println("");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：网络统计和监控
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 详细网络统计 */
struct net_stats_detail {
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_unicast;
    uint64_t tx_unicast;
    uint64_t rx_multicast;
    uint64_t tx_multicast;
    uint64_t rx_broadcast;
    uint64_t tx_broadcast;
    uint64_t rx_tcp;
    uint64_t tx_tcp;
    uint64_t rx_udp;
    uint64_t tx_udp;
    uint64_t rx_icmp;
    uint64_t tx_icmp;
};

static struct net_stats_detail net_stats_data;

/* 更新网络统计 */
void net_update_stats(int direction, int protocol, size_t len, int is_mcast) {
    if (direction == 0) {  /* RX */
        net_stats_data.rx_bytes += len;
        net_stats_data.rx_packets++;
        if (is_mcast == 1) net_stats_data.rx_multicast++;
        else if (is_mcast == 2) net_stats_data.rx_broadcast++;
        else net_stats_data.rx_unicast++;
        
        switch (protocol) {
            case 6: net_stats_data.rx_tcp++; break;
            case 17: net_stats_data.rx_udp++; break;
            case 1: net_stats_data.rx_icmp++; break;
        }
    } else {  /* TX */
        net_stats_data.tx_bytes += len;
        net_stats_data.tx_packets++;
        if (is_mcast == 1) net_stats_data.tx_multicast++;
        else if (is_mcast == 2) net_stats_data.tx_broadcast++;
        else net_stats_data.tx_unicast++;
        
        switch (protocol) {
            case 6: net_stats_data.tx_tcp++; break;
            case 17: net_stats_data.tx_udp++; break;
            case 1: net_stats_data.tx_icmp++; break;
        }
    }
}

/* 获取网络统计 */
void net_get_stats_detail(struct net_stats_detail *stats) {
    if (stats) *stats = net_stats_data;
}

/* 打印网络统计 */
void net_print_stats_detail(void) {
    // println("=== Network Statistics ===");
    
    // print("RX: ");
    // print_dec(net_stats_data.rx_packets);
    // print(" packets (");
    // print_dec(net_stats_data.rx_bytes / 1024 / 1024);
    // println(" MB)");
    
    // print("TX: ");
    // print_dec(net_stats_data.tx_packets);
    // print(" packets (");
    // print_dec(net_stats_data.tx_bytes / 1024 / 1024);
    // println(" MB)");
    
    // print("TCP: RX ");
    // print_dec(net_stats_data.rx_tcp);
    // print(" TX ");
    // print_dec(net_stats_data.tx_tcp);
    // println("");
    
    // print("UDP: RX ");
    // print_dec(net_stats_data.rx_udp);
    // print(" TX ");
    // print_dec(net_stats_data.tx_udp);
    // println("");
    
    // print("ICMP: RX ");
    // print_dec(net_stats_data.rx_icmp);
    // print(" TX ");
    // print_dec(net_stats_data.tx_icmp);
    // println("");
}
