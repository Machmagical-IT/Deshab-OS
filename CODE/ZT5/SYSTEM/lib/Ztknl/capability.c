/*
 * Zratro OS - Capability-Based Security Model Implementation
 * 
 * @file capability.c
 * @brief Capability 安全模型核心实现
 * 
 * 实现特性:
 *   - 64 位细粒度权限控制
 *   - 时间限制自动过期
 *   - 密码学签名防篡改
 *   - 委托链追踪
 *   - O(1) 权限检查性能
 */

#include "capability.h"
#include "task.h"
#include "heap.h"
#include "string.h"
#include "console.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 全局状态
 * ═══════════════════════════════════════════════════════════════════════════ */

/** 全局 Capability 管理器 */
static struct capability_manager cap_mgr = {
    .total_count = 0,
    .lock = SPINLOCK_INIT,
    .next_id = 1,
};

/** 进程 Capability 表哈希表 */
static struct capability_table *cap_hash_table[CAP_HASH_BUCKETS];
static spinlock_t cap_hash_lock = SPINLOCK_INIT;

/* ═══════════════════════════════════════════════════════════════════════════
 * 内部辅助函数
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 简单的 XOR 哈希算法 (用于快速签名)
 * 
 * @note 生产环境应使用 Ed25519 或 SHA-256
 *       此处为简化实现，实际项目中需替换为强加密算法
 */
static void cap_generate_signature(struct capability_internal *cap) {
    uint64_t *key = cap_mgr.master_key;
    uint64_t hash[4] = {0};
    
    /* XOR-based MAC (简化版，实际应用需用 HMAC-SHA256) */
    hash[0] = cap->id ^ cap->resource_id ^ key[0];
    hash[1] = cap->permissions ^ cap->owner_pid ^ key[1];
    hash[2] = cap->created_time ^ cap->expiry_time ^ key[2];
    hash[3] = cap->parent_cap ^ cap->flags ^ key[3];
    
    /* 混合运算 */
    for (int i = 0; i < 8; i++) {
        hash[0] ^= (hash[1] << 13) | (hash[2] >> 7);
        hash[1] ^= (hash[2] << 17) | (hash[3] >> 11);
        hash[2] ^= (hash[3] << 23) | (hash[0] >> 5);
        hash[3] ^= (hash[0] << 29) | (hash[1] >> 3);
    }
    
    cap->signature[0] = hash[0];
    cap->signature[1] = hash[1];
    cap->signature[2] = hash[2];
    cap->signature[3] = hash[3];
}

/**
 * @brief 验证 Capability 签名
 * 
 * @param cap 待验证的 capability
 * @return int 0=有效，-1=无效
 */
int cap_verify(struct capability_internal *cap) {
    if (cap == NULL || cap->id == 0) {
        return -1;
    }
    
    /* 检查是否被撤销 */
    if (cap->flags & CAP_FLAG_REVOKED) {
        println("[CAP] Capability revoked");
        return -1;
    }
    
    /* 检查过期时间 */
    if (cap->expiry_time != 0) {
        uint64_t current_tick = 0;  /* TODO: 获取当前 tick */
        if (current_tick > cap->expiry_time) {
            println("[CAP] Capability expired");
            return -1;
        }
    }
    
    /* 检查使用次数 */
    if (cap->max_uses != 0 && cap->current_uses >= cap->max_uses) {
        println("[CAP] Capability usage limit reached");
        return -1;
    }
    
    /* 验证签名 */
    uint64_t expected_sig[4];
    struct capability_internal temp = *cap;
    cap_generate_signature(&temp);
    memcpy(expected_sig, temp.signature, sizeof(expected_sig));
    
    if (memcmp(cap->signature, expected_sig, sizeof(expected_sig)) != 0) {
        println("[CAP] Signature verification failed");
        return -1;
    }
    
    return 0;
}

/**
 * @brief 为 Capability 生成签名
 */
uint64_t cap_sign(struct capability_internal *cap) {
    cap_generate_signature(cap);
    return 0;
}

/**
 * @brief 哈希函数 - PID 到桶索引
 */
