/*
 * Zratro OS - Cgroups Implementation
 * 
 * 资源控制组实现
 */

#include "cgroup.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"
#include "task.h"
#include "pmm.h"
#include <limits.h>

/* ========== 全局变量 ========== */

struct cgroup_root cgroup_root;
struct list_head cgroup_roots;
spinlock_t cgroup_mutex = SPINLOCK_INIT;

static uint64_t next_cgroup_id = 1;

/* 子系统数组 */
static struct cgroup_subsys *cgroup_subsys[CGROUP_SUBSYS_COUNT];
static int nr_subsys = 0;

/* ========== 子系统定义 ========== */

/* CPU 子系统 */
static int cpu_cgroup_create(struct cgroup *cgrp) {
    cgrp->cpu.shares = 1024;
    cgrp->cpu.cfs_period_us = 100000;
    cgrp->cpu.cfs_quota_us = -1;  /* 无限制 */
    return 0;
}

static void cpu_cgroup_attach(struct cgroup *cgrp, struct thread *tsk) {
    /* 设置任务的 CPU 份额 */
    if (tsk->sched_entity.weight != 0) {
        tsk->sched_entity.weight = (cgrp->cpu.shares * 1024) / 1024;
    }
}

static struct cgroup_subsys cpu_subsys = {
    .name = "cpu",
    .id = 0,
    .mask = CGRP_CONTROLLER_CPU,
    .create = cpu_cgroup_create,
    .attach = cpu_cgroup_attach,
};

/* 内存子系统 */
static int memory_cgroup_create(struct cgroup *cgrp) {
    cgrp->memory.limit_in_bytes = ULLONG_MAX;
    cgrp->memory.memsw_limit_in_bytes = ULLONG_MAX;
    cgrp->memory.soft_limit_in_bytes = 0;
    cgrp->memory.oom_control = 1;
    return 0;
}

static int memory_cgroup_can_attach(struct cgroup *cgrp, struct thread *tsk) {
    /* 检查内存限制 */
    if (cgrp->memory.usage_in_bytes > cgrp->memory.limit_in_bytes) {
        return -1;
    }
    return 0;
}

static struct cgroup_subsys memory_subsys = {
    .name = "memory",
    .id = 1,
    .mask = CGRP_CONTROLLER_MEMORY,
    .create = memory_cgroup_create,
    .can_attach = memory_cgroup_can_attach,
};

/* PIDs 子系统 */
static int pids_cgroup_create(struct cgroup *cgrp) {
    cgrp->pids.max = -1;  /* 无限制 */
    cgrp->pids.current = 0;
    return 0;
}

static int pids_cgroup_can_fork(struct thread *tsk) {
    struct cgroup *cgrp = cgroup_get_current(tsk);
    if (cgrp == NULL) return 0;
    
    if (cgrp->pids.max >= 0 && cgrp->pids.current >= (uint64_t)cgrp->pids.max) {
        return -1;  /* 达到进程数限制 */
    }
    
    cgrp->pids.current++;
    return 0;
}

static void pids_cgroup_exit(struct thread *tsk) {
    struct cgroup *cgrp = cgroup_get_current(tsk);
    if (cgrp != NULL && cgrp->pids.current > 0) {
        cgrp->pids.current--;
    }
}

static struct cgroup_subsys pids_subsys = {
    .name = "pids",
    .id = 2,
    .mask = CGRP_CONTROLLER_PIDS,
    .create = pids_cgroup_create,
    .fork = pids_cgroup_can_fork,
    .exit = pids_cgroup_exit,
};

/* CPUSET 子系统 */
static int cpuset_cgroup_create(struct cgroup *cgrp) {
    cgrp->cpuset.cpus_allowed = 0xFFFFFFFFFFFFFFFFULL;
    cgrp->cpuset.mems_allowed = 0xFFFFFFFFFFFFFFFFULL;
    cgrp->cpuset.cpu_exclusive = 0;
    cgrp->cpuset.mem_exclusive = 0;
    cgrp->cpuset.sched_load_balance = 1;
    return 0;
}

