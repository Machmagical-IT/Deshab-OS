/*
 * Zratro OS - TTY Subsystem Implementation
 *
 * 实现终端子系统核心功能：
 * - TTY 设备管理
 * - 线路规程（规范模式和原始模式）
 * - 缓冲区管理
 * - PTY 伪终端对
 */

#include "tty.h"
#include "console.h"
#include "vfs.h"
#include "task.h"
#include "heap.h"
#include "string.h"
#include "errno.h"

/* ========== 全局状态 ========== */

static struct tty_struct *tty_table[256];
static int tty_count = 0;
static volatile int tty_lock = 0;

/* 默认 termios 设置 */
static struct termios default_termios = {
    .c_iflag = ICRNL | IXON,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = CS8 | B38400 | CREAD | CLOCAL,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN,
    .c_line = 0,
    .c_cc = {
        [VINTR]    = 3,    /* Ctrl+C */
        [VQUIT]    = 0x1c, /* Ctrl+\ */
        [VERASE]   = 0x7f, /* Backspace */
        [VKILL]    = 0x15, /* Ctrl+U */
        [VEOF]     = 4,    /* Ctrl+D */
        [VTIME]    = 0,
        [VMIN]     = 1,
        [VSTART]   = 0x11, /* Ctrl+Q */
        [VSTOP]    = 0x13, /* Ctrl+S */
        [VSUSP]    = 0x1a, /* Ctrl+Z */
    },
    .c_ispeed = 38400,
    .c_ospeed = 38400,
};

/* ========== TTY 缓冲区操作 ========== */

static void tty_buf_init(struct tty_buffer *buf)
{
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
    for (int i = 0; i < TTY_BUFFER_SIZE; i++)
        buf->data[i] = 0;
}

static int tty_buf_put(struct tty_buffer *buf, uint8_t ch)
{
    if (buf->count >= TTY_BUFFER_SIZE)
        return -1;  /* 缓冲区满 */
    buf->data[buf->head] = ch;
    buf->head = (buf->head + 1) % TTY_BUFFER_SIZE;
    buf->count++;
    return 0;
}

static int tty_buf_get(struct tty_buffer *buf, uint8_t *ch)
{
    if (buf->count == 0)
        return -1;  /* 缓冲区空 */
    *ch = buf->data[buf->tail];
    buf->tail = (buf->tail + 1) % TTY_BUFFER_SIZE;
    buf->count--;
    return 0;
}

static int tty_buf_peek(struct tty_buffer *buf, uint8_t *ch)
{
    if (buf->count == 0)
        return -1;
    *ch = buf->data[buf->tail];
    return 0;
}

static void tty_buf_flush(struct tty_buffer *buf)
{
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
}

static int tty_buf_count(struct tty_buffer *buf)
{
    return buf->count;
}

/* ========== TTY 分配/释放 ========== */

struct tty_struct *tty_alloc(int index, enum tty_type type)
{
    struct tty_struct *tty = kzalloc(sizeof(struct tty_struct));
    if (!tty)
        return NULL;

    tty->index = index;
    tty->type = type;
    tty->ldisc = TTY_LDISC_CANON;
    tty->termios = default_termios;
    tty->major = 4;  /* TTY 主设备号 */
    tty->minor = index;
    tty->canon_pos = 0;
    tty->winsize.ws_row = 24;
    tty->winsize.ws_col = 80;
    tty->open = 0;
    tty->stopped = 0;
    tty->packet_mode = 0;
    tty->pgrp = 0;
    tty->session = 0;
    tty->pair = NULL;
    tty->priv = NULL;

    tty_buf_init(&tty->read_buf);
    tty_buf_init(&tty->write_buf);
    tty_buf_init(&tty->echo_buf);

    /* 设置名称 */
    switch (type) {
    case TTY_TYPE_CONSOLE:
        tty->minor = 0;
        snprintf(tty->name, TTY_NAME_MAX, "tty0");
        break;
    case TTY_TYPE_SERIAL:
        snprintf(tty->name, TTY_NAME_MAX, "ttyS%d", index);
        tty->minor = 64 + index;
        break;
    case TTY_TYPE_PTY_MASTER:
        snprintf(tty->name, TTY_NAME_MAX, "ptmx");
        tty->minor = 128 + index;
        break;
    case TTY_TYPE_PTY_SLAVE:
        snprintf(tty->name, TTY_NAME_MAX, "pts/%d", index);
        tty->minor = 128 + index;
        break;
    }

