/*
 * Zratro OS - 16550 UART 驱动
 *
 * 功能: 多端口支持 (COM1-COM4), 中断驱动 TX/RX, TTY 接口集成
 */

#include "uart.h"
#include "console.h"
#include "idt.h"
#include "apic.h"
#include "string.h"

/* ========== I/O 端口访问 ========== */

static inline void uart_outb(struct uart_port *port, uint8_t reg, uint8_t val) {
    uint16_t port_addr = port->io_base + reg;
    asm volatile("outb %0, %w1" : : "a"(val), "Nd"(port_addr));
}

static inline uint8_t uart_inb(struct uart_port *port, uint8_t reg) {
    uint8_t val;
    uint16_t port_addr = port->io_base + reg;
    asm volatile("inb %w1, %0" : "=a"(val) : "Nd"(port_addr));
    return val;
}

/* ========== 全局端口实例 ========== */

static struct uart_port uart_ports[UART_MAX_PORTS];
static const uint16_t uart_base_addrs[UART_MAX_PORTS] = {
    UART_COM1_BASE, UART_COM2_BASE, UART_COM3_BASE, UART_COM4_BASE
};
static const uint8_t uart_irqs[UART_MAX_PORTS] = { 4, 3, 4, 3 };

/* ========== 环形缓冲区操作 ========== */

static void uart_ring_buf_init(struct uart_ring_buf *rb) {
    rb->head = 0;
    rb->tail = 0;
    spinlock_init(&rb->lock);
}

static int uart_ring_buf_write(struct uart_ring_buf *rb, uint8_t data) {
    unsigned long flags = spin_lock_irqsave(&rb->lock);
    uint32_t next_head = (rb->head + 1) % UART_RX_BUF_SIZE;
    if (next_head == rb->tail) {
        /* 缓冲区满 */
        spin_unlock_irqrestore(&rb->lock, flags);
        return -1;
    }
    rb->buf[rb->head] = data;
    rb->head = next_head;
    spin_unlock_irqrestore(&rb->lock, flags);
    return 0;
}

static int uart_ring_buf_read(struct uart_ring_buf *rb, uint8_t *data) {
    unsigned long flags = spin_lock_irqsave(&rb->lock);
    if (rb->tail == rb->head) {
        /* 缓冲区空 */
        spin_unlock_irqrestore(&rb->lock, flags);
        return -1;
    }
    *data = rb->buf[rb->tail];
    rb->tail = (rb->tail + 1) % UART_RX_BUF_SIZE;
    spin_unlock_irqrestore(&rb->lock, flags);
    return 0;
}

static int uart_ring_buf_available(struct uart_ring_buf *rb) {
    int count = (int)(rb->head) - (int)(rb->tail);
    if (count < 0) count += UART_RX_BUF_SIZE;
    return count;
}

/* ========== 端口中断处理 ========== */

static void uart_handle_interrupt(struct uart_port *port) {
    if (!port->initialized) return;

    while (1) {
        uint8_t iir = uart_inb(port, UART_IIR);
        if (iir & UART_IIR_NO_INTERRUPT) break;

        uint8_t int_id = iir & UART_IIR_ID_MASK;

        switch (int_id) {
        case UART_IIR_RX_READY:
        case UART_IIR_TIMEOUT: {
            /* 接收数据就绪 - 读取所有可用数据 */
            while (uart_inb(port, UART_LSR) & UART_LSR_RX_READY) {
                uint8_t data = uart_inb(port, UART_RBR);
                uart_ring_buf_write(&port->rx_buf, data);
                port->rx_count++;
            }
            break;
        }

        case UART_IIR_TX_EMPTY: {
            /* 发送保持寄存器空 - 继续发送缓冲区中的数据 */
            port->tx_active = 0;
            uint8_t data;
            while (uart_ring_buf_read(&port->tx_buf, &data) == 0) {
                if (!(uart_inb(port, UART_LSR) & UART_LSR_TX_EMPTY)) {
                    /* 发送 FIFO 满，启用 TX 中断等待 */
                    uart_outb(port, UART_IER,
                              UART_IER_RX_ENABLE | UART_IER_TX_ENABLE);
                    port->tx_active = 1;
                    break;
                }
                uart_outb(port, UART_THR, data);
                port->tx_count++;
            }

            if (!port->tx_active) {
                /* 无更多数据发送，禁用 TX 中断 */
                uart_outb(port, UART_IER, UART_IER_RX_ENABLE);
            }
            break;
        }

        case UART_IIR_LINE_STATUS: {
            /* 线路状态错误 */
            uint8_t lsr = uart_inb(port, UART_LSR);
            if (lsr & (UART_LSR_OVERRUN | UART_LSR_PARITY_ERR |
                       UART_LSR_FRAME_ERR | UART_LSR_BREAK_IND)) {
                port->rx_errors++;
            }
            break;
        }

        case UART_IIR_MODEM_STATUS: {
            /* Modem 状态变化 - 读取 MSR 清除 */
            (void)uart_inb(port, UART_MSR);
            break;
        }

        default:
            break;
        }
    }
}

