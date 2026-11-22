#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* 字节序转换 */
#define ntohs(x)    __builtin_bswap16(x)
#define htons(x)    __builtin_bswap16(x)
#define ntohl(x)    __builtin_bswap32(x)
#define htonl(x)    __builtin_bswap32(x)

/* 常用 IP 地址 */
#define INADDR_ANY          0x00000000U
#define INADDR_LOOPBACK     0x7F000001U
#define INADDR_BROADCAST    0xFFFFFFFFU
#define INADDR_NONE         0xFFFFFFFFU

/* 常用端口 */
#define PORT_ECHO           7
#define PORT_DISCARD        9
#define PORT_DAYTIME        13
#define PORT_FTP            21
#define PORT_SSH            22
#define PORT_TELNET         23
#define PORT_SMTP           25
#define PORT_DNS            53
#define PORT_DHCP_SERVER    67
#define PORT_DHCP_CLIENT    68
#define PORT_TFTP           69
#define PORT_HTTP           80
#define PORT_POP3           110
#define PORT_NTP            123
#define PORT_NETBIOS        137
#define PORT_IMAP           143
#define PORT_SNMP           161
#define PORT_HTTPS          443
#define PORT_SMB            445

/* 以太网常量 */
#define ETH_ALEN       6       /* MAC 地址长度 */
#define ETH_HLEN       14      /* 以太网头长度 */
#define ETH_MTU        1500    /* 最大传输单元 */
#define ETH_FRAME_LEN  1514    /* 帧最大长度 */
#define ETH_MIN_LEN    60      /* 最小帧长度 */
#define ETH_MAX_LEN    1518    /* 最大帧长度 (不含 VLAN) */

/* 以太网类型 */
#define ETH_P_IP       0x0800
#define ETH_P_ARP      0x0806
#define ETH_P_RARP     0x8035
#define ETH_P_IPV6     0x86DD
#define ETH_P_LOOP     0x0060
#define ETH_P_8021Q    0x8100  /* VLAN */
#define ETH_P_8021AD   0x88A8  /* Q-in-Q */
#define ETH_P_PPP_DISC 0x8863
#define ETH_P_PPP_SES  0x8864

/* IP 协议号 */
#define IPPROTO_IP      0
#define IPPROTO_ICMP    1
#define IPPROTO_IGMP    2
#define IPPROTO_IPIP    4
#define IPPROTO_TCP     6
#define IPPROTO_EGP     8
#define IPPROTO_PUP     12
#define IPPROTO_UDP     17
#define IPPROTO_IDP     22
#define IPPROTO_TP      29
#define IPPROTO_IPV6    41
#define IPPROTO_ROUTING 43
#define IPPROTO_FRAGMENT 44
#define IPPROTO_RSVP    46
#define IPPROTO_GRE     47
#define IPPROTO_ESP     50
#define IPPROTO_AH      51
#define IPPROTO_ICMPV6  58
#define IPPROTO_NONE    59
#define IPPROTO_DSTOPTS 60
#define IPPROTO_MTP     92
#define IPPROTO_ENCAP   98
#define IPPROTO_PIM     103
#define IPPROTO_COMP    108
#define IPPROTO_SCTP    132
#define IPPROTO_UDPLITE 136
#define IPPROTO_RAW     255

/* 网卡状态 */
#define NIC_STATE_DOWN     0
#define NIC_STATE_UP       1
#define NIC_STATE_RUNNING  2
#define NIC_STATE_ERROR    3

/* 网卡标志 */
#define NIC_FLAG_PROMISC   0x01
#define NIC_FLAG_MULTICAST 0x02
#define NIC_FLAG_LOOPBACK  0x04
#define NIC_FLAG_UP        0x08
#define NIC_FLAG_RUNNING   0x10
#define NIC_FLAG_NOARP     0x20

/* MAC 地址 */
struct mac_addr {
    uint8_t addr[ETH_ALEN];
};

/* 以太网头 */
struct eth_hdr {
    struct mac_addr h_dest;
    struct mac_addr h_source;
    uint16_t h_proto;
} __attribute__((packed));

/* 兼容别名 */
typedef struct eth_hdr eth_header_t;
typedef struct ip_hdr ip_header_t;

/* IPv4 头 */
struct ip_hdr {
    uint8_t ihl:4;
    uint8_t version:4;
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
} __attribute__((packed));

/* IP 标志 */
#define IP_CE       0x8000  /* 拥塞标志 */
#define IP_DF       0x4000  /* 不分片 */
#define IP_MF       0x2000  /* 更多分片 */
#define IP_OFFSET   0x1FFF  /* 分片偏移掩码 */

/* 默认 TTL */
#define IP_DEFAULT_TTL   64

