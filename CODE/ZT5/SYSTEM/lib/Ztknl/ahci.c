#include "ahci.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "console.h"
#include "cpu.h"
#include "disk.h"
#include "string.h"
#include "ktypes.h"

/* AHCI 控制器实例 */
static struct ahci_controller ahci_ctrl;

/* AHCI 是否可用 */
static int ahci_available = 0;

/* 当前活动端口 */
static int active_port = -1;

/* 前向声明 */
int ahci_disk_init(uint64_t abar_phys);

/* ========== 内联函数：端口寄存器访问 ========== */

static inline uint32_t ahci_read_port(struct ahci_port *port, uint32_t reg) {
    return *(volatile uint32_t *)(port->base + reg);
}

static inline void ahci_write_port(struct ahci_port *port, uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(port->base + reg) = value;
}

/* ========== 辅助函数 ========== */

static void delay(int ms) {
    uint64_t tsc_start, tsc_end;
    uint64_t tsc_freq = cpu_get_tsc_frequency();
    if (tsc_freq == 0) tsc_freq = 1000000000ULL;  /* 默认 1GHz */
    
    asm volatile("rdtsc" : "=A"(tsc_start));
    uint64_t wait = (tsc_freq * ms) / 1000;
    do {
        asm volatile("rdtsc" : "=A"(tsc_end));
    } while ((tsc_end - tsc_start) < wait);
}

/* ========== 端口初始化 ========== */

