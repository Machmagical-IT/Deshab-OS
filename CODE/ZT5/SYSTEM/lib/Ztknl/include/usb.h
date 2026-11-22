/*
 * Zratro OS - USB 驱动框架
 *
 * 支持的控制器类型：
 * - UHCI (USB 1.1) - Universal Host Controller Interface
 * - OHCI (USB 1.1) - Open Host Controller Interface  
 * - EHCI (USB 2.0) - Enhanced Host Controller Interface
 * - XHCI (USB 3.0) - eXtensible Host Controller Interface
 *
 * 支持的设备类：
 * - MSC (Mass Storage Class) - 大容量存储设备
 * - HID (Human Interface Device) - 键盘/鼠标
 * - CDC (Communications Device Class) - 网络设备
 */

#ifndef USB_H
#define USB_H

#include <stdint.h>
#include <stddef.h>

/* ========== USB 常量定义 ========== */

/* USB 速度类型 */
typedef enum {
    USB_SPEED_LOW   = 0,    /* USB 1.1 Low Speed (1.5 Mbps) */
    USB_SPEED_FULL  = 1,    /* USB 1.1 Full Speed (12 Mbps) */
    USB_SPEED_HIGH  = 2,    /* USB 2.0 High Speed (480 Mbps) */
    USB_SPEED_SUPER = 3,    /* USB 3.0 Super Speed (5 Gbps) */
} usb_speed_t;

/* USB 传输类型 */
typedef enum {
    USB_TRANSFER_CONTROL = 0,
    USB_TRANSFER_ISOCHRONOUS = 1,
    USB_TRANSFER_BULK = 2,
    USB_TRANSFER_INTERRUPT = 3,
} usb_transfer_type_t;

/* USB 方向 */
#define USB_DIR_OUT     0
#define USB_DIR_IN      0x80

/* USB 请求类型 */
#define USB_REQ_TYPE_STANDARD   0x00
#define USB_REQ_TYPE_CLASS      0x20
#define USB_REQ_TYPE_VENDOR     0x40
#define USB_REQ_TYPE_MASK       0x60

#define USB_REQ_RECIP_DEVICE    0x00
#define USB_REQ_RECIP_INTERFACE 0x01
#define USB_REQ_RECIP_ENDPOINT  0x02
#define USB_REQ_RECIP_OTHER     0x03
#define USB_REQ_RECIP_MASK      0x1f

/* USB 标准请求 */
#define USB_REQ_GET_STATUS      0x00
#define USB_REQ_CLEAR_FEATURE   0x01
#define USB_REQ_SET_FEATURE     0x03
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_DESCRIPTOR  0x07
#define USB_REQ_GET_CONFIG      0x08
#define USB_REQ_SET_CONFIG      0x09
#define USB_REQ_GET_INTERFACE   0x0A
#define USB_REQ_SET_INTERFACE   0x0B
#define USB_REQ_SYNCH_FRAME     0x0C

/* USB 描述符类型 */
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_DEVICE_QUAL    0x06
#define USB_DESC_OTHER_SPEED    0x07
#define USB_DESC_INTERFACE_POWER 0x08
#define USB_DESC_HID            0x21
#define USB_DESC_HID_REPORT     0x22
#define USB_DESC_CS_INTERFACE   0x24

/* USB 设备类 */
#define USB_CLASS_PER_INTERFACE     0x00
#define USB_CLASS_AUDIO             0x01
#define USB_CLASS_COMM              0x02
#define USB_CLASS_HID               0x03
#define USB_CLASS_PHYSICAL          0x05
#define USB_CLASS_STILL_IMAGE       0x06
#define USB_CLASS_PRINTER           0x07
#define USB_CLASS_MASS_STORAGE      0x08
#define USB_CLASS_HUB               0x09
#define USB_CLASS_CDC_DATA          0x0A
#define USB_CLASS_SMART_CARD        0x0B
#define USB_CLASS_CONTENT_SECURITY  0x0D
#define USB_CLASS_VIDEO             0x0E
#define USB_CLASS_WIRELESS          0xE0
#define USB_CLASS_MISC              0xEF
#define USB_CLASS_APP_SPEC          0xFE
#define USB_CLASS_VENDOR_SPEC       0xFF

