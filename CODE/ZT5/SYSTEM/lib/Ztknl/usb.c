/*
 * Zratro OS - USB 核心驱动
 *
 * 实现 USB 设备枚举、驱动匹配等核心功能
 */

#include "usb.h"
#include "console.h"
#include "heap.h"
#include "string.h"
#include "cpu.h"

/* ========== 内部辅助函数 ========== */

static void delay_ms(int ms) {
    uint64_t tsc_start, tsc_end;
    uint64_t tsc_freq = cpu_get_tsc_frequency();
    if (tsc_freq == 0) tsc_freq = 1000000000ULL;  /* 默认 1GHz */
    
    asm volatile("rdtsc" : "=A"(tsc_start));
    uint64_t wait = (tsc_freq * ms) / 1000;
    do {
        asm volatile("rdtsc" : "=A"(tsc_end));
    } while ((tsc_end - tsc_start) < wait);
}

/* ========== 全局变量 ========== */

/* 控制器列表 */
#define MAX_CONTROLLERS 8
static struct usb_controller *controllers[MAX_CONTROLLERS];
static int controller_count = 0;

/* 驱动列表 */
static struct usb_driver *driver_list = NULL;

/* MSC 设备列表 */
#define MAX_MSC_DEVICES 8
static struct msc_device msc_devices[MAX_MSC_DEVICES];
static int msc_device_count = 0;

/* ========== 控制器管理 ========== */

int usb_register_controller(struct usb_controller *ctrl) {
    if (controller_count >= MAX_CONTROLLERS) {
        return -1;
    }
    
    controllers[controller_count++] = ctrl;
    return 0;
}

void usb_unregister_controller(struct usb_controller *ctrl) {
    for (int i = 0; i < controller_count; i++) {
        if (controllers[i] == ctrl) {
            /* 移动后续控制器 */
            for (int j = i; j < controller_count - 1; j++) {
                controllers[j] = controllers[j + 1];
            }
            controller_count--;
            return;
        }
    }
}

struct usb_controller *usb_find_controller(int index) {
    if (index < 0 || index >= controller_count) {
        return NULL;
    }
    return controllers[index];
}

/* ========== 驱动管理 ========== */

int usb_register_driver(struct usb_driver *driver) {
    driver->next = driver_list;
    driver_list = driver;
    
    /* 匹配已有设备 */
    for (int c = 0; c < controller_count; c++) {
        struct usb_controller *ctrl = controllers[c];
        for (int d = 0; d < USB_MAX_DEVICES; d++) {
            struct usb_device *dev = &ctrl->devices[d];
            if (dev->state >= USB_DEVICE_STATE_ADDRESS) {
                /* 检查驱动匹配 */
                struct usb_device_id *id = driver->id_table;
                while (id && id->class_code) {
                    if ((id->vendor == 0 || id->vendor == dev->device_desc.idVendor) &&
                        (id->product == 0 || id->product == dev->device_desc.idProduct) &&
                        (id->class_code == 0 || id->class_code == dev->device_desc.bDeviceClass)) {
                        if (driver->probe && driver->probe(dev) == 0) {
                            dev->driver_data = driver;
                        }
                        break;
                    }
                    id++;
                }
            }
        }
    }
    
    return 0;
}

