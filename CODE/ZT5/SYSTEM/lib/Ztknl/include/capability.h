/*
 * Zratro OS - Capability (能力安全系统)
 *
 * 创新设计：
 * 1. 能力令牌 - 取代传统 ACL/DAC
 * 2. 权限委托 - 受限权限传递
 * 3. 单调衰减 - 权限只能减少
 * 4. 无伪造 - 密码学签名保护
 * 5. 信息流控制 - 防止数据泄露
 */

#ifndef CAPABILITY_H
#define CAPABILITY_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== 能力配置 ========== */

#define CAP_MAX_RIGHTS          64          /* 最大权限位数 */
#define CAP_MAX_TYPES           32          /* 最大对象类型数 */
#define CAP_MAX_CAPS_PER_PROC   256         /* 每进程最大能力数 */
#define CAP_NAME_LEN            32          /* 能力名称长度 */
#define CAP_SIGNATURE_SIZE      32          /* 签名大小 */

#define CAP_TABLE_SIZE          4096        /* 全局能力表大小 */
#define CAP_MAX_PER_PROCESS     256         /* 每进程最大能力数 (内部) */
#define CAP_HASH_BUCKETS        256         /* 哈希桶数量 */

/* ========== 权限定义 ========== */

/* 通用权限位 */
#define CAP_RIGHT_READ          (1ULL << 0)    /* 读取 */
#define CAP_RIGHT_WRITE         (1ULL << 1)    /* 写入 */
#define CAP_RIGHT_EXECUTE       (1ULL << 2)    /* 执行 */
#define CAP_RIGHT_APPEND        (1ULL << 3)    /* 追加 */
#define CAP_RIGHT_DELETE        (1ULL << 4)    /* 删除 */
#define CAP_RIGHT_CREATE        (1ULL << 5)    /* 创建 */
#define CAP_RIGHT_LIST          (1ULL << 6)    /* 列表 */
#define CAP_RIGHT_ADMIN         (1ULL << 7)    /* 管理 */
#define CAP_RIGHT_DELEGATE      (1ULL << 8)    /* 委托 */
#define CAP_RIGHT_REVOKE        (1ULL << 9)    /* 撤销 */
#define CAP_RIGHT_GRANT         (1ULL << 10)   /* 授予 */

/* 特殊权限 */
#define CAP_RIGHT_ALL           0xFFFFFFFFFFFFFFFFULL  /* 所有权限 */
#define CAP_RIGHT_NONE          0ULL

/* 内部权限位 (capability.c 使用) */
#define CAP_FS_READ              (1ULL << 0)
#define CAP_FS_WRITE             (1ULL << 1)
#define CAP_FS_EXECUTE           (1ULL << 2)
#define CAP_PROC_FORK            (1ULL << 10)
#define CAP_PROC_EXEC            (1ULL << 11)
#define CAP_NET_CONNECT          (1ULL << 20)
#define CAP_NET_SEND             (1ULL << 21)
#define CAP_NET_RECV             (1ULL << 22)
#define CAP_HW_IO               (1ULL << 48)  /* I/O 端口访问 */
#define CAP_HW_DMA              (1ULL << 49)  /* DMA 访问 */
#define CAP_HW_MMIO             (1ULL << 50)  /* 内存映射 I/O */
#define CAP_HW_PCI              (1ULL << 51)  /* PCI 配置 */
#define CAP_HW_USB              (1ULL << 52)  /* USB 访问 */
#define CAP_HW_ADMIN            (1ULL << 63)  /* 硬件访问管理员 */
#define CAP_SYS_ADMIN            (1ULL << 60)

/* ========== 对象类型 ========== */

