/*
 * Zratro OS - 看门狗定时器驱动
 *
 * 功能: i6300ESB 看门狗 (QEMU 兼容), 框架 API, 喂狗/保活, 超时复位
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== i6300ESB PCI 标识 ========== */
#define WDT_VENDOR_ID       0x8086
#define WDT_DEVICE_ID       0x25AB   /* i6300ESB Watchdog */

/* ========== i6300ESB 寄存器 ========== */
#define WDT_RELOAD_REG      0x00    /* 重载值寄存器 */
#define WDT_CONFIG_REG      0x04    /* 配置寄存器 */

/* 配置寄存器位 */
#define WDT_CONFIG_REBOOT   0x01    /* 超时后重启 */
#define WDT_CONFIG_IRQ      0x02    /* 超时后中断 */
#define WDT_CONFIG_ENABLE   0x04    /* 使能看门狗 */

/* ========== 看门狗框架常量 ========== */
#define WDT_MAX_DEVICES     4
#define WDT_MIN_TIMEOUT     1       /* 最小超时 (秒) */
#define WDT_MAX_TIMEOUT     3600    /* 最大超时 (秒) */
#define WDT_DEFAULT_TIMEOUT 30      /* 默认超时 (秒) */

/* 看门狗状态 */
enum wdt_status {
    WDT_STATUS_UNKNOWN    = 0,
    WDT_STATUS_STOPPED    = 1,
    WDT_STATUS_RUNNING    = 2,
    WDT_STATUS_EXPIRED    = 3,
};

/* 看门狗操作 */
struct wdt_ops {
    int (*start)(void *priv);
    int (*stop)(void *priv);
    int (*keepalive)(void *priv);
    int (*set_timeout)(void *priv, uint32_t timeout);
    uint32_t (*get_timeleft)(void *priv);
};

/* 看门狗设备 */
struct wdt_device {
    char name[32];                   /* 设备名称 */
    int id;                          /* 设备 ID */
    enum wdt_status status;          /* 当前状态 */
    uint32_t timeout;                /* 超时时间 (秒) */
    uint32_t min_timeout;            /* 最小超时 */
    uint32_t max_timeout;            /* 最大超时 */
    int nowayout;                    /* 是否不允许停止 */

    struct wdt_ops *ops;             /* 操作函数 */
    void *priv;                      /* 私有数据 */

    /* i6300ESB 特有 */
    uint8_t pci_bus;
    uint8_t pci_dev;
    uint8_t pci_func;
    uint64_t mmio_base;              /* MMIO 基址 */

    /* 统计 */
    uint64_t keepalive_count;
    uint64_t expire_count;
    uint64_t last_feed_time;

    spinlock_t lock;
};

/* ========== API 函数声明 ========== */

/* 初始化看门狗子系统 */
int watchdog_init(void);

/* 探测 i6300ESB */
int wdt_probe(void);

/* 启动/停止看门狗 */
int wdt_start(void);
int wdt_stop(void);

/* 喂狗/保活 */
int wdt_keepalive(void);

/* 设置超时 */
int wdt_set_timeout(uint32_t seconds);
uint32_t wdt_get_timeout(void);

/* 获取剩余时间 */
uint32_t wdt_get_timeleft(void);

/* 获取状态 */
enum wdt_status wdt_get_status(void);

/* 获取设备实例 */
struct wdt_device *wdt_get_device(void);

#endif /* WATCHDOG_H */
