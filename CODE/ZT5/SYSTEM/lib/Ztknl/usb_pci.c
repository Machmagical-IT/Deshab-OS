/*
 * Zratro OS - USB PCI 控制器检测
 *
 * 扫描 PCI 总线查找 USB 控制器并初始化相应的驱动
 */

#include "usb.h"
#include "console.h"
#include "heap.h"
#include "string.h"
#include "pmm.h"
#include "vmm.h"
#include "cpu.h"

/* ========== PCI 常量定义 ========== */

/* PCI 配置空间地址 */
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

/* PCI 类代码 */
#define PCI_CLASS_SERIAL    0x0C
#define PCI_SUBCLASS_USB    0x03

/* USB 控制器接口类型 */
#define USB_IF_UHCI         0x00    /* UHCI (Intel) */
#define USB_IF_OHCI         0x10    /* OHCI (Compaq/AMD/etc) */
#define USB_IF_EHCI         0x20    /* EHCI (USB 2.0) */
#define USB_IF_XHCI         0x30    /* XHCI (USB 3.0) */

/* ========== PCI 配置空间访问 ========== */

static uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = (1U << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, addr);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, 
                              uint8_t offset, uint32_t value) {
    uint32_t addr = (1U << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, value);
}

/* ========== PCI 辅助函数 ========== */

static uint16_t pci_get_vendor(uint8_t bus, uint8_t dev, uint8_t func) {
    return pci_read_config(bus, dev, func, 0) & 0xFFFF;
}

static uint16_t pci_get_device(uint8_t bus, uint8_t dev, uint8_t func) {
    return (pci_read_config(bus, dev, func, 0) >> 16) & 0xFFFF;
}

static uint8_t pci_get_class(uint8_t bus, uint8_t dev, uint8_t func) {
    return (pci_read_config(bus, dev, func, 8) >> 24) & 0xFF;
}

static uint8_t pci_get_subclass(uint8_t bus, uint8_t dev, uint8_t func) {
    return (pci_read_config(bus, dev, func, 8) >> 16) & 0xFF;
}

static uint8_t pci_get_interface(uint8_t bus, uint8_t dev, uint8_t func) {
    return (pci_read_config(bus, dev, func, 8) >> 8) & 0xFF;
}

static uint8_t pci_get_header_type(uint8_t bus, uint8_t dev, uint8_t func) {
    return (pci_read_config(bus, dev, func, 12) >> 16) & 0xFF;
}

static uint32_t pci_get_bar(uint8_t bus, uint8_t dev, uint8_t func, int bar) {
    return pci_read_config(bus, dev, func, 16 + bar * 4);
}

static uint8_t pci_get_irq(uint8_t bus, uint8_t dev, uint8_t func) {
    return pci_read_config(bus, dev, func, 60) & 0xFF;
}

static void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t cmd = pci_read_config(bus, dev, func, 4);
    cmd |= (1 << 2) | (1 << 0);  /* Bus Master + I/O Space */
    pci_write_config(bus, dev, func, 4, cmd);
}

/* ========== USB 控制器探测 ========== */

/* 声明外部初始化函数 */
extern int xhci_init(struct usb_controller *ctrl);
extern int ehci_init(struct usb_controller *ctrl);
extern int uhci_init(struct usb_controller *ctrl);

