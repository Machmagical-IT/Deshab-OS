/*
 * Zratro OS - PMEM (持久内存管理子系统)
 * 
 * 创新设计：
 * 1. 内存/存储统一模型 - 消除序列化开销
 * 2. 瞬时启动 - 无需加载，直接执行
 * 3. 事务性持久化 - 与 Z-Journal 集成
 * 4. 崩溃一致性 - WAL 保证
 * 5. NUMA 感知 - 持久内存通常跨插槽
 */

#ifndef PMEM_H
#define PMEM_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ktypes.h"
#include "zjournal.h"

/* ========== PMEM 配置 ========== */

#define PMEM_MAX_SEGMENTS       64          /* 最大段数 */
#define PMEM_MAX_NAME_LEN       32          /* 段名最大长度 */
#define PMEM_MIN_SEGMENT_SIZE   (4 * 1024)  /* 最小段大小 4KB */
#define PMEM_ALIGN              4096        /* 默认对齐 */
#define PMEM_MAGIC              0x504D454D  /* 'PMEM' */

/* ========== 持久内存类型 ========== */

/* 持久内存区域类型 */
typedef enum {
    PMEM_TYPE_NONE = 0,
    PMEM_TYPE_PMEM,             /* 真正的持久内存 (如 Intel Optane) */
    PMEM_TYPE_PMEM_LEGACY,      /* 传统模拟持久内存 */
    PMEM_TYPE_NVDIMM,           /* NVDIMM 设备 */
    PMEM_TYPE_FILE_BACKED,      /* 文件后援 (用于测试) */
} pmem_type_t;

/* 持久内存拓扑 */
struct pmem_topology {
    uint64_t total_size;        /* 总大小 */
    uint64_t available_size;    /* 可用大小 */
    uint32_t numa_node;         /* NUMA 节点 */
    uint32_t interleaved;       /* 是否交错 */
    pmem_type_t type;           /* 类型 */
    uint32_t bandwidth;         /* 带宽 (MB/s) */
    uint32_t latency_ns;        /* 延迟 (纳秒) */
};

/* ========== 持久内存段 ========== */

/* 段标志 */
#define PMEM_SEG_FLAG_PERSISTENT   (1 << 0)   /* 持久化 */
#define PMEM_SEG_FLAG_TRANSACTIONAL (1 << 1)  /* 事务性 */
#define PMEM_SEG_FLAG_EXECUTABLE   (1 << 2)   /* 可执行 */
#define PMEM_SEG_FLAG_SHARED       (1 << 3)   /* 共享 */
#define PMEM_SEG_FLAG_SNAPSHOT     (1 << 4)   /* 支持快照 */
#define PMEM_SEG_FLAG_COMPRESSED   (1 << 5)   /* 已压缩 */
#define PMEM_SEG_FLAG_ENCRYPTED    (1 << 6)   /* 已加密 */

/* 段状态 */
typedef enum {
    PMEM_SEG_STATE_NONE = 0,
    PMEM_SEG_STATE_CREATING,
    PMEM_SEG_STATE_ACTIVE,
    PMEM_SEG_STATE_SNAPSHOTTING,
    PMEM_SEG_STATE_RECOVERING,
    PMEM_SEG_STATE_COMPACTING,
    PMEM_SEG_STATE_CLOSING,
    PMEM_SEG_STATE_CLOSED,
} pmem_seg_state_t;

/* 持久内存段头 (存储在段开头) */
struct pmem_seg_header {
    uint32_t magic;             /* 魔数: PMEM_MAGIC */
    uint32_t version;           /* 版本号 */
    uint64_t seg_id;            /* 段 ID */
    char name[PMEM_MAX_NAME_LEN]; /* 段名 */
    uint64_t size;              /* 段大小 */
    uint64_t used_size;         /* 已用大小 */
    uint32_t flags;             /* 标志 */
    uint32_t checksum;          /* 头校验 */
    
