# 2026-04-20 内核页错误修复与 back.bmp 背景实现

## 概述

修复了 ZratroOS 启动时的两个关键页错误崩溃，实现了 back.bmp 背景图片加载功能，并成功通过 VNC 进行 GUI 测试。

## 问题一：inter_kernel_init 页错误崩溃

### 问题描述

系统启动时在 `[INTER-KERNEL] Initializing...` 后立即崩溃：
```
[BOOT] Single kernel mode (inter-kernel only)
[INTER-KERNEL] Initializing...
[ISR] int_no=14 RIP=0xFFFFFFFF80003497 CR2=0xFFFF8000FEE00020 err=0x00
```

### 根因分析

1. `inter_kernel_init()` (`SYSTEM/lib/Ztknl/inter_kernel.c:68`) 尝试访问共享内存：
   ```c
   uint64_t phys_addr = INTER_KERNEL_SHM_PHYS;  // 0x1000000 (16MB)
   shm = (struct inter_kernel_shm *)phys_to_virt(phys_addr);
   ```

2. 在单内核模式下（`primary_role == 0`），共享内存区域没有被分配或映射

3. `phys_to_virt()` 返回的地址对应的虚拟页不在页表中，导致页错误

4. 错误码 `err=0x00` 表示页不存在，读操作

### 修复方案

在 `SYSTEM/lib/Ztknl/main.c:941-945` 中，当 `primary_role == 0` 时跳过 `inter_kernel_init()`：

```c
} else {
    /* 未知角色，单内核模式，跳过核间通信 */
    serial_print("[BOOT] Single kernel mode (no inter-kernel)\n");
    /* inter_kernel_init() skipped - no dual kernel configured */
}
```

**修改前**：
```c
} else {
    /* 未知角色，只初始化核间通信 */
    serial_print("[BOOT] Single kernel mode (inter-kernel only)\n");
    inter_kernel_init();
}
```

## 问题二：cap_init 页错误崩溃

### 问题描述

修复问题一后，系统在 `[CAP] Master key generated` 后崩溃：
```
[CAP] Initializing capability subsystem...
[CAP] Capability subsystem initialized
[CAP] Master key generated (secure storage required)
[ISR] int_no=14 RIP=0xFFFFFFFF800033DB CR2=0xFFFF8000FEE003E0 err=0x02
```

### 根因分析

1. **CR2 地址分析**：`0xFFFF8000FEE003E0`
   - 这是 HHDM（Higher Half Direct Map）区域地址
   - 对应物理地址：`0xFEE003E0` ≈ 4GB
   - `err=0x02` 表示写操作导致页错误

2. **HHDM 映射不足**：
   - `SYSTEM/lib/Ztknl/vmm.c:155` 只映射了 2GB：
     ```c
     uint64_t map_size = 0x80000000ULL;  /* 2GB */
     ```
   - QEMU 配置了 `-m 4G`，物理内存达到 4GB
   - `cap_mgr` 静态变量被链接到 BSS 段，通过 HHDM 访问时超出映射范围

3. **内核 BSS 段大小**：
   - `readelf` 显示 BSS 大小：`0x362a848` = 54MB
   - BSS 起始地址：`0xffffffff800c2000`
   - 静态变量通过 HHDM offset (`0xFFFF800000000000`) 访问

4. **页表映射缺失**：
   - HHDM 循环只映射物理 0-2GB (`phys < 0x80000000`)
   - 访问超过 2GB 的物理内存对应的 HHDM 地址时，页表项不存在

### 修复方案

在 `SYSTEM/lib/Ztknl/vmm.c:155` 增加 HHDM 映射大小到 4GB：

```c
uint64_t hhdm_base = 0xFFFF800000000000ULL;
uint64_t map_size = 0x100000000ULL;  /* 4GB */
```

**修改前**：
```c
uint64_t hhdm_base = 0xFFFF800000000000ULL;
uint64_t map_size = 0x80000000ULL;  /* 2GB */
```

## 功能实现：back.bmp 背景图片

### 实现内容

在 `SYSTEM/lib/desk/desk.c:1618-1672` 实现了背景图片加载和拉伸功能：

1. 从文件系统加载 `back.bmp`（通过 Limine module）
2. 解析 BMP 头获取像素数据偏移和位深度
3. 使用整数运算拉伸图像填充整个屏幕（避免 SSE/浮点指令）
4. 处理 Y 轴翻转（BMP 格式是从下到上存储的）
5. 如果没有图片，回退到深色背景（RGB 30,40,55）

### 核心代码

