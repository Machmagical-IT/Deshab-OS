/*
 * Zratro OS - DHCP Client Implementation
 */

#include "dhcp.h"
#include "net.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"

/* UDP 函数声明 (来自 net.h) */
struct udp_pcb *udp_new(void);
int udp_bind(struct udp_pcb *pcb, uint32_t addr, uint16_t port);
void udp_free(struct udp_pcb *pcb);

/* 前向声明 */
static void print_ip(uint32_t ip);

/* DHCP 魔数 */
#define DHCP_MAGIC  0x63825363

/* 超时设置 */
#define DHCP_DISCOVER_TIMEOUT    4000    /* 4 秒 */
#define DHCP_REQUEST_TIMEOUT     4000    /* 4 秒 */
#define DHCP_MAX_RETRIES         3

/* ========== DHCP 消息构建 ========== */

static uint32_t generate_xid(void) {
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    return (uint32_t)(tsc ^ (tsc >> 32));
}

int dhcp_build_discover(struct dhcp_client *client, struct dhcp_message *msg) {
    if (msg == NULL) return -1;
    
    memset(msg, 0, sizeof(*msg));
    
    msg->op = 1;        /* BOOTREQUEST */
    msg->htype = 1;     /* Ethernet */
    msg->hlen = 6;
    msg->xid = client->xid;
    msg->secs = 0;
    msg->flags = 0x8000;  /* Broadcast */
    
    /* 客户端 MAC 地址 */
    memcpy(msg->chaddr, client->nic->mac.addr, 6);
    
    /* DHCP 选项 */
    uint8_t *opt = msg->options;
    
    /* Magic cookie */
    *opt++ = 0x63; *opt++ = 0x82; *opt++ = 0x53; *opt++ = 0x63;
    
    /* DHCP Message Type: DISCOVER */
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_DISCOVER;
    
    /* Parameter Request List */
    *opt++ = DHCP_OPT_PARAM_LIST;
    *opt++ = 4;
    *opt++ = DHCP_OPT_NETMASK;      /* Subnet Mask */
    *opt++ = DHCP_OPT_ROUTER;       /* Router */
    *opt++ = DHCP_OPT_DNS;          /* DNS Server */
    *opt++ = DHCP_OPT_LEASE_TIME;   /* Lease Time */
    
    /* Client Identifier (可选) */
    *opt++ = DHCP_OPT_CLIENT_ID;
    *opt++ = 7;
    *opt++ = 1;  /* Hardware type: Ethernet */
    memcpy(opt, client->nic->mac.addr, 6);
    opt += 6;
    
    /* End */
    *opt++ = DHCP_OPT_END;
    
    return 0;
}

int dhcp_build_request(struct dhcp_client *client, struct dhcp_message *msg) {
    if (msg == NULL) return -1;
    
    memset(msg, 0, sizeof(*msg));
    
    msg->op = 1;
    msg->htype = 1;
    msg->hlen = 6;
    msg->xid = client->xid;
    msg->secs = 0;
    msg->flags = 0x8000;  /* Broadcast */
    
    memcpy(msg->chaddr, client->nic->mac.addr, 6);
    
    uint8_t *opt = msg->options;
    
    /* Magic cookie */
    *opt++ = 0x63; *opt++ = 0x82; *opt++ = 0x53; *opt++ = 0x63;
    
    /* DHCP Message Type: REQUEST */
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_REQUEST;
    
    /* Server Identifier */
    *opt++ = DHCP_OPT_SERVER_ID;
    *opt++ = 4;
    *opt++ = (client->config.server_id >> 24) & 0xFF;
    *opt++ = (client->config.server_id >> 16) & 0xFF;
    *opt++ = (client->config.server_id >> 8) & 0xFF;
    *opt++ = client->config.server_id & 0xFF;
    
    /* Requested IP Address */
    *opt++ = DHCP_OPT_REQUEST_IP;
    *opt++ = 4;
    *opt++ = (client->config.ip_address >> 24) & 0xFF;
    *opt++ = (client->config.ip_address >> 16) & 0xFF;
    *opt++ = (client->config.ip_address >> 8) & 0xFF;
    *opt++ = client->config.ip_address & 0xFF;
    
    /* Client Identifier */
    *opt++ = DHCP_OPT_CLIENT_ID;
    *opt++ = 7;
    *opt++ = 1;
    memcpy(opt, client->nic->mac.addr, 6);
    opt += 6;
    
    /* End */
    *opt++ = DHCP_OPT_END;
    
    return 0;
}

