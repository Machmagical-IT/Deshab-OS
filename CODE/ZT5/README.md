# ZratroOS

**版本**: v1.6.0
**状态**: 活跃开发中
**架构**: x86_64 Higher Half 独立操作系统
**主语言**: C (内核) + Rust (监控内核) + 汇编
**引导器**: Limine (UEFI/BIOS 双支持)

---

## 项目简介

ZratroOS 是一个从零开始编写的独立 64 位操作系统，具有完整的内核功能、用户空间程序和图形界面支持。内核空间映射在 `0xffffffff80000000` 以上地址 (Higher Half)。

**核心特性**：
- 完整的内核实现（内存管理、进程调度、文件系统、设备驱动）
- 多核支持（SMP、负载均衡、NUMA 感知调度）
- 网络协议栈（TCP/IP、DHCP、Socket、零拷贝网络）
- 多种文件系统支持（EXT4、FAT32、RAMFS、TMPFS、PROCFS）
- 图形界面（集成桌面环境、窗口管理器+文件管理器+任务栏、60fps 渲染）
- 安全机制（Capability 能力模型、栈保护、SMEP/SMAP、用户空间指针验证）
- **自愈框架**（故障检测、隔离域、检查点恢复、看门狗、心跳）
- **内核热更新**（模块热替换、差分更新、事务管理、回滚）
- **ZTS7 动态算法系统**（运行时算法加载/替换、API/Patch/Inject 模式）
- **双内核架构**（Ztknl 主内核 + Azknl/Rust 监控内核，VMX/EPT 隔离）
- **高级特性**（协程、异构计算、持久内存、零拷贝管道、实时调度）

---

## 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        ZratroOS 系统架构                         │
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                    硬件抽象层 (HAL)                        │  │
│  │  CPU (x86_64) | APIC | PCI Bus | Timer | CMOS/RTC         │  │
│  └───────────────────────────────────────────────────────────┘  │
│                              │                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                 Ztknl 主内核 (C - Higher Half)             │  │
│  │                                                           │  │
│  │  ┌──────────────┐ ┌──────────────┐ ┌──────────────────┐   │  │
│  │  │  内存管理子系统 │ │  进程管理子系统 │ │   文件系统子系统    │   │  │
│  │  │  PMM/VMM/Heap │ │ Task/Sched   │ │ VFS/EXT4/FAT32   │   │  │
│  │  │  SLAB/mmap/COW│ │ CFS/NUMA     │ │ RAMFS/TMPFS      │   │  │
│  │  └──────────────┘ └──────────────┘ └──────────────────┘   │  │

│  │  ┌──────────────┐ ┌──────────────┐ ┌──────────────────┐   │  │
│  │  │  设备驱动子系统 │ │  网络协议栈    │ │    安全子系统      │   │  │
│  │  │  AHCI/NVMe/   │ │ TCP/IP/DHCP  │ │ Capability/SMEP  │   │  │
│  │  │  USB/E1000/   │ │ Socket/      │ │ SMAP/StackProt   │   │  │
│  │  │  WiFi/DRM/FB  │ │ ZeroCopy     │ │ Selfheal         │   │  │
│  │  └──────────────┘ └──────────────┘ └──────────────────┘   │  │
│  │  ┌──────────────────────────────────────────────────────┐  │  │
│  │  │              高级特性子系统                            │  │  │
  │  │  │  Coroutine | Heterog | Hotupdate | VMX/EPT | PMEM     │  │  │
  │  │  │  ZPipe | Realtime | Perf | RCU | ZTS7 | AI          │  │  │
│  │  └──────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────┘  │
│                              │                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                  系统调用接口 (int 0x80)                    │  │
│  │                   180+ 系统调用                             │  │
│  └───────────────────────────────────────────────────────────┘  │
│                              │                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                     用户空间 (User Space)                   │  │
│  │                                                           │  │
│  │  ┌───────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ │  │
│  │  │  Init     │ │  Shell   │ │  Desk    │ │  WM      │ │  │
│  │  │  (PID 1)  │ │  (CLI)   │ │  Desktop │ │ zrdemage │ │  │
│  │  └───────────┘ └──────────┘ └──────────┘ └──────────┘ │  │
│  │  ┌──────────┐                                          │  │
│  │  │  FM      │                                          │  │
│  │  │ FileMgr  │                                          │  │
│  │  └──────────┘                                          │  │
│  │                                                           │  │
│  │  ┌──────────────────────────┐                             │  │
│  │  │  Rust 监控内核 v0.2       │                             │  │
│  │  │  (GDT/IDT/Watchdog)      │                             │  │
│  │  └──────────────────────────┘                             │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### 双内核架构

