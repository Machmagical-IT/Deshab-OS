# Linux 内核克隆替换计划 (Linux-to-ZTS Migration Plan)

**版本**: v1.0  
**日期**: 2026-04-23  
**目标**: 将 Linux 内核成熟算法编译为 ZTS 模块，动态替换 ZratroOS 缺陷实现

---

## 1. 项目背景

### 1.1 核心理念

ZratroOS 的 ZTS7 动态算法系统支持运行时加载 `.zts` 模块。我们可以：
1. 克隆 Linux 内核源代码
2. 提取核心算法 (如 TCP CUBIC、virtio-blk、FAT32 路径解析)
3. 通过适配层编译为 `.zts` 模块
4. 运行时动态替换 ZratroOS 的缺陷实现

### 1.2 优势

- **无需重写**: 直接使用 Linux 30+ 年成熟的内核代码
- **热替换**: 无需重启内核即可更新算法
- **可回滚**: 通过检查点机制回滚到原始实现
- **模块化**: 每个算法独立编译，按需加载

---

## 2. 优先替换清单

### 2.1 P0 - 紧急修复 (直接影响功能)

| 模块 | 当前问题 | Linux 来源 | ZTS 类别 | 预计大小 |
|------|----------|-----------|---------|---------|
| **virtio-blk** | 驱动超时，设备无响应 | `drivers/block/virtio_blk.c`<br>`drivers/virtio/virtio_ring.c` | DRIVER | ~15KB |
| **FAT32 路径解析** | mkdir 父目录查找失败 | `fs/fat/namei_vfat.c`<br>`fs/fat/dir.c` | FILESYSTEM | ~20KB |
| **鼠标光标渲染** | FB 损坏 | 自定义修复 (无需 Linux) | PATCH | ~2KB |

### 2.2 P1 - 性能提升

| 模块 | 当前问题 | Linux 来源 | ZTS 类别 | 预计大小 |
|------|----------|-----------|---------|---------|
| **TCP 拥塞控制** | 仅 Reno | `net/ipv4/tcp_cubic.c`<br>`net/ipv4/tcp_bbr.c` | NETWORK | ~10KB |
| **网络零拷贝** | 实际使用 memcpy | `net/core/skbuff.c`<br>`net/ipv4/tcp.c` (MSG_ZEROCOPY) | NETWORK | ~15KB |
| **WiFi RX/EAPOL** | 接收/握手未完成 | `drivers/net/wireless/realtek/rtl8188eu/` | DRIVER | ~50KB |

### 2.3 P2 - 架构改进

| 模块 | 当前问题 | Linux 来源 | ZTS 类别 | 预计大小 |
|------|----------|-----------|---------|---------|
| **CFS 调度器** | 缺少 PELT/组调度 | `kernel/sched/fair.c` | SCHEDULER | ~30KB |
| **VFS 页缓存** | 基础 LRU | `mm/filemap.c`<br>`fs/buffer.c` | FILESYSTEM | ~25KB |
| **内存分配器** | 位图+Per-CPU 缓存 | `mm/page_alloc.c` (buddy) | CUSTOM | ~20KB |

### 2.4 P3 - 完善功能

| 模块 | 当前问题 | Linux 来源 | ZTS 类别 | 预计大小 |
|------|----------|-----------|---------|---------|
| **自愈时间函数** | 全部返回 0 | `kernel/time/timekeeping.c` | CUSTOM | ~5KB |
| **Capability 过期检查** | 未实现 | `security/commoncap.c` | CUSTOM | ~3KB |
| **Signal core dump** | 未实现 | `fs/coredump.c` | CUSTOM | ~15KB |

---

## 3. ZTS 适配层设计

### 3.1 架构

