/*
 * Zratro OS - Z-Journal (原创日志系统)
 * 
 * 创新设计：
 * 1. 链式事务日志 - 非传统环形缓冲区
 * 2. 多级日志缓冲 - 减少磁盘 I/O
 * 3. 快照隔离协议 - 无锁读取
 * 4. 版本号冲突检测 - 并行提交支持
 * 5. 智能压缩回放 - 快速恢复
 */

#ifndef ZJOURNAL_H
#define ZJOURNAL_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== Z-Journal 配置 ========== */

#define ZJ_MAX_TRANS_SIZE      (64 * 1024)   /* 单事务最大 64KB */
#define ZJ_LOG_BLOCK_SIZE      4096          /* 日志块大小 */
#define ZJ_MAX_LOG_BLOCKS      4096          /* 最大日志块数 */
#define ZJ_CHECKPOINT_INTERVAL 100           /* 检查点间隔 (事务数) */
#define ZJ_VERSION_BITS        56            /* 版本号位数 */
#define ZJ_FLAGS_BITS          8             /* 标志位数 */

/* ========== 链式事务结构 ========== */

/* 日志块头 */
struct zj_block_header {
    uint64_t block_id;           /* 块唯一 ID */
    uint64_t prev_block;         /* 前一块物理位置 */
    uint64_t next_block;         /* 下一块物理位置 */
    uint32_t magic;              /* 魔数: 'ZJBL' */
    uint32_t flags;              /* 标志 */
    uint32_t data_size;          /* 数据大小 */
    uint32_t checksum;           /* CRC32 校验 */
    uint64_t timestamp;          /* 时间戳 */
    uint64_t trans_id;           /* 所属事务 ID */
} __attribute__((packed));

#define ZJ_BLOCK_MAGIC    0x4C424A5A  /* 'ZJBL' */

/* 块标志 */
#define ZJ_BLK_FLAG_HEAD      (1 << 0)   /* 事务头块 */
#define ZJ_BLK_FLAG_TAIL      (1 << 1)   /* 事务尾块 */
#define ZJ_BLK_FLAG_COMMIT    (1 << 2)   /* 已提交 */
#define ZJ_BLK_FLAG_ABORT     (1 << 3)   /* 已中止 */
#define ZJ_BLK_FLAG_CHECKPOINT (1 << 4)  /* 检查点 */
#define ZJ_BLK_FLAG_COMPRESSED (1 << 5)  /* 已压缩 */

/* 事务操作类型 */
typedef enum {
    ZJ_OP_WRITE_BLOCK = 1,       /* 写块 */
    ZJ_OP_UPDATE_INODE,          /* 更新 Inode */
    ZJ_OP_CREATE_FILE,           /* 创建文件 */
    ZJ_OP_DELETE_FILE,           /* 删除文件 */
    ZJ_OP_RENAME,                /* 重命名 */
    ZJ_OP_TRUNCATE,              /* 截断 */
    ZJ_OP_SET_XATTR,             /* 设置扩展属性 */
    ZJ_OP_DELETE_XATTR,          /* 删除扩展属性 */
    ZJ_OP_SYNC,                  /* 同步 */
    ZJ_OP_2PC_PREPARE,           /* 两阶段提交 - 准备 */
    ZJ_OP_2PC_COMMIT,            /* 两阶段提交 - 提交 */
    ZJ_OP_2PC_ABORT,             /* 两阶段提交 - 中止 */
    ZJ_OP_WAL_WRITE,             /* WAL 写入 */
    ZJ_OP_CHECKPOINT,            /* 检查点 */
} zj_op_type_t;

/* 操作标志 */
#define ZJ_OP_FLAG_MERGED    (1 << 0)   /* 已合并 */

/* 事务状态 (扩展) - 在 zj_trans_state_t 枚举中已定义 */

/* 事务操作记录 */
struct zj_op_record {
    uint8_t op_type;             /* 操作类型 */
    uint8_t flags;               /* 标志 */
    uint16_t reserved;
    uint32_t data_size;          /* 数据大小 */
    uint64_t target_block;       /* 目标块号 */
    uint64_t target_offset;      /* 目标偏移 */
    uint64_t version_before;     /* 操作前版本 */
    uint64_t version_after;      /* 操作后版本 */
    uint8_t data[];              /* 变量长度数据 */
} __attribute__((packed));

/* ========== 多级缓冲区 ========== */

/* 日志缓冲区级别 */
typedef enum {
    ZJ_BUF_LEVEL_HOT = 0,        /* 热缓冲 - 最近事务 */
    ZJ_BUF_LEVEL_WARM = 1,       /* 温缓冲 - 待刷盘 */
    ZJ_BUF_LEVEL_COLD = 2,       /* 冷缓冲 - 已刷盘 */
    ZJ_BUF_LEVELS = 3
} zj_buf_level_t;

