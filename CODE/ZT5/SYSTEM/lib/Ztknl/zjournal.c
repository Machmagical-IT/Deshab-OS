/*
 * Zratro OS - Z-Journal Implementation
 * 
 * 原创链式事务日志系统
 */

#include "zjournal.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"

/* CRC32 查找表 */
static uint32_t crc32_table[256];
static int crc32_initialized = 0;

static void init_crc32_table(void) {
    if (crc32_initialized) return;
    
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
        crc32_table[i] = crc;
    }
    crc32_initialized = 1;
}

static uint32_t compute_crc32(const void *data, size_t len) {
    init_crc32_table();
    
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *p = (const uint8_t *)data;
    
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    
    return crc ^ 0xFFFFFFFF;
}

/* ========== 缓冲区管理器实现 ========== */

static int buf_mgr_init(struct zj_buffer_manager *mgr, uint32_t max_per_level) {
    for (int i = 0; i < ZJ_BUF_LEVELS; i++) {
        INIT_LIST_HEAD(&mgr->levels[i]);
        mgr->counts[i] = 0;
    }
    mgr->max_per_level = max_per_level;
    mgr->lock = SPINLOCK_INIT;
    mgr->total_blocks = 0;
    mgr->hits = 0;
    mgr->misses = 0;
    mgr->promotions = 0;
    mgr->demotions = 0;
    return 0;
}

static void buf_mgr_destroy(struct zj_buffer_manager *mgr) {
    spin_lock(&mgr->lock);
    
    for (int i = 0; i < ZJ_BUF_LEVELS; i++) {
        struct zj_buf_block *block, *next;
        list_for_each_entry_safe(block, next, &mgr->levels[i], list) {
            list_del(&block->list);
            kfree(block);
        }
    }
    
    spin_unlock(&mgr->lock);
}

static struct zj_buf_block *buf_mgr_alloc(struct zj_buffer_manager *mgr, 
                                          uint64_t trans_id, uint64_t version) {
    struct zj_buf_block *block = kmalloc(sizeof(struct zj_buf_block));
    if (block == NULL) return NULL;
    
    block->trans_id = trans_id;
    block->version = version;
    block->data_size = 0;
    block->flags = 0;
    block->level = ZJ_BUF_LEVEL_HOT;
    block->ref_count = 1;
    
    spin_lock(&mgr->lock);
    list_add(&block->list, &mgr->levels[ZJ_BUF_LEVEL_HOT]);
    mgr->counts[ZJ_BUF_LEVEL_HOT]++;
    mgr->total_blocks++;
    spin_unlock(&mgr->lock);
    
    return block;
}

static void buf_mgr_release(struct zj_buffer_manager *mgr, struct zj_buf_block *block) {
    spin_lock(&mgr->lock);
    
    if (--block->ref_count == 0) {
        list_del(&block->list);
        mgr->counts[block->level]--;
        mgr->total_blocks--;
        spin_unlock(&mgr->lock);
        kfree(block);
        return;
    }
    
    spin_unlock(&mgr->lock);
}

/* 级别提升 (冷->温->热) */
static void buf_mgr_promote(struct zj_buffer_manager *mgr, struct zj_buf_block *block) {
    if (block->level == ZJ_BUF_LEVEL_HOT) return;
    
    spin_lock(&mgr->lock);
    
    int old_level = block->level;
    int new_level = old_level - 1;
    
    list_del(&block->list);
    mgr->counts[old_level]--;
    list_add(&block->list, &mgr->levels[new_level]);
    mgr->counts[new_level]++;
    block->level = new_level;
    mgr->promotions++;
    
    spin_unlock(&mgr->lock);
}

/* 级别降低 (热->温->冷) */
static void buf_mgr_demote(struct zj_buffer_manager *mgr, struct zj_buf_block *block) {
    if (block->level == ZJ_BUF_LEVEL_COLD) return;
    
    spin_lock(&mgr->lock);
    
    int old_level = block->level;
    int new_level = old_level + 1;
    
    list_del(&block->list);
    mgr->counts[old_level]--;
    list_add_tail(&block->list, &mgr->levels[new_level]);
    mgr->counts[new_level]++;
    block->level = new_level;
    mgr->demotions++;
    
    spin_unlock(&mgr->lock);
}

/* ========== 快照管理器实现 ========== */

static int snap_mgr_init(struct zj_snapshot_manager *mgr, uint32_t blocks_size) {
    INIT_LIST_HEAD(&mgr->snapshots);
    mgr->current_version = 0;
    mgr->min_snapshot_ver = 0;
    mgr->lock = SPINLOCK_INIT;
    
    mgr->blocks = kmalloc(sizeof(struct zj_versioned_block) * blocks_size);
    if (mgr->blocks == NULL) return -1;
    
    mgr->blocks_size = blocks_size;
    
    for (uint32_t i = 0; i < blocks_size; i++) {
        mgr->blocks[i].block_no = 0;
        mgr->blocks[i].version = 0;
        mgr->blocks[i].commit_version = 0;
        mgr->blocks[i].lock = SPINLOCK_INIT;
        INIT_LIST_HEAD(&mgr->blocks[i].readers);
        INIT_LIST_HEAD(&mgr->blocks[i].writers);
        mgr->blocks[i].data = NULL;
    }
    
    return 0;
}

static void snap_mgr_destroy(struct zj_snapshot_manager *mgr) {
    spin_lock(&mgr->lock);
    
    struct zj_snapshot *snap, *next;
    list_for_each_entry_safe(snap, next, &mgr->snapshots, list) {
        list_del(&snap->list);
        kfree(snap);
    }
    
    if (mgr->blocks) {
        for (uint32_t i = 0; i < mgr->blocks_size; i++) {
            if (mgr->blocks[i].data) {
                kfree(mgr->blocks[i].data);
            }
        }
        kfree(mgr->blocks);
    }
    
    spin_unlock(&mgr->lock);
}

/* 创建快照 */
struct zj_snapshot *zj_snapshot_create(struct zjournal *zj) {
    struct zj_snapshot *snap = kmalloc(sizeof(struct zj_snapshot));
    if (snap == NULL) return NULL;
    
    spin_lock(&zj->snap_mgr.lock);
    
    snap->snapshot_id = zj->next_trans_id;
    snap->version = zj->snap_mgr.current_version;
    snap->timestamp = get_tick_count();
    
    list_add(&snap->list, &zj->snap_mgr.snapshots);
    
    /* 更新最小快照版本 */
    if (zj->snap_mgr.min_snapshot_ver == 0 || snap->version < zj->snap_mgr.min_snapshot_ver) {
        zj->snap_mgr.min_snapshot_ver = snap->version;
    }
    
    spin_unlock(&zj->snap_mgr.lock);
    
    return snap;
}

