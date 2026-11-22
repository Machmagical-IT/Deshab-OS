#ifndef E1000_H
#define E1000_H

#include <stdint.h>
#include <stddef.h>
#include "net.h"
#include "idt.h"  /* for struct interrupt_frame */

/* ============================================================
 * Intel e1000 Gigabit Ethernet Driver for Ztknl
 * Architecture: x86_64, MMIO + Port I/O for PCI config
 * ============================================================ */

/* PCI Vendor/Device IDs */
#define E1000_VENDOR_ID         0x8086
#define E1000_DEV_ID_82540EM    0x100E  /* QEMU default */
#define E1000_DEV_ID_82545EM    0x100F
#define E1000_DEV_ID_82541GI    0x1076
#define E1000_DEV_ID_82541PI    0x1077
#define E1000_DEV_ID_82543GC_F  0x1004
#define E1000_DEV_ID_82546EB    0x1010
#define E1000_DEV_ID_82571EB    0x105E
#define E1000_DEV_ID_82572EI    0x107D
#define E1000_DEV_ID_82573L     0x108B
#define E1000_DEV_ID_82574L     0x10D3
#define E1000_DEV_ID_82583V     0x1501
#define E1000_DEV_ID_I350       0x1521
#define E1000_DEV_ID_I210       0x1533
#define E1000_DEV_ID_I211       0x1539

/* MMIO Registers */
#define E1000_CTRL      0x00000  /* Device Control */
#define E1000_STATUS    0x00008  /* Device Status */
#define E1000_CTRL_EXT  0x00018  /* Extended Device Control */
#define E1000_MDIC      0x00020  /* MDI Control */
#define E1000_FCAL      0x00028  /* Flow Control Address Low */
#define E1000_FCAH      0x0002C  /* Flow Control Address High */
#define E1000_FCT       0x00030  /* Flow Control Type */
#define E1000_VET       0x00038  /* VLAN EtherType */
#define E1000_ICR       0x000C0  /* Interrupt Cause Read */
#define E1000_ITR       0x000C4  /* Interrupt Throttling Rate */
#define E1000_ICS       0x000C8  /* Interrupt Cause Set */
#define E1000_IMS       0x000D0  /* Interrupt Mask Set */
#define E1000_IMC       0x000D8  /* Interrupt Mask Clear */
#define E1000_IAM       0x000E0  /* Interrupt Acknowledge Auto Mask */
#define E1000_RCTL      0x00100  /* RX Control */
#define E1000_TCTL      0x00400  /* TX Control */
#define E1000_TIPG      0x00410  /* TX Inter-Packet Gap */
#define E1000_LEDCTL    0x00E00  /* LED Control */
#define E1000_PBA       0x01000  /* Packet Buffer Allocation */
#define E1000_FCTTV     0x00170  /* Flow Control Transmit Timer */
#define E1000_TXCW      0x00178  /* TX Configuration Word */
#define E1000_RXCW      0x00180  /* RX Configuration Word */
#define E1000_FCRTL     0x02160  /* Flow Control Receive Threshold Low */
#define E1000_FCRTH     0x02168  /* Flow Control Receive Threshold High */
#define E1000_RDTR      0x02820  /* RX Delay Timer */
#define E1000_RXDCTL    0x02828  /* RX Descriptor Control */
#define E1000_RADV      0x0282C  /* RX Absolute Interrupt Delay */
#define E1000_RSRPD     0x02C00  /* RX Small Packet Detect Interrupt */
#define E1000_TIDV      0x03820  /* TX Interrupt Delay Value */
#define E1000_TXDCTL    0x03828  /* TX Descriptor Control */
#define E1000_TADV      0x0382C  /* TX Absolute Interrupt Delay */
#define E1000_TSPMT     0x03830  /* TX TCP Segmentation Payload Match */
#define E1000_RXCSUM    0x05000  /* RX Checksum Control */
#define E1000_MTA       0x05200  /* Multicast Table Array (base) */
#define E1000_RAL       0x05400  /* Receive Address Low (base) */
#define E1000_RAH       0x05404  /* Receive Address High (base) */
#define E1000_VFTA      0x05600  /* VLAN Filter Table Array (base) */
#define E1000_MRQC      0x05818  /* Multiple Receive Queues Command */
#define E1000_RDBAL     0x02800  /* RX Descriptor Base Address Low */
#define E1000_RDBAH     0x02804  /* RX Descriptor Base Address High */
#define E1000_RDLEN     0x02808  /* RX Descriptor Length */
#define E1000_RDH       0x02810  /* RX Descriptor Head */
#define E1000_RDT       0x02818  /* RX Descriptor Tail */
#define E1000_TDBAL     0x03800  /* TX Descriptor Base Address Low */
#define E1000_TDBAH     0x03804  /* TX Descriptor Base Address High */
#define E1000_TDLEN     0x03808  /* TX Descriptor Length */
#define E1000_TDH       0x03810  /* TX Descriptor Head */
#define E1000_TDT       0x03818  /* TX Descriptor Tail */