ZratroOS 采用双内核设计：

| 组件 | 说明 |
|------|------|
| **Ztknl** (主内核) | 完整的 C 语言内核，负责所有核心功能 |
| **Rsknl** (辅助内核) | 监控内核，可以是 Rust 实现或 C 实现 |


双内核通过 `inter_kernel.c` 进行进程间通信，共享内存机制在 `shared_mem` 中实现。主内核通过 VMX/EPT 技术加载副内核，实现隔离运行。

---

## 项目结构


```
ZratroOS/
├── SYSTEM/                          # 系统核心目录
│   ├── lib/                         # 库和源代码
│   │   ├── Ztknl/                   # 主内核 (Ztknl) - 100+ 个 C 模块
│   │   │   ├── include/             # 内核头文件 (~100 个)
│   │   │   ├── vmm/                 # VMM 子模块 (ept.c, vmexit.c, vmx.c, dual_kernel.c)
│   │   │   ├── examples/            # ZTS7 算法示例
│   │   │   ├── main.c               # 内核入口与分阶段初始化
│   │   │   ├── Makefile             # 内核编译脚本
│   │   │   └── linker.ld            # 内核链接脚本 (Higher Half)
│   │   ├── rust_kernel/             # Rust 监控内核 (v0.2)
│   │   │   ├── src/lib.rs           # 入口与监控循环
│   │   │   ├── src/gdt.rs           # GDT 实现
│   │   │   ├── src/idt.rs           # IDT 实现 (256 中断门)
│   │   │   ├── src/watchdog.rs      # 看门狗监控
│   │   │   └── src/shared_mem.rs    # 共享内存通信
│   │   ├── init/                    # Init 程序 (PID 1)
│   │   ├── shell/                   # Shell 程序 (CLI)
│   │   ├── desk/                    # 桌面环境 (Win11+KDE风格)
│   │   ├── wm/                      # 窗口管理器 (zrdemage)
│   │   ├── fm/                      # 文件管理器
│   │   └── lints/                   # ZTS7 Linux 内核移植
│   │       ├── linux/               # Linux 内核源码 (适配层)
│   │       │   ├── block/           # 块设备
│   │       │   ├── crypto/          # 加密算法
│   │       │   ├── fs/              # 文件系统 (ext2/ext4/fat/btrfs/...)
│   │       │   ├── io_uring/        # io_uring 异步 I/O
│   │       │   ├── ipc/             # 进程间通信
│   │       │   ├── kernel/          # 内核核心
│   │       │   └── lib/             # 通用库
│   │       ├── tools/               # 构建工具
│   │       └── build_zts.sh         # 编译脚本
│   ├── prgm/sys/                    # 编译后的用户程序 (init, shell, desk, fm)
│   ├── system/                      # 系统镜像输出
│   │   └── knl.bin                  # 编译后的内核 (~850KB)
│   │   └── zrdemage                 # 窗口管理器
│   ├── desktop/                     # 桌面配置
│   │   └── desktop.conf
│   ├── ini/                         # 用户初始化配置
│   │   └── usrroot/user.cfg
│   ├── boot/limine.conf             # Limine 引导配置
│   └── EFI/BOOT/BOOTX64.EFI         # Limine UEFI 引导器
│
├── ISO/                             # 镜像构建
│   ├── build_iso.sh                 # FAT32/ISO 打包脚本
│   ├── test_qemu_uefi.sh            # QEMU UEFI 测试脚本
│   └── zratroos.img                 # UEFI 可引导镜像
│
├── SERVER/                          # 服务器端 (网站/下载)
│   └── deaicup/                     # Web 服务文件
│
└── RE/                              # 研究与文档
    ├── DEVELOPMENT_GUIDE.md         # 完整开发文档
    ├── KERNEL_IMPLEMENTATION_DETAILS.md  # 内核实现细节
    ├── NETWORK_FEATURES.md          # 网络功能文档
    ├── LINUX_TO_ZTS_MIGRATION.md    # Linux 到 ZTS 迁移文档
    └── LOG/                         # 开发日志
```

