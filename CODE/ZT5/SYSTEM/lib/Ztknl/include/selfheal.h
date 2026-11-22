/*
 * Zratro OS - Self-Healing Kernel (自愈内核框架)
 * 
 * 创新设计：
 * 1. 故障检测 - 异常监控、看门狗、心跳检测
 * 2. 故障隔离 - 基于 Capability 的组件隔离
 * 3. 状态回滚 - 基于 Z-Journal 的检查点恢复
 * 4. 热修复 - 基于 HotUpdate 的运行时修补
 * 5. 渐进恢复 - 分级恢复策略
 */

#ifndef SELFHEAL_H
#define SELFHEAL_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== 自愈配置 ========== */

#define SH_MAX_FAULT_HANDLERS   32          /* 最大故障处理器数 */
#define SH_MAX_ISOLATION_DOMAINS 16         /* 最大隔离域数 */
#define SH_MAX_CHECKPOINTS      64          /* 最大检查点数 */
#define SH_HEARTBEAT_INTERVAL   5000        /* 心跳间隔 (ms) */
#define SH_WATCHDOG_TIMEOUT     30000       /* 看门狗超时 (ms) */

/* ========== 故障类型 ========== */

typedef enum {
    SH_FAULT_NONE = 0,
    
    /* 异常类故障 */
    SH_FAULT_EXCEPTION,         /* CPU 异常 (Page Fault, GPF 等) */
    SH_FAULT_PANIC,             /* 内核恐慌 */
    SH_FAULT_OOPS,              /* 内核 Oops */
    
    /* 内存类故障 */
    SH_FAULT_OOM,               /* 内存不足 */
    SH_FAULT_MEMORY_CORRUPTION, /* 内存损坏 */
    SH_FAULT_MEMORY_LEAK,       /* 内存泄漏 */
    
    /* 资源类故障 */
    SH_FAULT_DEADLOCK,          /* 死锁 */
    SH_FAULT_STARVATION,        /* 资源饥饿 */
    SH_FAULT_RESOURCE_EXHAUST,  /* 资源耗尽 */
    
    /* 子系统故障 */
    SH_FAULT_DRIVER,            /* 驱动故障 */
    SH_FAULT_FILESYSTEM,        /* 文件系统故障 */
    SH_FAULT_NETWORK,           /* 网络故障 */
    SH_FAULT_DEVICE,            /* 设备故障 */
    
    /* 安全类故障 */
    SH_FAULT_SECURITY_VIOLATION,/* 安全违规 */
    SH_FAULT_INTEGRITY_ERROR,   /* 完整性错误 */
    
    /* 性能类故障 */
    SH_FAULT_PERF_DEGRADATION,  /* 性能降级 */
    SH_FAULT_HANG,              /* 挂起 */
    
    SH_FAULT_MAX
} sh_fault_type_t;

/* ========== 故障严重级别 ========== */

typedef enum {
    SH_SEVERITY_INFO = 0,       /* 信息 - 不需要恢复 */
    SH_SEVERITY_MINOR,          /* 轻微 - 自动恢复 */
    SH_SEVERITY_MAJOR,          /* 严重 - 需要隔离 */
    SH_SEVERITY_CRITICAL,       /* 危急 - 需要回滚 */
    SH_SEVERITY_FATAL,          /* 致命 - 需要重启 */
} sh_severity_t;

/* ========== 故障上下文 ========== */

struct sh_fault_context {
    sh_fault_type_t type;           /* 故障类型 */
    sh_severity_t severity;         /* 严重级别 */
    
    /* 故障位置 */
    const char *component;          /* 组件名 */
    const char *function;           /* 函数名 */
    uint64_t address;               /* 故障地址 */
    
    /* 故障详情 */
    uint64_t error_code;            /* 错误码 */
    char message[256];              /* 详细消息 */
    
    /* 时间 */
    uint64_t timestamp;             /* 发生时间 */
    uint64_t recovery_deadline;     /* 恢复截止时间 */
    
    /* 上下文数据 */
    void *context_data;             /* 额外上下文 */
    size_t context_size;
    
    /* CPU 信息 */
    uint32_t cpu_id;                /* 发生故障的 CPU */
    uint64_t registers[16];         /* 寄存器快照 */
};

/* ========== 恢复策略 ========== */

