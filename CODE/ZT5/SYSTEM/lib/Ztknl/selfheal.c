/*
 * Zratro OS - Self-Healing Kernel (自愈内核框架)
 * 
 * 实现：
 * 1. 故障检测与报告
 * 2. 隔离域管理
 * 3. 检查点与恢复
 * 4. 看门狗与心跳
 */

#include "selfheal.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "task.h"
#include "isr.h"
#include "hotupdate.h"

/* ========== 全局管理器 ========== */

static struct selfheal_manager global_shm;

/* ========== 初始化/销毁 ========== */

int selfheal_init(struct selfheal_manager *shm) {
    if (shm == NULL) {
        return -1;
    }
    
    memset(shm, 0, sizeof(*shm));
    
    /* 初始化各链表 */
    for (int i = 0; i < SH_FAULT_MAX; i++) {
        INIT_LIST_HEAD(&shm->handlers[i]);
    }
    INIT_LIST_HEAD(&shm->domains);
    INIT_LIST_HEAD(&shm->checkpoints);
    INIT_LIST_HEAD(&shm->watchdogs);
    INIT_LIST_HEAD(&shm->heartbeats);
    
    spinlock_init(&shm->lock);
    
    /* 默认配置 */
    shm->config_flags = SH_CONFIG_AUTO_RECOVER | SH_CONFIG_AUDIT_LOG;
    shm->max_recovery_attempts = 3;
    shm->recovery_timeout = 10000;  /* 10 秒 */
    
    /* 注册默认故障处理器 */
    sh_register_handler(shm, SH_FAULT_EXCEPTION, sh_exception_handler, 100, 0);
    sh_register_handler(shm, SH_FAULT_OOM, sh_memory_handler, 100, 0);
    sh_register_handler(shm, SH_FAULT_MEMORY_CORRUPTION, sh_memory_handler, 100, 0);
    sh_register_handler(shm, SH_FAULT_DEADLOCK, sh_deadlock_handler, 100, 0);
    sh_register_handler(shm, SH_FAULT_DRIVER, sh_driver_handler, 100, 0);
    
    return 0;
}

void selfheal_destroy(struct selfheal_manager *shm) {
    if (shm == NULL) {
        return;
    }
    
    spin_lock(&shm->lock);
    
    /* 释放所有检查点 */
    struct sh_checkpoint *ckpt, *ckpt_tmp;
    list_for_each_entry_safe(ckpt, ckpt_tmp, &shm->checkpoints, list) {
        list_del(&ckpt->list);
        if (ckpt->state_snapshot != NULL) {
            kfree(ckpt->state_snapshot);
        }
        for (uint32_t i = 0; i < ckpt->region_count; i++) {
            if (ckpt->memory_regions[i].data != NULL) {
                kfree(ckpt->memory_regions[i].data);
            }
        }
        kfree(ckpt);
    }
    
    /* 释放所有隔离域 */
    struct sh_isolation_domain *dom, *dom_tmp;
    list_for_each_entry_safe(dom, dom_tmp, &shm->domains, list) {
        list_del(&dom->list);
        kfree(dom);
    }
    
    /* 释放所有看门狗 */
    struct sh_watchdog *wd, *wd_tmp;
    list_for_each_entry_safe(wd, wd_tmp, &shm->watchdogs, list) {
        list_del(&wd->list);
        kfree(wd);
    }
    
    /* 释放所有心跳 */
    struct sh_heartbeat *hb, *hb_tmp;
    list_for_each_entry_safe(hb, hb_tmp, &shm->heartbeats, list) {
        list_del(&hb->list);
        kfree(hb);
    }
    
    spin_unlock(&shm->lock);
}

/* ========== 故障报告 ========== */

