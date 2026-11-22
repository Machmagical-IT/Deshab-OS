# Deshab 项目上下文

## 项目概览

Deshab 当前包含一个内核/系统实验方向，启动目标为：

```text
CODE/boot -> SYSTEM/ESP/EFI/BOOT/BOOTX64.EFI   （UEFI 引导器，替代 Limine）
CODE/UTSM -> SYSTEM/ESP/boot/utsm.elf          （唯一内核）
```

**启动链（A 方案：UTSM 由引导器直接装载）**：

```text
UEFI 固件 → BOOTX64.EFI（CODE/boot/main.c）
    → GOP 渐变背景 + 无步骤文字的加载条动画
    → EFI SimpleFileSystem 读取 FUCK / manifest / 全部 .drv / 可选的 Linux guest 载荷
    → 解析 utsm.elf（静态 ET_EXEC）并按 PT_LOAD 复制到物理内存
    → 自建页表（恒等映射 + HHDM 0xffff800000000000 + 内核高半区 0xffffffff80000000）
    → 伪造 Limine 兼容响应（module/fb/hhdm/rsdp/kernel_address/memmap）
    → ExitBootServices → 切 CR3/GDT/栈 → 回填内核 .limine_requests → 跳 UTSM
```

`utsm.elf` 仍是 Limine 协议入口（六类 request 结构不变），但不再依赖 Limine：
`CODE/boot` 提供同名响应。引导器把启动卷 FAT32 卷序列号作为 `boot:volid`
伪模块下传，`fat32.drv` 据此在实机多盘场景锁定启动 U 盘（AHCI/NVMe 内置盘常先于
xHCI 注册为块设备 0）。`SYSTEM/ESP/limine/` 与 `limine.conf` 保留但不再参与启动。

`SYSTEM/ESP/boot/utsm.elf` 由 UTSM 负责 UTSM、DKM 驱动加载、平台发现和早期系统服务；
DSK（`SYSTEM/ESP/system/deshab64/deshab.elf`）已退役，会话编排由 `session.drv`
（stage4，`srv.session.run`）承担。

FirstInit（`CODE/firstInit`）与 `firstinit.drv` 负责用户级设置向导：
- 淡蓝背景 → "欢迎使用 Deshab"（中英双语）→ 账户设置卡片
- PS/2 键盘轮询输入：计算机名、用户名、密码
- SHA256 密码摘要 + XOR 加密配置缓冲（写盘经 `srv.fs.fat32.*`）
- 18px 黑体中文位图（预渲染嵌入）

## ZTS 子系统 / ZT5 guest（2026-10-18）

**源码母本**：`CODE/ZT5/`（从 `D:\Code\DEAICUP\ZT5` 复制的完整源码树，排除
AIquekao/target/ISO 等构建产物）。运行期实现分成两部分：

```text
CODE/DKM/zts/                 → SYSTEM/ESP/driver/misc/zts.drv（DKM stage3）
  zts_api.c / zts_elf.c / zts_module.c / zts_hotupdate.c / zts_store.c
  zts_selftest.c / zts_main.c / zts.h
CODE/zts/                     → ZTS 模块工具链与示例
  include/zts_module.h, modules/example_module*.c, mkzts.py
SYSTEM/ESP/zts/                   → ESP 根 /zts/（ZTS.CNF + .zts + A/B 状态）
```

ZTS 热更子系统：

- `.zts` = 760B `zts_header` + 16B 对齐 + ELF64 **ET_REL**正文；重定位
  `R_X86_64_64/32/32S/PC32/PLT32`，未定义符号解析到宿主 `zts_api` 导出表。
- 功能：模块加载/执行/卸载、热替换、检查点（槽位引用+注册表+patch 快照）、
  差分更新（modify/insert/delete 变长块）、事务（内存/文件/模块 op）、
  ftrace 式函数替换、版本快照回滚、符号/依赖检查、A/B 状态持久化
  （`STATEA/B.CNF` + CRC32 + SEQ）。
- 入口：`srv.zts.*` 服务总线、shell `zts` 命令、settings ZTS 页（F4）、
  `ZTS.CNF` 开机自动加载。FUCK `[zts] enable/autoload/selftest`。
- 模块失败只回滚并打印 `ZTS-E01..E14/E90`，不 panic 内核。
- **驱动内必须自带 `memcpy/memset/snprintf`**：clang 会把结构体赋值与
  `__builtin_snprintf` 降级为 libc 调用；DKM 没有 libc，PLT32 会解析到
  0/低地址（曾表现为 `#UD rip=0xa0000`）。

ZT5 guest 子系统（VMX non-root）：

- `CODE/UTSM/vmm/zt5_loader.c` + `zt5_boot.c` + `include/utsm/zt5_loader.h`：
  装载 `SYSTEM/ESP/boot/zt5.krl`，构造 guest 页表（恒等 0-4GB + HHDM +
  内核高半区；**PDPT 索引=510**）、GDT、伪 Limine 响应
  （hhdm/memmap/fb/kernel_file/module/cmdline），回填 `.limine_reqs`
  （knl.bin 无 section 头时扫描镜像前 64KB）；EPT 映射 boot area/fb/
  modules/kernel(64MB)/RAM(1GB)。
