# ZratroOS 项目上下文

**版本**: ZT5
**架构**: x86_64 Higher Half 独立操作系统
**主语言**: C (内核) + Rust (监控内核) + 汇编
**引导器**: Limine (UEFI/BIOS 双支持)

---

## 项目简介

ZratroOS 是一个从零开始编写的独立 64 位操作系统，具有完整的内核功能、用户空间程序和图形界面支持。内核空间映射在 `0xffffffff80000000` 以上地址 (Higher Half)。

### 核心特性
- **内存管理**: PMM (物理内存) / VMM (虚拟内存) / Heap / SLAB 分配器
- **进程管理**: Task / CFS + EEVDF 调度器 / 实时调度 / SMP 多核支持
- **文件系统**: VFS / EXT4 / FAT32 / RAMFS / TMPFS / PROCFS
- **网络协议栈**: TCP/IP / UDP / ICMP / DHCP / Socket API
- **设备驱动**: AHCI / NVMe / E1000 / WiFi (RTL8188EU) / USB (UHCI/EHCI/XHCI)
- **安全机制**: Capability 能力模型 / 栈保护 / SMEP/SMAP / 基于路径的权限控制 / sudo 提权
- **高级特性**: 协程 / 异构计算 / 内核热更新 / 零拷贝网络 / AI 子系统
- **GUI**: Win11 + KDE Plasma 融合风格 / 窗口管理器 / 文件管理器 / 桌面环境
- **Rsknl 监控内核**: 自检 / 实时健康监测 / 自动修复 / 内核回滚 / 升级恢复策略
- **软件管理**: zinstall 软件安装器 / 在线软件仓库 / HTTP 下载

---

## 目录结构

```
Deaicup-ZratroOS/
├── SYSTEM/                    # 系统核心目录
│   ├── lib/                   # 源代码
│   │   ├── Ztknl/             # 主内核 (93+ C 模块, ~100 头文件)
│   │   ├── rust_kernel/       # Rust 监控内核 (Rsknl)
│   │   ├── init/              # Init 程序 (PID 1)
│   │   ├── shell/             # Shell 程序 (CLI)
│   │   ├── desk/              # 桌面环境 (GUI)
│   │   ├── wm/                # 窗口管理器 (zrdemage)
│   │   ├── fm/                # 文件管理器
│   │   ├── zinstall/          # 软件安装器
│   │   ├── zrrWindowsManager/ # 窗口管理器库 (旧)
│   │   └── zrrExfileManager/  # 文件管理器库 (旧)
│   ├── prgm/                  # 编译后的用户程序
│   │   ├── sys/               # 系统程序 (init, shell, desk, wm)
│   │   ├── fm                 # 文件管理器
│   │   └── zinstall           # 软件安装器
│   ├── system/                # 系统镜像输出 (knl.bin, zrdemage)
│   ├── boot/limine.conf       # Limine 引导配置
│   └── EFI/BOOT/BOOTX64.EFI   # UEFI 引导器
├── build_all.sh               # 全量构建脚本
├── ISO/                       # 镜像构建
│   ├── build_iso.sh           # FAT32/ISO 打包脚本
│   └── zratroos.img           # UEFI 可引导镜像
└── RE/                        # 研究文档
    ├── DEVELOPMENT_GUIDE.md   # 完整开发文档
    └── KERNEL_IMPLEMENTATION_DETAILS.md  # 内核实现细节
```

---

## 构建系统
### 全量构建

```bash
./build_all.sh
# 一次性编译内核和所有用户程序
# 注意: build_all.sh 在项目根目录，不在 SYSTEM/ 下
```

### 编译 Rust 监控内核 (Rsknl)

```bash
cd SYSTEM/lib/rust_kernel
cargo build --lib --release
# 输出: target/x86_64-unknown-none/release/librustknl.a
```

### 构建可启动镜像

```bash
cd ISO
./build_iso.sh
# 输出: ISO/zratroos.img (UEFI 可引导)
```

### 构建选项

```bash
./build_iso.sh --clean    # 清理临时文件
./build_iso.sh --verify   # 验证配置文件
./build_iso.sh --keep     # 保留 staging 目录
```