    int idx = index & 0xFF;
    if (!tty_table[idx]) {
        tty_table[idx] = tty;
        tty_count++;
    }

    return tty;
}

void tty_free(struct tty_struct *tty)
{
    if (!tty)
        return;

    int idx = tty->index & 0xFF;
    if (tty_table[idx] == tty) {
        tty_table[idx] = NULL;
        tty_count--;
    }

    kfree(tty);
}

/* ========== TTY 打开/关闭 ========== */

int tty_open(struct tty_struct *tty)
{
    if (!tty)
        return -ENODEV;
    if (tty->open)
        return 0;

    tty->open = 1;
    tty_buf_flush(&tty->read_buf);
    tty_buf_flush(&tty->write_buf);
    tty->canon_pos = 0;
    tty->stopped = 0;

    return 0;
}

int tty_close(struct tty_struct *tty)
{
    if (!tty || !tty->open)
        return 0;

    tty->open = 0;
    tty_buf_flush(&tty->read_buf);
    tty_buf_flush(&tty->write_buf);

    /* 如果配对的 PTY 也打开了，关闭它 */
    if (tty->pair && tty->pair->open) {
        tty->pair->open = 0;
        tty->pair->pair = NULL;
    }
    tty->pair = NULL;

    return 0;
}

/* ========== 线路规程：规范模式 ========== */

static int tty_canon_process(struct tty_struct *tty, uint8_t ch)
{
    struct termios *t = &tty->termios;

    /* 处理特殊控制字符 */
    if (ch == t->c_cc[VINTR]) {
        /* Ctrl+C - 中断 */
        /* TODO: 发送 SIGINT 到前台进程组 */
        return 0;
    }
    if (ch == t->c_cc[VSUSP]) {
        /* Ctrl+Z - 挂起 */
        /* TODO: 发送 SIGTSTP 到前台进程组 */
        return 0;
    }
    if (ch == t->c_cc[VSTART]) {
        /* Ctrl+Q - 继续输出 */
        tty->stopped = 0;
        return 0;
    }
    if (ch == t->c_cc[VSTOP]) {
        /* Ctrl+S - 停止输出 */
        tty->stopped = 1;
        return 0;
    }

    /* 退格 */
    if (ch == t->c_cc[VERASE]) {
        if (tty->canon_pos > 0) {
            tty->canon_pos--;
            /* 回显退格 */
            if (t->c_lflag & ECHO) {
                serial_print("\b \b");
            }
        }
        return 0;
    }

    /* 删除整行 */
    if (ch == t->c_cc[VKILL]) {
        if (t->c_lflag & ECHOE) {
            for (int i = 0; i < tty->canon_pos; i++) {
                serial_print("\b \b");
            }
        }
        tty->canon_pos = 0;
        return 0;
    }

    /* EOF */
    if (ch == t->c_cc[VEOF]) {
        if (tty->canon_pos == 0) {
            /* 单独 EOF 表示空输入 */
            tty_buf_put(&tty->read_buf, 4);  /* EOT char */
        }
        return 0;
    }

    /* 换行处理 */
    if (ch == '\n' || ch == '\r') {
        /* 添加行结束符 */
        for (int i = 0; i < tty->canon_pos; i++) {
            tty_buf_put(&tty->read_buf, tty->canon_line[i]);
        }
        tty_buf_put(&tty->read_buf, '\n');

        if (t->c_lflag & ECHO) {
            serial_putchar('\r');
            serial_putchar('\n');
        }

        tty->canon_pos = 0;
        return 0;
    }

    /* 正常字符 */
    if (tty->canon_pos < TTY_CANON_LINE_MAX - 1) {
        tty->canon_line[tty->canon_pos++] = ch;

        /* 回显 */
        if (t->c_lflag & ECHO) {
            serial_putchar(ch);
        }
    }

    return 0;
}

