/*
 * Zratro OS - DHCP Client Implementation
 * 
 * 动态主机配置协议客户端
 */

#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>
#include "net.h"

/* DHCP 端口 */
#define DHCP_SERVER_PORT    67
#define DHCP_CLIENT_PORT    68

/* DHCP 消息类型 */
#define DHCP_DISCOVER       1
#define DHCP_OFFER          2
#define DHCP_REQUEST        3
#define DHCP_DECLINE        4
#define DHCP_ACK            5
#define DHCP_NAK            6
#define DHCP_RELEASE        7
#define DHCP_INFORM         8

/* DHCP 选项 */
#define DHCP_OPT_PAD            0
#define DHCP_OPT_NETMASK        1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS            6
#define DHCP_OPT_HOSTNAME       12
#define DHCP_OPT_DOMAIN         15
#define DHCP_OPT_REQUEST_IP     50
#define DHCP_OPT_LEASE_TIME     51
#define DHCP_OPT_MSG_TYPE       53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_LIST     55
#define DHCP_OPT_RENEWAL_TIME   58
#define DHCP_OPT_REBIND_TIME    59
#define DHCP_OPT_CLIENT_ID      61
#define DHCP_OPT_END            255

/* DHCP 消息 */
struct dhcp_message {
    uint8_t  op;            /* 操作: 1=请求, 2=回复 */
    uint8_t  htype;         /* 硬件类型: 1=以太网 */
    uint8_t  hlen;          /* 硬件地址长度: 6 */
    uint8_t  hops;          /* 跳数 */
    uint32_t xid;           /* 事务 ID */
    uint16_t secs;          /* 秒数 */
    uint16_t flags;         /* 标志 */
    uint32_t ciaddr;        /* 客户端 IP */
    uint32_t yiaddr;        /* 你的 IP */
    uint32_t siaddr;        /* 服务器 IP */
    uint32_t giaddr;        /* 网关 IP */
    uint8_t  chaddr[16];    /* 客户端硬件地址 */
    uint8_t  sname[64];     /* 服务器名称 */
    uint8_t  file[128];     /* 启动文件名 */
    uint8_t  options[312];  /* 选项 */
} __attribute__((packed));

/* DHCP 客户端状态 */
typedef enum {
    DHCP_STATE_INIT = 0,
    DHCP_STATE_SELECTING,
    DHCP_STATE_REQUESTING,
    DHCP_STATE_BOUND,
    DHCP_STATE_RENEWING,
    DHCP_STATE_REBINDING
} dhcp_state_t;

/* DHCP 配置 */
struct dhcp_config {
    uint32_t ip_address;
    uint32_t subnet_mask;
    uint32_t router;
    uint32_t dns_servers[3];
    uint32_t ntp_server;
    
    uint32_t lease_time;
    uint32_t renewal_time;
    uint32_t rebind_time;
    
    uint32_t server_id;
    
    char hostname[64];
    char domain[64];
    
    dhcp_state_t state;
    uint32_t xid;
    
    uint64_t lease_expires;
    uint64_t renewal_expires;
    uint64_t rebind_expires;
};

/* DHCP 客户端 */
struct dhcp_client {
    struct nic_device *nic;
    struct dhcp_config config;
    
    struct udp_pcb *pcb;
    
    uint32_t xid;
    int retries;
    uint64_t timeout;
    
    void (*on_bound)(struct dhcp_client *client);
    void (*on_timeout)(struct dhcp_client *client);
    void *user_data;
};

/* ========== DHCP 客户端 API ========== */

/* 初始化 DHCP 客户端 */
int dhcp_client_init(struct dhcp_client *client, struct nic_device *nic);

/* 启动 DHCP 配置 */
int dhcp_start(struct dhcp_client *client);

/* 手动释放地址 */
int dhcp_release(struct dhcp_client *client);

/* 更新 DHCP 客户端 */
void dhcp_tick(struct dhcp_client *client);

/* 处理 DHCP 消息 */
void dhcp_input(struct dhcp_client *client, struct net_buf *buf);

/* ========== DHCP 消息构建 ========== */

/* 构建 DHCP Discover */
int dhcp_build_discover(struct dhcp_client *client, struct dhcp_message *msg);

/* 构建 DHCP Request */
int dhcp_build_request(struct dhcp_client *client, struct dhcp_message *msg);

/* 构建 DHCP Release */
int dhcp_build_release(struct dhcp_client *client, struct dhcp_message *msg);

/* 解析 DHCP 消息 */
int dhcp_parse_message(struct dhcp_client *client, struct dhcp_message *msg);

/* ========== 辅助函数 ========== */

/* 获取 IP 地址 */
static inline uint32_t dhcp_get_ip(struct dhcp_client *client) {
    return client->config.ip_address;
}

/* 获取子网掩码 */
static inline uint32_t dhcp_get_netmask(struct dhcp_client *client) {
    return client->config.subnet_mask;
}

/* 获取网关 */
static inline uint32_t dhcp_get_router(struct dhcp_client *client) {
    return client->config.router;
}

/* 获取 DNS 服务器 */
static inline uint32_t dhcp_get_dns(struct dhcp_client *client, int index) {
    if (index >= 0 && index < 3) {
        return client->config.dns_servers[index];
    }
    return 0;
}

/* 检查是否已绑定 */
static inline int dhcp_is_bound(struct dhcp_client *client) {
    return client->config.state == DHCP_STATE_BOUND;
}

#endif /* DHCP_H */
