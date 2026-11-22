/*
 * Zratro OS - TTY Subsystem
 *
 * 实现终端子系统，支持：
 * - /dev/tty0 (虚拟控制台)
 * - /dev/ttyS0 (串口终端)
 * - PTY (伪终端) 对，用于交互式 shell
 */

#ifndef TTY_H
#define TTY_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== termios 常量 ========== */

/* 输入模式 (c_iflag) */
#define IGNBRK  0x0001
#define BRKINT  0x0002
#define IGNPAR  0x0004
#define PARMRK  0x0008
#define INPCK   0x0010
#define ISTRIP  0x0020
#define INLCR   0x0040
#define IGNCR   0x0080
#define ICRNL   0x0100
#define IUCLC   0x0200
#define IXON    0x0400
#define IXANY   0x0800
#define IXOFF   0x1000

/* 输出模式 (c_oflag) */
#define OPOST   0x0001
#define OLCUC   0x0002
#define ONLCR   0x0004
#define OCRNL   0x0008
#define ONOCR   0x0010
#define ONLRET  0x0020
#define OFILL   0x0040
#define OFDEL   0x0080

/* 控制模式 (c_cflag) */
#define CBAUD   0x100F
#define B0      0x0000
#define B50     0x0001
#define B110    0x0003
#define B1200   0x0009
#define B2400   0x000A
#define B4800   0x000B
#define B9600   0x000D
#define B19200  0x000E
#define B38400  0x000F
#define CSIZE   0x0030
#define CS5     0x0000
#define CS6     0x0010
#define CS7     0x0020
#define CS8     0x0030
#define CSTOPB  0x0040
#define CREAD   0x0080
#define PARENB  0x0100
#define PARODD  0x0200
#define CLOCAL  0x0800

/* 本地模式 (c_lflag) */
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define NOFLSH  0x0080
#define TOSTOP  0x0100
#define IEXTEN  0x8000

/* 控制字符索引 */
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSWTC   7
#define VSTART  8
#define VSTOP   9
#define VSUSP   10
#define VEOL    11
#define VREPRINT 12
#define VWERASE 13
#define VLNEXT  14
#define VEOL2   15
#define NCCS    32

/* TC 操作 */
#define TCIFLUSH    0
#define TCOFLUSH    1
#define TCIOFLUSH   2
#define TCOOFF      3
#define TCOON       4
#define TCIOFF      5
#define TCION       6

#define TCSANOW     0
#define TCSADRAIN   1
#define TCSAFLUSH   2

/* PTY 相关 */
#define PTY_MAX     64
#define PTY_MAGIC   0x50545900  /* "PTY" */

/* IOCTL 命令 */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TCGETA      0x5405
#define TCSETA      0x5406
#define TCSETAW     0x5407
#define TCSETAF     0x5408
#define TCSBRK      0x5409
#define TCXONC      0x540A
#define TCFLSH      0x540B
#define TIOCEXCL    0x540C
#define TIOCNXCL    0x540D
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCOUTQ    0x5411
#define TIOCSTI     0x5412
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCMGET    0x5415
#define TIOCMBIS    0x5416
#define TIOCMBIC    0x5417
#define TIOCMSET    0x5418
#define TIOCGSOFTCAR 0x5419
#define TIOCSSOFTCAR 0x541A
#define TIOCINQ     0x541B
#define FIONREAD    0x541B  /* 与 TIOCINQ 相同 */
#define TIOCLINUX   0x541C
#define TIOCCONS    0x541D
#define TIOCGSERIAL 0x541E
#define TIOCSSERIAL 0x541F
#define TIOCPKT     0x5420
#define FIONBIO     0x5421
#define TIOCNOTTY   0x5422
#define TIOCSETD    0x5423
#define TIOCGETD    0x5424

/* Winsize 结构 */
struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* termios 结构 */
struct termios {
    uint32_t c_iflag;     /* 输入模式 */
    uint32_t c_oflag;     /* 输出模式 */
    uint32_t c_cflag;     /* 控制模式 */
    uint32_t c_lflag;     /* 本地模式 */
    uint8_t c_line;       /* 线路规程 */
    uint8_t c_cc[NCCS];   /* 控制字符 */
    uint32_t c_ispeed;    /* 输入速度 */
    uint32_t c_ospeed;    /* 输出速度 */
};

/* termio 结构 (兼容旧版) */
struct termio {
    uint16_t c_iflag;
    uint16_t c_oflag;
    uint16_t c_cflag;
    uint16_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[NCCS];
};

/* ========== TTY 缓冲区 ========== */

#define TTY_BUFFER_SIZE  4096

