/*
 * Zratro OS - Cgroups (Control Groups)
 * 
 * 资源控制组，用于限制、记录和隔离进程组使用的物理资源
 */

#ifndef CGROUP_H
#define CGROUP_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* Forward declarations */
struct thread;
struct cgroup;

/* 最大 CPU 数 */
#ifndef MAX_CPUS
#define MAX_CPUS        256
#endif

/* ========== cgroup 控制器类型 ========== */

/* 控制器标志 */
#define CGRP_CONTROLLER_CPU         (1 << 0)    /* CPU 调度 */
#define CGRP_CONTROLLER_CPUACCT     (1 << 1)    /* CPU 使用统计 */
#define CGRP_CONTROLLER_CPUSET      (1 << 2)    /* CPU 亲和性 */
#define CGRP_CONTROLLER_MEMORY      (1 << 3)    /* 内存限制 */
#define CGRP_CONTROLLER_BLKIO       (1 << 4)    /* 块 I/O */
#define CGRP_CONTROLLER_DEVICES     (1 << 5)    /* 设备访问 */
#define CGRP_CONTROLLER_FREEZER     (1 << 6)    /* 冻结进程 */
#define CGRP_CONTROLLER_NET_CLS     (1 << 7)    /* 网络分类 */
#define CGRP_CONTROLLER_NET_PRIO    (1 << 8)    /* 网络优先级 */
#define CGRP_CONTROLLER_PIDS        (1 << 9)    /* 进程数限制 */
#define CGRP_CONTROLLER_PERF_EVENT  (1 << 10)   /* 性能事件 */
#define CGRP_CONTROLLER_HUGETLB     (1 << 11)   /* 大页内存 */

/* cgroup 状态 */
#define CGRP_THAWED         0           /* 正常运行 */
#define CGRP_FREEZING       1           /* 正在冻结 */
#define CGRP_FROZEN         2           /* 已冻结 */

/* cgroup 标志 */
#define CGROUP_FLAG_THREADED        (1 << 0)
#define CGROUP_FLAG_CPU_EXCLUSIVE   (1 << 1)
#define CGROUP_FLAG_MEM_EXCLUSIVE   (1 << 2)

/* ========== CPU 控制器 ========== */

struct cpu_cgroup {
    /* CPU 份额 (相对权重) */
    uint64_t shares;             /* CPU 份额 (默认 1024) */
    uint64_t cfs_quota_us;       /* CFS 配额 (微秒) */
    uint64_t cfs_period_us;      /* CFS 周期 (微秒) */
    uint64_t rt_runtime_us;      /* 实时运行时间 */
    uint64_t rt_period_us;       /* 实时周期 */
    
    /* 统计 */
    uint64_t usage_usec;         /* CPU 使用时间 (微秒) */
    uint64_t user_usec;          /* 用户态时间 */
    uint64_t system_usec;        /* 内核态时间 */
    uint64_t nr_periods;         /* 周期数 */
    uint64_t nr_throttled;       /* 被限制次数 */
    uint64_t throttled_time;     /* 被限制时间 */
};

/* ========== CPU 统计控制器 ========== */

struct cpuacct_cgroup {
    /* CPU 使用统计 */
    uint64_t usage;              /* 总使用时间 (纳秒) */
    uint64_t usage_user;         /* 用户态时间 */
    uint64_t usage_system;       /* 内核态时间 */
    
    /* Per-CPU 统计 */
    uint64_t usage_percpu[MAX_CPUS];
    uint64_t usage_percpu_user[MAX_CPUS];
    uint64_t usage_percpu_system[MAX_CPUS];
};

/* ========== CPUSET 控制器 ========== */

struct cpuset_cgroup {
    /* CPU 掩码 */
    uint64_t cpus_allowed;       /* 允许的 CPU */
    uint64_t mems_allowed;       /* 允许的内存节点 */
    
    /* 标志 */
    int cpu_exclusive;           /* CPU 独占 */
    int mem_exclusive;           /* 内存独占 */
    int mem_hardwall;            /* 内存硬墙 */
    int sched_load_balance;      /* 负载均衡 */
    int sched_relax_domain_level;/* 放松域级别 */
    
    /* 内存策略 */
    int memory_migrate;          /* 内存迁移 */
    int memory_spread_page;      /* 页 spreading */
    int memory_spread_slab;      /* slab spreading */
};

/* ========== 内存控制器 ========== */

struct memory_cgroup {
    /* 限制 */
    uint64_t limit_in_bytes;     /* 内存限制 (字节) */
    uint64_t memsw_limit_in_bytes; /* 内存+交换限制 */
    uint64_t soft_limit_in_bytes;  /* 软限制 */
    
