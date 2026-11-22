/*
 * Zratro OS - I2C 总线驱动
 *
 * 功能: I2C 总线控制器框架, SMBus 兼容层, 设备注册/探测, 传输 API
 */

#ifndef I2C_H
#define I2C_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== I2C 常量 ========== */
#define I2C_MAX_ADAPTERS     8      /* 最大适配器数 */
#define I2C_MAX_DEVICES      32     /* 最大设备数 */
#define I2C_MAX_XFER_LEN    4096   /* 最大传输长度 */
#define I2C_ADDR_7BIT_MAX   0x7F   /* 7 位地址最大值 */
#define I2C_ADDR_10BIT_MAX  0x3FF  /* 10 位地址最大值 */

/* I2C 传输标志 */
#define I2C_M_WR            0x0000  /* 写操作 */
#define I2C_M_RD            0x0001  /* 读操作 */
#define I2C_M_TEN           0x0010  /* 10 位地址 */
#define I2C_M_NOSTART       0x0020  /* 不发送 START */
#define I2C_M_REV_DIR       0x0040  /* 反转方向 */
#define I2C_M_STOP          0x0080  /* 发送 STOP */

/* SMBus 协议 */
#define I2C_SMBUS_READ      1
#define I2C_SMBUS_WRITE     0

/* SMBus 事务类型 */
enum i2c_smbus_type {
    I2C_SMBUS_QUICK,
    I2C_SMBUS_BYTE,
    I2C_SMBUS_BYTE_DATA,
    I2C_SMBUS_WORD_DATA,
    I2C_SMBUS_PROC_CALL,
    I2C_SMBUS_BLOCK_DATA,
    I2C_SMBUS_I2C_BLOCK_DATA,
    I2C_SMBUS_BLOCK_PROC_CALL,
};

/* ========== I2C 消息结构 ========== */
struct i2c_msg {
    uint16_t addr;          /* 从设备地址 */
    uint16_t flags;         /* 传输标志 */
    uint16_t len;           /* 数据长度 */
    uint8_t *buf;           /* 数据缓冲区 */
};

/* ========== I2C 适配器操作 ========== */
struct i2c_adapter;

struct i2c_algorithm {
    const char *name;
    /* 传输函数 - 适配器必须实现 */
    int (*xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
    /* SMBus 传输 (可选) */
    int (*smbus_xfer)(struct i2c_adapter *adap, uint16_t addr,
                      uint16_t flags, uint8_t read_write,
                      uint8_t command, int size, union i2c_smbus_data *data);
    /* 功能查询 */
    uint32_t (*functionality)(struct i2c_adapter *adap);
};

/* I2C 功能位 */
#define I2C_FUNC_I2C            0x00000001
#define I2C_FUNC_10BIT_ADDR     0x00000002
#define I2C_FUNC_SMBUS_QUICK    0x00010000
#define I2C_FUNC_SMBUS_BYTE     0x00020000
#define I2C_FUNC_SMBUS_BYTE_DATA 0x00080000
#define I2C_FUNC_SMBUS_WORD_DATA 0x00100000
#define I2C_FUNC_SMBUS_BLOCK_DATA 0x01000000

/* ========== I2C 适配器结构 ========== */
struct i2c_adapter {
    int id;                         /* 适配器 ID */
    char name[32];                  /* 适配器名称 */
    struct i2c_algorithm *algo;     /* 算法操作 */
    void *algo_data;                /* 算法私有数据 */

    /* 控制器寄存器 */
    uint64_t mmio_base;             /* MMIO 基址 (如果是 MMIO) */
    uint16_t io_base;               /* I/O 基址 (如果是端口 I/O) */
    int irq;                        /* 中断号 */

    /* 状态 */
    int busy;                       /* 是否繁忙 */
    spinlock_t lock;                /* 自旋锁 */

    /* 设备列表 */
    struct list_head devices;        /* 已注册的设备 */
    struct list_head list;           /* 全局适配器列表 */

    /* 统计 */
    uint64_t xfer_count;
    uint64_t error_count;
};

/* ========== I2C 设备结构 ========== */
struct i2c_device_id {
    char name[32];
    uint16_t driver_data;
};

struct i2c_driver {
    const char *name;
    int (*probe)(struct i2c_client *client);
    int (*remove)(struct i2c_client *client);
    void (*shutdown)(struct i2c_client *client);
};

struct i2c_client {
    uint16_t addr;                  /* 从设备地址 */
    char name[32];                  /* 设备名称 */
    struct i2c_adapter *adapter;    /* 所属适配器 */
    struct i2c_driver *driver;      /* 驱动 */
    void *driver_data;              /* 驱动私有数据 */
    int irq;                        /* 设备中断 */
    struct list_head list;          /* 适配器设备列表 */
};

/* ========== SMBus 数据联合体 ========== */
union i2c_smbus_data {
    uint8_t byte;
    uint16_t word;
    uint8_t block[34];  /* block[0] = 长度, block[1-33] = 数据 */
};

/* ========== I2C 核心 API ========== */

/* 初始化 I2C 子系统 */
int i2c_init(void);

/* 适配器注册/注销 */
int i2c_add_adapter(struct i2c_adapter *adapter);
int i2c_del_adapter(struct i2c_adapter *adapter);
struct i2c_adapter *i2c_get_adapter(int id);

/* 设备注册/注销 */
int i2c_add_device(struct i2c_client *client);
int i2c_del_device(struct i2c_client *client);
struct i2c_client *i2c_find_device(uint16_t addr);

/* 传输 API */
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int i2c_read(struct i2c_adapter *adap, uint16_t addr, uint8_t *buf, size_t len);
int i2c_write(struct i2c_adapter *adap, uint16_t addr, const uint8_t *buf, size_t len);

/* SMBus 兼容 API */
int i2c_smbus_read_byte(struct i2c_client *client);
int i2c_smbus_write_byte(struct i2c_client *client, uint8_t value);
int i2c_smbus_read_byte_data(struct i2c_client *client, uint8_t command);
int i2c_smbus_write_byte_data(struct i2c_client *client, uint8_t command, uint8_t value);
int i2c_smbus_read_word_data(struct i2c_client *client, uint8_t command);
int i2c_smbus_write_word_data(struct i2c_client *client, uint8_t command, uint16_t value);

/* 探测设备 */
int i2c_probe_device(struct i2c_adapter *adap, uint16_t addr);

/* 扫描总线 */
void i2c_scan_bus(struct i2c_adapter *adap);

#endif /* I2C_H */