- Guest 布局：boot area 0x20000、fb 0x100000、modules 0x400000、
  kernel 0x1000000（64MB 窗口）、RAM 0x6000000。**不要 identity-map
  低 1MB**（会覆盖 boot area 的 EPT 映射）。
- VMCS：VPID=4；guest CR4 必须含 VMXE、CR0 含 MP/ET；selector=0 的
  FS/GS/LDTR 必须标 unusable；MSR bitmap 物理地址走
  `vmx_kernel_virt_to_phys`（不能用 `-HHDM`）；vmlaunch 前
  VMCLEAR+VMPTRLD。
- FUCK `[zt5] enable=0|1`。默认 0；开启后 ZT5 独占运行（host 暂停到 guest
  终止），协作切片 `kern.zt5.pump` 待 P4。`kern.zt5.status` 已注册。
- `build.ps1` 复制 `CODE/ZT5/SYSTEM/system/knl.bin` → `SYSTEM/ESP/boot/zt5.krl`，
  用户程序/配置 → `SYSTEM/ESP/boot/zt5/`；EFI 引导器以 `zt5:kernel` /
  `zt5:<guest 路径>` 预加载。
- 已验证（WSL2+KVM nested VMX）：ZT5 `_start` → GDT/IDT/PMM/VMM/Heap →
  APIC/SMP → VFS/RAMFS → ZTS7/AI 初始化 → Stage 9 → 加载 desk → 进入
  scheduler。剩余阻塞：scheduler 后 guest 页错误（desktop 返回路径，
  待 guest profile/用户态页表继续定位）。

## 当前核心架构

```text
SAS-R0-PCQ
    单地址空间 Ring0 任务模型
    Per-CPU O(1) 位图调度器

DRR
    Dedicated Recovery Root
    看门狗 / checkpoint / recovery log / A-B 回滚

UTSM
    UUID-Tuned Sealed Memory
    UUID 调谐封缄内存

UTRW
    UTSM Tagged Reader/Writer
    UTSM 带标记读写器

DMP
    Descriptor Meta Page
    段描述符元页

PCKC
    Per-CPU Key Cache
    每 CPU 密钥缓存

FUCK
    内核启动配置文件
    INI 分区格式，控制启动行为/驱动开关/算法参数/桌面行为
```

## 关键设计约束

1. 优先考虑极高权限和极高效率。
2. 主系统采用单地址空间 Ring0 模型。
3. 不追求传统多用户强安全隔离。
4. DRR 作为恢复根，应尽量独立于普通调度器。
5. UTSM 数据区连续存密文，UUID 不嵌入数据区。
6. 调度器热路径必须保持 O(1)。
7. checkpoint 只处理 dirty shard / dirty page，不扫描全内存。
8. DRR 必须拥有独立 Emergency Pool，普通 OOM 不应阻断 recovery。
9. checkpoint metadata 使用 A/B 双槽、CRC 校验和原子 active slot 切换。
10. 驱动模块采用 DKM ELF64 .drv ABI，通过 driver_desc / driver_init / driver_exit 暴露入口。
11. 驱动默认通过 dkm_kernel_api 调用内核服务，不直接依赖任意内核符号。
12. stage0/stage1 驱动必须支持 bootloader 预加载 boot module 方案。

## 重要文件