typedef enum {
    CAP_TYPE_NONE = 0,
    CAP_TYPE_FILE,               /* 文件 */
    CAP_TYPE_DIRECTORY,          /* 目录 */
    CAP_TYPE_PROCESS,            /* 进程 */
    CAP_TYPE_THREAD,             /* 线程 */
    CAP_TYPE_MEMORY,             /* 内存区域 */
    CAP_TYPE_PORT,               /* 端口 (IPC) */
    CAP_TYPE_SOCKET,             /* 套接字 */
    CAP_TYPE_DEVICE,             /* 设备 */
    CAP_TYPE_PMEM_SEGMENT,       /* 持久内存段 */
    CAP_TYPE_ZPIPE_CHANNEL,      /* 零拷贝管道通道 */
    CAP_TYPE_ZRING,              /* 异步 I/O 环 */
    CAP_TYPE_NAMESPACE,          /* 命名空间 */
    CAP_TYPE_CGROUP,             /* 控制组 */
    CAP_TYPE_KEY,                /* 密钥 */
    CAP_TYPE_MAX
} cap_type_t;

/* ========== 能力句柄 ========== */

/* 能力句柄 (用户空间可见) */
typedef uint64_t cap_handle_t;

#define CAP_INVALID_HANDLE      0

/* ========== 能力标志 ========== */

#define CAP_FLAG_REVOKED       (1 << 0)   /* 已撤销 */
#define CAP_FLAG_EXPIRED       (1 << 1)   /* 已过期 */
#define CAP_FLAG_DELEGATED     (1 << 2)   /* 委托得来 */
#define CAP_FLAG_UNREVOKABLE   (1 << 3)   /* 不可撤销 */
#define CAP_FLAG_NO_DELEGATE   (1 << 4)   /* 不可委托 */
#define CAP_FLAG_DELEGATABLE   (1 << 5)   /* 可委托 */

/* ========== 内部能力结构 (capability.c 使用的扩展版本) ========== */

/* 内部能力结构 */
struct capability_internal {
    uint64_t id;                 /* 能力 ID */
    uint64_t resource_id;        /* 资源 ID */
    uint64_t permissions;        /* 权限位图 */
    uint32_t flags;              /* 标志 */
    uint64_t parent_cap;         /* 父能力 ID */
    uint64_t owner_pid;          /* 所有者 PID */
    uint64_t max_uses;           /* 最大使用次数 (0=无限) */
    uint64_t current_uses;       /* 当前使用次数 */
    uint64_t created_time;       /* 创建时间 */
    uint64_t expiry_time;        /* 过期时间 */
    uint64_t signature[4];       /* 签名 */
    uint64_t authenticator;      /* 认证器 */
    uint32_t depth;              /* 委托深度 */
    uint32_t max_depth;          /* 最大委托深度 */
    spinlock_t lock;             /* 操作锁 */
};

/* 内部能力管理器 */
struct capability_manager {
    uint64_t total_count;
    spinlock_t lock;
    uint64_t next_id;
    struct capability_internal table[CAP_TABLE_SIZE];
    uint64_t master_key[4];
    struct list_head all_caps;
};

/* 内部能力表 (per-process) */
struct capability_table {
    uint64_t pid;                    /* 进程 PID */
    struct capability_internal caps[CAP_MAX_PER_PROCESS];
    uint32_t count;
    uint8_t bitmap[CAP_MAX_PER_PROCESS / 8]; /* 能力位图 */
    spinlock_t lock;
    struct capability_table *hash_next;
};

/* ========== 能力描述符 (公共结构) ========== */

/* 能力描述符 (内核内部公共接口) */
struct capability {
    cap_handle_t handle;         /* 能力句柄 */
    uint64_t object_id;          /* 对象 ID */
    cap_type_t object_type;      /* 对象类型 */
    uint64_t rights;             /* 权限位图 */

    /* 权限衰减 (委托时使用) */
    uint64_t max_rights;         /* 最大可委托权限 */
    uint32_t depth;              /* 委托深度 */
    uint32_t max_depth;          /* 最大委托深度 */

    /* 时间限制 */
    uint64_t create_time;        /* 创建时间 */
    uint64_t expiry_time;        /* 过期时间 */

