/* ps2.c - PS/2 Controller and Mouse Driver for ZratroOS */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ps2.h"
#include "input.h"
#include "idt.h"
#include "apic.h"
#include "console.h"  /* for printk */

/* Port I/O functions */
static inline uint8_t ps2_inb(uint16_t port) {
    uint8_t val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void ps2_outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* PS/2 controller status register bits */
#define PS2_STATUS_OBF 0x01  /* Output buffer full */
#define PS2_STATUS_IBF 0x02  /* Input buffer full */

/* Wait for input buffer to be empty */
static int ps2_wait_ibf(int timeout_ms) {
    for (int i = 0; i < timeout_ms * 100; i++) {
        if (!(ps2_inb(PS2_CMD_PORT) & PS2_STATUS_IBF))
            return 0;
        asm volatile("pause");
    }
    return -1; /* Timeout */
}

/* Wait for output buffer to be full */
static int ps2_wait_obf(int timeout_ms) {
    for (int i = 0; i < timeout_ms * 100; i++) {
        if (ps2_inb(PS2_CMD_PORT) & PS2_STATUS_OBF)
            return 0;
        asm volatile("pause");
    }
    return -1; /* Timeout */
}

/* Read from PS/2 data port */
static uint8_t ps2_read_data(void) {
    ps2_wait_obf(100);
    return ps2_inb(PS2_DATA_PORT);
}

/* Write to PS/2 data port */
static int ps2_write_data(uint8_t data) {
    if (ps2_wait_ibf(100) < 0)
        return -1;
    ps2_outb(PS2_DATA_PORT, data);
    return 0;
}

/* Write PS/2 command */
static int ps2_write_command(uint8_t cmd) {
    if (ps2_wait_ibf(100) < 0)
        return -1;
    ps2_outb(PS2_CMD_PORT, cmd);
    return 0;
}

/* Send command to mouse */
static int ps2_mouse_write(uint8_t cmd) {
    /* Send "write to mouse" command to controller */
    if (ps2_write_command(0xD4) < 0)
        return -1;
    /* Send data to mouse */
    return ps2_write_data(cmd);
}

/* Read response from mouse */
static uint8_t ps2_mouse_read(void) {
    return ps2_read_data();
}

/* Wait for mouse ACK */
static int ps2_mouse_wait_ack(void) {
    uint8_t ack = ps2_mouse_read();
    return (ack == 0xFA) ? 0 : -1;
}

/* Global device pointers - PS/2 mouse is statically allocated to avoid heap corruption */
static struct input_dev ps2_mouse_dev_static;
static int ps2_mouse_dev_initialized = 0;
static struct input_dev *ps2_kbd_dev = NULL;

/* Kernel key event queue - avoids missing fast key presses between polls */
#define KBD_EVT_QUEUE_SIZE 64
static struct {
    uint32_t events[KBD_EVT_QUEUE_SIZE];
    int head;
    int tail;
} kbd_evt_queue;

static void kbd_evt_push(uint32_t evt) {
    int next = (kbd_evt_queue.tail + 1) % KBD_EVT_QUEUE_SIZE;
    if (next != kbd_evt_queue.head) {
        kbd_evt_queue.events[kbd_evt_queue.tail] = evt;
        kbd_evt_queue.tail = next;
    }
}

int kbd_evt_pop(uint32_t *evt) {
    if (kbd_evt_queue.head == kbd_evt_queue.tail) return 0;
    *evt = kbd_evt_queue.events[kbd_evt_queue.head];
    kbd_evt_queue.head = (kbd_evt_queue.head + 1) % KBD_EVT_QUEUE_SIZE;
    return 1;
}

/* Getter for PS/2 mouse device - direct access bypassing input_get_mouse() */
struct input_dev *ps2_get_mouse_dev(void) {
    return ps2_mouse_dev_initialized ? &ps2_mouse_dev_static : NULL;
}

/* ========== PS/2 Keyboard Driver ========== */

/* Scan code set 1 to keycode translation table (US keyboard) */
static const uint8_t scancode_to_keycode[128] = {
    0, KEY_ESC, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6,       /* 0x00-0x07 */
    KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_TAB, /* 0x08-0x0F */
    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I,     /* 0x10-0x17 */
    KEY_O, KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_ENTER, KEY_LEFTCTRL, KEY_A, KEY_S, /* 0x18-0x1F */
    KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON, /* 0x20-0x27 */
    KEY_APOSTROPHE, KEY_GRAVE, KEY_LEFTSHIFT, KEY_BACKSLASH, KEY_Z, KEY_X, KEY_C, KEY_V, /* 0x28-0x2F */
    KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_RIGHTSHIFT, KEY_KPASTERISK, /* 0x30-0x37 */
    KEY_LEFTALT, KEY_SPACE, KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, /* 0x38-0x3F */
    KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, 0, 0, KEY_F11,     /* 0x40-0x47 */
    KEY_F12, 0, 0, 0, 0, 0, 0, 0,                               /* 0x48-0x4F */
    0, 0, 0, 0, 0, 0, 0, 0,                                     /* 0x50-0x57 */
    0, 0, 0, 0, 0, 0, 0, 0,                                     /* 0x58-0x5F */
    0, 0, 0, 0, 0, 0, 0, 0,                                     /* 0x60-0x67 */
    0, 0, 0, 0, 0, 0, 0, 0,                                     /* 0x68-0x6F */
    0, 0, 0, 0, 0, 0, 0, 0,                                     /* 0x70-0x77 */
    0, 0, 0, 0, 0, 0, 0, 0,                                     /* 0x78-0x7F */
};

/* Extended (E0-prefixed) scancode to keycode translation */
static const uint8_t ext_scancode_to_keycode[128] = {
    0, 0, 0, 0, 0, 0, 0, 0,     /* 0x00-0x07 */
    0, 0, 0, 0, 0, 0, 0, 0,     /* 0x08-0x0F */
    0, 0, 0, 0, 0, 0, 0, 0,     /* 0x10-0x17 */
    KEY_ENTER, KEY_RIGHTCTRL, 0, 0, 0, 0, 0, 0, /* 0x18-0x1F */
    0, 0, 0, 0, 0, 0, 0, 0,     /* 0x20-0x27 */
    0, 0, 0, 0, 0, 0, 0, 0,     /* 0x28-0x2F */
    0, 0, 0, 0, 0, KEY_SLASH, 0, 0, /* 0x30-0x37 */
    KEY_RIGHTALT, 0, KEY_HOME, KEY_UP, KEY_PAGEUP, 0, KEY_LEFT, 0, /* 0x38-0x3F */
    KEY_RIGHT, 0, KEY_END, KEY_DOWN, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE, 0, /* 0x40-0x47 */
    0, 0, 0, 0, 0, 0, 0, 0,     /* 0x48-0x4F */
    0, 0, 0, 0, 0, 0, 0, 0,     /* 0x50-0x57 */
    0, 0, 0, 0, 0, 0, 0, 0,     /* 0x58-0x5F */
    0, 0, 0, 0, 0, 0, 0, 0,     /* 0x60-0x67 */
    0, 0, 0, 0, 0, 0, 0, 0,     /* 0x68-0x6F */
    0, 0, 0, 0, 0, 0, 0, 0,     /* 0x70-0x77 */
    0, 0, 0, 0, 0, 0, 0, 0,     /* 0x78-0x7F */
};

/* PS/2 keyboard interrupt handler */
void ps2_keyboard_isr_handler(struct interrupt_frame *frame) {
    (void)frame;

    if (!ps2_kbd_dev) return;

    uint8_t sc = ps2_inb(PS2_DATA_PORT);
    {
        static int kbd_isr_cnt = 0;
        if (++kbd_isr_cnt <= 10) {
            serial_print("[PS2_KBD_ISR] sc=0x");
            { char h[4]; h[0]="0123456789abcdef"[sc>>4]; h[1]="0123456789abcdef"[sc&0xf]; h[2]='\n'; h[3]=0; serial_print(h); }
        }
    }

    /* Ignore ACK (0xFA) and RESEND (0xFE) responses */
    if (sc == 0xFA || sc == 0xFE) return;

    /* Handle extended scancode prefix (E0) */
    static int ext_prefix = 0;
    if (sc == 0xE0) {
        ext_prefix = 1;
        return;
    }

    /* Handle E1 prefix (Pause/Break key - skip next 2 bytes) */
    if (sc == 0xE1) {
        static int e1_skip = 0;
        e1_skip = 2;
        (void)e1_skip;
        return;
    }

    /* Set 1: bit 7 set = key release */
    int released = 0;
    if (sc & 0x80) {
        released = 1;
        sc &= 0x7F;
    }

    /* Translate scancode to keycode */
    uint16_t keycode;
    if (ext_prefix) {
        keycode = ext_scancode_to_keycode[sc];
        ext_prefix = 0;
    } else {
        keycode = scancode_to_keycode[sc];
    }

    if (keycode == 0) return; /* Unknown key */

    /* Push event to kernel queue first (before key_state update) */
    kbd_evt_push(((released ? 2 : 1) << 16) | (uint32_t)keycode);

    /* Directly update key_state to avoid input_event crash from memory corruption.
     * sys_input_poll reads key_state directly, so ring buffer and handlers are not needed. */
    if (ps2_kbd_dev && keycode < KEY_CNT) {
        int byte_idx = keycode / 8;
        int bit_idx = keycode % 8;
        if (released)
            ps2_kbd_dev->key_state[byte_idx] &= ~(1 << bit_idx);
        else
            ps2_kbd_dev->key_state[byte_idx] |= (1 << bit_idx);
        {
            static int kbd_update_cnt = 0;
            if (++kbd_update_cnt <= 6) {
                serial_print("[PS2_KBD] keycode=");
                { char tb[8]; int tp=0; int kv=keycode; if(kv==0) tb[tp++]='0'; else { while(kv){tb[tp++]='0'+(kv%10);kv/=10;} } char buf[32]; int bp=0; const char *pfx="[PS2_KBD] keycode="; while(*pfx) buf[bp++]=*pfx++; while(tp) buf[bp++]=tb[--tp]; buf[bp++]=' '; buf[bp++]=(released?'R':'P'); buf[bp++]='\n'; buf[bp]=0; serial_print(buf); }
                serial_print("[PS2_KBD] key_state[0]=0x");
                { int v=ps2_kbd_dev->key_state[0]; char h[4]; h[0]="0123456789abcdef"[v>>4]; h[1]="0123456789abcdef"[v&0xf]; h[2]='\n'; h[3]=0; serial_print(h); }
            }
        }
    }
}

/* Initialize PS/2 keyboard device */
static int ps2_keyboard_init_device(void) {
    printk("PS2: Initializing PS/2 keyboard...\n");

    ps2_kbd_dev = input_allocate_device();
    if (!ps2_kbd_dev) {
        printk("PS2: ERROR: Failed to allocate keyboard device\n");
        return -1;
    }

    const char kbd_name[] = "PS/2 Keyboard";
    for (int i = 0; i < 63 && kbd_name[i]; i++)
        ps2_kbd_dev->name[i] = kbd_name[i];
    ps2_kbd_dev->name[63] = '\0';

    ps2_kbd_dev->type = INPUT_TYPE_KEYBOARD;
    ps2_kbd_dev->id_bustype = 0x10; /* BUS_ISA */
    ps2_kbd_dev->id_vendor = 0x0001;
    ps2_kbd_dev->id_product = 0x0001;
    ps2_kbd_dev->id_version = 0x0001;

    /* Set keyboard capabilities */
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_LEFTCTRL);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_LEFTSHIFT);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_LEFTALT);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_RIGHTCTRL);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_RIGHTSHIFT);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_RIGHTALT);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_A);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_ESC);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_ENTER);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_BACKSPACE);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_TAB);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_SPACE);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_CAPSLOCK);
    /* Letters */
    for (int i = KEY_Q; i <= KEY_M; i++)
        input_set_capability(ps2_kbd_dev, EV_KEY, i);
    /* Numbers */
    for (int i = KEY_1; i <= KEY_0; i++)
        input_set_capability(ps2_kbd_dev, EV_KEY, i);
    /* Function keys */
    for (int i = KEY_F1; i <= KEY_F12; i++)
        input_set_capability(ps2_kbd_dev, EV_KEY, i);
    /* Arrow keys & navigation */
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_UP);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_DOWN);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_LEFT);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_RIGHT);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_HOME);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_END);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_INSERT);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_DELETE);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_PAGEUP);
    input_set_capability(ps2_kbd_dev, EV_KEY, KEY_PAGEDOWN);

    if (input_register_device(ps2_kbd_dev) < 0) {
        printk("PS2: Failed to register keyboard device\n");
        input_free_device(ps2_kbd_dev);
        ps2_kbd_dev = NULL;
        return -1;
    }

    printk("PS2: Keyboard device registered\n");
    return 0;
}

