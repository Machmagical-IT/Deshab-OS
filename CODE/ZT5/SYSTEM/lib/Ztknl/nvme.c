/*
 * NVMe (NVM Express) Driver for Ztknl
 *
 * Based on understanding of Linux NVMe driver architecture,
 * rewritten in Ztknl style (not copied).
 *
 * Supports: Basic NVMe controller with admin + 1 I/O queue
 */

#include "nvme.h"
#include "console.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "cpu.h"
#include "string.h"

/* MMIO access helpers */
static inline uint32_t nvme_reg_read(struct nvme_adapter *adapter, uint32_t offset) {
    return *(volatile uint32_t *)(adapter->mmio_virt + offset);
}

static inline void nvme_reg_write(struct nvme_adapter *adapter, uint32_t offset, uint32_t val) {
    *(volatile uint32_t *)(adapter->mmio_virt + offset) = val;
}

static inline uint64_t nvme_reg_read64(struct nvme_adapter *adapter, uint32_t offset) {
    return *(volatile uint64_t *)(adapter->mmio_virt + offset);
}

static inline void nvme_reg_write64(struct nvme_adapter *adapter, uint32_t offset, uint64_t val) {
    *(volatile uint64_t *)(adapter->mmio_virt + offset) = val;
}

/* Doorbell access */
static inline void nvme_sq_doorbell(struct nvme_adapter *adapter, uint16_t qid, uint16_t tail) {
    uint32_t offset = NVME_REG_DBS + (qid * 2 * adapter->db_stride);
    *(volatile uint32_t *)(adapter->mmio_virt + offset) = tail;
}

static inline void nvme_cq_doorbell(struct nvme_adapter *adapter, uint16_t qid, uint16_t head) {
    uint32_t offset = NVME_REG_DBS + ((qid * 2 + 1) * adapter->db_stride);
    *(volatile uint32_t *)(adapter->mmio_virt + offset) = head;
}

/* Global adapter instance */
static struct nvme_adapter nvme_adapter;

/* Forward declarations */
static int nvme_pci_probe(void);
static int nvme_ctrl_enable(struct nvme_adapter *adapter);
static int nvme_ctrl_disable(struct nvme_adapter *adapter);
static int nvme_setup_admin_queue(struct nvme_adapter *adapter);
static int nvme_identify_ctrl(struct nvme_adapter *adapter, struct nvme_id_ctrl *id);
static int nvme_identify_ns(struct nvme_adapter *adapter, uint32_t nsid, struct nvme_id_ns *id);
static int nvme_setup_io_queue(struct nvme_adapter *adapter);
static int nvme_submit_admin_cmd(struct nvme_adapter *adapter, struct nvme_cmd *cmd,
                                 void *buf, uint32_t buf_len);
static uint16_t nvme_alloc_cid(struct nvme_adapter *adapter);

/* TSC-based delay */
static void nvme_delay_us(uint64_t us) {
    uint64_t tsc_start = rdtsc();
    uint64_t tsc_per_us = 2500;  /* Assume 2.5GHz CPU, adjust as needed */
    uint64_t tsc_end = tsc_start + (tsc_per_us * us);
    while (rdtsc() < tsc_end);
}

/* ============================================================
 * PCI Probe
 * ============================================================ */

static int nvme_pci_probe(void) {
    printk("nvme: Probing PCI bus for NVMe controllers...");

    for (uint8_t bus = 0; bus < 255; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_read_config32(bus, dev, func, 0x00);
                if (vendor_device == 0xFFFFFFFF) continue;

                uint8_t class_code = pci_get_class(bus, dev, func);
                uint8_t subclass = pci_get_subclass(bus, dev, func);

                if (class_code != NVME_PCI_CLASS || subclass != NVME_PCI_SUBCLASS)
                    continue;

                /* Found NVMe controller */
                nvme_adapter.pci_bus = bus;
                nvme_adapter.pci_dev = dev;
                nvme_adapter.pci_func = func;
                nvme_adapter.vendor_id = vendor_device & 0xFFFF;
                nvme_adapter.device_id = (vendor_device >> 16) & 0xFFFF;

                /* Read BAR0 (MMIO) */
                uint32_t bar0 = pci_get_bar(bus, dev, func, 0);
                nvme_adapter.mmio_phys = bar0 & 0xFFFFFFF0;

                /* Enable bus mastering */
                pci_enable_bus_master(bus, dev, func);

                printk("nvme: Found controller %04x:%04x at bus %d, dev %d",
                       nvme_adapter.vendor_id, nvme_adapter.device_id,
                       bus, dev);
                printk("nvme: BAR0 phys = %llx", nvme_adapter.mmio_phys);

                return 0;
            }
        }
    }

    printk("nvme: No NVMe controller found");
    return -1;
}

