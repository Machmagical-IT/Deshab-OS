/*
 * Intel e1000 Gigabit Ethernet Driver for Ztknl
 *
 * Based on understanding of Linux e1000 driver architecture,
 * rewritten in Ztknl style (not copied).
 *
 * Supports: 82540EM (QEMU default), 82545EM, 8257x, I350, I210/211
 */

#include "e1000.h"
#include "console.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "idt.h"
#include "apic.h"
#include "string.h"
#include "idt.h"  /* for struct interrupt_frame */

/* MMIO access helpers */
static inline uint32_t e1000_reg_read(struct e1000_adapter *adapter, uint32_t reg) {
    return *(volatile uint32_t *)(adapter->mmio_virt + reg);
}

static inline void e1000_reg_write(struct e1000_adapter *adapter, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(adapter->mmio_virt + reg) = val;
}

static inline void e1000_reg_flush(struct e1000_adapter *adapter) {
    (void)e1000_reg_read(adapter, E1000_STATUS);
}

/* Global adapter instance */
static struct e1000_adapter e1000_adapter;

/* Forward declarations */
static int e1000_pci_probe(void);
static int e1000_reset_hw(struct e1000_adapter *adapter);
static int e1000_init_hw(struct e1000_adapter *adapter);
static int e1000_setup_tx_ring(struct e1000_adapter *adapter);
static int e1000_setup_rx_ring(struct e1000_adapter *adapter);
static void e1000_configure_tx(struct e1000_adapter *adapter);
static void e1000_configure_rx(struct e1000_adapter *adapter);
static int e1000_read_mac_addr(struct e1000_adapter *adapter);
static void e1000_set_multi(struct e1000_adapter *adapter);
static void e1000_irq_enable(struct e1000_adapter *adapter);
static void e1000_irq_disable(struct e1000_adapter *adapter);
static void e1000_update_link_status(struct e1000_adapter *adapter);

/* NIC ops */
static int e1000_nic_init(void *priv);
static int e1000_nic_start(void *priv);
static void e1000_nic_stop(void *priv);
static int e1000_nic_send(void *priv, struct net_buf *buf);
static struct net_buf *e1000_nic_recv(void *priv);
static int e1000_nic_set_promisc(void *priv, int enable);
static int e1000_nic_get_mac(void *priv, struct mac_addr *mac);
static int e1000_nic_set_mac(void *priv, struct mac_addr *mac);

/* ============================================================
 * PCI Probe - scan for e1000 devices
 * ============================================================ */

static int e1000_pci_probe(void) {
    printk("e1000: Probing PCI bus for Intel Gigabit Ethernet...");

    for (uint8_t bus = 0; bus < 255; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor = pci_read_config32(bus, dev, func, 0x00);
                if (vendor == 0xFFFFFFFF) continue;  /* No device */

                uint16_t vid = vendor & 0xFFFF;
                uint16_t did = (vendor >> 16) & 0xFFFF;

                if (vid != E1000_VENDOR_ID) continue;

                /* Check if device ID matches known e1000 devices */
                int supported = 0;
                switch (did) {
                case E1000_DEV_ID_82540EM:
                case E1000_DEV_ID_82545EM:
                case E1000_DEV_ID_82541GI:
                case E1000_DEV_ID_82541PI:
                case E1000_DEV_ID_82543GC_F:
                case E1000_DEV_ID_82546EB:
                case E1000_DEV_ID_82571EB:
                case E1000_DEV_ID_82572EI:
                case E1000_DEV_ID_82573L:
                case E1000_DEV_ID_82574L:
                case E1000_DEV_ID_82583V:
                case E1000_DEV_ID_I350:
                case E1000_DEV_ID_I210:
                case E1000_DEV_ID_I211:
                    supported = 1;
                    break;
                }

                if (!supported) continue;

                /* Found a supported e1000 device */
                printk("e1000: Found device %x:%x at bus %d, dev %d, func %d",
                       vid, did, bus, dev, func);

                e1000_adapter.pci_bus = bus;
                e1000_adapter.pci_dev = dev;
                e1000_adapter.pci_func = func;
                e1000_adapter.device_id = did;

                /* Read BAR0 (MMIO base) */
                uint32_t bar0 = pci_read_config32(bus, dev, func, 0x10);
                e1000_adapter.mmio_phys = bar0 & 0xFFFFFFF0;  /* Mask lower 4 bits */

                /* Map MMIO region to virtual memory (128KB = 32 pages) */
                e1000_adapter.mmio_virt = 0xFFFFFFFF81000000ULL;  /* Use high kernel address */
                struct address_space *kspace = vmm_get_kernel_space();
                if (kspace == NULL) {
                    printk("e1000: WARNING: Cannot get kernel space, using direct phys access");
                    e1000_adapter.mmio_virt = e1000_adapter.mmio_phys;
                } else {
                    /* Map 32 pages (128KB) for MMIO with proper MMIO flags */
                    if (vmm_map_range(kspace, e1000_adapter.mmio_virt,
                                      e1000_adapter.mmio_phys, 32,
                                      VMM_FLAGS_MMIO) != 0) {
                        printk("e1000: WARNING: MMIO mapping failed, using direct phys access");
                        e1000_adapter.mmio_virt = e1000_adapter.mmio_phys;
                    }
                }

                /* Read IRQ line */
                uint32_t irq_cfg = pci_read_config32(bus, dev, func, 0x3C);
                e1000_adapter.irq_line = irq_cfg & 0xFF;

                /* Enable bus mastering and memory access */
                uint32_t cmd = pci_read_config32(bus, dev, func, 0x04);
                cmd |= 0x0007;  /* I/O + Memory + Bus Master */
                pci_write_config32(bus, dev, func, 0x04, cmd);

                printk("e1000: BAR0 phys = %llx, virt = %llx, IRQ = %d",
                       e1000_adapter.mmio_phys, e1000_adapter.mmio_virt, e1000_adapter.irq_line);

                return 0;
            }
        }
    }

    printk("e1000: No supported device found");
    return -1;
}