/* 缓冲区块 */
struct zj_buf_block {
    struct list_head list;       /* 链表节点 */
    uint64_t trans_id;           /* 事务 ID */
    uint64_t version;            /* 版本号 */
    uint32_t data_size;          /* 数据大小 */
    uint32_t flags;              /* 标志 */
    zj_buf_level_t level;        /* 缓冲级别 */
    int ref_count;               /* 引用计数 */
    uint8_t data[ZJ_LOG_BLOCK_SIZE];
};

/* 多级缓冲区管理器 */
struct zj_buffer_manager {
    struct list_head levels[ZJ_BUF_LEVELS];  /* 各级链表 */
    uint32_t counts[ZJ_BUF_LEVELS];          /* 各级计数 */
    uint32_t max_per_level;                  /* 每级最大块数 */
    spinlock_t lock;
    
    /* 统计 */
    uint64_t total_blocks;
    uint64_t hits;
    uint64_t misses;
    uint64_t promotions;        /* 升级次数 */
    uint64_t demotions;         /* 降级次数 */
};

/* ========== 版本化事务系统 ========== */

/* 事务状态 */
typedef enum {
    ZJ_TRANS_NEW = 0,
    ZJ_TRANS_ACTIVE,
    ZJ_TRANS_PREPARING,
    ZJ_TRANS_PREPARED,
    ZJ_TRANS_COMMITTING,
    ZJ_TRANS_COMMITTED,
    ZJ_TRANS_ABORTED,
} zj_trans_state_t;

/* 事务描述符 */
struct zj_transaction {
    uint64_t trans_id;           /* 事务 ID (唯一) */
    uint64_t version;            /* 提交版本号 */
    zj_trans_state_t state;      /* 状态 */
    uint32_t flags;              /* 标志 */
    uint32_t op_count;           /* 操作数 */
    
    /* 操作链表 */
    struct list_head ops;        /* 操作记录链 */
    struct list_head blocks;     /* 日志块链 */
    
    /* 冲突检测 */
    uint64_t read_set[16];       /* 读集合 (块号) */
    uint64_t write_set[16];      /* 写集合 (块号) */
    int read_count;
    int write_count;
    
    /* 时间戳 */
    uint64_t start_time;
    uint64_t commit_time;
    
    /* 链表 */
    struct list_head list;
};

/* 事务标志 */
#define ZJ_TRANS_FLAG_READONLY   (1 << 0)
#define ZJ_TRANS_FLAG_SYNC       (1 << 1)   /* 同步提交 */
#define ZJ_TRANS_FLAG_NESTED     (1 << 2)   /* 嵌套事务 */

/* ========== 快照隔离管理器 ========== */

/* 版本化块 */
struct zj_versioned_block {
    uint64_t block_no;           /* 块号 */
    uint64_t version;            /* 当前版本 */
    uint64_t commit_version;     /* 提交版本 */
    spinlock_t lock;
    struct list_head readers;    /* 读取者列表 */
    struct list_head writers;    /* 写入者列表 */
    uint8_t *data;               /* 数据缓冲 */
};

/* 快照 */
struct zj_snapshot {
    uint64_t snapshot_id;        /* 快照 ID */
    uint64_t version;            /* 快照版本 */
    uint64_t timestamp;          /* 创建时间 */
    struct list_head list;
};

/* 快照隔离管理器 */
struct zj_snapshot_manager {
    struct list_head snapshots;  /* 快照链表 */
    uint64_t current_version;    /* 当前全局版本 */
    uint64_t min_snapshot_ver;   /* 最老快照版本 */
    spinlock_t lock;
    
    /* 版本化块哈希表 */
    struct zj_versioned_block *blocks;
    uint32_t blocks_size;
};

/* ========== 检查点系统 ========== */

/* 检查点记录 */
struct zj_checkpoint {
    uint64_t checkpoint_id;      /* 检查点 ID */
    uint64_t version;            /* 检查点版本 */
    uint64_t trans_id;           /* 最后事务 ID */
    uint64_t timestamp;          /* 创建时间 */
    uint32_t block_count;        /* 块数 */
    uint32_t flags;
    
    /* 元数据 */
    uint64_t superblock_copy;    /* 超级块副本位置 */
    uint64_t group_desc_copy;    /* 块组描述符副本 */
    
    struct list_head list;
};

/* 检查点标志 */
#define ZJ_CKPT_FLAG_FULL      (1 << 0)   /* 完整检查点 */
#define ZJ_CKPT_FLAG_DIRTY     (1 << 1)   /* 脏页检查点 */
#define ZJ_CKPT_FLAG_COMPRESSED (1 << 2)  /* 已压缩 */

