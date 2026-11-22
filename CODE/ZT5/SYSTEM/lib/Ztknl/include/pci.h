#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* PCI 配置空间地址端口 */
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

/* PCI 配置空间读32位 */
static inline uint32_t pci_read_config32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = (1U << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    uint32_t value;
    asm volatile("outl %%eax, %%dx" : : "a"(addr), "d"(PCI_CONFIG_ADDRESS));
    asm volatile("inl %%dx, %%eax" : "=a"(value) : "d"(PCI_CONFIG_DATA));
    return value;
}

/* PCI 配置空间写32位 */
static inline void pci_write_config32(uint8_t bus, uint8_t dev, uint8_t func,
                                       uint8_t offset, uint32_t value) {
    uint32_t addr = (1U << 31) | (bus << 16) | (dev << 11) | (func << 8) | (offset & 0xFC);
    asm volatile("outl %%eax, %%dx" : : "a"(addr), "d"(PCI_CONFIG_ADDRESS));
    asm volatile("outl %%eax, %%dx" : : "a"(value), "d"(PCI_CONFIG_DATA));
}

/* PCI 配置空间读16位 */
static inline uint16_t pci_read_config16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read_config32(bus, dev, func, offset & ~3);
    return (val >> ((offset & 3) * 8)) & 0xFFFF;
}

/* PCI 配置空间读8位 */
static inline uint8_t pci_read_config8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read_config32(bus, dev, func, offset & ~3);
    return (val >> ((offset & 3) * 8)) & 0xFF;
}

/* PCI 辅助函数 */

static inline uint16_t pci_get_vendor(uint8_t bus, uint8_t dev, uint8_t func) {
    return pci_read_config16(bus, dev, func, 0);
}

static inline uint16_t pci_get_device(uint8_t bus, uint8_t dev, uint8_t func) {
    return pci_read_config16(bus, dev, func, 2);
}

static inline uint8_t pci_get_class(uint8_t bus, uint8_t dev, uint8_t func) {
    return pci_read_config8(bus, dev, func, 11);
}

static inline uint8_t pci_get_subclass(uint8_t bus, uint8_t dev, uint8_t func) {
    return pci_read_config8(bus, dev, func, 10);
}

static inline uint8_t pci_get_prog_if(uint8_t bus, uint8_t dev, uint8_t func) {
    return pci_read_config8(bus, dev, func, 9);
}

static inline uint8_t pci_get_header_type(uint8_t bus, uint8_t dev, uint8_t func) {
    return pci_read_config8(bus, dev, func, 14);
}

static inline uint32_t pci_get_bar(uint8_t bus, uint8_t dev, uint8_t func, int bar) {
    return pci_read_config32(bus, dev, func, 16 + bar * 4);
}

static inline uint8_t pci_get_irq_line(uint8_t bus, uint8_t dev, uint8_t func) {
    return pci_read_config8(bus, dev, func, 60);
}

static inline void pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t cmd = pci_read_config32(bus, dev, func, 4);
    cmd |= (1 << 2) | (1 << 0);  /* Bus Master + Memory Space */
    pci_write_config32(bus, dev, func, 4, cmd);
}

#endif /* PCI_H */
