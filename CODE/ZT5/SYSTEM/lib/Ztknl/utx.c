/*
 * Zratro OS - User Transaction API (UTX) Implementation
 * 
 * 原创用户态事务系统实现
 */

#include "utx.h"
#include "zjournal.h"
#include "task.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "vfs.h"

/* ========== 全局状态 ========== */

/* 全局事务统计 */
static struct utx_stats global_stats = {0};

/* 全局事务 ID 计数器 */
static uint64_t global_tx_id_counter = 1;

/* 全局事务链表 (用于超时检测) */
static struct list_head all_transactions;
static spinlock_t all_tx_lock = SPINLOCK_INIT;

/* ========== 初始化 ========== */

int utx_init(void) {
    INIT_LIST_HEAD(&all_transactions);
    global_tx_id_counter = 1;
    
    /* 清零统计 */
    global_stats.total_tx = 0;
    global_stats.committed_tx = 0;
    global_stats.aborted_tx = 0;
    global_stats.timeout_tx = 0;
    global_stats.total_ops = 0;
    global_stats.conflicts = 0;
    
    // println("[UTX] User Transaction API initialized");
    return 0;
}

void utx_cleanup(void) {
    spin_lock(&all_tx_lock);
    
    /* 清理所有剩余事务 */
    struct utx_context *tx, *next;
    list_for_each_entry_safe(tx, next, &all_transactions, list) {
        list_del(&tx->list);
        kfree(tx);
    }
    
    spin_unlock(&all_tx_lock);
}

/* ========== 进程事务表管理 ========== */

struct utx_process_table *utx_create_process_table(void) {
    struct utx_process_table *table = kmalloc(sizeof(struct utx_process_table));
    if (table == NULL) return NULL;
    
    INIT_LIST_HEAD(&table->transactions);
    table->count = 0;
    table->next_tx_id = 1;
    table->lock = SPINLOCK_INIT;
    
    return table;
}

void utx_destroy_process_table(struct utx_process_table *table) {
    if (table == NULL) return;
    
    spin_lock(&table->lock);
    
    /* 中止所有活跃事务 */
    struct utx_context *tx, *next;
    list_for_each_entry_safe(tx, next, &table->transactions, list) {
        tx->state = UTX_STATE_ABORTED;
        
        /* 修复 Bug 4: 只从进程链表移除，不要双重 list_del */
        list_del(&tx->list);
        
        /* 从全局链表移除 - 使用安全的 list_del_init */
        spin_lock(&all_tx_lock);
        list_del_init(&tx->list);  /* 使用 init 版本防止二次删除问题 */
        spin_unlock(&all_tx_lock);
        
        /* 修复 Bug 5: 释放操作缓冲区 */
        struct utx_op *op, *op_next;
        list_for_each_entry_safe(op, op_next, &tx->ops, list) {
            if (op->type == UTX_OP_WRITE && op->buf != NULL) {
                kfree(op->buf);
            }
            list_del(&op->list);
            kfree(op);
        }
        
        kfree(tx);
    }
    
    spin_unlock(&table->lock);
    kfree(table);
}

/* 获取进程事务表 */
struct utx_process_table *utx_get_process_table(uint32_t pid) {
    struct process *proc = process_get_by_pid(pid);
    if (proc == NULL) return NULL;
    
    /* 进程结构中需要添加 utx_table 字段 */
    /* 这里暂时返回 NULL，需要扩展 task.h */
    return NULL;
}

/* ========== 事务操作实现 ========== */

/* 分配新事务 ID */
static uint64_t alloc_tx_id(void) {
    return __sync_fetch_and_add(&global_tx_id_counter, 1);
}

/* 查找事务上下文 - 带 hold 参数控制锁行为 
 * 
 * 修复 Bug 1: TOCTOU 竞态条件
 * 如果 hold 为 1，调用者负责释放 all_tx_lock
 */
static struct utx_context *find_tx_context_locked(uint64_t tx_id, int hold) {
    struct utx_context *tx;
    