/* ========== Z-Journal 主结构 ========== */

struct zjournal {
    /* 设备信息 */
    void *device;
    int (*read_block)(void *, uint64_t, void *);
    int (*write_block)(void *, uint64_t, const void *);
    
    /* 日志区域 */
    uint64_t log_start;          /* 日志起始块 */
    uint64_t log_size;           /* 日志大小 (块数) */
    uint64_t log_head;           /* 日志头位置 */
    uint64_t log_tail;           /* 日志尾位置 */
    
    /* 事务管理 */
    uint64_t next_trans_id;      /* 下一个事务 ID */
    uint64_t next_version;       /* 下一个版本号 */
    struct list_head active_trans; /* 活跃事务 */
    struct list_head committed_trans; /* 已提交事务 */
    spinlock_t trans_lock;
    
    /* 缓冲区管理器 */
    struct zj_buffer_manager buf_mgr;
    
    /* 快照管理器 */
    struct zj_snapshot_manager snap_mgr;
    
    /* 检查点 */
    struct list_head checkpoints;
    struct zj_checkpoint *last_checkpoint;
    spinlock_t ckpt_lock;
    
    /* 统计 */
    uint64_t trans_committed;
    uint64_t trans_aborted;
    uint64_t blocks_written;
    uint64_t blocks_read;
    
    /* 状态 */
    int is_recovery;
    int is_syncing;
    int recovery_needed;
};

/* ========== 公共 API ========== */

/* 初始化/销毁 */
int zjournal_init(struct zjournal *zj, void *device,
                  int (*read_block)(void *, uint64_t, void *),
                  int (*write_block)(void *, uint64_t, const void *),
                  uint64_t log_start, uint64_t log_size);

void zjournal_destroy(struct zjournal *zj);

/* 事务 API */
struct zj_transaction *zj_trans_begin(struct zjournal *zj, uint32_t flags);
int zj_trans_commit(struct zjournal *zj, struct zj_transaction *trans);
int zj_trans_abort(struct zjournal *zj, struct zj_transaction *trans);

/* 操作 API */
int zj_add_write_op(struct zjournal *zj, struct zj_transaction *trans,
                    uint64_t block_no, uint64_t offset, 
                    const void *data, size_t size);

int zj_add_inode_op(struct zjournal *zj, struct zj_transaction *trans,
                    uint32_t ino, const void *inode_data, size_t size);

/* 快照 API */
struct zj_snapshot *zj_snapshot_create(struct zjournal *zj);
void zj_snapshot_release(struct zjournal *zj, struct zj_snapshot *snap);
int zj_snapshot_read(struct zjournal *zj, struct zj_snapshot *snap,
                     uint64_t block_no, void *buf);

/* 检查点 API */
int zj_checkpoint_create(struct zjournal *zj, int full);
int zj_checkpoint_recover(struct zjournal *zj, uint64_t checkpoint_id);

/* 恢复 API */
int zj_recover(struct zjournal *zj);
int zj_replay_log(struct zjournal *zj);

/* 同步 API */
int zj_sync(struct zjournal *zj, int wait);
int zj_flush_buffers(struct zjournal *zj, zj_buf_level_t level);

/* 统计 API */
void zj_get_stats(struct zjournal *zj, uint64_t *committed, 
                  uint64_t *aborted, uint64_t *written);

/* ========== 高级特性 ========== */

/* 并行提交支持 */
int zj_can_parallel_commit(struct zjournal *zj, 
                           struct zj_transaction *t1,
                           struct zj_transaction *t2);

/* 智能压缩 */
int zj_compress_log(struct zjournal *zj, uint64_t start_trans, 
                    uint64_t end_trans);

/* 版本号比较 (用于冲突检测) */
static inline int zj_version_conflict(uint64_t v1, uint64_t v2) {
    /* 使用版本号高位作为冲突检测 */
    return ((v1 ^ v2) & 0x00FFFFFFFFFFFFFFULL) != 0;
}

/* 块版本提取 */
static inline uint64_t zj_block_version(uint64_t versioned_addr) {
    return versioned_addr >> ZJ_FLAGS_BITS;
}

/* 块标志提取 */
static inline uint64_t zj_block_flags(uint64_t versioned_addr) {
    return versioned_addr & ((1ULL << ZJ_FLAGS_BITS) - 1);
}

/* 组合版本和标志 */
static inline uint64_t zj_make_versioned(uint64_t version, uint64_t flags) {
    return (version << ZJ_FLAGS_BITS) | (flags & ((1ULL << ZJ_FLAGS_BITS) - 1));
}

#endif /* ZJOURNAL_H */
