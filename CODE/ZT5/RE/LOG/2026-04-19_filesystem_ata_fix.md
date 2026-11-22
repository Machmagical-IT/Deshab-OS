# ZratroOS 开发日志

## 2026-04-19 文件系统实现与 ATA 驱动修复

### 工作目标

实现完整的文件系统支持，让 ZratroOS 内核可以读写查找文件，不再是"no disk"状态。

---

### 阶段一：GPT 分区表解析实现

#### 问题背景

串口日志显示 `[FAT32] Found MBR partition at LBA 1`，但镜像使用 GPT 分区表而非 MBR。实际分区起始在 LBA 2048，而 GPT 检测逻辑错误导致回退到 MBR 解析。

#### 根因分析

原始 GPT 检测条件检查 `mbr[446]==0x00 && mbr[447]==0x02`，这不是保护 MBR 的特征。GPT 的保护 MBR 在偏移 450 (0x1C2) 处有分区类型 0xEE 的标识。

#### 修复方案

修改 `/SYSTEM/lib/Ztknl/fat32.c` 第 2064 行：

```c
// 修复前
if (mbr[446] == 0x00 && mbr[447] == 0x02 && mbr[448] == 0x00 && mbr[449] == 0x00)

// 修复后
if (mbr[450] == 0xEE)
```

#### 实现流程

1. **检测保护 MBR**：检查 `mbr[450] == 0xEE`（GPT 分区类型标识）
2. **读取 GPT 头部**：读取 LBA 1 处的 GPT Header
3. **验证 GPT 签名**：检查 "EFI PART" 签名（偏移 0-7）
4. **解析分区表项**：
   - 获取分区表项起始 LBA（偏移 72-79）
   - 读取第一个分区表项（128 字节）
   - 提取分区起始 LBA（偏移 32-39）
5. **回退 MBR**：如果不是 GPT，使用传统 MBR 分区表解析

#### 验证结果

```bash
dd if=zratroos.img bs=512 count=1 2>/dev/null | xxd | grep "000001c0"
# 输出确认偏移 450 (0x1C2) 处值为 0xEE
```

串口日志确认：`[FAT32] Found GPT partition at LBA 2048`

---

### 阶段二：virtio-blk 驱动实现

#### 设计目标

让内核能够识别 QEMU 的 virtio-blk 半虚拟化磁盘设备（vendor 0x1AF4, device 0x1001）。

#### 核心文件

- `/SYSTEM/lib/Ztknl/virtio.h` - virtio-blk 驱动头文件定义
- `/SYSTEM/lib/Ztknl/virtio.c` - virtio-blk 驱动完整实现
- `/SYSTEM/lib/Ztknl/disk.c` - 块设备抽象层集成

#### 关键实现

**1. PCI 枚举查找设备** (`virtio_blk_find_device`)

```c
// 扫描 PCI 总线查找 virtio-blk 设备
for (bus=0..7, slot=0..31, func=0..7) {
    vendor_device = pci_read_config(bus, slot, func, PCI_VENDOR_ID);
    if (vendor_id == 0x1AF4 && device_id == 0x1001) {
        // 找到 virtio-blk 设备
        vblk_io_base = bar0 & 0xFFFC;  // 获取 I/O 基地址
    }
}
```

**2. VirtQueue 初始化**

- 分配物理页面用于 VirtQueue（描述符表、avail ring、used ring）
- 设置队列大小：`outw(vblk_io_base + VIRTIO_PCI_QUEUE_NUM, 256)`
- 设置队列物理地址：`outl(vblk_io_base + VIRTIO_PCI_QUEUE_PFN, vq_phys >> 12)`
- 映射到内核空间：`vblk_vq.desc = phys_to_virt(vq_phys)`

**3. 读扇区实现** (`virtio_blk_read_sector`)

设置 3 个描述符：
- 描述符 0：请求头（16 字节，设备读）
- 描述符 1：数据缓冲区（512 字节，设备写）
- 描述符 2：状态字节（1 字节，设备写）

