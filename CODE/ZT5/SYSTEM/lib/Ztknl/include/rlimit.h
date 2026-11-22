/*
 * Zratro OS - Resource Limits (资源限制系统)
 * 
 * 提供进程级别的资源限制：
 * 1. CPU 时间限制
 * 2. 内存限制
 * 3. 文件描述符限制
 * 4. 进程数限制
 * 5. 文件大小限制
 */

#ifndef RLIMIT_H
#define RLIMIT_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* 前向声明 */
struct process;

/* ========== 资源类型 ========== */

typedef enum {
    RLIMIT_CPU = 0,             /* CPU 时间 (秒) */
    RLIMIT_FSIZE,               /* 文件大小 (字节) */
    RLIMIT_DATA,                /* 数据段大小 (字节) */
    RLIMIT_STACK,               /* 栈大小 (字节) */
    RLIMIT_CORE,                /* Core 文件大小 (字节) */
    RLIMIT_RSS,                 /* 驻留集大小 (字节) */
    RLIMIT_NPROC,               /* 进程数 */
    RLIMIT_NOFILE,              /* 文件描述符数 */
    RLIMIT_MEMLOCK,             /* 锁定内存 (字节) */
    RLIMIT_AS,                  /* 地址空间 (字节) */
    RLIMIT_LOCKS,               /* 文件锁数 */
    RLIMIT_SIGPENDING,          /* 待处理信号数 */
    RLIMIT_MSGQUEUE,            /* 消息队列字节数 */
    RLIMIT_NICE,                /* Nice 值 */
    RLIMIT_RTPRIO,              /* 实时优先级 */
    RLIMIT_RTTIME,              /* 实时时间 (微秒) */
    RLIMIT_COUNT
} rlimit_resource_t;

/* ========== 资源限制结构 ========== */

struct rlimit {
    uint64_t rlim_cur;          /* 软限制 */
    uint64_t rlim_max;          /* 硬限制 */
};

/* ========== 资源使用结构 ========== */

struct rusage {
    /* 用户态时间 */
    uint64_t ru_utime_sec;
    uint64_t ru_utime_usec;
    
    /* 内核态时间 */
    uint64_t ru_stime_sec;
    uint64_t ru_stime_usec;
    
    /* 资源使用 */
    uint64_t ru_maxrss;         /* 最大驻留集大小 */
    uint64_t ru_ixrss;          /* 共享内存大小 */
    uint64_t ru_idrss;          /* 非共享数据大小 */
    uint64_t ru_isrss;          /* 非共享栈大小 */
    uint64_t ru_minflt;         /* 软页错误 */
    uint64_t ru_majflt;         /* 硬页错误 */
    uint64_t ru_nswap;          /* 交换次数 */
    uint64_t ru_inblock;        /* 块输入操作 */
    uint64_t ru_oublock;        /* 块输出操作 */
    uint64_t ru_msgsnd;         /* 发送消息数 */
    uint64_t ru_msgrcv;         /* 接收消息数 */
    uint64_t ru_nsignals;       /* 信号数 */
    uint64_t ru_nvcsw;          /* 自愿上下文切换 */
    uint64_t ru_nivcsw;         /* 非自愿上下文切换 */
};

/* ========== 进程资源统计 ========== */

struct process_rusage {
    uint64_t start_time;        /* 启动时间 */
    
    /* CPU 时间 */
    uint64_t user_time;         /* 用户态时间 */
    uint64_t system_time;       /* 内核态时间 */
    
    /* 内存使用 */
    uint64_t peak_rss;          /* 峰值驻留集 */
    uint64_t current_rss;       /* 当前驻留集 */
    uint64_t virtual_size;      /* 虚拟内存大小 */
    
    /* I/O 统计 */
    uint64_t read_bytes;        /* 读取字节数 */
    uint64_t write_bytes;       /* 写入字节数 */
    uint64_t read_ops;          /* 读操作数 */
    uint64_t write_ops;         /* 写操作数 */
    
    /* 页错误 */
    uint64_t minor_faults;
    uint64_t major_faults;
    
    /* 上下文切换 */
    uint64_t voluntary_switches;
    uint64_t involuntary_switches;
};

/* ========== 系统默认限制 ========== */

#define RLIM_INFINITY           ((uint64_t)-1)

