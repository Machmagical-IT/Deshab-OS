# ZratroOS 网络功能文档

**版本**: v1.5.1  
**最后更新**: 2026-04-21

## 代码统计

| 文件 | 行数 | 功能 |
|------|------|------|
| tcp.c | 2522 | TCP 协议完整实现 |
| net.c | 1601 | 网络协议栈核心 |
| wifi.c | 1403 | RTL8188EU WiFi 驱动 |
| dhcp.c + net_dhcp.c | ~450 | DHCP 客户端 |
| socket.c | ~400 | Socket API |
| e1000.c | ~900 | Intel 千兆网卡驱动 |
| **网络代码总计** | **~7276 行** |

## 概述

ZratroOS 实现了完整的网络协议栈，支持有线以太网 (e1000) 和无线 WiFi (RTL8188EU) 两种网络接口。

## 架构概览

```
┌─────────────────────────────────────────────────────────┐
│                    用户空间程序                          │
│              (shell, desk, 网络工具)                     │
├─────────────────────────────────────────────────────────┤
│                    Socket API                           │
│           (socket, bind, listen, accept, send, recv)    │
├─────────────────────────────────────────────────────────┤
│              传输层 (TCP/UDP)                            │
│    ┌──────────────┐    ┌──────────────┐                 │
│    │  TCP (tcp.c) │    │  UDP (net.c) │                 │
│    │  连接管理     │    │  无状态传输   │                 │
│    │  拥塞控制     │    │  数据报      │                 │
│    └──────────────┘    └──────────────┘                 │
├─────────────────────────────────────────────────────────┤
│              网络层 (IP)                                │
│    ┌──────────────────────────────────────┐             │
│    │         IP (net.c)                   │             │
│    │  - IPv4 数据报处理                    │             │
│    │  - 路由选择                           │             │
│    │  - ICMP (ping)                       │             │
│    │  - ARP 地址解析                       │             │
│    └──────────────────────────────────────┘             │
├─────────────────────────────────────────────────────────┤
│              数据链路层                                  │
│    ┌──────────────┐    ┌──────────────┐                 │
│    │  e1000 (有线) │    │ WiFi (无线)  │                 │
│    │  82540EM     │    │ RTL8188EU    │                 │
│    │  1000Mbps    │    │ 802.11 b/g/n │                 │
│    └──────────────┘    └──────────────┘                 │
├─────────────────────────────────────────────────────────┤
│              物理层                                      │
│    ┌──────────────┐    ┌──────────────┐                 │
│    │  PCI MMIO    │    │  USB Bulk    │                 │
│    │  DMA 描述符环 │    │  固件加载     │                 │
│    └──────────────┘    └──────────────┘                 │
└─────────────────────────────────────────────────────────┘
```

## 网络驱动

### 1. e1000 有线网卡驱动

**文件**: `SYSTEM/lib/Ztknl/e1000.c`, `SYSTEM/lib/Ztknl/include/e1000.h`

**支持设备**:
- Intel 82540EM (QEMU 默认)
- Intel 82545EM
- Intel 82541GI/PI
- Intel 82543GC_F
- Intel 82546EB
- Intel 82571EB/82572EI/82573L/82574L/82583V
- Intel I350/I210/I211

**MMIO 映射**:
```c
// 物理地址 (BAR0)
mmio_phys = 0xFEBC0000

// 虚拟地址 (通过 VMM 映射)
mmio_virt = 0xFFFFFFFF81000000

// 映射大小: 128KB (32 页)
// 映射标志: VMM_FLAGS_MMIO (PRESENT | WRITABLE | CACHE_DISABLE | WRITETHROUGH | NX)
```

**关键函数**:
```c
int e1000_init(void);              // 驱动初始化
int e1000_pci_probe(void);         // PCI 设备探测
int e1000_nic_init(void *priv);    // 网卡初始化
int e1000_nic_send(void *priv, struct net_buf *buf);  // 发送数据包
struct net_buf *e1000_nic_recv(void *priv);           // 接收数据包
```

**描述符环**:
- TX 环: 128 个描述符，每个 16 字节
- RX 环: 128 个描述符，每个 16 字节
- DMA 缓冲区: 通过 PMM 分配

**当前工作模式**: 轮询模式 (无中断)

### 2. RTL8188EU USB WiFi 驱动

**文件**: `SYSTEM/lib/Ztknl/wifi.c`, `SYSTEM/lib/Ztknl/include/wifi.h`

**支持设备**:
| 厂商 ID | 产品 ID | 设备名称 |
|---------|---------|----------|
| 0x0bda  | 0x8179  | RTL8188EUS |
| 0x0bda  | 0x0179  | RTL8188ETV |
| 0x0bda  | 0xf179  | RTL8188FU |
| 0x2357  | 0x010c  | TP-Link TL-WN722N v2 |
| 0x2001  | 0x330F  | DLink DWA-125 |

