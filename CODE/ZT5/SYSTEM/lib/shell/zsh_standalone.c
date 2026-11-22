/*
 * zsh_standalone.c - Zratro Shell 独立版本
 * 
 * 使用标准 C 库编译，方便测试
 * 包含所有功能：
 * - 键盘输入捕获（原始模式）
 * - 命令行编辑和历史
 * - 内置命令
 * - 外部程序加载
 * 
 * 作者：ZratroOS 开发团队
 * 日期：2026-04-11
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>

/* 函数声明 - config.c */
void config_init(void);
int config_read(const char *path);
const char* config_get(const char *key);
int config_get_int(const char *key, int default_val);
int config_get_bool(const char *key, int default_val);
char** config_get_programs(int *count);
char** config_get_paths(int *count);
char** config_get_npaths(int *count);
int config_is_path_forbidden(const char *path);
void config_dump(void);

/* 函数声明 - parser */
int parse_command(const char *cmd, char **argv, int max_args);
int execute_builtin(char **argv, int argc);
int execute_external(char **argv, int argc);

/* ============================================
 * 常量定义
 * ============================================ */

#define MAX_PATH    256
#define MAX_CMD_LEN 1024
#define MAX_HISTORY 32
#define MAX_ARGS    64

/* ============================================
 * Shell 状态
 * ============================================ */

typedef struct {
    int running;
    char cwd[MAX_PATH];
    char prompt[64];
    char cmd_line[MAX_CMD_LEN];
    int cmd_pos;
    int cmd_len;
    char history[MAX_HISTORY][MAX_CMD_LEN];
    int history_count;
    int history_pos;
    int history_current;
    struct termios orig_termios;
    int raw_mode;
} shell_t;

shell_t g_shell;

/* ============================================
 * 终端控制
 * ============================================ */

int term_enable_raw_mode(void)
{
    if (g_shell.raw_mode) return 0;
    
    if (tcgetattr(STDIN_FILENO, &g_shell.orig_termios) == -1) {
        return -1;
    }
    
    struct termios raw = g_shell.orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(ICRNL | INLCR | ISTRIP);
    raw.c_oflag &= ~OPOST;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        return -1;
    }
    
    g_shell.raw_mode = 1;
    return 0;
}

int term_disable_raw_mode(void)
{
    if (!g_shell.raw_mode) return 0;
    
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_shell.orig_termios) == -1) {
        return -1;
    }
    
    g_shell.raw_mode = 0;
    return 0;
}

/* ============================================
 * 输出函数
 * ============================================ */

void print(const char *str)
{
    if (str) {
        printf("%s", str);
        fflush(stdout);
    }
}

void println(const char *str)
{
    print(str);
    printf("\n");
    fflush(stdout);
}

void print_char(char c)
{
    putchar(c);
    fflush(stdout);
}

void print_prompt(void)
{
    const char *cwd = g_shell.cwd;
    const char *home = "/root";
    
    printf("\033[1;32mroot@zratro\033[0m:");
    
    if (strncmp(cwd, home, strlen(home)) == 0) {
        printf("\033[1;34m~%s\033[0m", cwd + strlen(home));
    } else {
        printf("\033[1;34m%s\033[0m", cwd);
    }
    
    printf("\033[1;33m# \033[0m");
    fflush(stdout);
}

void refresh_line(void)
{
    printf("\r\033[K");
    print_prompt();
    
    if (g_shell.cmd_len > 0) {
        char temp = g_shell.cmd_line[g_shell.cmd_len];
        g_shell.cmd_line[g_shell.cmd_len] = '\0';
        printf("%s", g_shell.cmd_line);
        g_shell.cmd_line[g_shell.cmd_len] = temp;
    }
    
    int diff = g_shell.cmd_len - g_shell.cmd_pos;
    for (int i = 0; i < diff; i++) {
        printf("\033[D");
    }
    fflush(stdout);
}

/* ============================================
 * 键盘输入
 * ============================================ */

