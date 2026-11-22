/*
 * parser.c - 命令解析器
 *
 * 解析命令行字符串为管道命令链
 * 支持：
 * - 空格分隔参数
 * - 引号字符串（"..." 和 '...'）
 * - 转义字符 (\)
 * - 管道操作符 (|)
 * - 输出重定向 (>, >>)
 * - 输入重定向 (<)
 * - 错误重定向 (2>, 2>>)
 * - 全部重定向 (&>, &>>)
 * - 通配符展开 (*, ?)
 *
 * 作者：ZratroOS 开发团队
 * 日期：2026-04-12
 */

#include "zsh.h"

/* ============================================
 * 简单字符串工具（内部使用）
 * ============================================ */

static char *p_strcpy(char *d, const char *s)
{
    char *p = d;
    while ((*p++ = *s++));
    return d;
}

static void p_memset(void *s, int c, int n)
{
    char *p = (char*)s;
    for (int i = 0; i < n; i++) p[i] = (char)c;
}

/* ============================================
 * 通配符匹配
 *
 * 检查字符串是否包含通配符
 * ============================================ */

int is_glob_pattern(const char *str)
{
    while (*str) {
        if (*str == '*' || *str == '?') return 1;
        if (*str == '\\') str++;  /* 跳过转义字符 */
        str++;
    }
    return 0;
}

/* 简单通配符匹配: * 匹配任意字符序列, ? 匹配单个字符 */
static int glob_match(const char *pattern, const char *str)
{
    while (*pattern && *str) {
        if (*pattern == '*') {
            pattern++;
            /* * 匹配 0 个或多个字符 */
            if (*pattern == '\0') return 1;
            while (*str) {
                if (glob_match(pattern, str)) return 1;
                str++;
            }
            return glob_match(pattern, str);
        } else if (*pattern == '?') {
            pattern++;
            str++;
        } else if (*pattern == '\\') {
            pattern++;
            if (*pattern != *str) return 0;
            pattern++;
            str++;
        } else {
            if (*pattern != *str) return 0;
            pattern++;
            str++;
        }
    }

    /* 处理末尾的 * */
    while (*pattern == '*') pattern++;

    return (*pattern == '\0' && *str == '\0');
}

/* 通配符展开: 在指定目录中查找匹配的文件名 */
int glob_expand(const char *pattern, char **results, int max_results)
{
    if (!is_glob_pattern(pattern)) {
        /* 不是通配符模式，直接返回 */
        if (max_results > 0) {
            results[0] = (char*)pattern;
            return 1;
        }
        return 0;
    }

    /* 确定搜索目录 */
    char dir[MAX_PATH];
    char file_pattern[MAX_PATH];
    int last_slash = -1;
    int plen = str_len(pattern);

    for (int i = plen - 1; i >= 0; i--) {
        if (pattern[i] == '/') {
            last_slash = i;
            break;
        }
    }

    if (last_slash >= 0) {
        str_ncpy(dir, pattern, last_slash + 1);
        dir[last_slash + 1] = '\0';
        str_cpy(file_pattern, &pattern[last_slash + 1]);
    } else {
        str_cpy(dir, ".");
        str_cpy(file_pattern, pattern);
    }

    /* 如果目录路径中包含通配符，暂不处理 (简化版) */
    if (is_glob_pattern(dir)) {
        if (max_results > 0) {
            results[0] = (char*)pattern;
            return 1;
        }
        return 0;
    }

    /* 打开目录 */
    long fd = sys_open(dir, O_RDONLY, 0);
    if (fd < 0) {
        /* 目录不存在，返回原始模式 */
        if (max_results > 0) {
            results[0] = (char*)pattern;
            return 1;
        }
        return 0;
    }

    char buf[4096];
    int count = 0;

    while (count < max_results) {
        long nread = sys_readdir(fd, buf, 4096);
        if (nread <= 0) break;

        long offset = 0;
        while (offset < nread && count < max_results) {
            struct linux_dirent64 *dirent = (struct linux_dirent64*)(buf + offset);

            /* 跳过 . 和 .. 如果模式不以 . 开头 */
            if (dirent->d_name[0] == '.' && file_pattern[0] != '.') {
                offset += dirent->d_reclen;
                continue;
            }

            /* 检查通配符匹配 */
            if (glob_match(file_pattern, dirent->d_name)) {
                /* 构建完整路径 */
                static char glob_results[64][MAX_PATH];
                if (count < 64) {
                    if (last_slash >= 0) {
                        str_cpy(glob_results[count], dir);
                        str_cat(glob_results[count], dirent->d_name);
                    } else {
                        str_cpy(glob_results[count], dirent->d_name);
                    }
                    /* 如果是目录，加 / */
                    if (dirent->d_type == DT_DIR) {
                        int len = str_len(glob_results[count]);
                        if (len < MAX_PATH - 1) {
                            glob_results[count][len] = '/';
                            glob_results[count][len + 1] = '\0';
                        }
                    }
                    results[count] = glob_results[count];
                    count++;
                }
            }

            offset += dirent->d_reclen;
        }
    }

    sys_close(fd);

    /* 如果没有匹配，返回原始模式 */
    if (count == 0 && max_results > 0) {
        results[0] = (char*)pattern;
        return 1;
    }

    return count;
}

