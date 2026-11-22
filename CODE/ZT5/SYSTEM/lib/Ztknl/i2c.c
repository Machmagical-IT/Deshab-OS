/*
 * Zratro OS - I2C 总线驱动
 *
 * 功能: I2C 总线控制器框架, SMBus 兼容层, 设备注册/探测, 传输 API
 */

#include "i2c.h"
#include "console.h"
#include "heap.h"
#include "string.h"

/* ========== 全局 I2C 子系统 ========== */

/* 适配器列表 */
static struct i2c_adapter *i2c_adapters[I2C_MAX_ADAPTERS];
static int i2c_adapter_count = 0;

/* 全局设备列表 */
static LIST_HEAD(i2c_device_list);
static spinlock_t i2c_core_lock = SPINLOCK_INIT;

/* ========== 适配器管理 ========== */

int i2c_add_adapter(struct i2c_adapter *adapter) {
    if (adapter == NULL) return Z_ERR_NULL;
    if (i2c_adapter_count >= I2C_MAX_ADAPTERS) {
        printk("i2c: 适配器数量已达上限");
        return Z_ERR_RANGE;
    }

    spinlock_init(&adapter->lock);
    INIT_LIST_HEAD(&adapter->devices);
    adapter->busy = 0;
    adapter->xfer_count = 0;
    adapter->error_count = 0;

    /* 分配 ID */
    adapter->id = i2c_adapter_count;
    i2c_adapters[i2c_adapter_count] = adapter;
    i2c_adapter_count++;

    printk("i2c: 注册适配器 %d: %s", adapter->id, adapter->name);
    return 0;
}

int i2c_del_adapter(struct i2c_adapter *adapter) {
    if (adapter == NULL) return Z_ERR_NULL;

    unsigned long flags = spin_lock_irqsave(&i2c_core_lock);

    for (int i = 0; i < i2c_adapter_count; i++) {
        if (i2c_adapters[i] == adapter) {
            i2c_adapters[i] = i2c_adapters[i2c_adapter_count - 1];
            i2c_adapters[i2c_adapter_count - 1] = NULL;
            i2c_adapter_count--;
            break;
        }
    }

    spin_unlock_irqrestore(&i2c_core_lock, flags);
    printk("i2c: 注销适配器 %d", adapter->id);
    return 0;
}

struct i2c_adapter *i2c_get_adapter(int id) {
    if (id < 0 || id >= i2c_adapter_count) return NULL;
    return i2c_adapters[id];
}

/* ========== 设备管理 ========== */

int i2c_add_device(struct i2c_client *client) {
    if (client == NULL || client->adapter == NULL) return Z_ERR_NULL;

    unsigned long flags = spin_lock_irqsave(&i2c_core_lock);

    /* 添加到适配器的设备列表 */
    list_add_tail(&client->list, &client->adapter->devices);

    /* 添加到全局设备列表 */
    list_add_tail(&client->list, &i2c_device_list);

    spin_unlock_irqrestore(&i2c_core_lock, flags);

    printk("i2c: 注册设备 %s 地址=0x%02X 适配器=%d",
           client->name, client->addr, client->adapter->id);
    return 0;
}

int i2c_del_device(struct i2c_client *client) {
    if (client == NULL) return Z_ERR_NULL;

    unsigned long flags = spin_lock_irqsave(&i2c_core_lock);
    list_del(&client->list);
    spin_unlock_irqrestore(&i2c_core_lock, flags);
    return 0;
}

struct i2c_client *i2c_find_device(uint16_t addr) {
    struct i2c_client *client;
    struct list_head *pos;

    list_for_each(pos, &i2c_device_list) {
        client = list_entry(pos, struct i2c_client, list);
        if (client->addr == addr) return client;
    }
    return NULL;
}