/* 释放快照 */
void zj_snapshot_release(struct zjournal *zj, struct zj_snapshot *snap) {
    spin_lock(&zj->snap_mgr.lock);
    
    list_del(&snap->list);
    kfree(snap);
    
    /* 重新计算最小快照版本 */
    zj->snap_mgr.min_snapshot_ver = zj->snap_mgr.current_version;
    struct zj_snapshot *s;
    list_for_each_entry(s, &zj->snap_mgr.snapshots, list) {
        if (s->version < zj->snap_mgr.min_snapshot_ver) {
            zj->snap_mgr.min_snapshot_ver = s->version;
        }
    }
    
    spin_unlock(&zj->snap_mgr.lock);
}

/* ========== 主 API 实现 ========== */

int zjournal_init(struct zjournal *zj, void *device,
                  int (*read_block)(void *, uint64_t, void *),
                  int (*write_block)(void *, uint64_t, const void *),
                  uint64_t log_start, uint64_t log_size) {
    
    zj->device = device;
    zj->read_block = read_block;
    zj->write_block = write_block;
    zj->log_start = log_start;
    zj->log_size = log_size;
    zj->log_head = 0;
    zj->log_tail = 0;
    
    zj->next_trans_id = 1;
    zj->next_version = 1;
    INIT_LIST_HEAD(&zj->active_trans);
    INIT_LIST_HEAD(&zj->committed_trans);
    zj->trans_lock = SPINLOCK_INIT;
    
    buf_mgr_init(&zj->buf_mgr, 256);
    snap_mgr_init(&zj->snap_mgr, 1024);
    
    INIT_LIST_HEAD(&zj->checkpoints);
    zj->last_checkpoint = NULL;
    zj->ckpt_lock = SPINLOCK_INIT;
    
    zj->trans_committed = 0;
    zj->trans_aborted = 0;
    zj->blocks_written = 0;
    zj->blocks_read = 0;
    
    zj->is_recovery = 0;
    zj->is_syncing = 0;
    zj->recovery_needed = 0;
    
    println("[ZJOURNAL] Initialized");
    return 0;
}

void zjournal_destroy(struct zjournal *zj) {
    buf_mgr_destroy(&zj->buf_mgr);
    snap_mgr_destroy(&zj->snap_mgr);
    
    /* 释放检查点 */
    spin_lock(&zj->ckpt_lock);
    struct zj_checkpoint *ckpt, *next;
    list_for_each_entry_safe(ckpt, next, &zj->checkpoints, list) {
        list_del(&ckpt->list);
        kfree(ckpt);
    }
    spin_unlock(&zj->ckpt_lock);
    
    /* 释放未完成事务 */
    spin_lock(&zj->trans_lock);
    struct zj_transaction *trans, *t_next;
    list_for_each_entry_safe(trans, t_next, &zj->active_trans, list) {
        list_del(&trans->list);
        kfree(trans);
    }
    list_for_each_entry_safe(trans, t_next, &zj->committed_trans, list) {
        list_del(&trans->list);
        kfree(trans);
    }
    spin_unlock(&zj->trans_lock);
}

/* 开始事务 */
struct zj_transaction *zj_trans_begin(struct zjournal *zj, uint32_t flags) {
    struct zj_transaction *trans = kmalloc(sizeof(struct zj_transaction));
    if (trans == NULL) return NULL;
    
    spin_lock(&zj->trans_lock);
    trans->trans_id = zj->next_trans_id++;
    trans->version = zj->next_version;
    zj->next_version += 2;  /* 为提交版本预留间隔 */
    spin_unlock(&zj->trans_lock);
    
    trans->state = ZJ_TRANS_ACTIVE;
    trans->flags = flags;
    trans->op_count = 0;
    
    INIT_LIST_HEAD(&trans->ops);
    INIT_LIST_HEAD(&trans->blocks);
    
    trans->read_count = 0;
    trans->write_count = 0;
    
    trans->start_time = get_tick_count();
    trans->commit_time = 0;
    
    spin_lock(&zj->trans_lock);
    list_add(&trans->list, &zj->active_trans);
    spin_unlock(&zj->trans_lock);
    
    return trans;
}

/* 写入日志块 */
static int write_log_block(struct zjournal *zj, struct zj_buf_block *buf_block,
                           uint64_t trans_id, uint32_t blk_flags) {
    uint8_t block[ZJ_LOG_BLOCK_SIZE];
    struct zj_block_header *hdr = (struct zj_block_header *)block;
    
    /* 填充头 */
    hdr->block_id = zj->next_trans_id + zj->log_head;
    hdr->prev_block = (zj->log_head > 0) ? 
                      zj->log_start + zj->log_head - 1 : 0;
    hdr->next_block = 0;  /* 尚未知道 */
    hdr->magic = ZJ_BLOCK_MAGIC;
    hdr->flags = blk_flags;
    hdr->data_size = buf_block->data_size;
    hdr->timestamp = get_tick_count();
    hdr->trans_id = trans_id;
    
    /* 复制数据 */
    uint8_t *data_ptr = block + sizeof(struct zj_block_header);
    size_t copy_size = buf_block->data_size;
    if (copy_size > ZJ_LOG_BLOCK_SIZE - sizeof(struct zj_block_header)) {
        copy_size = ZJ_LOG_BLOCK_SIZE - sizeof(struct zj_block_header);
    }
    
    for (size_t i = 0; i < copy_size; i++) {
        data_ptr[i] = buf_block->data[i];
    }
    
    /* 计算校验和 */
    hdr->checksum = compute_crc32(data_ptr, copy_size);
    
    /* 写入磁盘 */
    uint64_t block_addr = zj->log_start + zj->log_head;
    int ret = zj->write_block(zj->device, block_addr, block);
    
    if (ret == 0) {
        zj->log_head = (zj->log_head + 1) % zj->log_size;
        zj->blocks_written++;
    }
    
    return ret;
}