    /* 事务关联 */
    uint64_t last_trans_id;     /* 最后事务 ID */
    uint64_t last_commit_ts;    /* 最后提交时间戳 */
    
    /* 快照 */
    uint64_t snapshot_id;       /* 当前快照 ID */
    uint64_t snapshot_offset;   /* 快照偏移 */
    
    /* 元数据 */
    uint64_t create_time;       /* 创建时间 */
    uint64_t modify_time;       /* 修改时间 */
    uint32_t owner_pid;         /* 所有者进程 */
    uint32_t ref_count;         /* 引用计数 */
} __attribute__((packed));

/* 持久内存段描述符 */
struct pmem_segment {
    uint64_t seg_id;            /* 段 ID */
    char name[PMEM_MAX_NAME_LEN]; /* 段名 */
    
    /* 地址映射 */
    void *base;                 /* 直接映射地址 (虚拟) */
    uint64_t phys_addr;         /* 物理地址 */
    uint64_t size;              /* 大小 */
    
    /* 状态 */
    pmem_seg_state_t state;
    uint32_t flags;
    
    /* 事务关联 */
    struct zj_transaction *active_tx;  /* 活跃事务 */
    struct zjournal *journal;          /* 关联日志 */
    
    /* 链表 */
    struct list_head list;
    struct list_head global_list;
    
    /* 锁 */
    spinlock_t lock;
    
    /* 引用计数 */
    int ref_count;
};

/* ========== 事务性持久化 API ========== */

/* 持久化操作类型 */
typedef enum {
    PMEM_OP_WRITE = 1,          /* 写入 */
    PMEM_OP_MEMCPY,             /* 内存复制 */
    PMEM_OP_MEMSET,             /* 内存设置 */
    PMEM_OP_ALLOC,              /* 分配 */
    PMEM_OP_FREE,               /* 释放 */
    PMEM_OP_SNAPSHOT,           /* 快照 */
    PMEM_OP_ROLLBACK,           /* 回滚 */
} pmem_op_type_t;

/* 持久化操作记录 */
struct pmem_op_record {
    pmem_op_type_t type;
    uint64_t offset;            /* 段内偏移 */
    uint64_t size;              /* 大小 */
    void *data;                 /* 数据 (用于恢复) */
    struct list_head list;
};

/* ========== 快照系统 ========== */

/* 持久内存快照 */
struct pmem_snapshot {
    uint64_t snapshot_id;       /* 快照 ID */
    uint64_t seg_id;            /* 段 ID */
    uint64_t timestamp;         /* 创建时间 */
    uint64_t size;              /* 快照大小 */
    uint64_t trans_id;          /* 关联事务 ID */
    
    /* 增量快照 */
    uint64_t base_snapshot;     /* 基础快照 ID (0 = 完整快照) */
    uint32_t is_incremental;    /* 是否增量 */
    uint32_t change_count;      /* 变更数 */
    
    void *data;                 /* 快照数据 */
    struct list_head list;
};

/* ========== 分配器扩展 ========== */

/* 持久内存堆 */
struct pmem_heap {
    struct pmem_segment *seg;   /* 所属段 */
    uint64_t free_offset;       /* 空闲偏移 */
    uint64_t allocated;         /* 已分配 */
    
    /* 空闲块链表 */
    struct list_head free_list;
    uint32_t free_count;
    
    spinlock_t lock;
};

/* 持久内存块头 */
struct pmem_block_header {
    uint32_t magic;             /* 魔数 */
    uint32_t size;              /* 块大小 */
    uint64_t alloc_id;          /* 分配 ID */
    uint32_t flags;
    uint32_t checksum;
} __attribute__((packed));

#define PMEM_BLOCK_MAGIC    0x424D504D  /* 'MPMB' */

/* ========== 全局 PMEM 管理器 ========== */

struct pmem_manager {
    /* 拓扑信息 */
    struct pmem_topology topology;
    