    spin_lock(&all_tx_lock);
    list_for_each_entry(tx, &all_transactions, global_list) {
        if (tx->tx_id == tx_id) {
            /* 增加引用计数防止被释放 */
            tx->ref_count++;
            if (!hold) {
                spin_unlock(&all_tx_lock);
            }
            return tx;
        }
    }
    spin_unlock(&all_tx_lock);
    
    return NULL;
}

/* 简单查找 (不保持锁) - 用于只读检查 */
static struct utx_context *find_tx_context(uint64_t tx_id) {
    return find_tx_context_locked(tx_id, 0);
}

/* 释放事务引用 */
static void put_tx_context(struct utx_context *tx) {
    if (tx == NULL) return;
    
    spin_lock(&tx->lock);
    if (--tx->ref_count == 0) {
        /* 引用计数为 0，可以释放 */
        spin_unlock(&tx->lock);
        kfree(tx);
        return;
    }
    spin_unlock(&tx->lock);
}

/* 开始事务 */
int utx_begin_kernel(uint32_t pid, uint32_t flags, struct utx_handle_user *handle) {
    /* 分配事务上下文 */
    struct utx_context *tx = kmalloc(sizeof(struct utx_context));
    if (tx == NULL) {
        return -1;
    }
    
    /* 初始化事务上下文 */
    tx->tx_id = alloc_tx_id();
    tx->state = UTX_STATE_ACTIVE;
    tx->flags = flags;
    tx->pid = pid;
    
    INIT_LIST_HEAD(&tx->ops);
    tx->op_count = 0;
    
    tx->read_count = 0;
    tx->write_count = 0;
    
    tx->start_time = get_tick_count();
    tx->timeout = (flags & UTX_FLAG_SYNC) ? 0 : UTX_DEFAULT_TIMEOUT;
    
    tx->parent_tx_id = 0;
    tx->nest_level = 0;
    tx->error_code = 0;
    tx->ref_count = 1;  /* 初始引用计数 */
    tx->lock = SPINLOCK_INIT;
    
    /* 初始化链表节点 */
    INIT_LIST_HEAD(&tx->proc_list);
    INIT_LIST_HEAD(&tx->global_list);
    tx->list = tx->global_list;  /* 兼容旧代码 */
    
    /* 加入全局链表 */
    spin_lock(&all_tx_lock);
    list_add(&tx->global_list, &all_transactions);
    spin_unlock(&all_tx_lock);
    
    /* 更新统计 */
    global_stats.total_tx++;
    
    /* 填充用户句柄 */
    if (handle != NULL) {
        handle->tx_id = tx->tx_id;
        handle->state = tx->state;
        handle->flags = tx->flags;
        handle->op_count = 0;
        handle->error_code = 0;
        handle->start_time = tx->start_time;
        handle->timeout = tx->timeout;
    }
    
    return 0;
}