/* ========== 线路规程：原始模式 ========== */

static int tty_raw_process(struct tty_struct *tty, uint8_t ch)
{
    /* 原始模式：直接放入缓冲区 */
    if (tty_buf_put(&tty->read_buf, ch) < 0) {
        return -1;  /* 缓冲区满，丢弃 */
    }
    return 0;
}

/* ========== 字符输入处理 ========== */

int tty_insert_char(struct tty_struct *tty, uint8_t ch)
{
    if (!tty || !tty->open)
        return -ENODEV;

    /* CR -> NL 映射 */
    if (ch == '\r' && (tty->termios.c_iflag & ICRNL)) {
        ch = '\n';
    }

    /* 根据线路规程处理 */
    if (tty->ldisc == TTY_LDISC_CANON) {
        return tty_canon_process(tty, ch);
    } else {
        return tty_raw_process(tty, ch);
    }
}

int tty_insert_flip_string(struct tty_struct *tty, const uint8_t *chars, size_t count)
{
    int ret = 0;
    for (size_t i = 0; i < count; i++) {
        if (tty_insert_char(tty, chars[i]) < 0) {
            ret = -1;
        }
    }
    return ret;
}

void tty_flip_buffer_push(struct tty_struct *tty)
{
    /* 数据已经在 tty_insert_char 中放入 read_buf */
    (void)tty;
}

int tty_flush_buffer(struct tty_struct *tty, int queue)
{
    if (!tty)
        return -ENODEV;

    switch (queue) {
    case TCIFLUSH:
        tty_buf_flush(&tty->read_buf);
        if (tty->ldisc == TTY_LDISC_CANON)
            tty->canon_pos = 0;
        break;
    case TCOFLUSH:
        tty_buf_flush(&tty->write_buf);
        break;
    case TCIOFLUSH:
        tty_buf_flush(&tty->read_buf);
        tty_buf_flush(&tty->write_buf);
        if (tty->ldisc == TTY_LDISC_CANON)
            tty->canon_pos = 0;
        break;
    }
    return 0;
}

/* ========== TTY 读写 ========== */

ssize_t tty_read(struct tty_struct *tty, uint8_t *buf, size_t count)
{
    if (!tty || !tty->open)
        return -ENODEV;
    if (!buf || count == 0)
        return -EINVAL;

    size_t total = 0;
    struct termios *t = &tty->termios;

    if (tty->ldisc == TTY_LDISC_CANON) {
        /* 规范模式：按行读取 */
        while (total < count) {
            uint8_t ch;
            if (tty_buf_get(&tty->read_buf, &ch) < 0) {
                /* 缓冲区空 */
                if (total > 0)
                    return total;
                /* 阻塞等待：检查 VMIN/VTIME */
                if (t->c_cc[VMIN] == 0 && t->c_cc[VTIME] == 0) {
                    return -EAGAIN;  /* 非阻塞 */
                }
                /* TODO: 实现阻塞等待 */
                return -EAGAIN;
            }
            buf[total++] = ch;
            if (ch == '\n')
                break;  /* 行结束 */
        }
    } else {
        /* 原始模式：直接读取 */
        while (total < count) {
            uint8_t ch;
            if (tty_buf_get(&tty->read_buf, &ch) < 0) {
                if (total > 0)
                    return total;
                return -EAGAIN;
            }
            buf[total++] = ch;
        }
    }

    return total;
}

ssize_t tty_write(struct tty_struct *tty, const uint8_t *buf, size_t count)
{
    if (!tty || !tty->open)
        return -ENODEV;
    if (!buf || count == 0)
        return -EINVAL;

    struct termios *t = &tty->termios;
    size_t written = 0;

    for (size_t i = 0; i < count; i++) {
        if (tty->stopped) {
            break;
        }

        uint8_t ch = buf[i];

        /* 输出处理 */
        if (t->c_oflag & OPOST) {
            if (ch == '\n' && (t->c_oflag & ONLCR)) {
                /* NL -> CR NL 映射 */
                serial_putchar('\r');
                serial_putchar('\n');
            } else {
                serial_putchar(ch);
            }
        } else {
            serial_putchar(ch);
        }

        written++;
    }

    return written;
}