/* ========== 传输 API ========== */

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num) {
    if (adap == NULL || msgs == NULL || num <= 0) return Z_ERR_NULL;
    if (adap->algo == NULL || adap->algo->xfer == NULL) return Z_ERR_NOTSUP;

    unsigned long flags = spin_lock_irqsave(&adap->lock);

    if (adap->busy) {
        spin_unlock_irqrestore(&adap->lock, flags);
        return Z_ERR_BUSY;
    }
    adap->busy = 1;

    int ret = adap->algo->xfer(adap, msgs, num);
    if (ret < 0) {
        adap->error_count++;
    } else {
        adap->xfer_count++;
    }

    adap->busy = 0;
    spin_unlock_irqrestore(&adap->lock, flags);
    return ret;
}

int i2c_read(struct i2c_adapter *adap, uint16_t addr, uint8_t *buf, size_t len) {
    if (adap == NULL || buf == NULL) return Z_ERR_NULL;

    struct i2c_msg msg;
    msg.addr = addr;
    msg.flags = I2C_M_RD;
    msg.len = (uint16_t)len;
    msg.buf = buf;

    return i2c_transfer(adap, &msg, 1);
}

int i2c_write(struct i2c_adapter *adap, uint16_t addr, const uint8_t *buf, size_t len) {
    if (adap == NULL || buf == NULL) return Z_ERR_NULL;

    struct i2c_msg msg;
    msg.addr = addr;
    msg.flags = I2C_M_WR;
    msg.len = (uint16_t)len;
    msg.buf = (uint8_t *)buf;

    return i2c_transfer(adap, &msg, 1);
}

/* ========== SMBus 兼容层 ========== */

static int i2c_smbus_xfer_emulated(struct i2c_adapter *adap, uint16_t addr,
                                     uint16_t flags, uint8_t read_write,
                                     uint8_t command, int size,
                                     union i2c_smbus_data *data) {
    /* 使用 I2C 传输模拟 SMBus 操作 */
    struct i2c_msg msg[2];
    int num = 0;
    uint8_t buf[35];  /* 最大 SMBus 块大小 */

    switch (size) {
    case I2C_SMBUS_QUICK:
        msg[num].addr = addr;
        msg[num].flags = flags | (read_write == I2C_SMBUS_READ ? I2C_M_RD : 0);
        msg[num].len = 0;
        msg[num].buf = NULL;
        num = 1;
        break;

    case I2C_SMBUS_BYTE:
        if (read_write == I2C_SMBUS_READ) {
            msg[num].addr = addr;
            msg[num].flags = flags | I2C_M_RD;
            msg[num].len = 1;
            msg[num].buf = &buf[0];
            num = 1;
        } else {
            msg[num].addr = addr;
            msg[num].flags = flags;
            msg[num].len = 1;
            msg[num].buf = &command;
            num = 1;
        }
        break;

    case I2C_SMBUS_BYTE_DATA:
        if (read_write == I2C_SMBUS_READ) {
            /* 写命令字节, 然后读数据 */
            msg[0].addr = addr;
            msg[0].flags = flags;
            msg[0].len = 1;
            msg[0].buf = &command;
            msg[1].addr = addr;
            msg[1].flags = flags | I2C_M_RD;
            msg[1].len = 1;
            msg[1].buf = &buf[0];
            num = 2;
        } else {
            buf[0] = command;
            buf[1] = data->byte;
            msg[0].addr = addr;
            msg[0].flags = flags;
            msg[0].len = 2;
            msg[0].buf = buf;
            num = 1;
        }
        break;

    case I2C_SMBUS_WORD_DATA:
        if (read_write == I2C_SMBUS_READ) {
            msg[0].addr = addr;
            msg[0].flags = flags;
            msg[0].len = 1;
            msg[0].buf = &command;
            msg[1].addr = addr;
            msg[1].flags = flags | I2C_M_RD;
            msg[1].len = 2;
            msg[1].buf = buf;
            num = 2;
        } else {
            buf[0] = command;
            buf[1] = data->word & 0xFF;
            buf[2] = (data->word >> 8) & 0xFF;
            msg[0].addr = addr;
            msg[0].flags = flags;
            msg[0].len = 3;
            msg[0].buf = buf;
            num = 1;
        }
        break;

    default:
        return Z_ERR_NOTSUP;
    }

    int ret = i2c_transfer(adap, msg, num);
    if (ret < 0) return ret;

    /* 处理读取结果 */
    if (read_write == I2C_SMBUS_READ) {
        switch (size) {
        case I2C_SMBUS_BYTE:
        case I2C_SMBUS_BYTE_DATA:
            data->byte = buf[0];
            break;
        case I2C_SMBUS_WORD_DATA:
            data->word = buf[0] | ((uint16_t)buf[1] << 8);
            break;
        default:
            break;
        }
    }

    return 0;
}

