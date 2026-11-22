/*
 * Zratro OS - PWM 控制器驱动
 *
 * 功能: PWM 控制器框架, 背光控制, 风扇速度控制
 */

#ifndef PWM_H
#define PWM_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== PWM 常量 ========== */
#define PWM_MAX_DEVICES      8
#define PWM_MAX_CHANNELS     4      /* 每设备最大通道数 */

/* PWM 极性 */
#define PWM_POLARITY_NORMAL  0     /* 正常极性 (高电平开始) */
#define PWM_POLARITY_INVERSED 1    /* 反转极性 (低电平开始) */

/* PWM 通道模式 */
enum pwm_mode {
    PWM_MODE_NORMAL = 0,           /* 正常 PWM 输出 */
    PWM_MODE_BACKLIGHT,            /* 背光控制模式 */
    PWM_MODE_FAN,                  /* 风扇控制模式 */
};

/* PWM 通道配置 */
struct pwm_channel_config {
    uint32_t period_ns;            /* 周期 (纳秒) */
    uint32_t duty_ns;              /* 占空比 (纳秒) */
    uint8_t polarity;              /* 极性 */
    int enabled;                   /* 是否启用 */
    enum pwm_mode mode;            /* 模式 */
};

/* PWM 通道状态 */
struct pwm_channel {
    int id;                        /* 通道 ID */
    struct pwm_channel_config config;
    spinlock_t lock;

    /* 背光专用 */
    uint8_t brightness;            /* 亮度 (0-100%) */

    /* 风扇专用 */
    uint8_t fan_speed;             /* 风扇速度 (0-100%) */
    uint32_t fan_rpm;              /* 实际 RPM */
};

/* PWM 操作 */
struct pwm_ops {
    int (*config)(void *priv, int channel, uint32_t duty_ns, uint32_t period_ns);
    int (*enable)(void *priv, int channel);
    int (*disable)(void *priv, int channel);
    int (*set_polarity)(void *priv, int channel, uint8_t polarity);
};

/* PWM 设备 */
struct pwm_device {
    char name[32];
    int id;
    int num_channels;
    struct pwm_ops *ops;
    void *priv;

    /* MMIO */
    uint64_t mmio_base;

    /* 通道 */
    struct pwm_channel channels[PWM_MAX_CHANNELS];

    /* 锁 */
    spinlock_t lock;

    struct list_head list;
};

/* ========== API 函数声明 ========== */

/* 初始化 PWM 子系统 */
int pwm_init(void);

/* 设备注册 */
int pwm_add_device(struct pwm_device *dev);
int pwm_del_device(struct pwm_device *dev);
struct pwm_device *pwm_get_device(int id);

/* PWM 配置 */
int pwm_config(struct pwm_device *dev, int channel, uint32_t duty_ns, uint32_t period_ns);
int pwm_enable(struct pwm_device *dev, int channel);
int pwm_disable(struct pwm_device *dev, int channel);
int pwm_set_polarity(struct pwm_device *dev, int channel, uint8_t polarity);

/* 背光控制 */
int pwm_backlight_set(int id, uint8_t brightness);
uint8_t pwm_backlight_get(int id);

/* 风扇控制 */
int pwm_fan_set_speed(int id, uint8_t speed);
uint8_t pwm_fan_get_speed(int id);
uint32_t pwm_fan_get_rpm(int id);

#endif /* PWM_H */
