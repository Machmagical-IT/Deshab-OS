/*
 * io.c - 输入输出函数实现
 *
 * 实现屏幕输出、键盘输入、命令行编辑、Tab 补全和 Ctrl+R 搜索
 */

#include "zsh.h"

/* ============================================
 * 基础输出函数
 * ============================================ */

void print(const char *str)
{
    if (str) {
        size_t len = str_len(str);
        sys_write(STDOUT_FD, str, len);

        /* 同时写入日志文件 */
        if (g_shell.log_fd >= 0) {
            sys_write(g_shell.log_fd, str, len);
        }
    }
}

void println(const char *str)
{
    print(str);
    print("\n\r");
}

void print_char(char c)
{
    sys_write(STDOUT_FD, &c, 1);
}

/* ============================================
 * 提示符显示
 * ============================================ */

void print_prompt(void)
{
    const char *username = g_shell.username;
    const char *hostname = get_hostname();
    const char *cwd = g_shell.cwd;

    /* user@host:cwd$ */
    print(username);
    print("@");
    print(hostname);
    print(":");

    /* 显示当前目录 */
    if (cwd[0] == '\0') {
        print("/");
    } else {
        /* 如果在家目录，显示 ~ */
        const char *home = "/root";
        size_t home_len = str_len(home);
        if (str_ncmp(cwd, home, home_len) == 0 &&
            (cwd[home_len] == '\0' || cwd[home_len] == '/')) {
            print("~");
            print(cwd + home_len);
        } else {
            print(cwd);
        }
    }

    /* root 用户显示 #，否则显示 $ */
    if (g_shell.is_root) {
        print("# ");
    } else {
        print("$ ");
    }
}

/* ============================================
 * 命令行刷新
 *
 * 清除当前行并重新显示提示符和命令行
 * ============================================ */

void refresh_line(void)
{
    /* 移动到行首 */
    print("\r");
    /* 清除整行 */
    print("\033[2K");

    /* 显示提示符 */
    print_prompt();

    /* 显示命令行内容 */
    if (g_shell.cmd_len > 0) {
        char temp = g_shell.cmd_line[g_shell.cmd_len];
        g_shell.cmd_line[g_shell.cmd_len] = '\0';
        print(g_shell.cmd_line);
        g_shell.cmd_line[g_shell.cmd_len] = temp;
    }

    /* 移动光标到正确位置 */
    if (g_shell.cmd_pos < g_shell.cmd_len) {
        int diff = g_shell.cmd_len - g_shell.cmd_pos;
        for (int i = 0; i < diff; i++) {
            print("\033[D");  /* 光标左移 */
        }
    }
}

/* ============================================
 * 键盘输入读取
 *
 * 读取一个按键，处理转义序列（箭头键、Home/End 等）
 * ============================================ */

int read_key(void)
{
    char c;
    long nread = sys_read(STDIN_FD, &c, 1);

    if (nread != 1) {
        return -1;
    }

    /* 处理转义序列 */
    if (c == KEY_ESC) {
        char seq[4];
        if (sys_read(STDIN_FD, &seq[0], 1) != 1) {
            return KEY_ESC;
        }

        /* ANSI 转义序列: ESC [ ... */
        if (seq[0] == '[') {
            if (sys_read(STDIN_FD, &seq[1], 1) != 1) {
                return KEY_ESC;
            }

            /* ESC [ A/B/C/D - 方向键 */
            if (seq[1] >= 'A' && seq[1] <= 'D') {
                return KEY_UP + (seq[1] - 'A');
            }

            /* ESC [ H - Home */
            if (seq[1] == 'H') {
                return KEY_HOME;
            }

            /* ESC [ F - End */
            if (seq[1] == 'F') {
                return KEY_END;
            }

            /* ESC [ 1/2/3/4/5/6 ~ - 特殊键 */
            if (seq[1] >= '1' && seq[1] <= '6') {
                if (sys_read(STDIN_FD, &seq[2], 1) != 1) {
                    return KEY_ESC;
                }
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return KEY_HOME;
                        case '2': return KEY_INSERT;
                        case '3': return KEY_DELETE;
                        case '4': return KEY_END;
                        case '5': return KEY_PAGEUP;
                        case '6': return KEY_PAGEDOWN;
                    }
                }
                /* ESC [ 1 ; 5 C/D - Ctrl+Left/Right */
                if (seq[2] == ';') {
                    if (sys_read(STDIN_FD, &seq[3], 1) == 1) {
                        /* 读取最后的字符 */
                        char last;
                        if (sys_read(STDIN_FD, &last, 1) == 1) {
                            if (seq[1] == '1' && seq[3] == '5') {
                                if (last == 'C') return KEY_RIGHT;  /* Ctrl+Right */
                                if (last == 'D') return KEY_LEFT;   /* Ctrl+Left */
                            }
                        }
                    }
                }
            }

            /* ESC [ 7 ~ - Home (vt100) */
            if (seq[1] == '7') {
                if (sys_read(STDIN_FD, &seq[2], 1) == 1 && seq[2] == '~') {
                    return KEY_HOME;
                }
            }

            /* ESC [ 8 ~ - End (vt100) */
            if (seq[1] == '8') {
                if (sys_read(STDIN_FD, &seq[2], 1) == 1 && seq[2] == '~') {
                    return KEY_END;
                }
            }
        }

        /* ESC O - 应用键盘模式 */
        if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
                case 'P': return KEY_ESC;  /* F1 - 忽略 */
                case 'Q': return KEY_ESC;  /* F2 */
                case 'R': return KEY_ESC;  /* F3 */
                case 'S': return KEY_ESC;  /* F4 */
            }
        }

        return KEY_ESC;
    }

    return (unsigned char)c;
}

