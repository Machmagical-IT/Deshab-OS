/*
 * Zratro OS - Resource Limits (资源限制系统)
 * 
 * 实现进程级别资源限制
 */

#include "rlimit.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "task.h"

/* ========== 全局管理器 ========== */

static struct rlimit_manager global_rlimit;

/* ========== 资源名称 ========== */

static const char *resource_names[RLIMIT_COUNT] = {
    [RLIMIT_CPU]       = "CPU time",
    [RLIMIT_FSIZE]     = "File size",
    [RLIMIT_DATA]      = "Data size",
    [RLIMIT_STACK]     = "Stack size",
    [RLIMIT_CORE]      = "Core file size",
    [RLIMIT_RSS]       = "Resident set size",
    [RLIMIT_NPROC]     = "Processes",
    [RLIMIT_NOFILE]    = "Open files",
    [RLIMIT_MEMLOCK]   = "Locked memory",
    [RLIMIT_AS]        = "Address space",
    [RLIMIT_LOCKS]     = "File locks",
    [RLIMIT_SIGPENDING] = "Pending signals",
    [RLIMIT_MSGQUEUE]  = "Message queues",
    [RLIMIT_NICE]      = "Nice priority",
    [RLIMIT_RTPRIO]    = "Real-time priority",
    [RLIMIT_RTTIME]    = "Real-time time",
};

/* ========== 初始化 ========== */

int rlimit_init(struct rlimit_manager *mgr) {
    if (mgr == NULL) {
        return -1;
    }
    
    memset(mgr, 0, sizeof(*mgr));
    
    /* 设置系统默认限制 */
    mgr->system_limits[RLIMIT_CPU].rlim_cur = RLIMIT_CPU_DEFAULT;
    mgr->system_limits[RLIMIT_CPU].rlim_max = RLIMIT_CPU_DEFAULT;
    
    mgr->system_limits[RLIMIT_FSIZE].rlim_cur = RLIMIT_FSIZE_DEFAULT;
    mgr->system_limits[RLIMIT_FSIZE].rlim_max = RLIMIT_FSIZE_DEFAULT;
    
    mgr->system_limits[RLIMIT_DATA].rlim_cur = RLIMIT_DATA_DEFAULT;
    mgr->system_limits[RLIMIT_DATA].rlim_max = RLIMIT_DATA_DEFAULT * 2;
    
    mgr->system_limits[RLIMIT_STACK].rlim_cur = RLIMIT_STACK_DEFAULT;
    mgr->system_limits[RLIMIT_STACK].rlim_max = RLIMIT_STACK_DEFAULT * 2;
    
    mgr->system_limits[RLIMIT_CORE].rlim_cur = RLIMIT_CORE_DEFAULT;
    mgr->system_limits[RLIMIT_CORE].rlim_max = RLIMIT_CORE_DEFAULT;
    
    mgr->system_limits[RLIMIT_RSS].rlim_cur = RLIMIT_RSS_DEFAULT;
    mgr->system_limits[RLIMIT_RSS].rlim_max = RLIMIT_RSS_DEFAULT;
    
    mgr->system_limits[RLIMIT_NPROC].rlim_cur = RLIMIT_NPROC_DEFAULT;
    mgr->system_limits[RLIMIT_NPROC].rlim_max = RLIMIT_NPROC_DEFAULT * 4;
    
    mgr->system_limits[RLIMIT_NOFILE].rlim_cur = RLIMIT_NOFILE_DEFAULT;
    mgr->system_limits[RLIMIT_NOFILE].rlim_max = RLIMIT_NOFILE_DEFAULT * 4;
    
    mgr->system_limits[RLIMIT_MEMLOCK].rlim_cur = RLIMIT_MEMLOCK_DEFAULT;
    mgr->system_limits[RLIMIT_MEMLOCK].rlim_max = RLIMIT_MEMLOCK_DEFAULT * 4;
    
    mgr->system_limits[RLIMIT_AS].rlim_cur = RLIMIT_AS_DEFAULT;
    mgr->system_limits[RLIMIT_AS].rlim_max = RLIMIT_AS_DEFAULT;
    
    mgr->system_limits[RLIMIT_LOCKS].rlim_cur = RLIMIT_LOCKS_DEFAULT;
    mgr->system_limits[RLIMIT_LOCKS].rlim_max = RLIMIT_LOCKS_DEFAULT;
    
    mgr->system_limits[RLIMIT_SIGPENDING].rlim_cur = RLIMIT_SIGPENDING_DEFAULT;
    mgr->system_limits[RLIMIT_SIGPENDING].rlim_max = RLIMIT_SIGPENDING_DEFAULT * 4;
    
    mgr->system_limits[RLIMIT_MSGQUEUE].rlim_cur = RLIMIT_MSGQUEUE_DEFAULT;
    mgr->system_limits[RLIMIT_MSGQUEUE].rlim_max = RLIMIT_MSGQUEUE_DEFAULT * 4;
    
    mgr->system_limits[RLIMIT_NICE].rlim_cur = RLIMIT_NICE_DEFAULT;
    mgr->system_limits[RLIMIT_NICE].rlim_max = RLIMIT_NICE_DEFAULT;
    
    mgr->system_limits[RLIMIT_RTPRIO].rlim_cur = RLIMIT_RTPRIO_DEFAULT;
    mgr->system_limits[RLIMIT_RTPRIO].rlim_max = RLIMIT_RTPRIO_DEFAULT;
    
    mgr->system_limits[RLIMIT_RTTIME].rlim_cur = RLIMIT_RTTIME_DEFAULT;
    mgr->system_limits[RLIMIT_RTTIME].rlim_max = RLIMIT_RTTIME_DEFAULT * 10;
    
    spinlock_init(&mgr->lock);
    
    return 0;
}

