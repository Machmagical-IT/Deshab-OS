/*
 * termios.c - 终端 I/O 控制
 * 
 * 实现终端原始模式设置，用于捕获键盘输入
 */

#include "zsh.h"

/* ============================================
 * 终端属性获取和设置
 * ============================================ */

int term_get_attr(int fd, struct termios *term)
{
    long ret = sys_ioctl(fd, TCGETS, term);
    return (ret < 0) ? -1 : 0;
}

int term_set_attr(int fd, const struct termios *term)
{
    long ret = sys_ioctl(fd, TCSETS, (void*)term);
    return (ret < 0) ? -1 : 0;
}

/* ============================================
 * 启用原始模式
 * 
 * 原始模式特点：
 * - 禁用回显 (ECHO)
 * - 禁用规范模式 (ICANON)
 * - 禁用信号处理 (ISIG)
 * - 每次读取一个字符
 * ============================================ */

int term_enable_raw_mode(void)
{
    if (g_shell.raw_mode) {
        return 0;  /* 已经是原始模式 */
    }
    
    /* 获取当前终端属性 */
    if (term_get_attr(STDIN_FD, &g_shell.orig_termios) == -1) {
        return -1;
    }
    
    /* 创建原始模式设置 */
    struct termios raw = g_shell.orig_termios;
    
    /* 禁用输入模式 */
    raw.c_iflag &= ~(0x00000400 | 0x00000020 | 0x00000010 | 0x00000008 | 0x00000004 | 0x00000002 | 0x00000001);
    /* BRKINT, ICRNL, INPCK, ISTRIP, IXON, IUCLC, IXANY */
    
    /* 禁用输出模式 */
    raw.c_oflag &= ~(OPOST);
    
    /* 禁用控制模式 */
    raw.c_cflag &= ~(0x00000060 | 0x00000010);
    /* CS8, CREAD */
    raw.c_cflag |= 0x00000030;  /* CS8 */
    
    /* 禁用本地模式 */
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    /* ECHO, ICANON, ISIG, IEXTEN */
    
    /* 设置读取参数：最少 1 字符，无超时 */
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    
    /* 应用设置 */
    if (term_set_attr(STDIN_FD, &raw) == -1) {
        return -1;
    }
    
    g_shell.raw_mode = 1;
    return 0;
}

/* ============================================
 * 禁用原始模式
 * 
 * 恢复终端到原始设置
 * ============================================ */

int term_disable_raw_mode(void)
{
    if (!g_shell.raw_mode) {
        return 0;  /* 不是原始模式 */
    }
    
    /* 恢复原始终端设置 */
    if (term_set_attr(STDIN_FD, &g_shell.orig_termios) == -1) {
        return -1;
    }
    
    g_shell.raw_mode = 0;
    return 0;
}