/* ============================================
 * 命令行编辑函数
 * ============================================ */

void cmd_line_init(void)
{
    g_shell.cmd_len = 0;
    g_shell.cmd_pos = 0;
    g_shell.cmd_line[0] = '\0';
}

void cmd_line_insert(char c)
{
    if (g_shell.cmd_len >= MAX_CMD_LEN - 1) {
        return;  /* 命令行已满 */
    }

    /* 如果光标在末尾，直接添加 */
    if (g_shell.cmd_pos == g_shell.cmd_len) {
        g_shell.cmd_line[g_shell.cmd_pos] = c;
        g_shell.cmd_pos++;
        g_shell.cmd_len++;
        g_shell.cmd_line[g_shell.cmd_len] = '\0';

        /* 显示新字符 */
        print_char(c);
    } else {
        /* 在中间插入，需要移动后续字符 */
        int move_count = g_shell.cmd_len - g_shell.cmd_pos;
        for (int i = move_count; i > 0; i--) {
            g_shell.cmd_line[g_shell.cmd_pos + i] = g_shell.cmd_line[g_shell.cmd_pos + i - 1];
        }
        g_shell.cmd_line[g_shell.cmd_pos] = c;
        g_shell.cmd_pos++;
        g_shell.cmd_len++;
        g_shell.cmd_line[g_shell.cmd_len] = '\0';

        /* 刷新整行 */
        refresh_line();
    }
}

void cmd_line_overwrite(char c)
{
    if (g_shell.cmd_pos >= MAX_CMD_LEN - 1) {
        return;
    }

    if (g_shell.cmd_pos >= g_shell.cmd_len) {
        /* 在末尾，等同于插入 */
        cmd_line_insert(c);
    } else {
        /* 覆盖当前字符 */
        g_shell.cmd_line[g_shell.cmd_pos] = c;
        g_shell.cmd_pos++;
        print_char(c);
    }
}

void cmd_line_delete(void)
{
    if (g_shell.cmd_pos >= g_shell.cmd_len) {
        return;  /* 已在末尾 */
    }

    /* 删除光标处字符 */
    int move_count = g_shell.cmd_len - g_shell.cmd_pos - 1;
    for (int i = 0; i < move_count; i++) {
        g_shell.cmd_line[g_shell.cmd_pos + i] = g_shell.cmd_line[g_shell.cmd_pos + i + 1];
    }
    g_shell.cmd_len--;
    g_shell.cmd_line[g_shell.cmd_len] = '\0';

    refresh_line();
}

void cmd_line_backspace(void)
{
    if (g_shell.cmd_pos <= 0) {
        return;  /* 已在开头 */
    }

    /* 光标左移 */
    g_shell.cmd_pos--;

    /* 删除字符 */
    int move_count = g_shell.cmd_len - g_shell.cmd_pos - 1;
    for (int i = 0; i < move_count; i++) {
        g_shell.cmd_line[g_shell.cmd_pos + i] = g_shell.cmd_line[g_shell.cmd_pos + i + 1];
    }
    g_shell.cmd_len--;
    g_shell.cmd_line[g_shell.cmd_len] = '\0';

    refresh_line();
}

void cmd_line_move_left(void)
{
    if (g_shell.cmd_pos > 0) {
        g_shell.cmd_pos--;
        print("\033[D");  /* 光标左移 */
    }
}

