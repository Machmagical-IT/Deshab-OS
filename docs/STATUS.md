# Deshab-OS 状态总表（单一事实源）

> 本文件是全项目唯一的子系统状态来源。其他文档（README/ROADMAP/ARCHITECTURE/CLAUDE.md）只引用本表，不得自抄状态。
> 状态规范：✅ 已实现 · 🔶 演示级（注明边界） · ⬜ 仅设计/未实现。
> 更新规则：状态变更必须同步本表，并附证据（串口日志路径 / QEMU 现象）。

## 内核底座

| 子系统 | 状态 | 边界/说明 | 证据 |
|--------|------|----------|------|
| 早期启动（Limine→long mode→C） | ✅ | cli/cld 纪律、early serial 非阻塞 | QEMU 启动日志 |
| IDT/异常 | ✅ | 0–255 stub 全覆盖；异常路径走 F1 莲花崩溃屏 | `arch/x86_64/idt.c` |
| PIC remap + IRQ 分发 | ✅ | remap 0x20–0x2f，handler 注册 + EOI | — |
| IOAPIC→LAPIC 路由（B7） | ✅ | apic_route=1 接管；失败自动降级 PIC | `.build_tmp/qemu_serial_*.log` |
| LAPIC timer | ✅ | TSC 校准 + 周期模式，[sched] 配置 | M4 QEMU 日志 |
| DMA 物理页分配器 | ✅ | bitmap，低 4G 钳位 | e1000 DMA 根治记录 |
| 页表映射接口（map/unmap） | ✅ | `kernel_api.mm_map_mmio` / `mm_unmap_mmio`（PCD\|PWT 独立窗口）；apic / ehci / xhci / nvme 四个驱动在用，含 BAR>4G 高位映射 | 串口 `BAR mapped via mm_map_mmio window` |
| panic 统一兜底（F1） | ✅ | DEAICUP 莲花崩溃屏（ohMyLogo.png 内嵌，背景=图片背景色）+ panic_symbol + DRR 归档；CPU 异常/驱动 panic 统一入口 | `.build_tmp/panic_screen.png` + qemu_serial_f1logo2.log |
| panic 双模式（FUCK `[panic] mode`） | ✅ | strict（默认，零降级）/ fallback（panic 点记 `[PANIC:FALLBACK]`+错误码+计数后走备用路径；经 `DSK_BOOT_FLAG_PANIC_FALLBACK` 下传 DSK/FirstInit/login/desktop/DKM）；CPU 异常/VMM-E11/VMM-E15/UTSM-E42 两模式均停机（`panic_halt`） | `.build_tmp/qemu_panic_strict.log`、`.build_tmp/qemu_panic_fallback.log`（attest 全 require=1：DRR-E31 hits=1 → DSK 收到 fallback → FirstInit） |
| boot 分级自检（F4） | ✅ | `[boot] selftest=0\|1\|2`，5 项全过 BOOT-OK | `.build_tmp/qemu_serial_f1f4f5.log` |
| 帧缓冲控制台抽象 fbcon（F2） | 🔶 | `include/utsm/fbcon.h`：像素/混合/填充/RGBA 图像/文本（panic 路径已用）；统一控制台后端 ⬜ | `CODE/UTSM/include/utsm/fbcon.h` |
| 构建系统 | ✅ | Windows build.ps1 + 跨平台根 Makefile（Linux/macOS `make dev/release/realtest`，打包走 pack_system_image.sh）；macOS 缺 mkfs.ext4 时打包不可用 | `Makefile` |

## UTSM / UTRW

| 子系统 | 状态 | 边界/说明 | 证据 |
|--------|------|----------|------|
| 封缄段表 / UUID 调谐（U1） | ✅ | 段创建/查询闭环 | `CODE/UTSM/core/segment.c` |
| UTRW Fast Path | ✅ | capability→PCKC→line 解密→seqlock | `utrw/read.c` |
| UTRW Slow Path（U2） | ✅ | EPOCH 刷新 / KEY_MISS 重派生 / POISONED+MAC_FAILED→DRR / STALE_CAP 拒绝；全分支 selftest PASS | `utrw/slow_path.c` + `.build_tmp/qemu_serial_u2slowpath.log` |
| `@sealed` 段标记（U3） | ✅ | 默认封缄；`UTSM_SEG_F_UNSEALED` 段明文直存（0 加解密开销，capability/dirty/checkpoint 照走）；linker.ld `.sealed` section + `__sealed_start/end` 符号就位 | `.build_tmp/qemu_serial_u3sealed.log` + llvm-nm |
| 密文页 dump（U4） | ✅ | `utrw_debug_dump_page(slot,page,offset)`：串口输出 64B 密文/明文 hex 对照（selftest 内置演示） | `.build_tmp/qemu_serial_u4dump.log` |
| selftest（U5） | ✅ | chacha20 KAT 落地为 RFC 8439 §2.4.2 官方向量（gen_chacha_kat.py 生成，pycryptodome 独立对拍）；"间歇 FAIL" 确认为幽灵缺陷（该测试从未合并进本树）；page MAC 确定性检查已加；全量 selftest 稳定 PASS | `.build_tmp/qemu_serial_u5kat.log` |
| 真实加密原语 | 🔶 | chacha20/KDF/MAC 已落（Phase 9 早期），PCKC schedule 待接 | `crypto/` |

## DKM 驱动系统