int sh_report_fault(struct selfheal_manager *shm,
                    struct sh_fault_context *ctx) {
    if (shm == NULL || ctx == NULL) {
        return -1;
    }
    
    spin_lock(&shm->lock);
    shm->total_faults++;
    spin_unlock(&shm->lock);
    
    if (shm->config_flags & SH_CONFIG_AUTO_RECOVER) {
        return sh_handle_fault(shm, ctx);
    }
    
    return 0;
}

int sh_report_fault_simple(struct selfheal_manager *shm,
                           sh_fault_type_t type, sh_severity_t severity,
                           const char *component, const char *message) {
    struct sh_fault_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    ctx.type = type;
    ctx.severity = severity;
    ctx.component = component;
    ctx.timestamp = 0;  /* TODO: 获取当前时间 */
    
    if (message != NULL) {
        strncpy(ctx.message, message, sizeof(ctx.message) - 1);
    }
    
    return sh_report_fault(shm, &ctx);
}

int sh_report_exception(struct selfheal_manager *shm,
                        uint32_t vector, uint64_t error_code,
                        uint64_t rip, uint32_t cpu_id) {
    struct sh_fault_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    ctx.type = SH_FAULT_EXCEPTION;
    
    /* 根据异常向量确定严重级别 */
    if (vector == 0x02 || vector == 0x08 || vector == 0x12) {
        /* NMI, Double Fault, Machine Check */
        ctx.severity = SH_SEVERITY_FATAL;
    } else if (vector == 0x0E) {
        /* Page Fault */
        ctx.severity = SH_SEVERITY_MAJOR;
    } else {
        ctx.severity = SH_SEVERITY_CRITICAL;
    }
    
    ctx.component = "kernel";
    ctx.function = "exception_handler";
    ctx.address = rip;
    ctx.error_code = error_code;
    ctx.cpu_id = cpu_id;
    
    /* 记录异常类型 */
    const char *exc_name = get_exception_name(vector);
    if (exc_name != NULL) {
        strncpy(ctx.message, "Exception: ", sizeof(ctx.message) - 1);
        strncat(ctx.message, exc_name, sizeof(ctx.message) - strlen(ctx.message) - 1);
    }
    
    return sh_report_fault(shm, &ctx);
}

/* ========== 故障处理 ========== */

int sh_handle_fault(struct selfheal_manager *shm,
                    struct sh_fault_context *ctx) {
    if (shm == NULL || ctx == NULL) {
        return -1;
    }
    
    /* 检查是否已在恢复中 */
    if (shm->recovery_in_progress) {
        console_error("[SelfHeal] Recovery already in progress");
        return -1;
    }
    
    shm->recovery_in_progress = 1;
    
    /* 查找处理器 */
    sh_recover_strategy_t strategy = SH_RECOVER_NONE;
    int handled = 0;
    
    struct sh_fault_handler *hdl;
    list_for_each_entry(hdl, &shm->handlers[ctx->type], list) {
        if (hdl->handler != NULL) {
            int ret = hdl->handler(ctx, &strategy);
            if (ret == 0) {
                handled = 1;
                if (hdl->flags & SH_HANDLER_FLAG_OVERRIDE) {
                    break;  /* 覆盖标志，停止继续查找 */
                }
            }
        }
    }
    
    /* 根据严重级别确定默认策略 */
    if (!handled || strategy == SH_RECOVER_NONE) {
        switch (ctx->severity) {
            case SH_SEVERITY_INFO:
                strategy = SH_RECOVER_NONE;
                break;
            case SH_SEVERITY_MINOR:
                strategy = SH_RECOVER_RETRY;
                break;
            case SH_SEVERITY_MAJOR:
                strategy = SH_RECOVER_RESTART;
                break;
            case SH_SEVERITY_CRITICAL:
                strategy = SH_RECOVER_ROLLBACK;
                break;
            case SH_SEVERITY_FATAL:
                strategy = SH_RECOVER_REBOOT;
                break;
        }
    }
    
    /* 执行恢复策略 */
    int ret = sh_execute_recovery(shm, ctx, strategy);
    
    spin_lock(&shm->lock);
    shm->total_recoveries++;
    if (ret == 0) {
        shm->successful_recoveries++;
    } else {
        shm->failed_recoveries++;
    }
    spin_unlock(&shm->lock);
    
    shm->recovery_in_progress = 0;
    
    return ret;
}