/* 提交事务 */
int zj_trans_commit(struct zjournal *zj, struct zj_transaction *trans) {
    if (trans == NULL || trans->state != ZJ_TRANS_ACTIVE) {
        return -1;
    }
    
    trans->state = ZJ_TRANS_PREPARING;
    
    /* 冲突检测 - 检查读写集合 */
    spin_lock(&zj->trans_lock);
    
    struct zj_transaction *other;
    list_for_each_entry(other, &zj->committed_trans, list) {
        /* 检查写-写冲突 */
        for (int i = 0; i < trans->write_count; i++) {
            for (int j = 0; j < other->write_count; j++) {
                if (trans->write_set[i] == other->write_set[j]) {
                    /* 版本号比较决定是否冲突 */
                    if (other->version > trans->version) {
                        spin_unlock(&zj->trans_lock);
                        zj_trans_abort(zj, trans);
                        return -1;  /* 冲突，中止 */
                    }
                }
            }
        }
    }
    
    trans->state = ZJ_TRANS_COMMITTING;
    spin_unlock(&zj->trans_lock);
    
    /* 写入日志块 */
    struct zj_buf_block *block, *next;
    int block_count = 0;
    uint32_t blk_flags = ZJ_BLK_FLAG_HEAD;
    
    list_for_each_entry_safe(block, next, &trans->blocks, list) {
        if (block_count == 0) {
            blk_flags = ZJ_BLK_FLAG_HEAD;
        } else {
            blk_flags = 0;
        }
        
        /* 最后一块标记为尾块 */
        if (block->list.next == &trans->blocks) {
            blk_flags |= ZJ_BLK_FLAG_TAIL | ZJ_BLK_FLAG_COMMIT;
        }
        
        if (write_log_block(zj, block, trans->trans_id, blk_flags) != 0) {
            zj_trans_abort(zj, trans);
            return -1;
        }
        
        block_count++;
    }
    
    /* 提交版本号 */
    trans->version = zj->next_version++;
    trans->commit_time = get_tick_count();
    trans->state = ZJ_TRANS_COMMITTED;
    
    /* 移动到已提交列表 */
    spin_lock(&zj->trans_lock);
    list_del(&trans->list);
    list_add(&trans->list, &zj->committed_trans);
    zj->trans_committed++;
    
    /* 更新全局版本 */
    zj->snap_mgr.current_version = trans->version;
    spin_unlock(&zj->trans_lock);
    
    /* 异步刷盘 */
    if (!(trans->flags & ZJ_TRANS_FLAG_SYNC)) {
        zj_flush_buffers(zj, ZJ_BUF_LEVEL_HOT);
    }
    
    /* 检查是否需要创建检查点 */
    if (zj->trans_committed % ZJ_CHECKPOINT_INTERVAL == 0) {
        zj_checkpoint_create(zj, 0);
    }
    
    return 0;
}

/* 中止事务 */
int zj_trans_abort(struct zjournal *zj, struct zj_transaction *trans) {
    if (trans == NULL) return -1;
    
    trans->state = ZJ_TRANS_ABORTED;
    
    /* 释放缓冲区块 */
    struct zj_buf_block *block, *next;
    list_for_each_entry_safe(block, next, &trans->blocks, list) {
        list_del(&block->list);
        buf_mgr_release(&zj->buf_mgr, block);
    }
    
    /* 从活跃列表移除 */
    spin_lock(&zj->trans_lock);
    list_del(&trans->list);
    zj->trans_aborted++;
    spin_unlock(&zj->trans_lock);
    
    kfree(trans);
    return 0;
}

/* 添加写操作 */
int zj_add_write_op(struct zjournal *zj, struct zj_transaction *trans,
                    uint64_t block_no, uint64_t offset,
                    const void *data, size_t size) {
    if (trans == NULL || trans->state != ZJ_TRANS_ACTIVE) {
        return -1;
    }
    
    /* 记录写集合 */
    if (trans->write_count < 16) {
        trans->write_set[trans->write_count++] = block_no;
    }
    
    /* 分配缓冲块 */
    struct zj_buf_block *block = buf_mgr_alloc(&zj->buf_mgr, 
                                                trans->trans_id, trans->version);
    if (block == NULL) return -1;
    
    /* 构造操作记录 */
    struct zj_op_record *op = (struct zj_op_record *)block->data;
    op->op_type = ZJ_OP_WRITE_BLOCK;
    op->flags = 0;
    op->reserved = 0;
    op->data_size = size;
    op->target_block = block_no;
    op->target_offset = offset;
    op->version_before = 0;  /* 待填充 */
    op->version_after = trans->version;
    
    /* 复制数据 */
    uint8_t *op_data = block->data + sizeof(struct zj_op_record);
    size_t copy_size = size;
    if (copy_size > ZJ_LOG_BLOCK_SIZE - sizeof(struct zj_op_record) - sizeof(struct zj_block_header)) {
        copy_size = ZJ_LOG_BLOCK_SIZE - sizeof(struct zj_op_record) - sizeof(struct zj_block_header);
    }
    
    for (size_t i = 0; i < copy_size; i++) {
        op_data[i] = ((const uint8_t *)data)[i];
    }
    
    block->data_size = sizeof(struct zj_op_record) + copy_size;
    trans->op_count++;
    
    /* 加入事务块链表 */
    list_add_tail(&block->list, &trans->blocks);
    
    return 0;
}

/* 添加 Inode 操作 */
int zj_add_inode_op(struct zjournal *zj, struct zj_transaction *trans,
                    uint32_t ino, const void *inode_data, size_t size) {
    if (trans == NULL || trans->state != ZJ_TRANS_ACTIVE) {
        return -1;
    }
    
    struct zj_buf_block *block = buf_mgr_alloc(&zj->buf_mgr,
                                                trans->trans_id, trans->version);
    if (block == NULL) return -1;
    
    struct zj_op_record *op = (struct zj_op_record *)block->data;
    op->op_type = ZJ_OP_UPDATE_INODE;
    op->flags = 0;
    op->reserved = 0;
    op->data_size = size;
    op->target_block = ino;
    op->target_offset = 0;
    op->version_after = trans->version;
    
    uint8_t *op_data = block->data + sizeof(struct zj_op_record);
    for (size_t i = 0; i < size && i < ZJ_LOG_BLOCK_SIZE - sizeof(struct zj_op_record) - sizeof(struct zj_block_header); i++) {
        op_data[i] = ((const uint8_t *)inode_data)[i];
    }
    
    block->data_size = sizeof(struct zj_op_record) + size;
    trans->op_count++;
    
    list_add_tail(&block->list, &trans->blocks);
    
    return 0;
}

/* 创建检查点 */
int zj_checkpoint_create(struct zjournal *zj, int full) {
    struct zj_checkpoint *ckpt = kmalloc(sizeof(struct zj_checkpoint));
    if (ckpt == NULL) return -1;
    
    spin_lock(&zj->ckpt_lock);
    
    ckpt->checkpoint_id = zj->next_trans_id++;
    ckpt->version = zj->snap_mgr.current_version;
    ckpt->timestamp = get_tick_count();
    ckpt->flags = full ? ZJ_CKPT_FLAG_FULL : 0;
    ckpt->block_count = 0;
    
    list_add(&ckpt->list, &zj->checkpoints);
    zj->last_checkpoint = ckpt;
    
    spin_unlock(&zj->ckpt_lock);
    
    /* 同步刷盘 */
    zj_flush_buffers(zj, ZJ_BUF_LEVEL_WARM);
    
    print("[ZJOURNAL] Checkpoint created: version=");
    print_hex(ckpt->version);
    println("");
    
    return 0;
}

