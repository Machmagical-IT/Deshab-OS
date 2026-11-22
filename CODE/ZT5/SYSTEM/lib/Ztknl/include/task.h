#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "vmm.h"
#include "abi.h"
#include "rlimit.h"

/* 前向声明 */
struct fd_table;
struct cgroup;
struct signal_state;
struct pe_module;

/* 最大进程数 */
#define MAX_PROCESSES   256
#define MAX_THREADS     1024

/* 最大 CPU 数 */
#ifndef MAX_CPUS
#define MAX_CPUS        256
#endif

/* 线程状态 */
typedef enum {
    THREAD_STATE_UNUSED = 0,
    THREAD_STATE_READY,
    THREAD_STATE_RUNNING,
    THREAD_STATE_BLOCKED,
    THREAD_STATE_SLEEPING,
    THREAD_STATE_TERMINATED
} thread_state_t;

/* 线程标志 */
#define THREAD_FLAG_NEED_RESCHED   (1 << 0)
#define THREAD_FLAG_KTHREAD        (1 << 1)
#define THREAD_FLAG_EXITING        (1 << 2)
#define THREAD_FLAG_FROZEN         (1 << 3)
#define THREAD_FLAG_SIGPENDING     (1 << 4)
#define THREAD_FLAG_STOPPED        (1 << 5)
#define THREAD_FLAG_TRACING        (1 << 6)
#define THREAD_FLAG_PTRACED        (1 << 7)

/* 进程状态 */
typedef enum {
    PROCESS_STATE_UNUSED = 0,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_ZOMBIE,
    PROCESS_STATE_TERMINATED
} process_state_t;

/* 线程优先级 */
typedef enum {
    PRIORITY_IDLE      = 0,
    PRIORITY_LOWEST    = 1,
    PRIORITY_LOW       = 2,
    PRIORITY_NORMAL    = 3,
    PRIORITY_HIGH      = 4,
    PRIORITY_HIGHEST   = 5,
    PRIORITY_REALTIME  = 6
} priority_t;

/* 默认时间片 (tick 数) */
#define DEFAULT_TIME_SLICE  10

/* nice 值范围 */
#define MIN_NICE    -20
#define MAX_NICE    19
#define DEFAULT_NICE 0

/* ═══ 原创优化：实时调度策略定义 ═══ */
#define SCHED_NORMAL    0   /* 普通分时调度 */
#define SCHED_FIFO      1   /* 先进先出实时调度 */
#define SCHED_RR        2   /* 时间片轮转实时调度 */
#define SCHED_BATCH     3   /* 批处理调度 */
#define SCHED_IDLE      5   /* 空闲任务调度 */
#define SCHED_DEADLINE  6   /* 截止时间调度 */

/* 实时优先级范围 */
#define MIN_RT_PRIO     0
#define MAX_RT_PRIO     99
#define DEFAULT_RT_PRIO 50

/* 实时时间片 (SCHED_RR) */
#define RT_TIME_SLICE   100  /* 100ms */

/* CPU 上下文 (保存/恢复的寄存器) */
/*
 * 与 context_asm.S 中的偏移量匹配
 * 
 * 偏移量计算:
 *   r15: 0x00, r14: 0x08, r13: 0x10, r12: 0x18
 *   r11: 0x20, r10: 0x28, r9:  0x30, r8:  0x38
 *   rdi: 0x40, rsi: 0x48, rbp: 0x50, rbx: 0x58
 *   rdx: 0x60, rcx: 0x68, rax: 0x70
 *   rip: 0x78, cs:  0x80, rflags: 0x88
 *   rsp: 0x90, ss:  0x98
 *   user_rsp: 0xA0 (用户态栈指针，用于 iretq)
 *
 * 注意：
 * - 内核线程：rsp = 内核栈，user_rsp 未使用
 * - 用户线程：rsp = 内核栈，user_rsp = 用户栈
 */
struct cpu_context {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
    uint64_t user_rsp;    /* 用户态栈指针 (仅用户线程使用) */
} __attribute__((packed));

/* ========== 编译时验证结构体布局 ========== */
/* 参考 context_asm.S 中的偏移量 */

_Static_assert(sizeof(struct cpu_context) == 168,
    "cpu_context size mismatch! Expected 168 bytes. Check struct layout vs context_asm.S");

_Static_assert(offsetof(struct cpu_context, r15) == 0x00,
    "cpu_context.r15 offset must be 0x00");