    /* 安全标签 (信息流控制) */
    uint32_t security_label;     /* 安全标签 (如机密级别) */
    uint32_t integrity_label;    /* 完整性标签 */

    /* 完整性保护 */
    uint64_t authenticator;      /* 认证器 (简单实现) */
    uint32_t flags;              /* 标志 */
    uint32_t ref_count;          /* 引用计数 */

    /* 所有者 */
    pid_t owner_pid;             /* 所有者 PID */

    /* 链表 */
    struct list_head list;       /* 进程能力链表 */
    struct list_head global_list; /* 全局能力链表 */

    /* 锁 */
    spinlock_t lock;
};

/* ========== 公共能力表 ========== */

/* 进程能力表 (公共接口) */
struct cap_table {
    struct list_head caps;       /* 能力链表 */
    uint32_t count;              /* 能力数 */
    uint32_t max_count;          /* 最大能力数 */

    /* 安全策略 */
    uint32_t default_label;      /* 默认安全标签 */
    uint32_t max_label;          /* 最大可访问标签 */

    /* 锁 */
    spinlock_t lock;
};

/* ========== 全局能力管理器 (公共接口) ========== */

struct cap_manager {
    /* 全局能力链表 */
    struct list_head all_caps;
    uint64_t total_caps;

    /* 句柄分配 */
    cap_handle_t next_handle;

    /* 对象类型表 */
    struct {
        uint64_t default_rights;
        uint64_t max_rights;
        int (*verify)(uint64_t object_id, cap_type_t type);
        int (*revoke)(struct capability *cap);
    } types[CAP_TYPE_MAX];

    /* 安全策略 */
    uint32_t policy_flags;

    /* 统计 */
    uint64_t creates;
    uint64_t delegates;
    uint64_t revokes;
    uint64_t verifications;

    /* 锁 */
    spinlock_t lock;
};

/* 策略标志 */
#define CAP_POLICY_ENFORCE_LABELS  (1 << 0)  /* 强制信息流标签 */
#define CAP_POLICY_AUDIT           (1 << 1)  /* 审计日志 */
#define CAP_POLICY_STRICT_DELEGATE (1 << 2)  /* 严格委托限制 */

/* ========== 核心 API (与 capability.c 实现匹配) ========== */

/* 初始化 */
int cap_init(void);
int cap_init_process(uint64_t pid);

/* 能力分配/撤销 */
uint64_t cap_allocate(uint64_t pid, uint64_t resource_id,
                      uint64_t permissions, uint64_t expiry_ticks);
int cap_revoke(uint64_t cap_id);

/* 能力验证/签名 */
int cap_verify(struct capability_internal *cap);
uint64_t cap_sign(struct capability_internal *cap);

/* 权限检查 */
int cap_check(uint64_t pid, uint64_t resource_id, uint64_t required_perm);
int cap_check_current(uint64_t resource_id, uint64_t required_perm);

/* 能力委托 */
int cap_delegate(uint64_t parent_cap_id, uint64_t target_pid,
                 uint64_t sub_permissions, uint64_t expiry_ticks);

/* 能力查询 */
int cap_list(uint64_t pid, struct capability_internal *out_caps, int max_count);
int cap_info(uint64_t cap_id, struct capability_internal *out_cap);

/* 进程清理 */
void cap_cleanup(uint64_t pid);

/* 原子操作 (TOCTOU 安全) */
int cap_atomic_check_and_set_flag(struct capability_internal *cap, uint32_t flag_check, uint32_t flag_set);
int cap_atomic_inc_uses(struct capability_internal *cap);

/* 委托链验证 */
int cap_verify_delegation_chain(struct capability_internal *cap, int max_depth);
int cap_get_delegation_depth(struct capability_internal *cap);

/* ========== 公共 API (兼容接口) ========== */

/* 初始化/销毁 */
void cap_destroy(struct cap_manager *cm);