void cmd_line_move_right(void)
{
    if (g_shell.cmd_pos < g_shell.cmd_len) {
        g_shell.cmd_pos++;
        print("\033[C");  /* 光标右移 */
    }
}

void cmd_line_move_home(void)
{
    if (g_shell.cmd_pos > 0) {
        int diff = g_shell.cmd_pos;
        g_shell.cmd_pos = 0;
        for (int i = 0; i < diff; i++) {
            print("\033[D");
        }
    }
}

void cmd_line_move_end(void)
{
    if (g_shell.cmd_pos < g_shell.cmd_len) {
        int diff = g_shell.cmd_len - g_shell.cmd_pos;
        g_shell.cmd_pos = g_shell.cmd_len;
        for (int i = 0; i < diff; i++) {
            print("\033[C");
        }
    }
}

void cmd_line_clear(void)
{
    /* 清除整行并重新显示提示符 */
    print("\r\033[2K");
    print_prompt();
    cmd_line_init();
}

void cmd_line_kill_to_end(void)
{
    if (g_shell.cmd_pos >= g_shell.cmd_len) {
        return;
    }
    g_shell.cmd_len = g_shell.cmd_pos;
    g_shell.cmd_line[g_shell.cmd_len] = '\0';
    refresh_line();
}

void cmd_line_kill_to_start(void)
{
    if (g_shell.cmd_pos <= 0) {
        return;
    }
    int move_count = g_shell.cmd_len - g_shell.cmd_pos;
    for (int i = 0; i < move_count; i++) {
        g_shell.cmd_line[i] = g_shell.cmd_line[g_shell.cmd_pos + i];
    }
    g_shell.cmd_len = move_count;
    g_shell.cmd_pos = 0;
    g_shell.cmd_line[g_shell.cmd_len] = '\0';
    refresh_line();
}

void cmd_line_kill_word_back(void)
{
    if (g_shell.cmd_pos <= 0) {
        return;
    }

    /* 跳过尾部空格 */
    int pos = g_shell.cmd_pos - 1;
    while (pos > 0 && g_shell.cmd_line[pos] == ' ') {
        pos--;
    }

    /* 跳过当前单词 */
    while (pos > 0 && g_shell.cmd_line[pos - 1] != ' ') {
        pos--;
    }

    /* 删除从 pos 到 cmd_pos 的字符 */
    int del_count = g_shell.cmd_pos - pos;
    int move_count = g_shell.cmd_len - g_shell.cmd_pos;
    for (int i = 0; i < move_count; i++) {
        g_shell.cmd_line[pos + i] = g_shell.cmd_line[g_shell.cmd_pos + i];
    }
    g_shell.cmd_len -= del_count;
    g_shell.cmd_pos = pos;
    g_shell.cmd_line[g_shell.cmd_len] = '\0';

    refresh_line();
}

/* ============================================
 * Tab 补全
 * ============================================ */

/* 读取目录中的匹配项 */
static int read_dir_matches(const char *dir_path, const char *prefix,
                            char matches[][MAX_PATH], int max_matches)
{
    long fd = sys_open(dir_path, O_RDONLY, 0);
    if (fd < 0) return 0;

    char buf[4096];
    int count = 0;
    int prefix_len = str_len(prefix);

    while (count < max_matches) {
        long nread = sys_readdir(fd, buf, 4096);
        if (nread <= 0) break;

        long offset = 0;
        while (offset < nread && count < max_matches) {
            struct linux_dirent64 *dirent = (struct linux_dirent64*)(buf + offset);

            /* 跳过 . 和 .. */
            if (dirent->d_name[0] == '.' &&
                (dirent->d_name[1] == '\0' ||
                 (dirent->d_name[1] == '.' && dirent->d_name[2] == '\0'))) {
                offset += dirent->d_reclen;
                continue;
            }

            /* 检查前缀匹配 */
            if (str_ncmp(dirent->d_name, prefix, prefix_len) == 0) {
                str_ncpy(matches[count], dirent->d_name, MAX_PATH - 1);
                /* 如果是目录，加 / */
                if (dirent->d_type == DT_DIR) {
                    int len = str_len(matches[count]);
                    if (len < MAX_PATH - 1) {
                        matches[count][len] = '/';
                        matches[count][len + 1] = '\0';
                    }
                }
                count++;
            }

            offset += dirent->d_reclen;
        }
    }

    sys_close(fd);
    return count;
}

