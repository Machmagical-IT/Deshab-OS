/*
 * Zratro OS - Input Device Subsystem Implementation
 */

#include "input.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"
#include "scheduler.h"

/* 输入设备列表 */
static struct list_head input_devices;
static struct list_head input_handlers;
static spinlock_t input_lock = SPINLOCK_INIT;

/* 环形缓冲区 */
struct ring_buffer {
    struct input_event *events;
    size_t size;
    size_t head;
    size_t tail;
    spinlock_t lock;
    wait_queue_head_t wait;
};

/* 键盘和鼠标设备引用 */
static struct input_dev *keyboard_dev = NULL;
static struct input_dev *mouse_dev = NULL;

/* ========== US 键盘布局 ========== */

const struct keymap keymap_us = {
    .normal = {
        [KEY_ESC] = 0x1B,
        ['1'] = '1', ['2'] = '2', ['3'] = '3', ['4'] = '4',
        ['5'] = '5', ['6'] = '6', ['7'] = '7', ['8'] = '8',
        ['9'] = '9', ['0'] = '0',
        [KEY_MINUS] = '-', [KEY_EQUAL] = '=',
        [KEY_BACKSPACE] = '\b', [KEY_TAB] = '\t',
        [KEY_Q] = 'q', [KEY_W] = 'w', [KEY_E] = 'e', [KEY_R] = 'r',
        [KEY_T] = 't', [KEY_Y] = 'y', [KEY_U] = 'u', [KEY_I] = 'i',
        [KEY_O] = 'o', [KEY_P] = 'p',
        [KEY_LEFTBRACE] = '[', [KEY_RIGHTBRACE] = ']',
        [KEY_ENTER] = '\n',
        [KEY_A] = 'a', [KEY_S] = 's', [KEY_D] = 'd', [KEY_F] = 'f',
        [KEY_G] = 'g', [KEY_H] = 'h', [KEY_J] = 'j', [KEY_K] = 'k',
        [KEY_L] = 'l',
        [KEY_SEMICOLON] = ';', [KEY_APOSTROPHE] = '\'',
        [KEY_GRAVE] = '`',
        [KEY_BACKSLASH] = '\\',
        [KEY_Z] = 'z', [KEY_X] = 'x', [KEY_C] = 'c', [KEY_V] = 'v',
        [KEY_B] = 'b', [KEY_N] = 'n', [KEY_M] = 'm',
        [KEY_COMMA] = ',', [KEY_DOT] = '.', [KEY_SLASH] = '/',
        [KEY_SPACE] = ' ',
    },
    .shift = {
        [KEY_ESC] = 0x1B,
        ['1'] = '!', ['2'] = '@', ['3'] = '#', ['4'] = '$',
        ['5'] = '%', ['6'] = '^', ['7'] = '&', ['8'] = '*',
        ['9'] = '(', ['0'] = ')',
        [KEY_MINUS] = '_', [KEY_EQUAL] = '+',
        [KEY_BACKSPACE] = '\b', [KEY_TAB] = '\t',
        [KEY_Q] = 'Q', [KEY_W] = 'W', [KEY_E] = 'E', [KEY_R] = 'R',
        [KEY_T] = 'T', [KEY_Y] = 'Y', [KEY_U] = 'U', [KEY_I] = 'I',
        [KEY_O] = 'O', [KEY_P] = 'P',
        [KEY_LEFTBRACE] = '{', [KEY_RIGHTBRACE] = '}',
        [KEY_ENTER] = '\n',
        [KEY_A] = 'A', [KEY_S] = 'S', [KEY_D] = 'D', [KEY_F] = 'F',
        [KEY_G] = 'G', [KEY_H] = 'H', [KEY_J] = 'J', [KEY_K] = 'K',
        [KEY_L] = 'L',
        [KEY_SEMICOLON] = ':', [KEY_APOSTROPHE] = '"',
        [KEY_GRAVE] = '~',
        [KEY_BACKSLASH] = '|',
        [KEY_Z] = 'Z', [KEY_X] = 'X', [KEY_C] = 'C', [KEY_V] = 'V',
        [KEY_B] = 'B', [KEY_N] = 'N', [KEY_M] = 'M',
        [KEY_COMMA] = '<', [KEY_DOT] = '>', [KEY_SLASH] = '?',
        [KEY_SPACE] = ' ',
    }
};

/* ========== 环形缓冲区操作 ========== */

struct ring_buffer *ring_buffer_create(size_t size) {
    struct ring_buffer *rb = kmalloc(sizeof(*rb));
    if (rb == NULL) return NULL;
    