```text
SYSTEM/
    打包为 IMG 后的系统根目录。

SYSTEM/ESP/driver/manifest.json
    内核启动驱动模块清单，控制 ELF .drv 驱动的分阶段加载。

SYSTEM/STARTMENU.CNF
    M3 开始菜单注册表（FAT32 根目录，desktop 启动时解析）。
    格式 NAME|显示名|path|args；path 支持 builtin:<name> / program/xxx.exe（PE 应用，
    g++ 编译，run_windowed 嵌入桌面）/ bin/xxx.elf（ELF 工具，全屏加载）/
    /guest/path（Linux guest 应用）。显示名可 UTF-8 中文；文件缺失时回退全部已注册应用。
    目录约定：bin/ = ELF 工具，program/ = exe（cmd PATH：根 → bin/ → program/）。
    纯 CLI exe（program/pciscan.exe、program/portio.exe）不入菜单——在 CMD 里敲名字
    运行，输出走串口（shim printf/putchar 只到 COM1）。源码在 CODE/tools/pe-tools/
    （build.ps1 用 mingw-w64 g++ 编译，-nostdlib + 空 __main，导入面必须保持在
    pe_shim 表内，objdump -p 自检），ELF 工具源码在 CODE/tools/diskinfo/
    （→ SYSTEM/ESP/bin/diskinfo.elf，diskpart 风格只读磁盘工具）。

SYSTEM/ESP/program/ + SYSTEM/ESP/bin/ + SYSTEM/ESP/lib/ + PEAPPS.CNF + LINUXAPP.CNF + STARTMENU.CNF + AUTOEXEC.BAT
    桌面 PE/工具链全部经 DSK FAT32 只读 ESP——pack_system_image.sh 会把这几项
    额外拷到 FAT32 ESP（p1）。漏拷则开始菜单/PE 应用在 GPT 镜像里静默失效。

> **应用启动链路（desktop launcher.c）**：内置 ELF 应用（shell/cmd/browser/
> settings/proedit）用 `app_descriptor.elf_path`（FS 路径如
> `system/deshab64/shell.elf`），**不要**用 `elf_name`（那是 FS 根目录 8.3 名，
> 这些文件不在根）。桌面侧外部 ELF 加载缓冲是**固定地址 0x8800000 起 8MB**
> （`DT_ELF_IMAGE_ADDR`）——不能放 .bss：desktop.elf 自身映像须装进 DSK 的
> 8MB ibuf，静态 4MB+ 缓冲会撑爆它；8MB 需容纳 cmd.elf 的 4.27MB 映像。

SYSTEM/ESP/system/deshab64/FUCK
    内核启动配置文件（INI 分区格式），控制启动行为开关、驱动加载开关、UTSM 算法参数、DSK 桌面行为、panic 策略（[panic] mode=strict|fallback）。
    UTSM 通过 Limine boot module 预加载读取（cmdline="fuck:config")。
    DSK 通过 FAT32 子目录遍历读取（路径 "SYSTEM  /DESHAB64 /FUCK    ")。
    缺失或为空时使用硬编码默认值，行为与无配置文件时一致。

CODE/UTSM/kernel/ini_parser.h / ini_parser.c
    UTSM INI 配置解析器，支持分区格式解析、key/value 查询、整型和布尔值便捷获取。

CODE/DKM/README.md
    DKM 驱动模块系统说明，记录驱动加载目标、ABI、loader、状态机和实现清单。

CODE/UTSM/README.md
    UTSM 模块设计说明。

docs/RE/UTSM_算法记录.md
    UTSM 核心算法记录。

docs/RE/UTSM_设计架构.md
    UTSM 架构记录。

docs/RE/驱动模块ABI设计.md
    DKM 驱动模块 ABI、kernel_api、ELF relocation、manifest、状态机、boot module 方案和服务总线 ABI（§8.5）。

docs/RE/万物互联与服务总线设计.md
    万物互联架构定稿：dkm_bus 按名服务总线（kern.*/srv.*）、dsk_boot_context 逐槽迁移表、
    DSK 溶解地图、P0-P6 落地路线与错误码登记（BUS-E/DKM-E44..47/SESSION-E）。

ISO/deshab.img
    build.ps1 将 SYSTEM 打包生成的 GPT 磁盘镜像，包含一个 FAT32 EFI System Partition。

ISO/run_qemu.bat
    使用 QEMU 启动 ISO/deshab.img 的脚本。
```

## 构建相关

已知说明：