int sh_register_handler(struct selfheal_manager *shm,
                        sh_fault_type_t type,
                        sh_fault_handler_fn handler,
                        uint32_t priority, uint32_t flags) {
    if (shm == NULL || handler == NULL || type >= SH_FAULT_MAX) {
        return -1;
    }
    
    struct sh_fault_handler *hdl = kmalloc(sizeof(*hdl));
    if (hdl == NULL) {
        return -1;
    }
    
    hdl->type = type;
    hdl->handler = handler;
    hdl->priority = priority;
    hdl->flags = flags;
    INIT_LIST_HEAD(&hdl->list);
    
    /* 按优先级插入 */
    spin_lock(&shm->lock);
    struct sh_fault_handler *pos;
    list_for_each_entry(pos, &shm->handlers[type], list) {
        if (priority > pos->priority) {
            list_add_tail(&hdl->list, &pos->list);
            goto done;
        }
    }
    list_add_tail(&hdl->list, &shm->handlers[type]);
done:
    shm->handler_count++;
    spin_unlock(&shm->lock);
    
    return 0;
}

int sh_unregister_handler(struct selfheal_manager *shm,
                          sh_fault_type_t type,
                          sh_fault_handler_fn handler) {
    if (shm == NULL || type >= SH_FAULT_MAX) {
        return -1;
    }
    
    spin_lock(&shm->lock);
    struct sh_fault_handler *hdl, *tmp;
    list_for_each_entry_safe(hdl, tmp, &shm->handlers[type], list) {
        if (hdl->handler == handler) {
            list_del(&hdl->list);
            kfree(hdl);
            shm->handler_count--;
            spin_unlock(&shm->lock);
            return 0;
        }
    }
    spin_unlock(&shm->lock);
    
    return -1;
}

/* ========== 恢复操作 ========== */

int sh_execute_recovery(struct selfheal_manager *shm,
                        struct sh_fault_context *ctx,
                        sh_recover_strategy_t strategy) {
    switch (strategy) {
        case SH_RECOVER_NONE:
            return 0;
            
        case SH_RECOVER_RETRY:
            return sh_retry_operation(ctx);
            
        case SH_RECOVER_RESTART:
            return sh_restart_component(shm, ctx->component);
            
        case SH_RECOVER_ISOLATE:
            {
                struct sh_isolation_domain *dom = sh_find_domain(shm, ctx->component);
                if (dom != NULL) {
                    return sh_isolate_domain(shm, dom->domain_id);
                }
            }
            return -1;
            
        case SH_RECOVER_ROLLBACK:
            {
                struct sh_checkpoint *ckpt = sh_get_latest_checkpoint(shm);
                if (ckpt != NULL) {
                    return sh_restore_checkpoint(shm, ckpt->checkpoint_id);
                }
            }
            return -1;
            
        case SH_RECOVER_HOTFIX:
        case SH_RECOVER_FAILOVER:
            return -1;
            
        case SH_RECOVER_REBOOT:
            console_error("[SelfHeal] Fatal error, system reboot required");
            return -1;
            
        default:
            return -1;
    }
}

int sh_retry_operation(struct sh_fault_context *ctx) {
    /* TODO: 未实现 - 重试操作功能尚未完成 */
    /* 注意：当前此函数忽略故障上下文，直接返回成功 */
    (void)ctx;
    return 0;  /* 返回成功但实际未执行任何重试 */
}

int sh_restart_component(struct selfheal_manager *shm,
                         const char *component) {
    (void)shm;
    
    struct module_info *mod = hu_module_get(component);
    if (mod != NULL && mod->on_unload != NULL && mod->on_load != NULL) {
        mod->on_unload();
        return mod->on_load();
    }
    
    return -1;
}

