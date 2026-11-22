/*
 * Zratro OS - USB HID Mouse Driver
 *
 * Implements USB Human Interface Device (HID) mouse support
 * to work with QEMU's USB mouse emulation
 */

#include "usb.h"
#include "input.h"
#include "console.h"
#include "heap.h"
#include "string.h"
#include "cpu.h"

/* ========== HID Report Descriptor Parsing ========== */

/* HID Report Item tags */
#define HID_ITEM_TYPE_MAIN      0
#define HID_ITEM_TYPE_GLOBAL    1
#define HID_ITEM_TYPE_LOCAL     2

/* Main items */
#define HID_MAIN_ITEM_TAG_INPUT          8
#define HID_MAIN_ITEM_TAG_OUTPUT         9
#define HID_MAIN_ITEM_TAG_COLLECTION     10
#define HID_MAIN_ITEM_TAG_END_COLLECTION 12

/* Global items */
#define HID_GLOBAL_ITEM_TAG_USAGE_PAGE   0
#define HID_GLOBAL_ITEM_TAG_LOGICAL_MIN  1
#define HID_GLOBAL_ITEM_TAG_LOGICAL_MAX  2
#define HID_GLOBAL_ITEM_TAG_PHYSICAL_MIN 3
#define HID_GLOBAL_ITEM_TAG_PHYSICAL_MAX 4
#define HID_GLOBAL_ITEM_TAG_UNIT_EXPONENT 5
#define HID_GLOBAL_ITEM_TAG_UNIT         6
#define HID_GLOBAL_ITEM_TAG_REPORT_SIZE  7
#define HID_GLOBAL_ITEM_TAG_REPORT_ID    8
#define HID_GLOBAL_ITEM_TAG_REPORT_COUNT 9

/* Local items */
#define HID_LOCAL_ITEM_TAG_USAGE        0
#define HID_LOCAL_ITEM_TAG_USAGE_MIN    1
#define HID_LOCAL_ITEM_TAG_USAGE_MAX    2

/* Usage pages */
#define HID_USAGE_PAGE_GENERIC_DESKTOP  0x01
#define HID_USAGE_PAGE_BUTTON           0x09

/* Generic desktop usages */
#define HID_USAGE_X     0x30
#define HID_USAGE_Y     0x31
#define HID_USAGE_WHEEL 0x38

/* Mouse usage */
#define HID_USAGE_POINTER   0x02

/* Collection types */
#define HID_COLLECTION_PHYSICAL     0x00
#define HID_COLLECTION_APPLICATION  0x01

/* HID report state */
struct hid_report_state {
    int report_id;
    int usage_page;
    int logical_min;
    int logical_max;
    int physical_min;
    int physical_max;
    int report_size;
    int report_count;
    int usage;
    int usage_min;
    int usage_max;
};

/* HID mouse report format */
struct hid_mouse_report {
    int8_t  buttons;    /* Button state: bit0=left, bit1=right, bit2=middle */
    int8_t  x;          /* X movement (relative) */
    int8_t  y;          /* Y movement (relative) */
    int8_t  wheel;      /* Wheel movement (relative) */
} __attribute__((packed));

/* HID mouse device structure */
struct hid_mouse_device {
    struct usb_device *usb_dev;
    struct usb_controller *controller;  /* 保存controller指针，避免udev->controller变NULL */
    uint8_t dev_address;                /* 保存设备地址 */
    uint8_t dev_port;                   /* 保存端口号 */
    usb_speed_t dev_speed;              /* 保存设备速度 */
    int interface_num;
    int interrupt_endpoint;
    uint16_t max_packet_size;
    uint8_t report_id;

    /* Report descriptor info */
    int report_size;      /* Report size in bits */
    int report_count;     /* Number of fields */
    int logical_min;
    int logical_max;

    /* Input device */
    struct input_dev *input_dev;
    int is_tablet;  /* 1=usb-tablet(绝对坐标), 0=usb-mouse(相对坐标) */

    /* Interrupt transfer buffer */
    uint8_t *report_buf;
    int report_buf_len;
};

