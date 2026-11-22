# ZratroOS 开发日志

## 2026-04-17 DHCP 修复、WiFi 配置加载修复、Rust 监控内核开发

### 工作内容

#### 1. 修复 DHCP 发送失败问题

**问题**: 在 QEMU 测试中，e1000 网卡驱动正常初始化（MAC=52:54:00:12:34:56, 链路 1000Mbps 全双工），但 DHCP DISCOVER 发送失败，日志显示 `[DHCP] Failed to send DISCOVER`。

**根因分析**:

调用链追踪:
```
dhcp_start() → udp_sendto() → ip_send() → net_send_packet() → e1000_nic_send()
                                          ↑
                              get_primary_nic() 返回 NULL
```

`get_primary_nic()` (net.c:628-636) 只返回状态为 `NIC_STATE_RUNNING` 或 `NIC_STATE_UP` 的网卡:
```c
if (nic->state == NIC_STATE_RUNNING || nic->state == NIC_STATE_UP) {
    return nic;
}
```

但 `e1000_init()` (e1000.c:717-773) 中:
- `e1000_nic_init()` 初始化硬件
- `net_register_nic()` 注册网卡
- **缺少** `e1000_nic_start()` 调用

NIC 设备状态停留在 `NIC_STATE_DOWN`（在 `e1000_init` 第 744 行设置），导致 `get_primary_nic()` 永远找不到它。

**修复方法**:

在 `SYSTEM/lib/Ztknl/e1000.c` 的 `e1000_init()` 中，硬件初始化成功后调用 `e1000_nic_start()`:

```c
/* Initialize the hardware */
if (e1000_nic_init(&e1000_adapter) < 0) {
    printk("e1000: Failed to initialize hardware");
    return -1;
}

/* Start the NIC (enable TX/RX, transition to RUNNING state) */
if (e1000_nic_start(&e1000_adapter) < 0) {
    printk("e1000: Failed to start NIC");
    return -1;
}

/* Register with network stack */
net_register_nic(&e1000_adapter.nic_dev);
```

`e1000_nic_start()` 会:
- 启用 TX 和 RX 控制寄存器
- 将 `nic_dev.state` 设置为 `NIC_STATE_RUNNING`

**修改文件**:
- `SYSTEM/lib/Ztknl/e1000.c` (+6 行)

---

#### 2. 修复 WiFi 配置文件加载失败

**问题**: `wifi_autoconnect()` 调用 `net_config_parse("/system/internet/e1w.cfg")` 时，VFS 找不到文件，返回 `wifi: No network config found`。

**根因分析**:

1. **Limine 模块缺失**: `e1w.cfg` 和 `rtl8188eufw.bin` 没有作为 `module_path` 注册到 `limine.conf` 中
2. **RAMFS 目录缺失**: 在 QEMU 测试环境（无物理磁盘，使用 RAMFS 回退模式）下，`main.c` 只创建了 `/system/dbg/...` 目录，没有创建 `/system/internet/` 和 `/system/firmware/`
3. **文件未提取**: Limine 模块以内存镜像形式加载，不会自动出现在 VFS 文件系统中

**修复方法**:

**修复 2a**: 添加 Limine 模块注册 (`SYSTEM/boot/limine.conf`):
```
module_path: boot():/system/internet/e1w.cfg
module_path: boot():/system/firmware/rtl8188eufw.bin
```

**修复 2b**: 在 RAMFS 挂载时创建必要的目录并提取 Limine 模块 (`SYSTEM/lib/Ztknl/main.c`):
- 创建 `/system/internet/` 和 `/system/firmware/` 目录
- 遍历所有 Limine 模块，跳过 ELF 二进制文件（knl.bin, init, shell），将配置文件和固件写入 RAMFS
- 添加日志输出，方便调试

**修改文件**:
- `SYSTEM/boot/limine.conf` (+2 行)
- `SYSTEM/lib/Ztknl/main.c` (+45 行)

---

#### 3. Rust 监控内核 (rustknl) 开发

**当前状态**: 完成度从 ~15% 提升到 ~40%

**已完成的工作**:

##### 3.1 GDT 集成
- 修复 inline assembly 语法：使用 Intel syntax（Rust 默认），`lgdt [{0}]` 替代 `lgdt [%0]`
- 正确实现 `lgdt` 后的 CS 段寄存器刷新（`.intel_syntax noprefix` → `retfq` → `.att_syntax`）
- 移除未使用的 `global_asm` import

