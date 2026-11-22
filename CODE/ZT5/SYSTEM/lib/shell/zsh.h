/*
 * zsh.h - Zratro Shell 主头文件
 *
 * 一个完整的用户态 shell 程序，支持：
 * - 键盘输入捕获（原始模式）
 * - 命令行编辑（左右箭头、Home/End、Insert、退格、删除）
 * - 命令历史（上下箭头、Ctrl+R 增量搜索）
 * - 内置命令（cd, echo, exit, help, clear, ls, pwd, cat, mkdir, rm, cp, mv, whoami, uname, touch, chmod）
 * - 外部程序加载（fork+execve）
 * - 管道和重定向
 * - Tab 补全
 * - 通配符展开
 *
 * 作者：ZratroOS 开发团队
 * 日期：2026-04-11
 */

#ifndef ZSH_H
#define ZSH_H

#include <stdint.h>
#include <stddef.h>

/* 基本类型定义 */
typedef long off_t;

/* ============================================
 * 系统调用号定义 (ZratroOS 原生 ABI)
 * 注意：必须与内核 include/syscall.h 保持一致
 * ============================================ */
#define SYS_exit        0
#define SYS_read        1
#define SYS_write       2
#define SYS_open        3
#define SYS_close       4
#define SYS_seek        5
#define SYS_stat        6
#define SYS_exec        7
#define SYS_yield       8
#define SYS_sleep       9
#define SYS_getpid      10
#define SYS_getppid     11
#define SYS_uname       12
#define SYS_sysinfo     13
#define SYS_gettimeofday 14
#define SYS_time        15
#define SYS_kill        16
#define SYS_signal      17
#define SYS_getsockname 18
#define SYS_mmap        19
#define SYS_munmap      20
#define SYS_brk         21
#define SYS_getcwd      22
#define SYS_chdir       23
#define SYS_ioctl       24
#define SYS_mkdir       25
#define SYS_rmdir       26
#define SYS_unlink      27
#define SYS_opendir     28
#define SYS_readdir     29
#define SYS_closedir    30
#define SYS_fork        31
#define SYS_wait        32
#define SYS_pipe        33
#define SYS_dup         34
#define SYS_dup2        35
#define SYS_execve      36
#define SYS_execveat    37

/* 权限系统调用 */
#define SYS_getuid      190
#define SYS_getgid      191
#define SYS_setuid      192
#define SYS_setgid      193
#define SYS_chmod       194
#define SYS_chown       195
#define SYS_sudo        196

/* 输入子系统 */
#define SYS_input_poll  188

/* ============================================
 * 文件操作标志
 * ============================================ */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x40
#define O_TRUNC     0x200
#define O_APPEND    0x400

/* ============================================
 * 终端控制
 * ============================================ */
#define STDIN_FD    0
#define STDOUT_FD   1
#define STDERR_FD   2

/* TCGETS 和 TCSETS ioctl 命令 */
#define TCGETS  0x5401
#define TCSETS  0x5402

/* 按键定义 */
#define KEY_ESC         27
#define KEY_ENTER       13
#define KEY_BACKSPACE   127
#define KEY_DELETE      1004
#define KEY_HOME        1005
#define KEY_END         1006
#define KEY_INSERT      1007
#define KEY_PAGEUP      1008
#define KEY_PAGEDOWN    1009
#define KEY_TAB         9
#define KEY_CTRL_C      3
#define KEY_CTRL_D      4
#define KEY_CTRL_L      12
#define KEY_CTRL_R      18
#define KEY_CTRL_A      1
#define KEY_CTRL_E      5
#define KEY_CTRL_U      21
#define KEY_CTRL_K      11
#define KEY_CTRL_W      23

#define KEY_UP      1000
#define KEY_DOWN    1001
#define KEY_LEFT    1002
#define KEY_RIGHT   1003

/* 终端属性 */
struct termios {
    uint32_t c_iflag;  /* 输入模式 */
    uint32_t c_oflag;  /* 输出模式 */
    uint32_t c_cflag;  /* 控制模式 */
    uint32_t c_lflag;  /* 本地模式 */
    uint8_t  c_cc[32]; /* 控制字符 */
};

#define ECHO        0x00000008
#define ICANON      0x00000002
#define ISIG        0x00000001
#define IEXTEN      0x00008000
#define OPOST       0x00000001

#define VMIN        6
#define VTIME       5

/* ============================================
 * Shell 日志
 * ============================================ */