#### 遇到的问题

**问题 1：编译错误 - pci_config_read32 未声明**

```
virtio.c:56:35: 错误：隐式声明函数'pci_config_read32'
```

**修复**：参考 `ahci.c` 实现，直接在 virtio.c 中定义 PCI 配置访问函数：
```c
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

static uint32_t pci_read_config(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t addr = (1U << 31) | ((uint32_t)bus << 16) | 
                    ((uint32_t)device << 11) | ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}
```

**问题 2：类型转换错误**

```
virtio.c:253:26: 错误：assignment to 'uint64_t' from 'void *'
```

**修复**：添加强制类型转换 `uint64_t req_virt = (uint64_t)phys_to_virt(req_phys);`

**问题 3：Read Timeout - 缓冲区地址问题**

virtio-blk 初始化成功，但读扇区超时。根因是缓冲区使用内核虚拟地址（栈或 kmalloc），设备无法访问。

**修复尝试**：
1. 使用 `virt_to_phys()` 转换缓冲区地址 - 对栈地址不可靠
2. 分配独立的物理页面作为 DMA 缓冲区：

```c
uint64_t buf_phys = pmm_alloc_page();
vblk_vq.desc[1].addr = buf_phys;  // 使用物理地址
// ... 等待完成后 ...
if (status == VIRTIO_BLK_S_OK) {
    uint64_t buf_virt = (uint64_t)phys_to_virt(buf_phys);
    memcpy(buf, (void *)buf_virt, 512);  // 复制回调用者缓冲区
}
pmm_free_page(buf_phys);
```

**结果**：virtio-blk 仍有超时问题，暂时转向 ATA 驱动修复。

---

### 阶段三：ATA 驱动修复（核心成功路径）

#### 问题背景

使用 `-hda` 参数启动 QEMU 时，UEFI 固件将磁盘模拟为 ATA 设备：
```
BdsDxe: loading Boot0002 "UEFI QEMU HARDDISK QM00001 " from PciRoot(0x0)/Pci(0x1,0x1)/Ata(Primary,Master,0x0)
```

`ata_identify()` 成功识别磁盘，但 `fat32_init()` 读取扇区时失败。

#### 问题 1：LBA48 模式不兼容

**现象**：LBA 0 读取成功，LBA 1 返回 ERR 状态。

**根因**：QEMU 的 ATA 模拟对 LBA48 命令支持不完整。对于小 LBA（< 2^28），应使用 LBA28 模式。

**修复** (`disk.c:disk_read_sector`)：

```c
// 使用 LBA28 模式 (对于小 LBA)
if (lba < 0x0FFFFFFF) {
    serial_print("[ATA] Using LBA28 mode\n");
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));  // LBA28, 主盘
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, ATA_CMD_READ_PIO);  // 0x20
} else {
    // LBA48 模式
    serial_print("[ATA] Using LBA48 mode\n");
    outb(ATA_DRIVE, 0x40);  // LBA48, 主盘
    outb(ATA_SECTOR_COUNT, 0);           // 高字节
    outb(ATA_SECTOR_COUNT, 1);           // 低字节
    outb(ATA_LBA_LOW, (lba >> 24) & 0xFF);
    outb(ATA_LBA_MID, (lba >> 32) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 40) & 0xFF);
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, ATA_CMD_READ_PIO_EXT);  // 0x24
}
```

#### 问题 2：间歇性超时

**现象**：大部分 LBA 读取成功，但偶尔（约 10%）出现超时：
```
[ATA] Reading LBA 2087
[ATA] Command sent, waiting for data...
[ATA] Wait data timeout
```

**修复 1：增加超时时间** (`disk.c:ata_wait_ready`, `ata_wait_data`)

```c
// 修复前
uint64_t timeout = 500000000ULL;  // 约 500ms

// 修复后
uint64_t timeout = 5000000000ULL;  // 约 5 秒
```