/* ============================================================
 * Controller Enable
 * ============================================================ */

static int nvme_ctrl_enable(struct nvme_adapter *adapter) {
    /* Read capabilities */
    adapter->cap = nvme_reg_read64(adapter, NVME_REG_CAP);
    adapter->vs = nvme_reg_read(adapter, NVME_REG_VS);

    printk("nvme: CAP = %llx, Version = %x", adapter->cap, adapter->vs);

    /* Calculate doorbell stride */
    adapter->db_stride = 2 << NVME_CAP_STRIDE(adapter->cap);

    /* Read max queue entries */
    uint16_t mqes = NVME_CAP_MQES(adapter->cap);
    printk("nvme: Max queue entries = %d", mqes + 1);

    /* Disable controller first (if enabled) */
    nvme_ctrl_disable(adapter);

    /* Write Controller Configuration */
    uint32_t cc = 0;
    cc |= NVME_CC_ENABLE;
    cc |= NVME_CC_CSS_NVM;
    cc |= (0 << NVME_CC_MPS_SHIFT);  /* 4K page size */
    cc |= NVME_CC_AMS_RR;            /* Round-robin arbitration */
    cc |= NVME_CC_SHN_NONE;
    cc |= NVME_CC_IOSQES;            /* 64-byte SQ entries */
    cc |= NVME_CC_IOCQES;            /* 16-byte CQ entries */

    nvme_reg_write(adapter, NVME_REG_CC, cc);

    /* Wait for controller to become ready */
    for (int i = 0; i < 100000; i++) {
        uint32_t csts = nvme_reg_read(adapter, NVME_REG_CSTS);
        if (csts & NVME_CSTS_RDY) {
            adapter->ready = 1;
            printk("nvme: Controller enabled and ready");
            return 0;
        }
        if (csts & NVME_CSTS_CFS) {
            printk("nvme: Controller fatal status!");
            return -1;
        }
        nvme_delay_us(10);
    }

    printk("nvme: Controller enable timeout");
    return -1;
}

static int nvme_ctrl_disable(struct nvme_adapter *adapter) {
    uint32_t cc = nvme_reg_read(adapter, NVME_REG_CC);
    cc &= ~NVME_CC_ENABLE;
    nvme_reg_write(adapter, NVME_REG_CC, cc);

    /* Wait for controller to become not-ready */
    for (int i = 0; i < 100000; i++) {
        uint32_t csts = nvme_reg_read(adapter, NVME_REG_CSTS);
        if (!(csts & NVME_CSTS_RDY)) {
            adapter->ready = 0;
            return 0;
        }
        nvme_delay_us(10);
    }

    return -1;
}

/* ============================================================
 * Admin Queue Setup
 * ============================================================ */

static int nvme_setup_admin_queue(struct nvme_adapter *adapter) {
    uint16_t depth = NVME_ADMIN_QUEUE_DEPTH;

    /* Allocate Admin Submission Queue */
    uint64_t sq_phys = pmm_alloc_page();
    if (!sq_phys) {
        printk("nvme: Failed to allocate admin SQ");
        return -1;
    }
    adapter->admin_sq = (struct nvme_cmd *)phys_to_virt(sq_phys);
    adapter->admin_sq_phys = sq_phys;
    memset(adapter->admin_sq, 0, PAGE_SIZE);

    /* Allocate Admin Completion Queue */
    uint64_t cq_phys = pmm_alloc_page();
    if (!cq_phys) {
        printk("nvme: Failed to allocate admin CQ");
        pmm_free_page(sq_phys);
        return -1;
    }
    adapter->admin_cq = (struct nvme_cqe *)phys_to_virt(cq_phys);
    adapter->admin_cq_phys = cq_phys;
    memset(adapter->admin_cq, 0, PAGE_SIZE);

    adapter->admin_q_depth = depth;
    adapter->admin_sq_tail = 0;
    adapter->admin_cq_head = 0;
    adapter->admin_cq_phase = 1;

    /* Write Admin Queue Attributes (AQA) */
    uint32_t aqa = (depth - 1) | ((depth - 1) << 16);
    nvme_reg_write(adapter, NVME_REG_AQA, aqa);

    /* Write Admin SQ/CQ base addresses */
    nvme_reg_write64(adapter, NVME_REG_ASQ, sq_phys);
    nvme_reg_write64(adapter, NVME_REG_ACQ, cq_phys);

    printk("nvme: Admin queue setup complete (depth=%d)", depth);
    return 0;
}