static int ahci_port_init(struct ahci_port *port) {
    /* 初始化指针为 NULL，便于错误处理 */
    uint64_t cmd_list_phys = 0;
    uint64_t rfis_phys = 0;
    uint64_t cmd_table_phys = 0;
    
    /* 停止命令引擎 */
    uint32_t cmd = ahci_read_port(port, AHCI_PORT_CMD);
    cmd &= ~AHCI_CMD_ST;
    ahci_write_port(port, AHCI_PORT_CMD, cmd);
    
    /* 等待 CR 清除 */
    int timeout = 500;
    while ((ahci_read_port(port, AHCI_PORT_CMD) & AHCI_CMD_CR) && timeout-- > 0) {
        delay(1);
    }
    
    /* 禁用 FIS 接收 */
    cmd = ahci_read_port(port, AHCI_PORT_CMD);
    cmd &= ~AHCI_CMD_FRE;
    ahci_write_port(port, AHCI_PORT_CMD, cmd);
    
    /* 等待 FR 清除 */
    timeout = 500;
    while ((ahci_read_port(port, AHCI_PORT_CMD) & AHCI_CMD_FR) && timeout-- > 0) {
        delay(1);
    }
    
    /* 分配命令列表 (1KB, 32 命令) */
    cmd_list_phys = pmm_alloc_pages(1);  /* 分配 1 页 (4KB) */
    if (cmd_list_phys == 0) {
        // println("[AHCI] Error: Failed to allocate command list");
        return -1;
    }
    void *cmd_list_virt = phys_to_virt(cmd_list_phys);
    memset(cmd_list_virt, 0, 4096);
    
    /* 分配 FIS 接收缓冲区 (256B) */
    rfis_phys = pmm_alloc_page();
    if (rfis_phys == 0) {
        // println("[AHCI] Error: Failed to allocate FIS buffer");
        pmm_free_pages(cmd_list_phys, 1);  /* 释放之前分配的 */
        return -1;
    }
    void *rfis_virt = phys_to_virt(rfis_phys);
    memset(rfis_virt, 0, PAGE_SIZE);
    
    /* 分配命令表 (每条命令最多 256 PRDT，共 32 条命令 */
    /* 我们只用第一条命令，所以只分配一个命令表 */
    cmd_table_phys = pmm_alloc_pages(2);  /* 8KB */
    if (cmd_table_phys == 0) {
        // println("[AHCI] Error: Failed to allocate command table");
        pmm_free_pages(rfis_phys, 1);     /* 释放 FIS */
        pmm_free_pages(cmd_list_phys, 1); /* 释放命令列表 */
        return -1;
    }
    void *cmd_table_virt = phys_to_virt(cmd_table_phys);
    memset(cmd_table_virt, 0, 8192);
    
    port->cmd_list = (struct ahci_cmd_header *)cmd_list_virt;
    port->cmd_table = (struct ahci_cmd_table *)cmd_table_virt;
    port->rfis = (struct ahci_received_fis *)rfis_virt;
    port->cmd_list_phys = cmd_list_phys;
    port->cmd_table_phys = cmd_table_phys;
    port->rfis_phys = rfis_phys;
    
    /* 设置命令列表地址 */
    ahci_write_port(port, AHCI_PORT_CLB, cmd_list_phys & 0xFFFFFFFF);
    ahci_write_port(port, AHCI_PORT_CLBU, (cmd_list_phys >> 32) & 0xFFFFFFFF);
    
    /* 设置 FIS 地址 */
    ahci_write_port(port, AHCI_PORT_FB, rfis_phys & 0xFFFFFFFF);
    ahci_write_port(port, AHCI_PORT_FBU, (rfis_phys >> 32) & 0xFFFFFFFF);
    
    /* 清除中断状态 */
    ahci_write_port(port, AHCI_PORT_IS, 0xFFFFFFFF);

    /* 清除 SERR (SATA Error) */
    ahci_write_port(port, AHCI_PORT_SERR, 0xFFFFFFFF);

    /* COMRESET: 发送链路复位以建立 SATA 连接 */
    {
        uint32_t sctl = ahci_read_port(port, AHCI_PORT_SCTL);
        sctl = (sctl & ~0xF) | 0x1;  /* SCTL.DET = 1: COMRESET */
        ahci_write_port(port, AHCI_PORT_SCTL, sctl);
        delay(10);  /* 等待 COMRESET 开始 */

        /* 等待链路断开 (SSTS.DET == 0) */
        timeout = 1000;
        while (timeout-- > 0) {
            uint32_t ssts = ahci_read_port(port, AHCI_PORT_SSTS);
            if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_NOPRESENT)
                break;
            delay(1);
        }

        /* 清除 SCTL.DET 恢复正常操作 */
        sctl = ahci_read_port(port, AHCI_PORT_SCTL);
        sctl &= ~0xF;  /* SCTL.DET = 0: normal */
        ahci_write_port(port, AHCI_PORT_SCTL, sctl);
        delay(10);

        /* 等待 PHY 建立 (SSTS.DET == 3) */
        timeout = 5000;
        while (timeout-- > 0) {
            uint32_t ssts = ahci_read_port(port, AHCI_PORT_SSTS);
            if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_PHY)
                break;
            delay(1);
        }
        {
            uint32_t ssts = ahci_read_port(port, AHCI_PORT_SSTS);
            serial_print("[AHCI] Port ");
            { char _nb[4]; int _np=0; int _v=port->num; if(_v==0){_nb[_np++]='0';}else{char _tb[4];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
            serial_print(" SSTS=");
            serial_print_hex(ssts);
            serial_print("\n");
        }
    }

    /* 启用 FIS 接收 */
    cmd = ahci_read_port(port, AHCI_PORT_CMD);
    cmd |= AHCI_CMD_FRE;
    ahci_write_port(port, AHCI_PORT_CMD, cmd);
    
    /* 等待 FR 设置 */
    timeout = 500;
    while (!(ahci_read_port(port, AHCI_PORT_CMD) & AHCI_CMD_FR) && timeout-- > 0) {
        delay(1);
    }
    
    /* 启动命令引擎 */
    cmd = ahci_read_port(port, AHCI_PORT_CMD);
    cmd |= AHCI_CMD_ST;
    ahci_write_port(port, AHCI_PORT_CMD, cmd);
    
    /* 等待 CR 设置 */
    timeout = 500;
    while (!(ahci_read_port(port, AHCI_PORT_CMD) & AHCI_CMD_CR) && timeout-- > 0) {
        delay(1);
    }
    
    /* COMRESET 后等待设备就绪 (TFD.BSY=0) */
    timeout = 10000;
    while (timeout-- > 0) {
        uint32_t tfd = ahci_read_port(port, AHCI_PORT_TFD);
        if (!(tfd & 0x80)) break;  /* BSY bit (bit 7) cleared */
        delay(1);
    }
    
    /* 清除 SERR */
    ahci_write_port(port, AHCI_PORT_SERR, 0xFFFFFFFF);
    
    /* 清除中断状态 */
    ahci_write_port(port, AHCI_PORT_IS, 0xFFFFFFFF);
    
    {
        uint32_t cmd_st = ahci_read_port(port, AHCI_PORT_CMD);
        uint32_t tfd = ahci_read_port(port, AHCI_PORT_TFD);
        serial_print("[AHCI] Port ");
        { char _nb[4]; int _np=0; int _v=port->num; if(_v==0){_nb[_np++]='0';}else{char _tb[4];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
        serial_print(" init done CMD=");
        serial_print_hex(cmd_st);
        serial_print(" TFD=");
        serial_print_hex(tfd);
        serial_print(" CR=");
        serial_print((cmd_st & AHCI_CMD_CR) ? "Y" : "N");
        serial_print(" FR=");
        serial_print((cmd_st & AHCI_CMD_FR) ? "Y" : "N");
        serial_print("\n");
    }
    
    return 0;
}

/* ========== 设备检测 ========== */

static int ahci_port_detect_device(struct ahci_port *port) {
    uint32_t ssts = ahci_read_port(port, AHCI_PORT_SSTS);
    uint32_t det = ssts & AHCI_SSTS_DET_MASK;

    serial_print("[AHCI] Detect Port ");
    { char _nb[4]; int _np=0; int _v=port->num; if(_v==0){_nb[_np++]='0';}else{char _tb[4];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
    serial_print(" SSTS=");
    serial_print_hex(ssts);
    serial_print(" DET=");
    { char _nb[4]; int _np=0; int _v=det; if(_v==0){_nb[_np++]='0';}else{char _tb[4];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }

    if (det != AHCI_SSTS_DET_PHY) {
        serial_print(" -> no device\n");
        return -1;
    }
    
    /* 检查设备签名 */
    uint32_t sig = ahci_read_port(port, AHCI_PORT_SIG);

    serial_print(" SIG=");
    serial_print_hex(sig);
    serial_print("\n");
    
    switch (sig) {
        case AHCI_SIG_ATA:
            port->type = 1;
            port->is_atapi = 0;
            break;
        case AHCI_SIG_ATAPI:
            port->type = 2;
            port->is_atapi = 1;
            break;
        case AHCI_SIG_SEMB:
        case AHCI_SIG_PM:
            port->type = 0;
            return -1;
        default:
            port->type = 1;
            port->is_atapi = 0;
            break;
    }
    
    return 0;
}

/* ========== 发送命令 ========== */

static int ahci_send_command(struct ahci_port *port, int slot, int write, int atapi) {
    /* 构建命令头 */
    struct ahci_cmd_header *cmd_hdr = &port->cmd_list[slot];
    memset(cmd_hdr, 0, sizeof(struct ahci_cmd_header));
    
    cmd_hdr->dw0 = 5;  /* CFL = 5 (5 DW = 20 bytes) */
    if (write) cmd_hdr->dw0 |= (1 << 6);  /* W bit */
    if (atapi) cmd_hdr->dw0 |= (1 << 5);  /* A bit */
    cmd_hdr->dw0 |= (1 << 16);  /* PRDTL = 1 */
    cmd_hdr->ctba = port->cmd_table_phys & 0xFFFFFFFF;
    cmd_hdr->ctbau = (port->cmd_table_phys >> 32) & 0xFFFFFFFF;
    
    /* 清除中断状态 */
    ahci_write_port(port, AHCI_PORT_IS, 0xFFFFFFFF);
    
    /* 发送命令 */
    ahci_write_port(port, AHCI_PORT_CI, 1 << slot);
    
    /* 等待命令完成 */
    int timeout = 5000;  /* 5 seconds */
    while ((ahci_read_port(port, AHCI_PORT_CI) & (1 << slot)) && timeout-- > 0) {
        delay(1);
        
        /* 检查错误 */
        uint32_t is = ahci_read_port(port, AHCI_PORT_IS);
        if (is & (1 << 30)) {  /* Task File Error */
            serial_print("[AHCI] send_cmd: TASK FILE ERROR IS=");
            serial_print_hex(is);
            serial_print(" TFD=");
            serial_print_hex(ahci_read_port(port, AHCI_PORT_TFD));
            serial_print("\n");
            ahci_write_port(port, AHCI_PORT_IS, 0xFFFFFFFF);
            return -1;
        }
    }
    
    if (timeout <= 0) {
        serial_print("[AHCI] send_cmd: TIMEOUT CI=");
        serial_print_hex(ahci_read_port(port, AHCI_PORT_CI));
        serial_print(" TFD=");
        serial_print_hex(ahci_read_port(port, AHCI_PORT_TFD));
        serial_print("\n");
        return -1;
    }
    
    return 0;
}

/* ========== IDENTIFY 命令 ========== */

static int ahci_identify(struct ahci_port *port, void *buf) {
    /* 分配缓冲区 */
    uint64_t buf_phys = pmm_alloc_page();
    if (buf_phys == 0) return -1;
    void *buf_virt = phys_to_virt(buf_phys);
    memset(buf_virt, 0, PAGE_SIZE);
    
    /* 构建 FIS */
    struct ahci_cmd_table *cmd_tbl = port->cmd_table;
    memset(cmd_tbl, 0, sizeof(struct ahci_cmd_table));
    
    struct ahci_fis_h2d *fis = (struct ahci_fis_h2d *)cmd_tbl->cfis;
    memset(fis, 0, sizeof(struct ahci_fis_h2d));
    
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;  /* Command */
    fis->command = port->is_atapi ? ATA_CMD_IDENTIFY_PACKET : ATA_CMD_IDENTIFY;
    
    /* 设置 PRDT */
    cmd_tbl->prdt[0].dba = buf_phys & 0xFFFFFFFF;
    cmd_tbl->prdt[0].dbau = (buf_phys >> 32) & 0xFFFFFFFF;
    cmd_tbl->prdt[0].dbc = 512 - 1;  /* 512 bytes */
    
    /* 发送命令 */
    if (ahci_send_command(port, 0, 0, 0) != 0) {
        pmm_free_page(buf_phys);
        return -1;
    }
    
    /* 复制数据 */
    memcpy(buf, buf_virt, 512);
    
    pmm_free_page(buf_phys);
    return 0;
}

/* ========== 读写扇区 ========== */

int ahci_read_sector(uint64_t lba, void *buf) {
    if (!ahci_available || active_port < 0) {
        return -1;
    }
    
    struct ahci_port *port = &ahci_ctrl.ports[active_port];
    
    /* 分配缓冲区 */
    uint64_t buf_phys = pmm_alloc_page();
    if (buf_phys == 0) return -1;
    
    /* 构建 FIS */
    struct ahci_cmd_table *cmd_tbl = port->cmd_table;
    memset(cmd_tbl, 0, sizeof(struct ahci_cmd_table));
    
    struct ahci_fis_h2d *fis = (struct ahci_fis_h2d *)cmd_tbl->cfis;
    memset(fis, 0, sizeof(struct ahci_fis_h2d));
    
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;  /* Command */
    fis->command = ATA_CMD_READ_DMA_EXT;
    
    /* LBA 48-bit */
    fis->lba0 = lba & 0xFF;
    fis->lba1 = (lba >> 8) & 0xFF;
    fis->lba2 = (lba >> 16) & 0xFF;
    fis->lba3 = (lba >> 24) & 0xFF;
    fis->lba4 = (lba >> 32) & 0xFF;
    fis->lba5 = (lba >> 40) & 0xFF;
    
    /* 设备: LBA mode */
    fis->device = 0x40;  /* LBA bit */
    
    /* 扇区计数 */
    fis->count_low = 1;
    fis->count_high = 0;
    
    /* 设置 PRDT */
    cmd_tbl->prdt[0].dba = buf_phys & 0xFFFFFFFF;
    cmd_tbl->prdt[0].dbau = (buf_phys >> 32) & 0xFFFFFFFF;
    cmd_tbl->prdt[0].dbc = 512 - 1;  /* 512 bytes */
    
    /* 发送命令 */
    if (ahci_send_command(port, 0, 0, 0) != 0) {
        pmm_free_page(buf_phys);
        return -1;
    }
    
    /* 复制数据 */
    memcpy(buf, phys_to_virt(buf_phys), 512);
    
    pmm_free_page(buf_phys);
    return 0;
}

int ahci_write_sector(uint64_t lba, const void *buf) {
    if (!ahci_available || active_port < 0) {
        return -1;
    }
    
    struct ahci_port *port = &ahci_ctrl.ports[active_port];
    
    /* 分配缓冲区 */
    uint64_t buf_phys = pmm_alloc_page();
    if (buf_phys == 0) return -1;
    
    /* 复制数据 */
    memcpy(phys_to_virt(buf_phys), buf, 512);
    
    /* 构建 FIS */
    struct ahci_cmd_table *cmd_tbl = port->cmd_table;
    memset(cmd_tbl, 0, sizeof(struct ahci_cmd_table));
    
    struct ahci_fis_h2d *fis = (struct ahci_fis_h2d *)cmd_tbl->cfis;
    memset(fis, 0, sizeof(struct ahci_fis_h2d));
    
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;  /* Command */
    fis->command = ATA_CMD_WRITE_DMA_EXT;
    
    /* LBA 48-bit */
    fis->lba0 = lba & 0xFF;
    fis->lba1 = (lba >> 8) & 0xFF;
    fis->lba2 = (lba >> 16) & 0xFF;
    fis->lba3 = (lba >> 24) & 0xFF;
    fis->lba4 = (lba >> 32) & 0xFF;
    fis->lba5 = (lba >> 40) & 0xFF;
    
    /* 设备: LBA mode */
    fis->device = 0x40;  /* LBA bit */
    
    /* 扇区计数 */
    fis->count_low = 1;
    fis->count_high = 0;
    
    /* 设置 PRDT */
    cmd_tbl->prdt[0].dba = buf_phys & 0xFFFFFFFF;
    cmd_tbl->prdt[0].dbau = (buf_phys >> 32) & 0xFFFFFFFF;
    cmd_tbl->prdt[0].dbc = 512 - 1;  /* 512 bytes */
    
    /* 发送命令 */
    int ret = ahci_send_command(port, 0, 1, 0);
    
    pmm_free_page(buf_phys);
    return ret;
}

/* ========== 获取磁盘大小 ========== */

static uint64_t ahci_disk_sectors = 0;

uint64_t ahci_get_disk_size(void) {
    return ahci_disk_sectors;
}

/* ========== PCI 配置空间访问 ========== */

#define PCI_CONFIG_ADDR     0xCF8
#define PCI_CONFIG_DATA     0xCFC

#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_AHCI      0x06
#define PCI_SUBCLASS_IDE       0x01

static inline void pci_outl(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t pci_inl(uint16_t port) {
    uint32_t value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* 读取 PCI 配置空间 */
static uint32_t pci_read_config(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t addr = (1U << 31) | ((uint32_t)bus << 16) | 
                    ((uint32_t)device << 11) | ((uint32_t)func << 8) | (offset & 0xFC);
    pci_outl(PCI_CONFIG_ADDR, addr);
    return pci_inl(PCI_CONFIG_DATA);
}

/* 写入 PCI 配置空间 */
static void pci_write_config(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t addr = (1U << 31) | ((uint32_t)bus << 16) | 
                    ((uint32_t)device << 11) | ((uint32_t)func << 8) | (offset & 0xFC);
    pci_outl(PCI_CONFIG_ADDR, addr);
    pci_outl(PCI_CONFIG_DATA, value);
}

/* 查找 AHCI 控制器 */
static uint64_t pci_find_ahci(void) {
    serial_print("[AHCI] Scanning PCI bus for AHCI controller...\n");
    
    for (int bus = 0; bus < 256; bus++) {
        for (int device = 0; device < 32; device++) {
            for (int func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_read_config(bus, device, func, 0);
                
                if ((vendor_device & 0xFFFF) == 0xFFFF) {
                    continue;
                }
                
                uint32_t class_code = pci_read_config(bus, device, func, 0x08);
                uint8_t base_class = (class_code >> 24) & 0xFF;
                uint8_t sub_class = (class_code >> 16) & 0xFF;
                uint8_t prog_if   = (class_code >> 8)  & 0xFF;

                int is_ahci_mode = (base_class == PCI_CLASS_MASS_STORAGE && sub_class == PCI_SUBCLASS_AHCI);
                int is_sata_ide  = (base_class == PCI_CLASS_MASS_STORAGE && sub_class == PCI_SUBCLASS_IDE &&
                                   (prog_if == 0x01 || prog_if == 0x05 || prog_if == 0x80 || prog_if == 0x85 ||
                                    prog_if == 0x0A || prog_if == 0x8A || prog_if == 0x0F || prog_if == 0x8F));

                if (!is_ahci_mode && !is_sata_ide) continue;

                if (is_sata_ide) {
                    serial_print("[AHCI] Found SATA in IDE mode, switching to AHCI...\n");
                    
                    uint32_t map_reg = pci_read_config(bus, device, func, 0x90);
                    map_reg |= 0x01;
                    pci_write_config(bus, device, func, 0x90, map_reg);

                    uint32_t cmd = pci_read_config(bus, device, func, 0x04);
                    cmd |= 0x07;
                    pci_write_config(bus, device, func, 0x04, cmd);

                    volatile int delay = 100000;
                    while(delay-- > 0) { asm volatile("pause"); }

                    serial_print("[AHCI] Switched to AHCI mode\n");
                }

                uint32_t bar5_low = pci_read_config(bus, device, func, 0x24);
                uint32_t bar5_high = pci_read_config(bus, device, func, 0x28);
                
                uint64_t abar = ((uint64_t)bar5_high << 32) | (bar5_low & 0xFFFFFFF0);

                if ((bar5_low & 0x06) == 0x04) {
                    bar5_low = pci_read_config(bus, device, func, 0x1C);
                    bar5_high = pci_read_config(bus, device, func, 0x20);
                    abar = ((uint64_t)bar5_high << 32) | (bar5_low & 0xFFFFFFF0);
                }

                if (abar == 0) {
                    serial_print("[AHCI] ABAR is zero, skipping\n");
                    continue;
                }
                
                uint32_t cmd = pci_read_config(bus, device, func, 0x04);
                cmd |= 0x07;
                pci_write_config(bus, device, func, 0x04, cmd);
                
                serial_print("[AHCI] Found controller, ABAR=");
                print_hex(abar);
                serial_print("\n");
                
                return abar;
            }
        }
    }
    serial_print("[AHCI] No AHCI/SATA controller found on any PCI bus\n");
    return 0;
}

/* IDE 端口地址全局变量 (定义在 disk.c) */
extern uint16_t ide_primary_cmd;
extern uint16_t ide_primary_ctrl;
extern uint16_t ide_secondary_cmd;
extern uint16_t ide_secondary_ctrl;
extern int ide_ports_initialized;

/* 查找 IDE 控制器并尝试传统模式 */
static int pci_find_ide_controller(void) {
    // println("    Scanning PCI bus for IDE controller...");
    
    for (int bus = 0; bus < 256; bus++) {
        for (int device = 0; device < 32; device++) {
            for (int func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_read_config(bus, device, func, 0);
                
                if ((vendor_device & 0xFFFF) == 0xFFFF) {
                    continue;
                }
                
                uint32_t class_code = pci_read_config(bus, device, func, 0x08);
                uint8_t base_class = (class_code >> 24) & 0xFF;
                uint8_t sub_class = (class_code >> 16) & 0xFF;
                
                /* IDE 控制器 (Class 0x01, Subclass 0x01) */
                if (base_class == PCI_CLASS_MASS_STORAGE && sub_class == PCI_SUBCLASS_IDE) {
                    uint16_t vendor_id = vendor_device & 0xFFFF;
                    uint16_t device_id = (vendor_device >> 16) & 0xFFFF;
                    
                    // print("    Found IDE controller: ");
                    // print_hex(vendor_id);
                    // print(":");
                    // print_hex(device_id);
                    // print(" at ");
                    // print_dec(bus);
                    // print(":");
                    // print_dec(device);
                    // print(".");
                    // print_dec(func);
                    // println("");
                    
                    /* 启用 I/O 访问和总线主控 */
                    uint32_t cmd = pci_read_config(bus, device, func, 0x04);
                    cmd |= 0x07;  /* I/O + Memory + Bus Master */
                    pci_write_config(bus, device, func, 0x04, cmd);
                    
                    /* 读取 BAR 获取端口地址 */
                    uint32_t bar0 = pci_read_config(bus, device, func, 0x10);  /* 主通道命令端口 */
                    uint32_t bar1 = pci_read_config(bus, device, func, 0x14);  /* 主通道控制端口 */
                    uint32_t bar2 = pci_read_config(bus, device, func, 0x18);  /* 从通道命令端口 */
                    uint32_t bar3 = pci_read_config(bus, device, func, 0x1C);  /* 从通道控制端口 */
                    
                    /* BAR 可能是 I/O 端口或内存映射 */
                    /* 如果是 I/O 端口，最低位为 1，地址在 bit 2+ */
                    if (bar0 & 0x01) {
                        ide_primary_cmd = bar0 & 0xFFFC;
                    }
                    if (bar1 & 0x01) {
                        ide_primary_ctrl = bar1 & 0xFFFC;
                    }
                    if (bar2 & 0x01) {
                        ide_secondary_cmd = bar2 & 0xFFFC;
                    }
                    if (bar3 & 0x01) {
                        ide_secondary_ctrl = bar3 & 0xFFFC;
                    }
                    
                    ide_ports_initialized = 1;
                    
                    // print("    IDE ports: Primary cmd=0x");
                    // print_hex(ide_primary_cmd);
                    // print(" ctrl=0x");
                    // print_hex(ide_primary_ctrl);
                    // println("");
                    
                    // print("              Secondary cmd=0x");
                    // print_hex(ide_secondary_cmd);
                    // print(" ctrl=0x");
                    // print_hex(ide_secondary_ctrl);
                    // println("");
                    
                    return 1;  /* IDE 控制器已找到并启用 */
                }
            }
        }
    }
    
    // println("    No IDE controller found");
    return 0;
}

/* ========== 控制器初始化 ========== */

int ahci_init(void) {
    // println("[AHCI] Initializing AHCI controller...");
    
    memset(&ahci_ctrl, 0, sizeof(ahci_ctrl));
    
    /* 通过 PCI 枚举查找 AHCI 控制器 */
    uint64_t abar = pci_find_ahci();
    
    if (abar != 0) {
        /* 找到 AHCI 控制器，初始化它 */
        return ahci_disk_init(abar);
    }
    
    /* 没有找到 AHCI，尝试启用 IDE 控制器 */
    pci_find_ide_controller();
    
    // println("[AHCI] No AHCI controller found, trying legacy ATA");
    return -1;
}

int ahci_detect(void) {
    return ahci_available;
}

struct ahci_controller *ahci_get_controller(void) {
    return &ahci_ctrl;
}

int ahci_is_available(void) {
    return ahci_available;
}

/* ========== AHCI 磁盘驱动接口 ========== */

int ahci_disk_init(uint64_t abar_phys) {
    // println("[AHCI] Initializing AHCI disk driver...");
    
    if (abar_phys == 0) {
        // println("[AHCI] Error: Invalid ABAR address");
        return -1;
    }
    
    /* 映射 ABAR (AHCI MMIO 区域，通常为 2KB) */
    ahci_ctrl.base = (uint64_t)phys_to_virt(abar_phys);
    
    /* 手动映射 AHCI MMIO 区域 (禁用缓存) */
    /* AHCI ABAR 通常为 2KB，但我们映射 1 页 (4KB) */
    uint64_t mmio_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | 
                          VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITETHROUGH |
                          VMM_FLAG_NX;
    vmm_map_page(vmm_get_kernel_space(), ahci_ctrl.base, abar_phys, mmio_flags);
    
    /* 读取能力寄存器 */
    ahci_ctrl.capabilities = *(volatile uint32_t *)(ahci_ctrl.base + 0x00);
    ahci_ctrl.ports_implemented = *(volatile uint32_t *)(ahci_ctrl.base + AHCI_PI);
    ahci_ctrl.version = *(volatile uint32_t *)(ahci_ctrl.base + AHCI_VS);

    serial_print("[AHCI] CAP=");
    serial_print_hex(ahci_ctrl.capabilities);
    serial_print(" PI=");
    serial_print_hex(ahci_ctrl.ports_implemented);
    serial_print(" VS=");
    serial_print_hex(ahci_ctrl.version);
    serial_print("\n");

    /* 启用 AHCI 模式: 设置 GHC.AE 位 */
    {
        volatile uint32_t *ghc = (volatile uint32_t *)(ahci_ctrl.base + 0x04);
        uint32_t ghc_val = *ghc;
        if (!(ghc_val & AHCI_GHC_AE)) {
            *ghc = ghc_val | AHCI_GHC_AE;
            delay(10);
            serial_print("[AHCI] GHC.AE enabled\n");
        }
    }
    
    // print("  AHCI version: ");
    // print_hex(ahci_ctrl.version);
    // println("");
    
    /* 计算端口数量 */
    ahci_ctrl.port_count = 0;
    for (int i = 0; i < 32; i++) {
        if (ahci_ctrl.ports_implemented & (1 << i)) {
            ahci_ctrl.port_count++;
        }
    }
    
    // print("  Ports implemented: ");
    // print_dec(ahci_ctrl.port_count);
    // println("");
    
    /* 初始化端口 */
    for (int i = 0; i < 32; i++) {
        if (ahci_ctrl.ports_implemented & (1 << i)) {
            struct ahci_port *port = &ahci_ctrl.ports[i];
            port->base = ahci_ctrl.base + 0x100 + i * 0x80;
            port->num = i;
            
            // print("  [Port ");
            // print_dec(i);
            // print("] ");
            
            if (ahci_port_init(port) != 0) {
                // println("init failed");
                continue;
            }
            
            if (ahci_port_detect_device(port) != 0) {
                // println("no device");
                continue;
            }
            
            // println("device found");
            serial_print("[AHCI] Port ");
            { char _nb[4]; int _np=0; int _v=i; if(_v==0){_nb[_np++]='0';}else{char _tb[4];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
            serial_print(" device detected, identifying...\n");
            
            /* 读取设备信息 */
            uint16_t identify[256];
            if (ahci_identify(port, identify) == 0) {
                /* 获取 LBA48 扇区数 */
                ahci_disk_sectors = ((uint64_t)identify[100] << 0) |
                                   ((uint64_t)identify[101] << 16) |
                                   ((uint64_t)identify[102] << 32) |
                                   ((uint64_t)identify[103] << 48);
                
                serial_print("[AHCI] Port ");
                { char _nb[4]; int _np=0; int _v=i; if(_v==0){_nb[_np++]='0';}else{char _tb[4];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
                serial_print(" IDENTIFY OK sectors=");
                serial_print_hex(ahci_disk_sectors);
                serial_print("\n");
                
                if (ahci_disk_sectors > 0 && active_port < 0) {
                    active_port = i;
                    
                    // print("    Disk size: ");
                    // print_dec(ahci_disk_sectors * 512 / (1024 * 1024));
                    // println(" MB");
                }
            } else {
                serial_print("[AHCI] Port ");
                { char _nb[4]; int _np=0; int _v=i; if(_v==0){_nb[_np++]='0';}else{char _tb[4];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
                serial_print(" IDENTIFY FAILED\n");
            }
        }
    }
    
    serial_print("[AHCI] active_port=");
    { char _nb[8]; int _np=0; int _v=active_port; if(_v<0){serial_print("-");_v=-_v;} if(_v==0){_nb[_np++]='0';}else{char _tb[8];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
    serial_print("\n");
    
    if (active_port >= 0) {
        ahci_available = 1;
        ahci_ctrl.initialized = 1;
        serial_print("[AHCI] Initialized successfully\n");
        return 0;
    } else {
        // println("[AHCI] No active device found");
        return -1;
    }
}

/* ========== AHCI 高级功能增强 ========== */

/* AHCI 统计 */
struct ahci_stats {
    uint64_t reads;
    uint64_t writes;
    uint64_t errors;
    uint64_t timeouts;
};

static struct ahci_stats ahci_stats;

void ahci_get_stats(struct ahci_stats *stats) {
    if (stats != NULL) {
        *stats = ahci_stats;
    }
}

void ahci_print_stats(void) {
    // println("=== AHCI Statistics ===");
    // print("Reads:    ");
    // print_dec(ahci_stats.reads);
    // println("");
    // print("Writes:   ");
    // print_dec(ahci_stats.writes);
    // println("");
    // print("Errors:   ");
    // print_dec(ahci_stats.errors);
    // println("");
    // print("Timeouts: ");
    // print_dec(ahci_stats.timeouts);
    // println("");
}

/* AHCI 端口诊断 */
void ahci_diagnose_port(int port) {
    if (port < 0 || port >= 32) return;
    
    // println("=== AHCI Port Diagnostics ===");
    // print("Port: ");
    // print_dec(port);
    // println("");
    
    uint32_t ssts = ahci_read_port(&ahci_ctrl.ports[port], AHCI_PORT_SSTS);
    // print("SSTS: ");
    // print_hex(ssts);
    // println("");
    
    uint32_t serr = ahci_read_port(&ahci_ctrl.ports[port], AHCI_PORT_SERR);
    // print("SERR: ");
    // print_hex(serr);
    // println("");
    
    uint32_t ci = ahci_read_port(&ahci_ctrl.ports[port], AHCI_PORT_CI);
    // print("CI: ");
    // print_hex(ci);
    // println("");
}

/* 全局 AHCI 诊断 */
void ahci_diagnose_all(void) {
    // println("=== AHCI Controller Diagnostics ===");
    // print("Initialized: ");
    println(ahci_ctrl.initialized ? "Yes" : "No");
    // print("Ports implemented: ");
    // print_hex(ahci_ctrl.ports_implemented);
    // println("");
    // print("Active port: ");
    // print_dec(active_port);
    // println("");
    // print("Disk sectors: ");
    // print_dec(ahci_disk_sectors);
    // println("");
}

/* 带重试的读取 */
int ahci_read_retry(uint64_t lba, void *buf, int max_retries) {
    int retries = 0;
    int result;
    
    while (retries < max_retries) {
        result = ahci_read_sector(lba, buf);
        if (result == 0) {
            ahci_stats.reads++;
            return 0;
        }
        retries++;
        ahci_stats.errors++;
    }
    
    ahci_stats.timeouts++;
    return -1;
}

/* 带重试的写入 */
int ahci_write_retry(uint64_t lba, const void *buf, int max_retries) {
    int retries = 0;
    int result;
    
    while (retries < max_retries) {
        result = ahci_write_sector(lba, buf);
        if (result == 0) {
            ahci_stats.writes++;
            return 0;
        }
        retries++;
        ahci_stats.errors++;
    }
    
    ahci_stats.timeouts++;
    return -1;
}

/* 检查 AHCI 健康状态 */
int ahci_check_health(void) {
    if (!ahci_ctrl.initialized) {
        return -1;
    }
    
    if (active_port < 0) {
        return -1;
    }
    
    /* 检查端口状态 */
    uint32_t ssts = ahci_read_port(&ahci_ctrl.ports[active_port], AHCI_PORT_SSTS);
    if ((ssts & 0x0F) != 3) {  /* 设备未激活 */
        return -1;
    }
    
    return 0;
}

/* ========== AHCI 高级增强功能 ========== */

/* === NCQ (Native Command Queuing) === */

/* NCQ 队列深度 */
#define AHCI_NCQ_DEPTH     32

struct ahci_ncq_cmd {
    uint64_t lba;
    uint32_t count;
    void *buf;
    int direction;      /* 0=读, 1=写 */
    int tag;
    int completed;
};

static struct ahci_ncq_cmd ncq_queue[AHCI_NCQ_DEPTH];
static int ncq_queue_head = 0;
static int ncq_queue_tail = 0;
static spinlock_t ncq_lock = SPINLOCK_INIT;

/* 提交 NCQ 命令 */
int ahci_ncq_submit(uint64_t lba, uint32_t count, void *buf, int direction) {
    spin_lock(&ncq_lock);
    
    int next_tail = (ncq_queue_tail + 1) % AHCI_NCQ_DEPTH;
    if (next_tail == ncq_queue_head) {
        spin_unlock(&ncq_lock);
        return -1;  /* 队列满 */
    }
    
    struct ahci_ncq_cmd *cmd = &ncq_queue[ncq_queue_tail];
    cmd->lba = lba;
    cmd->count = count;
    cmd->buf = buf;
    cmd->direction = direction;
    cmd->tag = ncq_queue_tail;
    cmd->completed = 0;
    
    ncq_queue_tail = next_tail;
    
    spin_unlock(&ncq_lock);
    return cmd->tag;
}

/* 处理 NCQ 完成 */
void ahci_ncq_complete(int tag) {
    if (tag < 0 || tag >= AHCI_NCQ_DEPTH) return;
    
    spin_lock(&ncq_lock);
    ncq_queue[tag].completed = 1;
    spin_unlock(&ncq_lock);
}

/* === 多端口支持 === */

/* 扫描所有端口 */
int ahci_scan_ports(uint32_t *ports_found, int max_ports) {
    if (!ahci_ctrl.initialized) return -1;
    
    int count = 0;
    uint32_t pi = ahci_ctrl.ports_implemented;
    
    for (int i = 0; i < 32 && count < max_ports; i++) {
        if (pi & (1 << i)) {
            uint32_t ssts = ahci_read_port(&ahci_ctrl.ports[i], AHCI_PORT_SSTS);
            if ((ssts & 0x0F) == 3) {  /* 设备存在且激活 */
                ports_found[count++] = i;
            }
        }
    }
    
    return count;
}

/* 切换活动端口 */
int ahci_switch_port(int port) {
    if (!ahci_ctrl.initialized) return -1;
    if (port < 0 || port >= 32) return -1;
    if (!(ahci_ctrl.ports_implemented & (1 << port))) return -1;
    
    uint32_t ssts = ahci_read_port(&ahci_ctrl.ports[port], AHCI_PORT_SSTS);
    if ((ssts & 0x0F) != 3) return -1;
    
    active_port = port;
    return 0;
}

/* === DMA 缓冲区管理 === */

#define AHCI_DMA_POOL_SIZE      (64 * 1024)   /* 64KB DMA 池 */
#define AHCI_DMA_MAX_BUFS       16

struct ahci_dma_buf {
    uint64_t phys;
    void *virt;
    size_t size;
    int in_use;
};

static struct ahci_dma_buf dma_pool[AHCI_DMA_MAX_BUFS];
static int dma_pool_count = 0;
static spinlock_t dma_pool_lock = SPINLOCK_INIT;

/* 分配 DMA 缓冲区 */
void *ahci_dma_alloc(size_t size, uint64_t *phys) {
    if (size == 0 || size > 4096) return NULL;
    
    spin_lock(&dma_pool_lock);
    
    /* 查找空闲槽 */
    for (int i = 0; i < AHCI_DMA_MAX_BUFS; i++) {
        if (!dma_pool[i].in_use && dma_pool[i].size >= size) {
            dma_pool[i].in_use = 1;
            *phys = dma_pool[i].phys;
            spin_unlock(&dma_pool_lock);
            return dma_pool[i].virt;
        }
    }
    
    /* 需要新分配 */
    if (dma_pool_count < AHCI_DMA_MAX_BUFS) {
        int idx = dma_pool_count++;
        /* 简化: 使用 kmalloc */
        dma_pool[idx].virt = kmalloc(size + 4096);  /* 对齐 */
        if (dma_pool[idx].virt == NULL) {
            spin_unlock(&dma_pool_lock);
            return NULL;
        }
        /* 关键修复：kmalloc返回虚拟地址，必须转换为物理地址用于DMA */
        dma_pool[idx].phys = virt_to_phys(dma_pool[idx].virt);
        dma_pool[idx].size = size;
        dma_pool[idx].in_use = 1;
        *phys = dma_pool[idx].phys;
        spin_unlock(&dma_pool_lock);
        return dma_pool[idx].virt;
    }
    
    spin_unlock(&dma_pool_lock);
    return NULL;
}

/* 释放 DMA 缓冲区 */
void ahci_dma_free(void *virt) {
    spin_lock(&dma_pool_lock);
    
    for (int i = 0; i < AHCI_DMA_MAX_BUFS; i++) {
        if (dma_pool[i].virt == virt) {
            dma_pool[i].in_use = 0;
            break;
        }
    }
    
    spin_unlock(&dma_pool_lock);
}

/* === 电源管理 === */

/* 设置链路电源状态 */
int ahci_set_power_state(int port, int state) {
    if (!ahci_ctrl.initialized) return -1;
    if (port < 0 || port >= 32) return -1;
    
    struct ahci_port *p = &ahci_ctrl.ports[port];
    
    /* SCTL - SATA 控制 */
    uint32_t sctl = ahci_read_port(p, AHCI_PORT_SCTL);
    sctl = (sctl & ~0xF00) | (state << 8);
    ahci_write_port(p, AHCI_PORT_SCTL, sctl);
    
    return 0;
}

/* 进入低功耗模式 */
int ahci_enter_low_power(int port) {
    return ahci_set_power_state(port, 1);  /* Partial */
}

/* 唤醒端口 */
int ahci_wakeup_port(int port) {
    if (ahci_set_power_state(port, 0) != 0) return -1;
    
    /* 等待链路恢复 */
    for (int i = 0; i < 1000; i++) {
        uint32_t ssts = ahci_read_port(&ahci_ctrl.ports[port], AHCI_PORT_SSTS);
        if ((ssts & 0x0F) == 3) return 0;  /* 激活 */
        delay(100);  /* 100 微秒 */
    }
    
    return -1;
}

/* === 智能错误恢复 === */

/* 错误类型 */
#define AHCI_ERR_NONE          0
#define AHCI_ERR_TIMEOUT       1
#define AHCI_ERR_CRC           2
#define AHCI_ERR_INTERFACE     3
#define AHCI_ERR_FATAL         4

/* 检测错误类型 */
int ahci_detect_error(int port) {
    struct ahci_port *p = &ahci_ctrl.ports[port];
    
    uint32_t serr = ahci_read_port(p, AHCI_PORT_SERR);
    uint32_t is = ahci_read_port(p, AHCI_PORT_IS);
    
    /* 清除错误状态 */
    ahci_write_port(p, AHCI_PORT_SERR, serr);
    ahci_write_port(p, AHCI_PORT_IS, is);
    
    if (serr & 0x04000000) return AHCI_ERR_FATAL;      /* 交换错误 */
    if (serr & 0x02000000) return AHCI_ERR_INTERFACE;   /* 接口错误 */
    if (serr & 0x01000000) return AHCI_ERR_CRC;         /* CRC 错误 */
    if (is & 0x80000000) return AHCI_ERR_TIMEOUT;       /* 任务文件错误 */
    
    return AHCI_ERR_NONE;
}

/* 恢复端口 */
int ahci_recover_port(int port) {
    if (!ahci_ctrl.initialized) return -1;
    if (port < 0 || port >= 32) return -1;
    
    struct ahci_port *p = &ahci_ctrl.ports[port];
    
    /* 停止端口 */
    uint32_t cmd = ahci_read_port(p, AHCI_PORT_CMD);
    cmd &= ~0x0001;  /* ST - Start */
    ahci_write_port(p, AHCI_PORT_CMD, cmd);
    
    /* 等待停止 */
    for (int i = 0; i < 1000; i++) {
        if (!(ahci_read_port(p, AHCI_PORT_CMD) & 0x8000)) break;  /* CR cleared */
        delay(100);
    }
    
    /* 清除错误 */
    ahci_write_port(p, AHCI_PORT_SERR, 0xFFFFFFFF);
    ahci_write_port(p, AHCI_PORT_IS, 0xFFFFFFFF);
    
    /* 重启端口 */
    cmd |= 0x0001;  /* ST */
    ahci_write_port(p, AHCI_PORT_CMD, cmd);
    
    return 0;
}

/* === 性能监控 === */

struct ahci_perf_stats {
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t read_ops;
    uint64_t write_ops;
    uint64_t total_latency;
    uint64_t max_latency;
    uint32_t queue_depth_avg;
};

static struct ahci_perf_stats perf_stats;

/* 记录 IO 操作 */
void ahci_record_io(int direction, uint32_t bytes, uint64_t latency) {
    if (direction == 0) {  /* 读 */
        perf_stats.read_bytes += bytes;
        perf_stats.read_ops++;
    } else {  /* 写 */
        perf_stats.write_bytes += bytes;
        perf_stats.write_ops++;
    }
    
    perf_stats.total_latency += latency;
    if (latency > perf_stats.max_latency) {
        perf_stats.max_latency = latency;
    }
}

/* 获取性能统计 */
void ahci_get_perf_stats(struct ahci_perf_stats *stats) {
    *stats = perf_stats;
}

/* 重置性能统计 */
void ahci_reset_perf_stats(void) {
    memset(&perf_stats, 0, sizeof(perf_stats));
}

/* === ATA 命令扩展 === */

/* 获取 IDENTIFY 数据 */
int ahci_identify_device(int port, void *buf) {
    if (!ahci_ctrl.initialized) return -1;
    if (buf == NULL) return -1;
    
    struct ahci_port *p = &ahci_ctrl.ports[port];
    
    /* 分配命令表 */
    struct ahci_cmd_table *cmd_tbl = kmalloc(sizeof(struct ahci_cmd_table));
    if (cmd_tbl == NULL) return -1;
    
    memset(cmd_tbl, 0, sizeof(struct ahci_cmd_table));
    
    /* 设置数据缓冲 - 关键修复：DMA必须使用物理地址，不能使用虚拟地址 */
    cmd_tbl->prdt[0].dba = (uint32_t)virt_to_phys(buf);
    cmd_tbl->prdt[0].dbau = (uint32_t)(virt_to_phys(buf) >> 32);
    cmd_tbl->prdt[0].dbc = 511;  /* 512 字节 */
    cmd_tbl->prdt[0].reserved = 0;

    /* 命令头 - 关键修复：ctba必须使用物理地址 */
    struct ahci_cmd_header cmd_hdr = {0};
    cmd_hdr.dw0 = 5 | (1 << 16);  /* CFL=5, PRDTL=1 */
    cmd_hdr.ctba = (uint32_t)(uint64_t)virt_to_phys(cmd_tbl);
    cmd_hdr.ctbau = (uint32_t)((uint64_t)virt_to_phys(cmd_tbl) >> 32);
    
    /* IDENTIFY 命令 */
    cmd_tbl->cfis[0] = 0x27;  /* FIS type */
    cmd_tbl->cfis[1] = 0x80;  /* Command */
    cmd_tbl->cfis[2] = 0xEC;  /* IDENTIFY DEVICE */
    cmd_tbl->cfis[3] = 0;
    cmd_tbl->cfis[4] = 0;
    cmd_tbl->cfis[5] = 0;
    
    /* 提交命令 */
    ahci_write_port(p, AHCI_PORT_CLB, (uint32_t)(uint64_t)&cmd_hdr);
    ahci_write_port(p, AHCI_PORT_CI, 1);
    
    /* 等待完成 */
    for (int i = 0; i < 10000; i++) {
        if (!(ahci_read_port(p, AHCI_PORT_CI) & 1)) {
            kfree(cmd_tbl);
            return 0;
        }
        delay(100);
    }
    
    kfree(cmd_tbl);
    return -1;  /* 超时 */
}

/* FLUSH 缓存 */
int ahci_flush_cache(int port) {
    if (!ahci_ctrl.initialized) return -1;
    
    struct ahci_port *p = &ahci_ctrl.ports[port];
    struct ahci_cmd_table *cmd_tbl = kmalloc(sizeof(struct ahci_cmd_table));
    if (cmd_tbl == NULL) return -1;
    
    memset(cmd_tbl, 0, sizeof(struct ahci_cmd_table));
    
    /* FLUSH CACHE 命令 */
    cmd_tbl->cfis[0] = 0x27;
    cmd_tbl->cfis[1] = 0x80;
    cmd_tbl->cfis[2] = 0xE7;  /* FLUSH CACHE */

    struct ahci_cmd_header cmd_hdr = {0};
    /* 关键修复：ctba必须使用物理地址 */
    cmd_hdr.ctba = (uint32_t)(uint64_t)virt_to_phys(cmd_tbl);
    cmd_hdr.ctbau = (uint32_t)((uint64_t)virt_to_phys(cmd_tbl) >> 32);
    
    ahci_write_port(p, AHCI_PORT_CLB, (uint32_t)(uint64_t)&cmd_hdr);
    ahci_write_port(p, AHCI_PORT_CI, 1);
    
    /* 等待完成 */
    for (int i = 0; i < 10000; i++) {
        if (!(ahci_read_port(p, AHCI_PORT_CI) & 1)) {
            kfree(cmd_tbl);
            return 0;
        }
        delay(100);
    }
    
    kfree(cmd_tbl);
    return -1;
}

/* === 诊断报告 === */

void ahci_print_detailed_report(void) {
    // println("╔══════════════════════════════════════════╗");
    // println("║        AHCI Controller Report            ║");
    // println("╚══════════════════════════════════════════╝");
    
    // println("");
    // println("=== 控制器状态 ===");
    // print("已初始化: "); println(ahci_ctrl.initialized ? "是" : "否");
    // print("实现端口: "); print_hex(ahci_ctrl.ports_implemented); println("");
    // print("活动端口: "); print_dec(active_port); println("");
    // print("磁盘扇区: "); print_dec(ahci_disk_sectors); println("");
    
    // println("");
    // println("=== 性能统计 ===");
    // print("读取字节: "); print_dec(perf_stats.read_bytes); println("");
    // print("写入字节: "); print_dec(perf_stats.write_bytes); println("");
    // print("读取操作: "); print_dec(perf_stats.read_ops); println("");
    // print("写入操作: "); print_dec(perf_stats.write_ops); println("");
    // print("最大延迟: "); print_dec(perf_stats.max_latency); println(" us");
    
    // println("");
    // println("=== 错误统计 ===");
    // print("总错误: "); print_dec(ahci_stats.errors); println("");
    // print("超时: "); print_dec(ahci_stats.timeouts); println("");
}