/* ========== 隔离域管理 ========== */

struct sh_isolation_domain *sh_create_domain(struct selfheal_manager *shm,
                                              const char *name,
                                              uint32_t flags) {
    if (shm == NULL || name == NULL) {
        return NULL;
    }
    
    struct sh_isolation_domain *dom = kmalloc(sizeof(*dom));
    if (dom == NULL) {
        return NULL;
    }
    
    memset(dom, 0, sizeof(*dom));
    strncpy(dom->name, name, sizeof(dom->name) - 1);
    dom->flags = flags;
    INIT_LIST_HEAD(&dom->list);
    
    spin_lock(&shm->lock);
    dom->domain_id = shm->domain_count++;
    list_add_tail(&dom->list, &shm->domains);
    spin_unlock(&shm->lock);
    
    return dom;
}

int sh_destroy_domain(struct selfheal_manager *shm, uint32_t domain_id) {
    if (shm == NULL) {
        return -1;
    }
    
    spin_lock(&shm->lock);
    struct sh_isolation_domain *dom, *tmp;
    list_for_each_entry_safe(dom, tmp, &shm->domains, list) {
        if (dom->domain_id == domain_id) {
            list_del(&dom->list);
            kfree(dom);
            spin_unlock(&shm->lock);
            return 0;
        }
    }
    spin_unlock(&shm->lock);
    
    return -1;
}

int sh_isolate_domain(struct selfheal_manager *shm, uint32_t domain_id) {
    if (shm == NULL) {
        return -1;
    }
    
    spin_lock(&shm->lock);
    struct sh_isolation_domain *dom;
    list_for_each_entry(dom, &shm->domains, list) {
        if (dom->domain_id == domain_id) {
            dom->is_isolated = 1;
            dom->fault_count++;
            spin_unlock(&shm->lock);
            return 0;
        }
    }
    spin_unlock(&shm->lock);
    
    return -1;
}

int sh_unisolate_domain(struct selfheal_manager *shm, uint32_t domain_id) {
    if (shm == NULL) {
        return -1;
    }
    
    spin_lock(&shm->lock);
    struct sh_isolation_domain *dom;
    list_for_each_entry(dom, &shm->domains, list) {
        if (dom->domain_id == domain_id) {
            dom->is_isolated = 0;
            spin_unlock(&shm->lock);
            return 0;
        }
    }
    spin_unlock(&shm->lock);
    
    return -1;
}

struct sh_isolation_domain *sh_find_domain(struct selfheal_manager *shm,
                                            const char *name) {
    if (shm == NULL || name == NULL) {
        return NULL;
    }
    
    spin_lock(&shm->lock);
    struct sh_isolation_domain *dom;
    list_for_each_entry(dom, &shm->domains, list) {
        if (strcmp(dom->name, name) == 0) {
            spin_unlock(&shm->lock);
            return dom;
        }
    }
    spin_unlock(&shm->lock);
    
    return NULL;
}

/* ========== 检查点管理 ========== */

struct sh_checkpoint *sh_create_checkpoint(struct selfheal_manager *shm,
                                            const char *name,
                                            void *state_data,
                                            size_t state_size) {
    if (shm == NULL) {
        return NULL;
    }
    
    struct sh_checkpoint *ckpt = kmalloc(sizeof(*ckpt));
    if (ckpt == NULL) {
        return NULL;
    }
    
    memset(ckpt, 0, sizeof(*ckpt));
    
    /* 复制状态数据 */
    if (state_data != NULL && state_size > 0) {
        ckpt->state_snapshot = kmalloc(state_size);
        if (ckpt->state_snapshot == NULL) {
            kfree(ckpt);
            return NULL;
        }
        memcpy(ckpt->state_snapshot, state_data, state_size);
        ckpt->snapshot_size = state_size;
    }
    
    if (name != NULL) {
        strncpy(ckpt->name, name, sizeof(ckpt->name) - 1);
    }
    
    ckpt->is_valid = 1;
    INIT_LIST_HEAD(&ckpt->list);
    
    spin_lock(&shm->lock);
    ckpt->checkpoint_id = ++shm->next_checkpoint_id;
    ckpt->timestamp = 0;
    list_add_tail(&ckpt->list, &shm->checkpoints);
    shm->checkpoint_count++;
    spin_unlock(&shm->lock);
    
    return ckpt;
}

