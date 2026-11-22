/*
 * Zratro OS - SPI 总线驱动
 *
 * 功能: SPI 控制器框架, 设备注册, 传输 API
 */

#include "spi.h"
#include "console.h"
#include "heap.h"
#include "string.h"

/* ========== 全局 SPI 子系统 ========== */

static struct spi_adapter *spi_adapters[SPI_MAX_ADAPTERS];
static int spi_adapter_count = 0;
static spinlock_t spi_core_lock = SPINLOCK_INIT;

/* ========== 适配器管理 ========== */

int spi_add_adapter(struct spi_adapter *adapter) {
    if (adapter == NULL) return Z_ERR_NULL;
    if (spi_adapter_count >= SPI_MAX_ADAPTERS) {
        printk("spi: 适配器数量已达上限");
        return Z_ERR_RANGE;
    }

    spinlock_init(&adapter->lock);
    INIT_LIST_HEAD(&adapter->devices);
    adapter->busy = 0;
    adapter->xfer_count = 0;
    adapter->error_count = 0;
    adapter->id = spi_adapter_count;

    spi_adapters[spi_adapter_count] = adapter;
    spi_adapter_count++;

    printk("spi: 注册适配器 %d: %s (最大速率=%uHz CS=%u)",
           adapter->id, adapter->name, adapter->max_speed_hz, adapter->num_chipselect);
    return 0;
}

int spi_del_adapter(struct spi_adapter *adapter) {
    if (adapter == NULL) return Z_ERR_NULL;

    unsigned long flags = spin_lock_irqsave(&spi_core_lock);
    for (int i = 0; i < spi_adapter_count; i++) {
        if (spi_adapters[i] == adapter) {
            spi_adapters[i] = spi_adapters[spi_adapter_count - 1];
            spi_adapters[spi_adapter_count - 1] = NULL;
            spi_adapter_count--;
            break;
        }
    }
    spin_unlock_irqrestore(&spi_core_lock, flags);

    printk("spi: 注销适配器 %d", adapter->id);
    return 0;
}

struct spi_adapter *spi_get_adapter(int id) {
    if (id < 0 || id >= spi_adapter_count) return NULL;
    return spi_adapters[id];
}

/* ========== 设备管理 ========== */

int spi_add_device(struct spi_device *device) {
    if (device == NULL || device->adapter == NULL) return Z_ERR_NULL;

    /* 配置设备 */
    if (device->adapter->ops && device->adapter->ops->setup) {
        int ret = device->adapter->ops->setup(device->adapter, device);
        if (ret < 0) {
            printk("spi: 设备 %s 配置失败", device->name);
            return ret;
        }
    }

    unsigned long flags = spin_lock_irqsave(&spi_core_lock);
    list_add_tail(&device->list, &device->adapter->devices);
    spin_unlock_irqrestore(&spi_core_lock, flags);

    printk("spi: 注册设备 %s CS=%u 速率=%uHz 模式=%u",
           device->name, device->chip_select, device->max_speed_hz, device->mode);
    return 0;
}

int spi_del_device(struct spi_device *device) {
    if (device == NULL) return Z_ERR_NULL;

    unsigned long flags = spin_lock_irqsave(&spi_core_lock);
    list_del(&device->list);
    spin_unlock_irqrestore(&spi_core_lock, flags);
    return 0;
}

/* ========== 传输 API ========== */

int spi_transfer(struct spi_adapter *adap, struct spi_device *dev,
                 struct spi_transfer *xfer) {
    if (adap == NULL || dev == NULL || xfer == NULL) return Z_ERR_NULL;
    if (adap->ops == NULL) return Z_ERR_NOTSUP;

    unsigned long flags = spin_lock_irqsave(&adap->lock);
    if (adap->busy) {
        spin_unlock_irqrestore(&adap->lock, flags);
        return Z_ERR_BUSY;
    }
    adap->busy = 1;

    int ret;
    if (adap->ops->transfer) {
        ret = adap->ops->transfer(adap, dev, xfer);
    } else if (adap->ops->transfer_one) {
        /* 使用 transfer_one 实现分块传输 */
        uint32_t len = xfer->len;
        const uint8_t *tx = xfer->tx_buf;
        uint8_t *rx = xfer->rx_buf;
        ret = 0;

        /* 选中设备 */
        if (adap->ops->chip_select) {
            adap->ops->chip_select(adap, dev->chip_select, 1);
        }

        while (len > 0) {
            uint32_t chunk = MIN(len, SPI_MAX_XFER_LEN);
            int r = adap->ops->transfer_one(adap, dev, tx, rx, chunk);
            if (r < 0) {
                ret = r;
                break;
            }
            if (tx) tx += chunk;
            if (rx) rx += chunk;
            len -= chunk;
        }

        /* 取消选中 */
        if (adap->ops->chip_select) {
            adap->ops->chip_select(adap, dev->chip_select, 0);
        }
    } else {
        ret = Z_ERR_NOTSUP;
    }

    if (ret < 0) {
        adap->error_count++;
    } else {
        adap->xfer_count++;
    }

    adap->busy = 0;
    spin_unlock_irqrestore(&adap->lock, flags);
    return ret;
}

