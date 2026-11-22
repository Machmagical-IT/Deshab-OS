# ZratroOS 内核实现细节分析

> 本文档深入分析 ZratroOS 内核的核心数据结构、关键算法、函数逻辑、同步机制和性能优化。

---

## 目录

1. [物理内存管理 (PMM)](#1-物理内存管理-pmm)
2. [虚拟内存管理 (VMM)](#2-虚拟内存管理-vmm)
3. [堆分配器 (Heap)](#3-堆分配器-heap)
4. [进程/线程管理 (Task)](#4-进程线程管理-task)
5. [完全公平调度器 (CFS)](#5-完全公平调度器-cfs)
6. [虚拟文件系统 (VFS)](#6-虚拟文件系统-vfs)
7. [系统调用 (Syscall)](#7-系统调用-syscall)
8. [中断处理 (IDT/ISR)](#8-中断处理-idtisr)
9. [APIC 与多核支持 (SMP)](#9-apic-与多核支持-smp)
10. [网络协议栈](#10-网络协议栈)
11. [协程系统](#11-协程系统)
12. [写时复制 (COW)](#12-写时复制-cow)
13. [Slab 分配器](#13-slab-分配器)
14. [安全能力模型 (Capability)](#14-安全能力模型-capability)
15. [自愈框架 (Self-Heal)](#15-自愈框架-self-heal)

---

## 1. 物理内存管理 (PMM)

**源文件**: `SYSTEM/lib/Ztknl/pmm.c` (1776行)

### 1.1 核心数据结构

```c
/* Per-CPU 页面缓存 - 原创优化 */
struct pcpu_page_cache {
    uint64_t pages[32];    // 每CPU缓存32页(128KB)
    uint32_t count;        // 缓存中的页数
    uint32_t pad;          // 对齐到64位
};

/* 内存区域统计 */
struct zone_stats {
    uint64_t total_pages;   // 区域总页数
    uint64_t free_pages;    // 区域空闲页数
    uint64_t min_free;      // 水位线:最低(512KB)
    uint64_t low_free;      // 水位线:低(1MB)
    uint64_t high_free;     // 水位线:高(2MB)
};

/* KSM 页面去重 */
struct ksm_page {
    uint64_t phys_addr;     // 物理地址
    uint64_t content_hash;  // 内容哈希
    uint32_t ref_count;     // 引用计数
    struct ksm_page *next;  // 链表指针
};

/* 内存块热插拔 */
struct memory_block {
    uint64_t start_phys;    // 起始物理地址
    uint64_t size;          // 大小
    int online;             // 是否在线
    int removable;          // 是否可移除
    struct memory_block *next;
};
```

### 1.2 位图分配算法

**分配策略**:
1. **单页分配** - 使用 `last_alloc_hint` 优化搜索:
   ```c
   // 从上次分配位置开始,按64位块搜索
   for (uint64_t word = start >> 6; word < total_words; word++) {
       uint64_t bits = ~bitmap[word];  // 取反得到空闲位
       if (bits != 0) {
           // 使用 BSF 指令快速找到第一个空闲位
           asm volatile("bsf %1, %0" : "=r"(bit) : "r"(bits));
           return (word << 6) + bit;
       }
   }
   ```
2. **多页分配** - 查找连续空闲区域,先从前次位置开始,再回绕到起始处

**关键优化**:
- 跳过前 1MB (`MIN_ALLOC_PAGE = 256`),避免与保留区域冲突
- 使用 BSF (Bit Scan Forward) x86 指令,O(1) 查找首个空闲位
- 记录 `last_allocated`,减少搜索范围

### 1.3 Per-CPU 缓存机制

**分配快速路径**:
```c
// 尝试从 Per-CPU 缓存分配 (无锁)
do {
    count = cache->count;
    if (count <= 0) break;
} while (!__sync_bool_compare_and_swap(&cache->count, count, count - 1));

if (count > 0) {
    uint64_t phys = cache->pages[count - 1];
    fast_zero_page(phys_to_virt(phys));
    return phys;
}
```

**缓存填充** (慢路径):
1. 获取全局锁
2. 批量分配 32 页 (`PCPU_CACHE_SIZE`)
3. 填充缓存,返回 1 页
4. 更新内存统计 (`__sync_sub_and_fetch`)

**释放策略**:
- 缓存未满: 直接放入 `cache->pages[count++]`
- 缓存已满: 获取锁,批量释放 32+1 页到位图

### 1.4 高性能页面清零

使用 `rep stosq` 指令批量清零,每次 8 字节:
```c
static inline void fast_zero_page(void *page) {
    uint64_t *p = (uint64_t *)page;
    uint64_t count = 512;  // PAGE_SIZE / 8
    asm volatile (
        "xor %%rax, %%rax\n\t"
        "rep stosq"
        : "+D"(p), "+c"(count)
        : : "rax", "memory"
    );
}
```

### 1.5 内存去重 (KSM)

**哈希算法**:
```c
static uint64_t ksm_calc_hash(uint64_t virt_addr) {
    uint64_t hash = 0;
    uint64_t *data = (uint64_t *)virt_addr;
    for (int i = 0; i < PAGE_SIZE / 8; i++) {
        hash ^= data[i];
        hash = (hash << 5) | (hash >> 59);  // 循环移位
        hash *= 0x5BD1E995;  // MurmurHash 乘法常数
    }
    return hash;
}
```

**分桶策略**: 256 个哈希桶,每桶独立自旋锁,支持并发扫描

### 1.6 内存压力检测

四级压力阈值:
- `MEM_PRESSURE_NONE`: 空闲 ≥ 25%
- `MEM_PRESSURE_LOW`: 空闲 < 25%
- `MEM_PRESSURE_MEDIUM`: 空闲 < 15%
- `MEM_PRESSURE_CRITICAL`: 空闲 < 5%

### 1.7 碎片统计

```c
// 碎片率计算 (基点表示,10000 = 100%)
ratio = (max_contiguous * 10000ULL) / free_pages;
fragmentation_ratio = 10000 - ratio;
```

---

## 2. 虚拟内存管理 (VMM)

**源文件**: `SYSTEM/lib/Ztknl/vmm.c` (2349行)

### 2.1 页表结构

支持 4 级页表 (x86_64):
- **PML4** (Page Map Level 4) - 512 项,每项 512GB
- **PDPT** (Page Directory Pointer Table) - 512 项,每项 1GB
- **PD** (Page Directory) - 512 项,每项 2MB
- **PT** (Page Table) - 512 项,每项 4KB

**虚拟地址空间布局**:
```
0xFFFF8000_00000000 - 内核 Higher Half 起始
0xFFFFFFFF_FFFFFFFF - 内核空间 (512GB)
0x00000000_00000000 - 0x00007FFF_FFFFFFFF - 用户空间 (128TB)
```

### 2.2 智能映射策略

```c
int vmm_map_smart(uint64_t virt, uint64_t phys, uint64_t flags, uint64_t size) {
    // 自动选择最佳页大小
    if (size >= GB(1) && IS_ALIGNED(phys, GB(1))) {
        return vmm_map_1gb_page(virt, phys, flags);
    } else if (size >= MB(2) && IS_ALIGNED(phys, MB(2))) {
        return vmm_map_2mb_page(virt, phys, flags);
    } else {
        return vmm_map_page(virt, phys, flags);
    }
}
```

### 2.3 用户空间访问保护

使用异常表和 SMAP 指令:
```c
int copy_from_user(void *dst, const void *src, size_t len) {
    // 检查地址是否在用户空间 (< higher_half_offset)
    if ((uint64_t)src >= higher_half_offset) return -EFAULT;
    
    // 启用 SMAP 保护
    asm volatile("stac");  // 允许访问用户空间
    int ret = __copy_from_user(dst, src, len);
    asm volatile("clac");  // 恢复保护
    return ret;
}
```

### 2.4 大页支持

**2MB 页映射**:
1. 检查物理地址 2MB 对齐
2. 在 PD 级别设置 PSE (Page Size Extension) 位
3. 跳过 PT 创建,直接映射 2MB

**1GB 页映射**:
1. 检查物理地址 1GB 对齐
2. 在 PDPT 级别设置 PSE 位
3. 跳过 PD 和 PT 创建

---

## 3. 堆分配器 (Heap)

**源文件**: `SYSTEM/lib/Ztknl/heap.c` (3290行)

### 3.1 分离适配算法

16 个大小类:
```
32B, 64B, 128B, 256B, 512B, 1KB, 2KB, 4KB,
8KB, 16KB, 32KB, 64KB, 128KB, 256KB, 512KB, 1MB
```

**分配流程**:
1. 计算请求大小所属的大小类
2. 检查该大小类的空闲链表
3. 有空闲块: 直接返回
4. 无空闲块: 从更大的类拆分,或向系统请求新页

### 3.2 安全保护

**Canary 检测**:
```c
#define HEAP_CANARY_VALUE  0xDEADBEEFCAFEBABE

struct heap_block {
    uint64_t canary_start;  // 块前魔数
    size_t size;
    struct heap_block *next;
    // ... 用户数据 ...
    uint64_t canary_end;    // 块后魔数
};
```

**双重释放检测**: 检查块是否在空闲链表中

### 3.3 预分配池

启动时预分配固定大小的内存池,减少早期系统调用开销

---

## 4. 进程/线程管理 (Task)

**源文件**: `SYSTEM/lib/Ztknl/task.c` (2062行)

### 4.1 PID 哈希表

**哈希函数** (黄金比例乘法):
```c
#define PID_HASH_GOLDEN  2654435761U
#define PID_HASH_SIZE    256

uint32_t pid_hash(uint32_t pid) {
    return (pid * PID_HASH_GOLDEN) % PID_HASH_SIZE;
}
```

**O(1) 查找**: 通过 PID 直接计算哈希桶,遍历链表(通常长度 ≤ 2)

### 4.2 Fork 实现

**写时复制地址空间**:
```c
int fork(struct thread *parent) {
    // 1. 创建新线程结构
    struct thread *child = thread_create();
    
    // 2. 复制用户空间 (COW)
    for (pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        // 只复制用户空间 (0-255),不复制内核空间 (256-511)
        copy_pml4_entry(parent, child, pml4_idx);
    }
    
    // 3. 复制内核栈 (Per-CPU 缓存分配)
    child->kstack = alloc_kstack();
    
    // 4. 复制文件描述符表、信号处理等
    copy_files(parent, child);
    
    return child->tid;
}
```

### 4.3 线程状态机

```
CREATED → READY → RUNNING → (WAITING/SUSPENDED) → TERMINATED
                   ↑              ↓
                   └──── YIELD ←─┘
```

---

## 5. 完全公平调度器 (CFS)

**源文件**: `SYSTEM/lib/Ztknl/cfs.c` (1357行)

### 5.1 红黑树实现

**节点结构**:
```c
struct rb_node {
    struct rb_node *parent;
    struct rb_node *left;
    struct rb_node *right;
    uint64_t key;          // vruntime
    int color;             // RB_RED / RB_BLACK
};

struct rb_root {
    struct rb_node *rb_node;      // 根节点
    struct rb_node *rb_leftmost;  // 最左节点缓存 (O(1) 查找最小)
    uint64_t nr_nodes;
};
```

**旋转操作**:
- **左旋**: 节点 A 的右子 B 提升,A 成为 B 的左子
- **右旋**: 节点 A 的左子 B 提升,A 成为 B 的右子

**插入修复** (标准红黑树算法):
1. 新节点涂红
2. 如果父节点也是红色:
   - Case 1: 叔叔红色 → 父叔涂黑,祖父涂红,继续上溯
   - Case 2: 叔叔黑色,节点是右子 → 左旋父节点
   - Case 3: 叔叔黑色,节点是左子 → 右旋祖父,父涂黑,祖父涂红

### 5.2 虚拟时间计算

```c
// 权重与 nice 值对应表
static const uint64_t sched_weights[] = {
    /* -20 */ 88761, /* -19 */ 71755, /* -18 */ 56483,
    /* -10 */ 3350,  /*  -5 */ 2048,   /*  0 */ 1024,  // NICE_0_LOAD
    /*  10 */ 335,   /*  15 */ 208,    /*  19 */ 127
};

// 虚拟时间增量
delta_vruntime = delta_exec * NICE_0_LOAD / weight;

// 时间片计算
slice = sched_latency * weight / total_weight;
```

**关键参数**:
- `CFS_SCHED_LATENCY = 24ms` - 目标调度延迟
- `CFS_MIN_GRANULARITY = 3ms` - 最小时间粒度
- `CFS_MIN_SLICE = 0.75ms` - 最小时间片
- `CFS_MAX_SLICE = 6ms` - 最大时间片
- `CFS_WAKEUP_GRANULARITY = 1.5ms` - 唤醒抢占粒度

### 5.3 负载均衡

**Per-CPU 运行队列**:
```c
struct cfs_rq cfs_rqs[MAX_CPUS];  // 每 CPU 一个红黑树
```

**迁移策略**:
1. 检查源队列 `nr_running > dst->nr_running + 1`
2. 使用 `double_spin_lock` 安全获取两把锁
3. 从源队列取出最左节点 (vruntime 最小)
4. 调整 vruntime: `if (se->vruntime < dst->min_vruntime) se->vruntime = dst->min_vruntime`
5. 插入目标队列红黑树

### 5.4 NUMA 感知调度

**拓扑结构**:
```c
struct numa_node {
    int node_id;
    uint64_t cpu_mask;     // 该节点的 CPU 掩码
    uint64_t load;         // 当前负载
    uint64_t capacity;     // 容量
};
```

**迁移决策**: 跨 NUMA 节点迁移需要 20% 负载差 (`dst->load * 120 < src->load * 100`)

### 5.5 动态优先级调整

根据任务行为自动调整:
```c
struct dynamic_prio_data {
    uint64_t sleep_avg;    // 平均睡眠时间 (EMA)
    uint64_t run_avg;      // 平均运行时间 (EMA)
    int bonus;             // 优先级奖励 (-5 到 +5)
    int interactive;       // 是否交互式任务
};

// 交互式判断
if (sleep_avg > run_avg * 2) {
    bonus = +5;  // 交互式任务奖励
} else if (run_avg > sleep_avg * 4) {
    bonus = -5;  // CPU 密集型惩罚
}
```

### 5.6 带宽控制

```c
struct cfs_bandwidth {
    uint64_t quota;      // 配额 (纳秒)
    uint64_t period;     // 周期 (默认 100ms)
    uint64_t runtime;    // 剩余运行时间
};
```

节流机制: `runtime < delta` 时任务必须等待下一个周期

---

## 5.5 EEVDF 调度器 (Earliest Eligible Virtual Deadline First)

**源文件**: `SYSTEM/lib/Ztknl/scheduler.c` (EEVDF 部分)

> Linux 6.6+ 引入的下一代调度算法，替代 CFS 作为默认调度器。

### 5.5.1 核心思想

EEVDF 与 CFS 的关键区别：
- **CFS**: 选择 vruntime 最小的任务运行 (贪心公平)
- **EEVDF**: 选择最早**合格**的虚拟截止时间任务运行 (延迟敏感优化)

关键公式：
```
vdeadline = vruntime + slice * weight / total_weight
eligible  = vruntime <= avg_vruntime + slice
lag       = (avg_vruntime - vruntime) * weight
```

### 5.5.2 数据结构

```c
/* EEVDF 调度实体 */
struct eevdf_entity {
    struct rb_node run_node;        /* 红黑树节点 (按 vdeadline 排序) */
    struct thread *thread;          /* 所属线程 */

    uint64_t vruntime;              /* 虚拟运行时间 */
    uint64_t vdeadline;             /* 虚拟截止时间 */

    uint32_t weight;                /* 调度权重 */
    uint32_t wmult_idx;             /* 逆权重表索引 */
    int nice;                       /* nice 值 */

    int eligible;                   /* 是否合格可被调度 */
    int latency_sensitive;          /* 延迟敏感标志 (自适应) */
    uint64_t avg_runtime;           /* 平均运行时间 (EMA) */

    uint64_t slice;                 /* 当前时间片 */
    uint64_t sum_exec_runtime;      /* 累计执行时间 */
    uint64_t prev_sum_exec_runtime; /* 上次累计执行时间 */
    int on_rq;                      /* 是否在运行队列中 */
};

/* EEVDF 运行队列 (Per-CPU) */
struct eevdf_rq {
    struct rb_root timeline;        /* 主时间线 (按 vdeadline 排序) */
    struct rb_root eligible_tree;   /* 合格任务树 */
    struct eevdf_entity *curr;      /* 当前运行实体 */

    uint32_t nr_running;
    uint64_t load_weight;           /* 总负载权重 */
    uint64_t min_vruntime;          /* 最小虚拟时间 */
    uint64_t avg_vruntime;          /* 平均虚拟时间 */

    uint64_t clock;                 /* 队列时钟 */
    volatile int lock;
    int cpu;
};
```

### 5.5.3 核心算法

**选择下一个任务** (`eevdf_pick_next`):
1. 从红黑树最左节点开始遍历
2. 检查每个任务的合格性 (`eevdf_entity_eligible`)
3. 选择第一个合格且 vdeadline 最早的任务
4. 如果没有合格任务，选择 vruntime 最小的 (最欠 CPU 时间)

**合格性检查** (`eevdf_entity_eligible`):
```c
// lag = avg_vruntime - vruntime
// lag >= 0: 该实体欠 CPU 时间，应该被调度
// lag < 0 但 |lag| < slice: 仍在可接受范围内
int64_t lag = (int64_t)(rq->avg_vruntime - ee->vruntime);
return lag >= 0 || (uint64_t)(-lag) < ee->slice;
```

**延迟敏感自适应**:
```c
// 使用指数移动平均 (EMA) 跟踪任务运行时间
ee->avg_runtime = (ee->avg_runtime * 7 + delta) / 8;

// 短任务自动标记为延迟敏感
if (ee->avg_runtime < EEVDF_BASE_SLICE_NS && ee->avg_runtime > 0) {
    ee->latency_sensitive = 1;
}

// 延迟敏感任务获得更短时间片但更频繁调度
if (ee->latency_sensitive) {
    slice = slice * 3 / 4;  /* 缩短 25% */
}
```

**唤醒抢占** (`eevdf_check_preempt_wakeup`):
- 延迟敏感任务优先抢占非延迟敏感任务
- 唤醒任务截止时间更早则抢占
- 唤醒任务欠 CPU 时间 (lag > 0) 则抢占

### 5.5.4 配置参数

| 参数 | 值 | 说明 |
|------|-----|------|
| `EEVDF_BASE_SLICE_NS` | 3ms | 基础时间片 |
| `EEVDF_MIN_SLICE_NS` | 0.3ms | 最小时间片 |
| `EEVDF_MAX_SLICE_NS` | 100ms | 最大时间片 |
| `EEVDF_SCHED_PERIOD_NS` | 6ms | 调度周期 |
| `EEVDF_MIN_GRAN_NS` | 0.75ms | 最小调度粒度 |
| `EEVDF_WAKEUP_GRAN_NS` | 1ms | 唤醒抢占粒度 |

### 5.5.5 与 CFS 的对比

| 特性 | CFS | EEVDF |
|------|-----|-------|
| 排序键 | vruntime | vdeadline |
| 选择策略 | 最小 vruntime | 最早合格 vdeadline |
| 延迟优化 | 无 | 自适应延迟敏感 |
| 公平性 | 绝对公平 | 合格性约束下的公平 |
| 唤醒抢占 | 基于粒度 | 基于 lag + 截止时间 |
| 时间片 | 固定计算 | 延迟敏感自适应 |

---

## 6. 虚拟文件系统 (VFS)

**源文件**: `SYSTEM/lib/Ztknl/vfs.c` (3122行)

### 6.1 三层缓存架构

**Buffer Cache** (块级缓存):
```c
struct buffer_head {
    uint64_t block_no;        // 块号
    uint8_t data[BLOCK_SIZE]; // 块数据 (通常 512B 或 4KB)
    int dirty;                // 脏页标志
    int valid;                // 数据有效
    struct list_head lru;     // LRU 链表
};
```

**预读算法**:
```c
// 自适应预读大小: 4 → 8 → 16 → 32 (指数增长)
if (consecutive_reads > threshold) {
    readahead_size = min(readahead_size * 2, MAX_READAHEAD);
}
```

**Page Cache** (页级缓存):
- 8192 页 (32MB) 容量
- 8192 个哈希桶
- 支持文件偏移 → 页映射

**Dentry Cache** (目录项缓存):
- 加速路径名查找
- LRU 淘汰策略

### 6.2 LRU 淘汰

```c
// 找到最久未使用的 buffer
struct buffer_head *lru_find_victim(void) {
    return list_last_entry(&buffer_lru, struct buffer_head, lru);
}

// 脏页回写
if (bh->dirty) {
    block_write(bh->block_no, bh->data);
    bh->dirty = 0;
}
```

---

## 7. 系统调用 (Syscall)

**源文件**: `SYSTEM/lib/Ztknl/syscall.c` (1160行)

### 7.1 双 ABI 分发

```c
long syscall_handler(struct pt_regs *regs) {
    long nr = regs->rax;  // 系统调用号
    
    // 检查进程 ABI 类型
    if (current->abi == ABI_ZRATRO) {
        return zratro_syscall_dispatch(nr, regs);
    } else {
        return linux_syscall_dispatch(nr, regs);
    }
}
```

### 7.2 16 个子系统分类

| 范围 | 子系统 | 示例调用 |
|------|--------|----------|
| 0-19 | 进程管理 | fork, exec, exit, wait4 |
| 20-39 | 内存管理 | mmap, brk, mprotect, munmap |
| 40-59 | 文件 I/O | open, read, write, close, lseek |
| 60-79 | 目录/文件操作 | mkdir, unlink, rename, stat |
| 80-99 | 网络 | socket, bind, connect, send, recv |
| 100-119 | 线程 | clone, set_tid, gettid |
| 120-139 | IPC | pipe, dup, dup2, eventfd |
| 140-159 | 时间 | gettimeofday, clock_gettime, nanosleep |
| 160-179 | 系统信息 | uname, sysinfo, getcpu |
| 180-199 | 设备 | ioctl, ioperm, iopl |
| 200-219 | 能力 | cap_get, cap_set, cap_check |
| 220-239 | 信号 | sigaction, sigprocmask, kill |
| 240-259 | 协程 | co_create, co_yield, co_channel |
| 260-279 | 虚拟化 | vmx_enter, vmx_exit |
| 280-299 | Cgroup | cgroup_create, cgroup_set |
| 300-319 | Linux 兼容 | linux_mmap, linux_clone |

### 7.3 安全检查

1. **地址验证**: `access_ok(addr, size)` 检查用户空间范围
2. **安全拷贝**: `copy_from_user()` 使用异常表处理页错误
3. **SMAP**: `stac/clac` 指令防止内核意外访问用户空间

---

## 8. 中断处理 (IDT/ISR)

**源文件**: `SYSTEM/lib/Ztknl/idt.c`, `isr.c`

### 8.1 IDT 结构

256 个中断门:
```c
struct idt_entry {
    uint16_t offset_low;     // 处理函数低 16 位
    uint16_t selector;       // 代码段选择子
    uint8_t ist;             // 中断堆栈表
    uint8_t flags;           // 类型 (0x8E = 中断门)
    uint16_t offset_mid;     // 处理函数中 16 位
    uint32_t offset_high;    // 处理函数高 32 位
    uint32_t reserved;
};
```

### 8.2 异常处理分类

| 向量 | 异常 | 处理方式 |
|------|------|----------|
| 0 | #DE (除零错误) | 发送 SIGFPE,记录 CR2 |
| 13 | #GP (一般保护) | 打印错误码,杀死进程 |
| 14 | #PF (页错误) | 查错误码(U/S, W/R, I/D),处理 COW 或 OOM |
| 32-255 | IRQ | 发送到对应 APIC 处理 |

**页错误处理**:
```c
void handle_page_fault(uint64_t error_code, uint64_t fault_addr) {
    // 检查错误码
    if (error_code & (1 << 1)) {  // W/R 位
        // 写操作 → 检查 COW
        if (is_cow_page(fault_addr)) {
            handle_cow_fault(fault_addr);
            return;
        }
    }
    if (!(error_code & (1 << 0))) {  // P 位
        // 页不存在 → 可能是按需分配
        handle_demand_page(fault_addr);
        return;
    }
    // 其他情况 → 杀死进程
    force_sig(SIGSEGV);
}
```

---

## 9. APIC 与多核支持 (SMP)

**源文件**: `SYSTEM/lib/Ztknl/apic.c`, `smp.c` (1518行)

### 9.1 APIC 架构

**Local APIC** (每 CPU):
- 计时器中断 (LVT Timer)
- IPI 发送/接收
- 任务优先级 (TPR) 控制

**I/O APIC**:
- 路由外部 IRQ 到特定 CPU
- 支持 24 个中断引脚

### 9.2 AP 启动流程

1. **BSP 发送 INIT IPI** 到目标 AP
2. **等待 10ms**
3. **发送 2× STARTUP IPI** (SIPI),包含启动代码段
4. **AP 执行启动代码**:
   - 设置 GDT/IDT
   - 启用分页
   - 跳转到 `kernel_main`
   - 设置 `cpu_local.ready = 1`
5. **BSP 等待 5 秒超时** 检查 ready 标志

### 9.3 Per-CPU 数据结构

```c
struct cpu_local {
    int cpu_id;              // CPU ID
    int ready;               // 就绪标志
    struct thread *current;  // 当前线程
    struct runqueue *rq;     // 运行队列
    char stack[PER_CPU_STACK_SIZE];  // 内核栈
} __attribute__((aligned(4096)));  // 4KB 对齐避免缓存行共享
```

### 9.4 IPI (核间中断)

```c
void apic_send_ipi(int cpu, uint32_t vector, uint32_t flags) {
    // 设置 ICR (Interrupt Command Register)
    uint32_t icr_low = vector | flags | (cpu << 24);
    apic_write(APIC_ICR, icr_low);
}

// 常用 IPI 类型:
// - RESCHEDULE_VECTOR (0xF0): 通知目标 CPU 重新调度
// - TLB_FLUSH_VECTOR (0xF1): TLB 刷新
// - FUNCTION_CALL_VECTOR (0xF2): 远程函数调用
```

---

## 10. 网络协议栈

**源文件**: `SYSTEM/lib/Ztknl/net.c`, `tcp.c`, `socket.c`

### 10.1 TCP 状态机

```
LISTEN → SYN_RCVD → ESTABLISHED
  ↑        ↑           ↓
  └─ SYN ←┘        FIN_WAIT_1
                       ↓
                   FIN_WAIT_2 → TIME_WAIT → CLOSED
```

### 10.2 环形缓冲区

**64KB 发送/接收缓冲**:
```c
int tcp_send_ring_buffer(struct tcp_sock *tsk, const void *data, size_t len) {
    size_t head = tsk->send_head;
    size_t tail = tsk->send_tail;
    size_t avail = (tail - head - 1) % BUF_SIZE;
    
    // 处理回绕
    if (head + len > BUF_SIZE) {
        size_t first = BUF_SIZE - head;
        memcpy(buffer + head, data, first);
        memcpy(buffer, data + first, len - first);
    } else {
        memcpy(buffer + head, data, len);
    }
    
    tsk->send_head = (head + len) % BUF_SIZE;
}
```

### 10.3 ISN 生成

```c
// 使用 TSC 和计数器生成随机初始序列号
uint32_t tcp_generate_isn(void) {
    uint64_t tsc;
    rdtsc(tsc);
    return (tsc ^ isn_counter++) + boot_time_seconds * 250000;
}
```

### 10.4 批量 TX

减少系统调用开销,一次发送多个数据包:
```c
int net_batch_tx(struct sk_buff *pkts[], int count) {
    for (int i = 0; i < count; i++) {
        enqueue_to_nic(pkts[i]);
    }
    nic_kick_tx();  // 一次性通知网卡
    return count;
}
```

### 10.5 QoS 流量整形

**Token Bucket 算法**:
```c
struct token_bucket {
    uint64_t tokens;       // 当前令牌数
    uint64_t max_tokens;   // 桶容量
    uint64_t refill_rate;  // 每秒补充率
};

int tb_check(struct token_bucket *tb, uint64_t pkt_size) {
    // 补充令牌
    uint64_t now = get_time_ns();
    uint64_t elapsed = now - tb->last_refill;
    tb->tokens = min(tb->max_tokens, 
                     tb->tokens + elapsed * tb->refill_rate / 1e9);
    tb->last_refill = now;
    
    // 检查是否有足够令牌
    if (tb->tokens < pkt_size) {
        return -1;  // 限速
    }
    tb->tokens -= pkt_size;
    return 0;
}
```

---

## 11. 协程系统

**源文件**: `SYSTEM/lib/Ztknl/coroutine.c` (1049行)

### 11.1 协程上下文

保存 callee-saved 寄存器:
```c
struct co_context {
    uint64_t rsp;  // 栈指针
    uint64_t rbp;  // 基址指针
    uint64_t rip;  // 指令指针
    uint64_t rbx;  // 被调用者保存
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
};
```

### 11.2 上下文切换 (汇编)

```asm
; co_switch(from, to)
global co_switch
co_switch:
    ; 保存当前上下文
    mov [rdi + co_context.rbx], rbx
    mov [rdi + co_context.r12], r12
    mov [rdi + co_context.r13], r13
    mov [rdi + co_context.r14], r14
    mov [rdi + co_context.r15], r15
    mov [rdi + co_context.rsp], rsp
    mov [rdi + co_context.rbp], rbp
    
    ; 加载目标上下文
    mov rbx, [rsi + co_context.rbx]
    mov r12, [rsi + co_context.r12]
    mov r13, [rsi + co_context.r13]
    mov r14, [rsi + co_context.r14]
    mov r15, [rsi + co_context.r15]
    mov rsp, [rsi + co_context.rsp]
    mov rbp, [rsi + co_context.rbp]
    jmp [rsi + co_context.rip]
```

### 11.3 Go-Style Channel

**环形缓冲区实现**:
```c
struct co_channel {
    void **buffer;        // 环形缓冲
    uint32_t capacity;    // 容量
    uint32_t count;       // 当前消息数
    uint32_t head;        // 读指针
    uint32_t tail;        // 写指针
    struct list_head send_waiters;  // 发送等待者
    struct list_head recv_waiters;  // 接收等待者
};
```

**发送逻辑**:
1. 缓冲区满 → 当前协程加入 `send_waiters`,执行 `co_yield()`
2. 有空间 → 写入 `buffer[tail]`, `tail = (tail + 1) % capacity`
3. 唤醒 `recv_waiters` 中的一个

**无缓冲通道** (`capacity = 0`):
- 发送者必须等待接收者就绪
- 数据直接传递,不入队

### 11.4 工作窃取

```c
struct coroutine *co_steal(struct co_scheduler *target) {
    spin_lock(&target->pool.lock);
    // 从低优先级开始偷 (不影响高优先级任务)
    for (int i = CO_PRIORITY_MIN; i <= CO_PRIORITY_MAX; i++) {
        if (!list_empty(&target->pool.ready_queues[i])) {
            stolen = list_first_entry(...);
            list_del(&stolen->ready_list);
            break;
        }
    }
    spin_unlock(&target->pool.lock);
    return stolen;
}
```

### 11.5 同步原语

**协程互斥锁**:
```c
void co_mutex_lock(struct co_mutex *mutex) {
    while (mutex->locked) {
        current->state = CO_STATE_WAITING;
        list_add_tail(&current->wait_list, &mutex->waiters);
        co_yield();  // 让出,不阻塞线程
    }
    mutex->locked = 1;
}
```

**条件变量**: 支持 `wait/signal/broadcast`,与 mutex 配合使用

**信号量**: 计数信号量,支持 `P/V` 操作

---

## 12. 写时复制 (COW)

**源文件**: `SYSTEM/lib/Ztknl/cow.c`

### 12.1 哈希引用计数

```c
#define COW_HASH_SIZE  256

struct cow_entry {
    uint64_t phys_addr;    // 物理地址
    uint32_t ref_count;    // 引用计数
    spinlock_t lock;       // 每桶独立锁
    struct cow_entry *next;
};

struct cow_bucket {
    struct cow_entry *head;
    spinlock_t lock;
};

static struct cow_bucket cow_table[COW_HASH_SIZE];
```

**哈希函数**: `bucket = phys_addr % COW_HASH_SIZE`

### 12.2 COW 页错误处理

```c
void handle_cow_fault(uint64_t virt_addr) {
    uint64_t phys = get_phys_addr(virt_addr);
    
    // 检查引用计数
    if (cow_get_ref(phys) == 1) {
        // 唯一引用,直接设为可写
        set_page_writable(virt_addr);
        return;
    }
    
    // 分配新页
    uint64_t new_phys = pmm_alloc_page();
    memcpy(phys_to_virt(new_phys), phys_to_virt(phys), PAGE_SIZE);
    
    // 更新页表
    update_page_table(virt_addr, new_phys, WRITABLE);
    
    // 减少旧页引用
    cow_dec_ref(phys);
}
```

---

## 13. Slab 分配器

**源文件**: `SYSTEM/lib/Ztknl/slab.c`

### 13.1 对象缓存

为频繁分配的小对象提供专门缓存:
```c
struct kmem_cache {
    char name[32];          // 缓存名称
    size_t obj_size;        // 对象大小
    size_t align;           // 对齐要求
    struct list_head slabs_full;   // 满 slab 链表
    struct list_head slabs_partial; // 部分满 slab
    struct list_head slabs_empty;   // 空 slab 链表
    struct kmem_bufctl *bufctl;     // 空闲对象链表
};
```

### 13.2 Per-CPU 缓存

```c
struct kmem_cpu_cache {
    void **avail;           // 可用对象数组
    int avail_idx;          // 当前索引
    int batchcount;         // 批量大小
};
```

**无锁快速路径**:
- 分配: `cpu_cache->avail[--avail_idx]`
- 释放: `cpu_cache->avail[avail_idx++]`

**批量填充**:
当 Per-CPU 缓存空时,从 slab 批量获取 `batchcount` 个对象

### 13.3 缓存行着色

减少缓存冲突 miss:
```c
// 计算着色偏移
color_offset = (color * align) % PAGE_SIZE;
obj_addr = slab_base + color_offset + obj_index * obj_size;
```

---

## 14. 安全能力模型 (Capability)

**源文件**: `SYSTEM/lib/Ztknl/capability.c`

### 14.1 Capability 结构

```c
struct capability {
    uint64_t uid;           // 唯一标识
    uint32_t rights;        // 权限位掩码
    uint64_t expiry;        // 过期时间
    uint64_t signature[4];  // XOR-MAC 签名
    struct capability *parent_cap;  // 父能力 (委托链)
};
```

### 14.2 XOR-MAC 签名

8 轮混合:
```c
void compute_signature(struct capability *cap) {
    uint64_t hash[8];
    // 初始化: 混合 uid, rights, expiry
    hash[0] = cap->uid;
    hash[1] = cap->rights;
    hash[2] = cap->expiry;
    hash[3] = SECRET_KEY;
    
    // 8 轮混合
    for (int i = 0; i < 8; i++) {
        hash[i % 4] ^= hash[(i + 1) % 4] << (i * 3 + 5);
        hash[i % 4] += hash[(i + 2) % 4];
    }
    
    memcpy(cap->signature, hash, sizeof(cap->signature));
}
```

### 14.3 权限委托

**子集验证**:
```c
int cap_delegate(struct capability *parent, struct capability *child,
                 uint32_t requested_rights) {
    // 子能力权限必须是父能力的子集
    if ((requested_rights & parent->rights) != requested_rights) {
        return -EPERM;
    }
    child->parent_cap = parent;
    compute_signature(child);
    return 0;
}
```

---

## 15. 自愈框架 (Self-Heal)

**源文件**: `SYSTEM/lib/Ztknl/selfheal.c`

### 15.1 故障处理流水线

```
故障报告 → 查找处理器 → 执行恢复策略 → 记录结果
```

### 15.2 恢复策略

```c
enum recovery_action {
    RECOVERY_RETRY,      // 重试操作
    RECOVERY_RESTART,    // 重启服务
    RECOVERY_ISOLATE,    // 隔离故障组件
    RECOVERY_ROLLBACK,   // 回滚到检查点
    RECOVERY_REBOOT      // 重启系统
};
```

### 15.3 看门狗机制

**超时检测**:
```c
struct watchdog {
    uint64_t timeout_ns;     // 超时阈值
    uint64_t last_heartbeat; // 最后心跳时间
    void (*callback)(void);  // 超时回调
};

void watchdog_check(struct watchdog *wd) {
    if (get_time_ns() - wd->last_heartbeat > wd->timeout_ns) {
        wd->callback();  // 触发恢复
    }
}
```

---

## 附录 A: 关键常量定义

| 常量 | 值 | 说明 |
|------|-----|------|
| PAGE_SIZE | 4096 | 页大小 |
| MAX_CPUS | 8 | 最大 CPU 数 |
| MAX_THREADS | 1024 | 最大线程数 |
| NICE_0_LOAD | 1024 | Nice 0 的权重 |
| CFS_SCHED_LATENCY | 24000000 ns | CFS 目标延迟 |
| PCPU_CACHE_SIZE | 32 | Per-CPU 页面缓存大小 |
| KSM_HASH_BUCKETS | 256 | KSM 哈希桶数 |
| COW_HASH_SIZE | 256 | COW 引用计数哈希桶数 |
| CO_MAX_NAME_LEN | 32 | 协程名称最大长度 |
| CO_DEFAULT_STACK_SIZE | 65536 | 协程默认栈大小 (64KB) |

---

## 附录 B: 同步机制总结

| 机制 | 使用场景 | 性能特点 |
|------|----------|----------|
| 自旋锁 (spinlock) | 短临界区,中断上下文 | 低延迟,忙等 |
| 原子操作 (atomic) | 计数器,标志位 | 无锁,最快 |
| CAS (Compare-And-Swap) | Per-CPU 缓存 | 无锁快速路径 |
| Per-CPU 数据 | 统计,缓存 | 零竞争,最好 |
| 双锁获取 (double_spin_lock) | 负载均衡 | 避免死锁 |
| RCU (Read-Copy-Update) | 读多写少 | 读无锁,写延迟 |

---

## 附录 C: 性能优化技巧

1. **BSF 指令加速位图搜索**: 从 O(n) 到 O(n/64)
2. **rep stosq 批量清零**: 比循环快 2-3 倍
3. **Per-CPU 缓存减少锁竞争**: 分配快 10 倍
4. **红黑树 rb_leftmost 缓存**: O(1) 找最小 vruntime
5. **分离适配堆分配**: 减少碎片,提高缓存命中率
6. **批量 TX**: 减少系统调用开销
7. **缓存行着色**: 减少 L1 缓存冲突
8. **Higher Half 内核**: 用户/内核切换无需刷新 TLB

---

## 附录 D: 新增模块说明

### D.1 页错误处理 (page_fault.c)

**源文件**: `SYSTEM/lib/Ztknl/page_fault.c` (23KB)

**核心功能**:
- COW (写时复制) 页面处理
- 按需分页 (Demand Paging)
- 用户栈自动扩展
- 访问权限验证

**错误码解析**:
| 位 | 含义 |
|---|------|
| 0 (P) | 页是否存在 |
| 1 (W/R) | 写操作触发 |
| 2 (U/S) | 用户态触发 |
| 3 (RSVD) | 保留位错误 |
| 4 (I/D) | 指令获取 |

### D.2 栈保护 (stack_protection.c)

**源文件**: `SYSTEM/lib/Ztknl/stack_protection.c` (4KB)

**核心功能**:
- 栈溢出检测
- Canary 值插入
- 边界检查

### D.3 双内核通信 (inter_kernel.c)

**源文件**: `SYSTEM/lib/Ztknl/inter_kernel.c`

**核心功能**:
- 主内核 (Ztknl) 与辅助内核 (Azknl) 通信
- 共享内存接口
- 心跳检测
- 单内核模式支持（跳过初始化避免页错误）

### D.4 Windows NT 兼容层 (ntdll.c)

**源文件**: `SYSTEM/lib/Ztknl/ntdll.c` (4032行)

**核心功能**:
- Windows NT 系统调用模拟
- PE 文件加载支持
- Windows API 兼容

---

## 附录 E: 已知问题与待修复

| 问题 | 状态 | 说明 |
|------|------|------|
| virtio-blk 驱动超时 | 待修复 | VirtQueue 通知后设备未响应 |
| DHCP 发送失败 | 待修复 | `udp_sendto` 返回 -1 |
| 鼠标光标渲染后 FB 损坏 | 待修复 | 帧缓冲区数据损坏 |
| FAT32 mkdir 父目录查找失败 | 待修复 | 路径解析 bug |

---

*文档版本: 1.1*  
*分析日期: 2026-04-21*  
*内核版本: ZratroOS v1.6.0*  
*分析文件数: 99 个核心源文件*

---

## 附录 F: 自愈框架 (Self-Healing Framework)

**源文件**: `SYSTEM/lib/Ztknl/selfheal.c` (25KB, 897行)

### F.1 核心数据结构

```c
/* 故障上下文 */
struct sh_fault_context {
    sh_fault_type_t type;           // 故障类型
    sh_severity_t severity;         // 严重级别 (INFO/MINOR/MAJOR/CRITICAL/FATAL)
    const char *component;          // 组件名称
    const char *function;           // 函数名
    uint64_t address;               // 故障地址
    uint64_t error_code;            // 错误码
    uint32_t cpu_id;                // CPU ID
    char message[256];              // 故障描述
};

/* 故障处理器 */
struct sh_fault_handler {
    sh_fault_type_t type;
    sh_fault_handler_fn handler;    // 处理回调
    uint32_t priority;              // 优先级
    uint32_t flags;                 // 标志 (SH_HANDLER_FLAG_OVERRIDE)
    struct list_head list;          // 链表
};

/* 隔离域 */
struct sh_isolation_domain {
    uint32_t domain_id;
    char name[64];
    uint32_t flags;
    int is_isolated;                // 是否已隔离
    uint32_t fault_count;           // 故障计数
    struct list_head list;
};

/* 检查点 */
struct sh_checkpoint {
    uint64_t checkpoint_id;
    char name[64];
    void *state_snapshot;           // 状态快照
    size_t snapshot_size;
    int is_valid;
    uint32_t restore_count;         // 恢复次数
    struct list_head list;
};

/* 管理器 */
struct selfheal_manager {
    struct list_head handlers[SH_FAULT_MAX];
    struct list_head domains;
    struct list_head checkpoints;
    struct list_head watchdogs;
    struct list_head heartbeats;
    spinlock_t lock;
    uint32_t config_flags;          // 配置标志
    uint32_t max_recovery_attempts;
    uint64_t recovery_timeout;
    uint64_t total_faults;
    uint64_t total_recoveries;
    uint64_t successful_recoveries;
    uint64_t failed_recoveries;
    int recovery_in_progress;
};
```

### F.2 故障处理流水线

```
故障发生
    ↓
sh_report_fault() / sh_report_exception()
    ↓
sh_handle_fault()
    ├── 检查恢复状态 (防止嵌套恢复)
    ├── 遍历处理器链表 (按优先级)
    ├── 确定恢复策略
    └── sh_execute_recovery()
            ↓
        根据策略执行:
        ├── SH_RECOVER_RETRY → sh_retry_operation()
        ├── SH_RECOVER_RESTART → sh_restart_component()
        ├── SH_RECOVER_ISOLATE → sh_isolate_domain()
        ├── SH_RECOVER_ROLLBACK → sh_restore_checkpoint()
        └── SH_RECOVER_REBOOT → 系统重启
```

### F.3 预定义故障处理器

| 处理器 | 处理故障类型 | 策略 |
|--------|-------------|------|
| `sh_exception_handler` | 异常 | FATAL→REBOOT, CRITICAL→ROLLBACK, 其他→ISOLATE |
| `sh_memory_handler` | OOM/内存损坏/泄漏 | OOM→RETRY, 损坏→ISOLATE, 泄漏→NONE |
| `sh_deadlock_handler` | 死锁 | 有组件→ISOLATE, 无→ROLLBACK |
| `sh_driver_handler` | 驱动故障 | RESTART |

### F.4 看门狗实现

```c
struct sh_watchdog {
    uint32_t id;
    const char *name;
    uint64_t timeout;         // 超时阈值 (纳秒)
    uint64_t last_feed;       // 最后喂狗时间
    void (*on_timeout)(struct sh_watchdog *);  // 超时回调
    int is_running;
    int triggered;
    struct list_head list;
};
```

**检测逻辑**:
```c
void sh_check_watchdogs(struct selfheal_manager *shm) {
    uint64_t now = get_time();  // TODO: 时间获取
    for_each_watchdog(wd) {
        if (wd->is_running && !wd->triggered) {
            if (now - wd->last_feed > wd->timeout) {
                wd->triggered = 1;
                wd->on_timeout(wd);  // 调用超时回调
                sh_report_fault_simple(shm, SH_FAULT_HANG, ...);
            }
        }
    }
}
```

### F.5 与 Hotupdate 集成

自愈框架通过 `sh_restart_component()` 调用 hotupdate 模块的模块重新加载功能：

```c
int sh_restart_component(struct selfheal_manager *shm, const char *component) {
    struct module_info *mod = hu_module_get(component);
    if (mod != NULL && mod->on_unload != NULL && mod->on_load != NULL) {
        mod->on_unload();
        return mod->on_load();
    }
    return -1;
}
```

---

## 附录 G: 内核热更新 (Hotupdate)

**源文件**: `SYSTEM/lib/Ztknl/hotupdate.c` (33KB, 1171行)

### G.1 模块管理

```c
struct module_info {
    char name[32];
    char version[16];
    void *base;                 // 模块基址
    size_t size;                // 模块大小
    enum module_state state;    // LOADED/ACTIVE/UPDATING/ROLLBACK
    int (*on_load)(void);       // 加载回调
    void (*on_unload)(void);    // 卸载回调
    int (*on_update)(void *);   // 更新回调
    struct module_info *next;   // 链表
};
```

**状态机**:
```
LOADED → ACTIVE → UPDATING → ACTIVE
                     ↓
                  ROLLBACK → ACTIVE
```

### G.2 检查点实现

```c
struct update_checkpoint {
    uint64_t id;
    uint64_t timestamp;
    char description[64];
    void *module_snapshot;      // 模块快照
    size_t snapshot_size;
    void *state_backup;         // 状态备份
    size_t state_size;
};
```

**恢复流程**:
1. 遍历检查点链表查找目标 ID
2. 恢复模块快照 (遍历模块，恢复状态/基址/大小)
3. 恢复状态备份 (解析地址头，复制数据)
4. 重新注册缺失的模块

### G.3 差分更新系统

**差分包格式**:
```c
struct diff_header {
    uint32_t magic;             // 'DIFF' (0x46464944)
    uint32_t version;
    uint64_t old_checksum;      // FNV-1a 哈希
    uint64_t new_checksum;
    uint32_t block_count;
    uint32_t flags;
};

struct diff_block {
    uint64_t offset;
    uint32_t size;
    uint32_t type;              // 0=保留, 1=修改, 2=插入, 3=删除
    uint8_t data[4096];
};
```

**校验和算法** (FNV-1a):
```c
uint64_t compute_checksum(const void *data, size_t size) {
    uint64_t hash = 14695981039346656037ULL;
    const uint8_t *ptr = data;
    for (size_t i = 0; i < size; i++) {
        hash ^= ptr[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}
```

### G.4 事务管理

```c
struct update_transaction {
    uint64_t id;
    enum tx_state state;        // IDLE/PREPARING/PREPARED/COMMITTING/COMMITTED/ABORTED
    struct tx_operation *ops;   // 操作链表
    int op_count;
    uint64_t checkpoint_id;     // 关联检查点
};

struct tx_operation {
    int type;                   // 0=写内存, 1=写文件, 2=加载模块
    void *target;
    void *old_value;            // 旧值 (用于回滚)
    void *new_value;            // 新值
    size_t size;
    struct tx_operation *next;
};
```

**事务流程**:
```
tx_begin() → tx_add_op() → tx_prepare() → tx_commit() / tx_rollback()
```

### G.5 更新状态机

```
IDLE → DOWNLOADING → VERIFYING → PREPARING → APPLYING → COMPLETED
                                      ↓           ↓
                                   FAILED ← ROLLING_BACK
```

**状态转换验证**: 每个状态只允许特定的转换，防止非法状态跃迁。

### G.6 更新通道

```c
enum update_channel {
    CHANNEL_STABLE,    // 稳定版
    CHANNEL_BETA,      // 测试版
    CHANNEL_DEV,       // 开发版
    CHANNEL_NIGHTLY    // 每夜构建
};
```

**通道限制**: 只能升级到相同或更稳定的通道 (`channel <= current_channel`)

---

## 附录 H: ZTS7 动态算法系统

**源文件**: `SYSTEM/lib/Ztknl/zts_algorithm.c` (15KB, 492行)

### H.1 模块头部

```c
struct zts_header {
    uint32_t magic;              // ZTS_MAGIC (0x5A545337)
    uint32_t version;
    uint32_t api_min_version;
    char name[32];
    uint8_t uuid[16];
    uint32_t category;           // FILESYSTEM/NETWORK/CRYPTO/SCHEDULER/DRIVER/PATCH/INJECT
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t priority;
    size_t init_func_offset;
    size_t execute_func_offset;
    size_t cleanup_func_offset;
    uint32_t checksum;
};
```

### H.2 API 表实现

```c
static void *zts_api_alloc(size_t size) { return kzalloc(size); }
static void zts_api_free(void *ptr) { kfree(ptr); }
static void *zts_api_realloc(void *ptr, size_t size) { return krealloc(ptr, size); }
static void zts_api_log(const char *level, const char *fmt, ...) { /* printk */ }
static uint64_t zts_api_time(void) { return rdtsc(); }
static int zts_api_strcmp(const char *s1, const char *s2) { return strcmp(s1, s2); }
static void *zts_api_memcpy(void *dst, const void *src, size_t n) { return memcpy(dst, src, n); }
static void *zts_api_memset(void *s, int c, size_t n) { return memset(s, c, n); }
```

### H.3 模块加载流程

```
zts_register_module(binary, size, priority)
    ↓
zts_validate_header(hdr)
    ├── 魔数验证 (ZTS_MAGIC)
    ├── 版本检查 (api_min_version <= ZTS_API_VERSION)
    └── 校验和验证
    ↓
解析入口点 (init/execute/cleanup offsets)
    ↓
存储到 zts_sys.modules[]
```

### H.4 优先级解析

从文件名 `0xPPPPPPPP.zts` 解析优先级：
```c
static int zts_parse_priority(const char *filename) {
    // "0x00000000" → 0 (最高优先级)
    // "0xFFFFFFFF" → 4294967295 (最低优先级)
    if (filename[0] != '0' || filename[1] != 'x') return 0;
    uint64_t val = 0;
    // 解析十六进制...
    return (int)val;
}
```

### H.5 Patch 模式

```c
void zts_restore_patch(struct zts_module *mod) {
    if (!mod || !mod->patch_address || !mod->patch_original) return;
    memcpy(mod->patch_address, mod->patch_original, mod->patch_size);
    kfree(mod->patch_original);
    mod->patch_original = NULL;
}
```

---

## 附录 I: 双内核管理器 (Dual Kernel)

**源文件**: `SYSTEM/lib/Ztknl/vmm/dual_kernel.c` (19KB, 641行)

### I.1 架构概述

```
┌────────────────────────────────────────────┐
│                主内核 (Ztknl)                │
│  ┌──────────────────────────────────────┐   │
│  │         VMX/EPT 虚拟化层              │   │
│  │  ┌────────────────────────────────┐  │   │
│  │  │      副内核 (Guest)             │  │   │
│  │  │  (Azknl 或 RustKnl)            │  │   │
│  │  │  GPA: 0x04200000               │  │   │
│  │  └────────────────────────────────┘  │   │
│  └──────────────────────────────────────┘   │
│                    ↕ EPT                     │
│  ┌──────────────────────────────────────┐   │
│  │         共享内存                      │   │
│  │  (INTER_KERNEL_SHM_PHYS)             │   │
│  └──────────────────────────────────────┘   │
└────────────────────────────────────────────┘
```

### I.2 VMX 配置

**CPU-based Controls**:
- HLT/INVLPG/MWAIT/RDTSC exiting
- CR8 load/store exiting
- 无条件 I/O exiting
- MOV DR exiting
- MSR bitmap 启用

**Secondary Controls**:
- EPT 启用
- VPID 启用
- Unrestricted guest 模式

**异常位图**: 捕获所有异常 (`0xFFFFFFFF`)

### I.3 内存映射

**副内核 GPA 基址**: `0x04200000` (66MB)  
**最大副内核大小**: `256MB`

**EPT 映射**:
```c
for (size_t i = 0; i < pages; i++) {
    uint64_t gpa = SECONDARY_KERNEL_GPA_BASE + i * 4096;
    uint64_t hpa = image_hpa + i * 4096;
    ept_map_page(gpa, hpa, EPT_READ | EPT_WRITE | EPT_EXECUTE);
}
```

### I.4 VM-Exit 处理

| Exit Reason | 处理方式 |
|-------------|----------|
| EXCEPTION_NMI (1) | 记录异常信息 |
| HLT (6) | 跳过指令 |
| EPT Violation (49) | 尝试 1:1 映射 |
| I/O Instruction (50) | 跳过 I/O |
| 其他 | 记录并跳过 |

### I.5 共享内存

```c
struct inter_kernel_shm {
    struct inter_kernel_header header;
    struct inter_kernel_queue ztknl_to_azknl;
    struct inter_kernel_queue azknl_to_ztknl;
    uint8_t data[INTER_KERNEL_SHM_SIZE];
};
```

**消息传递**:
```c
dual_kernel_send_msg(type, data, count);
dual_kernel_recv_msg(&msg);
```

---

## 16. ZTS7 动态算法系统

**源文件**: `SYSTEM/lib/Ztknl/zts_algorithm.c` (15KB)  
**适配层**: `SYSTEM/lib/lints/include/zts_adapt.h`  
**构建工具**: `SYSTEM/lib/lints/build_zts.sh`  
**头注入工具**: `SYSTEM/lib/lints/tools/zts_header_tool.c`

### 16.1 ZTS 模块格式

```c
struct zts_header {
    uint32_t magic;              // 魔数 0x5A545337 ("ZTS7")
    uint32_t version;            // 版本 0x010000
    uint32_t api_min_version;    // 最低 API 版本
    char name[ZTS_MAX_NAME];     // 模块名称
    uint8_t uuid[ZTS_UUID_LEN];  // UUID
    uint32_t category;           // 类别
    uint32_t major_version;
    uint32_t minor_version;
    size_t init_func_offset;     // init 入口偏移
    size_t execute_func_offset;  // exec 入口偏移
    size_t cleanup_func_offset;  // cleanup 入口偏移
    uint32_t checksum;           // 校验和
};
```

**模块加载路径**: `/zrr64/zts/p.zts6/`

**优先级命名**: `0xPPPPPPPP.zts`（纯十六进制，不带名称）

### 16.2 ZTS 适配层实现

**API 表结构**:
```c
struct zts_api_table {
    uint32_t api_version;
    void *(*alloc)(size_t size);
    void (*free)(void *ptr);
    void *(*realloc)(void *ptr, size_t size);
    void (*log_print)(const char *level, const char *fmt, ...);
    uint64_t (*time_now)(void);
    int (*vfs_open)(const char *path, uint32_t mode);
    int (*vfs_close)(int fd);
    int (*vfs_read)(int fd, void *buf, size_t count);
    int (*vfs_write)(int fd, const void *buf, size_t count);
    int (*net_send)(const void *data, size_t len);
    int (*net_recv)(void *data, size_t len);
    int (*strcmp)(const char *s1, const char *s2);
    void *(*__memcpy)(void *dst, const void *src, size_t n);
    void *(*__memset)(void *s, int c, size_t n);
};
```

**宏映射示例**:
```c
#define kmalloc(size)           zts_api->alloc(size)
#define kzalloc(size)           ({ void *p = zts_api->alloc(size); \
                                   __zts_memset(p, 0, size); p; })
#define kfree(ptr)              zts_api->free(ptr)
#define printk(fmt, ...)        zts_api->log_print("KERN", fmt, ##__VA_ARGS__)
```

**关键设计**: 使用 `__zts_memset` 和 `__zts_memcpy` 辅助宏避免宏递归展开。

### 16.3 统一驱动模块 (0x00000020.zts)

**源文件**: `SYSTEM/lib/lints/modules/all_drivers/all_drivers.c` (20KB)

**架构**:
```
                    driver_input
                         │
                         ▼
┌─────────────────────────────────────────────┐
│           all_drivers_exec                  │
│                                             │
│  driver_id ─────────────────────────────┐   │
│  │ 0 → virtblk_exec()                  │   │
│  │ 1 → virtnet_exec()                  │   │
│  │ 2 → ahci_exec()                     │   │
│  │ 3 → nvme_exec()                     │   │
│  │ 4 → e1000_tx/rx()                   │   │
│  │ 5 → rtl8188eu_tx/rx()               │   │
│  │ 6 → xhci_exec()                     │   │
│  │ 7 → QUERY (返回所有驱动状态位掩码)   │   │
│  └─────────────────────────────────────┘   │
│                                             │
│                  ▼                          │
│            driver_output                    │
└─────────────────────────────────────────────┘
```

**各驱动实现细节**:

#### virtio-blk (driver_id=0)
```c
struct virtblk_dev {
    u16 io_base;
    u64 capacity;           // 扇区数
    u32 blk_size;           // 512 字节
    struct virtblk_vq *vq;
    void *queue_mem;
    spinlock_t lock;
    u64 total_reads;
    u64 total_writes;
    u64 total_errors;
    int initialized;
};

// 命令: 0=READ, 1=WRITE, 2=FLUSH, 3=PROBE
```

**virtqueue 布局**:
```
内存布局: [desc table][avail ring][used ring]
          └── N*16  ──┘  └─2+N*2──┘  └─4+N*8──┘
```

#### virtio-net (driver_id=1)
```c
struct virtnet_dev {
    u8 mac[6];
    u32 mtu;               // 1500
    struct virtnet_vq *rx_vq;
    struct virtnet_vq *tx_vq;
    void *rx_bufs[256];
    void *tx_bufs[256];
    u64 rx_packets, tx_packets;
    u64 rx_bytes, tx_bytes;
    int initialized;
};

// 命令: 0=TX, 1=RX, 2=SET_MAC, 3=PROBE, 4=PROMISC
```

**接收队列预填充**: 初始化时分配 32 个 64KB 接收缓冲区。

#### AHCI SATA (driver_id=2)
```c
struct ahci_dev {
    u64 mmio_base;         // 0xFE010000
    u32 ports_implemented; // 位掩码
    struct ahci_port ports[32];
    spinlock_t lock;
    int initialized;
};

// MMIO 端口偏移: 0x100
```

#### NVMe (driver_id=3)
```c
struct nvme_dev {
    u64 mmio_base;         // 0xFE020000
    struct nvme_queue *admin_queue;
    struct nvme_queue *io_queues[16];
    u32 num_io_queues;
    struct nvme_ns namespaces[8];
    u32 num_namespaces;
    spinlock_t lock;
    u64 total_reads, total_writes;
    int initialized;
};

// 管理队列大小: 64
// I/O 队列大小: 256
```

**队列结构**:
```c
struct nvme_queue {
    u32 qid;
    u32 size;
    void *sq;  // submission queue (64 bytes/entry)
    void *cq;  // completion queue (16 bytes/entry)
    u32 sq_head, sq_tail;
    u32 cq_head;
};
```

#### e1000 (driver_id=4)
```c
struct e1000_dev {
    u64 io_base;           // 0xC100
    u64 mmio_base;         // 0xFE030000
    u8 mac[6];
    u32 mtu;
    struct e1000_rx_desc *rx_desc;  // 256 entries
    void *rx_bufs[256];
    void *tx_desc;
    void *tx_bufs[256];
    u64 rx_packets, tx_packets;
    spinlock_t lock;
    int initialized;
};

// RX 描述符: 16 bytes
// 最大包大小: 2048 bytes
```

#### RTL8188EU WiFi (driver_id=5)
```c
struct rtl8188eu_dev {
    u64 io_base;           // 0xC200
    u8 bssid[6];
    u8 mac[6];
    u32 channel;           // 默认 6
    int connected;
    int promiscuous;
    void *rx_queue[256];
    void *tx_queue[256];
    u64 rx_packets, tx_packets;
    spinlock_t lock;
    int initialized;
};
```

#### XHCI USB 3.0 (driver_id=6)
```c
struct xhci_dev {
    u64 mmio_base;         // 0xFE040000
    u32 num_slots;         // 256
    u32 num_ports;         // 4 (default)
    struct xhci_slot slots[256];
    spinlock_t lock;
    u64 total_transfers;
    int initialized;
};

// 传输类型: 0=CONTROL, 1=BULK IN, 2=BULK OUT, 3=INTERRUPT
```

### 16.4 编译流程

```bash
# 1. 设置环境
cd SYSTEM/lib/lints
mkdir -p zts_modules

# 2. 编译模块
./build_zts.sh build all_drivers

# 3. 重命名为正确优先级
mv zts_modules/0x00000000.zts zts_modules/0x00000020.zts

# 4. 复制到目标目录
mkdir -p ../../zrr64/zts/p.zts6
cp zts_modules/0x00000020.zts ../../zrr64/zts/p.zts6/
```

**编译标志**:
```
-nostdlib -fPIC -shared -O2 -Wall -fno-stack-protector -fno-builtin
-I include -I ${LINUX_SRC}/include -I ${LINUX_SRC}/arch/x86/include
```

### 16.5 已知问题与限制

1. **驱动通信未实现**: 当前驱动只实现了软件框架，真正的硬件通信（PCI 配置空间读取、MMIO 寄存器读写）需要在内核中实现

2. **中断处理**: 驱动中断请求 (IRQ) 处理需要在 ZratroOS 内核中注册中断处理程序

3. **DMA 支持**: 直接内存访问 (DMA) 需要 IOMMU 支持和物理地址映射

4. **SMP 同步**: 自旋锁实现简化为 busy-wait，未使用 CPU PAUSE 指令优化

### 16.6 性能优化建议

1. **合并接收缓冲区**: virtio-net 使用 mergeable receive buffers 减少数据包复制

2. **多队列支持**: virtio-blk 和 NVMe 支持多 I/O 队列，可以利用多核并行

3. **零拷贝网络**: 实现零拷贝数据包传输，避免内核和用户空间之间的数据复制

4. **预分配内存池**: 为高频操作（如网络包收发）预分配内存池，减少动态分配开销

---

*文档版本: 1.3*  
*分析日期: 2026-04-22*  
*更新: ZTS7 动态算法系统、Linux 内核移植、统一驱动模块*  
*内核版本: ZratroOS v1.6.0*  
*分析文件数: 99 个核心源文件*