/* USB 端点属性 */
#define USB_ENDPOINT_NUMBER_MASK    0x0f
#define USB_ENDPOINT_DIR_MASK       0x80
#define USB_ENDPOINT_XFERTYPE_MASK  0x03
#define USB_ENDPOINT_XFER_CONTROL   0x00
#define USB_ENDPOINT_XFER_ISOC      0x01
#define USB_ENDPOINT_XFER_BULK      0x02
#define USB_ENDPOINT_XFER_INT       0x03

/* USB 最大值 */
#define USB_MAX_ENDPOINTS       16
#define USB_MAX_INTERFACES      32
#define USB_MAX_ALTSETTINGS     8
#define USB_MAX_CONFIGS         8
#define USB_MAX_DEVICES         128
#define USB_MAX_PORTS           16

/* ========== USB 描述符结构 ========== */

/* 设备描述符 (18 bytes) */
struct usb_device_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

/* 配置描述符 (9 bytes) */
struct usb_config_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

/* 接口描述符 (9 bytes) */
struct usb_interface_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

/* 端点描述符 (7 bytes) */
struct usb_endpoint_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

/* 字符串描述符 */
struct usb_string_desc {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bString[];
} __attribute__((packed));

/* ========== USB 端点结构 ========== */

struct usb_endpoint {
    uint8_t  address;
    uint8_t  attributes;
    uint16_t max_packet_size;
    uint8_t  interval;
    uint8_t  type;          /* 传输类型 */
    uint8_t  direction;     /* 方向 */
    int      halted;        /* 是否停止 */
};

/* ========== USB 接口结构 ========== */

struct usb_interface {
    struct usb_interface_desc desc;
    struct usb_endpoint endpoints[USB_MAX_ENDPOINTS];
    int num_endpoints;
    int active;
};

/* ========== USB 设备结构 ========== */

struct usb_device {
    uint8_t  address;
    uint8_t  port;
    usb_speed_t speed;
    
    /* 设备描述符 */
    struct usb_device_desc device_desc;
    
    /* 配置 */
    struct usb_interface interfaces[USB_MAX_INTERFACES];
    int num_interfaces;
    int active_config;
    
    /* 父设备和Hub */
    struct usb_device *parent;
    int hub_port;
    
    /* 控制器指针 */
    struct usb_controller *controller;
    
    /* 驱动私有数据 */
    void *driver_data;
    
    /* 设备状态 */
    int state;
    int flags;
    
    /* 设备类信息 */
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
};

/* 设备状态 */
#define USB_DEVICE_STATE_NOT_PRESENT    0
#define USB_DEVICE_STATE_ATTACHED       1
#define USB_DEVICE_STATE_POWERED        2
#define USB_DEVICE_STATE_RECONNECTING   3
#define USB_DEVICE_STATE_UNAUTHENTICATED 4
#define USB_DEVICE_STATE_DEFAULT        5
#define USB_DEVICE_STATE_ADDRESS        6
#define USB_DEVICE_STATE_CONFIGURED     7
#define USB_DEVICE_STATE_SUSPENDED      8

/* ========== USB 控制器结构 ========== */

/* 控制器类型 */
typedef enum {
    USB_CTRL_UHCI = 0,
    USB_CTRL_OHCI = 1,
    USB_CTRL_EHCI = 2,
    USB_CTRL_XHCI = 3,
} usb_ctrl_type_t;

/* 控制器操作函数 */
struct usb_controller_ops {
    int (*init)(struct usb_controller *ctrl);
    void (*shutdown)(struct usb_controller *ctrl);
    int (*reset)(struct usb_controller *ctrl);
    
