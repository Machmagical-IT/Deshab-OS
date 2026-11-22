/*
 * Zratro OS - SPI 总线驱动
 *
 * 功能: SPI 控制器框架, 设备注册, 传输 API
 */

#ifndef SPI_H
#define SPI_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== SPI 常量 ========== */
#define SPI_MAX_ADAPTERS     8
#define SPI_MAX_DEVICES      32
#define SPI_MAX_XFER_LEN    65535

/* SPI 模式 */
#define SPI_MODE_0           0   /* CPOL=0, CPHA=0 */
#define SPI_MODE_1           1   /* CPOL=0, CPHA=1 */
#define SPI_MODE_2           2   /* CPOL=1, CPHA=0 */
#define SPI_MODE_3           3   /* CPOL=1, CPHA=1 */

/* SPI 片选极性 */
#define SPI_CS_HIGH          0x01   /* 片选高有效 */
#define SPI_LSB_FIRST        0x02   /* LSB 先传输 */
#define SPI_3WIRE            0x04   /* 三线模式 (双向) */
#define SPI_LOOP             0x08   /* 回环模式 */
#define SPI_NO_CS            0x10   /* 无片选 */
#define SPI_READY            0x20   /* 从设备拉低以暂停 */
#define SPI_TX_DUAL          0x40   /* 双线发送 */
#define SPI_RX_DUAL          0x80   /* 双线接收 */

/* SPI 传输标志 */
#define SPI_XFER_TX          0x01   /* 发送缓冲区有效 */
#define SPI_XFER_RX          0x02   /* 接收缓冲区有效 */
#define SPI_XFER_DMA         0x04   /* 使用 DMA */

/* ========== SPI 传输结构 ========== */
struct spi_transfer {
    const uint8_t *tx_buf;     /* 发送缓冲区 */
    uint8_t *rx_buf;           /* 接收缓冲区 */
    uint32_t len;              /* 传输长度 (字节) */
    uint32_t speed_hz;         /* 传输速率 (0=使用默认) */
    uint16_t delay_usecs;      /* 片选间延迟 */
    uint8_t bits_per_word;     /* 每字比特数 (0=使用默认) */
    uint8_t flags;             /* 传输标志 */
    struct list_head list;     /* 链表 */
};

/* ========== SPI 设备结构 ========== */
struct spi_adapter;

struct spi_device {
    char name[32];                  /* 设备名称 */
    uint8_t chip_select;            /* 片选编号 */
    uint32_t max_speed_hz;          /* 最大速率 */
    uint8_t mode;                   /* SPI 模式 */
    uint8_t bits_per_word;          /* 每字比特数 */
    int irq;                        /* 中断号 */
    void *driver_data;              /* 驱动私有数据 */
    struct spi_adapter *adapter;    /* 所属适配器 */
    struct list_head list;          /* 设备列表 */
};

/* ========== SPI 驱动结构 ========== */
struct spi_driver {
    const char *name;
    int (*probe)(struct spi_device *dev);
    int (*remove)(struct spi_device *dev);
    void (*shutdown)(struct spi_device *dev);
};

/* ========== SPI 适配器操作 ========== */
struct spi_adapter_ops {
    int (*setup)(struct spi_adapter *adap, struct spi_device *dev);
    int (*transfer)(struct spi_adapter *adap, struct spi_device *dev,
                    struct spi_transfer *xfer);
    int (*transfer_one)(struct spi_adapter *adap, struct spi_device *dev,
                        const uint8_t *tx, uint8_t *rx, uint32_t len);
    void (*chip_select)(struct spi_adapter *adap, uint8_t cs, int enable);
};

/* ========== SPI 适配器结构 ========== */
struct spi_adapter {
    int id;                         /* 适配器 ID */
    char name[32];                  /* 适配器名称 */
    uint32_t max_speed_hz;          /* 最大速率 */
    uint8_t num_chipselect;         /* 片选数量 */
    uint8_t bits_per_word;          /* 默认每字比特数 */
    uint8_t mode;                   /* 默认模式 */

    struct spi_adapter_ops *ops;    /* 操作函数 */
    void *priv;                     /* 控制器私有数据 */

    /* MMIO / I/O */
    uint64_t mmio_base;
    uint16_t io_base;
    int irq;

    /* 状态 */
    int busy;
    spinlock_t lock;

    /* 设备列表 */
    struct list_head devices;
    struct list_head list;

    /* 统计 */
    uint64_t xfer_count;
    uint64_t error_count;
};

/* ========== SPI 核心 API ========== */

/* 初始化 SPI 子系统 */
int spi_init(void);

/* 适配器注册/注销 */
int spi_add_adapter(struct spi_adapter *adapter);
int spi_del_adapter(struct spi_adapter *adapter);
struct spi_adapter *spi_get_adapter(int id);

/* 设备注册/注销 */
int spi_add_device(struct spi_device *device);
int spi_del_device(struct spi_device *device);

/* 传输 API */
int spi_transfer(struct spi_adapter *adap, struct spi_device *dev,
                 struct spi_transfer *xfer);
int spi_read(struct spi_adapter *adap, struct spi_device *dev,
             uint8_t *buf, size_t len);
int spi_write(struct spi_adapter *adap, struct spi_device *dev,
              const uint8_t *buf, size_t len);
int spi_write_then_read(struct spi_adapter *adap, struct spi_device *dev,
                        const uint8_t *txbuf, size_t n_tx,
                        uint8_t *rxbuf, size_t n_rx);

/* 片选控制 */
void spi_chip_select(struct spi_adapter *adap, uint8_t cs, int enable);

/* 设备配置 */
int spi_setup_device(struct spi_device *dev);

#endif /* SPI_H */
