/*
 * Zratro OS - 硬件随机数生成器驱动
 *
 * 功能: Intel RDRAND, Virtio-RNG, 熵池管理, /dev/random 接口
 */

#include "hwrng.h"
#include "console.h"
#include "string.h"
#include "pmm.h"
#include "pci.h"

/* ========== SHA-256 实现 (用于熵池混合) ========== */

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA256_CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_SIGMA0(x) (SHA256_ROTR(x, 2) ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22))
#define SHA256_SIGMA1(x) (SHA256_ROTR(x, 6) ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25))
#define SHA256_sigma0(x) (SHA256_ROTR(x, 7) ^ SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define SHA256_sigma1(x) (SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ ((x) >> 10))

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;

    /* 准备消息调度 */
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SHA256_sigma1(w[i - 2]) + w[i - 7] +
               SHA256_sigma0(w[i - 15]) + w[i - 16];
    }

    /* 初始化工作变量 */
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    /* 压缩 */
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + SHA256_SIGMA1(e) + SHA256_CH(e, f, g) + sha256_k[i] + w[i];
        uint32_t t2 = SHA256_SIGMA0(a) + SHA256_MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

/* ========== 全局设备实例 ========== */

static struct hwrng_device hwrng_dev;

/* ========== Intel RDRAND/RDSEED ========== */

int hwrng_rdrand_available(void) {
    /* 检查 CPU 是否支持 RDRAND (ECX bit 30) */
    uint32_t eax, ebx, ecx, edx;
    eax = 1;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax));
    return (ecx & (1 << 30)) != 0;
}

int hwrng_rdrand_read(uint64_t *val) {
    /* 尝试 RDRAND 指令, 带重试 */
    for (int retry = 0; retry < 10; retry++) {
        unsigned char ok;
        uint64_t rand_val;
        asm volatile("rdrand %0; setc %1"
                     : "=r"(rand_val), "=qm"(ok)
                     :: "cc");
        if (ok) {
            *val = rand_val;
            return 0;
        }
    }
    return -1;  /* RDRAND 失败 */
}

int hwrng_rdseed_read(uint64_t *val) {
    /* 检查 RDSEED 支持 (EBX bit 18 of CPUID leaf 7) */
    uint32_t eax, ebx, ecx, edx;
    eax = 7; ecx = 0;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax), "c"(ecx));
    if (!(ebx & (1 << 18))) return -1;

    for (int retry = 0; retry < 10; retry++) {
        unsigned char ok;
        uint64_t seed_val;
        asm volatile("rdseed %0; setc %1"
                     : "=r"(seed_val), "=qm"(ok)
                     :: "cc");
        if (ok) {
            *val = seed_val;
            return 0;
        }
    }
    return -1;
}

/* ========== Virtio-RNG ========== */

int hwrng_virtio_init(void) {
    /* 扫描 PCI 总线寻找 Virtio-RNG 设备 (1af4:1005) */
    for (uint8_t bus = 0; bus < 255; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor = pci_read_config32(bus, dev, func, 0x00);
                if (vendor == 0xFFFFFFFF) continue;

                uint16_t vid = vendor & 0xFFFF;
                uint16_t did = (vendor >> 16) & 0xFFFF;

                /* Virtio RNG: Vendor=0x1AF4, Device=0x1005 */
                if (vid == 0x1AF4 && did == 0x1005) {
                    printk("hwrng: 发现 Virtio-RNG 设备");
                    hwrng_dev.has_virtio = 1;
                    return 0;
                }
            }
        }
    }
    return -1;
}

int hwrng_virtio_read(void *buf, size_t len) {
    /* TODO: 完整的 Virtio-RNG 传输需要 Virtio 队列支持
     * 这里提供框架，实际传输依赖 virtio.c 的队列操作 */
    (void)buf;
    (void)len;
    return -1;
}

/* ========== 熵池操作 ========== */