struct tty_buffer {
    uint8_t data[TTY_BUFFER_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t count;
};

/* ========== TTY 线路规程 ========== */

#define TTY_LDISC_RAW    0
#define TTY_LDISC_CANON  1

/* 规范模式行缓冲区 */
#define TTY_CANON_LINE_MAX  256

/* ========== TTY 结构 ========== */

#define TTY_NAME_MAX  16

enum tty_type {
    TTY_TYPE_CONSOLE,   /* /dev/tty0 */
    TTY_TYPE_SERIAL,    /* /dev/ttyS0 */
    TTY_TYPE_PTY_MASTER,/* PTY master */
    TTY_TYPE_PTY_SLAVE, /* PTY slave */
};

struct tty_struct {
    int index;                  /* TTY 索引 */
    enum tty_type type;         /* TTY 类型 */
    char name[TTY_NAME_MAX];    /* 设备名称 */
    int major, minor;           /* 设备号 */

    /* 线路规程 */
    int ldisc;                  /* 当前线路规程 */

    /* termios 设置 */
    struct termios termios;

    /* 缓冲区 */
    struct tty_buffer read_buf;   /* 输入缓冲区 */
    struct tty_buffer write_buf;  /* 输出缓冲区 */
    struct tty_buffer echo_buf;   /* 回显缓冲区 */

    /* 规范模式状态 */
    char canon_line[TTY_CANON_LINE_MAX];
    int canon_pos;

    /* 窗口大小 */
    struct winsize winsize;

    /* 进程组 */
    uint32_t pgrp;              /* 前台进程组 */
    uint32_t session;           /* 会话 ID */

    /* 标志 */
    volatile int open;          /* 是否打开 */
    volatile int stopped;       /* 输出停止 */
    volatile int packet_mode;   /* 包模式 (PTY) */

    /* 等待队列 */
    volatile int read_wait;
    volatile int write_wait;

    /* PTY 配对 */
    struct tty_struct *pair;    /* 配对的 PTY (master<->slave) */

    /* 私有数据 */
    void *priv;
};

/* ========== TTY 驱动 ========== */

struct tty_driver {
    int major;                  /* 主设备号 */
    int minor_start;            /* 起始次设备号 */
    int num;                    /* 设备数量 */
    const char *name;           /* 设备名称前缀 */

    /* 操作 */
    int (*open)(struct tty_struct *tty);
    int (*close)(struct tty_struct *tty);
    ssize_t (*read)(struct tty_struct *tty, uint8_t *buf, size_t count);
    ssize_t (*write)(struct tty_struct *tty, const uint8_t *buf, size_t count);
    int (*ioctl)(struct tty_struct *tty, uint64_t cmd, uint64_t arg);
    int (*write_room)(struct tty_struct *tty);
};

/* ========== 核心 TTY API ========== */

/* 初始化 TTY 子系统 */
int tty_init(void);

/* 注册 TTY 驱动 */
int tty_register_driver(struct tty_driver *drv);

/* 分配/释放 TTY */
struct tty_struct *tty_alloc(int index, enum tty_type type);
void tty_free(struct tty_struct *tty);

/* 打开/关闭 TTY */
int tty_open(struct tty_struct *tty);
int tty_close(struct tty_struct *tty);

/* TTY 读写 */
ssize_t tty_read(struct tty_struct *tty, uint8_t *buf, size_t count);
ssize_t tty_write(struct tty_struct *tty, const uint8_t *buf, size_t count);

/* TTY IOCTL */
int tty_ioctl(struct tty_struct *tty, uint64_t cmd, uint64_t arg);

/* 字符输入处理 (从驱动调用) */
int tty_insert_char(struct tty_struct *tty, uint8_t ch);
int tty_insert_flip_string(struct tty_struct *tty, const uint8_t *chars, size_t count);

/* 刷新缓冲区 */
void tty_flip_buffer_push(struct tty_struct *tty);
int tty_flush_buffer(struct tty_struct *tty, int queue);

/* 查找 TTY */
struct tty_struct *tty_get_by_index(int index, enum tty_type type);

/* ========== 串口 TTY 驱动 ========== */

int serial_tty_init(void);
struct tty_struct *serial_tty_get(void);

/* ========== 控制台 TTY 驱动 ========== */

int console_tty_init(void);
struct tty_struct *console_tty_get(void);

/* ========== PTY 驱动 ========== */

int pty_init(void);
int pty_alloc_pair(struct tty_struct **master, struct tty_struct **slave);
int pty_free_pair(struct tty_struct *master);
struct tty_struct *pty_get_by_index(int index, int is_master);

/* ========== 线路规程 ========== */

int tty_ldisc_receive(struct tty_struct *tty, uint8_t ch);
void tty_ldisc_flush(struct tty_struct *tty);

#endif /* TTY_H */
