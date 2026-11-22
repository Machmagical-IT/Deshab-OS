/*
 * Zratro OS - TPM 2.0 驱动
 *
 * 功能: TIS 接口, 随机数生成, 基本密钥操作
 */

#include "tpm.h"
#include "console.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"

/* ========== MMIO 寄存器访问 ========== */

static inline uint8_t tpm_read8(struct tpm_device *dev, uint32_t reg) {
    return *(volatile uint8_t *)(dev->mmio_base + reg);
}

static inline uint32_t tpm_read32(struct tpm_device *dev, uint32_t reg) {
    return *(volatile uint32_t *)(dev->mmio_base + reg);
}

static inline void tpm_write8(struct tpm_device *dev, uint32_t reg, uint8_t val) {
    *(volatile uint8_t *)(dev->mmio_base + reg) = val;
}

static inline void tpm_write32(struct tpm_device *dev, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(dev->mmio_base + reg) = val;
}

/* ========== 全局设备实例 ========== */

static struct tpm_device tpm_dev;

/* ========== TIS 本地性管理 ========== */

int tpm_tis_request_locality(int locality) {
    struct tpm_device *dev = &tpm_dev;
    if (locality < 0 || locality > 4) return -1;

    uint32_t base = locality * 0x1000;

    /* 请求使用本地性 */
    tpm_write8(dev, base + TPM_TIS_ACCESS, TPM_TIS_ACCESS_REQUEST_USE);

    /* 等待获取本地性 */
    for (int timeout = 0; timeout < 1000; timeout++) {
        uint8_t access = tpm_read8(dev, base + TPM_TIS_ACCESS);
        if (access & TPM_TIS_ACCESS_ACTIVE_LOCALITY) {
            dev->active_locality = locality;
            return 0;
        }
        for (volatile int i = 0; i < 100; i++);
    }

    printk("tpm: 请求本地性 %d 超时", locality);
    return -1;
}

int tpm_tis_release_locality(int locality) {
    struct tpm_device *dev = &tpm_dev;
    if (locality < 0 || locality > 4) return -1;

    uint32_t base = locality * 0x1000;
    uint8_t access = tpm_read8(dev, base + TPM_TIS_ACCESS);

    /* 只有活跃的本地性可以释放 */
    if (access & TPM_TIS_ACCESS_ACTIVE_LOCALITY) {
        tpm_write8(dev, base + TPM_TIS_ACCESS, TPM_TIS_ACCESS_ACTIVE_LOCALITY);
        dev->active_locality = -1;
    }
    return 0;
}

/* ========== TIS 数据传输 ========== */

static int tpm_tis_wait_for_status(struct tpm_device *dev, uint8_t mask,
                                    uint8_t value, int timeout_ms) {
    uint32_t base = dev->active_locality * 0x1000;

    for (int i = 0; i < timeout_ms * 10; i++) {
        uint8_t sts = tpm_read8(dev, base + TPM_TIS_STS);
        if ((sts & mask) == value) return 0;
        for (volatile int j = 0; j < 100; j++);
    }
    return -1;  /* 超时 */
}