---

## 测试方法

### QEMU UEFI 测试

```bash
# 使用 OVMF UEFI 固件
qemu-system-x86_64 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
    -drive if=pflash,format=raw,file=/tmp/OVMF_VARS.fd \
    -drive file=ISO/zratroos.img,format=raw \
    -m 2G -smp 2 -serial stdio
```

`

### 真实硬件测试

```bash
# 写入 U 盘
sudo dd if=ISO/zratroos.img of=/dev/sdX bs=4M status=progress conv=fsync
```

---

## 内核架构

### 系统调用 ABI

```
中断方式: int 0x80
参数传递: RAX = 系统调用号, RDI/RSI/R10/R8/R9 = 参数 1-5
返回值: RAX
```

### 系统调用分类 (180+)

| 范围 | 类别 |
|------|------|
| 0-39 | POSIX 基础 (exit, read, write, open, close, fork, exec, mmap...) |
| 40-49 | 用户事务 (tx_begin, tx_commit, tx_abort...) |
| 50-59 | 持久内存 (pmem_create, pmem_alloc...) |
| 70-79 | Z-Pipe 零拷贝 |
| 80-89 | 自愈内核 |
| 90-99 | 协程 (co_create, co_yield, co_channel...) |
| 100-109 | 异构计算 |
| 110-119 | 网络零拷贝 |
| 120-129 | 实时调度 |
| 130-139 | 性能分析 |
| 140-149 | 资源限制 (rlimit, priority) |
| 150-159 | 内核测试 |
| 160-169 | PEXLF IPC |
| 170-179 | Capability 安全 |
| 180+ | AI 子系统 / sudo 提权 |

### 内存布局 (Higher Half)

```
0xFFFFFFFFFFFFFFFF ┌────────────────────┐
                   │   内核空间 (128TB+)  │
0xFFFFFFFFC0000000 │   Rsknl 受保护区域   │
0xFFFFFFFF80000000 │   Ztknl 内核代码/数据 │
                   ├────────────────────┤
                   │   用户空间 (512GB)   │
0x00000000400000   │   程序加载 (4MB+)    │
0x00000000000000   │   NULL 页保护        │
                   └────────────────────┘