int sh_restore_checkpoint(struct selfheal_manager *shm, uint64_t checkpoint_id) {
    /* TODO: 未实现 - 检查点状态恢复功能尚未完成 */
    /* 注意：当前此函数仅增加计数器，不实际恢复保存的状态 */
    if (shm == NULL) {
        return -1;
    }
    
    spin_lock(&shm->lock);
    struct sh_checkpoint *ckpt;
    list_for_each_entry(ckpt, &shm->checkpoints, list) {
        if (ckpt->checkpoint_id == checkpoint_id && ckpt->is_valid) {
            ckpt->restore_count++;
            ckpt->last_restore_time = 0;
            spin_unlock(&shm->lock);
            return 0;  /* 返回成功但实际未恢复任何状态 */
        }
    }
    spin_unlock(&shm->lock);
    
    return -1;
}

int sh_delete_checkpoint(struct selfheal_manager *shm, uint64_t checkpoint_id) {
    if (shm == NULL) {
        return -1;
    }
    
    spin_lock(&shm->lock);
    struct sh_checkpoint *ckpt, *tmp;
    list_for_each_entry_safe(ckpt, tmp, &shm->checkpoints, list) {
        if (ckpt->checkpoint_id == checkpoint_id) {
            list_del(&ckpt->list);
            if (ckpt->state_snapshot != NULL) {
                kfree(ckpt->state_snapshot);
            }
            kfree(ckpt);
            shm->checkpoint_count--;
            spin_unlock(&shm->lock);
            return 0;
        }
    }
    spin_unlock(&shm->lock);
    
    return -1;
}

struct sh_checkpoint *sh_get_latest_checkpoint(struct selfheal_manager *shm) {
    if (shm == NULL) {
        return NULL;
    }
    
    spin_lock(&shm->lock);
    if (!list_empty(&shm->checkpoints)) {
        /* 获取最后一个检查点 (prev 指向最后一个) */
        struct sh_checkpoint *ckpt = list_entry(shm->checkpoints.prev,
                                                 struct sh_checkpoint, list);
        spin_unlock(&shm->lock);
        return ckpt;
    }
    spin_unlock(&shm->lock);
    
    return NULL;
}

int sh_auto_checkpoint(struct selfheal_manager *shm) {
    if (shm == NULL || !(shm->config_flags & SH_CONFIG_AUTO_CHECKPOINT)) {
        return -1;
    }
    
    /* 创建自动检查点 */
    return sh_create_checkpoint(shm, "auto", NULL, 0) != NULL ? 0 : -1;
}

/* ========== 看门狗管理 ========== */

struct sh_watchdog *sh_register_watchdog(struct selfheal_manager *shm,
                                          const char *name,
                                          uint64_t timeout,
                                          void (*on_timeout)(struct sh_watchdog *)) {
    if (shm == NULL || name == NULL) {
        return NULL;
    }
    
    struct sh_watchdog *wd = kmalloc(sizeof(*wd));
    if (wd == NULL) {
        return NULL;
    }
    
    memset(wd, 0, sizeof(*wd));
    wd->name = name;
    wd->timeout = timeout;
    wd->on_timeout = on_timeout;
    wd->is_running = 1;
    wd->triggered = 0;
    INIT_LIST_HEAD(&wd->list);
    
    spin_lock(&shm->lock);
    wd->id = shm->watchdog_count++;
    wd->last_feed = 0;  /* TODO: 获取当前时间 */
    list_add_tail(&wd->list, &shm->watchdogs);
    spin_unlock(&shm->lock);
    
    return wd;
}

