/*
 * Zratro OS - 蓝牙驱动
 *
 * 功能: HCI 接口框架, L2CAP 协议基础, 蓝牙设备发现
 */

#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== 蓝牙常量 ========== */
#define BT_MAX_DEVICES          8
#define BT_MAX_CONNECTIONS      4
#define BT_ADDR_LEN             6      /* 蓝牙地址长度 */
#define BT_MAX_NAME_LEN         248    /* 最大设备名长度 */
#define BT_MAX_HCI_PKT_SIZE     1024   /* 最大 HCI 包大小 */

/* 蓝牙地址 */
typedef struct {
    uint8_t b[BT_ADDR_LEN];
} bt_addr_t;

/* HCI 包类型 */
#define BT_HCI_CMD_PKT         0x01
#define BT_HCI_ACL_PKT         0x02
#define BT_HCI_SCO_PKT         0x03
#define BT_HCI_EVT_PKT         0x04

/* HCI 命令 OGF (操作码组字段) */
#define BT_OGF_LINK_CTRL       0x01
#define BT_OGF_LINK_POLICY     0x02
#define BT_OGF_HOST_CTRL       0x03
#define BT_OGF_INFO_PARAM      0x04
#define BT_OGF_STATUS_PARAM    0x05
#define BT_OGF_TESTING         0x06
#define BT_OGF_LE              0x08

/* HCI 命令 OCF (操作码命令字段) */
/* 链路控制 */
#define BT_OCF_INQUIRY                 0x0001
#define BT_OCF_INQUIRY_CANCEL          0x0002
#define BT_OCF_CREATE_CONN             0x0005
#define BT_OCF_DISCONNECT              0x0006
#define BT_OCF_REMOTE_NAME_REQ         0x0019
#define BT_OCF_REMOTE_NAME_CANCEL      0x001A

/* 主机控制 */
#define BT_OCF_RESET                   0x0003
#define BT_OCF_SET_EVENT_MASK          0x0001
#define BT_OCF_SET_SCAN_ENABLE         0x001A
#define BT_OCF_READ_BD_ADDR            0x0009

/* 信息参数 */
#define BT_OCF_READ_LOCAL_VERSION      0x0001
#define BT_OCF_READ_LOCAL_FEATURES     0x0003
#define BT_OCF_READ_BUFFER_SIZE        0x0005

/* HCI 事件码 */
#define BT_EVT_INQUIRY_COMPLETE        0x01
#define BT_EVT_INQUIRY_RESULT          0x02
#define BT_EVT_CONN_COMPLETE           0x03
#define BT_EVT_CONN_REQUEST            0x04
#define BT_EVT_DISCONN_COMPLETE        0x05
#define BT_EVT_REMOTE_NAME_COMPLETE    0x07
#define BT_EVT_CMD_COMPLETE            0x0E
#define BT_EVT_CMD_STATUS              0x0F
#define BT_EVT_LE_META                 0x3E

/* 扫描类型 */
#define BT_SCAN_NONE                   0x00
#define BT_SCAN_INQUIRY                0x01
#define BT_SCAN_PAGE                   0x02

/* L2CAP 常量 */
#define BT_L2CAP_CID_ATT               0x0004
#define BT_L2CAP_CID_LE_SIGNALING      0x0005
#define BT_L2CAP_CID_SMP               0x0006

/* ========== HCI 命令结构 ========== */
struct bt_hci_cmd {
    uint16_t opcode;          /* OGF(6) | OCF(10) */
    uint8_t plen;             /* 参数长度 */
    uint8_t params[8];        /* 参数数据 */
} __attribute__((packed));

/* HCI 事件结构 */
struct bt_hci_evt {
    uint8_t code;             /* 事件码 */
    uint8_t plen;             /* 参数长度 */
    uint8_t params[8];        /* 参数数据 */
} __attribute__((packed));

/* HCI ACL 数据包 */
struct bt_hci_acl {
    uint16_t handle;          /* 连接句柄 (12位) + PB(2) + BC(2) */
    uint16_t dlen;            /* 数据长度 */
    uint8_t data[8];          /* L2CAP 数据 */
} __attribute__((packed));

/* L2CAP 帧 */
struct bt_l2cap_hdr {
    uint16_t len;             /* PDU 长度 */
    uint16_t cid;             /* 通道 ID */
} __attribute__((packed));

/* ========== 蓝牙设备信息 ========== */
struct bt_device_info {
    bt_addr_t addr;           /* 蓝牙地址 */
    uint8_t psrm;             /* 页扫描重复模式 */
    uint8_t psm;              /* 页扫描模式 */
    uint16_t co;              /* 时钟偏移 */
    uint32_t cod;             /* 设备类别 */
    char name[BT_MAX_NAME_LEN]; /* 设备名 */
    int name_known;           /* 是否已获取名称 */
};

/* ========== 蓝牙连接 ========== */
struct bt_connection {
    int active;               /* 是否活跃 */
    uint16_t handle;          /* 连接句柄 */
    bt_addr_t addr;           /* 远端地址 */
    uint8_t type;             /* 连接类型 (ACL/SCO) */
    uint8_t role;             /* 角色 (主/从) */
};

/* ========== HCI 传输接口 ========== */
struct bt_hci_transport {
    const char *name;
    int (*open)(void);
    int (*close)(void);
    int (*send_cmd)(const uint8_t *data, size_t len);
    int (*send_acl)(const uint8_t *data, size_t len);
    int (*recv)(uint8_t *buf, size_t len);
};

/* ========== 蓝牙设备结构 ========== */
struct bt_device {
    int initialized;                    /* 是否已初始化 */
    bt_addr_t local_addr;               /* 本地蓝牙地址 */
    char local_name[BT_MAX_NAME_LEN];   /* 本地设备名 */

    /* HCI 传输 */
    struct bt_hci_transport *transport;

    /* 连接 */
    struct bt_connection connections[BT_MAX_CONNECTIONS];
    int conn_count;

    /* 设备发现结果 */
    struct bt_device_info discovered[16];
    int discovered_count;
    int inquiring;                       /* 是否正在扫描 */

    /* HCI 状态 */
    uint8_t hci_version;
    uint16_t hci_revision;
    uint8_t lmp_version;
    uint16_t manufacturer;
    uint16_t acl_mtu;
    uint8_t sco_mtu;
    uint16_t acl_max_pkt;
    uint16_t sco_max_pkt;

    /* 锁 */
    spinlock_t lock;
};

/* ========== API 函数声明 ========== */

/* 初始化蓝牙子系统 */
int bluetooth_init(void);

/* HCI 命令 */
int bt_hci_send_cmd(uint16_t opcode, const void *params, uint8_t plen);
int bt_hci_send_cmd_sync(uint16_t opcode, const void *params, uint8_t plen,
                          struct bt_hci_evt *response);

/* 设备发现 */
int bt_inquiry(int duration);          /* duration: 1.28s * duration */
int bt_inquiry_cancel(void);
int bt_remote_name_request(bt_addr_t *addr);

/* 连接管理 */
int bt_create_connection(bt_addr_t *addr);
int bt_disconnect(uint16_t handle);

/* L2CAP */
int bt_l2cap_send(uint16_t cid, const void *data, size_t len);

/* 本地信息 */
int bt_read_local_info(void);
const char *bt_addr_to_str(bt_addr_t *addr);

/* 获取设备实例 */
struct bt_device *bt_get_device(void);

#endif /* BLUETOOTH_H */