---

## 内核功能 (Ztknl)

### 内存管理
- ✅ 物理内存管理 (PMM) - 位图分配器、Per-CPU 页缓存、KSM 页合并、内存压缩
- ✅ 虚拟内存管理 (VMM) - 4 级页表、2MB/1GB 大页、智能映射、SMAP 支持
- ✅ 堆内存管理 (Heap) - 16 大小类分离适配器、预分配池、Canary 溢出检测
- ✅ SLAB 分配器 - 对象缓存、Per-CPU 缓存、缓存行着色
- ✅ 内存映射 (mmap) - 红黑树 VMA 管理、匿名/文件映射、共享/私有
- ✅ 持久内存管理 (PMEM) - 非易失性内存分配、快照
- ✅ 写时复制 (COW) - 哈希引用计数、页错误处理

### 进程管理
- ✅ 进程/任务管理 (Task) - PID 哈希表、Per-CPU 内核栈、命名空间
- ✅ 完全公平调度器 (CFS) - 红黑树运行队列、虚拟时间、优先级加权
- ✅ EEVDF 调度器 - 最早合格虚拟截止时间优先 (Linux 6.6+ 算法)、延迟敏感自适应
- ✅ 实时调度 (SCHED_FIFO, SCHED_RR) - 硬实时任务、截止时间保证
- ✅ SMP 多核调度 - Per-CPU 运行队列、负载均衡、NUMA 感知
- ✅ 控制组 (Cgroup) - CPU/内存/IO 资源限制、层级结构
- ✅ 信号处理 (Signal) - POSIX 信号、实时信号
- ✅ ELF 加载和执行 - ELF64 解析、段映射、入口点设置

### 文件系统
- ✅ 虚拟文件系统 (VFS) - 统一接口、LRU 缓冲、32MB 页缓存、DCache、自适应预读
- ✅ EXT4 文件系统 - extent 映射、基本读写
- ✅ FAT32 文件系统 - 完整读写、LFN 长文件名、GPT/MBR 分区表支持
- ✅ RAM 文件系统 - 内存中文件系统
- ✅ 临时文件系统 (TMPFS)
- ✅ /proc 文件系统 (PROCFS) - 进程信息暴露

### 设备驱动
- ✅ Intel e1000 网卡驱动 - PCI MMIO、DMA 描述符环
- ✅ Realtek RTL8188EU WiFi 驱动 - USB 驱动、802.11 MAC、WPA/WPA2 安全
- ✅ AHCI SATA 控制器 - NCQ、FIS 协议
- ✅ NVMe SSD 接口 - 队列管理
- ✅ USB 主控制器 (UHCI/EHCI/XHCI) - USB 1.1/2.0/3.0
- ✅ DRM 直接渲染管理 - GPU 加速
- ✅ 帧缓冲驱动 (FB)
- ✅ **ZTS7 统一驱动模块** - Linux 内核驱动编译为可加载 .zts 模块
  - virtio-blk (QEMU/KVM 块存储)
  - virtio-net (QEMU/KVM 网络)
  - AHCI SATA 控制器
  - NVMe PCIe SSD
  - Intel e1000 网卡
  - RTL8188EU WiFi 适配器
  - XHCI USB 3.0 控制器

### 网络协议栈
- ✅ TCP/IP 协议栈 - 以太网/IP/TCP/UDP/ICMP/ARP
- ✅ Socket 接口 - socket/bind/listen/accept/connect/send/recv
- ✅ DHCP 客户端 - DORA 过程、自动获取 IP
- ✅ 零拷贝网络 - 避免内核-用户空间数据拷贝
- ✅ 流量控制 (QoS) - Token Bucket 算法