/* ============================================
 * 解析单个命令（含重定向和通配符展开）
 * ============================================ */

static int parse_single_cmd(const char *cmd, pipeline_cmd_t *pcmd)
{
    p_memset(pcmd, 0, sizeof(*pcmd));
    pcmd->redir_type = REDIR_NONE;

    const char *p = cmd;
    int argc = 0;

    /* 重置参数缓冲区 */
    static char arg_buf[MAX_CMD_LEN];
    static int arg_buf_pos = 0;
    arg_buf_pos = 0;

    /* 跳过前导空格 */
    while (*p == ' ' || *p == '\t') p++;

    while (*p && argc < MAX_ARGS - 1) {
        /* 跳过空格 */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        /* 检查重定向操作符 */
        /* &> 或 &>> (全部重定向) */
        if (*p == '&' && (*(p+1) == '>' || (*(p+1) == '>' && *(p+2) == '>'))) {
            if (*(p+1) == '>' && *(p+2) == '>') {
                pcmd->redir_type = REDIR_ALL_OUT_APP;
                p += 3;
            } else {
                pcmd->redir_type = REDIR_ALL_OUT;
                p += 2;
            }
            while (*p == ' ' || *p == '\t') p++;
            int i = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '|' && i < MAX_PATH - 1) {
                pcmd->redir_file[i++] = *p++;
            }
            pcmd->redir_file[i] = '\0';
            break;
        }

        /* 2> 或 2>> (错误重定向) */
        if (*p == '2' && (*(p+1) == '>' || (*(p+1) == '>' && *(p+2) == '>'))) {
            if (*(p+1) == '>' && *(p+2) == '>') {
                pcmd->redir_type = REDIR_ERR_OUT_APP;
                p += 3;
            } else {
                pcmd->redir_type = REDIR_ERR_OUT;
                p += 2;
            }
            while (*p == ' ' || *p == '\t') p++;
            int i = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '|' && i < MAX_PATH - 1) {
                pcmd->redir_file[i++] = *p++;
            }
            pcmd->redir_file[i] = '\0';
            break;
        }

        /* > 或 >> (输出重定向) */
        if (*p == '>' && (p == cmd || *(p-1) == ' ' || *(p-1) == '\t')) {
            if (*(p+1) == '>') {
                pcmd->redir_type = REDIR_OUT_APP;
                p += 2;
            } else {
                pcmd->redir_type = REDIR_OUT;
                p += 1;
            }
            while (*p == ' ' || *p == '\t') p++;
            int i = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '|' && i < MAX_PATH - 1) {
                pcmd->redir_file[i++] = *p++;
            }
            pcmd->redir_file[i] = '\0';
            break;
        }

        /* < (输入重定向) */
        if (*p == '<' && (p == cmd || *(p-1) == ' ' || *(p-1) == '\t')) {
            pcmd->redir_type = REDIR_IN;
            p += 1;
            while (*p == ' ' || *p == '\t') p++;
            int i = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '|' && i < MAX_PATH - 1) {
                pcmd->redir_file[i++] = *p++;
            }
            pcmd->redir_file[i] = '\0';
            break;
        }

        /* 遇到管道符，停止 */
        if (*p == '|') break;

        /* 解析参数 - 收集到临时缓冲区 */
        int arg_start = arg_buf_pos;

        while (*p) {
            if (*p == ' ' || *p == '\t' || *p == '|') break;
            if (*p == '>' || *p == '<') break;

            /* 处理引号 */
            if (*p == '"' || *p == '\'') {
                char quote = *p;
                p++;
                while (*p && *p != quote) {
                    if (*p == '\\' && *(p+1)) p++;
                    if (arg_buf_pos < MAX_CMD_LEN - 1) {
                        arg_buf[arg_buf_pos++] = *p;
                    }
                    p++;
                }
                if (*p == quote) p++;
                continue;
            }

            /* 处理转义 */
            if (*p == '\\' && *(p+1)) {
                p++;
            }

            if (arg_buf_pos < MAX_CMD_LEN - 1) {
                arg_buf[arg_buf_pos++] = *p;
            }
            p++;
        }

        arg_buf[arg_buf_pos] = '\0';

        /* 检查是否需要通配符展开 */
        if (is_glob_pattern(&arg_buf[arg_start]) && argc > 0) {
            /* 不对命令名进行通配符展开 */
            char *glob_results[64];
            int glob_count = glob_expand(&arg_buf[arg_start], glob_results, 64);
            for (int g = 0; g < glob_count && argc < MAX_ARGS - 1; g++) {
                pcmd->argv[argc] = glob_results[g];
                argc++;
            }
        } else {
            pcmd->argv[argc] = &arg_buf[arg_start];
            argc++;
        }

        if (*p == '\0' || *p == '|' || *p == '>' || *p == '<') break;
    }

    pcmd->argc = argc;
    pcmd->argv[argc] = NULL;
    return argc;
}