int spi_read(struct spi_adapter *adap, struct spi_device *dev,
             uint8_t *buf, size_t len) {
    struct spi_transfer xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.rx_buf = buf;
    xfer.len = (uint32_t)len;
    xfer.speed_hz = dev->max_speed_hz;
    xfer.bits_per_word = dev->bits_per_word;
    xfer.flags = SPI_XFER_RX;
    return spi_transfer(adap, dev, &xfer);
}

int spi_write(struct spi_adapter *adap, struct spi_device *dev,
              const uint8_t *buf, size_t len) {
    struct spi_transfer xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.tx_buf = buf;
    xfer.len = (uint32_t)len;
    xfer.speed_hz = dev->max_speed_hz;
    xfer.bits_per_word = dev->bits_per_word;
    xfer.flags = SPI_XFER_TX;
    return spi_transfer(adap, dev, &xfer);
}

int spi_write_then_read(struct spi_adapter *adap, struct spi_device *dev,
                        const uint8_t *txbuf, size_t n_tx,
                        uint8_t *rxbuf, size_t n_rx) {
    if (adap == NULL || dev == NULL) return Z_ERR_NULL;

    unsigned long flags = spin_lock_irqsave(&adap->lock);
    if (adap->busy) {
        spin_unlock_irqrestore(&adap->lock, flags);
        return Z_ERR_BUSY;
    }
    adap->busy = 1;

    /* 选中设备 */
    if (adap->ops && adap->ops->chip_select) {
        adap->ops->chip_select(adap, dev->chip_select, 1);
    }

    int ret = 0;

    /* 先写 */
    if (n_tx > 0) {
        struct spi_transfer tx_xfer;
        memset(&tx_xfer, 0, sizeof(tx_xfer));
        tx_xfer.tx_buf = txbuf;
        tx_xfer.len = (uint32_t)n_tx;
        tx_xfer.speed_hz = dev->max_speed_hz;
        tx_xfer.bits_per_word = dev->bits_per_word;
        tx_xfer.flags = SPI_XFER_TX;

        if (adap->ops && adap->ops->transfer) {
            ret = adap->ops->transfer(adap, dev, &tx_xfer);
        } else if (adap->ops && adap->ops->transfer_one) {
            ret = adap->ops->transfer_one(adap, dev, txbuf, NULL, (uint32_t)n_tx);
        }
    }

    /* 后读 */
    if (ret >= 0 && n_rx > 0) {
        struct spi_transfer rx_xfer;
        memset(&rx_xfer, 0, sizeof(rx_xfer));
        rx_xfer.rx_buf = rxbuf;
        rx_xfer.len = (uint32_t)n_rx;
        rx_xfer.speed_hz = dev->max_speed_hz;
        rx_xfer.bits_per_word = dev->bits_per_word;
        rx_xfer.flags = SPI_XFER_RX;

        if (adap->ops && adap->ops->transfer) {
            ret = adap->ops->transfer(adap, dev, &rx_xfer);
        } else if (adap->ops && adap->ops->transfer_one) {
            ret = adap->ops->transfer_one(adap, dev, NULL, rxbuf, (uint32_t)n_rx);
        }
    }

    /* 取消选中 */
    if (adap->ops && adap->ops->chip_select) {
        adap->ops->chip_select(adap, dev->chip_select, 0);
    }

    if (ret < 0) {
        adap->error_count++;
    } else {
        adap->xfer_count++;
    }

    adap->busy = 0;
    spin_unlock_irqrestore(&adap->lock, flags);
    return ret;
}

/* ========== 片选控制 ========== */

void spi_chip_select(struct spi_adapter *adap, uint8_t cs, int enable) {
    if (adap == NULL || adap->ops == NULL) return;
    if (adap->ops->chip_select) {
        adap->ops->chip_select(adap, cs, enable);
    }
}

/* ========== 设备配置 ========== */

int spi_setup_device(struct spi_device *dev) {
    if (dev == NULL || dev->adapter == NULL) return Z_ERR_NULL;
    if (dev->adapter->ops && dev->adapter->ops->setup) {
        return dev->adapter->ops->setup(dev->adapter, dev);
    }
    return 0;
}

/* ========== 初始化 ========== */

int spi_init(void) {
    printk("spi: 初始化 SPI 子系统...");
    spi_adapter_count = 0;
    spinlock_init(&spi_core_lock);
    printk("spi: SPI 子系统初始化完成");
    return 0;
}