    /* 段管理 */
    struct list_head segments;          /* 活跃段链表 */
    uint32_t segment_count;
    spinlock_t seg_lock;
    
    /* ID 分配 */
    uint64_t next_seg_id;
    uint64_t next_snapshot_id;
    
    /* 快照管理 */
    struct list_head snapshots;
    spinlock_t snap_lock;
    
    /* 日志集成 */
    struct zjournal *journal;
    
    /* 统计 */
    uint64_t total_allocated;
    uint64_t total_freed;
    uint64_t snapshots_created;
    uint64_t snapshots_restored;
    
    /* 状态 */
    int initialized;
    int recovery_mode;
};

/* ========== 系统调用接口 ========== */

/* 用户态段句柄 */
struct pmem_handle_user {
    uint64_t seg_id;            /* 段 ID */
    uint64_t size;              /* 大小 */
    uint32_t flags;             /* 标志 */
    uint32_t state;             /* 状态 */
    uint64_t base_addr;         /* 用户态映射地址 */
};

/* 系统调用号 (扩展) */
#define SYS_PMEM_CREATE     50    /* 创建持久段 */
#define SYS_PMEM_OPEN       51    /* 打开持久段 */
#define SYS_PMEM_CLOSE      52    /* 关闭持久段 */
#define SYS_PMEM_ALLOC      53    /* 持久分配 */
#define SYS_PMEM_FREE       54    /* 持久释放 */
#define SYS_PMEM_SNAPSHOT   55    /* 创建快照 */
#define SYS_PMEM_RESTORE    56    /* 恢复快照 */
#define SYS_PMEM_SYNC       57    /* 同步到持久存储 */
#define SYS_PMEM_STAT       58    /* 获取统计信息 */

/* ========== 公共 API ========== */

/* 初始化/销毁 */
int pmem_init(struct pmem_manager *pm, uint64_t pmem_start, uint64_t pmem_size,
              pmem_type_t type, struct zjournal *journal);
void pmem_destroy(struct pmem_manager *pm);

/* 段管理 */
struct pmem_segment *pmem_create_segment(struct pmem_manager *pm,
                                          const char *name, uint64_t size,
                                          uint32_t flags);
struct pmem_segment *pmem_open_segment(struct pmem_manager *pm,
                                        const char *name, uint32_t flags);
int pmem_close_segment(struct pmem_manager *pm, struct pmem_segment *seg);
int pmem_destroy_segment(struct pmem_manager *pm, const char *name);

/* 段查找 */
struct pmem_segment *pmem_find_segment(struct pmem_manager *pm, uint64_t seg_id);
struct pmem_segment *pmem_find_segment_by_name(struct pmem_manager *pm, 
                                                const char *name);

/* 持久内存操作 */
int pmem_write(struct pmem_segment *seg, uint64_t offset,
               const void *data, size_t size);
int pmem_read(struct pmem_segment *seg, uint64_t offset,
              void *data, size_t size);

/* 事务性操作 */
int pmem_trans_write(struct pmem_segment *seg, uint64_t offset,
                     const void *data, size_t size,
                     struct zj_transaction *tx);
int pmem_trans_memcpy(struct pmem_segment *seg, uint64_t dst_offset,
                      uint64_t src_offset, size_t size,
                      struct zj_transaction *tx);

/* 持久分配 */
void *pmem_alloc(struct pmem_segment *seg, size_t size);
void pmem_free(struct pmem_segment *seg, void *ptr);
uint64_t pmem_alloc_offset(struct pmem_segment *seg, size_t size);
int pmem_free_offset(struct pmem_segment *seg, uint64_t offset);

/* 快照 */
struct pmem_snapshot *pmem_snapshot_create(struct pmem_manager *pm,
                                            struct pmem_segment *seg,
                                            int incremental);
int pmem_snapshot_restore(struct pmem_manager *pm,
                          struct pmem_snapshot *snap);
int pmem_snapshot_delete(struct pmem_manager *pm, uint64_t snapshot_id);

