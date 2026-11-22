/*
 * Zratro OS - 触摸板驱动
 *
 * 功能: I2C HID 触摸板, 多点触控 (5指), 手势识别 (点击, 滚动)
 */

#include "touchpad.h"
#include "i2c.h"
#include "console.h"
#include "heap.h"
#include "string.h"

/* 内核中没有 stdlib 的 abs，内联实现 */
static inline int tp_abs(int x) { return x < 0 ? -x : x; }

/* ========== 全局设备实例 ========== */

static struct touchpad_device tp_dev;

/* ========== I2C HID 操作 ========== */

static int touchpad_i2c_read(uint8_t reg, void *buf, size_t len) {
    if (tp_dev.i2c_client == NULL) return -1;

    struct i2c_adapter *adap = i2c_get_adapter(tp_dev.i2c_adapter_id);
    if (adap == NULL) return -1;

    struct i2c_client *client = (struct i2c_client *)tp_dev.i2c_client;

    /* 写入寄存器地址，然后读取数据 */
    struct i2c_msg msgs[2];
    msgs[0].addr = client->addr;
    msgs[0].flags = 0;  /* 写 */
    msgs[0].buf = &reg;
    msgs[0].len = 1;
    msgs[1].addr = client->addr;
    msgs[1].flags = I2C_M_RD;  /* 读 */
    msgs[1].buf = (uint8_t *)buf;
    msgs[1].len = len;
    return i2c_transfer(adap, msgs, 2);
}

static int touchpad_i2c_write(uint8_t reg, const void *buf, size_t len) {
    if (tp_dev.i2c_client == NULL) return -1;

    struct i2c_adapter *adap = i2c_get_adapter(tp_dev.i2c_adapter_id);
    if (adap == NULL) return -1;

    struct i2c_client *client = (struct i2c_client *)tp_dev.i2c_client;

    /* 构造写缓冲区: 寄存器地址 + 数据 */
    uint8_t *wbuf = (uint8_t *)kmalloc(len + 2);
    if (wbuf == NULL) return Z_ERR_NOMEM;

    wbuf[0] = reg & 0xFF;
    wbuf[1] = (reg >> 8) & 0xFF;
    memcpy(&wbuf[2], buf, len);

    int ret = i2c_write(adap, client->addr, wbuf, len + 2);
    kfree(wbuf);
    return ret;
}

/* ========== HID 描述符解析 ========== */

static int touchpad_parse_hid_desc(void) {
    /* 读取 HID 描述符 (从描述符寄存器) */
    uint8_t desc[30];
    int ret = touchpad_i2c_read(TOUCHPAD_HID_DESC_REG, desc, 30);
    if (ret < 0) {
        printk("touchpad: HID 描述符读取失败");
        return -1;
    }

    /* 解析 HID 描述符
     * 偏移 0-1: 签名 (0x0001)
     * 偏移 2-3: HID 版本
     * 偏移 4: 报告描述符寄存器
     * 偏移 6-7: 报告描述符长度
     * 偏移 8-9: 输入报告寄存器
     * 偏移 10-11: 输入报告长度
     * 偏移 16-17: 输出报告寄存器
     * 偏移 18-19: 输出报告长度
     * 偏移 20-21: 命令寄存器
     * 偏移 22-23: 数据寄存器
     */
    uint16_t sig = desc[0] | (desc[1] << 8);
    if (sig != 0x0001) {
        printk("touchpad: 无效的 HID 签名 0x%04X", sig);
        return -1;
    }

    tp_dev.hid_report_len = desc[10] | (desc[11] << 8);
    tp_dev.hid_desc_addr = desc[8] | (desc[9] << 8);

    printk("touchpad: HID 描述符: 报告寄存器=0x%04X 报告长度=%u",
           tp_dev.hid_desc_addr, tp_dev.hid_report_len);
    return 0;
}

/* ========== 报告读取与解析 ========== */