    /* 端口操作 */
    int (*port_reset)(struct usb_controller *ctrl, int port);
    int (*port_enable)(struct usb_controller *ctrl, int port);
    usb_speed_t (*port_get_speed)(struct usb_controller *ctrl, int port);
    int (*port_status)(struct usb_controller *ctrl, int port);
    
    /* 传输操作 */
    int (*submit_control)(struct usb_device *dev, 
                          uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index,
                          void *data, uint16_t length);
    int (*submit_bulk)(struct usb_device *dev, int endpoint,
                       void *data, int length, int direction);
    int (*submit_interrupt)(struct usb_device *dev, int endpoint,
                            void *data, int length);
    
    /* 设备管理 */
    int (*alloc_address)(struct usb_controller *ctrl);
    void (*free_address)(struct usb_controller *ctrl, int addr);
};

/* USB 控制器 */
struct usb_controller {
    usb_ctrl_type_t type;
    uint32_t pci_bus;
    uint32_t pci_device;
    uint32_t pci_function;
    uint64_t mmio_base;         /* MMIO 基地址 (EHCI/XHCI) */
    uint32_t io_base;           /* I/O 基地址 (UHCI/OHCI) */
    uint32_t irq;
    
    int num_ports;
    int num_devices;
    
    struct usb_device devices[USB_MAX_DEVICES];
    struct usb_device *root_hub;
    
    const struct usb_controller_ops *ops;
    void *private_data;
    
    int initialized;
};

/* ========== USB 设备驱动结构 ========== */

struct usb_device_id {
    uint16_t vendor;
    uint16_t product;
    uint32_t class_mask;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  protocol;
};

struct usb_driver {
    const char *name;
    struct usb_device_id *id_table;
    
    int (*probe)(struct usb_device *dev);
    void (*disconnect)(struct usb_device *dev);
    int (*suspend)(struct usb_device *dev);
    int (*resume)(struct usb_device *dev);
    
    struct usb_driver *next;
};

/* ========== USB 请求块 (URB) ========== */

struct usb_urb {
    struct usb_device *dev;
    int endpoint;
    int direction;
    int transfer_type;
    
    void *buffer;
    int buffer_length;
    int actual_length;
    
    uint8_t setup_packet[8];
    
    int status;
    int timeout;
    
    void (*callback)(struct usb_urb *urb);
    void *context;
    
    struct usb_urb *next;
};

/* URB 状态 */
#define URB_STATUS_PENDING      0
#define URB_STATUS_COMPLETED    1
#define URB_STATUS_ERROR        (-1)
#define URB_STATUS_TIMEOUT      (-2)
#define URB_STATUS_CANCELLED    (-3)

/* ========== USB 大容量存储 (MSC) 定义 ========== */

/* MSC 子类 */
#define MSC_SUBCLASS_RBC        0x01    /* Reduced Block Commands */
#define MSC_SUBCLASS_ATAPI      0x02    /* ATAPI */
#define MSC_SUBCLASS_QIC_157    0x03    /* QIC-157 */
#define MSC_SUBCLASS_UFI        0x04    /* UFI */
#define MSC_SUBCLASS_SFF_8070I  0x05    /* SFF-8070i */
#define MSC_SUBCLASS_SCSI       0x06    /* SCSI transparent */

/* MSC 协议 */
#define MSC_PROTOCOL_CBI_INT    0x00    /* CBI with interrupt */
#define MSC_PROTOCOL_CBI_NO_INT 0x01    /* CBI without interrupt */
#define MSC_PROTOCOL_BBB        0x50    /* Bulk-Only Transport */

/* BBB CBW (Command Block Wrapper) */
struct msc_cbw {
    uint32_t dCBWSignature;         /* 0x43425355 */
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;
    uint8_t  CBWCB[16];
} __attribute__((packed));