/* ========== PS/2 Mouse Driver ========== */

/* PS/2 mouse interrupt handler */
void ps2_mouse_isr_handler(struct interrupt_frame *frame) {
    (void)frame;

    if (!ps2_mouse_dev_initialized) return;

    /* In QEMU, the status register bit 5 may not reliably indicate data type.
     * Since this ISR is registered for IRQ12 (mouse), we always read
     * and process the data as mouse packet bytes. */

    /* PS/2 mouse sends one byte per interrupt.
     * A complete packet is 3 bytes: [flags, dx, dy]
     * We must accumulate bytes across multiple interrupts. */
    static uint8_t pkt[3];
    static int pkt_idx = 0;

    /* Read the single byte from this interrupt.
     * Don't use ps2_read_data() which calls ps2_wait_obf() —
     * the interrupt itself guarantees OBF is set. */
    uint8_t b = ps2_inb(PS2_DATA_PORT);

    /* Synchronize: byte 0 always has bit 3 set */
    if (pkt_idx == 0 && !(b & 0x08)) {
        /* Out of sync - discard and wait for a valid first byte */
        return;
    }

    pkt[pkt_idx++] = b;

    if (pkt_idx < 3) return;  /* Need more bytes */

    /* Complete packet received */
    pkt_idx = 0;

    uint8_t byte0 = pkt[0];
    int x_delta = (int8_t)pkt[1];
    int y_delta = (int8_t)pkt[2];

    /* Sign-extend if negative */
    if (byte0 & 0x10) x_delta |= ~0xFF;
    if (byte0 & 0x20) y_delta |= ~0xFF;

    /* Check for overflow */
    if (byte0 & 0x40) x_delta = 0;
    if (byte0 & 0x80) y_delta = 0;

    /* Button states */
    int left_btn   = byte0 & 1;
    int right_btn  = (byte0 >> 1) & 1;
    int middle_btn = (byte0 >> 2) & 1;

    /* Report events: invert Y because mouse Y is inverted relative to screen.
     * Apply 2x speed multiplier for better responsiveness. */
    input_report_rel(&ps2_mouse_dev_static, REL_X, x_delta * 2);
    input_report_rel(&ps2_mouse_dev_static, REL_Y, -y_delta * 2);
    input_report_key(&ps2_mouse_dev_static, BTN_LEFT, left_btn);
    input_report_key(&ps2_mouse_dev_static, BTN_RIGHT, right_btn);
    input_report_key(&ps2_mouse_dev_static, BTN_MIDDLE, middle_btn);
    input_sync(&ps2_mouse_dev_static);
}