| 子系统 | 状态 | 边界/说明 | 证据 |
|--------|------|----------|------|
| ELF64 .drv 加载（4 stage） | ✅ | 17/17 驱动编译并装载 + driver_init（console_fb/timer/apic/acpi/pci/ahci/nvme/ehci/xhci/bootfs/vfs/fat32/devfs/e1000/virtio_net/ath9k/ps2kbd），与 manifest.json 一致 | QEMU 启动日志 `[<drv>] driver ready` ×14（ath9k/console_fb 无对应设备时走各自路径） |
| 驱动列表（timer/acpi/pci/ahci/nvme/bootfs/vfs/devfs/fat32/ps2kbd/e1000/virtio_net/console_fb/apic） | ✅ | 能力各异，见下行分项 | manifest.json |
| AHCI | ✅ | IDENTIFY/READ/WRITE DMA + ahci0 provider | — |
| NVMe | ✅ | admin 队列 + IDENTIFY + IO 队列(qid=1) + 读写/flush + PRP 跨页 + MSI/MSI-X（vector allocator 0x40–0xDF 池）+ block provider；NVME-E01..E03 零降级插桩。无控制器时 WARN 后正常 ready（设计内回退） | `CODE/DKM/nvme/nvme.c`；未挂盘时串口 `[nvme] NVMe controller not found` → `driver ready` |
| FAT32（DKM 侧） | 🔶 | 只读 BPB/FAT/目录项解析 + 根目录遍历；**无文件系统 API 层**（无 open/read/stat），当前仅 demo 级读取。大盘读写路径另由 DSK 侧 FAT32 实现承担（见下行 FS-E 表） | `CODE/DKM/fat32/fat32.c`；串口 `[fat32] driver ready` |
| e1000 | ✅ | RX/TX + DHCP/ARP/UDP/DNS（polling） | — |
| virtio_net | ✅ | legacy split-ring virtqueue（desc/avail/used）+ vq_setup + 32×2048B RX 缓冲 + TX/RX 环 + DHCP DISCOVER→OFFER selftest；VNET-E01/E02 零降级插桩。无设备时 WARN 后正常 ready（设计内回退） | `CODE/DKM/virtio_net/virtio_net.c`；未挂设备时串口 `[virtio_net] virtio-net device not found` → `driver ready` |
| 热卸载（D1） | ⬜ | 仅有 NO_UNLOAD 标志与状态机文档 | — |
| 按名 API 导出表（D3） | ✅ | `dkm_kapi_table`（magic/version/count/entries/lookup）挂在 `kernel_api->kapi` 尾部（ABI 兼容追加）；14 条按名导出（log.*/dma.alloc_pages/irq.register/irq.vector_*/mm.map_mmio/mm.unmap_mmio/apic.register_eoi/drr.*/utrw.debug_dump）；selftest 含正例调用 + 未命中负例 + TST-E21..E24 插桩 | `CODE/UTSM/dkm/kernel_api.c` + `test/utsm_selftest.c`（需 FUCK `[boot] selftest=2` 才执行该段） |
| 彩色日志（D4） | ✅ | `dkm_log_set_driver`（驱动名 FNV hash → 8 色调色板，确定可复现）；init 期间驱动日志带 ANSI 色（仅串口，BOOTLOG 纯文本）；FUCK `[debug] color_log` 总开关 | `.build_tmp/qemu_serial_d4color.log`（1150 彩色行） |
| ZTS 热更新子系统（ZTS7） | 🔶 | `CODE/DKM/zts` → `driver/misc/zts.drv`（stage3，depends fat32，provides zts）：760B 头 + ELF64 ET_REL 模块加载（R_X86_64_64/32/32S/PC32/PLT32）→ 执行/卸载/热替换；检查点（槽位引用+注册表+patch 快照）、差分更新（modify/insert/delete 变长块）、事务（内存/文件/模块 op）、ftrace 式函数替换、版本快照回滚、符号/依赖检查、A/B 状态持久化（STATEA/B + CRC32 + SEQ）；入口 = srv.zts.* + shell `zts` + settings ZTS 页 + ZTS.CNF 开机自动加载；模块仓库 `SYSTEM/zts`（打包后 ESP `/zts/`）。模块失败回滚并打印 ZTS-E##，不 panic 内核。启动自检 `[zts] selftest=1` 覆盖执行/热更/回滚/patch/差分/事务/状态，failures=0（**边界**：shell `zts` 子命令与 settings ZTS 页尚未做交互式回归；当前只在 dev 变体开自检） | `.build_tmp/qemu_zts6.log`（`[zts-test] PASS` ×9、`failures=0`）；`.build_tmp/qemu_zts4.log`（srv.zts.* ×14、desktop ready） |
| ZT5 VMX guest 子系统 | 🔶 | `CODE/ZT5`（完整源码树）+ UTSM `vmm/zt5_loader.c`/`zt5_boot.c`：**ZTS 头之外的第二个 ZT5 集成面**——装载 `SYSTEM/boot/zt5.krl`，构造 guest 页表/GDT/伪 Limine 响应（hhdm/memmap/fb/kernel_file/module/cmdline），EPT 映射 boot area/fb/modules/64MB kernel/1GB RAM，FUCK `[zt5] enable=1` 启动。已验证（WSL2+KVM nested VMX）：`_start`→GDT/IDT/PMM/VMM→APIC/SMP→VFS/RAMFS→ZTS7/AI→Stage 9→加载 desk→进入 scheduler。剩余阻塞：scheduler 后 guest 页错误（desktop 返回路径），待 guest profile/用户态页表继续定位 | `.build_tmp/qemu_zt5_kvm12.log`（`[BOOT] _start() entered`→`Stage 9`→`[LOAD] Limine module loaded successfully`→`scheduler_start`） |
| Windows 11 Guest 制备/分区 | 🔶 | 构建侧已接入 `-WindowsGuest`：官方 ISO 经 DISM 展开/驱动注入/离线策略→本地 WIM，`bcdboot` 生成 boot capsule；GPT 只新增一个 `DESHABWIN` NTFS 持久分区，启动资源位于 p1 `boot/windows-esp`，不创建持久 Guest ESP/MSR。**运行侧未完成**：OVMF profile、PCI/ACPI/APIC、virtio-pci 与 Guest Agent 仍待实现，当前不得宣称 Windows 可启动 | `tools/windows/prepare_windows_guest.ps1`；`CODE/linux/pack_system_image.sh`；`docs/RE/Windows_Guest兼容层设计.md` |
| ACPI 电源（关机/重启） | ✅ | `acpi.drv` 解析 FADT → DSDT `_S5_` AML 包 → PM1a/PM1b_CNT 写 `SLP_TYP\|SLP_EN` 进 S5；复位走 FADT `reset_reg` (ACPI 2.0+ GAS)。服务 `srv.acpi.poweroff` / `srv.acpi.reboot`，消费方：shell `poweroff/reboot`、cmd `shutdown/reboot`、桌面托盘+开始菜单电源钮（`desktop_poweroff/reboot`）。母本 `SYSTEMCOPY/OpenXJ380/driver/power.cpp`（Apache-2.0，DSDT 解析与 VM 端口兜底照搬）。服务缺席走设计内回退（shell/cmd 停机自旋或 0xCF9 三击，桌面退回 `g_quit`） | `.build_tmp/qemu_powertest.log`：`[cmd] running AUTOEXEC.BAT`→`[acpi] entering S5`→QEMU 自行退出（exit=0，非 timeout） |
| Intel HDA 音频 | 🔶 | `CODE/DKM/hda/hda.c` → `driver/hda/hda.drv`（stage3，depends pci）：PCI class 04:03/04:01 探测 → GCTL 复位 → Immediate Command 通道（无 CORB/RIRB DMA）→ AFG 子节点遍历选 DAC + Speaker/HP/LineOut 引脚 → BDL + Interrupt/Normal TRB 同步回放（轮询 BCIS，不注册 IRQ）。服务 `srv.audio.play`/`stop`/`info`（消费契约 `CODE/DKM/dkm_audio.h`）。FUCK `[drivers] hda=1` + `hda_selftest=1` 播 440Hz 自检。**边界**：只做回放（无录音/音量混音器）；无 HDA 控制器时静默 ready（可选驱动） | `.build_tmp/qemu_realtest_hda.log`：codec `1af4:0022`、DAC nid=2、PIN nid=3、`[hda] selftest rc=0x0`、`srv.audio.*` ×3 注册 |
| USB HID 键盘（xHCI） | 🔶 | `xhci.drv` 新增 HID boot 键盘：端口扫描按配置描述符判型（MSC / HID Boot Keyboard 0x03:01:01），HID 走 Interrupt IN + `SET_PROTOCOL(boot)`/`SET_IDLE`，8 字节报告差分翻译成 PS/2 Set-1 扫描码，经 `srv.input.kbd.push` 注入 ps2kbd 的 ring——所有消费者无感知。`srv.input.usb.pump` 由 ps2kbd 在每次 `kbd_poll` 前调用（无需调度任务）。**边界**：仅 Root Hub 直连（外部 hub 需 hub 类驱动 + route string，未做）；只支持键盘（无 USB 鼠标）。顺带修复两个 xHCI 真实缺陷：HCSPARAMS1 偏移（0x00→0x04，原值使 MaxPorts 恒为 1）、Address Device 必须 BSR=0 收尾（否则 Configure Endpoint 返回 0x13） | `.build_tmp/qemu_hidtest.log`：`[xhci] HID keyboard ready`、`srv.input.usb.pump registered`、QMP `sendkey` 后 `[cmd] kbd event via srv.input.kbd.poll` |