/* 查找命令名补全 */
static int find_cmd_matches(const char *prefix, char matches[][MAX_PATH], int max_matches)
{
    int count = 0;
    int prefix_len = str_len(prefix);

    /* 检查内置命令 */
    for (int i = 0; builtins[i].name && count < max_matches; i++) {
        if (str_ncmp(builtins[i].name, prefix, prefix_len) == 0) {
            str_ncpy(matches[count], builtins[i].name, MAX_PATH - 1);
            count++;
        }
    }

    /* 检查 /prgm/ 下的程序 */
    count += read_dir_matches("/prgm", prefix, &matches[count], max_matches - count);
    /* 检查 /prgm/sys/ 下的程序 */
    count += read_dir_matches("/prgm/sys", prefix, &matches[count], max_matches - count);
    /* 检查 /bin/ 下的程序 */
    count += read_dir_matches("/bin", prefix, &matches[count], max_matches - count);

    return count;
}

/* 计算公共前缀 */
static int common_prefix_len(char matches[][MAX_PATH], int count)
{
    if (count <= 0) return 0;
    int len = 0;
    while (1) {
        char c = matches[0][len];
        if (c == '\0') break;
        int all_match = 1;
        for (int i = 1; i < count; i++) {
            if (matches[i][len] != c) {
                all_match = 0;
                break;
            }
        }
        if (!all_match) break;
        len++;
    }
    return len;
}

int cmd_line_tab_complete(void)
{
    /* 找到当前正在输入的单词 */
    int word_start = g_shell.cmd_pos;
    while (word_start > 0 && g_shell.cmd_line[word_start - 1] != ' ') {
        word_start--;
    }

    /* 空行或命令位置：补全命令名 */
    int is_command_pos = 1;
    for (int i = 0; i < word_start; i++) {
        if (g_shell.cmd_line[i] != ' ') {
            is_command_pos = 0;
            break;
        }
    }

    char prefix[MAX_PATH] = {0};
    int prefix_len = g_shell.cmd_pos - word_start;
    if (prefix_len > 0) {
        str_ncpy(prefix, &g_shell.cmd_line[word_start], prefix_len);
    }
    prefix[prefix_len] = '\0';

    char matches[64][MAX_PATH];
    int match_count = 0;

    if (is_command_pos) {
        /* 补全命令名 */
        match_count = find_cmd_matches(prefix, matches, 64);
    } else {
        /* 补全文件名 */
        /* 确定目录和前缀 */
        char dir[MAX_PATH] = {0};
        char file_prefix[MAX_PATH] = {0};

        /* 查找最后一个 / */
        int last_slash = -1;
        for (int i = prefix_len - 1; i >= 0; i--) {
            if (prefix[i] == '/') {
                last_slash = i;
                break;
            }
        }

        if (last_slash >= 0) {
            /* 有路径前缀 */
            str_ncpy(dir, prefix, last_slash + 1);
            dir[last_slash + 1] = '\0';
            str_cpy(file_prefix, &prefix[last_slash + 1]);
        } else {
            /* 无路径前缀，使用当前目录 */
            str_cpy(dir, ".");
            str_cpy(file_prefix, prefix);
        }

        match_count = read_dir_matches(dir, file_prefix, matches, 64);

        /* 如果有目录前缀，需要在匹配结果前加上目录路径 */
        if (last_slash >= 0 && match_count > 0) {
            for (int i = 0; i < match_count; i++) {
                char temp[MAX_PATH];
                str_cpy(temp, dir);
                str_cat(temp, matches[i]);
                str_ncpy(matches[i], temp, MAX_PATH - 1);
            }
        }
    }

    if (match_count == 0) {
        /* 无匹配 */
        return 0;
    }

    if (match_count == 1) {
        /* 唯一匹配：直接补全 */
        const char *completion = matches[0];
        int comp_len = str_len(completion);

        /* 删除已输入的前缀部分，插入补全内容 */
        for (int i = word_start; i < g_shell.cmd_pos; i++) {
            /* 退格删除 */
        }
        /* 移动光标到 word_start */
        while (g_shell.cmd_pos > word_start) {
            g_shell.cmd_pos--;
            g_shell.cmd_line[g_shell.cmd_pos] = '\0';
        }
        g_shell.cmd_len = word_start;

        /* 插入补全内容 */
        for (int i = 0; i < comp_len; i++) {
            cmd_line_insert(completion[i]);
        }

        /* 如果是命令补全且不是目录，加空格 */
        if (is_command_pos || matches[0][comp_len - 1] != '/') {
            cmd_line_insert(' ');
        }

        return 1;
    }

    /* 多个匹配：补全公共前缀 */
    int cpl = common_prefix_len(matches, match_count);
    if (cpl > prefix_len) {
        /* 补全公共前缀部分 */
        while (g_shell.cmd_pos > word_start) {
            g_shell.cmd_pos--;
        }
        g_shell.cmd_len = word_start;

        for (int i = 0; i < cpl; i++) {
            cmd_line_insert(matches[0][i]);
        }
        return 1;
    }

    /* 显示所有匹配 */
    print("\n\r");
    for (int i = 0; i < match_count; i++) {
        print(matches[i]);
        print("  ");
        if ((i + 1) % 6 == 0) print("\n\r");
    }
    print("\n\r");

    /* 重新显示提示符和命令行 */
    print_prompt();
    refresh_line();
    return 1;
}