int sh_feed_watchdog(struct selfheal_manager *shm, uint32_t watchdog_id) {
    if (shm == NULL) {
        return -1;
    }
    
    spin_lock(&shm->lock);
    struct sh_watchdog *wd;
    list_for_each_entry(wd, &shm->watchdogs, list) {
        if (wd->id == watchdog_id && wd->is_running) {
            wd->last_feed = 0;  /* TODO: 获取当前时间 */
            wd->triggered = 0;
            spin_unlock(&shm->lock);
            return 0;
        }
    }
    spin_unlock(&shm->lock);
    
    return -1;
}

int sh_stop_watchdog(struct selfheal_manager *shm, uint32_t watchdog_id) {
    if (shm == NULL) {
        return -1;
    }
    
    spin_lock(&shm->lock);
    struct sh_watchdog *wd;
    list_for_each_entry(wd, &shm->watchdogs, list) {
        if (wd->id == watchdog_id) {
            wd->is_running = 0;
            spin_unlock(&shm->lock);
            return 0;
        }
    }
    spin_unlock(&shm->lock);
    
    return -1;
}

void sh_check_watchdogs(struct selfheal_manager *shm) {
    if (shm == NULL) {
        return;
    }
    
    uint64_t now = 0;  /* TODO: 获取当前时间 */
    
    spin_lock(&shm->lock);
    struct sh_watchdog *wd;
    list_for_each_entry(wd, &shm->watchdogs, list) {
        if (wd->is_running && !wd->triggered) {
            if (now - wd->last_feed > wd->timeout) {
                wd->triggered = 1;
                spin_unlock(&shm->lock);
                
                console_error("[SelfHeal] Watchdog timeout");
                print("  Watchdog: "); println((char *)wd->name);
                
                if (wd->on_timeout != NULL) {
                    wd->on_timeout(wd);
                }
                
                /* 报告故障 */
                sh_report_fault_simple(shm, SH_FAULT_HANG, SH_SEVERITY_MAJOR,
                                       wd->name, "Watchdog timeout");
                
                spin_lock(&shm->lock);
            }
        }
    }
    spin_unlock(&shm->lock);
}

/* ========== 心跳管理 ========== */

int sh_register_heartbeat(struct selfheal_manager *shm,
                          const char *component,
                          uint64_t interval,
                          int threshold,
                          void (*on_miss)(const char *)) {
    if (shm == NULL || component == NULL) {
        return -1;
    }
    
    struct sh_heartbeat *hb = kmalloc(sizeof(*hb));
    if (hb == NULL) {
        return -1;
    }
    
    memset(hb, 0, sizeof(*hb));
    hb->component = component;
    hb->interval = interval;
    hb->threshold = threshold;
    hb->on_miss = on_miss;
    hb->missed_beats = 0;
    INIT_LIST_HEAD(&hb->list);
    
    spin_lock(&shm->lock);
    list_add_tail(&hb->list, &shm->heartbeats);
    shm->heartbeat_count++;
    spin_unlock(&shm->lock);
    
    return 0;
}

int sh_send_heartbeat(struct selfheal_manager *shm, const char *component) {
    if (shm == NULL || component == NULL) {
        return -1;
    }
    
    spin_lock(&shm->lock);
    struct sh_heartbeat *hb;
    list_for_each_entry(hb, &shm->heartbeats, list) {
        if (strcmp(hb->component, component) == 0) {
            hb->last_beat = 0;  /* TODO: 获取当前时间 */
            hb->missed_beats = 0;
            spin_unlock(&shm->lock);
            return 0;
        }
    }
    spin_unlock(&shm->lock);
    
    return -1;
}