/* 全局鼠标设备引用，供轮询使用 */
static struct hid_mouse_device *g_hid_mouse = NULL;

/* ========== HID Report Descriptor Parser ========== */

static int parse_hid_report_descriptor(struct hid_mouse_device *hid, 
                                       uint8_t *desc, int len) {
    int idx = 0;
    struct hid_report_state state;
    memset(&state, 0, sizeof(state));
    
    /* Default values */
    state.logical_min = 0;
    state.logical_max = 0;
    state.report_size = 8;
    state.report_count = 1;
    
    int found_mouse = 0;
    int in_mouse_collection = 0;
    
    while (idx < len) {
        uint8_t item = desc[idx++];
        uint8_t item_type = (item >> 2) & 0x03;
        uint8_t item_tag = (item >> 4) & 0x0F;
        uint8_t item_size = item & 0x03;
        
        uint32_t value = 0;
        if (item_size < 3) {
            int size = (item_size == 0) ? 1 : (1 << item_size);
            for (int i = 0; i < size; i++) {
                value |= (desc[idx++] << (i * 8));
            }
        }
        
        if (item_type == HID_ITEM_TYPE_GLOBAL) {
            switch (item_tag) {
                case HID_GLOBAL_ITEM_TAG_USAGE_PAGE:
                    state.usage_page = value;
                    break;
                case HID_GLOBAL_ITEM_TAG_LOGICAL_MIN:
                    state.logical_min = (int32_t)value;
                    break;
                case HID_GLOBAL_ITEM_TAG_LOGICAL_MAX:
                    state.logical_max = (int32_t)value;
                    break;
                case HID_GLOBAL_ITEM_TAG_REPORT_SIZE:
                    state.report_size = value;
                    break;
                case HID_GLOBAL_ITEM_TAG_REPORT_COUNT:
                    state.report_count = value;
                    break;
                case HID_GLOBAL_ITEM_TAG_REPORT_ID:
                    state.report_id = value;
                    break;
            }
        } else if (item_type == HID_ITEM_TYPE_LOCAL) {
            switch (item_tag) {
                case HID_LOCAL_ITEM_TAG_USAGE:
                    state.usage = value;
                    break;
                case HID_LOCAL_ITEM_TAG_USAGE_MIN:
                    state.usage_min = value;
                    break;
                case HID_LOCAL_ITEM_TAG_USAGE_MAX:
                    state.usage_max = value;
                    break;
            }
        } else if (item_type == HID_ITEM_TYPE_MAIN) {
            switch (item_tag) {
                case HID_MAIN_ITEM_TAG_COLLECTION:
                    if (value == HID_COLLECTION_APPLICATION &&
                        state.usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP &&
                        state.usage == HID_USAGE_POINTER) {
                        in_mouse_collection = 1;
                    }
                    break;
                    
                case HID_MAIN_ITEM_TAG_END_COLLECTION:
                    in_mouse_collection = 0;
                    break;
                    
                case HID_MAIN_ITEM_TAG_INPUT:
                    if (in_mouse_collection) {
                        found_mouse = 1;
                        /* Store report descriptor info for mouse */
                        hid->logical_min = state.logical_min;
                        hid->logical_max = state.logical_max;
                        hid->report_size = state.report_size;
                        hid->report_count = state.report_count;
                    }
                    break;
            }
        }
    }
    
    return found_mouse ? 0 : -1;
}

/* ========== HID Mouse Interrupt Handler ========== */