/* ============================================================
 * Command Submission
 * ============================================================ */

static uint16_t nvme_alloc_cid(struct nvme_adapter *adapter) {
    static uint16_t cid_counter = 0;
    return cid_counter++;
}

static int nvme_submit_admin_cmd(struct nvme_adapter *adapter, struct nvme_cmd *cmd,
                                 void *buf, uint32_t buf_len) {
    uint16_t cid = nvme_alloc_cid(adapter);
    cmd->cid = cid;

    /* Set PRP1 for data buffer */
    if (buf && buf_len > 0) {
        uint64_t buf_phys_addr = virt_to_phys(buf);
        cmd->prp1 = buf_phys_addr;

        /* If buffer spans more than 1 page, need PRP2 (simplified - not implemented) */
        if (buf_len > PAGE_SIZE) {
            printk("nvme: Multi-page buffer not supported in admin cmd");
            return -1;
        }
    }

    /* Copy command to submission queue */
    uint16_t tail = adapter->admin_sq_tail;
    memcpy(&adapter->admin_sq[tail], cmd, sizeof(struct nvme_cmd));

    /* Ring doorbell */
    adapter->admin_sq_tail = (tail + 1) % adapter->admin_q_depth;
    nvme_sq_doorbell(adapter, 0, adapter->admin_sq_tail);

    /* Poll for completion */
    uint16_t head = adapter->admin_cq_head;
    for (int i = 0; i < 100000; i++) {
        struct nvme_cqe *cqe = &adapter->admin_cq[head];

        /* Check phase bit */
        uint16_t status = cqe->status;
        uint8_t phase = status & 1;
        uint16_t cmd_id = cqe->cid;

        if (phase == adapter->admin_cq_phase && cmd_id == cid) {
            /* Check status (bits 1-15) */
            uint16_t sc = (status >> 1) & 0x7FF;  /* Status Code */
            uint16_t sct = (status >> 9) & 0x7;   /* Status Code Type */

            if (sc != 0 || sct != 0) {
                printk("nvme: Admin command failed: sct=%d, sc=%d", sct, sc);
            }

            /* Update completion queue head */
            adapter->admin_cq_head = (head + 1) % adapter->admin_q_depth;
            nvme_cq_doorbell(adapter, 0, adapter->admin_cq_head);

            /* Check if phase wrapped */
            if (adapter->admin_cq_head == 0) {
                adapter->admin_cq_phase = !adapter->admin_cq_phase;
            }

            return (sc == 0 && sct == 0) ? 0 : -1;
        }

        nvme_delay_us(10);
    }

    printk("nvme: Admin command timeout");
    return -1;
}

/* ============================================================
 * Identify Commands
 * ============================================================ */

static int nvme_identify_ctrl(struct nvme_adapter *adapter, struct nvme_id_ctrl *id) {
    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid = 0;
    cmd.cdw10 = NVME_IDENTIFY_CTRL;
    cmd.cdw11 = 0;

    memset(id, 0, sizeof(*id));
    return nvme_submit_admin_cmd(adapter, &cmd, id, sizeof(*id));
}

static int nvme_identify_ns(struct nvme_adapter *adapter, uint32_t nsid, struct nvme_id_ns *id) {
    struct nvme_cmd cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid = nsid;
    cmd.cdw10 = NVME_IDENTIFY_NS;
    cmd.cdw11 = 0;

    memset(id, 0, sizeof(*id));
    return nvme_submit_admin_cmd(adapter, &cmd, id, sizeof(*id));
}

/* ============================================================
 * I/O Queue Setup
 * ============================================================ */