void sh_check_heartbeats(struct selfheal_manager *shm) {
    if (shm == NULL) {
        return;
    }
    
    uint64_t now = 0;  /* TODO: 获取当前时间 */
    
    spin_lock(&shm->lock);
    struct sh_heartbeat *hb;
    list_for_each_entry(hb, &shm->heartbeats, list) {
        if (now - hb->last_beat > hb->interval) {
            hb->missed_beats++;
            
            if (hb->missed_beats >= hb->threshold) {
                spin_unlock(&shm->lock);
                
                if (hb->on_miss != NULL) {
                    hb->on_miss(hb->component);
                }
                
                sh_report_fault_simple(shm, SH_FAULT_HANG, SH_SEVERITY_MINOR,
                                       hb->component, "Heartbeat missed");
                
                spin_lock(&shm->lock);
            }
        }
    }
    spin_unlock(&shm->lock);
}

/* ========== 预定义故障处理器 ========== */

int sh_exception_handler(struct sh_fault_context *ctx,
                         sh_recover_strategy_t *strategy) {
    if (ctx->severity == SH_SEVERITY_FATAL) {
        *strategy = SH_RECOVER_REBOOT;
    } else if (ctx->severity == SH_SEVERITY_CRITICAL) {
        *strategy = SH_RECOVER_ROLLBACK;
    } else {
        *strategy = SH_RECOVER_ISOLATE;
    }
    
    return 0;
}

int sh_memory_handler(struct sh_fault_context *ctx,
                      sh_recover_strategy_t *strategy) {
    switch (ctx->type) {
        case SH_FAULT_OOM:
            *strategy = SH_RECOVER_RETRY;
            break;
            
        case SH_FAULT_MEMORY_CORRUPTION:
            *strategy = SH_RECOVER_ISOLATE;
            break;
            
        case SH_FAULT_MEMORY_LEAK:
            *strategy = SH_RECOVER_NONE;
            break;
            
        default:
            *strategy = SH_RECOVER_RESTART;
    }
    
    return 0;
}

int sh_deadlock_handler(struct sh_fault_context *ctx,
                        sh_recover_strategy_t *strategy) {
    if (ctx->component != NULL) {
        *strategy = SH_RECOVER_ISOLATE;
    } else {
        *strategy = SH_RECOVER_ROLLBACK;
    }
    
    return 0;
}

int sh_driver_handler(struct sh_fault_context *ctx,
                      sh_recover_strategy_t *strategy) {
    *strategy = SH_RECOVER_RESTART;
    return 0;
}

/* ========== 统计 ========== */

void sh_get_stats(struct selfheal_manager *shm,
                  uint64_t *faults, uint64_t *recoveries,
                  uint64_t *successes, uint64_t *failures) {
    if (shm == NULL) {
        return;
    }
    
    spin_lock(&shm->lock);
    if (faults != NULL) *faults = shm->total_faults;
    if (recoveries != NULL) *recoveries = shm->total_recoveries;
    if (successes != NULL) *successes = shm->successful_recoveries;
    if (failures != NULL) *failures = shm->failed_recoveries;
    spin_unlock(&shm->lock);
}

void sh_print_status(struct selfheal_manager *shm) {
    if (shm == NULL) {
        return;
    }
    
    println("=== Self-Healing Status ===");
    
    print("  Total faults: ");
    print_dec(shm->total_faults);
    println("");
    
    print("  Total recoveries: ");
    print_dec(shm->total_recoveries);
    println("");
    
    print("  Successful: ");
    print_dec(shm->successful_recoveries);
    println("");
    
    print("  Failed: ");
    print_dec(shm->failed_recoveries);
    println("");
    
    print("  Checkpoints: ");
    print_dec(shm->checkpoint_count);
    println("");
    
    print("  Isolation domains: ");
    print_dec(shm->domain_count);
    println("");
    
    print("  Watchdogs: ");
    print_dec(shm->watchdog_count);
    println("");
}

/* ========== 全局管理器 ========== */

struct selfheal_manager *sh_get_global(void) {
    return &global_shm;
}

int sh_global_init(void) {
    return selfheal_init(&global_shm);
}