/* ========== TTY IOCTL ========== */

int tty_ioctl(struct tty_struct *tty, uint64_t cmd, uint64_t arg)
{
    if (!tty)
        return -ENODEV;

    switch (cmd) {
    case TCGETS: {
        struct termios *u = (struct termios *)arg;
        if (!u) return -EFAULT;
        /* TODO: copy_to_user */
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct termios *u = (struct termios *)arg;
        if (!u) return -EFAULT;
        /* TODO: copy_from_user */
        return 0;
    }
    case TIOCGWINSZ: {
        struct winsize *ws = (struct winsize *)arg;
        if (!ws) return -EFAULT;
        /* TODO: copy_to_user */
        return 0;
    }
    case TIOCSWINSZ: {
        struct winsize *ws = (struct winsize *)arg;
        if (!ws) return -EFAULT;
        /* TODO: copy_from_user */
        return 0;
    }
    case TIOCINQ: {
        int *count = (int *)arg;
        if (!count) return -EFAULT;
        /* TODO: copy_to_user */
        return 0;
    }
    case TCFLSH:
        return tty_flush_buffer(tty, (int)arg);
    case TIOCGPGRP:
    case TIOCSPGRP:
    case TIOCSCTTY:
    case TIOCNOTTY:
        return 0;  /* 简化实现 */
    default:
        serial_print("[TTY] Unknown ioctl: 0x");
        return -ENOTTY;
    }
}

/* ========== PTY 实现 ========== */

static struct tty_struct *pty_masters[PTY_MAX];
static struct tty_struct *pty_slaves[PTY_MAX];
static int pty_alloc_map[(PTY_MAX + 31) / 32];

static int pty_find_free_index(void)
{
    for (int i = 0; i < PTY_MAX; i++) {
        int word = i / 32;
        int bit = i % 32;
        if (!(pty_alloc_map[word] & (1 << bit))) {
            pty_alloc_map[word] |= (1 << bit);
            return i;
        }
    }
    return -1;
}

int pty_alloc_pair(struct tty_struct **master, struct tty_struct **slave)
{
    int idx = pty_find_free_index();
    if (idx < 0)
        return -ENOSPC;

    *master = tty_alloc(idx, TTY_TYPE_PTY_MASTER);
    *slave = tty_alloc(idx, TTY_TYPE_PTY_SLAVE);

    if (!*master || !*slave) {
        if (*master) tty_free(*master);
        if (*slave) tty_free(*slave);
        *master = NULL;
        *slave = NULL;
        return -ENOMEM;
    }

    /* 配对 */
    (*master)->pair = *slave;
    (*slave)->pair = *master;

    pty_masters[idx] = *master;
    pty_slaves[idx] = *slave;

    /* PTY 使用原始模式 */
    (*master)->ldisc = TTY_LDISC_RAW;
    (*slave)->ldisc = TTY_LDISC_RAW;

    return 0;
}

int pty_free_pair(struct tty_struct *master)
{
    if (!master)
        return -EINVAL;

    int idx = master->index;
    if (idx < 0 || idx >= PTY_MAX)
        return -EINVAL;

    int word = idx / 32;
    int bit = idx % 32;
    pty_alloc_map[word] &= ~(1 << bit);

    if (master->pair) {
        master->pair->pair = NULL;
        tty_free(master->pair);
    }
    master->pair = NULL;

    tty_free(master);
    pty_masters[idx] = NULL;
    pty_slaves[idx] = NULL;

    return 0;
}

struct tty_struct *pty_get_by_index(int index, int is_master)
{
    if (index < 0 || index >= PTY_MAX)
        return NULL;

    return is_master ? pty_masters[index] : pty_slaves[index];
}

/* ========== 串口 TTY 驱动 ========== */

static struct tty_struct *serial_tty = NULL;

static int serial_tty_open(struct tty_struct *tty)
{
    (void)tty;
    serial_print("[TTY] Serial TTY opened\n");
    return 0;
}