/* 进程能力表 */
struct cap_table *cap_table_create(uint32_t max_count);
void cap_table_destroy(struct cap_table *ct);

/* 创建能力 (公共接口) */
cap_handle_t cap_create(struct cap_manager *cm, struct cap_table *ct,
                        uint64_t object_id, cap_type_t type,
                        uint64_t rights, uint32_t flags);

/* 销毁能力 */
int cap_destroy_cap(struct cap_manager *cm, struct cap_table *ct,
                    cap_handle_t handle);

/* 查找能力 */
struct capability *cap_lookup(struct cap_manager *cm, struct cap_table *ct,
                               cap_handle_t handle);

/* 增加引用 */
void cap_get(struct capability *cap);

/* 释放引用 */
void cap_put(struct capability *cap);

/* 检查权限 */
int cap_check_right(struct capability *cap, uint64_t rights);

/* 验证能力 (带对象 ID) */
int cap_verify_object(struct cap_manager *cm, struct cap_table *ct,
                      cap_handle_t handle, uint64_t object_id,
                      cap_type_t type, uint64_t rights);

/* 批量委托 */
int cap_delegate_batch(struct cap_manager *cm,
                        struct cap_table *src_table,
                        struct cap_table *dst_table,
                        cap_handle_t *src_handles,
                        cap_handle_t *dst_handles,
                        uint64_t *rights,
                        uint32_t count);

/* 撤销所有派生能力 */
int cap_revoke_descendants(struct cap_manager *cm, cap_handle_t handle);

/* 撤销进程所有能力 */
int cap_revoke_all(struct cap_manager *cm, struct cap_table *ct);

/* 信息流控制 */
int cap_set_label(struct capability *cap, uint32_t security_label,
                  uint32_t integrity_label);
int cap_check_flow(struct capability *src, struct capability *dst);
int cap_table_set_max_label(struct cap_table *ct, uint32_t max_label);

/* 能力查询 (公共接口) */
int cap_get_info(struct capability *cap, uint64_t *object_id,
                 cap_type_t *type, uint64_t *rights);
void cap_get_stats(struct cap_manager *cm, uint64_t *creates,
                   uint64_t *delegates, uint64_t *revokes);

/* 对象类型注册 */
int cap_register_type(struct cap_manager *cm, cap_type_t type,
                      uint64_t default_rights, uint64_t max_rights,
                      int (*verify)(uint64_t, cap_type_t),
                      int (*revoke)(struct capability *));

/* 全局管理器 */
struct cap_manager *cap_get_global(void);
int cap_global_init(void);

/* ========== Capability 审计日志 ========== */

/* 审计事件类型 */
typedef enum {
    CAP_AUDIT_CHECK = 0,       /* 权限检查 */
    CAP_AUDIT_GRANT,           /* 权限授予 */
    CAP_AUDIT_REVOKE,          /* 权限撤销 */
    CAP_AUDIT_DELEGATE,        /* 权限委托 */
    CAP_AUDIT_VERIFY,          /* 权限验证 */
    CAP_AUDIT_FAIL_CHECK,      /* 权限检查失败 */
    CAP_AUDIT_FAIL_DELEGATE    /* 委托失败 */
} cap_audit_event_t;

/* 审计日志条目 */
struct cap_audit_entry {
    uint64_t timestamp;        /* 时间戳 */
    cap_audit_event_t event;   /* 事件类型 */
    uint64_t cap_id;           /* Capability ID */
    uint64_t pid;              /* 进程 ID */
    uint64_t resource_id;      /* 资源 ID */
    uint64_t permissions;      /* 权限 */
    int result;                /* 结果 (0=成功, -1=失败) */
};

/* 审计日志配置 */
#define CAP_AUDIT_LOG_SIZE     1024    /* 日志缓冲区大小 */

