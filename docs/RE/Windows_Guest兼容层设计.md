# Windows 11 Guest 兼容层设计

## 1. 目标

Windows 11 作为 Deshab 内核 VMX Guest 运行，提供两种呈现方式：

1. 全屏切换：从 Deshab 桌面进入定制 Windows Shell，再由 Guest Agent 返回。
2. 无缝应用：Windows 不显示完整桌面，只把 Trae 等顶层窗口合成到 Deshab。

Windows 文件不随源码仓库分发。构建者必须提供来自微软官方渠道的 ISO，并自行
负责许可证和激活。

## 2. 磁盘约束

最终 Deshab GPT 镜像只新增一个持久 Windows 分区：

```text
p1  DESHABBOOT  FAT32  Deshab ESP + Windows boot capsule
p2  DESHAB      ext4   Deshab 数据
p3  DESHABWIN   NTFS   完整 Windows 系统、注册表和应用
```

不创建持久 Windows ESP 或 MSR。`p3` 使用 Microsoft Basic Data GPT 类型，但
Deshab 文件管理器不枚举该分区。

“解压 ISO”不能实现为复制 ISO 目录树。ISO 中真正的系统映像位于
`sources/install.wim` 或 `sources/install.esd`，构建链执行：

1. DISM 将指定 Edition 展开到 `SYSTEM/WIN/`，该目录就是可检查的 Windows
   系统卷母本。
2. 离线注入 virtio 驱动、Guest Agent 和自定义 Shell。
3. 离线写入首阶段 TPM/Secure Boot 绕过和冻结更新策略。
4. `bcdboot` 向临时 FAT VHD 生成标准 UEFI 启动树。
5. 将启动树保存为 `SYSTEM/ESP/boot/windows-esp/` boot capsule。
6. 把处理后的 Windows 重新捕获为 `SYSTEM/windows/deshab-windows.wim`；
   打包时使用该 WIM 保留 ACL、硬链接和重解析点。
7. WSL 打包器创建 `p3` NTFS，并用 wimlib 将 WIM 应用到该分区。

boot capsule 只是启动资源，不包含 Windows 系统主体。未来 Windows profile
把它合成为只读内存 FAT ESP交给 OVMF；Windows 系统盘仍然只有 `p3`。

## 3. 构建接口

普通构建保持不变：

```powershell
.\build.ps1 -Variant dev
```

Windows Guest 构建必须使用管理员 PowerShell，并且一次只构建一个 variant：

```powershell
.\build.ps1 -Variant dev `
  -WindowsGuest `
  -WindowsIso D:\ISO\Win11_English_x64.iso `
  -WindowsEditionIndex 4 `
  -WindowsSizeGiB 80 `
  -WindowsDrivers D:\Drivers\virtio-win `
  -WindowsGuestAgent D:\Build\DeshabGuestAgent
```

首次制备生成（`SYSTEM/windows/` 和 boot capsule 均为本机生成物，不提交）：

```text
SYSTEM/WIN/Windows/...
SYSTEM/windows/deshab-windows.wim
SYSTEM/windows/metadata.json
SYSTEM/ESP/boot/windows-esp/EFI/...
```

不传 `-WindowsIso` 时，可复用上述本地缓存。ISO、WIM、ESD 和 boot capsule 均被
`.gitignore` 排除，禁止提交微软文件。

WSL 额外依赖：

```text
ntfs-3g
wimtools
```

对应命令必须提供 `mkfs.ntfs`、`wimlib-imagex` 和 NTFS mount helper。

## 4. 启动契约

Windows profile 尚需在 UTSM VMM 中实现以下契约：

- OVMF reset vector、pflash CODE/VARS。
- 2 vCPU、LAPIC/IOAPIC、ACPI/SMBIOS 和 PCIe。
- 将 boot capsule 暴露为只读内存 FAT ESP。
- 将 `DESHABWIN` 分区边界作为独立虚拟块设备暴露，禁止 Guest 访问 p1/p2。
- 安装期 i8042、GOP framebuffer、virtio-pci block/net/serial。
- Guest Agent 的 park、shutdown、窗口、输入和剪贴板协议。

在这些能力完成前，构建链产出的 NTFS 分区不代表 Windows Guest 已可启动。

## 5. 更新与安全边界

首阶段为固定版本兼容设备：

- Windows Update 自动扫描由离线策略禁用，但保留 WinSxS 和 Servicing Stack。
- Guest 默认隔离网络；应用联网应通过受控 NAT。
- 首阶段允许绕过 TPM 2.0 和 Secure Boot 检查，不宣称满足正式 Windows 11
  安全要求。
- 后续合规阶段再实现 TPM2 CRB、TCG2、PCR/事件日志、认证变量和 Secure Boot。

## 6. 验收

构建侧验收：

1. 不带 `-WindowsGuest` 的 dev 构建与分区布局不变。
2. Windows 构建只新增 `p3 DESHABWIN`，不存在第二个持久 ESP/MSR。
3. `p3` 为 NTFS，包含 `Windows/System32`。
4. p1 包含 `boot/windows-esp/EFI/Microsoft/Boot/bootmgfw.efi` 与 BCD。
5. ISO/WIM/boot capsule 不进入 Git。

运行侧验收在 VMM 平台完成后执行：OVMF 启动、双核 ACPI、Windows 首次启动、
全屏进出、Trae 无缝窗口和 Guest Agent 退出。
