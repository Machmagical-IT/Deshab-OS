/*
 * Zratro OS - 蓝牙驱动
 *
 * 功能: HCI 接口框架, L2CAP 协议基础, 蓝牙设备发现
 */

#include "bluetooth.h"
#include "console.h"
#include "string.h"
#include "heap.h"

/* ========== 全局设备实例 ========== */

static struct bt_device bt_dev;

/* ========== 辅助函数 ========== */

const char *bt_addr_to_str(bt_addr_t *addr) {
    static char str[18];
    if (addr == NULL) return "00:00:00:00:00:00";
    /* 蓝牙地址以低字节在前存储 */
    snprintf(str, sizeof(str), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr->b[5], addr->b[4], addr->b[3],
             addr->b[2], addr->b[1], addr->b[0]);
    return str;
}

static int bt_addr_is_zero(bt_addr_t *addr) {
    return addr->b[0] == 0 && addr->b[1] == 0 && addr->b[2] == 0 &&
           addr->b[3] == 0 && addr->b[4] == 0 && addr->b[5] == 0;
}

/* ========== HCI 命令发送 ========== */

int bt_hci_send_cmd(uint16_t opcode, const void *params, uint8_t plen) {
    if (bt_dev.transport == NULL || bt_dev.transport->send_cmd == NULL) return -1;

    /* 构造 HCI 命令包 */
    uint8_t pkt[4 + 256];
    pkt[0] = BT_HCI_CMD_PKT;
    pkt[1] = opcode & 0xFF;
    pkt[2] = (opcode >> 8) & 0xFF;
    pkt[3] = plen;
    if (params && plen > 0) {
        memcpy(&pkt[4], params, plen);
    }

    return bt_dev.transport->send_cmd(pkt, 4 + plen);
}

int bt_hci_send_cmd_sync(uint16_t opcode, const void *params, uint8_t plen,
                          struct bt_hci_evt *response) {
    /* 发送命令 */
    int ret = bt_hci_send_cmd(opcode, params, plen);
    if (ret < 0) return ret;

    /* 等待 Command Complete 事件 */
    if (response && bt_dev.transport && bt_dev.transport->recv) {
        for (int retry = 0; retry < 100; retry++) {
            uint8_t buf[BT_MAX_HCI_PKT_SIZE];
            int len = bt_dev.transport->recv(buf, sizeof(buf));
            if (len > 0 && buf[0] == BT_HCI_EVT_PKT) {
                struct bt_hci_evt *evt = (struct bt_hci_evt *)&buf[1];
                if (evt->code == BT_EVT_CMD_COMPLETE) {
                    memcpy(response, evt, sizeof(struct bt_hci_evt) + evt->plen);
                    return 0;
                }
            }
            for (volatile int i = 0; i < 1000; i++);
        }
    }
    return -1;
}

/* ========== 本地信息读取 ========== */

int bt_read_local_info(void) {
    /* 读取本地版本信息 */
    {
        uint8_t params[0];
        struct bt_hci_evt resp;
        if (bt_hci_send_cmd_sync(
                (BT_OGF_INFO_PARAM << 10) | BT_OCF_READ_LOCAL_VERSION,
                params, 0, &resp) == 0) {
            if (resp.plen >= 9) {
                bt_dev.hci_version = resp.params[0];
                bt_dev.hci_revision = resp.params[1] | (resp.params[2] << 8);
                bt_dev.lmp_version = resp.params[3];
                bt_dev.manufacturer = resp.params[4] | (resp.params[5] << 8);
                printk("bt: HCI v%u 修订=%u LMP v%u 厂商=%u",
                       bt_dev.hci_version, bt_dev.hci_revision,
                       bt_dev.lmp_version, bt_dev.manufacturer);
            }
        }
    }

    /* 读取缓冲区大小 */
    {
        struct bt_hci_evt resp;
        if (bt_hci_send_cmd_sync(
                (BT_OGF_INFO_PARAM << 10) | BT_OCF_READ_BUFFER_SIZE,
                NULL, 0, &resp) == 0) {
            if (resp.plen >= 8) {
                bt_dev.acl_mtu = resp.params[0] | (resp.params[1] << 8);
                bt_dev.sco_mtu = resp.params[2];
                bt_dev.acl_max_pkt = resp.params[3] | (resp.params[4] << 8);
                bt_dev.sco_max_pkt = resp.params[5] | (resp.params[6] << 8);
                printk("bt: ACL MTU=%u SCO MTU=%u", bt_dev.acl_mtu, bt_dev.sco_mtu);
            }
        }
    }

    /* 读取本地地址 */
    {
        struct bt_hci_evt resp;
        if (bt_hci_send_cmd_sync(
                (BT_OGF_INFO_PARAM << 10) | BT_OCF_READ_BD_ADDR,
                NULL, 0, &resp) == 0) {
            if (resp.plen >= 6) {
                memcpy(&bt_dev.local_addr, resp.params, 6);
                printk("bt: 本地地址 %s", bt_addr_to_str(&bt_dev.local_addr));
            }
        }
    }

    return 0;
}

/* ========== 设备发现 ========== */