/* CTRL Register Bits */
#define E1000_CTRL_FD       0x00000001  /* Full Duplex */
#define E1000_CTRL_GIO      0x00000004  /* GIO Master Disable */
#define E1000_CTRL_SLU      0x00000040  /* Set Link Up */
#define E1000_CTRL_FRCSPD   0x00000800  /* Force Speed */
#define E1000_CTRL_FRCDP    0x00001000  /* Force Duplex */
#define E1000_CTRL_RST      0x04000000  /* Device Reset */
#define E1000_CTRL_RFCE     0x08000000  /* RX Flow Control Enable */
#define E1000_CTRL_TFCE     0x10000000  /* TX Flow Control Enable */
#define E1000_CTRL_VME      0x40000000  /* VLAN Mode Enable */
#define E1000_CTRL_PHY_RST  0x80000000  /* PHY Reset */

/* STATUS Register Bits */
#define E1000_STATUS_FD     0x00000001  /* Full Duplex */
#define E1000_STATUS_LU     0x00000002  /* Link Up */
#define E1000_STATUS_TXOFF  0x00000010  /* Transmission Paused */
#define E1000_STATUS_SPEED_100  0x00000100  /* 100 Mbps */
#define E1000_STATUS_SPEED_1000 0x00000200  /* 1000 Mbps */

/* Interrupt Cause Read (ICR) */
#define E1000_ICR_TXDW    0x00000001  /* TX Descriptor Write Back */
#define E1000_ICR_LSC     0x00000004  /* Link Status Change */
#define E1000_ICR_RXSEQ   0x00000008  /* RX Sequence Error */
#define E1000_ICR_RXDMT0  0x00000010  /* RX Descriptor Min Threshold */
#define E1000_ICR_RXT0    0x00000080  /* RX Timer Interrupt */

/* Interrupt Mask Set (IMS) */
#define E1000_IMS_TXDW    E1000_ICR_TXDW
#define E1000_IMS_LSC     E1000_ICR_LSC
#define E1000_IMS_RXSEQ   E1000_ICR_RXSEQ
#define E1000_IMS_RXDMT0  E1000_ICR_RXDMT0
#define E1000_IMS_RXT0    E1000_ICR_RXT0
#define E1000_IMS_ENABLE  (E1000_IMS_RXT0 | E1000_IMS_TXDW | \
                           E1000_IMS_RXDMT0 | E1000_IMS_RXSEQ | E1000_IMS_LSC)

/* RCTL Register Bits */
#define E1000_RCTL_EN     0x00000002  /* RX Enable */
#define E1000_RCTL_SBP    0x00000004  /* Store Bad Packets */
#define E1000_RCTL_UPE    0x00000008  /* Unicast Promiscuous Enable */
#define E1000_RCTL_MPE    0x00000010  /* Multicast Promiscuous Enable */
#define E1000_RCTL_LPE    0x00000020  /* Long Packet Reception */
#define E1000_RCTL_BAM    0x00008000  /* Broadcast Accept Mode */
#define E1000_RCTL_SZ_2048 0x00030000 /* 2048 byte buffer */
#define E1000_RCTL_SZ_4096 0x00038000 /* 4096 byte buffer */
#define E1000_RCTL_SECRC  0x04000000  /* Strip Ethernet CRC */
#define E1000_RCTL_BSIZE_2048 E1000_RCTL_SZ_2048