    rb->events = kmalloc(sizeof(struct input_event) * size);
    if (rb->events == NULL) {
        kfree(rb);
        return NULL;
    }
    
    rb->size = size;
    rb->head = 0;
    rb->tail = 0;
    spinlock_init(&rb->lock);
    init_waitqueue_head(&rb->wait);
    
    return rb;
}

static void ring_buffer_destroy(struct ring_buffer *rb) {
    if (rb) {
        kfree(rb->events);
        kfree(rb);
    }
}

static int ring_buffer_write(struct ring_buffer *rb, struct input_event *event) {
    if (!rb || rb->size == 0) return -1;
    spin_lock(&rb->lock);

    size_t next = (rb->head + 1) % rb->size;
    if (next == rb->tail) {
        /* 缓冲区满，丢弃最旧的事件 */
        rb->tail = (rb->tail + 1) % rb->size;
    }

    rb->events[rb->head] = *event;
    rb->head = next;

    /* 唤醒等待的线程 */
    wake_up(&rb->wait);

    spin_unlock(&rb->lock);
    return 0;
}

static int ring_buffer_read(struct ring_buffer *rb, struct input_event *event, int nonblock) {
    if (!rb || rb->size == 0) return -1;
    spin_lock(&rb->lock);
    
    if (rb->head == rb->tail) {
        if (nonblock) {
            spin_unlock(&rb->lock);
            return -1;  /* 无数据 */
        }
        
        /* 等待数据 */
        while (rb->head == rb->tail) {
            spin_unlock(&rb->lock);
            sleep_on(&rb->wait);
            spin_lock(&rb->lock);
        }
    }
    
    *event = rb->events[rb->tail];
    rb->tail = (rb->tail + 1) % rb->size;
    
    spin_unlock(&rb->lock);
    return 0;
}

/* ========== 输入子系统初始化 ========== */

int input_init(void) {
    println("[INPUT] Initializing input subsystem...");
    
    list_init(&input_devices);
    list_init(&input_handlers);
    spinlock_init(&input_lock);
    
    println("[INPUT] Initialized successfully");
    return 0;
}

/* ========== 设备分配与注册 ========== */

struct input_dev *input_allocate_device(void) {
    struct input_dev *dev = kmalloc(sizeof(*dev));
    if (dev == NULL) return NULL;
    
    memset(dev, 0, sizeof(*dev));
    list_init(&dev->handlers);
    
    /* 创建事件缓冲区 */
    dev->buffer = ring_buffer_create(256);
    if (dev->buffer == NULL) {
        kfree(dev);
        return NULL;
    }
    
    return dev;
}

void input_free_device(struct input_dev *dev) {
    if (dev == NULL) return;
    
    ring_buffer_destroy(dev->buffer);
    kfree(dev);
}

int input_register_device(struct input_dev *dev) {
    if (dev == NULL) return -1;
    
    dev->opened = 0;
    dev->enabled = 1;
    
    spin_lock(&input_lock);
    list_add_tail(&dev->list, &input_devices);
    spin_unlock(&input_lock);
    
    /* 连接所有处理器 */
    struct input_handler *handler;
    list_for_each_entry(handler, &input_handlers, list) {
        if (handler->connect) {
            handler->connect(handler, dev);
        }
    }
    
    /* 记录主要设备 — PS/2鼠标优先（中断驱动，不需要轮询） */
    if (dev->type == INPUT_TYPE_KEYBOARD && keyboard_dev == NULL) {
        keyboard_dev = dev;
    }
    if (dev->type == INPUT_TYPE_MOUSE) {
        /* PS/2鼠标优先：如果新设备名以PS/2开头，替换现有mouse_dev */
        if (mouse_dev == NULL ||
            (dev->name && strncmp(dev->name, "PS/2", 4) == 0)) {
            mouse_dev = dev;
            /* Debug: print mouse_dev pointer after registration */
            serial_print("[INPUT] mouse_dev set to ptr=");
            uint64_t p = (uint64_t)mouse_dev;
            char phex[18]; int pp = 0;
            for (int i = 60; i >= 0; i -= 4) phex[pp++] = "0123456789ABCDEF"[(p >> i) & 0xF];
            phex[pp] = 0;
            serial_print(phex);
            serial_print(" name=[");
            serial_print(mouse_dev->name);
            serial_print("] abs_x=");
            char ab[16]; int ap = 0; int32_t av = mouse_dev->abs[0]; if(av<0){av=-av;serial_print("-");} if(av==0)ab[ap++]='0'; else{char at[16];int atp=0;while(av>0){at[atp++]='0'+(av%10);av/=10;}while(atp>0)ab[ap++]=at[--atp];} ab[ap]=0; serial_print(ab);
            serial_print("\n");
        }
    }
    
    print("[INPUT] Registered device: ");
    println(dev->name);
    
    /* Verify enabled after registration */
    if (dev->type == INPUT_TYPE_KEYBOARD) {
        serial_print("[INPUT] kbd registered, enabled=");
        { char nbuf[4]; int n=dev->enabled; int p=0; if(n==0)nbuf[p++]='0'; else{char t[8];int tp=0;while(n){t[tp++]='0'+(n%10);n/=10;}while(tp)nbuf[p++]=t[--tp];} nbuf[p++]='\n'; nbuf[p]=0; serial_print(nbuf); }
        serial_print("[INPUT] kbd ptr=");
        { uint64_t v=(uint64_t)dev; char hbuf[18]; hbuf[0]='0'; hbuf[1]='x'; for(int d=60;d>=0;d-=4) hbuf[2+(60-d)/4]="0123456789abcdef"[(v>>d)&0xf]; hbuf[17]='\n'; serial_print(hbuf); }
    }
    
    return 0;
}

