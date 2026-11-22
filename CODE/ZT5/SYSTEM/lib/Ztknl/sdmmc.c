/*
 * Zratro OS - SD/MMC 驱动
 *
 * 功能: SD 卡读写, SDHC/SDXC, eMMC, SPI 模式回退
 */

#include "sdmmc.h"
#include "console.h"
#include "heap.h"
#include "string.h"

/* ========== 全局主机列表 ========== */

static struct sdmmc_host sdmmc_hosts[SDMMC_MAX_DEVICES];
static int sdmmc_host_count = 0;

/* ========== 命令发送辅助 ========== */

static int sdmmc_send_cmd(struct sdmmc_host *host, uint8_t cmd, uint32_t arg,
                           uint8_t resp_type, uint32_t *resp) {
    if (host == NULL || host->ops == NULL || host->ops->send_cmd == NULL) return -1;
    return host->ops->send_cmd(host, cmd, arg, resp_type, resp);
}

/* ========== 卡初始化流程 ========== */

static int sdmmc_go_idle(struct sdmmc_host *host) {
    /* CMD0: 复位卡到空闲状态 */
    return sdmmc_send_cmd(host, SD_CMD_GO_IDLE_STATE, 0, SD_RESP_NONE, NULL);
}

static int sdmmc_send_if_cond(struct sdmmc_host *host) {
    /* CMD8: 发送接口条件 (SD 2.0+ 支持) */
    uint32_t arg = 0x000001AA;  /* 电压 2.7-3.6V, 检查模式 0xAA */
    uint32_t resp;
    int ret = sdmmc_send_cmd(host, SD_CMD_SEND_IF_COND, arg, SD_RESP_R7, &resp);
    if (ret == 0 && (resp & 0xFF) == 0xAA) {
        return 0;  /* SD 2.0+ 卡 */
    }
    return -1;  /* SD 1.x 或不支持 */
}

static int sdmmc_send_op_cond(struct sdmmc_host *host, int is_hcs) {
    /* ACMD41: 发送操作条件 */
    uint32_t arg = SD_OCR_VDD_27_28 | SD_OCR_VDD_28_29 | SD_OCR_VDD_29_30 |
                   SD_OCR_VDD_30_31 | SD_OCR_VDD_31_32 | SD_OCR_VDD_32_33 |
                   SD_OCR_VDD_33_34;
    if (is_hcs) arg |= SD_OCR_CCS;

    /* 先发 CMD55 (应用命令前缀) */
    uint32_t resp55;
    sdmmc_send_cmd(host, SD_CMD_APP_CMD, 0, SD_RESP_R1, &resp55);

    uint32_t resp;
    for (int retry = 0; retry < 100; retry++) {
        sdmmc_send_cmd(host, SD_CMD_APP_CMD, 0, SD_RESP_R1, &resp55);
        int ret = sdmmc_send_cmd(host, SD_ACMD_SD_SEND_OP_COND, arg, SD_RESP_R3, &resp);
        if (ret != 0) continue;
        if (resp & SD_OCR_BUSY) {
            host->card.ocr = resp;
            host->card.type = (resp & SD_OCR_CCS) ? SDMMC_TYPE_SDHC : SDMMC_TYPE_SD_V2;
            return 0;
        }
        /* 等待卡就绪 */
        for (volatile int i = 0; i < 100000; i++);
    }
    return -1;
}

static int sdmmc_all_send_cid(struct sdmmc_host *host) {
    /* CMD2: 请求所有卡发送 CID */
    uint32_t resp[4];
    int ret = sdmmc_send_cmd(host, SD_CMD_ALL_SEND_CID, 0, SD_RESP_R2, resp);
    if (ret != 0) return -1;

    /* 解析 CID */
    host->card.cid.mid = (resp[0] >> 24) & 0xFF;
    host->card.cid.oid[0] = (resp[0] >> 16) & 0xFF;
    host->card.cid.oid[1] = (resp[0] >> 8) & 0xFF;
    host->card.cid.pnm[0] = resp[0] & 0xFF;
    host->card.cid.pnm[1] = (resp[1] >> 24) & 0xFF;
    host->card.cid.pnm[2] = (resp[1] >> 16) & 0xFF;
    host->card.cid.pnm[3] = (resp[1] >> 8) & 0xFF;
    host->card.cid.pnm[4] = resp[1] & 0xFF;
    host->card.cid.prv = (resp[2] >> 24) & 0xFF;
    host->card.cid.psn = (resp[2] & 0xFFFFFF) | ((resp[3] & 0xFF) << 24);

    return 0;
}

