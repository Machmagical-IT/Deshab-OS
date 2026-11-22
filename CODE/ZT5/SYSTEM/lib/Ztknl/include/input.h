/*
 * Zratro OS - Input Device Subsystem
 * 
 * 统一的输入设备抽象层
 */

#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* 输入设备类型 */
#define INPUT_TYPE_KEYBOARD     1
#define INPUT_TYPE_MOUSE        2
#define INPUT_TYPE_TOUCHSCREEN  3
#define INPUT_TYPE_JOYSTICK     4
#define INPUT_TYPE_TABLET       5

/* 键盘事件类型 */
#define EV_KEY          0x01
#define EV_REL          0x02    /* 相对坐标 */
#define EV_ABS          0x03    /* 绝对坐标 */
#define EV_MSC          0x04    /* 杂项 */
#define EV_SYN          0x00    /* 同步事件 */

/* 键码定义 */
#define KEY_RESERVED    0
#define KEY_ESC         1
#define KEY_1           2
#define KEY_2           3
#define KEY_3           4
#define KEY_4           5
#define KEY_5           6
#define KEY_6           7
#define KEY_7           8
#define KEY_8           9
#define KEY_9           10
#define KEY_0           11
#define KEY_MINUS       12
#define KEY_EQUAL       13
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_Q           16
#define KEY_W           17
#define KEY_E           18
#define KEY_R           19
#define KEY_T           20
#define KEY_Y           21
#define KEY_U           22
#define KEY_I           23
#define KEY_O           24
#define KEY_P           25
#define KEY_LEFTBRACE   26
#define KEY_RIGHTBRACE  27
#define KEY_ENTER       28
#define KEY_LEFTCTRL    29
#define KEY_A           30
#define KEY_S           31
#define KEY_D           32
#define KEY_F           33
#define KEY_G           34
#define KEY_H           35
#define KEY_J           36
#define KEY_K           37
#define KEY_L           38
#define KEY_SEMICOLON   39
#define KEY_APOSTROPHE  40
#define KEY_GRAVE       41
#define KEY_LEFTSHIFT   42
#define KEY_BACKSLASH   43
#define KEY_Z           44
#define KEY_X           45
#define KEY_C           46
#define KEY_V           47
#define KEY_B           48
#define KEY_N           49
#define KEY_M           50
#define KEY_COMMA       51
#define KEY_DOT         52
#define KEY_SLASH       53
#define KEY_RIGHTSHIFT  54
#define KEY_KPASTERISK  55
#define KEY_LEFTALT     56
#define KEY_SPACE       57
#define KEY_CAPSLOCK    58

#define KEY_F1          59
#define KEY_F2          60
#define KEY_F3          61
#define KEY_F4          62
#define KEY_F5          63
#define KEY_F6          64
#define KEY_F7          65
#define KEY_F8          66
#define KEY_F9          67
#define KEY_F10         68
#define KEY_F11         87
#define KEY_F12         88

/* 功能键 */
#define KEY_RIGHTCTRL   97
#define KEY_RIGHTALT    100
#define KEY_HOME        102
#define KEY_UP          103
#define KEY_PAGEUP      104
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_END         107
#define KEY_DOWN        108
#define KEY_PAGEDOWN    109
#define KEY_INSERT      110
#define KEY_DELETE      111

/* 鼠标按钮 */
#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112
#define BTN_SIDE        0x113
#define BTN_EXTRA       0x114

/* 键码计数 */
#define KEY_CNT         0x200   /* 最大键码数 */
#define KEY_MAX         (KEY_CNT - 1)

/* 相对轴 */
#define REL_X           0x00
#define REL_Y           0x01
#define REL_Z           0x02
#define REL_RX          0x03
#define REL_RY          0x04
#define REL_RZ          0x05
#define REL_HWHEEL      0x06
#define REL_DIAL        0x07
#define REL_WHEEL       0x08
#define REL_CNT         0x10    /* 相对轴数量 */
#define REL_MAX         (REL_CNT - 1)

/* 绝对轴 */
#define ABS_X           0x00
#define ABS_Y           0x01
#define ABS_Z           0x02
#define ABS_RX          0x03
#define ABS_RY          0x04
#define ABS_RZ          0x05
#define ABS_CNT         0x40    /* 绝对轴数量 */
#define ABS_MAX         (ABS_CNT - 1)
#define ABS_THROTTLE    0x06
#define ABS_RUDDER      0x07
#define ABS_WHEEL       0x08
#define ABS_GAS         0x09
#define ABS_BRAKE       0x0a
#define ABS_PRESSURE    0x18
#define ABS_DISTANCE    0x19
#define ABS_MT_SLOT     0x2f
#define ABS_MT_POSITION_X   0x35
#define ABS_MT_POSITION_Y   0x36
#define ABS_MT_TRACKING_ID  0x39

/* 键状态 */
#define KEY_STATE_RELEASE   0
#define KEY_STATE_PRESS     1
#define KEY_STATE_REPEAT    2

/* 修饰键标志 */
#define MOD_SHIFT       (1 << 0)
#define MOD_CTRL        (1 << 1)
#define MOD_ALT         (1 << 2)
#define MOD_GUI         (1 << 3)
#define MOD_CAPSLOCK    (1 << 4)
#define MOD_NUMLOCK     (1 << 5)

/* ========== 输入事件 ========== */