int dhcp_build_release(struct dhcp_client *client, struct dhcp_message *msg) {
    if (msg == NULL) return -1;
    
    memset(msg, 0, sizeof(*msg));
    
    msg->op = 1;
    msg->htype = 1;
    msg->hlen = 6;
    msg->xid = generate_xid();
    
    memcpy(msg->chaddr, client->nic->mac.addr, 6);
    msg->ciaddr = client->config.ip_address;
    
    uint8_t *opt = msg->options;
    
    /* Magic cookie */
    *opt++ = 0x63; *opt++ = 0x82; *opt++ = 0x53; *opt++ = 0x63;
    
    /* DHCP Message Type: RELEASE */
    *opt++ = DHCP_OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_RELEASE;
    
    /* Server Identifier */
    *opt++ = DHCP_OPT_SERVER_ID;
    *opt++ = 4;
    *opt++ = (client->config.server_id >> 24) & 0xFF;
    *opt++ = (client->config.server_id >> 16) & 0xFF;
    *opt++ = (client->config.server_id >> 8) & 0xFF;
    *opt++ = client->config.server_id & 0xFF;
    
    /* End */
    *opt++ = DHCP_OPT_END;
    
    return 0;
}

/* ========== DHCP 消息解析 ========== */

int dhcp_parse_message(struct dhcp_client *client, struct dhcp_message *msg) {
    if (msg == NULL) return -1;
    
    /* 检查是否是回复 */
    if (msg->op != 2) return -1;
    
    /* 检查事务 ID */
    if (msg->xid != client->xid) return -1;
    
    /* 解析选项 */
    uint8_t *opt = msg->options;
    
    /* 检查 Magic cookie */
    if (opt[0] != 0x63 || opt[1] != 0x82 || opt[2] != 0x53 || opt[3] != 0x63) {
        return -1;
    }
    opt += 4;
    
    uint8_t msg_type = 0;
    uint8_t *opt_end = msg->options + sizeof(msg->options);

    while (opt < opt_end && *opt != DHCP_OPT_END) {
        uint8_t code = *opt++;

        if (code == DHCP_OPT_PAD) continue;

        if (opt >= opt_end) break;
        uint8_t len = *opt++;

        if (opt + len > opt_end) break;
        
        switch (code) {
            case DHCP_OPT_MSG_TYPE:
                msg_type = opt[0];
                break;
                
            case DHCP_OPT_NETMASK:
                client->config.subnet_mask = 
                    (opt[0] << 24) | (opt[1] << 16) | (opt[2] << 8) | opt[3];
                break;
                
            case DHCP_OPT_ROUTER:
                client->config.router = 
                    (opt[0] << 24) | (opt[1] << 16) | (opt[2] << 8) | opt[3];
                break;
                
            case DHCP_OPT_DNS:
                for (int i = 0; i < len / 4 && i < 3; i++) {
                    client->config.dns_servers[i] =
                        (opt[i*4] << 24) | (opt[i*4+1] << 16) |
                        (opt[i*4+2] << 8) | opt[i*4+3];
                }
                break;
                
            case DHCP_OPT_LEASE_TIME:
                client->config.lease_time = 
                    (opt[0] << 24) | (opt[1] << 16) | (opt[2] << 8) | opt[3];
                break;
                
            case DHCP_OPT_RENEWAL_TIME:
                client->config.renewal_time =
                    (opt[0] << 24) | (opt[1] << 16) | (opt[2] << 8) | opt[3];
                break;
                
            case DHCP_OPT_REBIND_TIME:
                client->config.rebind_time =
                    (opt[0] << 24) | (opt[1] << 16) | (opt[2] << 8) | opt[3];
                break;
                
            case DHCP_OPT_SERVER_ID:
                client->config.server_id =
                    (opt[0] << 24) | (opt[1] << 16) | (opt[2] << 8) | opt[3];
                break;
                
            case DHCP_OPT_HOSTNAME:
                if (len < sizeof(client->config.hostname)) {
                    memcpy(client->config.hostname, opt, len);
                    client->config.hostname[len] = '\0';
                }
                break;
                
            case DHCP_OPT_DOMAIN:
                if (len < sizeof(client->config.domain)) {
                    memcpy(client->config.domain, opt, len);
                    client->config.domain[len] = '\0';
                }
                break;
        }
        
        opt += len;
    }
    
    /* 设置提供的 IP 地址 */
    client->config.ip_address = msg->yiaddr;
    
    return msg_type;
}