int bt_inquiry(int duration) {
    /* INQUIRY 命令参数:
     * LAP[3]: 0x9E8B33 (通用/无限发现)
     * Inquiry_Length: duration (1.28s 单位)
     * Num_Responses: 0 (无限)
     */
    uint8_t params[5];
    params[0] = 0x33;   /* LAP[0] */
    params[1] = 0x8B;   /* LAP[1] */
    params[2] = 0x9E;   /* LAP[2] */
    params[3] = (uint8_t)MIN(duration, 0x30);  /* 最大 61.44s */
    params[4] = 0;      /* 无限响应 */

    int ret = bt_hci_send_cmd(
        (BT_OGF_LINK_CTRL << 10) | BT_OCF_INQUIRY, params, 5);
    if (ret == 0) {
        bt_dev.inquiring = 1;
        bt_dev.discovered_count = 0;
        printk("bt: 开始设备扫描 (时长=%.2fs)", duration * 1.28);
    }
    return ret;
}

int bt_inquiry_cancel(void) {
    int ret = bt_hci_send_cmd(
        (BT_OGF_LINK_CTRL << 10) | BT_OCF_INQUIRY_CANCEL, NULL, 0);
    if (ret == 0) {
        bt_dev.inquiring = 0;
        printk("bt: 设备扫描已取消 (发现 %d 个设备)", bt_dev.discovered_count);
    }
    return ret;
}

int bt_remote_name_request(bt_addr_t *addr) {
    if (addr == NULL) return Z_ERR_NULL;

    uint8_t params[10];
    memcpy(params, addr->b, 6);
    params[6] = 0x00;  /* 页扫描重复模式 */
    params[7] = 0x00;  /* 页扫描模式 */
    params[8] = 0x00;  /* 时钟偏移低 */
    params[9] = 0x00;  /* 时钟偏移高 */

    return bt_hci_send_cmd(
        (BT_OGF_LINK_CTRL << 10) | BT_OCF_REMOTE_NAME_REQ, params, 10);
}

/* ========== 连接管理 ========== */

int bt_create_connection(bt_addr_t *addr) {
    if (addr == NULL) return Z_ERR_NULL;

    uint8_t params[13];
    memcpy(params, addr->b, 6);
    params[6] = 0x00;  /* 数据包类型: DM1/DH1 */
    params[7] = 0x18;
    params[8] = 0x00;  /* 页扫描重复模式 */
    params[9] = 0x00;  /* 页扫描模式 */
    params[10] = 0x00; /* 时钟偏移 */
    params[11] = 0x00;
    params[12] = 0x00; /* 角色: 主设备 */

    return bt_hci_send_cmd(
        (BT_OGF_LINK_CTRL << 10) | BT_OCF_CREATE_CONN, params, 13);
}

int bt_disconnect(uint16_t handle) {
    uint8_t params[3];
    params[0] = handle & 0xFF;
    params[1] = (handle >> 8) & 0xFF;
    params[2] = 0x13;  /* 原因: 远端用户终止连接 */

    return bt_hci_send_cmd(
        (BT_OGF_LINK_CTRL << 10) | BT_OCF_DISCONNECT, params, 3);
}

/* ========== L2CAP ========== */

int bt_l2cap_send(uint16_t cid, const void *data, size_t len) {
    if (data == NULL || len == 0) return Z_ERR_NULL;
    if (bt_dev.transport == NULL || bt_dev.transport->send_acl == NULL) return Z_ERR_NOTSUP;
    if (bt_dev.conn_count == 0) return Z_ERR_NOENT;

    /* 构造 ACL 包 + L2CAP 帧 */
    size_t total = 1 + 4 + 4 + len;  /* HCI头 + ACL头 + L2CAP头 + 数据 */
    uint8_t *pkt = (uint8_t *)kmalloc(total);
    if (pkt == NULL) return Z_ERR_NOMEM;

    /* HCI ACL 头 */
    pkt[0] = BT_HCI_ACL_PKT;
    uint16_t handle = bt_dev.connections[0].handle;
    pkt[1] = handle & 0xFF;
    pkt[2] = ((handle >> 8) & 0x0F) | 0x20;  /* PB=01 (首包) */
    uint16_t acl_len = 4 + len;  /* L2CAP头 + 数据 */
    pkt[3] = acl_len & 0xFF;
    pkt[4] = (acl_len >> 8) & 0xFF;

    /* L2CAP 头 */
    pkt[5] = len & 0xFF;
    pkt[6] = (len >> 8) & 0xFF;
    pkt[7] = cid & 0xFF;
    pkt[8] = (cid >> 8) & 0xFF;

    /* 数据 */
    memcpy(&pkt[9], data, len);

    int ret = bt_dev.transport->send_acl(pkt, 9 + len);
    kfree(pkt);
    return ret;
}

/* ========== 初始化 ========== */

int bluetooth_init(void) {
    printk("bt: 初始化蓝牙子系统...");

    memset(&bt_dev, 0, sizeof(bt_dev));
    spinlock_init(&bt_dev.lock);

    /* HCI 复位 */
    bt_hci_send_cmd((BT_OGF_HOST_CTRL << 10) | BT_OCF_RESET, NULL, 0);
    for (volatile int i = 0; i < 100000; i++);

    /* 读取本地信息 */
    bt_read_local_info();

    bt_dev.initialized = 1;
    printk("bt: 蓝牙子系统初始化完成");
    return 0;
}

struct bt_device *bt_get_device(void) {
    return &bt_dev;
}
