/*
 * Zratro OS - 16550 UART 驱动
 *
 * 功能: 多端口支持 (COM1-COM4), 中断驱动 TX/RX, TTY 接口集成
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "idt.h"

/* ========== UART 端口基址 ========== */
#define UART_COM1_BASE      0x3F8
#define UART_COM2_BASE      0x2F8
#define UART_COM3_BASE      0x3E8
#define UART_COM4_BASE      0x2E8

#define UART_MAX_PORTS      4

/* ========== UART 寄存器偏移 ========== */
#define UART_THR            0x00   /* 发送保持寄存器 (写) / 接收缓冲寄存器 (读) */
#define UART_RBR            0x00   /* 接收缓冲寄存器 (读) */
#define UART_IER            0x01   /* 中断使能寄存器 */
#define UART_IIR            0x02   /* 中断标识寄存器 (读) */
#define UART_FCR            0x02   /* FIFO 控制寄存器 (写) */
#define UART_LCR            0x03   /* 线路控制寄存器 */
#define UART_MCR            0x04   /* Modem 控制寄存器 */
#define UART_LSR            0x05   /* 线路状态寄存器 */
#define UART_MSR            0x06   /* Modem 状态寄存器 */
#define UART_SCR            0x07   /* 暂存寄存器 */

/* ========== IER 位 ========== */
#define UART_IER_RX_ENABLE      0x01   /* 接收数据中断使能 */
#define UART_IER_TX_ENABLE      0x02   /* 发送保持寄存器空中断使能 */
#define UART_IER_LINE_STATUS    0x04   /* 线路状态中断使能 */
#define UART_IER_MODEM_STATUS   0x08   /* Modem 状态中断使能 */

/* ========== IIR 位 ========== */
#define UART_IIR_NO_INTERRUPT   0x01   /* 无中断挂起 */
#define UART_IIR_ID_MASK        0x0E   /* 中断 ID 掩码 */
#define UART_IIR_MODEM_STATUS   0x00   /* Modem 状态 */
#define UART_IIR_TX_EMPTY       0x02   /* 发送保持寄存器空 */
#define UART_IIR_RX_READY       0x04   /* 接收数据就绪 */
#define UART_IIR_LINE_STATUS    0x06   /* 线路状态 */
#define UART_IIR_TIMEOUT        0x0C   /* 字符超时 */

/* ========== FCR 位 ========== */
#define UART_FCR_ENABLE_FIFO    0x01   /* 使能 FIFO */
#define UART_FCR_CLEAR_RX       0x02   /* 清空接收 FIFO */
#define UART_FCR_CLEAR_TX       0x04   /* 清空发送 FIFO */
#define UART_FCR_DMA_MODE       0x08   /* DMA 模式 */
#define UART_FCR_TRIGGER_1      0x00   /* FIFO 触发: 1 字节 */
#define UART_FCR_TRIGGER_4      0x40   /* FIFO 触发: 4 字节 */
#define UART_FCR_TRIGGER_8      0x80   /* FIFO 触发: 8 字节 */
#define UART_FCR_TRIGGER_14     0xC0   /* FIFO 触发: 14 字节 */

/* ========== LCR 位 ========== */
#define UART_LCR_WORD_5BIT      0x00   /* 5 数据位 */
#define UART_LCR_WORD_6BIT      0x01   /* 6 数据位 */
#define UART_LCR_WORD_7BIT      0x02   /* 7 数据位 */
#define UART_LCR_WORD_8BIT      0x03   /* 8 数据位 */
#define UART_LCR_STOP_1BIT      0x00   /* 1 停止位 */
#define UART_LCR_STOP_2BIT      0x04   /* 2 停止位 */
#define UART_LCR_PARITY_NONE    0x00   /* 无校验 */
#define UART_LCR_PARITY_ODD     0x08   /* 奇校验 */
#define UART_LCR_PARITY_EVEN    0x18   /* 偶校验 */
#define UART_LCR_PARITY_MARK    0x28   /* 标记校验 */
#define UART_LCR_PARITY_SPACE   0x38   /* 空格校验 */
#define UART_LCR_DLAB           0x80   /* 分频锁存访问位 */