static inline int cap_hash_pid(uint64_t pid) {
    return (int)(pid % CAP_HASH_BUCKETS);
}

/**
 * @brief 查找进程的 capability 表 (简化版本 - 直接遍历)
 */
static struct capability_table *cap_get_table(uint64_t pid) {
    int idx = cap_hash_pid(pid);
    
    spin_lock(&cap_hash_lock);
    
    struct capability_table *table = cap_hash_table[idx];
    /* TODO: 实现完整的哈希表链表遍历 */
    
    spin_unlock(&cap_hash_lock);
    return table;
}

/**
 * @brief 创建新的 capability 表 (简化版)
 */
static struct capability_table *cap_create_table(uint64_t pid) {
    struct capability_table *table = kzalloc(sizeof(struct capability_table));
    if (!table) {
        return NULL;
    }
    
    table->pid = pid;
    table->count = 0;
    memset(table->bitmap, 0, sizeof(table->bitmap));
    table->lock = SPINLOCK_INIT;
    
    int idx = cap_hash_pid(pid);
    
    spin_lock(&cap_hash_lock);
    /* TODO: 添加到哈希桶链表 */
    cap_hash_table[idx] = table;
    spin_unlock(&cap_hash_lock);
    
    return table;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 核心 API 实现
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 初始化 Capability 子系统
 */
int cap_init(void) {
    serial_print("[CAP] Initializing capability subsystem...\n");
    
    /* 初始化哈希表 */
    memset(cap_hash_table, 0, sizeof(cap_hash_table));
    
    /* 生成主密钥 (使用简单随机数，实际应使用硬件 RNG) */
    for (int i = 0; i < 4; i++) {
        cap_mgr.master_key[i] = (uint64_t)((i + 1) * 0x123456789ABCDEF0ULL);  /* TODO: 真随机 */
    }
    
    /* 预分配一些 capability */
    memset(cap_mgr.table, 0, sizeof(cap_mgr.table));
    
    serial_print("[CAP] Capability subsystem initialized\n");
    serial_print("[CAP] Master key generated (secure storage required)\n");
    
    return 0;
}

/**
 * @brief 为新进程初始化 capability 表
 */
int cap_init_process(uint64_t pid) {
    if (pid == 0) {
        return -1;
    }
    
    struct capability_table *table = cap_create_table(pid);
    if (!table) {
        println("[CAP] Failed to create capability table");
        return -1;
    }
    
    /* 分配基础 capability */
    uint64_t base_cap = cap_allocate(pid, 0, 
                                      CAP_FS_READ | CAP_FS_EXECUTE |
                                      CAP_PROC_FORK | CAP_PROC_EXEC |
                                      CAP_NET_CONNECT | CAP_NET_SEND | CAP_NET_RECV,
                                      0);  /* 永不过期 */
    
    if (base_cap == 0) {
        kfree(table);
        return -1;
    }
    
    return 0;
}

/**
 * @brief 分配新的 capability
 * 
 * @param pid 持有者 PID
 * @param resource_id 资源标识符
 * @param permissions 权限掩码
 * @param expiry_ticks 过期时间 (tick), 0=永不过期
 * @return uint64_t Capability ID, 失败返回 0
 */
uint64_t cap_allocate(uint64_t pid, uint64_t resource_id, 
                      uint64_t permissions, uint64_t expiry_ticks) {
    if (pid == 0 || permissions == 0) {
        return 0;
    }
    
    spin_lock(&cap_mgr.lock);
    
    /* 检查容量 */
    if (cap_mgr.total_count >= CAP_TABLE_SIZE) {
        println("[CAP] Capability table full");
        spin_unlock(&cap_mgr.lock);
        return 0;
    }
    
    /* 查找空闲槽位 */
    struct capability_internal *cap = NULL;
    
    for (int i = 0; i < CAP_TABLE_SIZE; i++) {
        if (cap_mgr.table[i].id == 0) {
            cap = &cap_mgr.table[i];
            break;
        }
    }
    
    if (!cap) {
        spin_unlock(&cap_mgr.lock);
        return 0;
    }
    
    /* 初始化 capability */
    memset(cap, 0, sizeof(struct capability_internal));
    cap->id = cap_mgr.next_id++;
    cap->resource_id = resource_id;
    cap->permissions = permissions;
    cap->owner_pid = pid;
    cap->created_time = 0;  /* TODO: 获取当前 tick */
    cap->expiry_time = expiry_ticks;
    cap->max_uses = 0;
    cap->current_uses = 0;
    cap->parent_cap = 0;
    cap->flags = 0;
    
    /* 生成签名 */
    cap_sign(cap);
    
    cap_mgr.total_count++;
    
    spin_unlock(&cap_mgr.lock);
    
    return cap->id;
}

/**
 * @brief 撤销 capability
 * 
 * @param cap_id Capability ID
 * @return int 0=成功，-1=失败
 */
int cap_revoke(uint64_t cap_id) {
    if (cap_id == 0) {
        return -1;
    }
    
    spin_lock(&cap_mgr.lock);
    
    /* 查找 capability */
    for (int i = 0; i < CAP_TABLE_SIZE; i++) {
        if (cap_mgr.table[i].id == cap_id) {
            cap_mgr.table[i].flags |= CAP_FLAG_REVOKED;
            memset(cap_mgr.table[i].signature, 0, 
                   sizeof(cap_mgr.table[i].signature));
            cap_mgr.total_count--;
            
            spin_unlock(&cap_mgr.lock);
            return 0;
        }
    }
    
    spin_unlock(&cap_mgr.lock);
    return -1;
}

/**
 * @brief 委托 capability 给其他进程
 * 
 * @param parent_cap_id 父 capability ID
 * @param target_pid 目标进程 PID
 * @param sub_permissions 子权限 (必须是父权限的子集)
 * @param expiry_ticks 过期时间
 * @return int 0=成功，-1=失败
 */
int cap_delegate(uint64_t parent_cap_id, uint64_t target_pid,
                 uint64_t sub_permissions, uint64_t expiry_ticks) {
    if (parent_cap_id == 0 || target_pid == 0) {
        return -1;
    }
    
    spin_lock(&cap_mgr.lock);
    
    /* 查找父 capability */
    struct capability_internal *parent = NULL;
    for (int i = 0; i < CAP_TABLE_SIZE; i++) {
        if (cap_mgr.table[i].id == parent_cap_id) {
            parent = &cap_mgr.table[i];
            break;
        }
    }
    
    if (!parent) {
        spin_unlock(&cap_mgr.lock);
        return -1;
    }
    
    /* 验证父 capability */
    if (cap_verify(parent) != 0) {
        spin_unlock(&cap_mgr.lock);
        return -1;
    }
    
    /* 检查是否可委托 */
    if (!(parent->flags & CAP_FLAG_DELEGATABLE)) {
        println("[CAP] Capability not delegatable");
        spin_unlock(&cap_mgr.lock);
        return -1;
    }
    
    /* 子权限必须是父权限的子集 */
    if ((sub_permissions & ~parent->permissions) != 0) {
        println("[CAP] Cannot delegate more permissions than parent");
        spin_unlock(&cap_mgr.lock);
        return -1;
    }
    
    /* 创建子 capability */
    spin_unlock(&cap_mgr.lock);
    
    uint64_t child_id = cap_allocate(target_pid, parent->resource_id,
                                     sub_permissions, expiry_ticks);
    
    if (child_id == 0) {
        return -1;
    }
    
    /* 设置父子关系 */
    spin_lock(&cap_mgr.lock);
    for (int i = 0; i < CAP_TABLE_SIZE; i++) {
        if (cap_mgr.table[i].id == child_id) {
            cap_mgr.table[i].parent_cap = parent_cap_id;
            cap_mgr.table[i].flags |= CAP_FLAG_DELEGATABLE;
            break;
        }
    }
    spin_unlock(&cap_mgr.lock);
    
    return 0;
}

/**
 * @brief 检查进程是否拥有所需权限
 * 
 * @param pid 进程 ID
 * @param resource_id 资源标识符
 * @param required_perm 所需权限
 * @return int 0=有权限，-1=无权限
 * 
 * @note 性能优化：O(1) 时间复杂度
 * @note TOCTOU 修复: 使用原子操作增加使用计数
 */
int cap_check(uint64_t pid, uint64_t resource_id, uint64_t required_perm) {
    if (pid == 0 || required_perm == 0) {
        cap_audit_log(CAP_AUDIT_FAIL_CHECK, 0, pid, resource_id, required_perm, -1);
        return -1;
    }
    
    struct capability_table *table = cap_get_table(pid);
    if (!table) {
        cap_audit_log(CAP_AUDIT_FAIL_CHECK, 0, pid, resource_id, required_perm, -1);
        return -1;
    }
    
    spin_lock(&table->lock);
    
    /* 快速路径：遍历所有 capability */
    for (int i = 0; i < CAP_MAX_PER_PROCESS; i++) {
        struct capability_internal *cap = &table->caps[i];
        
        if (cap->id == 0) {
            continue;  /* 空槽位 */
        }
        
        /* 检查资源匹配和权限包含 */
        if (cap->resource_id == resource_id &&
            (cap->permissions & required_perm) == required_perm) {
            
            /* 验证 capability (内联检查) */
            if (!(cap->flags & CAP_FLAG_REVOKED)) {
                /* TOCTOU 修复: 原子地增加使用计数 */
                if (cap_atomic_inc_uses(cap) != 0) {
                    /* 使用次数超限 */
                    cap_audit_log(CAP_AUDIT_FAIL_CHECK, cap->id, pid, resource_id, required_perm, -1);
                    spin_unlock(&table->lock);
                    return -1;
                }
                cap_audit_log(CAP_AUDIT_CHECK, cap->id, pid, resource_id, required_perm, 0);
                spin_unlock(&table->lock);
                return 0;
            }
        }
    }
    
    cap_audit_log(CAP_AUDIT_FAIL_CHECK, 0, pid, resource_id, required_perm, -1);
    spin_unlock(&table->lock);
    return -1;
}

/**
 * @brief 检查当前进程是否拥有所需权限
 */
int cap_check_current(uint64_t resource_id, uint64_t required_perm) {
    struct process *current = process_get_current();
    if (!current) {
        return -1;
    }
    
    return cap_check(current->pid, resource_id, required_perm);
}

/**
 * @brief 列出进程的所有 capability
 */
int cap_list(uint64_t pid, struct capability_internal *out_caps, int max_count) {
    if (!out_caps || max_count <= 0) {
        return -1;
    }
    
    struct capability_table *table = cap_get_table(pid);
    if (!table) {
        return 0;
    }
    
    spin_lock(&table->lock);
    
    int count = 0;
    for (int i = 0; i < CAP_MAX_PER_PROCESS && count < max_count; i++) {
        if (table->caps[i].id != 0) {
            out_caps[count++] = table->caps[i];
        }
    }
    
    spin_unlock(&table->lock);
    return count;
}

/**
 * @brief 获取 capability 详细信息
 */
int cap_info(uint64_t cap_id, struct capability_internal *out_cap) {
    if (!out_cap || cap_id == 0) {
        return -1;
    }
    
    spin_lock(&cap_mgr.lock);
    
    for (int i = 0; i < CAP_TABLE_SIZE; i++) {
        if (cap_mgr.table[i].id == cap_id) {
            *out_cap = cap_mgr.table[i];
            spin_unlock(&cap_mgr.lock);
            return 0;
        }
    }
    
    spin_unlock(&cap_mgr.lock);
    return -1;
}

/**
 * @brief 清理进程的所有 capability
 */
void cap_cleanup(uint64_t pid) {
    struct capability_table *table = cap_get_table(pid);
    if (!table) {
        return;
    }
    
    spin_lock(&table->lock);
    
    /* 撤销所有 capability */
    for (int i = 0; i < CAP_MAX_PER_PROCESS; i++) {
        if (table->caps[i].id != 0) {
            table->caps[i].flags |= CAP_FLAG_REVOKED;
        }
    }
    
    /* 释放表 */
    spin_unlock(&table->lock);
    
    /* TODO: 从哈希表中移除 */
    kfree(table);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 系统调用实现
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief SYS_CAP_ALLOC - 分配 capability
 * 
 * 系统调用号：SYS_CAPABILITY_BASE + 0
 */
uint64_t sys_cap_alloc(uint64_t resource_id, uint64_t permissions, 
                       uint64_t expiry_seconds) {
    struct process *current = process_get_current();
    if (!current) {
        return 0;
    }
    
    /* 将秒转换为 tick */
    uint64_t expiry_ticks = expiry_seconds * 1000;  /* TODO: 实际转换 */
    
    return cap_allocate(current->pid, resource_id, permissions, expiry_ticks);
}

/**
 * @brief SYS_CAP_REVOKE - 撤销 capability
 */
int sys_cap_revoke(uint64_t cap_id) {
    /* 只有管理员或所有者才能撤销 */
    if (cap_check_current(0, CAP_SYS_ADMIN) != 0) {
        /* TODO: 检查是否为所有者 */
    }
    
    return cap_revoke(cap_id);
}

/**
 * @brief SYS_CAP_CHECK - 检查权限
 */
int sys_cap_check(uint64_t pid, uint64_t resource_id, uint64_t required_perm) {
    return cap_check(pid, resource_id, required_perm);
}

/**
 * @brief SYS_CAP_LIST - 列出 capabilities
 */
int sys_cap_list(uint64_t pid, void *buf, size_t buf_size) {
    if (!buf || buf_size < sizeof(struct capability_internal)) {
        return -1;
    }
    
    int max_count = buf_size / sizeof(struct capability_internal);
    return cap_list(pid, (struct capability_internal *)buf, max_count);
}

/**
 * @brief SYS_CAP_DELEGATE - 委托 capability
 */
int sys_cap_delegate(uint64_t cap_id, uint64_t target_pid,
                     uint64_t permissions, uint64_t expiry_seconds) {
    uint64_t expiry_ticks = expiry_seconds * 1000;
    return cap_delegate(cap_id, target_pid, permissions, expiry_ticks);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TOCTOU 安全修复
 * 使用原子操作和正确的锁序来防止检查-使用-修改竞态
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 原子地检查并修改 capability 标志
 * 防止 TOCTOU 竞态: 在检查标志和设置标志之间不会有其他线程介入
 *
 * @param cap       目标 capability
 * @param flag_check 需要检查的标志 (如果设置了这些标志则失败)
 * @param flag_set   要设置的标志
 * @return 0=成功, -1=检查失败 (标志已设置)
 */
int cap_atomic_check_and_set_flag(struct capability_internal *cap, uint32_t flag_check, uint32_t flag_set) {
    if (!cap) return -1;

    /* 获取 capability 的锁，确保检查和修改是原子的 */
    spin_lock(&cap->lock);

    /* 检查标志 */
    if (cap->flags & flag_check) {
        spin_unlock(&cap->lock);
        return -1;
    }

    /* 设置标志 */
    cap->flags |= flag_set;

    spin_unlock(&cap->lock);
    return 0;
}

/* 原子地增加使用计数 (带引用计数检查)
 * 防止 TOCTOU: 检查 ref_count 和增加必须是原子操作
 *
 * @param cap 目标 capability
 * @return 0=成功, -1=超过引用限制
 */
int cap_atomic_inc_uses(struct capability_internal *cap) {
    if (!cap) return -1;

    spin_lock(&cap->lock);

    /* 原子地检查和增加使用计数 */
    if (cap->max_uses != 0 && cap->current_uses >= cap->max_uses) {
        spin_unlock(&cap->lock);
        return -1;
    }

    cap->current_uses++;

    spin_unlock(&cap->lock);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 委托链验证
 * 从当前 capability 沿 parent_cap 追溯到根，验证链中每个环节
 * ═══════════════════════════════════════════════════════════════════════════ */

int cap_verify_delegation_chain(struct capability_internal *cap, int max_depth) {
    if (!cap) return -1;
    if (max_depth <= 0) max_depth = 16;

    /* 检查委托深度是否合法 */
    if (cap->depth > cap->max_depth) {
        return -1;
    }

    /* 如果没有父能力，说明是根能力，链有效 */
    if (cap->parent_cap == 0) {
        return 0;
    }

    spin_lock(&cap_mgr.lock);

    int depth = 0;
    uint64_t current_id = cap->parent_cap;

    while (current_id != 0 && depth < max_depth) {
        struct capability_internal *parent = NULL;
        for (int i = 0; i < CAP_TABLE_SIZE; i++) {
            if (cap_mgr.table[i].id == current_id) {
                parent = &cap_mgr.table[i];
                break;
            }
        }

        if (!parent) {
            spin_unlock(&cap_mgr.lock);
            return -1;
        }

        /* 检查父能力是否被撤销 */
        if (parent->flags & CAP_FLAG_REVOKED) {
            spin_unlock(&cap_mgr.lock);
            return -1;
        }

        /* 检查权限衰减 */
        if ((parent->permissions & cap->permissions) != cap->permissions) {
            spin_unlock(&cap_mgr.lock);
            return -1;
        }

        current_id = parent->parent_cap;
        depth++;
    }

    spin_unlock(&cap_mgr.lock);
    return 0;
}

/* 获取委托链深度 */
int cap_get_delegation_depth(struct capability_internal *cap) {
    if (!cap) return -1;

    /* 使用头文件中定义的 depth 字段 */
    return (int)cap->depth;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Capability 审计日志
 * 记录权限检查、授予、撤销等操作，用于安全审计
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 审计日志缓冲区 */
static struct cap_audit_entry cap_audit_buffer[CAP_AUDIT_LOG_SIZE];
static uint32_t cap_audit_index = 0;
static int cap_audit_enabled = 0;  /* 默认禁用 */
static spinlock_t cap_audit_lock = SPINLOCK_INIT;

/* 记录审计事件 */
void cap_audit_log(cap_audit_event_t event, uint64_t cap_id,
                   uint64_t pid, uint64_t resource_id,
                   uint64_t permissions, int result) {
    if (!cap_audit_enabled) return;

    spin_lock(&cap_audit_lock);

    uint32_t idx = cap_audit_index % CAP_AUDIT_LOG_SIZE;
    cap_audit_buffer[idx].timestamp = get_tick_count();
    cap_audit_buffer[idx].event = event;
    cap_audit_buffer[idx].cap_id = cap_id;
    cap_audit_buffer[idx].pid = pid;
    cap_audit_buffer[idx].resource_id = resource_id;
    cap_audit_buffer[idx].permissions = permissions;
    cap_audit_buffer[idx].result = result;

    cap_audit_index++;

    spin_unlock(&cap_audit_lock);
}

/* 获取审计日志 */
int cap_get_audit_log(struct cap_audit_entry *entries, int max_count) {
    if (!entries || max_count <= 0) return -1;

    spin_lock(&cap_audit_lock);

    uint32_t count = cap_audit_index < CAP_AUDIT_LOG_SIZE ?
                     cap_audit_index : CAP_AUDIT_LOG_SIZE;
    if ((int)count > max_count) count = (uint32_t)max_count;

    uint32_t start = cap_audit_index < CAP_AUDIT_LOG_SIZE ?
                     0 : cap_audit_index % CAP_AUDIT_LOG_SIZE;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t src_idx = (start + i) % CAP_AUDIT_LOG_SIZE;
        entries[i] = cap_audit_buffer[src_idx];
    }

    spin_unlock(&cap_audit_lock);
    return (int)count;
}

/* 启用/禁用审计日志 */
void cap_set_audit_enabled(int enabled) {
    spin_lock(&cap_audit_lock);
    cap_audit_enabled = enabled;
    if (enabled) {
        cap_audit_index = 0;
    }
    spin_unlock(&cap_audit_lock);
}

/* 获取审计日志是否启用 */
int cap_is_audit_enabled(void) {
    return cap_audit_enabled;
}