/* TCP 头 */
struct tcp_hdr {
    uint16_t source;
    uint16_t dest;
    uint32_t seq;
    uint32_t ack_seq;
    uint8_t res1:4;
    uint8_t doff:4;
    uint8_t fin:1;
    uint8_t syn:1;
    uint8_t rst:1;
    uint8_t psh:1;
    uint8_t ack:1;
    uint8_t urg:1;
    uint8_t res2:2;
    uint16_t window;
    uint16_t check;
    uint16_t urg_ptr;
} __attribute__((packed));

/* TCP 标志位 */
#define TCP_FIN     0x01
#define TCP_SYN     0x02
#define TCP_RST     0x04
#define TCP_PSH     0x08
#define TCP_ACK     0x10
#define TCP_URG     0x20
#define TCP_ECE     0x40
#define TCP_CWR     0x80

/* TCP 状态定义在 tcp.h 中 */

/* 默认窗口大小 */
#define TCP_DEFAULT_WINDOW  65535

/* UDP 头 */
struct udp_hdr {
    uint16_t source;
    uint16_t dest;
    uint16_t len;
    uint16_t check;
} __attribute__((packed));

/* UDP PCB */
struct udp_pcb {
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
};

/* UDP 函数 */
struct udp_pcb *udp_new(void);
int udp_bind(struct udp_pcb *pcb, uint32_t addr, uint16_t port);
void udp_free(struct udp_pcb *pcb);
int udp_sendto(struct udp_pcb *pcb, const void *data, size_t len, 
               uint32_t dst_ip, uint16_t dst_port);

/* ICMP 头 */
struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

/* ICMP 类型 */
#define ICMP_ECHO_REPLY        0
#define ICMP_DEST_UNREACH      3
#define ICMP_SOURCE_QUENCH     4
#define ICMP_REDIRECT          5
#define ICMP_ECHO_REQUEST      8
#define ICMP_TIME_EXCEEDED     11
#define ICMP_PARAM_PROBLEM     12
#define ICMP_TIMESTAMP         13
#define ICMP_TIMESTAMP_REPLY   14
#define ICMP_INFO_REQUEST      15
#define ICMP_INFO_REPLY        16

/* ICMP 不可达代码 */
#define ICMP_NET_UNREACH       0
#define ICMP_HOST_UNREACH      1
#define ICMP_PROT_UNREACH      2
#define ICMP_PORT_UNREACH      3
#define ICMP_FRAG_NEEDED       4
#define ICMP_SR_FAILED         5

/* ARP 头 */
struct arp_hdr {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    struct mac_addr sha;
    uint32_t spa;
    struct mac_addr tha;
    uint32_t tpa;
} __attribute__((packed));

/* ARP 操作码 */
#define ARP_REQUEST    1
#define ARP_REPLY      2
#define RARP_REQUEST   3
#define RARP_REPLY     4

/* 网络缓冲区 */
struct net_buf {
    uint8_t data[ETH_FRAME_LEN];
    size_t len;
    struct net_buf *next;
    uint32_t flags;
    uint64_t timestamp;
};

/* 缓冲区标志 */
#define NET_BUF_FLAG_TX       0x01
#define NET_BUF_FLAG_RX       0x02
#define NET_BUF_FLAG_ERROR    0x04

/* 网卡操作 */
struct nic_ops {
    int (*init)(void *priv);
    void (*cleanup)(void *priv);
    int (*start)(void *priv);
    void (*stop)(void *priv);
    int (*send)(void *priv, struct net_buf *buf);
    struct net_buf *(*recv)(void *priv);
    int (*set_mac)(void *priv, struct mac_addr *mac);
    int (*get_mac)(void *priv, struct mac_addr *mac);
    int (*set_promisc)(void *priv, int enable);
    int (*set_mtu)(void *priv, uint32_t mtu);
    int (*get_stats)(void *priv, void *stats);
};

/* 网卡设备 */
struct nic_device {
    char name[32];
    uint32_t id;
    int state;
    int flags;
    
    struct mac_addr mac;
    uint32_t ip_addr;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t mtu;
    
    void *priv;             /* 驱动私有数据 */
    struct nic_ops *ops;
    
    /* 统计 */
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint64_t tx_errors;
    uint64_t rx_errors;
    uint64_t tx_dropped;
    uint64_t rx_dropped;
    uint64_t collisions;
    uint64_t multicast;
};

/* 网络接口 */
struct net_interface {
    char name[16];
    struct nic_device *nic;
    uint32_t mtu;
    int up;
};

/* ========== MAC 地址工具函数 ========== */

/* 检查 MAC 是否为广播 */
static inline int mac_is_broadcast(const struct mac_addr *mac) {
    return mac->addr[0] == 0xFF && mac->addr[1] == 0xFF &&
           mac->addr[2] == 0xFF && mac->addr[3] == 0xFF &&
           mac->addr[4] == 0xFF && mac->addr[5] == 0xFF;
}