```text
SYSTEM/ 是唯一打包输入：build.ps1 只扫描 SYSTEM/，不从 /home 或其它路径取发行物。
所有要进镜像的软件（utsm.elf、deshab.elf、.drv、bzImage/initrd、
linux-rootfs.img、linux-extra-rootfs.img、Python/KDE 内容）必须先落到 SYSTEM/。

SYSTEM/ 下只有三个分区目录，一个目录 = 一个分区，打包器原样拷贝、不再按白名单
重新分类（位置即归属）：

  SYSTEM/ESP   -> GPT p1 FAT32 ESP (EF00)   UEFI/引导器读的分区
  SYSTEM/DATA  -> GPT p2 ext4      (8300)   内核与系统数据分区，uuid 固定
                                            64657368-6162-4000-8000-000000000002
  SYSTEM/WIN   -> GPT p3 NTFS      (0700)   Windows Guest 系统卷

三个分区一律创建（数量固定，运行期 fat32/ext4/VMM 依赖稳定磁盘形状）。
打包由 WSL CODE/linux/pack_system_image.sh 执行 mkfs.vfat + mkfs.ext4 + mkfs.ntfs。

跨分区“同一份内容出现在两处”写在 SYSTEM/LINKS.CNF（符号链接表）：
  <目标分区>:<目标路径> <- <来源分区>:<来源路径>
源树只保留一份真实内容；同一文件系统内建硬链接，跨文件系统退化为复制。
分区容量计算已把链接表额外承载的字节数计入。当前表项：
  DATA:/system/deshab64/FUCK <- ESP:/system/deshab64/FUCK  （ext4 启动自检要求）
  DATA:/driver              <- ESP:/driver

内核/驱动/DSK 一律走 boot(): （FAT32 ESP）。
FAT32 单文件上限 4GiB-1 仍适用于 ESP 上的 /boot 文件，linux-extra-rootfs.img 必须小于 4GiB。
SYSTEM/ESP/boot/ 存放启动模块，例如 utsm.elf。
SYSTEM/ESP/EFI/BOOT/BOOTX64.EFI 由 build.ps1 的 Build-EfiBootStub 从 CODE/boot/main.c 生成
  （clang -target x86_64-pc-windows-msvc + lld-link /subsystem:efi_application）。
  CODE/boot 装载 SYSTEM/ESP/boot/utsm.elf 与 SYSTEM/ESP/driver/**，不再需要 Limine。
SYSTEM/ESP/driver/ 存放 ELF .drv 驱动模块和 manifest.json。
SYSTEM/ESP/boot/zt5/ 是 ZT5（ZratroOS）guest 的用户态载荷（prgm/sys/init、
  desk、zrdemage、zinstall、prgm/shell、windowprogram/fm、zratro.conf），
  引导器按 "zt5:<guest 路径>" 预加载为 boot module；zt5.krl 是 guest 内核本体。
CODE/UTSM/README.md: 编译到 SYSTEM/ESP/boot/utsm.elf
build.bat: 调用 build.ps1，透传参数（如 -Variant dev）。
build.ps1: 编译 UTSM/工具/驱动，把 SYSTEM/{ESP,DATA,WIN} 打成 GPT 三分区镜像：
  - ISO/deshab-dev.img      开发者模式（dev_mode=1，自动跑 shell.elf 测试，调试全开，QEMU 验证选项，panic=strict）
  - ISO/deshab-release.img  发布版（dev_mode=0，进桌面+登录，调试零开销，真机安全选项，panic=strict）
  - ISO/deshab-realtest.img 真机测试版（release 同款真机选项 + 登录路径，panic=strict 即启用 panic）
  - ISO/deshab-ne.img       NE 版（No-panic Edition：release 同款真机选项 + 登录路径，panic=fallback）
  参数 -Variant dev|release|realtest|ne|both|all（默认 both = dev+release；all = 四者）。both/all 构建后恢复 SYSTEM 为 dev 配置。
  每张镜像固定 25GiB：p1 ESP 7GiB、p2 DATA 3GiB、p3 NTFS 拿走余下约 15GiB。有 SYSTEM/windows/deshab-windows.wim 时两张镜像都把 WIM 装进 p3。
  配置模板在 build/configs/{dev,release,realtest,ne}/（FUCK + firstInit.txt），打包前注入到 SYSTEM。
  realtest/FUCK 由 release/FUCK 复制而来，仅头注释与 [panic] mode 不同；ne/FUCK 同样复制自 release，仅 [panic] mode=fallback；改 release 真机选项须同步三份。
  运行期改 [panic] mode（strict|fallback）即可在 NE 镜像上恢复严格 panic，无需重新打包。
ISO/run_qemu.bat [dev|release|realtest|ne]: 用 QEMU + UEFI 固件启动对应镜像（默认 dev），串口输出到 stdio。
ISO/run_qemu_kvm.sh: 默认 12G 内存（Limine 会把 SYSTEM/boot 里的 rootfs 模块装进内存）。
build.ps1 自动把 Git usr/bin 加入 PATH（sh.exe + mkdir），解决 make `mkdir -p` 在 cmd.exe 下失败的问题。
```

当前 UTSM 已有首阶段内核代码骨架和构建脚本，可生成 utsm.elf 与 GPT（FAT32 引导分区 + ext4 数据分区）镜像。早期启动要求：进入 C 前 `cli/cld`，early serial 不允许无限等待硬件 ready 位，未启用 FPU/SSE 前编译必须使用 `-mno-sse -mno-sse2 -mno-mmx -msoft-float`。DATA/BSS 段使用 PF_R|PF_W|PF_X（SAS-R0 下驱动模块内存需可执行）。

DKM/DSM 驱动加载系统已完工：内置 `console_early` + manifest.json 解析 + Limine boot module 预加载 + 4 stage 分阶段加载 + 全部外部 `.drv` ET_REL 装载（ELF64/SHT_NOBITS/R_X86_64_64/32/32S/PC32/PLT32 relocation + driver_desc 校验 + driver_init 调用）。当前镜像额外预加载 `driver/test.fat32` 作为 FAT32 驱动测试镜像。

> **relocation 集合注意**：`R_X86_64_PLT32(4)` 语义等同 PC32（静态可重定位下的 `S+A-P`）。多编译单元驱动（如 `ath9k.drv = ath9k.o + wpa2_crypto.o`）跨 TU 的全局函数调用会被 lld 生成为 PLT32；loader 不支持即启动 `DKM-E37`。新增多源 `.drv` 前用 `llvm-readobj --relocations` 核对类型是否在上述集合内。

> **ath9k 硬件要求（实机 WiFi 试验）**：原生驱动仅支持 **Atheros AR928x 系列 PCIe 无线网卡（vendor `168c`，如 AR9285/9280/9287）**。FUCK `[drivers] ath9k=1` 时才加载。若机器无匹配 Atheros 卡、BAR0 位于 4G 以上、或复位/初始化失败，`driver_init` 返回非零 → `DKM-E39 EXTERNAL DRIVER INIT FAILED` **开机即 panic**（零降级，不豁免）；串口会打印本机所有无线控制器 (class 02:80) 的 vendor:device 供确认卡型号。Intel/Realtek/MediaTek 等其他厂商网卡**不支持**。`wlan.drv` 为框架骨架，在有无线控制器的机器上只注册设备，其 scan/connect 调用即 `NETWL-E10/E11` panic。