**架构**:
```
wifi.c (1403 行)
├── USB 设备驱动层
│   ├── rtl8188eu_probe()       - 设备探测
│   ├── rtl8188eu_init()        - 设备初始化
│   ├── rtl8188eu_fw_load()     - 固件加载
│   └── usb endpoint discovery  - 端点发现
├── 802.11 MAC 层
│   ├── rtl8188eu_process_rx()  - RX 帧处理
│   ├── rtl8188eu_connect()     - 连接 AP
│   └── 802.11 frame building  - 帧构建
├── WPA/WPA2 安全层
│   ├── sha1_transform()        - SHA-1
│   ├── hmac_sha1()             - HMAC-SHA1
│   ├── wpa_derive_pmk()        - PBKDF2
│   ├── wpa_derive_ptk()        - PTK 派生
│   └── wpa_process_eapol()     - EAPOL 处理
└── 网络集成层
    ├── wifi_init()             - WiFi 子系统初始化
    ├── wifi_scan()             - 扫描网络
    ├── wifi_connect()          - 连接网络
    └── wifi_autoconnect()      - 自动连接
```

**帧格式**:

TX 描述符 (32 字节):
```
┌─────────────────────────────────────────┐
│ TX 描述符 (32 字节)                     │
│ ┌───────────┬───────────┬─────────────┐ │
│ │ 长度      │ 偏移      │ 控制位       │ │
│ └───────────┴───────────┴─────────────┘ │
│ 802.11 帧 (管理/控制/数据)               │
└─────────────────────────────────────────┘
```

RX 描述符 (24 字节):
```
┌─────────────────────────────────────────┐
│ RX 描述符 (24 字节)                     │
│ ┌───────────┬───────────┬─────────────┐ │
│ │ 长度      │ 状态      │ RSSI        │ │
│ └───────────┴───────────┴─────────────┘ │
│ 802.11 帧                                │
│ ↓ 解封装 ↓                               │
│ 以太网帧 (供网络栈使用)                   │
└─────────────────────────────────────────┘
```

**WPA2 四次握手流程**:
```
STA (客户端)                    AP (接入点)
    │                              │
    │  ─── Association Request ──► │
    │  ◄── Association Response ── │
    │                              │
    │  ◄── EAPOL Message 1 (ANonce)│
    │  计算 PTK                    │
    │  ─── EAPOL Message 2 ──────► │
    │                              │ 计算 PTK
    │  ◄── EAPOL Message 3 ────── │
    │  ─── EAPOL Message 4 ──────► │
    │                              │
    │  ←── 加密数据传输 ──►        │
```

**密钥派生**:
```
PMK = PBKDF2-SHA1(passphrase, ssid, 4096 iterations, 256 bits)
PTK = PRF-384(PMK, "Pairwise key expansion", 
              Min(AA,SPA) || Max(AA,SPA) || Min(ANonce,SNonce) || Max(ANonce,SNonce))
```

## 网络协议栈

### TCP/IP 协议栈

**文件**: `SYSTEM/lib/Ztknl/net.c`, `SYSTEM/lib/Ztknl/tcp.c`, `SYSTEM/lib/Ztknl/socket.c`

**协议实现**:
| 协议 | 文件 | 功能 |
|------|------|------|
| ARP  | net.c | 地址解析，缓存管理 |
| ICMP | net.c | ping 请求/响应 |
| IP   | net.c | IPv4 数据报，分片重组 |
| UDP  | net.c | 无状态数据报 |
| TCP  | tcp.c | 面向连接，可靠传输 |

**网络缓冲区**:
```c
struct net_buf {
    uint8_t data[MAX_PKT_SIZE];  // 数据包数据
    size_t len;                   // 数据长度
    struct net_buf *next;         // 链表指针
};
```

### DHCP 客户端

**文件**: `SYSTEM/lib/Ztknl/dhcp.c`, `SYSTEM/lib/Ztknl/net_dhcp.c`

**DHCP 流程**:
```
1. DHCP DISCOVER (广播)
   ↓
2. DHCP OFFER (来自服务器)
   ↓
3. DHCP REQUEST (广播)
   ↓
4. DHCP ACK (来自服务器)
   ↓
5. 配置网络接口 (IP, 子网掩码, 网关, DNS)
```

**配置解析**:
```c
// 配置文件格式 (SYSTEM/system/internet/e1w.cfg)
name = "SSID_NAME"
password = "wifi_password"

// 解析函数
int net_config_parse(const char *path);
```

### 自动连接机制

**文件**: `SYSTEM/lib/Ztknl/main.c`

**启动流程**:
```
main()
├── net_init()              // 网络子系统初始化
├── socket_init()           // Socket API 初始化
├── e1000_init()            // 有线网卡驱动
├── wifi_init()             // WiFi 子系统
├── rtl8188eu_driver_init() // USB WiFi 驱动
├── tcp_init()              // TCP 协议初始化
├── net_autoconnect()       // 有线网络自动连接 (DHCP)
└── wifi_autoconnect()      // WiFi 自动连接 (如果有线失败)
```