```
┌──────────────────────────────────────────────────────────┐
│                  Linux 内核源代码                         │
│  (git clone https://github.com/torvalds/linux.git)       │
└───────────────────────┬──────────────────────────────────┘
                        │ 提取核心算法
                        ▼
┌──────────────────────────────────────────────────────────┐
│              ZTS 适配层 (zts_adapt.h)                     │
│                                                          │
│  Linux API → ZratroOS API 映射:                          │
│  ├── kmalloc()      → zts_api_alloc()                    │
│  ├── kfree()        → zts_api_free()                     │
│  ├── printk()       → zts_api_log()                      │
│  ├── jiffies        → zts_api_time()                     │
│  ├── spin_lock()    → 内联自旋锁 (需实现)                  │
│  ├── list_head      → ZratroOS 链表 (兼容)                │
│  └── workqueue      → ZratroOS 协程/任务                  │
└───────────────────────┬──────────────────────────────────┘
                        │ 编译
                        ▼
┌──────────────────────────────────────────────────────────┐
│                  .zts 模块                                │
│  (含 ZTS 头部 + 编译后代码)                                │
└───────────────────────┬──────────────────────────────────┘
                        │ zts_register_module()
                        ▼
┌──────────────────────────────────────────────────────────┐
│               ZratroOS 内核运行时                         │
│  ┌────────────────────────────────────────────┐          │
│  │  ZTS7 加载器                                │          │
│  │  ├── 验证模块 (魔数/版本/校验和)             │          │
│  │  ├── 注入 API 表                            │          │
│  │  └── 执行初始化函数                          │          │
│  └────────────────────────────────────────────┘          │
└──────────────────────────────────────────────────────────┘
```

### 3.2 适配层头文件 (zts_adapt.h)

```c
#ifndef ZTS_ADAPT_H
#define ZTS_ADAPT_H

#include "zts_algorithm.h"

/* ========== 全局 API 表指针 ========== */
extern struct zts_api_table *zts_api;

/* ========== 内存管理映射 ========== */
#define kmalloc(size)       zts_api->alloc(size)
#define kzalloc(size)       ({ void *p = zts_api->alloc(size); \
                               zts_api->memset(p, 0, size); p; })
#define kfree(ptr)          zts_api->free(ptr)
#define vmalloc(size)       zts_api->alloc(size)
#define vfree(ptr)          zts_api->free(ptr)
#define krealloc(ptr, size) zts_api->realloc(ptr, size)

/* ========== 字符串/内存操作 ========== */
#define memcpy(dst, src, n)     zts_api->memcpy(dst, src, n)
#define memset(s, c, n)         zts_api->memset(s, c, n)
#define memcmp(s1, s2, n)       ({ int ret; /* 需实现 */ })
#define strlen(s)               ({ size_t len = 0; \
                                   while (s[len]) len++; len; })
#define strcpy(d, s)            ({ char *p = d; \
                                   while ((*p++ = *s++)); d; })
#define strncpy(d, s, n)        ({ size_t i; \
                                   for (i = 0; i < n && s[i]; i++) d[i] = s[i]; \
                                   while (i < n) d[i++] = '\0'; d; })
#define strcmp(s1, s2)          zts_api->strcmp(s1, s2)
#define strncmp(s1, s2, n)      ({ int ret = 0; size_t i; \
                                   for (i = 0; i < n; i++) { \
                                       if (s1[i] != s2[i]) { ret = s1[i] - s2[i]; break; } \
                                       if (s1[i] == '\0') break; \
                                   } ret; })

/* ========== 日志映射 ========== */
#define printk(fmt, ...)    zts_api->log_print("KERN", fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)   zts_api->log_print("INFO", fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)    zts_api->log_print("ERR", fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)   zts_api->log_print("WARN", fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)  zts_api->log_print("DBG", fmt, ##__VA_ARGS__)

/* ========== 时间映射 ========== */
#define get_jiffies_64()    zts_api->time_now()
#define jiffies             zts_api->time_now()
#define msecs_to_jiffies(ms) ((ms) * 1000000ULL)  /* 假设纳秒精度 */

/* ========== 错误码 ========== */
#define ENOMEM      12
#define EINVAL      22
#define EFAULT      14
#define EAGAIN      11
#define EBUSY       16
#define ENODEV      19
#define EPERM       1
#define EACCES      13
#define ENOENT      2
#define EEXIST      17

/* ========== 简化版链表 (与 ZratroOS 兼容) ========== */
struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static inline void list_add(struct list_head *new_node, struct list_head *head) {
    new_node->next = head->next;
    new_node->prev = head;
    head->next->prev = new_node;
    head->next = new_node;
}

static inline void list_del(struct list_head *entry) {
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
}

#define list_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - (size_t)(&((type *)0)->member)))

#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); \
         pos = n, n = pos->next)

/* ========== 简化版自旋锁 (需 ZratroOS 提供原子操作) ========== */
typedef struct {
    volatile int locked;
} spinlock_t;

#define SPINLOCK_UNLOCK 0
#define SPINLOCK_LOCKED 1

static inline void spin_lock_init(spinlock_t *lock) {
    lock->locked = SPINLOCK_UNLOCK;
}

static inline void spin_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(&lock->locked, SPINLOCK_LOCKED)) {
        while (lock->locked) { /* busy wait */ }
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    __sync_lock_release(&lock->locked);
}

/* ========== 数据类型兼容 ========== */
typedef unsigned long       u_long;
typedef unsigned int        u_int;
typedef unsigned short      u_short;
typedef unsigned char       u_char;

typedef signed char         s8;
typedef unsigned char       u8;
typedef signed short        s16;
typedef unsigned short      u16;
typedef signed int          s32;
typedef unsigned int        u32;
typedef signed long long    s64;
typedef unsigned long long  u64;

/* ========== 常用宏 ========== */
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#define min(x, y)       ({ typeof(x) _x = (x); typeof(y) _y = (y); \
                           (void) (&_x == &_y); _x < _y ? _x : _y; })
#define max(x, y)       ({ typeof(x) _x = (x); typeof(y) _y = (y); \
                           (void) (&_x == &_y); _x > _y ? _x : _y; })
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ALIGN(x, a)     (((x) + (a) - 1) & ~((a) - 1))

/* ========== BUG_ON/WARN_ON 映射 ========== */
#define BUG_ON(cond)    do { if (cond) { \
                               zts_api->log_print("BUG", "BUG at %s:%d", __FILE__, __LINE__); \
                           } } while (0)
#define WARN_ON(cond)   ({ int _cond = (cond); \
                           if (_cond) \
                               zts_api->log_print("WARN", "WARN at %s:%d", __FILE__, __LINE__); \
                           _cond; })

/* ========== ZTS 模块入口宏 ========== */
#define ZTS_MODULE_INIT(fn) \
    int zts_module_init(struct zts_api_table *api) { \
        zts_api = api; \
        return fn(); \
    }

#define ZTS_MODULE_EXEC(fn) \
    int zts_module_execute(void *input, void *output, void *ctx) { \
        return fn(input, output, ctx); \
    }

#define ZTS_MODULE_CLEANUP(fn) \
    void zts_module_cleanup(void) { \
        fn(); \
    }

#endif /* ZTS_ADAPT_H */
```