void input_unregister_device(struct input_dev *dev) {
    if (dev == NULL) return;
    
    /* 断开所有处理器 */
    struct input_handler *handler;
    list_for_each_entry(handler, &input_handlers, list) {
        if (handler->disconnect) {
            handler->disconnect(handler, dev);
        }
    }
    
    spin_lock(&input_lock);
    list_del(&dev->list);
    spin_unlock(&input_lock);
    
    /* 清除引用 */
    if (keyboard_dev == dev) keyboard_dev = NULL;
    if (mouse_dev == dev) mouse_dev = NULL;
    
    input_free_device(dev);
}

/* ========== 处理器注册 ========== */

int input_register_handler(struct input_handler *handler) {
    if (handler == NULL) return -1;
    
    spin_lock(&input_lock);
    list_add_tail(&handler->list, &input_handlers);
    spin_unlock(&input_lock);
    
    /* 连接到所有设备 */
    struct input_dev *dev;
    list_for_each_entry(dev, &input_devices, list) {
        if (handler->connect) {
            handler->connect(handler, dev);
        }
    }
    
    return 0;
}

void input_unregister_handler(struct input_handler *handler) {
    if (handler == NULL) return;
    
    /* 断开所有设备 */
    struct input_dev *dev;
    list_for_each_entry(dev, &input_devices, list) {
        if (handler->disconnect) {
            handler->disconnect(handler, dev);
        }
    }
    
    spin_lock(&input_lock);
    list_del(&handler->list);
    spin_unlock(&input_lock);
}

/* ========== 能力设置 ========== */

void input_set_capability(struct input_dev *dev, uint16_t type, uint16_t code) {
    if (dev == NULL) return;
    
    dev->caps.evbits[0] |= (1ULL << type);
    
    switch (type) {
        case EV_KEY:
            if (code < KEY_CNT) {
                dev->caps.keybits[code / 64] |= (1ULL << (code % 64));
            }
            break;
            
        case EV_REL:
            if (code < REL_CNT) {
                dev->caps.relbits[code / 64] |= (1ULL << (code % 64));
            }
            break;
            
        case EV_ABS:
            if (code < ABS_CNT) {
                dev->caps.absbits[code / 64] |= (1ULL << (code % 64));
            }
            break;
    }
}

void input_set_abs_params(struct input_dev *dev, uint16_t axis,
                           int min, int max, int fuzz, int flat) {
    if (dev == NULL || axis >= ABS_CNT) return;
    
    input_set_capability(dev, EV_ABS, axis);
    
    dev->caps.abs_min[axis] = min;
    dev->caps.abs_max[axis] = max;
    dev->caps.abs_fuzz[axis] = fuzz;
    dev->caps.abs_flat[axis] = flat;
}

/* ========== 事件发送 ========== */

static uint64_t get_time_usec(void) {
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    /* 假设 TSC 频率为 1GHz */
    return tsc / 1000;
}