struct input_event {
    uint64_t time;          /* 时间戳 (微秒) */
    uint16_t type;          /* 事件类型 */
    uint16_t code;          /* 事件代码 */
    int32_t value;          /* 事件值 */
} __attribute__((packed));

/* ========== 输入设备 ========== */

/* 输入设备能力 */
struct input_dev_caps {
    uint64_t evbits[1];     /* 支持的事件类型 */
    uint64_t keybits[KEY_CNT / 64];    /* 支持的键码 */
    uint64_t relbits[REL_CNT / 64];    /* 支持的相对轴 */
    uint64_t absbits[ABS_CNT / 64];    /* 支持的绝对轴 */
    
    int16_t abs_min[ABS_CNT];          /* 绝对轴最小值 */
    int16_t abs_max[ABS_CNT];          /* 绝对轴最大值 */
    int16_t abs_fuzz[ABS_CNT];         /* 模糊值 */
    int16_t abs_flat[ABS_CNT];         /* 平坦值 */
};

/* 输入设备 */
struct input_dev {
    char name[64];
    char phys[64];
    char uniq[64];
    
    uint16_t id_bustype;
    uint16_t id_vendor;
    uint16_t id_product;
    uint16_t id_version;
    
    int type;
    int opened;
    int enabled;
    
    /* 能力 */
    struct input_dev_caps caps;
    
    /* 当前状态 */
    uint32_t modifiers;
    int32_t abs[ABS_CNT];
    uint8_t key_state[KEY_CNT / 8];
    
    /* 驱动 */
    struct input_driver *driver;
    void *driver_data;
    
    /* 事件处理 */
    struct list_head handlers;
    
    /* 缓冲区 */
    struct ring_buffer *buffer;
    
    /* 链表 */
    struct list_head list;
};

/* 输入驱动 */
struct input_driver {
    const char *name;
    int (*probe)(struct input_dev *dev);
    void (*remove)(struct input_dev *dev);
    int (*open)(struct input_dev *dev);
    void (*close)(struct input_dev *dev);
    int (*event)(struct input_dev *dev, uint16_t type, uint16_t code, int32_t value);
};

/* 输入处理器 */
struct input_handler {
    const char *name;
    
    void (*event)(struct input_handler *handler, struct input_dev *dev,
                  struct input_event *event);
    int (*connect)(struct input_handler *handler, struct input_dev *dev);
    void (*disconnect)(struct input_handler *handler, struct input_dev *dev);
    
    struct list_head list;
};

/* ========== 键盘布局 ========== */

struct keymap {
    uint16_t normal[256];
    uint16_t shift[256];
    uint16_t altgr[256];
    uint16_t ctrl[256];
};

/* 默认 US 键盘布局 */
extern const struct keymap keymap_us;

/* ========== 输入子系统 API ========== */

/* 初始化输入子系统 */
int input_init(void);

/* 注册/注销输入设备 */
struct input_dev *input_allocate_device(void);
void input_free_device(struct input_dev *dev);
int input_register_device(struct input_dev *dev);
void input_unregister_device(struct input_dev *dev);

/* 注册/注销输入处理器 */
int input_register_handler(struct input_handler *handler);
void input_unregister_handler(struct input_handler *handler);

/* 设备能力设置 */
void input_set_capability(struct input_dev *dev, uint16_t type, uint16_t code);
void input_set_abs_params(struct input_dev *dev, uint16_t axis,
                           int min, int max, int fuzz, int flat);

/* 发送事件 */
void input_event(struct input_dev *dev, uint16_t type, uint16_t code, int32_t value);
void input_sync(struct input_dev *dev);

/* 键盘特定 API */
void input_report_key(struct input_dev *dev, uint16_t code, int value);

/* 鼠标特定 API */
void input_report_rel(struct input_dev *dev, uint16_t code, int32_t value);
void input_report_abs(struct input_dev *dev, uint16_t code, int32_t value);

/* Ring buffer API (for static device allocation) */
struct ring_buffer;
extern struct ring_buffer *ring_buffer_create(size_t size);

/* ========== 设备查找 ========== */

/* 获取输入设备 */
struct input_dev *input_get_device(int index);

/* 获取键盘设备 */
struct input_dev *input_get_keyboard(void);

/* 获取鼠标设备 */
struct input_dev *input_get_mouse(void);

/* ========== 事件读取 ========== */

/* 读取输入事件 */
int input_read_event(struct input_dev *dev, struct input_event *event);

/* 非阻塞读取 */
int input_read_event_nonblock(struct input_dev *dev, struct input_event *event);

/* 等待事件 */
int input_wait_event(struct input_dev *dev, uint64_t timeout);

/* ========== 键盘辅助函数 ========== */

/* 转换键码到字符 */
char input_keycode_to_char(struct input_dev *dev, uint16_t keycode);

/* 获取修饰键状态 */
uint32_t input_get_modifiers(struct input_dev *dev);

/* 检查键是否按下 */
int input_key_is_pressed(struct input_dev *dev, uint16_t code);

/* ========== 设备枚举 ========== */

/* REL_CNT 和 ABS_CNT 已在上方定义 (REL_CNT=0x10, ABS_CNT=0x40) */

/* 获取设备列表 */
int input_get_devices(struct input_dev **devs, int max_count);

/* 打印设备信息 */
void input_print_device(struct input_dev *dev);

#endif /* INPUT_H */