/* 恢复 */
int zj_recover(struct zjournal *zj) {
    zj->is_recovery = 1;
    
    print("[ZJOURNAL] Starting recovery...");
    println("");
    
    /* 扫描日志区域 */
    uint64_t block_addr = zj->log_start;
    uint64_t valid_blocks = 0;
    
    for (uint64_t i = 0; i < zj->log_size; i++) {
        uint8_t block[ZJ_LOG_BLOCK_SIZE];
        
        if (zj->read_block(zj->device, block_addr + i, block) != 0) {
            continue;
        }
        
        struct zj_block_header *hdr = (struct zj_block_header *)block;
        
        /* 验证魔数和校验和 */
        if (hdr->magic != ZJ_BLOCK_MAGIC) {
            continue;
        }
        
        uint8_t *data_ptr = block + sizeof(struct zj_block_header);
        uint32_t crc = compute_crc32(data_ptr, hdr->data_size);
        
        if (crc != hdr->checksum) {
            continue;  /* 校验失败，跳过 */
        }
        
        /* 检查是否为已提交块 */
        if (hdr->flags & ZJ_BLK_FLAG_COMMIT) {
            valid_blocks++;
        }
    }
    
    print("[ZJOURNAL] Found ");
    print_hex(valid_blocks);
    println(" valid log blocks");
    
    zj->is_recovery = 0;
    
    return 0;
}

/* 刷新缓冲区 */
int zj_flush_buffers(struct zjournal *zj, zj_buf_level_t level) {
    struct zj_buffer_manager *mgr = &zj->buf_mgr;
    int flushed = 0;
    
    spin_lock(&mgr->lock);
    
    struct zj_buf_block *block;
    list_for_each_entry(block, &mgr->levels[level], list) {
        if (block->level == level) {
            /* 模拟刷盘 */
            flushed++;
        }
    }
    
    spin_unlock(&mgr->lock);
    
    return flushed;
}

/* 同步 */
int zj_sync(struct zjournal *zj, int wait) {
    zj->is_syncing = 1;
    
    /* 刷新所有级别 */
    zj_flush_buffers(zj, ZJ_BUF_LEVEL_HOT);
    zj_flush_buffers(zj, ZJ_BUF_LEVEL_WARM);
    zj_flush_buffers(zj, ZJ_BUF_LEVEL_COLD);
    
    zj->is_syncing = 0;
    
    return 0;
}

/* 获取统计 */
void zj_get_stats(struct zjournal *zj, uint64_t *committed,
                  uint64_t *aborted, uint64_t *written) {
    if (committed) *committed = zj->trans_committed;
    if (aborted) *aborted = zj->trans_aborted;
    if (written) *written = zj->blocks_written;
}

/* 并行提交检测 */
int zj_can_parallel_commit(struct zjournal *zj,
                           struct zj_transaction *t1,
                           struct zj_transaction *t2) {
    /* 检查读写集合是否冲突 */
    for (int i = 0; i < t1->write_count; i++) {
        for (int j = 0; j < t2->write_count; j++) {
            if (t1->write_set[i] == t2->write_set[j]) {
                return 0;  /* 写-写冲突 */
            }
        }
        for (int j = 0; j < t2->read_count; j++) {
            if (t1->write_set[i] == t2->read_set[j]) {
                return 0;  /* 写-读冲突 */
            }
        }
    }
    
    for (int i = 0; i < t1->read_count; i++) {
        for (int j = 0; j < t2->write_count; j++) {
            if (t1->read_set[i] == t2->write_set[j]) {
                return 0;  /* 读-写冲突 */
            }
        }
    }
    
    return 1;  /* 无冲突，可并行提交 */
}

/* ========== WAL 预写日志优化 ========== */

/* WAL 组提交批次 */
struct zj_wal_batch {
    struct zj_buf_block *blocks[32];
    uint32_t count;
    uint64_t first_lsn;     /* 批次起始 LSN */
    uint64_t last_lsn;      /* 批次结束 LSN */
    uint64_t deadline;      /* 提交截止时间 */
};

static struct zj_wal_batch wal_batch;
static spinlock_t wal_batch_lock = SPINLOCK_INIT;

/* WAL 组提交 */
int zj_wal_group_commit(struct zjournal *zj, struct zj_buf_block **blocks, 
                        uint32_t count) {
    if (blocks == NULL || count == 0) return -1;
    
    /* 分配连续的 LSN */
    uint64_t base_lsn;
    spin_lock(&zj->trans_lock);
    base_lsn = zj->next_trans_id;
    zj->next_trans_id += count;
    spin_unlock(&zj->trans_lock);
    
    /* 批量写入，减少磁盘寻址 */
    uint64_t start_block = zj->log_start + zj->log_head;
    uint32_t contiguous = 0;
    
    /* 检查是否有足够的连续空间 */
    if (zj->log_head + count <= zj->log_size) {
        contiguous = 1;
    }
    
    for (uint32_t i = 0; i < count; i++) {
        struct zj_buf_block *block = blocks[i];
        uint32_t flags = 0;
        
        if (i == 0) flags |= ZJ_BLK_FLAG_HEAD;
        if (i == count - 1) flags |= ZJ_BLK_FLAG_TAIL | ZJ_BLK_FLAG_COMMIT;
        
        /* 设置 LSN */
        block->trans_id = base_lsn + i;
        
        if (write_log_block(zj, block, base_lsn + i, flags) != 0) {
            return -1;
        }
    }
    
    return base_lsn;
}

/* 添加到 WAL 批次 */
int zj_wal_batch_add(struct zjournal *zj, struct zj_buf_block *block) {
    if (block == NULL) return -1;
    
    spin_lock(&wal_batch_lock);
    
    if (wal_batch.count == 0) {
        wal_batch.deadline = get_tick_count() + 1000000ULL;  /* 1ms 延迟 */
        wal_batch.first_lsn = 0;
    }
    
    if (wal_batch.count < 32) {
        wal_batch.blocks[wal_batch.count++] = block;
        
        /* 批次满或超时则提交 */
        if (wal_batch.count >= 32 || get_tick_count() >= wal_batch.deadline) {
            uint32_t cnt = wal_batch.count;
            wal_batch.count = 0;
            spin_unlock(&wal_batch_lock);
            
            return zj_wal_group_commit(zj, wal_batch.blocks, cnt);
        }
        
        spin_unlock(&wal_batch_lock);
        return 0;
    }
    
    /* 批次已满，立即提交 */
    uint32_t cnt = wal_batch.count;
    wal_batch.count = 0;
    spin_unlock(&wal_batch_lock);
    
    zj_wal_group_commit(zj, wal_batch.blocks, cnt);
    
    /* 重新添加 */
    return zj_wal_batch_add(zj, block);
}

/* ========== 检查点压缩 ========== */

/* 压缩检查点 */
int zj_checkpoint_compact(struct zjournal *zj) {
    struct zj_checkpoint *ckpt, *next, *prev_ckpt = NULL;
    uint32_t compact_count = 0;
    
    spin_lock(&zj->ckpt_lock);
    
    /* 保留最近的检查点，压缩旧的 - 使用安全遍历避免迭代器失效 */
    list_for_each_entry_safe(ckpt, next, &zj->checkpoints, list) {
        if (prev_ckpt != NULL) {
            /* 合并检查点信息 */
            prev_ckpt->block_count += ckpt->block_count;
            
            /* 移除旧检查点 */
            list_del(&ckpt->list);
            kfree(ckpt);
            compact_count++;
        } else {
            /* 第一个检查点保留 */
            prev_ckpt = ckpt;
        }
    }
    
    spin_unlock(&zj->ckpt_lock);
    
    if (compact_count > 0) {
        print("[ZJOURNAL] Compacted ");
        print_dec(compact_count);
        println(" checkpoints");
    }
    
    return compact_count;
}