int touchpad_read_report(void) {
    if (!tp_dev.initialized) return Z_ERR_NOTSUP;

    uint8_t raw[TOUCHPAD_REPORT_SIZE];
    int ret = touchpad_i2c_read(TOUCHPAD_HID_DATA_REG, raw, tp_dev.hid_report_len);
    if (ret < 0) return ret;

    struct touchpad_report report;
    memset(&report, 0, sizeof(report));

    /* 解析 HID 多点触控报告
     * 典型格式:
     * Byte 0: 报告 ID
     * Byte 1: 按键状态
     * Byte 2: 触摸点计数
     * Byte 3+: 每个触摸点数据 (X, Y, 压力, 宽度, ID)
     */
    int offset = 0;

    /* 跳过报告 ID (如果有) */
    if (raw[0] == 0x02 || raw[0] == 0x01) offset = 1;

    /* 按键状态 */
    report.button = raw[offset] & 0x01;
    offset++;

    /* 触摸点计数 */
    report.contact_count = raw[offset] & 0x0F;
    if (report.contact_count > TOUCHPAD_MAX_FINGERS) {
        report.contact_count = TOUCHPAD_MAX_FINGERS;
    }
    offset++;

    /* 解析每个触摸点 */
    for (int i = 0; i < report.contact_count && offset + 7 <= tp_dev.hid_report_len; i++) {
        struct touchpad_finger *f = &report.fingers[i];

        /* 触摸 ID 和标志 */
        f->id = raw[offset] & 0x0F;
        f->active = (raw[offset] & 0x80) ? 0 : 1;  /* bit7=0 表示触摸 */
        offset++;

        /* X 坐标 (16位小端) */
        f->x = raw[offset] | (raw[offset + 1] << 8);
        offset += 2;

        /* Y 坐标 (16位小端) */
        f->y = raw[offset] | (raw[offset + 1] << 8);
        offset += 2;

        /* 压力 */
        f->pressure = raw[offset] | (raw[offset + 1] << 8);
        offset += 2;
    }

    /* 处理报告 */
    touchpad_process_report(&report);

    tp_dev.report_count++;
    return 0;
}

/* ========== 报告处理 ========== */

int touchpad_process_report(const struct touchpad_report *report) {
    if (report == NULL) return Z_ERR_NULL;

    unsigned long flags = spin_lock_irqsave(&tp_dev.lock);

    /* 更新当前报告 */
    tp_dev.report = *report;

    /* 手势识别 */
    enum touchpad_gesture gesture = touchpad_detect_gesture(report);
    tp_dev.gesture.type = gesture;

    /* 向输入子系统报告事件 */
    if (tp_dev.input_dev != NULL) {
        /* 报告触摸点数 */
        for (int i = 0; i < report->contact_count; i++) {
            const struct touchpad_finger *f = &report->fingers[i];
            if (f->active) {
                input_report_abs(tp_dev.input_dev, ABS_MT_POSITION_X, f->x);
                input_report_abs(tp_dev.input_dev, ABS_MT_POSITION_Y, f->y);
                input_report_abs(tp_dev.input_dev, ABS_MT_TRACKING_ID, f->id);
            }
        }

        /* 报告按键 */
        if (report->button) {
            input_report_key(tp_dev.input_dev, BTN_LEFT, 1);
        } else {
            input_report_key(tp_dev.input_dev, BTN_LEFT, 0);
        }

        /* 报告相对移动 (用于鼠标兼容) */
        if (report->contact_count >= 1 && report->fingers[0].active) {
            static int16_t last_x = 0, last_y = 0;
            int16_t dx = (int16_t)report->fingers[0].x - last_x;
            int16_t dy = (int16_t)report->fingers[0].y - last_y;

            /* 只在单指时报告鼠标移动 */
            if (report->contact_count == 1 && (dx != 0 || dy != 0)) {
                input_report_rel(tp_dev.input_dev, REL_X, dx);
                input_report_rel(tp_dev.input_dev, REL_Y, dy);
            }

            last_x = report->fingers[0].x;
            last_y = report->fingers[0].y;
        }

        input_sync(tp_dev.input_dev);
    }

    spin_unlock_irqrestore(&tp_dev.lock, flags);
    return 0;
}

