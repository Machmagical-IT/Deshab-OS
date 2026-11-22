#ifndef WAIT_H
#define WAIT_H

#include "types.h"
#include "syscall.h"

/* waitpid 选项 */
#define WNOHANG     1   /* 非阻塞 */
#define WUNTRACED   2   /* 报告停止的进程 */
#define WCONTINUED  4   /* 报告继续的进程 */

/* wait 状态宏 */
#define WIFEXITED(status)    (((status) & 0x7F) == 0)
#define WEXITSTATUS(status)  (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status)  (((status) & 0x7F) != 0 && ((status) & 0x7F) != 0x7F)
#define WTERMSIG(status)     ((status) & 0x7F)
#define WIFSTOPPED(status)   (((status) & 0xFF) == 0x7F)
#define WSTOPSIG(status)     (((status) >> 8) & 0xFF)

/* wait 函数实现 */
static inline pid_t wait(int *status) {
    return sys_wait(status);
}

static inline pid_t waitpid(pid_t pid, int *status, int options) {
    /* 简化实现：忽略 pid 和 options */
    (void)pid;
    (void)options;
    return sys_wait(status);
}

#endif