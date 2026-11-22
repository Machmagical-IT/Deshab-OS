#include "hotupdate.h"
#include "console.h"
#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "ktypes.h"  /* for spin_lock/spin_unlock */
#include "task.h"    /* for get_tick_count */

/* 模块链表头 */
static struct module_info *module_list = NULL;

/* 模块链表锁 - 保护模块链表的并发访问 */
static volatile int module_list_lock = 0;

/* 检查点存储 */
#define MAX_CHECKPOINTS 16
static struct update_checkpoint checkpoints[MAX_CHECKPOINTS];
static int checkpoint_count = 0;
static uint64_t next_checkpoint_id = 1;

/* 内核版本 */
static kernel_version_t current_version = {0, 1, 0, "alpha"};

void hotupdate_init(void) {
    module_list = NULL;
    checkpoint_count = 0;
    module_list_lock = 0;
}

int module_register(struct module_info *mod) {
    if (mod == NULL) {
        return -1;
    }
    
    /* 获取模块链表锁 */
    spin_lock(&module_list_lock);
    
    /* 检查是否已存在 */
    struct module_info *curr = module_list;
    while (curr != NULL) {
        if (strcmp(curr->name, mod->name) == 0) {
            print("[MODULE] Already registered: ");
            println(mod->name);
            spin_unlock(&module_list_lock);
            return -1;
        }
        curr = curr->next;
    }
    
    /* 添加到链表 */
    mod->next = module_list;
    module_list = mod;
    mod->state = MODULE_STATE_LOADED;
    
    /* 调用加载回调 */
    if (mod->on_load != NULL) {
        if (mod->on_load() != 0) {
            print("[MODULE] Load callback failed: ");
            println(mod->name);
            module_list = mod->next;
            spin_unlock(&module_list_lock);
            return -1;
        }
    }
    
    mod->state = MODULE_STATE_ACTIVE;
    
    print("[MODULE] Registered: ");
    print(mod->name);
    print(" v");
    println(mod->version);
    
    spin_unlock(&module_list_lock);
    return 0;
}