/* 同步 */
int pmem_sync(struct pmem_segment *seg);
int pmem_flush(struct pmem_segment *seg, uint64_t offset, size_t size);

/* 恢复 */
int pmem_recover(struct pmem_manager *pm);
int pmem_recover_segment(struct pmem_manager *pm, const char *name);

/* 统计 */
void pmem_get_stats(struct pmem_manager *pm, uint64_t *allocated,
                    uint64_t *freed, uint64_t *segments);

/* ========== 高级特性 ========== */

/* 瞬时启动支持 */
int pmem_bootstrap(struct pmem_manager *pm);
int pmem_save_boot_state(struct pmem_manager *pm);
int pmem_restore_boot_state(struct pmem_manager *pm);

/* 压缩 */
int pmem_compact_segment(struct pmem_segment *seg);
uint64_t pmem_fragmentation(struct pmem_segment *seg);

/* 加密 */
int pmem_encrypt_segment(struct pmem_segment *seg, const void *key, size_t key_len);
int pmem_decrypt_segment(struct pmem_segment *seg, const void *key, size_t key_len);

/* NUMA 感知 */
int pmem_set_numa_policy(struct pmem_manager *pm, uint32_t node, int interleave);
uint32_t pmem_get_numa_node(struct pmem_segment *seg);

/* ========== 全局管理器 API ========== */

int pmem_global_init(uint64_t pmem_start, uint64_t pmem_size,
                     pmem_type_t type, struct zjournal *journal);
struct pmem_manager *pmem_get_global(void);

/* ========== 内联辅助 ========== */

/* 刷新 CPU 缓存到持久内存 */
static inline void pmem_persist(void *addr, size_t size) {
    /* 使用 CLFLUSH 或 CLWB 指令 */
    __asm__ volatile (
        "mfence\n"
        :: : "memory"
    );
    
    /* 逐缓存行刷新 */
    for (size_t i = 0; i < size; i += 64) {
        __asm__ volatile (
            "clflush %0\n"
            :: "m" (*((char *)addr + i))
        );
    }
    
    __asm__ volatile (
        "sfence\n"
        :: : "memory"
    );
}

/* 非临时存储 (绕过缓存) */
static inline void pmem_memcpy_nodrain(void *dst, const void *src, size_t size) {
    /* 使用 MOVNTI 指令 */
    __asm__ volatile (
        "mfence\n"
        :: : "memory"
    );
    
    /* 简化实现，实际应使用非临时存储 */
    memcpy(dst, src, size);
}

/* 检查持久内存是否可用 */
static inline int pmem_is_available(void) {
    /* 检查 CPU 是否支持 CLFLUSH */
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1)
    );
    return (edx >> 19) & 1;  /* CLFLUSH 位 */
}

/* 获取段头 */
static inline struct pmem_seg_header *pmem_get_header(struct pmem_segment *seg) {
    return (struct pmem_seg_header *)seg->base;
}

/* 获取段数据区 */
static inline void *pmem_get_data_area(struct pmem_segment *seg) {
    return (void *)((uint8_t *)seg->base + sizeof(struct pmem_seg_header));
}

/* 计算段数据区大小 */
static inline uint64_t pmem_get_data_size(struct pmem_segment *seg) {
    return seg->size - sizeof(struct pmem_seg_header);
}

/* 检查偏移是否有效 */
static inline int pmem_offset_valid(struct pmem_segment *seg, uint64_t offset) {
    return offset < pmem_get_data_size(seg);
}

/* 引用计数 */
static inline void pmem_seg_get(struct pmem_segment *seg) {
    spin_lock(&seg->lock);
    seg->ref_count++;
    spin_unlock(&seg->lock);
}

static inline void pmem_seg_put(struct pmem_segment *seg) {
    spin_lock(&seg->lock);
    seg->ref_count--;
    spin_unlock(&seg->lock);
}

#endif /* PMEM_H */
