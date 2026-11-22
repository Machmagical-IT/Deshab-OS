/* net_lease.h — 网络租约契约（netman 生产者 + 消费者共享头）
 *
 * P4a 万物互联：从 UTSM/include/utsm/dsk.h 抽出 net_lease_info 结构与
 * NET_LEASE_MAGIC/NET_LEASE_F_* 常量。netman.drv 自持有该结构，经服务总线
 * srv.net.lease 暴露只读指针；用户态工具（ping/curl/browser）与 DSK 均按此
 * 布局读取，无需再依赖 DSK 的 reserved[7] 指针传递。
 *
 * 布局即为 ABI：字段顺序/宽度不可变（与 dsk.h 定义逐字节一致）。
 */
#ifndef DKM_NET_LEASE_H
#define DKM_NET_LEASE_H

#include <stdint.h>

#define NET_LEASE_MAGIC   0x4E45544Cu   /* "NETL" */
#define NET_LEASE_F_VALID  (1u << 0)    /* ip/gateway/dns 有效 */
#define NET_LEASE_F_STATIC (1u << 1)    /* 静态配置（非 DHCP） */

typedef struct net_lease_info {
    uint32_t magic;      /* NET_LEASE_MAGIC */
    uint32_t flags;      /* NET_LEASE_F_* */
    uint32_t ip;         /* guest IP（网络序） */
    uint32_t gateway;    /* 网关（网络序） */
    uint32_t dns;        /* DNS（网络序） */
    uint32_t server;     /* DHCP server（网络序，调试） */
    uint32_t reserved[2];
} net_lease_info;

#endif /* DKM_NET_LEASE_H */