/* PS/2 鼠标轮询（备用方案：当中断不工作时使用）
 * 检查 PS/2 控制器输出缓冲区是否有鼠标数据，如果有就读取处理 */
void ps2_mouse_poll(void) {
    if (!ps2_mouse_dev_initialized) return;

    /* 静态状态：累积3字节鼠标包 */
    static uint8_t pkt[3];
    static int pkt_idx = 0;

    /* 检查输出缓冲区是否满，且数据来自鼠标（状态寄存器 bit 5） */
    uint8_t status = ps2_inb(PS2_CMD_PORT);
    if (!(status & PS2_STATUS_OBF)) return;     /* 没有数据 */
    if (!(status & 0x20)) return;               /* 数据来自键盘，不是鼠标 */

    uint8_t b = ps2_inb(PS2_DATA_PORT);

    /* 同步：第一个字节 bit 3 必须为 1 */
    if (pkt_idx == 0 && !(b & 0x08)) return;

    pkt[pkt_idx++] = b;
    if (pkt_idx < 3) return;

    pkt_idx = 0;

    /* 解析鼠标包 */
    uint8_t byte0 = pkt[0];
    int8_t x_delta = pkt[1];
    int8_t y_delta = pkt[2];

    if (byte0 & 0x40) return;  /* 溢出，丢弃 */
    if (byte0 & 0x80) return;  /* 溢出，丢弃 */

    if (byte0 & 0x10) x_delta |= 0xFE00;  /* X sign extend */
    if (byte0 & 0x20) y_delta |= 0xFE00;  /* Y sign extend */

    int left_btn = byte0 & 1;
    int right_btn = (byte0 >> 1) & 1;
    int middle_btn = (byte0 >> 2) & 1;

    input_report_rel(&ps2_mouse_dev_static, REL_X, x_delta * 2);
    input_report_rel(&ps2_mouse_dev_static, REL_Y, -y_delta * 2);
    input_report_key(&ps2_mouse_dev_static, BTN_LEFT, left_btn);
    input_report_key(&ps2_mouse_dev_static, BTN_RIGHT, right_btn);
    input_report_key(&ps2_mouse_dev_static, BTN_MIDDLE, middle_btn);
    input_sync(&ps2_mouse_dev_static);
}