/* ========== MCR 位 ========== */
#define UART_MCR_DTR            0x01   /* 数据终端就绪 */
#define UART_MCR_RTS            0x02   /* 请求发送 */
#define UART_MCR_OUT1           0x04   /* 输出 1 */
#define UART_MCR_OUT2           0x08   /* 输出 2 (中断使能) */
#define UART_MCR_LOOPBACK       0x10   /* 回环模式 */

/* ========== LSR 位 ========== */
#define UART_LSR_RX_READY       0x01   /* 接收数据就绪 */
#define UART_LSR_OVERRUN        0x02   /* 溢出错误 */
#define UART_LSR_PARITY_ERR     0x04   /* 校验错误 */
#define UART_LSR_FRAME_ERR      0x08   /* 帧错误 */
#define UART_LSR_BREAK_IND      0x10   /* 中断指示 */
#define UART_LSR_TX_EMPTY       0x20   /* 发送保持寄存器空 */
#define UART_LSR_TX_IDLE        0x40   /* 发送器空闲 */
#define UART_LSR_FIFO_ERR       0x80   /* FIFO 错误 */

/* ========== 波特率分频值 ========== */
#define UART_BAUD_115200    1
#define UART_BAUD_57600     2
#define UART_BAUD_38400     3
#define UART_BAUD_19200     6
#define UART_BAUD_9600      12
#define UART_BAUD_4800      24
#define UART_BAUD_2400      48
#define UART_BAUD_1200      96

/* ========== 环形缓冲区大小 ========== */
#define UART_RX_BUF_SIZE    4096
#define UART_TX_BUF_SIZE    4096

/* ========== UART 端口结构 ========== */
struct uart_ring_buf {
    uint8_t buf[UART_RX_BUF_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
    spinlock_t lock;
};

struct uart_port {
    uint16_t io_base;               /* I/O 基址 */
    uint8_t irq;                    /* IRQ 号 */
    uint8_t index;                  /* 端口索引 (0-3) */
    int initialized;                /* 是否已初始化 */

    /* 配置 */
    uint32_t baud_rate;             /* 波特率 */
    uint8_t data_bits;              /* 数据位 (5-8) */
    uint8_t stop_bits;              /* 停止位 (1-2) */
    uint8_t parity;                 /* 校验 (0=无, 1=奇, 2=偶) */

    /* 缓冲区 */
    struct uart_ring_buf rx_buf;    /* 接收缓冲区 */
    struct uart_ring_buf tx_buf;    /* 发送缓冲区 */

    /* 状态 */
    int tx_active;                  /* 是否正在发送 */
    spinlock_t lock;                /* 端口锁 */

    /* 统计 */
    uint64_t rx_count;
    uint64_t tx_count;
    uint64_t rx_errors;
    uint64_t tx_errors;
};

/* ========== API 函数声明 ========== */

/* 初始化 UART 子系统 */
int uart_init(void);

/* 初始化指定端口 */
int uart_port_init(int port_index, uint32_t baud_rate);

/* 发送/接收 */
int uart_putc(int port_index, char c);
int uart_getc(int port_index);
int uart_write(int port_index, const void *buf, size_t len);
int uart_read(int port_index, void *buf, size_t len);

/* 非阻塞读取 */
int uart_read_nonblock(int port_index, void *buf, size_t len);

/* 检查数据是否可读 */
int uart_data_available(int port_index);

/* 配置 */
int uart_set_baud_rate(int port_index, uint32_t baud_rate);
int uart_set_format(int port_index, uint8_t data_bits, uint8_t stop_bits, uint8_t parity);

/* TTY 接口 */
int uart_tty_register(int port_index);

/* 中断处理 */
void uart_com1_interrupt_handler(struct interrupt_frame *frame);
void uart_com2_interrupt_handler(struct interrupt_frame *frame);

/* 获取端口实例 */
struct uart_port *uart_get_port(int port_index);

#endif /* UART_H */