## DSK 主内核 / 用户态

| 子系统 | 状态 | 边界/说明 | 证据 |
|--------|------|----------|------|
| 启动 Logo（静态莲花） | ✅ | 旋转加载环已移除 | QEMU 现象 |
| FirstInit 向导 | ✅ | 输入+SHA256+写盘闭环；**联网不是进系统的前置条件**：检测到无无线设备直接跳过网络页，有设备时提供 Skip 按钮（渲染+鼠标+键盘三路），Connect 仍为零降级 NET-E01 panic | `.build_tmp/d2.log`（`no wireless device; skipping network setup page` → `USER.CONF` → login → desktop）|
| 可跳过 FirstInit（F5） | ✅ | `[dsk] skip_firstinit=1` 直达 desktop，跳过向导与登录 | `.build_tmp/qemu_serial_f1f4f5.log` |
| mouseInit（PS/2 鼠标） | ✅ | IRQ12 + 包解码 + 光标（M1） | `.build_tmp/qemu_serial_mouse.log` |
| desktop.elf | ✅ | 三页面+窗口+任务栏+双缓冲；打字机效果已删（整屏一次绘制）；DSK-E01 fatal 插桩 | — |
| 应用启动链路修复 | ✅ | ① 内置 ELF 应用（shell/cmd/browser/settings/proedit）原用 `elf_name`（FS 根 8.3 名）但文件在 `system/deshab64/`→ 改 `elf_path` 按路径加载；② 桌面 `g_elf_image` 仅 1MB 装不下 cmd.elf(4.27MB)/shell.elf(1.9MB)→ 改固定地址 0x8800000 起 8MB（放 .bss 会撑爆 DSK 的 8MB ibuf）。9 类应用（ELF/窗口/PE）串口钩子启动全过。**BUG-20261018-P01 回归修复**：P4f 化删掉 cmd 的 `dsk_entry` → cmd.elf e_entry=0 → 点击 CMD 跳到 0x87ff000 崩溃停机；已恢复双入口 + ELF loader 入口/边界校验 + 窗口应用启动切回 DESKTOP 页 + 无 guest 不切空白 IDE 页 + PS/2 配置字节安全回退 | `.build_tmp/vfy2_serial.log`（`serial a-f`→`launching external elf`/`app launched`/`pe: run_windowed`）；`.build_tmp/mx_cmd.log`（修复前 `[PANIC] rip=0x87ff000` → `halt`） |
| login/FirstInit 白卡主题 | ✅ | 卡片改纯白底 + 深色文字 + 浅蓝输入框（原深色主题），中英文字/按钮/下拉/网络页联动改色 | `.build_tmp/m3fix.out`（向导→登录同启动内通过） |
| diskinfo 鼠标修复 | ✅ | 输入循环原无脑读 0x60 会吞鼠标包（退出后 PS/2 包错位→光标乱走"反转"）；改按 AUX 位分流 + SDK 光标（`dsb_cursor`/`dsb_mouse_poll`，Y 方向正确） | `CODE/tools/diskinfo/main.c` |
| 开始菜单（M3） | 🔶 | Aqua Launchpad：本机条目仍来自 STARTMENU.CNF。打包前 `tools/desktop/scan_guest_menus.ps1` 扫 Linux `/usr/share/applications`（含 persist upper）和 `SYSTEM/WIN` 开始菜单 `.lnk`，写入 `SYSTEM/ESP/GUESTMENU.CNF`；启动台接在本机条目后面（橙=Linux，蓝=Windows），超出一屏用方向键/PgUp/PgDn。点 Linux 项走原 exec_async；点 Windows 项 `DESK-E15`（逐进程拉起 Windows guest 未接线）。Dock 仍只钉本机前 8 项 | 扫描脚本 + desktop 编译；未做整链 QEMU |
| macOS Aqua 外壳 | ✅ | 替换 Win11 任务栏，无风格开关（`style=` 删除，KATE `fluent=0` 回退保留）：毛玻璃顶栏（Apple 菜单/应用名/窗口菜单/Wi-Fi/控制中心/时钟）、Dock（弹簧放大 48→72、运行点、启动弹跳、气泡）、Launchpad、红黄绿三钮圆角窗口 + 打开/关闭/最小化/恢复/缩放动画、右侧桌面图标列。`aqua_draw.c` `menubar.c` `dock.c` `launchpad.c`，见 docs/桌面设计.md §12 | `.build_tmp/aqua_1_desktop.png`…`aqua_5_control_center.png`、`.build_tmp/aqua_qemu.log`（`[cfg] shell=aqua` → `desktop ready`，无 PANIC） |
| 快速设置卡片（M3） | ✅ | 托盘音量钮弹出：音量/亮度滑块（du_wf_slider），亮度真实调暗（apply_brightness）；开合与 WiFi 卡片互斥 | `.build_tmp/m3_serial.log` |
| Fluent UI 组件扩容（M3） | ✅ | `fluent_ui.h` 增 7 组件（复选框/进度条/pill/分隔线/应用瓦片/菜单行/徽标）+ 11 图标（APPS/CLOCK/INFO/DISK/PICTURE/MUSIC/GAME/CHIP/REFRESH/LOCK/TRASH） | `CODE/UTSM/include/utsm/fluent_ui.h` |
| PE 系统工具（M3） | ✅ | `CODE/tools/pe-tools/` 6 工具（build.ps1 mingw g++）：GUI sysinfo/clock/paint/notes（program/，开始菜单启动）；CLI pciscan/portio（program/，CMD 运行，串口输出）。导入面保持在 pe_shim 表内 | `.build_tmp/m3_sysinfo.ppm`、`[shim] BitBlt first frame` + `pe: exited` |
| ELF 磁盘工具（M3） | ✅ | `CODE/tools/diskinfo/` → SYSTEM/bin/diskinfo.elf（diskpart 风格只读：block 设备/GPT 分区/FAT32 容量）；开始菜单 bin/xxx.elf 全屏启动 | `.build_tmp/m3_diskinfo.ppm` |
| 目录约定（M3） | ✅ | `bin/`=ELF 工具，`program/`=exe；cmd.elf PATH 扩展为 根→bin/→program/；pack 脚本把 bin/program/lib/*.CNF/AUTOEXEC.BAT 一并落 FAT32 ESP | `CODE/linux/pack_system_image.sh` |
| SATA 边车镜像 | ✅ | `make_sata_sidecar.sh` 每次构建自动重建（旧 mkfat32.ps1 缺失导致 login.elf 长期缺盘的静默降级已根治） | build.ps1 |
| shell.elf | ✅ | 25+ 命令（ls/cat/cp/mv/run/probe/test/pci…） | — |
| login.elf | ✅ | USER.CONF + skip_login；ohMyLogo 开场屏 + 严格错误插桩（LOGIN-E01..E05 fatal，零降级） | `.build_tmp/login4.png` + qemu_serial_login4.log |
| PE/EXE 兼容层 | ✅ | PE32+ 原生 / PE32 走 x86emu32 | — |
| Linux 兼容层 | 🔶 | VMM+park-and-resume；VMX 实机验证待 VT-x/KVM | — |
| Pacman | 🔶 | Arch rootfs 内置 pacman（v7.1.0/libalpm v16）；软件源指 Arch 官方仓库的中国镜像（清华/中科大/阿里/腾讯/华为云，`SigLevel=Never`）。**`pacman -Sy` 同步、`-Ss/-Si` 查询、`-S` 真实安装、跨重启持久化全部可用**。可写层 = guest 启动 wrapper 给 `/usr` `/etc` `/var` 各挂一层 overlay，**upper 落在 `/dev/vdc`（deshab-persist ext4）**，写的包在 guest 重启后仍在；`/dev/vdc` 不可用时退回 tmpfs（可用但重启即失，串口 WARNING）。`/var/lib/pacman` 原是指向 tmpfs 的符号链接（只读时代遗留），启动时替换为真实目录，已装包清单因此落进上层持久化。镜像内 `/usr/local/share/deshab/pacman-local` 作只读母本，首次启动播种——此前构建脚本把本地库整个丢掉，导致 pacman 认为系统为空、装任何 base 系依赖都报 `exists in filesystem`。**持久卷位置**：`SYSTEM/DATA/persist/deshab-persist.img`（ext4 分区内的文件），由 UTSM `vmm/ext4_blkmap.c` 只读 extent 元数据映射成块设备（避开 FAT32 4GiB 上限）；遗留 ESP 布局仍支持。**已知约束**：① 卷由 `mkfs.ext4 -O ^has_journal` 建且开 `metadata_csum`，无日志下写事务被打断会使 inode 校验和不符（历史 `iget checksums fail` 来源），故挂载前跑 `fsck.ext4 -p`；② 持久卷写满时 pacman 只报 `transaction aborted`，wrapper 打印 `persist free=<MB>` 并在 <512MB 预警；③ 挂载点必须在可写路径（用 `/tmp/persist`，guest 根只读）。guest 内网段固定 slirp `10.0.2.0/24`（DNS `10.0.2.3`） | 三次启动周期实测（每次丢弃全部 tmpfs，仅保留 vdc）：cycle1 装 less → 重启后 `LESS_RUN=less 710` → 装 git → 再重启后 `LESS_RUN`/`GIT_RUN=git version 2.56.1` 同时仍在，`pacman -Q` 139→146；ext4 映射对拍：2GB 持久卷（15 extent 含 Uninit 段）按扇区重建后 md5 与真实文件一致 |

## 调度 / 恢复

| 子系统 | 状态 | 边界/说明 | 证据 |
|--------|------|----------|------|
| SAS-R0-PCQ 调度器 | 🔶 | O(1) 位图 + LAPIC tick + xv6 式切换 ✅；BSP-only，per-cpu 多核 ⬜ | `.build_tmp/qemu_serial_sched_m4.log` |
| task_kill crypto erase | ⬜ | key_epoch++ 待做 | — |
| DRR checkpoint（R1） | ✅ | dirty shard → dma 页快照 + A/B 双槽；提交协议修复：`active → seq → CRC`（CRC 最后写，唯一提交字），recover 取合法槽中 seq 最大者；槽记录 `snap_gen/snap_crc` 与快照区头绑定，代际不一致拒绝回滚。CRC64 表补全为 CRC-64/XZ（KAT `995DC9BBDF1939FA`） | `.build_tmp/qemu_serial_abslot.log`（新证据；旧 `qemu_serial_r4ab.log` 早于当前 utsm.elf，不再作为证据） |
| DRR 回滚（R4） | ✅ | page/segment/system 三级闭环：`drr_rollback_segment`/`drr_rollback_system`（CRC64+keyed BLAKE2b 页 MAC，失败段 POISONED；双槽皆坏按 §8.2 升级复位）；页条目校验 `used/region_idx` 上界、段内页号、目标地址重算（不再信任 `cipher_vaddr`）；元数据 fallback 与数据区代际不符时拒绝（system 级 dry-run 返回 -5），不静默恢复不一致数据 | `.build_tmp/qemu_serial_abslot.log`（SELFTEST PASS） |
| DRR 看门狗（R2） | ✅ | timeout 检出→回滚/复位；负向测试 PASS | M4 QEMU 日志 |
| Emergency Pool（R3） | ✅ | 64KB 编译期独立池；`drr_emergency_pool_kb`/`drr_recovery_log_size` FUCK 接线 ✅（运行期钳位） | `.build_tmp/qemu_serial_r3b.log` |

## 万物互联（DKM-BUS / D6-D7，2026-10-10 立项）

> 设计定稿：[RE/万物互联与服务总线设计.md](./RE/万物互联与服务总线设计.md)。目标形态：没有主内核，一切皆 DKM 模块，模块间经按名服务总线（kern.\*/srv.\*）互联；DSK 退役；调度器常驻 + 真 IRQ 输入。