static void hid_mouse_interrupt_callback(struct usb_device *dev,
                                         uint8_t *data, int len) {
    struct hid_mouse_device *hid = (struct hid_mouse_device *)dev->driver_data;
    if (!hid || !hid->input_dev) return;

    if (len < 3) return;

    /* Report button states */
    uint8_t buttons = data[0];
    input_report_key(hid->input_dev, BTN_LEFT, buttons & 0x01);
    input_report_key(hid->input_dev, BTN_RIGHT, buttons & 0x02);
    input_report_key(hid->input_dev, BTN_MIDDLE, buttons & 0x04);

    if (len >= 6) {
        /* usb-tablet 发送绝对坐标：buttons(1) + X_lo(1) + X_hi(1) + Y_lo(1) + Y_hi(1) + wheel(1) */
        hid->is_tablet = 1;
        uint16_t abs_x = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
        uint16_t abs_y = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
        input_report_abs(hid->input_dev, ABS_X, abs_x);
        input_report_abs(hid->input_dev, ABS_Y, abs_y);
    } else {
        /* usb-mouse 发送相对坐标：buttons(1) + rel_x(1) + rel_y(1) */
        int8_t x = (int8_t)data[1];
        int8_t y = (int8_t)data[2];
        input_report_rel(hid->input_dev, REL_X, x);
        input_report_rel(hid->input_dev, REL_Y, y);
    }

    /* Report wheel */
    int8_t wheel = (len >= 6) ? (int8_t)data[5] : (len >= 4) ? (int8_t)data[3] : 0;
    if (wheel != 0) {
        input_report_rel(hid->input_dev, REL_WHEEL, -wheel);
    }

    /* Sync event */
    input_sync(hid->input_dev);
}

/* ========== HID Mouse Driver Functions ========== */

/* Get HID report descriptor */
static int hid_get_report_descriptor(struct usb_device *dev, 
                                     uint8_t *buf, uint16_t len) {
    return usb_control_transfer(dev,
                                USB_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_INTERFACE,
                                USB_REQ_GET_DESCRIPTOR,
                                (USB_DESC_HID_REPORT << 8) | 0,
                                0,
                                buf, len);
}

/* Set HID idle time */
static int hid_set_idle(struct usb_device *dev, int interface, 
                        uint8_t duration, uint8_t report_id) {
    return usb_control_transfer(dev,
                                USB_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE,
                                0x0A,  /* SET_IDLE */
                                (duration << 8) | report_id,
                                interface,
                                NULL, 0);
}

/* Set HID protocol */
static int hid_set_protocol(struct usb_device *dev, int interface,
                            uint8_t protocol) {
    return usb_control_transfer(dev,
                                USB_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE,
                                0x03,  /* SET_PROTOCOL */
                                protocol,
                                interface,
                                NULL, 0);
}

/* ========== 鼠标轮询函数 ========== */

int hid_mouse_poll(void) {
    if (!g_hid_mouse || !g_hid_mouse->usb_dev || !g_hid_mouse->controller) {
        return 0;
    }

    /* USB HID 鼠标/tablet: 使用控制传输(GET_REPORT)轮询当前坐标
     * EHCI 周期调度的中断传输对全速设备不可靠，改用控制传输 */
    struct usb_device *udev = g_hid_mouse->usb_dev;

    /* 恢复 probe 时保存的关键字段（可能被其他代码清零） */
    udev->controller = g_hid_mouse->controller;
    if (udev->address == 0) udev->address = g_hid_mouse->dev_address;
    if (udev->speed == 0) udev->speed = g_hid_mouse->dev_speed;

    if (!udev->controller->ops || !udev->controller->ops->submit_control) {
        return 0;
    }

    uint8_t buf[8];
    /* GET_REPORT: HID class request, Report Type=1(Input), Report ID=0 */
    int ret = usb_control_transfer(udev,
                                   USB_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE,
                                   0x01,  /* GET_REPORT */
                                   (1 << 8) | 0,  /* Input report, ID=0 */
                                   g_hid_mouse->interface_num,
                                   buf, g_hid_mouse->max_packet_size);

    if (ret >= 3) {
        hid_mouse_interrupt_callback(udev, buf, ret);
    }

    return ret;
}

/* 获取 USB HID 鼠标的输入设备（供 sys_input_poll 使用） */
struct input_dev *hid_mouse_get_input_dev(void) {
    if (!g_hid_mouse) return NULL;
    return g_hid_mouse->input_dev;
}

/* 检查当前HID鼠标是否为tablet设备（绝对坐标） */
int hid_mouse_is_tablet(void) {
    if (!g_hid_mouse) return 0;
    return g_hid_mouse->is_tablet;
}

