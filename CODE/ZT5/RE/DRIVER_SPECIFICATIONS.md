# ZratroOS 驱动规格文档

**版本**: ZT5  
**架构**: x86_64 Higher Half  
**日期**: 2026-06  
**状态**: 活跃开发

---

## 目录

- [第一部分: 现有驱动规格](#第一部分-现有驱动规格)
  - [1. AHCI SATA 控制器](#1-ahci-sata-控制器)
  - [2. NVMe 控制器](#2-nvme-控制器)
  - [3. E1000 网卡](#3-e1000-网卡)
  - [4. WiFi (RTL8188EU)](#4-wifi-rtl8188eu)
  - [5. XHCI USB 3.0](#5-xhci-usb-30)
  - [6. EHCI USB 2.0](#6-ehci-usb-20)
  - [7. UHCI USB 1.1](#7-uhci-usb-11)
  - [8. USB HID](#8-usb-hid)
  - [9. PS/2 键盘鼠标](#9-ps2-键盘鼠标)
  - [10. DRM 显示](#10-drm-显示)
  - [11. FB 帧缓冲](#11-fb-帧缓冲)
  - [12. VirtIO](#12-virtio)
  - [13. Disk 通用磁盘](#13-disk-通用磁盘)
- [第二部分: 新驱动规格](#第二部分-新驱动规格)
  - [14. AC97 音频](#14-ac97-音频)
  - [15. HDA 音频](#15-hda-音频)
  - [16. I2C 总线](#16-i2c-总线)
  - [17. SPI 总线](#17-spi-总线)
  - [18. RTC 时钟](#18-rtc-时钟)
  - [19. UART 串口](#19-uart-串口)
  - [20. HWRNG 随机数](#20-hwrng-随机数)
  - [21. ACPI 电源](#21-acpi-电源)
  - [22. 看门狗](#22-看门狗)
  - [23. PWM 控制](#23-pwm-控制)
  - [24. SD/MMC](#24-sdmmc)
  - [25. GPU 2D 加速](#25-gpu-2d-加速)
  - [26. 蓝牙](#26-蓝牙)
  - [27. 触摸板](#27-触摸板)
  - [28. TPM 2.0](#28-tpm-20)

---

# 第一部分: 现有驱动规格

---

## 1. AHCI SATA 控制器

### 硬件兼容性

| 设备 | PCI 厂商 ID | PCI 设备 ID | 说明 |
|------|------------|------------|------|
| Intel AHCI | 0x8086 | 多种 | ICH6/ICH7/ICH8/ICH9/ICH10/PCH |
| AMD AHCI | 0x1022 | 0x7801 | AMD SB7x0/SB8x0 |
| QEMU AHCI | 0x8086 | 0x2922 | QEMU 模拟 ICH9 |
| VMware AHCI | 0x15AD | 0x07E0 | VMware 虚拟 AHCI |

### 已修复 Bug 和改进

| Bug | 说明 | 修复方式 |
|-----|------|---------|
| DMA 地址计算错误 | 使用虚拟地址设置 PRDT/命令表地址 | 改用 `virt_to_phys()` 转换 |
| 端口重置超时 | 端口重置未设超时，可能无限等待 | 增加 500ms 超时机制 |
| FIS 接收未启用 | 启动命令前未启用 FIS 接收 | 在 CMD.ST 之前设置 CMD.FRE |

### API 参考

```c
/* 初始化 */
int ahci_init(void);                    /* 初始化 AHCI 控制器 */
int ahci_detect(void);                  /* 检测 AHCI 控制器 */
struct ahci_controller *ahci_get_controller(void);  /* 获取控制器实例 */

/* 读写操作 */
int ahci_read_sector(uint64_t lba, void *buf);       /* 读取一个扇区 */
int ahci_write_sector(uint64_t lba, const void *buf); /* 写入一个扇区 */
uint64_t ahci_get_disk_size(void);                    /* 获取磁盘大小 */
int ahci_is_available(void);                          /* 检查是否可用 */
```

### 关键数据结构

```c
struct ahci_controller {
    uint64_t base;              /* 控制器基地址 (ABAR) */
    uint32_t capabilities;      /* CAP 寄存器 */
    uint32_t capabilities2;     /* CAP2 寄存器 */
    uint32_t version;           /* 版本号 */
    uint32_t ports_implemented; /* 已实现的端口 */
    int port_count;             /* 端口数量 */
    struct ahci_port ports[32]; /* 端口数组 */
    int initialized;            /* 是否已初始化 */
};

struct ahci_port {
    uint64_t base;              /* 端口寄存器基地址 */
    int num;                    /* 端口号 */
    int type;                   /* 设备类型 */
    int is_atapi;               /* 是否为 ATAPI */
    struct ahci_cmd_header *cmd_list;
    struct ahci_cmd_table *cmd_table;
    struct ahci_received_fis *rfis;
    uint64_t cmd_list_phys;     /* 命令列表物理地址 */
    uint64_t cmd_table_phys;    /* 命令表物理地址 */
    uint64_t rfis_phys;         /* FIS 物理地址 */
};
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -drive file=disk.img,if=none,id=ahci0 \
    -device ich9-ahci,id=ahci \
    -device ide-hd,drive=ahci0,bus=ahci.0
```

---

## 2. NVMe 控制器

### 硬件兼容性

| 设备 | PCI 厂商 ID | 说明 |
|------|------------|------|
| Intel NVMe | 0x8086 | Intel 600p/760p/905p |
| Samsung NVMe | 0x144D | Samsung 960/970/980 |
| QEMU NVMe | 0x1B36 | QEMU nvme 设备 |

### 已修复 Bug 和改进

| Bug | 说明 | 修复方式 |
|-----|------|---------|
| 提交队列尾指针更新 | 未使用写屏障 | 在写入 DB 前添加 `wmb()` |
| 完成队列轮询 | 未检查相位标签变化 | 增加相位标签检查 |

### API 参考

```c
int nvme_init(void);                              /* 初始化 NVMe 控制器 */
int nvme_read_sector(uint64_t lba, void *buf);    /* 读取扇区 */
int nvme_write_sector(uint64_t lba, const void *buf); /* 写入扇区 */
uint64_t nvme_get_disk_size(void);                /* 获取磁盘大小 */
int nvme_is_available(void);                      /* 检查是否可用 */
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -drive file=nvme.img,if=none,id=nvme0 \
    -device nvme,drive=nvme0,serial=zratro-nvme
```

---

## 3. E1000 网卡

### 硬件兼容性

| 设备 | PCI 厂商 ID | PCI 设备 ID | 说明 |
|------|------------|------------|------|
| Intel E1000 | 0x8086 | 0x100E | 82540EM (QEMU 默认) |
| Intel E1000e | 0x8086 | 0x10D3 | 82574L |
| Intel I210 | 0x8086 | 0x1533 | I210 千兆 |

### 已修复 Bug 和改进

| Bug | 说明 | 修复方式 |
|-----|------|---------|
| 接收描述符环回绕 | 未正确处理 RDT 超过 RDH 的情况 | 增加环形缓冲区回绕检查 |
| 中断处理遗漏 | 中断处理中未清除 ICR | 在 ISR 中写入 IMC 清除中断 |

### API 参考

```c
int e1000_init(void);                    /* 初始化 E1000 网卡 */
void e1000_send_packet(struct net_buf *buf);  /* 发送数据包 */
struct net_buf *e1000_recv_packet(void);      /* 接收数据包 */
int e1000_get_mac(struct mac_addr *mac);      /* 获取 MAC 地址 */
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -netdev user,id=net0 \
    -device e1000,netdev=net0
```

---

## 4. WiFi (RTL8188EU)

### 硬件兼容性

| 设备 | USB 厂商 ID | USB 设备 ID | 说明 |
|------|------------|------------|------|
| Realtek RTL8188EU | 0x0BDA | 0x8179 | USB WiFi 适配器 |
| Realtek RTL8188EUS | 0x0BDA | 0x8176 | 变体 |

### 已修复 Bug 和改进

| Bug | 说明 | 修复方式 |
|-----|------|---------|
| PBKDF2 迭代次数 | 仅 1 次迭代 (应为 4096) | 修正为 4096 次 |
| AES-CCMP 加密 | 使用 XOR 替代 AES | 实现完整 AES-CCMP |
| WPA2 握手 | 4 次握手状态机错误 | 修复状态机转换逻辑 |

### API 参考

```c
int wifi_init(void);                                    /* 初始化 WiFi 驱动 */
int wifi_connect(const char *ssid, const char *password); /* 连接网络 */
int wifi_disconnect(void);                              /* 断开连接 */
int wifi_get_status(void);                              /* 获取连接状态 */
int wifi_scan(struct wifi_scan_result *results, int max); /* 扫描网络 */
```

### 测试配置 (QEMU)

```bash
# WiFi 需要真实硬件或 USB 透传，QEMU 不直接支持
# 可使用 USB 透传:
qemu-system-x86_64 \
    -usb -device usb-host,vendorid=0x0bda,productid=0x8179
```

---

## 5. XHCI USB 3.0

### 硬件兼容性

| 设备 | PCI 厂商 ID | 说明 |
|------|------------|------|
| Intel XHCI | 0x8086 | Panther Point/Lynx Point/Sunrise Point |
| AMD XHCI | 0x1022 | AMD XHCI 控制器 |
| QEMU XHCI | 0x1B36 | QEMU xhci |
| NEC XHCI | 0x1033 | NEC uPD720200 |

### 已修复 Bug 和改进

| Bug | 说明 | 修复方式 |
|-----|------|---------|
| 数据传输未实现 | 仅有控制器初始化 | 实现完整 Transfer Ring 和 Event Ring |
| TRB 对齐 | TRB 未 16 字节对齐 | 确保 TRB 分配 16 字节对齐 |
| 端口复位超时 | 端口复位无超时 | 增加 10s 超时 |

### API 参考

```c
int xhci_init(void);                   /* 初始化 XHCI 控制器 */
int xhci_detect(void);                 /* 检测 XHCI 控制器 */
int xhci_reset_port(int port);         /* 复位端口 */
int xhci_enable_slot(int port);        /* 分配设备槽位 */
int xhci_address_device(int slot);     /* 分配设备地址 */
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -device qemu-xhci \
    -usb -device usb-kbd -device usb-mouse
```

---

## 6. EHCI USB 2.0

### 硬件兼容性

| 设备 | PCI 厂商 ID | 说明 |
|------|------------|------|
| Intel EHCI | 0x8086 | ICH6/ICH7/ICH8/ICH9 |
| QEMU EHCI | - | QEMU ich9-ehci-0 |

### API 参考

```c
int ehci_init(void);                   /* 初始化 EHCI 控制器 */
int ehci_detect(void);                 /* 检测 EHCI 控制器 */
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -device ich9-ehci-0 \
    -usb -device usb-kbd
```

---

## 7. UHCI USB 1.1

### 硬件兼容性

| 设备 | PCI 厂商 ID | 说明 |
|------|------------|------|
| Intel UHCI | 0x8086 | PIIX3/PIIX4 |
| QEMU UHCI | - | QEMU piix3-usb-uhci |

### API 参考

```c
int uhci_init(void);                   /* 初始化 UHCI 控制器 */
int uhci_detect(void);                 /* 检测 UHCI 控制器 */
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -device piix3-usb-uhci \
    -usb -device usb-kbd
```

---

## 8. USB HID

### 硬件兼容性

| 设备 | 类代码 | 说明 |
|------|--------|------|
| USB 键盘 | 0x03/0x01/0x01 | HID/Boot/Keyboard |
| USB 鼠标 | 0x03/0x01/0x02 | HID/Boot/Mouse |

### API 参考

```c
int usb_hid_init(void);                /* 初始化 USB HID */
int usb_hid_probe(void);               /* 探测 HID 设备 */
```

---

## 9. PS/2 键盘鼠标

### 硬件兼容性

| 设备 | I/O 端口 | IRQ | 说明 |
|------|---------|-----|------|
| PS/2 键盘 | 0x60/0x64 | 1 | 标准 AT 键盘 |
| PS/2 鼠标 | 0x60/0x64 | 12 | 辅助 PS/2 端口 |

### API 参考

```c
int ps2_init(void);                    /* 初始化 PS/2 控制器 */
int ps2_keyboard_init(void);           /* 初始化键盘 */
int ps2_mouse_init(void);              /* 初始化鼠标 */
```

### 测试配置 (QEMU)

```bash
# PS/2 默认启用，无需额外参数
qemu-system-x86_64
```

---

## 10. DRM 显示

### 硬件兼容性

| 设备 | 说明 |
|------|------|
| Bochs VBE | QEMU 标准 VGA |
| Virtio-GPU | QEMU virtio-gpu |

### API 参考

```c
int drm_init(void);                    /* 初始化 DRM 子系统 */
int drm_probe(void);                   /* 探测 GPU 设备 */
```

---

## 11. FB 帧缓冲

### 硬件兼容性

| 设备 | 说明 |
|------|------|
| VBE LFB | VESA BIOS Extensions 线性帧缓冲 |
| QEMU std-vga | QEMU 标准 VGA 帧缓冲 |

### 已修复 Bug 和改进

| Bug | 说明 | 修复方式 |
|-----|------|---------|
| ASCII 字体不完整 | 仅部分字符有字体数据 | 补全 128 字符 8x16 位图 |

### API 参考

```c
int fb_init(void);                     /* 初始化帧缓冲 */
void fb_putchar(char c, int x, int y, uint32_t fg, uint32_t bg); /* 绘制字符 */
void fb_draw_rect(int x, int y, int w, int h, uint32_t color);  /* 绘制矩形 */
void *fb_get_buffer(void);             /* 获取帧缓冲地址 */
uint32_t fb_get_width(void);           /* 获取宽度 */
uint32_t fb_get_height(void);          /* 获取高度 */
uint32_t fb_get_bpp(void);             /* 获取每像素位数 */
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -vga std -display gtk
```

---

## 12. VirtIO

### 硬件兼容性

| 设备 | VirtIO 设备 ID | 说明 |
|------|---------------|------|
| VirtIO-Blk | 2 | 块设备 |
| VirtIO-Net | 1 | 网络设备 |
| VirtIO-RNG | 4 | 随机数设备 |
| VirtIO-GPU | 16 | GPU 设备 |
| VirtIO-Serial | 3 | 串口设备 |

### 已修复 Bug 和改进

| Bug | 说明 | 修复方式 |
|-----|------|---------|
| 描述符标志错误 | 未正确设置 VRING_DESC_F_NEXT | 对链中非最后描述符设置 NEXT 标志 |
| 可用环更新 | 未使用写屏障 | 在更新可用环后添加 `wmb()` |

### API 参考

```c
int virtio_init(void);                 /* 初始化 VirtIO 子系统 */
int virtio_blk_init(void);             /* 初始化 VirtIO 块设备 */
int virtio_net_init(void);             /* 初始化 VirtIO 网卡 */
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -drive file=virtio.img,if=none,id=vblk \
    -device virtio-blk-pci,drive=vblk \
    -netdev user,id=vnet \
    -device virtio-net-pci,netdev=vnet \
    -device virtio-rng-pci
```

---

## 13. Disk 通用磁盘

### 硬件兼容性

| 设备 | 说明 |
|------|------|
| AHCI SATA | 通过 AHCI 控制器访问 |
| NVMe | 通过 NVMe 控制器访问 |
| VirtIO-Blk | 通过 VirtIO 访问 |
| IDE | 传统 IDE 控制器 |

### API 参考

```c
int disk_init(void);                                    /* 初始化磁盘子系统 */
int disk_read_sector(uint64_t lba, void *buf);          /* 读取扇区 */
int disk_write_sector(uint64_t lba, const void *buf);   /* 写入扇区 */
uint64_t disk_get_size(void);                           /* 获取磁盘大小 */
int disk_get_sector_size(void);                         /* 获取扇区大小 */
```

---

# 第二部分: 新驱动规格

---

## 14. AC97 音频

### 硬件兼容性

| 设备 | PCI 厂商 ID | PCI 设备 ID | 说明 |
|------|------------|------------|------|
| Intel AC97 | 0x8086 | 0x2415 | ICH4/ICH5 AC97 音频 |
| Intel AC97 | 0x8086 | 0x2425 | ICH4 AC97 |
| QEMU AC97 | 0x8086 | 0x2415 | QEMU 模拟 AC97 |

### 完整 API 参考

```c
/* 初始化与探测 */
int ac97_init(void);                   /* 初始化 AC97 驱动 */
int ac97_probe(void);                  /* 探测 AC97 设备 */
struct ac97_device *ac97_get_device(void);  /* 获取设备实例 */

/* 混音器控制 */
int ac97_set_master_volume(uint8_t left, uint8_t right);  /* 设置主音量 */
int ac97_get_master_volume(uint8_t *left, uint8_t *right); /* 获取主音量 */
int ac97_set_pcm_volume(uint8_t left, uint8_t right);     /* 设置 PCM 音量 */
int ac97_get_pcm_volume(uint8_t *left, uint8_t *right);   /* 获取 PCM 音量 */
int ac97_mute(int mute);               /* 静音/取消静音 */
int ac97_is_muted(void);               /* 查询是否静音 */

/* PCM 播放 */
int ac97_play(const void *data, size_t len);  /* 播放音频数据 */
int ac97_stop(void);                   /* 停止播放 */
int ac97_pause(void);                  /* 暂停播放 */
int ac97_resume(void);                 /* 恢复播放 */
int ac97_is_playing(void);             /* 查询播放状态 */

/* 采样率 */
int ac97_set_sample_rate(uint32_t rate);   /* 设置采样率 */
uint32_t ac97_get_sample_rate(void);       /* 获取采样率 */

/* 中断处理 */
void ac97_interrupt_handler(struct interrupt_frame *frame);
```

### 关键数据结构

```c
struct ac97_device {
    /* PCI 信息 */
    uint8_t pci_bus, pci_dev, pci_func, irq_line;

    /* I/O 端口 */
    uint16_t nam_base;        /* Native Audio Mixer 基址 */
    uint16_t nabm_base;       /* Native Audio Bus Master 基址 */

    /* BDL 缓冲区 */
    struct ac97_bdl_entry *bdl;
    uint64_t bdl_phys;

    /* PCM 缓冲区 */
    uint8_t *buffers[AC97_NUM_BUFFERS];
    uint64_t buffer_phys[AC97_NUM_BUFFERS];

    /* 播放状态 */
    uint32_t play_civ, play_lvi;
    int playing;
    spinlock_t lock;

    /* 混音器 */
    struct ac97_mixer_state mixer;

    /* 编解码器 */
    uint16_t codec_vendor_id, codec_revision;
    int codec_ready;

    /* 统计 */
    uint64_t bytes_played, underruns;
};

struct ac97_bdl_entry {
    uint32_t buffer_addr;    /* 缓冲区物理地址 */
    uint16_t length;         /* 采样数 */
    uint16_t ctrl;           /* 控制位 */
} __attribute__((packed));

struct ac97_mixer_state {
    uint16_t master_vol;     /* 主音量 (0-0x3F) */
    uint16_t pcm_vol;        /* PCM 音量 (0-0x1F) */
    uint16_t mic_vol;
    uint16_t linein_vol;
    uint16_t cd_vol;
    int muted;
};
```

### 初始化序列

```
1. PCI 探测 (8086:2415)
2. 启用 PCI 设备 (总线主控 + I/O 空间)
3. 获取 NAM/NABM I/O 基址
4. 冷复位编解码器 (GLOB_CNT.COLD)
5. 等待编解码器就绪 (GLOB_STA.PCR)
6. 重置所有 Bus Master 寄存器 (CR.RR)
7. 设置默认混音器值
8. 分配 BDL 和音频缓冲区
9. 注册中断处理程序
10. 启用全局中断 (GLOB_CNT.GIE)
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -device AC97 \
    -audiodev pa,id=audio0
```

---

## 15. HDA 音频

### 硬件兼容性

| 设备 | PCI 厂商 ID | PCI 设备 ID | 说明 |
|------|------------|------------|------|
| Intel HDA ICH6 | 0x8086 | 0x2668 | ICH6 HDA |
| Intel HDA ICH7 | 0x8086 | 0x284B | ICH7 HDA |
| Intel HDA ICH9 | 0x8086 | 0x293E | ICH9 HDA |
| Intel HDA ICH10 | 0x8086 | 0x3A3E | ICH10 HDA |
| Intel HDA PCH | 0x8086 | 0x3B56 | PCH HDA |
| QEMU HDA | 0x8086 | 0x2668 | QEMU 模拟 ICH6 |

### 完整 API 参考

```c
/* 初始化与探测 */
int hda_init(void);                    /* 初始化 HDA 驱动 */
int hda_probe(void);                   /* 探测 HDA 设备 */
struct hda_device *hda_get_device(void);  /* 获取设备实例 */

/* 编解码器命令 */
uint32_t hda_codec_command(uint8_t codec_addr, uint32_t verb);
uint32_t hda_codec_get_param(uint8_t codec_addr, uint8_t nid, uint32_t param);
uint32_t hda_codec_set_amp(uint8_t codec_addr, uint8_t nid, uint32_t payload);

/* Widget 树 */
int hda_parse_widgets(struct hda_codec *codec);

/* 流管理 */
int hda_stream_start(int stream_index, int direction);
int hda_stream_stop(int stream_index, int direction);
int hda_stream_play(const void *data, size_t len);

/* PCM 播放 */
int hda_play(const void *data, size_t len);
int hda_stop(void);

/* 音量控制 */
int hda_set_volume(uint8_t nid, int left_db, int right_db);
int hda_set_mute(uint8_t nid, int mute);

/* 中断处理 */
void hda_interrupt_handler(struct interrupt_frame *frame);
```

### 关键数据结构

```c
struct hda_device {
    uint8_t pci_bus, pci_dev, pci_func, irq_line;
    uint64_t mmio_phys, mmio_virt;

    /* 控制器能力 */
    uint16_t num_output_streams, num_input_streams, num_bidir_streams;

    /* CORB/RIRB */
    uint32_t *corb;
    uint64_t corb_phys;
    uint16_t corb_wp;
    struct hda_rirb_entry *rirb;
    uint64_t rirb_phys;
    uint16_t rirb_rp;

    /* 编解码器 */
    struct hda_codec codecs[4];
    int num_codecs;

    /* 流管理 */
    struct hda_stream output_streams[8];
    struct hda_stream input_streams[4];

    spinlock_t lock;
    uint64_t bytes_played, underruns;
};

struct hda_codec {
    uint8_t addr;
    uint32_t vendor_id, revision_id;
    int ready;
    struct hda_widget widgets[64];
    int num_widgets;
    uint8_t afg_nid, root_nid, dac_nid, adc_nid, pin_nid;
};

struct hda_widget {
    uint8_t nid, type, nconn;
    uint32_t conn_list[16];
    uint32_t amp_in_caps, amp_out_caps, pin_caps;
    uint32_t config_default;
    uint16_t power_state;
};
```

### 初始化序列

```
1. PCI 探测 (8086:2668/284B/293E/3A3E/3B56)
2. 启用 PCI 设备 (总线主控 + MMIO)
3. 映射 MMIO 寄存器空间
4. 控制器复位 (GCTL.CRST=0, 等待, GCTL.CRST=1)
5. 检测编解码器 (STATESTS 位掩码)
6. 初始化 CORB (分配缓冲区, 设置基址, 运行)
7. 初始化 RIRB (分配缓冲区, 设置基址, 运行)
8. 枚举编解码器 Widget 树
9. 配置 DAC -> Mixer -> Pin 路径
10. 分配输出流 BDL 和缓冲区
11. 注册中断处理程序
12. 启用中断 (INTCTL.GIE)
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -device intel-hda -device hda-duplex \
    -audiodev pa,id=audio0
```

---

## 16. I2C 总线

### 硬件兼容性

| 设备 | 说明 |
|------|------|
| Intel SMBus I801 | ICH6/ICH7/ICH8/ICH9 SMBus 控制器 |
| 设计ware I2C | Synopsys DesignWare I2C 控制器 |
| 通用 I2C | 任何 I2C/SMBus 兼容控制器 |

### 完整 API 参考

```c
/* 初始化 */
int i2c_init(void);                    /* 初始化 I2C 子系统 */

/* 适配器管理 */
int i2c_add_adapter(struct i2c_adapter *adapter);    /* 注册适配器 */
int i2c_del_adapter(struct i2c_adapter *adapter);    /* 注销适配器 */
struct i2c_adapter *i2c_get_adapter(int id);         /* 获取适配器 */

/* 设备管理 */
int i2c_add_device(struct i2c_client *client);       /* 注册设备 */
int i2c_del_device(struct i2c_client *client);       /* 注销设备 */
struct i2c_client *i2c_find_device(uint16_t addr);   /* 查找设备 */

/* 传输 API */
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int i2c_read(struct i2c_adapter *adap, uint16_t addr, uint8_t *buf, size_t len);
int i2c_write(struct i2c_adapter *adap, uint16_t addr, const uint8_t *buf, size_t len);

/* SMBus 兼容 API */
int i2c_smbus_read_byte(struct i2c_client *client);
int i2c_smbus_write_byte(struct i2c_client *client, uint8_t value);
int i2c_smbus_read_byte_data(struct i2c_client *client, uint8_t command);
int i2c_smbus_write_byte_data(struct i2c_client *client, uint8_t command, uint8_t value);
int i2c_smbus_read_word_data(struct i2c_client *client, uint8_t command);
int i2c_smbus_write_word_data(struct i2c_client *client, uint8_t command, uint16_t value);

/* 探测 */
int i2c_probe_device(struct i2c_adapter *adap, uint16_t addr);
void i2c_scan_bus(struct i2c_adapter *adap);
```

### 关键数据结构

```c
struct i2c_adapter {
    int id;
    char name[32];
    struct i2c_algorithm *algo;
    void *algo_data;
    uint64_t mmio_base;
    uint16_t io_base;
    int irq;
    int busy;
    spinlock_t lock;
    struct list_head devices;
    struct list_head list;
    uint64_t xfer_count, error_count;
};

struct i2c_algorithm {
    const char *name;
    int (*xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
    int (*smbus_xfer)(struct i2c_adapter *adap, uint16_t addr,
                      uint16_t flags, uint8_t read_write,
                      uint8_t command, int size, union i2c_smbus_data *data);
    uint32_t (*functionality)(struct i2c_adapter *adap);
};

struct i2c_msg {
    uint16_t addr;          /* 从设备地址 */
    uint16_t flags;         /* 传输标志 */
    uint16_t len;           /* 数据长度 */
    uint8_t *buf;           /* 数据缓冲区 */
};

struct i2c_client {
    uint16_t addr;
    char name[32];
    struct i2c_adapter *adapter;
    struct i2c_driver *driver;
    void *driver_data;
    int irq;
    struct list_head list;
};

union i2c_smbus_data {
    uint8_t byte;
    uint16_t word;
    uint8_t block[34];  /* block[0] = 长度 */
};
```

### 初始化序列

```
1. 初始化 I2C 子系统 (全局链表、锁)
2. 探测 PCI 上的 I2C/SMBus 控制器
3. 映射控制器 MMIO/IO 端口
4. 初始化控制器硬件 (复位、设置时钟)
5. 注册适配器到 I2C 核心
6. 扫描总线上的设备 (0x03-0x77)
7. 为发现的设备创建 i2c_client
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -device i2c-ddc  # I2C 显示设备
```

---

## 17. SPI 总线

### 硬件兼容性

| 设备 | 说明 |
|------|------|
| 设计ware SPI | Synopsys DesignWare SPI 控制器 |
| Intel SPI | Intel PCH SPI 控制器 |
| 通用 SPI | 任何 SPI 兼容控制器 |

### 完整 API 参考

```c
/* 初始化 */
int spi_init(void);                    /* 初始化 SPI 子系统 */

/* 适配器管理 */
int spi_add_adapter(struct spi_adapter *adapter);
int spi_del_adapter(struct spi_adapter *adapter);
struct spi_adapter *spi_get_adapter(int id);

/* 设备管理 */
int spi_add_device(struct spi_device *device);
int spi_del_device(struct spi_device *device);

/* 传输 API */
int spi_transfer(struct spi_adapter *adap, struct spi_device *dev,
                 struct spi_transfer *xfer);
int spi_read(struct spi_adapter *adap, struct spi_device *dev,
             uint8_t *buf, size_t len);
int spi_write(struct spi_adapter *adap, struct spi_device *dev,
              const uint8_t *buf, size_t len);
int spi_write_then_read(struct spi_adapter *adap, struct spi_device *dev,
                        const uint8_t *txbuf, size_t n_tx,
                        uint8_t *rxbuf, size_t n_rx);

/* 片选控制 */
void spi_chip_select(struct spi_adapter *adap, uint8_t cs, int enable);

/* 设备配置 */
int spi_setup_device(struct spi_device *dev);
```

### 关键数据结构

```c
struct spi_adapter {
    int id;
    char name[32];
    uint32_t max_speed_hz;
    uint8_t num_chipselect;
    uint8_t bits_per_word;
    uint8_t mode;
    struct spi_adapter_ops *ops;
    void *priv;
    uint64_t mmio_base;
    uint16_t io_base;
    int irq;
    int busy;
    spinlock_t lock;
    struct list_head devices;
    struct list_head list;
};

struct spi_adapter_ops {
    int (*setup)(struct spi_adapter *adap, struct spi_device *dev);
    int (*transfer)(struct spi_adapter *adap, struct spi_device *dev,
                    struct spi_transfer *xfer);
    int (*transfer_one)(struct spi_adapter *adap, struct spi_device *dev,
                        const uint8_t *tx, uint8_t *rx, uint32_t len);
    void (*chip_select)(struct spi_adapter *adap, uint8_t cs, int enable);
};

struct spi_transfer {
    const uint8_t *tx_buf;
    uint8_t *rx_buf;
    uint32_t len;
    uint32_t speed_hz;
    uint16_t delay_usecs;
    uint8_t bits_per_word;
    uint8_t flags;
    struct list_head list;
};

struct spi_device {
    char name[32];
    uint8_t chip_select;
    uint32_t max_speed_hz;
    uint8_t mode;           /* SPI_MODE_0 ~ SPI_MODE_3 */
    uint8_t bits_per_word;
    int irq;
    void *driver_data;
    struct spi_adapter *adapter;
    struct list_head list;
};
```

### 初始化序列

```
1. 初始化 SPI 子系统
2. 探测 SPI 控制器
3. 映射控制器 MMIO
4. 配置默认模式 (MODE_0, 8bit, 低速)
5. 注册适配器
6. 枚举已知的 SPI 设备
```

---

## 18. RTC 时钟

### 硬件兼容性

| 设备 | I/O 端口 | 说明 |
|------|---------|------|
| MC146818 RTC | 0x70/0x71 | 标准 PC RTC |
| QEMU RTC | 0x70/0x71 | QEMU 模拟 RTC |

### 完整 API 参考

```c
/* 初始化 */
int rtc_init(void);                    /* 初始化 RTC 驱动 */
struct rtc_device *rtc_get_device(void);  /* 获取设备实例 */

/* 日期时间 */
int rtc_read_time(struct rtc_time *time);     /* 读取日期时间 */
int rtc_set_time(const struct rtc_time *time); /* 设置日期时间 */
time_t rtc_get_unix_time(void);               /* 获取 Unix 时间戳 */
int rtc_set_unix_time(time_t timestamp);      /* 从时间戳设置 */

/* 闹钟 */
int rtc_set_alarm(const struct rtc_alarm *alarm);  /* 设置闹钟 */
int rtc_get_alarm(struct rtc_alarm *alarm);        /* 获取闹钟 */
int rtc_enable_alarm(int enable);                   /* 启用/禁用闹钟 */

/* NTP 同步 */
int rtc_ntp_sync(const char *server);        /* NTP 同步 */
int rtc_ntp_get_status(struct ntp_state *state); /* 获取 NTP 状态 */

/* 中断处理 */
void rtc_interrupt_handler(struct interrupt_frame *frame);

/* 辅助函数 */
uint8_t rtc_bcd_to_bin(uint8_t bcd);
uint8_t rtc_bin_to_bcd(uint8_t bin);
```

### 关键数据结构

```c
struct rtc_time {
    uint8_t sec, min, hour;
    uint8_t mday, mon;
    uint16_t year;
    uint8_t wday;
};

struct rtc_alarm {
    uint8_t sec, min, hour;  /* 0xFF = 忽略 */
    int enabled;
};

struct ntp_state {
    int synced;
    int64_t offset_ms;
    uint64_t last_sync;
    char server[64];
};

struct rtc_device {
    int initialized;
    int bcd_mode;
    int use_24h;
    spinlock_t lock;
    struct rtc_alarm alarm;
    void (*alarm_callback)(void *data);
    void *alarm_data;
    struct ntp_state ntp;
    uint64_t read_count, write_count;
};
```

### 初始化序列

```
1. 禁用 NMI (0x70 bit7)
2. 读取寄存器 B，检测 BCD/二进制模式
3. 检测 12h/24h 模式
4. 读取当前时间验证 RTC 工作正常
5. 设置 24h 二进制模式
6. 启用周期中断 (可选)
7. 注册 IRQ8 中断处理程序
```

### 测试配置 (QEMU)

```bash
# RTC 默认启用
qemu-system-x86_64 \
    -rtc base=localtime
```

---

## 19. UART 串口

### 硬件兼容性

| 端口 | I/O 基址 | IRQ | 说明 |
|------|---------|-----|------|
| COM1 | 0x3F8 | 4 | 主串口 |
| COM2 | 0x2F8 | 3 | 辅助串口 |
| COM3 | 0x3E8 | 4 | COM3 |
| COM4 | 0x2E8 | 3 | COM4 |

### 完整 API 参考

```c
/* 初始化 */
int uart_init(void);                   /* 初始化 UART 子系统 */
int uart_port_init(int port_index, uint32_t baud_rate); /* 初始化指定端口 */

/* 发送/接收 */
int uart_putc(int port_index, char c);
int uart_getc(int port_index);
int uart_write(int port_index, const void *buf, size_t len);
int uart_read(int port_index, void *buf, size_t len);

/* 非阻塞 */
int uart_read_nonblock(int port_index, void *buf, size_t len);
int uart_data_available(int port_index);

/* 配置 */
int uart_set_baud_rate(int port_index, uint32_t baud_rate);
int uart_set_format(int port_index, uint8_t data_bits,
                    uint8_t stop_bits, uint8_t parity);

/* TTY 接口 */
int uart_tty_register(int port_index);

/* 中断处理 */
void uart_com1_interrupt_handler(struct interrupt_frame *frame);
void uart_com2_interrupt_handler(struct interrupt_frame *frame);

/* 获取端口实例 */
struct uart_port *uart_get_port(int port_index);
```

### 关键数据结构

```c
struct uart_port {
    uint16_t io_base;
    uint8_t irq;
    uint8_t index;
    int initialized;

    /* 配置 */
    uint32_t baud_rate;
    uint8_t data_bits;      /* 5-8 */
    uint8_t stop_bits;      /* 1-2 */
    uint8_t parity;         /* 0=无, 1=奇, 2=偶 */

    /* 缓冲区 */
    struct uart_ring_buf rx_buf;
    struct uart_ring_buf tx_buf;

    /* 状态 */
    int tx_active;
    spinlock_t lock;

    /* 统计 */
    uint64_t rx_count, tx_count;
    uint64_t rx_errors, tx_errors;
};

struct uart_ring_buf {
    uint8_t buf[4096];
    volatile uint32_t head, tail;
    spinlock_t lock;
};
```

### 初始化序列

```
1. 禁用所有中断 (IER = 0)
2. 设置 DLAB，配置波特率分频值
3. 设置 8N1 格式 (LCR = 0x03)
4. 启用 FIFO，清空缓冲区，14 字节触发 (FCR = 0xC7)
5. 设置 MCR (DTR + RTS + OUT2)
6. 启用接收中断 (IER = 0x01)
7. 注册 IRQ 中断处理程序
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -serial mon:stdio \
    -serial file:com2.log
```

---

## 20. HWRNG 随机数

### 硬件兼容性

| 熵源 | 说明 |
|------|------|
| Intel RDRAND | Ivy Bridge 及更新 CPU |
| Intel RDSEED | Broadwell 及更新 CPU |
| Virtio-RNG | QEMU Virtio 随机数设备 |
| TPM RNG | TPM 2.0 随机数生成器 |
| 定时器抖动 | 软件回退方案 |

### 完整 API 参考

```c
/* 初始化 */
int hwrng_init(void);                  /* 初始化 HWRNG 子系统 */
struct hwrng_device *hwrng_get_device(void);  /* 获取设备实例 */

/* 读取随机数 */
int hwrng_read(void *buf, size_t len);         /* 阻塞读取 */
int hwrng_read_nonblock(void *buf, size_t len); /* 非阻塞读取 */

/* 熵管理 */
int hwrng_add_entropy(const void *data, size_t len, int entropy_bits);
int hwrng_get_entropy_count(void);              /* 获取当前熵位估计 */

/* Intel RDRAND/RDSEED */
int hwrng_rdrand_available(void);               /* 检查 RDRAND 可用性 */
int hwrng_rdrand_read(uint64_t *val);           /* 读取 RDRAND */
int hwrng_rdseed_read(uint64_t *val);           /* 读取 RDSEED */

/* Virtio-RNG */
int hwrng_virtio_init(void);                    /* 初始化 Virtio-RNG */
int hwrng_virtio_read(void *buf, size_t len);   /* 从 Virtio 读取 */

/* 熵池混合 */
void hwrng_pool_mix(struct entropy_pool *pool); /* SHA-256 混合 */

/* 软件回退 */
int hwrng_timer_jitter(void *buf, size_t len);  /* 定时器抖动 */
```

### 关键数据结构

```c
struct hwrng_device {
    int initialized;
    int active_source;         /* 当前活跃熵源 */
    int has_rdrand, has_rdseed, has_virtio;

    /* 双熵池 */
    struct entropy_pool input_pool;   /* 输入池 */
    struct entropy_pool output_pool;  /* 输出池 */

    /* SHA-256 上下文 */
    uint32_t sha256_state[8];
    uint8_t sha256_buf[64];
    uint32_t sha256_buflen;

    spinlock_t read_lock;
    uint64_t rdrand_count, virtio_count, fallback_count;
};

struct entropy_pool {
    uint32_t pool[1024];      /* 4096 字节熵池 */
    uint32_t digest[8];       /* SHA-256 中间摘要 */
    int entropy_count;        /* 当前熵位估计 */
    spinlock_t lock;
    uint64_t bytes_added, bytes_extracted, mix_count;
};
```

### 初始化序列

```
1. 检测 CPU RDRAND/RDSEED 支持 (CPUID.01H:ECX[30], CPUID.07H:EBX[18])
2. 尝试初始化 Virtio-RNG
3. 初始化输入/输出熵池 (清零)
4. 初始化 SHA-256 上下文
5. 从所有可用熵源填充初始熵
6. 混合熵池 (SHA-256)
7. 设置活跃熵源优先级: RDRAND > Virtio > Timer Jitter
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -device virtio-rng-pci \
    -cpu qemu64,+rdrand
```

---

## 21. ACPI 电源

### 硬件兼容性

| 设备 | 说明 |
|------|------|
| ACPI 1.0 | RSDT, FADT, DSDT |
| ACPI 2.0+ | XSDT, 扩展 FADT |
| QEMU ACPI | QEMU 生成的 ACPI 表 |

### 完整 API 参考

```c
/* 初始化 */
int acpi_init(void);                   /* 初始化 ACPI */
struct acpi_device *acpi_get_device(void);  /* 获取设备实例 */

/* 表操作 */
int acpi_find_rsdp(void);              /* 查找 RSDP */
int acpi_verify_checksum(const void *table, size_t len); /* 验证校验和 */
int acpi_parse_tables(void);           /* 解析所有表 */
int acpi_parse_madt(void);             /* 解析 MADT */
int acpi_parse_fadt(void);             /* 解析 FADT */
struct acpi_table_header *acpi_find_table(const char *signature); /* 查找表 */

/* 电源管理 */
int acpi_enable(void);                 /* 启用 ACPI */
int acpi_disable(void);                /* 禁用 ACPI */
int acpi_sleep(enum acpi_sleep_state state);  /* 睡眠 */
int acpi_reboot(void);                 /* 重启 */
int acpi_shutdown(void);               /* 关机 */

/* 电池 */
int acpi_battery_update(void);         /* 更新电池信息 */
int acpi_battery_get_info(struct acpi_battery_info *info); /* 获取电池信息 */
```

### 关键数据结构

```c
struct acpi_device {
    int initialized, acpi_enabled;
    uint64_t rsdp_phys;
    struct acpi_rsdp rsdp;
    uint64_t rsdt_phys, xsdt_phys;
    int use_xsdt;
    uint32_t table_count;

    /* 已解析的表 */
    struct acpi_madt *madt;
    struct acpi_fadt *fadt;

    /* MADT 解析结果 */
    uint32_t lapic_addr;
    int cpu_count;
    struct { uint8_t processor_id, apic_id; int enabled; } cpus[256];
    int ioapic_count;
    struct { uint8_t id; uint32_t addr, gsi_base; } ioapics[8];

    /* 电源管理 */
    uint16_t sci_irq;
    uint32_t pm1a_evt, pm1a_cnt, pm_tmr;

    /* 电池 */
    struct acpi_battery_info battery;
    spinlock_t lock;
};

struct acpi_battery_info {
    int present, charging, discharging, critical;
    uint32_t capacity, full_capacity, design_capacity;
    uint32_t voltage, rate;
    uint8_t percent;
};

enum acpi_sleep_state {
    ACPI_STATE_S0, ACPI_STATE_S1, ACPI_STATE_S2,
    ACPI_STATE_S3, ACPI_STATE_S4, ACPI_STATE_S5
};
```

### 初始化序列

```
1. 在 EBDA 和 BIOS ROM 区域搜索 RSDP (签名 "RSD PTR ")
2. 验证 RSDP 校验和
3. 根据 RSDP 版本选择 RSDT (1.0) 或 XSDT (2.0+)
4. 映射并验证 RSDT/XSDT
5. 遍历表入口，解析 MADT 和 FADT
6. 从 MADT 提取 CPU 和 IOAPIC 信息
7. 从 FADT 获取电源管理寄存器地址
8. 启用 ACPI (写入 SMI_CMD + ACPI_ENABLE)
9. 映射电源管理寄存器
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -machine q35 \
    -global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=on
```

---

## 22. 看门狗

### 硬件兼容性

| 设备 | PCI 厂商 ID | PCI 设备 ID | 说明 |
|------|------------|------------|------|
| Intel i6300ESB | 0x8086 | 0x25AB | QEMU 兼容看门狗 |
| Intel TCO | 0x8086 | 多种 | PCH TCO 看门狗 |

### 完整 API 参考

```c
/* 初始化 */
int watchdog_init(void);               /* 初始化看门狗子系统 */
int wdt_probe(void);                   /* 探测 i6300ESB */
struct wdt_device *wdt_get_device(void);  /* 获取设备实例 */

/* 控制 */
int wdt_start(void);                   /* 启动看门狗 */
int wdt_stop(void);                    /* 停止看门狗 */
int wdt_keepalive(void);               /* 喂狗/保活 */
int wdt_set_timeout(uint32_t seconds); /* 设置超时 */
uint32_t wdt_get_timeout(void);        /* 获取超时 */
uint32_t wdt_get_timeleft(void);       /* 获取剩余时间 */
enum wdt_status wdt_get_status(void);  /* 获取状态 */
```

### 关键数据结构

```c
struct wdt_device {
    char name[32];
    int id;
    enum wdt_status status;
    uint32_t timeout;          /* 超时时间 (秒) */
    uint32_t min_timeout;      /* 最小超时 (1s) */
    uint32_t max_timeout;      /* 最大超时 (3600s) */
    int nowayout;              /* 是否不允许停止 */

    struct wdt_ops *ops;
    void *priv;

    /* i6300ESB 特有 */
    uint8_t pci_bus, pci_dev, pci_func;
    uint64_t mmio_base;

    uint64_t keepalive_count, expire_count, last_feed_time;
    spinlock_t lock;
};

struct wdt_ops {
    int (*start)(void *priv);
    int (*stop)(void *priv);
    int (*keepalive)(void *priv);
    int (*set_timeout)(void *priv, uint32_t timeout);
    uint32_t (*get_timeleft)(void *priv);
};

enum wdt_status {
    WDT_STATUS_UNKNOWN, WDT_STATUS_STOPPED,
    WDT_STATUS_RUNNING, WDT_STATUS_EXPIRED
};
```

### 初始化序列

```
1. PCI 探测 (8086:25AB)
2. 启用 PCI 设备
3. 映射 MMIO 寄存器
4. 停止看门狗 (确保不会立即触发)
5. 设置默认超时 (30s)
6. 注册看门狗框架
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -device i6300esb
```

---

## 23. PWM 控制

### 硬件兼容性

| 设备 | 说明 |
|------|------|
| Intel PWM | PCH PWM 控制器 (背光) |
| 通用 PWM | 任何 PWM 控制器 |

### 完整 API 参考

```c
/* 初始化 */
int pwm_init(void);                    /* 初始化 PWM 子系统 */

/* 设备管理 */
int pwm_add_device(struct pwm_device *dev);
int pwm_del_device(struct pwm_device *dev);
struct pwm_device *pwm_get_device(int id);

/* PWM 配置 */
int pwm_config(struct pwm_device *dev, int channel,
               uint32_t duty_ns, uint32_t period_ns);
int pwm_enable(struct pwm_device *dev, int channel);
int pwm_disable(struct pwm_device *dev, int channel);
int pwm_set_polarity(struct pwm_device *dev, int channel, uint8_t polarity);

/* 背光控制 */
int pwm_backlight_set(int id, uint8_t brightness);  /* 设置亮度 (0-100%) */
uint8_t pwm_backlight_get(int id);                   /* 获取亮度 */

/* 风扇控制 */
int pwm_fan_set_speed(int id, uint8_t speed);        /* 设置风扇速度 (0-100%) */
uint8_t pwm_fan_get_speed(int id);                   /* 获取风扇速度 */
uint32_t pwm_fan_get_rpm(int id);                    /* 获取实际 RPM */
```

### 关键数据结构

```c
struct pwm_device {
    char name[32];
    int id;
    int num_channels;           /* 最大 4 通道 */
    struct pwm_ops *ops;
    void *priv;
    uint64_t mmio_base;
    struct pwm_channel channels[4];
    spinlock_t lock;
    struct list_head list;
};

struct pwm_channel {
    int id;
    struct pwm_channel_config config;
    spinlock_t lock;
    uint8_t brightness;         /* 背光亮度 */
    uint8_t fan_speed;          /* 风扇速度 */
    uint32_t fan_rpm;           /* 实际 RPM */
};

struct pwm_channel_config {
    uint32_t period_ns;         /* 周期 (纳秒) */
    uint32_t duty_ns;           /* 占空比 (纳秒) */
    uint8_t polarity;           /* 极性 */
    int enabled;
    enum pwm_mode mode;         /* NORMAL/BACKLIGHT/FAN */
};

struct pwm_ops {
    int (*config)(void *priv, int channel, uint32_t duty_ns, uint32_t period_ns);
    int (*enable)(void *priv, int channel);
    int (*disable)(void *priv, int channel);
    int (*set_polarity)(void *priv, int channel, uint8_t polarity);
};
```

### 初始化序列

```
1. 初始化 PWM 子系统
2. 探测 PWM 控制器
3. 映射 MMIO 寄存器
4. 禁用所有通道
5. 注册 PWM 设备
6. 配置默认模式 (背光/风扇)
```

---

## 24. SD/MMC

### 硬件兼容性

| 卡类型 | 容量 | 总线宽度 | 说明 |
|--------|------|---------|------|
| SD V1.x | ≤ 2GB | 1/4 位 | 标准容量 |
| SD V2.0 | ≤ 32GB | 1/4 位 | 高容量 (SDHC) |
| SDXC | ≤ 2TB | 1/4/8 位 | 扩展容量 |
| eMMC | 多种 | 1/4/8 位 | 嵌入式 MMC |
| MMC | 多种 | 1/4/8 位 | MultiMediaCard |

### 完整 API 参考

```c
/* 初始化 */
int sdmmc_init(void);                  /* 初始化 SD/MMC 子系统 */
int sdmmc_card_init(struct sdmmc_host *host); /* 初始化卡 */
struct sdmmc_host *sdmmc_get_host(int id);    /* 获取主机实例 */

/* 块读写 */
int sdmmc_read_blocks(struct sdmmc_card *card, uint32_t start_block,
                      void *buf, uint32_t count);
int sdmmc_write_blocks(struct sdmmc_card *card, uint32_t start_block,
                       const void *buf, uint32_t count);

/* 卡信息 */
uint64_t sdmmc_get_capacity(struct sdmmc_card *card);
uint32_t sdmmc_get_block_size(struct sdmmc_card *card);
enum sdmmc_card_type sdmmc_get_card_type(struct sdmmc_card *card);

/* SPI 模式 */
int sdmmc_spi_init(struct sdmmc_host *host);
```

### 关键数据结构

```c
struct sdmmc_host {
    char name[32];
    int id;
    struct sdmmc_host_ops *ops;
    void *priv;
    uint64_t mmio_base;
    int irq;
    struct sdmmc_card card;
    int initialized;
    spinlock_t lock;
};

struct sdmmc_card {
    enum sdmmc_card_type type;
    enum sdmmc_state state;
    uint16_t rca;
    uint32_t ocr;
    struct sdmmc_cid cid;
    struct sdmmc_csd csd;
    uint64_t capacity;
    uint32_t block_size, num_blocks;
    uint8_t bus_width;
    uint32_t clock_freq;
    int high_speed, spi_mode;
    struct sdmmc_host *host;
    int initialized, write_protected;
};

struct sdmmc_host_ops {
    int (*send_cmd)(struct sdmmc_host *host, uint8_t cmd, uint32_t arg,
                    uint8_t resp_type, uint32_t *resp);
    int (*set_clock)(struct sdmmc_host *host, uint32_t freq);
    int (*set_bus_width)(struct sdmmc_host *host, uint8_t width);
    int (*read_data)(struct sdmmc_host *host, void *buf, size_t len);
    int (*write_data)(struct sdmmc_host *host, const void *buf, size_t len);
};
```

### 初始化序列

```
1. 初始化 SD/MMC 主机控制器
2. 发送 CMD0 (GO_IDLE_STATE) - 复位卡
3. 发送 CMD8 (SEND_IF_COND) - 检测 SD V2.0+
4. 发送 ACMD41 (SD_SEND_OP_COND) - 初始化卡，等待就绪
5. 发送 CMD2 (ALL_SEND_CID) - 获取 CID
6. 发送 CMD3 (SEND_RELATIVE_ADDR) - 获取 RCA
7. 发送 CMD9 (SEND_CSD) - 获取 CSD
8. 发送 CMD7 (SELECT_CARD) - 选择卡
9. 配置总线宽度 (ACMD6)
10. 配置高速模式 (CMD6)
```

---

## 25. GPU 2D 加速

### 硬件兼容性

| 设备 | PCI 厂商 ID | PCI 设备 ID | 说明 |
|------|------------|------------|------|
| Bochs VBE | 0x1234 | 0x1111 | QEMU 标准 VGA |
| Virtio-GPU | 0x1AF4 | 0x1050 | Virtio GPU 设备 |

### 完整 API 参考

```c
/* 初始化 */
int gpu_init(void);                    /* 初始化 GPU 驱动 */
int gpu_probe(void);                   /* 探测 Bochs VBE */
struct gpu_device *gpu_get_device(void);  /* 获取设备实例 */

/* 模式设置 */
int gpu_set_mode(uint32_t xres, uint32_t yres, uint32_t bpp);
int gpu_get_mode(uint32_t *xres, uint32_t *yres, uint32_t *bpp);

/* 2D 加速 */
int gpu_fill_rect(const struct gpu_fill_op *op);   /* 矩形填充 */
int gpu_bitblt(const struct gpu_bitblt_op *op);     /* 位块传输 */
int gpu_draw_line(uint32_t x1, uint32_t y1,
                  uint32_t x2, uint32_t y2, uint32_t color); /* 画线 */

/* 帧缓冲 */
void *gpu_get_framebuffer(void);       /* 获取帧缓冲地址 */
uint64_t gpu_get_fb_size(void);        /* 获取帧缓冲大小 */

/* GEM 对象管理 */
struct gpu_gem_object *gpu_gem_create(uint32_t width, uint32_t height, uint32_t bpp);
void gpu_gem_destroy(struct gpu_gem_object *obj);
struct gpu_gem_object *gpu_gem_lookup(uint32_t id);
void gpu_gem_get(struct gpu_gem_object *obj);
void gpu_gem_put(struct gpu_gem_object *obj);
```

### 关键数据结构

```c
struct gpu_device {
    uint8_t pci_bus, pci_dev, pci_func;
    uint64_t fb_phys, fb_virt, fb_size;
    uint32_t xres, yres, bpp, pitch;
    int enabled;
    struct gpu_gem_object *gem_objects;
    int gem_count;
    uint32_t next_gem_id;
    spinlock_t gem_lock, lock;
    uint64_t fill_count, blit_count;
};

struct gpu_gem_object {
    uint32_t id;
    uint32_t width, height, bpp, pitch;
    uint64_t size;
    uint64_t vaddr, paddr;
    struct kref refcount;
    struct list_head list;
};

struct gpu_fill_op {
    struct gpu_rect rect;     /* 目标矩形 */
    uint32_t color;           /* ARGB32 颜色 */
};

struct gpu_bitblt_op {
    struct gpu_rect src_rect;
    uint32_t src_x, src_y;
    uint32_t dst_x, dst_y;
    uint32_t width, height;
};

struct gpu_rect {
    uint32_t x, y, width, height;
};
```

### 初始化序列

```
1. PCI 探测 (1234:1111)
2. 启用 PCI 设备
3. 映射帧缓冲 MMIO
4. 设置显示模式 (1024x768x32)
5. 初始化 GEM 对象管理器
6. 清屏
```

### 测试配置 (QEMU)

```bash
qemu-system-x86_64 \
    -vga std -display gtk
```

---

## 26. 蓝牙

### 硬件兼容性

| 设备 | 传输方式 | 说明 |
|------|---------|------|
| USB 蓝牙适配器 | USB HCI | 通用 USB 蓝牙 dongle |
| Intel 蓝牙 | USB/UART | Intel 组合 WiFi/蓝牙 |

### 完整 API 参考

```c
/* 初始化 */
int bluetooth_init(void);              /* 初始化蓝牙子系统 */
struct bt_device *bt_get_device(void); /* 获取设备实例 */

/* HCI 命令 */
int bt_hci_send_cmd(uint16_t opcode, const void *params, uint8_t plen);
int bt_hci_send_cmd_sync(uint16_t opcode, const void *params, uint8_t plen,
                          struct bt_hci_evt *response);

/* 设备发现 */
int bt_inquiry(int duration);          /* 扫描设备 (1.28s * duration) */
int bt_inquiry_cancel(void);           /* 取消扫描 */
int bt_remote_name_request(bt_addr_t *addr); /* 获取远端名称 */

/* 连接管理 */
int bt_create_connection(bt_addr_t *addr);  /* 创建连接 */
int bt_disconnect(uint16_t handle);          /* 断开连接 */

/* L2CAP */
int bt_l2cap_send(uint16_t cid, const void *data, size_t len);

/* 本地信息 */
int bt_read_local_info(void);          /* 读取本地信息 */
const char *bt_addr_to_str(bt_addr_t *addr); /* 地址转字符串 */
```

### 关键数据结构

```c
struct bt_device {
    int initialized;
    bt_addr_t local_addr;
    char local_name[248];
    struct bt_hci_transport *transport;
    struct bt_connection connections[4];
    int conn_count;
    struct bt_device_info discovered[16];
    int discovered_count, inquiring;

    /* HCI 状态 */
    uint8_t hci_version;
    uint16_t hci_revision, manufacturer;
    uint16_t acl_mtu, acl_max_pkt;
    uint8_t sco_mtu;
    uint16_t sco_max_pkt;
    spinlock_t lock;
};

struct bt_hci_transport {
    const char *name;
    int (*open)(void);
    int (*close)(void);
    int (*send_cmd)(const uint8_t *data, size_t len);
    int (*send_acl)(const uint8_t *data, size_t len);
    int (*recv)(uint8_t *buf, size_t len);
};

struct bt_connection {
    int active;
    uint16_t handle;
    bt_addr_t addr;
    uint8_t type, role;
};

struct bt_device_info {
    bt_addr_t addr;
    uint8_t psrm, psm;
    uint16_t co;
    uint32_t cod;
    char name[248];
    int name_known;
};
```

### 初始化序列

```
1. 初始化 HCI 传输层 (USB/UART)
2. 发送 HCI Reset 命令
3. 读取本地信息 (版本、特性、地址)
4. 读取缓冲区大小
5. 设置事件掩码
6. 配置扫描参数
```

---

## 27. 触摸板

### 硬件兼容性

| 设备 | 接口 | 说明 |
|------|------|------|
| I2C HID 触摸板 | I2C | 现代笔记本触摸板 |
| Synaptics I2C | I2C | Synaptics 触摸板 |
| Elan I2C | I2C | ELAN 触摸板 |

### 完整 API 参考

```c
/* 初始化 */
int touchpad_init(void);               /* 初始化触摸板驱动 */
int touchpad_probe_i2c(uint16_t i2c_addr, int adapter_id); /* I2C 探测 */
struct touchpad_device *touchpad_get_device(void);  /* 获取设备实例 */

/* 报告处理 */
int touchpad_read_report(void);        /* 读取 HID 报告 */
int touchpad_process_report(const struct touchpad_report *report); /* 处理报告 */

/* 手势识别 */
enum touchpad_gesture touchpad_detect_gesture(const struct touchpad_report *report);

/* 状态查询 */
int touchpad_get_fingers(struct touchpad_finger *fingers, int *count);
int touchpad_get_gesture(enum touchpad_gesture *gesture);

/* 配置 */
int touchpad_set_sensitivity(int level);
int touchpad_set_tap_to_click(int enable);
```

### 关键数据结构

```c
struct touchpad_device {
    int initialized;
    char name[64];

    /* I2C 连接 */
    uint16_t i2c_addr;
    int i2c_adapter_id;
    void *i2c_client;

    /* HID 描述符 */
    uint16_t hid_desc_addr, hid_report_len;

    /* 触摸板参数 */
    uint16_t max_x, max_y;
    uint16_t resolution_x, resolution_y;

    /* 当前状态 */
    struct touchpad_report report;
    struct touchpad_gesture_state gesture;

    /* 输入设备集成 */
    struct input_dev *input_dev;

    /* 手势参数 */
    uint16_t tap_threshold;         /* 点击移动阈值 */
    uint16_t tap_time_threshold;    /* 点击时间阈值 */
    uint16_t scroll_threshold;      /* 滚动阈值 */

    uint64_t report_count, gesture_count;
    spinlock_t lock;
};

struct touchpad_report {
    uint8_t contact_count;
    uint8_t button;
    struct touchpad_finger fingers[5];
    uint16_t scan_time;
};

struct touchpad_finger {
    int active;
    uint16_t x, y, pressure, width;
    uint8_t id;
};

enum touchpad_gesture {
    TOUCHPAD_GESTURE_NONE,
    TOUCHPAD_GESTURE_TAP,             /* 单击 */
    TOUCHPAD_GESTURE_DOUBLE_TAP,      /* 双击 */
    TOUCHPAD_GESTURE_TWO_FINGER_TAP,  /* 双指点击 (右键) */
    TOUCHPAD_GESTURE_SCROLL_UP,       /* 向上滚动 */
    TOUCHPAD_GESTURE_SCROLL_DOWN,     /* 向下滚动 */
    TOUCHPAD_GESTURE_SCROLL_LEFT,     /* 向左滚动 */
    TOUCHPAD_GESTURE_SCROLL_RIGHT,    /* 向右滚动 */
    TOUCHPAD_GESTURE_PINCH_IN,        /* 缩小 */
    TOUCHPAD_GESTURE_PINCH_OUT,       /* 放大 */
};
```

### 初始化序列

```
1. 通过 I2C 探测触摸板 (读取 HID 描述符)
2. 解析 HID 描述符，获取报告格式
3. 设置触摸板参数 (分辨率、最大坐标)
4. 初始化手势识别状态机
5. 注册为输入设备
6. 启用 HID 报告模式
```

---

## 28. TPM 2.0

### 硬件兼容性

| 设备 | MMIO 基址 | 说明 |
|------|----------|------|
| TPM TIS | 0xFED40000 | TPM Interface Specification |
| QEMU TPM | 0xFED40000 | QEMU 模拟 TPM (SWTPM) |
| Intel PTT | 0xFED40000 | Intel Platform Trust Technology |

### 完整 API 参考

```c
/* 初始化 */
int tpm_init(void);                    /* 初始化 TPM 驱动 */
int tpm_probe(void);                   /* 探测 TPM 设备 */
struct tpm_device *tpm_get_device(void);  /* 获取设备实例 */

/* TIS 接口 */
int tpm_tis_request_locality(int locality);  /* 请求本地性 */
int tpm_tis_release_locality(int locality);  /* 释放本地性 */
int tpm_tis_send(const uint8_t *buf, size_t len);  /* 发送命令 */
int tpm_tis_recv(uint8_t *buf, size_t *len);       /* 接收响应 */

/* TPM 2.0 命令 */
int tpm2_send_cmd(const void *cmd, size_t cmd_len,
                  void *resp, size_t *resp_len);     /* 发送 TPM2 命令 */
int tpm2_self_test(int full_test);                   /* 自检 */
int tpm2_startup(void);                              /* 启动 TPM */
int tpm2_shutdown(void);                             /* 关闭 TPM */

/* 随机数 */
int tpm2_get_random(void *buf, size_t len);          /* 获取随机数 */
int tpm2_stir_random(const void *data, size_t len);  /* 添加熵 */

/* 密钥操作 */
int tpm2_create_primary_key(void);                   /* 创建主密钥 */
int tpm2_flush_context(uint32_t handle);             /* 刷新上下文 */

/* PCR 操作 */
int tpm2_pcr_extend(uint32_t pcr_idx, const void *digest, size_t digest_len);
int tpm2_pcr_read(uint32_t pcr_idx, void *digest, size_t digest_len);
```

### 关键数据结构

```c
struct tpm_device {
    int initialized;
    uint64_t mmio_base;              /* 0xFED40000 */
    struct tpm_capability caps;
    int active_locality;
    int tpm_ready, self_test_passed;
    uint64_t cmd_count, error_count;
    spinlock_t lock;
};

struct tpm_capability {
    uint32_t tpm2_family;
    uint32_t firmware_version_major, firmware_version_minor;
    uint32_t max_command_size, max_response_size;
    uint32_t manufacturer_id;
};

struct tpm_cmd_header {
    uint16_t tag;     /* 0x8001=ST_NO_SESSIONS, 0x8002=ST_SESSIONS */
    uint32_t size;    /* 命令总大小 */
    uint32_t code;    /* 命令码 */
} __attribute__((packed));

struct tpm_resp_header {
    uint16_t tag;
    uint32_t size;
    uint32_t code;    /* 0x000=成功 */
} __attribute__((packed));
```

### 初始化序列

```
1. 映射 TPM MMIO 空间 (0xFED40000)
2. 请求 Locality 0
3. 等待 TPM 就绪 (STS.COMMAND_READY)
4. 发送 TPM2_Startup(ST_CLEAR)
5. 执行 TPM2_SelfTest(YES)
6. 获取 TPM 能力 (TPM2_CC_GetCapability)
7. 验证固件版本
8. 释放 Locality (保持可用)
```

### 测试配置 (QEMU)

```bash
# 需要安装 swtpm
swtpm socket --tpmstate dir=/tmp/tpm --ctrl type=unixio,path=/tmp/swtpm-sock &
qemu-system-x86_64 \
    -chardev socket,id=chrtpm,path=/tmp/swtpm-sock \
    -tpmdev emulator,id=tpm0,chardev=chrtpm \
    -device tpm-tis,tpmdev=tpm0
```

---

## 附录: 驱动框架通用模式

### PCI 驱动模板

```c
int xxx_init(void) {
    /* 1. PCI 探测 */
    if (pci_find_device(VENDOR_ID, DEVICE_ID, &bus, &dev, &func) != 0)
        return -1;

    /* 2. 启用 PCI 设备 */
    pci_enable_device(bus, dev, func);
    pci_set_master(bus, dev, func);

    /* 3. 获取资源 */
    uint64_t mmio = pci_get_bar(bus, dev, func, 0);
    uint8_t irq = pci_get_irq(bus, dev, func);

    /* 4. 映射 MMIO */
    void *mmio_virt = vmm_map_mmio(mmio, size);

    /* 5. 硬件初始化 */
    /* ... */

    /* 6. 注册中断 */
    idt_register_handler(IRQ_BASE + irq, xxx_interrupt_handler);

    return 0;
}
```

### 中断处理模板

```c
void xxx_interrupt_handler(struct interrupt_frame *frame) {
    unsigned long flags = spin_lock_irqsave(&dev->lock);

    /* 1. 读取中断状态 */
    uint32_t status = mmio_read32(dev->mmio + REG_STATUS);

    /* 2. 处理中断 */
    if (status & STATUS_RX_COMPLETE) {
        xxx_handle_rx(dev);
    }
    if (status & STATUS_TX_COMPLETE) {
        xxx_handle_tx(dev);
    }

    /* 3. 清除中断 */
    mmio_write32(dev->mmio + REG_STATUS, status);

    spin_unlock_irqrestore(&dev->lock, flags);
}
```

### QEMU 测试通用参数

```bash
# 完整测试配置
qemu-system-x86_64 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
    -drive if=pflash,format=raw,file=/tmp/OVMF_VARS.fd \
    -drive file=ISO/zratroos.img,format=raw \
    -m 2G -smp 2 \
    -serial stdio \
    -device AC97 \
    -device intel-hda -device hda-duplex \
    -device i6300esb \
    -device virtio-rng-pci \
    -device qemu-xhci \
    -rtc base=localtime
```