### 3.3 ZTS 模块模板

```c
/*
 * ZTS 模块模板: Linux 算法 → ZratroOS
 * 编译: gcc -nostdlib -fPIC -shared -o tcp_cubic.zts tcp_cubic_zts.c
 */

#include "zts_adapt.h"

/* ========== 模块头部 (自动填充) ========== */
/* 编译时由构建脚本填充魔数、版本、UUID、校验和 */

/* ========== 从 Linux 提取的核心算法 ========== */

/* 示例: Linux TCP CUBIC 核心函数 */
static u32 bictcp_recalc_ssthresh(struct tcp_sock *tp) {
    /* ... Linux 原始逻辑 ... */
    return 0; /* 简化示例 */
}

static void bictcp_cong_avoid(struct tcp_sock *tp, u32 ack, u32 acked) {
    /* ... Linux 原始逻辑 ... */
}

/* ========== ZTS 接口 ========== */

static int tcp_cubic_init(void) {
    printk("[ZTS:TCP_CUBIC] TCP CUBIC congestion control loaded");
    /* 注册到 ZratroOS TCP 子系统 */
    /* tcp_register_congestion_control("cubic", &cubic_ops); */
    return 0;
}

static int tcp_cubic_execute(void *input, void *output, void *ctx) {
    /* 可选: 运行时参数调整 */
    (void)input;
    (void)output;
    (void)ctx;
    return 0;
}

static void tcp_cubic_cleanup(void) {
    printk("[ZTS:TCP_CUBIC] TCP CUBIC unloaded");
    /* tcp_unregister_congestion_control("cubic"); */
}

/* ========== 模块声明 ========== */
ZTS_MODULE_INIT(tcp_cubic_init)
ZTS_MODULE_EXEC(tcp_cubic_execute)
ZTS_MODULE_CLEANUP(tcp_cubic_cleanup)
```

---

## 4. 编译工具链

### 4.1 构建脚本 (build_zts.sh)