int read_key(void)
{
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) {
        return -1;
    }
    
    if (c == '\033') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\033';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\033';
        
        if (seq[0] == '[') {
            if (seq[1] >= 'A' && seq[1] <= 'D') {
                return 1000 + (seq[1] - 'A');  /* UP, DOWN, LEFT, RIGHT */
            }
        }
        return '\033';
    }
    
    return (unsigned char)c;
}

/* ============================================
 * 命令行编辑
 * ============================================ */

void cmd_line_init(void)
{
    g_shell.cmd_len = 0;
    g_shell.cmd_pos = 0;
    g_shell.cmd_line[0] = '\0';
}

void cmd_line_insert(char c)
{
    if (g_shell.cmd_len >= MAX_CMD_LEN - 1) return;
    
    if (g_shell.cmd_pos == g_shell.cmd_len) {
        g_shell.cmd_line[g_shell.cmd_pos] = c;
        g_shell.cmd_pos++;
        g_shell.cmd_len++;
        g_shell.cmd_line[g_shell.cmd_len] = '\0';
        print_char(c);
    } else {
        memmove(&g_shell.cmd_line[g_shell.cmd_pos + 1],
                &g_shell.cmd_line[g_shell.cmd_pos],
                g_shell.cmd_len - g_shell.cmd_pos + 1);
        g_shell.cmd_line[g_shell.cmd_pos] = c;
        g_shell.cmd_pos++;
        g_shell.cmd_len++;
        refresh_line();
    }
}

void cmd_line_backspace(void)
{
    if (g_shell.cmd_pos <= 0) return;
    
    g_shell.cmd_pos--;
    memmove(&g_shell.cmd_line[g_shell.cmd_pos],
            &g_shell.cmd_line[g_shell.cmd_pos + 1],
            g_shell.cmd_len - g_shell.cmd_pos);
    g_shell.cmd_len--;
    g_shell.cmd_line[g_shell.cmd_len] = '\0';
    refresh_line();
}

void cmd_line_move_left(void)
{
    if (g_shell.cmd_pos > 0) {
        g_shell.cmd_pos--;
        printf("\033[D");
        fflush(stdout);
    }
}

void cmd_line_move_right(void)
{
    if (g_shell.cmd_pos < g_shell.cmd_len) {
        g_shell.cmd_pos++;
        printf("\033[C");
        fflush(stdout);
    }
}

void cmd_line_clear(void)
{
    printf("\r\033[K");
    fflush(stdout);
    cmd_line_init();
}

/* ============================================
 * 历史记录
 * ============================================ */

void history_init(void)
{
    g_shell.history_count = 0;
    g_shell.history_pos = 0;
    g_shell.history_current = 0;
    
    for (int i = 0; i < MAX_HISTORY; i++) {
        g_shell.history[i][0] = '\0';
    }
}

void history_add(const char *cmd)
{
    if (!cmd || cmd[0] == '\0') return;
    
    if (g_shell.history_count > 0) {
        int last_idx = (g_shell.history_pos - 1 + MAX_HISTORY) % MAX_HISTORY;
        if (strcmp(g_shell.history[last_idx], cmd) == 0) return;
    }
    
    strncpy(g_shell.history[g_shell.history_pos], cmd, MAX_CMD_LEN - 1);
    g_shell.history_pos = (g_shell.history_pos + 1) % MAX_HISTORY;
    
    if (g_shell.history_count < MAX_HISTORY) {
        g_shell.history_count++;
    }
    
    g_shell.history_current = g_shell.history_pos;
}

const char* history_prev(void)
{
    if (g_shell.history_count == 0) return NULL;
    if (g_shell.history_current == 0) {
        if (g_shell.history_count < MAX_HISTORY) return NULL;
        g_shell.history_current = MAX_HISTORY;
    }
    
    g_shell.history_current--;
    return g_shell.history[g_shell.history_current];
}

const char* history_next(void)
{
    if (g_shell.history_count == 0) return NULL;
    
    g_shell.history_current++;
    if (g_shell.history_current >= g_shell.history_pos) {
        g_shell.history_current = g_shell.history_pos;
        return "";
    }
    
    return g_shell.history[g_shell.history_current];
}