| 项 | 状态 | 说明 | 证据 |
|----|------|------|------|
| P0 manifest 依赖图强制 | ✅ | depends 校验 + 同 stage 不动点排序（DKM-E44/E47/E48）；VMM 路径 pci+ahci 顺序豁免（dsm_load_named_boot，白名单登记） | 正向 `.build_tmp/p1_serial.log`（4 stage 全加载）；负向 `.build_tmp/p0_neg.log`（假 depends→`DKM-E44 DEPENDENCY NOT SATISFIED` 莲花屏） |
| P1 服务总线 dkm_bus | ✅ | 双向注册-消费（BUS-E01..E05 全 panic）、kern.* 播种 22 条、provider 上下文（存 desc 防悬垂）、stage≥4 provides 校验（DKM-E45）、`/dev/services`、kapi 兼容层保留 | `.build_tmp/p1_serial2.log`（`[BUS] count=0x16`、`[UTSM] selftest bus ok`、kapi 14 条 ok、老 14 驱动不变） |
| P2 fat32.drv 实装 + ext4.drv | 🔶 | fat32.drv 全 R/W 栈（GPT 分区感知，移植 dsk_loader.c）+ 5 服务；ext4.drv（从 dsk/ext4.c 移植）+ 2 服务；UTSM 侧 PE reader/disk_log 改总线（**disk_log 在 GPT 盘上的写 bug 顺带修复**）；dsk_loader.c 死 FAT32 工具链已删（1205→657 行，仅留 deshab.elf 装载路径）。**DSK 侧（dsk/main.c）总线化下沉 P5**——DSK 整体退役，重写其自带 FAT32 属一次性工作且与在飞的 M3 WIP 冲突 | `.build_tmp/p2_serial4.log`（`[fat32] FAT32 volume live`、`srv.fs.fat32.*`×5、`srv.fs.ext4.*`×2、`disk_log: BOOTLOG.TXT written`、`[DSK] boot`→`[FirstInit] boot` 无回归） |
| P3 加载器 FS 化 + stage4 + ps2mouse.drv | ✅ | manifest.c：DSM_MAX_STAGES 6 / PER_STAGE 12、`dsm_read_module_fs`（boot module 优先 → srv.fs.fat32 回退，双源皆无 DKM-E46，load_named 同）；ps2mouse.drv 新建（stage4，IRQ12+srv.input.mouse.poll，provides input.mouse）；manifest 新增 stage4 session；DSK 删 mouseInit 调度块 | `.build_tmp/p3_serial3.log`（`[DSM] module from FS`、`[ps2mouse] srv.input.mouse.poll registered`、`loading mouseInit`=0 次、`[FirstInit] boot` 无回归） |
| P4 应用模块 DKM 化（4a-4f） | ✅ | 六个 app 全部 .drv 化：**4a netman**、**4b shell**、**4c firstInit**、**4d login**、**4e desktop**（20-TU 多源）、**4f cmd**（PE/EXE 前端）。均 driver_init 注册 `srv.<mod>.run`、ctx 消费点改总线（reserved[]/api+0xA8 消灭）；DSK 经 bus@0xF0 拉起。载入时 `[DSM] module from FS`。顺带修复：fat32.drv 11 字符路径组件匹配 bug；UTSM arena 64→128MB（stage4 大模块 + Linux guest 挤压，见 config.h）；make 漏 config.h 依赖导致 arena.o 陈旧（已清 .o 强制重编） | 4a-e 各 log + **4f** `p4f_v4.log`（`[DSK] invoking srv.cmd.run`→`[cmd] boot`→block/PE service ok→AUTOEXEC→`[desktop] boot`） |
| P5 DSK 退役 + session.drv | ✅ | **session.drv**（会话排序器，stage4，`srv.session.run`）移植 DSK 编排（FUCK/firstInit/netman/login/desktop/cmd），全经服务总线；UTSM 加载完 stage4 后调 `srv.session.run`。**DSK 物理退役**：删 `CODE/UTSM/kernel/dsk_loader.c`（1205→660 行时的 DSK 装载路径）、`CODE/dsk/` 整个目录、`deshab.elf`、limine `dsk:main` 模块、build.ps1/Makefile 的 DSK 构建段；`utsm_probe_info_get` 迁至 probe.c。启动链无任何 `[DSK]` 行。简化项（登记）：splash 文本版（无 dcplogo/xjlogo 图像）；session=ask/kde 回落 native（KDE guest 会话待接）。`dsk_boot_context` 因外部 app ELF ABI（desktop launcher 子进程）保留于 dsk.h | `p5a_v3.log`（首启向导）、`p5b_v.log`（DSK 退役后：`[UTSM] invoking srv.session.run`→`[FirstInit] welcome drawn`，零 `[DSK]`） |
| 双 Guest 后台装载 + 进度横幅 | 🔶 | `[boot] smp=1` + `guest_background=1`：BSP 先进桌面，Linux/Windows 装载在 AP1 串行进行；`kern.guest.start`/`kern.guest.progress` 总线服务；桌面右下角横幅按 phase/percent 显示。BSP 不再执行 VMX，Linux 兼容层经邮箱代理到 AP。`smp=0`/`guest_background=0` 回旧同步路径 | 待 QEMU `-smp 2` 验证（`[desktop] desktop ready` 早于 `[GUEST] linux loader init`） |
| P6 常驻调度 + 事件循环 + 真 IRQ | 🔶 | **P6a ✅**：调度器常驻；ps2kbd IRQ1 ring + `srv.input.kbd.poll/.snapshot`；ps2mouse `srv.input.mouse.poll`。**P6b 输入总线化 ✅**：firstInit/login/shell/cmd/desktop（含 legacy ELF 源）全部改用 `srv.input.kbd.poll` / `srv.input.mouse.poll`，业务代码 `inb(0x60)` 归零（新增 `CODE/DKM/dkm_input.h` 客户端）；session.drv 经 `kern.irq.unmask` 解屏蔽 IRQ1/12 并置 IF=1；`pic_irq_unmask` 在 IOAPIC 接管时保持 PIC 屏蔽（修复双投递：每键收到两个扫描码）。**P6b-desktop 任务化 ✅**：`srv.desktop.spawn`（desktop.drv 自建任务，slot=1，prio 20，每帧 `sched->sleep_ms(1)` 让出）；session 返回 1 后 UTSM bootstrap 转 `sti; hlt` idle；`utsm_task_create` 加中断保护（抢占已开启时建任务原子）。设计文档 §1.6/§P6 | P6b `.build_tmp/p6b_qemu.log`：`[SCHED] enable (LAPIC timer)`、`[IDT] irq unmask delegated to IOAPIC routing (PIC stays masked)`、`[session] IF=1 (true IRQ input armed)`、`[kbd] srv.input.kbd.poll registered`、`[firstinit] kbd event via srv.input.kbd.poll`（输入 `test/test/test`）、`[login] kbd event via srv.input.kbd.poll`→`login success`、`[desktop] boot`→`desktop ready`→`[desktop] kbd event via srv.input.kbd.poll` + `mouse first pkt via srv.input.mouse.poll`。任务化证据 `.build_tmp/p6c_final.log`：`[desktop] task slot=1`、`[session] desktop spawned as task`、`[UTSM] session orchestrated; bootstrap idle`、`[desktop] desktop ready`、`kbd event via srv.input.kbd.poll` + `mouse first pkt via srv.input.mouse.poll`（QEMU monitor 注入按键与鼠标位移）。构建：UTSM/驱动/ELF 编译通过（`.build_tmp/p6b_build3.log`、本次 `p6c` 全量 dev 构建）；**打包阶段 WSL 在写 >0.4GB 时 VM 崩溃重启**（既有环境问题，非本次改动），本次证据用 mtools 将产物注入既有 dev 镜像并逐文件 md5 校验（`.build_tmp/p6b_verify.sh`） |