static struct cgroup_subsys cpuset_subsys = {
    .name = "cpuset",
    .id = 3,
    .mask = CGRP_CONTROLLER_CPUSET,
    .create = cpuset_cgroup_create,
};

/* Freezer 子系统 */
static int freezer_cgroup_create(struct cgroup *cgrp) {
    cgrp->freezer.state = CGRP_THAWED;
    cgrp->freezer.lock = SPINLOCK_INIT;
    return 0;
}

static void freezer_cgroup_attach(struct cgroup *cgrp, struct thread *tsk) {
    /* 如果 cgroup 处于冻结状态，冻结任务 */
    if (cgrp->freezer.state == CGRP_FROZEN) {
        tsk->flags |= THREAD_FLAG_FROZEN;
    }
}

static struct cgroup_subsys freezer_subsys = {
    .name = "freezer",
    .id = 4,
    .mask = CGRP_CONTROLLER_FREEZER,
    .create = freezer_cgroup_create,
    .attach = freezer_cgroup_attach,
};

/* blkio 子系统 */
static int blkio_cgroup_create(struct cgroup *cgrp) {
    cgrp->blkio.weight = 500;
    cgrp->blkio.leaf_weight = 500;
    return 0;
}

static struct cgroup_subsys blkio_subsys = {
    .name = "blkio",
    .id = 5,
    .mask = CGRP_CONTROLLER_BLKIO,
    .create = blkio_cgroup_create,
};

/* devices 子系统 */
static int devices_cgroup_create(struct cgroup *cgrp) {
    cgrp->devices.allow_default = 1;
    list_init(&cgrp->devices.allow_list);
    list_init(&cgrp->devices.deny_list);
    return 0;
}

static struct cgroup_subsys devices_subsys = {
    .name = "devices",
    .id = 6,
    .mask = CGRP_CONTROLLER_DEVICES,
    .create = devices_cgroup_create,
};

/* ========== 初始化 ========== */

int cgroup_init(void) {
    println("[CGROUP] Initializing cgroups...");
    
    /* 初始化根列表 */
    list_init(&cgroup_roots);
    
    /* 初始化根 cgroup */
    memset(&cgroup_root, 0, sizeof(cgroup_root));
    cgroup_root.hierarchy_id = 0;
    cgroup_root.subsystem_mask = CGRP_CONTROLLER_CPU | CGRP_CONTROLLER_MEMORY |
                                  CGRP_CONTROLLER_PIDS | CGRP_CONTROLLER_CPUSET |
                                  CGRP_CONTROLLER_FREEZER | CGRP_CONTROLLER_BLKIO |
                                  CGRP_CONTROLLER_DEVICES;
    
    /* 初始化根 cgroup */
    cgroup_root.cgrp.id = next_cgroup_id++;
    strcpy(cgroup_root.cgrp.name, "/");
    cgroup_root.cgrp.parent = NULL;
    cgroup_root.cgrp.root = &cgroup_root;
    list_init(&cgroup_root.cgrp.children);
    list_init(&cgroup_root.cgrp.tasks);
    cgroup_root.cgrp.ref_count = 1;
    cgroup_root.cgrp.lock = SPINLOCK_INIT;
    
    /* 初始化控制器 */
    cpu_cgroup_create(&cgroup_root.cgrp);
    memory_cgroup_create(&cgroup_root.cgrp);
    pids_cgroup_create(&cgroup_root.cgrp);
    cpuset_cgroup_create(&cgroup_root.cgrp);
    freezer_cgroup_create(&cgroup_root.cgrp);
    blkio_cgroup_create(&cgroup_root.cgrp);
    devices_cgroup_create(&cgroup_root.cgrp);
    
    /* 添加到根列表 */
    list_add(&cgroup_root.root_list, &cgroup_roots);
    
    /* 注册子系统 */
    cgroup_subsys[nr_subsys++] = &cpu_subsys;
    cgroup_subsys[nr_subsys++] = &memory_subsys;
    cgroup_subsys[nr_subsys++] = &pids_subsys;
    cgroup_subsys[nr_subsys++] = &cpuset_subsys;
    cgroup_subsys[nr_subsys++] = &freezer_subsys;
    cgroup_subsys[nr_subsys++] = &blkio_subsys;
    cgroup_subsys[nr_subsys++] = &devices_subsys;
    
    println("[CGROUP] Cgroups initialized");
    return 0;
}

