#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>
#include <stddef.h>

/* AHCI 寄存器偏移 */
#define AHCI_GHC        0x04    /* Global HBA Control */
#define AHCI_IS         0x08    /* Interrupt Status */
#define AHCI_PI         0x0C    /* Ports Implemented */
#define AHCI_VS         0x10    /* AHCI Version */
#define AHCI_CCC_CTL    0x14    /* Coalescing Control */
#define AHCI_CCC_PORTS  0x18    /* Coalescing Ports */
#define AHCI_EM_LOC     0x1C    /* Enclosure Management Location */
#define AHCI_EM_CTL     0x20    /* Enclosure Management Control */
#define AHCI_CAP2       0x24    /* Host Capabilities Extended */
#define AHCI_BOHC       0x28    /* BIOS/OS Handoff Control and Status */

/* 端口寄存器偏移 (每个端口 0x80 字节) */
#define AHCI_PORT_CLB       0x00    /* Command List Base Address */
#define AHCI_PORT_CLBU      0x04    /* Command List Base Address Upper */
#define AHCI_PORT_FB        0x08    /* FIS Base Address */
#define AHCI_PORT_FBU       0x0C    /* FIS Base Address Upper */
#define AHCI_PORT_IS        0x10    /* Interrupt Status */
#define AHCI_PORT_IE        0x14    /* Interrupt Enable */
#define AHCI_PORT_CMD       0x18    /* Command and Status */
#define AHCI_PORT_TFD       0x20    /* Task File Data */
#define AHCI_PORT_SIG       0x24    /* Signature */
#define AHCI_PORT_SSTS      0x28    /* SATA Status (SCR0: SStatus) */
#define AHCI_PORT_SCTL      0x2C    /* SATA Control (SCR2: SControl) */
#define AHCI_PORT_SERR      0x30    /* SATA Error (SCR1: SError) */
#define AHCI_PORT_SACT      0x34    /* SATA Active (SCR3: SActive) */
#define AHCI_PORT_CI        0x38    /* Command Issue */
#define AHCI_PORT_SNTF      0x3C    /* SATA Notification (SCR4: SNotification) */
#define AHCI_PORT_FBS       0x40    /* FIS-based Switching Control */
#define AHCI_PORT_DEVSLP    0x44    /* Device Sleep */
#define AHCI_PORT_VS        0x70    /* Vendor Specific */

/* GHC 标志 */
#define AHCI_GHC_AE     (1 << 31)   /* AHCI Enable */
#define AHCI_GHC_MRSM   (1 << 2)    /* MSI Revert to Single Message */
#define AHCI_GHC_IE     (1 << 1)    /* Interrupt Enable */
#define AHCI_GHC_HR     (1 << 0)    /* HBA Reset */

/* 端口命令标志 */
#define AHCI_CMD_ICC    (0xF << 28) /* Interface Communication Control */
#define AHCI_CMD_ASP    (1 << 27)   /* Aggressive Slumber/Partial */
#define AHCI_CMD_ALPE   (1 << 26)   /* Aggressive Link Power Management Enable */
#define AHCI_CMD_DLAE   (1 << 25)   /* Drive Light on ATAPI Enable */
#define AHCI_CMD_ATAPI  (1 << 24)   /* Device is ATAPI */
#define AHCI_CMD_APSTE  (1 << 23)   /* Automatic Partial to Slumber */
#define AHCI_CMD_FBSCP  (1 << 22)   /* FIS-based Switching Capable Port */
#define AHCI_CMD_ESP    (1 << 21)   /* External SATA Port */
#define AHCI_CMD_CPD    (1 << 20)   /* Cold Presence Detection */
#define AHCI_CMD_MPSP   (1 << 19)   /* Mechanical Presence Switch */
#define AHCI_CMD_HPCP   (1 << 18)   /* Hot Plug Capable Port */
#define AHCI_CMD_PMA    (1 << 17)   /* Port Multiplier Attached */
#define AHCI_CMD_CPS    (1 << 16)   /* Cold Presence State */
#define AHCI_CMD_CR     (1 << 15)   /* Command List Running */
#define AHCI_CMD_FR     (1 << 14)   /* FIS Receive Running */
#define AHCI_CMD_MPSS   (1 << 13)   /* Mechanical Presence Switch State */
#define AHCI_CMD_CCS    (0x1F << 8) /* Current Command Slot */
#define AHCI_CMD_FRE    (1 << 4)    /* FIS Receive Enable */
#define AHCI_CMD_CLO    (1 << 3)    /* Command List Override */
#define AHCI_CMD_POD    (1 << 2)    /* Power On Device */
#define AHCI_CMD_SUD    (1 << 1)    /* Start Device */
#define AHCI_CMD_ST     (1 << 0)    /* Start */