/* ========== DHCP 客户端 API ========== */

int dhcp_client_init(struct dhcp_client *client, struct nic_device *nic) {
    if (client == NULL || nic == NULL) return -1;
    
    memset(client, 0, sizeof(*client));
    
    client->nic = nic;
    client->config.state = DHCP_STATE_INIT;
    
    /* 创建 UDP PCB */
    client->pcb = udp_new();
    if (client->pcb == NULL) {
        return -1;
    }
    
    /* 绑定到 DHCP 客户端端口 */
    if (udp_bind(client->pcb, 0, DHCP_CLIENT_PORT) != 0) {
        udp_free(client->pcb);
        return -1;
    }
    
    return 0;
}

int dhcp_start(struct dhcp_client *client) {
    if (client == NULL) return -1;
    
    print("[DHCP] Starting DHCP on interface: ");
    print(client->nic ? client->nic->name : "unknown");
    println("");
    
    /* 生成事务 ID */
    client->xid = generate_xid();
    client->retries = DHCP_MAX_RETRIES;
    client->config.state = DHCP_STATE_SELECTING;
    
    /* 构建 DHCP Discover */
    struct dhcp_message msg;
    dhcp_build_discover(client, &msg);
    
    print("[DHCP] Sending DISCOVER, xid=0x");
    print_hex(client->xid);
    println("");
    
    /* 发送到广播地址 */
    struct net_buf *buf = net_buf_alloc();
    if (buf == NULL) {
        print("[DHCP] Failed to allocate net_buf");
        println("");
        return -1;
    }
    
    memcpy(buf->data, &msg, sizeof(msg));
    buf->len = sizeof(msg);
    
    /* UDP 发送到 255.255.255.255:67 */
    int ret = udp_sendto(client->pcb, buf->data, buf->len, 0xFFFFFFFF, DHCP_SERVER_PORT);
    
    /* 释放缓冲区 */
    net_buf_free(buf);
    
    if (ret != 0) {
        print("[DHCP] udp_sendto failed, ret=");
        print_dec(ret);
        println("");
        print("[DHCP] Failed to send DISCOVER");
        println("");
        return -1;
    }
    
    /* 设置超时 */
    client->timeout = get_tick_count() + DHCP_DISCOVER_TIMEOUT;
    
    print("[DHCP] Sent DISCOVER, xid=0x");
    print_hex(client->xid);
    println("");
    
    return 0;
}

int dhcp_release(struct dhcp_client *client) {
    if (client == NULL) return -1;
    
    if (client->config.state != DHCP_STATE_BOUND) {
        return -1;
    }
    
    /* 构建 DHCP Release */
    struct dhcp_message msg;
    dhcp_build_release(client, &msg);
    
    /* 发送 */
    struct net_buf *buf = net_buf_alloc();
    if (buf) {
        memcpy(buf->data, &msg, sizeof(msg));
        buf->len = sizeof(msg);
        udp_sendto(client->pcb, buf->data, buf->len, client->config.server_id, DHCP_SERVER_PORT);
        net_buf_free(buf);
    }
    
    client->config.state = DHCP_STATE_INIT;
    
    println("[DHCP] Released IP address");
    
    return 0;
}

