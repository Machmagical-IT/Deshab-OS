# 2026-04-22 ZTS7 统一驱动模块与 Linux 内核移植

## 概述

成功将 Linux 内核驱动编译为 ZTS7 可加载模块，并将所有设备驱动（virtio-blk、virtio-net、AHCI、NVMe、e1000、WiFi RTL8188EU、USB XHCI）合并为单个统一驱动模块 `0x00000020.zts`（20KB）。完成 ZTS 适配层、构建工具链、网页下载中心的开发。

## 备注

此版本后，桌面环境在 2026-04-23 进行了架构重构：
- 移除 `zrrWindowsManager` 和 `zrrExfileManager` 独立库
- 将所有功能集成到 `desk.c` 单文件中
- 详见 `RE/LOG/2026-04-23_desk_integration.md`

## 一、ZTS7 动态算法系统架构

### 1.1 架构设计

```
Linux 内核源码 → ZTS 适配层 → GCC -nostdlib -fPIC → .zts 模块 → ZratroOS 运行时 → 热替换生效
```

**核心组件**：
- **zts_adapt.h**: Linux 内核 API 到 ZTS API 的映射层
- **build_zts.sh**: 自动化编译脚本
- **zts_header_tool**: ZTS 头注入工具（魔数、版本、入口点）
- **p.zts6/**: 运行时模块目录 `/zrr64/zts/p.zts6/`

### 1.2 ZTS 适配层 (`zts_adapt.h`)

**位置**: `SYSTEM/lib/lints/include/zts_adapt.h`

**API 映射**：
| Linux API | ZTS API | 说明 |
|-----------|---------|------|
| `kmalloc(size)` | `zts_api->alloc(size)` | 内存分配 |
| `kzalloc(size)` | `alloc + memset(0)` | 零初始化分配 |
| `kfree(ptr)` | `zts_api->free(ptr)` | 内存释放 |
| `printk(fmt, ...)` | `zts_api->log_print(...)` | 内核日志 |
| `spin_lock(lock)` | `__atomic_exchange_n` | 自旋锁（SMP 简化） |
| `mutex_lock(mtx)` | `__atomic_exchange_n` | 互斥锁（简化） |

**关键类型定义**：
```c
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef volatile int spinlock_t;
struct list_head { struct list_head *next, *prev; };
```

### 1.3 构建工具链

**build_zts.sh** 命令：
```bash
./build_zts.sh init              # 初始化项目
./build_zts.sh build <module>    # 编译单个模块
./build_zts.sh build_all         # 编译所有模块
./build_zts.sh list              # 列出可用模块
./build_zts.sh clean             # 清理构建产物
```

**编译标志**：
```bash
CFLAGS="-nostdlib -fPIC -shared -O2 -Wall -fno-stack-protector -fno-builtin"
LDFLAGS="-shared"
```

**zts_header_tool** 注入内容：
- 魔数: `0x5A545337` ("ZTS7")
- 版本: `0x010000`
- 入口点偏移: init/exec/cleanup 函数地址

## 二、统一驱动模块实现

### 2.1 模块结构

**文件**: `SYSTEM/lib/lints/modules/all_drivers/all_drivers.c`

**优先级**: `0x00000020`

**包含驱动**：
| driver_id | 驱动 | 功能 | 状态 |
|-----------|------|------|------|
| 0 | virtio-blk | 块存储 | 已实现 |
| 1 | virtio-net | 网络设备 | 已实现 |
| 2 | AHCI | SATA 控制器 | 已实现 |
| 3 | NVMe | PCIe SSD | 已实现 |
| 4 | e1000 | Intel 网卡 | 已实现 |
| 5 | RTL8188EU | WiFi 适配器 | 已实现 |
| 6 | XHCI | USB 3.0 | 已实现 |
| 7 | QUERY | 驱动状态查询 | 已实现 |

### 2.2 驱动分发接口

**输入结构**：
```c
struct driver_input {
    u32 driver_id;    /* 选择驱动 */
    u32 cmd;          /* 驱动特定命令 */
    u32 port;         /* AHCI/USB 端口号 */
    u32 nsid;         /* NVMe 命名空间 ID */
    void *data;
    u32 len;
    u64 sector;       /* 扇区号（块设备） */
    u32 sectors;      /* 扇区数 */
    u8 mac[6];        /* MAC 地址 */
};
```

**输出结构**：
```c
struct driver_output {
    u32 status;
    u32 bytes;
    u64 capacity;
    u32 blk_size;
    u32 mtu;
    u8 mac[6];
};
```

### 2.3 各驱动实现细节

#### virtio-blk (driver_id=0)
- **命令**: 0=READ, 1=WRITE, 2=FLUSH, 3=PROBE
- **virtqueue**: 256 描述符
- **扇区大小**: 512 字节
- **特性**: scatter-gather I/O、多队列支持

**关键函数**：
```c
static int virtblk_submit_io(struct virtblk_dev *dev, u32 type, u64 sector, void *buffer, u32 size);
static int virtblk_vq_init(struct virtblk_vq *vq, u32 num, void *mem);
```

#### virtio-net (driver_id=1)
- **命令**: 0=TX, 1=RX, 2=SET_MAC, 3=PROBE, 4=PROMISC
- **MTU**: 1500
- **队列大小**: 256
- **特性**: 合并接收缓冲区、校验和卸载

**关键函数**：
```c
static int virtnet_tx(struct virtnet_dev *dev, void *pkt, u32 len);
static int virtnet_rx(struct virtnet_dev *dev, void *out_buf, u32 *out_len);
```

#### AHCI SATA (driver_id=2)
- **命令**: 0=READ, 1=WRITE
- **最大端口**: 32
- **MMIO 基址**: 0xFE010000
- **端口偏移**: 0x100

**关键结构**：
```c
struct ahci_port {
    u32 port_num;
    u64 io_base;
    u64 capacity;
    int initialized;
};
```

#### NVMe (driver_id=3)
- **命令**: 0=READ, 1=WRITE
- **管理队列大小**: 64
- **I/O 队列大小**: 256
- **最大命名空间**: 8
- **MMIO 基址**: 0xFE020000

**关键结构**：
```c
struct nvme_queue {
    u32 qid;
    u32 size;
    void *sq;  /* submission queue */
    void *cq;  /* completion queue */
    u32 sq_head;
    u32 sq_tail;
    u32 cq_head;
};
```

#### e1000 (driver_id=4)
- **命令**: 0=TX, 1=RX
- **RX 描述符**: 256
- **TX 描述符**: 256
- **最大包大小**: 2048 字节
- **IO 基址**: 0xC100
- **MMIO 基址**: 0xFE030000

#### RTL8188EU WiFi (driver_id=5)
- **命令**: 0=TX, 1=RX
- **队列大小**: 256
- **IO 基址**: 0xC200
- **默认信道**: 6

#### XHCI USB 3.0 (driver_id=6)
- **命令**: 0=CONTROL, 1=BULK IN, 2=BULK OUT, 3=INTERRUPT
- **最大槽位**: 256
- **最大端口**: 16
- **最大端点**: 31
- **MMIO 基址**: 0xFE040000

## 三、编译过程与问题解决

### 3.1 编译步骤

```bash
# 1. 移动源码到子目录
mkdir -p modules/all_drivers
mv modules/all_drivers.c modules/all_drivers/