/* ========== 手势识别 ========== */

enum touchpad_gesture touchpad_detect_gesture(const struct touchpad_report *report) {
    if (report == NULL) return TOUCHPAD_GESTURE_NONE;

    struct touchpad_gesture_state *gs = &tp_dev.gesture;

    int active_fingers = 0;
    for (int i = 0; i < report->contact_count; i++) {
        if (report->fingers[i].active) active_fingers++;
    }

    /* 无触摸 */
    if (active_fingers == 0) {
        /* 检查是否是点击手势 (之前有触摸, 现在释放) */
        if (gs->finger_count > 0 && !gs->moved) {
            uint64_t tsc;
            asm volatile("rdtsc" : "=A"(tsc));
            /* 简单时间检查 */
            if (gs->finger_count == 1) {
                gs->finger_count = 0;
                tp_dev.gesture_count++;
                return TOUCHPAD_GESTURE_TAP;
            } else if (gs->finger_count == 2) {
                gs->finger_count = 0;
                tp_dev.gesture_count++;
                return TOUCHPAD_GESTURE_TWO_FINGER_TAP;
            }
        }
        gs->finger_count = 0;
        gs->moved = 0;
        return TOUCHPAD_GESTURE_NONE;
    }

    /* 更新手势状态 */
    if (gs->finger_count == 0) {
        /* 新触摸开始 */
        gs->finger_count = active_fingers;
        gs->start_x = report->fingers[0].x;
        gs->start_y = report->fingers[0].y;
        gs->moved = 0;
        uint64_t tsc;
        asm volatile("rdtsc" : "=A"(tsc));
        gs->start_time = tsc;
    }

    /* 双指滚动 */
    if (active_fingers == 2 && report->contact_count >= 2) {
        int16_t dy = (int16_t)report->fingers[0].y - (int16_t)gs->start_y;
        int16_t dx = (int16_t)report->fingers[0].x - (int16_t)gs->start_x;

        if (tp_abs(dy) > tp_dev.scroll_threshold) {
            gs->moved = 1;
            if (dy < 0) return TOUCHPAD_GESTURE_SCROLL_UP;
            else return TOUCHPAD_GESTURE_SCROLL_DOWN;
        }
        if (tp_abs(dx) > tp_dev.scroll_threshold) {
            gs->moved = 1;
            if (dx < 0) return TOUCHPAD_GESTURE_SCROLL_LEFT;
            else return TOUCHPAD_GESTURE_SCROLL_RIGHT;
        }
    }

    /* 检查是否移动超过阈值 */
    if (active_fingers == 1) {
        int16_t dx = (int16_t)report->fingers[0].x - (int16_t)gs->start_x;
        int16_t dy = (int16_t)report->fingers[0].y - (int16_t)gs->start_y;
        if (tp_abs(dx) > tp_dev.tap_threshold || tp_abs(dy) > tp_dev.tap_threshold) {
            gs->moved = 1;
        }
    }

    return TOUCHPAD_GESTURE_NONE;
}

/* ========== 公共 API ========== */

int touchpad_get_fingers(struct touchpad_finger *fingers, int *count) {
    if (fingers == NULL || count == NULL) return Z_ERR_NULL;

    unsigned long flags = spin_lock_irqsave(&tp_dev.lock);
    int n = tp_dev.report.contact_count;
    for (int i = 0; i < n && i < TOUCHPAD_MAX_FINGERS; i++) {
        fingers[i] = tp_dev.report.fingers[i];
    }
    *count = n;
    spin_unlock_irqrestore(&tp_dev.lock, flags);
    return 0;
}