/* ========== cgroup 创建/销毁 ========== */

struct cgroup *cgroup_create(struct cgroup *parent, const char *name) {
    if (parent == NULL || name == NULL) return NULL;
    
    struct cgroup *cgrp = kmalloc(sizeof(*cgrp));
    if (cgrp == NULL) return NULL;
    
    memset(cgrp, 0, sizeof(*cgrp));
    
    spin_lock(&cgroup_mutex);
    
    cgrp->id = next_cgroup_id++;
    strncpy(cgrp->name, name, sizeof(cgrp->name) - 1);
    cgrp->parent = parent;
    cgrp->root = parent->root;
    
    list_init(&cgrp->children);
    list_init(&cgrp->tasks);
    cgrp->ref_count = 1;
    cgrp->lock = SPINLOCK_INIT;
    
    /* 调用子系统创建 */
    for (int i = 0; i < nr_subsys; i++) {
        if (cgroup_subsys[i]->create) {
            cgroup_subsys[i]->create(cgrp);
        }
    }
    
    /* 添加到父的子列表 */
    list_add(&cgrp->sibling, &parent->children);
    
    spin_unlock(&cgroup_mutex);
    
    return cgrp;
}

void cgroup_destroy(struct cgroup *cgrp) {
    if (cgrp == NULL) return;
    
    spin_lock(&cgroup_mutex);
    
    /* 检查是否有子 cgroup */
    if (!list_empty(&cgrp->children)) {
        spin_unlock(&cgroup_mutex);
        return;
    }
    
    /* 检查是否有任务 */
    if (!list_empty(&cgrp->tasks)) {
        spin_unlock(&cgroup_mutex);
        return;
    }
    
    /* 从父移除 */
    if (cgrp->parent != NULL) {
        list_del(&cgrp->sibling);
    }
    
    /* 调用子系统销毁 */
    for (int i = 0; i < nr_subsys; i++) {
        if (cgroup_subsys[i]->destroy) {
            cgroup_subsys[i]->destroy(cgrp);
        }
    }
    
    spin_unlock(&cgroup_mutex);
    
    kfree(cgrp);
}

/* ========== 进程操作 ========== */

int cgroup_attach_task(struct cgroup *cgrp, struct thread *tsk) {
    if (cgrp == NULL || tsk == NULL) return -1;
    
    spin_lock(&cgroup_mutex);
    
    /* 调用子系统 can_attach */
    for (int i = 0; i < nr_subsys; i++) {
        if (cgroup_subsys[i]->can_attach) {
            if (cgroup_subsys[i]->can_attach(cgrp, tsk) != 0) {
                spin_unlock(&cgroup_mutex);
                return -1;
            }
        }
    }
    
    /* 从旧 cgroup 移除 */
    /* TODO: 实现 */
    
    /* 添加到新 cgroup */
    list_add(&tsk->cgroup_list, &cgrp->tasks);
    tsk->cgroup = cgrp;
    cgrp->nr_tasks++;
    
    /* 调用子系统 attach */
    for (int i = 0; i < nr_subsys; i++) {
        if (cgroup_subsys[i]->attach) {
            cgroup_subsys[i]->attach(cgrp, tsk);
        }
    }
    
    spin_unlock(&cgroup_mutex);
    
    return 0;
}

void cgroup_exit_task(struct thread *tsk) {
    if (tsk == NULL || tsk->cgroup == NULL) return;
    
    spin_lock(&cgroup_mutex);
    
    struct cgroup *cgrp = tsk->cgroup;
    
    list_del(&tsk->cgroup_list);
    cgrp->nr_tasks--;
    
    /* 调用子系统 exit */
    for (int i = 0; i < nr_subsys; i++) {
        if (cgroup_subsys[i]->exit) {
            cgroup_subsys[i]->exit(tsk);
        }
    }
    
    spin_unlock(&cgroup_mutex);
}