_Static_assert(offsetof(struct cpu_context, r8) == 0x38,
    "cpu_context.r8 offset must be 0x38");
_Static_assert(offsetof(struct cpu_context, rdi) == 0x40,
    "cpu_context.rdi offset must be 0x40");
_Static_assert(offsetof(struct cpu_context, rax) == 0x70,
    "cpu_context.rax offset must be 0x70");
_Static_assert(offsetof(struct cpu_context, rip) == 0x78,
    "cpu_context.rip offset must be 0x78");
_Static_assert(offsetof(struct cpu_context, cs) == 0x80,
    "cpu_context.cs offset must be 0x80");
_Static_assert(offsetof(struct cpu_context, rflags) == 0x88,
    "cpu_context.rflags offset must be 0x88");
_Static_assert(offsetof(struct cpu_context, rsp) == 0x90,
    "cpu_context.rsp offset must be 0x90");
_Static_assert(offsetof(struct cpu_context, ss) == 0x98,
    "cpu_context.ss offset must be 0x98");
_Static_assert(offsetof(struct cpu_context, user_rsp) == 0xA0,
    "cpu_context.user_rsp offset must be 0xA0");

/* ========== CFS 调度实体 ========== */

struct sched_entity {
    /* 红黑树节点 */
    struct rb_node run_node;
    
    /* 是否在运行队列中 */
    int on_rq;
    
    /* 权重和优先级 - 热路径访问 */
    uint32_t weight;            /* 调度权重 */
    uint32_t wmult_idx;         /* 逆权重表索引 (nice+20)，O(1) 查找 */
    int nice;                   /* nice 值 (-20 到 19) */
    
    /* 虚拟运行时间 - 最热路径，单独缓存行 */
    uint64_t vruntime;          /* 虚拟运行时间 */
    uint64_t sum_exec_runtime;  /* 累计执行时间 */
    uint64_t prev_sum_exec_runtime; /* 上次累计执行时间 */
    
    /* 统计 */
    uint64_t sleep_start;       /* 睡眠开始时间 */
    uint64_t sleep_max;         /* 最大睡眠时间 */
    uint64_t block_start;       /* 阻塞开始时间 */
    uint64_t block_max;         /* 最大阻塞时间 */
    uint64_t wait_start;        /* 等待开始时间 */
    uint64_t wait_max;          /* 最大等待时间 */
    uint64_t wait_sum;          /* 累计等待时间 */
    uint32_t wait_count;        /* 等待次数 */
    
    /* 时间片 */
    uint64_t slice;             /* 当前时间片 */
    
    /* 所属线程 (后向引用) */
    struct thread *thread;
} __attribute__((aligned(64)));

/* 线程控制块 (TCB) */
struct thread {
    /* 基本信息 - 热路径访问 */
    uint32_t tid;                   /* 线程 ID */
    uint32_t pid;                   /* 所属进程 ID */
    uint32_t flags;                 /* 线程标志 */
    int cpu;                        /* 运行的 CPU (=-1 表示无) */
    
    /* 状态和优先级 - 热路径访问 */
    thread_state_t state;           /* 线程状态 */
    priority_t priority;            /* 静态优先级 */
    int32_t time_slice;             /* 剩余时间片 */
    int32_t default_slice;          /* 默认时间片 */
    
    /* ═══ 原创优化：实时调度支持 ═══ */
    int sched_policy;               /* 调度策略 (SCHED_NORMAL/FIFO/RR) */
    int rt_priority;                /* 实时优先级 (0-99) */
    int rt_time_slice;              /* 实时时间片 (SCHED_RR) */
    
    /* CFS 调度实体 */
    struct sched_entity sched_entity;
    
    /* 调度相关 */
    uint64_t last_ran;              /* 上次运行时间 */
    uint64_t total_run_time;        /* 总运行时间 */
    uint64_t wake_time;             /* 唤醒时间 (tick) */
    
    /* ═══ Per-Thread 调度统计 (schedstat) ═══ */
    uint64_t sched_wait_sum;        /* 累计等待时间 (tick) */
    uint64_t sched_run_sum;         /* 累计运行时间 (tick) */
    uint64_t sched_last_wait_start; /* 本次等待开始时间戳 */
    uint32_t sched_nvcsw;           /* 自愿上下文切换次数 */
    uint32_t sched_nivcsw;          /* 非自愿上下文切换次数 */
    