**错误码登记**：BUS-E01..E05（总线注册/查找/注销违规）、DKM-E44..E48（依赖图/加载源/容量）、SESSION-E01..E03（会话排序模块）、TST-E34..E38（总线 selftest）、TST-E39/E40（DRR-3 segment 回滚失败/校验不符）、TST-E41..E44（DRR-4 A/B fallback 失败/system 回滚失败/校验不符/回滚后活性）、TST-E45（DRR CRC64 KAT 不符）、TST-E46（recover 未选中最新提交槽）、TST-E47（槽与快照区代际不一致仍被接受/被改写）——全部零降级 panic，详见设计文档 §5。`drr_rollback_system` 在槽/区域代际不一致且复位关闭（dry-run）时返回 -5（不写页）。
**INPUT 错误码（P6b）**：INPUT-E01 `KBD POLL SERVICE MISSING`（`srv.input.kbd.poll` 缺席，firstInit/login/shell/cmd/desktop 交互入口零降级 panic，fallback 下返回让会话收尾）、INPUT-E02 `MOUSE POLL SERVICE MISSING`（`srv.input.mouse.poll` 缺席 → warn + 鼠标禁用，键盘仍可用，probe 白名单）、INPUT-E03 `IRQ UNMASK SERVICE MISSING`（`kern.irq.unmask` 缺席，session.drv panic）——客户端见 `CODE/DKM/dkm_input.h`。
**双 Guest 后台装载错误码**：`SMP-E01`（AP 低页/CR3 不可用）、`SMP-E02`（AP 启动/ICR 超时）、`SMP-E03`（AP VMXON 失败）、`SMP-E04`（非法 AP 入口）、`SMP-E05`（SMP 环境/参数块缺失）、`SMP-E11`（AP 已被调度器接管）、`GUEST-E02`（后台模式请求了不支持的 guest 组合）、`GUEST-E03`（未知邮箱 op）、`GUEST-E04`（AP 抢占 VMCS 失败）、`VMM-E42`（VMCS CPU 所有权冲突）——除 `smp=0`/`guest_background=0` 的同步回退外均零降级 panic。
**音频 / 电源 / USB 错误码**：HDA-E01..E05（控制器复位超时 / 无 codec 响应 / 无输出路径 / DMA 分配失败 / BDL 或流复位失败）——仅在 `[drivers] hda=1` 且存在 HDA 控制器时生效，无控制器属可选驱动静默就绪；XHC-E01/E02（端口复位失败 / MSC 枚举失败）沿用原有语义，HID 枚举失败只 warn 并继续下一个端口；`srv.acpi.*` 与 `srv.audio.*` 缺席是设计内回退（消费方各自兜底），不 panic。
**ZTS 错误码**：ZTS-E01..E14（模块头/ELF/重定位/符号/校验/依赖/槽位/检查点/差分/状态写/签名/执行/状态槽/参数）、ZTS-E90（内部不变量）——模块是外部输入，失败回滚+日志，不 panic 内核；详见 `docs/RE/ZTS子系统设计.md` §7。
**ZT5 guest 错误码**：ZT5-E01..E14（ELF/入口/boot area/kernel/fb/modules/RAM 分配与 EPT 映射）、ZT5-E20/E21（请求 guest 但模块缺失/启动失败）——属于启动期硬依赖，strict 模式 panic；详见 `CODE/UTSM/include/utsm/zt5_loader.h` 与 `docs/RE/ZTS子系统设计.md` §9。
**引导转圈**：SAS-E04/E05（调度器 LAPIC 定时器 / tick 处理函数）、SAS-E06 `SPINNER TASK FAILED`（深蓝引导屏的转圈任务创建失败）。转圈是优先级 0 的内核任务，16ms 一拍；guest 运行期间 `lapic_irq_quiesce` 罩住主机定时器，返回后重新 `sti`。
**probe 型缺席白名单**（登记于 CLAUDE.md §3）：无块设备时 srv.fs.fat32.* 缺席 warn 跳过；FUCK `[drivers]` 关闭模块的服务缺席由消费方 probe 显式处理；P6b 新增 `srv.input.mouse.poll` 缺席（INPUT-E02）→ 鼠标禁用、键盘继续，因为交互入口的硬依赖只有 `srv.input.kbd.poll`。

