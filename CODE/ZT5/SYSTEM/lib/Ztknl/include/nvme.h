#ifndef NVME_H
#define NVME_H

#include <stdint.h>
#include <stddef.h>
#include "disk.h"

/* ============================================================
 * NVMe (NVM Express) Driver for Ztknl
 * Simplified implementation supporting basic read/write
 * ============================================================ */

/* NVMe PCI class code */
#define NVME_PCI_CLASS      0x01
#define NVME_PCI_SUBCLASS   0x08

/* NVMe Registers (offset from BAR0) */
#define NVME_REG_CAP        0x0000  /* Controller Capabilities (64-bit) */
#define NVME_REG_VS         0x0008  /* Version */
#define NVME_REG_INTMS      0x000C  /* Interrupt Mask Set */
#define NVME_REG_INTMC      0x0010  /* Interrupt Mask Clear */
#define NVME_REG_CC         0x0014  /* Controller Configuration */
#define NVME_REG_CSTS       0x001C  /* Controller Status */
#define NVME_REG_NSSR       0x0020  /* NVM Subsystem Reset */
#define NVME_REG_AQA        0x0024  /* Admin Queue Attributes */
#define NVME_REG_ASQ        0x0028  /* Admin SQ Base Address (64-bit) */
#define NVME_REG_ACQ        0x0030  /* Admin CQ Base Address (64-bit) */
#define NVME_REG_DBS        0x1000  /* Doorbell base */

/* CAP Register Masks */
#define NVME_CAP_MQES(cap)      ((cap) & 0xFFFF)
#define NVME_CAP_STRIDE(cap)    (((cap) >> 32) & 0xF)
#define NVME_CAP_MPSMIN(cap)    (((cap) >> 48) & 0xF)

/* CC Register Masks */
#define NVME_CC_ENABLE      (1 << 0)
#define NVME_CC_CSS_NVM     0
#define NVME_CC_MPS_SHIFT   7
#define NVME_CC_AMS_RR      (0 << 11)
#define NVME_CC_SHN_NONE    (0 << 14)
#define NVME_CC_IOSQES      (6 << 16)  /* 64 bytes */
#define NVME_CC_IOCQES      (4 << 20)  /* 16 bytes */

/* CSTS Register Masks */
#define NVME_CSTS_RDY       (1 << 0)
#define NVME_CSTS_CFS       (1 << 1)

/* Admin Command Opcodes */
#define NVME_ADMIN_DELETE_SQ    0x00
#define NVME_ADMIN_CREATE_SQ    0x01
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_SET_FEATURES 0x09
#define NVME_ADMIN_GET_FEATURES 0x0A

/* Identify CNS values */
#define NVME_IDENTIFY_CTRL      0x01
#define NVME_IDENTIFY_NS        0x00

/* NVMe Command structure (64 bytes) */
struct nvme_cmd {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd2;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

/* NVMe Completion structure (16 bytes) */
struct nvme_cqe {
    uint32_t result;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed));

/* NVMe Identify Controller */
struct nvme_id_ctrl {
    uint16_t vid;
    uint16_t ssvid;
    char sn[20];
    char mn[40];
    char fr[8];
    uint8_t rab;
    uint8_t ieee[3];
    uint8_t cmic;
    uint8_t mdts;
    uint16_t cntlid;
    uint32_t ver;
    uint8_t rsvd84[168];
    uint16_t oacs;
    uint8_t acl;
    uint8_t aerl;
    uint8_t frmw;
    uint8_t lpa;
    uint8_t elpe;
    uint8_t npss;
    uint8_t avscc;
    uint8_t apsta;
    uint16_t wctemp;
    uint8_t rsvd262[246];
    uint32_t sgls;
    uint8_t rsvd512[256];
    uint8_t sqes;
    uint8_t cqes;
    uint8_t rsvd770[2];
    uint32_t nn;
    uint16_t oncs;
    uint16_t fuses;
    uint8_t fna;
    uint8_t vwc;
    uint16_t awun;
    uint8_t rsvd786[70];
    uint8_t psd0[32];
    uint8_t psd1[32];
} __attribute__((packed));

/* NVMe Identify Namespace */
struct nvme_id_ns {
    uint64_t nsze;
    uint64_t ncap;
    uint64_t nuse;
    uint8_t nsfeat;
    uint8_t nlbaf;
    uint8_t flbas;
    uint8_t mc;
    uint8_t dpc;
    uint8_t dps;
    uint8_t nmic;
    uint8_t rescap;
    uint8_t fpi;
    uint8_t rsvd33;
    uint16_t nawun;
    uint16_t nawupf;
    uint16_t nacwu;
    uint16_t nabsn;
    uint16_t nabo;
    uint16_t nabspf;
    uint8_t rsvd46[18];
    uint64_t nvmcap[2];
    uint8_t rsvd72[40];
    uint8_t nguid[16];
    uint8_t eui64[8];
    struct nvme_lbaf {
        uint16_t ms;
        uint8_t ds;
        uint8_t rp;
    } lbaf[16];
    uint8_t rsvd192[192];
    uint8_t vs[3712];
} __attribute__((packed));

/* Queue configuration */
#define NVME_ADMIN_QUEUE_DEPTH  32
#define NVME_IO_QUEUE_DEPTH     128
#define NVME_MAX_NAMESPACES     16

/* Driver state */
struct nvme_adapter {
    /* PCI information */
    uint8_t pci_bus;
    uint8_t pci_dev;
    uint8_t pci_func;
    uint16_t device_id;
    uint16_t vendor_id;

    /* MMIO */
    uint64_t mmio_phys;
    uint64_t mmio_virt;
    uint64_t mmio_size;

    /* Controller info */
    uint64_t cap;
    uint32_t vs;
    uint32_t db_stride;
    uint16_t cntlid;
    uint32_t nn;  /* Number of namespaces */

    /* Admin queue */
    struct nvme_cmd *admin_sq;
    uint64_t admin_sq_phys;
    struct nvme_cqe *admin_cq;
    uint64_t admin_cq_phys;
    uint16_t admin_sq_tail;
    uint16_t admin_cq_head;
    uint8_t admin_cq_phase;
    uint16_t admin_q_depth;

    /* I/O queue */
    struct nvme_cmd *io_sq;
    uint64_t io_sq_phys;
    struct nvme_cqe *io_cq;
    uint64_t io_cq_phys;
    uint16_t io_sq_tail;
    uint16_t io_cq_head;
    uint8_t io_cq_phase;
    uint16_t io_q_depth;
    uint16_t io_qid;

    /* Namespace info (NSID 1) */
    uint32_t nsid;
    uint64_t ns_size;      /* Total LBA count */
    uint8_t lba_shift;     /* LBA size (9 = 512B, 12 = 4K) */
    uint32_t lba_size;

    /* Buffer for 512-byte sector translation */
    uint8_t *sector_buf;
    uint64_t sector_buf_phys;

    /* Disk device registration */
    struct disk_device disk_dev;
    struct disk_ops disk_ops;

    /* Controller state */
    int initialized;
    int ready;
};

/* NVMe initialization */
int nvme_init(void);

/* Low-level functions */
int nvme_read_sectors(struct nvme_adapter *adapter, uint32_t nsid,
                      uint64_t lba, uint16_t count, void *buf);
int nvme_write_sectors(struct nvme_adapter *adapter, uint32_t nsid,
                       uint64_t lba, uint16_t count, const void *buf);

#endif /* NVME_H */