当前已实现 14 个真实 DKM 驱动：
- **timer** (stage0): PIT 校准, 100ms busy-wait demo
- **apic** (stage0): CPUID Local APIC 探测, IA32_APIC_BASE MSR, ACPI MADT 枚举, LAPIC enable + IOAPIC 重定向表编程（ISO 表感知）; B7: FUCK `apic_route=1` 时 IOAPIC→LAPIC 接管（注册 UTSM LAPIC EOI 钩子 + PIC 全屏蔽 + TPR=0 + tick vec 0x20 链路自验，失败自动完整降级回 PIC）; `apic_route_legacy=1` 额外放开 IRQ1/12/11 持续投递（e1000 强制 level/active-low）; `apic_diag=1` HPET 绝对时基对拍 + 键鼠流观察窗
- **acpi** (stage0): Limine RSDP → XSDT, ACPI 表枚举；FADT → DSDT `_S5_` AML 解析 → S5 关机 + FADT `reset_reg` 重启，注册 `srv.acpi.poweroff` / `srv.acpi.reboot`（母本 `SYSTEMCOPY/OpenXJ380/driver/power.cpp`，Apache-2.0）
- **pci** (stage0): PCI config space 扫描, 设备枚举
- **console_fb** (stage1): Limine framebuffer, 淡蓝色背景清屏（原四色圆弧加载环已移除，启动画面改由 DSK 静态 Logo 接管）
- **ahci** (stage1): PCI AHCI `8086:2922` 探测, BAR5/ABAR HHDM MMIO 映射, HBA CAP/GHC/PI/VS 与 port 枚举；已接入 `kernel_api.dma`，支持 SATA disk IDENTIFY 与 LBA0 READ 最小 DMA 路径；可向 `kernel_api.block` 注册 `ahci0` block provider；默认无 SATA 盘/ATAPI 场景会跳过命令
- **nvme** (stage1): PCI NVMe `1b36:0010` 探测, BAR0/BAR1 解析, IRQ line 读取；当前 QEMU BAR0 位于 4G 以上，MMIO register 读取等待高位 PCI MMIO 映射能力
- **bootfs** (stage1): Limine boot module 内存文件系统
- **vfs** (stage2): 挂载 bootfs, 文件查找/读取
- **devfs** (stage2): 暴露 `/dev/version`, `/dev/platform`, `/dev/fb0`, `/dev/modules`, `/dev/boot/`
- **fat32** (stage2): 只读 FAT32 BPB/FAT/目录项解析, 通过 `driver/test.fat32` 验证读取 `README.TXT`
- **ps2kbd** (stage3): PS/2 IRQ1 注册, 端口 `0x60` 扫描码读取, scan code set 1 → ASCII demo；事件 ring 经 `srv.input.kbd.poll` 暴露，另有 `srv.input.kbd.push` 供其他输入源（USB HID）注入扫描码
- **e1000** (stage3): PCI `8086:10d3` 探测, BAR0 HHDM MMIO 映射, MAC 读取, IRQ 注册, 基础 link setup
- **virtio_net** (stage3): PCI virtio-net `1af4:1001` 探测, BAR 枚举, modern virtio PCI capability 枚举；暂不进行 feature negotiation / virtqueue / 收发包
- **hda** (stage3): Intel HDA 控制器 + codec AFG/DAC/引脚选择 + BDL 同步回放；注册 `srv.audio.play/stop/info`（消费契约 `CODE/DKM/dkm_audio.h`），FUCK `[drivers] hda` + `hda_selftest`。母本 `SYSTEMCOPY/OpenXJ380/driver/hda`（Apache-2.0）。只做回放，无录音/混音器
- **xhci** (stage1): USB Mass Storage（BOT+SCSI）block provider；新增 **USB HID boot 键盘**——Root Hub 直连，8 字节报告差分翻译成 Set-1 扫描码，经 `srv.input.kbd.push` 注入 ps2kbd ring，`srv.input.usb.pump` 由 ps2kbd 每次 poll 前驱动（无需调度任务）。外部 hub 未支持

当前内核平台能力：IDT 0–255 stub 全覆盖、异常处理、PIC remap 到 `0x20–0x2f`、IRQ handler 注册与 EOI、UTSM 侧 LAPIC EOI 钩子（`idt_register_apic_eoi`，IRQ 分发末尾调用，spurious 0xFF 特判跳过）、IOAPIC→LAPIC 路由接管（B7，legacy IRQ1/12/11 可持续投递）、Limine RSDP/framebuffer/module/HHDM request。

kernel_api 已暴露能力：log, rsdp_address, fb_address/width/height/pitch/bpp, boot_modules_response, irq_register, hhdm_offset, dma.alloc_pages, block provider registry, mmio/mm_map_mmio/mm_unmap_mmio, register_apic_eoi（尾部追加，ABI 兼容）。