### 安全机制
- ✅ Capability 安全系统 - 不可伪造权限令牌、XOR-MAC 签名、权限委托/撤销
- ✅ 栈保护 - 栈溢出检测、Canary 值插入
- ✅ SMEP/SMAP - 内核态执行/访问用户空间保护
- ✅ 用户空间指针验证 - 地址范围检查
- ✅ 内核自检 (kselfcheck) - 核心功能验证
- ✅ **自愈机制 (Selfheal)** - 故障检测、隔离域、检查点恢复、看门狗、心跳

### 高级特性
- ✅ **内核热更新 (Hotupdate)** - 模块热替换、检查点回滚、差分更新、事务管理
- ✅ **ZTS7 动态算法系统** - 运行时加载/执行算法、API/Patch/Inject 模式、算法热替换
- ✅ **双内核架构 (Dual Kernel)** - VMX/EPT 隔离、共享内存通信、副内核加载
- ✅ 协程 (Coroutine) - 内核级协程、Go-style channel、工作窃取调度
- ✅ 异构计算 (Heterog) - GPU/FPGA 设备管理、任务提交
- ✅ 虚拟化 (VMX/EPT) - Intel VT-x、扩展页表
- ✅ 零拷贝管道 (ZPipe) - 零拷贝 IPC、Scatter-Gather I/O、背压控制
- ✅ 实时调度 (Realtime) - 硬实时、优先级继承
- ✅ 性能分析 (Perf) - 采样分析、火焰图
- ✅ RCU 同步机制 - 读复制更新、无锁读取
- ✅ 持久内存 (PMEM) - 非易失性段分配
- ✅ 用户事务 (Transaction) - tx_begin/commit/abort
- ✅ PEXLF IPC - 进程间通信、通道管理
- ✅ Windows NT 兼容层 (NTDLL) - NT 系统调用模拟、PE 加载
- ✅ 用户模式驱动框架 (UMDF)
- ✅ **AI 子系统** - 内核级 AI 核心逻辑、HTTP 通信、日志记录

---

## 用户程序

### init
- **位置**: `SYSTEM/prgm/sys/init`
- **大小**: ~18 KB
- **功能**: 系统初始化，启动窗口管理器/桌面/文件管理器，PID 1 孤儿进程回收，崩溃服务自动重启

### shell (zsh)
- **位置**: `SYSTEM/prgm/sys/shell`
- **大小**: ~25 KB
- **功能**: 交互式命令行界面
  - 命令历史 (32 条)
  - 命令行编辑
  - 内置命令 (cd, pwd, ls, echo, clear, help, exit)
  - 管道和重定向 (|, <, >, >>)
  - 外部程序执行 (fork + exec)

### wm (zrdemage) - 窗口管理器
- **位置**: `SYSTEM/system/zrdemage`
- **功能**: Win11 + KDE Plasma 融合风格窗口管理
  - 窗口 CRUD、焦点管理、Z-order 层级
  - Mica/Acrylic 半透明效果、圆角、阴影
  - Snap Layouts (Win11 风格窗口布局)
  - 鼠标/键盘事件分发、图层合成

### desk - 桌面环境
- **位置**: `SYSTEM/prgm/sys/desk`
- **功能**: 桌面 Shell
  - Mica 桌面背景
  - 桌面图标、右键上下文菜单
  - 任务栏 (KDE Plasma 风格)
  - 开机 LOGO: 屏幕显示大字 "ZT5" (带发光效果)
  - 60fps 渲染循环

### fm - 文件管理器
- **位置**: `SYSTEM/prgm/sys/fm`
- **功能**: Win11 + KDE Dolphin 融合风格文件管理
  - 正确显示文件信息 (大小、权限、修改时间)
  - 导航栏、侧边栏、面包屑路径
  - 文件/文件夹图标、列表/网格视图

---

## 图形界面

### UI 设计风格
- **Win11 + KDE Plasma 融合风格**: Mica/Acrylic 半透明效果、圆角窗口、阴影
- **模块化架构**: 窗口管理器 (wm)、桌面环境 (desk)、文件管理器 (fm) 分离为独立进程
- **开机 LOGO**: 屏幕显示大字 "ZT5" (带发光渐变效果)

