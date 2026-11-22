/*
 * history.c - 命令历史记录
 *
 * 实现命令历史的存储、添加、导航和搜索
 */

#include "zsh.h"

/* ============================================
 * 历史记录初始化
 * ============================================ */

void history_init(void)
{
    g_shell.history_count = 0;
    g_shell.history_pos = 0;
    g_shell.history_current = 0;
    g_shell.history_saved_current = 0;

    /* 清空历史记录缓冲区 */
    for (int i = 0; i < MAX_HISTORY; i++) {
        g_shell.history[i][0] = '\0';
    }
}

/* ============================================
 * 添加命令到历史
 *
 * 使用循环缓冲区，旧记录会被覆盖
 * ============================================ */

void history_add(const char *cmd)
{
    if (!cmd || cmd[0] == '\0') {
        return;  /* 不记录空命令 */
    }

    /* 检查是否与上一条相同 */
    if (g_shell.history_count > 0) {
        int last_idx = (g_shell.history_pos - 1 + MAX_HISTORY) % MAX_HISTORY;
        if (str_cmp(g_shell.history[last_idx], cmd) == 0) {
            return;  /* 不记录重复命令 */
        }
    }

    /* 添加到当前位置 */
    str_ncpy(g_shell.history[g_shell.history_pos], cmd, MAX_CMD_LEN - 1);
    g_shell.history[g_shell.history_pos][MAX_CMD_LEN - 1] = '\0';
    g_shell.history_pos = (g_shell.history_pos + 1) % MAX_HISTORY;

    if (g_shell.history_count < MAX_HISTORY) {
        g_shell.history_count++;
    }

    /* 重置导航位置 */
    g_shell.history_current = g_shell.history_pos;
    g_shell.history_saved_current = g_shell.history_pos;

    /* 写入日志文件 */
    if (g_shell.log_fd >= 0) {
        sys_write(g_shell.log_fd, cmd, str_len(cmd));
        sys_write(g_shell.log_fd, "\n", 1);
    }
}

/* ============================================
 * 导航到上一条历史命令
 * ============================================ */

const char* history_prev(void)
{
    if (g_shell.history_count == 0) {
        return NULL;
    }

    /* 计算前一个位置 */
    int prev = (g_shell.history_current - 1 + MAX_HISTORY) % MAX_HISTORY;

    /* 检查是否到达最早的历史 */
    /* history_pos 指向下一个写入位置，最早的历史在 history_pos 处（如果已满） */
    int oldest;
    if (g_shell.history_count < MAX_HISTORY) {
        oldest = 0;
    } else {
        oldest = g_shell.history_pos;
    }

    /* 如果当前位置已经是最早的，不再往前 */
    if (g_shell.history_current == oldest) {
        return g_shell.history[g_shell.history_current];
    }

    g_shell.history_current = prev;
    return g_shell.history[g_shell.history_current];
}

/* ============================================
 * 导航到下一条历史命令
 * ============================================ */

const char* history_next(void)
{
    if (g_shell.history_count == 0) {
        return NULL;
    }

    /* 如果已经在最新位置，返回空字符串 */
    if (g_shell.history_current == g_shell.history_pos) {
        return "";  /* 返回空字符串，表示清除当前行 */
    }

    g_shell.history_current = (g_shell.history_current + 1) % MAX_HISTORY;

    if (g_shell.history_current == g_shell.history_pos) {
        return "";  /* 回到最新位置，清除当前行 */
    }

    return g_shell.history[g_shell.history_current];
}

/* ============================================
 * 重置导航位置
 * ============================================ */

void history_reset_nav(void)
{
    g_shell.history_current = g_shell.history_pos;
}

/* ============================================
 * 搜索历史命令
 *
 * direction: -1 向前搜索（更早的命令）
 *            1  向后搜索（更新的命令）
 * 返回匹配的历史命令，或 NULL
 * ============================================ */

const char* history_search(const char *prefix, int direction)
{
    if (!prefix || prefix[0] == '\0' || g_shell.history_count == 0) {
        return NULL;
    }

    int prefix_len = str_len(prefix);
    int start = g_shell.history_current;
    int idx = start;

    for (int i = 0; i < g_shell.history_count; i++) {
        idx = (idx + direction + MAX_HISTORY) % MAX_HISTORY;

        /* 检查是否越界 */
        if (idx == g_shell.history_pos) {
            break;
        }

        /* 检查前缀匹配 */
        if (str_ncmp(g_shell.history[idx], prefix, prefix_len) == 0) {
            g_shell.history_current = idx;
            return g_shell.history[idx];
        }
    }

    return NULL;
}