/* 检测并初始化 USB 控制器 */
static int probe_usb_controller(uint8_t bus, uint8_t dev, uint8_t func, 
                                 uint8_t interface) {
    struct usb_controller *ctrl;
    int ret;
    
    ctrl = kmalloc(sizeof(struct usb_controller));
    if (!ctrl) {
        return -1;
    }
    
    memset(ctrl, 0, sizeof(*ctrl));
    
    ctrl->pci_bus = bus;
    ctrl->pci_device = dev;
    ctrl->pci_function = func;
    ctrl->irq = pci_get_irq(bus, dev, func);
    
    // print("[USB-PCI] Found controller at ");
    print_hex(bus);
    // print(":");
    print_hex(dev);
    // print(".");
    print_hex(func);
    // print(" - ");
    
    switch (interface) {
        case USB_IF_XHCI:
            // println("XHCI (USB 3.0)");
            /* 获取 MMIO 基地址 (BAR0/1) */
            {
                uint32_t bar0 = pci_get_bar(bus, dev, func, 0);
                uint32_t bar1 = pci_get_bar(bus, dev, func, 1);
                ctrl->mmio_base = ((uint64_t)bar1 << 32) | (bar0 & 0xFFFFFFF0);
                
                /* 映射 MMIO 区域 */
                // print("[USB-PCI] MMIO base: 0x");
                print_hex((uint32_t)(ctrl->mmio_base >> 32));
                print_hex((uint32_t)ctrl->mmio_base);
                // println("");
            }
            
            pci_enable_bus_master(bus, dev, func);
            ctrl->type = USB_CTRL_XHCI;
            ret = xhci_init(ctrl);
            break;
            
        case USB_IF_EHCI:
            // println("EHCI (USB 2.0)");
            /* 获取 MMIO 基地址 */
            {
                uint32_t bar0 = pci_get_bar(bus, dev, func, 0);
                ctrl->mmio_base = bar0 & 0xFFFFFFF0;
                
                /* 映射 MMIO 区域 */
                // print("[USB-PCI] MMIO base: 0x");
                print_hex((uint32_t)ctrl->mmio_base);
                // println("");
            }
            
            pci_enable_bus_master(bus, dev, func);
            ctrl->type = USB_CTRL_EHCI;
            ret = ehci_init(ctrl);
            break;
            
        case USB_IF_UHCI:
            // println("UHCI (USB 1.1)");
            /* 获取 I/O 基地址 (BAR4) */
            {
                uint32_t bar4 = pci_get_bar(bus, dev, func, 4);
                ctrl->io_base = bar4 & 0xFFE0;  /* I/O 端口，低 5 位保留 */
                
                // print("[USB-PCI] I/O base: 0x");
                print_hex(ctrl->io_base);
                // println("");
            }
            
            pci_enable_bus_master(bus, dev, func);
            ctrl->type = USB_CTRL_UHCI;
            ret = uhci_init(ctrl);
            break;
            
        case USB_IF_OHCI:
            // println("OHCI (USB 1.1) - Not supported yet");
            kfree(ctrl);
            return -1;
            
        default:
            // print("Unknown interface 0x");
            print_hex(interface);
            // println("");
            kfree(ctrl);
            return -1;
    }
    
    if (ret == 0) {
        usb_register_controller(ctrl);
        return 0;
    }
    
    kfree(ctrl);
    return -1;
}

/* ========== USB PCI 扫描 ========== */

/* 扫描单个 PCI 设备 */
static void scan_pci_device(uint8_t bus, uint8_t dev) {
    /* 检查功能 0 */
    uint16_t vendor = pci_get_vendor(bus, dev, 0);
    if (vendor == 0xFFFF) {
        return;  /* 设备不存在 */
    }
    
    uint8_t header_type = pci_get_header_type(bus, dev, 0);
    uint8_t class_code = pci_get_class(bus, dev, 0);
    uint8_t subclass = pci_get_subclass(bus, dev, 0);
    uint8_t interface = pci_get_interface(bus, dev, 0);
    
    /* 检查是否为 USB 控制器 */
    if (class_code == PCI_CLASS_SERIAL && subclass == PCI_SUBCLASS_USB) {
        probe_usb_controller(bus, dev, 0, interface);
    }
    
    /* 如果是多功能设备，扫描其他功能 */
    if (header_type & 0x80) {
        for (uint8_t func = 1; func < 8; func++) {
            vendor = pci_get_vendor(bus, dev, func);
            if (vendor == 0xFFFF) {
                continue;
            }
            
            class_code = pci_get_class(bus, dev, func);
            subclass = pci_get_subclass(bus, dev, func);
            interface = pci_get_interface(bus, dev, func);
            
            if (class_code == PCI_CLASS_SERIAL && subclass == PCI_SUBCLASS_USB) {
                probe_usb_controller(bus, dev, func, interface);
            }
        }
    }
}

/* 扫描 PCI 总线 */
static void scan_pci_bus(uint8_t bus) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        scan_pci_device(bus, dev);
    }
}

/* 扫描所有 USB 控制器 */
void usb_scan_controllers(void) {
    // println("[USB-PCI] Scanning for USB controllers...");
    
    /* 首先检查 PCI 桥 (bus 0, device 0) */
    uint8_t header_type = pci_get_header_type(0, 0, 0);
    
    if ((header_type & 0x7F) == 0) {
        /* 单 PCI 主机控制器 */
        scan_pci_bus(0);
    } else {
        /* 多 PCI 主机控制器 */
        for (uint8_t func = 0; func < 8; func++) {
            uint16_t vendor = pci_get_vendor(0, 0, func);
            if (vendor == 0xFFFF) {
                break;
            }
            scan_pci_bus(func);
        }
    }
}