int tpm_tis_send(const uint8_t *buf, size_t len) {
    struct tpm_device *dev = &tpm_dev;
    if (buf == NULL || len == 0) return Z_ERR_NULL;

    uint32_t base = dev->active_locality * 0x1000;

    /* 发送 COMMAND_READY */
    tpm_write8(dev, base + TPM_TIS_STS, TPM_TIS_STS_COMMAND_READY);

    /* 等待就绪 */
    if (tpm_tis_wait_for_status(dev, TPM_TIS_STS_COMMAND_READY,
                                 TPM_TIS_STS_COMMAND_READY, 1000) != 0) {
        printk("tpm: 命令就绪超时");
        return -1;
    }

    /* 写入命令数据 */
    size_t offset = 0;
    int burst_count;

    while (offset < len) {
        /* 获取突发计数 */
        burst_count = (int)((tpm_read8(dev, base + TPM_TIS_STS_X + 1) << 8) |
                            tpm_read8(dev, base + TPM_TIS_STS_X + 2));
        if (burst_count == 0) {
            for (volatile int i = 0; i < 100; i++);
            continue;
        }

        size_t count = MIN((size_t)burst_count, len - offset);
        for (size_t i = 0; i < count; i++) {
            tpm_write8(dev, base + TPM_TIS_DATA_FIFO, buf[offset++]);
        }

        /* 检查是否期望更多数据 */
        if (offset < len) {
            uint8_t sts = tpm_read8(dev, base + TPM_TIS_STS);
            if (!(sts & TPM_TIS_STS_EXPECT)) {
                printk("tpm: 发送意外终止 (偏移=%zu/%zu)", offset, len);
                return -1;
            }
        }
    }

    /* 发送 TPM_GO */
    tpm_write8(dev, base + TPM_TIS_STS, TPM_TIS_STS_TPM_GO);

    return 0;
}

int tpm_tis_recv(uint8_t *buf, size_t *len) {
    struct tpm_device *dev = &tpm_dev;
    if (buf == NULL || len == NULL) return Z_ERR_NULL;

    uint32_t base = dev->active_locality * 0x1000;

    /* 等待数据可用 */
    if (tpm_tis_wait_for_status(dev, TPM_TIS_STS_DATA_AVAILABLE,
                                 TPM_TIS_STS_DATA_AVAILABLE, 5000) != 0) {
        printk("tpm: 等待响应超时");
        return -1;
    }

    /* 读取响应头 (10 字节) */
    size_t offset = 0;
    int burst_count;

    /* 先读取头部确定响应长度 */
    for (int i = 0; i < 10 && offset < *len; i++) {
        burst_count = (int)((tpm_read8(dev, base + TPM_TIS_STS_X + 1) << 8) |
                            tpm_read8(dev, base + TPM_TIS_STS_X + 2));
        if (burst_count == 0) break;

        size_t count = MIN((size_t)burst_count, 10 - offset);
        for (size_t j = 0; j < count && offset < *len; j++) {
            buf[offset++] = tpm_read8(dev, base + TPM_TIS_DATA_FIFO);
        }
    }

    /* 解析响应大小 */
    if (offset >= 6) {
        uint32_t resp_size = (buf[2] << 24) | (buf[3] << 16) | (buf[4] << 8) | buf[5];
        if (resp_size > *len) {
            printk("tpm: 响应过大 (%u > %zu)", resp_size, *len);
            *len = offset;
            return -1;
        }

        /* 继续读取剩余数据 */
        while (offset < resp_size) {
            burst_count = (int)((tpm_read8(dev, base + TPM_TIS_STS_X + 1) << 8) |
                                tpm_read8(dev, base + TPM_TIS_STS_X + 2));
            if (burst_count == 0) {
                if (tpm_tis_wait_for_status(dev, TPM_TIS_STS_DATA_AVAILABLE,
                                             TPM_TIS_STS_DATA_AVAILABLE, 100) != 0) {
                    break;
                }
                continue;
            }

            size_t count = MIN((size_t)burst_count, resp_size - offset);
            for (size_t j = 0; j < count; j++) {
                buf[offset++] = tpm_read8(dev, base + TPM_TIS_DATA_FIFO);
            }
        }

        *len = offset;
    }

    return 0;
}

/* ========== TPM 2.0 命令 ========== */

