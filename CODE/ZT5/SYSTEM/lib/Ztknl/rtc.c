/*
 * Zratro OS - MC146818 RTC 驱动
 *
 * 功能: 日期时间读写, 闹钟中断, NTP 同步接口
 */

#include "rtc.h"
#include "console.h"
#include "idt.h"
#include "apic.h"
#include "string.h"

/* ========== I/O 端口访问 ========== */

static inline uint8_t rtc_read_reg(uint8_t reg) {
    asm volatile("outb %0, %1" : : "a"(reg), "Nd"(RTC_INDEX_PORT));
    uint8_t val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(RTC_DATA_PORT));
    return val;
}

static inline void rtc_write_reg(uint8_t reg, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(reg), "Nd"(RTC_INDEX_PORT));
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(RTC_DATA_PORT));
}

/* ========== 全局设备实例 ========== */

static struct rtc_device rtc_dev;

/* ========== BCD 转换 ========== */

uint8_t rtc_bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) & 0x0F) * 10;
}

uint8_t rtc_bin_to_bcd(uint8_t bin) {
    return ((bin / 10) << 4) | (bin % 10);
}

/* ========== 等待更新完成 ========== */

static void rtc_wait_update(void) {
    /* 等待 UIP 位清零 (更新周期结束) */
    for (int i = 0; i < 100000; i++) {
        if (!(rtc_read_reg(RTC_REGISTER_A) & RTC_A_UIP)) return;
    }
    /* 超时，继续执行 */
}

/* ========== 日期时间操作 ========== */

int rtc_read_time(struct rtc_time *time) {
    if (time == NULL) return Z_ERR_NULL;

    struct rtc_device *dev = &rtc_dev;
    unsigned long flags = spin_lock_irqsave(&dev->lock);

    /* 等待更新完成 */
    rtc_wait_update();

    /* 读取时间寄存器 */
    uint8_t sec = rtc_read_reg(RTC_SECONDS);
    uint8_t min = rtc_read_reg(RTC_MINUTES);
    uint8_t hour = rtc_read_reg(RTC_HOURS);
    uint8_t mday = rtc_read_reg(RTC_DAY_OF_MONTH);
    uint8_t mon = rtc_read_reg(RTC_MONTH);
    uint8_t year = rtc_read_reg(RTC_YEAR);
    uint8_t wday = rtc_read_reg(RTC_DAY_OF_WEEK);

    /* BCD 转换 */
    if (!dev->bcd_mode) {
        /* 硬件返回 BCD，需要转换 */
        sec = rtc_bcd_to_bin(sec);
        min = rtc_bcd_to_bin(min);
        mday = rtc_bcd_to_bin(mday);
        mon = rtc_bcd_to_bin(mon);
        year = rtc_bcd_to_bin(year);
        wday = rtc_bcd_to_bin(wday);

        /* 处理 12 小时制 */
        if (!dev->use_24h && (hour & 0x80)) {
            hour = rtc_bcd_to_bin(hour & 0x7F);
            if (hour != 12) hour += 12;
        } else {
            hour = rtc_bcd_to_bin(hour);
        }
    }

    /* 计算完整年份 */
    time->sec = sec;
    time->min = min;
    time->hour = hour;
    time->mday = mday;
    time->mon = mon;
    time->wday = wday;

    /* 世纪处理 */
    uint8_t century = rtc_read_reg(RTC_CENTURY);
    if (century != 0) {
        century = dev->bcd_mode ? century : rtc_bcd_to_bin(century);
        time->year = century * 100 + year;
    } else {
        /* 无世纪寄存器，假设 2000+ */
        time->year = (year < 80) ? 2000 + year : 1900 + year;
    }

    dev->read_count++;
    spin_unlock_irqrestore(&dev->lock, flags);
    return 0;
}