manifest 中全部外部 `.drv` 均为真实 DKM 驱动并可加载执行。第三方源码（libvterm / libutf / MikanOS hankaku / dr_mp3，均 MIT/Apache-2.0/Unlicense 兼容）vendor 在 `CODE/third_party/`，来源与用法见该目录 `NOTICE.md`。

下一阶段优先路线（活跃路线 = **万物互联 P0-P6**（D6/D7，设计定稿 `docs/RE/万物互联与服务总线设计.md`）+ `docs/ROADMAP_INSANE.md` 五期填补计划，任务编号 F/U/D/R/E；状态唯一来源 = `docs/STATUS.md`）：
0. **万物互联（D6/D7，当前最高优先）**: P0 manifest 依赖图强制 → P1 dkm_bus 双向服务总线（kern.*/srv.*）→ P2 fat32/ext4.drv 归位 → P3 加载器 FS 化 + stage4 + ps2mouse.drv → P4 六 app 模块 DKM 化（netman/shell/firstInit/login/desktop/cmd）→ P5 DSK 退役 + session.drv → P6 常驻调度 + 真 IRQ 输入。每 phase QEMU 验证后提交，进度见 STATUS.md 万物互联表。
1. **一期 F 系列收尾**: F2 统一控制台后端（fbcon.h 抽象已落，panic 已用）。（F1 莲花 panic / F3 跨平台 Makefile / F4 boot 分级自检 / F5 skip_firstinit 已完成）
2. **二期 U 系列**: ✅ 已完成（U1 UUID 表 / U2 Slow Path 真分支 / U3 @sealed 可选标记+linker .sealed section / U4 密文 dump / U5 RFC 8439 KAT）。chacha20 KAT 三方对拍通过（pycryptodome+官方向量+C），"间歇 FAIL" 确认为从未合并的幽灵缺陷。
3. **三期 D 系列**: DKM 热卸载（引用计数）、裸机器码加载器、按名 API 导出表升级为服务总线（并入万物互联 P1）。（~~彩色日志~~ D4 已完成：驱动名 hash → 8 色 ANSI，FUCK color_log 开关，BOOTLOG 保持纯文本）
4. **四期 R 系列收尾**: （~~segment/system 级回滚验证~~ R4 已完成：page/segment/system 三级闭环 + A/B 槽破坏 fallback 硬验收 PASS）、PCQ per-cpu 多核（多核在 P6 常驻调度之后排队）。（~~drr_emergency_pool_kb/drr_recovery_log_size 接线~~ R3b 已完成，运行期钳位验证 PASS）
5. **五期 E 系列**: 启动莲花动画、假想时光键彩蛋、提交信息规范、风味 ISO。
6. **工程债（Phase 存档遗留）**: vector allocator/MSI-X、NVMe 高位 BAR、virtio-net virtqueue、DKM 零 warning。

## M1 里程碑完成状态（2026-07-29）

### 任务 A: B6 FirstInit 按键崩溃修复
**状态**: [RESOLVED-NOT-REPRODUCIBLE]

**验证结果**: 
- QEMU 复现测试完成，系统完整运行 FirstInit 流程无崩溃
- 按键扫描码处理正常，字段输入正确（测试序列: a, b, Enter, shift+comma, Enter, 1, 2, Enter）
- user.conf 成功写入磁盘，firstInit.txt 成功更新为 "1"
- 系统继续加载 desktop

**证据**: `.build_tmp/qemu_serial_b6.log` 串口日志，调试记录 `debug-firstinit-key-crash.md`

**结论**: B6 崩溃在当前构建中未复现，可能已在之前的修改中修复，或崩溃条件较特殊未触发。

### 任务 B: PS/2 鼠标完整实现
**状态**: [COMPLETED]

**实现内容**:
1. **完整初始化序列** (`CODE/mouse/main.c`):
   - ps2_drain() 冲刷控制器残留数据
   - 读配置字节 → 启用 AUX 时钟/IRQ/翻译 → 写回
   - 启用 AUX 端口
   - 复位鼠标 → 验证 ACK/自检/设备 ID
   - Set Defaults → 采样率 100 → 分辨率 2 → 启用数据报告
   - 全程超时保护（100000 次），单步失败只记日志继续

2. **IRQ12 中断处理**:
   - 经 kernel_api.irq_register 注册 IRQ12 handler
   - 读数据前检查 AUX 位（键盘数据留给键盘路径）
   - 3 字节包解码：同步位验证、溢出检测、9 位有符号位移合成
   - 坐标累计并钳位到 framebuffer 边界
   - 安全不变式：注册但不解屏蔽 PIC，DSK 阶段 IF=0

3. **共享状态接口**:
   - `mouse_state_t` 结构体（magic/version/x/y/buttons/计数器/边界）
   - `g_mouse_state` 导出全局符号
   - `mouse_poll_state()` 只读快照查询接口

