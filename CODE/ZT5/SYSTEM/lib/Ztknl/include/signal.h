/*
 * Zratro OS - Signal Handling Subsystem
 * 
 * Unix 风格的信号处理机制
 */

#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "task.h"

/* 前向声明 */
struct process;

/* 信号编号 */
#define SIGHUP          1       /* 挂起 */
#define SIGINT          2       /* 中断 (Ctrl+C) */
#define SIGQUIT         3       /* 退出 (Ctrl+\) */
#define SIGILL          4       /* 非法指令 */
#define SIGTRAP         5       /* 跟踪陷阱 */
#define SIGABRT         6       /* 中止 */
#define SIGBUS          7       /* 总线错误 */
#define SIGFPE          8       /* 浮点异常 */
#define SIGKILL         9       /* 强制终止 */
#define SIGUSR1         10      /* 用户信号1 */
#define SIGSEGV         11      /* 段错误 */
#define SIGUSR2         12      /* 用户信号2 */
#define SIGPIPE         13      /* 管道破裂 */
#define SIGALRM         14      /* 闹钟 */
#define SIGTERM         15      /* 终止 */
#define SIGSTKFLT       16      /* 栈错误 */
#define SIGCHLD         17      /* 子进程状态改变 */
#define SIGCONT         18      /* 继续 */
#define SIGSTOP         19      /* 停止 */
#define SIGTSTP         20      /* 终端停止 (Ctrl+Z) */
#define SIGTTIN         21      /* 后台读取 */
#define SIGTTOU         22      /* 后台写入 */
#define SIGURG          23      /* 紧急数据 */
#define SIGXCPU         24      /* CPU 时间超限 */
#define SIGXFSZ         25      /* 文件大小超限 */
#define SIGVTALRM       26      /* 虚拟闹钟 */
#define SIGPROF         27      /* 性能分析闹钟 */
#define SIGWINCH        28      /* 窗口大小改变 */
#define SIGIO           29      /* I/O 就绪 */
#define SIGPWR          30      /* 电源故障 */
#define SIGSYS          31      /* 错误系统调用 */

#define SIGRTMIN        32      /* 实时信号起始 */
#define SIGRTMAX        64      /* 实时信号结束 */

#define NSIG            65      /* 信号总数 */

/* 信号默认行为 */
#define SIG_DFL         ((void (*)(int))0)     /* 默认处理 */
#define SIG_IGN         ((void (*)(int))1)     /* 忽略 */
#define SIG_ERR         ((void (*)(int))-1)    /* 错误 */

/* 信号标志 */
#define SA_NOCLDSTOP    0x00000001  /* 子进程停止时不产生 SIGCHLD */
#define SA_NOCLDWAIT    0x00000002  /* 子进程退出时不会成为僵尸 */
#define SA_SIGINFO      0x00000004  /* 使用 sa_sigaction */
#define SA_ONSTACK      0x08000000  /* 使用备用栈 */
#define SA_RESTART      0x10000000  /* 重启被中断的系统调用 */
#define SA_NODEFER      0x40000000  /* 执行时不清除信号 */
#define SA_RESETHAND    0x80000000  /* 执行后重置为默认 */

/* 信号掩码标志 */
#define SIG_BLOCK       0   /* 阻塞信号 */
#define SIG_UNBLOCK     1   /* 解除阻塞 */
#define SIG_SETMASK     2   /* 设置掩码 */

/* siginfo_t 信号代码 */
#define SI_USER         0   /* 用户发送 */
#define SI_KERNEL       1   /* 内核发送 */
#define SI_QUEUE        2   /* sigqueue 发送 */
#define SI_TIMER        3   /* 定时器到期 */
#define SI_MESGQ        4   /* 消息队列状态改变 */
#define SI_ASYNCIO      5   /* 异步 I/O 完成 */
#define SI_SIGIO        6   /* SIGIO 信号 */
#define SI_TKILL        7   /* tkill 发送 */

/* SIGSEGV 代码 */
#define SEGV_MAPERR     1   /* 地址未映射 */
#define SEGV_ACCERR     2   /* 访问权限错误 */

/* SIGBUS 代码 */
#define BUS_ADRALN      1   /* 地址对齐错误 */
#define BUS_ADRERR      2   /* 地址不存在 */

/* SIGFPE 代码 */
#define FPE_INTDIV      1   /* 整数除零 */
#define FPE_INTOVF      2   /* 整数溢出 */
#define FPE_FLTDIV      3   /* 浮点除零 */
#define FPE_FLTOVF      4   /* 浮点上溢 */
#define FPE_FLTUND      5   /* 浮点下溢 */
#define FPE_FLTRES      6   /* 浮点不精确 */
#define FPE_FLTINV      7   /* 浮点无效操作 */

/* SIGILL 代码 */
#define ILL_ILLOPC      1   /* 非法操作码 */
#define ILL_ILLOPN      2   /* 非法操作数 */
#define ILL_ILLADR      3   /* 非法地址模式 */
#define ILL_ILLTRP      4   /* 非法陷阱 */
#define ILL_PRVOPC      5   /* 特权操作码 */
#define ILL_PRVREG      6   /* 特权寄存器 */
#define ILL_COPROC      7   /* 协处理器错误 */
#define ILL_BADSTK      8   /* 栈错误 */

/* ========== 数据结构 ========== */

/* 信号集合 */
typedef struct {
    uint64_t sig[2];    /* 64 位信号掩码 */
} sigset_t;

/* 信号联合值 */
union sigval {
    int sival_int;
    void *sival_ptr;
};

/* 进程 ID 类型 */
typedef int32_t pid_t;