/* Initialize PS/2 mouse device */
static int ps2_mouse_init_device(void) {
    printk("PS2: Initializing PS/2 mouse...\n");
    
    /* Use statically allocated device to avoid heap corruption */
    memset(&ps2_mouse_dev_static, 0, sizeof(ps2_mouse_dev_static));
    list_init(&ps2_mouse_dev_static.handlers);
    
    /* Set device info */
    const char mouse_name[] = "PS/2 Mouse";
    for (int i = 0; i < 31 && mouse_name[i]; i++) {
        ps2_mouse_dev_static.name[i] = mouse_name[i];
    }
    ps2_mouse_dev_static.name[31] = '\0';
    
    const char mouse_phys[] = "isa0060/serio1/input0";
    for (int i = 0; i < 31 && mouse_phys[i]; i++) {
        ps2_mouse_dev_static.phys[i] = mouse_phys[i];
    }
    ps2_mouse_dev_static.phys[31] = '\0';
    
    ps2_mouse_dev_static.type = INPUT_TYPE_MOUSE;
    ps2_mouse_dev_static.id_bustype = 0x10;  /* BUS_ISA */
    ps2_mouse_dev_static.id_vendor = 0x0001;
    ps2_mouse_dev_static.id_product = 0x0002;
    ps2_mouse_dev_static.id_version = 0x0001;
    
    /* Set capabilities */
    input_set_capability(&ps2_mouse_dev_static, EV_REL, REL_X);
    input_set_capability(&ps2_mouse_dev_static, EV_REL, REL_Y);
    input_set_capability(&ps2_mouse_dev_static, EV_REL, REL_WHEEL);
    input_set_capability(&ps2_mouse_dev_static, EV_KEY, BTN_LEFT);
    input_set_capability(&ps2_mouse_dev_static, EV_KEY, BTN_RIGHT);
    input_set_capability(&ps2_mouse_dev_static, EV_KEY, BTN_MIDDLE);
    
    /* Set absolute ranges (will be updated by desktop) */
    input_set_abs_params(&ps2_mouse_dev_static, ABS_X, 0, 1280, 0, 0);
    input_set_abs_params(&ps2_mouse_dev_static, ABS_Y, 0, 800, 0, 0);

    /* Initialize cursor to screen center so it's visible immediately */
    ps2_mouse_dev_static.abs[ABS_X] = 640;
    ps2_mouse_dev_static.abs[ABS_Y] = 400;
    
    /* Create event buffer */
    ps2_mouse_dev_static.buffer = ring_buffer_create(256);
    if (!ps2_mouse_dev_static.buffer) {
        printk("PS2: Failed to create mouse event buffer\n");
        return -1;
    }
    
    /* Register device */
    if (input_register_device(&ps2_mouse_dev_static) < 0) {
        printk("PS2: Failed to register mouse device\n");
        return -1;
    }
    
    ps2_mouse_dev_initialized = 1;
    printk("PS2: Mouse device registered (static alloc at %p)\n", &ps2_mouse_dev_static);
    return 0;
}