void hwrng_pool_mix(struct entropy_pool *pool) {
    /* 使用 SHA-256 混合熵池 */
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    /* 对熵池数据进行多轮 SHA-256 混合 */
    for (int i = 0; i < HWRNG_POOL_WORDS; i += 16) {
        sha256_transform(state, (const uint8_t *)&pool->pool[i]);
    }

    /* 将混合结果写回熵池 */
    for (int i = 0; i < 8; i++) {
        pool->digest[i] = state[i];
    }

    pool->mix_count++;
}

int hwrng_add_entropy(const void *data, size_t len, int entropy_bits) {
    if (data == NULL || len == 0) return Z_ERR_NULL;

    struct entropy_pool *pool = &hwrng_dev.input_pool;
    unsigned long flags = spin_lock_irqsave(&pool->lock);

    /* 将数据混合到熵池中 (使用简单折叠) */
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t i = (uint32_t)pool->bytes_added;

    for (size_t j = 0; j < len; j++) {
        uint32_t word_idx = (i / 4) % HWRNG_POOL_WORDS;
        uint32_t byte_shift = (i % 4) * 8;
        pool->pool[word_idx] ^= ((uint32_t)bytes[j] << byte_shift);
        i++;
    }

    pool->bytes_added += len;
    pool->entropy_count = MIN(pool->entropy_count + entropy_bits,
                               HWRNG_MAX_ENTROPY_BITS);

    /* 每 64 字节混合一次 */
    if (pool->bytes_added % 64 == 0) {
        hwrng_pool_mix(pool);
    }

    spin_unlock_irqrestore(&pool->lock, flags);
    return 0;
}

int hwrng_get_entropy_count(void) {
    return hwrng_dev.input_pool.entropy_count;
}

/* ========== 软件回退: 定时器抖动 ========== */

int hwrng_timer_jitter(void *buf, size_t len) {
    uint8_t *out = (uint8_t *)buf;
    if (out == NULL) return Z_ERR_NULL;

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = 0;
        /* 使用 TSC 低位和循环抖动收集熵 */
        for (int bit = 0; bit < 8; bit++) {
            uint64_t tsc1, tsc2;
            asm volatile("rdtsc" : "=A"(tsc1));
            /* 执行一些不可预测的操作 */
            for (volatile int j = 0; j < (tsc1 & 0xF); j++);
            asm volatile("rdtsc" : "=A"(tsc2));
            byte |= ((tsc2 ^ (tsc2 >> 8)) & 1) << bit;
        }
        out[i] = byte;
    }

    hwrng_dev.fallback_count += len;
    return (int)len;
}

/* ========== 随机数读取 ========== */

static int hwrng_fill_from_hw(void *buf, size_t len) {
    uint8_t *out = (uint8_t *)buf;

    /* 优先使用 RDRAND */
    if (hwrng_dev.has_rdrand) {
        size_t filled = 0;
        while (filled < len) {
            uint64_t val;
            if (hwrng_rdrand_read(&val) == 0) {
                size_t chunk = MIN(len - filled, 8);
                memcpy(out + filled, &val, chunk);
                filled += chunk;
            } else {
                break;
            }
        }
        hwrng_dev.rdrand_count += filled;
        if (filled == len) return (int)len;
    }

    /* 尝试 Virtio-RNG */
    if (hwrng_dev.has_virtio) {
        int ret = hwrng_virtio_read(out, len);
        if (ret > 0) {
            hwrng_dev.virtio_count += ret;
            return ret;
        }
    }

    /* 软件回退 */
    return hwrng_timer_jitter(buf, len);
}