/* 记录审计事件 */
void cap_audit_log(cap_audit_event_t event, uint64_t cap_id,
                   uint64_t pid, uint64_t resource_id,
                   uint64_t permissions, int result);

/* 获取审计日志 */
int cap_get_audit_log(struct cap_audit_entry *entries, int max_count);

/* 启用/禁用审计日志 */
void cap_set_audit_enabled(int enabled);

/* 获取审计日志是否启用 */
int cap_is_audit_enabled(void);

/* ========== 内联辅助函数 ========== */

/* 权限包含检查 */
static inline int cap_rights_contains(uint64_t has, uint64_t needs) {
    return (has & needs) == needs;
}

/* 权限子集检查 */
static inline int cap_rights_subset(uint64_t subset, uint64_t superset) {
    return (subset & ~superset) == 0;
}

/* 权限交集 */
static inline uint64_t cap_rights_intersect(uint64_t a, uint64_t b) {
    return a & b;
}

/* 权限并集 */
static inline uint64_t cap_rights_union(uint64_t a, uint64_t b) {
    return a | b;
}

/* 权限差集 */
static inline uint64_t cap_rights_diff(uint64_t a, uint64_t b) {
    return a & ~b;
}

/* 权限字符串化 */
static inline const char *cap_right_name(uint64_t right) {
    switch (right) {
        case CAP_RIGHT_READ:     return "read";
        case CAP_RIGHT_WRITE:    return "write";
        case CAP_RIGHT_EXECUTE:  return "execute";
        case CAP_RIGHT_APPEND:   return "append";
        case CAP_RIGHT_DELETE:   return "delete";
        case CAP_RIGHT_CREATE:   return "create";
        case CAP_RIGHT_LIST:     return "list";
        case CAP_RIGHT_ADMIN:    return "admin";
        case CAP_RIGHT_DELEGATE: return "delegate";
        case CAP_RIGHT_REVOKE:   return "revoke";
        case CAP_RIGHT_GRANT:    return "grant";
        default:                 return "unknown";
    }
}

/* 类型字符串化 */
static inline const char *cap_type_name(cap_type_t type) {
    switch (type) {
        case CAP_TYPE_FILE:          return "file";
        case CAP_TYPE_DIRECTORY:     return "directory";
        case CAP_TYPE_PROCESS:       return "process";
        case CAP_TYPE_THREAD:        return "thread";
        case CAP_TYPE_MEMORY:        return "memory";
        case CAP_TYPE_PORT:          return "port";
        case CAP_TYPE_SOCKET:        return "socket";
        case CAP_TYPE_DEVICE:        return "device";
        case CAP_TYPE_PMEM_SEGMENT:  return "pmem_segment";
        case CAP_TYPE_ZPIPE_CHANNEL: return "zpipe_channel";
        case CAP_TYPE_ZRING:         return "zring";
        case CAP_TYPE_NAMESPACE:     return "namespace";
        case CAP_TYPE_CGROUP:        return "cgroup";
        case CAP_TYPE_KEY:           return "key";
        default:                     return "unknown";
    }
}

/* 检查能力有效性 */
static inline int cap_is_valid(struct capability *cap) {
    return cap != NULL &&
           cap->handle != CAP_INVALID_HANDLE &&
           !(cap->flags & CAP_FLAG_REVOKED);
}

/* 检查能力是否过期 */
static inline int cap_is_expired(struct capability *cap) {
    if (cap->expiry_time == 0) {
        return 0;  /* 永不过期 */
    }
    /* TODO: 获取当前时间比较 */
    return 0;
}

/* 检查委托权限 */
static inline int cap_can_delegate(struct capability *cap, uint64_t rights) {
    if (!cap_is_valid(cap)) return 0;
    if (cap->flags & CAP_FLAG_NO_DELEGATE) return 0;
    if (cap->depth >= cap->max_depth) return 0;
    if (!cap_rights_subset(rights, cap->max_rights)) return 0;
    return 1;
}

#endif /* CAPABILITY_H */