/* HID mouse probe function */
static int hid_mouse_probe(struct usb_device *dev) {
    struct hid_mouse_device *hid;
    int ret;
    
    /* Find HID interface with interrupt endpoint */
    int iface_num = -1;
    int interrupt_ep = -1;
    uint16_t max_packet = 8;
    int found_interrupt = 0;
    
    for (int i = 0; i < dev->num_interfaces; i++) {
        struct usb_interface *iface = &dev->interfaces[i];
        
        /* Check if this is a HID interface */
        if (iface->desc.bInterfaceClass == USB_CLASS_HID) {
            iface_num = i;
            
            /* Look for interrupt IN endpoint */
            for (int j = 0; j < iface->num_endpoints; j++) {
                struct usb_endpoint *ep = &iface->endpoints[j];
                
                if (ep->type == USB_ENDPOINT_XFER_INT && ep->direction) {
                    interrupt_ep = ep->address;
                    max_packet = ep->max_packet_size;
                    found_interrupt = 1;
                    break;
                }
            }
            break;
        }
    }
    
    if (!found_interrupt || iface_num < 0) {
        return -1;
    }
    
    /* Allocate HID mouse device */
    hid = kmalloc(sizeof(*hid));
    if (!hid) {
        return -1;
    }
    memset(hid, 0, sizeof(*hid));
    
    hid->usb_dev = dev;
    hid->controller = dev->controller;  /* 保存controller，避免后续udev->controller变NULL */
    hid->dev_address = dev->address;
    hid->dev_port = dev->port;
    hid->dev_speed = dev->speed;
    hid->interface_num = iface_num;
    hid->interrupt_endpoint = interrupt_ep;
    hid->max_packet_size = max_packet;
    
    /* Allocate report buffer */
    hid->report_buf_len = max_packet;
    hid->report_buf = kmalloc(hid->report_buf_len);
    if (!hid->report_buf) {
        kfree(hid);
        return -1;
    }
    memset(hid->report_buf, 0, hid->report_buf_len);
    
    /* Get and parse HID report descriptor */
    uint8_t report_desc[256];
    ret = hid_get_report_descriptor(dev, report_desc, sizeof(report_desc));
    if (ret >= 0) {
        parse_hid_report_descriptor(hid, report_desc, ret);
    }
    
    /* Set HID idle time to infinite (report only on state change) */
    hid_set_idle(dev, hid->interface_num, 0, 0);
    
    /* Set HID boot protocol */
    hid_set_protocol(dev, hid->interface_num, 0x01);  /* Boot protocol */
    
    /* Create input device */
    struct input_dev *input = input_allocate_device();
    if (!input) {
        kfree(hid->report_buf);
        kfree(hid);
        return -1;
    }
    
    /* Configure input device */
    input->type = INPUT_TYPE_MOUSE;
    
    /* Copy USB device strings */
    input->id_vendor = dev->device_desc.idVendor;
    input->id_product = dev->device_desc.idProduct;
    input->id_bustype = 0x0003;  /* BUS_USB */
    
    /* Set name */
    char name[64];
    int name_len = 0;
    const char prefix[] = "USB HID Mouse ";
    for (int i = 0; prefix[i] && i < sizeof(name) - 1; i++) {
        name[name_len++] = prefix[i];
    }
    /* Add vendor/product ID in hex */
    const char hex[] = "0123456789ABCDEF";
    name[name_len++] = '[';
    name[name_len++] = hex[(dev->device_desc.idVendor >> 12) & 0xF];
    name[name_len++] = hex[(dev->device_desc.idVendor >> 8) & 0xF];
    name[name_len++] = hex[(dev->device_desc.idVendor >> 4) & 0xF];
    name[name_len++] = hex[dev->device_desc.idVendor & 0xF];
    name[name_len++] = ':';
    name[name_len++] = hex[(dev->device_desc.idProduct >> 12) & 0xF];
    name[name_len++] = hex[(dev->device_desc.idProduct >> 8) & 0xF];
    name[name_len++] = hex[(dev->device_desc.idProduct >> 4) & 0xF];
    name[name_len++] = hex[dev->device_desc.idProduct & 0xF];
    name[name_len++] = ']';
    name[name_len] = '\0';
    
    for (int i = 0; i < name_len && i < sizeof(input->name) - 1; i++) {
        input->name[i] = name[i];
    }
    input->name[name_len] = '\0';
    
    /* Set capabilities */
    input_set_capability(input, EV_KEY, BTN_LEFT);

    /* Detect tablet by vendor/product ID:
     * QEMU USB Tablet: 0627:0001 (Adomax)
     * Other common: 0eef:0001 (eGalax) */
    if (dev->device_desc.idVendor == 0x0627 && dev->device_desc.idProduct == 0x0001) {
        hid->is_tablet = 1;
    }
    /* Also set ABS capabilities for tablet */
    if (hid->is_tablet) {
        input_set_capability(input, EV_ABS, ABS_X);
        input_set_capability(input, EV_ABS, ABS_Y);
    }
    input_set_capability(input, EV_KEY, BTN_RIGHT);
    input_set_capability(input, EV_KEY, BTN_MIDDLE);
    input_set_capability(input, EV_REL, REL_X);
    input_set_capability(input, EV_REL, REL_Y);
    input_set_capability(input, EV_REL, REL_WHEEL);
    
    /* Register input device */
    ret = input_register_device(input);
    if (ret < 0) {
        input_free_device(input);
        kfree(hid->report_buf);
        kfree(hid);
        return -1;
    }
    
    hid->input_dev = input;

    /* Store pointer in USB device */
    dev->driver_data = hid;

    /* 保存全局引用，供hid_mouse_poll()使用 */
    g_hid_mouse = hid;

    /* Print info */
    print("[HID_MOUSE] Found USB mouse: ");
    print(input->name);
    print(" (ep=");
    print_hex(hid->interrupt_endpoint);
    print(" maxpkt=");
    print_dec(hid->max_packet_size);
    println(")");
    
    return 0;
}