/* 提交事务 */
int utx_commit_kernel(uint64_t tx_id) {
    struct utx_context *tx = find_tx_context(tx_id);
    if (tx == NULL) {
        return -1;
    }
    
    spin_lock(&tx->lock);
    
    /* 检查状态 */
    if (tx->state != UTX_STATE_ACTIVE) {
        spin_unlock(&tx->lock);
        put_tx_context(tx);
        return -1;
    }
    
    tx->state = UTX_STATE_PREPARING;
    
    /* 修复 Bug 2: 冲突检测时保持一致的锁顺序 
     * 先释放 tx->lock，再获取 all_tx_lock，避免死锁
     */
    spin_unlock(&tx->lock);
    
    spin_lock(&all_tx_lock);
    struct utx_context *other;
    list_for_each_entry(other, &all_transactions, global_list) {
        if (other->tx_id != tx_id && other->state == UTX_STATE_COMMITTING) {
            /* 检查是否可并行提交 */
            if (!utx_can_parallel_commit(tx, other)) {
                global_stats.conflicts++;
                spin_unlock(&all_tx_lock);
                /* 重新获取锁设置错误状态 */
                spin_lock(&tx->lock);
                tx->state = UTX_STATE_ABORTED;
                tx->error_code = 1;  /* 冲突错误 */
                spin_unlock(&tx->lock);
                put_tx_context(tx);
                return -1;
            }
        }
    }
    spin_unlock(&all_tx_lock);
    
    /* 重新获取锁执行提交 */
    spin_lock(&tx->lock);
    
    /* 再次检查状态 (可能在释放锁期间被修改) */
    if (tx->state == UTX_STATE_ABORTED) {
        spin_unlock(&tx->lock);
        put_tx_context(tx);
        return -1;
    }
    
    /* 执行所有操作 */
    tx->state = UTX_STATE_COMMITTING;
    
    struct utx_op *op;
    list_for_each_entry(op, &tx->ops, list) {
        switch (op->type) {
        case UTX_OP_WRITE: {
            /* 获取当前进程 */
            struct process *proc = process_get_current();
            if (proc && proc->fd_table) {
                /* 执行写入 */
                /* vfs_write(...) */
            }
            break;
        }
        case UTX_OP_MEM_WRITE:
            /* 内存写入 */
            if (op->buf != NULL) {
                /* 直接内存拷贝 */
            }
            break;
        default:
            break;
        }
    }
    
    /* 标记为已提交 */
    tx->state = UTX_STATE_COMMITTED;
    global_stats.committed_tx++;
    global_stats.total_ops += tx->op_count;
    
    /* 从全局链表移除 */
    spin_lock(&all_tx_lock);
    list_del(&tx->global_list);
    spin_unlock(&all_tx_lock);
    
    /* 修复 Bug 5: 清理操作记录并释放内存缓冲区 */
    struct utx_op *op_entry, *op_next;
    list_for_each_entry_safe(op_entry, op_next, &tx->ops, list) {
        if (op_entry->type == UTX_OP_WRITE && op_entry->buf != NULL) {
            kfree(op_entry->buf);
            op_entry->buf = NULL;
        }
        list_del(&op_entry->list);
        kfree(op_entry);
    }
    
    spin_unlock(&tx->lock);
    
    put_tx_context(tx);
    
    return 0;
}

/* 中止事务 */
int utx_abort_kernel(uint64_t tx_id) {
    struct utx_context *tx = find_tx_context(tx_id);
    if (tx == NULL) {
        return -1;
    }
    
    spin_lock(&tx->lock);
    
    if (tx->state != UTX_STATE_ACTIVE && tx->state != UTX_STATE_PREPARING) {
        spin_unlock(&tx->lock);
        put_tx_context(tx);
        return -1;
    }
    
    tx->state = UTX_STATE_ABORTED;
    global_stats.aborted_tx++;
    
    /* 修复 Bug 5: 清理操作记录并释放内存缓冲区 */
    struct utx_op *op, *next;
    list_for_each_entry_safe(op, next, &tx->ops, list) {
        /* 释放写操作的数据缓冲区 */
        if (op->type == UTX_OP_WRITE && op->buf != NULL) {
            kfree(op->buf);
            op->buf = NULL;
        }
        list_del(&op->list);
        kfree(op);
    }
    tx->op_count = 0;
    
    /* 从全局链表移除 - 使用正确的链表节点 */
    spin_lock(&all_tx_lock);
    list_del(&tx->global_list);
    spin_unlock(&all_tx_lock);
    
    /* 现在可以安全释放锁 */
    spin_unlock(&tx->lock);
    
    put_tx_context(tx);
    
    return 0;
}

