/*
 * Zratro OS - 触摸板驱动
 *
 * 功能: I2C HID 触摸板, 多点触控 (5指), 手势识别 (点击, 滚动)
 */

#ifndef TOUCHPAD_H
#define TOUCHPAD_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "input.h"

/* ========== 触摸板常量 ========== */
#define TOUCHPAD_MAX_FINGERS    5      /* 最大触摸点数 */
#define TOUCHPAD_MAX_WIDTH      4096   /* 最大宽度 */
#define TOUCHPAD_MAX_HEIGHT     4096   /* 最大高度 */
#define TOUCHPAD_REPORT_SIZE    64     /* HID 报告大小 */

/* HID 描述符类型 */
#define TOUCHPAD_HID_DESC_REG   0x01   /* HID 描述符寄存器 */
#define TOUCHPAD_HID_REPORT_REG 0x02   /* HID 报告寄存器 */
#define TOUCHPAD_HID_CMD_REG    0x03   /* HID 命令寄存器 */
#define TOUCHPAD_HID_DATA_REG   0x04   /* HID 数据寄存器 */

/* HID 命令 */
#define TOUCHPAD_HID_CMD_RESET  0x01
#define TOUCHPAD_HID_CMD_SET_POWER 0x08

/* 手势类型 */
enum touchpad_gesture {
    TOUCHPAD_GESTURE_NONE = 0,
    TOUCHPAD_GESTURE_TAP,             /* 单击 */
    TOUCHPAD_GESTURE_DOUBLE_TAP,      /* 双击 */
    TOUCHPAD_GESTURE_TWO_FINGER_TAP,  /* 双指点击 (右键) */
    TOUCHPAD_GESTURE_SCROLL_UP,       /* 向上滚动 */
    TOUCHPAD_GESTURE_SCROLL_DOWN,     /* 向下滚动 */
    TOUCHPAD_GESTURE_SCROLL_LEFT,     /* 向左滚动 */
    TOUCHPAD_GESTURE_SCROLL_RIGHT,    /* 向右滚动 */
    TOUCHPAD_GESTURE_PINCH_IN,        /* 缩小 */
    TOUCHPAD_GESTURE_PINCH_OUT,       /* 放大 */
};

/* 触摸点状态 */
struct touchpad_finger {
    int active;                       /* 是否触摸中 */
    uint16_t x;                       /* X 坐标 */
    uint16_t y;                       /* Y 坐标 */
    uint16_t pressure;                /* 压力值 */
    uint16_t width;                   /* 触摸宽度 */
    uint8_t id;                       /* 触摸 ID */
};

/* 触摸板报告 */
struct touchpad_report {
    uint8_t contact_count;            /* 触摸点数 */
    uint8_t button;                   /* 按键状态 (bit0=左键) */
    struct touchpad_finger fingers[TOUCHPAD_MAX_FINGERS];
    uint16_t scan_time;               /* 扫描时间 */
};

/* 手势识别状态 */
struct touchpad_gesture_state {
    enum touchpad_gesture type;       /* 当前手势 */
    uint64_t start_time;              /* 手势开始时间 */
    uint16_t start_x;                 /* 起始 X */
    uint16_t start_y;                 /* 起始 Y */
    int finger_count;                 /* 手指数 */
    int moved;                        /* 是否移动过 */
};

/* 触摸板设备结构 */
struct touchpad_device {
    int initialized;                    /* 是否已初始化 */
    char name[64];                      /* 设备名称 */

    /* I2C 连接 */
    uint16_t i2c_addr;                  /* I2C 地址 */
    int i2c_adapter_id;                 /* I2C 适配器 ID */
    void *i2c_client;                   /* I2C 客户端 */

    /* HID 描述符 */
    uint16_t hid_desc_addr;             /* HID 描述符地址 */
    uint16_t hid_report_len;            /* 报告长度 */

    /* 触摸板参数 */
    uint16_t max_x;                     /* 最大 X */
    uint16_t max_y;                     /* 最大 Y */
    uint16_t resolution_x;              /* X 分辨率 */
    uint16_t resolution_y;              /* Y 分辨率 */

    /* 当前状态 */
    struct touchpad_report report;       /* 当前报告 */
    struct touchpad_gesture_state gesture;/* 手势状态 */

    /* 输入设备集成 */
    struct input_dev *input_dev;

    /* 手势参数 */
    uint16_t tap_threshold;             /* 点击移动阈值 (像素) */
    uint16_t tap_time_threshold;        /* 点击时间阈值 (ms) */
    uint16_t scroll_threshold;          /* 滚动阈值 */

    /* 统计 */
    uint64_t report_count;
    uint64_t gesture_count;

    /* 锁 */
    spinlock_t lock;
};

/* ========== API 函数声明 ========== */

/* 初始化触摸板驱动 */
int touchpad_init(void);

/* I2C HID 探测 */
int touchpad_probe_i2c(uint16_t i2c_addr, int adapter_id);

/* 读取 HID 报告 */
int touchpad_read_report(void);

/* 处理报告 */
int touchpad_process_report(const struct touchpad_report *report);

/* 手势识别 */
enum touchpad_gesture touchpad_detect_gesture(const struct touchpad_report *report);

/* 获取当前状态 */
int touchpad_get_fingers(struct touchpad_finger *fingers, int *count);
int touchpad_get_gesture(enum touchpad_gesture *gesture);

/* 配置 */
int touchpad_set_sensitivity(int level);
int touchpad_set_tap_to_click(int enable);

/* 获取设备实例 */
struct touchpad_device *touchpad_get_device(void);

#endif /* TOUCHPAD_H */