### A 方案：EFI 引导器取代 Limine（2026-10-18）

| 项 | 状态 | 说明 | 证据 |
|----|------|------|------|
| BOOTX64.EFI（CODE/boot） | ✅ | GOP 渐变背景 + 无步骤文字加载条动画（≥2.2s）；EFI SimpleFileSystem 读 FUCK/manifest/全部 .drv/fw；静态 ELF PT_LOAD 装载；自建页表（恒等 + HHDM 0xffff800000000000 + 内核高半区）；伪造 Limine 六类响应；ExitBootServices → CR3/GDT/栈 → 回填 `.limine_requests` → 跳 UTSM。错误码 BOOT-E01..E22（缺件即串口报错停机） | `.build_tmp/efi_boot3.log`、`final_efi.log`（`[boot] ...`→`[DSM]` stage0-4→`srv.session.run`） |
| 实机多盘 FAT32 选卷 | ✅ | 引导器经 EFI BlockIo 读启动卷 BPB 卷序列号，作为 `boot:volid` 伪模块下传；fat32.drv 跨块设备探测（GPT UUID / 卷序列号 / 根目录 DRIVER+BOOT 打分），不再硬编码 dev 0（实机 AHCI/NVMe 常先于 xHCI 注册） | `final_efi.log`（`[boot] vol_serial=...`、`[fat32] boot volume serial=`、`selected dev=`） |
| stage4 预加载 + 首启判定 | ✅ | manifest 声明的全部 stage4 `.drv` 由引导器预加载（SESSION-E00 根因：实机 FAT32 选错盘导致 session.drv 缺席）；session 判定补齐 `firstInit.txt` LFN 路径与 `USER.CONF` 缺失回退（避免出厂镜像 LOGIN-E03） | `efi_boot3.log`（`firstInit!=0`）、`final_efi2.log`（`USER.CONF missing -> first boot`→`[firstinit] run`） |
| 加载条动画 | ✅ | 渐变底 + 12.8% 高亮段三角波往复扫描（无任何步骤文字）；QMP 连续截图验证亮段 x 坐标 469→711→792→684→559→442→424→550→666 往复，收尾整条填满 | `.build_tmp/shots/`、`.build_tmp/analyze_splash.py` |
| 实机分辨率 | ✅ | 引导器枚举 GOP 并 SetMode 到显存 ≤8MiB 的档位（1280x800/1280x720/1366x768/1600x900/1600x1200/1024x768）；FUCK `[boot] gop_width/gop_height` 可强制；帧缓冲 4K 页 + PCD\|PWT（UC）；PixelsPerScanLine 异常兜底；desktop 增加 DESK-E05 双缓冲越界硬校验 | `gop_test3.log`、`verify_final.log` |
| 实机多盘 FAT32 选卷（desktop/cmd） | ✅ | `fat32.drv` 注册 `srv.fs.fat32.volume_info`（dev/lba_base/serial）；desktop/cmd 自带 fat32_io.h 的块读写包装为设备重定向，不再硬编码设备 0 | `multidisk5.log`（`selected dev=1`、`[desktop] fat32 volume dev=1`） |
| xHCI BOT 传输修复 | ✅ | ① **Link TRB cycle 位必须是回绕前那圈的 cycle**（写成翻转后的新值会让控制器在回绕处读到无效 Link 并永久停机——实机表现为读盘在数百次成功后永久超时，卡死阶段固定为 CSW IN）；② CSW 用独立 DMA 缓冲（原与数据共用 g_xfer_buf，13 字节状态包覆盖数据开头 → 读盘返回 "USBS" 垃圾）；③ 事件环无 Link TRB（清掉 ring_init 写入的残留项）且消费 `%size` 而非 `%(size-1)`；④ TRB TD Size 按包数设置；⑤ 短包 residual 校验、IN 后 wbinvd；⑥ BOT Reset Recovery（class reset + Clear Feature HALT）+ 单次重试，失败不再级联；⑦ 超时降为 3s | `qemu_desktop_final.log`（`timeouts=0`、`[desktop] desktop ready`；readfail=2 仅出现在查找不存在的 USER.CONF，越界 LBA 被设备拒绝后经 recovery 恢复，语义=文件缺失）；`qemu_built.log`（SATA 干扰盘 + U 盘启动 → FirstInit 欢迎页） |
| 桌面资源预载 | ✅ | 引导器把字库/壁纸/贴图作为 `asset:*` boot module 送入内存；desktop 按路径读取时优先命中预载（`dt_read_to`），消除对块设备大文件读取的依赖 → DESK-E04 根治；memmap 将全部 boot module 标为 KERNEL_AND_MODULES 防回收 | `qemu_desktop_final.log`（`asset:font16`…`[zhfont] dbf loaded (16px ok)`→`[desktop] desktop ready`） |