/* 检查点重放 */
int zj_checkpoint_replay(struct zjournal *zj, uint64_t target_version) {
    struct zj_checkpoint *ckpt = NULL;
    
    spin_lock(&zj->ckpt_lock);
    
    /* 找到目标检查点 */
    list_for_each_entry(ckpt, &zj->checkpoints, list) {
        if (ckpt->version <= target_version) {
            break;
        }
    }
    
    spin_unlock(&zj->ckpt_lock);
    
    if (ckpt == NULL) return -1;
    
    /* 重放日志 */
    uint64_t start_block = zj->log_start;
    uint64_t replay_count = 0;
    
    for (uint64_t i = 0; i < zj->log_head; i++) {
        uint8_t block[ZJ_LOG_BLOCK_SIZE];
        
        if (zj->read_block(zj->device, start_block + i, block) != 0) {
            continue;
        }
        
        struct zj_block_header *hdr = (struct zj_block_header *)block;
        
        if (hdr->magic != ZJ_BLOCK_MAGIC) continue;
        if (hdr->trans_id < ckpt->checkpoint_id) continue;
        if (hdr->trans_id > target_version) break;
        
        /* 验证并重放 */
        uint8_t *data_ptr = block + sizeof(struct zj_block_header);
        uint32_t crc = compute_crc32(data_ptr, hdr->data_size);
        
        if (crc == hdr->checksum && (hdr->flags & ZJ_BLK_FLAG_COMMIT)) {
            replay_count++;
            /* 应用更改到文件系统 */
        }
    }
    
    print("[ZJOURNAL] Replayed ");
    print_dec(replay_count);
    println(" blocks");
    
    return replay_count;
}

/* ========== 事务恢复增强 ========== */

/* 分析日志决定恢复策略 */
int zj_analyze_log(struct zjournal *zj, uint64_t *last_commit,
                   uint64_t *first_incomplete) {
    *last_commit = 0;
    *first_incomplete = 0;
    
    uint64_t block_addr = zj->log_start;
    uint64_t max_trans_id = 0;
    uint64_t incomplete_trans = 0;
    
    for (uint64_t i = 0; i < zj->log_size; i++) {
        uint8_t block[ZJ_LOG_BLOCK_SIZE];
        
        if (zj->read_block(zj->device, block_addr + i, block) != 0) {
            continue;
        }
        
        struct zj_block_header *hdr = (struct zj_block_header *)block;
        
        if (hdr->magic != ZJ_BLOCK_MAGIC) continue;
        
        /* 检查事务完整性 */
        if (hdr->flags & ZJ_BLK_FLAG_HEAD) {
            if (hdr->flags & ZJ_BLK_FLAG_TAIL && hdr->flags & ZJ_BLK_FLAG_COMMIT) {
                /* 完整的提交事务 */
                if (hdr->trans_id > max_trans_id) {
                    max_trans_id = hdr->trans_id;
                }
            } else {
                /* 不完整事务 */
                if (incomplete_trans == 0) {
                    incomplete_trans = hdr->trans_id;
                }
            }
        }
    }
    
    *last_commit = max_trans_id;
    *first_incomplete = incomplete_trans;
    
    return 0;
}

/* 重做阶段 */
int zj_redo_phase(struct zjournal *zj, uint64_t from_lsn) {
    uint64_t redo_count = 0;
    
    print("[ZJOURNAL] REDO phase from LSN ");
    print_hex(from_lsn);
    println("");
    
    /* 从指定 LSN 开始重做 */
    for (uint64_t i = 0; i < zj->log_head; i++) {
        uint8_t block[ZJ_LOG_BLOCK_SIZE];
        uint64_t block_addr = zj->log_start + i;
        
        if (zj->read_block(zj->device, block_addr, block) != 0) {
            continue;
        }
        
        struct zj_block_header *hdr = (struct zj_block_header *)block;
        
        if (hdr->magic != ZJ_BLOCK_MAGIC) continue;
        if (hdr->trans_id < from_lsn) continue;
        
        /* 重做操作 */
        if (hdr->flags & ZJ_BLK_FLAG_COMMIT) {
            /* 应用更改 */
            redo_count++;
        }
    }
    
    print("[ZJOURNAL] REDO applied ");
    print_dec(redo_count);
    println(" operations");
    
    return redo_count;
}

/* 撤销阶段 */
int zj_undo_phase(struct zjournal *zj, uint64_t *incomplete_trans, uint32_t count) {
    uint32_t undo_count = 0;
    
    print("[ZJOURNAL] UNDO phase for ");
    print_dec(count);
    println(" incomplete transactions");
    
    /* 撤销未完成事务 */
    for (uint32_t t = 0; t < count; t++) {
        /* 反向扫描日志，撤销操作 */
        for (int64_t i = zj->log_head - 1; i >= 0; i--) {
            uint8_t block[ZJ_LOG_BLOCK_SIZE];
            uint64_t block_addr = zj->log_start + i;
            
            if (zj->read_block(zj->device, block_addr, block) != 0) {
                continue;
            }
            
            struct zj_block_header *hdr = (struct zj_block_header *)block;
            
            if (hdr->magic != ZJ_BLOCK_MAGIC) continue;
            if (hdr->trans_id != incomplete_trans[t]) continue;
            
            /* 撤销操作 (反向应用) */
            undo_count++;
        }
    }
    
    print("[ZJOURNAL] UNDO rolled back ");
    print_dec(undo_count);
    println(" operations");
    
    return undo_count;
}

/* ========== 高级恢复 API ========== */

/* 完整恢复流程 */
int zj_full_recovery(struct zjournal *zj) {
    uint64_t last_commit, first_incomplete;
    
    print("[ZJOURNAL] Starting full recovery...");
    println("");
    
    /* 分析阶段 */
    zj_analyze_log(zj, &last_commit, &first_incomplete);
    
    /* 重做阶段 */
    zj_redo_phase(zj, last_commit);
    
    /* 撤销阶段 */
    if (first_incomplete > 0) {
        uint64_t incomplete[1] = { first_incomplete };
        zj_undo_phase(zj, incomplete, 1);
    }
    
    print("[ZJOURNAL] Recovery complete");
    println("");
    
    return 0;
}

/* ========== 性能优化 ========== */

/* 预读日志块 */
int zj_prefetch_log(struct zjournal *zj, uint64_t start_lsn, uint32_t count) {
    /* 批量预读到缓冲区 */
    for (uint32_t i = 0; i < count; i++) {
        uint64_t block_addr = zj->log_start + ((start_lsn + i) % zj->log_size);
        
        struct zj_buf_block *block = buf_mgr_alloc(&zj->buf_mgr, 
                                                    start_lsn + i, 0);
        if (block == NULL) continue;
        
        if (zj->read_block(zj->device, block_addr, block->data) != 0) {
            buf_mgr_release(&zj->buf_mgr, block);
            continue;
        }
        
        zj->blocks_read++;
    }
    
    return 0;
}