int i2c_smbus_read_byte(struct i2c_client *client) {
    union i2c_smbus_data data;
    int ret = i2c_smbus_xfer_emulated(client->adapter, client->addr, 0,
                                       I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &data);
    return ret < 0 ? ret : data.byte;
}

int i2c_smbus_write_byte(struct i2c_client *client, uint8_t value) {
    union i2c_smbus_data data;
    data.byte = value;
    return i2c_smbus_xfer_emulated(client->adapter, client->addr, 0,
                                    I2C_SMBUS_WRITE, value, I2C_SMBUS_BYTE, &data);
}

int i2c_smbus_read_byte_data(struct i2c_client *client, uint8_t command) {
    union i2c_smbus_data data;
    int ret = i2c_smbus_xfer_emulated(client->adapter, client->addr, 0,
                                       I2C_SMBUS_READ, command, I2C_SMBUS_BYTE_DATA, &data);
    return ret < 0 ? ret : data.byte;
}

int i2c_smbus_write_byte_data(struct i2c_client *client, uint8_t command, uint8_t value) {
    union i2c_smbus_data data;
    data.byte = value;
    return i2c_smbus_xfer_emulated(client->adapter, client->addr, 0,
                                    I2C_SMBUS_WRITE, command, I2C_SMBUS_BYTE_DATA, &data);
}

int i2c_smbus_read_word_data(struct i2c_client *client, uint8_t command) {
    union i2c_smbus_data data;
    int ret = i2c_smbus_xfer_emulated(client->adapter, client->addr, 0,
                                       I2C_SMBUS_READ, command, I2C_SMBUS_WORD_DATA, &data);
    return ret < 0 ? ret : data.word;
}

int i2c_smbus_write_word_data(struct i2c_client *client, uint8_t command, uint16_t value) {
    union i2c_smbus_data data;
    data.word = value;
    return i2c_smbus_xfer_emulated(client->adapter, client->addr, 0,
                                    I2C_SMBUS_WRITE, command, I2C_SMBUS_WORD_DATA, &data);
}

/* ========== 设备探测 ========== */

int i2c_probe_device(struct i2c_adapter *adap, uint16_t addr) {
    if (adap == NULL) return Z_ERR_NULL;

    /* 尝试发送 SMBus Quick 命令来检测设备 */
    union i2c_smbus_data data;
    int ret = i2c_smbus_xfer_emulated(adap, addr, 0,
                                       I2C_SMBUS_WRITE, 0, I2C_SMBUS_QUICK, &data);
    return ret == 0 ? 1 : 0;
}

void i2c_scan_bus(struct i2c_adapter *adap) {
    if (adap == NULL) return;

    printk("i2c: 扫描适配器 %d (%s)...", adap->id, adap->name);
    printk("i2c:      0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f");

    for (int addr = 0; addr <= I2C_ADDR_7BIT_MAX; addr += 16) {
        printk("i2c: %02x:", addr);
        for (int i = 0; i < 16; i++) {
            uint16_t a = addr + i;
            /* 跳过保留地址 */
            if (a < 0x03 || a > 0x77) {
                print("   ");
                continue;
            }
            if (i2c_probe_device(adap, a)) {
                printk(" %02x", a);
            } else {
                print(" --");
            }
        }
        println("");
    }
}

/* ========== 初始化 ========== */

int i2c_init(void) {
    printk("i2c: 初始化 I2C 子系统...");
    i2c_adapter_count = 0;
    INIT_LIST_HEAD(&i2c_device_list);
    spinlock_init(&i2c_core_lock);
    printk("i2c: I2C 子系统初始化完成");
    return 0;
}