#define CBW_SIGNATURE    0x43425355
#define CBW_DIRECTION_IN 0x80

/* BBB CSW (Command Status Wrapper) */
struct msc_csw {
    uint32_t dCSWSignature;         /* 0x53425355 */
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;
} __attribute__((packed));

#define CSW_SIGNATURE    0x53425355
#define CSW_STATUS_OK    0x00
#define CSW_STATUS_FAILED 0x01
#define CSW_STATUS_PHASE  0x02

/* SCSI 命令 */
#define SCSI_TEST_UNIT_READY   0x00
#define SCSI_REQUEST_SENSE     0x03
#define SCSI_INQUIRY           0x12
#define SCSI_START_STOP        0x1B
#define SCSI_PREVENT_ALLOW     0x1E
#define SCSI_READ_CAPACITY     0x25
#define SCSI_READ_10           0x28
#define SCSI_WRITE_10          0x2A
#define SCSI_MODE_SENSE_6      0x1A
#define SCSI_MODE_SENSE_10     0x5A
#define SCSI_READ_FORMAT_CAPACITIES 0x23

/* SCSI 感知数据 */
struct scsi_sense {
    uint8_t  error_code;
    uint8_t  sense_key;
    uint8_t  asc;
    uint8_t  ascq;
} __attribute__((packed));

/* MSC 设备 */
struct msc_device {
    struct usb_device *usb_dev;
    
    int interface_num;
    int in_endpoint;
    int out_endpoint;
    uint16_t max_packet_size;
    
    uint64_t block_count;
    uint32_t block_size;
    int removable;
    
    uint32_t cbw_tag;
    
    char vendor[9];
    char product[17];
    char revision[5];
};

/* ========== 公共函数原型 ========== */

/* 控制器注册 */
int usb_register_controller(struct usb_controller *ctrl);
void usb_unregister_controller(struct usb_controller *ctrl);
struct usb_controller *usb_find_controller(int index);

/* 设备驱动注册 */
int usb_register_driver(struct usb_driver *driver);
void usb_unregister_driver(struct usb_driver *driver);

/* 设备枚举 */
int usb_enumerate_device(struct usb_device *dev);
int usb_get_descriptor(struct usb_device *dev, uint8_t type, 
                       uint8_t index, void *buf, uint16_t len);
int usb_set_address(struct usb_device *dev, uint8_t addr);
int usb_set_configuration(struct usb_device *dev, uint8_t config);

/* 传输函数 */
int usb_control_transfer(struct usb_device *dev,
                         uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index,
                         void *data, uint16_t length);
int usb_bulk_transfer(struct usb_device *dev, int endpoint,
                      void *data, int length, int direction);
int usb_interrupt_transfer(struct usb_device *dev, int endpoint,
                           void *data, int length);

/* 字符串获取 */
int usb_get_string_descriptor(struct usb_device *dev, int index,
                               int langid, void *buf, int len);

/* MSC 函数 */
int msc_init(void);
int msc_probe_device(struct usb_device *dev);
int msc_read_sector(struct msc_device *msc, uint64_t lba, void *buf);
int msc_write_sector(struct msc_device *msc, uint64_t lba, const void *buf);
int msc_get_capacity(struct msc_device *msc, uint64_t *blocks, uint32_t *block_size);
struct msc_device *msc_get_device(int index);
int msc_get_device_count(void);

/* HID 鼠标函数 */
int hid_mouse_init(void);
int hid_mouse_poll(void);
int hid_mouse_is_tablet(void);

/* UHCI 函数 */
int uhci_init(struct usb_controller *ctrl);

/* EHCI 函数 */
int ehci_init(struct usb_controller *ctrl);

/* XHCI 函数 */
int xhci_init(struct usb_controller *ctrl);

/* 全局初始化 */
void usb_init(void);
int usb_scan_devices(void);

/* 调试 */
void usb_print_device_info(struct usb_device *dev);

#endif /* USB_H */