int cgroup_can_fork(struct thread *tsk) {
    /* 调用子系统 fork 检查 */
    for (int i = 0; i < nr_subsys; i++) {
        if (cgroup_subsys[i]->fork) {
            if (cgroup_subsys[i]->fork(tsk) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

/* ========== 查找 ========== */

struct cgroup *cgroup_find(const char *path) {
    if (path == NULL) return NULL;
    
    /* 从根开始查找 */
    struct cgroup *cgrp = &cgroup_root.cgrp;
    
    if (strcmp(path, "/") == 0) {
        return cgrp;
    }
    
    /* 解析路径 */
    char buf[256];
    strncpy(buf, path, sizeof(buf) - 1);
    
    char *token = strtok(buf, "/");
    while (token != NULL) {
        /* 在子 cgroup 中查找 */
        struct cgroup *child;
        int found = 0;
        
        list_for_each_entry(child, &cgrp->children, sibling) {
            if (strcmp(child->name, token) == 0) {
                cgrp = child;
                found = 1;
                break;
            }
        }
        
        if (!found) {
            return NULL;
        }
        
        token = strtok(NULL, "/");
    }
    
    return cgrp;
}

struct cgroup *cgroup_get_current(struct thread *tsk) {
    if (tsk == NULL) return NULL;
    return tsk->cgroup ? tsk->cgroup : &cgroup_root.cgrp;
}

/* ========== 控制器操作 ========== */

struct cgroup_subsys *cgroup_get_subsys(int id) {
    if (id < 0 || id >= nr_subsys) return NULL;
    return cgroup_subsys[id];
}

/* ========== 资源限制检查 ========== */

int cgroup_can_attach_cpu(struct cgroup *cgrp) {
    if (cgrp == NULL) return 0;
    
    /* 检查 CPU 份额 */
    if (cgrp->cpu.shares < 2) {
        return -1;  /* 份额太小 */
    }
    
    return 0;
}

int cgroup_can_attach_memory(struct cgroup *cgrp) {
    if (cgrp == NULL) return 0;
    
    /* 检查内存使用是否超限 */
    if (cgrp->memory.usage_in_bytes > cgrp->memory.limit_in_bytes) {
        return -1;
    }
    
    return 0;
}

int cgroup_can_attach_pids(struct cgroup *cgrp) {
    if (cgrp == NULL) return 0;
    
    /* 检查进程数限制 */
    if (cgrp->pids.max >= 0 && cgrp->pids.current >= (uint64_t)cgrp->pids.max) {
        return -1;
    }
    
    return 0;
}

/* ========== 系统调用 ========== */

int sys_cgroup_create(const char *path, int flags) {
    (void)flags;
    
    if (path == NULL) return -1;
    
    /* 解析父路径和名称 */
    char buf[256];
    strncpy(buf, path, sizeof(buf) - 1);
    
    char *last_slash = strrchr(buf, '/');
    if (last_slash == NULL) return -1;
    
    *last_slash = '\0';
    char *name = last_slash + 1;
    
    struct cgroup *parent = cgroup_find(buf);
    if (parent == NULL) return -1;
    
    struct cgroup *cgrp = cgroup_create(parent, name);
    return cgrp ? 0 : -1;
}

int sys_cgroup_attach(const char *path, int pid) {
    if (path == NULL) return -1;
    
    struct cgroup *cgrp = cgroup_find(path);
    if (cgrp == NULL) return -1;
    
    struct thread *tsk = thread_get_by_tid(pid);
    if (tsk == NULL) return -1;
    
    return cgroup_attach_task(cgrp, tsk);
}

int sys_cgroup_set_param(const char *path, const char *param, uint64_t value) {
    if (path == NULL || param == NULL) return -1;
    
    struct cgroup *cgrp = cgroup_find(path);
    if (cgrp == NULL) return -1;
    
    spin_lock(&cgrp->lock);
    
    /* CPU 参数 */
    if (strcmp(param, "cpu.shares") == 0) {
        cgrp->cpu.shares = value;
    } else if (strcmp(param, "cpu.cfs_quota_us") == 0) {
        cgrp->cpu.cfs_quota_us = value;
    } else if (strcmp(param, "cpu.cfs_period_us") == 0) {
        cgrp->cpu.cfs_period_us = value;
    }
    /* 内存参数 */
    else if (strcmp(param, "memory.limit_in_bytes") == 0) {
        cgrp->memory.limit_in_bytes = value;
    } else if (strcmp(param, "memory.soft_limit_in_bytes") == 0) {
        cgrp->memory.soft_limit_in_bytes = value;
    }
    /* PIDs 参数 */
    else if (strcmp(param, "pids.max") == 0) {
        cgrp->pids.max = (int64_t)value;
    }
    /* blkio 参数 */
    else if (strcmp(param, "blkio.weight") == 0) {
        cgrp->blkio.weight = value;
    }
    /* freezer 参数 */
    else if (strcmp(param, "freezer.state") == 0) {
        cgrp->freezer.state = (int)value;
    }
    else {
        spin_unlock(&cgrp->lock);
        return -1;
    }
    
    spin_unlock(&cgrp->lock);
    return 0;
}

int sys_cgroup_get_param(const char *path, const char *param, uint64_t *value) {
    if (path == NULL || param == NULL || value == NULL) return -1;
    
    struct cgroup *cgrp = cgroup_find(path);
    if (cgrp == NULL) return -1;
    
    spin_lock(&cgrp->lock);
    
    /* CPU 参数 */
    if (strcmp(param, "cpu.shares") == 0) {
        *value = cgrp->cpu.shares;
    } else if (strcmp(param, "cpu.cfs_quota_us") == 0) {
        *value = cgrp->cpu.cfs_quota_us;
    }
    /* 内存参数 */
    else if (strcmp(param, "memory.limit_in_bytes") == 0) {
        *value = cgrp->memory.limit_in_bytes;
    } else if (strcmp(param, "memory.usage_in_bytes") == 0) {
        *value = cgrp->memory.usage_in_bytes;
    }
    /* PIDs 参数 */
    else if (strcmp(param, "pids.max") == 0) {
        *value = (uint64_t)cgrp->pids.max;
    } else if (strcmp(param, "pids.current") == 0) {
        *value = cgrp->pids.current;
    }
    else {
        spin_unlock(&cgrp->lock);
        return -1;
    }
    
    spin_unlock(&cgrp->lock);
    return 0;
}

/* ========== Cgroup 高级增强功能 ========== */

/* === 层级委托 === */

#define CGROUP_PERM_READ       0x01
#define CGROUP_PERM_WRITE      0x02
#define CGROUP_PERM_ADMIN      0x04

/* 检查委托权限 */
int cgroup_check_permission(struct cgroup *cgrp, int uid, int perm) {
    /* 根用户拥有所有权限 */
    if (uid == 0) return 1;
    
    /* 检查委托权限 */
    if (cgrp->delegation.enabled && cgrp->delegation.uid == uid) {
        return (cgrp->delegation.permissions & perm) != 0;
    }
    
    return 0;
}

/* 委托 cgroup 给用户 */
int cgroup_delegate(struct cgroup *cgrp, int uid, int gid, uint32_t perms) {
    if (cgrp == NULL) return -1;
    
    spin_lock(&cgrp->lock);
    cgrp->delegation.enabled = 1;
    cgrp->delegation.uid = uid;
    cgrp->delegation.gid = gid;
    cgrp->delegation.permissions = perms;
    spin_unlock(&cgrp->lock);
    
    return 0;
}

/* === 压力失速信息 (PSI) === */

/* PSI 输出结构体 */
struct psi_output {
    uint64_t some_avg10;
    uint64_t some_avg60;
    uint64_t some_avg300;
    uint64_t total_stall;
};

/* 更新 PSI 统计 */
void cgroup_update_psi(struct cgroup *cgrp) {
    if (cgrp == NULL) return;
    
    /* 模拟 PSI 计算 */
    uint64_t now = get_tick_count();
    uint64_t elapsed = now - cgrp->psi_last_update;
    
    if (elapsed > 0) {
        /* 计算资源压力 */
        uint64_t cpu_pressure = 0;
        uint64_t mem_pressure = 0;
        
        /* CPU 压力计算：需要检查 period 和 quota 都有效 */
        if (cgrp->cpu.cfs_period_us > 0 && cgrp->cpu.cfs_quota_us > 0) {
            cpu_pressure = (cgrp->cpu.cfs_period_us - cgrp->cpu.cfs_quota_us) * 100 / 
                          cgrp->cpu.cfs_period_us;
        }
        
        /* 内存压力计算：需要检查 limit 有效且非零 */
        if (cgrp->memory.limit_in_bytes < ULLONG_MAX && cgrp->memory.limit_in_bytes > 0) {
            mem_pressure = cgrp->memory.usage_in_bytes * 100 / cgrp->memory.limit_in_bytes;
        }
        
        /* 更新滑动平均 - CPU 压力 */
        cgrp->psi.some_avg10 = (cgrp->psi.some_avg10 * 9 + cpu_pressure) / 10;
        cgrp->psi.some_avg60 = (cgrp->psi.some_avg60 * 59 + cpu_pressure) / 60;
        cgrp->psi.some_avg300 = (cgrp->psi.some_avg300 * 299 + cpu_pressure) / 300;
        cgrp->psi.total_stall += cpu_pressure * elapsed;
        
        /* 更新内存压力 (PSI full) */
        cgrp->psi.full_avg10 = (cgrp->psi.full_avg10 * 9 + mem_pressure) / 10;
    }
    
    cgrp->psi_last_update = get_tick_count();
}

/* 获取 PSI 统计 */
int cgroup_get_psi(struct cgroup *cgrp, struct psi_output *stats) {
    if (cgrp == NULL || stats == NULL) return -1;
    
    spin_lock(&cgrp->lock);
    stats->some_avg10 = cgrp->psi.some_avg10;
    stats->some_avg60 = cgrp->psi.some_avg60;
    stats->some_avg300 = cgrp->psi.some_avg300;
    stats->total_stall = cgrp->psi.total_stall;
    spin_unlock(&cgrp->lock);
    
    return 0;
}

/* === 资源争用检测 === */

struct contention_info {
    int detected;
    int type;           /* 0=CPU, 1=内存, 2=IO */
    uint64_t severity;  /* 严重程度 0-100 */
    uint64_t duration;  /* 持续时间 */
};

/* 检测资源争用 */
int cgroup_detect_contention(struct cgroup *cgrp, struct contention_info *info) {
    if (cgrp == NULL || info == NULL) return -1;
    
    /* 初始化 */
    info->detected = 0;
    info->severity = 0;
    
    /* 检测 CPU 争用 */
    if (cgrp->cpu.cfs_quota_us > 0) {
        uint64_t usage_pct = cgrp->cpuacct.usage * 100 / 
                            (cgrp->cpu.cfs_period_us * 1000);
        if (usage_pct > 90) {
            info->detected = 1;
            info->type = 0;
            info->severity = usage_pct;
        }
    }
    
    /* 检测内存争用 */
    if (cgrp->memory.limit_in_bytes < ULLONG_MAX) {
        uint64_t mem_pct = cgrp->memory.usage_in_bytes * 100 / 
                          cgrp->memory.limit_in_bytes;
        if (mem_pct > 90) {
            info->detected = 1;
            info->type = 1;
            info->severity = mem_pct > info->severity ? mem_pct : info->severity;
        }
    }
    
    return 0;
}

/* === 线程模式 === */

/* 启用线程模式 */
int cgroup_enable_threaded(struct cgroup *cgrp) {
    if (cgrp == NULL) return -1;
    
    spin_lock(&cgrp->lock);
    
    /* 检查是否已有子 cgroup */
    if (!list_empty(&cgrp->children)) {
        spin_unlock(&cgrp->lock);
        return -1;  /* 有子 cgroup，无法启用 */
    }
    
    cgrp->flags |= CGROUP_FLAG_THREADED;
    spin_unlock(&cgrp->lock);
    
    return 0;
}

/* === 资源隔离 === */

struct resource_isolation {
    int cpu_exclusive;      /* 独占 CPU */
    int mem_exclusive;      /* 独占内存 */
    uint64_t cpuset;        /* CPU 掩码 */
    uint64_t memset;        /* 内存节点掩码 */
};

/* 设置资源隔离 */
int cgroup_set_isolation(struct cgroup *cgrp, struct resource_isolation *iso) {
    if (cgrp == NULL || iso == NULL) return -1;
    
    spin_lock(&cgrp->lock);
    
    if (iso->cpu_exclusive) {
        cgrp->flags |= CGROUP_FLAG_CPU_EXCLUSIVE;
        cgrp->cpuset_cpus = iso->cpuset;
    }
    
    if (iso->mem_exclusive) {
        cgrp->flags |= CGROUP_FLAG_MEM_EXCLUSIVE;
        cgrp->cpuset_mems = iso->memset;
    }
    
    spin_unlock(&cgrp->lock);
    return 0;
}

/* === 统计增强 === */

/* 打印详细统计 */
void cgroup_print_detailed_stats(struct cgroup *cgrp) {
    if (cgrp == NULL) return;
    
    println("╔══════════════════════════════════════════╗");
    print("║ Cgroup: "); println(cgrp->name);
    println("╚══════════════════════════════════════════╝");
    
    print("  Path: "); println(cgrp->name);
    print("  ID: "); print_dec(cgrp->id); println("");
    print("  Tasks: "); print_dec(cgrp->nr_tasks); println("");
    
    println("");
    println("  --- CPU ---");
    print("  Shares: "); print_dec(cgrp->cpu.shares); println("");
    print("  CFS Period: "); print_dec(cgrp->cpu.cfs_period_us); println(" us");
    print("  CFS Quota: "); print_dec(cgrp->cpu.cfs_quota_us); println(" us");
    
    println("");
    println("  --- Memory ---");
    print("  Limit: "); print_dec(cgrp->memory.limit_in_bytes / (1024*1024)); println(" MB");
    print("  Usage: "); print_dec(cgrp->memory.usage_in_bytes / (1024*1024)); println(" MB");
    print("  Cache: "); print_dec(cgrp->memory.cache / (1024*1024)); println(" MB");
    print("  OOM Kills: "); print_dec(cgrp->memory.oom_kill_count); println("");
    
    println("");
    println("  --- PIDs ---");
    print("  Max: "); print_dec(cgrp->pids.max); println("");
    print("  Current: "); print_dec(cgrp->pids.current); println("");
    
    println("");
    println("  --- PSI ---");
    print("  Some Avg10: "); print_dec(cgrp->psi.some_avg10); println("%");
    print("  Some Avg60: "); print_dec(cgrp->psi.some_avg60); println("%");
    print("  Total Stall: "); print_dec(cgrp->psi.total_stall / 1000000); println(" ms");
}

/* === 批量操作 === */

/* 批量设置参数 */
int cgroup_batch_set_params(struct cgroup *cgrp, const char **params, 
                           uint64_t *values, int count) {
    if (cgrp == NULL || params == NULL || values == NULL) return -1;
    
    spin_lock(&cgrp->lock);
    
    for (int i = 0; i < count; i++) {
        if (strcmp(params[i], "cpu.shares") == 0) {
            cgrp->cpu.shares = (uint32_t)values[i];
        } else if (strcmp(params[i], "memory.limit_in_bytes") == 0) {
            cgrp->memory.limit_in_bytes = values[i];
        } else if (strcmp(params[i], "pids.max") == 0) {
            cgrp->pids.max = (int64_t)values[i];
        }
    }
    
    spin_unlock(&cgrp->lock);
    return 0;
}

/* === 预设配置文件 === */

struct cgroup_profile {
    const char *name;
    uint32_t cpu_shares;
    uint64_t mem_limit;
    int64_t pids_max;
    int blkio_weight;
};

static struct cgroup_profile cgroup_profiles[] = {
    {"unlimited", 1024, ULLONG_MAX, -1, 500},
    {"low-priority", 256, 256*1024*1024, 100, 100},
    {"medium", 512, 512*1024*1024, 500, 300},
    {"high-priority", 2048, 2ULL*1024*1024*1024, 5000, 800},
    {"critical", 4096, ULLONG_MAX, -1, 1000},
};

/* 应用预设配置 */
int cgroup_apply_profile(struct cgroup *cgrp, const char *profile_name) {
    if (cgrp == NULL || profile_name == NULL) return -1;
    
    for (int i = 0; i < 5; i++) {
        if (strcmp(cgroup_profiles[i].name, profile_name) == 0) {
            struct cgroup_profile *p = &cgroup_profiles[i];
            
            spin_lock(&cgrp->lock);
            cgrp->cpu.shares = p->cpu_shares;
            cgrp->memory.limit_in_bytes = p->mem_limit;
            cgrp->pids.max = p->pids_max;
            cgrp->blkio.weight = p->blkio_weight;
            spin_unlock(&cgrp->lock);
            
            return 0;
        }
    }
    
    return -1;  /* 未找到配置文件 */
}

/* === 自动调整 === */

/* 根据负载自动调整资源 */
void cgroup_auto_adjust(struct cgroup *cgrp) {
    if (cgrp == NULL) return;
    
    cgroup_update_psi(cgrp);
    
    /* 高压力时增加资源 */
    if (cgrp->psi.some_avg10 > 80) {
        if (cgrp->cpu.shares < 4096) {
            cgrp->cpu.shares = cgrp->cpu.shares * 11 / 10;  /* 增加 10% */
        }
    }
    /* 低压力时减少资源 */
    else if (cgrp->psi.some_avg10 < 20) {
        if (cgrp->cpu.shares > 64) {
            cgrp->cpu.shares = cgrp->cpu.shares * 9 / 10;  /* 减少 10% */
        }
    }
}

/* === 健康检查 === */

/* 检查 cgroup 健康状态 */
int cgroup_health_check(struct cgroup *cgrp) {
    if (cgrp == NULL) return -1;
    
    int issues = 0;
    
    /* 检查内存压力 */
    if (cgrp->memory.limit_in_bytes < ULLONG_MAX) {
        uint64_t mem_pct = cgrp->memory.usage_in_bytes * 100 / 
                          cgrp->memory.limit_in_bytes;
        if (mem_pct > 95) {
            issues++;
        }
    }
    
    /* 检查 OOM */
    if (cgrp->memory.oom_kill_count > 0) {
        issues++;
    }
    
    /* 检查任务数限制 */
    if (cgrp->pids.max >= 0 && cgrp->pids.current >= (uint64_t)cgrp->pids.max * 9 / 10) {
        issues++;
    }
    
    return issues;
}

/* === 诊断报告 === */

/* 生成诊断报告 */
void cgroup_diagnose(struct cgroup *cgrp) {
    if (cgrp == NULL) return;
    
    println("╔══════════════════════════════════════════╗");
    println("║       Cgroup Diagnostic Report           ║");
    println("╚══════════════════════════════════════════╝");
    
    println("");
    cgroup_print_detailed_stats(cgrp);
    
    println("");
    println("  --- Health Check ---");
    int issues = cgroup_health_check(cgrp);
    if (issues == 0) {
        println("  Status: HEALTHY");
    } else {
        print("  Status: WARNING ("); print_dec(issues); println(" issues)");
    }
    
    println("");
    println("  --- Contentions ---");
    struct contention_info cont;
    if (cgroup_detect_contention(cgrp, &cont) == 0 && cont.detected) {
        print("  Type: ");
        switch (cont.type) {
            case 0: println("CPU"); break;
            case 1: println("Memory"); break;
            case 2: println("IO"); break;
        }
        print("  Severity: "); print_dec(cont.severity); println("%");
    } else {
        println("  No contentions detected");
    }
}
