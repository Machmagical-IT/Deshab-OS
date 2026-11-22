/*
 * main.c - Zratro Shell 主程序
 *
 * 完整的用户态 shell，支持：
 * - 键盘输入捕获（原始模式）
 * - 命令行编辑和历史
 * - 内置命令
 * - 外部程序加载
 * - Tab 补全
 * - Ctrl+R 增量搜索
 *
 * 作者：ZratroOS 开发团队
 * 日期：2026-04-11
 */

#include "zsh.h"

/* ============================================
 * 全局变量
 * ============================================ */

shell_t g_shell;

/* ============================================
 * 程序入口
 * ============================================ */

int main(void)
{
    /* 初始化 shell */
    shell_init();

    /* 运行 shell */
    shell_run();

    /* 清理 */
    shell_cleanup();

    return 0;
}

/* ============================================
 * Shell 初始化
 * ============================================ */

void shell_init(void)
{
    /* 初始化状态 */
    g_shell.running = 1;
    g_shell.raw_mode = 0;
    g_shell.log_fd = -1;
    g_shell.insert_mode = 0;
    g_shell.search_mode = 0;
    g_shell.search_query[0] = '\0';
    g_shell.search_result_idx = -1;

    /* 设置当前目录 */
    sys_getcwd(g_shell.cwd, MAX_PATH);
    if (g_shell.cwd[0] == '\0') {
        str_cpy(g_shell.cwd, "/");
    }

    /* 设置提示符 */
    str_cpy(g_shell.prompt, "\\u@\\h:\\w$ ");

    /* 初始化用户信息 */
    long uid = sys_getuid();
    g_shell.is_root = (uid == 0);
    str_cpy(g_shell.username, "root");

    /* 尝试获取用户名 (简化版: root 或 user) */
    if (!g_shell.is_root) {
        str_cpy(g_shell.username, "user");
    }

    /* 初始化命令行 */
    cmd_line_init();

    /* 初始化历史记录 */
    history_init();

    /* 打开日志文件 */
    g_shell.log_fd = sys_open(LOG_FILE_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);

    /* 启用原始模式 */
    if (term_enable_raw_mode() == 0) {
        /* 显示欢迎信息 */
        println("");
        println("Zratro Shell (zsh) - Version 2.0");
        println("Type 'help' for available commands.");
        println("");
    }
}

/* ============================================
 * Shell 主循环
 * ============================================ */

void shell_run(void)
{
    while (g_shell.running) {
        /* 显示提示符 */
        print_prompt();

        /* 重置命令行 */
        cmd_line_init();

        /* 读取命令行 */
        while (g_shell.running) {
            int key = read_key();

            if (key < 0) {
                continue;
            }

            /* Ctrl+R 搜索模式 */
            if (g_shell.search_mode) {
                int result = search_handle_key(key);
                if (result == 0 && key == KEY_ENTER) {
                    /* 搜索模式下按回车，执行当前命令行 */
                    if (g_shell.cmd_len > 0) {
                        history_add(g_shell.cmd_line);
                        print("\n\r");
                        g_shell.cmd_line[g_shell.cmd_len] = '\0';
                        execute_command(g_shell.cmd_line);
                    } else {
                        print("\n\r");
                    }
                    goto next_command;
                }
                continue;
            }

            switch (key) {
                case KEY_ENTER:
                    /* 执行命令 */
                    if (g_shell.cmd_len > 0) {
                        /* 添加到历史 */
                        history_add(g_shell.cmd_line);

                        /* 换行 */
                        print("\n\r");

                        /* 执行命令 */
                        g_shell.cmd_line[g_shell.cmd_len] = '\0';
                        execute_command(g_shell.cmd_line);
                    } else {
                        print("\n\r");
                    }
                    goto next_command;

                case KEY_BACKSPACE:
                    cmd_line_backspace();
                    break;

                case KEY_DELETE:
                    cmd_line_delete();
                    break;

                case KEY_LEFT:
                    cmd_line_move_left();
                    break;

                case KEY_RIGHT:
                    cmd_line_move_right();
                    break;

                case KEY_UP:
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

                case KEY_DOWN:
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

                case KEY_HOME:
                    cmd_line_move_home();
                    break;

                case KEY_END:
                    cmd_line_move_end();
                    break;

                case KEY_INSERT:
                    /* 切换插入/覆盖模式 */
                    g_shell.insert_mode = !g_shell.insert_mode;
                    break;

                case KEY_TAB:
                    cmd_line_tab_complete();
                    break;

                case KEY_CTRL_C:
                    /* 清除当前行 */
                    print("^C\n\r");
                    cmd_line_clear();
                    goto next_command;

                case KEY_CTRL_D:
                    /* EOF - 退出 */
                    if (g_shell.cmd_len == 0) {
                        println("exit");
                        g_shell.running = 0;
                        return;
                    }
                    break;

                case KEY_CTRL_L:
                    /* 清屏 */
                    print("\033[2J\033[H");
                    refresh_line();
                    break;

                case KEY_CTRL_R:
                    /* 增量搜索 */
                    search_start();
                    break;

                case KEY_CTRL_A:
                    /* 移到行首 (等同于 Home) */
                    cmd_line_move_home();
                    break;

                case KEY_CTRL_E:
                    /* 移到行尾 (等同于 End) */
                    cmd_line_move_end();
                    break;

                case KEY_CTRL_U:
                    /* 删除从行首到光标 */
                    cmd_line_kill_to_start();
                    break;

                case KEY_CTRL_K:
                    /* 删除从光标到行尾 */
                    cmd_line_kill_to_end();
                    break;

                case KEY_CTRL_W:
                    /* 删除前一个单词 */
                    cmd_line_kill_word_back();
                    break;

                case KEY_ESC:
                    /* ESC 键忽略 */
                    break;

                default:
                    /* 可打印字符 */
                    if (key >= 32 && key <= 126) {
                        if (g_shell.insert_mode) {
                            cmd_line_overwrite((char)key);
                        } else {
                            cmd_line_insert((char)key);
                        }
                    }
                    break;
            }
        }

        next_command:
        /* 继续下一条命令 */
        ;
    }
}

/* ============================================
 * Shell 清理
 * ============================================ */

void shell_cleanup(void)
{
    /* 关闭日志文件 */
    if (g_shell.log_fd >= 0) {
        sys_close(g_shell.log_fd);
        g_shell.log_fd = -1;
    }

    /* 恢复终端模式 */
    term_disable_raw_mode();

    /* 退出 */
    sys_exit(0);
}