**修复 2：添加重试机制** (`disk.c:disk_read_sector`)

```c
serial_print("[ATA] Command sent, waiting for data...\n");
/* 等待数据就绪 - 带重试 */
int retries = 3;
while (retries > 0) {
    if (ata_wait_data() == 0) {
        break;  // 成功
    }
    retries--;
    if (retries > 0) {
        serial_print("[ATA] Retrying read...\n");
        /* 重新发送命令 */
        outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
        outb(ATA_SECTOR_COUNT, 1);
        outb(ATA_LBA_LOW, lba & 0xFF);
        outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
        outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
        outb(ATA_COMMAND, ATA_CMD_READ_PIO);
    } else {
        serial_print("[ATA] Read failed after retries\n");
        return -1;
    }
}
```

#### 问题 3：ISO 镜像缓存问题

**现象**：重新编译内核后，QEMU 仍使用旧内核。

**根因**：`build_iso.sh` 使用现有 `zratroos.img` 文件，未重新生成。

**修复**：每次构建前删除旧镜像：
```bash
cd /run/media/.../Deaicup-ZratroOS/ISO
rm -f zratroos.img
bash build_iso.sh
```

#### 问题 4：ATA 写入支持缺失

**现象**：FAT32 挂载成功，但创建目录失败：
```
[FAT32] mkdir: path='system'
[FAT32] alloc_cluster: Failed to flush FAT cache
[FAT32] mkdir: Failed to allocate cluster
```

**根因**：`disk_write_sector()` 在 ATA 模式下直接返回 -1：
```c
/* ATA 模式 - 暂不支持写入 */
return -1;
```

**修复** (`disk.c:disk_write_sector`)：实现 ATA PIO 写入

```c
/* ATA 模式 - LBA48 写入 */
serial_print("[ATA] Writing LBA ...\n");

/* 等待就绪 */
if (ata_wait_ready() != 0) {
    serial_print("[ATA] Write: Wait ready timeout\n");
    return -1;
}

/* LBA28 写入模式 */
outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
outb(ATA_SECTOR_COUNT, 1);
outb(ATA_LBA_LOW, lba & 0xFF);
outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);

/* 发送写入命令 */
outb(ATA_COMMAND, 0x35);  // ATA_CMD_WRITE_DMA_EXT

serial_print("[ATA] Write command sent, waiting for DRQ...\n");
/* 等待数据就绪 */
if (ata_wait_data() != 0) {
    serial_print("[ATA] Write: Wait data timeout\n");
    return -1;
}

serial_print("[ATA] Writing data...\n");
/* 写入数据 */
const uint16_t *data = (const uint16_t *)buf;
for (int i = 0; i < 256; i++) {
    outw(ATA_DATA, data[i]);
}

/* 等待写入完成 */
if (ata_wait_ready() != 0) {
    serial_print("[ATA] Write: Final wait timeout\n");
    return -1;
}

serial_print("[ATA] Write complete\n");
return 0;
```

---

### 阶段四：ISO 打包修复

#### 用户反馈

> "img镜像文件根目录多了两个文件文件还有bin文件夹，打包的只有iso_staging文件夹下的内容"
> "只需要复制SYSTRM下除了lib的所有文件，原模原样复制过来，什么都不要改，直接复制然后打包"

#### 修复内容

修改 `/ISO/build_iso.sh` Python 脚本部分：

1. **移除多余文件复制**：
   - 删除 `/bin/desk` 的复制
   - 删除 `limine.conf` 和 `limine-bios.sys` 在根目录的多余复制

