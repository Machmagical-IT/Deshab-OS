/*
 * Zratro OS - SD/MMC 驱动
 *
 * 功能: SD 卡读写, SDHC/SDXC, eMMC, SPI 模式回退
 */

#ifndef SDMMC_H
#define SDMMC_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== SD/MMC 常量 ========== */
#define SDMMC_MAX_DEVICES    4
#define SDMMC_BLOCK_SIZE     512    /* 标准块大小 */
#define SDMMC_MAX_BLOCKS     0x7FFFFF  /* 最大块数 (SDHC) */

/* 卡类型 */
enum sdmmc_card_type {
    SDMMC_TYPE_UNKNOWN = 0,
    SDMMC_TYPE_SD_V1,          /* SD 1.x (标准容量) */
    SDMMC_TYPE_SD_V2,          /* SD 2.0 (标准容量) */
    SDMMC_TYPE_SDHC,           /* SDHC (高容量) */
    SDMMC_TYPE_SDXC,           /* SDXC (扩展容量) */
    SDMMC_TYPE_EMMC,           /* eMMC */
    SDMMC_TYPE_MMC,            /* MMC */
};

/* 卡状态 */
enum sdmmc_state {
    SDMMC_STATE_IDLE = 0,
    SDMMC_STATE_READY,
    SDMMC_STATE_IDENT,
    SDMMC_STATE_STBY,
    SDMMC_STATE_TRAN,
    SDMMC_STATE_DATA,
    SDMMC_STATE_RCV,
    SDMMC_STATE_PRG,
    SDMMC_STATE_DIS,
};

/* SD 命令 */
#define SD_CMD_GO_IDLE_STATE     0
#define SD_CMD_ALL_SEND_CID      2
#define SD_CMD_SEND_RELATIVE_ADDR 3
#define SD_CMD_SET_DSR           4
#define SD_CMD_SWITCH_FUNC       6
#define SD_CMD_SELECT_CARD       7
#define SD_CMD_SEND_IF_COND      8
#define SD_CMD_SEND_CSD          9
#define SD_CMD_SEND_CID          10
#define SD_CMD_STOP_TRANSMISSION 12
#define SD_CMD_SEND_STATUS       13
#define SD_CMD_SET_BLOCKLEN      16
#define SD_CMD_READ_SINGLE_BLOCK 17
#define SD_CMD_READ_MULTI_BLOCK  18
#define SD_CMD_WRITE_SINGLE_BLOCK 24
#define SD_CMD_WRITE_MULTI_BLOCK 25
#define SD_CMD_PROGRAM_CSD       27
#define SD_CMD_SET_WRITE_PROT    28
#define SD_CMD_CLR_WRITE_PROT    29
#define SD_CMD_SEND_WRITE_PROT   30
#define SD_CMD_ERASE_WR_BLK_START 32
#define SD_CMD_ERASE_WR_BLK_END  33
#define SD_CMD_ERASE             38
#define SD_CMD_APP_CMD           55
#define SD_CMD_GEN_CMD           56

/* SD 应用命令 (需先发 CMD55) */
#define SD_ACMD_SET_BUS_WIDTH    6
#define SD_ACMD_SD_STATUS        13
#define SD_ACMD_SEND_NUM_WR_BLOCKS 22
#define SD_ACMD_SET_WR_BLK_ERASE_COUNT 23
#define SD_ACMD_SD_SEND_OP_COND  41
#define SD_ACMD_SET_CLR_CARD_DETECT 42
#define SD_ACMD_SEND_SCR         51

/* 响应类型 */
#define SD_RESP_NONE             0
#define SD_RESP_R1               1   /* 48位, 包含状态 */
#define SD_RESP_R2               2   /* 136位, CID/CSD */
#define SD_RESP_R3               3   /* 48位, OCR */
#define SD_RESP_R6               6   /* 48位, 发布的 RCA */
#define SD_RESP_R7               7   /* 48位, CMD8 响应 */

/* OCR 寄存器位 */
#define SD_OCR_VDD_27_28        (1 << 15)
#define SD_OCR_VDD_28_29        (1 << 16)
#define SD_OCR_VDD_29_30        (1 << 17)
#define SD_OCR_VDD_30_31        (1 << 18)
#define SD_OCR_VDD_31_32        (1 << 19)
#define SD_OCR_VDD_32_33        (1 << 20)
#define SD_OCR_VDD_33_34        (1 << 21)
#define SD_OCR_VDD_34_35        (1 << 22)
#define SD_OCR_VDD_35_36        (1 << 23)
#define SD_OCR_CCS              (1 << 30)  /* 卡容量状态 */
#define SD_OCR_BUSY             (1 << 31)  /* 上电完成 */