static int sdmmc_send_rca(struct sdmmc_host *host) {
    /* CMD3: 请求卡发布新的 RCA */
    uint32_t resp;
    int ret = sdmmc_send_cmd(host, SD_CMD_SEND_RELATIVE_ADDR, 0, SD_RESP_R6, &resp);
    if (ret != 0) return -1;

    host->card.rca = (resp >> 16) & 0xFFFF;
    return 0;
}

static int sdmmc_select_card(struct sdmmc_host *host) {
    /* CMD7: 选择卡 */
    uint32_t arg = (uint32_t)host->card.rca << 16;
    return sdmmc_send_cmd(host, SD_CMD_SELECT_CARD, arg, SD_RESP_R1, NULL);
}

static int sdmmc_set_blocklen(struct sdmmc_host *host, uint32_t len) {
    /* CMD16: 设置块长度 (标准容量卡) */
    if (host->card.type == SDMMC_TYPE_SDHC || host->card.type == SDMMC_TYPE_SDXC) {
        return 0;  /* SDHC/SDXC 固定 512 字节块 */
    }
    return sdmmc_send_cmd(host, SD_CMD_SET_BLOCKLEN, len, SD_RESP_R1, NULL);
}

/* ========== 卡初始化入口 ========== */

int sdmmc_card_init(struct sdmmc_host *host) {
    if (host == NULL) return Z_ERR_NULL;

    printk("sdmmc: 初始化 SD/MMC 卡...");

    /* 设置低速时钟 (400kHz 用于识别) */
    if (host->ops && host->ops->set_clock) {
        host->ops->set_clock(host, 400000);
    }

    /* 步骤 1: 进入空闲状态 */
    sdmmc_go_idle(host);

    /* 步骤 2: 检查 SD 2.0+ */
    int is_v2 = (sdmmc_send_if_cond(host) == 0);

    /* 步骤 3: 发送操作条件 */
    if (sdmmc_send_op_cond(host, is_v2) != 0) {
        /* 尝试 SD 1.x */
        if (sdmmc_send_op_cond(host, 0) != 0) {
            printk("sdmmc: 卡初始化失败");
            return -1;
        }
        host->card.type = SDMMC_TYPE_SD_V1;
    }

    /* 步骤 4: 获取 CID */
    sdmmc_all_send_cid(host);

    /* 步骤 5: 发布 RCA */
    sdmmc_send_rca(host);

    /* 步骤 6: 选择卡 */
    sdmmc_select_card(host);

    /* 步骤 7: 设置块长度 */
    sdmmc_set_blocklen(host, SDMMC_BLOCK_SIZE);

    /* 提高时钟频率 */
    if (host->ops && host->ops->set_clock) {
        host->ops->set_clock(host, 25000000);  /* 25MHz */
    }

    /* 设置总线宽度 */
    if (host->ops && host->ops->set_bus_width) {
        host->ops->set_bus_width(host, 4);  /* 4-bit 模式 */
        host->card.bus_width = 4;
    }

    /* 计算容量 */
    host->card.block_size = SDMMC_BLOCK_SIZE;
    if (host->card.type == SDMMC_TYPE_SDHC || host->card.type == SDMMC_TYPE_SDXC) {
        /* SDHC/SDXC: 容量 = (C_SIZE+1) * 512KB */
        host->card.capacity = (uint64_t)(host->card.csd.c_size + 1) * 512 * 1024;
    } else {
        /* 标准容量: 容量 = (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^READ_BL_LEN */
        uint32_t mult = 1 << (host->card.csd.c_size_mult + 2);
        uint32_t bl_len = 1 << host->card.csd.read_bl_len;
        host->card.capacity = (uint64_t)(host->card.csd.c_size + 1) * mult * bl_len;
    }
    host->card.num_blocks = (uint32_t)(host->card.capacity / SDMMC_BLOCK_SIZE);

    host->card.initialized = 1;
    host->card.state = SDMMC_STATE_TRAN;

    printk("sdmmc: 卡初始化完成 类型=%s 容量=%lluMB RCA=%u",
           host->card.type == SDMMC_TYPE_SDHC ? "SDHC" :
           host->card.type == SDMMC_TYPE_SDXC ? "SDXC" :
           host->card.type == SDMMC_TYPE_SD_V2 ? "SDv2" :
           host->card.type == SDMMC_TYPE_SD_V1 ? "SDv1" : "未知",
           host->card.capacity / (1024 * 1024),
           host->card.rca);
    return 0;
}

/* ========== 块读写 ========== */