/* ========== 进程限制 ========== */

int sys_getrlimit(rlimit_resource_t resource, struct rlimit *rlim) {
    if (resource >= RLIMIT_COUNT || rlim == NULL) {
        return -1;
    }

    /* 优先检查当前进程级限制 */
    struct process *proc = process_get_current();
    if (proc != NULL && proc->process_limits[resource].rlim_cur != 0) {
        memcpy(rlim, &proc->process_limits[resource], sizeof(*rlim));
        return 0;
    }

    /* 回退到全局限制 */
    struct rlimit_manager *mgr = rlimit_get_global();

    spin_lock(&mgr->lock);
    memcpy(rlim, &mgr->system_limits[resource], sizeof(*rlim));
    spin_unlock(&mgr->lock);

    return 0;
}

int sys_setrlimit(rlimit_resource_t resource, const struct rlimit *rlim) {
    if (resource >= RLIMIT_COUNT || rlim == NULL) {
        return -1;
    }

    struct rlimit_manager *mgr = rlimit_get_global();

    spin_lock(&mgr->lock);

    /* 检查硬限制：不能超过系统硬限制 */
    if (rlim->rlim_max > mgr->system_limits[resource].rlim_max) {
        spin_unlock(&mgr->lock);
        return -1;  /* 不能超过系统硬限制 */
    }

    /* 软限制不能超过硬限制 */
    struct rlimit new_rlim;
    memcpy(&new_rlim, rlim, sizeof(new_rlim));
    if (new_rlim.rlim_cur > new_rlim.rlim_max) {
        new_rlim.rlim_cur = new_rlim.rlim_max;
    }

    spin_unlock(&mgr->lock);

    /* 设置到当前进程级限制 */
    struct process *proc = process_get_current();
    if (proc != NULL) {
        memcpy(&proc->process_limits[resource], &new_rlim, sizeof(new_rlim));
    }

    return 0;
}

int sys_prlimit(uint64_t pid, rlimit_resource_t resource,
                const struct rlimit *new_rlim, struct rlimit *old_rlim) {
    if (resource >= RLIMIT_COUNT) {
        return -1;
    }

    /* 查找目标进程 */
    struct process *proc = process_get_by_pid((uint32_t)pid);
    if (proc == NULL) {
        /* pid=0 表示当前进程 */
        proc = process_get_current();
    }
    if (proc == NULL) {
        return -1;
    }

    struct rlimit_manager *mgr = rlimit_get_global();

    /* 获取旧值 */
    if (old_rlim != NULL) {
        if (proc->process_limits[resource].rlim_cur != 0) {
            memcpy(old_rlim, &proc->process_limits[resource], sizeof(*old_rlim));
        } else {
            spin_lock(&mgr->lock);
            memcpy(old_rlim, &mgr->system_limits[resource], sizeof(*old_rlim));
            spin_unlock(&mgr->lock);
        }
    }

    /* 设置新值 */
    if (new_rlim != NULL) {
        spin_lock(&mgr->lock);
        if (new_rlim->rlim_max > mgr->system_limits[resource].rlim_max) {
            spin_unlock(&mgr->lock);
            return -1;
        }
        spin_unlock(&mgr->lock);

        struct rlimit copy;
        memcpy(&copy, new_rlim, sizeof(copy));
        if (copy.rlim_cur > copy.rlim_max) {
            copy.rlim_cur = copy.rlim_max;
        }
        memcpy(&proc->process_limits[resource], &copy, sizeof(copy));
    }

    return 0;
}

/* ========== 资源使用 ========== */

int sys_getrusage(int who, struct rusage *usage) {
    if (usage == NULL) {
        return -1;
    }
    
    memset(usage, 0, sizeof(*usage));
    
    /* TODO: 获取实际进程资源使用 */
    if (who == 0) {  /* RUSAGE_SELF */
        /* 返回当前进程资源使用 */
    } else if (who == -1) {  /* RUSAGE_CHILDREN */
        /* 返回子进程资源使用 */
    }
    
    return 0;
}

void rlimit_update_usage(uint64_t pid, struct process_rusage *usage) {
    (void)pid;
    (void)usage;
    /* 更新进程资源使用统计 */
}