    /* 统计 */
    uint64_t usage_in_bytes;     /* 当前使用 */
    uint64_t memsw_usage_in_bytes; /* 内存+交换使用 */
    uint64_t max_usage_in_bytes; /* 最大使用 */
    uint64_t memsw_max_usage_in_bytes;
    
    /* 页面统计 */
    uint64_t cache;              /* 缓存页 */
    uint64_t rss;                /* 驻留集 */
    uint64_t rss_huge;           /* 大页 RSS */
    uint64_t mapped_file;        /* 映射文件 */
    uint64_t swap;               /* 交换使用 */
    uint64_t pgpgin;             /* 页换入 */
    uint64_t pgpgout;            /* 页换出 */
    uint64_t pgfault;            /* 页错误 */
    uint64_t pgmajfault;         /* 主要页错误 */
    
    /* 阈值 */
    uint64_t threshold;          /* 通知阈值 */
    uint64_t swap_threshold;     /* 交换阈值 */
    
    /* OOM 控制 */
    int oom_control;             /* OOM 控制 */
    int oom_kill_disable;        /* 禁用 OOM kill */
    uint64_t under_oom;          /* 是否在 OOM 状态 */
    uint64_t oom_kill;           /* OOM kill 计数 */
    uint64_t oom_kill_count;     /* OOM kill 总次数 */
};

/* ========== 块 I/O 控制器 ========== */

struct blkio_cgroup {
    /* 权重 */
    uint64_t weight;             /* I/O 权重 (100-1000) */
    uint64_t leaf_weight;        /* 叶子权重 */
    
    /* 统计 */
    uint64_t io_service_bytes;   /* I/O 服务字节数 */
    uint64_t io_serviced;        /* I/O 服务请求数 */
    uint64_t io_service_time;    /* I/O 服务时间 */
    uint64_t io_wait_time;       /* I/O 等待时间 */
    uint64_t io_merged;          /* 合并的 I/O */
    uint64_t io_queued;          /* 排队的 I/O */
    
    /* 限速 (字节/秒) */
    uint64_t read_bps_device;    /* 读 BPS */
    uint64_t write_bps_device;   /* 写 BPS */
    uint64_t read_iops_device;   /* 读 IOPS */
    uint64_t write_iops_device;  /* 写 IOPS */
    
    /* 节流 */
    uint64_t throttle_io_service_bytes;
    uint64_t throttle_io_serviced;
};

/* ========== 设备控制器 ========== */

struct device_rule {
    char type;                   /* 'a'=all, 'b'=block, 'c'=char */
    int major;                   /* 主设备号 */
    int minor;                   /* 次设备号 (-1=所有) */
    char access[4];              /* rwa (读/写/mknod) */
    
    struct list_head list;
};

struct devices_cgroup {
    /* 允许列表 */
    struct list_head allow_list;
    struct list_head deny_list;
    
    /* 策略 */
    int allow_default;           /* 默认允许 */
};

/* ========== PIDs 控制器 ========== */

struct pids_cgroup {
    /* 限制 */
    int64_t max;                 /* 最大进程数 (-1=无限制) */
    
    /* 统计 */
    uint64_t current;            /* 当前进程数 */
};

/* ========== Freezer 控制器 ========== */

struct freezer_cgroup {
    /* 状态 */
    int state;                   /* THAWED/FREEZING/FROZEN */
    spinlock_t lock;
};

/* ========== PSI 压力失速信息 ========== */

struct psi_cgroup {
    uint64_t some_avg10;         /* 10 秒平均 - 部分失速 */
    uint64_t some_avg60;         /* 60 秒平均 */
    uint64_t some_avg300;        /* 300 秒平均 */
    uint64_t full_avg10;         /* 10 秒平均 - 完全失速 */
    uint64_t full_avg60;
    uint64_t full_avg300;
    uint64_t total_stall;        /* 总失速时间 */
};

/* ========== 委托信息 ========== */

struct cgroup_delegation {
    int enabled;
    int uid;                     /* 委托用户 ID */
    int gid;                     /* 委托组 ID */
    uint32_t permissions;        /* 权限位 */
};

/* ========== cgroup 结构 ========== */

struct cgroup {
    /* 基本信息 */
    uint64_t id;                 /* cgroup ID */
    char name[64];               /* 名称 */
    uint64_t flags;              /* 标志 */
    