int sdmmc_read_blocks(struct sdmmc_card *card, uint32_t start_block,
                      void *buf, uint32_t count) {
    if (card == NULL || buf == NULL || !card->initialized) return Z_ERR_INVALID;
    if (card->host == NULL || card->host->ops == NULL) return Z_ERR_NOTSUP;

    struct sdmmc_host *host = card->host;
    unsigned long flags = spin_lock_irqsave(&host->lock);

    /* SDHC/SDXC 使用块地址, 标准卡使用字节地址 */
    uint32_t addr = (card->type == SDMMC_TYPE_SDHC || card->type == SDMMC_TYPE_SDXC)
                    ? start_block : (start_block * SDMMC_BLOCK_SIZE);

    int ret;
    if (count == 1) {
        /* 单块读 */
        ret = sdmmc_send_cmd(host, SD_CMD_READ_SINGLE_BLOCK, addr, SD_RESP_R1, NULL);
        if (ret == 0 && host->ops->read_data) {
            ret = host->ops->read_data(host, buf, SDMMC_BLOCK_SIZE);
        }
    } else {
        /* 多块读 */
        ret = sdmmc_send_cmd(host, SD_CMD_READ_MULTI_BLOCK, addr, SD_RESP_R1, NULL);
        if (ret == 0 && host->ops->read_data) {
            ret = host->ops->read_data(host, buf, SDMMC_BLOCK_SIZE * count);
        }
        /* 发送停止传输 */
        sdmmc_send_cmd(host, SD_CMD_STOP_TRANSMISSION, 0, SD_RESP_R1, NULL);
    }

    spin_unlock_irqrestore(&host->lock, flags);
    return ret;
}

int sdmmc_write_blocks(struct sdmmc_card *card, uint32_t start_block,
                       const void *buf, uint32_t count) {
    if (card == NULL || buf == NULL || !card->initialized) return Z_ERR_INVALID;
    if (card->write_protected) return Z_ERR_ACCESS;
    if (card->host == NULL || card->host->ops == NULL) return Z_ERR_NOTSUP;

    struct sdmmc_host *host = card->host;
    unsigned long flags = spin_lock_irqsave(&host->lock);

    uint32_t addr = (card->type == SDMMC_TYPE_SDHC || card->type == SDMMC_TYPE_SDXC)
                    ? start_block : (start_block * SDMMC_BLOCK_SIZE);

    int ret;
    if (count == 1) {
        ret = sdmmc_send_cmd(host, SD_CMD_WRITE_SINGLE_BLOCK, addr, SD_RESP_R1, NULL);
        if (ret == 0 && host->ops->write_data) {
            ret = host->ops->write_data(host, buf, SDMMC_BLOCK_SIZE);
        }
    } else {
        ret = sdmmc_send_cmd(host, SD_CMD_WRITE_MULTI_BLOCK, addr, SD_RESP_R1, NULL);
        if (ret == 0 && host->ops->write_data) {
            ret = host->ops->write_data(host, buf, SDMMC_BLOCK_SIZE * count);
        }
        sdmmc_send_cmd(host, SD_CMD_STOP_TRANSMISSION, 0, SD_RESP_R1, NULL);
    }

    spin_unlock_irqrestore(&host->lock, flags);
    return ret;
}

/* ========== 卡信息 ========== */

uint64_t sdmmc_get_capacity(struct sdmmc_card *card) {
    return card ? card->capacity : 0;
}

uint32_t sdmmc_get_block_size(struct sdmmc_card *card) {
    return card ? card->block_size : 0;
}

enum sdmmc_card_type sdmmc_get_card_type(struct sdmmc_card *card) {
    return card ? card->type : SDMMC_TYPE_UNKNOWN;
}

/* ========== SPI 模式 ========== */

int sdmmc_spi_init(struct sdmmc_host *host) {
    if (host == NULL) return Z_ERR_NULL;

    printk("sdmmc: 使用 SPI 模式回退...");

    /* SPI 模式初始化:
     * 1. 拉高 CS 并发送 80+ 个时钟
     * 2. 发送 CMD0
     * 3. 发送 CMD8 (检查 SD 2.0)
     * 4. 发送 ACMD41
     * 5. 发送 CMD1 (MMC 回退)
     */
    host->card.spi_mode = 1;

    /* 发送 CMD0 */
    sdmmc_send_cmd(host, SD_CMD_GO_IDLE_STATE, 0, SD_RESP_NONE, NULL);

    /* 尝试 SD 2.0 */
    if (sdmmc_send_if_cond(host) == 0) {
        sdmmc_send_op_cond(host, 1);
    } else {
        sdmmc_send_op_cond(host, 0);
    }

    return 0;
}

/* ========== 初始化 ========== */

int sdmmc_init(void) {
    printk("sdmmc: 初始化 SD/MMC 子系统...");
    sdmmc_host_count = 0;
    printk("sdmmc: SD/MMC 子系统初始化完成");
    return 0;
}

struct sdmmc_host *sdmmc_get_host(int id) {
    if (id < 0 || id >= sdmmc_host_count) return NULL;
    return &sdmmc_hosts[id];
}