/* 检查 MAC 是否为组播 */
static inline int mac_is_multicast(const struct mac_addr *mac) {
    return (mac->addr[0] & 0x01) != 0;
}

/* 检查 MAC 是否为零 */
static inline int mac_is_zero(const struct mac_addr *mac) {
    return mac->addr[0] == 0 && mac->addr[1] == 0 &&
           mac->addr[2] == 0 && mac->addr[3] == 0 &&
           mac->addr[4] == 0 && mac->addr[5] == 0;
}

/* 比较两个 MAC 地址 */
static inline int mac_equals(const struct mac_addr *a, const struct mac_addr *b) {
    return memcmp(a->addr, b->addr, ETH_ALEN) == 0;
}

/* 复制 MAC 地址 */
static inline void mac_copy(struct mac_addr *dst, const struct mac_addr *src) {
    memcpy(dst->addr, src->addr, ETH_ALEN);
}

/* ========== IP 地址工具函数 ========== */

/* 检查 IP 是否为广播 */
static inline int ip_is_broadcast(uint32_t ip, uint32_t netmask) {
    return (ip & ~netmask) == ~netmask;
}

/* 检查 IP 是否为本地地址 */
static inline int ip_is_local(uint32_t ip) {
    /* 127.0.0.0/8 */
    return (ip & 0xFF000000) == 0x7F000000;
}

/* 检查 IP 是否为私有地址 */
static inline int ip_is_private(uint32_t ip) {
    /* 10.0.0.0/8 */
    if ((ip & 0xFF000000) == 0x0A000000) return 1;
    /* 172.16.0.0/12 */
    if ((ip & 0xFFF00000) == 0xAC100000) return 1;
    /* 192.168.0.0/16 */
    if ((ip & 0xFFFF0000) == 0xC0A80000) return 1;
    return 0;
}

/* 计算网络地址 */
static inline uint32_t ip_network(uint32_t ip, uint32_t netmask) {
    return ip & netmask;
}

/* 计算广播地址 */
static inline uint32_t ip_broadcast(uint32_t ip, uint32_t netmask) {
    return ip | ~netmask;
}

/* 初始化网络子系统 */
int net_init(void);

/* 注册网卡 */
int net_register_nic(struct nic_device *nic);

/* 注销网卡 */
int net_unregister_nic(struct nic_device *nic);

/* 获取网卡 */
struct nic_device *net_get_nic(const char *name);
struct nic_device *net_get_nic_by_index(int index);
struct nic_device *net_get_first_nic(void);  /* First non-loopback NIC */
int net_get_nic_count(void);

/* 发送/接收数据包 */
int net_send_packet(struct nic_device *nic, struct net_buf *buf);
struct net_buf *net_recv_packet(struct nic_device *nic);
void net_poll(void);  /* Poll all NICs (for ARP wait) */

/* 协议处理 */
void net_handle_ethernet(struct nic_device *nic, struct net_buf *buf);
void net_handle_ip(struct nic_device *nic, struct net_buf *buf);
void net_handle_tcp(struct nic_device *nic, struct net_buf *buf);
void net_handle_udp(struct nic_device *nic, struct net_buf *buf);
void net_handle_icmp(struct nic_device *nic, struct net_buf *buf);
void net_handle_arp(struct nic_device *nic, struct net_buf *buf);

/* 缓冲区管理 */
struct net_buf *net_buf_alloc(void);
void net_buf_free(struct net_buf *buf);

/* 校验和计算 */
uint16_t net_checksum(const void *data, size_t len);
uint16_t ip_checksum(const struct ip_hdr *ip);
uint16_t udp_checksum(const struct ip_hdr *ip, const struct udp_hdr *udp, size_t udp_len);
/* tcp_checksum 定义在 tcp.h 中 */

/* IP 发送 */
int ip_send(uint32_t dst_ip, uint8_t protocol, const void *data, size_t len);

/* IP 地址转换 */
uint32_t net_aton(const char *cp);
char *net_ntoa(uint32_t addr);
void net_mac_to_str(const struct mac_addr *mac, char *str);

/* ARP 缓存 */
int arp_resolve(struct nic_device *nic, uint32_t ip, struct mac_addr *mac);
void arp_add_entry(uint32_t ip, struct mac_addr *mac);
void arp_remove_entry(uint32_t ip);
void arp_flush(void);

/* 环回接口 */
int loopback_init(void);

/* 打印网络统计信息 */
void net_stats(void);

/* 网络状态查询 */
int net_is_connected(void);
uint32_t net_get_ip(void);
uint32_t net_get_gateway(void);
uint32_t net_get_dns(void);

#endif