**验证结果**:
- mouseInit 初始化成功：`cfg byte=0x47`, `data reporting enabled`, `IRQ12 handler registered`
- FirstInit 中鼠标轮询正常：AUX 端口启用、复位、自检、流模式启用成功
- 鼠标数据包正确接收和解码（串口日志显示 "[mouse] pkt"）

**证据**: `.build_tmp/qemu_serial_mouse.log` 串口日志

**技术要点**:
- 严格遵守"任何单步失败只记日志继续执行，绝不阻塞或崩溃首次启动"准则
- handler 存活性结论：镜像会被后续模块覆盖，IRQ12 保持屏蔽避免野指针执行
- 后续集成路径：DKM 输入驱动 或 DSK 加载器增加"常驻"标志

## M4 里程碑完成状态（2026-09-15）：SAS-R0-PCQ 调度器 + DRR 恢复根

### 任务 A: SAS-R0-PCQ 调度器（ROADMAP Phase 7）
**状态**: [COMPLETED]

**实现内容**:
1. **O(1) 位图调度器** (`CODE/UTSM/sched/sched.c`):
   - 64 槽 TCB 表 + 8 CPU runqueue（当前 BSP-only），ready_bitmap ctz 选最高优先级（0 最高）
   - 侵入式 O(1) FIFO runqueue（slot 索引非指针），冻结 ABI 9 API 真实现
   - 任务栈从 arena 分配，entry/arg/stack_base 存 task_aux
2. **xv6 式栈驱动切换** (`CODE/UTSM/sched/switch.S`):
   - 抢占 = IRQ 帧落在被抢占任务自己的栈上，切换只换 rsp
   - trampoline 跳 entry；schedule/yield/sleep/exit 全路径
   - 切换路径只触碰 crypto_context 指针（禁止扫描 capability/MAC/checkpoint）
3. **LAPIC timer tick** (`CODE/UTSM/arch/x86_64/lapic_timer.c`):
   - MMIO 映射 + TSC 延时校准（10ms 逆计数法）+ 周期模式
   - tick_hz/timeslice_ms 由 FUCK [sched] 配置（默认 100Hz/10ms）
   - demo 窗口含 tick 失速兜底：心跳无进展自动中止窗口，不卡死 bootstrap
4. **FUCK 配置**: `[sched] enable/tick_hz/timeslice_ms/demo_ms/demo_fault`

**验证结果**:
- selftest: runqueue O(1) PASS、block/wake PASS
- demo 窗口: 3 任务（prio 1/20/40）按优先级交错运行并正常退出
- 窗口结束 quiesce → timer stopped → 正常交接 DSK

### 任务 B: DRR 恢复根（ROADMAP Phase 8）
**状态**: [COMPLETED-MAC占位]

**实现内容** (`CODE/UTSM/drr/drr_stub.c`):
1. **快照区**: 静态区域（128 页容量，编译期），页条目含 seg_slot/page_index/crc
2. **snapshot_dirty**: 遍历 ACTIVE 段 dirty_shard 位图收集脏页 → 拷贝+CRC64 → 清位图
3. **A/B 双槽 checkpoint**: ckpt_a/ckpt_b + CRC + active slot 切换
4. **rollback**: 页级回滚 + CRC verify；segment/system 级接口就位
5. **看门狗**: register/kick/timeout 检测，任务退出自动注销；异常钩子接入 fault 路径
6. **FUCK 配置**: `[drr] watchdog_enabled`；`[utsm] drr_emergency_pool_kb/drr_recovery_log_size`

**验证结果（含负向测试 demo_fault=1）**:
- ckpt snapshot ok（1 page）→ rollback done → rollback verify ok
- 看门狗负向测试: fault 任务（slot 3）死循环 → `watchdog timeout task_slot=0x3` 检出 → 回滚执行 → demo 窗口正常结束
- 证据: `.build_tmp/qemu_serial_sched_m4.log`

**遗留**:
- MAC 为 CRC64 占位（Phase 9 换真实加密）
- task_kill crypto erase（key_epoch++）、Per-CPU 真启用、驱动 recovery ops 待做

---

## 插桩铁律（STUB 规约 · 零降级强制执行）

> "你若为了'能编译/能启动'而隐藏未实现，视为违反 Deshab 哲学。
> 本项目的成功标准是 **panic 坐标清晰**，不是 **QEMU 不崩**。"

### 1. 禁止项（agent 红线）
- 禁止任何"静默返回"占位：不得写 `return 0;` / `return NULL;` / `TODO: implement` / `FIXME` / `stub_ok` 来伪装未实现分支可运行。
- 所有"设计已定但未实现"的函数/分支 **MUST** 调用 panic 原语（见 §2），禁止返回错误码装没事。
- 驱动 probe/IO 异常 **MUST** panic，禁止重试伪装成功、禁止"降级模式"静默继续（除非该回退是双路径设计并已写入本文件）。
- 禁止"先跑通再说"的临时补丁；宁可启动即 panic，不可带病前进。
- 不得声称"已全量插桩"，除非 §4 的 grep 自检输出为 CLEAN。