int module_unregister(const char *name) {
    if (name == NULL) {
        return -1;
    }
    
    spin_lock(&module_list_lock);
    
    struct module_info *prev = NULL;
    struct module_info *curr = module_list;
    
    while (curr != NULL) {
        if (strcmp(curr->name, name) == 0) {
            /* 调用卸载回调 */
            if (curr->on_unload != NULL) {
                curr->on_unload();
            }
            
            if (prev == NULL) {
                module_list = curr->next;
            } else {
                prev->next = curr->next;
            }
            
            print("[MODULE] Unregistered: ");
            println(name);
            
            spin_unlock(&module_list_lock);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    
    spin_unlock(&module_list_lock);
    return -1;
}

struct module_info *hu_module_get(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    
    struct module_info *curr = module_list;
    while (curr != NULL) {
        if (strcmp(curr->name, name) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    
    return NULL;
}

int hu_module_load(const char *path) {
    /* TODO: 实现 ELF 模块加载器 */
    (void)path;
    
    print("[MODULE] Loading from: ");
    println(path);
    
    /* 1. 读取模块文件 */
    /* 2. 解析 ELF 格式 */
    /* 3. 重定位 */
    /* 4. 解析符号 */
    /* 5. 调用 module_register */
    
    return -1;
}

int hu_module_unload(const char *name) {
    return module_unregister(name);
}

int module_hotupdate(const char *name, const char *new_path) {
    struct module_info *mod = hu_module_get(name);
    if (mod == NULL) {
        print("[MODULE] Not found: ");
        println(name);
        return -1;
    }
    
    print("[MODULE] Hot updating: ");
    println(name);
    
    mod->state = MODULE_STATE_UPDATING;
    
    /* 创建检查点 */
    checkpoint_create("Pre-update checkpoint");
    
    /* 读取新模块 */
    size_t size;
    void *new_module = vfs_read_file(new_path, &size);
    if (new_module == NULL) {
        println("[MODULE] Failed to read new module");
        mod->state = MODULE_STATE_ACTIVE;
        return -1;
    }
    
    /* 调用更新回调 */
    if (mod->on_update != NULL) {
        if (mod->on_update(new_module) != 0) {
            println("[MODULE] Update callback failed, rolling back...");
            kfree(new_module);
            mod->state = MODULE_STATE_ROLLBACK;
            checkpoint_restore(next_checkpoint_id - 1);
            mod->state = MODULE_STATE_ACTIVE;
            return -1;
        }
    }
    
    kfree(new_module);
    mod->state = MODULE_STATE_ACTIVE;
    
    print("[MODULE] Hot update complete: ");
    println(name);
    
    return 0;
}

int checkpoint_create(const char *description) {
    if (checkpoint_count >= MAX_CHECKPOINTS) {
        /* 删除最旧的检查点 */
        checkpoint_delete(checkpoints[0].id);
    }
    
    struct update_checkpoint *cp = &checkpoints[checkpoint_count];
    cp->id = next_checkpoint_id++;
    cp->timestamp = 0;  /* TODO: 获取时间戳 */
    
    if (description != NULL) {
        int i = 0;
        while (description[i] && i < 63) {
            cp->description[i] = description[i];
            i++;
        }
        cp->description[i] = '\0';
    } else {
        cp->description[0] = '\0';
    }
    
    /* TODO: 创建实际快照 */
    cp->module_snapshot = NULL;
    cp->snapshot_size = 0;
    cp->state_backup = NULL;
    cp->state_size = 0;
    
    checkpoint_count++;
    
    print("[CHECKPOINT] Created #");
    print_hex(cp->id);
    print(": ");
    println(cp->description);
    
    return cp->id;
}

int checkpoint_restore(uint64_t checkpoint_id) {
    for (int i = 0; i < checkpoint_count; i++) {
        if (checkpoints[i].id == checkpoint_id) {
            print("[CHECKPOINT] Restoring #");
            print_hex(checkpoint_id);
            println("...");
            
            struct update_checkpoint *cp = &checkpoints[i];
            
            /* 1. 恢复模块快照 */
            if (cp->module_snapshot != NULL && cp->snapshot_size > 0) {
                /* 遍历快照中的模块信息并恢复 */
                struct module_info *snapshot_mods = (struct module_info *)cp->module_snapshot;
                int mod_count = cp->snapshot_size / sizeof(struct module_info);
                
                for (int j = 0; j < mod_count; j++) {
                    struct module_info *snap_mod = &snapshot_mods[j];
                    struct module_info *curr_mod = hu_module_get(snap_mod->name);
                    
                    if (curr_mod != NULL) {
                        /* 恢复模块状态 */
                        curr_mod->state = snap_mod->state;
                        curr_mod->base = snap_mod->base;
                        curr_mod->size = snap_mod->size;
                        
                        print("[CHECKPOINT] Restored module: ");
                        println(snap_mod->name);
                    } else {
                        /* 模块不存在，需要重新注册 */
                        struct module_info *new_mod = kmalloc(sizeof(*new_mod));
                        if (new_mod != NULL) {
                            memcpy(new_mod, snap_mod, sizeof(*new_mod));
                            new_mod->next = NULL;
                            if (module_register(new_mod) != 0) {
                                /* 注册失败，释放内存避免泄漏 */
                                kfree(new_mod);
                                print("[CHECKPOINT] Failed to re-register module: ");
                                println(snap_mod->name);
                            } else {
                                print("[CHECKPOINT] Re-registered module: ");
                                println(snap_mod->name);
                            }
                        }
                    }
                }
            }
            
            /* 2. 恢复状态备份 */
            if (cp->state_backup != NULL && cp->state_size > 0) {
                /* 状态备份格式：首个 uint64_t 是目标地址，后面是数据 */
                uint64_t *state_hdr = (uint64_t *)cp->state_backup;
                uint64_t target_addr = state_hdr[0];
                uint64_t state_data_size = state_hdr[1];
                void *state_data = (void *)(state_hdr + 2);
                
                /* 只有当目标地址在内核空间才恢复 */
                if (target_addr >= 0xffffffff80000000ULL && 
                    state_data_size <= cp->state_size - 16) {
                    memcpy((void *)target_addr, state_data, state_data_size);
                    print("[CHECKPOINT] Restored state at: ");
                    print_hex(target_addr);
                    println("");
                }
            }
            
            println("[CHECKPOINT] Restore complete");
            return 0;
        }
    }
    
    print("[CHECKPOINT] Not found: #");
    print_hex(checkpoint_id);
    println("");
    
    return -1;
}

int checkpoint_list(struct update_checkpoint **cp_list, int *count) {
    if (cp_list != NULL) {
        *cp_list = checkpoints;
    }
    if (count != NULL) {
        *count = checkpoint_count;
    }
    return 0;
}

int checkpoint_delete(uint64_t checkpoint_id) {
    for (int i = 0; i < checkpoint_count; i++) {
        if (checkpoints[i].id == checkpoint_id) {
            /* 释放快照内存 */
            if (checkpoints[i].module_snapshot != NULL) {
                kfree(checkpoints[i].module_snapshot);
            }
            if (checkpoints[i].state_backup != NULL) {
                kfree(checkpoints[i].state_backup);
            }
            
            /* 移动后面的检查点 */
            for (int j = i; j < checkpoint_count - 1; j++) {
                checkpoints[j] = checkpoints[j + 1];
            }
            checkpoint_count--;
            
            print("[CHECKPOINT] Deleted #");
            print_hex(checkpoint_id);
            println("");
            
            return 0;
        }
    }
    return -1;
}

kernel_version_t kernel_get_version(void) {
    return current_version;
}

void kernel_set_version(uint32_t major, uint32_t minor, uint32_t patch, const char *build) {
    current_version.major = major;
    current_version.minor = minor;
    current_version.patch = patch;
    
    if (build != NULL) {
        int i = 0;
        while (build[i] && i < 15) {
            current_version.build[i] = build[i];
            i++;
        }
        current_version.build[i] = '\0';
    } else {
        current_version.build[0] = '\0';
    }
}

int kernel_check_update(const char *update_path, kernel_version_t *new_version) {
    /* TODO: 解析更新包中的版本信息 */
    (void)update_path;
    (void)new_version;
    return -1;
}

int kernel_apply_update(const char *update_path) {
    print("[KERNEL] Applying update from: ");
    println(update_path);
    
    /* 创建检查点 */
    int cp_id = checkpoint_create("Pre-kernel-update checkpoint");
    if (cp_id < 0) {
        println("[KERNEL] Failed to create checkpoint");
        return -1;
    }
    
    /* TODO: 实际更新逻辑 */
    /* 1. 验证更新包 */
    /* 2. 应用更新 */
    /* 3. 重启内核或切换到新内核 */
    
    println("[KERNEL] Update complete");
    return 0;
}

/* ========== 热更新高级增强功能 ========== */

/* === 差分更新系统 === */

/* 差分块 */
#define DIFF_BLOCK_SIZE     4096

struct diff_block {
    uint64_t offset;            /* 目标偏移 */
    uint32_t size;              /* 数据大小 */
    uint32_t type;              /* 0=保留, 1=修改, 2=插入, 3=删除 */
    uint8_t data[DIFF_BLOCK_SIZE];
};

struct diff_header {
    uint32_t magic;             /* 魔数: 'DIFF' */
    uint32_t version;
    uint64_t old_checksum;      /* 原始文件校验和 */
    uint64_t new_checksum;      /* 新文件校验和 */
    uint32_t block_count;       /* 差分块数 */
    uint32_t flags;
};

/* 计算校验和 */
static uint64_t compute_checksum(const void *data, size_t size) {
    if (data == NULL || size == 0) return 0;
    
    /* FNV-1a 哈希 */
    uint64_t hash = 14695981039346656037ULL;
    const uint8_t *ptr = (const uint8_t *)data;
    
    for (size_t i = 0; i < size; i++) {
        hash ^= ptr[i];
        hash *= 1099511628211ULL;
    }
    
    return hash;
}

/* 创建差分包 */
int diff_create(const void *old_data, size_t old_size,
                const void *new_data, size_t new_size,
                void *diff_out, size_t *diff_size) {
    if (old_data == NULL || new_data == NULL || diff_out == NULL) {
        return -1;
    }
    
    struct diff_header *header = (struct diff_header *)diff_out;
    header->magic = 0x46464944;  /* 'DIFF' */
    header->version = 1;
    header->old_checksum = compute_checksum(old_data, old_size);
    header->new_checksum = compute_checksum(new_data, new_size);
    header->block_count = 0;
    header->flags = 0;
    
    size_t offset = sizeof(struct diff_header);
    
    /* 比较并生成差分块 */
    size_t min_size = old_size < new_size ? old_size : new_size;
    const uint8_t *old_ptr = (const uint8_t *)old_data;
    const uint8_t *new_ptr = (const uint8_t *)new_data;
    
    size_t i = 0;
    while (i < min_size) {
        /* 找到不同的位置 */
        while (i < min_size && old_ptr[i] == new_ptr[i]) i++;
        
        if (i >= min_size) break;
        
        /* 找到相同的位置 */
        size_t start = i;
        while (i < min_size && old_ptr[i] != new_ptr[i]) i++;
        
        if (i > start) {
            struct diff_block *block = (struct diff_block *)((uint8_t *)diff_out + offset);
            block->offset = start;
            block->size = i - start;
            block->type = 1;  /* 修改 */
            
            for (size_t j = 0; j < block->size && j < DIFF_BLOCK_SIZE; j++) {
                block->data[j] = new_ptr[start + j];
            }
            
            offset += sizeof(struct diff_block);
            header->block_count++;
        }
    }
    
    /* 处理新增数据 */
    if (new_size > old_size) {
        struct diff_block *block = (struct diff_block *)((uint8_t *)diff_out + offset);
        block->offset = old_size;
        block->size = new_size - old_size;
        block->type = 2;  /* 插入 */
        
        for (size_t j = 0; j < block->size && j < DIFF_BLOCK_SIZE; j++) {
            block->data[j] = new_ptr[old_size + j];
        }
        
        offset += sizeof(struct diff_block);
        header->block_count++;
    }
    
    /* 处理删除数据 */
    if (old_size > new_size) {
        struct diff_block *block = (struct diff_block *)((uint8_t *)diff_out + offset);
        block->offset = new_size;
        block->size = old_size - new_size;
        block->type = 3;  /* 删除 */
        
        offset += sizeof(struct diff_block);
        header->block_count++;
    }
    
    *diff_size = offset;
    
    return 0;
}

/* 应用差分包 */
int diff_apply(void *data, size_t *size, const void *diff_data, size_t diff_size) {
    if (data == NULL || diff_data == NULL || diff_size < sizeof(struct diff_header)) {
        return -1;
    }
    
    const struct diff_header *header = (const struct diff_header *)diff_data;
    
    /* 验证魔数 */
    if (header->magic != 0x46464944) {
        println("[DIFF] Invalid magic number");
        return -1;
    }
    
    /* 验证原始数据校验和 */
    uint64_t checksum = compute_checksum(data, *size);
    if (checksum != header->old_checksum) {
        println("[DIFF] Checksum mismatch");
        return -1;
    }
    
    /* 应用差分块 */
    size_t offset = sizeof(struct diff_header);
    uint8_t *ptr = (uint8_t *)data;
    
    for (uint32_t i = 0; i < header->block_count; i++) {
        if (offset + sizeof(struct diff_block) > diff_size) break;
        
        const struct diff_block *block = (const struct diff_block *)
            ((const uint8_t *)diff_data + offset);
        
        switch (block->type) {
            case 1:  /* 修改 */
                for (uint32_t j = 0; j < block->size && block->offset + j < *size; j++) {
                    ptr[block->offset + j] = block->data[j];
                }
                break;
                
            case 2:  /* 插入 */
                /* 需要扩展数据区域 (简化实现) */
                break;
                
            case 3:  /* 删除 */
                *size = block->offset;
                break;
        }
        
        offset += sizeof(struct diff_block);
    }
    
    return 0;
}

/* === 更新事务管理 === */

/* 事务状态 */
enum tx_state {
    TX_STATE_IDLE,
    TX_STATE_PREPARING,
    TX_STATE_PREPARED,
    TX_STATE_COMMITTING,
    TX_STATE_COMMITTED,
    TX_STATE_ABORTING,
    TX_STATE_ABORTED
};

/* 事务操作 */
struct tx_operation {
    int type;           /* 0=写内存, 1=写文件, 2=加载模块 */
    void *target;
    void *old_value;
    void *new_value;
    size_t size;
    struct tx_operation *next;
};

/* 事务 */
struct update_transaction {
    uint64_t id;
    enum tx_state state;
    struct tx_operation *ops;
    struct tx_operation *ops_tail;
    int op_count;
    uint64_t checkpoint_id;
};

#define MAX_TRANSACTIONS    8
static struct update_transaction transactions[MAX_TRANSACTIONS];

/* 初始化事务系统 */
void tx_init(void) {
    for (int i = 0; i < MAX_TRANSACTIONS; i++) {
        transactions[i].id = 0;
        transactions[i].state = TX_STATE_IDLE;
        transactions[i].ops = NULL;
        transactions[i].ops_tail = NULL;
        transactions[i].op_count = 0;
    }
}

/* 创建事务 */
uint64_t tx_begin(void) {
    for (int i = 0; i < MAX_TRANSACTIONS; i++) {
        if (transactions[i].state == TX_STATE_IDLE) {
            transactions[i].id = i + 1;
            transactions[i].state = TX_STATE_PREPARING;
            transactions[i].ops = NULL;
            transactions[i].ops_tail = NULL;
            transactions[i].op_count = 0;
            transactions[i].checkpoint_id = checkpoint_create("Transaction checkpoint");
            return transactions[i].id;
        }
    }
    return 0;
}

/* 添加操作到事务 */
int tx_add_op(uint64_t tx_id, int type, void *target, void *old_val, 
              void *new_val, size_t size) {
    if (tx_id == 0 || tx_id > MAX_TRANSACTIONS) return -1;
    
    struct update_transaction *tx = &transactions[tx_id - 1];
    if (tx->state != TX_STATE_PREPARING) return -1;
    
    struct tx_operation *op = (struct tx_operation *)kmalloc(sizeof(struct tx_operation));
    if (op == NULL) return -1;
    
    op->type = type;
    op->target = target;
    op->old_value = old_val;
    op->new_value = new_val;
    op->size = size;
    op->next = NULL;
    
    if (tx->ops == NULL) {
        tx->ops = op;
        tx->ops_tail = op;
    } else {
        tx->ops_tail->next = op;
        tx->ops_tail = op;
    }
    tx->op_count++;
    
    return 0;
}

/* 准备事务 */
int tx_prepare(uint64_t tx_id) {
    if (tx_id == 0 || tx_id > MAX_TRANSACTIONS) return -1;
    
    struct update_transaction *tx = &transactions[tx_id - 1];
    if (tx->state != TX_STATE_PREPARING) return -1;
    
    /* 验证所有操作 */
    struct tx_operation *op = tx->ops;
    while (op != NULL) {
        /* 验证操作 */
        if (op->target == NULL) {
            tx->state = TX_STATE_ABORTED;
            return -1;
        }
        op = op->next;
    }
    
    tx->state = TX_STATE_PREPARED;
    return 0;
}

/* 提交事务 */
int tx_commit(uint64_t tx_id) {
    if (tx_id == 0 || tx_id > MAX_TRANSACTIONS) return -1;
    
    struct update_transaction *tx = &transactions[tx_id - 1];
    if (tx->state != TX_STATE_PREPARED) return -1;
    
    tx->state = TX_STATE_COMMITTING;
    
    /* 执行所有操作 */
    struct tx_operation *op = tx->ops;
    while (op != NULL) {
        switch (op->type) {
            case 0:  /* 写内存 */
                if (op->new_value != NULL && op->target != NULL) {
                    for (size_t i = 0; i < op->size; i++) {
                        ((uint8_t *)op->target)[i] = ((uint8_t *)op->new_value)[i];
                    }
                }
                break;
                
            case 1:  /* 写文件 */
                /* TODO: 文件写入 */
                break;
                
            case 2:  /* 加载模块 */
                /* TODO: 模块加载 */
                break;
        }
        op = op->next;
    }
    
    tx->state = TX_STATE_COMMITTED;
    
    /* 清理操作列表 */
    op = tx->ops;
    while (op != NULL) {
        struct tx_operation *next = op->next;
        kfree(op);
        op = next;
    }
    tx->ops = NULL;
    tx->ops_tail = NULL;
    
    return 0;
}

/* 回滚事务 */
int tx_rollback(uint64_t tx_id) {
    if (tx_id == 0 || tx_id > MAX_TRANSACTIONS) return -1;
    
    struct update_transaction *tx = &transactions[tx_id - 1];
    tx->state = TX_STATE_ABORTING;
    
    /* 回滚操作 */
    struct tx_operation *op = tx->ops;
    while (op != NULL) {
        switch (op->type) {
            case 0:  /* 写内存 */
                if (op->old_value != NULL && op->target != NULL) {
                    for (size_t i = 0; i < op->size; i++) {
                        ((uint8_t *)op->target)[i] = ((uint8_t *)op->old_value)[i];
                    }
                }
                /* 释放旧值缓冲区 */
                if (op->old_value != NULL) {
                    kfree(op->old_value);
                }
                break;
        }
        /* 保存下一个指针，然后释放当前操作 */
        struct tx_operation *next = op->next;
        kfree(op);
        op = next;
    }
    
    /* 清空操作链表 */
    tx->ops = NULL;
    tx->op_count = 0;
    
    /* 恢复检查点 */
    if (tx->checkpoint_id > 0) {
        checkpoint_restore(tx->checkpoint_id);
    }
    
    tx->state = TX_STATE_ABORTED;
    
    return 0;
}

/* === 更新验证 === */

/* 数字签名结构 */
struct update_signature {
    uint32_t algorithm;     /* 0=none, 1=RSA, 2=ECDSA */
    uint32_t key_id;
    uint8_t signature[256];
    uint32_t sig_len;
};

/* 验证更新包签名 */
int update_verify_signature(const void *update_data, size_t size,
                            const struct update_signature *sig) {
    if (update_data == NULL || sig == NULL) return -1;
    
    switch (sig->algorithm) {
        case 0:  /* 无签名 */
            println("[VERIFY] No signature, allowing update");
            return 0;
            
        case 1:  /* RSA */
            /* TODO: RSA 验证 */
            println("[VERIFY] RSA signature verification not implemented");
            return 0;
            
        case 2:  /* ECDSA */
            /* TODO: ECDSA 验证 */
            println("[VERIFY] ECDSA signature verification not implemented");
            return 0;
            
        default:
            println("[VERIFY] Unknown signature algorithm");
            return -1;
    }
}

/* 验证更新包完整性 */
int update_verify_integrity(const void *update_data, size_t size,
                            uint64_t expected_checksum) {
    if (update_data == NULL) return -1;
    
    uint64_t checksum = compute_checksum(update_data, size);
    
    if (checksum != expected_checksum) {
        println("[VERIFY] Integrity check failed");
        return -1;
    }
    
    println("[VERIFY] Integrity check passed");
    return 0;
}

/* === 模块依赖管理 === */

#define MAX_DEPENDENCIES    16

struct module_dependency {
    char name[32];
    char min_version[16];
    int required;           /* 1=必需, 0=可选 */
};

/* 检查模块依赖 */
int module_check_dependencies(const char *name, struct module_dependency *deps, int count) {
    if (name == NULL || deps == NULL) return -1;
    
    int satisfied = 1;
    
    for (int i = 0; i < count && i < MAX_DEPENDENCIES; i++) {
        struct module_info *mod = hu_module_get(deps[i].name);
        
        if (mod == NULL) {
            if (deps[i].required) {
                print("[DEP] Missing required dependency: ");
                println(deps[i].name);
                satisfied = 0;
            } else {
                print("[DEP] Missing optional dependency: ");
                println(deps[i].name);
            }
        } else {
            /* TODO: 版本检查 */
            print("[DEP] Dependency satisfied: ");
            println(deps[i].name);
        }
    }
    
    return satisfied ? 0 : -1;
}

/* 解析模块依赖 */
int module_parse_dependencies(const char *dep_str, struct module_dependency *deps, int *count) {
    if (dep_str == NULL || deps == NULL || count == NULL) return -1;
    
    *count = 0;
    int i = 0;
    
    while (dep_str[i] && *count < MAX_DEPENDENCIES) {
        /* 跳过空白 */
        while (dep_str[i] == ' ' || dep_str[i] == ',') i++;
        
        if (!dep_str[i]) break;
        
        /* 解析模块名 */
        int j = 0;
        while (dep_str[i] && dep_str[i] != ',' && dep_str[i] != ' ' && 
               dep_str[i] != '(' && dep_str[i] != ':' && j < 31) {
            deps[*count].name[j++] = dep_str[i++];
        }
        deps[*count].name[j] = '\0';
        
        /* 解析版本 */
        deps[*count].min_version[0] = '\0';
        deps[*count].required = 1;
        
        if (dep_str[i] == '(' || dep_str[i] == ':') {
            i++;  /* 跳过分隔符 */
            j = 0;
            while (dep_str[i] && dep_str[i] != ')' && dep_str[i] != ',' && j < 15) {
                deps[*count].min_version[j++] = dep_str[i++];
            }
            deps[*count].min_version[j] = '\0';
            if (dep_str[i] == ')') i++;
        }
        
        /* 检查是否可选 */
        if (deps[*count].name[0] == '?') {
            deps[*count].required = 0;
            /* 移动名称 */
            j = 0;
            while (deps[*count].name[j + 1]) {
                deps[*count].name[j] = deps[*count].name[j + 1];
                j++;
            }
            deps[*count].name[j] = '\0';
        }
        
        (*count)++;
    }
    
    return 0;
}

/* === 更新状态机 === */

/* 更新状态 */
enum update_state {
    UPDATE_STATE_IDLE,
    UPDATE_STATE_DOWNLOADING,
    UPDATE_STATE_VERIFYING,
    UPDATE_STATE_PREPARING,
    UPDATE_STATE_APPLYING,
    UPDATE_STATE_REBOOTING,
    UPDATE_STATE_ROLLING_BACK,
    UPDATE_STATE_COMPLETED,
    UPDATE_STATE_FAILED
};

/* 更新上下文 */
struct update_context {
    enum update_state state;
    char update_path[256];
    uint64_t update_size;
    uint64_t bytes_processed;
    uint32_t progress;          /* 0-100 */
    int error_code;
    uint64_t tx_id;
    uint64_t start_time;
    uint64_t end_time;
};

static struct update_context update_ctx;

/* 初始化更新上下文 */
void update_ctx_init(void) {
    update_ctx.state = UPDATE_STATE_IDLE;
    update_ctx.update_path[0] = '\0';
    update_ctx.update_size = 0;
    update_ctx.bytes_processed = 0;
    update_ctx.progress = 0;
    update_ctx.error_code = 0;
    update_ctx.tx_id = 0;
}

/* 获取更新状态 */
enum update_state update_get_state(void) {
    return update_ctx.state;
}

/* 获取更新进度 */
uint32_t update_get_progress(void) {
    return update_ctx.progress;
}

/* 更新状态转换 */
int update_transition(enum update_state new_state) {
    println("[UPDATE] State transition");
    
    /* 验证状态转换 */
    switch (update_ctx.state) {
        case UPDATE_STATE_IDLE:
            if (new_state != UPDATE_STATE_DOWNLOADING && 
                new_state != UPDATE_STATE_PREPARING) {
                return -1;
            }
            break;
            
        case UPDATE_STATE_DOWNLOADING:
            if (new_state != UPDATE_STATE_VERIFYING && new_state != UPDATE_STATE_FAILED) {
                return -1;
            }
            break;
            
        case UPDATE_STATE_VERIFYING:
            if (new_state != UPDATE_STATE_PREPARING && new_state != UPDATE_STATE_FAILED) {
                return -1;
            }
            break;
            
        case UPDATE_STATE_PREPARING:
            if (new_state != UPDATE_STATE_APPLYING && new_state != UPDATE_STATE_FAILED) {
                return -1;
            }
            break;
            
        case UPDATE_STATE_APPLYING:
            if (new_state != UPDATE_STATE_REBOOTING && new_state != UPDATE_STATE_ROLLING_BACK &&
                new_state != UPDATE_STATE_COMPLETED && new_state != UPDATE_STATE_FAILED) {
                return -1;
            }
            break;
            
        case UPDATE_STATE_ROLLING_BACK:
            if (new_state != UPDATE_STATE_FAILED && new_state != UPDATE_STATE_COMPLETED) {
                return -1;
            }
            break;
            
        case UPDATE_STATE_REBOOTING:
        case UPDATE_STATE_COMPLETED:
        case UPDATE_STATE_FAILED:
            return -1;  /* 终态 */
    }
    
    update_ctx.state = new_state;
    return 0;
}

/* === 更新进度追踪 === */

/* 更新进度事件 */
struct update_progress_event {
    uint64_t timestamp;
    enum update_state state;
    uint32_t progress;
    char message[64];
};

#define MAX_PROGRESS_EVENTS    32
static struct update_progress_event progress_events[MAX_PROGRESS_EVENTS];
static int progress_event_count;

/* 记录进度事件 */
void update_record_progress(enum update_state state, uint32_t progress, const char *message) {
    if (progress_event_count >= MAX_PROGRESS_EVENTS) {
        /* 移动事件 */
        for (int i = 0; i < MAX_PROGRESS_EVENTS - 1; i++) {
            progress_events[i] = progress_events[i + 1];
        }
        progress_event_count--;
    }
    
    struct update_progress_event *evt = &progress_events[progress_event_count++];
    
    extern uint64_t get_tick_count(void);
    evt->timestamp = get_tick_count();
    evt->state = state;
    evt->progress = progress;
    
    if (message != NULL) {
        int i = 0;
        while (message[i] && i < 63) {
            evt->message[i] = message[i];
            i++;
        }
        evt->message[i] = '\0';
    } else {
        evt->message[0] = '\0';
    }
}

/* 打印更新历史 */
void update_print_history(void) {
    println("=== Update Progress History ===");
    
    for (int i = 0; i < progress_event_count; i++) {
        struct update_progress_event *evt = &progress_events[i];
        
        print("[");
        print_dec(evt->timestamp);
        print("] ");
        print_dec(evt->progress);
        print("% - ");
        println(evt->message);
    }
}

/* === 安全更新通道 === */

/* 更新通道 */
enum update_channel {
    CHANNEL_STABLE,
    CHANNEL_BETA,
    CHANNEL_DEV,
    CHANNEL_NIGHTLY
};

static enum update_channel current_channel = CHANNEL_STABLE;

/* 设置更新通道 */
void update_set_channel(enum update_channel channel) {
    current_channel = channel;
    
    const char *name;
    switch (channel) {
        case CHANNEL_STABLE:  name = "stable"; break;
        case CHANNEL_BETA:    name = "beta"; break;
        case CHANNEL_DEV:     name = "dev"; break;
        case CHANNEL_NIGHTLY: name = "nightly"; break;
        default:              name = "unknown"; break;
    }
    
    print("[UPDATE] Channel set to: ");
    println(name);
}

/* 检查是否允许从该通道更新 */
int update_channel_allowed(enum update_channel channel) {
    /* 只能升级到相同或更稳定的通道 */
    return channel <= current_channel;
}

/* === 更新统计 === */

struct update_stats {
    uint64_t total_updates;
    uint64_t successful_updates;
    uint64_t failed_updates;
    uint64_t rollbacks;
    uint64_t total_bytes_updated;
    uint64_t total_update_time;
};

static struct update_stats update_stats;

/* 记录更新统计 */
void update_record_stats(int success, uint64_t bytes, uint64_t time) {
    update_stats.total_updates++;
    
    if (success) {
        update_stats.successful_updates++;
    } else {
        update_stats.failed_updates++;
    }
    
    update_stats.total_bytes_updated += bytes;
    update_stats.total_update_time += time;
}

/* 获取更新统计 */
void update_get_stats(struct update_stats *stats) {
    if (stats != NULL) {
        *stats = update_stats;
    }
}

/* 打印更新统计 */
void update_print_stats(void) {
    println("=== Update Statistics ===");
    
    print("Total updates: ");
    print_dec(update_stats.total_updates);
    println("");
    
    print("Successful: ");
    print_dec(update_stats.successful_updates);
    println("");
    
    print("Failed: ");
    print_dec(update_stats.failed_updates);
    println("");
    
    print("Rollbacks: ");
    print_dec(update_stats.rollbacks);
    println("");
    
    print("Total bytes: ");
    print_dec(update_stats.total_bytes_updated);
    println("");
}

/* ========== 安全函数替换 (ftrace-like 机制) ========== */

/* 函数替换链表 */
static struct hu_func_replace *func_replace_list = NULL;
static spinlock_t func_replace_lock = SPINLOCK_INIT;
static uint32_t func_replace_version = 0;

/*
 * 安装函数替换
 * 使用 ftrace-like 机制：在目标函数入口插入 JMP 指令
 * x86_64 JMP 指令: E9 rel32 (5 字节) 或 REX.W JMP abs64 (需要更多空间)
 * 简化实现：使用 5 字节相对跳转
 */
int hu_func_replace_install(uint64_t target_addr, uint64_t replacement_addr,
                             const char *target_name, const char *replacement_name) {
    if (target_addr == 0 || replacement_addr == 0) {
        return -1;
    }

    /* 检查是否已存在替换 */
    spin_lock(&func_replace_lock);
    struct hu_func_replace *existing = func_replace_list;
    while (existing != NULL) {
        if (existing->target_addr == target_addr) {
            /* 已存在，更新替换地址 */
            existing->replacement_addr = replacement_addr;
            existing->version = ++func_replace_version;
            if (replacement_name != NULL) {
                int i = 0;
                while (replacement_name[i] && i < 63) {
                    existing->replacement_name[i] = replacement_name[i];
                    i++;
                }
                existing->replacement_name[i] = '\0';
            }
            existing->active = 1;
            spin_unlock(&func_replace_lock);

            /* 写入跳转指令 */
            /* 计算 32 位相对偏移: replacement - (target + 5) */
            int64_t rel_offset = (int64_t)replacement_addr - (int64_t)(target_addr + 5);
            if (rel_offset < -2147483648LL || rel_offset > 2147483647LL) {
                /* 偏移超出 32 位范围，需要使用间接跳转 */
                /* 简化：使用 14 字节绝对跳转 */
                /* REX.W FF 25 00 00 00 00 + 8字节绝对地址 */
                uint8_t jmp_code[14];
                jmp_code[0] = 0x48;  /* REX.W */
                jmp_code[1] = 0xFF;  /* JMP */
                jmp_code[2] = 0x25;  /* ModRM: [rip+0] */
                jmp_code[3] = 0x00;
                jmp_code[4] = 0x00;
                jmp_code[5] = 0x00;
                jmp_code[6] = 0x00;
                *(uint64_t *)&jmp_code[7] = replacement_addr;

                /* 写入跳转指令 (需要确保目标地址可写) */
                memcpy((void *)target_addr, jmp_code, 14);
            } else {
                /* 使用 5 字节相对跳转 */
                uint8_t jmp_code[5];
                jmp_code[0] = 0xE9;  /* JMP rel32 */
                *(int32_t *)&jmp_code[1] = (int32_t)rel_offset;

                memcpy((void *)target_addr, jmp_code, 5);
            }

            return 0;
        }
        existing = existing->next;
    }
    spin_unlock(&func_replace_lock);

    /* 创建新的替换记录 */
    struct hu_func_replace *rep = kmalloc(sizeof(*rep));
    if (rep == NULL) {
        return -1;
    }

    memset(rep, 0, sizeof(*rep));
    rep->target_addr = target_addr;
    rep->replacement_addr = replacement_addr;
    rep->version = ++func_replace_version;
    rep->active = 1;

    /* 保存原始指令 (最多 32 字节) */
    memcpy(rep->original_code, (void *)target_addr, 32);
    rep->original_size = 14;  /* 保存足够多的字节以覆盖跳转指令 */

    if (target_name != NULL) {
        int i = 0;
        while (target_name[i] && i < 63) {
            rep->target_name[i] = target_name[i];
            i++;
        }
        rep->target_name[i] = '\0';
    }
    if (replacement_name != NULL) {
        int i = 0;
        while (replacement_name[i] && i < 63) {
            rep->replacement_name[i] = replacement_name[i];
            i++;
        }
        rep->replacement_name[i] = '\0';
    }

    /* 写入跳转指令 */
    int64_t rel_offset = (int64_t)replacement_addr - (int64_t)(target_addr + 5);
    if (rel_offset < -2147483648LL || rel_offset > 2147483647LL) {
        uint8_t jmp_code[14];
        jmp_code[0] = 0x48;
        jmp_code[1] = 0xFF;
        jmp_code[2] = 0x25;
        jmp_code[3] = 0x00;
        jmp_code[4] = 0x00;
        jmp_code[5] = 0x00;
        jmp_code[6] = 0x00;
        *(uint64_t *)&jmp_code[7] = replacement_addr;
        memcpy((void *)target_addr, jmp_code, 14);
    } else {
        uint8_t jmp_code[5];
        jmp_code[0] = 0xE9;
        *(int32_t *)&jmp_code[1] = (int32_t)rel_offset;
        memcpy((void *)target_addr, jmp_code, 5);
    }

    /* 添加到链表 */
    spin_lock(&func_replace_lock);
    rep->next = func_replace_list;
    func_replace_list = rep;
    spin_unlock(&func_replace_lock);

    print("[HOTUPDATE] Function replaced: ");
    print(target_name ? target_name : "(unknown)");
    print(" -> ");
    println(replacement_name ? replacement_name : "(unknown)");

    return 0;
}

/* 卸载函数替换 (恢复原始指令) */
int hu_func_replace_uninstall(uint64_t target_addr) {
    if (target_addr == 0) {
        return -1;
    }

    spin_lock(&func_replace_lock);

    struct hu_func_replace *rep = func_replace_list;
    struct hu_func_replace *prev = NULL;

    while (rep != NULL) {
        if (rep->target_addr == target_addr) {
            /* 恢复原始指令 */
            memcpy((void *)target_addr, rep->original_code, rep->original_size);

            /* 从链表移除 */
            if (prev == NULL) {
                func_replace_list = rep->next;
            } else {
                prev->next = rep->next;
            }

            spin_unlock(&func_replace_lock);

            print("[HOTUPDATE] Function restored: ");
            println(rep->target_name);

            kfree(rep);
            return 0;
        }
        prev = rep;
        rep = rep->next;
    }

    spin_unlock(&func_replace_lock);
    return -1;  /* 未找到 */
}

/* 获取函数替换记录 */
struct hu_func_replace *hu_func_replace_get(uint64_t target_addr) {
    spin_lock(&func_replace_lock);

    struct hu_func_replace *rep = func_replace_list;
    while (rep != NULL) {
        if (rep->target_addr == target_addr) {
            spin_unlock(&func_replace_lock);
            return rep;
        }
        rep = rep->next;
    }

    spin_unlock(&func_replace_lock);
    return NULL;
}

/* 列出所有函数替换 */
int hu_func_replace_list(struct hu_func_replace **list, int *count) {
    if (list == NULL || count == NULL) return -1;

    spin_lock(&func_replace_lock);

    int n = 0;
    struct hu_func_replace *rep = func_replace_list;
    while (rep != NULL) {
        n++;
        rep = rep->next;
    }

    *count = n;
    *list = func_replace_list;

    spin_unlock(&func_replace_lock);
    return 0;
}

/* ========== 版本回滚 ========== */

/* 版本快照链表 */
static struct hu_version_snapshot *version_snapshots = NULL;
static uint32_t hu_snapshot_version = 0;
static spinlock_t version_lock = SPINLOCK_INIT;

/* 创建版本快照 */
int hu_version_snapshot_create(const char *description) {
    struct hu_version_snapshot *snap = kmalloc(sizeof(*snap));
    if (snap == NULL) {
        return -1;
    }

    memset(snap, 0, sizeof(*snap));
    snap->version = ++hu_snapshot_version;
    snap->timestamp = get_tick_count();

    if (description != NULL) {
        int i = 0;
        while (description[i] && i < 63) {
            snap->description[i] = description[i];
            i++;
        }
        snap->description[i] = '\0';
    }

    /* 复制当前所有函数替换记录到快照 */
    spin_lock(&func_replace_lock);
    struct hu_func_replace *rep = func_replace_list;
    int count = 0;
    while (rep != NULL) {
        struct hu_func_replace *copy = kmalloc(sizeof(*copy));
        if (copy != NULL) {
            memcpy(copy, rep, sizeof(*copy));
            copy->next = snap->replacements;
            snap->replacements = copy;
            count++;
        }
        rep = rep->next;
    }
    snap->replacement_count = count;
    spin_unlock(&func_replace_lock);

    /* 添加到链表 */
    spin_lock(&version_lock);
    snap->next = version_snapshots;
    version_snapshots = snap;
    spin_unlock(&version_lock);

    print("[HOTUPDATE] Version snapshot #");
    print_dec(snap->version);
    print(" created: ");
    println(description ? description : "");

    return snap->version;
}

/* 回滚到指定版本 */
int hu_version_rollback(uint32_t version) {
    spin_lock(&version_lock);

    struct hu_version_snapshot *snap = version_snapshots;
    while (snap != NULL) {
        if (snap->version == version) {
            break;
        }
        snap = snap->next;
    }

    if (snap == NULL) {
        spin_unlock(&version_lock);
        print("[HOTUPDATE] Version #");
        print_dec(version);
        println(" not found");
        return -1;
    }

    spin_unlock(&version_lock);

    print("[HOTUPDATE] Rolling back to version #");
    print_dec(version);
    println("...");

    /* 卸载所有当前替换 */
    spin_lock(&func_replace_lock);
    struct hu_func_replace *rep = func_replace_list;
    while (rep != NULL) {
        /* 恢复原始指令 */
        memcpy((void *)rep->target_addr, rep->original_code, rep->original_size);
        rep = rep->next;
    }
    spin_unlock(&func_replace_lock);

    /* 重新安装快照中的替换 */
    rep = snap->replacements;
    while (rep != NULL) {
        hu_func_replace_install(rep->target_addr, rep->replacement_addr,
                                 rep->target_name, rep->replacement_name);
        rep = rep->next;
    }

    hu_snapshot_version = version;

    print("[HOTUPDATE] Rollback to version #");
    print_dec(version);
    println(" complete");

    return 0;
}

/* 列出所有版本快照 */
int hu_version_list(struct hu_version_snapshot **list, int *count) {
    if (list == NULL || count == NULL) return -1;

    spin_lock(&version_lock);

    int n = 0;
    struct hu_version_snapshot *snap = version_snapshots;
    while (snap != NULL) {
        n++;
        snap = snap->next;
    }

    *count = n;
    *list = version_snapshots;

    spin_unlock(&version_lock);
    return 0;
}

/* 删除版本快照 */
int hu_version_delete(uint32_t version) {
    spin_lock(&version_lock);

    struct hu_version_snapshot *snap = version_snapshots;
    struct hu_version_snapshot *prev = NULL;

    while (snap != NULL) {
        if (snap->version == version) {
            if (prev == NULL) {
                version_snapshots = snap->next;
            } else {
                prev->next = snap->next;
            }

            /* 释放替换记录 */
            struct hu_func_replace *rep = snap->replacements;
            while (rep != NULL) {
                struct hu_func_replace *next = rep->next;
                kfree(rep);
                rep = next;
            }

            kfree(snap);
            spin_unlock(&version_lock);
            return 0;
        }
        prev = snap;
        snap = snap->next;
    }

    spin_unlock(&version_lock);
    return -1;
}

/* 获取当前版本号 */
uint32_t hu_version_current(void) {
    return hu_snapshot_version;
}

/* ========== 依赖检查 ========== */

/* 内核符号表 */
#define HU_MAX_SYMBOLS  512

struct hu_symbol_entry {
    char name[64];
    uint64_t addr;
};

static struct hu_symbol_entry hu_symbols[HU_MAX_SYMBOLS];
static int hu_symbol_count = 0;
static spinlock_t hu_symbol_lock = SPINLOCK_INIT;

/* 注册内核符号 */
int hu_symbol_register(const char *name, uint64_t addr) {
    if (name == NULL || addr == 0) return -1;

    spin_lock(&hu_symbol_lock);

    if (hu_symbol_count >= HU_MAX_SYMBOLS) {
        spin_unlock(&hu_symbol_lock);
        return -1;  /* 符号表已满 */
    }

    /* 检查是否已注册 */
    for (int i = 0; i < hu_symbol_count; i++) {
        int match = 1;
        for (int j = 0; j < 63 && (name[j] || hu_symbols[i].name[j]); j++) {
            if (name[j] != hu_symbols[i].name[j]) { match = 0; break; }
        }
        if (match) {
            /* 更新地址 */
            hu_symbols[i].addr = addr;
            spin_unlock(&hu_symbol_lock);
            return 0;
        }
    }

    /* 添加新符号 */
    struct hu_symbol_entry *entry = &hu_symbols[hu_symbol_count];
    int i = 0;
    while (name[i] && i < 63) {
        entry->name[i] = name[i];
        i++;
    }
    entry->name[i] = '\0';
    entry->addr = addr;
    hu_symbol_count++;

    spin_unlock(&hu_symbol_lock);
    return 0;
}

/* 查找内核符号地址 */
uint64_t hu_symbol_lookup(const char *name) {
    if (name == NULL) return 0;

    spin_lock(&hu_symbol_lock);

    for (int i = 0; i < hu_symbol_count; i++) {
        int match = 1;
        for (int j = 0; j < 63 && (name[j] || hu_symbols[i].name[j]); j++) {
            if (name[j] != hu_symbols[i].name[j]) { match = 0; break; }
        }
        if (match) {
            uint64_t addr = hu_symbols[i].addr;
            spin_unlock(&hu_symbol_lock);
            return addr;
        }
    }

    spin_unlock(&hu_symbol_lock);
    return 0;
}

/* 检查符号是否在内核中可用 */
int hu_symbol_available(const char *name) {
    return hu_symbol_lookup(name) != 0;
}

/* 批量检查依赖 */
int hu_check_dependencies(struct hu_symbol_dep *deps, int count,
                           struct hu_dep_check_result *result) {
    if (deps == NULL || result == NULL) return -1;

    memset(result, 0, sizeof(*result));
    result->total_deps = count;

    for (int i = 0; i < count; i++) {
        int found = hu_symbol_available(deps[i].name);
        deps[i].found = found;

        if (found) {
            result->satisfied_deps++;
        } else {
            if (deps[i].required) {
                result->missing_required++;
                /* 记录缺失的符号名 */
                if (result->missing_required <= 16) {
                    int j = 0;
                    while (deps[i].name[j] && j < 63) {
                        result->missing_names[result->missing_required - 1][j] = deps[i].name[j];
                        j++;
                    }
                    result->missing_names[result->missing_required - 1][j] = '\0';
                }
            } else {
                result->missing_optional++;
            }
        }
    }

    result->all_satisfied = (result->missing_required == 0);

    if (!result->all_satisfied) {
        print("[HOTUPDATE] Dependency check failed: ");
        print_dec(result->missing_required);
        println(" required symbols missing");
    }

    return result->all_satisfied ? 0 : -1;
}