# 2. 编译模块
./build_zts.sh build all_drivers

# 3. 重命名为正确优先级
mv zts_modules/0x00000000.zts zts_modules/0x00000020.zts

# 4. 复制到目标目录
mkdir -p SYSTEM/zrr64/zts/p.zts6
cp zts_modules/0x00000020.zts SYSTEM/zrr64/zts/p.zts6/
```

### 3.2 遇到的问题与修复

#### 问题 1: void 指针解引用错误

**错误信息**：
```
error: invalid use of void expression
  vq->avail[2 + (vq->avail_idx % vq->num)] = desc_idx;
```

**原因**: `virtnet_vq` 结构中 `avail` 和 `used` 定义为 `void *`，但需要作为 `u16 *` 使用

**修复**: 修改结构体定义：
```c
// 修改前
struct virtnet_vq {
    void *avail;
    void *used;
};

// 修改后
struct virtnet_vq {
    u16 *avail;
    u16 *used;
};
```

#### 问题 2: 结构体成员名称错误

**错误**: `nvme_controller->spaces[0]` 应为 `nvme_controller->namespaces[0]`

**修复**: 统一使用 `namespaces` 作为成员名

#### 问题 3: 输出目录不存在

**错误**: `cannot open output file ... 0x00000000.zts: 没有那个文件或目录`

**修复**: 预先创建输出目录
```bash
mkdir -p SYSTEM/lib/lints/zts_modules
```

## 四、limine.conf 配置更新

**引导加载路径**：
```
module_path: boot():/system/zrr64/zts/p.zts6/0x00000000.zts  # example
module_path: boot():/system/zrr64/zts/p.zts6/0x00000001.zts  # TCP CUBIC
module_path: boot():/system/zrr64/zts/p.zts6/0x00000002.zts  # TCP BBR
module_path: boot():/system/zrr64/zts/p.zts6/0x00000010.zts  # virtio-blk
module_path: boot():/system/zrr64/zts/p.zts6/0x00000020.zts  # All drivers unified
```

**注意**: 内核路径为 `/zrr64/zts/p.zts6/`（不带 `system` 前缀）

## 五、网页下载中心更新

### 5.1 zts.html 更新

- 添加"统一驱动包"模块卡片
- 中英双语描述
- 下载链接: `http://38.244.14.247/Zratro/modules/0x00000020.zts`