```

### 引导流程

```
UEFI/BIOS → Limine → _start → boot_diag → 
GDT/IDT → PMM/VMM → SMP → 设备初始化 → 
文件系统 → 网络栈 → Rsknl 加载 → ELF 加载 → init (PID 1)
```

---

## 核心模块说明

### Ztknl 主内核 (`SYSTEM/lib/Ztknl/`)

| 模块 | 文件 | 功能 |
|------|------|------|
| 内存管理 | pmm.c, vmm.c, heap.c, slab.c, mmap.c, cow.c, pmem.c | 物理/虚拟内存、堆、SLAB、写时复制、持久内存 |
| 进程管理 | task.c, scheduler.c, cfs.c, signal.c, realtime.c, rlimit.c | 进程、CFS+EEVDF调度、信号、实时调度、资源限制 |
| 文件系统 | vfs.c, fat32.c, ext4.c, ramfs.c, tmpfs.c, procfs.c | VFS、FAT32、EXT4、RAMFS、TMPFS、PROCFS |
| 网络栈 | net.c, net_dhcp.c, net_zerocopy.c, tcp.c, socket.c, dhcp.c | TCP/IP、Socket、DHCP、零拷贝网络 |
| 设备驱动 | ahci.c, nvme.c, e1000.c, wifi.c, usb.c, ehci.c, xhci.c, uhci.c, usb_hid.c, usb_pci.c, virtio.c, drm.c, fb.c, ps2.c, disk.c | 存储、网络、USB、Virtio、GPU、输入设备 |
| 中断处理 | idt.c, isr.c, apic.c, page_fault.c, smp.c | IDT、ISR、APIC、缺页、SMP |
| 安全机制 | capability.c, selfheal.c, stack_protection.c | 能力、自愈、栈保护 |
| 权限系统 | vfs.c (权限检查), syscall.c (sudo) | 基于路径的访问控制、sudo 提权 |
| 虚拟化 | vmm/ept.c, vmm/vmx.c, vmm/vmexit.c, vmm/dual_kernel.c, inter_kernel.c, subkernel.c | EPT、VMX、VM Exit、双内核 |
| 高级特性 | coroutine.c, heterog.c, hotupdate.c, zts_algorithm.c, zpipe.c, rcu.c, perf.c, perf_analysis.c, ipc.c, pexlf.c | 协程、热更新、ZTS7、零拷贝管道、RCU、性能分析 |
| AI 子系统 | ai_core.c, ai_http.c, ai_log.c | AI 核心逻辑、HTTP 通信、日志 |
| 系统服务 | abi.c, syscall.c, elf.c, exec.c, loader.c, module.c, ntdll.c, pe.c, umdf.c | ABI、系统调用、ELF/PE 加载、模块、NT 兼容层 |
| Rsknl 加载 | rsknl_loader.c | Rsknl 加载、共享内存初始化、健康块管理 |
| 基础设施 | main.c, boot_diag.c, gdt.c, console.c, config.c, string.c, cpu.c, image.c, chinese_font.c, tty.c, input.c, opu.c, power.c, lockdebug.c, kselfcheck.c, ktest.c, limine_file.c, fs_proxy.c, cgroup.c, nsproxy.c | 入口、引导、控制台、测试等 |

### 头文件 (`include/`)

核心头文件:
- `knl.h` - 内核主入口
- `ktypes.h` - 内核类型定义
- `syscall.h` - 系统调用定义
- `task.h` - 进程/线程结构
- `vmm.h` / `pmm.h` - 内存管理
- `vfs.h` - 文件系统接口
- `net.h` / `tcp.h` - 网络协议
- `ai.h` - AI 子系统
- `hotupdate.h` - 内核热更新
- `zts_algorithm.h` - ZTS7 动态算法
- `dual_kernel.h` / `inter_kernel.h` - 双内核架构

---

## 用户空间程序

### init (`SYSTEM/lib/init/`)

- PID 1 进程
- 系统初始化入口
- 启动窗口管理器、桌面、文件管理器
- 监控子进程，自动重启崩溃服务

### shell (`SYSTEM/lib/shell/`)

- 交互式命令行界面
- 内置命令: cd, pwd, ls, echo, clear, help, exit, sudo
- 支持: 管道 (|), 重定向 (>, >>, <)
- 命令历史 (32 条)
- 命令记录到 `/zrr64/shlg/otp.out`
- sudo 提权: 验证 root 密码哈希后临时提升权限

### wm - 窗口管理器 (`SYSTEM/lib/wm/`)

- 编译输出: `SYSTEM/prgm/sys/zrdemage`
- Win11 + KDE Plasma 融合风格窗口装饰
- 窗口管理: 创建、移动、缩放、最小化、最大化、关闭
- 窗口合成: Mica/Acrylic 半透明效果、圆角、阴影
- 焦点管理、层级合成、事件分发
- Snap Layouts (Win11 风格窗口布局)

### desk - 桌面环境 (`SYSTEM/lib/desk/`)

- 编译输出: `SYSTEM/prgm/sys/desk`
- Mica 桌面背景
- 桌面图标: 显示 `/desktop/user` (或 `/desktop/root`) 下的 `.prd` 快捷方式
- 默认快捷方式: Files (fm), Terminal (shell)
- 右键上下文菜单
- 任务栏 (ZT5 图标，KDE Plasma 风格)
  - ZT5 Logo 点击弹出应用启动器
  - 网络连接面板 (读取 `/system/internet/e1w.cfg`)
  - WiFi 连接/断开控制
  - 音量控制
  - 时钟显示
- 开机 LOGO: 屏幕显示大字 "ZT5" (抗锯齿渲染)
- 登录界面: 用户名/密码输入，Tab 切换
- 交叉淡入淡出过渡 (登录→桌面)
- 60fps 渲染循环

### fm - 文件管理器 (`SYSTEM/lib/fm/`)

- 编译输出: `SYSTEM/prgm/fm` (用户级程序)
- Win11 + KDE Dolphin 融合风格
- 正确显示文件信息 (大小、权限、修改时间)
- 导航栏、侧边栏、面包屑路径
- 文件/文件夹图标、列表/网格视图

### zinstall - 软件安装器 (`SYSTEM/lib/zinstall/`)

- 编译输出: `SYSTEM/prgm/zinstall`
- 在线软件仓库: `www.deaicup.com/zratro/osprogram`
- 功能: 搜索、安装、更新软件包
- 使用 HTTP GET 下载软件包
- 命令: `zinstall list`, `zinstall search <query>`, `zinstall install <name>`, `zinstall update`

---

## 权限系统

### 基于路径的访问控制

用户权限由 `/ini/usrroot/usrroot.cfg` 配置:
- 普通用户只能读写 `/usr/[用户名]` 和 `/desktop/[用户名]` 下的文件
- 普通用户可以运行 `/prgm/` 下的程序 (非 `/prgm/sys/`)
- `/prgm/sys/` 下的系统程序 (init, shell, desk, wm) 仅 root 可执行

### sudo 提权

- 系统调用: `SYS_SUDO` (验证 root 密码哈希)
- Shell 命令: `sudo <command>` (临时提升权限执行命令)
- 密码哈希存储在内核中

### 程序分类

| 路径 | 类型 | 权限 |
|------|------|------|
| `/prgm/sys/init` | 系统程序 | root only |
| `/prgm/sys/shell` | 系统程序 | root only |
| `/prgm/sys/desk` | 系统程序 | root only |
| `/prgm/sys/zrdemage` | 系统程序 | root only |
| `/prgm/fm` | 用户程序 | 所有用户 |
| `/prgm/zinstall` | 用户程序 | 所有用户 |

---

## Rust 监控内核 (Rsknl)

**位置**: `SYSTEM/lib/rust_kernel/`
**状态**: 活跃开发 (~70% 完成度)
**受保护地址**: `0xFFFFFFFF_C0000000` (任何程序不得触碰)

### 架构

Rsknl 作为独立内核运行在受保护的内存地址，主内核启动时加载。通过共享内存与 Ztknl 通信。

### 模块

| 模块 | 文件 | 功能 |
|------|------|------|
| 核心入口 | lib.rs | 初始化、监控主循环 |
| 自检系统 | health_monitor.rs (SelfTest) | 内存/校验和/原子/栈/GDT-IDT 自检 |
| 健康监测 | health_monitor.rs (HealthMonitor) | 实时内核状态监测、异常检测 |
| 自愈系统 | selfheal.rs | 故障报告、恢复策略、升级恢复 |
| 回滚管理 | selfheal.rs (RollbackManager) | 内核快照、回滚到健康状态 |
| 看门狗 | watchdog.rs | Ztknl 存活检测 |
| 共享内存 | shared_mem.rs | Ztknl <-> Rsknl 通信 |
| 故障类型 | selfheal_types.rs | 19 种故障类型、6 级严重度、9 种恢复动作 |
| VGA 输出 | vga.rs | 80x25 文本模式 |
| 串口输出 | serial.rs | COM1 115200bps |
| GDT/IDT | gdt.rs, idt.rs | 独立的 GDT/IDT |

### 自检项目

1. 内存读写测试
2. 校验和计算测试
3. 原子操作测试
4. 栈完整性测试
5. GDT/IDT 加载验证

### 健康监测

- 通过共享内存读取 Ztknl 的 `kernel_health_block`
- 监测: tick 计数器、任务数、空闲页、堆使用、缺页数
- 检测: 内核挂起 (tick 不推进)、内存压力、堆压力、panic
- 周期性自检 (每 10000 tick)

### 恢复策略 (升级恢复)

| 升级级别 | 轻微故障 | 中度故障 | 严重故障 | 致命故障 |
|----------|----------|----------|----------|----------|
| 0 (正常) | 无动作 | 重试 | 重启 | 隔离/重启 |
| 1 (提升) | 重试 | 重启 | 隔离 | 回滚 |
| 2 (严重) | 重启 | 回滚 | 回滚 | 重启 |
| 3 (紧急) | 重启 | 重启 | 重启 | 重启 |

### 回滚机制

- 保存最近 4 个健康快照
- 仅当健康分数 >= 80 时允许回滚
- 回滚请求通过共享内存发送给 Ztknl

### 编译

```bash
cd SYSTEM/lib/rust_kernel
cargo build --lib --release
```

---

## 网络配置

### WiFi 连接

网络配置文件: `/system/internet/e1w.cfg`

```
name = "网络名称"
password = "密码"
```

桌面任务栏提供网络连接面板，可连接/断开 WiFi。

---

## 开发依赖

### 必需工具

- `gcc` - C 编译器
- `ld` - 链接器
- `nasm` - 汇编器 (部分代码)
- `make` - 构建工具
- `xorriso` - ISO 生成（IMG FAT32 格式）
- `limine` - 引导加载器

### Rust 工具链 (可选)

- `rustup` + `nightly` 工具链
- `cargo`

### 测试工具

- `qemu-system-x86_64` - QEMU 模拟器
- `edk2-ovmf` - UEFI 固件 (OVMF)

---

## 调试技巧

### 串口输出

内核通过 COM1 (115200bps) 输出调试信息:

```bash
qemu-system-x86_64 ... -serial stdio
```

### 内核日志级别

在 `limine.conf` 中设置:
```
kernel_cmdline: debug=on loglevel=7 console=ttyS0,115200
```

### 常见问题

1. **页错误**: 检查 `page_fault.c` 中的错误码解析
2. **ATA 超时**: QEMU 模拟器可能有 5 秒超时，需要重试
3. **内存不足**: 检查 PMM 水位线和碎片率
4. **Rsknl 加载失败**: 检查 Limine 模块是否包含 rustknl
5. **权限拒绝**: 检查用户路径权限和 sudo 提权

---

## 代码规范

### 文件命名

- C 源文件: 小写 + 下划线 (如 `page_fault.c`)
- 头文件: 小写 + 下划线 (如 `syscall.h`)
- 汇编文件: 后缀 `.S` (如 `idt_asm.S`)
- Rust 文件: 小写 + 下划线 (如 `health_monitor.rs`)

### 函数命名

- C: 模块前缀 + 下划线 (如 `pmm_alloc_page()`, `vmm_map()`)
- Rust: snake_case (如 `save_snapshot()`, `report_fault()`)
- 静态函数可省略前缀

### 数据结构

- C 结构体: `struct xxx_struct` 或简写 `struct xxx`
- C 类型定义: `typedef struct xxx xxx_t`
- Rust: PascalCase (如 `KernelSnapshot`, `HealthMonitor`)

---

## 参考文档

- `RE/DEVELOPMENT_GUIDE.md` - 完整开发文档 (系统架构、API 参考)
- `RE/KERNEL_IMPLEMENTATION_DETAILS.md` - 内核实现细节 (算法、数据结构)
- `RE/NETWORK_FEATURES.md` - 网络功能文档
- `RE/LOG/` - 开发日志

---

## 项目状态

| 组件 | 状态 |
|------|------|
| Ztknl 主内核 | 活跃开发，功能完整 |
| EEVDF 调度器 | 已实现 (CFS+EEVDF 双调度) |
| Rsknl 监控内核 | 活跃开发 (~70%，自检+监测+修复+回滚) |
| init | 完成 (服务监控+自动重启) |
| shell | 完成 (sudo+命令历史) |
| wm (zrdemage) | 已实现 (Win11+KDE融合风格) |
| desk | 已实现 (Mica背景+ZT5开机LOGO+网络面板) |
| fm (文件管理器) | 已实现 (正确文件信息显示) |
| zinstall (软件安装器) | 已实现 (搜索+安装+更新) |
| FAT32 文件系统 | 完成 (含 GPT 支持) |
| TCP/IP 协议栈 | 完成 |
| WiFi 驱动 | 完成 (RTL8188EU) |
| 权限系统 | 已实现 (基于路径+sudo) |
| PE 兼容层 | 开发中 (ntdll+winhttp) |