void uart_com1_interrupt_handler(struct interrupt_frame *frame) {
    (void)frame;
    uart_handle_interrupt(&uart_ports[0]);
    apic_eoi();
}

void uart_com2_interrupt_handler(struct interrupt_frame *frame) {
    (void)frame;
    uart_handle_interrupt(&uart_ports[1]);
    apic_eoi();
}

/* ========== 端口初始化 ========== */

int uart_port_init(int port_index, uint32_t baud_rate) {
    if (port_index < 0 || port_index >= UART_MAX_PORTS) return Z_ERR_RANGE;

    struct uart_port *port = &uart_ports[port_index];
    port->io_base = uart_base_addrs[port_index];
    port->irq = uart_irqs[port_index];
    port->index = port_index;
    port->baud_rate = baud_rate;
    port->data_bits = 8;
    port->stop_bits = 1;
    port->parity = 0;

    /* 初始化缓冲区 */
    uart_ring_buf_init(&port->rx_buf);
    uart_ring_buf_init(&port->tx_buf);
    spinlock_init(&port->lock);
    port->tx_active = 0;

    /* 禁用所有中断 */
    uart_outb(port, UART_IER, 0x00);

    /* 设置 DLAB 以访问分频寄存器 */
    uart_outb(port, UART_LCR, UART_LCR_DLAB);

    /* 设置波特率分频值 */
    uint16_t divisor = 115200 / baud_rate;
    uart_outb(port, UART_THR, divisor & 0xFF);        /* 低字节 */
    uart_outb(port, UART_IER, (divisor >> 8) & 0xFF); /* 高字节 */

    /* 设置 8N1 格式, 清除 DLAB */
    uart_outb(port, UART_LCR, UART_LCR_WORD_8BIT | UART_LCR_STOP_1BIT | UART_LCR_PARITY_NONE);

    /* 启用 FIFO, 清空缓冲区, 14 字节触发 */
    uart_outb(port, UART_FCR,
              UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_RX | UART_FCR_CLEAR_TX | UART_FCR_TRIGGER_14);

    /* 设置 MCR: DTR + RTS + OUT2 (中断使能) */
    uart_outb(port, UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);

    /* 启用接收数据中断 */
    uart_outb(port, UART_IER, UART_IER_RX_ENABLE);

    /* 注册中断处理 */
    if (port_index == 0) {
        isr_register_handler(IRQ_BASE + port->irq, uart_com1_interrupt_handler);
    } else if (port_index == 1) {
        isr_register_handler(IRQ_BASE + port->irq, uart_com2_interrupt_handler);
    }

    /* 测试: 回环模式 */
    uart_outb(port, UART_MCR, UART_MCR_LOOPBACK | UART_MCR_OUT2);
    uart_outb(port, UART_THR, 0xAE);
    uint8_t test = uart_inb(port, UART_RBR);
    if (test != 0xAE) {
        printk("uart: COM%d 回环测试失败 (发送=0xAE 接收=0x%02X)", port_index + 1, test);
    }

    /* 恢复正常模式 */
    uart_outb(port, UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);

    port->initialized = 1;
    printk("uart: COM%d 初始化完成 (基址=0x%04X 波特率=%u IRQ=%u)",
           port_index + 1, port->io_base, baud_rate, port->irq);
    return 0;
}

/* ========== 发送/接收 ========== */

int uart_putc(int port_index, char c) {
    if (port_index < 0 || port_index >= UART_MAX_PORTS) return Z_ERR_RANGE;
    struct uart_port *port = &uart_ports[port_index];
    if (!port->initialized) return Z_ERR_NOTSUP;

    unsigned long flags = spin_lock_irqsave(&port->lock);

    /* 如果发送缓冲区空且 TX 空闲，直接发送 */
    if (!port->tx_active && uart_ring_buf_available(&port->tx_buf) == 0) {
        /* 等待发送保持寄存器空 */
        int timeout = 10000;
        while (!(uart_inb(port, UART_LSR) & UART_LSR_TX_EMPTY) && timeout > 0) {
            timeout--;
        }
        if (timeout > 0) {
            uart_outb(port, UART_THR, (uint8_t)c);
            port->tx_count++;
            spin_unlock_irqrestore(&port->lock, flags);
            return 0;
        }
    }

    /* 放入发送缓冲区 */
    if (uart_ring_buf_write(&port->tx_buf, (uint8_t)c) != 0) {
        port->tx_errors++;
        spin_unlock_irqrestore(&port->lock, flags);
        return Z_ERR_BUSY;
    }

    /* 启用 TX 中断以开始发送 */
    if (!port->tx_active) {
        uart_outb(port, UART_IER, UART_IER_RX_ENABLE | UART_IER_TX_ENABLE);
        port->tx_active = 1;
    }

    spin_unlock_irqrestore(&port->lock, flags);
    return 0;
}