/* HID mouse disconnect function */
static void hid_mouse_disconnect(struct usb_device *dev) {
    struct hid_mouse_device *hid = (struct hid_mouse_device *)dev->driver_data;
    if (!hid) return;
    
    /* Unregister input device */
    if (hid->input_dev) {
        input_unregister_device(hid->input_dev);
    }
    
    /* Free resources */
    if (hid->report_buf) {
        kfree(hid->report_buf);
    }
    kfree(hid);
    dev->driver_data = NULL;
    
    print("[HID_MOUSE] Disconnected");
    println("");
}

/* HID mouse suspend function */
static int hid_mouse_suspend(struct usb_device *dev) {
    /* TODO: Implement suspend */
    return 0;
}

/* HID mouse resume function */
static int hid_mouse_resume(struct usb_device *dev) {
    /* TODO: Implement resume */
    return 0;
}

/* HID mouse driver ID table */
static struct usb_device_id hid_mouse_id_table[] = {
    { .class_code = USB_CLASS_HID, .subclass = 0x01, .protocol = 0x02 },  /* Boot interface mouse */
    { .class_code = USB_CLASS_HID, .subclass = 0x01, .protocol = 0x00 },  /* Boot interface */
    { .class_code = USB_CLASS_HID, .subclass = 0, .protocol = 0 },        /* Any HID */
    { 0 }
};

/* HID mouse driver structure */
static struct usb_driver hid_mouse_driver = {
    .name = "usb-hid-mouse",
    .id_table = hid_mouse_id_table,
    .probe = hid_mouse_probe,
    .disconnect = hid_mouse_disconnect,
    .suspend = hid_mouse_suspend,
    .resume = hid_mouse_resume,
};

/* ========== Initialization ========== */

int hid_mouse_init(void) {
    print("[HID_MOUSE] Initializing USB HID mouse driver...");
    println("");
    return usb_register_driver(&hid_mouse_driver);
}