```bash
#!/bin/bash
# build_zts.sh - 编译 Linux 算法为 ZTS 模块

set -e

LINUX_SRC="${LINUX_SRC:-/path/to/linux}"
ZTS_OUT="${ZTS_OUT:-SYSTEM/zrr64/zts}"
ADAPT_H="include/zts_adapt.h"

# 颜色输出
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[ZTS-BUILD]${NC} $1"; }
warn()  { echo -e "${YELLOW}[ZTS-BUILD]${NC} $1"; }

# 检查依赖
check_deps() {
    command -v gcc >/dev/null 2>&1 || { echo "gcc required"; exit 1; }
    [ -f "$ADAPT_H" ] || { echo "zts_adapt.h not found"; exit 1; }
}

# 编译单个 ZTS 模块
compile_zts() {
    local name="$1"
    local src_files="$2"
    local priority="${3:-0x00000000}"
    local category="${4:-CUSTOM}"

    info "Compiling $name..."

    local out_file="${ZTS_OUT}/${priority}.${name}.zts"

    gcc -nostdlib -fPIC -shared \
        -O2 -Wall \
        -I include \
        -I "${LINUX_SRC}/include" \
        -o "$out_file" \
        ${src_files}

    # 填充 ZTS 头部 (需要专用工具)
    zts_header_tool \
        --name "$name" \
        --priority "$priority" \
        --category "$category" \
        --version "1.0" \
        --file "$out_file"

    info "Output: $out_file"
}

# ========== 预定义模块 ==========

compile_tcp_cubic() {
    compile_zts "tcp_cubic" \
        "modules/tcp/tcp_cubic_zts.c" \
        "0x00000001" \
        "NETWORK"
}

compile_virtio_blk() {
    compile_zts "virtio_blk" \
        "modules/drivers/virtio_blk_zts.c" \
        "0x00000000" \
        "DRIVER"
}

compile_fat32_path() {
    compile_zts "fat32_path" \
        "modules/fs/fat32_path_zts.c" \
        "0x00000002" \
        "FILESYSTEM"
}

# ========== 主流程 ==========

main() {
    check_deps
    mkdir -p "$ZTS_OUT"

    case "${1:-all}" in
        tcp_cubic)    compile_tcp_cubic ;;
        virtio_blk)   compile_virtio_blk ;;
        fat32_path)   compile_fat32_path ;;
        all)
            compile_tcp_cubic
            compile_virtio_blk
            compile_fat32_path
            ;;
        *)
            echo "Usage: $0 {tcp_cubic|virtio_blk|fat32_path|all}"
            exit 1
            ;;
    esac

    info "Done!"
}

main "$@"
```

### 4.2 ZTS 头部填充工具 (zts_header_tool.c)

```c
/*
 * zts_header_tool.c - 为编译后的模块填充 ZTS 头部
 * 用法: zts_header_tool --name tcp_cubic --priority 0x00000001 --file module.zts
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ZTS_MAGIC 0x5A545337
#define ZTS_MAX_NAME 32
#define ZTS_UUID_LEN 16

struct zts_header {
    uint32_t magic;
    uint32_t version;
    uint32_t api_min_version;
    char name[ZTS_MAX_NAME];
    uint8_t uuid[ZTS_UUID_LEN];
    uint32_t category;
    uint32_t major_version;
    uint32_t minor_version;
    uint64_t init_func_offset;
    uint64_t execute_func_offset;
    uint64_t cleanup_func_offset;
    uint32_t checksum;
};

/* 计算校验和 */
static uint32_t calc_checksum(void *data, size_t size) {
    uint32_t sum = 0;
    uint8_t *p = data;
    for (size_t i = 0; i < size; i++) {
        sum += p[i];
    }
    return sum;
}

int main(int argc, char *argv[]) {
    char *name = NULL;
    char *file = NULL;
    uint32_t priority = 0;
    uint32_t category = 0; /* CUSTOM */

    /* 解析参数... */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            file = argv[++i];
        } else if (strcmp(argv[i], "--priority") == 0 && i + 1 < argc) {
            priority = strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--category") == 0 && i + 1 < argc) {
            category = strtoul(argv[++i], NULL, 0);
        }
    }

    if (!name || !file) {
        fprintf(stderr, "Usage: %s --name <name> --file <file> [--priority <p>] [--category <c>]\n", argv[0]);
        return 1;
    }

    /* 读取文件 */
    FILE *f = fopen(file, "r+b");
    if (!f) {
        perror("fopen");
        return 1;
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc(size);
    fread(data, 1, size, f);

    /* 构建头部 */
    struct zts_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = ZTS_MAGIC;
    hdr.version = 1;
    hdr.api_min_version = 1;
    strncpy(hdr.name, name, ZTS_MAX_NAME - 1);
    hdr.category = category;
    hdr.major_version = 1;
    hdr.minor_version = 0;

    /* 查找符号 (简化: 假设固定偏移) */
    hdr.init_func_offset = 0x1000;      /* 实际需解析 ELF 符号表 */
    hdr.execute_func_offset = 0x1100;
    hdr.cleanup_func_offset = 0x1200;

    hdr.checksum = calc_checksum(&hdr, sizeof(hdr));

    /* 写入头部到文件开头 */
    fseek(f, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, f);

    fclose(f);
    free(data);

    printf("[ZTS-HEADER] Module '%s' header written to %s\n", name, file);
    return 0;
}
```