int rtc_set_time(const struct rtc_time *time) {
    if (time == NULL) return Z_ERR_NULL;

    struct rtc_device *dev = &rtc_dev;
    unsigned long flags = spin_lock_irqsave(&dev->lock);

    /* 禁止更新 */
    uint8_t reg_b = rtc_read_reg(RTC_REGISTER_B);
    rtc_write_reg(RTC_REGISTER_B, reg_b | RTC_B_SET);

    /* 写入时间寄存器 (BCD 格式) */
    uint8_t year = time->year % 100;
    uint8_t century = time->year / 100;

    rtc_write_reg(RTC_SECONDS, rtc_bin_to_bcd(time->sec));
    rtc_write_reg(RTC_MINUTES, rtc_bin_to_bcd(time->min));
    rtc_write_reg(RTC_HOURS, rtc_bin_to_bcd(time->hour));
    rtc_write_reg(RTC_DAY_OF_MONTH, rtc_bin_to_bcd(time->mday));
    rtc_write_reg(RTC_MONTH, rtc_bin_to_bcd(time->mon));
    rtc_write_reg(RTC_YEAR, rtc_bin_to_bcd(year));
    rtc_write_reg(RTC_DAY_OF_WEEK, rtc_bin_to_bcd(time->wday));

    /* 写入世纪 */
    if (century > 0) {
        rtc_write_reg(RTC_CENTURY, rtc_bin_to_bcd(century));
    }

    /* 恢复更新 */
    rtc_write_reg(RTC_REGISTER_B, reg_b & ~RTC_B_SET);

    dev->write_count++;
    spin_unlock_irqrestore(&dev->lock, flags);
    return 0;
}

/* ========== Unix 时间戳转换 ========== */