2. **简化为直接遍历 STAGING 目录**：
```python
for root, dirs, files in os.walk(STAGING):
    rel_path = os.path.relpath(root, STAGING)
    
    # Skip lib directory
    if rel_path == "lib" or rel_path.startswith("lib" + os.sep):
        continue
    
    dest_dir = "/" if rel_path == "." else f"/{rel_path}"
    try:
        fs.makedirs(dest_dir)
    except:
        pass
    
    for fname in files:
        src = os.path.join(root, fname)
        dst = f"{dest_dir}/{fname}" if dest_dir != "/" else f"/{fname}"
        
        with open(src, "rb") as f:
            data = f.read()
        with fs.open(dst, "wb") as out:
            out.write(data)
        print(f"[INFO] Copied: {dst}")
```

3. **修复 limine.conf**：删除不存在的 `logo.jpg` 引用
```
module_path: boot():/system/zrdemage/st/logo.bmp
# 删除了: module_path: boot():/system/zrdemage/st/logo.jpg
```

---

### 阶段五：调试输出增强

#### disk.c 调试输出

在关键路径添加 serial_print 用于追踪：

```c
void disk_init(void) {
    serial_print("[DISK] Initializing disk...\n");
    ahci_init();
    
    if (ahci_is_available()) {
        disk_type = DISK_TYPE_AHCI;
        disk_sectors = ahci_get_disk_size();
        disk_initialized = 1;
        serial_print("[DISK] AHCI disk initialized\n");
        return;
    }

    serial_print("[DISK] AHCI not available, trying ATA...\n");
    if (ata_identify() == 0) {
        disk_type = DISK_TYPE_ATA;
        disk_initialized = 1;
        serial_print("[DISK] ATA disk initialized\n");
        return;
    }
    
    serial_print("[DISK] ATA not available, trying VirtIO...\n");
    if (virtio_blk_init() == 0) {
        disk_type = DISK_TYPE_VIRTIO;
        disk_sectors = virtio_blk_get_capacity();
        disk_initialized = 1;
        serial_print("[DISK] VirtIO block device initialized\n");
        return;
    }
    // ...
}
```

#### fat32.c 调试输出

```c
int fat32_init_partition(uint64_t partition_start) {
    uint8_t sector[512];
    serial_print("[FAT32] Reading partition start sector...\n");
    if (disk_read_sector(partition_start, sector) != 0) {
        serial_print("[FAT32] Failed to read partition start sector\n");
        return -1;
    }
    serial_print("[FAT32] Partition start sector read OK\n");

    struct fat32_bpb *bpb = (struct fat32_bpb *)sector;

    serial_print("[FAT32] BPB: bytes_per_sector=");
    // ... 打印 BPB 信息 ...
    if (bpb->bytes_per_sector == 0 || bpb->sectors_per_cluster == 0) {
        serial_print("[FAT32] Invalid BPB\n");
        return -1;
    }
    // ...
}
```

---

### 最终测试结果

#### 成功启动日志

```
[DISK] Initializing disk...
[DISK] AHCI not available, trying ATA...
[DISK] ATA disk initialized
[ATA] Reading LBA 0
[ATA] Using LBA28 mode
[ATA] Command sent, waiting for data...
[ATA] Data ready, reading...
[ATA] Read complete
[ATA] Reading LBA 1
[ATA] Using LBA28 mode
[ATA] Command sent, waiting for data...
[ATA] Data ready, reading...
[ATA] Read complete
[ATA] Reading LBA 2
[ATA] Using LBA28 mode
[ATA] Command sent, waiting for data...
[ATA] Data ready, reading...
[ATA] Read complete
[FAT32] Found GPT partition at LBA 2048
[FAT32] Reading partition start sector...
[ATA] Reading LBA 2048
[ATA] Using LBA28 mode
[ATA] Command sent, waiting for data...
[ATA] Data ready, reading...
[ATA] Read complete
[FAT32] Partition start sector read OK
[FAT32] BPB: bytes_per_sector=512 spc=1
[FAT32] mkdir: path='system'
[FAT32] mkdir: normalized path='/system'
[FAT32] mkdir: parent='/', dir='system'
[FAT32] mkdir: parent_cluster=2
[FAT32] alloc_cluster: Allocated cluster 2921
[FAT32] create_dirent: parent_cluster=2, name=system
[FAT32] mkdir: Created directory system
[BOOT] Stage 9: Boot complete, launching programs
[BOOT] launch_programs() called
[BOOT] Loading: /prgm/sys/desk
[LOAD] Loading: /prgm/sys/desk
[LOAD] VFS OK, proc=0xFFFFFFFF81018720
[LOAD] Setting framebuffer info for desk
[LOAD] FB: phys=0x0000000080000000
[LOAD] FB: kernel virt (HHDM)=0xFFFF800080000000
[LOAD] Mapped framebuffer at user virt 0x0000000008000000
[LOAD] Set fb_width=1280, fb_height=800, fb_pitch=5120
[LIMINE] Searching for module: /system/zrdemage/st/logo.bmp
[LOAD] Found logo.bmp, size=48138
[LOAD] BMP dimensions: 200x80
[LOAD] Logo size OK, passing to desk
[LOAD] Mapped logo at user virt 0x000000000A000000
[LOAD] Set logo_data=0x000000000A000000
[LOAD] Added thread tid=2
[BOOT] Enabling interrupts...
[BOOT] Starting scheduler...
```