typedef enum {
    SH_RECOVER_NONE = 0,        /* 不恢复 */
    SH_RECOVER_RETRY,           /* 重试 */
    SH_RECOVER_RESTART,         /* 重启组件 */
    SH_RECOVER_ISOLATE,         /* 隔离 */
    SH_RECOVER_ROLLBACK,        /* 回滚检查点 */
    SH_RECOVER_HOTFIX,          /* 热修复 */
    SH_RECOVER_FAILOVER,        /* 故障转移 */
    SH_RECOVER_REBOOT,          /* 重启系统 */
} sh_recover_strategy_t;

/* ========== 隔离域 ========== */

struct sh_isolation_domain {
    uint32_t domain_id;             /* 域 ID */
    char name[32];                  /* 域名 */
    uint32_t flags;                 /* 标志 */
    
    /* 资源限制 */
    uint64_t memory_limit;          /* 内存限制 */
    uint64_t cpu_limit;             /* CPU 时间限制 */
    uint32_t file_limit;            /* 文件描述符限制 */
    
    /* 状态 */
    int is_isolated;                /* 是否已隔离 */
    int fault_count;                /* 故障计数 */
    uint64_t last_fault_time;       /* 上次故障时间 */
    
    /* Capability 表 */
    void *cap_table;                /* 能力表 */
    
    /* 统计 */
    uint64_t total_faults;          /* 总故障数 */
    uint64_t recoveries;            /* 恢复次数 */
    
    struct list_head list;
};

/* 隔离域标志 */
#define SH_DOMAIN_FLAG_CRITICAL    (1 << 0)   /* 关键域 - 不允许完全隔离 */
#define SH_DOMAIN_FLAG_WATCHDOG    (1 << 1)   /* 启用看门狗 */
#define SH_DOMAIN_FLAG_HEARTBEAT   (1 << 2)   /* 启用心跳 */

/* ========== 检查点 ========== */

struct sh_checkpoint {
    uint64_t checkpoint_id;         /* 检查点 ID */
    char name[32];                  /* 名称 */
    uint64_t timestamp;             /* 创建时间 */
    
    /* 状态快照 */
    void *state_snapshot;           /* 状态快照 */
    size_t snapshot_size;           /* 快照大小 */
    
    /* 内存快照 */
    struct {
        uint64_t addr;
        uint64_t size;
        void *data;
    } memory_regions[16];
    uint32_t region_count;
    
    /* 统计 */
    uint32_t restore_count;         /* 恢复次数 */
    uint64_t last_restore_time;     /* 上次恢复时间 */
    
    /* 有效性 */
    int is_valid;                   /* 是否有效 */
    
    struct list_head list;
};

/* ========== 故障处理器 ========== */

typedef int (*sh_fault_handler_fn)(struct sh_fault_context *ctx, 
                                    sh_recover_strategy_t *strategy);

struct sh_fault_handler {
    sh_fault_type_t type;           /* 处理的故障类型 */
    sh_fault_handler_fn handler;    /* 处理函数 */
    uint32_t priority;              /* 优先级 */
    uint32_t flags;                 /* 标志 */
    
    struct list_head list;
};

/* 处理器标志 */
#define SH_HANDLER_FLAG_OVERRIDE  (1 << 0)   /* 覆盖默认处理 */

/* ========== 看门狗 ========== */

struct sh_watchdog {
    uint32_t id;                    /* 看门狗 ID */
    const char *name;               /* 名称 */
    uint64_t timeout;               /* 超时时间 (ms) */
    uint64_t last_feed;             /* 上次喂狗时间 */
    int is_running;                 /* 是否运行中 */
    int triggered;                  /* 是否已触发 */
    
    /* 回调 */
    void (*on_timeout)(struct sh_watchdog *wd);
    
    struct list_head list;
};

/* ========== 心跳 ========== */

struct sh_heartbeat {
    const char *component;          /* 组件名 */
    uint64_t interval;              /* 心跳间隔 (ms) */
    uint64_t last_beat;             /* 上次心跳时间 */
    int missed_beats;               /* 连续错过的心跳数 */
    int threshold;                  /* 触发阈值 */
    
    void (*on_miss)(const char *component);
    
    struct list_head list;
};

/* ========== 自愈管理器 ========== */

struct selfheal_manager {
    /* 故障处理器链表 */
    struct list_head handlers[SH_FAULT_MAX];
    uint32_t handler_count;
    
    /* 隔离域 */
    struct list_head domains;
    uint32_t domain_count;
    
    /* 检查点 */
    struct list_head checkpoints;
    uint64_t next_checkpoint_id;
    uint32_t checkpoint_count;
    
