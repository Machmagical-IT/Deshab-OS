/*
 * Zratro OS - MC146818 RTC 驱动
 *
 * 功能: 日期时间读写, 闹钟中断, NTP 同步接口
 */

#ifndef RTC_H
#define RTC_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "idt.h"

/* ========== RTC I/O 端口 ========== */
#define RTC_INDEX_PORT      0x70
#define RTC_DATA_PORT       0x71

/* ========== RTC 寄存器 ========== */
#define RTC_SECONDS         0x00   /* 秒 (0-59) */
#define RTC_SECONDS_ALARM   0x01   /* 闹钟秒 */
#define RTC_MINUTES         0x02   /* 分 (0-59) */
#define RTC_MINUTES_ALARM   0x03   /* 闹钟分 */
#define RTC_HOURS           0x04   /* 时 (0-23) */
#define RTC_HOURS_ALARM     0x05   /* 闹钟时 */
#define RTC_DAY_OF_WEEK     0x06   /* 星期 (1-7) */
#define RTC_DAY_OF_MONTH    0x07   /* 日 (1-31) */
#define RTC_MONTH           0x08   /* 月 (1-12) */
#define RTC_YEAR            0x09   /* 年 (0-99) */
#define RTC_REGISTER_A      0x0A   /* 寄存器 A */
#define RTC_REGISTER_B      0x0B   /* 寄存器 B */
#define RTC_REGISTER_C      0x0C   /* 寄存器 C */
#define RTC_REGISTER_D      0x0D   /* 寄存器 D */
#define RTC_CENTURY         0x32   /* 世纪 (某些系统) */

/* ========== 寄存器 A 位 ========== */
#define RTC_A_UIP           0x80   /* 更新进行中 */
#define RTC_A_DV2           0x40   /* 分频位 2 */
#define RTC_A_DV1           0x20   /* 分频位 1 */
#define RTC_A_DV0           0x10   /* 分频位 0 */
#define RTC_A_RS3           0x08   /* 速率选择位 3 */
#define RTC_A_RS2           0x04   /* 速率选择位 2 */
#define RTC_A_RS1           0x02   /* 速率选择位 1 */
#define RTC_A_RS0           0x01   /* 速率选择位 0 */

/* ========== 寄存器 B 位 ========== */
#define RTC_B_SET           0x80   /* 禁止更新 */
#define RTC_B_PIE           0x40   /* 周期中断使能 */
#define RTC_B_AIE           0x20   /* 闹钟中断使能 */
#define RTC_B_UIE           0x10   /* 更新结束中断使能 */
#define RTC_B_SQWE          0x08   /* 方波使能 */
#define RTC_B_DM            0x04   /* 数据模式 (1=二进制, 0=BCD) */
#define RTC_B_2412          0x02   /* 24小时制 (1=24h, 0=12h) */
#define RTC_B_DSE           0x01   /* 夏令时使能 */

/* ========== 寄存器 C 位 ========== */
#define RTC_C_IRQF          0x80   /* 中断请求标志 */
#define RTC_C_PF            0x40   /* 周期中断标志 */
#define RTC_C_AF            0x20   /* 闹钟中断标志 */
#define RTC_C_UF            0x10   /* 更新结束中断标志 */

/* ========== 日期时间结构 ========== */
struct rtc_time {
    uint8_t sec;           /* 秒 (0-59) */
    uint8_t min;           /* 分 (0-59) */
    uint8_t hour;          /* 时 (0-23) */
    uint8_t mday;          /* 日 (1-31) */
    uint8_t mon;           /* 月 (1-12) */
    uint16_t year;         /* 年 (完整4位) */
    uint8_t wday;          /* 星期 (0-6, 0=周日) */
};

/* ========== 闹钟结构 ========== */
struct rtc_alarm {
    uint8_t sec;           /* 闹钟秒 (0xFF=忽略) */
    uint8_t min;           /* 闹钟分 (0xFF=忽略) */
    uint8_t hour;          /* 闹钟时 (0xFF=忽略) */
    int enabled;           /* 是否启用 */
};

/* ========== NTP 同步状态 ========== */
struct ntp_state {
    int synced;            /* 是否已同步 */
    int64_t offset_ms;     /* NTP 偏移量 (毫秒) */
    uint64_t last_sync;    /* 上次同步时间 */
    char server[64];       /* NTP 服务器地址 */
};

/* ========== RTC 设备结构 ========== */
struct rtc_device {
    int initialized;                /* 是否已初始化 */
    int bcd_mode;                   /* 是否 BCD 模式 */
    int use_24h;                    /* 是否 24 小时制 */
    spinlock_t lock;                /* 自旋锁 */

    /* 闹钟 */
    struct rtc_alarm alarm;
    void (*alarm_callback)(void *data);
    void *alarm_data;

    /* NTP */
    struct ntp_state ntp;

    /* 统计 */
    uint64_t read_count;
    uint64_t write_count;
};

/* ========== API 函数声明 ========== */

/* 初始化 RTC 驱动 */
int rtc_init(void);

/* 读取日期时间 */
int rtc_read_time(struct rtc_time *time);

/* 设置日期时间 */
int rtc_set_time(const struct rtc_time *time);

/* 获取 Unix 时间戳 */
time_t rtc_get_unix_time(void);

/* 从 Unix 时间戳设置 */
int rtc_set_unix_time(time_t timestamp);

/* 闹钟控制 */
int rtc_set_alarm(const struct rtc_alarm *alarm);
int rtc_get_alarm(struct rtc_alarm *alarm);
int rtc_enable_alarm(int enable);

/* NTP 同步 */
int rtc_ntp_sync(const char *server);
int rtc_ntp_get_status(struct ntp_state *state);

/* 中断处理 */
void rtc_interrupt_handler(struct interrupt_frame *frame);

/* 获取设备实例 */
struct rtc_device *rtc_get_device(void);

/* BCD 转换辅助 */
uint8_t rtc_bcd_to_bin(uint8_t bcd);
uint8_t rtc_bin_to_bcd(uint8_t bin);

#endif /* RTC_H */