/* SATA 状态 */
#define AHCI_SSTS_DET_MASK      0xF
#define AHCI_SSTS_DET_NOPRESENT 0x0     /* No device present */
#define AHCI_SSTS_DET_PRESENT   0x1     /* Device present, no phy */
#define AHCI_SSTS_DET_PHY       0x3     /* Device present, phy established */
#define AHCI_SSTS_DET_OFFLINE   0x4     /* Phy offline */

#define AHCI_SSTS_SPD_MASK      0xF0
#define AHCI_SSTS_SPD_GEN1      0x10    /* Gen 1 (1.5 Gbps) */
#define AHCI_SSTS_SPD_GEN2      0x20    /* Gen 2 (3 Gbps) */
#define AHCI_SSTS_SPD_GEN3      0x30    /* Gen 3 (6 Gbps) */

#define AHCI_SSTS_IPM_MASK      0xF00
#define AHCI_SSTS_IPM_ACTIVE    0x100   /* Active */
#define AHCI_SSTS_IPM_PARTIAL   0x200   /* Partial */
#define AHCI_SSTS_IPM_SLUMBER   0x600   /* Slumber */

/* 设备签名 */
#define AHCI_SIG_ATA    0x00000101    /* ATA drive */
#define AHCI_SIG_ATAPI  0xEB140101    /* ATAPI drive */
#define AHCI_SIG_SEMB   0xC33C0101    /* Enclosure management bridge */
#define AHCI_SIG_PM     0x96690101    /* Port multiplier */

/* 命令表大小 */
#define AHCI_CMD_TABLE_SIZE   256   /* 256 PRDT entries max */
#define AHCI_PRDT_ENTRIES     256

/* FIS 类型 */
#define FIS_TYPE_REG_H2D      0x27    /* Register - Host to Device */
#define FIS_TYPE_REG_D2H      0x34    /* Register - Device to Host */
#define FIS_TYPE_DMA_ACT      0x39    /* DMA Activate */
#define FIS_TYPE_DMA_SETUP    0x41    /* DMA Setup */
#define FIS_TYPE_DATA         0x46    /* Data */
#define FIS_TYPE_BIST         0x58    /* BIST Activate */
#define FIS_TYPE_PIO_SETUP    0x5F    /* PIO Setup */
#define FIS_TYPE_DEV_BITS     0xA1    /* Set Device Bits */

/* ATA 命令 */
#define ATA_CMD_READ_DMA_EXT     0x25
#define ATA_CMD_WRITE_DMA_EXT    0x35
#define ATA_CMD_IDENTIFY         0xEC
#define ATA_CMD_IDENTIFY_PACKET  0xA1
#define ATA_CMD_PACKET           0xA0

/* Command Header */
struct ahci_cmd_header {
    uint32_t dw0;           /* DW0: CFL[4:0], PMP[8:5], B[10], R[11], P[12], W[13], A[14], PRDTL[31:16] */
    uint32_t prdbc;         /* DW1: PRD Byte Count */
    uint32_t ctba;          /* DW2: Command Table Base Address */
    uint32_t ctbau;         /* DW3: Command Table Base Address Upper */
    uint32_t reserved[4];   /* DW4-7: Reserved */
} __attribute__((packed));

/* PRDT (Physical Region Descriptor Table) */
struct ahci_prdt {
    uint32_t dba;           /* Data Base Address */
    uint32_t dbau;          /* Data Base Address Upper */
    uint32_t reserved;      /* Reserved */
    uint32_t dbc;           /* Data Byte Count (max 4MB - 1) */
} __attribute__((packed));