static int nvme_setup_io_queue(struct nvme_adapter *adapter) {
    uint16_t depth = NVME_IO_QUEUE_DEPTH;
    uint16_t qid = 1;  /* I/O queue ID */

    /* Allocate I/O Submission Queue */
    uint64_t sq_pages = (depth * sizeof(struct nvme_cmd) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t sq_phys = pmm_alloc_pages(sq_pages);
    if (!sq_phys) {
        printk("nvme: Failed to allocate I/O SQ");
        return -1;
    }
    adapter->io_sq = (struct nvme_cmd *)phys_to_virt(sq_phys);
    adapter->io_sq_phys = sq_phys;
    memset(adapter->io_sq, 0, sq_pages * PAGE_SIZE);

    /* Allocate I/O Completion Queue */
    uint64_t cq_pages = (depth * sizeof(struct nvme_cqe) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t cq_phys = pmm_alloc_pages(cq_pages);
    if (!cq_phys) {
        printk("nvme: Failed to allocate I/O CQ");
        pmm_free_pages(sq_phys, sq_pages);
        return -1;
    }
    adapter->io_cq = (struct nvme_cqe *)phys_to_virt(cq_phys);
    adapter->io_cq_phys = cq_phys;
    memset(adapter->io_cq, 0, cq_pages * PAGE_SIZE);

    adapter->io_q_depth = depth;
    adapter->io_qid = qid;
    adapter->io_sq_tail = 0;
    adapter->io_cq_head = 0;
    adapter->io_cq_phase = 1;

    /* Create Completion Queue */
    struct nvme_cmd create_cq = {0};
    create_cq.opcode = NVME_ADMIN_CREATE_SQ;  /* Actually Create CQ opcode is 0x05 */
    create_cq.opcode = 0x05;
    create_cq.prp1 = cq_phys;
    create_cq.cdw10 = ((depth - 1) << 16) | (qid & 0xFFFF);
    create_cq.cdw11 = (1 << 0) | (1 << 1);  /* Physical contiguous, IRQ enabled */

    if (nvme_submit_admin_cmd(adapter, &create_cq, NULL, 0) < 0) {
        printk("nvme: Failed to create I/O CQ");
        return -1;
    }

    /* Create Submission Queue */
    struct nvme_cmd create_sq = {0};
    create_sq.opcode = NVME_ADMIN_CREATE_SQ;
    create_sq.prp1 = sq_phys;
    create_sq.cdw10 = ((depth - 1) << 16) | (qid & 0xFFFF);
    create_sq.cdw11 = (1 << 0) | (qid << 16);  /* Physical contiguous, CQ ID */

    if (nvme_submit_admin_cmd(adapter, &create_sq, NULL, 0) < 0) {
        printk("nvme: Failed to create I/O SQ");
        return -1;
    }

    printk("nvme: I/O queue setup complete (qid=%d, depth=%d)", qid, depth);
    return 0;
}

/* ============================================================
 * I/O Operations
 * ============================================================ */

static int nvme_submit_io_cmd(struct nvme_adapter *adapter, struct nvme_cmd *cmd,
                              void *buf, uint32_t buf_len) {
    uint16_t cid = nvme_alloc_cid(adapter);
    cmd->cid = cid;

    /* Set PRP1 for data buffer */
    if (buf && buf_len > 0) {
        uint64_t buf_phys_addr = virt_to_phys(buf);
        cmd->prp1 = buf_phys_addr;

        if (buf_len > PAGE_SIZE) {
            printk("nvme: Multi-page I/O buffer not supported");
            return -1;
        }
    }

    /* Copy command to I/O submission queue */
    uint16_t tail = adapter->io_sq_tail;
    memcpy(&adapter->io_sq[tail], cmd, sizeof(struct nvme_cmd));

    /* Ring doorbell */
    adapter->io_sq_tail = (tail + 1) % adapter->io_q_depth;
    nvme_sq_doorbell(adapter, adapter->io_qid, adapter->io_sq_tail);

    /* Poll for completion */
    uint16_t head = adapter->io_cq_head;
    for (int i = 0; i < 100000; i++) {
        struct nvme_cqe *cqe = &adapter->io_cq[head];

        uint16_t status = cqe->status;
        uint8_t phase = status & 1;
        uint16_t cmd_id = cqe->cid;

        if (phase == adapter->io_cq_phase && cmd_id == cid) {
            uint16_t sc = (status >> 1) & 0x7FF;
            uint16_t sct = (status >> 9) & 0x7;

            adapter->io_cq_head = (head + 1) % adapter->io_q_depth;
            nvme_cq_doorbell(adapter, adapter->io_qid, adapter->io_cq_head);

            if (adapter->io_cq_head == 0) {
                adapter->io_cq_phase = !adapter->io_cq_phase;
            }

            return (sc == 0 && sct == 0) ? 0 : -1;
        }

        nvme_delay_us(10);
    }

    printk("nvme: I/O command timeout");
    return -1;
}

int nvme_read_sectors(struct nvme_adapter *adapter, uint32_t nsid,
                      uint64_t lba, uint16_t count, void *buf) {
    struct nvme_cmd cmd = {0};
    cmd.opcode = 0x02;  /* NVMe Read */
    cmd.nsid = nsid;
    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = (lba >> 32) & 0xFFFFFFFF;
    cmd.cdw12 = count - 1;  /* 0-based */

    return nvme_submit_io_cmd(adapter, &cmd, buf, count * adapter->lba_size);
}

int nvme_write_sectors(struct nvme_adapter *adapter, uint32_t nsid,
                       uint64_t lba, uint16_t count, const void *buf) {
    struct nvme_cmd cmd = {0};
    cmd.opcode = 0x01;  /* NVMe Write */
    cmd.nsid = nsid;
    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = (lba >> 32) & 0xFFFFFFFF;
    cmd.cdw12 = count - 1;  /* 0-based */

    return nvme_submit_io_cmd(adapter, &cmd, (void *)buf, count * adapter->lba_size);
}

/* ============================================================
 * Disk Interface (for disk.c integration)
 * ============================================================ */

static int nvme_disk_read_sector(struct disk_device *dev, uint64_t lba, void *buf) {
    struct nvme_adapter *adapter = (struct nvme_adapter *)dev->private_data;

    /* Translate 512-byte LBA to device LBA */
    if (adapter->lba_size == 512) {
        return nvme_read_sectors(adapter, adapter->nsid, lba, 1, buf);
    } else {
        /* Handle different LBA sizes (e.g., 4K) */
        uint64_t device_lba = (lba * 512) / adapter->lba_size;
        uint64_t offset = (lba * 512) % adapter->lba_size;

        if (adapter->sector_buf) {
            memset(adapter->sector_buf, 0, adapter->lba_size);
            int ret = nvme_read_sectors(adapter, adapter->nsid, device_lba, 1, adapter->sector_buf);
            if (ret == 0) {
                memcpy(buf, adapter->sector_buf + offset, 512);
            }
            return ret;
        }
        return -1;
    }
}

static int nvme_disk_write_sector(struct disk_device *dev, uint64_t lba, const void *buf) {
    struct nvme_adapter *adapter = (struct nvme_adapter *)dev->private_data;

    if (adapter->lba_size == 512) {
        return nvme_write_sectors(adapter, adapter->nsid, lba, 1, buf);
    } else {
        uint64_t device_lba = (lba * 512) / adapter->lba_size;
        uint64_t offset = (lba * 512) % adapter->lba_size;

        if (adapter->sector_buf) {
            memset(adapter->sector_buf, 0, adapter->lba_size);
            int ret = nvme_read_sectors(adapter, adapter->nsid, device_lba, 1, adapter->sector_buf);
            if (ret < 0) return ret;

            memcpy(adapter->sector_buf + offset, buf, 512);
            return nvme_write_sectors(adapter, adapter->nsid, device_lba, 1, adapter->sector_buf);
        }
        return -1;
    }
}

/* ============================================================
 * Main Initialization
 * ============================================================ */

int nvme_init(void) {
    printk("=== NVMe Driver ===");

    /* Clear adapter */
    memset(&nvme_adapter, 0, sizeof(nvme_adapter));

    /* PCI probe */
    if (nvme_pci_probe() < 0) {
        printk("nvme: No NVMe controller found");
        return -1;
    }

    /* Map MMIO region */
    nvme_adapter.mmio_virt = (uint64_t)phys_to_virt(nvme_adapter.mmio_phys);

    /* Map at least first page for registers + admin queue doorbells */
    nvme_adapter.mmio_size = NVME_REG_DBS + 4096;

    printk("nvme: MMIO mapped at virt %llx", nvme_adapter.mmio_virt);

    /* Enable controller */
    if (nvme_ctrl_enable(&nvme_adapter) < 0) {
        printk("nvme: Failed to enable controller");
        return -1;
    }

    /* Setup admin queue */
    if (nvme_setup_admin_queue(&nvme_adapter) < 0) {
        printk("nvme: Failed to setup admin queue");
        return -1;
    }

    /* Identify controller */
    struct nvme_id_ctrl id_ctrl;
    if (nvme_identify_ctrl(&nvme_adapter, &id_ctrl) < 0) {
        printk("nvme: Failed to identify controller");
        return -1;
    }

    nvme_adapter.cntlid = id_ctrl.cntlid;
    nvme_adapter.nn = id_ctrl.nn;

    printk("nvme: Controller %04x:%04x, SN=%.*s, MN=%.*s",
           nvme_adapter.vendor_id, nvme_adapter.device_id,
           20, id_ctrl.sn, 40, id_ctrl.mn);
    printk("nvme: Number of namespaces = %d", nvme_adapter.nn);

    /* Setup I/O queue */
    if (nvme_setup_io_queue(&nvme_adapter) < 0) {
        printk("nvme: Failed to setup I/O queue (using admin queue for I/O)");
        /* Fallback: use admin queue for I/O */
        nvme_adapter.io_sq = nvme_adapter.admin_sq;
        nvme_adapter.io_cq = nvme_adapter.admin_cq;
        nvme_adapter.io_sq_phys = nvme_adapter.admin_sq_phys;
        nvme_adapter.io_cq_phys = nvme_adapter.admin_cq_phys;
        nvme_adapter.io_q_depth = nvme_adapter.admin_q_depth;
        nvme_adapter.io_qid = 0;
    }

    /* Identify namespace 1 */
    if (nvme_adapter.nn > 0) {
        struct nvme_id_ns id_ns;
        nvme_adapter.nsid = 1;

        if (nvme_identify_ns(&nvme_adapter, 1, &id_ns) == 0) {
            uint8_t flbas = id_ns.flbas & 0x0F;
            nvme_adapter.lba_shift = id_ns.lbaf[flbas].ds;
            nvme_adapter.lba_size = 1 << nvme_adapter.lba_shift;
            nvme_adapter.ns_size = id_ns.nsze;

            printk("nvme: NS 1: size=%llu LBAs, LBA size=%d bytes",
                   nvme_adapter.ns_size, nvme_adapter.lba_size);

            /* Allocate sector buffer for 512-byte translation */
            if (nvme_adapter.lba_size != 512) {
                uint64_t buf_phys = pmm_alloc_page();
                nvme_adapter.sector_buf = (uint8_t *)phys_to_virt(buf_phys);
                nvme_adapter.sector_buf_phys = buf_phys;
            }
        }
    }

    /* Setup disk device */
    nvme_adapter.disk_dev.type = DISK_TYPE_NVME;
    nvme_adapter.disk_dev.index = 0;
    nvme_adapter.disk_dev.sector_size = 512;

    /* Calculate size in 512-byte sectors */
    if (nvme_adapter.lba_size == 512) {
        nvme_adapter.disk_dev.sectors = nvme_adapter.ns_size;
    } else {
        nvme_adapter.disk_dev.sectors = (nvme_adapter.ns_size * nvme_adapter.lba_size) / 512;
    }

    nvme_adapter.disk_dev.private_data = &nvme_adapter;
    strcpy(nvme_adapter.disk_dev.name, "nvme0");
    snprintf(nvme_adapter.disk_dev.vendor, 16, "%04x", nvme_adapter.vendor_id);
    snprintf(nvme_adapter.disk_dev.product, 32, "%04x", nvme_adapter.device_id);

    nvme_adapter.disk_ops.read_sector = nvme_disk_read_sector;
    nvme_adapter.disk_ops.write_sector = nvme_disk_write_sector;

    /* Register disk device */
    disk_register_device(&nvme_adapter.disk_dev, &nvme_adapter.disk_ops);

    nvme_adapter.initialized = 1;

    printk("nvme: Registered as '%s', %llu sectors (%llu MB)",
           nvme_adapter.disk_dev.name,
           nvme_adapter.disk_dev.sectors,
           (nvme_adapter.disk_dev.sectors * 512) / (1024 * 1024));

    return 0;
}