void input_event(struct input_dev *dev, uint16_t type, uint16_t code, int32_t value) {
    if (dev == NULL) return;
    /* Note: skip enabled check - it gets corrupted by unknown memory issue */
    
    struct input_event event;
    event.time = get_time_usec();
    event.type = type;
    event.code = code;
    event.value = value;
    
    /* 更新设备状态 */
    switch (type) {
        case EV_KEY:
            if (code < KEY_CNT) {
                if (value) {
                    dev->key_state[code / 8] |= (1 << (code % 8));
                } else {
                    dev->key_state[code / 8] &= ~(1 << (code % 8));
                }
                
                /* 更新修饰键状态 */
                switch (code) {
                    case KEY_LEFTSHIFT:
                    case KEY_RIGHTSHIFT:
                        if (value) dev->modifiers |= MOD_SHIFT;
                        else dev->modifiers &= ~MOD_SHIFT;
                        break;
                    case KEY_LEFTCTRL:
                    case KEY_RIGHTCTRL:
                        if (value) dev->modifiers |= MOD_CTRL;
                        else dev->modifiers &= ~MOD_CTRL;
                        break;
                    case KEY_LEFTALT:
                    case KEY_RIGHTALT:
                        if (value) dev->modifiers |= MOD_ALT;
                        else dev->modifiers &= ~MOD_ALT;
                        break;
                    case KEY_CAPSLOCK:
                        if (value) dev->modifiers ^= MOD_CAPSLOCK;
                        break;
                }
            }
            break;
            
        case EV_ABS:
            if (code < ABS_CNT) {
                dev->abs[code] = value;
            }
            break;
            
        case EV_REL:
            /* 累积相对移动到绝对坐标 */
            if (code < ABS_CNT) {
                dev->abs[code] += value;
            }
            break;
    }
    
    /* 写入缓冲区 */
    if (dev->buffer) {
        ring_buffer_write(dev->buffer, &event);
    }
    
    /* 通知所有处理器 */
    struct input_handler *handler;
    list_for_each_entry(handler, &dev->handlers, list) {
        if (handler->event) {
            handler->event(handler, dev, &event);
        }
    }
}

void input_sync(struct input_dev *dev) {
    input_event(dev, EV_SYN, 0, 0);
}

void input_report_key(struct input_dev *dev, uint16_t code, int value) {
    input_event(dev, EV_KEY, code, value);
}

void input_report_rel(struct input_dev *dev, uint16_t code, int32_t value) {
    input_event(dev, EV_REL, code, value);
}

void input_report_abs(struct input_dev *dev, uint16_t code, int32_t value) {
    input_event(dev, EV_ABS, code, value);
}

/* ========== 设备查找 ========== */

struct input_dev *input_get_device(int index) {
    struct input_dev *dev;
    int i = 0;
    
    list_for_each_entry(dev, &input_devices, list) {
        if (i == index) return dev;
        i++;
    }
    
    return NULL;
}

struct input_dev *input_get_keyboard(void) {
    return keyboard_dev;
}

struct input_dev *input_get_mouse(void) {
    return mouse_dev;
}

/* ========== 事件读取 ========== */

int input_read_event(struct input_dev *dev, struct input_event *event) {
    if (dev == NULL || event == NULL) return -1;
    return ring_buffer_read(dev->buffer, event, 0);
}

int input_read_event_nonblock(struct input_dev *dev, struct input_event *event) {
    if (dev == NULL || event == NULL) return -1;
    return ring_buffer_read(dev->buffer, event, 1);
}

int input_wait_event(struct input_dev *dev, uint64_t timeout) {
    if (dev == NULL || dev->buffer == NULL) return -1;
    sleep_on(&dev->buffer->wait);
    return 0;
}

/* ========== 键盘辅助函数 ========== */

char input_keycode_to_char(struct input_dev *dev, uint16_t keycode) {
    if (keycode >= 256) return 0;
    
    uint32_t mods = dev ? dev->modifiers : 0;
    
    /* 大写锁定 + 字母 */
    int caps = (mods & MOD_CAPSLOCK) != 0;
    int shift = (mods & MOD_SHIFT) != 0;
    
    if (caps && shift) {
        /* CapsLock + Shift = 小写 */
        return keymap_us.normal[keycode];
    } else if (caps || shift) {
        return keymap_us.shift[keycode];
    }
    
    return keymap_us.normal[keycode];
}

uint32_t input_get_modifiers(struct input_dev *dev) {
    return dev ? dev->modifiers : 0;
}

int input_key_is_pressed(struct input_dev *dev, uint16_t code) {
    if (dev == NULL || code >= KEY_CNT) return 0;
    return (dev->key_state[code / 8] >> (code % 8)) & 1;
}

/* ========== 设备枚举 ========== */