void dhcp_input(struct dhcp_client *client, struct net_buf *buf) {
    if (client == NULL || buf == NULL) return;
    
    struct dhcp_message *msg = (struct dhcp_message *)buf->data;
    
    /* 解析消息 */
    int msg_type = dhcp_parse_message(client, msg);
    
    print("[DHCP] Received message type ");
    print_dec(msg_type);
    println("");
    
    switch (client->config.state) {
        case DHCP_STATE_SELECTING:
            if (msg_type == DHCP_OFFER) {
                /* 收到 OFFER，发送 REQUEST */
                client->config.state = DHCP_STATE_REQUESTING;
                client->retries = DHCP_MAX_RETRIES;
                
                struct dhcp_message req;
                dhcp_build_request(client, &req);
                
                struct net_buf *req_buf = net_buf_alloc();
                if (req_buf) {
                    memcpy(req_buf->data, &req, sizeof(req));
                    req_buf->len = sizeof(req);
                    udp_sendto(client->pcb, req_buf->data, req_buf->len, 0xFFFFFFFF, DHCP_SERVER_PORT);
                }
                
                client->timeout = get_tick_count() + DHCP_REQUEST_TIMEOUT;
                
                print("[DHCP] Sent REQUEST for ");
                print_hex(client->config.ip_address);
                println("");
            }
            break;
            
        case DHCP_STATE_REQUESTING:
            if (msg_type == DHCP_ACK) {
                /* 成功获取 IP 地址 */
                client->config.state = DHCP_STATE_BOUND;
                
                /* 计算续租时间 */
                if (client->config.renewal_time == 0) {
                    client->config.renewal_time = client->config.lease_time / 2;
                }
                if (client->config.rebind_time == 0) {
                    client->config.rebind_time = client->config.lease_time * 7 / 8;
                }
                
                /* 设置定时器 */
                uint64_t now = get_tick_count();
                client->config.renewal_expires = now + client->config.renewal_time * 1000;
                client->config.rebind_expires = now + client->config.rebind_time * 1000;
                client->config.lease_expires = now + client->config.lease_time * 1000;
                
                /* 配置网卡 */
                client->nic->ip_addr = client->config.ip_address;
                client->nic->netmask = client->config.subnet_mask;
                client->nic->gateway = client->config.router;
                
                println("[DHCP] Bound to IP address:");
                print("  IP:      ");
                print_ip(client->config.ip_address);
                println("");
                print("  Netmask: ");
                print_ip(client->config.subnet_mask);
                println("");
                print("  Gateway: ");
                print_ip(client->config.router);
                println("");
                print("  DNS:     ");
                print_ip(client->config.dns_servers[0]);
                println("");
                
                /* 回调 */
                if (client->on_bound) {
                    client->on_bound(client);
                }
            } else if (msg_type == DHCP_NAK) {
                /* 被拒绝，重新开始 */
                client->config.state = DHCP_STATE_INIT;
                dhcp_start(client);
            }
            break;
            
        case DHCP_STATE_RENEWING:
        case DHCP_STATE_REBINDING:
            if (msg_type == DHCP_ACK) {
                client->config.state = DHCP_STATE_BOUND;
                
                /* 更新定时器 */
                uint64_t now = get_tick_count();
                client->config.renewal_expires = now + client->config.renewal_time * 1000;
                client->config.rebind_expires = now + client->config.rebind_time * 1000;
                client->config.lease_expires = now + client->config.lease_time * 1000;
                
                println("[DHCP] Lease renewed");
            }
            break;
            
        default:
            break;
    }
}

