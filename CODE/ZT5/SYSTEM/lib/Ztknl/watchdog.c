/*
 * Zratro OS - 看门狗定时器驱动
 *
 * 功能: i6300ESB 看门狗 (QEMU 兼容), 框架 API, 喂狗/保活, 超时复位
 */

#include "watchdog.h"
#include "console.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"

/* ========== MMIO 访问 ========== */

static inline uint32_t wdt_mmio_read32(struct wdt_device *dev, uint32_t reg) {
    return *(volatile uint32_t *)(dev->mmio_base + reg);
}

static inline void wdt_mmio_write32(struct wdt_device *dev, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(dev->mmio_base + reg) = val;
}

/* ========== 全局设备实例 ========== */

static struct wdt_device wdt_dev;

/* ========== i6300ESB 操作 ========== */

static int wdt_esb_start(void *priv) {
    struct wdt_device *dev = (struct wdt_device *)priv;
    if (dev->mmio_base == 0) return -1;

    /* 启用看门狗: 重启模式 + 使能 */
    uint32_t config = WDT_CONFIG_REBOOT | WDT_CONFIG_ENABLE;
    wdt_mmio_write32(dev, WDT_CONFIG_REG, config);

    /* 设置初始重载值 */
    wdt_mmio_write32(dev, WDT_RELOAD_REG, dev->timeout * 60);  /* 转换为滴答 */

    dev->status = WDT_STATUS_RUNNING;
    printk("wdt: 看门狗已启动 (超时=%us)", dev->timeout);
    return 0;
}

static int wdt_esb_stop(void *priv) {
    struct wdt_device *dev = (struct wdt_device *)priv;
    if (dev->mmio_base == 0) return -1;

    if (dev->nowayout) {
        printk("wdt: nowayout 模式, 不允许停止");
        return -1;
    }

    /* 禁用看门狗 */
    wdt_mmio_write32(dev, WDT_CONFIG_REG, 0);
    dev->status = WDT_STATUS_STOPPED;
    printk("wdt: 看门狗已停止");
    return 0;
}

static int wdt_esb_keepalive(void *priv) {
    struct wdt_device *dev = (struct wdt_device *)priv;
    if (dev->mmio_base == 0 || dev->status != WDT_STATUS_RUNNING) return -1;

    /* 重载计数器 */
    wdt_mmio_write32(dev, WDT_RELOAD_REG, dev->timeout * 60);
    dev->keepalive_count++;

    /* 记录喂狗时间 */
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    dev->last_feed_time = tsc;

    return 0;
}

static int wdt_esb_set_timeout(void *priv, uint32_t timeout) {
    struct wdt_device *dev = (struct wdt_device *)priv;
    if (timeout < dev->min_timeout || timeout > dev->max_timeout) return -1;

    dev->timeout = timeout;

    /* 如果正在运行, 立即更新重载值 */
    if (dev->status == WDT_STATUS_RUNNING) {
        wdt_mmio_write32(dev, WDT_RELOAD_REG, timeout * 60);
    }

    return 0;
}

static uint32_t wdt_esb_get_timeleft(void *priv) {
    struct wdt_device *dev = (struct wdt_device *)priv;
    if (dev->mmio_base == 0) return 0;

    /* 读取当前计数器值 */
    uint32_t reload = wdt_mmio_read32(dev, WDT_RELOAD_REG);
    return reload / 60;  /* 转换为秒 */
}

/* ========== 看门狗操作表 ========== */

static struct wdt_ops wdt_esb_ops = {
    .start       = wdt_esb_start,
    .stop        = wdt_esb_stop,
    .keepalive   = wdt_esb_keepalive,
    .set_timeout = wdt_esb_set_timeout,
    .get_timeleft = wdt_esb_get_timeleft,
};

/* ========== PCI 探测 ========== */

int wdt_probe(void) {
    printk("wdt: 扫描 i6300ESB 看门狗...");

    for (uint8_t bus = 0; bus < 255; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor = pci_read_config32(bus, dev, func, 0x00);
                if (vendor == 0xFFFFFFFF) continue;

                uint16_t vid = vendor & 0xFFFF;
                uint16_t did = (vendor >> 16) & 0xFFFF;

                if (vid == WDT_VENDOR_ID && did == WDT_DEVICE_ID) {
                    printk("wdt: 发现 i6300ESB bus=%u dev=%u func=%u", bus, dev, func);
                    wdt_dev.pci_bus = bus;
                    wdt_dev.pci_dev = dev;
                    wdt_dev.pci_func = func;
                    return 0;
                }
            }
        }
    }

    printk("wdt: 未找到 i6300ESB 设备");
    return -1;
}