/* ============================================
 * 内置命令
 * ============================================ */

int cmd_cd(char **argv, int argc)
{
    const char *path = (argc < 2) ? "/root" : argv[1];
    
    if (chdir(path) != 0) {
        fprintf(stderr, "cd: %s: No such file or directory\n", path);
        return 1;
    }
    
    getcwd(g_shell.cwd, MAX_PATH);
    return 0;
}

int cmd_echo(char **argv, int argc)
{
    for (int i = 1; i < argc; i++) {
        printf("%s", argv[i]);
        if (i < argc - 1) printf(" ");
    }
    printf("\n");
    return 0;
}

int cmd_pwd(char **argv, int argc)
{
    printf("%s\n", g_shell.cwd);
    return 0;
}

int cmd_ls(char **argv, int argc)
{
    const char *path = (argc < 2) ? "." : argv[1];
    
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "ls: %s: Cannot open directory\n", path);
        return 1;
    }
    
    struct dirent *entry;
    int first = 1;
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' && 
            (entry->d_name[1] == '\0' || 
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }
        
        if (!first) printf("  ");
        printf("%s", entry->d_name);
        
        if (entry->d_type == DT_DIR) {
            printf("/");
        }
        first = 0;
    }
    
    if (!first) printf("\n");
    closedir(dir);
    return 0;
}

int cmd_clear(char **argv, int argc)
{
    printf("\033[2J\033[H");
    fflush(stdout);
    return 0;
}

int cmd_help(char **argv, int argc)
{
    printf("\n");
    printf("Zratro Shell (zsh) - Version 1.0\n");
    printf("\n");
    printf("Available commands:\n");
    printf("  cd [dir]      - Change directory\n");
    printf("  echo <text>   - Print text\n");
    printf("  pwd           - Print working directory\n");
    printf("  ls [dir]      - List directory contents\n");
    printf("  clear         - Clear screen\n");
    printf("  config [file] - Show/load configuration\n");
    printf("  run           - Run programs from config\n");
    printf("  cat <file>    - Show file contents\n");
    printf("  help          - Show this help\n");
    printf("  exit          - Exit shell\n");
    printf("\n");
    printf("Keyboard shortcuts:\n");
    printf("  Up/Down       - Command history\n");
    printf("  Left/Right    - Move cursor\n");
    printf("  Backspace     - Delete character\n");
    printf("  Ctrl+C        - Clear line\n");
    printf("  Ctrl+L        - Clear screen\n");
    printf("\n");
    return 0;
}

/* ============================================
 * config - 显示/加载配置
 * ============================================ */

int cmd_config(char **argv, int argc)
{
    if (argc < 2) {
        /* 显示当前配置 */
        config_dump();
    } else {
        /* 加载指定配置文件 */
        if (config_read(argv[1]) == 0) {
            printf("[SHELL] Configuration loaded: %s\n", argv[1]);
            config_dump();
        } else {
            printf("[SHELL] Failed to load: %s\n", argv[1]);
            return 1;
        }
    }
    return 0;
}

/* ============================================
 * run - 运行配置文件中的程序
 * ============================================ */

int cmd_run(char **argv, int argc)
{
    int prog_count;
    char **progs = config_get_programs(&prog_count);
    
    if (prog_count == 0) {
        printf("[SHELL] No programs in configuration\n");
        return 0;
    }
    
    printf("[SHELL] Running %d program(s) from configuration:\n", prog_count);
    
    for (int i = 0; i < prog_count; i++) {
        printf("  [%d/%d] %s\n", i + 1, prog_count, progs[i]);
        
        /* 解析程序路径和参数 */
        char *cmd_argv[MAX_ARGS];
        int cmd_argc = parse_command(progs[i], cmd_argv, MAX_ARGS);
        
        if (cmd_argc > 0) {
            execute_external(cmd_argv, cmd_argc);
        }
    }
    
    return 0;
}

/* ============================================
 * cat - 显示文件内容
 * ============================================ */