int uart_getc(int port_index) {
    if (port_index < 0 || port_index >= UART_MAX_PORTS) return Z_ERR_RANGE;
    struct uart_port *port = &uart_ports[port_index];
    if (!port->initialized) return Z_ERR_NOTSUP;

    uint8_t data;
    if (uart_ring_buf_read(&port->rx_buf, &data) != 0) {
        return -1;  /* 无数据 */
    }
    return (int)data;
}

int uart_write(int port_index, const void *buf, size_t len) {
    if (buf == NULL) return Z_ERR_NULL;
    const uint8_t *data = (const uint8_t *)buf;
    int written = 0;
    for (size_t i = 0; i < len; i++) {
        if (uart_putc(port_index, (char)data[i]) == 0) {
            written++;
        } else {
            break;
        }
    }
    return written;
}

int uart_read(int port_index, void *buf, size_t len) {
    if (buf == NULL) return Z_ERR_NULL;
    uint8_t *data = (uint8_t *)buf;
    int read_count = 0;
    for (size_t i = 0; i < len; i++) {
        int c = uart_getc(port_index);
        if (c < 0) break;
        data[i] = (uint8_t)c;
        read_count++;
    }
    return read_count;
}

int uart_read_nonblock(int port_index, void *buf, size_t len) {
    return uart_read(port_index, buf, len);
}

int uart_data_available(int port_index) {
    if (port_index < 0 || port_index >= UART_MAX_PORTS) return 0;
    return uart_ring_buf_available(&uart_ports[port_index].rx_buf);
}

/* ========== 配置 ========== */

int uart_set_baud_rate(int port_index, uint32_t baud_rate) {
    if (port_index < 0 || port_index >= UART_MAX_PORTS) return Z_ERR_RANGE;
    struct uart_port *port = &uart_ports[port_index];
    if (!port->initialized) return Z_ERR_NOTSUP;

    unsigned long flags = spin_lock_irqsave(&port->lock);

    uint16_t divisor = 115200 / baud_rate;
    uint8_t lcr = uart_inb(port, UART_LCR);

    /* 设置 DLAB */
    uart_outb(port, UART_LCR, lcr | UART_LCR_DLAB);
    uart_outb(port, UART_THR, divisor & 0xFF);
    uart_outb(port, UART_IER, (divisor >> 8) & 0xFF);
    uart_outb(port, UART_LCR, lcr);  /* 恢复 LCR */

    port->baud_rate = baud_rate;
    spin_unlock_irqrestore(&port->lock, flags);
    return 0;
}

int uart_set_format(int port_index, uint8_t data_bits, uint8_t stop_bits, uint8_t parity) {
    if (port_index < 0 || port_index >= UART_MAX_PORTS) return Z_ERR_RANGE;
    struct uart_port *port = &uart_ports[port_index];
    if (!port->initialized) return Z_ERR_NOTSUP;

    unsigned long flags = spin_lock_irqsave(&port->lock);

    uint8_t lcr = 0;
    switch (data_bits) {
    case 5: lcr |= UART_LCR_WORD_5BIT; break;
    case 6: lcr |= UART_LCR_WORD_6BIT; break;
    case 7: lcr |= UART_LCR_WORD_7BIT; break;
    case 8: default: lcr |= UART_LCR_WORD_8BIT; break;
    }
    if (stop_bits == 2) lcr |= UART_LCR_STOP_2BIT;
    switch (parity) {
    case 1: lcr |= UART_LCR_PARITY_ODD; break;
    case 2: lcr |= UART_LCR_PARITY_EVEN; break;
    default: break;  /* 无校验 */
    }

    uart_outb(port, UART_LCR, lcr);

    port->data_bits = data_bits;
    port->stop_bits = stop_bits;
    port->parity = parity;
    spin_unlock_irqrestore(&port->lock, flags);
    return 0;
}

/* ========== TTY 接口 ========== */

int uart_tty_register(int port_index) {
    if (port_index < 0 || port_index >= UART_MAX_PORTS) return Z_ERR_RANGE;
    /* TODO: 与 TTY 子系统集成 */
    printk("uart: COM%d TTY 接口注册 (待集成)", port_index + 1);
    return 0;
}

/* ========== 初始化 ========== */

int uart_init(void) {
    printk("uart: 初始化 16550 UART 驱动...");

    /* 初始化 COM1 (主串口) */
    uart_port_init(0, 115200);

    /* 尝试初始化 COM2 */
    uart_port_init(1, 115200);

    printk("uart: 16550 UART 驱动初始化完成");
    return 0;
}

struct uart_port *uart_get_port(int port_index) {
    if (port_index < 0 || port_index >= UART_MAX_PORTS) return NULL;
    return &uart_ports[port_index];
}