/* ========== 公共 API ========== */

int wdt_start(void) {
    struct wdt_device *dev = &wdt_dev;
    if (dev->ops == NULL) return -1;

    unsigned long flags = spin_lock_irqsave(&dev->lock);
    int ret = dev->ops->start(dev->priv ? dev->priv : dev);
    spin_unlock_irqrestore(&dev->lock, flags);
    return ret;
}

int wdt_stop(void) {
    struct wdt_device *dev = &wdt_dev;
    if (dev->ops == NULL) return -1;

    unsigned long flags = spin_lock_irqsave(&dev->lock);
    int ret = dev->ops->stop(dev->priv ? dev->priv : dev);
    spin_unlock_irqrestore(&dev->lock, flags);
    return ret;
}

int wdt_keepalive(void) {
    struct wdt_device *dev = &wdt_dev;
    if (dev->ops == NULL || dev->ops->keepalive == NULL) return -1;

    unsigned long flags = spin_lock_irqsave(&dev->lock);
    int ret = dev->ops->keepalive(dev->priv ? dev->priv : dev);
    spin_unlock_irqrestore(&dev->lock, flags);
    return ret;
}

int wdt_set_timeout(uint32_t seconds) {
    struct wdt_device *dev = &wdt_dev;
    if (dev->ops == NULL || dev->ops->set_timeout == NULL) return -1;

    unsigned long flags = spin_lock_irqsave(&dev->lock);
    int ret = dev->ops->set_timeout(dev->priv ? dev->priv : dev, seconds);
    spin_unlock_irqrestore(&dev->lock, flags);
    return ret;
}

uint32_t wdt_get_timeout(void) {
    return wdt_dev.timeout;
}

uint32_t wdt_get_timeleft(void) {
    struct wdt_device *dev = &wdt_dev;
    if (dev->ops == NULL || dev->ops->get_timeleft == NULL) return 0;
    return dev->ops->get_timeleft(dev->priv ? dev->priv : dev);
}

enum wdt_status wdt_get_status(void) {
    return wdt_dev.status;
}

struct wdt_device *wdt_get_device(void) {
    return &wdt_dev;
}

/* ========== 初始化 ========== */

int watchdog_init(void) {
    printk("wdt: 初始化看门狗驱动...");

    memset(&wdt_dev, 0, sizeof(wdt_dev));
    spinlock_init(&wdt_dev.lock);

    strncpy_safe(wdt_dev.name, "i6300ESB", sizeof(wdt_dev.name) - 1);
    wdt_dev.timeout = WDT_DEFAULT_TIMEOUT;
    wdt_dev.min_timeout = WDT_MIN_TIMEOUT;
    wdt_dev.max_timeout = WDT_MAX_TIMEOUT;
    wdt_dev.status = WDT_STATUS_UNKNOWN;
    wdt_dev.nowayout = 0;

    /* 探测 PCI 设备 */
    if (wdt_probe() != 0) {
        printk("wdt: 未找到看门狗硬件, 驱动未加载");
        return -1;
    }

    /* 获取 MMIO 基址 */
    uint32_t bar0 = pci_get_bar(wdt_dev.pci_bus, wdt_dev.pci_dev, wdt_dev.pci_func, 0);
    if (bar0 & 0x01) {
        printk("wdt: BAR0 是 I/O 空间, 不支持");
        return -1;
    }
    wdt_dev.mmio_base = (uint64_t)phys_to_virt(bar0 & ~0xF);

    /* 启用 PCI 总线主控 */
    pci_enable_bus_master(wdt_dev.pci_bus, wdt_dev.pci_dev, wdt_dev.pci_func);

    /* 设置操作函数 */
    wdt_dev.ops = &wdt_esb_ops;
    wdt_dev.priv = &wdt_dev;

    wdt_dev.status = WDT_STATUS_STOPPED;
    printk("wdt: i6300ESB 看门狗驱动初始化完成 (MMIO=0x%016llX)", wdt_dev.mmio_base);
    return 0;
}
