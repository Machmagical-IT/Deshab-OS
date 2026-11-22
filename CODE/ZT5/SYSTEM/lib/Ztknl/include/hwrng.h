/*
 * Zratro OS - 硬件随机数生成器驱动
 *
 * 功能: Intel RDRAND, Virtio-RNG, 熵池管理, /dev/random 接口
 */

#ifndef HWRNG_H
#define HWRNG_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== 熵源类型 ========== */
#define HWRNG_SRC_NONE          0
#define HWRNG_SRC_RDRAND        1    /* Intel RDRAND/RDSEED */
#define HWRNG_SRC_VIRTIO        2    /* Virtio-RNG */
#define HWRNG_SRC_TPM           3    /* TPM RNG */
#define HWRNG_SRC_TIMER         4    /* 定时器抖动 (软件回退) */

/* ========== 熵池参数 ========== */
#define HWRNG_POOL_SIZE         4096   /* 熵池大小 (字节) */
#define HWRNG_POOL_WORDS        (HWRNG_POOL_SIZE / 4)
#define HWRNG_MIN_ENTROPY_BITS  128    /* 最小熵位数才允许读取 */
#define HWRNG_MAX_ENTROPY_BITS  (HWRNG_POOL_SIZE * 8)

/* ========== SHA-256 常量 (用于熵池混合) ========== */
#define SHA256_BLOCK_SIZE       64
#define SHA256_DIGEST_SIZE      32

/* ========== 熵池结构 ========== */
struct entropy_pool {
    uint32_t pool[HWRNG_POOL_WORDS];  /* 熵池数据 */
    uint32_t digest[8];               /* SHA-256 中间摘要 */
    int entropy_count;                /* 当前熵位估计 */
    spinlock_t lock;                  /* 自旋锁 */

    /* 统计 */
    uint64_t bytes_added;
    uint64_t bytes_extracted;
    uint64_t mix_count;
};

/* ========== HWRNG 设备结构 ========== */
struct hwrng_device {
    int initialized;                    /* 是否已初始化 */
    int active_source;                  /* 当前活跃熵源 */

    /* 熵源能力 */
    int has_rdrand;                     /* 是否支持 RDRAND */
    int has_rdseed;                     /* 是否支持 RDSEED */
    int has_virtio;                     /* 是否有 Virtio-RNG */

    /* 熵池 */
    struct entropy_pool input_pool;     /* 输入熵池 */
    struct entropy_pool output_pool;    /* 输出熵池 (从输入池提取) */

    /* SHA-256 上下文 (用于混合) */
    uint32_t sha256_state[8];
    uint8_t sha256_buf[SHA256_BLOCK_SIZE];
    uint32_t sha256_buflen;

    /* 锁 */
    spinlock_t read_lock;

    /* 统计 */
    uint64_t rdrand_count;
    uint64_t virtio_count;
    uint64_t fallback_count;
};

/* ========== API 函数声明 ========== */

/* 初始化 HWRNG 子系统 */
int hwrng_init(void);

/* 读取随机字节 */
int hwrng_read(void *buf, size_t len);

/* 读取非阻塞随机字节 */
int hwrng_read_nonblock(void *buf, size_t len);

/* 向熵池添加熵 */
int hwrng_add_entropy(const void *data, size_t len, int entropy_bits);

/* 获取当前熵位估计 */
int hwrng_get_entropy_count(void);

/* Intel RDRAND/RDSEED */
int hwrng_rdrand_available(void);
int hwrng_rdrand_read(uint64_t *val);
int hwrng_rdseed_read(uint64_t *val);

/* Virtio-RNG */
int hwrng_virtio_init(void);
int hwrng_virtio_read(void *buf, size_t len);

/* 熵池混合 (SHA-256) */
void hwrng_pool_mix(struct entropy_pool *pool);

/* 软件回退: 定时器抖动 */
int hwrng_timer_jitter(void *buf, size_t len);

/* 获取设备实例 */
struct hwrng_device *hwrng_get_device(void);

#endif /* HWRNG_H */