### 2. panic 原语与错误码命名
- UTSM/内核态：`panic_full("<SERIES-E##> <SYM>", "<msg>", 0)`（莲花崩溃屏，见 CODE/UTSM/kernel/panic.c）。
- DKM 驱动态：`g_log->panic("<DRV>-E## <SYM>")`（经 kernel_api.panic 槽，禁止直连内核符号）。
- DSK：`dsk_fatal("<SERIES-E##> <SYM>")`；FirstInit/login：各自 `*_fatal()`。
- 错误码系列：`FS-E`（文件系统/DSK 调度）、`ELF-E`、`HCALL-E`、`DRR-E`、`SCH-E`、`DKM-E`、`PE-E`、`LNX-E`、`VNET/ATH/XHC/E1K/NVME-E`（驱动）、`PKG-E`、`BRAND-E`、`NET-E`（FirstInit）、`LOGIN-E`、`SMP-E`（AP 启动/所有权）、`GUEST-E`（后台 guest 运行器/邮箱）。新模块按 `<MODULE>-E##` 注册到 docs/STATUS.md。

### 3. 改动节奏（防"一次性插完"幻觉）
- 每次只处理 **一个文件** 的 STUB→panic，改完输出：改动行号 + 剩余 STUB 计数。
- 骨架允许"不接线"（无人调用的 skeleton 函数），但接线后行为必须是 panic。
- 例外白名单（设计内双路径回退，允许非 panic）：APIC 自验失败回 PIC、块设备缺失回 boot module、KDE 回落 native、VMX/bzImage/kernel.krl 未配置跳过、ps2mouse 探测不到 vmmouse 回 PS/2 相对包（vmmouse 是 QEMU vmport 的绝对指针，真机没有这个后门端口）；`[boot] smp=0` / `guest_background=0` 时双 guest 后台装载退回旧的进桌面前同步路径（AP 不参与）；桌面 `kern.guest.progress` 缺席时不画进度横幅（旧内核兼容）。新增白名单项必须改本文件并说明理由。
- **panic 双模式（FUCK `[panic] mode=strict|fallback`，默认 strict）**：strict = 本规约原样执行。fallback = 全局白名单开关，用于真机/演示"带病看全貌"：
  - UTSM `panic_full(sym,msg,NULL)` 只打 `[PANIC:FALLBACK]` + 错误码 + 命中计数后返回，调用点走紧随其后的备用路径（跳过该能力/驱动/段/页面）；DKM `kernel_api.panic` / `kern.log.panic` 同样返回驱动，驱动 init 返回非零。
  - 模式经 `DSK_BOOT_FLAG_PANIC_FALLBACK (1<<4)` 下传，DSK/FirstInit/login/desktop 的 `*_fatal` 同样只留串口后返回；DSK ext4 内部 fatal 经 `__builtin_setjmp/longjmp` 中止本次操作 → 只用 FAT32 ESP。
  - **永远停机（`panic_halt`，两种模式都不返回）**：CPU 异常（带 regs）、LA57 已启用（VMM-E11，4 级页表遍历会写坏内存）、DSK 跳转失败（VMM-E15）、panic 测试钩子（UTSM-E42）。
  - 新增 panic 点必须保证 panic 调用后存在安全的继续路径（return 错误码 / continue / 置能力未就绪），否则改用 `panic_halt` 并登记到上一条。
- 启动期能力证明（attest，CODE/UTSM/kernel/attest.c）：FUCK `[attest]` `require_<name>=1`（默认）而能力未就绪 → 交接 DSK 前 panic（莲花屏，错误码=该能力登记码）。`require_<name>=0` 豁免行是设计内白名单，仅限两类：① 该构建环境不可能具备（QEMU 无 wlan/嵌套 VMX）；② STATUS.md ⬜/🔶 未落地项（实现后逐项删除豁免行）。当前豁免清单见 build/configs/*/FUCK [attest]。

### 4. 自检命令（验收只认这个输出，不认自然语言保证）
```
powershell -Command "Get-ChildItem CODE\UTSM,CODE\DKM,CODE\dsk -Recurse -Include *.c,*.h |
  Select-String -Pattern 'TODO|FIXME|stub_ok|\bstub\b|未实现|暂不支持|待实现|not implemented' |
  Group-Object Path | Sort-Object Count -Descending"
```
- 命中行 **只允许** 两类：① panic 调用/其注释（含错误码字面量）；② 本文件白名单内的设计内回退日志。
- 其余命中 = 未清干净，禁止提交。基线快照（2026-10-03）：~118 命中，绝大多数已是 panic 坐标注释；新增代码不得抬高裸命中数。

### 5. 验收格式（每次插桩提交必须附）
```
[STUB] <阶段>/<模块>/<文件>::<函数> | 改动行=<L1-L2> | 剩余STUB=<N> | “<一句疯洁标语>” | 已提交=<hash>
```
- 例：`[STUB] P1/UTSM/vmm/hypercall.c::hcall_utrw_read | 改动行=176-181 | 剩余STUB=112 | “封缄之下，未成之物当自爆”`