int rlimit_check(uint64_t pid, rlimit_resource_t resource, uint64_t value) {
    if (resource >= RLIMIT_COUNT) {
        return 0;
    }

    /* 优先检查进程级限制 */
    struct process *proc = process_get_by_pid((uint32_t)pid);
    if (proc != NULL && proc->process_limits[resource].rlim_cur != 0) {
        uint64_t limit = proc->process_limits[resource].rlim_cur;
        if (limit == RLIM_INFINITY) {
            return 1;  /* 无限制 */
        }
        return (value < limit) ? 1 : 0;
    }

    /* 回退到全局限制 */
    struct rlimit_manager *mgr = rlimit_get_global();

    spin_lock(&mgr->lock);

    uint64_t limit = mgr->system_limits[resource].rlim_cur;

    spin_unlock(&mgr->lock);

    if (limit == RLIM_INFINITY) {
        return 1;  /* 无限制 */
    }

    return (value < limit) ? 1 : 0;
}

/* ========== 优先级 ========== */

int sys_setpriority(int which, uint64_t who, int prio) {
    (void)which;
    (void)who;
    (void)prio;
    
    /* TODO: 实现优先级设置 */
    return 0;
}

int sys_getpriority(int which, uint64_t who) {
    (void)which;
    (void)who;
    
    /* TODO: 实现优先级获取 */
    return 0;
}

/* ========== 调度策略 ========== */

int sys_sched_setscheduler(uint64_t pid, int policy, const struct sched_param *param) {
    (void)pid;
    (void)policy;
    (void)param;
    
    /* TODO: 实现调度策略设置 */
    return 0;
}

int sys_sched_getscheduler(uint64_t pid) {
    (void)pid;
    
    /* TODO: 实现调度策略获取 */
    return SCHED_OTHER;
}

int sys_sched_setparam(uint64_t pid, const struct sched_param *param) {
    (void)pid;
    (void)param;
    
    /* TODO: 实现调度参数设置 */
    return 0;
}

int sys_sched_getparam(uint64_t pid, struct sched_param *param) {
    (void)pid;
    
    if (param == NULL) {
        return -1;
    }
    
    param->sched_priority = 0;
    return 0;
}

/* ========== 工具函数 ========== */

const char *rlimit_name(rlimit_resource_t resource) {
    if (resource >= RLIMIT_COUNT) {
        return "Unknown";
    }
    return resource_names[resource];
}

uint64_t rlimit_scale(rlimit_resource_t resource) {
    switch (resource) {
        case RLIMIT_CPU:
            return 1000000;  /* 秒 -> 微秒 */
        case RLIMIT_RTTIME:
            return 1;        /* 已经是微秒 */
        default:
            return 1;        /* 字节 */
    }
}

void rlimit_print_limits(uint64_t pid) {
    println("=== Resource Limits ===");

    /* 查找进程级限制 */
    struct process *proc = process_get_by_pid((uint32_t)pid);
    struct rlimit_manager *mgr = rlimit_get_global();

    for (int i = 0; i < RLIMIT_COUNT; i++) {
        print("  ");
        print((char *)resource_names[i]);
        print(": ");

        /* 优先使用进程级限制 */
        struct rlimit *lim = NULL;
        struct rlimit fallback;
        if (proc != NULL && proc->process_limits[i].rlim_cur != 0) {
            lim = &proc->process_limits[i];
        } else {
            spin_lock(&mgr->lock);
            memcpy(&fallback, &mgr->system_limits[i], sizeof(fallback));
            spin_unlock(&mgr->lock);
            lim = &fallback;
        }

        if (lim->rlim_cur == RLIM_INFINITY) {
            print("unlimited");
        } else {
            print_dec(lim->rlim_cur);
        }

        print(" / ");

        if (lim->rlim_max == RLIM_INFINITY) {
            print("unlimited");
        } else {
            print_dec(lim->rlim_max);
        }

        println("");
    }
}

/* ========== 全局管理器 ========== */

struct rlimit_manager *rlimit_get_global(void) {
    return &global_rlimit;
}

int rlimit_global_init(void) {
    return rlimit_init(&global_rlimit);
}

/* ========== 进程级 rlimit 初始化 ========== */

/**
 * rlimit_init_process - 用系统默认值初始化进程级资源限制
 * @proc: 目标进程
 */
void rlimit_init_process(struct process *proc) {
    if (proc == NULL) {
        return;
    }

    struct rlimit_manager *mgr = rlimit_get_global();

    spin_lock(&mgr->lock);
    memcpy(proc->process_limits, mgr->system_limits, sizeof(proc->process_limits));
    spin_unlock(&mgr->lock);
}

/**
 * rlimit_inherit_process - 从父进程继承资源限制
 * @child:  子进程
 * @parent: 父进程
 */
void rlimit_inherit_process(struct process *child, struct process *parent) {
    if (child == NULL || parent == NULL) {
        return;
    }

    /* 直接复制父进程的限制 */
    memcpy(child->process_limits, parent->process_limits, sizeof(child->process_limits));
}