---

## 5. 实施步骤

### 5.1 Phase 1: 基础设施 (1-2 周)

```bash
# 1. 克隆 Linux 内核
git clone --depth 1 https://github.com/torvalds/linux.git linux-src

# 2. 创建 ZTS 项目结构
mkdir -p SYSTEM/zrr64/zts/{modules,include,tools}

# 3. 创建适配层头文件
cp zts_adapt.h SYSTEM/zrr64/zts/include/

# 4. 编译头部工具
gcc -o SYSTEM/zrr64/zts/tools/zts_header_tool \
    SYSTEM/zrr64/zts/tools/zts_header_tool.c

# 5. 创建构建脚本
cp build_zts.sh SYSTEM/zrr64/zts/
```

### 5.2 Phase 2: TCP CUBIC 替换 (示例)

```bash
# 1. 提取 Linux TCP CUBIC 代码
cp linux-src/net/ipv4/tcp_cubic.c \
   SYSTEM/zrr64/zts/modules/tcp/tcp_cubic_zts.c

# 2. 修改源代码，添加适配层
sed -i '1i #include "zts_adapt.h"' \
    SYSTEM/zrr64/zts/modules/tcp/tcp_cubic_zts.c

# 3. 替换 Linux 特定 API
#   - 移除 #include <linux/xxx>
#   - 添加 ZTS_MODULE_INIT/EXEC/CLEANUP 宏

# 4. 编译
cd SYSTEM/zrr64/zts
./build_zts.sh tcp_cubic

# 5. 测试加载 (在 ZratroOS 中)
# zts_register_module(tcp_cubic_data, size, 1);
```

### 5.3 Phase 3: 运行时集成

在 ZratroOS 内核中添加 ZTS 模块自动加载逻辑：

```c
/* main.c 或初始化代码中 */
void load_zts_modules(void) {
    /* 从文件系统加载 ZTS 模块 */
    struct vfs_dir_entry *entries;
    int count = vfs_readdir("/system/zts", &entries);

    for (int i = 0; i < count; i++) {
        if (strstr(entries[i].name, ".zts")) {
            void *data;
            size_t size;
            data = vfs_read_file(entries[i].name, &size);
            if (data) {
                int priority = parse_priority(entries[i].name);
                zts_register_module(data, size, priority);
            }
        }
    }
}
```

---

## 6. 风险与缓解

### 6.1 风险矩阵

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|---------|
| Linux 代码依赖过多内核 API | 高 | 高 | 选择低依赖算法 (如 CUBIC) |
| 编译后模块过大 | 中 | 中 | 裁剪不必要代码 |
| 运行时兼容性问题 | 中 | 高 | 充分测试 + 检查点回滚 |
| 许可证冲突 (GPL) | 高 | 中 | ZratroOS 为内部使用 |
| 性能不如预期 | 低 | 低 | 可回滚到原始实现 |

### 6.2 许可证说明

Linux 内核使用 GPL v2 许可证。由于 ZratroOS 标注为"内部使用"，在个人/学习项目中可以：
- 用于学习目的
- 不对外分发
- 如果未来考虑开源，需采用兼容许可证

---

## 7. 预期收益

| 模块 | 替换前 | 替换后 | 收益 |
|------|--------|--------|------|
| TCP 拥塞控制 | Reno (慢) | CUBIC/BBR (快 3-10x) | 网络性能 |
| virtio-blk | 超时不可用 | 正常工作 | QEMU 测试 |
| FAT32 mkdir | 嵌套目录失败 | 完整支持 | 文件系统 |
| CFS 调度 | 基础 vruntime | PELT/组调度 | 调度公平性 |

---

## 8. 参考资源

- Linux 源码: https://github.com/torvalds/linux
- ZTS7 文档: `RE/DEVELOPMENT_GUIDE.md` §11
- 热更新文档: `RE/DEVELOPMENT_GUIDE.md` §13
- 内核实现细节: `RE/KERNEL_IMPLEMENTATION_DETAILS.md` 附录 G/H

---

*文档版本: 1.0*  
*创建日期: 2026-04-21*  
*状态: 待实施*