### 窗口管理器 (zrdemage)
- **位置**: `SYSTEM/lib/wm/` → `SYSTEM/system/zrdemage`
- **功能**: 窗口 CRUD、焦点管理、Z-order 层级、鼠标/键盘事件分发、图层合成、Snap Layouts

### 桌面环境 (desk)
- **位置**: `SYSTEM/lib/desk/` → `SYSTEM/prgm/sys/desk`
- **功能**: Mica 桌面背景、桌面图标、右键菜单、任务栏、开机 LOGO

### 文件管理器 (fm)
- **位置**: `SYSTEM/lib/fm/` → `SYSTEM/prgm/sys/fm`
- **功能**: 文件系统操作、目录浏览、正确文件信息显示、导航栏、面包屑路径

---

## rustknl (Rust 监控内核 v0.2)

**开发状态**: 活跃开发中 (完成度 ~40%)

**已实现功能**:
- ✅ VGA 输出 (80x25 文本模式)
- ✅ 串口输出 (COM1, 115200bps)
- ✅ GDT 实现 (5 个段描述符，含代码段/数据段/TSS)
- ✅ IDT 实现 (256 个中断门，4 个异常处理: #DE, #UD, #GP, #PF)
- ✅ 看门狗监控 (非阻塞 run_once 模式，tick 计数)
- ✅ 共享内存通信 (校验和验证)
- ✅ 主内核/辅助内核健康状态监控
- ✅ 异常处理与恢复机制

**未实现功能**:
- ❌ 动态内存管理 (独立堆分配器)
- ❌ 异常日志记录到 VFS
- ❌ 网络栈监控
- ❌ 进程调度监控
- ❌ 性能分析工具

**编译方法**:
```bash
cd SYSTEM/lib/rust_kernel
cargo build --lib --release
```

---

## ZTS7 动态算法系统

**位置**: `SYSTEM/lib/Ztknl/zts_algorithm.c` + `SYSTEM/lib/lints/`

**功能**:
- 运行时动态加载和执行 `.zts` 模块
- 三种运行模式：API 模式、Patch 模式、Inject 模式
- 完整的内核 API 表 (alloc/free/log/strcmp/memcpy/VFS/net)
- 模块验证 (魔数、版本、校验和)
- 优先级排序加载
- 支持模块热替换 (Patch 模式)

### Linux 内核移植 (2026-04-22 新增)

**适配层**: `SYSTEM/lib/lints/include/zts_adapt.h`

**原理**: 将 Linux 内核驱动和算法通过适配层映射到 ZTS API，编译为可加载模块。

**API 映射示例**:
```c
#include "zts_adapt.h"

void *p = kmalloc(1024);        // → zts_api->alloc(1024)
printk("Hello: %p\n", p);       // → zts_api->log_print(...)
kfree(p);                       // → zts_api->free(p)
```

**构建工具链**:
```bash
cd SYSTEM/lib/lints
./build_zts.sh init              # 初始化项目
./build_zts.sh build <module>    # 编译模块
./build_zts.sh build_all         # 编译所有模块
```

### 已编译的 ZTS 模块

| 模块 | 优先级 | 大小 | 说明 |
|------|--------|------|------|
| example | 0x00000000 | 15KB | 示例模块 |
| TCP CUBIC | 0x00000001 | 15KB | TCP 拥塞控制 |
| TCP BBR | 0x00000002 | 15KB | BBR v2 拥塞控制 |
| virtio-blk | 0x00000010 | 15KB | Virtio 块驱动 |
| **All Drivers** | **0x00000020** | **20KB** | **统一驱动包** |

### 统一驱动模块 (0x00000020.zts)

**设计理念**: 将所有设备驱动合并为单个 ZTS 模块，通过 `driver_id` 分发请求。

| driver_id | 驱动 | 功能 | 状态 |
|-----------|------|------|------|
| 0 | virtio-blk | 块存储 | ✅ |
| 1 | virtio-net | 网络设备 | ✅ |
| 2 | AHCI | SATA 控制器 | ✅ |
| 3 | NVMe | PCIe SSD | ✅ |
| 4 | e1000 | Intel 网卡 | ✅ |
| 5 | RTL8188EU | WiFi 适配器 | ✅ |
| 6 | XHCI | USB 3.0 | ✅ |
| 7 | QUERY | 驱动状态查询 | ✅ |

**加载路径**: `/zrr64/zts/p.zts6/`

**引导配置** (`limine.conf`):
```
module_path: boot():/system/zrr64/zts/p.zts6/0x00000020.zts
```

**使用示例**:
```c
// 查询所有驱动状态
struct driver_input inp = { .driver_id = 7 };
struct driver_output out;
zts_execute_module("all_drivers", &inp, &out);
// out.bytes 位掩码: bit0=virtio-blk, bit1=virtio-net, ...

// 使用 virtio-blk 读取
inp.driver_id = 0;
inp.cmd = 0;  // READ
inp.sector = 0;
inp.sectors = 8;
inp.data = buffer;
zts_execute_module("all_drivers", &inp, &out);
```

---

## 编译和构建

### 编译内核

```bash
cd SYSTEM/lib/Ztknl
make clean
make -j$(nproc)
```

**输出**: `SYSTEM/system/knl.bin` (约 850KB)

### 编译用户程序

```bash
# Init (PID 1)
cd SYSTEM/lib/init && make clean && make

# Shell
cd SYSTEM/lib/shell && make clean && make

# Desktop (桌面环境)
cd SYSTEM/lib/desk && make clean && make

# Window Manager (窗口管理器)
cd SYSTEM/lib/wm && make clean && make

# File Manager (文件管理器)
cd SYSTEM/lib/fm && make clean && make
```

### 全量构建

```bash
./build_all.sh
# 一次性编译内核和所有用户程序
# 注意: build_all.sh 在项目根目录，不在 SYSTEM/ 下
```

### 编译 Rust 监控内核

```bash
cd SYSTEM/lib/rust_kernel
cargo build --lib --release
```

### 构建镜像

```bash
cd ISO
./build_iso.sh
```

**输出**: `ISO/zratroos.img` (UEFI 可引导)

---

## 测试方法

### QEMU UEFI 测试

```bash
qemu-system-x86_64 \
    -drive file=ISO/zratroos.img,format=raw \
    -m 2G -smp 2 -serial stdio
```

### QEMU BIOS 测试

```bash
qemu-system-x86_64 \
    -drive file=ISO/zratroos.img,format=raw \
    -m 2G -smp 2 -serial stdio
```

### 真实硬件测试

```bash
sudo dd if=ISO/zratroos.img of=/dev/sdX bs=4M status=progress conv=fsync
```

---

## 开发文档

- `RE/DEVELOPMENT_GUIDE.md` - 完整开发文档 (系统架构、API 参考)
- `RE/KERNEL_IMPLEMENTATION_DETAILS.md` - 内核实现细节 (算法、数据结构)
- `RE/NETWORK_FEATURES.md` - 网络功能文档
- `RE/LOG/` - 开发日志

---

## 已知限制

1. **rustknl 未完成** - 约 40% 完成度，缺少独立内存管理
2. **动态链接** - 只支持静态链接程序
3. **POSIX 兼容** - 部分功能未完全实现
4. **多用户系统** - 未实现用户和权限

---

## 许可证

ZratroOS 项目内部使用

---

**项目维护**: ZratroOS 开发团队
**文档版本**: v1.6.0
**最后更新**: 2026-05-23

### 最新更新 (2026-05-23)

- ✅ **AI 子系统**: 新增 ai_core/ai_http/ai_log 内核模块，支持 AI 核心逻辑与 HTTP 通信
- ✅ **ZTS7 Linux 内核移植**: 完成 Linux 内核驱动到 ZTS 模块的编译适配
- ✅ **统一驱动模块**: 将 7 个设备驱动合并为单个 `0x00000020.zts` 模块 (20KB)
- ✅ **ZTS 适配层**: 完整的 Linux API 到 ZTS API 映射 (`zts_adapt.h`)
- ✅ **构建工具链**: 自动化编译脚本 (`build_zts.sh`) 和头注入工具
- ✅ **下载中心**: 网页下载中心更新，支持模块下载
- ✅ **开发文档**: 完整记录 ZTS7 架构、驱动实现、编译流程