int hwrng_read(void *buf, size_t len) {
    if (buf == NULL) return Z_ERR_NULL;
    if (len == 0) return 0;

    unsigned long flags = spin_lock_irqsave(&hwrng_dev.read_lock);

    /* 从输入熵池提取 (如果熵足够) */
    struct entropy_pool *pool = &hwrng_dev.input_pool;
    if (pool->entropy_count >= HWRNG_MIN_ENTROPY_BITS) {
        /* 使用熵池数据作为种子，SHA-256 生成输出 */
        uint32_t state[8];
        memcpy(state, pool->digest, sizeof(state));

        uint8_t *out = (uint8_t *)buf;
        size_t remaining = len;
        size_t offset = 0;

        while (remaining > 0) {
            /* 混合计数器到状态 */
            state[0] ^= (uint32_t)offset;
            state[1] ^= (uint32_t)(offset >> 32);
            sha256_transform(state, (const uint8_t *)pool->pool);

            size_t chunk = MIN(remaining, 32);
            memcpy(out + offset, state, chunk);
            offset += chunk;
            remaining -= chunk;
        }

        /* 减少熵估计 */
        pool->entropy_count = MAX(0, pool->entropy_count - (int)(len * 8));
        pool->bytes_extracted += len;

        /* 重新混合熵池 */
        hwrng_pool_mix(pool);

        spin_unlock_irqrestore(&hwrng_dev.read_lock, flags);
        return (int)len;
    }

    spin_unlock_irqrestore(&hwrng_dev.read_lock, flags);

    /* 熵不足，直接从硬件读取 */
    return hwrng_fill_from_hw(buf, len);
}

int hwrng_read_nonblock(void *buf, size_t len) {
    return hwrng_read(buf, len);
}

/* ========== 初始化 ========== */

int hwrng_init(void) {
    printk("hwrng: 初始化硬件随机数生成器...");

    memset(&hwrng_dev, 0, sizeof(hwrng_dev));
    spinlock_init(&hwrng_dev.input_pool.lock);
    spinlock_init(&hwrng_dev.output_pool.lock);
    spinlock_init(&hwrng_dev.read_lock);

    /* 检测 RDRAND */
    hwrng_dev.has_rdrand = hwrng_rdrand_available();
    if (hwrng_dev.has_rdrand) {
        printk("hwrng: Intel RDRAND 可用");
        /* 测试 RDRAND */
        uint64_t val;
        if (hwrng_rdrand_read(&val) == 0) {
            printk("hwrng: RDRAND 测试通过 (样本=0x%016llX)", val);
        }
    }

    /* 检测 RDSEED */
    {
        uint32_t eax, ebx, ecx, edx;
        eax = 7; ecx = 0;
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax), "c"(ecx));
        hwrng_dev.has_rdseed = (ebx & (1 << 18)) != 0;
        if (hwrng_dev.has_rdseed) {
            printk("hwrng: Intel RDSEED 可用");
        }
    }

    /* 检测 Virtio-RNG */
    hwrng_virtio_init();

    /* 确定活跃熵源 */
    if (hwrng_dev.has_rdrand) {
        hwrng_dev.active_source = HWRNG_SRC_RDRAND;
    } else if (hwrng_dev.has_virtio) {
        hwrng_dev.active_source = HWRNG_SRC_VIRTIO;
    } else {
        hwrng_dev.active_source = HWRNG_SRC_TIMER;
    }

    /* 初始化熵池 (使用硬件随机数种子) */
    uint64_t seed;
    for (int i = 0; i < HWRNG_POOL_WORDS; i += 2) {
        if (hwrng_rdrand_read(&seed) == 0) {
            hwrng_dev.input_pool.pool[i] = (uint32_t)seed;
            hwrng_dev.input_pool.pool[i + 1] = (uint32_t)(seed >> 32);
        } else {
            /* 使用 TSC 作为回退种子 */
            uint64_t tsc;
            asm volatile("rdtsc" : "=A"(tsc));
            hwrng_dev.input_pool.pool[i] = (uint32_t)(tsc ^ (tsc >> 32) ^ i);
            hwrng_dev.input_pool.pool[i + 1] = (uint32_t)(tsc * 2654435761ULL + i);
        }
    }

    /* 初始混合 */
    hwrng_pool_mix(&hwrng_dev.input_pool);
    hwrng_dev.input_pool.entropy_count = 256;  /* 初始估计 256 位 */

    hwrng_dev.initialized = 1;
    printk("hwrng: 硬件随机数生成器初始化完成 (熵源=%s)",
           hwrng_dev.has_rdrand ? "RDRAND" :
           hwrng_dev.has_virtio ? "Virtio-RNG" : "定时器抖动");
    return 0;
}

struct hwrng_device *hwrng_get_device(void) {
    return &hwrng_dev;
}