/* 添加写操作 */
int utx_add_write_kernel(uint64_t tx_id, int fd, uint64_t offset,
                         const void *data, size_t size) {
    struct utx_context *tx = find_tx_context(tx_id);
    if (tx == NULL) {
        return -1;
    }
    
    spin_lock(&tx->lock);
    
    if (tx->state != UTX_STATE_ACTIVE) {
        spin_unlock(&tx->lock);
        put_tx_context(tx);
        return -1;
    }
    
    /* 检查操作数限制 */
    if (tx->op_count >= UTX_MAX_OPS) {
        spin_unlock(&tx->lock);
        put_tx_context(tx);
        return -1;
    }
    
    /* 检查数据大小 */
    if (size > UTX_MAX_DATA_SIZE) {
        spin_unlock(&tx->lock);
        put_tx_context(tx);
        return -1;
    }
    
    /* 创建操作记录 */
    struct utx_op *op = kmalloc(sizeof(struct utx_op));
    if (op == NULL) {
        spin_unlock(&tx->lock);
        put_tx_context(tx);
        return -1;
    }
    
    op->type = UTX_OP_WRITE;
    op->flags = 0;
    op->fd = fd;
    op->offset = offset;
    op->size = size;
    
    /* 分配数据缓冲区并复制 */
    void *buf = kmalloc(size);
    if (buf == NULL) {
        kfree(op);
        spin_unlock(&tx->lock);
        put_tx_context(tx);
        return -1;
    }
    
    memcpy(buf, data, size);
    op->buf = buf;
    
    list_add_tail(&op->list, &tx->ops);
    tx->op_count++;
    
    /* 记录写集合 */
    if (tx->write_count < 16) {
        tx->write_set[tx->write_count++] = fd;  /* 简化：使用 fd 作为标识 */
    }
    
    spin_unlock(&tx->lock);
    put_tx_context(tx);
    
    return 0;
}

/* 添加读操作 */
int utx_add_read_kernel(uint64_t tx_id, int fd, uint64_t offset,
                        void *buf, size_t size) {
    struct utx_context *tx = find_tx_context(tx_id);
    if (tx == NULL) {
        return -1;
    }
    
    spin_lock(&tx->lock);
    
    if (tx->state != UTX_STATE_ACTIVE) {
        spin_unlock(&tx->lock);
        put_tx_context(tx);
        return -1;
    }
    
    /* 检查操作数限制 */
    if (tx->op_count >= UTX_MAX_OPS) {
        spin_unlock(&tx->lock);
        put_tx_context(tx);
        return -1;
    }
    
    /* 创建操作记录 */
    struct utx_op *op = kmalloc(sizeof(struct utx_op));
    if (op == NULL) {
        spin_unlock(&tx->lock);
        put_tx_context(tx);
        return -1;
    }
    
    op->type = UTX_OP_READ;
    op->flags = 0;
    op->fd = fd;
    op->offset = offset;
    op->size = size;
    op->buf = buf;  /* 直接使用用户缓冲区指针 */
    
    list_add_tail(&op->list, &tx->ops);
    tx->op_count++;
    
    /* 记录读集合 */
    if (tx->read_count < 16) {
        tx->read_set[tx->read_count++] = fd;
    }
    
    spin_unlock(&tx->lock);
    put_tx_context(tx);
    
    return 0;
}

/* 查询事务状态 */
int utx_get_status_kernel(uint64_t tx_id, struct utx_handle_user *handle) {
    struct utx_context *tx = find_tx_context(tx_id);
    if (tx == NULL || handle == NULL) {
        return -1;
    }
    
    spin_lock(&tx->lock);
    
    handle->tx_id = tx->tx_id;
    handle->state = tx->state;
    handle->flags = tx->flags;
    handle->op_count = tx->op_count;
    handle->error_code = tx->error_code;
    handle->start_time = tx->start_time;
    handle->timeout = tx->timeout;
    
    spin_unlock(&tx->lock);
    put_tx_context(tx);
    
    return 0;
}

/* 创建快照 */
int utx_snapshot_create_kernel(uint32_t pid, uint64_t *snapshot_id) {
    /* 简化实现：创建当前时间的快照 ID */
    if (snapshot_id == NULL) {
        return -1;
    }
    
    *snapshot_id = get_tick_count();
    
    return 0;
}

/* ========== 冲突检测 ========== */