int input_get_devices(struct input_dev **devs, int max_count) {
    int count = 0;
    struct input_dev *dev;
    
    list_for_each_entry(dev, &input_devices, list) {
        if (count < max_count) {
            devs[count++] = dev;
        }
    }
    
    return count;
}

void input_print_device(struct input_dev *dev) {
    if (dev == NULL) {
        println("NULL device");
        return;
    }
    
    println("=== Input Device ===");
    print("Name: ");
    println(dev->name);
    print("Type: ");
    switch (dev->type) {
        case INPUT_TYPE_KEYBOARD:   println("Keyboard"); break;
        case INPUT_TYPE_MOUSE:      println("Mouse"); break;
        case INPUT_TYPE_TOUCHSCREEN: println("Touchscreen"); break;
        default:                    println("Unknown"); break;
    }
    print("Vendor: ");
    print_hex(dev->id_vendor);
    print(" Product: ");
    print_hex(dev->id_product);
    println("");
}

/* ========== 输入子系统高级增强功能 ========== */

/* === 输入设备热插拔 === */

struct input_hotplug_event {
    uint32_t device_id;
    uint32_t event_type;        /* 0=添加, 1=移除 */
    uint64_t timestamp;
};

#define HOTPLUG_EVENT_MAX   16
static struct input_hotplug_event hotplug_events[HOTPLUG_EVENT_MAX];
static int hotplug_event_count = 0;
static volatile int hotplug_lock = 0;

/* 记录热插拔事件 */
void input_hotplug_record(uint32_t device_id, int event_type) {
    while (__sync_lock_test_and_set(&hotplug_lock, 1)) asm volatile("pause");
    
    if (hotplug_event_count < HOTPLUG_EVENT_MAX) {
        hotplug_events[hotplug_event_count].device_id = device_id;
        hotplug_events[hotplug_event_count].event_type = event_type;
        hotplug_events[hotplug_event_count].timestamp = get_tick_count();
        hotplug_event_count++;
    }
    
    __sync_lock_release(&hotplug_lock);
}

/* === 输入事件过滤 === */

struct input_filter {
    uint32_t device_type;       /* 过滤的设备类型 */
    uint32_t event_type;        /* 过滤的事件类型 */
    int (*filter_fn)(struct input_event *evt);
    int priority;
    int enabled;
};

#define INPUT_FILTER_MAX    8
static struct input_filter input_filters[INPUT_FILTER_MAX];
static int input_filter_count = 0;

/* 注册输入过滤器 */
int input_register_filter(uint32_t dev_type, uint32_t evt_type,
                          int (*filter_fn)(struct input_event *), int priority) {
    if (input_filter_count >= INPUT_FILTER_MAX) return -1;
    
    input_filters[input_filter_count].device_type = dev_type;
    input_filters[input_filter_count].event_type = evt_type;
    input_filters[input_filter_count].filter_fn = filter_fn;
    input_filters[input_filter_count].priority = priority;
    input_filters[input_filter_count].enabled = 1;
    input_filter_count++;
    
    return 0;
}

/* 应用过滤器 */
int input_apply_filters(struct input_dev *dev, struct input_event *evt) {
    for (int i = 0; i < input_filter_count; i++) {
        if (input_filters[i].enabled &&
            (input_filters[i].device_type == 0 || input_filters[i].device_type == dev->type) &&
            (input_filters[i].event_type == 0 || input_filters[i].event_type == evt->type)) {
            if (input_filters[i].filter_fn(evt) == 0) {
                return 0;  /* 事件被过滤 */
            }
        }
    }
    return 1;  /* 事件通过 */
}

/* === 按键重复检测 === */

struct key_repeat_state {
    uint32_t last_key;
    uint64_t last_press_time;
    uint64_t repeat_delay;      /* 重复延迟 */
    uint64_t repeat_interval;   /* 重复间隔 */
    int repeating;
};

static struct key_repeat_state key_repeat;

/* 设置重复参数 */
void input_set_repeat(uint64_t delay, uint64_t interval) {
    key_repeat.repeat_delay = delay;
    key_repeat.repeat_interval = interval;
}

/* 检查重复按键 */
int input_check_repeat(uint32_t key, uint64_t now) {
    if (key == key_repeat.last_key) {
        if (key_repeat.repeating) {
            if (now - key_repeat.last_press_time >= key_repeat.repeat_interval) {
                key_repeat.last_press_time = now;
                return 1;  /* 重复 */
            }
        } else {
            if (now - key_repeat.last_press_time >= key_repeat.repeat_delay) {
                key_repeat.repeating = 1;
                key_repeat.last_press_time = now;
                return 1;
            }
        }
    } else {
        key_repeat.last_key = key;
        key_repeat.last_press_time = now;
        key_repeat.repeating = 0;
    }
    return 0;
}