/* 信号信息结构 */
typedef struct {
    int si_signo;       /* 信号编号 */
    int si_errno;       /* 错误号 */
    int si_code;        /* 信号来源 */
    
    union {
        int _pad[28];   /* 填充 */
        
        /* kill() 发送 */
        struct {
            uint32_t _pid;
            uint32_t _uid;
        } _kill;
        
        /* POSIX.1b 信号 */
        struct {
            uint32_t _pid;
            uint32_t _uid;
            union sigval _sigval;
        } _rt;
        
        /* SIGCHLD */
        struct {
            uint32_t _pid;
            uint32_t _uid;
            int _status;
            int _utime;
            int _stime;
        } _sigchld;
        
        /* SIGILL, SIGFPE, SIGSEGV, SIGBUS */
        struct {
            void *_addr;
            int _trapno;
        } _sigfault;
        
        /* SIGPOLL */
        struct {
            int _band;
            int _fd;
        } _sigpoll;
    } _sifields;
} siginfo_t;

/* 栈信息 */
typedef struct {
    void *ss_sp;        /* 栈指针 */
    size_t ss_size;     /* 栈大小 */
    int ss_flags;       /* 标志 */
} stack_t;

#define SS_ONSTACK      1       /* 正在使用信号栈 */
#define SS_DISABLE      2       /* 禁用信号栈 */

/* 信号动作结构 */
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    } _u;
    sigset_t sa_mask;       /* 阻塞的信号 */
    int sa_flags;           /* 标志 */
    void (*sa_restorer)(void);  /* 恢复函数 */
};

#define sa_handler      _u.sa_handler
#define sa_sigaction    _u.sa_sigaction

/* 信号帧 (内核保存) */
struct signal_frame {
    uint64_t sig;           /* 信号编号 */
    siginfo_t info;         /* 信号信息 */
    struct sigaction *action; /* 信号动作 */
    
    /* 用户上下文 */
    struct cpu_context context;
    
    /* 备用栈 */
    int onstack;
};

/* ========== 进程信号状态 ========== */

/* 进程信号状态 (嵌入 struct process) */
struct signal_state {
    spinlock_t lock;                /* 保护锁 */
    struct sigaction actions[NSIG]; /* 信号动作 */
    sigset_t blocked;               /* 阻塞的信号 */
    sigset_t pending;               /* 待处理的信号 */
    
    struct sigqueue *queue_head;    /* 信号队列头 */
    struct sigqueue *queue_tail;    /* 信号队列尾 */
    int queue_count;                /* 队列中信号数量 */
    
    stack_t altstack;               /* 备用信号栈 */
    
    int has_pending;                /* 有待处理信号 */
};

/* 信号队列限制 */
#define SIGNAL_QUEUE_MAX    1024    /* 每个进程最大排队信号数 */

/* 信号队列项 */
struct sigqueue {
    struct sigqueue *next;
    siginfo_t info;
};

/* ========== 信号 API ========== */

/* 初始化信号子系统 */
int signal_init(void);

/* 初始化进程信号状态 */
int signal_state_init(struct signal_state *state);

/* 销毁进程信号状态 */
void signal_state_destroy(struct signal_state *state);

/* 发送信号 */
int signal_send(pid_t pid, int sig);
int signal_send_info(pid_t pid, int sig, siginfo_t *info);

/* 发送信号给线程 */
int signal_send_thread(uint32_t tid, int sig);

/* 发送信号给进程组 */
int signal_send_group(int32_t pgid, int sig);

/* ========== 信号处理 ========== */

/* 处理待处理信号 */
int signal_handle_pending(struct signal_state *state, struct cpu_context *context);

/* 检查是否有待处理信号 */
int signal_has_pending(struct signal_state *state);

/* 构建信号帧 */
int signal_build_frame(struct signal_state *state, int sig,
                        struct cpu_context *context, struct signal_frame *frame);

/* 从信号帧返回 */
int signal_return(struct signal_frame *frame);

/* ========== 信号操作 ========== */

/* 设置信号动作 */
int signal_action(struct signal_state *state, int sig,
                   const struct sigaction *act, struct sigaction *oldact);

/* 信号掩码操作 */
int signal_procmask(struct signal_state *state, int how,
                     const sigset_t *set, sigset_t *oldset);

/* 挂起信号 */
int signal_suspend(struct signal_state *state, const sigset_t *mask);

/* 等待信号 */
int signal_wait(struct signal_state *state, const sigset_t *set, siginfo_t *info);

/* ========== sigset_t 操作 ========== */

static inline void sigemptyset(sigset_t *set) {
    set->sig[0] = 0;
    set->sig[1] = 0;
}

static inline void sigfillset(sigset_t *set) {
    set->sig[0] = ~0ULL;
    set->sig[1] = ~0ULL;
}

static inline void sigaddset(sigset_t *set, int sig) {
    if (sig > 0 && sig < NSIG) {
        set->sig[(sig - 1) / 64] |= (1ULL << ((sig - 1) % 64));
    }
}

static inline void sigdelset(sigset_t *set, int sig) {
    if (sig > 0 && sig < NSIG) {
        set->sig[(sig - 1) / 64] &= ~(1ULL << ((sig - 1) % 64));
    }
}

static inline int sigismember(const sigset_t *set, int sig) {
    if (sig > 0 && sig < NSIG) {
        return (set->sig[(sig - 1) / 64] >> ((sig - 1) % 64)) & 1;
    }
    return 0;
}

/* ========== 辅助函数 ========== */

/* 检查信号是否可捕获 */
int signal_is_catchable(int sig);

/* 获取默认信号动作 */
int signal_get_default_action(int sig);

/* 打印信号信息 */
void signal_print_info(siginfo_t *info);

/* 信号名称 */
const char *signal_name(int sig);

#endif /* SIGNAL_H */