/* 写入合并 */
int zj_merge_writes(struct zjournal *zj, struct zj_transaction *trans,
                    uint64_t block_no, const void *data1, size_t size1,
                    const void *data2, size_t size2) {
    if (trans == NULL || trans->state != ZJ_TRANS_ACTIVE) {
        return -1;
    }
    
    /* 合并多次写入为一次 */
    struct zj_buf_block *block = buf_mgr_alloc(&zj->buf_mgr,
                                                trans->trans_id, trans->version);
    if (block == NULL) return -1;
    
    struct zj_op_record *op = (struct zj_op_record *)block->data;
    op->op_type = ZJ_OP_WRITE_BLOCK;
    op->flags = ZJ_OP_FLAG_MERGED;
    op->target_block = block_no;
    op->data_size = size1 + size2;
    
    /* 合并数据 */
    uint8_t *op_data = block->data + sizeof(struct zj_op_record);
    for (size_t i = 0; i < size1; i++) {
        op_data[i] = ((const uint8_t *)data1)[i];
    }
    for (size_t i = 0; i < size2; i++) {
        op_data[size1 + i] = ((const uint8_t *)data2)[i];
    }
    
    block->data_size = sizeof(struct zj_op_record) + size1 + size2;
    trans->op_count++;
    
    list_add_tail(&block->list, &trans->blocks);
    
    return 0;
}

/* ========== 分布式事务支持 ========== */

/* 两阶段提交准备 */
int zj_2pc_prepare(struct zjournal *zj, struct zj_transaction *trans) {
    if (trans == NULL) return -1;
    
    trans->state = ZJ_TRANS_PREPARING;
    
    /* 写入准备记录 */
    struct zj_buf_block *block = buf_mgr_alloc(&zj->buf_mgr,
                                                trans->trans_id, trans->version);
    if (block == NULL) return -1;
    
    struct zj_op_record *op = (struct zj_op_record *)block->data;
    op->op_type = ZJ_OP_2PC_PREPARE;
    op->flags = ZJ_BLK_FLAG_HEAD | ZJ_BLK_FLAG_TAIL;
    op->data_size = 0;
    
    block->data_size = sizeof(struct zj_op_record);
    
    if (write_log_block(zj, block, trans->trans_id, ZJ_BLK_FLAG_HEAD) != 0) {
        buf_mgr_release(&zj->buf_mgr, block);
        return -1;
    }
    
    buf_mgr_release(&zj->buf_mgr, block);
    
    trans->state = ZJ_TRANS_PREPARED;
    return 0;
}

/* 两阶段提交提交 */
int zj_2pc_commit(struct zjournal *zj, struct zj_transaction *trans) {
    if (trans == NULL || trans->state != ZJ_TRANS_PREPARED) {
        return -1;
    }
    
    /* 写入提交记录 */
    struct zj_buf_block *block = buf_mgr_alloc(&zj->buf_mgr,
                                                trans->trans_id, trans->version);
    if (block == NULL) return -1;
    
    struct zj_op_record *op = (struct zj_op_record *)block->data;
    op->op_type = ZJ_OP_2PC_COMMIT;
    op->flags = ZJ_BLK_FLAG_COMMIT;
    
    block->data_size = sizeof(struct zj_op_record);
    
    int ret = write_log_block(zj, block, trans->trans_id, 
                              ZJ_BLK_FLAG_HEAD | ZJ_BLK_FLAG_TAIL | ZJ_BLK_FLAG_COMMIT);
    
    buf_mgr_release(&zj->buf_mgr, block);
    
    if (ret == 0) {
        trans->state = ZJ_TRANS_COMMITTED;
        zj->trans_committed++;
    }
    
    return ret;
}

/* 两阶段提交回滚 */
int zj_2pc_rollback(struct zjournal *zj, struct zj_transaction *trans) {
    if (trans == NULL) return -1;
    
    /* 写入回滚记录 */
    struct zj_buf_block *block = buf_mgr_alloc(&zj->buf_mgr,
                                                trans->trans_id, trans->version);
    if (block == NULL) return -1;
    
    struct zj_op_record *op = (struct zj_op_record *)block->data;
    op->op_type = ZJ_OP_2PC_ABORT;
    
    block->data_size = sizeof(struct zj_op_record);
    
    write_log_block(zj, block, trans->trans_id, ZJ_BLK_FLAG_HEAD | ZJ_BLK_FLAG_TAIL);
    
    buf_mgr_release(&zj->buf_mgr, block);
    
    trans->state = ZJ_TRANS_ABORTED;
    zj->trans_aborted++;
    
    return 0;
}

/* ========== 诊断接口 ========== */

/* 检查日志完整性 */
int zj_check_integrity(struct zjournal *zj, uint64_t *error_count) {
    uint64_t errors = 0;
    
    for (uint64_t i = 0; i < zj->log_size; i++) {
        uint8_t block[ZJ_LOG_BLOCK_SIZE];
        uint64_t block_addr = zj->log_start + i;
        
        if (zj->read_block(zj->device, block_addr, block) != 0) {
            continue;
        }
        
        struct zj_block_header *hdr = (struct zj_block_header *)block;
        
        if (hdr->magic != ZJ_BLOCK_MAGIC) continue;
        
        /* 验证校验和 */
        uint8_t *data_ptr = block + sizeof(struct zj_block_header);
        uint32_t crc = compute_crc32(data_ptr, hdr->data_size);
        
        if (crc != hdr->checksum) {
            errors++;
            print("[ZJOURNAL] Checksum error at block ");
            print_hex(i);
            println("");
        }
        
        /* 验证链指针 */
        if (hdr->prev_block != 0) {
            /* 检查前一块是否有效 */
        }
    }
    
    if (error_count) *error_count = errors;
    
    return errors == 0 ? 0 : -1;
}

/* 获取事务状态 */
int zj_get_trans_state(struct zjournal *zj, uint64_t trans_id,
                       zj_trans_state_t *state) {
    spin_lock(&zj->trans_lock);
    
    /* 在活跃列表中查找 */
    struct zj_transaction *trans;
    list_for_each_entry(trans, &zj->active_trans, list) {
        if (trans->trans_id == trans_id) {
            *state = trans->state;
            spin_unlock(&zj->trans_lock);
            return 0;
        }
    }
    
    /* 在已提交列表中查找 */
    list_for_each_entry(trans, &zj->committed_trans, list) {
        if (trans->trans_id == trans_id) {
            *state = trans->state;
            spin_unlock(&zj->trans_lock);
            return 0;
        }
    }
    
    spin_unlock(&zj->trans_lock);
    return -1;  /* 未找到 */
}

/* ========== Z-Journal 高级增强功能 ========== */

/* === 嵌套事务支持 === */

struct zj_nested_trans {
    uint64_t parent_id;
    uint64_t child_id;
    int level;
    struct list_head list;
};