/* ============================================
 * 解析管道命令链
 * ============================================ */

int parse_pipeline(const char *cmd, pipeline_t *pipeline)
{
    p_memset(pipeline, 0, sizeof(*pipeline));

    if (!cmd || cmd[0] == '\0') {
        return 0;
    }

    /* 创建可修改的副本 */
    char cmd_buf[MAX_CMD_LEN * 2];
    p_strcpy(cmd_buf, cmd);

    char *p = cmd_buf;
    int count = 0;

    while (*p && count < MAX_PIPELINE) {
        /* 跳过前导空格和管道符 */
        while (*p == ' ' || *p == '\t' || *p == '|') p++;
        if (*p == '\0') break;

        /* 找到当前命令的结束位置 */
        char *cmd_start = p;
        int in_quote = 0;
        char quote_char = 0;

        while (*p) {
            if (in_quote) {
                if (*p == quote_char) {
                    in_quote = 0;
                }
            } else {
                if (*p == '"' || *p == '\'') {
                    in_quote = 1;
                    quote_char = *p;
                } else if (*p == '\\') {
                    if (*(p+1)) p++;
                } else if (*p == '|') {
                    break;
                }
            }
            p++;
        }

        /* 终止当前命令字符串 */
        char saved = *p;
        *p = '\0';

        /* 解析单个命令 */
        if (parse_single_cmd(cmd_start, &pipeline->cmds[count]) > 0) {
            count++;
        }

        *p = saved;
    }

    pipeline->count = count;
    return count;
}

/* ============================================
 * 旧的 parse_command 函数（保留向后兼容）
 * ============================================ */

int parse_command(const char *cmd, char **argv, int max_args)
{
    pipeline_t pipeline;
    int count = parse_pipeline(cmd, &pipeline);
    if (count == 0) {
        argv[0] = NULL;
        return 0;
    }

    /* 只返回第一个命令 */
    int i;
    for (i = 0; i < pipeline.cmds[0].argc && i < max_args - 1; i++) {
        argv[i] = pipeline.cmds[0].argv[i];
    }
    argv[i] = NULL;
    return i;
}

/* ============================================
 * 释放 argv
 * ============================================ */

void free_argv(char **argv, int count)
{
    (void)argv;
    (void)count;
}

/* ============================================
 * 执行管道命令链
 * ============================================ */

