/*
 * Zratro OS - PWM 控制器驱动
 *
 * 功能: PWM 控制器框架, 背光控制, 风扇速度控制
 */

#include "pwm.h"
#include "console.h"
#include "string.h"

/* ========== 全局 PWM 子系统 ========== */

static struct pwm_device *pwm_devices[PWM_MAX_DEVICES];
static int pwm_device_count = 0;
static spinlock_t pwm_core_lock = SPINLOCK_INIT;

/* ========== 设备管理 ========== */

int pwm_add_device(struct pwm_device *dev) {
    if (dev == NULL) return Z_ERR_NULL;
    if (pwm_device_count >= PWM_MAX_DEVICES) return Z_ERR_RANGE;

    spinlock_init(&dev->lock);
    dev->id = pwm_device_count;

    /* 初始化所有通道 */
    for (int i = 0; i < dev->num_channels && i < PWM_MAX_CHANNELS; i++) {
        dev->channels[i].id = i;
        dev->channels[i].config.period_ns = 1000000;  /* 默认 1ms */
        dev->channels[i].config.duty_ns = 500000;     /* 默认 50% */
        dev->channels[i].config.polarity = PWM_POLARITY_NORMAL;
        dev->channels[i].config.enabled = 0;
        dev->channels[i].config.mode = PWM_MODE_NORMAL;
        dev->channels[i].brightness = 100;
        dev->channels[i].fan_speed = 0;
        dev->channels[i].fan_rpm = 0;
        spinlock_init(&dev->channels[i].lock);
    }

    pwm_devices[pwm_device_count] = dev;
    pwm_device_count++;

    printk("pwm: 注册设备 %d: %s (%d 通道)", dev->id, dev->name, dev->num_channels);
    return 0;
}

int pwm_del_device(struct pwm_device *dev) {
    if (dev == NULL) return Z_ERR_NULL;

    unsigned long flags = spin_lock_irqsave(&pwm_core_lock);
    for (int i = 0; i < pwm_device_count; i++) {
        if (pwm_devices[i] == dev) {
            pwm_devices[i] = pwm_devices[pwm_device_count - 1];
            pwm_devices[pwm_device_count - 1] = NULL;
            pwm_device_count--;
            break;
        }
    }
    spin_unlock_irqrestore(&pwm_core_lock, flags);
    return 0;
}

struct pwm_device *pwm_get_device(int id) {
    if (id < 0 || id >= pwm_device_count) return NULL;
    return pwm_devices[id];
}

/* ========== PWM 配置 ========== */

int pwm_config(struct pwm_device *dev, int channel, uint32_t duty_ns, uint32_t period_ns) {
    if (dev == NULL || channel < 0 || channel >= dev->num_channels) return Z_ERR_INVALID;
    if (dev->ops == NULL || dev->ops->config == NULL) return Z_ERR_NOTSUP;

    unsigned long flags = spin_lock_irqsave(&dev->channels[channel].lock);

    int ret = dev->ops->config(dev->priv, channel, duty_ns, period_ns);
    if (ret == 0) {
        dev->channels[channel].config.duty_ns = duty_ns;
        dev->channels[channel].config.period_ns = period_ns;
    }

    spin_unlock_irqrestore(&dev->channels[channel].lock, flags);
    return ret;
}

int pwm_enable(struct pwm_device *dev, int channel) {
    if (dev == NULL || channel < 0 || channel >= dev->num_channels) return Z_ERR_INVALID;
    if (dev->ops == NULL || dev->ops->enable == NULL) return Z_ERR_NOTSUP;

    unsigned long flags = spin_lock_irqsave(&dev->channels[channel].lock);
    int ret = dev->ops->enable(dev->priv, channel);
    if (ret == 0) dev->channels[channel].config.enabled = 1;
    spin_unlock_irqrestore(&dev->channels[channel].lock, flags);
    return ret;
}

int pwm_disable(struct pwm_device *dev, int channel) {
    if (dev == NULL || channel < 0 || channel >= dev->num_channels) return Z_ERR_INVALID;
    if (dev->ops == NULL || dev->ops->disable == NULL) return Z_ERR_NOTSUP;

    unsigned long flags = spin_lock_irqsave(&dev->channels[channel].lock);
    int ret = dev->ops->disable(dev->priv, channel);
    if (ret == 0) dev->channels[channel].config.enabled = 0;
    spin_unlock_irqrestore(&dev->channels[channel].lock, flags);
    return ret;
}