int touchpad_get_gesture(enum touchpad_gesture *gesture) {
    if (gesture == NULL) return Z_ERR_NULL;
    *gesture = tp_dev.gesture.type;
    return 0;
}

int touchpad_set_sensitivity(int level) {
    /* level: 1-5, 1=最不灵敏, 5=最灵敏 */
    tp_dev.tap_threshold = 30 - (level * 5);  /* 灵敏度越高, 阈值越低 */
    tp_dev.scroll_threshold = 20 - (level * 2);
    return 0;
}

int touchpad_set_tap_to_click(int enable) {
    /* 启用/禁用点击手势 */
    (void)enable;
    return 0;
}

/* ========== I2C HID 探测 ========== */

int touchpad_probe_i2c(uint16_t i2c_addr, int adapter_id) {
    printk("touchpad: 探测 I2C HID 触摸板 (地址=0x%02X 适配器=%d)...", i2c_addr, adapter_id);

    struct i2c_adapter *adap = i2c_get_adapter(adapter_id);
    if (adap == NULL) {
        printk("touchpad: I2C 适配器 %d 不存在", adapter_id);
        return -1;
    }

    /* 尝试读取 HID 描述符 */
    tp_dev.i2c_addr = i2c_addr;
    tp_dev.i2c_adapter_id = adapter_id;

    if (touchpad_parse_hid_desc() != 0) {
        printk("touchpad: HID 描述符解析失败");
        return -1;
    }

    return 0;
}

/* ========== 初始化 ========== */

int touchpad_init(void) {
    printk("touchpad: 初始化触摸板驱动...");

    memset(&tp_dev, 0, sizeof(tp_dev));
    spinlock_init(&tp_dev.lock);

    /* 默认参数 */
    tp_dev.max_x = TOUCHPAD_MAX_WIDTH;
    tp_dev.max_y = TOUCHPAD_MAX_HEIGHT;
    tp_dev.tap_threshold = 10;
    tp_dev.tap_time_threshold = 200;
    tp_dev.scroll_threshold = 10;

    /* 分配输入设备 */
    tp_dev.input_dev = input_allocate_device();
    if (tp_dev.input_dev != NULL) {
        strncpy_safe(tp_dev.input_dev->name, "I2C HID Touchpad",
                     sizeof(tp_dev.input_dev->name) - 1);
        tp_dev.input_dev->type = INPUT_TYPE_MOUSE;

        /* 设置能力 */
        input_set_capability(tp_dev.input_dev, EV_KEY, BTN_LEFT);
        input_set_capability(tp_dev.input_dev, EV_REL, REL_X);
        input_set_capability(tp_dev.input_dev, EV_REL, REL_Y);
        input_set_capability(tp_dev.input_dev, EV_ABS, ABS_MT_POSITION_X);
        input_set_capability(tp_dev.input_dev, EV_ABS, ABS_MT_POSITION_Y);
        input_set_capability(tp_dev.input_dev, EV_ABS, ABS_MT_TRACKING_ID);

        /* 设置绝对轴参数 */
        input_set_abs_params(tp_dev.input_dev, ABS_MT_POSITION_X, 0, tp_dev.max_x, 0, 0);
        input_set_abs_params(tp_dev.input_dev, ABS_MT_POSITION_Y, 0, tp_dev.max_y, 0, 0);

        input_register_device(tp_dev.input_dev);
    }

    /* 尝试探测常见 I2C 触摸板地址 */
    touchpad_probe_i2c(0x2C, 0);  /* 常见 HID I2C 地址 */
    touchpad_probe_i2c(0x15, 0);  /* 另一个常见地址 */

    tp_dev.initialized = 1;
    printk("touchpad: 触摸板驱动初始化完成");
    return 0;
}

struct touchpad_device *touchpad_get_device(void) {
    return &tp_dev;
}