/* 判断闰年 */
static int is_leap_year(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/* 每月天数 */
static const uint8_t days_per_month[] = {
    0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

time_t rtc_get_unix_time(void) {
    struct rtc_time time;
    if (rtc_read_time(&time) != 0) return 0;

    /* 计算从 1970-01-01 到给定日期的总秒数 */
    time_t days = 0;

    /* 累加完整年份的天数 */
    for (uint16_t y = 1970; y < time.year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }

    /* 累加当年月份的天数 */
    for (int m = 1; m < time.mon; m++) {
        days += days_per_month[m];
        if (m == 2 && is_leap_year(time.year)) days++;
    }

    /* 加上当月天数 */
    days += time.mday - 1;

    /* 计算总秒数 */
    time_t unix_time = days * 86400 + time.hour * 3600 + time.min * 60 + time.sec;

    /* 应用 NTP 偏移 */
    unix_time += rtc_dev.ntp.offset_ms / 1000;

    return unix_time;
}

int rtc_set_unix_time(time_t timestamp) {
    struct rtc_time time;

    /* 从 Unix 时间戳计算日期时间 */
    int64_t days = timestamp / 86400;
    int64_t rem = timestamp % 86400;
    if (rem < 0) { days--; rem += 86400; }

    time.hour = (uint8_t)(rem / 3600);
    rem %= 3600;
    time.min = (uint8_t)(rem / 60);
    time.sec = (uint8_t)(rem % 60);

    /* 计算年份 */
    uint16_t year = 1970;
    while (1) {
        int64_t ydays = is_leap_year(year) ? 366 : 365;
        if (days < ydays) break;
        days -= ydays;
        year++;
    }
    time.year = year;

    /* 计算月份 */
    time.mday = 1;
    for (int m = 1; m <= 12; m++) {
        uint8_t mdays = days_per_month[m];
        if (m == 2 && is_leap_year(year)) mdays++;
        if (days < mdays) {
            time.mon = (uint8_t)m;
            time.mday = (uint8_t)(days + 1);
            break;
        }
        days -= mdays;
    }

    /* 计算星期 (Zeller 公式简化) */
    time.wday = (uint8_t)((timestamp / 86400 + 4) % 7);

    return rtc_set_time(&time);
}

/* ========== 闹钟控制 ========== */

int rtc_set_alarm(const struct rtc_alarm *alarm) {
    if (alarm == NULL) return Z_ERR_NULL;

    struct rtc_device *dev = &rtc_dev;
    unsigned long flags = spin_lock_irqsave(&dev->lock);

    /* 禁止更新 */
    uint8_t reg_b = rtc_read_reg(RTC_REGISTER_B);
    rtc_write_reg(RTC_REGISTER_B, reg_b | RTC_B_SET);

    /* 写入闹钟寄存器 */
    rtc_write_reg(RTC_SECONDS_ALARM,
                  alarm->sec == 0xFF ? 0xFF : rtc_bin_to_bcd(alarm->sec));
    rtc_write_reg(RTC_MINUTES_ALARM,
                  alarm->min == 0xFF ? 0xFF : rtc_bin_to_bcd(alarm->min));
    rtc_write_reg(RTC_HOURS_ALARM,
                  alarm->hour == 0xFF ? 0xFF : rtc_bin_to_bcd(alarm->hour));

    /* 恢复更新 */
    rtc_write_reg(RTC_REGISTER_B, reg_b & ~RTC_B_SET);

    dev->alarm = *alarm;

    spin_unlock_irqrestore(&dev->lock, flags);
    return 0;
}

int rtc_get_alarm(struct rtc_alarm *alarm) {
    if (alarm == NULL) return Z_ERR_NULL;
    *alarm = rtc_dev.alarm;
    return 0;
}

int rtc_enable_alarm(int enable) {
    struct rtc_device *dev = &rtc_dev;
    unsigned long flags = spin_lock_irqsave(&dev->lock);

    uint8_t reg_b = rtc_read_reg(RTC_REGISTER_B);
    if (enable) {
        reg_b |= RTC_B_AIE;
    } else {
        reg_b &= ~RTC_B_AIE;
    }
    rtc_write_reg(RTC_REGISTER_B, reg_b);
    dev->alarm.enabled = enable;

    spin_unlock_irqrestore(&dev->lock, flags);
    return 0;
}

/* ========== NTP 同步 ========== */

int rtc_ntp_sync(const char *server) {
    if (server == NULL) return Z_ERR_NULL;

    struct rtc_device *dev = &rtc_dev;

    /* 记录 NTP 服务器 */
    strncpy_safe(dev->ntp.server, server, sizeof(dev->ntp.server) - 1);

    /* TODO: 实际 NTP 同步需要网络栈支持
     * 这里只记录服务器地址和同步状态
     * 完整实现需要:
     * 1. 创建 UDP socket
     * 2. 发送 NTP 请求 (端口 123)
     * 3. 解析 NTP 响应
     * 4. 计算偏移量并调整 RTC
     */
    printk("rtc: NTP 同步请求已记录 (服务器=%s)", server);

    dev->ntp.synced = 0;  /* 暂时标记为未同步 */
    return 0;
}

int rtc_ntp_get_status(struct ntp_state *state) {
    if (state == NULL) return Z_ERR_NULL;
    *state = rtc_dev.ntp;
    return 0;
}

/* ========== 中断处理 ========== */

void rtc_interrupt_handler(struct interrupt_frame *frame) {
    (void)frame;

    /* 读取寄存器 C 以清除中断标志 */
    uint8_t reg_c = rtc_read_reg(RTC_REGISTER_C);

    if (reg_c & RTC_C_AF) {
        /* 闹钟中断 */
        if (rtc_dev.alarm_callback) {
            rtc_dev.alarm_callback(rtc_dev.alarm_data);
        }
    }

    if (reg_c & RTC_C_UF) {
        /* 更新结束中断 */
    }

    if (reg_c & RTC_C_PF) {
        /* 周期中断 */
    }

    apic_eoi();
}

/* ========== 初始化 ========== */

int rtc_init(void) {
    printk("rtc: 初始化 MC146818 RTC 驱动...");

    memset(&rtc_dev, 0, sizeof(rtc_dev));
    spinlock_init(&rtc_dev.lock);

    /* 读取寄存器 B 确定模式 */
    uint8_t reg_b = rtc_read_reg(RTC_REGISTER_B);
    rtc_dev.bcd_mode = (reg_b & RTC_B_DM) ? 1 : 0;  /* 1=二进制, 0=BCD */
    rtc_dev.use_24h = (reg_b & RTC_B_2412) ? 1 : 0;

    /* 如果是 BCD 模式 (大多数系统), 标记以便读取时转换 */
    rtc_dev.bcd_mode = !(reg_b & RTC_B_DM);

    printk("rtc: 数据模式=%s 时间格式=%s",
           rtc_dev.bcd_mode ? "BCD" : "二进制",
           rtc_dev.use_24h ? "24小时" : "12小时");

    /* 初始化速率 - 1024Hz 周期中断 (可选) */
    rtc_write_reg(RTC_REGISTER_A, 0x26);  /* 分频=32768/1024, RS=0110 */

    /* 启用周期中断和方波输出 (可选) */
    /* rtc_write_reg(RTC_REGISTER_B, reg_b | RTC_B_PIE | RTC_B_SQWE); */

    /* 注册 RTC 中断 (IRQ8) */
    isr_register_handler(IRQ_RTC, rtc_interrupt_handler);

    /* 读取并显示当前时间 */
    struct rtc_time time;
    if (rtc_read_time(&time) == 0) {
        printk("rtc: 当前时间 %04u-%02u-%02u %02u:%02u:%02u",
               time.year, time.mon, time.mday, time.hour, time.min, time.sec);
    }

    rtc_dev.initialized = 1;
    printk("rtc: MC146818 RTC 驱动初始化完成");
    return 0;
}

struct rtc_device *rtc_get_device(void) {
    return &rtc_dev;
}