    /* ═══ 原创优化：CPU 亲和性与 NUMA 感知 ═══ */
    uint32_t cpu_affinity;          /* CPU 亲和性掩码 */
    int last_cpu;                   /* 上次运行的 CPU */
    int numa_preferred_node;        /* NUMA 首选节点 */
    int numa_policy;                /* NUMA 内存策略 */
    uint64_t *affinity_history;     /* 亲和性历史 */
    
    /* CPU 上下文 - 必须在固定偏移量 (syscall_fast.S 使用 0x1C8) */
    struct cpu_context context;     /* CPU 上下文 */

    /* ═══ 优先级继承 (PI) 支持 ═══ */
    int boosted_priority;           /* 提升后的优先级 (0=未提升) */
    int pi_original_prio;           /* PI 原始优先级 (提升前保存) */
    void *pi_waiting_for;           /* 等待的锁 (用于 PI 链追踪) */
    struct list_head pi_waiters;    /* 被此线程阻塞的等待者链表 */
    struct list_head pi_list;       /* PI 链表节点 (挂入 holder 的 pi_waiters) */
    volatile int pi_lock;           /* PI 操作自旋锁 */

    /* 栈信息 */
    uint64_t kernel_stack;          /* 内核栈 (虚拟地址) */
    uint64_t user_stack;            /* 用户栈 (虚拟地址) */
    size_t stack_size;              /* 栈大小 */
    
    /* 等待相关 */
    void *wait_data;                /* 等待数据 */
    struct list_head wait_list;     /* 等待队列链表 */
    
    /* cgroup */
    struct cgroup *cgroup;          /* 所属 cgroup */
    struct list_head cgroup_list;   /* cgroup 任务链表 */
    
    /* 调度链表 */
    struct list_head run_list;      /* 运行队列链表 */
    struct list_head sleep_list;    /* 睡眠链表 (用于 scheduler_tick 快速唤醒扫描) */
    struct thread *next;            /* 下一个线程 (调度队列) */
    struct thread *prev;            /* 上一个线程 */
    struct thread *process_next;    /* 同进程的下一个线程 */
    
    /* Linux 兼容 */
    int *clear_child_tid;           /* set_tid_address 指针 */
    void *robust_list;              /* robust futex 列表 */
    
    /* 名称 */
    char name[32];                  /* 线程名称 */
} __attribute__((aligned(64)));

/* 验证 thread->context.rip 的总偏移量 (用于 syscall_fast.S) */
_Static_assert(offsetof(struct thread, context) + offsetof(struct cpu_context, rip) == 0x1C8,
    "thread->context.rip offset must be 0x1C8 (used in syscall_fast.S)");

/* 进程控制块 (PCB) */
struct process {
    uint32_t pid;                   /* 进程 ID */
    uint32_t parent_pid;            /* 父进程 ID */
    
    process_state_t state;          /* 进程状态 */
    
    struct address_space *space;    /* 地址空间 */
    struct fd_table *fd_table;      /* 文件描述符表 */
    
    struct thread *threads;         /* 线程链表 */
    uint32_t thread_count;          /* 线程数 */
    
    uint64_t entry_point;           /* 入口点 */
    
    abi_type_t abi;                 /* ABI 类型 (Zratro/Linux/...) */
    
    int exit_code;                  /* 退出码 */
    
    char name[64];                  /* 进程名称 */
    
    /* 进程级堆管理 (brk) */
    uint64_t brk_start;             /* 堆起始地址 */
    uint64_t brk_end;               /* 堆结束地址 (当前 brk) */
    uint64_t brk_limit;             /* 堆最大限制 */
    
    /* PID 哈希表链接 */
    struct process *hash_next;      /* 哈希表下一个 */
    struct process *hash_prev;      /* 哈希表上一个 */
    
    /* 信号处理 (需要时初始化) */
    struct signal_state *signals;   /* 信号状态 */
    
    /* 统计信息 */
    uint64_t start_time;            /* 启动时间 */
    uint64_t user_time;             /* 用户态时间 */
    uint64_t kernel_time;           /* 内核态时间 */

    /* 用户身份 */
    uint32_t uid;                   /* 用户 ID */
    uint32_t gid;                   /* 组 ID */
    char username[32];              /* 用户名 */

    /* 工作目录 */
    char cwd[256];                  /* 当前工作目录 (绝对路径) */

    /* ═══ 每进程资源限制 ═══ */
    struct rlimit process_limits[RLIMIT_COUNT]; /* 进程级 rlimit */

    /* PE 模块 (Windows 进程) */
    struct pe_module *pe_mod;       /* 关联的 PE 模块，进程退出时清理 */
};

