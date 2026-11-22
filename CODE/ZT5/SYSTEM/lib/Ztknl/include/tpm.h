/*
 * Zratro OS - TPM 2.0 驱动
 *
 * 功能: TIS 接口, 随机数生成, 基本密钥操作
 */

#ifndef TPM_H
#define TPM_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== TPM TIS (TPM Interface Spec) 常量 ========== */
#define TPM_TIS_BASE           0xFED40000   /* 默认 MMIO 基址 */

/* TIS 寄存器偏移 */
#define TPM_TIS_ACCESS         0x000   /* 访问寄存器 */
#define TPM_TIS_INT_ENABLE     0x008   /* 中断使能 */
#define TPM_TIS_INT_VECTOR     0x00C   /* 中断向量 */
#define TPM_TIS_INT_STATUS     0x010   /* 中断状态 */
#define TPM_TIS_INTF_CAP       0x014   /* 接口能力 */
#define TPM_TIS_STS            0x018   /* 状态寄存器 (1字节偏移) */
#define TPM_TIS_STS_X          0x01B   /* 状态寄存器 (3字节偏移) */
#define TPM_TIS_DATA_FIFO      0x024   /* 数据 FIFO */
#define TPM_TIS_INTERFACE_ID   0x030   /* 接口标识 */
#define TPM_TIS_XDATA_FIFO     0x083   /* 扩展数据 FIFO */
#define TPM_TIS_DID_VID        0xF00   /* 设备/厂商 ID */
#define TPM_TIS_RID            0xF04   /* 修订 ID */

/* ACCESS 寄存器位 */
#define TPM_TIS_ACCESS_ACTIVE_LOCALITY  0x20
#define TPM_TIS_ACCESS_BEEN_SEIZED     0x40
#define TPM_TIS_ACCESS_SEIZE           0x10
#define TPM_TIS_ACCESS_PENDING_REQUEST 0x04
#define TPM_TIS_ACCESS_REQUEST_USE     0x02
#define TPM_TIS_ACCESS_TPM_ESTABLISHED 0x01

/* STS 寄存器位 */
#define TPM_TIS_STS_COMMAND_READY      0x02
#define TPM_TIS_STS_TPM_GO             0x20
#define TPM_TIS_STS_DATA_AVAILABLE     0x10
#define TPM_TIS_STS_EXPECT             0x08
#define TPM_TIS_STS_RESPONSE_RETRY     0x02
#define TPM_TIS_STS_VALID              0x80

/* ========== TPM 2.0 命令码 ========== */
#define TPM2_CC_FIRST                  0x0000011F
#define TPM2_CC_SELF_TEST              0x00000143
#define TPM2_CC_GET_CAPABILITY         0x0000017A
#define TPM2_CC_GET_RANDOM             0x0000017B
#define TPM2_CC_STIR_RANDOM            0x0000017C
#define TPM2_CC_CREATE_PRIMARY         0x00000131
#define TPM2_CC_CREATE                 0x00000153
#define TPM2_CC_LOAD                   0x00000157
#define TPM2_CC_UNSEAL                 0x0000015E
#define TPM2_CC_FLUSH_CONTEXT          0x00000165
#define TPM2_CC_PCR_EXTEND             0x00000182
#define TPM2_CC_PCR_READ               0x0000017E
#define TPM2_CC_SIGN                   0x0000015D
#define TPM2_CC_VERIFY_SIGNATURE       0x00000177
#define TPM2_CC_HASH                   0x00000164
#define TPM2_CC_HIERARCHY_CHANGE_AUTH  0x00000129
#define TPM2_CC_CLEAR                  0x00000126
#define TPM2_CC_SHUTDOWN               0x00000145
#define TPM2_CC_STARTUP                0x00000144

/* ========== TPM 响应码 ========== */
#define TPM2_RC_SUCCESS                0x000
#define TPM2_RC_BAD_TAG                0x01E
#define TPM2_RC_INITIALIZE             0x100
#define TPM2_RC_FAILURE                0x101
#define TPM2_RC_DISABLED               0x120

/* ========== TPM 结构 ========== */

/* TPM 命令头 */
struct tpm_cmd_header {
    uint16_t tag;              /* 标签: 0x8001=ST_NO_SESSIONS, 0x8002=ST_SESSIONS */
    uint32_t size;             /* 命令总大小 */
    uint32_t code;             /* 命令码 */
} __attribute__((packed));

/* TPM 响应头 */
struct tpm_resp_header {
    uint16_t tag;              /* 标签 */
    uint32_t size;             /* 响应总大小 */
    uint32_t code;             /* 响应码 */
} __attribute__((packed));

/* TPM 能力数据 */
struct tpm_capability {
    uint32_t tpm2_family;      /* TPM 2.0 */
    uint32_t firmware_version_major;
    uint32_t firmware_version_minor;
    uint32_t max_command_size;
    uint32_t max_response_size;
    uint32_t manufacturer_id;
};

/* TPM 设备结构 */
struct tpm_device {
    int initialized;                    /* 是否已初始化 */
    uint64_t mmio_base;                 /* MMIO 基址 */

    /* 能力 */
    struct tpm_capability caps;

    /* 本地性 */
    int active_locality;

    /* 状态 */
    int tpm_ready;                      /* TPM 是否就绪 */
    int self_test_passed;               /* 自检是否通过 */

    /* 统计 */
    uint64_t cmd_count;
    uint64_t error_count;

    /* 锁 */
    spinlock_t lock;
};

/* ========== API 函数声明 ========== */

/* 初始化 TPM 驱动 */
int tpm_init(void);

/* 探测 TPM 设备 */
int tpm_probe(void);

/* TIS 接口操作 */
int tpm_tis_request_locality(int locality);
int tpm_tis_release_locality(int locality);
int tpm_tis_send(const uint8_t *buf, size_t len);
int tpm_tis_recv(uint8_t *buf, size_t *len);

/* TPM 2.0 命令 */
int tpm2_send_cmd(const void *cmd, size_t cmd_len, void *resp, size_t *resp_len);

/* 自检 */
int tpm2_self_test(int full_test);

/* 随机数生成 */
int tpm2_get_random(void *buf, size_t len);
int tpm2_stir_random(const void *data, size_t len);

/* 密钥操作 */
int tpm2_create_primary_key(void);
int tpm2_flush_context(uint32_t handle);

/* PCR 操作 */
int tpm2_pcr_extend(uint32_t pcr_idx, const void *digest, size_t digest_len);
int tpm2_pcr_read(uint32_t pcr_idx, void *digest, size_t digest_len);

/* 启动/关机 */
int tpm2_startup(void);
int tpm2_shutdown(void);

/* 获取设备实例 */
struct tpm_device *tpm_get_device(void);

#endif /* TPM_H */