int pwm_set_polarity(struct pwm_device *dev, int channel, uint8_t polarity) {
    if (dev == NULL || channel < 0 || channel >= dev->num_channels) return Z_ERR_INVALID;
    if (dev->ops == NULL || dev->ops->set_polarity == NULL) return Z_ERR_NOTSUP;

    unsigned long flags = spin_lock_irqsave(&dev->channels[channel].lock);
    int ret = dev->ops->set_polarity(dev->priv, channel, polarity);
    if (ret == 0) dev->channels[channel].config.polarity = polarity;
    spin_unlock_irqrestore(&dev->channels[channel].lock, flags);
    return ret;
}

/* ========== 背光控制 ========== */

int pwm_backlight_set(int id, uint8_t brightness) {
    struct pwm_device *dev = pwm_get_device(id);
    if (dev == NULL) return Z_ERR_NOENT;

    /* 查找背光模式通道 */
    int ch = -1;
    for (int i = 0; i < dev->num_channels; i++) {
        if (dev->channels[i].config.mode == PWM_MODE_BACKLIGHT) {
            ch = i;
            break;
        }
    }
    if (ch < 0) {
        /* 使用第一个通道 */
        ch = 0;
        dev->channels[ch].config.mode = PWM_MODE_BACKLIGHT;
    }

    brightness = MIN(brightness, 100);

    /* 根据亮度计算占空比 */
    uint32_t period_ns = dev->channels[ch].config.period_ns;
    uint32_t duty_ns = (period_ns * brightness) / 100;

    int ret = pwm_config(dev, ch, duty_ns, period_ns);
    if (ret == 0) {
        dev->channels[ch].brightness = brightness;
        if (!dev->channels[ch].config.enabled) {
            pwm_enable(dev, ch);
        }
    }

    return ret;
}

uint8_t pwm_backlight_get(int id) {
    struct pwm_device *dev = pwm_get_device(id);
    if (dev == NULL) return 0;

    for (int i = 0; i < dev->num_channels; i++) {
        if (dev->channels[i].config.mode == PWM_MODE_BACKLIGHT) {
            return dev->channels[i].brightness;
        }
    }
    return 100;
}

/* ========== 风扇控制 ========== */

int pwm_fan_set_speed(int id, uint8_t speed) {
    struct pwm_device *dev = pwm_get_device(id);
    if (dev == NULL) return Z_ERR_NOENT;

    /* 查找风扇模式通道 */
    int ch = -1;
    for (int i = 0; i < dev->num_channels; i++) {
        if (dev->channels[i].config.mode == PWM_MODE_FAN) {
            ch = i;
            break;
        }
    }
    if (ch < 0) {
        ch = MIN(1, dev->num_channels - 1);
        dev->channels[ch].config.mode = PWM_MODE_FAN;
    }

    speed = MIN(speed, 100);

    /* 风扇 PWM 通常 25kHz (40000ns 周期) */
    uint32_t period_ns = 40000;
    uint32_t duty_ns = (period_ns * speed) / 100;

    int ret = pwm_config(dev, ch, duty_ns, period_ns);
    if (ret == 0) {
        dev->channels[ch].fan_speed = speed;
        if (!dev->channels[ch].config.enabled) {
            pwm_enable(dev, ch);
        }
    }

    return ret;
}

uint8_t pwm_fan_get_speed(int id) {
    struct pwm_device *dev = pwm_get_device(id);
    if (dev == NULL) return 0;

    for (int i = 0; i < dev->num_channels; i++) {
        if (dev->channels[i].config.mode == PWM_MODE_FAN) {
            return dev->channels[i].fan_speed;
        }
    }
    return 0;
}

uint32_t pwm_fan_get_rpm(int id) {
    struct pwm_device *dev = pwm_get_device(id);
    if (dev == NULL) return 0;

    for (int i = 0; i < dev->num_channels; i++) {
        if (dev->channels[i].config.mode == PWM_MODE_FAN) {
            return dev->channels[i].fan_rpm;
        }
    }
    return 0;
}

/* ========== 初始化 ========== */

int pwm_init(void) {
    printk("pwm: 初始化 PWM 子系统...");
    pwm_device_count = 0;
    spinlock_init(&pwm_core_lock);
    printk("pwm: PWM 子系统初始化完成");
    return 0;
}