/* ============================================
 * Ctrl+R 增量搜索
 * ============================================ */

void search_start(void)
{
    g_shell.search_mode = 1;
    g_shell.search_query[0] = '\0';
    g_shell.search_result_idx = -1;

    /* 显示搜索提示 */
    print("\n\r\033[2K");
    print("\r\033[1;33m(reverse-i-search)`'`\033[0m: ");
}

void search_stop(void)
{
    g_shell.search_mode = 0;

    /* 清除搜索提示行 */
    print("\r\033[2K");

    /* 重新显示提示符和当前命令行 */
    print_prompt();
    refresh_line();
}

int search_handle_key(int key)
{
    if (key == KEY_CTRL_R) {
        /* 继续搜索下一个匹配 */
        const char *result = history_search(g_shell.search_query, -1);
        if (result) {
            /* 更新命令行 */
            cmd_line_clear();
            for (int i = 0; result[i]; i++) {
                cmd_line_insert(result[i]);
            }
        }
        /* 更新搜索提示 */
        print("\r\033[2K");
        print("\033[1;33m(reverse-i-search)`");
        print(g_shell.search_query);
        print("'\033[0m: ");
        if (g_shell.cmd_len > 0) {
            char temp = g_shell.cmd_line[g_shell.cmd_len];
            g_shell.cmd_line[g_shell.cmd_len] = '\0';
            print(g_shell.cmd_line);
            g_shell.cmd_line[g_shell.cmd_len] = temp;
        }
        return 1;
    }

    if (key == KEY_ENTER || key == KEY_CTRL_C || key == KEY_ESC) {
        /* 退出搜索模式 */
        search_stop();
        return (key == KEY_ENTER) ? 0 : 1;
    }

    if (key == KEY_BACKSPACE) {
        /* 删除搜索查询的最后一个字符 */
        int qlen = str_len(g_shell.search_query);
        if (qlen > 0) {
            g_shell.search_query[qlen - 1] = '\0';
        }
        /* 重新搜索 */
        const char *result = history_search(g_shell.search_query, -1);
        cmd_line_clear();
        if (result) {
            for (int i = 0; result[i]; i++) {
                cmd_line_insert(result[i]);
            }
        }
        /* 更新搜索提示 */
        print("\r\033[2K");
        print("\033[1;33m(reverse-i-search)`");
        print(g_shell.search_query);
        print("'\033[0m: ");
        if (g_shell.cmd_len > 0) {
            char temp = g_shell.cmd_line[g_shell.cmd_len];
            g_shell.cmd_line[g_shell.cmd_len] = '\0';
            print(g_shell.cmd_line);
            g_shell.cmd_line[g_shell.cmd_len] = temp;
        }
        return 1;
    }

    /* 可打印字符：添加到搜索查询 */
    if (key >= 32 && key <= 126) {
        int qlen = str_len(g_shell.search_query);
        if (qlen < MAX_CMD_LEN - 1) {
            g_shell.search_query[qlen] = (char)key;
            g_shell.search_query[qlen + 1] = '\0';
        }

        /* 搜索匹配 */
        const char *result = history_search(g_shell.search_query, -1);
        cmd_line_clear();
        if (result) {
            for (int i = 0; result[i]; i++) {
                cmd_line_insert(result[i]);
            }
        }

        /* 更新搜索提示 */
        print("\r\033[2K");
        print("\033[1;33m(reverse-i-search)`");
        print(g_shell.search_query);
        print("'\033[0m: ");
        if (g_shell.cmd_len > 0) {
            char temp = g_shell.cmd_line[g_shell.cmd_len];
            g_shell.cmd_line[g_shell.cmd_len] = '\0';
            print(g_shell.cmd_line);
            g_shell.cmd_line[g_shell.cmd_len] = temp;
        }
        return 1;
    }

    return 1;
}

const char* search_get_prompt(void)
{
    return "(reverse-i-search)`': ";
}