    /* 看门狗 */
    struct list_head watchdogs;
    uint32_t watchdog_count;
    
    /* 心跳 */
    struct list_head heartbeats;
    uint32_t heartbeat_count;
    
    /* 恢复统计 */
    uint64_t total_faults;
    uint64_t total_recoveries;
    uint64_t successful_recoveries;
    uint64_t failed_recoveries;
    
    /* 配置 */
    uint32_t config_flags;
    uint32_t max_recovery_attempts;
    uint64_t recovery_timeout;
    
    /* 状态 */
    int is_recovering;
    int recovery_in_progress;
    
    /* 锁 */
    spinlock_t lock;
};

/* 配置标志 */
#define SH_CONFIG_AUTO_RECOVER     (1 << 0)   /* 自动恢复 */
#define SH_CONFIG_AUTO_CHECKPOINT  (1 << 1)   /* 自动检查点 */
#define SH_CONFIG_STRICT_ISOLATION (1 << 2)   /* 严格隔离 */
#define SH_CONFIG_AUDIT_LOG        (1 << 3)   /* 审计日志 */

/* ========== 公共 API ========== */

/* 初始化/销毁 */
int selfheal_init(struct selfheal_manager *shm);
void selfheal_destroy(struct selfheal_manager *shm);

/* ========== 故障报告 ========== */

/* 报告故障 */
int sh_report_fault(struct selfheal_manager *shm, 
                    struct sh_fault_context *ctx);

/* 快速报告故障 (简化版) */
int sh_report_fault_simple(struct selfheal_manager *shm,
                           sh_fault_type_t type, sh_severity_t severity,
                           const char *component, const char *message);

/* 从异常帧报告故障 */
int sh_report_exception(struct selfheal_manager *shm,
                        uint32_t vector, uint64_t error_code,
                        uint64_t rip, uint32_t cpu_id);

/* ========== 故障处理 ========== */

/* 处理故障 */
int sh_handle_fault(struct selfheal_manager *shm,
                    struct sh_fault_context *ctx);

/* 注册故障处理器 */
int sh_register_handler(struct selfheal_manager *shm,
                        sh_fault_type_t type,
                        sh_fault_handler_fn handler,
                        uint32_t priority, uint32_t flags);

/* 注销故障处理器 */
int sh_unregister_handler(struct selfheal_manager *shm,
                          sh_fault_type_t type,
                          sh_fault_handler_fn handler);

/* ========== 恢复操作 ========== */

/* 执行恢复策略 */
int sh_execute_recovery(struct selfheal_manager *shm,
                        struct sh_fault_context *ctx,
                        sh_recover_strategy_t strategy);

/* 重试操作 */
int sh_retry_operation(struct sh_fault_context *ctx);

/* 重启组件 */
int sh_restart_component(struct selfheal_manager *shm,
                         const char *component);

/* ========== 隔离域管理 ========== */

/* 创建隔离域 */
struct sh_isolation_domain *sh_create_domain(struct selfheal_manager *shm,
                                              const char *name,
                                              uint32_t flags);

/* 销毁隔离域 */
int sh_destroy_domain(struct selfheal_manager *shm, uint32_t domain_id);

/* 隔离域 */
int sh_isolate_domain(struct selfheal_manager *shm, uint32_t domain_id);

/* 解除隔离 */
int sh_unisolate_domain(struct selfheal_manager *shm, uint32_t domain_id);

/* 查找隔离域 */
struct sh_isolation_domain *sh_find_domain(struct selfheal_manager *shm,
                                            const char *name);

/* ========== 检查点管理 ========== */

/* 创建检查点 */
struct sh_checkpoint *sh_create_checkpoint(struct selfheal_manager *shm,
                                            const char *name,
                                            void *state_data,
                                            size_t state_size);

/* 恢复到检查点 */
int sh_restore_checkpoint(struct selfheal_manager *shm, 
                          uint64_t checkpoint_id);

/* 删除检查点 */
int sh_delete_checkpoint(struct selfheal_manager *shm, uint64_t checkpoint_id);

/* 获取最新检查点 */
struct sh_checkpoint *sh_get_latest_checkpoint(struct selfheal_manager *shm);

/* 自动检查点 */
int sh_auto_checkpoint(struct selfheal_manager *shm);

/* ========== 看门狗管理 ========== */

/* 注册看门狗 */
struct sh_watchdog *sh_register_watchdog(struct selfheal_manager *shm,
                                          const char *name,
                                          uint64_t timeout,
                                          void (*on_timeout)(struct sh_watchdog *));