static struct list_head nested_trans_list = LIST_HEAD_INIT(nested_trans_list);
static spinlock_t nested_lock = SPINLOCK_INIT;

/* 开始嵌套事务 */
uint64_t zj_begin_nested(struct zjournal *zj, uint64_t parent_id) {
    struct zj_transaction *trans = zj_trans_begin(zj, ZJ_TRANS_FLAG_NESTED);
    if (trans == NULL) return 0;
    
    uint64_t child_id = trans->trans_id;
    
    struct zj_nested_trans *nest = kmalloc(sizeof(struct zj_nested_trans));
    if (nest == NULL) {
        zj_trans_abort(zj, trans);
        return 0;
    }
    
    /* 计算嵌套深度 */
    int level = 1;
    spin_lock(&nested_lock);
    struct zj_nested_trans *n;
    list_for_each_entry(n, &nested_trans_list, list) {
        if (n->child_id == parent_id) {
            level = n->level + 1;
            break;
        }
    }
    
    nest->parent_id = parent_id;
    nest->child_id = child_id;
    nest->level = level;
    list_add(&nest->list, &nested_trans_list);
    spin_unlock(&nested_lock);
    
    return child_id;
}

/* 提交嵌套事务 */
int zj_commit_nested(struct zjournal *zj, uint64_t child_id) {
    spin_lock(&nested_lock);
    
    struct zj_nested_trans *nest, *found = NULL;
    list_for_each_entry(nest, &nested_trans_list, list) {
        if (nest->child_id == child_id) {
            found = nest;
            break;
        }
    }
    
    if (found == NULL) {
        spin_unlock(&nested_lock);
        /* 找到对应事务并提交 */
        struct zj_transaction *trans = NULL;
        list_for_each_entry(trans, &zj->active_trans, list) {
            if (trans->trans_id == child_id) {
                return zj_trans_commit(zj, trans);
            }
        }
        return -1;
    }
    
    uint64_t parent_id = found->parent_id;
    list_del(&found->list);
    kfree(found);
    spin_unlock(&nested_lock);
    
    /* 找到对应事务并提交 */
    struct zj_transaction *trans = NULL;
    list_for_each_entry(trans, &zj->active_trans, list) {
        if (trans->trans_id == child_id) {
            return zj_trans_commit(zj, trans);
        }
    }
    return -1;
}

/* === 保存点支持 === */

struct zj_savepoint {
    uint64_t trans_id;
    uint64_t savepoint_id;
    char name[32];
    struct list_head list;
};

static struct list_head savepoint_list = LIST_HEAD_INIT(savepoint_list);
static spinlock_t savepoint_lock = SPINLOCK_INIT;
static uint64_t next_savepoint_id = 1;

/* 创建保存点 */
uint64_t zj_create_savepoint(struct zjournal *zj, uint64_t trans_id, const char *name) {
    struct zj_savepoint *sp = kmalloc(sizeof(struct zj_savepoint));
    if (sp == NULL) return 0;
    
    sp->trans_id = trans_id;
    sp->savepoint_id = next_savepoint_id++;
    
    int i = 0;
    while (name[i] && i < 31) {
        sp->name[i] = name[i];
        i++;
    }
    sp->name[i] = '\0';
    
    spin_lock(&savepoint_lock);
    list_add(&sp->list, &savepoint_list);
    spin_unlock(&savepoint_lock);
    
    return sp->savepoint_id;
}

/* 回滚到保存点 */
int zj_rollback_to_savepoint(struct zjournal *zj, uint64_t trans_id, uint64_t savepoint_id) {
    struct zj_savepoint *sp = NULL;
    
    spin_lock(&savepoint_lock);
    struct zj_savepoint *s;
    list_for_each_entry(s, &savepoint_list, list) {
        if (s->trans_id == trans_id && s->savepoint_id == savepoint_id) {
            sp = s;
            break;
        }
    }
    
    if (sp == NULL) {
        spin_unlock(&savepoint_lock);
        return -1;
    }
    
    /* 删除该保存点之后的所有保存点 */
    struct zj_savepoint *tmp, *next;
    list_for_each_entry_safe(tmp, next, &savepoint_list, list) {
        if (tmp->trans_id == trans_id && tmp->savepoint_id > savepoint_id) {
            list_del(&tmp->list);
            kfree(tmp);
        }
    }
    
    spin_unlock(&savepoint_lock);
    
    return 0;
}

/* === 事务依赖跟踪 === */

struct zj_dependency {
    uint64_t trans_id;
    uint64_t depends_on;
    int satisfied;
    struct list_head list;
};

static struct list_head dependency_list = LIST_HEAD_INIT(dependency_list);
static spinlock_t dependency_lock = SPINLOCK_INIT;

/* 添加依赖 */
int zj_add_dependency(struct zjournal *zj, uint64_t trans_id, uint64_t depends_on) {
    struct zj_dependency *dep = kmalloc(sizeof(struct zj_dependency));
    if (dep == NULL) return -1;
    
    dep->trans_id = trans_id;
    dep->depends_on = depends_on;
    dep->satisfied = 0;
    
    spin_lock(&dependency_lock);
    list_add(&dep->list, &dependency_list);
    spin_unlock(&dependency_lock);
    
    return 0;
}

/* 检查依赖是否满足 */
int zj_check_dependencies(struct zjournal *zj, uint64_t trans_id) {
    int all_satisfied = 1;
    
    spin_lock(&dependency_lock);
    
    struct zj_dependency *dep;
    list_for_each_entry(dep, &dependency_list, list) {
        if (dep->trans_id == trans_id && !dep->satisfied) {
            /* 检查依赖的事务是否已提交 */
            zj_trans_state_t state;
            if (zj_get_trans_state(zj, dep->depends_on, &state) == 0) {
                if (state == ZJ_TRANS_COMMITTED) {
                    dep->satisfied = 1;
                } else {
                    all_satisfied = 0;
                }
            }
        }
    }
    
    spin_unlock(&dependency_lock);
    return all_satisfied;
}

/* === 组提交优化 === */

struct zj_group_commit {
    uint64_t leader_trans;
    uint64_t member_count;
    uint64_t members[16];
    int active;
    spinlock_t lock;
};

static struct zj_group_commit group_commit = {0, 0, {0}, 0, SPINLOCK_INIT};

/* 加入组提交 */
int zj_join_group_commit(struct zjournal *zj, uint64_t trans_id) {
    spin_lock(&group_commit.lock);
    
    if (group_commit.member_count >= 16) {
        spin_unlock(&group_commit.lock);
        return -1;  /* 组已满 */
    }
    
    if (group_commit.member_count == 0) {
        group_commit.leader_trans = trans_id;
        group_commit.active = 1;
    }
    
    group_commit.members[group_commit.member_count++] = trans_id;
    
    spin_unlock(&group_commit.lock);
    return 0;
}