## 文档体系

### FS-E 系列：文件系统读写严格模式（2026-10-02）

| 项 | 状态 | 说明 | 证据 |
|----|------|------|------|
| ext4 读写驱动（CODE/dsk/ext4.c） | ✅ | GPT 定位 p2 + feature 校验 + extent 树(depth0/1)读写 + 块/inode 分配回收 + 目录项插入 + 写后回读校验；启动自检 PASS | `.build_tmp/qemu_run.log`（[EXT4] selftest PASS） |
| FAT32 大盘写修复 | ✅ | 按需 FAT 表项 RMW（fat32_fat_read/write_entry）+ 块状空闲簇扫描（fat32_find_free_cluster）；修复 rec_len +4 收缩偏移与 chunk 对齐两处错位；**根目录链满时自动扩展**（分配新簇挂链尾后写条目，不再直接 `-4` no free dir entry）| 同上（USER.CONF verify ok → firstInit.txt flip ok）|
| ESP 簇大小（打包侧） | ✅ | `-s 4`（2KiB 簇 / 64 目录项）固定：避开 `spc=8` 的 65535 簇 OVMF 拒绝边界，也避开 `spc=16` 的 FAT32 下限，同时给 ESP 根目录留出 64 项余量（`spc=1` 只有 16 项，打包内容正好塞满导致首次启动写 USER.CONF 必失败）| `CODE/linux/pack_system_image.sh`；`.build_tmp/d2.log` |
| 中文字体位图字库（.dbf） | ✅ | `build.ps1` 调用 `CODE/font/mkfont.py` 由 `simhei.ttf` 生成 `simhei_16.dbf` / `simhei_24.dbf`（GB2312+ASCII）；此前无人生成，桌面必 panic DESK-E04 FONT PACK MISSING | `.build_tmp/d2.log`（`[zhfont] dbf loaded (16px ok)` → `desktop ready`）|
| FS-E01..E07 / FS-E2x / FS-E3x 插桩 | ✅ | 写失败/校验不符/feature 不支持/路径缺失 → 莲花 panic 零降级 | `.build_tmp/qemu_run.log`（FS-E02/E06/E22 复现） |
| 向导后强制重登 | ✅ | first-boot 分支 FirstInit→persist→login；登录跳过/失败 → FS-E06 panic | 同上（login success → desktop ready） |
| NET-E01 网络页 Connect panic | ✅ | 无 wlan 栈，Connect 即 panic；Save(仅存配置) 为唯一前进路径 | 同上（NET-E01 复现，fb pmemsave 确认莲花屏） |
| 主镜像改挂 SATA（run_qemu.bat） | ✅ | DSK/AHCI 块层必须可见 GPT 盘才能访问 p2 ext4；边车退居 ide.1 | run_qemu.bat |
| 开机画面去 Logo | ✅ | 移除 Logo.png 图像，仅保留居中 "DESHAB P01" 文字；dcp/xj logo 改由 TEXTURES/ 重新生成 | build 日志 |