/* Command Table */
struct ahci_cmd_table {
    uint8_t  cfis[64];                              /* Command FIS */
    uint8_t  acmd[16];                              /* ATAPI Command */
    uint8_t  reserved[48];                          /* Reserved */
    struct ahci_prdt prdt[AHCI_PRDT_ENTRIES];       /* PRDT entries */
} __attribute__((packed));

/* FIS (Host to Device) */
struct ahci_fis_h2d {
    uint8_t  fis_type;      /* FIS type (0x27) */
    uint8_t  pm_port : 4;   /* Port multiplier */
    uint8_t  reserved0 : 3;
    uint8_t  c : 1;         /* Command bit */
    uint8_t  command;       /* Command */
    uint8_t  feature_low;   /* Feature (low) */
    uint8_t  lba0;          /* LBA 0 */
    uint8_t  lba1;          /* LBA 1 */
    uint8_t  lba2;          /* LBA 2 */
    uint8_t  device;        /* Device */
    uint8_t  lba3;          /* LBA 3 */
    uint8_t  lba4;          /* LBA 4 */
    uint8_t  lba5;          /* LBA 5 */
    uint8_t  feature_high;  /* Feature (high) */
    uint8_t  count_low;     /* Count (low) */
    uint8_t  count_high;    /* Count (high) */
    uint8_t  icc;           /* Isochronous Command Completion */
    uint8_t  control;       /* Control */
    uint8_t  reserved1[4];  /* Reserved */
} __attribute__((packed));

/* Received FIS */
struct ahci_received_fis {
    uint8_t  dsfis[28];     /* DMA Setup FIS */
    uint8_t  reserved0[4];
    uint8_t  psfis[20];     /* PIO Setup FIS */
    uint8_t  reserved1[12];
    uint8_t  rfis[20];      /* D2H Register FIS */
    uint8_t  reserved2[4];
    uint8_t  sdbfis[8];     /* Set Device Bits FIS */
    uint8_t  ufis[64];      /* Unknown FIS */
    uint8_t  reserved3[96];
} __attribute__((packed));

/* AHCI 端口结构 */
struct ahci_port {
    uint64_t base;              /* 端口寄存器基地址 */
    int      num;               /* 端口号 */
    int      type;              /* 设备类型 */
    int      is_atapi;          /* 是否为 ATAPI */
    struct ahci_cmd_header *cmd_list;    /* 命令列表 */
    struct ahci_cmd_table  *cmd_table;   /* 命令表 */
    struct ahci_received_fis *rfis;      /* 接收 FIS */
    uint64_t cmd_list_phys;     /* 命令列表物理地址 */
    uint64_t cmd_table_phys;    /* 命令表物理地址 */
    uint64_t rfis_phys;         /* FIS 物理地址 */
} __attribute__((packed));

/* AHCI 控制器结构 */
struct ahci_controller {
    uint64_t base;              /* 控制器基地址 (ABAR) */
    uint32_t capabilities;      /* CAP 寄存器 */
    uint32_t capabilities2;     /* CAP2 寄存器 */
    uint32_t version;           /* 版本号 */
    uint32_t ports_implemented; /* 已实现的端口 */
    int      port_count;        /* 端口数量 */
    struct ahci_port ports[32]; /* 端口数组 */
    int      initialized;       /* 是否已初始化 */
};

/* 初始化 AHCI 控制器 */
int ahci_init(void);

/* 检测 AHCI 控制器是否存在 */
int ahci_detect(void);

/* 获取 AHCI 控制器 */
struct ahci_controller *ahci_get_controller(void);

/* 读取扇区 */
int ahci_read_sector(uint64_t lba, void *buf);

/* 写入扇区 */
int ahci_write_sector(uint64_t lba, const void *buf);

/* 获取磁盘大小 */
uint64_t ahci_get_disk_size(void);

/* 检查 AHCI 是否可用 */
int ahci_is_available(void);

#endif /* AHCI_H */