##### 3.2 IDT 集成
- 为 `IDTEntry` 添加 `#[derive(Copy, Clone)]` 解决数组初始化编译错误
- 修复函数指针转换：`handler as u64` 改为 `handler as *const () as u64`（消除 warning）
- 添加 4 个异常处理门:
  - 向量 0: 除法错误 (#DE)
  - 向量 6: 无效操作码 (#UD)
  - 向量 13: 通用保护异常 (#GP)
  - 向量 14: 页错误 (#PF) — 读取 CR2 获取错误地址
- 所有处理函数添加 `#[no_mangle]` 和 `extern "C"`
- `lidt` 使用正确的 Intel 语法 `lidt [{0}]`

##### 3.3 串口驱动修复
- 修复 `serial.rs` 中的 inline assembly：`out dx, al` / `in al, dx`（Intel syntax）
- 添加 `options(nomem, nostack)` 优化

##### 3.4 模块整合
- 重构 `lib.rs`，移除重复的内联代码，使用 `mod` 导入各模块:
  - `mod gdt` - GDT 实现
  - `mod idt` - IDT 实现
  - `mod serial` - 串口驱动
  - `mod vga` - VGA 文本模式驱动
  - `mod watchdog` - 看门狗监控
  - `mod shared_mem` - 共享内存监控
- 创建 `KernelState` 结构统一管理驱动状态
- 实现 `kprint`/`kprintln`/`print_hex`/`print_dec` 工具方法（同时输出到 VGA 和串口）

##### 3.5 Watchdog 改进
- 将阻塞式 `run() -> !` 改为非阻塞 `run_once()`，便于集成到主循环
- 添加 `tick_count` 计数器追踪运行周期
- 内存检查读取起始和结束地址，确保整个镜像可访问

##### 3.6 共享内存监控
- 在主循环中定期验证 checksum
- 发现不匹配时输出警告并更新基准 checksum

##### 3.7 编译验证
- `cargo build --lib --release` 成功编译
- 仅有 11 个 warning（dead_code、static_mut_refs 等），无 error

**修改文件**:
- `SYSTEM/lib/rust_kernel/src/lib.rs` (重写，~194 行)
- `SYSTEM/lib/rust_kernel/src/gdt.rs` (修复 asm 语法)
- `SYSTEM/lib/rust_kernel/src/idt.rs` (修复编译错误，添加异常处理)
- `SYSTEM/lib/rust_kernel/src/serial.rs` (修复 asm 语法)
- `SYSTEM/lib/rust_kernel/src/watchdog.rs` (改为非阻塞 run_once)

**构建验证**:
- 内核编译: `make -j$(nproc)` ✅
- ISO 构建: `./build_iso.sh` ✅ (img: 34MB, ISO: 1.9MB)
- Rust 内核: `cargo build --release` ✅

### 遗留问题

1. **DHCP 实际接收**: 发送修复后需要验证是否能收到 DHCP OFFER（QEMU 网络配置正确时有 DHCP 服务器）
2. **WiFi 硬件连接**: RTL8188EU 需要真实 USB WiFi 设备或 QEMU USB 直通，当前 QEMU 测试无法验证 WiFi 连接
3. **rustknl 内存管理**: 尚未实现物理/虚拟内存管理（当前作为监控内核运行在主内核的内存空间中）
4. **rustknl 中断处理**: 异常处理门已设置但处理函数仅 halt，需要实现完整的异常日志和恢复机制

### 代码统计

| 文件 | 变更 | 说明 |
|------|------|------|
| `e1000.c` | +6 | NIC 启动修复 |
| `limine.conf` | +2 | 添加配置/固件模块 |
| `main.c` | +45 | Limine 模块提取到 RAMFS |
| `rust_kernel/src/lib.rs` | ~194 | 重写，整合所有模块 |
| `rust_kernel/src/gdt.rs` | ~5 | 修复 asm 语法 |
| `rust_kernel/src/idt.rs` | ~93 | 修复编译错误，完善 IDT |
| `rust_kernel/src/serial.rs` | ~50 | 修复 asm 语法 |
| `rust_kernel/src/watchdog.rs` | ~87 | 改为非阻塞模式 |

**总计新增/修改代码**: ~482 行

### 下一步计划

1. QEMU 测试验证 DHCP 获取 IP 地址
2. 实现 shell 网络命令（ifconfig, ping, netstat）
3. rustknl 添加完整的异常日志输出（通过串口）
4. rustknl 实现独立的内存管理（PMM/VMM）
5. 完善 WiFi 实际连接流程测试