int cmd_cat(char **argv, int argc)
{
    if (argc < 2) {
        printf("cat: Usage: cat <file>\n");
        return 1;
    }
    
    for (int i = 1; i < argc; i++) {
        FILE *fp = fopen(argv[i], "r");
        if (!fp) {
            fprintf(stderr, "cat: %s: No such file or directory\n", argv[i]);
            continue;
        }
        
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
            fwrite(buf, 1, n, stdout);
        }
        
        fclose(fp);
    }
    
    return 0;
}

int cmd_exit(char **argv, int argc)
{
    g_shell.running = 0;
    return 0;
}

/* ============================================
 * 命令执行
 * ============================================ */

int execute_builtin(char **argv, int argc)
{
    if (!argv || !argv[0]) return 1;
    
    if (strcmp(argv[0], "cd") == 0) return cmd_cd(argv, argc);
    if (strcmp(argv[0], "echo") == 0) return cmd_echo(argv, argc);
    if (strcmp(argv[0], "pwd") == 0) return cmd_pwd(argv, argc);
    if (strcmp(argv[0], "ls") == 0) return cmd_ls(argv, argc);
    if (strcmp(argv[0], "clear") == 0) return cmd_clear(argv, argc);
    if (strcmp(argv[0], "config") == 0) return cmd_config(argv, argc);
    if (strcmp(argv[0], "run") == 0) return cmd_run(argv, argc);
    if (strcmp(argv[0], "cat") == 0) return cmd_cat(argv, argc);
    if (strcmp(argv[0], "help") == 0) return cmd_help(argv, argc);
    if (strcmp(argv[0], "exit") == 0) return cmd_exit(argv, argc);
    
    return -1;
}

int execute_external(char **argv, int argc)
{
    if (!argv || !argv[0]) return 1;
    
    /* 检查路径是否被禁止 */
    if (config_is_path_forbidden(argv[0])) {
        fprintf(stderr, "%s: Access denied (forbidden by configuration)\n", argv[0]);
        return 1;
    }
    
    pid_t pid = fork();
    
    if (pid < 0) {
        fprintf(stderr, "fork failed\n");
        return 1;
    }
    
    if (pid == 0) {
        /* 子进程 */
        char *envp[] = {
            "PATH=/bin:/usr/bin:/sbin:/usr/sbin:/prgm:/prgm/sys",
            "HOME=/root",
            "TERM=linux",
            NULL
        };
        
        /* 直接执行，由 Ztknl 内核自动检测 ELF/PE 格式并加载 */
        execve(argv[0], argv, envp);
        
        /* execve 失败时的错误处理 */
        fprintf(stderr, "%s: Cannot execute\n", argv[0]);
        exit(127);
    }
    
    /* 父进程 - 等待子进程 */
    int status;
    waitpid(pid, &status, 0);
    
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    
    return 1;
}

int parse_command(const char *cmd, char **argv, int max_args)
{
    int argc = 0;
    const char *p = cmd;
    
    while (*p == ' ' || *p == '\t') p++;
    
    while (*p && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        
        argv[argc] = (char*)p;
        
        while (*p && *p != ' ' && *p != '\t') p++;
        
        if (*p) {
            *(char*)p = '\0';
            p++;
        }
        
        argc++;
    }
    
    argv[argc] = NULL;
    return argc;
}

int execute_command(const char *cmd)
{
    if (!cmd || cmd[0] == '\0') return 0;
    
    char *argv[MAX_ARGS];
    int argc = parse_command(cmd, argv, MAX_ARGS);
    
    if (argc == 0) return 0;
    
    int ret = execute_builtin(argv, argc);
    if (ret >= 0) return ret;
    
    return execute_external(argv, argc);
}

/* ============================================
 * Shell 主循环
 * ============================================ */

void shell_init(void)
{
    g_shell.running = 1;
    g_shell.raw_mode = 0;
    
    getcwd(g_shell.cwd, MAX_PATH);
    if (g_shell.cwd[0] == '\0') {
        strcpy(g_shell.cwd, "/");
    }
    
    cmd_line_init();
    history_init();
    
    if (term_enable_raw_mode() == 0) {
        printf("\n");
        printf("Zratro Shell (zsh) - Version 1.0\n");
        printf("Type 'help' for available commands.\n");
        printf("\n");
    }
}