**连接优先级**:
1. 有线网络 (e1000) - 优先
2. 无线网络 (WiFi) - 备用

## 网络配置

### 配置文件位置

| 文件 | 路径 | 用途 |
|------|------|------|
| WiFi 配置 | `/system/internet/e1w.cfg` | SSID 和密码 |
| 系统配置 | `/system/zratro.conf` | 启动程序配置 |
| 固件 | `/system/firmware/rtl8188eufw.bin` | RTL8188EU 固件 |

### 当前网络配置

**WiFi 配置** (`e1w.cfg`):
```
name = "8-1-101"
password = "linyao335577"
```

**有线网络**:
- DHCP 自动获取 IP
- 支持静态 IP 配置 (待实现)

## 已知问题

### 1. DHCP 发送失败

**现象**: `[DHCP] Failed to send DISCOVER`

**可能原因**:
- e1000 发送描述符环配置问题
- TX 轮询时机问题
- 网络缓冲区未正确分配

**调试建议**:
```c
// 在 e1000_nic_send() 中添加调试输出
printk("e1000: Sending packet, len=%d", buf->len);
printk("e1000: TX descriptor %d, phys=0x%llx", idx, desc->buffer_addr);

// 检查 TDT (Transmit Descriptor Tail) 寄存器
uint32_t tdt = e1000_reg_read(adapter, E1000_TDT);
printk("e1000: TDT = %d", tdt);
```

### 2. WiFi 配置未加载

**现象**: `wifi: No network config found`

**可能原因**:
- VFS 中文件路径不正确
- Limine 模块未注册
- FAT32 文件系统未挂载

**调试建议**:
```c
// 在 wifi_autoconnect() 中添加调试输出
printk("wifi: Trying to open: /system/internet/e1w.cfg");
struct vfs_file *f = vfs_open("/system/internet/e1w.cfg", "r");
if (f) {
    printk("wifi: Config file opened successfully");
} else {
    printk("wifi: Config file NOT found");
}
```

### 3. USB WiFi 设备模拟

QEMU 不直接支持 RTL8188EU 设备模拟，需要:
- 使用 `usb-host` 传递真实 USB 设备
- 或实现自定义 QEMU 设备模型

## API 参考

### WiFi API

```c
// 初始化 WiFi 子系统
int wifi_init(void);

// 注册 WiFi 设备
int wifi_register_device(struct wifi_device *wdev);

// 扫描网络
int wifi_scan(struct wifi_device *wdev);

// 连接到网络
int wifi_connect(struct wifi_device *wdev, struct wifi_config *config);

// 断开连接
int wifi_disconnect(struct wifi_device *wdev);

// 发送数据
int wifi_send(struct wifi_device *wdev, uint8_t *data, int len);

// 接收数据
int wifi_recv(struct wifi_device *wdev, uint8_t *buf, int max_len);

// 自动连接
int wifi_autoconnect(void);
```

### 网络 API

```c
// 初始化网络子系统
int net_init(void);

// 发送 IP 数据包
int ip_send(struct nic_device *nic, uint8_t *data, size_t len, 
            uint32_t dest_ip, uint8_t protocol);

// UDP 发送
int udp_sendto(struct udp_pcb *pcb, void *data, size_t len,
               uint32_t dest_ip, uint16_t dest_port);

// TCP 连接
int tcp_connect(struct tcp_pcb *pcb, uint32_t dest_ip, uint16_t dest_port);

// Socket API
int socket(int domain, int type, int protocol);
int bind(int sockfd, struct sockaddr *addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int send(int sockfd, void *buf, size_t len, int flags);
int recv(int sockfd, void *buf, size_t len, int flags);
```

## 未来计划

### 短期
- [ ] 修复 DHCP 发送功能
- [x] 完善 WiFi 配置文件加载 (已实现 Limine 模块提取)
- [ ] 实现 ping 命令
- [ ] 添加简单的网络工具 (ifconfig, route)

### 中期
- [ ] 实现完整的 TCP 服务器/客户端
- [ ] 支持 HTTP 协议
- [ ] 添加 DNS 解析
- [ ] 实现网络壳命令 (通过 syscalls/IPC)

### 长期
- [ ] IPv6 支持
- [ ] WPA3 安全
- [ ] 网络文件系统 (NFS)
- [ ] TLS/SSL 加密

## 参考资料

- Intel 82540EM 数据手册
- RTL8188EU 编程指南
- IEEE 802.11-2020 标准
- WPA2 规范 (802.11i)
- Linux e1000 驱动源码 (参考)
- Linux rtl8188eu 驱动源码 (参考)

---

*文档版本: 1.2*  
*最后更新: 2026-04-21*
*内核版本: ZratroOS v1.5.1*