```c
/* 1. 加载 back.bmp 并拉伸填充整个屏幕 */
if (logo_data && logo_width > 0 && logo_height > 0) {
    /* logo_data 现在存储 back.bmp 数据 */
    unsigned char *bmp = (unsigned char *)logo_data;
    int bmp_w = logo_width;
    int bmp_h = logo_height;
    
    /* 从 BMP 头读取像素数据偏移 */
    unsigned int pixel_offset = (unsigned int)bmp[10] | 
                               ((unsigned int)bmp[11] << 8) | 
                               ((unsigned int)bmp[12] << 16) | 
                               ((unsigned int)bmp[13] << 24);
    
    /* 从 BMP 头读取位深度 */
    unsigned int bits_per_pixel = (unsigned int)bmp[28] | 
                                 ((unsigned int)bmp[29] << 8);
    int bytes_per_pixel = bits_per_pixel / 8;
    int bmp_row_size = ((bmp_w * bytes_per_pixel + 3) / 4) * 4;
    
    unsigned char *pixel_data = &bmp[pixel_offset];
    
    /* 拉伸绘制：使用整数运算（避免 SSE/浮点） */
    for (int fy = 0; fy < h; fy++) {
        /* 计算对应的 BMP 行（翻转Y轴，因为 BMP 是从下到上） */
        int src_y = ((h - 1 - fy) * bmp_h) / h;
        if (src_y < 0) src_y = 0;
        if (src_y >= bmp_h) src_y = bmp_h - 1;
        
        unsigned char *src_row = &pixel_data[src_y * bmp_row_size];
        uint32_t *dst_row = &fb[fy * stride];
        
        for (int fx = 0; fx < w; fx++) {
            /* 计算对应的 BMP 列 */
            int src_x = (fx * bmp_w) / w;
            if (src_x < 0) src_x = 0;
            if (src_x >= bmp_w) src_x = bmp_w - 1;
            
            unsigned char *px = &src_row[src_x * bytes_per_pixel];
            int b = px[0];
            int gr = px[1];
            int r = px[2];
            
            dst_row[fx] = (r << 16) | (gr << 8) | b;
        }
    }
}
```

### 技术细节

- **整数缩放算法**：`src_x = (fx * bmp_w) / w`，避免浮点运算
- **Y 轴翻转**：BMP 格式像素从下到上存储，需要翻转
- **行对齐**：BMP 每行对齐到 4 字节边界
- **帧缓冲区**：直接写入 framebuffer 像素，格式 RGB888

## VNC 测试

### 测试方法

使用 QEMU 的 VNC 显示功能进行测试：

```bash
OVMF_VARS_TMP=$(mktemp /tmp/ovmf_vars_XXXXXX.fd)
cp /usr/share/OVMF/x64/OVMF_VARS.4m.fd "$OVMF_VARS_TMP"
qemu-system-x86_64 \
    -drive file=zratroos.img,format=raw,if=virtio \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/x64/OVMF_CODE.4m.fd \
    -drive if=pflash,format=raw,file="$OVMF_VARS_TMP" \
    -m 4G \
    -smp 2 \
    -serial stdio \
    -no-reboot \
    -vnc :1 \
    -name "ZratroOS VNC Test"
```

连接 VNC：`localhost:5901`

### 测试结果

```
[VMM] Kernel HHDM mapped 4096 MB via 2MB pages
[BOOT] Single kernel mode (no inter-kernel)
[CAP] Capability subsystem initialized
[CAP] Master key generated (secure storage required)
[DESK] fb OK, clearing screen
[DESK] GUI state initialized
[LOAD] Found back.bmp, size=2922070
[LOAD] BMP dimensions: 1354x719
[LOAD] Logo size OK, passing to desk
[LOAD] Mapped logo at user virt 0x000000000A000000
[FB_SYNC] called
[DESK] Files button clicked!
[DESK] Start button clicked!
```

**测试通过**：
- [x] 内核启动无崩溃
- [x] HHDM 映射 4GB 正常
- [x] back.bmp 加载成功（2.8MB，1354x719）
- [x] 帧缓冲区同步成功
- [x] GUI 事件循环运行
- [x] 鼠标点击事件响应

## 构建流程

```bash
# 1. 编译 Ztknl 内核
cd SYSTEM/lib/Ztknl
make clean && make

# 2. 编译 desk 程序
cd SYSTEM/lib/desk
make clean && make

# 3. 构建镜像
cd ISO
bash build_iso.sh
```

## 影响文件

| 文件 | 修改内容 |
|------|----------|
| `SYSTEM/lib/Ztknl/main.c` | 单内核模式下跳过 inter_kernel_init |
| `SYSTEM/lib/Ztknl/vmm.c` | HHDM 映射从 2GB 扩展到 4GB |
| `SYSTEM/lib/desk/desk.c` | back.bmp 加载和拉伸渲染 |
| `ISO/zratroos.img` | 重新构建的启动镜像 |

## 已知问题

1. **FB corrupted after cursor** - 鼠标光标绘制后帧缓冲区数据损坏
2. **FAT32 读取超时** - VirtIO 块设备读取偶发超时
3. **DHCP 发送失败** - 网络驱动 `udp_sendto` 返回 -1
4. **VFS stat 失败** - RAMFS 模式下部分路径操作失败

## 后续工作

1. 修复鼠标光标渲染导致的 FB 损坏
2. 实现键盘/鼠标输入 syscall
3. 完善 VirtIO 块设备驱动
4. 修复 e1000 网络驱动 UDP 发送
5. 添加 desktop.conf 配置加载
6. 实现桌面文件图标显示

## 状态

- [x] inter_kernel_init 页错误修复
- [x] HHDM 映射不足修复
- [x] back.bmp 背景图片实现
- [x] VNC 测试通过
- [x] GUI 事件循环运行
- [ ] 鼠标光标渲染修复
- [ ] 网络驱动修复
- [ ] 桌面文件显示