/* ========== 辅助宏 ========== */

/* 检查线程是否有效 */
#define THREAD_VALID(t) ((t) != NULL && (t)->tid != 0 && (t)->state != THREAD_STATE_UNUSED)

/* 检查进程是否有效 */
#define PROCESS_VALID(p) ((p) != NULL && (p)->pid != 0 && (p)->state != PROCESS_STATE_UNUSED)

/* 检查线程是否在运行 */
#define THREAD_IS_RUNNING(t) ((t) != NULL && (t)->state == THREAD_STATE_RUNNING)

/* 检查线程是否可调度 */
#define THREAD_IS_RUNNABLE(t) ((t) != NULL && \
    ((t)->state == THREAD_STATE_READY || (t)->state == THREAD_STATE_RUNNING))

/* 检查是否为内核线程 */
#define THREAD_IS_KTHREAD(t) ((t) != NULL && ((t)->flags & THREAD_FLAG_KTHREAD))

/* 检查是否为用户线程 */
#define THREAD_IS_USER(t) ((t) != NULL && !((t)->flags & THREAD_FLAG_KTHREAD))

/* 设置线程名称 */
static inline void thread_set_name(struct thread *t, const char *name) {
    if (t && name) {
        int i;
        for (i = 0; i < 31 && name[i]; i++) {
            t->name[i] = name[i];
        }
        t->name[i] = '\0';
    }
}

/* 设置进程名称 */
static inline void process_set_name(struct process *p, const char *name) {
    if (p && name) {
        int i;
        for (i = 0; i < 63 && name[i]; i++) {
            p->name[i] = name[i];
        }
        p->name[i] = '\0';
    }
}

/* 进程/线程管理 */

/* 初始化任务管理器 */
void task_init(void);

/* 创建进程 */
struct process *process_create(const char *name, uint64_t entry_point);

/* 销毁进程 */
void process_destroy(struct process *proc);

/* 获取当前进程 */
struct process *process_get_current(void);

/* 获取当前进程的 UID */
uint32_t process_get_current_uid(void);

/* 获取当前进程的 GID */
uint32_t process_get_current_gid(void);

/* 获取进程 */
struct process *process_get_by_pid(uint32_t pid);
struct process *process_find_zombie_child(uint32_t parent_pid);
int process_get_children(uint32_t pid, uint32_t *children, uint32_t *count);

/* 创建线程 */
struct thread *thread_create(struct process *proc, const char *name, 
                             uint64_t entry_point, void *arg);

/* 创建内核线程 */
struct thread *thread_create_kernel(const char *name, void (*entry)(void *), void *arg);

/* 销毁线程 */
void thread_destroy(struct thread *thread);

/* 获取当前线程 */
struct thread *thread_get_current(void);

/* 获取线程 */
struct thread *thread_get_by_tid(uint32_t tid);

/* 设置线程状态 */
void thread_set_state(struct thread *thread, thread_state_t state);

/* 线程休眠 */
void thread_sleep(uint64_t ticks);

/* 线程退出 */
void thread_exit(int code);

/* 进程退出 */
void process_exit(int code);

/* 进程复制 (fork) */
struct process *process_fork(struct process *parent);

/* 进程冻结/解冻 */
int task_freeze(struct process *proc);
int task_thaw(struct process *proc);
int freeze_all_tasks(void);
int thaw_all_tasks(void);

/* 获取 tick 计数 */
uint64_t get_tick_count(void);

/* 增加 tick 计数 */
void increment_tick_count(void);

/* 获取线程数量 */
uint32_t get_thread_count(void);

/* 获取进程数量 */
uint32_t get_process_count(void);

/* 打印任务统计信息 */
void task_stats(void);

/* 当前线程 (全局可见) */
extern struct thread *current_thread;

/* Per-CPU 睡眠队列 */
struct sleep_queue {
    struct list_head list;
    volatile int lock;
} __attribute__((aligned(64)));

extern struct sleep_queue percpu_sleepq[];

/* 获取当前任务（进程）的辅助函数 */
static inline struct process *get_current_task(void) {
    if (current_thread == NULL) return NULL;
    return process_get_by_pid(current_thread->pid);
}

#define current_task (get_current_task())

/* 任务结构兼容性宏 */
#define current_task_id (current_task ? current_task->pid : 0)
#define current_task_name (current_task ? current_task->name : "unknown")
#define current_task_space (current_task ? current_task->space : NULL)

#endif