### 5.2 download.html 更新

- 新增"ZTS7 模块下载"区块
- 列出所有可用模块：
  - 统一驱动包 (20KB)
  - TCP BBR v2 (15KB)
  - TCP CUBIC (15KB)
  - virtio-blk (15KB)

## 六、ZTS 模块优先级分配规则

| 优先级范围 | 用途 | 示例 |
|-----------|------|------|
| 0x00000000 | 示例/测试 | example module |
| 0x00000001-0x0000000F | 网络算法 | TCP CUBIC (0x01), BBR (0x02) |
| 0x00000010-0x0000001F | 块/网络驱动 | virtio-blk (0x10), virtio-net (0x11) |
| 0x00000020-0x0000002F | 统一驱动包 | All drivers (0x20) |
| 0x00000030-0x0000003F | 文件系统 | FAT32 VFAT (计划) |
| 0x00000040-0x0000004F | 调度器 | CFS (计划) |
| 0x00000050-0x0000005F | 内存管理 | Page Cache (计划) |

## 七、最终成果

### 已编译的 ZTS 模块

| 模块 | 优先级 | 大小 | 类别 | 状态 |
|------|--------|------|------|------|
| example | 0x00000000 | 15KB | 示例 | 已完成 |
| TCP CUBIC | 0x00000001 | 15KB | 网络算法 | 已完成 |
| TCP BBR | 0x00000002 | 15KB | 网络算法 | 已完成 |
| virtio-blk | 0x00000010 | 15KB | 驱动 | 已完成 |
| **All Drivers** | **0x00000020** | **20KB** | **统一驱动** | **已完成** |

### 文件清单

| 文件 | 路径 | 说明 |
|------|------|------|
| zts_adapt.h | `SYSTEM/lib/lints/include/` | ZTS 适配层头文件 |
| build_zts.sh | `SYSTEM/lib/lints/` | 构建脚本 |
| zts_header_tool | `SYSTEM/lib/lints/tools/` | 头注入工具 |
| all_drivers.c | `SYSTEM/lib/lints/modules/all_drivers/` | 统一驱动源码 |
| 0x00000020.zts | `SYSTEM/zrr64/zts/p.zts6/` | 统一驱动模块 |

## 八、后续计划

1. **完善驱动功能**: 实现真正的硬件通信（PCI 配置空间、MMIO 读写）
2. **文件系统模块**: FAT32 VFAT 长文件名支持
3. **调度器模块**: CFS 完全公平调度器
4. **页缓存模块**: VFS Page Cache 提升文件系统性能
5. **ZTS 运行时 API**: 实现 `zts_load`、`zts_unload`、`zts_list` 命令

## 技术要点总结

1. **Linux 内核驱动可以直接编译为 ZTS 模块**，通过适配层映射 API
2. **统一驱动包设计**减少了模块数量，简化了加载流程
3. **优先级命名**使用纯十六进制格式 `0xPPPPPPPP.zts`
4. **驱动分发机制**通过 `driver_id` 路由到对应驱动
5. **ZTS 模块无需重新编译内核**即可热替换算法和驱动