int tpm2_send_cmd(const void *cmd, size_t cmd_len, void *resp, size_t *resp_len) {
    struct tpm_device *dev = &tpm_dev;
    if (cmd == NULL || resp == NULL || resp_len == NULL) return Z_ERR_NULL;

    unsigned long flags = spin_lock_irqsave(&dev->lock);

    /* 发送命令 */
    int ret = tpm_tis_send((const uint8_t *)cmd, cmd_len);
    if (ret < 0) {
        dev->error_count++;
        spin_unlock_irqrestore(&dev->lock, flags);
        return ret;
    }

    /* 接收响应 */
    ret = tpm_tis_recv((uint8_t *)resp, resp_len);
    if (ret < 0) {
        dev->error_count++;
        spin_unlock_irqrestore(&dev->lock, flags);
        return ret;
    }

    dev->cmd_count++;

    /* 检查响应码 */
    struct tpm_resp_header *hdr = (struct tpm_resp_header *)resp;
    if (hdr->code != TPM2_RC_SUCCESS) {
        printk("tpm: 命令失败 响应码=0x%08X", hdr->code);
        spin_unlock_irqrestore(&dev->lock, flags);
        return -1;
    }

    spin_unlock_irqrestore(&dev->lock, flags);
    return 0;
}

/* ========== 自检 ========== */

int tpm2_self_test(int full_test) {
    /* 构造 TPM2_SelfTest 命令 */
    uint8_t cmd[12];
    struct tpm_cmd_header *hdr = (struct tpm_cmd_header *)cmd;
    hdr->tag = 0x8001;  /* ST_NO_SESSIONS */
    hdr->size = 12;
    hdr->code = TPM2_CC_SELF_TEST;
    cmd[10] = full_test ? 0x01 : 0x00;  /* fullTest */

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);

    int ret = tpm2_send_cmd(cmd, sizeof(cmd), resp, &resp_len);
    if (ret == 0) {
        tpm_dev.self_test_passed = 1;
        printk("tpm: 自检通过 (%s)", full_test ? "完整" : "部分");
    }
    return ret;
}

/* ========== 随机数生成 ========== */

int tpm2_get_random(void *buf, size_t len) {
    if (buf == NULL || len == 0 || len > 1024) return Z_ERR_INVALID;

    /* 构造 TPM2_GetRandom 命令 */
    uint8_t cmd[14];
    struct tpm_cmd_header *hdr = (struct tpm_cmd_header *)cmd;
    hdr->tag = 0x8001;
    hdr->size = 14;
    hdr->code = TPM2_CC_GET_RANDOM;
    cmd[10] = (len >> 8) & 0xFF;  /* bytesRequested (16位) */
    cmd[11] = len & 0xFF;
    cmd[12] = 0;
    cmd[13] = 0;

    uint8_t resp[1024 + 20];
    size_t resp_len = sizeof(resp);

    int ret = tpm2_send_cmd(cmd, 14, resp, &resp_len);
    if (ret != 0) return ret;

    /* 解析响应: header(10) + parameterSize(4) + randomBytes */
    if (resp_len < 14) return -1;

    uint32_t param_size = (resp[10] << 24) | (resp[11] << 16) |
                          (resp[12] << 8) | resp[13];
    if (param_size < 2 || param_size > len + 2) return -1;

    /* 读取随机数据 */
    uint16_t random_len = (resp[14] << 8) | resp[15];
    if (random_len > len) random_len = (uint16_t)len;

    memcpy(buf, &resp[16], random_len);
    return random_len;
}

int tpm2_stir_random(const void *data, size_t len) {
    if (data == NULL || len == 0 || len > 128) return Z_ERR_INVALID;

    /* 构造 TPM2_StirRandom 命令 */
    uint8_t cmd[12 + 4 + 128];
    struct tpm_cmd_header *hdr = (struct tpm_cmd_header *)cmd;
    hdr->tag = 0x8001;
    hdr->size = 12 + 4 + (uint32_t)len;
    hdr->code = TPM2_CC_STIR_RANDOM;

    /* inData 参数 */
    cmd[10] = 0x00;  /* TPML_DIGEST_VALUES: count = 0 */
    cmd[11] = 0x00;
    cmd[12] = 0x00;
    cmd[13] = 0x01;  /* 1 个摘要 */
    cmd[14] = 0x00;  /* hashAlg = TPM_ALG_SHA256 */
    cmd[15] = 0x0B;
    cmd[16] = (len >> 8) & 0xFF;  /* digest size */
    cmd[17] = len & 0xFF;
    memcpy(&cmd[18], data, len);

    hdr->size = 18 + (uint32_t)len;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);
    return tpm2_send_cmd(cmd, hdr->size, resp, &resp_len);
}