void dhcp_tick(struct dhcp_client *client) {
    if (client == NULL) return;
    
    uint64_t now = get_tick_count();
    
    switch (client->config.state) {
        case DHCP_STATE_SELECTING:
        case DHCP_STATE_REQUESTING:
            if (now >= client->timeout) {
                client->retries--;
                
                if (client->retries > 0) {
                    /* 重试 */
                    if (client->config.state == DHCP_STATE_SELECTING) {
                        struct dhcp_message msg;
                        dhcp_build_discover(client, &msg);
                        
                        struct net_buf *buf = net_buf_alloc();
                        if (buf) {
                            memcpy(buf->data, &msg, sizeof(msg));
                            buf->len = sizeof(msg);
                            udp_sendto(client->pcb, buf->data, buf->len, 0xFFFFFFFF, DHCP_SERVER_PORT);
                        }
                    } else {
                        struct dhcp_message msg;
                        dhcp_build_request(client, &msg);
                        
                        struct net_buf *buf = net_buf_alloc();
                        if (buf) {
                            memcpy(buf->data, &msg, sizeof(msg));
                            buf->len = sizeof(msg);
                            udp_sendto(client->pcb, buf->data, buf->len, 0xFFFFFFFF, DHCP_SERVER_PORT);
                        }
                    }
                    
                    /* 指数退避 */
                    client->timeout = now + DHCP_DISCOVER_TIMEOUT * (1 << (DHCP_MAX_RETRIES - client->retries));
                } else {
                    /* 超时 */
                    client->config.state = DHCP_STATE_INIT;
                    
                    println("[DHCP] Timeout");
                    
                    if (client->on_timeout) {
                        client->on_timeout(client);
                    }
                }
            }
            break;
            
        case DHCP_STATE_BOUND:
            /* 检查续租时间 */
            if (now >= client->config.renewal_expires) {
                client->config.state = DHCP_STATE_RENEWING;
                
                /* 发送续租请求 */
                client->xid = generate_xid();
                client->retries = DHCP_MAX_RETRIES;
                
                struct dhcp_message msg;
                dhcp_build_request(client, &msg);
                
                struct net_buf *buf = net_buf_alloc();
                if (buf) {
                    memcpy(buf->data, &msg, sizeof(msg));
                    buf->len = sizeof(msg);
                    udp_sendto(client->pcb, buf->data, buf->len, client->config.server_id, DHCP_SERVER_PORT);
                }
                
                client->timeout = now + DHCP_REQUEST_TIMEOUT;
                
                println("[DHCP] Sending renewal request");
            }
            break;
            
        case DHCP_STATE_RENEWING:
            if (now >= client->config.rebind_expires) {
                client->config.state = DHCP_STATE_REBINDING;
                
                /* 广播续租请求 */
                client->xid = generate_xid();
                
                struct dhcp_message msg;
                dhcp_build_request(client, &msg);
                
                struct net_buf *buf = net_buf_alloc();
                if (buf) {
                    memcpy(buf->data, &msg, sizeof(msg));
                    buf->len = sizeof(msg);
                    udp_sendto(client->pcb, buf->data, buf->len, 0xFFFFFFFF, DHCP_SERVER_PORT);
                }
                
                println("[DHCP] Sending rebind request");
            }
            break;
            
        case DHCP_STATE_REBINDING:
            if (now >= client->config.lease_expires) {
                /* 租约过期 */
                client->config.state = DHCP_STATE_INIT;
                
                println("[DHCP] Lease expired");
                
                /* 重新获取 */
                dhcp_start(client);
            }
            break;
            
        default:
            break;
    }
}

/* ========== 辅助函数 ========== */

static void print_ip(uint32_t ip) {
    print_dec((ip >> 24) & 0xFF);
    print(".");
    print_dec((ip >> 16) & 0xFF);
    print(".");
    print_dec((ip >> 8) & 0xFF);
    print(".");
    print_dec(ip & 0xFF);
}