void usb_unregister_driver(struct usb_driver *driver) {
    struct usb_driver **pp = &driver_list;
    while (*pp) {
        if (*pp == driver) {
            *pp = driver->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ========== 控制传输 ========== */

int usb_control_transfer(struct usb_device *dev,
                         uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index,
                         void *data, uint16_t length) {
    if (!dev || !dev->controller || !dev->controller->ops ||
        !dev->controller->ops->submit_control) {
        return -1;
    }
    
    return dev->controller->ops->submit_control(dev, request_type, request,
                                                 value, index, data, length);
}

/* ========== 批量传输 ========== */

int usb_bulk_transfer(struct usb_device *dev, int endpoint,
                      void *data, int length, int direction) {
    if (!dev || !dev->controller || !dev->controller->ops ||
        !dev->controller->ops->submit_bulk) {
        return -1;
    }
    
    return dev->controller->ops->submit_bulk(dev, endpoint, data, length, direction);
}

/* ========== 中断传输 ========== */

int usb_interrupt_transfer(struct usb_device *dev, int endpoint,
                           void *data, int length) {
    if (!dev || !dev->controller || !dev->controller->ops ||
        !dev->controller->ops->submit_interrupt) {
        return -1;
    }
    
    return dev->controller->ops->submit_interrupt(dev, endpoint, data, length);
}

/* ========== 设备枚举 ========== */

int usb_get_descriptor(struct usb_device *dev, uint8_t type,
                       uint8_t index, void *buf, uint16_t len) {
    return usb_control_transfer(dev,
                                USB_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE,
                                USB_REQ_GET_DESCRIPTOR,
                                (type << 8) | index,
                                0,
                                buf, len);
}

int usb_set_address(struct usb_device *dev, uint8_t addr) {
    int ret = usb_control_transfer(dev,
                                   USB_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE,
                                   USB_REQ_SET_ADDRESS,
                                   addr,
                                   0,
                                   NULL, 0);
    
    if (ret == 0) {
        dev->address = addr;
        dev->state = USB_DEVICE_STATE_ADDRESS;
    }
    
    return ret;
}

int usb_set_configuration(struct usb_device *dev, uint8_t config) {
    int ret = usb_control_transfer(dev,
                                   USB_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE,
                                   USB_REQ_SET_CONFIG,
                                   config,
                                   0,
                                   NULL, 0);
    
    if (ret == 0) {
        dev->active_config = config;
        dev->state = USB_DEVICE_STATE_CONFIGURED;
    }
    
    return ret;
}

/* 获取字符串描述符 */
int usb_get_string_descriptor(struct usb_device *dev, int index,
                               int langid, void *buf, int len) {
    return usb_control_transfer(dev,
                                USB_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE,
                                USB_REQ_GET_DESCRIPTOR,
                                (USB_DESC_STRING << 8) | index,
                                langid,
                                buf, len);
}

/* 解析配置描述符 */
static int parse_config_descriptor(struct usb_device *dev, 
                                   uint8_t *data, int len) {
    int offset = 0;
    int iface_idx = 0;
    int ep_idx = 0;
    struct usb_interface *current_iface = NULL;
    
    while (offset < len) {
        uint8_t desc_len = data[offset];
        uint8_t desc_type = data[offset + 1];
        
        if (desc_len == 0) break;
        
        switch (desc_type) {
            case USB_DESC_CONFIG: {
                struct usb_config_desc *cfg = (struct usb_config_desc *)(data + offset);
                dev->num_interfaces = cfg->bNumInterfaces;
                break;
            }
            
            case USB_DESC_INTERFACE: {
                struct usb_interface_desc *if_desc = 
                    (struct usb_interface_desc *)(data + offset);
                
                if (iface_idx < USB_MAX_INTERFACES) {
                    current_iface = &dev->interfaces[iface_idx];
                    current_iface->desc = *if_desc;
                    current_iface->num_endpoints = 0;
                    current_iface->active = 1;
                    iface_idx++;
                    ep_idx = 0;
                }
                break;
            }
            
            case USB_DESC_ENDPOINT: {
                struct usb_endpoint_desc *ep_desc =
                    (struct usb_endpoint_desc *)(data + offset);
                
                if (current_iface && ep_idx < USB_MAX_ENDPOINTS) {
                    struct usb_endpoint *ep = 
                        &current_iface->endpoints[current_iface->num_endpoints];
                    
                    ep->address = ep_desc->bEndpointAddress;
                    ep->attributes = ep_desc->bmAttributes;
                    ep->max_packet_size = ep_desc->wMaxPacketSize;
                    ep->interval = ep_desc->bInterval;
                    ep->type = ep_desc->bmAttributes & 0x03;
                    ep->direction = (ep_desc->bEndpointAddress & 0x80) ? 1 : 0;
                    
                    current_iface->num_endpoints++;
                    ep_idx++;
                }
                break;
            }
        }
        
        offset += desc_len;
    }
    
    dev->num_interfaces = iface_idx;
    return 0;
}

/* 设备枚举 */
int usb_enumerate_device(struct usb_device *dev) {
    uint8_t buf[256];
    int ret;
    int retries;
    
    print("[USB_ENUM] Starting enumeration for device at address 0...");
    println("");
    
    /* 1. 获取设备描述符 (前8字节，获取最大包大小) */
    retries = 5;
    while (retries-- > 0) {
        print("[USB_ENUM] Getting device descriptor, retries left: ");
        print_dec(retries + 1);
        println("");
        ret = usb_get_descriptor(dev, USB_DESC_DEVICE, 0, buf, 8);
        print("[USB_ENUM] usb_get_descriptor returned: ");
        {
            char tmp[16]; int p = 0;
            if (ret < 0) { tmp[p++] = '-'; ret = -ret; }
            if (ret == 0) tmp[p++] = '0';
            else { char t[16]; int tp = 0; while (ret > 0) { t[tp++] = '0' + (ret % 10); ret /= 10; } while (tp > 0) tmp[p++] = t[--tp]; }
            tmp[p] = '\0';
            print(tmp);
        }
        println("");
        if (ret >= 0) break;
        delay_ms(50);
    }
    if (ret < 0) {
        print("[USB_ENUM] Failed to get device descriptor");
        println("");
        return -1;
    }
    
    /* 更新最大包大小 */
    struct usb_device_desc *desc = (struct usb_device_desc *)buf;
    dev->device_desc.bMaxPacketSize0 = desc->bMaxPacketSize0;
    
    /* 2. 分配地址 */
    int addr = dev->controller->ops->alloc_address(dev->controller);
    if (addr < 0) {
        return -1;
    }
    
    ret = usb_set_address(dev, addr);
    if (ret < 0) {
        return -1;
    }
    
    /* 等待地址设置生效 */
    delay_ms(10);
    
    /* 3. 获取完整设备描述符 */
    retries = 3;
    while (retries-- > 0) {
        ret = usb_get_descriptor(dev, USB_DESC_DEVICE, 0, 
                                 &dev->device_desc, sizeof(struct usb_device_desc));
        if (ret >= 0) break;
        delay_ms(50);
    }
    if (ret < 0) {
        return -1;
    }
    
    // print("[USB] Device: Vendor=");
    print_hex(dev->device_desc.idVendor);
    // print(" Product=");
    print_hex(dev->device_desc.idProduct);
    // print(" Class=");
    print_hex(dev->device_desc.bDeviceClass);
    // println("");
    
    /* 4. 获取配置描述符 */
    ret = usb_get_descriptor(dev, USB_DESC_CONFIG, 0, buf, sizeof(buf));
    if (ret < 0) {
        // println("[USB] Failed to get config descriptor");
        return -1;
    }
    
    struct usb_config_desc *cfg = (struct usb_config_desc *)buf;
    uint16_t total_len = cfg->wTotalLength;
    
    /* 获取完整配置描述符 */
    uint8_t *config_buf = kmalloc(total_len);
    if (!config_buf) {
        return -1;
    }
    
    ret = usb_get_descriptor(dev, USB_DESC_CONFIG, 0, config_buf, total_len);
    if (ret < 0) {
        kfree(config_buf);
        return -1;
    }
    
    /* 解析配置描述符 */
    parse_config_descriptor(dev, config_buf, total_len);
    kfree(config_buf);
    
    /* 5. 设置配置 */
    ret = usb_set_configuration(dev, 1);
    if (ret < 0) {
        // println("[USB] Failed to set configuration");
        return -1;
    }
    
    dev->state = USB_DEVICE_STATE_CONFIGURED;
    
    /* 6. 匹配驱动 */
    struct usb_driver *drv = driver_list;
    while (drv) {
        struct usb_device_id *id = drv->id_table;
        while (id && id->class_code) {
            /* 检查设备类匹配 */
            int match = 0;
            
            /* 首先检查设备级别的类（适用于专用设备如MSC） */
            if (id->vendor != 0 && id->vendor != dev->device_desc.idVendor) {
                id++;
                continue;
            }
            if (id->product != 0 && id->product != dev->device_desc.idProduct) {
                id++;
                continue;
            }
            
            /* 检查设备类或接口类 */
            if (dev->device_desc.bDeviceClass != 0) {
                /* 设备级别的类（如MSC） */
                if (id->class_code == dev->device_desc.bDeviceClass) {
                    match = 1;
                }
            } else {
                /* 接口级别的类（如HID）- 检查所有接口 */
                for (int i = 0; i < dev->num_interfaces; i++) {
                    if (dev->interfaces[i].desc.bInterfaceClass == id->class_code) {
                        /* 如果指定了subclass，也需要匹配 */
                        if (id->subclass != 0 && 
                            dev->interfaces[i].desc.bInterfaceSubClass != id->subclass) {
                            continue;
                        }
                        match = 1;
                        break;
                    }
                }
            }
            
            if (match && drv->probe) {
                ret = drv->probe(dev);
                if (ret == 0) {
                    dev->driver_data = drv;
                    break;
                }
            }
            id++;
        }
        
        if (dev->driver_data) break;
        drv = drv->next;
    }
    
    return 0;
}

/* ========== MSC (大容量存储) 驱动 ========== */

/* MSC 驱动 ID 表 */
static struct usb_device_id msc_id_table[] = {
    { .class_code = USB_CLASS_MASS_STORAGE, .subclass = MSC_SUBCLASS_SCSI, .protocol = MSC_PROTOCOL_BBB },
    { .class_code = USB_CLASS_MASS_STORAGE, .subclass = MSC_SUBCLASS_SCSI, .protocol = 0 },
    { .class_code = USB_CLASS_MASS_STORAGE, .subclass = 0, .protocol = 0 },
    { 0 }
};

/* 最大重试次数 */
#define MSC_MAX_RETRIES     3
#define MSC_RETRY_DELAY_MS  50

/* MSC 错误恢复 - 重置设备 */
static int msc_reset_recovery(struct msc_device *msc) {
    /* 发送 Bulk-Only Mass Storage Reset */
    int ret = usb_control_transfer(msc->usb_dev,
                                   USB_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE,
                                   0xFF,  /* BMRESET */
                                   0,
                                   msc->interface_num,
                                   NULL, 0);
    if (ret < 0) {
        // println("[MSC] Reset recovery failed");
        return -1;
    }
    
    /* 清除 IN 端点停止 */
    usb_control_transfer(msc->usb_dev,
                         USB_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_ENDPOINT,
                         0x02,  /* CLEAR_FEATURE */
                         0,     /* ENDPOINT_HALT */
                         msc->in_endpoint | 0x80,
                         NULL, 0);
    
    /* 清除 OUT 端点停止 */
    usb_control_transfer(msc->usb_dev,
                         USB_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_ENDPOINT,
                         0x02,
                         0,
                         msc->out_endpoint,
                         NULL, 0);
    
    // println("[MSC] Reset recovery completed");
    return 0;
}

/* 发送 SCSI 命令 (带重试) */
static int msc_send_command(struct msc_device *msc, uint8_t *cdb, 
                            int cdb_len, void *data, int data_len,
                            int direction) {
    struct msc_cbw cbw;
    struct msc_csw csw;
    int ret;
    int retries = 0;
    
    while (retries < MSC_MAX_RETRIES) {
        /* 构建 CBW */
        memset(&cbw, 0, sizeof(cbw));
        cbw.dCBWSignature = CBW_SIGNATURE;
        cbw.dCBWTag = ++msc->cbw_tag;
        cbw.dCBWDataTransferLength = data_len;
        cbw.bmCBWFlags = direction ? CBW_DIRECTION_IN : 0;
        cbw.bCBWLUN = 0;
        cbw.bCBWCBLength = cdb_len;
        memcpy(cbw.CBWCB, cdb, cdb_len);
        
        /* 发送 CBW */
        ret = usb_bulk_transfer(msc->usb_dev, msc->out_endpoint,
                                &cbw, sizeof(cbw), USB_DIR_OUT);
        if (ret < 0) {
            // println("[MSC] Failed to send CBW, retrying...");
            retries++;
            if (retries < MSC_MAX_RETRIES) {
                msc_reset_recovery(msc);
                /* 短暂延迟 */
                for (volatile int i = 0; i < MSC_RETRY_DELAY_MS * 1000; i++);
            }
            continue;
        }
        
        /* 数据传输 */
        if (data_len > 0) {
            ret = usb_bulk_transfer(msc->usb_dev,
                                    direction ? msc->in_endpoint : msc->out_endpoint,
                                    data, data_len, direction ? USB_DIR_IN : USB_DIR_OUT);
            if (ret < 0) {
                // println("[MSC] Data transfer failed, retrying...");
                retries++;
                if (retries < MSC_MAX_RETRIES) {
                    msc_reset_recovery(msc);
                    for (volatile int i = 0; i < MSC_RETRY_DELAY_MS * 1000; i++);
                }
                continue;
            }
        }
        
        /* 接收 CSW */
        ret = usb_bulk_transfer(msc->usb_dev, msc->in_endpoint,
                                &csw, sizeof(csw), USB_DIR_IN);
        if (ret < 0) {
            // println("[MSC] Failed to receive CSW, retrying...");
            retries++;
            if (retries < MSC_MAX_RETRIES) {
                msc_reset_recovery(msc);
                for (volatile int i = 0; i < MSC_RETRY_DELAY_MS * 1000; i++);
            }
            continue;
        }
        
        /* 验证 CSW */
        if (csw.dCSWSignature != CSW_SIGNATURE) {
            // println("[MSC] Invalid CSW signature, retrying...");
            retries++;
            continue;
        }
        
        /* 检查状态 */
        if (csw.bCSWStatus == CSW_STATUS_OK) {
            return 0;  /* 成功 */
        }
        
        /* CSW_STATUS_FAILED 或 CSW_STATUS_PHASE_ERROR */
        if (csw.bCSWStatus == CSW_STATUS_FAILED) {
            /* 发送 REQUEST SENSE 获取详细错误 */
            uint8_t sense_cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
            uint8_t sense_data[18];
            msc_send_command(msc, sense_cdb, 6, sense_data, 18, 1);
            
            // println("[MSC] Command failed with sense data");
            return -1;
        }
        
        /* 相位错误，需要恢复 */
        // println("[MSC] Phase error, performing recovery...");
        msc_reset_recovery(msc);
        retries++;
    }
    
    // println("[MSC] Max retries exceeded");
    return -1;
}

/* SCSI Inquiry */
static int msc_inquiry(struct msc_device *msc) {
    uint8_t cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    uint8_t response[36];
    int ret;
    
    ret = msc_send_command(msc, cdb, 6, response, 36, 1);
    if (ret < 0) {
        return -1;
    }
    
    /* 解析响应 */
    memcpy(msc->vendor, response + 8, 8);
    msc->vendor[8] = '\0';
    memcpy(msc->product, response + 16, 16);
    msc->product[16] = '\0';
    memcpy(msc->revision, response + 32, 4);
    msc->revision[4] = '\0';
    
    msc->removable = (response[1] & 0x80) != 0;
    
    return 0;
}

/* 读取容量 */
static int msc_read_capacity(struct msc_device *msc) {
    uint8_t cdb[10] = { SCSI_READ_CAPACITY, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t response[8];
    int ret;
    
    ret = msc_send_command(msc, cdb, 10, response, 8, 1);
    if (ret < 0) {
        return -1;
    }
    
    /* 大端序转换 */
    msc->block_count = ((uint64_t)response[0] << 24) |
                       ((uint64_t)response[1] << 16) |
                       ((uint64_t)response[2] << 8) |
                       ((uint64_t)response[3]) + 1;
    
    msc->block_size = ((uint32_t)response[4] << 24) |
                      ((uint32_t)response[5] << 16) |
                      ((uint32_t)response[6] << 8) |
                      ((uint32_t)response[7]);
    
    return 0;
}

/* MSC 探测 */
static int msc_probe(struct usb_device *dev) {
    struct msc_device *msc;
    int i;
    
    /* 查找 MSC 接口和端点 */
    int iface_num = -1;
    int in_ep = -1, out_ep = -1;
    uint16_t max_packet = 512;
    
    for (i = 0; i < dev->num_interfaces; i++) {
        struct usb_interface *iface = &dev->interfaces[i];
        
        if (iface->desc.bInterfaceClass == USB_CLASS_MASS_STORAGE) {
            iface_num = i;
            
            /* 查找批量端点 */
            for (int j = 0; j < iface->num_endpoints; j++) {
                struct usb_endpoint *ep = &iface->endpoints[j];
                
                if ((ep->attributes & 0x03) == USB_ENDPOINT_XFER_BULK) {
                    if (ep->direction) {
                        in_ep = ep->address & 0x0F;
                    } else {
                        out_ep = ep->address & 0x0F;
                    }
                    if (ep->max_packet_size > max_packet) {
                        max_packet = ep->max_packet_size;
                    }
                }
            }
            break;
        }
    }
    
    if (iface_num < 0 || in_ep < 0 || out_ep < 0) {
        return -1;
    }
    
    /* 分配 MSC 设备 */
    if (msc_device_count >= MAX_MSC_DEVICES) {
        return -1;
    }
    
    msc = &msc_devices[msc_device_count];
    memset(msc, 0, sizeof(*msc));
    
    msc->usb_dev = dev;
    msc->interface_num = iface_num;
    msc->in_endpoint = in_ep;
    msc->out_endpoint = out_ep;
    msc->max_packet_size = max_packet;
    msc->cbw_tag = 0;
    
    /* 初始化设备 */
    if (msc_inquiry(msc) < 0) {
        // println("[MSC] Inquiry failed");
        return -1;
    }
    
    if (msc_read_capacity(msc) < 0) {
        // println("[MSC] Read capacity failed");
        return -1;
    }
    
    msc_device_count++;
    
    // print("[MSC] Found device: ");
    // print(msc->vendor);
    // print(" ");
    // print(msc->product);
    // print(" (");
    print_dec(msc->block_count * msc->block_size / (1024 * 1024));
    // println(" MB)");
    
    return 0;
}

/* MSC 驱动 */
static struct usb_driver msc_driver = {
    .name = "usb-storage",
    .id_table = msc_id_table,
    .probe = msc_probe,
};

/* 读取扇区 */
int msc_read_sector(struct msc_device *msc, uint64_t lba, void *buf) {
    uint8_t cdb[10];
    
    cdb[0] = SCSI_READ_10;
    cdb[1] = 0;
    cdb[2] = (lba >> 24) & 0xFF;
    cdb[3] = (lba >> 16) & 0xFF;
    cdb[4] = (lba >> 8) & 0xFF;
    cdb[5] = lba & 0xFF;
    cdb[6] = 0;
    cdb[7] = 0;
    cdb[8] = 1;  /* 1 个扇区 */
    cdb[9] = 0;
    
    return msc_send_command(msc, cdb, 10, buf, msc->block_size, 1);
}

/* 写入扇区 */
int msc_write_sector(struct msc_device *msc, uint64_t lba, const void *buf) {
    uint8_t cdb[10];
    
    cdb[0] = SCSI_WRITE_10;
    cdb[1] = 0;
    cdb[2] = (lba >> 24) & 0xFF;
    cdb[3] = (lba >> 16) & 0xFF;
    cdb[4] = (lba >> 8) & 0xFF;
    cdb[5] = lba & 0xFF;
    cdb[6] = 0;
    cdb[7] = 0;
    cdb[8] = 1;
    cdb[9] = 0;
    
    return msc_send_command(msc, cdb, 10, (void *)buf, msc->block_size, 0);
}

/* 获取容量 */
int msc_get_capacity(struct msc_device *msc, uint64_t *blocks, uint32_t *block_size) {
    if (!msc) return -1;
    
    *blocks = msc->block_count;
    *block_size = msc->block_size;
    return 0;
}

/* 获取 MSC 设备 */
struct msc_device *msc_get_device(int index) {
    if (index < 0 || index >= msc_device_count) {
        return NULL;
    }
    return &msc_devices[index];
}

int msc_get_device_count(void) {
    return msc_device_count;
}

/* MSC 初始化 */
int msc_init(void) {
    return usb_register_driver(&msc_driver);
}

/* ========== USB 核心初始化 ========== */

/* 扫描设备 */
int usb_scan_devices(void) {
    int total_devices = 0;
    
    print("[USB] Scanning ");
    print_dec(controller_count);
    println(" controller(s)");
    
    for (int c = 0; c < controller_count; c++) {
        struct usb_controller *ctrl = controllers[c];
        
        if (!ctrl->initialized) {
            print("[USB] Controller ");
            print_dec(c);
            println(" not initialized");
            continue;
        }
        
        print("[USB] Controller ");
        print_dec(c);
        print(" has ");
        print_dec(ctrl->num_ports);
        println(" port(s)");
        
        for (int port = 0; port < ctrl->num_ports; port++) {
            /* 检查端口状态 */
            if (ctrl->ops->port_status) {
                int status = ctrl->ops->port_status(ctrl, port);
                print("[USB] Port ");
                print_dec(port);
                print(" status: ");
                print_dec(status);
                println("");
                if (!(status & 1)) {
                    print("[USB] Port ");
                    print_dec(port);
                    println(" - no device");
                    continue;
                }
            }
            
            /* 复位端口 */
            if (ctrl->ops->port_reset) {
                print("[USB] Resetting port ");
                print_dec(port);
                println("");
                if (ctrl->ops->port_reset(ctrl, port) < 0) {
                    print("[USB] Port ");
                    print_dec(port);
                    println(" reset failed");
                    continue;
                }
            }
            
            /* 使能端口 */
            if (ctrl->ops->port_enable) {
                print("[USB] Enabling port ");
                print_dec(port);
                println("");
                if (ctrl->ops->port_enable(ctrl, port) < 0) {
                    print("[USB] Port ");
                    print_dec(port);
                    println(" enable failed");
                    continue;
                }
            }
            
            /* 等待设备稳定（增加等待时间） */
            print("[USB] Waiting for device to stabilize...");
            println("");
            delay_ms(500);  /* 增加到500ms */
            
            print("[USB] Port ");
            print_dec(port);
            println(" - device connected, enumerating...");
            
            /* 获取速度 */
            usb_speed_t speed = USB_SPEED_FULL;
            if (ctrl->ops->port_get_speed) {
                speed = ctrl->ops->port_get_speed(ctrl, port);
            }
            
            print("[USB] Port ");
            print_dec(port);
            print(" device speed: ");
            print_dec(speed);
            println("");
            
            /* 分配设备结构 */
            if (ctrl->num_devices >= USB_MAX_DEVICES) {
                print("[USB] No more device slots");
                println("");
                continue;
            }
            
            struct usb_device *dev = &ctrl->devices[ctrl->num_devices];
            memset(dev, 0, sizeof(*dev));
            
            dev->address = 0;  /* 默认地址 */
            dev->port = port;
            dev->speed = speed;
            dev->controller = ctrl;
            dev->state = USB_DEVICE_STATE_DEFAULT;
            dev->device_desc.bMaxPacketSize0 = 8;  /* 默认 */
            
            /* 枚举设备 */
            if (usb_enumerate_device(dev) == 0) {
                ctrl->num_devices++;
                total_devices++;
            }
        }
    }
    
    print("[USB] Found ");
    print_dec(total_devices);
    println(" device(s)");
    
    return total_devices;
}

/* 打印设备信息 */
void usb_print_device_info(struct usb_device *dev) {
    // print("  Address: ");
    print_dec(dev->address);
    // println("");
    
    // print("  Vendor: ");
    print_hex(dev->device_desc.idVendor);
    // print("  Product: ");
    print_hex(dev->device_desc.idProduct);
    // println("");
    
    // print("  Class: ");
    print_hex(dev->device_desc.bDeviceClass);
    // print("  Subclass: ");
    print_hex(dev->device_desc.bDeviceSubClass);
    // print("  Protocol: ");
    print_hex(dev->device_desc.bDeviceProtocol);
    // println("");
    
    // print("  Speed: ");
    switch (dev->speed) {
        case USB_SPEED_LOW:   print("Low (1.5 Mbps)"); break;
        case USB_SPEED_FULL:  print("Full (12 Mbps)"); break;
        case USB_SPEED_HIGH:  print("High (480 Mbps)"); break;
        case USB_SPEED_SUPER: print("Super (5 Gbps)"); break;
    }
    // println("");
    
    // print("  Interfaces: ");
    print_dec(dev->num_interfaces);
    // println("");
    
    for (int i = 0; i < dev->num_interfaces; i++) {
        struct usb_interface *iface = &dev->interfaces[i];
        // print("    Interface ");
        print_dec(i);
        // print(": Class=");
        print_hex(iface->desc.bInterfaceClass);
        // print(" Endpoints=");
        print_dec(iface->num_endpoints);
        // println("");
    }
}

/* USB 全局初始化 */
void usb_init(void) {
    controller_count = 0;
    msc_device_count = 0;

    /* 输入子系统已在 main.c 中初始化，不需要重复初始化 */

    /* 初始化 MSC 驱动 */
    msc_init();
    
    /* 初始化 HID 鼠标驱动 */
    extern int hid_mouse_init(void);
    hid_mouse_init();
    
    /* 扫描 PCI 总线上的 USB 控制器 */
    extern void usb_scan_controllers(void);
    usb_scan_controllers();
}