static int serial_tty_close(struct tty_struct *tty)
{
    (void)tty;
    serial_print("[TTY] Serial TTY closed\n");
    return 0;
}

static ssize_t serial_tty_read(struct tty_struct *tty, uint8_t *buf, size_t count)
{
    /* 串口读取：从输入缓冲区读取数据 */
    (void)tty;
    size_t read_count = 0;

    while (read_count < count) {
        int c = serial_getchar();
        if (c < 0) break;  /* 无输入 */

        uint8_t ch = (uint8_t)c;

        /* 处理回车 -> 换行 */
        if (ch == '\r') {
            ch = '\n';
        }

        /* 回显 */
        if (tty->termios.c_lflag & ECHO) {
            serial_putchar(ch);
        }

        buf[read_count++] = ch;

        /* 规范模式：遇到换行返回 */
        if (ch == '\n' && (tty->termios.c_lflag & ICANON)) {
            break;
        }
    }

    return read_count > 0 ? (ssize_t)read_count : -EAGAIN;
}

static ssize_t serial_tty_write(struct tty_struct *tty, const uint8_t *buf, size_t count)
{
    /* 使用串口输出 */
    (void)tty;
    for (size_t i = 0; i < count; i++) {
        serial_putchar(buf[i]);
    }
    return count;
}

int serial_tty_init(void)
{
    serial_tty = tty_alloc(0, TTY_TYPE_SERIAL);
    if (!serial_tty)
        return -ENOMEM;

    tty_open(serial_tty);
    serial_tty->ldisc = TTY_LDISC_CANON;

    /* 设置默认窗口大小 */
    serial_tty->winsize.ws_row = 24;
    serial_tty->winsize.ws_col = 80;

    serial_print("[TTY] Serial TTY initialized (ttyS0)\n");
    return 0;
}

struct tty_struct *serial_tty_get(void)
{
    return serial_tty;
}

/* ========== 控制台 TTY 驱动 ========== */

static struct tty_struct *console_tty = NULL;

int console_tty_init(void)
{
    console_tty = tty_alloc(0, TTY_TYPE_CONSOLE);
    if (!console_tty)
        return -ENOMEM;

    tty_open(console_tty);
    console_tty->ldisc = TTY_LDISC_CANON;

    console_tty->winsize.ws_row = 24;
    console_tty->winsize.ws_col = 80;

    serial_print("[TTY] Console TTY initialized (tty0)\n");
    return 0;
}

struct tty_struct *console_tty_get(void)
{
    return console_tty;
}

/* ========== PTY 初始化 ========== */

int pty_init(void)
{
    for (int i = 0; i < PTY_MAX; i++) {
        pty_masters[i] = NULL;
        pty_slaves[i] = NULL;
    }
    for (int i = 0; i < (PTY_MAX + 31) / 32; i++) {
        pty_alloc_map[i] = 0;
    }

    serial_print("[TTY] PTY subsystem initialized (max ");
    /* 打印数字 */
    {
        char buf[8];
        int n = PTY_MAX, pos = 0;
        if (n == 0) buf[pos++] = '0';
        else {
            char tmp[8]; int t = 0;
            while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
            while (t > 0) buf[pos++] = tmp[--t];
        }
        buf[pos] = '\0';
        serial_print(buf);
    }
    serial_print(" ptys)\n");
    return 0;
}

/* ========== 查找 TTY ========== */

struct tty_struct *tty_get_by_index(int index, enum tty_type type)
{
    int idx = index & 0xFF;
    struct tty_struct *tty = tty_table[idx];
    if (tty && tty->type == type)
        return tty;
    return NULL;
}

/* ========== TTY 子系统初始化 ========== */

int tty_init(void)
{
    for (int i = 0; i < 256; i++)
        tty_table[i] = NULL;
    tty_count = 0;

    /* 初始化控制台 TTY */
    console_tty_init();

    /* 初始化串口 TTY */
    serial_tty_init();

    /* 初始化 PTY */
    pty_init();

    serial_print("[TTY] TTY subsystem initialized\n");
    return 0;
}
