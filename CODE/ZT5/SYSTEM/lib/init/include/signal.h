#ifndef SIGNAL_H
#define SIGNAL_H

#include "types.h"

/* 信号编号 */
#define SIGHUP      1   /* 终端挂起 */
#define SIGINT      2   /* 中断 (Ctrl+C) */
#define SIGQUIT     3   /* 退出 (Ctrl+\) */
#define SIGILL      4   /* 非法指令 */
#define SIGTRAP     5   /* 跟踪陷阱 */
#define SIGABRT     6   /* 中止 */
#define SIGBUS      7   /* 总线错误 */
#define SIGFPE      8   /* 浮点异常 */
#define SIGKILL     9   /* 强制终止 */
#define SIGUSR1     10  /* 用户信号 1 */
#define SIGSEGV     11  /* 段错误 */
#define SIGUSR2     12  /* 用户信号 2 */
#define SIGPIPE     13  /* 管道断开 */
#define SIGALRM     14  /* 定时器 */
#define SIGTERM     15  /* 终止 */
#define SIGSTKFLT   16  /* 栈错误 */
#define SIGCHLD     17  /* 子进程状态改变 */
#define SIGCONT     18  /* 继续 */
#define SIGSTOP     19  /* 停止 */
#define SIGTSTP     20  /* 终端停止 (Ctrl+Z) */
#define SIGTTIN     21  /* 后台读终端 */
#define SIGTTOU     22  /* 后台写终端 */
#define SIGURG      23  /* 紧急 I/O */
#define SIGXCPU     24  /* CPU 时间超限 */
#define SIGXFSZ     25  /* 文件大小超限 */
#define SIGVTALRM   26  /* 虚拟定时器 */
#define SIGPROF     27  /*  profiling 定时器 */
#define SIGWINCH    28  /* 窗口大小改变 */
#define SIGIO       29  /* I/O 就绪 */
#define SIGPWR      30  /* 电源故障 */
#define SIGSYS      31  /* 错误系统调用 */

/* 信号默认行为 */
#define SIG_DFL     ((void (*)(int))0)
#define SIG_IGN     ((void (*)(int))1)
#define SIG_ERR     ((void (*)(int))-1)

/* 信号集 */
typedef struct {
    uint32_t sig[2];
} sigset_t;

/* 信号处理函数类型 */
typedef void (*sighandler_t)(int);

/* 信号函数 */
sighandler_t signal(int sig, sighandler_t handler);
int sigaction(int sig, const void *act, void *oact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);

/* 向进程发送信号 */
int kill(pid_t pid, int sig);
int raise(int sig);

#endif