/* TCTL Register Bits */
#define E1000_TCTL_EN     0x00000002  /* TX Enable */
#define E1000_TCTL_PSP    0x00000008  /* Pad Short Packets */
#define E1000_TCTL_CT     0x00000FF0  /* Collision Threshold */
#define E1000_TCTL_COLD   0x003FF000  /* Collision Distance */
#define E1000_TCTL_RTLC   0x01000000  /* Re-transmit on Late Collision */

/* RX Descriptor (Legacy, 16 bytes) */
struct e1000_rx_desc {
    uint64_t buffer_addr;  /* DMA address of buffer */
    uint16_t length;       /* Length of data in buffer */
    uint16_t csum;         /* Packet checksum */
    uint8_t status;        /* Descriptor status */
    uint8_t errors;        /* Error flags */
    uint16_t special;      /* Special/ VLAN */
} __attribute__((packed));

/* RX Descriptor Status Bits */
#define E1000_RXD_STAT_DD  0x01  /* Descriptor Done */
#define E1000_RXD_STAT_EOP 0x02  /* End of Packet */

/* TX Descriptor (Legacy, 16 bytes) */
struct e1000_tx_desc {
    uint64_t buffer_addr;   /* DMA address of data */
    uint16_t length;        /* Data buffer length */
    uint8_t  cso;           /* Checksum offset */
    uint8_t  cmd;           /* Command bits */
    uint8_t  status;        /* Status (set by HW) */
    uint8_t  popt_stat;     /* Packet options/status */
    uint8_t  css;           /* Checksum start */
    uint16_t special;
} __attribute__((packed));

/* TX Descriptor Command Bits (8-bit field) */
#define E1000_TXD_CMD_EOP  0x01  /* End of Packet */
#define E1000_TXD_CMD_IFCS 0x02  /* Insert FCS */
#define E1000_TXD_CMD_IC   0x04  /* Insert Checksum */
#define E1000_TXD_CMD_RS   0x08  /* Report Status */
#define E1000_TXD_CMD_DEXT 0x20  /* Descriptor Extension */

/* TX Descriptor Status Bits */
#define E1000_TXD_STAT_DD  0x00000001  /* Descriptor Done */

/* Ring configuration */
#define E1000_TX_RING_SIZE  256  /* Number of TX descriptors */
#define E1000_RX_RING_SIZE  256  /* Number of RX descriptors */
#define E1000_RX_BUF_SIZE   2048 /* RX buffer size per descriptor */

/* Descriptor ring alignment */
#define E1000_RING_ALIGN    4096

/* Driver private data */
struct e1000_adapter {
    /* PCI information */
    uint8_t pci_bus;
    uint8_t pci_dev;
    uint8_t pci_func;
    uint16_t device_id;
    uint8_t irq_line;

    /* MMIO */
    uint64_t mmio_phys;     /* BAR0 physical address */
    uint64_t mmio_virt;     /* BAR0 virtual address (mapped) */

    /* Ring buffers */
    struct e1000_tx_desc *tx_ring;
    uint64_t tx_ring_phys;
    uint8_t *tx_buf[E1000_TX_RING_SIZE];      /* TX data buffers */
    uint64_t tx_buf_phys[E1000_TX_RING_SIZE]; /* TX buffer physical addresses */

    struct e1000_rx_desc *rx_ring;
    uint64_t rx_ring_phys;
    uint8_t *rx_buf[E1000_RX_RING_SIZE];      /* RX data buffers */
    uint64_t rx_buf_phys[E1000_RX_RING_SIZE]; /* RX buffer physical addresses */

    /* Ring state */
    uint32_t tx_next_to_use;
    uint32_t tx_next_to_clean;
    uint32_t rx_next_to_use;
    uint32_t rx_next_to_clean;

    /* MAC address */
    struct mac_addr mac;

    /* Link state */
    int link_up;
    int full_duplex;
    int speed_mbps;

    /* NIC device registration */
    struct nic_device nic_dev;
    struct nic_ops nic_ops;

    /* Statistics */
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint64_t tx_errors;
    uint64_t rx_errors;
};

/* E1000 initialization */
int e1000_init(void);

/* Interrupt handler (registered with IDT) */
void e1000_interrupt_handler(struct interrupt_frame *frame);

#endif /* E1000_H */