    /* 层级 */
    struct cgroup *parent;       /* 父 cgroup */
    struct list_head children;   /* 子 cgroup 链表 */
    struct list_head sibling;    /* 兄弟链表 */
    struct cgroup_root *root;    /* 所属根 */
    
    /* 进程列表 */
    struct list_head tasks;      /* 任务链表 */
    uint64_t nr_tasks;           /* 任务数 */
    
    /* 控制器数据 */
    struct cpu_cgroup cpu;
    struct cpuacct_cgroup cpuacct;
    struct cpuset_cgroup cpuset;
    struct memory_cgroup memory;
    struct blkio_cgroup blkio;
    struct devices_cgroup devices;
    struct pids_cgroup pids;
    struct freezer_cgroup freezer;
    
    /* PSI 压力失速信息 */
    struct psi_cgroup psi;
    uint64_t psi_last_update;
    
    /* 委托信息 */
    struct cgroup_delegation delegation;
    
    /* cpuset 扩展 */
    uint64_t cpuset_cpus;        /* CPU 掩码 */
    uint64_t cpuset_mems;        /* 内存节点掩码 */
    
    /* 引用计数 */
    atomic_t ref_count;
    
    /* 文件系统 */
    struct vfs_node *dentry;     /* 文件系统节点 */
    
    spinlock_t lock;
};

/* ========== cgroup 根 (层级) ========== */

struct cgroup_root {
    /* 层级 ID */
    int hierarchy_id;
    
    /* 启用的控制器 */
    uint64_t subsystem_mask;
    
    /* 根 cgroup */
    struct cgroup cgrp;
    
    /* 名称 */
    char name[64];
    
    /* 标志 */
    int flags;
    #define CGRP_ROOT_NOPREFIX    (1 << 0)  /* 无前缀 */
    #define CGRP_ROOT_XATTR       (1 << 1)  /* 支持 xattr */
    
    /* 链表 */
    struct list_head root_list;
};

/* ========== cgroup 子系统 (控制器) ========== */

struct cgroup_subsys {
    const char *name;
    int id;
    uint64_t mask;
    
    /* 操作 */
    int (*create)(struct cgroup *cgrp);
    void (*destroy)(struct cgroup *cgrp);
    int (*can_attach)(struct cgroup *cgrp, struct thread *tsk);
    void (*attach)(struct cgroup *cgrp, struct thread *tsk);
    int (*fork)(struct thread *tsk);
    void (*exit)(struct thread *tsk);
    void (*bind)(struct cgroup *root);
    
    /* 文件操作 */
    int (*populate)(struct cgroup *cgrp);
    
    /* 早期初始化 */
    int early_init;
};

/* ========== 全局定义 ========== */

#define CGROUP_SUBSYS_COUNT     12

extern struct cgroup_root cgroup_root;
extern struct list_head cgroup_roots;
extern spinlock_t cgroup_mutex;

/* ========== 函数原型 ========== */

/* 初始化 */
int cgroup_init(void);

/* cgroup 创建/销毁 */
struct cgroup *cgroup_create(struct cgroup *parent, const char *name);
void cgroup_destroy(struct cgroup *cgrp);

/* 进程操作 */
int cgroup_attach_task(struct cgroup *cgrp, struct thread *tsk);
int cgroup_attach_task_all(struct cgroup *cgrp);
void cgroup_exit_task(struct thread *tsk);

/* 查找 */
struct cgroup *cgroup_find(const char *path);
struct cgroup *cgroup_get_current(struct thread *tsk);

/* 资源限制检查 */
int cgroup_can_fork(struct thread *tsk);
int cgroup_can_attach_cpu(struct cgroup *cgrp);
int cgroup_can_attach_memory(struct cgroup *cgrp);
int cgroup_can_attach_pids(struct cgroup *cgrp);

/* 控制器操作 */
int cgroup_subsys_init(void);
struct cgroup_subsys *cgroup_get_subsys(int id);

/* 系统调用 */
int sys_cgroup_create(const char *path, int flags);
int sys_cgroup_attach(const char *path, int pid);
int sys_cgroup_set_param(const char *path, const char *param, uint64_t value);
int sys_cgroup_get_param(const char *path, const char *param, uint64_t *value);

/* 文件系统接口 */
int cgroup_fs_init(void);
struct vfs_node *cgroup_get_dentry(struct cgroup *cgrp);

/* ========== 辅助宏 ========== */

#define for_each_subsys(ss, ssid) \
    for ((ssid) = 0; (ssid) < CGROUP_SUBSYS_COUNT && \
         ((ss) = cgroup_subsys[ssid]); (ssid)++)

#endif /* CGROUP_H */