int execute_pipeline(const pipeline_t *pipeline)
{
    if (!pipeline || pipeline->count == 0) {
        return 1;
    }

    /* 如果只有一条命令且没有重定向，直接执行 */
    if (pipeline->count == 1) {
        pipeline_cmd_t *pcmd = &pipeline->cmds[0];

        /* 检查是否有重定向 */
        if (pcmd->redir_type == REDIR_NONE) {
            /* 没有重定向，直接执行 */
            int ret = execute_builtin(pcmd->argv, pcmd->argc);
            if (ret >= 0) return ret;
            return execute_external(pcmd->argv, pcmd->argc);
        }

        /* 有重定向，需要 fork */
        long pid = sys_fork();
        if (pid < 0) {
            print("fork failed\n\r");
            return 1;
        }

        if (pid == 0) {
            /* 子进程 */
            int fd = -1;

            if (pcmd->redir_type == REDIR_IN) {
                fd = sys_open(pcmd->redir_file, O_RDONLY, 0);
                if (fd < 0) {
                    print(pcmd->redir_file);
                    print(": No such file or directory\n\r");
                    sys_exit(1);
                }
                sys_dup2(fd, STDIN_FD);
                sys_close(fd);
            } else {
                int flags = O_WRONLY | O_CREAT;
                if (pcmd->redir_type == REDIR_OUT_APP ||
                    pcmd->redir_type == REDIR_ERR_OUT_APP ||
                    pcmd->redir_type == REDIR_ALL_OUT_APP) {
                    flags |= O_APPEND;
                } else {
                    flags |= O_TRUNC;
                }

                fd = sys_open(pcmd->redir_file, flags, 0644);
                if (fd < 0) {
                    print(pcmd->redir_file);
                    print(": Cannot create file\n\r");
                    sys_exit(1);
                }

                if (pcmd->redir_type == REDIR_ERR_OUT ||
                    pcmd->redir_type == REDIR_ERR_OUT_APP) {
                    sys_dup2(fd, STDERR_FD);
                } else {
                    sys_dup2(fd, STDOUT_FD);
                    if (pcmd->redir_type == REDIR_ALL_OUT ||
                        pcmd->redir_type == REDIR_ALL_OUT_APP) {
                        sys_dup2(fd, STDERR_FD);
                    }
                }
                sys_close(fd);
            }

            /* 执行命令 */
            int ret = execute_builtin(pcmd->argv, pcmd->argc);
            if (ret >= 0) {
                sys_exit(ret);
            }
            execute_external(pcmd->argv, pcmd->argc);
            sys_exit(127);
        } else {
            int status = 0;
            sys_wait(-1, &status, 0, NULL);
            return (status >> 8) & 0xFF;
        }
    }

    /* 多条命令的管道 */
    int num_cmds = pipeline->count;
    int pipes[MAX_PIPELINE][2];

    /* 创建所有管道 */
    for (int i = 0; i < num_cmds - 1; i++) {
        if (sys_pipe(pipes[i]) < 0) {
            print("pipe failed\n\r");
            return 1;
        }
    }

    /* fork 每个命令 */
    for (int i = 0; i < num_cmds; i++) {
        long pid = sys_fork();
        if (pid < 0) {
            print("fork failed\n\r");
            return 1;
        }

        if (pid == 0) {
            /* 子进程 */

            /* 设置输入 */
            if (i > 0) {
                sys_dup2(pipes[i-1][0], STDIN_FD);
            } else if (pipeline->cmds[i].redir_type == REDIR_IN) {
                int fd = sys_open(pipeline->cmds[i].redir_file, O_RDONLY, 0);
                if (fd < 0) {
                    print(pipeline->cmds[i].redir_file);
                    print(": No such file or directory\n\r");
                    sys_exit(1);
                }
                sys_dup2(fd, STDIN_FD);
                sys_close(fd);
            }

            /* 设置输出 */
            if (i < num_cmds - 1) {
                sys_dup2(pipes[i][1], STDOUT_FD);
            } else if (pipeline->cmds[i].redir_type == REDIR_OUT ||
                       pipeline->cmds[i].redir_type == REDIR_OUT_APP ||
                       pipeline->cmds[i].redir_type == REDIR_ALL_OUT ||
                       pipeline->cmds[i].redir_type == REDIR_ALL_OUT_APP) {
                int flags = O_WRONLY | O_CREAT;
                if (pipeline->cmds[i].redir_type == REDIR_OUT_APP ||
                    pipeline->cmds[i].redir_type == REDIR_ALL_OUT_APP) {
                    flags |= O_APPEND;
                } else {
                    flags |= O_TRUNC;
                }

                int fd = sys_open(pipeline->cmds[i].redir_file, flags, 0644);
                if (fd < 0) {
                    print(pipeline->cmds[i].redir_file);
                    print(": Cannot create file\n\r");
                    sys_exit(1);
                }
                sys_dup2(fd, STDOUT_FD);
                if (pipeline->cmds[i].redir_type == REDIR_ALL_OUT ||
                    pipeline->cmds[i].redir_type == REDIR_ALL_OUT_APP) {
                    sys_dup2(fd, STDERR_FD);
                }
                sys_close(fd);
            }

            /* 关闭所有管道 fd */
            for (int j = 0; j < num_cmds - 1; j++) {
                sys_close(pipes[j][0]);
                sys_close(pipes[j][1]);
            }

            /* 执行命令 */
            int ret = execute_builtin(pipeline->cmds[i].argv, pipeline->cmds[i].argc);
            if (ret >= 0) {
                sys_exit(ret);
            }
            execute_external(pipeline->cmds[i].argv, pipeline->cmds[i].argc);
            sys_exit(127);
        }
    }

    /* 父进程：关闭所有管道 fd */
    for (int i = 0; i < num_cmds - 1; i++) {
        sys_close(pipes[i][0]);
        sys_close(pipes[i][1]);
    }

    /* 等待所有子进程 */
    int last_status = 0;
    for (int i = 0; i < num_cmds; i++) {
        int status = 0;
        sys_wait(-1, &status, 0, NULL);
        if (i == num_cmds - 1) {
            last_status = (status >> 8) & 0xFF;
        }
    }

    return last_status;
}

/* ============================================
 * 执行命令
 * ============================================ */

int execute_command(const char *cmd)
{
    if (!cmd || cmd[0] == '\0') {
        return 0;
    }

    pipeline_t pipeline;
    int count = parse_pipeline(cmd, &pipeline);

    if (count == 0) {
        return 0;
    }

    return execute_pipeline(&pipeline);
}