/* Initialize PS/2 mouse hardware (called later when system is stable) */
static int ps2_mouse_init_hardware(void) {
    printk("PS2: Attempting mouse hardware initialization...\n");
    
    /* Check if PS/2 controller is responding */
    uint8_t status = ps2_inb(PS2_CMD_PORT);
    if (status == 0xFF || status == 0x00) {
        printk("PS2: PS/2 controller not responding\n");
        return -1;
    }
    
    /* Enable auxiliary (mouse) port */
    if (ps2_write_command(PS2_CMD_ENABLE_AUX) < 0) {
        printk("PS2: Failed to enable AUX port\n");
        return -1;
    }
    
    printk("PS2: Mouse port enabled, configuring device...\n");
    
    /* Reset mouse - with timeout */
    int ret = ps2_mouse_write(PS2_MOUSE_CMD_RESET);
    if (ret < 0) {
        printk("PS2: Failed to send reset command\n");
        return -1;
    }
    
    /* Wait for ACK with timeout */
    uint8_t ack = ps2_mouse_read();
    if (ack != 0xFA) {
        printk("PS2: No ACK from mouse (0x%02x)\n", ack);
        return -1;
    }
    
    /* Read ID bytes */
    uint8_t id1 = ps2_mouse_read();
    uint8_t id2 = ps2_mouse_read();
    printk("PS2: Mouse reset complete, ID: 0x%02x 0x%02x\n", id1, id2);
    
    /* Set defaults */
    ps2_mouse_write(PS2_MOUSE_CMD_SET_DEFAULT);
    ps2_mouse_wait_ack();
    
    /* Set sample rate to 100 */
    ps2_mouse_write(PS2_MOUSE_CMD_SET_RATE);
    ps2_mouse_wait_ack();
    ps2_mouse_write(100);
    ps2_mouse_wait_ack();
    
    /* Set resolution to 4 counts/mm */
    ps2_mouse_write(PS2_MOUSE_CMD_SET_RES);
    ps2_mouse_wait_ack();
    ps2_mouse_write(3); /* 4 counts/mm */
    ps2_mouse_wait_ack();
    
    /* Enable data reporting */
    ps2_mouse_write(PS2_MOUSE_CMD_ENABLE);
    if (ps2_mouse_wait_ack() < 0) {
        printk("PS2: Failed to enable mouse reporting\n");
        return -1;
    }
    
    printk("PS2: Mouse hardware initialized successfully\n");
    return 0;
}