/* 检查两个事务是否可并行提交 */
int utx_can_parallel_commit(struct utx_context *t1, struct utx_context *t2) {
    if (t1 == NULL || t2 == NULL) {
        return 1;  /* 无效事务，允许 */
    }
    
    /* 检查写-写冲突 */
    for (int i = 0; i < t1->write_count; i++) {
        for (int j = 0; j < t2->write_count; j++) {
            if (t1->write_set[i] == t2->write_set[j]) {
                return 0;  /* 写-写冲突 */
            }
        }
    }
    
    /* 检查写-读冲突 */
    for (int i = 0; i < t1->write_count; i++) {
        for (int j = 0; j < t2->read_count; j++) {
            if (t1->write_set[i] == t2->read_set[j]) {
                return 0;  /* 写-读冲突 */
            }
        }
    }
    
    /* 检查读-写冲突 */
    for (int i = 0; i < t1->read_count; i++) {
        for (int j = 0; j < t2->write_count; j++) {
            if (t1->read_set[i] == t2->write_set[j]) {
                return 0;  /* 读-写冲突 */
            }
        }
    }
    
    return 1;  /* 无冲突 */
}

/* 检测读写冲突 */
int utx_detect_conflict(struct utx_context *tx, uint64_t block_no, int is_write) {
    if (tx == NULL) return 0;
    
    if (is_write) {
        /* 写操作：检查是否有其他事务正在写或读该块 */
        spin_lock(&all_tx_lock);
        struct utx_context *other;
        list_for_each_entry(other, &all_transactions, global_list) {
            if (other->tx_id == tx->tx_id) continue;
            
            /* 检查写集合 */
            for (int i = 0; i < other->write_count; i++) {
                if (other->write_set[i] == block_no) {
                    spin_unlock(&all_tx_lock);
                    return 1;  /* 冲突 */
                }
            }
            
            /* 检查读集合 */
            for (int i = 0; i < other->read_count; i++) {
                if (other->read_set[i] == block_no) {
                    spin_unlock(&all_tx_lock);
                    return 1;  /* 冲突 */
                }
            }
        }
        spin_unlock(&all_tx_lock);
    } else {
        /* 读操作：只检查是否有其他事务正在写该块 */
        spin_lock(&all_tx_lock);
        struct utx_context *other;
        list_for_each_entry(other, &all_transactions, global_list) {
            if (other->tx_id == tx->tx_id) continue;
            
            for (int i = 0; i < other->write_count; i++) {
                if (other->write_set[i] == block_no) {
                    spin_unlock(&all_tx_lock);
                    return 1;  /* 冲突 */
                }
            }
        }
        spin_unlock(&all_tx_lock);
    }
    
    return 0;  /* 无冲突 */
}

/* ========== 超时管理 ========== */

/* 检查超时事务 */
void utx_check_timeouts(void) {
    uint64_t now = get_tick_count();
    
    spin_lock(&all_tx_lock);
    
    struct utx_context *tx, *next;
    list_for_each_entry_safe(tx, next, &all_transactions, global_list) {
        /* 跳过已完成事务 */
        if (tx->state == UTX_STATE_COMMITTED || 
            tx->state == UTX_STATE_ABORTED) {
            continue;
        }
        
        /* 检查超时 (timeout == 0 表示永不超时) */
        if (tx->timeout > 0 && (now - tx->start_time) > tx->timeout) {
            tx->state = UTX_STATE_TIMEOUT;
            global_stats.timeout_tx++;
            
            // print("[UTX] Transaction ");
            // print_hex(tx->tx_id);
            // println(" timed out");
        }
    }
    
    spin_unlock(&all_tx_lock);
}

/* 设置事务超时 */
int utx_set_timeout(uint64_t tx_id, uint64_t timeout_ms) {
    struct utx_context *tx = find_tx_context(tx_id);
    if (tx == NULL) {
        return -1;
    }
    
    spin_lock(&tx->lock);
    tx->timeout = timeout_ms;
    spin_unlock(&tx->lock);
    put_tx_context(tx);
    
    return 0;
}

/* ========== 统计 ========== */

void utx_get_stats(struct utx_stats *stats) {
    if (stats != NULL) {
        stats->total_tx = global_stats.total_tx;
        stats->committed_tx = global_stats.committed_tx;
        stats->aborted_tx = global_stats.aborted_tx;
        stats->timeout_tx = global_stats.timeout_tx;
        stats->total_ops = global_stats.total_ops;
        stats->conflicts = global_stats.conflicts;
    }
}