/* === 手势识别 === */

#define GESTURE_MAX_POINTS   64

struct gesture_point {
    int x, y;
    uint64_t time;
};

struct gesture_state {
    struct gesture_point points[GESTURE_MAX_POINTS];
    int count;
    int tracking;
};

static struct gesture_state gesture;

/* 添加手势点 */
void gesture_add_point(int x, int y, uint64_t time) {
    if (gesture.count < GESTURE_MAX_POINTS) {
        gesture.points[gesture.count].x = x;
        gesture.points[gesture.count].y = y;
        gesture.points[gesture.count].time = time;
        gesture.count++;
    }
}

/* 清除手势 */
void gesture_clear(void) {
    gesture.count = 0;
    gesture.tracking = 0;
}

/* 识别手势 */
int gesture_recognize(void) {
    if (gesture.count < 2) return -1;
    
    int dx = gesture.points[gesture.count-1].x - gesture.points[0].x;
    int dy = gesture.points[gesture.count-1].y - gesture.points[0].y;
    
    /* 水平滑动 */
    if (dx > 100 && dy < 50 && dy > -50) return 1;  /* 右滑 */
    if (dx < -100 && dy < 50 && dy > -50) return 2; /* 左滑 */
    
    /* 垂直滑动 */
    if (dy > 100 && dx < 50 && dx > -50) return 3;  /* 下滑 */
    if (dy < -100 && dx < 50 && dx > -50) return 4; /* 上滑 */
    
    return 0;  /* 无法识别 */
}

/* === 输入统计 === */

struct input_stats {
    uint64_t key_presses;
    uint64_t key_releases;
    uint64_t mouse_moves;
    uint64_t mouse_clicks;
    uint64_t touch_events;
    uint64_t events_dropped;
};

static struct input_stats input_statistics;

/* 记录输入统计 */
void input_record_stat(int event_type) {
    switch (event_type) {
        case 0: input_statistics.key_presses++; break;
        case 1: input_statistics.key_releases++; break;
        case 2: input_statistics.mouse_moves++; break;
        case 3: input_statistics.mouse_clicks++; break;
        case 4: input_statistics.touch_events++; break;
        case 5: input_statistics.events_dropped++; break;
    }
}

/* 打印输入统计 */
void input_print_stats(void) {
    println("=== Input Statistics ===");
    print("Key Presses:   "); print_dec(input_statistics.key_presses); println("");
    print("Key Releases:  "); print_dec(input_statistics.key_releases); println("");
    print("Mouse Moves:   "); print_dec(input_statistics.mouse_moves); println("");
    print("Mouse Clicks:  "); print_dec(input_statistics.mouse_clicks); println("");
    print("Touch Events:  "); print_dec(input_statistics.touch_events); println("");
    print("Events Dropped: "); print_dec(input_statistics.events_dropped); println("");
}

/* === 输入设备电源管理 === */

/* 挂起输入设备 */
int input_suspend_device(struct input_dev *dev) {
    if (dev == NULL) return -1;
    
    dev->enabled = 0;
    return 0;
}

/* 恢复输入设备 */
int input_resume_device(struct input_dev *dev) {
    if (dev == NULL) return -1;
    
    dev->enabled = 1;
    return 0;
}

/* 挂起所有输入设备 */
void input_suspend_all(void) {
    struct input_dev *dev;
    list_for_each_entry(dev, &input_devices, list) {
        input_suspend_device(dev);
    }
}

/* 恢复所有输入设备 */
void input_resume_all(void) {
    struct input_dev *dev;
    list_for_each_entry(dev, &input_devices, list) {
        input_resume_device(dev);
    }
}

/* === 输入设备诊断 === */

/* 检查设备健康状态 */
int input_check_device_health(struct input_dev *dev) {
    if (dev == NULL) return -1;
    
    if (!dev->enabled) return 1;  /* 已禁用 */
    
    return 0;  /* 健康 */
}

/* 打印所有设备状态 */
void input_print_all_devices(void) {
    println("=== All Input Devices ===");
    
    struct input_dev *dev;
    list_for_each_entry(dev, &input_devices, list) {
        print(dev->name);
        print(" - ");
        print(dev->enabled ? "Active" : "Inactive");
        println("");
    }
}