#define LOG_FILE_PATH   "/zrr64/shlg/otp.out"

/* 进程控制参数 */
#define MAX_ARGS    64
#define MAX_PATH    256
#define MAX_CMD_LEN 1024
#define MAX_HISTORY 32
#define AT_FDCWD    -100

/* 重定向类型 */
#define REDIR_NONE      0
#define REDIR_OUT       1   /* > */
#define REDIR_OUT_APP   2   /* >> */
#define REDIR_IN        3   /* < */
#define REDIR_ERR_OUT   4   /* 2> */
#define REDIR_ERR_OUT_APP 5 /* 2>> */
#define REDIR_ALL_OUT   6   /* &> */
#define REDIR_ALL_OUT_APP 7 /* &>> */

/* 管道命令 */
#define MAX_PIPELINE    16

typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    int redir_type;
    char redir_file[MAX_PATH];
} pipeline_cmd_t;

typedef struct {
    pipeline_cmd_t cmds[MAX_PIPELINE];
    int count;
} pipeline_t;

/* ============================================
 * 错误码
 * ============================================ */
#define EPERM       1
#define ENOENT      2
#define EACCES      13
#define ENOMEM      12
#define ENOTTY      25

/* ============================================
 * stat 结构体
 * ============================================ */
struct zsh_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
};

#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFLNK  0120000

/* ============================================
 * uname 结构体
 * ============================================ */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

/* ============================================
 * 目录条目结构
 * ============================================ */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12

/* ============================================
 * shell 状态
 * ============================================ */
typedef struct {
    int running;              /* shell 是否运行 */
    char cwd[MAX_PATH];       /* 当前工作目录 */
    char prompt[64];          /* 提示符 */

    /* 命令行编辑 */
    char cmd_line[MAX_CMD_LEN];  /* 当前命令行 */
    int cmd_pos;                 /* 光标位置 */
    int cmd_len;                 /* 命令行长度 */
    int insert_mode;             /* 插入/覆盖模式 (0=插入, 1=覆盖) */

    /* 历史记录 */
    char history[MAX_HISTORY][MAX_CMD_LEN];
    int history_count;
    int history_pos;
    int history_current;
    int history_saved_current;   /* 保存导航前的位置 */

    /* Ctrl+R 搜索 */
    int search_mode;             /* 是否在搜索模式 */
    char search_query[MAX_CMD_LEN]; /* 搜索查询 */
    int search_result_idx;       /* 搜索结果索引 */

    /* 终端设置 */
    struct termios orig_termios;  /* 原始终端设置 */
    int raw_mode;                 /* 是否原始模式 */

    /* 日志文件 */
    int log_fd;                   /* 日志文件描述符 */

    /* 用户信息 */
    int is_root;                  /* 是否 root 用户 */
    char username[32];            /* 当前用户名 */
} shell_t;

/* ============================================
 * 全局变量
 * ============================================ */
extern shell_t g_shell;

/* ============================================
 * 系统调用包装函数
 * ============================================ */

/* 基础 I/O */
long sys_read(int fd, void *buf, size_t count);
long sys_write(int fd, const void *buf, size_t count);
long sys_open(const char *pathname, int flags, int mode);
long sys_close(int fd);
long sys_seek(int fd, off_t offset, int whence);
long sys_stat(const char *pathname, struct zsh_stat *statbuf);

/* 目录和文件 */
long sys_chdir(const char *path);
long sys_getcwd(char *buf, size_t size);
long sys_mkdir(const char *pathname, int mode);
long sys_rmdir(const char *pathname);
long sys_unlink(const char *pathname);
long sys_chmod(const char *pathname, int mode);
long sys_opendir(const char *name);
long sys_readdir(int fd, void *dirp, unsigned int count);
long sys_closedir(int fd);

/* 进程控制 */
long sys_execve(const char *pathname, char *const argv[], char *const envp[]);
long sys_exit(int status);
long sys_getpid(void);
long sys_getppid(void);
long sys_getuid(void);
long sys_getgid(void);
long sys_uname(struct utsname *buf);

/* 内存管理 */
long sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
long sys_munmap(void *addr, size_t length);

/* 进程控制 (fork, wait) */
long sys_fork(void);
long sys_wait(int pid, int *wstatus, int options, void *rusage);

/* 管道和文件描述符复制 */
long sys_pipe(int pipefd[2]);
long sys_dup(int oldfd);
long sys_dup2(int oldfd, int newfd);

/* 终端控制 */
long sys_ioctl(int fd, unsigned long request, void *argp);

