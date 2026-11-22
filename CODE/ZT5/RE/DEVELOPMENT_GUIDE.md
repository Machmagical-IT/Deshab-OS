# ZratroOS 完整开发文档

**版本**: ZT5
**最后更新**: 2026-05-25
**状态**: 活跃开发中

---

## 目录

1. [项目概览](#1-项目概览)
2. [系统架构](#2-系统架构)
3. [内核功能详解](#3-内核功能详解)
4. [用户空间程序](#4-用户空间程序)
5. [构建系统](#5-构建系统)
6. [开发环境搭建](#6-开发环境搭建)
7. [测试与调试](#7-测试与调试)
8. [代码规范](#8-代码规范)
9. [模块依赖关系](#9-模块依赖关系)
10. [API 参考](#10-api-参考)

---

## 1. 项目概览

### 1.1 基本信息

| 项目属性 | 说明 |
|---------|------|
| **名称** | ZratroOS |
| **版本** | ZT5 |
| **架构** | x86_64 Higher Half |
| **主语言** | C (内核) |
| **辅助语言** | Rust (监控内核), 汇编 (启动代码) |
| **引导器** | Limine (UEFI/BIOS 双支持) |
| **开发状态** | 活跃开发中 |

### 1.2 项目简介

ZratroOS 是一个从零开始编写的独立 64 位操作系统，具有完整的内核功能、用户空间程序和图形界面支持。项目采用 Higher Half 内存布局，内核空间映射在 `0xffffffff80000000` 以上地址。

### 1.3 核心特性

- **完整内核实现**: 内存管理 (PMM/VMM/Heap/SLAB)、进程调度 (CFS+EEVDF/实时)、文件系统 (VFS/EXT4/FAT32)、设备驱动
- **多核支持 (SMP)**: 多核引导、AP 启动、IPI 通信、Per-CPU 调度、负载均衡
- **网络协议栈**: 以太网/IP/TCP/UDP/ICMP/ARP 完整实现，DHCP 自动配置，Socket API
- **多文件系统**: VFS 虚拟文件系统，EXT4/FAT32/RAMFS/TMPFS/PROCFS 驱动
- **图形界面**: Win11+KDE Plasma 融合风格，窗口管理器/桌面/文件管理器分离架构，Mica/Acrylic效果，60fps 渲染
- **高级特性**: 零拷贝网络、协程、异构计算、内核热更新、持久内存、自愈框架、EEVDF调度
- **安全机制**: Capability 能力模型、栈保护、SMEP/SMAP、用户空间指针验证

### 1.4 项目目录结构

```
Deaicup-ZratroOS/
├── SYSTEM/                          # 系统核心目录
│   ├── lib/                         # 库和源代码
│   │   ├── Ztknl/                   # 主内核 (Ztknl) - 92+ 个 C 模块
│   │   │   ├── include/             # 内核头文件 (~100 个)
│   │   │   ├── vmm/                 # VMM 子模块
│   │   │   ├── main.c               # 内核入口与分阶段初始化
│   │   │   ├── Makefile             # 内核编译脚本
│   │   │   └── linker.ld            # 内核链接脚本 (Higher Half)
│   │   ├── init/                    # Init 程序 (PID 1)
│   │   ├── shell/                   # Shell 程序 (命令行)
│   │   ├── desk/                    # 桌面环境 (Win11+KDE风格)
│   │   ├── wm/                      # 窗口管理器 (zrdemage)
│   │   ├── fm/                      # 文件管理器
│   │   ├── rust_kernel/             # Rust 监控内核
│   │   └── zts7/                    # ZTS7 动态算法系统
│   ├── prgm/                        # 编译后的用户程序
│   │   └── sys/                     # init, shell, desk, fm 二进制
│   ├── system/                      # 系统镜像输出
│   │   ├── knl.bin                  # 编译后的内核 (~850KB)
│   │   └── zrdemage                 # 窗口管理器
│   ├── boot/                        # 启动配置
│   │   └── limine.conf              # Limine 引导配置
│   ├── zrr64/                       # 64 位子系统集成
│   └── EFI/                         # UEFI 引导文件
│       └── BOOT/
│           └── BOOTX64.EFI          # Limine UEFI 引导器
│
├── build_all.sh                     # 全量构建脚本
│
├── ISO/                             # 镜像构建
│   ├── build_iso.sh                 # FAT32/ISO 打包脚本
│   └── zratroos.img                 # FAT32 镜像 (UEFI 引导)
│
├── SERVER/                          # 服务器组件
│
└── RE/                              # 研究与文档
    ├── DEVELOPMENT_GUIDE.md         # 本文档
    └── ...                          # 其他研究文档
```

---

## 2. 系统架构

### 2.1 整体架构图

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
│  │  │  ZPipe | Realtime | Perf | RCU | ZTS7                │  │  │
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
│  │  ┌───────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │  │
│  │  │  Init     │ │  Shell   │ │  Desk    │ │  WM      │  │  │
│  │  │  (PID 1)  │ │  (CLI)   │ │ Desktop  │ │ zrdemage │  │  │
│  │  └───────────┘ └──────────┘ └──────────┘ └──────────┘  │  │
│  │  ┌──────────┐                                          │  │
│  │  │  FM      │                                          │  │
│  │  │ FileMgr  │                                          │  │
│  │  └──────────┘                                          │  │
│  │                                                           │  │
│  │  ┌──────────────────────────┐                             │  │
│  │  │  Rust 监控内核            │                             │  │
│  │  │  (串口/VGA双输出)         │                             │  │
│  │  └──────────────────────────┘                             │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 内存布局 (Higher Half)

```
0xFFFFFFFFFFFFFFFF ┌──────────────────────────────┐
                   │      内核 Higher Half         │
0xFFFFFFFF80000000 │  内核代码/数据/堆/栈 (128TB+)  │
                   │                              │
                   │                              │
                   │                              │
                   │                              │
                   │                              │
                   │                              │
                   │                              │
0x0000000100000000 │                              │
                   │        用户空间 (512GB)        │
0x00000000400000   │  用户程序加载地址 (4MB +)      │
0x00000000000000   │  NULL 页 / 低地址保护          │
                   └──────────────────────────────┘
```

### 2.3 引导流程

```
UEFI/BIOS
    │
    ▼
Limine Bootloader
    │ 加载 limine.conf
    │ 加载 knl.bin (ELF64)
    │ 加载 initrd (可选)
    │
    ▼
_start (entry point)
    │
    ▼
引导验证 (boot_diag.c)
    │ 验证 Limine 协议
    │ 检测内存映射
    │
    ▼
阶段 1: 核心初始化
    │ GDT 设置 (gdt.c + gdt_asm.S)
    │ IDT 设置 (idt.c + idt_asm.S)
    │ 中断处理 (isr.c)
    │
    ▼
阶段 2: 内存初始化
    │ PMM (pmm.c) - 物理内存分配
    │ VMM (vmm.c) - 虚拟内存映射
    │ Heap (heap.c) - 内核堆
    │
    ▼
阶段 3: 多核与设备
    │ SMP 引导 (smp.c + smp_asm.S)
    │ APIC 初始化 (apic.c)
    │ PCI 枚举 (pci.c)
    │
    ▼
阶段 4: 文件系统与驱动
    │ VFS 初始化 (vfs.c)
    │ 磁盘驱动 (disk.c, ahci.c, nvme.c)
    │ 文件系统挂载 (fat32.c, ext4.c)
    │
    ▼
阶段 5: 网络与高级功能
    │ 网络栈 (net.c, tcp.c)
    │ USB (usb.c)
    │ DRM/FB (drm.c, fb.c)
    │
    ▼
阶段 6: 用户空间启动
    │ ELF 加载器 (elf.c)
    │ 启动 init (PID 1)
    │
    ▼
init → shell / desk
```

### 2.4 双内核架构 (Ztknl / Azknl)

ZratroOS 采用双内核设计:

| 组件 | 说明 |
|------|------|
| **Ztknl** (主内核) | 完整的 C 语言内核，负责所有核心功能 |
| **Azknl** (辅助内核) | 监控内核，监控主内核运行状态 |

双内核通过 `inter_kernel.c` 进行进程间通信，共享内存机制在 `shared_mem` 中实现。

---

## 3. 内核功能详解

### 3.1 引导与初始化

**源文件**: `SYSTEM/lib/Ztknl/main.c`, `boot_diag.c`

#### 3.1.1 Limine 引导协议验证

内核通过 Limine 协议获取系统信息:
- 内存映射 (HHDM, 5 级页表支持)
- 内核模块信息
- 启动时间
- 命令行参数

#### 3.1.2 分阶段初始化

内核初始化严格按照以下顺序进行:

1. **GDT/IDT**: 设置段描述符表和中断描述符表
2. **PMM/VMM**: 物理和虚拟内存管理器
3. **设备初始化**: APIC、PCI、定时器
4. **文件系统**: VFS、磁盘驱动、文件系统挂载
5. **网络栈**: 网卡驱动、协议栈初始化
6. **用户空间**: ELF 加载器、init 进程启动

#### 3.1.3 ELF 程序加载

支持 ELF64 格式的程序加载:
- 解析 ELF 头部和程序头
- 创建进程地址空间
- 加载段到虚拟内存
- 设置入口点和栈

**关键文件**: `elf.c` (20900 字节), `loader.c` (22075 字节), `exec.c` (13199 字节)

### 3.2 内存管理

ZratroOS 的内存管理系统是分层设计的，从物理内存到虚拟内存，从内核堆到用户空间 mmap，形成完整的内存管理栈。

#### 3.2.1 PMM - 物理内存管理器

**源文件**: `SYSTEM/lib/Ztknl/pmm.c` (52KB)
**头文件**: `include/pmm.h`

**核心特性**:
- **位图分配器**: 使用位图追踪物理页分配状态 (4KB 页)
- **Per-CPU 页缓存**: 每个 CPU 核心拥有独立的页缓存，减少锁竞争
- **内存区域管理**: 将物理内存划分为多个区域进行管理
- **内存压缩**: 自动回收和压缩碎片化内存
- **KSM 页合并**: 内核相同页合并 (Kernel Samepage Merging)

**关键数据结构**:
```c
// 物理内存区域
struct pmm_region {
    uint64_t base_addr;
    uint64_t size;
    uint64_t free_pages;
    uint8_t *bitmap;
};

// 内存区域链表
struct pmm_area {
    struct pmm_region *regions;
    uint64_t num_regions;
    uint64_t total_free;
};
```

#### 3.2.2 VMM - 虚拟内存管理器

**源文件**: `SYSTEM/lib/Ztknl/vmm.c` (70KB) + `vmm/` 子目录
**头文件**: `include/vmm.h`

**核心特性**:
- **4 级页表管理**: PML4 -> PDPT -> PD -> PT
- **大页支持**: 2MB (Page Directory) 和 1GB (Page Directory Pointer Table) 大页映射
- **智能映射**: 根据内存地址自动选择最佳映射策略
- **SMAP 用户空间访问验证**: Supervisor Mode Access Prevention 支持
- **交换框架**: 页面换出到磁盘的准备框架
- **内存压力监控**: 实时监控内存使用率，触发回收机制

**内存映射函数**:
```c
// 映射物理页到虚拟地址
void *vmm_map(uint64_t phys_addr, uint64_t size, uint64_t flags);

// 取消映射
void vmm_unmap(uint64_t virt_addr, uint64_t size);

// 创建用户空间映射
void *vmm_map_user(uint64_t phys_addr, uint64_t size);
```

#### 3.2.3 Heap - 内核堆分配器

**源文件**: `SYSTEM/lib/Ztknl/heap.c` (106KB)
**头文件**: `include/heap.h`

**核心特性**:
- **16 大小类分离适配器**: 将内存请求分类到 16 个不同大小的类别，减少内部碎片
- **预分配池**: 常用大小的内存块预先分配，加速分配
- **堆溢出检测**: 检测缓冲区溢出
- **Canary 防护**: 在内存块前后插入 canary 值检测溢出

**分配器架构**:
```
┌─────────────────────────────────────────────┐
│              Heap 分配器架构                  │
│                                             │
│  小对象 (< 16 classes) → 分离适配器          │
│  中对象 (1KB - 64KB) → 预分配池              │
│  大对象 (> 64KB) → 直接映射                  │
│                                             │
│  每个分配: [Canary][Metadata][Data][Canary] │
└─────────────────────────────────────────────┘
```

#### 3.2.4 SLAB - 对象缓存分配器

**源文件**: `SYSTEM/lib/Ztknl/slab.c` (42KB)
**头文件**: `include/slab.h`

**核心特性**:
- **对象缓存**: 为频繁分配的小型对象创建专用缓存
- **Per-CPU 缓存**: 减少多核竞争
- **着色 (Coloring)**: 优化缓存行利用

#### 3.2.5 Mmap - 内存映射

**源文件**: `SYSTEM/lib/Ztknl/mmap.c` (18KB)
**头文件**: `include/mmap.h`

**核心特性**:
- **红黑树 VMA 管理**: 使用红黑树管理虚拟内存区域 (VMA)，支持 O(log n) 查找
- **匿名映射**: 无文件后端的内存映射
- **文件映射**: 将文件映射到内存
- **共享/私有映射**: 支持 MAP_SHARED 和 MAP_PRIVATE

#### 3.2.6 COW - 写时复制

**源文件**: `SYSTEM/lib/Ztknl/cow.c` (14KB)
**头文件**: `include/cow.h`

**核心特性**:
- **fork() COW**: 进程 fork 时共享页面，仅在写入时复制
- **页面错误处理**: 缺页异常触发 COW 复制
- **引用计数**: 跟踪共享页面的引用数

### 3.3 进程/任务管理

#### 3.3.1 Task - 进程/线程管理

**源文件**: `SYSTEM/lib/Ztknl/task.c` (60KB)
**头文件**: `include/task.h`

**核心特性**:
- **PID 哈希表**: 使用哈希表管理进程 ID，支持 O(1) 查找
- **Per-CPU 内核栈缓存**: 每个 CPU 核心独立的内核栈缓存
- **命名空间**: 支持进程命名空间隔离
- **资源限制**: 进程级资源限制 (RLIMIT)

**进程状态机**:
```
NEW → READY → RUNNING → SLEEPING/STOPPED → ZOMBIE → EXITED
              ↕
          WAITING (IO)
```

**关键数据结构**:
```c
struct task_struct {
    pid_t pid;
    pid_t ppid;
    char name[64];
    int state;
    uint64_t flags;
    struct task_struct *parent;
    struct list_head children;
    struct list_head sibling;
    uint64_t *kstack;         // 内核栈
    struct memory_space *mm;   // 内存空间
    struct cpu_state *ctx;     // CPU 上下文
    // ... 更多字段
};
```

#### 3.3.2 Scheduler - SMP 调度器

**源文件**: `SYSTEM/lib/Ztknl/scheduler.c` (58KB)
**头文件**: `include/scheduler.h`

**核心特性**:
- **Per-CPU SMP 调度**: 每个 CPU 核心独立的运行队列
- **实时调度**: SCHED_FIFO (先入先出) 和 SCHED_RR (轮转) 支持
- **NUMA 感知**: 考虑内存访问延迟的调度决策
- **负载均衡**: 在 CPU 核心间迁移任务，保持负载平衡

#### 3.3.3 CFS - 完全公平调度器

**源文件**: `SYSTEM/lib/Ztknl/cfs.c` (39KB)
**头文件**: `include/cfs.h`

**核心特性**:
- **红黑树运行队列**: 使用虚拟运行时间 (vruntime) 排序
- **公平调度**: 确保每个任务获得公平的 CPU 时间
- **优先级加权**: 根据 nice 值调整时间片
- **组调度**: 支持进程组的调度

#### 3.3.4 Cgroup - 控制组资源管理

**源文件**: `SYSTEM/lib/Ztknl/cgroup.c` (27KB)
**头文件**: `include/cgroup.h`

**核心特性**:
- **资源限制**: CPU、内存、IO 资源限制
- **层级结构**: 支持嵌套控制组
- **统计信息**: 实时资源使用统计

### 3.4 文件系统

#### 3.4.1 VFS - 虚拟文件系统

**源文件**: `SYSTEM/lib/Ztknl/vfs.c` (88KB)
**头文件**: `include/vfs.h`

**核心特性**:
- **统一文件操作接口**: open/read/write/close/stat 等 POSIX 接口
- **缓冲区缓存 (LRU)**: 最近最少使用策略的块缓存
- **页缓存 (32MB)**: 文件内容的页级缓存
- **目录项缓存 (DCache)**: 加速路径名查找
- **自适应预读**: 根据访问模式预测并预读数据
- **文件系统注册**: 支持动态注册新的文件系统类型

**VFS 层次结构**:
```
VFS (虚拟层)
  ├── FAT32 (fat32.c - 120KB)
  ├── EXT4 (ext4.c - 22KB)
  ├── RAMFS (ramfs.c - 11KB)
  ├── TMPFS (tmpfs.c - 6KB)
  └── PROCFS (procfs.c - 5KB)
```

#### 3.4.2 FAT32 文件系统

**源文件**: `SYSTEM/lib/Ztknl/fat32.c` (120KB)
**头文件**: `include/fat32.h`

**特性**: 
- 完整的 FAT32 读写支持，长文件名 (LFN)
- 目录操作（创建、删除、遍历）
- **GPT 分区表解析**：支持 GPT 和 MBR 两种分区表格式
- 保护 MBR 检测（偏移 450 处类型 0xEE）
- GPT Header 解析和分区表项读取
- 分区起始 LBA 自动提取
- 回退机制：当磁盘不可用时自动切换到 RAMFS

**GPT 检测流程**:
```
1. 读取 LBA 0 (MBR 扇区)
2. 检查 MBR 签名 (0x55AA)
3. 检查 mbr[450] == 0xEE (GPT 保护分区标识)
   ├─ 是: 读取 LBA 1 (GPT Header)
   │      ├─ 验证 "EFI PART" 签名
   │      ├─ 获取分区表项起始 LBA
   │      └─ 读取第一个分区表项，提取分区起始 LBA
   └─ 否: 使用传统 MBR 分区表
```

#### 3.4.3 EXT4 文件系统

**源文件**: `SYSTEM/lib/Ztknl/ext4.c` (22KB)
**头文件**: `include/ext4.h`

**特性**: EXT4 基本读写支持，extent 映射

#### 3.4.4 RAMFS / TMPFS

**源文件**: `SYSTEM/lib/Ztknl/ramfs.c`, `tmpfs.c`
**用途**: 内存中的文件系统，用于临时数据和 /proc 挂载

#### 3.4.5 PROCFS

**源文件**: `SYSTEM/lib/Ztknl/procfs.c` (5KB)
**用途**: 提供 /proc 接口，暴露内核和进程信息到用户空间

### 3.5 设备驱动

#### 3.5.1 存储驱动

| 驱动 | 源文件 | 大小 | 功能 |
|------|--------|------|------|
| **块设备抽象** | `disk.c` | 30KB | 通用块设备抽象层 |
| **AHCI** | `ahci.c` | 37KB | SATA 控制器驱动 (NCQ, FIS) |
| **NVMe** | `nvme.c` | 22KB | NVMe SSD 驱动 (队列管理) |
| **ATA/IDE** | `disk.c` | - | ATA PIO 模式读写 (LBA28/LBA48) |
| **virtio-blk** | `virtio.c` | 12KB | QEMU 半虚拟化块设备驱动 |

**ATA/IDE 驱动详情**:

- **I/O 端口**: 使用标准 ATA 端口 (0x1F0-0x1F7)
- **命令模式**:
  - LBA28: 适用于 LBA < 2^28 (支持最大 2TB)
  - LBA48: 适用于大磁盘 (>2TB)
- **读取命令**: `0x20` (LBA28 PIO Read), `0x24` (LBA48 PIO Read Extended)
- **写入命令**: `0x35` (LBA48 DMA Write Extended)
- **状态检查**: 
  - BSY (0x80): 设备忙
  - DRQ (0x08): 数据请求
  - ERR (0x01): 错误
- **重试机制**: 3 次重试，应对 QEMU 模拟超时
- **超时设置**: 5 秒 (5,000,000,000 TSC 周期)

**virtio-blk 驱动详情**:

- **PCI 设备**: Vendor 0x1AF4, Device 0x1001
- **接口类型**: 传统 PCI I/O 空间 (PIO)
- **VirtQueue**: 256 描述符环形缓冲区
- **描述符结构**:
  - 描述符 0: 请求头 (16 字节)
  - 描述符 1: 数据缓冲区 (512 字节)
  - 描述符 2: 状态字节 (1 字节)
- **特性协商**: VIRTIO_F_RING_INDIRECT_DESC, VIRTIO_BLK_F_BLK_SIZE
- **当前状态**: 驱动已实现，但在 QEMU 中有超时问题，正在调试

#### 3.5.2 USB 子系统

| 驱动 | 源文件 | 功能 |
|------|--------|------|
| **USB 核心** | `usb.c` | 27KB - USB 设备枚举、传输管理 |
| **UHCI** | `uhci.c` | 19KB - USB 1.1 主控制器 |
| **EHCI** | `ehci.c` | 23KB - USB 2.0 高速主控制器 |
| **XHCI** | `xhci.c` | 24KB - USB 3.0 超高速主控制器 |
| **USB-PCI** | `usb_pci.c` | 9KB - PCI USB 控制器发现 |

**USB 子系统架构**:
```
用户空间
    │
    ▼
USB Core (usb.c)
    ├── UHCI (USB 1.1 - 12Mbps)
    ├── EHCI (USB 2.0 - 480Mbps)
    └── XHCI (USB 3.0 - 5Gbps)
          │
          ▼
       PCI Bus 枚举
```

#### 3.5.3 网络驱动

| 驱动 | 源文件 | 大小 | 功能 |
|------|--------|------|------|
| **E1000** | `e1000.c` | 27KB | Intel 千兆以太网卡 |
| **WiFi** | `wifi.c` | 45KB | Realtek RTL8188EU 无线网卡 |

#### 3.5.4 显示驱动

| 驱动 | 源文件 | 大小 | 功能 |
|------|--------|------|------|
| **DRM** | `drm.c` | 38KB | GPU 直接渲染管理 |
| **FB** | `fb.c` | 13KB | 帧缓冲驱动 |

#### 3.5.5 输入子系统

**源文件**: `SYSTEM/lib/Ztknl/input.c` (22KB)
**头文件**: `include/input.h`

**功能**: 鼠标、键盘事件处理，事件队列管理

### 3.6 网络协议栈

#### 3.6.1 核心网络栈

**源文件**: `SYSTEM/lib/Ztknl/net.c` (45KB)
**头文件**: `include/net.h`

**协议层**:
```
应用层
    │
Socket API (socket.c)
    │
传输层
    ├── TCP (tcp.c - 72KB)  - 面向连接,可靠传输
    ├── UDP                  - 无连接,快速传输
    └── ICMP                 - 网络控制消息
    │
网络层
    └── IP                   - 路由,分片
    │
链路层
    ├── ARP                  - 地址解析
    └── Ethernet             - 帧格式
    │
物理层
    ├── E1000 (网卡驱动)
    └── WiFi (无线网卡)
```

#### 3.6.2 TCP 协议实现

**源文件**: `SYSTEM/lib/Ztknl/tcp.c` (72KB)
**头文件**: `include/tcp.h`

**核心特性**:
- TCP 状态机 (CLOSED, LISTEN, SYN_SENT, ESTABLISHED, FIN_WAIT, TIME_WAIT, CLOSE_WAIT)
- 滑动窗口流控
- 慢启动/拥塞避免
- 连接超时与重传
- 三向握手/四向挥手

#### 3.6.3 DHCP 客户端

**源文件**: `SYSTEM/lib/Ztknl/dhcp.c` (18KB) + `net_dhcp.c` (9KB)
**头文件**: `include/dhcp.h`

**功能**: DORA 过程 (Discover-Offer-Request-Ack)，自动获取 IP 地址

#### 3.6.4 Socket API

**源文件**: `SYSTEM/lib/Ztknl/socket.c` (12KB)
**头文件**: `include/socket.h`

**支持的操作**: socket, bind, listen, accept, connect, send, recv, shutdown

#### 3.6.5 零拷贝网络

**源文件**: `SYSTEM/lib/Ztknl/net_zerocopy.c` (14KB)
**头文件**: `include/net_zerocopy.h`

**核心特性**: 避免内核-用户空间数据拷贝，通过内存映射实现高速数据传输

#### 3.6.6 流量控制 (QoS)

网络层支持服务质量控制，优先级队列，带宽管理

### 3.7 中断/异常处理

#### 3.7.1 IDT - 中断描述符表

**源文件**: `SYSTEM/lib/Ztknl/idt.c` (23KB) + `idt_asm.S`
**头文件**: `include/idt.h`

**功能**: 设置 256 个中断向量，硬件中断和软件异常处理

#### 3.7.2 ISR - 中断服务例程

**源文件**: `SYSTEM/lib/Ztknl/isr.c` (46KB)
**头文件**: `include/isr.h`

**核心特性**:
- **详细异常分析**: 打印异常类型、错误码、寄存器状态
- **堆栈转储**: 异常时输出完整堆栈追踪
- **异常恢复**: 部分异常支持恢复执行

**中断向量分配**:
| 向量 | 类型 | 说明 |
|------|------|------|
| 0 | #DE | 除零错误 |
| 6 | #UD | 无效操作码 |
| 13 | #GP | 通用保护异常 |
| 14 | #PF | 页错误 |
| 32-47 | IRQ | PIC 硬件中断 |
| 128 | Syscall | 系统调用 |

#### 3.7.3 APIC - 高级可编程中断控制器

**源文件**: `SYSTEM/lib/Ztknl/apic.c` (19KB)
**头文件**: `include/apic.h`

**功能**: 本地 APIC 和 I/O APIC 管理，SMP 中断路由，IPI 发送

#### 3.7.4 Page Fault - 高级缺页处理

**源文件**: `SYSTEM/lib/Ztknl/page_fault.c` (23KB)
**头文件**: `include/page_fault.h`

**核心特性**:
- **COW 处理**: 写时复制页面
- **按需分页**: 惰性页面分配
- **栈扩展**: 用户栈自动增长
- **访问权限验证**: SMAP 支持下的用户指针验证

### 3.8 系统调用

**源文件**: `SYSTEM/lib/Ztknl/syscall.c` (36KB) + `syscall_fast.S`
**头文件**: `include/syscall.h`

#### 3.8.1 ABI 规范

```
中断方式: int 0x80
参数传递:
    RAX  = 系统调用号
    RDI  = 参数 1
    RSI  = 参数 2
    R10  = 参数 3
    R8   = 参数 4
    R9   = 参数 5
返回值:
    RAX  = 返回值
```

#### 3.8.2 系统调用分类 (180+)

| 范围 | 类别 | 调用号 | 说明 |
|------|------|--------|------|
| 0-39 | POSIX 基础 | 0-39 | exit, read, write, open, close, fork, exec, mmap 等 |
| 40-49 | 用户事务 | 40-49 | tx_begin, tx_commit, tx_abort, tx_read, tx_write |
| 50-59 | 持久内存 | 50-59 | pmem_create, pmem_alloc, pmem_free, pmem_snapshot |
| 60-69 | (已废弃) | - | 旧 Capability 系统，已迁移到 170-179 |
| 70-79 | Z-Pipe 零拷贝 | 70-79 | zpipe_create, zpipe_read, zpipe_write, zpipe_mmap |
| 80-89 | 自愈内核 | 80-89 | 故障注册, 检查点, 恢复, 看门狗 |
| 90-99 | 协程 | 90-99 | co_create, co_yield, co_chan_send, co_chan_recv |
| 100-109 | 异构计算 | 100-109 | 设备注册, 任务提交, 内存分配 |
| 110-119 | 网络零拷贝 | 110-119 | 零拷贝发送/接收, 批量传输 |
| 120-129 | 实时调度 | 120-129 | 实时任务创建, 参数设置, 优先级继承 |
| 130-139 | 性能分析 | 130-139 | 采样, 报告, 火焰图 |
| 140-149 | 资源限制 | 140-149 | rlimit, priority, scheduler |
| 150-159 | 内核测试 | 150-159 | 测试运行, 基准测试, 压力测试 |
| 160-169 | PEXLF IPC | 160-169 | 进程间通信, 通道管理 |
| 170-179 | Capability 安全 | 170-179 | 权限委托/撤销, 能力检查 |

**详细系统调用列表**:

```c
// POSIX 基础 (0-39)
SYS_EXIT(0), SYS_READ(1), SYS_WRITE(2), SYS_OPEN(3), SYS_CLOSE(4)
SYS_SEEK(5), SYS_STAT(6), SYS_EXEC(7), SYS_YIELD(8), SYS_SLEEP(9)
SYS_GETPID(10), SYS_MMAP(11), SYS_MUNMAP(12), SYS_BRK(13)
SYS_GETCWD(14), SYS_CHDIR(15), SYS_MKDIR(16), SYS_RMDIR(17)
SYS_UNLINK(18), SYS_OPENDIR(19), SYS_READDIR(20), SYS_CLOSEDIR(21)
SYS_FORK(22), SYS_WAIT(23), SYS_PIPE(24), SYS_DUP(25), SYS_DUP2(26)
SYS_EXECVE(27), SYS_EXECVEAT(28)
SYS_SOCKET(29), SYS_BIND(30), SYS_LISTEN(31), SYS_ACCEPT(32)
SYS_CONNECT(33), SYS_SEND(34), SYS_RECV(35), SYS_SHUTDOWN(36)
SYS_GETSOCKOPT(37), SYS_SETSOCKOPT(38), SYS_GETSOCKNAME(39)

// 用户态事务 (40-49)
SYS_TX_BEGIN(40), SYS_TX_COMMIT(41), SYS_TX_ABORT(42)
SYS_TX_WRITE(43), SYS_TX_READ(44), SYS_TX_SNAPSHOT(45)
SYS_TX_STATUS(46), SYS_TX_ROLLBACK(47), SYS_TX_LOCK(48), SYS_TX_UNLOCK(49)

// 持久内存 (50-59)
SYS_PMEM_CREATE(50), SYS_PMEM_OPEN(51), SYS_PMEM_CLOSE(52)
SYS_PMEM_ALLOC(53), SYS_PMEM_FREE(54), SYS_PMEM_SNAPSHOT(55)
SYS_PMEM_RESTORE(56), SYS_PMEM_SYNC(57), SYS_PMEM_STAT(58), SYS_PMEM_LIST(59)

// Z-Pipe 零拷贝 (70-79)
SYS_ZPIPE_CREATE(70), SYS_ZPIPE_DESTROY(71), SYS_ZPIPE_READ(72)
SYS_ZPIPE_WRITE(73), SYS_ZPIPE_SG_READ(74), SYS_ZPIPE_SG_WRITE(75)
SYS_ZPIPE_MMAP(76), SYS_ZPIPE_STAT(77), SYS_ZPIPE_BACKPRESS(78), SYS_ZPIPE_FLUSH(79)

// 自愈内核 (80-89)
SYS_SH_REGISTER(80), SYS_SH_UNREGISTER(81), SYS_SH_CHECKPOINT(82)
SYS_SH_RESTORE(83), SYS_SH_HEARTBEAT(84), SYS_SH_WATCHDOG(85)
SYS_SH_ISOLATE(86), SYS_SH_RECOVER(87), SYS_SH_STATUS(88), SYS_SH_STAT(89)

// 协程 (90-99)
SYS_CO_CREATE(90), SYS_CO_DESTROY(91), SYS_CO_YIELD(92), SYS_CO_RESUME(93)
SYS_CO_JOIN(94), SYS_CO_CHAN_CREATE(95), SYS_CO_CHAN_SEND(96), SYS_CO_CHAN_RECV(97)
SYS_CO_MUTEX_LOCK(98), SYS_CO_MUTEX_UNLOCK(99)

// 异构计算 (100-109)
SYS_HET_INIT(100), SYS_HET_REG_DEV(101), SYS_HET_UNREG_DEV(102)
SYS_HET_ALLOC_MEM(103), SYS_HET_FREE_MEM(104), SYS_HET_SUBMIT(105)
SYS_HET_WAIT(106), SYS_HET_SELECT(107), SYS_HET_QUERY(108), SYS_HET_STAT(109)

// 网络零拷贝 (110-119)
SYS_NZC_ENABLE(110), SYS_NZC_DISABLE(111), SYS_NZC_SEND(112), SYS_NZC_RECV(113)
SYS_NZC_SENDMSG(114), SYS_NZC_RECVMSG(115), SYS_NZC_SENDMMSG(116), SYS_NZC_RECVMMSG(117)
SYS_NZC_REG_REGION(118), SYS_NZC_UNREG_REGION(119)

// 实时调度 (120-129)
SYS_RT_CREATE(120), SYS_RT_DESTROY(121), SYS_RT_SET_PARAM(122), SYS_RT_GET_PARAM(123)
SYS_RT_SET_AFFINITY(124), SYS_RT_YIELD(125), SYS_RT_DEADLINE(126), SYS_RT_PERIODIC(127)
SYS_RT_STAT(128), SYS_RT_PI_BEGIN(129)

// 性能分析 (130-139)
SYS_PA_INIT(130), SYS_PA_START(131), SYS_PA_STOP(132), SYS_PA_SAMPLE(133)
SYS_PA_GET_REPORT(134), SYS_PA_REG_METRIC(135), SYS_PA_UPDATE(136)
SYS_PA_GET_SUBSYS(137), SYS_PA_FLAMEGRAPH(138), SYS_PA_RESET(139)

// 资源限制 (140-149)
SYS_GETRLIMIT(140), SYS_SETRLIMIT(141), SYS_PRLIMIT(142), SYS_GETRUSAGE(143)
SYS_SETPRIORITY(144), SYS_GETPRIORITY(145)
SYS_SCHED_SETSCHEDULER(146), SYS_SCHED_GETSCHEDULER(147)
SYS_SCHED_SETPARAM(148), SYS_SCHED_GETPARAM(149)

// 内核测试 (150-159)
SYS_KTEST_RUN(150), SYS_KTEST_LIST(151), SYS_KTEST_RESULT(152)
SYS_KTEST_BENCH(153), SYS_KTEST_STRESS(154)

// PEXLF IPC (160-169)
SYS_PEXLF_INIT(160), SYS_PEXLF_CREATE(161), SYS_PEXLF_JOIN(162), SYS_PEXLF_LEAVE(163)
SYS_PEXLF_SEND(164), SYS_PEXLF_RECV(165), SYS_PEXLF_BROADCAST(166)
SYS_PEXLF_LIST(167), SYS_PEXLF_PEER_INFO(168)

// Capability 安全 (170-179)
SYS_CAP_ALLOC(170), SYS_CAP_REVOKE(171), SYS_CAP_CHECK(172)
SYS_CAP_LIST(173), SYS_CAP_DELEGATE(174)
```

### 3.9 安全机制

#### 3.9.1 Capability 能力安全模型

**源文件**: `SYSTEM/lib/Ztknl/capability.c` (17KB)
**头文件**: `include/capability.h` (9KB)

**核心概念**: Capability 是一种不可伪造的权限令牌，持有者可以执行特定操作。

**系统调用**:
- `SYS_CAP_ALLOC(170)`: 分配新的 capability
- `SYS_CAP_REVOKE(171)`: 撤销已有 capability
- `SYS_CAP_CHECK(172)`: 检查当前进程是否具有特定 capability
- `SYS_CAP_LIST(173)`: 列出进程拥有的所有 capabilities
- `SYS_CAP_DELEGATE(174)`: 将 capability 委托给其他进程

#### 3.9.2 栈保护

**源文件**: `SYSTEM/lib/Ztknl/stack_protection.c` (4KB)
**头文件**: `include/stack_protection.h`

**特性**: 栈溢出检测、栈 canary 值、边界检查

#### 3.9.3 Selfheal 自愈框架

**源文件**: `SYSTEM/lib/Ztknl/selfheal.c` (25KB)
**头文件**: `include/selfheal.h`

**核心特性**:
- **故障处理器注册**: 注册特定故障类型的处理回调
- **检查点/恢复**: 创建系统状态快照，故障时恢复
- **心跳检测**: 定期检查关键组件运行状态
- **看门狗**: 监控进程超时
- **故障域隔离**: 隔离故障组件防止级联失败

#### 3.9.4 SMEP/SMAP 支持

**源文件**: `SYSTEM/lib/Ztknl/cpu.c` (6KB)
**头文件**: `include/cpu.h`

- **SMEP** (Supervisor Mode Execution Prevention): 阻止内核态执行用户空间代码
- **SMAP** (Supervisor Mode Access Prevention): 阻止内核态访问用户空间数据 (除非显式允许)

### 3.10 高级特性

#### 3.10.1 Coroutine - 原生协程

**源文件**: `SYSTEM/lib/Ztknl/coroutine.c` (27KB)
**头文件**: `include/coroutine.h`

**特性**: 内核级协程支持，Go 风格的 channel 通信，协程调度器

#### 3.10.2 Heterog - 异构计算调度

**源文件**: `SYSTEM/lib/Ztknl/heterog.c` (24KB)
**头文件**: `include/heterog.h`

**特性**: 管理 GPU/FPGA 等异构设备，任务提交和结果回收

#### 3.10.3 OPU - 开放处理单元

**源文件**: `SYSTEM/lib/Ztknl/opu.c` (6KB)
**头文件**: `include/opu.h`

**特性**: 提供可编程的内核扩展点

#### 3.10.4 Hotupdate - 内核热更新

**源文件**: `SYSTEM/lib/Ztknl/hotupdate.c` (33KB)
**头文件**: `include/hotupdate.h`

**特性**: 不停机更新内核代码，函数替换，状态迁移

#### 3.10.5 VMX/EPT - Intel 虚拟化

**源文件**: `SYSTEM/lib/Ztknl/vmx.c`, `ept.c`
**头文件**: `include/vmx.h`, `include/ept.h`

**特性**: Intel VT-x 虚拟化支持，扩展页表 (EPT)

#### 3.10.6 PMEM - 持久内存管理

**源文件**: `SYSTEM/lib/Ztknl/pmem.c` (26KB)
**头文件**: `include/pmem.h`

**特性**: 非易失性内存管理，持久化段分配，快照

#### 3.10.7 ZPipe - 零拷贝管道

**源文件**: `SYSTEM/lib/Ztknl/zpipe.c` (20KB)
**头文件**: `include/zpipe.h`

**特性**: 零拷贝 IPC 管道，Scatter-Gather I/O，背压控制

#### 3.10.8 Realtime - 硬实时调度

**源文件**: `SYSTEM/lib/Ztknl/realtime.c` (18KB)
**头文件**: `include/realtime.h`

**特性**: 硬实时任务调度，截止时间保证，优先级继承

#### 3.10.9 Perf - 性能分析

**源文件**: `SYSTEM/lib/Ztknl/perf.c` (36KB) + `perf_analysis.c` (17KB)
**头文件**: `include/perf.h`, `include/perf_analysis.h`

**特性**: 采样性能分析，指标注册，火焰图生成

#### 3.10.10 RCU - 读写复制同步

**源文件**: `SYSTEM/lib/Ztknl/rcu.c` (31KB)
**头文件**: `include/rcu.h`

**特性**: Read-Copy-Update 同步机制，无锁读取，延迟回收

#### 3.10.11 ZTS7 - 动态算法系统

**源文件**: `SYSTEM/lib/Ztknl/zts_algorithm.c` (15KB)
**头文件**: `include/zts_algorithm.h`
**示例代码**: `examples/example_algorithm.zts.c`, `examples/advanced_examples.zts.c`

**核心特性**:
- **运行时动态加载**: 支持在运行时加载 `.zts` 格式的算法模块
- **三种运行模式**:
  - **API 模式**: 通过标准 API 表与内核交互 (alloc/free/log/VFS/network)
  - **Patch 模式**: 直接替换内核函数代码，支持恢复原始代码
  - **Inject 模式**: 注入代码到内核地址空间执行
- **模块验证机制**: 魔数校验 (ZTS_MAGIC)、版本检查、校验和验证
- **优先级排序**: 从文件名解析优先级 (如 `0x00000000.zts`)，按优先级加载
- **类别系统**: 支持 FILESYSTEM/NETWORK/CRYPTO/SCHEDULER/DRIVER/PATCH/INJECT/CUSTOM 等类别

**API 表结构**:
```c
struct zts_api_table {
    uint32_t api_version;
    void *(*alloc)(size_t size);
    void (*free)(void *ptr);
    void *(*realloc)(void *ptr, size_t size);
    void (*log_print)(const char *level, const char *fmt, ...);
    uint64_t (*time_now)(void);
    int (*strcmp)(const char *s1, const char *s2);
    void *(*memcpy)(void *dst, const void *src, size_t n);
    void *(*memset)(void *s, int c, size_t n);
    /* VFS 和网络操作 (可选) */
};
```

**内核自更新支持**:
ZTS7 系统支持内核算法的运行时热替换，无需重启内核即可更新算法逻辑。结合 Hotupdate 模块，实现完整的内核自更新能力。

### 3.11 核心基础设施

| 模块 | 源文件 | 大小 | 功能 |
|------|--------|------|------|
| **GDT** | `gdt.c` + `gdt_asm.S` | 14KB | 全局描述符表，TSS 设置 |
| **CPU** | `cpu.c` | 6KB | CPU 初始化，特性检测，SMEP/SMAP |
| **SMP** | `smp.c` + `smp_asm.S` | 44KB | 多核引导，AP 启动，IPI |
| **Console** | `console.c` | 45KB | 控制台输出，格式化打印 |
| **String** | `string.c` | 11KB | 内核字符串操作 |
| **PCI** | `pci.c` (通过 input.c 引用) | - | PCI 总线枚举，设备发现 |
| **TTY** | `tty.c` | 17KB | TTY 子系统，行规程 |
| **IPC** | `ipc.c` | 50KB | 进程间通信 |
| **Signal** | `signal.c` | 58KB | POSIX 信号处理 |
| **Module** | `module.c` | 16KB | 内核模块管理 |
| **Input** | `input.c` | 22KB | 输入子系统 |
| **GUI** | `gui.c` (通过 image.c 引用) | - | GUI 子系统 |
| **NTDLL** | `ntdll.c` | 109KB | Windows NT 兼容层 |
| **UMDF** | `umdf.c` + `umdf.h` | 17KB | 用户模式驱动框架 |

---

## 4. 用户空间程序

### 4.1 Init (PID 1)

**位置**: `SYSTEM/lib/init/`
**输出**: `SYSTEM/prgm/sys/init` (约 18KB)

#### 4.1.1 文件结构

| 文件 | 说明 |
|------|------|
| `init.c` | PID 1 进程，系统初始化入口 |
| `crt0.S` | C 运行时启动代码 (设置栈，调用 main) |
| `libc.c` | 标准 C 库实现 (内核系统调用的包装) |
| `user.ld` | 用户空间链接脚本 |
| `Makefile` | 编译脚本 |

#### 4.1.2 功能

- 初始化系统环境
- 按顺序启动窗口管理器、桌面环境、文件管理器
- 监控子进程状态，崩溃自动重启
- 作为孤儿进程的父进程 (reaper)

#### 4.1.3 启动流程

```
init (PID 1)
  ├─ fork+exec → /system/zrdemage (窗口管理器)
  ├─ fork+exec → /prgm/sys/desk   (桌面环境)
  ├─ fork+exec → /prgm/sys/fm     (文件管理器)
  └─ 监控循环: wait() + 自动重启
```

#### 4.1.4 用户空间链接脚本 (user.ld)

```ld
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x400000;          /* 用户空间加载地址 4MB */

    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
    }
}
```

### 4.2 Shell (zsh)

**位置**: `SYSTEM/lib/shell/`
**输出**: `SYSTEM/prgm/sys/shell` (约 25KB)

#### 4.2.1 文件结构

| 文件 | 说明 |
|------|------|
| `main.c` | Shell 主程序，REPL 循环 |
| `zsh.h` | 完整头文件定义 |
| `commands.c` | 内置命令实现 |
| `parser.c` | 命令解析器 |
| `history.c` | 命令历史 (32 条) |
| `io.c` | I/O 操作 |
| `syscall.c` | 系统调用包装函数 |
| `termios.c` | 终端控制 |
| `string.c` | 字符串工具函数 |
| `crt0.s` | 启动代码 |

#### 4.2.2 内置命令

| 命令 | 功能 |
|------|------|
| `cd` | 切换目录 |
| `pwd` | 显示当前目录 |
| `ls` | 列出目录内容 |
| `echo` | 输出文本 |
| `clear` | 清屏 |
| `help` | 显示帮助 |
| `exit` | 退出 shell |

#### 4.2.3 高级特性

- **管道支持**: `cmd1 | cmd2` 通过 pipe 系统调用实现
- **重定向**: `>` (覆盖), `>>` (追加), `<` (输入重定向)
- **命令历史**: 最多 32 条历史记录
- **外部程序执行**: 通过 fork + exec 执行外部程序

### 4.3 Window Manager (zrdemage)

**位置**: `SYSTEM/lib/wm/`
**输出**: `SYSTEM/system/zrdemage`

#### 4.3.1 文件结构

| 文件 | 说明 |
|------|------|
| `wm.c` | 窗口管理器主程序 |
| `syscall_asm.S` | 系统调用汇编包装 |
| `wm.ld` | 链接脚本 |
| `Makefile` | 编译脚本 |

#### 4.3.2 功能

- **Win11 + KDE Plasma 融合风格**窗口装饰
- 窗口管理: 创建、移动、缩放、最小化、最大化、关闭
- 窗口合成: Mica/Acrylic 半透明效果、圆角、阴影
- 焦点管理、Z-order 层级合成
- 鼠标/键盘事件分发
- Snap Layouts (Win11 风格窗口布局)

### 4.4 Desktop (desk)

**位置**: `SYSTEM/lib/desk/`
**输出**: `SYSTEM/prgm/sys/desk`

#### 4.4.1 文件结构

| 文件 | 说明 |
|------|------|
| `desk.c` | 桌面环境主程序 |
| `syscall_asm.S` | 系统调用汇编包装 |
| `desk.ld` | 链接脚本 |
| `Makefile` | 编译脚本 |

#### 4.4.2 功能

- **Mica 桌面背景**渲染
- 桌面图标 (Terminal, Files, Monitor, Settings)
- 右键上下文菜单
- 任务栏 (KDE Plasma 风格，开始按钮 + 窗口列表 + 时钟)
- **开机 LOGO**: 屏幕显示大字 "ZT5" (带发光渐变效果)
- 60fps 渲染循环

#### 4.4.3 渲染顺序

1. 清屏
2. Mica 渐变背景
3. 桌面图标
4. 任务栏
5. 开始菜单 / 右键菜单
6. 通知
7. 鼠标光标

### 4.5 File Manager (fm)

**位置**: `SYSTEM/lib/fm/`
**输出**: `SYSTEM/prgm/sys/fm`

#### 4.5.1 文件结构

| 文件 | 说明 |
|------|------|
| `fm.c` | 文件管理器主程序 |
| `syscall_asm.S` | 系统调用汇编包装 |
| `fm.ld` | 链接脚本 |
| `Makefile` | 编译脚本 |

#### 4.5.2 功能

- **Win11 + KDE Dolphin 融合风格**
- 正确显示文件信息 (大小、权限、修改时间)
- 导航栏、面包屑路径
- 侧边栏快速访问
- 文件/文件夹图标
- 列表/网格视图切换
- 使用 `opendir/readdir/stat` 系统调用

### 4.6 ZTS7 Algorithm

**位置**: `SYSTEM/lib/zts7/`

| 文件 | 说明 |
|------|------|
| `0x00000000.zts.c` | ZTS7 动态算法实现 |

ZTS7 是 ZratroOS 的动态算法系统，支持运行时加载和执行算法。

### 4.7 Rust Kernel (监控内核)

**位置**: `SYSTEM/lib/rust_kernel/`
**开发状态**: 活跃开发阶段 (完成度 ~40%)

#### 4.5.1 文件结构

| 文件 | 说明 |
|------|------|
| `src/lib.rs` | Rust 监控内核入口 |
| `src/vga.rs` | VGA 文本模式输出 (80x25) |
| `src/serial.rs` | 串口输出 (COM1, 115200bps) |
| `src/gdt.rs` | GDT 实现 |
| `src/idt.rs` | IDT 实现 |
| `src/watchdog.rs` | 看门狗监控 |
| `src/shared_mem.rs` | 共享内存通信 |
| `Cargo.toml` | Rust 包配置 |

#### 4.5.2 功能

**已实现功能**:
- 监控主内核运行状态
- 串口/VGA 双路输出 (KernelState 统一管理)
- GDT 实现 (5 个段描述符，含代码段/数据段/TSS)
- IDT 实现 (256 个中断门，4 个异常处理: #DE, #UD, #GP, #PF)
- 看门狗监控 (非阻塞 run_once 模式，tick 计数)
- 通过共享内存与主内核通信 (校验和验证)
- 主内核/辅助内核健康状态监控
- 异常处理与恢复机制

**未实现功能**:
- 动态内存管理 (独立堆分配器)
- 异常日志记录到 VFS
- 网络栈监控
- 进程调度监控
- 性能分析工具
- 独立于 Ztknl 的完整启动流程

#### 4.5.3 Cargo.toml 配置

```toml
[package]
name = "zratro-rustkernel"
version = "0.1.0"
edition = "2021"

[lib]
name = "rustknl"
crate-type = ["staticlib"]
path = "src/lib.rs"

[profile.dev]
panic = "abort"

[profile.release]
panic = "abort"
opt-level = "z"
lto = true
```

#### 4.5.4 编译方法

```bash
cd SYSTEM/lib/rust_kernel
cargo build --lib --release
```

编译产物位于 `target/x86_64-unknown-none/release/libzratros_kernel.a`

---

## 5. 构建系统

### 5.1 内核编译

**Makefile 位置**: `SYSTEM/lib/Ztknl/Makefile`

#### 5.1.1 编译配置

```makefile
# 编译器
CC = gcc
LD = ld

# 基础编译选项
BASE_CFLAGS = -ffreestanding -fno-pic -mno-red-zone \
              -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel \
              -Wall -Wextra -O2 -pipe -I include

# 安全加固选项
SECURITY_CFLAGS = -fstack-protector-strong \
                  -D_FORTIFY_SOURCE=2 \
                  -fno-delete-null-pointer-checks \
                  -fno-strict-overflow

# 完整编译选项
CFLAGS = $(BASE_CFLAGS) $(SECURITY_CFLAGS) -Wno-error

# 链接选项
LDFLAGS = -static -nostdlib -T linker.ld
```

#### 5.1.2 编译选项说明

| 选项 | 说明 |
|------|------|
| `-ffreestanding` | 独立环境编译，不依赖标准库 |
| `-fno-pic` | 禁用位置无关代码 |
| `-mno-red-zone` | 禁用红区 (内核需要) |
| `-mno-mmx/sse/sse2` | 禁用 SIMD 指令 (内核态不推荐) |
| `-mcmodel=kernel` | 内核代码模型 |
| `-fstack-protector-strong` | 强栈保护 |
| `-D_FORTIFY_SOURCE=2` | 运行时缓冲区检查 |
| `-O2` | 优化级别 2 |

#### 5.1.3 编译命令

```bash
# 编译内核
cd SYSTEM/lib/Ztknl
make clean
make -j$(nproc)

# 输出: SYSTEM/system/knl.bin (~850KB)
```

#### 5.1.4 源码构成

内核编译包含约 77 个 C 源文件:

```
SOURCES_C = $(filter-out deadlock_free.c epoll.c futex.c, $(wildcard *.c))
```

排除的模块:
- `deadlock_free.c` - 实验性死锁检测 (未启用)
- `epoll.c` - epoll 实现 (未启用)
- `futex.c` - futex 实现 (未启用)

#### 5.1.5 内核链接脚本 (linker.ld)

```ld
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    limine_reqs PT_LOAD;
    text        PT_LOAD;
    rodata      PT_LOAD;
    data        PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;    /* Higher Half 起始地址 */

    /* Limine 请求段 - 必须对齐到 8 字节 */
    .limine_reqs ALIGN(8) : {
        KEEP(*(.limine_reqs_start))
        KEEP(*(.limine_reqs))
        KEEP(*(.limine_reqs_end))
    } :limine_reqs

    . = ALIGN(4096);
    .text : { *(.text .text.*) } :text

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) } :rodata

    . = ALIGN(4096);
    .data : { *(.data .data.*) } :data

    . = ALIGN(4096);
    .bss : { *(COMMON) *(.bss .bss.*) } :data

    . = ALIGN(4096);
    /DISCARD/ : { *(.eh_frame) *(.comment) }
}
```

### 5.2 用户程序编译

#### 5.2.1 Init 程序

```bash
cd SYSTEM/lib/init
make clean
make -j$(nproc)
# 输出: SYSTEM/prgm/sys/init
```

#### 5.2.2 Shell 程序

```bash
cd SYSTEM/lib/shell
make clean
make -j$(nproc)
# 输出: SYSTEM/prgm/sys/shell
```

#### 5.2.3 Window Manager (zrdemage)

```bash
cd SYSTEM/lib/wm
make clean
make -j$(nproc)
# 输出: SYSTEM/system/zrdemage
```

#### 5.2.4 Desktop 程序

```bash
cd SYSTEM/lib/desk
make clean
make -j$(nproc)
# 输出: SYSTEM/prgm/sys/desk
```

#### 5.2.5 File Manager 程序

```bash
cd SYSTEM/lib/fm
make clean
make -j$(nproc)
# 输出: SYSTEM/prgm/sys/fm
```

#### 5.2.6 全量构建

```bash
./build_all.sh
# 一次性编译内核和所有用户程序
```

#### 5.2.7 用户程序编译标志

```makefile
CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -fno-builtin -nostdlib -nostdinc \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -mcmodel=small -m64 \
         -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
         -O2 -pipe \
         -mgeneral-regs-only -fno-tree-vectorize
```

| 选项 | 说明 |
|------|------|
| `-ffreestanding` | 独立环境编译，不依赖标准库 |
| `-nostdlib` | 不使用标准库 |
| `-mcmodel=small` | 小代码模型 (用户空间) |
| `-mgeneral-regs-only` | 仅使用通用寄存器 |
| `-fno-tree-vectorize` | 禁用自动向量化 |

### 5.3 Rust 内核编译

```bash
cd SYSTEM/lib/rust_kernel
cargo build --lib --release
```

### 5.4 ISO/镜像打包

**脚本**: `ISO/build_iso.sh`

#### 5.4.1 打包原理

1. 创建 raw 镜像 (FAT32 格式)
2. 创建 GPT 分区表 (UEFI 引导需要)
3. 格式化为 FAT32
4. 使用 Python pyfatfs 或 loop device 写入文件
5. 生成 ISO (用于 QEMU CD-ROM 测试)

#### 5.4.2 使用方法

```bash
cd ISO

# 默认打包
./build_iso.sh

# 仅清理临时文件
./build_iso.sh --clean

# 仅验证配置
./build_iso.sh --verify

# 保留 staging 目录
./build_iso.sh --keep
```

#### 5.4.3 打包流程详解

```
1. 验证必需文件
   ├── boot/limine.conf
   ├── system/knl.bin
   ├── prgm/sys/init
   ├── prgm/sys/shell
   └── EFI/BOOT/BOOTX64.EFI

2. 准备 staging 目录
   └── 复制 SYSTEM/ 下除 lib/ 外的所有目录

3. 创建 FAT32 镜像 (128MB+)
   ├── dd 创建空镜像
   ├── sfdisk 创建 GPT 分区表
   └── mkfs.vfat 格式化为 FAT32

4. 写入文件 (两种方案)
   ├── 方案A: sudo + loop device (需要 root)
   └── 方案B: Python pyfatfs (不需要 root)

5. 生成 ISO
   └── xorriso 生成 ISO 镜像
```

#### 5.4.4 输出文件

| 文件 | 大小 | 用途 |
|------|------|------|
| `zratroos.img` | ~34MB | FAT32 镜像，UEFI 引导 |
| `zratroos.iso` | ~2MB | ISO 镜像，CD-ROM 引导 |

---

## 6. 开发环境搭建

### 6.1 系统要求

| 组件 | 最低要求 | 推荐配置 |
|------|---------|---------|
| **操作系统** | Linux (Arch Linux 推荐) | Arch Linux / Ubuntu |
| **CPU** | x86_64, 2 核心 | x86_64, 4+ 核心 |
| **内存** | 4GB | 8GB+ |
| **磁盘** | 10GB | 20GB+ |

### 6.2 工具链安装

#### 6.2.1 必需工具

```bash
# Arch Linux
sudo pacman -S gcc make qemu-system-x86 xorriso python pyfatfs limine

# Ubuntu/Debian
sudo apt install gcc make qemu-system-x86 xorriso python3-pip limine-bin
pip3 install pyfatfs
```

#### 6.2.2 可选工具

```bash
# 交叉编译工具链 (可选)
sudo pacman -S x86_64-elf-gcc x86_64-elf-binutils x86_64-elf-gdb

# Rust 工具链 (用于监控内核)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
rustup target add x86_64-unknown-none
```

#### 6.2.3 版本要求

| 工具 | 最低版本 | 检查命令 |
|------|---------|---------|
| GCC | 11.0+ | `gcc --version` |
| GNU LD | 2.35+ | `ld --version` |
| QEMU | 6.0+ | `qemu-system-x86_64 --version` |
| xorriso | 1.5+ | `xorriso --version` |
| Python | 3.8+ | `python3 --version` |
| Rust (可选) | 1.70+ | `rustc --version` |

### 6.3 克隆项目

```bash
git clone <repository-url> Deaicup-ZratroOS
cd Deaicup-ZratroOS
```

### 6.4 首次编译

```bash
# 1. 编译内核
cd SYSTEM/lib/Ztknl
make clean && make -j$(nproc)

# 2. 编译用户程序
cd ../init && make clean && make
cd ../shell && make clean && make
cd ../desk && make clean && make

# 3. (可选) 编译 Rust 监控内核
cd ../rust_kernel
cargo build --lib --release

# 4. 打包镜像
cd ../../../ISO
./build_iso.sh
```

---

## 7. 测试与调试

### 7.1 QEMU 测试

#### 7.1.1 UEFI 镜像测试

```bash
# 基本测试 (2 CPU, 2GB 内存)
qemu-system-x86_64 \
    -drive file=ISO/zratroos.img,format=raw \
    -m 2G \
    -smp 2 \
    -serial stdio

# 完整测试 (GTK 显示, 4 CPU, 4GB 内存)
qemu-system-x86_64 \
    -drive file=ISO/zratroos.img,format=raw \
    -m 4G \
    -smp 4 \
    -serial stdio \
    -display gtk,gl=off
```

#### 7.1.2 ISO 测试

```bash
# CD-ROM 引导
qemu-system-x86_64 \
    -cdrom ISO/zratroos.iso \
    -boot d \
    -m 2G \
    -smp 2 \
    -serial stdio
```

#### 7.1.3 QEMU 调试选项

```bash
# GDB 远程调试
qemu-system-x86_64 \
    -drive file=ISO/zratroos.img,format=raw \
    -m 2G \
    -smp 2 \
    -serial stdio \
    -s -S    # -s: 监听 1234 端口, -S: 启动时暂停

# 在另一个终端连接 GDB
gdb SYSTEM/system/knl.bin
(gdb) target remote localhost:1234
(gdb) break main
(gdb) continue
```

#### 7.1.4 网络测试

```bash
# 使用 E1000 网卡
qemu-system-x86_64 \
    -drive file=ISO/zratroos.img,format=raw \
    -m 2G \
    -smp 2 \
    -serial stdio \
    -netdev user,id=net0 \
    -device e1000,netdev=net0
```

### 7.2 内核自检

内核内置的自检模块:

| 模块 | 源文件 | 功能 |
|------|--------|------|
| **kselfcheck** | `kselfcheck.c` | 内核自检，验证核心功能 |
| **ktest** | `ktest.c` | 内核测试框架 |
| **boot_diag** | `boot_diag.c` | 引导诊断 |

### 7.3 串口调试

```bash
# QEMU 串口输出到文件
qemu-system-x86_64 \
    -drive file=ISO/zratroos.img,format=raw \
    -m 2G \
    -serial file:serial.log
```

### 7.4 真实硬件测试

```bash
# 写入 U 盘 (替换 /dev/sdX 为实际设备)
sudo dd if=ISO/zratroos.img of=/dev/sdX bs=4M conv=fsync status=progress

# 或使用 ISO
sudo dd if=ISO/zratroos.iso of=/dev/sdX bs=4M conv=fsync status=progress
```

### 7.5 常见问题排查

| 问题 | 可能原因 | 解决方法 |
|------|---------|---------|
| QEMU 黑屏 | 引导配置错误 | 检查 limine.conf 路径 |
| 内核崩溃 | 内存管理 bug | 使用 GDB 调试，检查 pmm/vmm |
| 文件系统挂载失败 | 磁盘驱动问题 | 检查 AHCI/NVMe 初始化日志 |
| 网络不通 | 网卡驱动问题 | 检查 E1000 初始化，确认 QEMU 网卡模型 |
| 用户程序不启动 | init 问题 | 检查 ELF 加载和 init 二进制 |

---

## 8. 代码规范

### 8.1 C 语言编码规范

#### 8.1.1 命名约定

```c
// 函数名: 小写蛇形命名
void pmm_init(void);
uint64_t vmm_map(uint64_t phys, uint64_t size);

// 结构体: 小写蛇形命名 + _t 后缀 (可选)
struct task_struct;
struct memory_space;

// 宏定义: 大写蛇形命名
#define MAX_TASKS 256
#define PAGE_SIZE 4096

// 全局变量: g_ 前缀
struct task_struct *g_idle_task;

// 静态变量: s_ 前缀 (可选)
static int s_initialized = 0;
```

#### 8.1.2 文件头注释

```c
/**
 * @file pmm.c
 * @brief 物理内存管理器 (Physical Memory Manager)
 * @author ZratroOS Team
 * @date 2026
 *
 * 功能:
 * - 位图物理内存分配器
 * - Per-CPU 页缓存
 * - 内存区域管理
 * - 内存压缩
 * - KSM 页合并
 */
```

#### 8.1.3 函数注释

```c
/**
 * @brief 分配物理页
 * @param flags 分配标志 (PMM_ZERO 等)
 * @return 物理页地址，失败返回 NULL
 *
 * @note 此函数可能阻塞，不能在中断上下文中调用
 */
void *pmm_alloc_page(uint64_t flags);
```

### 8.2 内核编程注意事项

#### 8.2.1 禁止使用标准库

```c
// 错误: 不能使用标准库
#include <stdio.h>   // 不可用
#include <stdlib.h>  // 不可用
#include <string.h>  // 不可用

// 正确: 使用内核自带实现
#include "console.h"  // printk 替代 printf
#include "string.h"   // 内核字符串函数
#include "heap.h"     // kmalloc/kfree 替代 malloc/free
```

#### 8.2.2 中断上下文注意事项

```c
// 在中断处理程序中:
// 1. 不能睡眠/阻塞
// 2. 不能使用可能调度的函数
// 3. 尽快完成处理

void isr_handler(struct interrupt_frame *frame) {
    // 快速处理
    // ...

    // 如果需要长时间处理，交给下半部
    schedule_softirq(process_interrupt);
}
```

#### 8.2.3 内存分配安全

```c
// 始终检查返回值
void *ptr = kmalloc(size);
if (!ptr) {
    printk(KERN_ERR "Failed to allocate %lu bytes\n", size);
    return -ENOMEM;
}

// 使用后及时释放
kfree(ptr);
ptr = NULL;
```

### 8.3 Rust 编码规范

#### 8.3.1 裸机 Rust 规范

```rust
// 禁止使用标准库
#![no_std]
#![no_main]

// 必须定义 panic handler
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

// 入口点
#[no_mangle]
pub extern "C" fn rustknl_main() {
    // ...
}
```

### 8.4 Git 提交规范

```
<type>: <description>

[optional body]

[optional footer]
```

**Type 说明**:

| Type | 说明 | 示例 |
|------|------|------|
| `feat` | 新功能 | `feat: 添加 NVMe 驱动支持` |
| `fix` | Bug 修复 | `fix: 修复 PMM 内存泄漏` |
| `docs` | 文档更新 | `docs: 添加开发文档` |
| `refactor` | 重构 | `refactor: 重构 VMM 页表管理` |
| `perf` | 性能优化 | `perf: 优化调度器负载均衡` |
| `test` | 测试相关 | `test: 添加内核自检测试` |

---

## 9. 模块依赖关系

### 9.1 内核模块依赖图

```
main.c (入口)
├── boot_diag.c (引导验证)
│   └── console.c (输出)
│
├── gdt.c + gdt_asm.S (GDT)
│   └── cpu.c (CPU 特性)
│
├── idt.c + idt_asm.S (IDT)
│   ├── isr.c (中断服务)
│   │   ├── console.c (异常输出)
│   │   └── page_fault.c (缺页处理)
│   └── apic.c (APIC)
│       └── smp.c (SMP)
│           └── smp_asm.S (AP 启动代码)
│
├── pmm.c (物理内存)
│   └── console.c
│
├── vmm.c (虚拟内存)
│   ├── pmm.c
│   ├── mmap.c
│   └── cow.c
│
├── heap.c (堆)
│   ├── pmm.c
│   └── vmm.c
│
├── slab.c (SLAB)
│   ├── heap.c
│   └── pmm.c
│
├── task.c (任务管理)
│   ├── heap.c
│   ├── pmm.c
│   ├── vmm.c
│   ├── scheduler.c
│   ├── nsproxy.c (命名空间)
│   └── rlimit.c (资源限制)
│
├── scheduler.c (调度器)
│   ├── task.c
│   ├── cfs.c
│   ├── cgroup.c
│   └── realtime.c
│
├── vfs.c (虚拟文件系统)
│   ├── pmm.c
│   ├── heap.c
│   ├── disk.c
│   ├── fat32.c
│   ├── ext4.c
│   ├── ramfs.c
│   ├── tmpfs.c
│   └── procfs.c
│
├── disk.c (块设备)
│   ├── ahci.c
│   └── nvme.c
│
├── net.c (网络栈)
│   ├── e1000.c / wifi.c (驱动)
│   ├── tcp.c
│   ├── dhcp.c
│   └── socket.c
│
├── syscall.c (系统调用)
│   ├── task.c
│   ├── vfs.c
│   ├── net.c
│   ├── ipc.c
│   ├── signal.c
│   ├── selfheal.c
│   ├── coroutine.c
│   ├── pmem.c
│   ├── zpipe.c
│   ├── realtime.c
│   ├── perf.c
│   ├── capability.c
│   └── pexlf.c
│
├── elf.c (ELF 加载)
│   ├── task.c
│   ├── vmm.c
│   └── vfs.c
│
└── console.c (控制台输出)
    ├── string.c
    └── fb.c (帧缓冲，可选)
```

### 9.2 用户空间依赖

```
init
├── libc.c (C 库包装)
│   └── syscall (int 0x80)
└── shell / desk

shell
├── main.c
├── parser.c (命令解析)
├── commands.c (内置命令)
├── history.c (历史)
├── io.c
├── syscall.c
├── termios.c
└── string.c

desk
├── desk.c (集成桌面环境: WM+FM+Taskbar)
```

### 9.3 编译依赖关系

```
编译顺序 (推荐):
1. 内核 (Ztknl)
   └── 输出: knl.bin

2. 用户程序 (按顺序)
   ├── init (依赖内核系统调用 ABI)
   ├── shell (依赖 init 的 user.ld)
   └── desk

3. (可选) Rust 监控内核
   └── 输出: libzratros_kernel.a

4. 打包 (build_iso.sh)
   ├── knl.bin
   ├── init
   ├── shell
   ├── limine.conf
   └── BOOTX64.EFI
```

---

## 10. API 参考

### 10.1 内核 API

#### 10.1.1 内存管理 API

```c
// PMM - 物理内存
void pmm_init(void);
void *pmm_alloc_page(uint64_t flags);
void pmm_free_page(void *page);
uint64_t pmm_get_free_pages(void);
uint64_t pmm_get_used_pages(void);

// VMM - 虚拟内存
void vmm_init(void);
void *vmm_map(uint64_t phys_addr, uint64_t size, uint64_t flags);
void vmm_unmap(uint64_t virt_addr, uint64_t size);
void *vmm_map_user(uint64_t phys_addr, uint64_t size);
int vmm_copy_from_user(void *dst, const void *src, uint64_t size);
int vmm_copy_to_user(void *dst, const void *src, uint64_t size);

// Heap - 内核堆
void heap_init(void);
void *kmalloc(uint64_t size);
void *kzalloc(uint64_t size);    // 零初始化分配
void kfree(void *ptr);
void *krealloc(void *ptr, uint64_t new_size);

// SLAB - 对象缓存
struct slab_cache *slab_cache_create(const char *name, uint64_t size);
void *slab_alloc(struct slab_cache *cache);
void slab_free(struct slab_cache *cache, void *obj);
void slab_cache_destroy(struct slab_cache *cache);

// Mmap - 内存映射
int sys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t offset);
int sys_munmap(uint64_t addr, uint64_t len);
```

#### 10.1.2 进程管理 API

```c
// Task - 任务管理
struct task_struct *task_create(const char *name, void (*entry)(void *), void *arg);
void task_exit(int status);
struct task_struct *task_get(pid_t pid);
int task_kill(pid_t pid, int signal);

// Scheduler - 调度
void scheduler_init(void);
void scheduler_tick(void);
void scheduler_yield(void);
void scheduler_sleep(struct task_struct *task);
void scheduler_wake(struct task_struct *task);
```

#### 10.1.3 文件系统 API

```c
// VFS - 虚拟文件系统
int vfs_init(void);
int vfs_mount(const char *device, const char *mountpoint, const char *fstype);
int vfs_open(const char *path, int flags);
int vfs_read(int fd, void *buf, uint64_t count);
int vfs_write(int fd, const void *buf, uint64_t count);
int vfs_close(int fd);
int vfs_stat(const char *path, struct stat *st);

// 文件系统注册
int vfs_register_fs(struct filesystem *fs);
int vfs_unregister_fs(struct filesystem *fs);
```

#### 10.1.4 控制台输出 API

```c
// Console - 控制台输出
void console_init(void);
void printk(const char *fmt, ...);
void printk_color(int color, const char *fmt, ...);

// 日志级别
#define KERN_EMERG   "[EMERG] "
#define KERN_ALERT   "[ALERT] "
#define KERN_CRIT    "[CRIT]  "
#define KERN_ERR     "[ERROR] "
#define KERN_WARNING "[WARN]  "
#define KERN_NOTICE  "[NOTICE]"
#define KERN_INFO    "[INFO]  "
#define KERN_DEBUG   "[DEBUG] "
```

#### 10.1.5 字符串 API

```c
// String - 内核字符串操作
int strlen(const char *s);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, uint64_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, uint64_t n);
char *strcat(char *dst, const char *src);
char *strchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
void *memcpy(void *dst, const void *src, uint64_t n);
void *memmove(void *dst, const void *src, uint64_t n);
void *memset(void *s, int c, uint64_t n);
int memcmp(const void *s1, const void *s2, uint64_t n);
```

#### 10.1.6 同步原语 API

```c
// Spinlock
void spinlock_init(spinlock_t *lock);
void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);

// Mutex (通过 semaphore 实现)
void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);

// RCU
void rcu_read_lock(void);
void rcu_read_unlock(void);
void synchronize_rcu(void);
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head));
```

### 10.2 系统调用 API (用户空间)

```c
// 系统调用包装函数 (内联汇编)
uint64_t syscall0(uint64_t num);
uint64_t syscall1(uint64_t num, uint64_t arg0);
uint64_t syscall2(uint64_t num, uint64_t arg0, uint64_t arg1);
uint64_t syscall3(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2);
uint64_t syscall4(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t syscall5(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);

// 常用系统调用示例
#define SYS_EXIT    0
#define SYS_READ    1
#define SYS_WRITE   2
#define SYS_OPEN    3
#define SYS_CLOSE   4
#define SYS_FORK    22
#define SYS_EXEC    7
#define SYS_GETPID  10

// 使用示例:
// int fd = syscall1(SYS_OPEN, (uint64_t)"/etc/config", O_RDONLY);
// syscall2(SYS_WRITE, fd, (uint64_t)"hello", 5);
// syscall0(SYS_EXIT);
```

### 10.3 错误码定义

```c
// 标准错误码 (include/errno.h)
#define EPERM           1   // 操作不允许
#define ENOENT          2   // 文件或目录不存在
#define ESRCH           3   // 进程不存在
#define EINTR           4   // 中断的系统调用
#define EIO             5   // I/O 错误
#define ENXIO           6   // 设备不存在
#define E2BIG           7   // 参数列表过长
#define ENOMEM          12  // 内存不足
#define EACCES          13  // 权限拒绝
#define EFAULT          14  // 错误地址
#define EBUSY           16  // 设备忙
#define EEXIST          17  // 文件已存在
#define ENODEV          19  // 设备不存在
#define EINVAL          22  // 无效参数
#define ENOSPC          28  // 设备空间不足
#define EPIPE           32  // 管道断开
#define EAGAIN          35  // 资源暂时不可用
```

### 10.4 核心数据结构

#### 10.4.1 任务结构体 (task_struct)

```c
struct task_struct {
    pid_t pid;              // 进程 ID
    pid_t ppid;             // 父进程 ID
    pid_t pgid;             // 进程组 ID
    char name[64];          // 进程名称
    int state;              // 进程状态
    uint64_t flags;         // 进程标志
    uint64_t priority;      // 优先级

    struct task_struct *parent;     // 父进程
    struct list_head children;      // 子进程列表
    struct list_head sibling;       // 兄弟进程

    uint64_t *kstack;               // 内核栈
    struct memory_space *mm;        // 内存空间
    struct cpu_state *ctx;          // CPU 上下文

    int exit_code;                  // 退出码
    struct rlimit rlimit[RLIMIT_NLIMITS];  // 资源限制

    // 信号相关
    struct signal_struct *signal;
    sigset_t blocked;

    // 文件系统相关
    struct fs_struct *fs;

    // 调度相关
    uint64_t vruntime;      // CFS 虚拟运行时间
    int policy;             // 调度策略
    struct list_head run_node;
};
```

#### 10.4.2 内存空间结构体

```c
struct memory_space {
    uint64_t *pml4;         // PML4 页表
    struct rb_root vmas;    // VMA 红黑树
    uint64_t start_code;
    uint64_t end_code;
    uint64_t start_data;
    uint64_t end_data;
    uint64_t start_brk;
    uint64_t brk;
    uint64_t start_stack;
    uint64_t arg_start;
    uint64_t arg_end;
    uint64_t total_vm;
    uint64_t locked_vm;
};
```

#### 10.4.3 文件结构体

```c
struct file {
    int fd;                     // 文件描述符
    struct vnode *vnode;        // VFS 节点
    uint64_t offset;            // 当前偏移
    int flags;                  // 打开标志 (O_RDONLY, O_WRONLY, O_RDWR)
    struct fs_ops *fs_ops;      // 文件系统操作
    void *private_data;         // 私有数据
};

struct vnode {
    char name[256];             // 文件名
    uint64_t inode;             // inode 号
    uint64_t size;              // 文件大小
    int type;                   // 类型 (FILE, DIR, SYMLINK)
    struct filesystem *fs;      // 所属文件系统
    void *private_data;         // 文件系统私有数据
};
```

#### 10.4.4 中断帧结构体

```c
struct interrupt_frame {
    uint64_t rip;       // 指令指针
    uint64_t cs;        // 代码段
    uint64_t rflags;    // 标志寄存器
    uint64_t rsp;       // 栈指针
    uint64_t ss;        // 栈段
};

struct cpu_state {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    struct interrupt_frame frame;
};
```

### 10.5 Limine 引导协议

```c
// Limine 协议结构 (include/limine.h)
struct limine_module_request {
    struct limine_header header;
    struct limine_module_response *response;
};

struct limine_module_response {
    uint64_t module_count;
    struct limine_module **modules;
};

struct limine_memory_map_request {
    struct limine_header header;
    struct limine_memory_map_response *response;
};

struct limine_memory_map_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
    uint64_t unused;
};
```

---

## 附录

### A. 快速参考卡

#### A.1 常用编译命令

```bash
# 编译全部
cd SYSTEM/lib/Ztknl && make -j$(nproc) && cd ../../..
cd SYSTEM/lib/init && make && cd ../../..
cd SYSTEM/lib/shell && make && cd ../../..
cd SYSTEM/lib/desk && make && cd ../../..

# 打包
cd ISO && ./build_iso.sh && cd ..

# 测试
qemu-system-x86_64 -drive file=ISO/zratroos.img,format=raw -m 2G -smp 2 -serial stdio
```

#### A.2 文件位置速查

| 文件 | 路径 |
|------|------|
| 内核源码 | `SYSTEM/lib/Ztknl/` |
| 内核头文件 | `SYSTEM/lib/Ztknl/include/` |
| 内核 Makefile | `SYSTEM/lib/Ztknl/Makefile` |
| 内核链接脚本 | `SYSTEM/lib/Ztknl/linker.ld` |
| 编译后内核 | `SYSTEM/system/knl.bin` |
| 用户程序源码 | `SYSTEM/lib/init/`, `shell/`, `desk/` |
| 用户程序二进制 | `SYSTEM/prgm/sys/` |
| 用户链接脚本 | `SYSTEM/lib/init/user.ld` |
| Limine 配置 | `SYSTEM/boot/limine.conf` |
| UEFI 引导文件 | `SYSTEM/EFI/BOOT/BOOTX64.EFI` |
| 打包脚本 | `ISO/build_iso.sh` |
| 输出镜像 | `ISO/zratroos.img`, `ISO/zratroos.iso` |
| Rust 监控内核 | `SYSTEM/lib/rust_kernel/` |
| Rust Cargo 配置 | `SYSTEM/lib/rust_kernel/Cargo.toml` |

#### A.3 内存地址速查

| 地址 | 用途 |
|------|------|
| `0xffffffff80000000` | 内核 Higher Half 起始 |
| `0x0000000000400000` | 用户程序加载地址 |
| `0x0000000000000000` | NULL 页保护 |

#### A.4 系统调用号速查

| 类别 | 范围 | 说明 |
|------|------|------|
| POSIX | 0-39 | 基础操作 |
| 事务 | 40-49 | 用户态事务 |
| PMEM | 50-59 | 持久内存 |
| ZPipe | 70-79 | 零拷贝管道 |
| Selfheal | 80-89 | 自愈框架 |
| Coroutine | 90-99 | 协程 |
| Heterog | 100-109 | 异构计算 |
| NetZC | 110-119 | 网络零拷贝 |
| Realtime | 120-129 | 实时调度 |
| Perf | 130-139 | 性能分析 |
| RLimit | 140-149 | 资源限制 |
| PEXLF | 160-169 | IPC |
| Cap | 170-179 | Capability 安全 |

### B. 项目统计

| 指标 | 数值 |
|------|------|
| **内核 C 源文件** | 99 个 |
| **内核头文件** | 102 个 |
| **汇编文件** | 5 个 |
| **内核 C 代码总行数** | 92,551 行 |
| **编译后内核大小** | ~850KB |
| **系统调用总数** | 180+ |
| **用户程序** | 3 个 (init/shell/desk) |
| **窗口管理器库** | ~50KB (1482行) |
| **文件管理器库** | ~40KB (1177行) |
| **Rust 监控内核模块** | 7 个 (~600行) |
| **支持文件系统** | 5 种 (FAT32/EXT4/RAMFS/TMPFS/PROCFS) |
| **支持网络协议** | 5 种 (TCP/UDP/ICMP/ARP/DHCP) |
| **支持 USB 版本** | 3 种 (UHCI/EHCI/XHCI) |
| **项目总代码行数** | ~96,151 行 |

### C. 代码规模 TOP 10 模块

| 文件 | 行数 | 功能 |
|------|------|------|
| fat32.c | 3983 | FAT32 文件系统 |
| ntdll.c | 4032 | Windows NT 兼容层 |
| heap.c | 3290 | 内核堆分配器 |
| vfs.c | 3122 | 虚拟文件系统 |
| tcp.c | 2522 | TCP 协议实现 |
| vmm.c | 2356 | 虚拟内存管理 |
| task.c | 2069 | 进程/线程管理 |
| scheduler.c | 2004 | SMP 调度器 |
| pmm.c | 1776 | 物理内存管理 |
| net.c | 1601 | 网络协议栈 |

### D. 版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| v1.5.1 | 2026-04-21 | 文档更新，代码统计完善 |
| v1.5.0 | 2026-04-20 | 页错误修复，back.bmp 背景实现 |
| v1.4.0 | 2026-04-19 | Desk GUI 事件循环修复，GPT 分区支持 |
| v1.3.0 | 2026-04-17 | DHCP/WiFi 修复，Rust 内核完善 |
| v1.2.0 | 2026-04-16 | WiFi 驱动实现 |

### E. 最近开发进展

#### 2026-04-20: 内核页错误修复与 back.bmp 背景实现
- 修复 `inter_kernel_init` 页错误崩溃（单内核模式下跳过初始化）
- 修复 HHDM 映射不足问题（从 2GB 扩展到 4GB）
- 实现 back.bmp 背景图片加载和拉伸渲染
- VNC 测试通过

#### 2026-04-19: Desk GUI 事件循环修复
- 实现 60fps 主事件循环
- 渲染顺序：清屏→背景→图标→窗口→任务栏→菜单→通知→光标
- 修复 desk 程序启动后无响应问题

#### 2026-04-19: 文件系统实现与 ATA 驱动修复
- 实现 GPT 分区表解析
- 实现 virtio-blk 驱动（仍有超时问题）
- 修复 ATA LBA28/LBA48 模式切换
- 实现 ATA PIO 写入支持
- FAT32 挂载成功

#### 2026-04-17: DHCP 修复、WiFi 配置加载修复、Rust 内核开发
- 修复 e1000 网卡启动状态问题
- 实现 Limine 模块提取到 RAMFS
- Rust 内核完成 GDT/IDT/Watchdog 实现

#### 2026-04-16: WiFi 驱动开发
- 实现 RTL8188EU USB WiFi 驱动
- 实现完整 WPA/WPA2-PSK 协议栈
- 修复 e1000 MMIO 映射问题

---

## F. ZratroOS 创新特性与优势分析

### F.1 与 Linux、Windows 的对比概览

| 特性 | ZratroOS | Linux | Windows |
|------|----------|-------|---------|
| **内核热更新** | ✅ 原生支持，无需重启 | ❌ 需要 kexec 或重启 | ❌ 需要重启 |
| **双程序格式支持** | ✅ ELF + PE 原生 | ✅ ELF | ✅ PE |
| **动态算法系统** | ✅ ZTS7 原生 | ❌ 需要 eBPF | ❌ 需要 eBPF/驱动 |
| **自愈内核** | ✅ 内置框架 | ❌ 需要 systemd | ❌ 需要 WER |
| **Capability 安全** | ✅ 原生能力模型 | ⚠️ POSIX capabilities | ⚠️ ACL/SDDL |
| **持久内存** | ✅ 原生 PMEM | ⚠️ 需要驱动 | ⚠️ 需要驱动 |
| **协程调度** | ✅ 内核级原生 | ⚠️ 用户态 (io_uring) | ⚠️ 用户态 |
| **零拷贝 IPC** | ✅ Z-Pipe 原生 | ⚠️ io_uring | ⚠️ RIO/注册IO |

### F.2 核心创新特性详解

#### F.2.1 ZTS7 动态算法系统

**概念**: 运行时加载和执行内核算法模块，无需重启或重新编译内核。

**三种执行模式**:

| 模式 | 说明 | 风险 | 应用场景 |
|------|------|------|----------|
| **API 模式** | 通过内核 API 安全交互 | 低 | 文件系统插件、网络协议扩展 |
| **Patch 模式** | 直接修改内核代码段 | 中 | 紧急补丁、性能优化 |
| **Inject 模式** | 插入函数钩子 | 高 | 调试、监控、安全审计 |

**与 Linux eBPF 对比**:

| 特性 | ZTS7 | eBPF |
|------|------|------|
| 代码大小限制 | 无限制 | 100万指令 |
| 执行模式 | 3种 | 1种 (验证器) |
| 内核修改 | 支持 | 不支持 |
| 文件系统扩展 | 支持 | 不支持 |
| 学习曲线 | 低 (C 语言) | 高 (专用指令集) |

**使用示例**:
```c
// 定义一个 ZTS7 模块 (0x00000000.zts.c)
ZTS_DECLARE("crypto_accel", "硬件加密加速", "Zratro Team", "MIT",
            ZTS_CAT_CRYPTO, 1, 0);

ZTS_MODULE_INIT(crypto_init) {
    // 初始化硬件加密引擎
    return zts_api->vfs_open("/dev/crypto", 0);
}

ZTS_MODULE_EXECUTE(crypto_execute) {
    // 执行加密操作
    return do_hardware_encrypt(input, output);
}
```

**系统调用接口**:
- `SYS_ZTS_LOAD(150)`: 加载算法模块
- `SYS_ZTS_EXECUTE(151)`: 执行算法
- `SYS_ZTS_UNLOAD(152)`: 卸载模块

#### F.2.2 内核热更新 (Hot Update)

**概念**: 在系统运行时动态替换内核模块，无需重启。

**核心机制**:

```
┌─────────────────────────────────────────────────────────┐
│                    热更新流程                             │
├─────────────────────────────────────────────────────────┤
│  1. 检查点创建 (checkpoint_create)                       │
│     ├── 保存模块状态                                     │
│     ├── 保存内存区域快照                                 │
│     └── 记录调用栈                                       │
│                                                         │
│  2. 加载新模块 (hu_module_load)                          │
│     ├── 解析 ELF 格式                                   │
│     ├── 重定位符号                                       │
│     └── 验证签名                                         │
│                                                         │
│  3. 原子切换 (module_hotupdate)                          │
│     ├── 调用 on_update 回调                              │
│     ├── 切换函数指针                                     │
│     └── 更新符号表                                       │
│                                                         │
│  4. 失败回滚 (checkpoint_restore)                        │
│     ├── 恢复模块状态                                     │
│     ├── 恢复内存快照                                     │
│     └── 重载旧模块                                       │
└─────────────────────────────────────────────────────────┘
```

**模块生命周期**:
```
UNLOADED → LOADED → ACTIVE → UPDATING → ACTIVE
                           ↓
                       ROLLBACK → ACTIVE (旧版本)
```

**与 Linux kexec/kpatch 对比**:

| 特性 | ZratroOS HotUpdate | Linux kexec | Linux kpatch |
|------|-------------------|-------------|--------------|
| 需要重启 | 否 | 是 | 否 |
| 应用范围 | 任意模块 | 整个内核 | 函数级 |
| 状态保存 | 完整检查点 | 无 | 无 |
| 回滚支持 | 原生 | 无 | 有限 |
| 安全验证 | 模块签名 | 无 | 无 |

**代码示例**:
```c
// 注册可热更新的模块
struct module_info net_driver = {
    .name = "e1000_driver",
    .version = "1.2.0",
    .type = MODULE_DRIVER,
    .on_load = e1000_load,
    .on_unload = e1000_unload,
    .on_update = e1000_update,  // 处理状态迁移
};
module_register(&net_driver);

// 热更新
checkpoint_create("Before driver update");
module_hotupdate("e1000_driver", "/system/modules/e1000_v2.ko");
```

#### F.2.3 双程序格式支持 (ELF + PE)

**概念**: 原生支持 Linux ELF 和 Windows PE 程序格式，无需模拟层。

**架构**:

```
┌─────────────────────────────────────────────────────────┐
│                   程序加载器                             │
├─────────────────────────────────────────────────────────┤
│  elf.c (20900 字节)          pe.c (880 行)             │
│  ├── ELF64 解析              ├── PE32+ 解析            │
│  ├── 段映射                   ├── 节映射                │
│  ├── 动态链接                 ├── 导入表解析            │
│  └── 重定位                   └── 重定位                │
├─────────────────────────────────────────────────────────┤
│  ntdll.c (4032 行) - Windows API 兼容层                 │
│  ├── 1000+ Windows API 函数模拟                         │
│  ├── SEH 异常处理链                                     │
│  ├── TLS 线程本地存储                                   │
│  ├── 句柄管理 (文件/进程/线程/互斥体/事件)               │
│  └── GDI 基础支持                                       │
└─────────────────────────────────────────────────────────┘
```

**Windows API 支持程度**:

| 类别 | 支持函数数 | 说明 |
|------|-----------|------|
| Kernel32 | ~200 | 文件、进程、线程、内存 |
| NTDLL | ~150 | 系统调用封装 |
| User32 | ~50 | 窗口、消息 |
| GDI32 | ~30 | 图形绘制 |
| AdvAPI32 | ~40 | 注册表、安全 |

**与 Wine/WSL 对比**:

| 特性 | ZratroOS | Wine | WSL1 | WSL2 |
|------|----------|------|------|------|
| 实现方式 | 原生内核支持 | 用户态模拟 | 内核翻译层 | 虚拟机 |
| 性能开销 | 极低 | 中 | 低 | 高 |
| 内存占用 | 共享内核 | 独立进程 | 共享内核 | 独立内核 |
| PE 支持 | 原生 | 用户态 | 无 | 无 |
| ELF 支持 | 原生 | 无 | 原生 | 原生 |

**系统调用双 ABI**:
```c
// syscall.c 中的分发逻辑
long syscall_handler(struct pt_regs *regs) {
    if (current->abi == ABI_ZRATRO) {
        return zratro_syscall_dispatch(nr, regs);
    } else {
        return linux_syscall_dispatch(nr, regs);  // Linux 兼容
    }
}
```

#### F.2.4 自愈内核框架 (Self-Heal)

**概念**: 自动检测、隔离、恢复内核故障，提高系统可靠性。

**故障类型与恢复策略**:

| 故障类型 | 检测方式 | 恢复策略 | 成功率 |
|----------|----------|----------|--------|
| 异常 (Exception) | IDT 中断 | 进程终止/重启 | 95% |
| 内存耗尽 (OOM) | 水位线监控 | 内存压缩/进程终止 | 80% |
| 内存损坏 | 堆 Canary/页校验 | 隔离域重置 | 70% |
| 死锁 | 超时检测 | 进程终止/资源释放 | 85% |
| 驱动故障 | 看门狗 | 驱动重载 | 90% |

**隔离域架构**:

```
┌─────────────────────────────────────────────────────────┐
│                    隔离域管理                             │
├─────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐    │
│  │ 域 0: 核心  │  │ 域 1: 网络  │  │ 域 2: 存储  │    │
│  │ (不可恢复)  │  │ (可重启)    │  │ (可重启)    │    │
│  └─────────────┘  └─────────────┘  └─────────────┘    │
│         ↓                ↓                ↓            │
│  ┌─────────────────────────────────────────────────┐  │
│  │              故障传播阻断                         │  │
│  └─────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

**看门狗与心跳**:
```c
// 注册心跳监控
struct sh_heartbeat *hb = sh_register_heartbeat(
    "network_stack",
    5000,  // 超时 5 秒
    network_heartbeat_callback
);

// 周期性心跳
void network_tick(void) {
    sh_heartbeat_tick(hb);
}

// 超时回调
void network_heartbeat_callback(void) {
    // 自动重启网络栈
    net_restart();
}
```

**与 systemd/Windows WER 对比**:

| 特性 | ZratroOS Self-Heal | systemd | Windows WER |
|------|-------------------|---------|-------------|
| 内核级支持 | 是 | 否 (用户态) | 部分 |
| 检查点恢复 | 支持 | 不支持 | 不支持 |
| 隔离域 | 支持 | cgroup | Job Object |
| 故障传播阻断 | 原生 | 有限 | 有限 |

#### F.2.5 Capability 安全模型

**概念**: 基于能力的细粒度权限控制，替代传统 DAC/MAC。

**权限粒度** (64位位图):

| 位范围 | 权限类别 | 示例 |
|--------|----------|------|
| 0-7 | 文件系统 | CAP_FS_READ, CAP_FS_WRITE |
| 8-15 | 网络 | CAP_NET_SEND, CAP_NET_RECV |
| 16-23 | 进程 | CAP_PROC_FORK, CAP_PROC_EXEC |
| 24-31 | 内存 | CAP_MEM_ALLOC, CAP_MEM_MMAP |
| 32-39 | 设备 | CAP_DEV_IOCTL, CAP_DEV_DMA |
| 40-47 | 安全 | CAP_CAP_DELEGATE, CAP_CAP_REVOKE |
| 48-55 | 系统 | CAP_SYS_REBOOT, CAP_SYS_TIME |
| 56-63 | 保留 | - |

**安全特性**:

| 特性 | 实现方式 | 安全等级 |
|------|----------|----------|
| 防篡改 | XOR-MAC 签名 | 中 (可升级为 Ed25519) |
| 时间限制 | 过期时间戳 | 高 |
| 使用限制 | max_uses 计数器 | 高 |
| 委托追踪 | parent_cap 链 | 高 |
| 撤销传播 | CAP_FLAG_REVOKED | 高 |

**与 POSIX Capabilities 对比**:

| 特性 | ZratroOS | POSIX |
|------|----------|-------|
| 权限粒度 | 64 位 | ~40 个常量 |
| 时间限制 | 支持 | 不支持 |
| 使用限制 | 支持 | 不支持 |
| 委托 | 原生支持 | 有限 |
| 密码学保护 | 签名验证 | 无 |

**使用示例**:
```c
// 分配 capability
struct capability *cap = cap_alloc();
cap->resource_id = file_inode;
cap->permissions = CAP_FS_READ | CAP_FS_WRITE;
cap->expiry_time = get_time() + 3600000;  // 1 小时
cap->max_uses = 100;
cap_sign(cap);

// 检查权限
if (cap_check(cap, CAP_FS_WRITE) == 0) {
    // 允许写入
}

// 委托给子进程
struct capability *child_cap = cap_delegate(cap, CAP_FS_READ);  // 只读
```

#### F.2.6 持久内存管理 (PMEM)

**概念**: 非易失性内存的原生支持，断电后数据不丢失。

**架构**:

```
┌─────────────────────────────────────────────────────────┐
│                    PMEM 子系统                           │
├─────────────────────────────────────────────────────────┤
│  段管理 (Segment)                                       │
│  ├── pmem_create_segment() - 创建持久段                 │
│  ├── pmem_open_segment() - 打开现有段                   │
│  └── pmem_close_segment() - 关闭段                      │
├─────────────────────────────────────────────────────────┤
│  事务支持                                               │
│  ├── pmem_tx_begin() - 开始事务                         │
│  ├── pmem_tx_commit() - 提交事务                        │
│  └── pmem_tx_abort() - 回滚事务                         │
├─────────────────────────────────────────────────────────┤
│  快照系统                                               │
│  ├── pmem_snapshot_create() - 完整快照                  │
│  ├── pmem_snapshot_create_inc() - 增量快照              │
│  └── pmem_snapshot_restore() - 恢复快照                 │
├─────────────────────────────────────────────────────────┤
│  持久分配器                                             │
│  ├── pmem_alloc() - 段内分配                            │
│  └── pmem_free() - 段内释放                             │
└─────────────────────────────────────────────────────────┘
```

**与 Linux PMEM (DAX) 对比**:

| 特性 | ZratroOS PMEM | Linux DAX |
|------|---------------|-----------|
| 内核级事务 | 原生支持 | 需要 libpmem |
| 快照系统 | 内置 | 需要应用实现 |
| 瞬时启动 | 支持 | 不支持 |
| 分配器 | 内置 | 需要 jemalloc |

#### F.2.7 Z-Pipe 零拷贝 IPC

**概念**: 进程间通信零数据拷贝，通过共享内存实现。

**架构**:

```
┌─────────────────────────────────────────────────────────┐
│                    Z-Pipe 通道                           │
├─────────────────────────────────────────────────────────┤
│  生产者                        消费者                    │
│    │                            ↑                        │
│    │  ┌──────────────────────┐  │                        │
│    └─→│ 无锁环形缓冲区        │──┘                        │
│       │ (DMA 可访问)          │                          │
│       │ 物理连续内存          │                          │
│       └──────────────────────┘                           │
│                ↑                                         │
│           共享内存池                                      │
└─────────────────────────────────────────────────────────┘
```

**性能对比** (理论值):

| IPC 机制 | 拷贝次数 | 上下文切换 | 延迟 |
|----------|----------|-----------|------|
| 传统 pipe | 2+ | 2+ | 高 |
| Linux io_uring | 0-1 | 0-1 | 中 |
| Z-Pipe | 0 | 0 | 低 |

**特性**:

| 特性 | 说明 |
|------|------|
| 无锁环形缓冲区 | 单生产者/单消费者无锁 |
| DMA 直通 | 直接网卡到用户空间 |
| 背压控制 | 防止消费者过载 |
| Scatter-Gather | 支持 iovec |

#### F.2.8 内核级协程

**概念**: 内核原生支持协程调度，比用户态协程更高效。

**架构**:

```
┌─────────────────────────────────────────────────────────┐
│                   协程调度器                             │
├─────────────────────────────────────────────────────────┤
│  线程 A                  线程 B                         │
│  ├── 协程 1              ├── 协程 5                     │
│  ├── 协程 2              ├── 协程 6                     │
│  └── 协程 3              └── 协程 7                     │
│        ↓                      ↓                         │
│  ┌─────────────────────────────────────────────────┐   │
│  │              工作窃取 (Work Stealing)            │   │
│  └─────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

**Go 风格 Channel**:
```c
// 创建 channel
struct co_channel *ch = co_chan_create(10, sizeof(struct packet));

// 发送 (协程 A)
co_chan_send(ch, &pkt);

// 接收 (协程 B)
co_chan_recv(ch, &pkt);
```

**与用户态协程对比**:

| 特性 | ZratroOS 内核协程 | 用户态协程 (goroutine/libco) |
|------|-------------------|------------------------------|
| 调度开销 | 与线程相同 | 更低 |
| 系统调用 | 可阻塞协程 | 需特殊处理 |
| I/O 多路复用 | 内核原生 | 需要 epoll/kqueue |
| 优先级 | 内核级 | 用户态 |

### F.3 系统设计哲学

#### F.3.1 单内核 + 模块化

ZratroOS 采用单内核架构，但通过模块系统实现灵活性：

```
┌─────────────────────────────────────────────────────────┐
│                    单内核设计                             │
├─────────────────────────────────────────────────────────┤
│  优点:                                                  │
│  • 系统调用零切换开销                                   │
│  • 模块间直接函数调用                                   │
│  • 简单的同步模型                                       │
│                                                        │
│  模块化机制:                                            │
│  • 热更新支持运行时替换                                 │
│  • ZTS7 支持动态扩展                                    │
│  • 隔离域限制故障传播                                   │
└─────────────────────────────────────────────────────────┘
```

#### F.3.2 双内核架构

Ztknl (主内核) + Rust 监控内核：

```
┌─────────────────────────────────────────────────────────┐
│                    双内核架构                            │
├─────────────────────────────────────────────────────────┤
│  ┌─────────────────┐    ┌─────────────────┐            │
│  │    Ztknl        │    │  Azknl (Rust)   │            │
│  │  主内核 (C)     │◄──►│  监控内核       │            │
│  │                 │    │                 │            │
│  │  功能内核       │    │  健康监控       │            │
│  │  用户程序       │    │  看门狗         │            │
│  │  设备驱动       │    │  异常捕获       │            │
│  └─────────────────┘    └─────────────────┘            │
│           │                    │                        │
│           └────────┬───────────┘                        │
│                    ↓                                    │
│           ┌─────────────────┐                           │
│           │   共享内存      │                           │
│           │   进程间通信    │                           │
│           └─────────────────┘                           │
└─────────────────────────────────────────────────────────┘
```

**优势**:
- 主内核崩溃时监控内核可记录状态
- Rust 的内存安全保证监控内核可靠性
- 独立的看门狗和心跳检测

### F.4 总结：ZratroOS 的独特价值

| 领域 | 创新点 | 竞争优势 |
|------|--------|----------|
| **可靠性** | 自愈框架 + 双内核 | 99.99% 可用性目标 |
| **灵活性** | ZTS7 + 热更新 | 无停机升级 |
| **兼容性** | ELF + PE 双支持 | 跨平台应用生态 |
| **性能** | Z-Pipe + 内核协程 | 零拷贝、低延迟 |
| **安全** | Capability 模型 | 细粒度权限控制 |
| **数据** | PMEM 原生支持 | 瞬时启动、持久化 |

---

**文档维护**: ZratroOS 开发团队
**联系方式**: forum.zratro.dev
**本文档路径**: `RE/DEVELOPMENT_GUIDE.md`

---

*本文档基于 ZratroOS v1.5.1 源码编写，随项目持续更新。*
*最后更新: 2026-04-21*

---

## 11. ZTS7 内核自更新支持

### 11.1 概述

ZTS7 (ZratroOS Dynamic Algorithm System v7) 是 ZratroOS 的动态算法系统，支持运行时加载和执行算法模块，实现内核功能的自更新和热替换。

**源文件**: `SYSTEM/lib/Ztknl/zts_algorithm.c` (15KB)  
**头文件**: `include/zts_algorithm.h`  
**示例**: `SYSTEM/lib/Ztknl/examples/example_algorithm.zts.c`

### 11.2 架构设计

```
┌────────────────────────────────────────────────────┐
│              ZTS7 系统架构                          │
│                                                    │
│  用户空间                                          │
│    │                                               │
│    ▼                                               │
│  .zts 模块文件                                     │
│    │ (优先级排序)                                   │
│    ▼                                               │
│  ZTS 加载器 (zts_algorithm.c)                      │
│    ├── 模块验证 (魔数/版本/校验和)                   │
│    ├── API 表注入                                   │
│    └── 执行调度                                     │
│                                                    │
│  运行模式:                                          │
│  ├── API 模式: 通过标准 API 与内核交互               │
│  ├── Patch 模式: 直接替换内核函数代码                │
│  └── Inject 模式: 注入代码到内核地址空间             │
└────────────────────────────────────────────────────┘
```

### 11.3 模块格式

每个 `.zts` 模块包含以下头部结构：

```c
struct zts_header {
    uint32_t magic;              // ZTS_MAGIC (0x5A545337)
    uint32_t version;            // 模块版本
    uint32_t api_min_version;    // 最低 API 版本要求
    char name[ZTS_MAX_NAME];     // 模块名称
    uint8_t uuid[ZTS_UUID_LEN];  // 唯一标识符
    uint32_t category;           // 类别 (FS/NET/CRYPTO/SCHED/DRIVER/PATCH/INJECT)
    uint32_t major_version;      // 主版本
    uint32_t minor_version;      // 次版本
    size_t init_func_offset;     // 初始化函数偏移
    size_t execute_func_offset;  // 执行函数偏移
    size_t cleanup_func_offset;  // 清理函数偏移
    uint32_t checksum;           // 校验和
};
```

### 11.4 API 表

ZTS7 提供完整的内核 API 表供模块使用：

```c
struct zts_api_table {
    uint32_t api_version;
    // 内存管理
    void *(*alloc)(size_t size);
    void (*free)(void *ptr);
    void *(*realloc)(void *ptr, size_t size);
    // 日志
    void (*log_print)(const char *level, const char *fmt, ...);
    // 时间
    uint64_t (*time_now)(void);
    // 字符串/内存操作
    int (*strcmp)(const char *s1, const char *s2);
    void *(*memcpy)(void *dst, const void *src, size_t n);
    void *(*memset)(void *s, int c, size_t n);
    // VFS 操作 (可选)
    // 网络操作 (可选)
};
```

### 11.5 使用方法

```c
// 初始化 ZTS7 系统
zts_init();

// 注册模块 (从二进制数据)
zts_register_module(binary_data, size, priority);

// 执行模块
zts_execute_module("module_name", input_data, output_data);

// 卸载模块
zts_unload_module("module_name");

// 列出所有模块
zts_list_modules();
```

### 11.6 优先级系统

模块优先级从文件名解析，格式为 `0xPPPPPPPP.zts`：
- `0x00000000.zts` - 最高优先级
- `0xFFFFFFFF.zts` - 最低优先级

加载时按优先级排序，确保依赖关系正确。

### 11.7 与 Hotupdate 集成

ZTS7 与 Hotupdate 模块紧密集成，实现完整的内核自更新：

1. **ZTS7 负责**: 算法模块的动态加载和执行
2. **Hotupdate 负责**: 模块状态管理、检查点、回滚

结合使用流程：
```
1. 创建检查点 (checkpoint_create)
2. 加载新 ZTS 模块 (zts_register_module)
3. 执行新算法 (zts_execute_module)
4. 验证结果
5. 成功：保留 / 失败：回滚 (checkpoint_restore)
```

### 11.8 Linux 内核移植 (2026-04-22 更新)

**概述**: 将 Linux 内核驱动和算法编译为 ZTS7 可加载模块，实现热替换。

**适配层**: `SYSTEM/lib/lints/include/zts_adapt.h`

**核心原理**:
1. Linux 内核 API 通过宏映射到 ZTS API 表
2. 使用 `-nostdlib -fPIC -shared` 编译为共享对象
3. 通过 `zts_header_tool` 注入 ZTS 头部（魔数、版本、入口点）
4. 运行时加载到 `/zrr64/zts/p.zts6/` 目录

**API 映射示例**:
```c
// Linux 内核代码可以直接编译为 ZTS 模块
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

**已编译模块** (2026-04-22):
| 模块 | 优先级 | 大小 | 说明 |
|------|--------|------|------|
| TCP CUBIC | 0x00000001 | 15KB | TCP 拥塞控制 |
| TCP BBR | 0x00000002 | 15KB | BBR v2 拥塞控制 |
| virtio-blk | 0x00000010 | 15KB | Virtio 块驱动 |
| **All Drivers** | **0x00000020** | **20KB** | **统一驱动包** |

### 11.9 统一驱动模块 (2026-04-22 新增)

**文件**: `SYSTEM/lib/lints/modules/all_drivers/all_drivers.c`

**设计理念**: 将所有设备驱动合并为单个 ZTS 模块，通过 `driver_id` 分发请求。

**包含驱动**:
| driver_id | 驱动 | 功能 | MMIO/IO |
|-----------|------|------|---------|
| 0 | virtio-blk | 块存储 | - |
| 1 | virtio-net | 网络设备 | - |
| 2 | AHCI | SATA 控制器 | 0xFE010000 |
| 3 | NVMe | PCIe SSD | 0xFE020000 |
| 4 | e1000 | Intel 网卡 | 0xC100 / 0xFE030000 |
| 5 | RTL8188EU | WiFi 适配器 | 0xC200 |
| 6 | XHCI | USB 3.0 | 0xFE040000 |
| 7 | QUERY | 驱动状态查询 | - |

**驱动分发接口**:
```c
struct driver_input {
    u32 driver_id;    /* 0-6 选择驱动，7=查询状态 */
    u32 cmd;          /* 驱动特定命令 */
    u32 port;         /* AHCI/USB 端口 */
    u32 nsid;         /* NVMe 命名空间 ID */
    void *data;
    u32 len;
    u64 sector;       /* 块设备扇区号 */
    u32 sectors;      /* 块设备扇区数 */
    u8 mac[6];        /* MAC 地址 */
};

struct driver_output {
    u32 status;
    u32 bytes;
    u64 capacity;
    u32 blk_size;
    u32 mtu;
    u8 mac[6];
};
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

// 使用 virtio-net 发送
inp.driver_id = 1;
inp.cmd = 0;  // TX
inp.data = packet;
inp.len = packet_len;
zts_execute_module("all_drivers", &inp, &out);
```

**引导配置** (`limine.conf`):
```
module_path: boot():/system/zrr64/zts/p.zts6/0x00000020.zts
```

**内核路径**: `/zrr64/zts/p.zts6/`（无 `system` 前缀）

---

## 12. 自愈内核框架

### 12.1 概述

ZratroOS 的自愈框架 (Self-Healing Framework) 提供故障检测、隔离、恢复机制，确保系统高可用性。

**源文件**: `SYSTEM/lib/Ztknl/selfheal.c` (25KB)  
**头文件**: `include/selfheal.h`

### 12.2 核心功能

```
故障检测 → 故障报告 → 查找处理器 → 执行恢复 → 记录结果
```

### 12.3 故障类型

| 类型 | 说明 | 默认策略 |
|------|------|----------|
| `SH_FAULT_EXCEPTION` | 异常/陷阱 | 根据严重级别 |
| `SH_FAULT_OOM` | 内存耗尽 | 重试 |
| `SH_FAULT_MEMORY_CORRUPTION` | 内存损坏 | 隔离 |
| `SH_FAULT_DEADLOCK` | 死锁 | 隔离 |
| `SH_FAULT_DRIVER` | 驱动故障 | 重启组件 |
| `SH_FAULT_HANG` | 挂起/超时 | 重试 |
| `SH_FAULT_MEMORY_LEAK` | 内存泄漏 | 记录 |

### 12.4 恢复策略

| 策略 | 说明 | 适用场景 |
|------|------|----------|
| `SH_RECOVER_NONE` | 不处理 | 信息级别 |
| `SH_RECOVER_RETRY` | 重试操作 | 临时故障 |
| `SH_RECOVER_RESTART` | 重启组件 | 驱动/服务故障 |
| `SH_RECOVER_ISOLATE` | 隔离故障域 | 内存损坏、死锁 |
| `SH_RECOVER_ROLLBACK` | 回滚到检查点 | 严重错误 |
| `SH_RECOVER_REBOOT` | 重启系统 | 致命错误 |

### 12.5 隔离域管理

```c
// 创建隔离域
struct sh_isolation_domain *dom = sh_create_domain(shm, "network", SH_DOMAIN_FLAG);

// 隔离域
sh_isolate_domain(shm, dom->domain_id);

// 解除隔离
sh_unisolate_domain(shm, dom->domain_id);
```

### 12.6 检查点机制

```c
// 创建检查点
struct sh_checkpoint *ckpt = sh_create_checkpoint(shm, "pre_update", state_data, state_size);

// 恢复检查点
sh_restore_checkpoint(shm, ckpt->checkpoint_id);

// 自动检查点
sh_auto_checkpoint(shm);
```

### 12.7 看门狗与心跳

```c
// 注册看门狗
struct sh_watchdog *wd = sh_register_watchdog(shm, "network_wd", 5000000000, on_timeout);

// 喂看门狗
sh_feed_watchdog(shm, wd->id);

// 注册心跳
sh_register_heartbeat(shm, "scheduler", 1000000000, 3, on_miss);

// 发送心跳
sh_send_heartbeat(shm, "scheduler");
```

### 12.8 与 Rust 监控内核集成

Rust 监控内核 (rustknl) 通过以下方式支持自愈：

1. **看门狗监控**: 定期检查 Ztknl 和 Azknl 心跳
2. **共享内存验证**: 通过校验和检测数据完整性
3. **异常处理**: IDT 捕获异常并报告
4. **健康状态输出**: 通过串口/输出内核状态

### 12.9 统计与诊断

```c
// 获取统计信息
uint64_t faults, recoveries, successes, failures;
sh_get_stats(shm, &faults, &recoveries, &successes, &failures);

// 打印状态
sh_print_status(shm);
```

输出示例：
```
=== Self-Healing Status ===
  Total faults: 15
  Total recoveries: 12
  Successful: 11
  Failed: 1
  Checkpoints: 3
  Isolation domains: 2
  Watchdogs: 4
```

---

## 13. 内核热更新系统

### 13.1 概述

Hotupdate 模块支持不停机更新内核代码，包括函数替换、状态迁移和回滚。

**源文件**: `SYSTEM/lib/Ztknl/hotupdate.c` (33KB)  
**头文件**: `include/hotupdate.h`

### 13.2 模块管理

```c
// 注册模块
struct module_info mod = {
    .name = "network_stack",
    .version = "1.0",
    .on_load = network_init,
    .on_unload = network_cleanup,
    .on_update = network_update
};
module_register(&mod);

// 热更新
module_hotupdate("network_stack", "/path/to/new_module.bin");
```

### 13.3 检查点系统

```c
// 创建检查点
int cp_id = checkpoint_create("Pre-update checkpoint");

// 恢复检查点
checkpoint_restore(cp_id);

// 列出检查点
struct update_checkpoint *cps;
int count;
checkpoint_list(&cps, &count);
```

### 13.4 差分更新

支持创建和应用差分包，仅传输变更部分：

```c
// 创建差分包
void *diff = kmalloc(diff_size);
size_t diff_size;
diff_create(old_data, old_size, new_data, new_size, diff, &diff_size);

// 应用差分包
diff_apply(data, &size, diff_data, diff_size);
```

### 13.5 事务管理

```c
// 开始事务
uint64_t tx_id = tx_begin();

// 添加操作
tx_add_op(tx_id, TX_OP_WRITE_MEM, target, old_value, new_value, size);

// 准备事务
tx_prepare(tx_id);

// 提交或回滚
if (validation_passes()) {
    tx_commit(tx_id);
} else {
    tx_rollback(tx_id);
}
```

### 13.6 更新状态机

```
IDLE → DOWNLOADING → VERIFYING → PREPARING → APPLYING → COMPLETED
                                                    ↓
                                               ROLLING_BACK → FAILED
```

### 13.7 更新通道

| 通道 | 说明 | 稳定性 |
|------|------|--------|
| `CHANNEL_STABLE` | 稳定版 | 最高 |
| `CHANNEL_BETA` | 测试版 | 高 |
| `CHANNEL_DEV` | 开发版 | 中 |
| `CHANNEL_NIGHTLY` | 每夜构建 | 低 |

```c
update_set_channel(CHANNEL_BETA);
```

---

## 14. 双内核架构 (Ztknl / Azknl / RustKnl)

### 14.1 概述

ZratroOS 采用三内核设计：

| 组件 | 语言 | 功能 |
|------|------|------|
| **Ztknl** | C | 主内核，完整功能 |
| **Azknl** | C | 辅助内核，监控/备份 |
| **RustKnl** | Rust | 监控内核，看门狗/共享内存 |

### 14.2 VMX/EPT 隔离

主内核通过 Intel VMX/EPT 技术加载副内核，实现硬件级隔离：

```c
// 初始化双内核系统
dual_kernel_init(1, "azknl.elf");  // 1 = 主内核

// 加载副内核
dual_kernel_load("azknl.elf");

// 启动副内核
dual_kernel_start_secondary();
```

### 14.3 共享内存通信

```c
// 发送消息
dual_kernel_send_msg(MSG_TYPE_HEARTBEAT, data, count);

// 接收消息
struct inter_kernel_msg msg;
dual_kernel_recv_msg(&msg);
```

### 14.4 架构优势

- **故障隔离**: 副内核崩溃不影响主内核
- **监控**: 主内核监控副内核，反之亦然
- **热备**: 副内核可在主内核故障时接管
- **安全**: 通过 EPT 实现内存隔离