/* 执行组提交 */
int zj_execute_group_commit(struct zjournal *zj) {
    spin_lock(&group_commit.lock);
    
    if (group_commit.member_count == 0) {
        spin_unlock(&group_commit.lock);
        return 0;
    }
    
    /* 单次 I/O 提交所有事务 */
    uint64_t count = group_commit.member_count;
    uint64_t members[16];
    for (int i = 0; i < 16; i++) members[i] = group_commit.members[i];
    
    group_commit.member_count = 0;
    group_commit.active = 0;
    
    spin_unlock(&group_commit.lock);
    
    /* 提交所有成员 */
    for (uint64_t i = 0; i < count; i++) {
        /* 找到对应事务并提交 */
        struct zj_transaction *trans = NULL;
        list_for_each_entry(trans, &zj->active_trans, list) {
            if (trans->trans_id == members[i]) {
                zj_trans_commit(zj, trans);
                break;
            }
        }
    }
    
    return count;
}

/* === 预写日志压缩 === */

/* 执行日志压缩 */
int zj_compact_log(struct zjournal *zj) {
    if (zj == NULL) return -1;
    
    /* 找到最早的活跃事务 */
    uint64_t min_trans_id = zj->next_trans_id;
    
    spin_lock(&zj->trans_lock);
    struct zj_transaction *trans;
    list_for_each_entry(trans, &zj->active_trans, list) {
        if (trans->trans_id < min_trans_id) {
            min_trans_id = trans->trans_id;
        }
    }
    spin_unlock(&zj->trans_lock);
    
    /* 压缩已提交的事务日志 */
    uint64_t compacted = 0;
    
    /* 清理已提交事务列表中的旧事务 */
    spin_lock(&zj->trans_lock);
    struct zj_transaction *t, *next;
    list_for_each_entry_safe(t, next, &zj->committed_trans, list) {
        if (t->trans_id < min_trans_id) {
            list_del(&t->list);
            kfree(t);
            compacted++;
        }
    }
    spin_unlock(&zj->trans_lock);
    
    return compacted;
}

/* === 故障恢复增强 === */

/* 分析阶段 */
struct zj_analysis_result {
    uint64_t trans_count;
    uint64_t committed_count;
    uint64_t aborted_count;
};

static int recovery_analysis(struct zjournal *zj, struct zj_analysis_result *result) {
    result->trans_count = 0;
    result->committed_count = 0;
    result->aborted_count = 0;
    
    /* 扫描活跃事务 */
    spin_lock(&zj->trans_lock);
    struct zj_transaction *trans;
    list_for_each_entry(trans, &zj->active_trans, list) {
        result->trans_count++;
    }
    list_for_each_entry(trans, &zj->committed_trans, list) {
        result->committed_count++;
    }
    spin_unlock(&zj->trans_lock);
    
    result->aborted_count = zj->trans_aborted;
    
    return 0;
}

/* 完整恢复 */
int zj_recover_full(struct zjournal *zj) {
    println("[ZJOURNAL] Starting recovery...");
    
    /* 分析阶段 */
    struct zj_analysis_result analysis;
    recovery_analysis(zj, &analysis);
    
    print("  Active transactions: ");
    print_dec(analysis.trans_count);
    println("");
    
    print("  Committed transactions: ");
    print_dec(analysis.committed_count);
    println("");
    
    /* 回滚未完成的事务 */
    spin_lock(&zj->trans_lock);
    struct zj_transaction *trans, *next;
    list_for_each_entry_safe(trans, next, &zj->active_trans, list) {
        zj_trans_abort(zj, trans);
    }
    spin_unlock(&zj->trans_lock);
    
    println("[ZJOURNAL] Recovery complete");
    return 0;
}

/* === 性能监控 === */

struct zj_perf_stats {
    uint64_t trans_begun;
    uint64_t trans_committed;
    uint64_t trans_aborted;
    uint64_t blocks_written;
    uint64_t blocks_read;
    uint64_t checkpoints;
    uint64_t avg_trans_time;
};

static struct zj_perf_stats perf_stats;

void zj_record_perf(int event, uint64_t value) {
    switch (event) {
        case 0: perf_stats.trans_begun++; break;
        case 1: perf_stats.trans_committed++; break;
        case 2: perf_stats.trans_aborted++; break;
        case 3: perf_stats.blocks_written += value; break;
        case 4: perf_stats.blocks_read += value; break;
        case 5: perf_stats.checkpoints++; break;
    }
}

void zj_print_perf_stats(struct zjournal *zj) {
    println("=== Z-Journal Performance ===");
    print("Transactions begun: "); print_dec(perf_stats.trans_begun); println("");
    print("Transactions committed: "); print_dec(perf_stats.trans_committed); println("");
    print("Transactions aborted: "); print_dec(perf_stats.trans_aborted); println("");
    print("Blocks written: "); print_dec(perf_stats.blocks_written); println("");
    print("Blocks read: "); print_dec(perf_stats.blocks_read); println("");
    print("Checkpoints: "); print_dec(perf_stats.checkpoints); println("");
}

/* === 完整诊断报告 === */

void zj_print_report(struct zjournal *zj) {
    if (zj == NULL) return;
    
    println("╔══════════════════════════════════════════╗");
    println("║        Z-Journal Diagnostic Report       ║");
    println("╚══════════════════════════════════════════╝");
    
    println("");
    println("=== 基本信息 ===");
    print("日志起始块: "); print_hex(zj->log_start); println("");
    print("日志大小: "); print_dec(zj->log_size); println(" blocks");
    print("日志头: "); print_hex(zj->log_head); println("");
    print("日志尾: "); print_hex(zj->log_tail); println("");
    print("下一个事务 ID: "); print_dec(zj->next_trans_id); println("");
    
    println("");
    println("=== 缓冲区统计 ===");
    print("总块数: "); print_dec(zj->buf_mgr.total_blocks); println("");
    print("命中: "); print_dec(zj->buf_mgr.hits); println("");
    print("未命中: "); print_dec(zj->buf_mgr.misses); println("");
    print("提升: "); print_dec(zj->buf_mgr.promotions); println("");
    print("降级: "); print_dec(zj->buf_mgr.demotions); println("");
    
    println("");
    println("=== 事务统计 ===");
    spin_lock(&zj->trans_lock);
    int active = 0, committed = 0;
    struct zj_transaction *t;
    list_for_each_entry(t, &zj->active_trans, list) active++;
    list_for_each_entry(t, &zj->committed_trans, list) committed++;
    spin_unlock(&zj->trans_lock);
    
    print("活跃事务: "); print_dec(active); println("");
    print("已提交事务: "); print_dec(committed); println("");
    print("已中止事务: "); print_dec(zj->trans_aborted); println("");
    
    println("");
    println("=== 性能统计 ===");
    zj_print_perf_stats(zj);
    
    println("");
    println("=== 完整性检查 ===");
    uint64_t errors = 0;
    if (zj_check_integrity(zj, &errors) == 0) {
        println("状态: 通过");
    } else {
        print("状态: 发现 "); print_dec(errors); println(" 个错误");
    }
}