/* 默认软/硬限制 */
#define RLIMIT_CPU_DEFAULT      RLIM_INFINITY
#define RLIMIT_FSIZE_DEFAULT    RLIM_INFINITY
#define RLIMIT_DATA_DEFAULT     (256 * 1024 * 1024)   /* 256 MB */
#define RLIMIT_STACK_DEFAULT    (8 * 1024 * 1024)     /* 8 MB */
#define RLIMIT_CORE_DEFAULT     0
#define RLIMIT_RSS_DEFAULT      RLIM_INFINITY
#define RLIMIT_NPROC_DEFAULT    1024
#define RLIMIT_NOFILE_DEFAULT   1024
#define RLIMIT_MEMLOCK_DEFAULT  (64 * 1024 * 1024)    /* 64 MB */
#define RLIMIT_AS_DEFAULT       RLIM_INFINITY
#define RLIMIT_LOCKS_DEFAULT    RLIM_INFINITY
#define RLIMIT_SIGPENDING_DEFAULT 1024
#define RLIMIT_MSGQUEUE_DEFAULT (256 * 1024)          /* 256 KB */
#define RLIMIT_NICE_DEFAULT     0
#define RLIMIT_RTPRIO_DEFAULT   0
#define RLIMIT_RTTIME_DEFAULT   200000                /* 200 ms */

/* ========== 全局资源限制管理器 ========== */

struct rlimit_manager {
    /* 系统全局限制 */
    struct rlimit system_limits[RLIMIT_COUNT];
    
    /* 系统资源使用 */
    uint64_t total_memory;
    uint64_t used_memory;
    uint64_t total_processes;
    uint64_t total_fds;
    
    /* 锁 */
    spinlock_t lock;
};

/* ========== 公共 API ========== */

/* 初始化 */
int rlimit_init(struct rlimit_manager *mgr);

/* ========== 进程限制 ========== */

/* 获取进程资源限制 */
int sys_getrlimit(rlimit_resource_t resource, struct rlimit *rlim);

/* 设置进程资源限制 */
int sys_setrlimit(rlimit_resource_t resource, const struct rlimit *rlim);

/* 进程资源限制 (扩展) */
int sys_prlimit(uint64_t pid, rlimit_resource_t resource,
                const struct rlimit *new_rlim, struct rlimit *old_rlim);

/* ========== 资源使用 ========== */

/* 获取资源使用 */
int sys_getrusage(int who, struct rusage *usage);

/* 更新进程资源使用 */
void rlimit_update_usage(uint64_t pid, struct process_rusage *usage);

/* 检查资源限制 */
int rlimit_check(uint64_t pid, rlimit_resource_t resource, uint64_t value);

/* ========== 优先级 ========== */

/* 设置优先级 */
int sys_setpriority(int which, uint64_t who, int prio);

/* 获取优先级 */
int sys_getpriority(int which, uint64_t who);

/* ========== 调度策略 ========== */

/* 调度参数结构 */
struct sched_param {
    int sched_priority;
};

/* 调度策略 */
#define SCHED_OTHER     0
#define SCHED_FIFO      1
#define SCHED_RR        2
#define SCHED_BATCH     3
#define SCHED_IDLE      5
#define SCHED_DEADLINE  6

/* 设置调度策略 */
int sys_sched_setscheduler(uint64_t pid, int policy, const struct sched_param *param);

/* 获取调度策略 */
int sys_sched_getscheduler(uint64_t pid);

/* 设置调度参数 */
int sys_sched_setparam(uint64_t pid, const struct sched_param *param);

/* 获取调度参数 */
int sys_sched_getparam(uint64_t pid, struct sched_param *param);

/* ========== 工具函数 ========== */

/* 资源名称 */
const char *rlimit_name(rlimit_resource_t resource);

/* 资源缩放因子 */
uint64_t rlimit_scale(rlimit_resource_t resource);

/* 打印进程限制 */
void rlimit_print_limits(uint64_t pid);

/* 全局管理器 */
struct rlimit_manager *rlimit_get_global(void);
int rlimit_global_init(void);

/* 进程级 rlimit 初始化 */
void rlimit_init_process(struct process *proc);
void rlimit_inherit_process(struct process *child, struct process *parent);

#endif /* RLIMIT_H */