/* Initialize PS/2 controller - Stage 1: Register device */
int ps2_init(void) {
    printk("PS2: PS/2 controller driver loaded\n");

    /* Stage 1: Initialize PS/2 controller */
    /* Disable both ports first */
    ps2_write_command(PS2_CMD_DISABLE_KBD);  /* 0xAD */
    ps2_write_command(PS2_CMD_DISABLE_AUX);  /* 0xA7 */

    /* Flush output buffer - discard any stale data */
    for (int i = 0; i < 10; i++) {
        if (ps2_inb(PS2_CMD_PORT) & PS2_STATUS_OBF)
            ps2_inb(PS2_DATA_PORT);
        else
            break;
    }

    /* Read and configure controller */
    ps2_outb(PS2_CMD_PORT, PS2_CMD_READ_CONFIG);
    uint8_t config = ps2_inb(PS2_DATA_PORT);
    printk("PS2: Controller config: 0x%02x\n", config);

    /* Enable keyboard IRQ (bit 0), mouse IRQ (bit 1) */
    /* Clear bits 4,5 to enable keyboard and mouse clocks */
    config |= 0x03;   /* IRQ1 + IRQ12 */
    config &= ~0x10;  /* Enable keyboard clock */
    config &= ~0x20;  /* Enable mouse clock */
    config &= ~0x40;  /* Disable scan code translation - use raw Set 1 */
    ps2_outb(PS2_CMD_PORT, PS2_CMD_WRITE_CONFIG);
    ps2_outb(PS2_DATA_PORT, config);

    /* Enable keyboard port */
    ps2_write_command(PS2_CMD_ENABLE_KBD);

    /* Stage 2: Register interrupt handlers BEFORE sending keyboard commands */
    extern void ps2_mouse_isr_handler(struct interrupt_frame *frame);
    extern void ps2_keyboard_isr_handler(struct interrupt_frame *frame);
    extern void isr_register_handler(uint8_t num, void (*handler)(struct interrupt_frame *));
    isr_register_handler((uint8_t)IRQ_MOUSE, ps2_mouse_isr_handler);
    isr_register_handler((uint8_t)IRQ_KEYBOARD, ps2_keyboard_isr_handler);
    printk("PS2: Interrupt handlers registered\n");

    /* Configure IOAPIC: route IRQ1 and IRQ12 */
    extern void ioapic_set_irq(uint8_t irq, uint8_t vector, uint32_t apic_id);
    extern void ioapic_unmask_irq(uint8_t irq);
    extern uint32_t irq_get_gsi(uint8_t isa_irq);
    uint32_t kbd_gsi = irq_get_gsi(1);
    uint32_t mouse_gsi = irq_get_gsi(12);
    ioapic_set_irq(kbd_gsi, IRQ_KEYBOARD, 0);
    ioapic_unmask_irq(kbd_gsi);
    ioapic_set_irq(mouse_gsi, IRQ_MOUSE, 0);
    ioapic_unmask_irq(mouse_gsi);
    printk("PS2: IOAPIC IRQ1(GSI=%d)->vec%d, IRQ12(GSI=%d)->vec%d\n", kbd_gsi, IRQ_KEYBOARD, mouse_gsi, IRQ_MOUSE);

    /* Stage 3: Initialize keyboard hardware (IRQ handlers now active) */
    /* Reset keyboard */
    ps2_write_data(0xFF);  /* Reset command */
    {
        /* Wait for ACK (0xFA) then self-test pass (0xAA) */
        uint8_t ack = ps2_read_data();
        uint8_t bat = ps2_read_data();
        printk("PS2: Keyboard reset: ACK=0x%02x BAT=0x%02x\n", ack, bat);
    }

    /* Set scan code set 1 */
    ps2_write_data(0xF0);
    ps2_read_data();  /* ACK */
    ps2_write_data(0x01);
    ps2_read_data();  /* ACK */

    /* Enable keyboard scanning */
    ps2_write_data(0xF4);
    {
        uint8_t ack = ps2_read_data();
        printk("PS2: Keyboard scanning enabled, ACK=0x%02x\n", ack);
    }

    /* Stage 4: Register devices with input subsystem */
    if (ps2_keyboard_init_device() < 0) {
        printk("PS2: Failed to register keyboard device\n");
    }

    if (ps2_mouse_init_device() < 0) {
        printk("PS2: Failed to register mouse device\n");
        return -1;
    }

    /* Stage 5: Initialize mouse hardware */
    ps2_mouse_init_hardware();

    return 0;
}