/* ========== 密钥操作 ========== */

int tpm2_create_primary_key(void) {
    /* TODO: 完整的 CreatePrimary 需要:
     * 1. 构造 TPMS_SENSITIVE_CREATE
     * 2. 构造 TPMS_PUBLIC (RSA/ECDSA)
     * 3. 发送 TPM2_CreatePrimary 命令
     * 4. 保存返回的句柄
     */
    printk("tpm: CreatePrimary 密钥操作 (待实现)");
    return Z_ERR_NOTSUP;
}

int tpm2_flush_context(uint32_t handle) {
    uint8_t cmd[14];
    struct tpm_cmd_header *hdr = (struct tpm_cmd_header *)cmd;
    hdr->tag = 0x8001;
    hdr->size = 14;
    hdr->code = TPM2_CC_FLUSH_CONTEXT;
    cmd[10] = (handle >> 24) & 0xFF;
    cmd[11] = (handle >> 16) & 0xFF;
    cmd[12] = (handle >> 8) & 0xFF;
    cmd[13] = handle & 0xFF;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);
    return tpm2_send_cmd(cmd, 14, resp, &resp_len);
}

/* ========== PCR 操作 ========== */

int tpm2_pcr_extend(uint32_t pcr_idx, const void *digest, size_t digest_len) {
    if (digest == NULL || digest_len != 32) return Z_ERR_INVALID;

    /* TPM2_PCR_Extend 命令 */
    uint8_t cmd[12 + 4 + 2 + 2 + 32];
    struct tpm_cmd_header *hdr = (struct tpm_cmd_header *)cmd;
    hdr->tag = 0x8001;
    hdr->code = TPM2_CC_PCR_EXTEND;

    /* pcrHandle */
    cmd[10] = (pcr_idx >> 24) & 0xFF;
    cmd[11] = (pcr_idx >> 16) & 0xFF;
    cmd[12] = (pcr_idx >> 8) & 0xFF;
    cmd[13] = pcr_idx & 0xFF;

    /* digest count */
    cmd[14] = 0; cmd[15] = 0; cmd[16] = 0; cmd[17] = 1;

    /* hashAlg + digest */
    cmd[18] = 0x00; cmd[19] = 0x0B;  /* SHA256 */
    memcpy(&cmd[20], digest, 32);

    hdr->size = 52;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);
    return tpm2_send_cmd(cmd, 52, resp, &resp_len);
}

int tpm2_pcr_read(uint32_t pcr_idx, void *digest, size_t digest_len) {
    if (digest == NULL || digest_len < 32) return Z_ERR_INVALID;

    /* TPM2_PCR_Read 命令 */
    uint8_t cmd[12 + 4 + 2 + 3];
    struct tpm_cmd_header *hdr = (struct tpm_cmd_header *)cmd;
    hdr->tag = 0x8001;
    hdr->size = 21;
    hdr->code = TPM2_CC_PCR_READ;

    /* TPML_PCR_SELECTION: 1 selection */
    cmd[10] = 0; cmd[11] = 0; cmd[12] = 0; cmd[13] = 1;
    /* hashAlg = SHA256 */
    cmd[14] = 0x00; cmd[15] = 0x0B;
    /* sizeofSelect = 3 */
    cmd[16] = 3;
    /* pcrSelect: 设置指定位 */
    cmd[17] = (pcr_idx < 8) ? (1 << pcr_idx) : 0;
    cmd[18] = (pcr_idx >= 8 && pcr_idx < 16) ? (1 << (pcr_idx - 8)) : 0;
    cmd[19] = (pcr_idx >= 16 && pcr_idx < 24) ? (1 << (pcr_idx - 16)) : 0;

    uint8_t resp[256];
    size_t resp_len = sizeof(resp);

    int ret = tpm2_send_cmd(cmd, 21, resp, &resp_len);
    if (ret != 0) return ret;

    /* 解析 PCR 值 (简化) */
    if (resp_len > 40) {
        memcpy(digest, &resp[resp_len - 32], 32);
    }

    return 0;
}