/* 喂狗 */
int sh_feed_watchdog(struct selfheal_manager *shm, uint32_t watchdog_id);

/* 停止看门狗 */
int sh_stop_watchdog(struct selfheal_manager *shm, uint32_t watchdog_id);

/* 检查看门狗 */
void sh_check_watchdogs(struct selfheal_manager *shm);

/* ========== 心跳管理 ========== */

/* 注册心跳 */
int sh_register_heartbeat(struct selfheal_manager *shm,
                          const char *component,
                          uint64_t interval,
                          int threshold,
                          void (*on_miss)(const char *));

/* 发送心跳 */
int sh_send_heartbeat(struct selfheal_manager *shm, const char *component);

/* 检查心跳 */
void sh_check_heartbeats(struct selfheal_manager *shm);

/* ========== 预定义故障处理器 ========== */

/* 异常处理器 */
int sh_exception_handler(struct sh_fault_context *ctx, 
                         sh_recover_strategy_t *strategy);

/* 内存故障处理器 */
int sh_memory_handler(struct sh_fault_context *ctx,
                      sh_recover_strategy_t *strategy);

/* 死锁处理器 */
int sh_deadlock_handler(struct sh_fault_context *ctx,
                        sh_recover_strategy_t *strategy);

/* 驱动故障处理器 */
int sh_driver_handler(struct sh_fault_context *ctx,
                      sh_recover_strategy_t *strategy);

/* ========== 统计和调试 ========== */

/* 获取统计 */
void sh_get_stats(struct selfheal_manager *shm,
                  uint64_t *faults, uint64_t *recoveries,
                  uint64_t *successes, uint64_t *failures);

/* 打印状态 */
void sh_print_status(struct selfheal_manager *shm);

/* ========== 内联辅助 ========== */

/* 故障类型字符串 */
static inline const char *sh_fault_name(sh_fault_type_t type) {
    switch (type) {
        case SH_FAULT_EXCEPTION:         return "exception";
        case SH_FAULT_PANIC:             return "panic";
        case SH_FAULT_OOPS:              return "oops";
        case SH_FAULT_OOM:               return "oom";
        case SH_FAULT_MEMORY_CORRUPTION: return "memory_corruption";
        case SH_FAULT_MEMORY_LEAK:       return "memory_leak";
        case SH_FAULT_DEADLOCK:          return "deadlock";
        case SH_FAULT_STARVATION:        return "starvation";
        case SH_FAULT_RESOURCE_EXHAUST:  return "resource_exhaust";
        case SH_FAULT_DRIVER:            return "driver";
        case SH_FAULT_FILESYSTEM:        return "filesystem";
        case SH_FAULT_NETWORK:           return "network";
        case SH_FAULT_DEVICE:            return "device";
        case SH_FAULT_SECURITY_VIOLATION:return "security_violation";
        case SH_FAULT_INTEGRITY_ERROR:   return "integrity_error";
        case SH_FAULT_PERF_DEGRADATION:  return "perf_degradation";
        case SH_FAULT_HANG:              return "hang";
        default:                         return "unknown";
    }
}

/* 严重级别字符串 */
static inline const char *sh_severity_name(sh_severity_t severity) {
    switch (severity) {
        case SH_SEVERITY_INFO:     return "info";
        case SH_SEVERITY_MINOR:    return "minor";
        case SH_SEVERITY_MAJOR:    return "major";
        case SH_SEVERITY_CRITICAL:return "critical";
        case SH_SEVERITY_FATAL:   return "fatal";
        default:                  return "unknown";
    }
}

/* 恢复策略字符串 */
static inline const char *sh_strategy_name(sh_recover_strategy_t strategy) {
    switch (strategy) {
        case SH_RECOVER_NONE:     return "none";
        case SH_RECOVER_RETRY:    return "retry";
        case SH_RECOVER_RESTART:  return "restart";
        case SH_RECOVER_ISOLATE:  return "isolate";
        case SH_RECOVER_ROLLBACK: return "rollback";
        case SH_RECOVER_HOTFIX:   return "hotfix";
        case SH_RECOVER_FAILOVER: return "failover";
        case SH_RECOVER_REBOOT:   return "reboot";
        default:                  return "unknown";
    }
}

/* 全局管理器访问 */
struct selfheal_manager *sh_get_global(void);
int sh_global_init(void);

#endif /* SELFHEAL_H */