/* 权限 */
long sys_sudo(const char *hash);

/* ============================================
 * 终端 I/O 函数
 * ============================================ */

/* 终端模式控制 */
int term_get_attr(int fd, struct termios *term);
int term_set_attr(int fd, const struct termios *term);
int term_enable_raw_mode(void);
int term_disable_raw_mode(void);

/* 屏幕输出 */
void print(const char *str);
void println(const char *str);
void print_char(char c);
void print_prompt(void);
void refresh_line(void);

/* 键盘输入 */
int read_key(void);

/* ============================================
 * 命令行编辑函数
 * ============================================ */

/* 编辑操作 */
void cmd_line_init(void);
void cmd_line_insert(char c);
void cmd_line_overwrite(char c);
void cmd_line_delete(void);
void cmd_line_backspace(void);
void cmd_line_move_left(void);
void cmd_line_move_right(void);
void cmd_line_move_home(void);
void cmd_line_move_end(void);
void cmd_line_clear(void);
void cmd_line_kill_to_end(void);
void cmd_line_kill_to_start(void);
void cmd_line_kill_word_back(void);

/* Tab 补全 */
int cmd_line_tab_complete(void);

/* ============================================
 * 命令历史
 * ============================================ */
void history_init(void);
void history_add(const char *cmd);
const char* history_prev(void);
const char* history_next(void);
void history_reset_nav(void);
const char* history_search(const char *prefix, int direction);

/* Ctrl+R 增量搜索 */
void search_start(void);
void search_stop(void);
int search_handle_key(int key);
const char* search_get_prompt(void);

/* ============================================
 * 命令解析和执行
 * ============================================ */

/* 命令解析 */
int parse_command(const char *cmd, char **argv, int max_args);
void free_argv(char **argv, int count);

/* 通配符展开 */
int glob_expand(const char *pattern, char **results, int max_results);
int is_glob_pattern(const char *str);

/* 管道和重定向解析 */
int parse_pipeline(const char *cmd, pipeline_t *pipeline);
int execute_pipeline(const pipeline_t *pipeline);

/* 内置命令 */
int cmd_cd(char **argv, int argc);
int cmd_echo(char **argv, int argc);
int cmd_pwd(char **argv, int argc);
int cmd_ls(char **argv, int argc);
int cmd_cat(char **argv, int argc);
int cmd_mkdir(char **argv, int argc);
int cmd_rm(char **argv, int argc);
int cmd_cp(char **argv, int argc);
int cmd_mv(char **argv, int argc);
int cmd_clear(char **argv, int argc);
int cmd_help(char **argv, int argc);
int cmd_exit(char **argv, int argc);
int cmd_sudo(char **argv, int argc);
int cmd_whoami(char **argv, int argc);
int cmd_uname(char **argv, int argc);
int cmd_touch(char **argv, int argc);
int cmd_chmod(char **argv, int argc);

/* 内置命令表 (定义在 commands.c) */
typedef struct {
    const char *name;
    int (*func)(char **argv, int argc);
} builtin_cmd_t;

extern builtin_cmd_t builtins[];

/* 命令分发 */
int execute_command(const char *cmd);
int execute_builtin(char **argv, int argc);
int execute_external(char **argv, int argc);

/* ============================================
 * 工具函数
 * ============================================ */

/* 字符串操作 */
size_t str_len(const char *s);
int str_cmp(const char *s1, const char *s2);
int str_ncmp(const char *s1, const char *s2, size_t n);
char* str_cpy(char *dest, const char *src);
char* str_ncpy(char *dest, const char *src, size_t n);
char* str_cat(char *dest, const char *src);
char* str_chr(const char *s, int c);
int str_cmp_case(const char *s1, const char *s2);

/* 内存操作 */
void* mem_set(void *s, int c, size_t n);
void* mem_copy(void *dest, const void *src, size_t n);
int mem_cmp(const void *s1, const void *s2, size_t n);

/* 工具函数 */
void int_to_str(int num, char *buf, int base);
const char* get_username(void);
const char* get_hostname(void);

/* 路径工具 */
int path_is_dir(const char *path);
int path_join(char *dest, const char *dir, const char *name, int dest_size);
const char* path_basename(const char *path);
int path_dirname(char *dest, const char *path, int dest_size);

/* ============================================
 * 程序入口
 * ============================================ */
int main(void);
void shell_init(void);
void shell_run(void);
void shell_cleanup(void);

#endif /* ZSH_H */