void shell_run(void)
{
    while (g_shell.running) {
        print_prompt();
        cmd_line_init();
        
        while (g_shell.running) {
            int key = read_key();
            if (key < 0) continue;
            
            switch (key) {
                case 13:  /* Enter */
                    if (g_shell.cmd_len > 0) {
                        history_add(g_shell.cmd_line);
                        printf("\n");
                        g_shell.cmd_line[g_shell.cmd_len] = '\0';
                        execute_command(g_shell.cmd_line);
                    } else {
                        printf("\n");
                    }
                    goto next_command;
                
                case 127:  /* Backspace */
                    cmd_line_backspace();
                    break;
                
                case 1002:  /* Left */
                    cmd_line_move_left();
                    break;
                
                case 1003:  /* Right */
                    cmd_line_move_right();
                    break;
                
                case 1000:  /* Up */
                    {
                        const char *hist = history_prev();
                        if (hist) {
                            cmd_line_clear();
                            if (hist[0]) {
                                for (int i = 0; hist[i]; i++) {
                                    cmd_line_insert(hist[i]);
                                }
                            }
                        }
                    }
                    break;
                
                case 1001:  /* Down */
                    {
                        const char *hist = history_next();
                        if (hist) {
                            cmd_line_clear();
                            if (hist[0]) {
                                for (int i = 0; hist[i]; i++) {
                                    cmd_line_insert(hist[i]);
                                }
                            }
                        }
                    }
                    break;
                
                case 3:  /* Ctrl+C */
                    printf("^C\n");
                    cmd_line_clear();
                    goto next_command;
                
                case 4:  /* Ctrl+D */
                    if (g_shell.cmd_len == 0) {
                        printf("exit\n");
                        g_shell.running = 0;
                        return;
                    }
                    break;
                
                case 12:  /* Ctrl+L */
                    printf("\033[2J\033[H");
                    fflush(stdout);
                    break;
                
                case 27:  /* ESC */
                    break;
                
                default:
                    if (key >= 32 && key <= 126) {
                        cmd_line_insert((char)key);
                    }
                    break;
            }
        }
        
        next_command:
        ;
    }
}

void shell_cleanup(void)
{
    term_disable_raw_mode();
}

/* ============================================
 * 主程序
 * ============================================ */

int main(int argc, char **argv)
{
    /* 初始化 shell */
    shell_init();
    
    /* 读取配置文件 */
    config_init();
    
    /* 尝试从多个路径读取配置（优先读取 zrr64 下的配置） */
    const char *config_paths[] = {
        "/zrr64/zrr86/hook.cfg",      /* zrr64 主配置（环境变量、路径） */
        "/zrr64/zrr86/shn.cfg",       /* zrr64 安全配置（禁止运行的程序） */
        "/system/zratro.conf",        /* 系统配置（向后兼容） */
        "/etc/zratro.conf",
        "/zratro.conf",
        NULL
    };
    
    int config_loaded = 0;
    for (int i = 0; config_paths[i]; i++) {
        if (config_read(config_paths[i]) == 0) {
            printf("[SHELL] Configuration loaded: %s\n", config_paths[i]);
            config_dump();
            config_loaded = 1;
            break;
        }
    }
    
    if (!config_loaded) {
        printf("[SHELL] No configuration file found, using defaults\n");
    }
    
    /* 如果有命令行参数，执行它 */
    if (argc > 1) {
        /* 非交互模式：执行命令后退出 */
        char cmd[MAX_CMD_LEN] = "";
        for (int i = 1; i < argc; i++) {
            strcat(cmd, argv[i]);
            if (i < argc - 1) strcat(cmd, " ");
        }
        
        printf("[SHELL] Executing: %s\n", cmd);
        execute_command(cmd);
    } else {
        /* 交互模式：运行 shell */
        shell_run();
    }
    
    /* 清理 */
    shell_cleanup();
    
    return 0;
}