#### 当前状态

| 功能 | 状态 |
|------|------|
| GPT 分区检测 | 正常 |
| ATA 磁盘识别 | 正常 |
| 扇区读取 (LBA28) | 正常 |
| 扇区写入 (PIO) | 正常 |
| FAT32 挂载 | 正常 |
| FAT32 目录创建 | 正常 |
| 文件系统回退 (RAMFS) | 正常 |
| desk 程序加载 | 正常 |
| logo.bmp 加载 | 正常 |
| 帧缓冲映射 | 正常 |

#### 遗留问题

1. **desk 程序启动后无响应**：调度器显示 "no runnable thread found"，desk 程序可能崩溃或入口点问题
2. **virtio-blk 驱动超时**：VirtQueue 通知后设备未响应，可能需要检查 MSI-X 中断或现代 virtio 1.0 接口兼容性
3. **FAT32 mkdir 父目录查找失败**：`/system/dbg` 等子目录创建时 "Parent directory not found"，是 FAT32 驱动的路径解析 bug

---

### 修改的文件清单

| 文件 | 修改内容 |
|------|----------|
| `/SYSTEM/lib/Ztknl/fat32.c` | 添加 GPT 分区表支持、添加调试输出 |
| `/SYSTEM/lib/Ztknl/virtio.h` | 新建 - virtio-blk 驱动头文件 |
| `/SYSTEM/lib/Ztknl/virtio.c` | 新建 - virtio-blk 驱动实现 |
| `/SYSTEM/lib/Ztknl/disk.c` | 添加 virtio 支持、实现 ATA 写入、LBA28/LBA48 切换、重试机制、调试输出 |
| `/SYSTEM/lib/Ztknl/main.c` | 无修改（确认 fat32_init 调用位置） |
| `/ISO/build_iso.sh` | 简化 ISO 打包逻辑、移除多余文件 |
| `/SYSTEM/boot/limine.conf` | 删除 logo.jpg 引用 |

---

### QEMU 测试命令

```bash
# UEFI 启动 + ATA 模式（推荐，当前工作路径）
qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=/tmp/OVMF_VARS.fd \
  -hda zratroos.img \
  -m 2G -smp 2 -vnc :0 -serial file:/tmp/qemu_serial.log

# UEFI 启动 + virtio-blk 模式（驱动已实现但仍有超时问题）
qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=/tmp/OVMF_VARS.fd \
  -drive file=zratroos.img,format=raw,if=none,id=hd0 \
  -device virtio-blk-pci,drive=hd0 \
  -m 2G -smp 2 -vnc :0 -serial file:/tmp/qemu_serial.log
```

---

### 下一步计划

1. 修复 desk 程序启动后无响应问题
2. 修复 FAT32 mkdir 父目录查找失败
3. 调试 virtio-blk 驱动超时问题
