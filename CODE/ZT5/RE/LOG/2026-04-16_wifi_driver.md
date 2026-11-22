# ZratroOS 开发日志

## 2026-04-16 WiFi 驱动开发与网络测试

### 工作内容

#### 1. RTL8188EU USB WiFi 驱动实现

**目标**: 为 Ztknl 内核添加完整的 RTL8188EU USB WiFi 适配器支持

**实现文件**:
- `SYSTEM/lib/Ztknl/wifi.h` (314 行) - WiFi 子系统 API 定义
- `SYSTEM/lib/Ztknl/wifi.c` (1403 行) - RTL8188EU 驱动完整实现

**关键特性**:
- 支持 11 种 USB WiFi 设备 ID (RTL8188EUS, RTL8188ETV, TP-Link TL-WN722N v2 等)
- 完整 WPA/WPA2-PSK 安全协议栈:
  - SHA-1 哈希函数实现
  - HMAC-SHA1 消息认证
  - PBKDF2-SHA1 用于 PMK 派生
  - PRF-384 用于 PTK 派生
  - EAPOL 四次握手处理
- 802.11 MAC 层完整实现:
  - 管理帧 (信标、探测、认证、关联)
  - 控制帧
  - 数据帧 (带以太网桥接)
- USB 子系统集成:
  - 动态端点发现
  - 固件加载 (从文件系统读取 rtl8188eufw.bin)
  - TX/RX 缓冲区管理
- 网络栈集成:
  - 802.11 帧到以太网帧的转换
  - DHCP 客户端支持
  - 自动回退机制 (有线优先，WiFi 备用)

**网络配置**:
- 配置文件: `SYSTEM/system/internet/e1w.cfg`
- SSID: "8-1-101"
- 密码: "linyao335577"

#### 2. e1000 网卡驱动 MMIO 映射修复

**问题发现**:
- 在 QEMU UEFI 测试中发现 e1000 驱动初始化时发生页错误
- 错误地址: `0xFFFF8000FEBC00D8` (CR2)
- RIP: `0xFFFFFFFF800144D4`

**根因分析**:
- `e1000_pci_probe()` 正确设置了 `mmio_phys = 0xFEBC0000` 并通过 VMM 映射到 `mmio_virt = 0xFFFFFFFF81000000`
- 但 `e1000_nic_init()` 中错误地执行了:
  ```c
  adapter->mmio_virt = (uint64_t)phys_to_virt(adapter->mmio_phys);
  ```
- 这将地址覆盖为 `0xFFFF8000FEBC0000` (直接加上 higher half 偏移，但此地址未映射页表)
- 导致后续 MMIO 寄存器访问时发生页错误

**修复方法**:
- 删除 `e1000_nic_init()` 中的 `phys_to_virt()` 调用
- 保留 `e1000_pci_probe()` 中正确的 VMM 映射
- 添加 `VMM_FLAGS_MMIO` 标志 (包含 PRESENT | WRITABLE | CACHE_DISABLE | WRITETHROUGH | NX)

**修复代码**:
```c
// e1000_pci_probe() 中的映射 (保留)
e1000_adapter.mmio_virt = 0xFFFFFFFF81000000ULL;
struct address_space *kspace = vmm_get_kernel_space();
if (kspace != NULL) {
    vmm_map_range(kspace, e1000_adapter.mmio_virt,
                  e1000_adapter.mmio_phys, 32,  // 128KB = 32 pages
                  VMM_FLAGS_MMIO);
}

// e1000_nic_init() 中删除了错误的行:
// adapter->mmio_virt = (uint64_t)phys_to_virt(adapter->mmio_phys);  // 已删除
```

#### 3. Limine 引导配置优化

**问题**:
- 原 limine.conf 引用了不存在的模块 (`aknl.bin`, `rustknl`)
- 可能导致 limine 加载失败

**修复**:
```
timeout: 3
display_mode: internal

/Zratro OS
    protocol: limine
    kernel_path: boot():/system/knl.bin
    kernel_cmdline: debug=on loglevel=7
    module_path: boot():/system/zratro.conf
    module_path: boot():/prgm/sys/init
    module_path: boot():/prgm/sys/shell
```

### 测试记录

#### QEMU UEFI 引导测试

**测试环境**:
- QEMU 10.2.2
- OVMF UEFI 固件 (`/usr/share/OVMF/x64/OVMF.4m.fd`)
- 512MB RAM, 2 CPU cores
- e1000 网卡

**测试结果**:

✅ **成功项目**:
- UEFI 引导成功 (通过 Limine)
- 内核正常启动，串口输出正常
- VMM 初始化成功 (HHDM 映射 2048MB)
- TTY 控制台初始化成功
- e1000 网卡驱动正常初始化:
  - BAR0 phys = febc0000, virt = ffffffff81000000
  - MAC 地址 = 52:54:00:12:34:56
  - 链路状态: 1000 Mbps, 全双工
- 网络自动连接 (DHCP 尝试)
- WiFi 子系统初始化成功
- init 和 shell 进程正常启动
- 调度器正常运行

❌ **失败项目**:
- DHCP 获取 IP 失败: `[DHCP] Failed to send DISCOVER`
- WiFi 自动连接未激活: `wifi: No network config found`
- 文件系统检测失败 (RAMFS 模式，无物理磁盘)

#### 构建验证

**内核编译**:
```bash
cd SYSTEM/lib/Ztknl && make clean && make && make iso
```
- 编译成功，无错误
- 内核大小: 931KB
- ISO 生成成功: 4.8MB

**FAT32 镜像构建**:
```bash
cd ISO && bash build_iso.sh
```
- img 大小: 34MB
- ISO 大小: 1.9MB
- 包含所有必要文件:
  - knl.bin ✅
  - limine.conf ✅
  - BOOTX64.EFI ✅
  - rtl8188eufw.bin ✅
  - e1w.cfg ✅

### 遗留问题

1. **DHCP 发送失败**
   - e1000 驱动已初始化但 `udp_sendto()` 返回失败
   - 可能需要检查:
     - e1000 发送描述符环配置
     - TX 中断/轮询机制
     - 网络缓冲区分配

2. **WiFi 配置未加载**
   - 配置文件路径问题 (VFS 中找不到 `/system/internet/e1w.cfg`)
   - 可能需要检查:
     - Limine 模块注册
     - VFS 文件查找逻辑
     - 文件系统挂载顺序

3. **UEFI vs BIOS 串口差异**
   - BIOS 模式有串口输出
   - UEFI 模式最初无串口输出 (可能是 limine 配置问题)
   - 简化 limine.conf 后 UEFI 串口输出正常

### 代码统计

| 文件 | 行数 | 说明 |
|------|------|------|
| wifi.h | 314 | WiFi 子系统 API |
| wifi.c | 1403 | RTL8188EU 驱动实现 |
| e1000.c (修改) | ~20 行 | MMIO 映射修复 |
| main.c (修改) | ~15 行 | WiFi 初始化添加 |
| net_dhcp.c (修改) | ~5 行 | 全局变量导出 |
| Makefile (修改) | ~10 行 | ISO 构建规则 |

**总计新增代码**: ~1767 行

### 下一步计划

1. 修复 DHCP 发送功能
2. 完善 WiFi 配置文件加载
3. 在 QEMU 中测试 WiFi 连接 (需要 USB WiFi 设备模拟)
4. 验证 WPA2 四次握手
5. 测试数据传输 (ping/HTTP)
