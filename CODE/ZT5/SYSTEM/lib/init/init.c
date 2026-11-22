/*
 * Zratro OS - Init 进程 (PID 1)
 *
 * 系统初始化入口，负责:
 *   1. 显示开机 LOGO (ZT5)
 *   2. 启动窗口管理器 (zrdemage)
 *   3. 启动桌面环境 (desk)
 *   4. 启动文件管理器 (fm)
 *   5. 监控子进程，自动重启崩溃的服务
 */

#include "types.h"
#include "syscall.h"
#include "unistd.h"

/* 服务路径定义 */
#define WM_PATH     "/prgm/sys/zrdemage"     /* 窗口管理器 */
#define DESK_PATH   "/prgm/sys/desk"       /* 桌面环境 */
#define FM_PATH     "/prgm/windowprogram/fm"
#define SHELL_PATH  "/prgm/shell"          /* Shell (user program) */

/* 服务 PID */
static pid_t wm_pid = -1;
static pid_t desk_pid = -1;
static pid_t fm_pid = -1;

/* 重启计数和退避 */
#define MAX_RESTART_COUNT 5
static int wm_restart_count = 0;
static int desk_restart_count = 0;
static int fm_restart_count = 0;

/* 简单字符串长度 */
static int my_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* 输出字符串到串口/控制台 */
static void log_msg(const char *msg) {
    sys_write(1, msg, my_strlen(msg));
}

/* 输出带换行的消息 */
static void log_line(const char *msg) {
    log_msg(msg);
    sys_write(1, "\n", 1);
}

/* 启动一个服务进程 */
static pid_t start_service(const char *path, const char *name) {
    log_msg("[INIT] Starting ");
    log_msg(name);
    log_msg("...\n");

    pid_t pid = sys_fork();
    if (pid < 0) {
        log_msg("[INIT] ERROR: fork failed for ");
        log_line(name);
        return -1;
    }

    if (pid == 0) {
        /* 子进程：执行服务程序 */
        int ret = sys_exec(path);
        /* exec 失败才会到这里 */
        log_msg("[INIT] ERROR: exec failed for ");
        log_msg(path);
        log_msg(" (ret=");
        char buf[16];
        int val = ret, i = 0;
        if (val < 0) { buf[i++] = '-'; val = -val; }
        if (val == 0) buf[i++] = '0';
        else {
            char tb[16]; int ti = 0;
            while (val > 0) { tb[ti++] = '0' + (val % 10); val /= 10; }
            while (ti > 0) buf[i++] = tb[--ti];
        }
        buf[i] = 0;
        log_msg(buf);
        log_line(")");
        sys_exit(127);
    }

    /* 父进程：返回子进程 PID */
    log_msg("[INIT] ");
    log_msg(name);
    log_msg(" started, PID=");
    {
        char buf[16];
        int val = (int)pid, i = 0;
        if (val == 0) buf[i++] = '0';
        else {
            char tb[16]; int ti = 0;
            while (val > 0) { tb[ti++] = '0' + (val % 10); val /= 10; }
            while (ti > 0) buf[i++] = tb[--ti];
        }
        buf[i] = 0;
        log_msg(buf);
    }
    log_line("");

    return pid;
}

/* 检查并重启崩溃的服务 */
static void check_and_restart(pid_t *pid_ptr, const char *path, const char *name) {
    if (*pid_ptr <= 0) return;

    /* 非阻塞 waitpid 检查子进程状态 */
    /* 使用 SYS_WAIT (23) - 如果子进程还在运行，返回 0 */
    int status = 0;
    /* 简单方法：尝试 wait，如果子进程已退出则重启 */
    /* 这里我们用一种轮询方式 - 在主循环中处理 */
}

/* 简单延时 */
static void delay(int ticks) {
    for (volatile int i = 0; i < ticks * 100000; i++);
}

int main(int argc, char *argv[]) {
    log_line("[INIT] ZratroOS v1.6.0 - PID 1");
    log_line("[INIT] Starting system services...");

    /* 阶段1: 启动窗口管理器 (最高优先级) */
    delay(10);
    wm_pid = start_service(WM_PATH, "Window Manager (zrdemage)");

    /* 阶段2: 等待 WM 初始化 */
    delay(50);

    /* 阶段3: 启动桌面环境 */
    desk_pid = start_service(DESK_PATH, "Desktop");

    /* 阶段4: 启动文件管理器 */
    delay(20);
    fm_pid = start_service(FM_PATH, "File Manager");

    log_line("[INIT] All services started");
    log_line("[INIT] Entering monitor loop...");

    /* 主循环：监控子进程 */
    while (1) {
        sys_yield();

        /* 检查子进程状态 */
        int status = 0;
        pid_t exited = sys_wait(&status);

        if (exited > 0) {
            /* 某个子进程退出了，尝试重启 */
            if (exited == wm_pid) {
                wm_restart_count++;
                if (wm_restart_count <= MAX_RESTART_COUNT) {
                    log_line("[INIT] Window Manager crashed! Restarting...");
                    /* 退避：每次重启后等待更长时间 */
                    sys_sleep(wm_restart_count * 50);
                    wm_pid = start_service(WM_PATH, "Window Manager (zrdemage)");
                } else {
                    log_line("[INIT] Window Manager crashed too many times, giving up");
                }
            } else if (exited == desk_pid) {
                desk_restart_count++;
                if (desk_restart_count <= MAX_RESTART_COUNT) {
                    log_line("[INIT] Desktop crashed! Restarting...");
                    sys_sleep(desk_restart_count * 50);
                    desk_pid = start_service(DESK_PATH, "Desktop");
                } else {
                    log_line("[INIT] Desktop crashed too many times, giving up");
                }
            } else if (exited == fm_pid) {
                fm_restart_count++;
                if (fm_restart_count <= MAX_RESTART_COUNT) {
                    log_line("[INIT] File Manager crashed! Restarting...");
                    sys_sleep(fm_restart_count * 50);
                    fm_pid = start_service(FM_PATH, "File Manager");
                } else {
                    log_line("[INIT] File Manager crashed too many times, giving up");
                }
            } else {
                log_msg("[INIT] Unknown child exited: PID=");
                {
                    char buf[16];
                    int val = (int)exited, i = 0;
                    if (val == 0) buf[i++] = '0';
                    else {
                        char tb[16]; int ti = 0;
                        while (val > 0) { tb[ti++] = '0' + (val % 10); val /= 10; }
                        while (ti > 0) buf[i++] = tb[--ti];
                    }
                    buf[i] = 0;
                    log_msg(buf);
                }
                log_line("");
            }
        }

        /* 短暂休眠避免忙等 */
        sys_sleep(10);
    }

    return 0;
}