/* ============================================================
 * Hardware Reset
 * ============================================================ */

static int e1000_reset_hw(struct e1000_adapter *adapter) {
    /* Mask all interrupts */
    e1000_reg_write(adapter, E1000_IMC, 0xFFFFFFFF);
    e1000_reg_flush(adapter);

    /* Disable RX and TX */
    e1000_reg_write(adapter, E1000_RCTL, 0);
    e1000_reg_write(adapter, E1000_TCTL, E1000_TCTL_PSP);
    e1000_reg_flush(adapter);

    /* Issue global reset */
    uint32_t ctrl = e1000_reg_read(adapter, E1000_CTRL);
    e1000_reg_write(adapter, E1000_CTRL, ctrl | E1000_CTRL_RST);
    e1000_reg_flush(adapter);

    /* Wait for reset to complete (EEPROM auto-read done) */
    for (int i = 0; i < 1000; i++) {
        ctrl = e1000_reg_read(adapter, E1000_CTRL);
        if (!(ctrl & E1000_CTRL_RST)) break;
    }

    /* Clear all interrupts */
    e1000_reg_read(adapter, E1000_ICR);

    return 0;
}

/* ============================================================
 * Hardware Init - setup link, MAC, etc.
 * ============================================================ */

static int e1000_init_hw(struct e1000_adapter *adapter) {
    /* Setup link - force link up for now */
    uint32_t ctrl = e1000_reg_read(adapter, E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_FD;  /* Set Link Up, Full Duplex */
    ctrl &= ~(E1000_CTRL_FRCSPD | E1000_CTRL_FRCDP);  /* Don't force speed/duplex */
    e1000_reg_write(adapter, E1000_CTRL, ctrl);
    e1000_reg_flush(adapter);

    /* Read MAC address from EEPROM */
    e1000_read_mac_addr(adapter);

    /* Clear multicast table array */
    for (int i = 0; i < 128; i++) {
        e1000_reg_write(adapter, E1000_MTA + (i * 4), 0);
    }

    /* Clear VLAN filter table */
    for (int i = 0; i < 128; i++) {
        e1000_reg_write(adapter, E1000_VFTA + (i * 4), 0);
    }

    /* Update link status */
    e1000_update_link_status(adapter);

    printk("e1000: MAC = %x:%x:%x:%x:%x:%x",
           adapter->mac.addr[0], adapter->mac.addr[1], adapter->mac.addr[2],
           adapter->mac.addr[3], adapter->mac.addr[4], adapter->mac.addr[5]);
    printk("e1000: Link %s, %d Mbps, %s duplex",
           adapter->link_up ? "up" : "down",
           adapter->speed_mbps,
           adapter->full_duplex ? "full" : "half");

    return 0;
}

/* ============================================================
 * Read MAC Address from EEPROM
 * ============================================================ */

static int e1000_read_mac_addr(struct e1000_adapter *adapter) {
    /* Use EEPROM auto-read registers (RAR0) */
    uint32_t ral = e1000_reg_read(adapter, E1000_RAL);
    uint32_t rah = e1000_reg_read(adapter, E1000_RAH);

    /* Check if valid MAC address is present */
    if (rah & 0x80000000) {  /* Address Valid bit */
        adapter->mac.addr[0] = ral & 0xFF;
        adapter->mac.addr[1] = (ral >> 8) & 0xFF;
        adapter->mac.addr[2] = (ral >> 16) & 0xFF;
        adapter->mac.addr[3] = (ral >> 24) & 0xFF;
        adapter->mac.addr[4] = rah & 0xFF;
        adapter->mac.addr[5] = (rah >> 8) & 0xFF;

        /* Check if MAC is zero or broadcast - generate random if so */
        int all_zero = 1;
        for (int i = 0; i < 6; i++) {
            if (adapter->mac.addr[i] != 0) {
                all_zero = 0;
                break;
            }
        }

        if (all_zero) {
            /* Generate a random locally administered MAC */
            adapter->mac.addr[0] = 0x02;  /* Locally administered */
            adapter->mac.addr[1] = 0x00;
            adapter->mac.addr[2] = 0x00;
            adapter->mac.addr[3] = 0x00;
            adapter->mac.addr[4] = 0x00;
            adapter->mac.addr[5] = 0x01;
        }
        return 0;
    }

    /* No valid MAC - generate one */
    adapter->mac.addr[0] = 0x02;
    adapter->mac.addr[1] = 0x00;
    adapter->mac.addr[2] = 0x00;
    adapter->mac.addr[3] = 0x00;
    adapter->mac.addr[4] = 0x00;
    adapter->mac.addr[5] = 0x01;

    /* Write to RAR0 */
    uint32_t lo = adapter->mac.addr[0] |
                  (adapter->mac.addr[1] << 8) |
                  (adapter->mac.addr[2] << 16) |
                  (adapter->mac.addr[3] << 24);
    uint32_t hi = adapter->mac.addr[4] |
                  (adapter->mac.addr[5] << 8) |
                  0x80000000;  /* Set valid bit */
    e1000_reg_write(adapter, E1000_RAL, lo);
    e1000_reg_write(adapter, E1000_RAH, hi);

    return 0;
}

/* ============================================================
 * Setup TX Ring
 * ============================================================ */

static int e1000_setup_tx_ring(struct e1000_adapter *adapter) {
    /* Allocate TX descriptor ring (physically contiguous) */
    uint64_t ring_pages = (E1000_TX_RING_SIZE * sizeof(struct e1000_tx_desc) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t ring_phys = pmm_alloc_pages(ring_pages);
    if (!ring_phys) {
        printk("e1000: Failed to allocate TX ring");
        return -1;
    }

    adapter->tx_ring = (struct e1000_tx_desc *)phys_to_virt(ring_phys);
    adapter->tx_ring_phys = ring_phys;

    /* Clear the ring */
    memset(adapter->tx_ring, 0, E1000_TX_RING_SIZE * sizeof(struct e1000_tx_desc));

    /* Allocate TX data buffers */
    for (int i = 0; i < E1000_TX_RING_SIZE; i++) {
        uint64_t buf_phys = pmm_alloc_page();
        if (!buf_phys) {
            printk("e1000: Failed to allocate TX buffer %d", i);
            return -1;
        }
        adapter->tx_buf[i] = (uint8_t *)phys_to_virt(buf_phys);
        adapter->tx_buf_phys[i] = buf_phys;
    }

    adapter->tx_next_to_use = 0;
    adapter->tx_next_to_clean = 0;

    return 0;
}

/* ============================================================
 * Setup RX Ring
 * ============================================================ */

static int e1000_setup_rx_ring(struct e1000_adapter *adapter) {
    /* Allocate RX descriptor ring */
    uint64_t ring_pages = (E1000_RX_RING_SIZE * sizeof(struct e1000_rx_desc) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t ring_phys = pmm_alloc_pages(ring_pages);
    if (!ring_phys) {
        printk("e1000: Failed to allocate RX ring");
        return -1;
    }

    adapter->rx_ring = (struct e1000_rx_desc *)phys_to_virt(ring_phys);
    adapter->rx_ring_phys = ring_phys;

    /* Clear the ring */
    memset(adapter->rx_ring, 0, E1000_RX_RING_SIZE * sizeof(struct e1000_rx_desc));

    /* Allocate RX data buffers and fill descriptors */
    for (int i = 0; i < E1000_RX_RING_SIZE; i++) {
        uint64_t buf_phys = pmm_alloc_page();
        if (!buf_phys) {
            printk("e1000: Failed to allocate RX buffer %d", i);
            return -1;
        }
        adapter->rx_buf[i] = (uint8_t *)phys_to_virt(buf_phys);
        adapter->rx_buf_phys[i] = buf_phys;

        /* Clear buffer */
        memset(adapter->rx_buf[i], 0, PAGE_SIZE);

        /* Set buffer address in descriptor */
        adapter->rx_ring[i].buffer_addr = buf_phys;
        adapter->rx_ring[i].status = 0;
    }

    adapter->rx_next_to_use = 0;
    adapter->rx_next_to_clean = 0;

    return 0;
}

/* ============================================================
 * Configure TX Unit
 * ============================================================ */

static void e1000_configure_tx(struct e1000_adapter *adapter) {
    /* Set TX ring base and length */
    e1000_reg_write(adapter, E1000_TDBAL, adapter->tx_ring_phys & 0xFFFFFFFF);
    e1000_reg_write(adapter, E1000_TDBAH, (adapter->tx_ring_phys >> 32) & 0xFFFFFFFF);
    e1000_reg_write(adapter, E1000_TDLEN, E1000_TX_RING_SIZE * sizeof(struct e1000_tx_desc));

    /* Reset head/tail pointers */
    e1000_reg_write(adapter, E1000_TDH, 0);
    e1000_reg_write(adapter, E1000_TDT, 0);

    /* Set TX Inter-Packet Gap */
    e1000_reg_write(adapter, E1000_TIPG, 0x0060200A);  /* Copper: 10, 8, 6 */

    /* Configure TX control */
    uint32_t tctl = e1000_reg_read(adapter, E1000_TCTL);
    tctl &= ~E1000_TCTL_CT;
    tctl |= E1000_TCTL_PSP | E1000_TCTL_CT | E1000_TCTL_COLD | E1000_TCTL_EN;
    e1000_reg_write(adapter, E1000_TCTL, tctl);
    e1000_reg_flush(adapter);
}

/* ============================================================
 * Configure RX Unit
 * ============================================================ */

static void e1000_configure_rx(struct e1000_adapter *adapter) {
    /* Disable RX first */
    uint32_t rctl = e1000_reg_read(adapter, E1000_RCTL);
    rctl &= ~E1000_RCTL_EN;
    e1000_reg_write(adapter, E1000_RCTL, rctl);
    e1000_reg_flush(adapter);

    /* Set RX ring base and length */
    e1000_reg_write(adapter, E1000_RDBAL, adapter->rx_ring_phys & 0xFFFFFFFF);
    e1000_reg_write(adapter, E1000_RDBAH, (adapter->rx_ring_phys >> 32) & 0xFFFFFFFF);
    e1000_reg_write(adapter, E1000_RDLEN, E1000_RX_RING_SIZE * sizeof(struct e1000_rx_desc));

    /* Reset head/tail pointers */
    e1000_reg_write(adapter, E1000_RDH, 0);
    e1000_reg_write(adapter, E1000_RDT, E1000_RX_RING_SIZE - 1);  /* Give all buffers to HW */

    /* Set multicast address register */
    e1000_set_multi(adapter);

    /* Configure RX control */
    rctl = 0;
    rctl |= E1000_RCTL_EN;        /* Enable */
    rctl |= E1000_RCTL_BAM;       /* Accept broadcast */
    rctl |= E1000_RCTL_SECRC;     /* Strip CRC */
    rctl |= E1000_RCTL_BSIZE_2048; /* 2048 byte buffers */
    rctl |= E1000_RCTL_SBP;       /* Store bad packets (for debugging) */
    e1000_reg_write(adapter, E1000_RCTL, rctl);
    e1000_reg_flush(adapter);
}

/* ============================================================
 * Set multicast filters (for now, just accept all multicast)
 * ============================================================ */

static void e1000_set_multi(struct e1000_adapter *adapter) {
    /* Enable multicast promiscuous for now */
    uint32_t rctl = e1000_reg_read(adapter, E1000_RCTL);
    rctl |= E1000_RCTL_MPE;
    e1000_reg_write(adapter, E1000_RCTL, rctl);
}

/* ============================================================
 * IRQ Management
 * ============================================================ */

static void e1000_irq_enable(struct e1000_adapter *adapter) {
    e1000_reg_write(adapter, E1000_IMS, E1000_IMS_ENABLE);
    e1000_reg_read(adapter, E1000_ICR);  /* Clear pending */
    e1000_reg_flush(adapter);
}

static void e1000_irq_disable(struct e1000_adapter *adapter) {
    e1000_reg_write(adapter, E1000_IMC, 0xFFFFFFFF);
    e1000_reg_flush(adapter);
}

/* ============================================================
 * Update Link Status
 * ============================================================ */

static void e1000_update_link_status(struct e1000_adapter *adapter) {
    uint32_t status = e1000_reg_read(adapter, E1000_STATUS);

    adapter->link_up = (status & E1000_STATUS_LU) ? 1 : 0;
    adapter->full_duplex = (status & E1000_STATUS_FD) ? 1 : 0;

    if (status & E1000_STATUS_SPEED_1000) {
        adapter->speed_mbps = 1000;
    } else if (status & E1000_STATUS_SPEED_100) {
        adapter->speed_mbps = 100;
    } else {
        adapter->speed_mbps = 10;
    }
}

/* ============================================================
 * Interrupt Handler
 * ============================================================ */

void e1000_interrupt_handler(struct interrupt_frame *frame) {
    struct e1000_adapter *adapter = &e1000_adapter;

    /* Read and clear interrupt causes */
    uint32_t icr = e1000_reg_read(adapter, E1000_ICR);

    if (icr & E1000_ICR_LSC) {
        /* Link status change */
        e1000_update_link_status(adapter);
        printk("e1000: Link %s, %d Mbps, %s duplex",
               adapter->link_up ? "up" : "down",
               adapter->speed_mbps,
               adapter->full_duplex ? "full" : "half");
    }

    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0)) {
        /* RX interrupt - packets received (processed in poll mode) */
    }

    /* Note: EOI is automatically sent by idt.c's isr_handler() for vectors >= 32 */
}

/* ============================================================
 * NIC Operations
 * ============================================================ */

static int e1000_nic_init(void *priv) {
    struct e1000_adapter *adapter = (struct e1000_adapter *)priv;

    printk("e1000: Initializing device at bus %d, dev %d", adapter->pci_bus, adapter->pci_dev);

    /* MMIO region already mapped in e1000_pci_probe() via VMM */
    /* adapter->mmio_virt was set to 0xFFFFFFFF81000000 with proper page table entries */

    /* Reset hardware */
    e1000_reset_hw(adapter);

    /* Initialize hardware */
    e1000_init_hw(adapter);

    /* Setup descriptor rings */
    if (e1000_setup_tx_ring(adapter) < 0) return -1;
    if (e1000_setup_rx_ring(adapter) < 0) return -1;

    /* Configure TX/RX units */
    e1000_configure_tx(adapter);
    e1000_configure_rx(adapter);

    /* Disable interrupts for now (polling mode) */
    e1000_irq_disable(adapter);

    printk("e1000: NIC initialized successfully");
    return 0;
}

static int e1000_nic_start(void *priv) {
    struct e1000_adapter *adapter = (struct e1000_adapter *)priv;

    /* Enable TX and RX */
    uint32_t tctl = e1000_reg_read(adapter, E1000_TCTL);
    tctl |= E1000_TCTL_EN;
    e1000_reg_write(adapter, E1000_TCTL, tctl);

    uint32_t rctl = e1000_reg_read(adapter, E1000_RCTL);
    rctl |= E1000_RCTL_EN;
    e1000_reg_write(adapter, E1000_RCTL, rctl);

    e1000_reg_flush(adapter);

    adapter->nic_dev.state = NIC_STATE_RUNNING;

    printk("e1000: NIC started");
    return 0;
}

static void e1000_nic_stop(void *priv) {
    struct e1000_adapter *adapter = (struct e1000_adapter *)priv;

    /* Disable TX and RX */
    uint32_t tctl = e1000_reg_read(adapter, E1000_TCTL);
    tctl &= ~E1000_TCTL_EN;
    e1000_reg_write(adapter, E1000_TCTL, tctl);

    uint32_t rctl = e1000_reg_read(adapter, E1000_RCTL);
    rctl &= ~E1000_RCTL_EN;
    e1000_reg_write(adapter, E1000_RCTL, rctl);

    e1000_reg_flush(adapter);

    adapter->nic_dev.state = NIC_STATE_UP;
}

static int e1000_nic_send(void *priv, struct net_buf *buf) {
    struct e1000_adapter *adapter = (struct e1000_adapter *)priv;

    if (!buf || buf->len == 0) return -1;

    /* Find a free TX descriptor */
    uint32_t idx = adapter->tx_next_to_use;
    struct e1000_tx_desc *desc = &adapter->tx_ring[idx];

    /* Check if descriptor is still in use (DD bit not set) */
    if (desc->status & E1000_TXD_STAT_DD) {
        /* Clear status for reuse */
        desc->status = 0;
    } else {
        /* Ring is full - could wait or return error */
        return -1;
    }

    /* Copy data to TX buffer */
    if (buf->len <= PAGE_SIZE) {
        memcpy(adapter->tx_buf[idx], buf->data, buf->len);
    } else {
        /* Packet too large */
        adapter->tx_errors++;
        return -1;
    }

    /* Setup descriptor */
    desc->buffer_addr = adapter->tx_buf_phys[idx];
    desc->length = buf->len;

    /* Set command: EOP, IFCS, RS */
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->cso = 0;
    desc->css = 0;
    desc->popt_stat = 0;
    desc->special = 0;

    /* Write-back status will be set by hardware */
    desc->status = 0;

    /* Update next_to_use */
    adapter->tx_next_to_use = (idx + 1) % E1000_TX_RING_SIZE;

    /* Kick the hardware - write TDT */
    e1000_reg_write(adapter, E1000_TDT, adapter->tx_next_to_use);
    e1000_reg_flush(adapter);

    adapter->tx_packets++;
    adapter->tx_bytes += buf->len;

    return 0;
}

static struct net_buf *e1000_nic_recv(void *priv) {
    struct e1000_adapter *adapter = (struct e1000_adapter *)priv;

    /* Poll RX ring for completed descriptors */
    uint32_t idx = adapter->rx_next_to_clean;
    struct e1000_rx_desc *desc = &adapter->rx_ring[idx];

    /* Check if descriptor has been filled by hardware */
    if (!(desc->status & E1000_RXD_STAT_DD)) {
        /* No packet available */
        return NULL;
    }

    /* Allocate a network buffer */
    struct net_buf *buf = net_buf_alloc();
    if (!buf) {
        /* Drop packet - no buffer available */
        adapter->nic_dev.rx_dropped++;
        goto next;
    }

    /* Copy packet data */
    uint16_t len = desc->length;
    if (len > ETH_FRAME_LEN) {
        len = ETH_FRAME_LEN;  /* Truncate if oversized */
    }

    memcpy(buf->data, adapter->rx_buf[idx], len);
    buf->len = len;

    adapter->rx_packets++;
    adapter->rx_bytes += len;

next:
    /* Refill this descriptor with a new buffer */
    uint64_t new_phys = pmm_alloc_page();
    if (new_phys) {
        /* Free old buffer */
        pmm_free_page(adapter->rx_buf_phys[idx]);

        adapter->rx_buf[idx] = (uint8_t *)phys_to_virt(new_phys);
        adapter->rx_buf_phys[idx] = new_phys;
        memset(adapter->rx_buf[idx], 0, PAGE_SIZE);

        desc->buffer_addr = new_phys;
    } else {
        /* Cannot allocate new buffer — keep the old one so the descriptor
         * still points to valid memory.  The packet data will be
         * overwritten by the next incoming frame, which is better than
         * having the hardware DMA into a freed page. */
    }

    /* Clear status */
    desc->status = 0;
    desc->length = 0;

    /* Update next_to_clean */
    adapter->rx_next_to_clean = (idx + 1) % E1000_RX_RING_SIZE;

    /* Update RDT to give buffers back to hardware */
    e1000_reg_write(adapter, E1000_RDT, adapter->rx_next_to_clean);
    e1000_reg_flush(adapter);

    return buf;
}

static int e1000_nic_set_promisc(void *priv, int enable) {
    struct e1000_adapter *adapter = (struct e1000_adapter *)priv;

    uint32_t rctl = e1000_reg_read(adapter, E1000_RCTL);
    if (enable) {
        rctl |= E1000_RCTL_UPE | E1000_RCTL_MPE;
        adapter->nic_dev.flags |= NIC_FLAG_PROMISC;
    } else {
        rctl &= ~(E1000_RCTL_UPE | E1000_RCTL_MPE);
        adapter->nic_dev.flags &= ~NIC_FLAG_PROMISC;
    }
    e1000_reg_write(adapter, E1000_RCTL, rctl);

    return 0;
}

static int e1000_nic_get_mac(void *priv, struct mac_addr *mac) {
    struct e1000_adapter *adapter = (struct e1000_adapter *)priv;
    mac_copy(mac, &adapter->mac);
    return 0;
}

static int e1000_nic_set_mac(void *priv, struct mac_addr *mac) {
    struct e1000_adapter *adapter = (struct e1000_adapter *)priv;
    mac_copy(&adapter->mac, mac);

    /* Update RAR0 */
    uint32_t lo = mac->addr[0] | (mac->addr[1] << 8) |
                  (mac->addr[2] << 16) | (mac->addr[3] << 24);
    uint32_t hi = mac->addr[4] | (mac->addr[5] << 8) | 0x80000000;
    e1000_reg_write(adapter, E1000_RAL, lo);
    e1000_reg_write(adapter, E1000_RAH, hi);

    return 0;
}

/* ============================================================
 * Main Initialization
 * ============================================================ */

int e1000_init(void) {
    printk("=== Intel e1000 Gigabit Ethernet Driver ===");

    /* Clear adapter structure */
    memset(&e1000_adapter, 0, sizeof(e1000_adapter));

    /* Probe PCI bus */
    if (e1000_pci_probe() < 0) {
        printk("e1000: No device found");
        return -1;
    }

    /* Setup NIC operations */
    e1000_adapter.nic_ops.init = e1000_nic_init;
    e1000_adapter.nic_ops.start = e1000_nic_start;
    e1000_adapter.nic_ops.stop = e1000_nic_stop;
    e1000_adapter.nic_ops.send = e1000_nic_send;
    e1000_adapter.nic_ops.recv = e1000_nic_recv;
    e1000_adapter.nic_ops.set_mac = e1000_nic_set_mac;
    e1000_adapter.nic_ops.get_mac = e1000_nic_get_mac;
    e1000_adapter.nic_ops.set_promisc = e1000_nic_set_promisc;
    e1000_adapter.nic_ops.cleanup = NULL;
    e1000_adapter.nic_ops.set_mtu = NULL;
    e1000_adapter.nic_ops.get_stats = NULL;

    /* Setup NIC device structure */
    e1000_adapter.nic_dev.id = 0;
    e1000_adapter.nic_dev.state = NIC_STATE_DOWN;
    e1000_adapter.nic_dev.flags = NIC_FLAG_MULTICAST;
    e1000_adapter.nic_dev.mtu = ETH_MTU;
    e1000_adapter.nic_dev.priv = &e1000_adapter;
    e1000_adapter.nic_dev.ops = &e1000_adapter.nic_ops;

    /* Set default MAC (will be overwritten by EEPROM read) */
    e1000_adapter.mac.addr[0] = 0x52;
    e1000_adapter.mac.addr[1] = 0x54;
    e1000_adapter.mac.addr[2] = 0x00;
    e1000_adapter.mac.addr[3] = 0x12;
    e1000_adapter.mac.addr[4] = 0x34;
    e1000_adapter.mac.addr[5] = 0x56;

    /* Set device name */
    strcpy(e1000_adapter.nic_dev.name, "e1000");

    /* Initialize the hardware */
    if (e1000_nic_init(&e1000_adapter) < 0) {
        printk("e1000: Failed to initialize hardware");
        return -1;
    }

    /* Start the NIC (enable TX/RX, transition to RUNNING state) */
    if (e1000_nic_start(&e1000_adapter) < 0) {
        printk("e1000: Failed to start NIC");
        return -1;
    }

    /* Register with network stack */
    net_register_nic(&e1000_adapter.nic_dev);

    printk("e1000: Registered as '%s'", e1000_adapter.nic_dev.name);

    return 0;
}