## DSK-E70：ibuf 尾部写零破坏（2026-10-05 · 未解）

| 项 | 状态 | 说明 | 证据 |
|----|------|------|------|
| 现象 | 🔶 | DSK 经 `dsk_load_elf` 载入 login.elf 并跳转后，image 尾部（ibuf+0x1EAE10 附近，≥ 数百字节）被清零。`.text` ≤ ~16.5KB 时雷区覆盖 .got/.bss 关键数据 → login integrity 校验 len 计算错误 → LOGIN-E01 误报；`.text` ≥ ~17KB 时雷区落在 .rodata 无害 | `.build_tmp/qemu_dbg.log`（boot 矩阵：16248B FAIL×4 / 16504B FAIL×2 / 17352、17608B OK；DSK 侧 checksum 跳转前位精确 + QMP 物理内存 dump 停机后 .text 位精确与 got[0]=0 并存） |
| login 侧加固 | ✅ | ① `__text_start/end/__expected_hash` 加 hidden 可见性 → PIE 下 lea rip 取址，len 计算零 GOT 依赖；② `dsk_e70_layout_guard` 显式将 .text 推入安全布局（白名单登记，根因修复后删除） | CODE/login/main.c |
| patch 侧加固 | ✅ | patch_login_hash.ps1 改按 PT_LOAD filesz 计算 hash 范围（与 DSK loader 加载范围/运行时 [__text_start,__text_end) 严格一致），并用 e_shstrndx 定位 .shstrtab | CODE/login/patch_login_hash.ps1 |
| 根因（写零者） | ⬜ | 嫌疑面：AHCI DMA PRDT 边界 / fat32_read_sectors 缓冲边界 / DSK .bss 内大缓冲（g_fdata 10MB、ibuf 8MB）相邻越界。写零发生在跳转后、sha256 前；确切写者与范围待断点级排查 | — |

## 文档体系

| 文档 | 状态 | 说明 |
|------|------|------|
| [STATUS.md](./STATUS.md) | ✅ | 单一状态源（本文件） |
| [ROADMAP_INSANE.md](./ROADMAP_INSANE.md) | ✅ | 五期填补计划（唯一任务编号 F/U/D/R/E） |
| [TASKBOARD.md](./TASKBOARD.md) | ✅ | 可勾选任务表 |
| [PHILOSOPHY.md](./PHILOSOPHY.md) | ✅ | 疯圣典 |
| [ROADMAP.md](./ROADMAP.md) | ✅ | Phase 0–9 历史存档 + 用户态主线 |
| [ARCHITECTURE.md](./ARCHITECTURE.md) | ✅ | 架构 + 非目标章 |
| [BOOT_SEQUENCE.md](./BOOT_SEQUENCE.md) | ✅ | B0–B4 演示版 |
| ~~系统开发策划.md~~ | 已删除 | 状态与本表矛盾，收编于此 |