/* ========== CID 寄存器 ========== */
struct sdmmc_cid {
    uint8_t mid;                 /* 制造商 ID */
    char oid[2];                 /* OEM ID */
    char pnm[5];                 /* 产品名称 */
    uint8_t prv;                 /* 产品修订 */
    uint32_t psn;                /* 产品序列号 */
    uint16_t mdt;                /* 制造日期 */
} __attribute__((packed));

/* ========== CSD 寄存器 ========== */
struct sdmmc_csd {
    uint8_t csd_structure;       /* CSD 结构版本 */
    uint8_t taac;                /* 数据读取访问时间 */
    uint8_t nsac;                /* 时钟依赖读取时间 */
    uint8_t tran_speed;          /* 最大数据传输速率 */
    uint16_t ccc;                /* 卡命令类别 */
    uint8_t read_bl_len;         /* 最大读取块长度 */
    uint32_t c_size;             /* 设备大小 */
    uint8_t c_size_mult;         /* 设备大小乘数 */
    uint8_t sector_size;         /* 擦除扇区大小 */
    uint8_t write_bl_len;        /* 最大写入块长度 */
} __attribute__((packed));

/* ========== SD/MMC 设备结构 ========== */
struct sdmmc_host;

struct sdmmc_card {
    enum sdmmc_card_type type;   /* 卡类型 */
    enum sdmmc_state state;      /* 卡状态 */
    uint16_t rca;                /* 相对卡地址 */
    uint32_t ocr;                /* 操作条件寄存器 */
    struct sdmmc_cid cid;        /* 卡标识 */
    struct sdmmc_csd csd;        /* 卡特定数据 */

    /* 容量信息 */
    uint64_t capacity;           /* 总容量 (字节) */
    uint32_t block_size;         /* 块大小 */
    uint32_t num_blocks;         /* 块数量 */

    /* 总线配置 */
    uint8_t bus_width;           /* 总线宽度 (1/4/8) */
    uint32_t clock_freq;         /* 时钟频率 */
    int high_speed;              /* 是否高速模式 */

    /* SPI 模式回退 */
    int spi_mode;                /* 是否使用 SPI 模式 */

    /* 所属主机 */
    struct sdmmc_host *host;

    /* 状态 */
    int initialized;
    int write_protected;
};

/* SD/MMC 主机操作 */
struct sdmmc_host_ops {
    int (*send_cmd)(struct sdmmc_host *host, uint8_t cmd, uint32_t arg,
                    uint8_t resp_type, uint32_t *resp);
    int (*set_clock)(struct sdmmc_host *host, uint32_t freq);
    int (*set_bus_width)(struct sdmmc_host *host, uint8_t width);
    int (*read_data)(struct sdmmc_host *host, void *buf, size_t len);
    int (*write_data)(struct sdmmc_host *host, const void *buf, size_t len);
};

/* SD/MMC 主机控制器 */
struct sdmmc_host {
    char name[32];
    int id;
    struct sdmmc_host_ops *ops;
    void *priv;

    /* MMIO */
    uint64_t mmio_base;
    int irq;

    /* 卡 */
    struct sdmmc_card card;

    /* 状态 */
    int initialized;
    spinlock_t lock;
};

/* ========== API 函数声明 ========== */

/* 初始化 SD/MMC 子系统 */
int sdmmc_init(void);

/* 卡初始化 */
int sdmmc_card_init(struct sdmmc_host *host);

/* 块读写 */
int sdmmc_read_blocks(struct sdmmc_card *card, uint32_t start_block,
                      void *buf, uint32_t count);
int sdmmc_write_blocks(struct sdmmc_card *card, uint32_t start_block,
                       const void *buf, uint32_t count);

/* 卡信息 */
uint64_t sdmmc_get_capacity(struct sdmmc_card *card);
uint32_t sdmmc_get_block_size(struct sdmmc_card *card);
enum sdmmc_card_type sdmmc_get_card_type(struct sdmmc_card *card);

/* SPI 模式 */
int sdmmc_spi_init(struct sdmmc_host *host);

/* 获取主机实例 */
struct sdmmc_host *sdmmc_get_host(int id);

#endif /* SDMMC_H */