/* ========== 启动/关机 ========== */

int tpm2_startup(void) {
    uint8_t cmd[14];
    struct tpm_cmd_header *hdr = (struct tpm_cmd_header *)cmd;
    hdr->tag = 0x8001;
    hdr->size = 14;
    hdr->code = TPM2_CC_STARTUP;
    /* startupType: TPM_SU_CLEAR = 0x0000 */
    cmd[10] = 0x00;
    cmd[11] = 0x00;
    cmd[12] = 0;
    cmd[13] = 0;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);
    return tpm2_send_cmd(cmd, 14, resp, &resp_len);
}

int tpm2_shutdown(void) {
    uint8_t cmd[14];
    struct tpm_cmd_header *hdr = (struct tpm_cmd_header *)cmd;
    hdr->tag = 0x8001;
    hdr->size = 14;
    hdr->code = TPM2_CC_SHUTDOWN;
    /* shutdownType: TPM_SU_CLEAR = 0x0000 */
    cmd[10] = 0x00;
    cmd[11] = 0x00;
    cmd[12] = 0;
    cmd[13] = 0;

    uint8_t resp[64];
    size_t resp_len = sizeof(resp);
    return tpm2_send_cmd(cmd, 14, resp, &resp_len);
}

/* ========== 探测 ========== */

int tpm_probe(void) {
    struct tpm_device *dev = &tpm_dev;

    /* 尝试默认 TIS 基址 */
    dev->mmio_base = (uint64_t)phys_to_virt(TPM_TIS_BASE);

    /* 读取接口标识验证 */
    uint32_t did_vid = tpm_read32(dev, TPM_TIS_DID_VID);
    if (did_vid == 0 || did_vid == 0xFFFFFFFF) {
        printk("tpm: 未检测到 TPM 设备");
        return -1;
    }

    uint16_t vid = did_vid & 0xFFFF;
    uint16_t did = (did_vid >> 16) & 0xFFFF;
    printk("tpm: 发现 TPM 设备 VID=0x%04X DID=0x%04X", vid, did);

    return 0;
}

/* ========== 初始化 ========== */

int tpm_init(void) {
    printk("tpm: 初始化 TPM 2.0 驱动...");

    memset(&tpm_dev, 0, sizeof(tpm_dev));
    spinlock_init(&tpm_dev.lock);
    tpm_dev.active_locality = -1;

    /* 探测 TPM */
    if (tpm_probe() != 0) {
        printk("tpm: TPM 设备未找到");
        return -1;
    }

    /* 请求本地性 0 */
    if (tpm_tis_request_locality(0) != 0) {
        printk("tpm: 请求本地性 0 失败");
        return -1;
    }

    /* 发送 TPM2_Startup */
    int ret = tpm2_startup();
    if (ret != 0) {
        /* 可能已经启动, 不是致命错误 */
        printk("tpm: Startup 返回 %d (可能已启动)", ret);
    }

    /* 执行自检 */
    ret = tpm2_self_test(1);
    if (ret != 0) {
        printk("tpm: 自检失败");
    }

    tpm_dev.initialized = 1;
    printk("tpm: TPM 2.0 驱动初始化完成");
    return 0;
}

struct tpm_device *tpm_get_device(void) {
    return &tpm_dev;
}
