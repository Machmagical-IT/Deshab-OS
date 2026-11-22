/*
 * Zratro OS - Intel AC97 音频控制器驱动
 *
 * 支持: Intel AC97 (PCI 8086:2415)
 * 功能: PCM 回放 (48kHz, 16-bit 立体声), 混音器, BDL 缓冲区管理
 */

#include "ac97.h"
#include "console.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "idt.h"
#include "apic.h"
#include "string.h"

/* ========== I/O 端口访问辅助 ========== */

static inline void ac97_nam_write(struct ac97_device *dev, uint8_t reg, uint16_t val) {
    uint16_t port = dev->nam_base + reg;
    asm volatile("outw %w0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint16_t ac97_nam_read(struct ac97_device *dev, uint8_t reg) {
    uint16_t port = dev->nam_base + reg;
    uint16_t val;
    asm volatile("inw %w1, %w0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void ac97_nabm_write8(struct ac97_device *dev, uint8_t reg, uint8_t val) {
    uint16_t port = dev->nabm_base + reg;
    asm volatile("outb %b0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint8_t ac97_nabm_read8(struct ac97_device *dev, uint8_t reg) {
    uint16_t port = dev->nabm_base + reg;
    uint8_t val;
    asm volatile("inb %w1, %b0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void ac97_nabm_write16(struct ac97_device *dev, uint8_t reg, uint16_t val) {
    uint16_t port = dev->nabm_base + reg;
    asm volatile("outw %w0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint16_t ac97_nabm_read16(struct ac97_device *dev, uint8_t reg) {
    uint16_t port = dev->nabm_base + reg;
    uint16_t val;
    asm volatile("inw %w1, %w0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void ac97_nabm_write32(struct ac97_device *dev, uint8_t reg, uint32_t val) {
    uint16_t port = dev->nabm_base + reg;
    asm volatile("outl %0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint32_t ac97_nabm_read32(struct ac97_device *dev, uint8_t reg) {
    uint16_t port = dev->nabm_base + reg;
    uint32_t val;
    asm volatile("inl %w1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ========== 全局设备实例 ========== */

static struct ac97_device ac97_dev;

/* ========== 编解码器访问 ========== */

/* 等待编解码器就绪 (CAS 位清零) */
static int ac97_wait_codec_ready(struct ac97_device *dev) {
    /* 等待 CAS 位清零，表示编解码器可访问 */
    for (int i = 0; i < 1000; i++) {
        uint8_t cas = ac97_nabm_read8(dev, AC97_CAS);
        if (!(cas & 0x01)) {
            return 0;
        }
        /* 短暂延迟 */
        for (volatile int j = 0; j < 100; j++);
    }
    printk("ac97: 编解码器访问超时");
    return -1;
}

/* 读取编解码器寄存器 */
static uint16_t ac97_codec_read(struct ac97_device *dev, uint8_t reg) {
    if (ac97_wait_codec_ready(dev) != 0) {
        return 0xFFFF;
    }
    return ac97_nam_read(dev, reg);
}

/* 写入编解码器寄存器 */
static void ac97_codec_write(struct ac97_device *dev, uint8_t reg, uint16_t val) {
    if (ac97_wait_codec_ready(dev) != 0) {
        return;
    }
    ac97_nam_write(dev, reg, val);
}

/* ========== 混音器初始化 ========== */

static void ac97_mixer_init(struct ac97_device *dev) {
    /* 冷复位编解码器 */
    ac97_nam_write(dev, AC97_NAM_RESET, 0x0000);

    /* 等待编解码器就绪 */
    for (int i = 0; i < 500; i++) {
        uint16_t reset_val = ac97_nam_read(dev, AC97_NAM_RESET);
        if (reset_val != 0xFFFF) {
            dev->codec_ready = 1;
            break;
        }
        for (volatile int j = 0; j < 1000; j++);
    }

    if (!dev->codec_ready) {
        printk("ac97: 编解码器未就绪");
        return;
    }

    /* 读取编解码器厂商 ID */
    uint16_t vendor_id = ac97_codec_read(dev, 0x7C);
    uint16_t revision = ac97_codec_read(dev, 0x7E);
    dev->codec_vendor_id = vendor_id;
    dev->codec_revision = revision;
    printk("ac97: 编解码器厂商ID=0x%04X 修订=0x%04X", vendor_id, revision);

    /* 设置默认音量: 主音量 75%, PCM 音量 75% */
    ac97_set_master_volume(47, 47);
    ac97_set_pcm_volume(23, 23);

    /* 设置默认采样率 48kHz */
    ac97_codec_write(dev, AC97_NAM_PCM_FRONT_DAC_RATE, AC97_SAMPLE_RATE);

    /* 启用扩展音频功能 (变量采样率) */
    uint16_t ext_id = ac97_codec_read(dev, AC97_NAM_EXT_AUDIO_ID);
    if (ext_id & 0x0001) {
        /* 支持 DAC 变速率 */
        ac97_codec_write(dev, AC97_NAM_EXT_AUDIO_CTRL, ext_id & 0x0001);
    }

    /* 取消静音 */
    ac97_mute(0);
}

/* ========== BDL 缓冲区管理 ========== */

static int ac97_alloc_buffers(struct ac97_device *dev) {
    /* 分配 BDL (缓冲区描述符列表) - 必须 32 字节对齐 */
    /* 每个 BDL 条目 8 字节, 32 个条目 = 256 字节 */
    uint64_t bdl_phys = pmm_alloc_zero_pages(1);
    if (bdl_phys == 0) {
        printk("ac97: BDL 内存分配失败");
        return -1;
    }
    dev->bdl_phys = bdl_phys;
    dev->bdl = (struct ac97_bdl_entry *)phys_to_virt(bdl_phys);

    /* 分配音频缓冲区 */
    for (int i = 0; i < AC97_NUM_BUFFERS; i++) {
        uint64_t buf_phys = pmm_alloc_zero_pages(1);
        if (buf_phys == 0) {
            printk("ac97: 音频缓冲区 %d 分配失败", i);
            /* 释放已分配的缓冲区 */
            for (int j = 0; j < i; j++) {
                pmm_free_page(dev->buffer_phys[j]);
            }
            pmm_free_page(dev->bdl_phys);
            return -1;
        }
        dev->buffer_phys[i] = buf_phys;
        dev->buffers[i] = (uint8_t *)phys_to_virt(buf_phys);

        /* 填充 BDL 条目 */
        dev->bdl[i].buffer_addr = (uint32_t)buf_phys;
        dev->bdl[i].length = AC97_BUFFER_SIZE / (AC97_SAMPLE_BITS / 8 * AC97_CHANNELS);
        dev->bdl[i].ctrl = AC97_BDL_IOC;  /* 每个缓冲区完成时产生中断 */
    }

    return 0;
}

static void ac97_free_buffers(struct ac97_device *dev) {
    for (int i = 0; i < AC97_NUM_BUFFERS; i++) {
        if (dev->buffer_phys[i] != 0) {
            pmm_free_page(dev->buffer_phys[i]);
            dev->buffer_phys[i] = 0;
            dev->buffers[i] = NULL;
        }
    }
    if (dev->bdl_phys != 0) {
        pmm_free_page(dev->bdl_phys);
        dev->bdl_phys = 0;
        dev->bdl = NULL;
    }
}

/* ========== PCM 输出通道控制 ========== */

static void ac97_reset_pcm_out(struct ac97_device *dev) {
    /* 重置 PCM 输出通道 */
    ac97_nabm_write8(dev, AC97_NABM_PCM_OUT + AC97_CR, AC97_CR_RR);
    /* 等待重置完成 */
    for (int i = 0; i < 100; i++) {
        if (!(ac97_nabm_read8(dev, AC97_NABM_PCM_OUT + AC97_CR) & AC97_CR_RR)) {
            break;
        }
    }
    /* 清除状态位 */
    ac97_nabm_write16(dev, AC97_NABM_PCM_OUT + AC97_SR,
                       AC97_SR_LVBCI | AC97_SR_BCIS | AC97_SR_FIFOE);
}

static void ac97_setup_pcm_out(struct ac97_device *dev) {
    /* 设置 BDL 基址 */
    ac97_nabm_write32(dev, AC97_NABM_PCM_OUT + AC97_BDBAR, (uint32_t)dev->bdl_phys);

    /* 设置最后有效索引 */
    ac97_nabm_write8(dev, AC97_NABM_PCM_OUT + AC97_LVI, AC97_BDL_ENTRIES - 1);

    /* 使能中断: 缓冲区完成 + FIFO 错误 */
    uint8_t cr = AC97_CR_LVBIE | AC97_CR_FEIE | AC97_CR_IOCE;
    ac97_nabm_write8(dev, AC97_NABM_PCM_OUT + AC97_CR, cr);
}

/* ========== 中断处理 ========== */

void ac97_interrupt_handler(struct interrupt_frame *frame) {
    (void)frame;
    struct ac97_device *dev = &ac97_dev;

    /* 读取全局状态 */
    uint32_t glob_sta = ac97_nabm_read32(dev, AC97_GLOB_STA);

    /* 检查 PCM 输出中断 */
    if (glob_sta & AC97_GLOB_STA_POINT) {
        /* 读取 PCM 输出状态 */
        uint16_t sr = ac97_nabm_read16(dev, AC97_NABM_PCM_OUT + AC97_SR);

        if (sr & AC97_SR_BCIS) {
            /* 缓冲区完成中断 - 正常播放完成 */
            dev->play_civ = ac97_nabm_read8(dev, AC97_NABM_PCM_OUT + AC97_CIV);
            dev->bytes_played += AC97_BUFFER_SIZE;
        }

        if (sr & AC97_SR_FIFOE) {
            /* FIFO 欠载 - 增加欠载计数 */
            dev->underruns++;
        }

        /* 清除状态位 */
        ac97_nabm_write16(dev, AC97_NABM_PCM_OUT + AC97_SR, sr);
    }

    /* 发送 EOI */
    apic_eoi();
}

/* ========== 公共 API 实现 ========== */

int ac97_set_master_volume(uint8_t left, uint8_t right) {
    struct ac97_device *dev = &ac97_dev;
    if (!dev->codec_ready) return -1;

    /* AC97 主音量: 6位, 0=最大, 63=最小, bit15=静音 */
    left = MIN(left, 63);
    right = MIN(right, 63);
    uint16_t val = ((uint16_t)right << 8) | (uint16_t)left;
    if (dev->mixer.muted) val |= 0x8000;
    ac97_codec_write(dev, AC97_NAM_MASTER_VOL, val);
    dev->mixer.master_vol = val & 0x7F7F;
    return 0;
}

int ac97_get_master_volume(uint8_t *left, uint8_t *right) {
    struct ac97_device *dev = &ac97_dev;
    if (!dev->codec_ready) return -1;
    uint16_t val = ac97_codec_read(dev, AC97_NAM_MASTER_VOL);
    *left = val & 0x3F;
    *right = (val >> 8) & 0x3F;
    return 0;
}

int ac97_set_pcm_volume(uint8_t left, uint8_t right) {
    struct ac97_device *dev = &ac97_dev;
    if (!dev->codec_ready) return -1;

    /* PCM 音量: 5位, 0=最大, 31=最小 */
    left = MIN(left, 31);
    right = MIN(right, 31);
    uint16_t val = ((uint16_t)right << 8) | (uint16_t)left;
    ac97_codec_write(dev, AC97_NAM_PCM_OUT_VOL, val);
    dev->mixer.pcm_vol = val;
    return 0;
}

int ac97_get_pcm_volume(uint8_t *left, uint8_t *right) {
    struct ac97_device *dev = &ac97_dev;
    if (!dev->codec_ready) return -1;
    uint16_t val = ac97_codec_read(dev, AC97_NAM_PCM_OUT_VOL);
    *left = val & 0x1F;
    *right = (val >> 8) & 0x1F;
    return 0;
}

int ac97_mute(int mute) {
    struct ac97_device *dev = &ac97_dev;
    if (!dev->codec_ready) return -1;

    dev->mixer.muted = mute;
    uint16_t val = dev->mixer.master_vol;
    if (mute) val |= 0x8000;
    ac97_codec_write(dev, AC97_NAM_MASTER_VOL, val);
    return 0;
}

int ac97_is_muted(void) {
    return ac97_dev.mixer.muted;
}

int ac97_play(const void *data, size_t len) {
    struct ac97_device *dev = &ac97_dev;
    if (!dev->codec_ready || dev->bdl == NULL) return -1;

    unsigned long flags = spin_lock_irqsave(&dev->lock);

    /* 将音频数据复制到环形缓冲区 */
    const uint8_t *src = (const uint8_t *)data;
    size_t remaining = len;
    int buf_idx = dev->play_lvi % AC97_NUM_BUFFERS;

    while (remaining > 0) {
        size_t chunk = MIN(remaining, AC97_BUFFER_SIZE);
        if (dev->buffers[buf_idx] == NULL) break;

        memcpy(dev->buffers[buf_idx], src, chunk);
        /* 如果数据不足一个缓冲区，剩余部分清零 */
        if (chunk < AC97_BUFFER_SIZE) {
            memset(dev->buffers[buf_idx] + chunk, 0, AC97_BUFFER_SIZE - chunk);
        }

        /* 更新 BDL 条目长度 */
        dev->bdl[buf_idx].length = chunk / (AC97_SAMPLE_BITS / 8 * AC97_CHANNELS);
        dev->bdl[buf_idx].ctrl = AC97_BDL_IOC;

        src += chunk;
        remaining -= chunk;
        buf_idx = (buf_idx + 1) % AC97_NUM_BUFFERS;
    }

    /* 设置最后有效索引 */
    dev->play_lvi = (buf_idx == 0) ? AC97_BDL_ENTRIES - 1 : buf_idx - 1;
    ac97_nabm_write8(dev, AC97_NABM_PCM_OUT + AC97_LVI, dev->play_lvi);

    /* 启动播放 */
    if (!dev->playing) {
        ac97_reset_pcm_out(dev);
        ac97_setup_pcm_out(dev);

        /* 设置 BDL 基址并启动 */
        ac97_nabm_write32(dev, AC97_NABM_PCM_OUT + AC97_BDBAR, (uint32_t)dev->bdl_phys);
        ac97_nabm_write8(dev, AC97_NABM_PCM_OUT + AC97_CR,
                         AC97_CR_RPBM | AC97_CR_LVBIE | AC97_CR_FEIE | AC97_CR_IOCE);
        dev->playing = 1;
    }

    spin_unlock_irqrestore(&dev->lock, flags);
    return (int)(len - remaining);
}

int ac97_stop(void) {
    struct ac97_device *dev = &ac97_dev;
    if (!dev->codec_ready) return -1;

    unsigned long flags = spin_lock_irqsave(&dev->lock);

    /* 停止 PCM 输出 */
    ac97_nabm_write8(dev, AC97_NABM_PCM_OUT + AC97_CR, 0);
    ac97_reset_pcm_out(dev);
    dev->playing = 0;
    dev->play_civ = 0;
    dev->play_lvi = 0;

    spin_unlock_irqrestore(&dev->lock, flags);
    return 0;
}

int ac97_pause(void) {
    struct ac97_device *dev = &ac97_dev;
    if (!dev->playing) return -1;

    /* 暂停: 清除 RPBM 位 */
    uint8_t cr = ac97_nabm_read8(dev, AC97_NABM_PCM_OUT + AC97_CR);
    cr &= ~AC97_CR_RPBM;
    ac97_nabm_write8(dev, AC97_NABM_PCM_OUT + AC97_CR, cr);
    return 0;
}

int ac97_resume(void) {
    struct ac97_device *dev = &ac97_dev;
    if (!dev->codec_ready) return -1;

    /* 恢复: 设置 RPBM 位 */
    uint8_t cr = ac97_nabm_read8(dev, AC97_NABM_PCM_OUT + AC97_CR);
    cr |= AC97_CR_RPBM;
    ac97_nabm_write8(dev, AC97_NABM_PCM_OUT + AC97_CR, cr);
    dev->playing = 1;
    return 0;
}

int ac97_is_playing(void) {
    return ac97_dev.playing;
}

int ac97_set_sample_rate(uint32_t rate) {
    struct ac97_device *dev = &ac97_dev;
    if (!dev->codec_ready) return -1;

    /* 检查扩展音频是否支持变速率 */
    uint16_t ext_id = ac97_codec_read(dev, AC97_NAM_EXT_AUDIO_ID);
    if (!(ext_id & 0x0001)) {
        /* 不支持变速率，只能 48kHz */
        if (rate != 48000) return -1;
        return 0;
    }

    /* 启用变速率 */
    ac97_codec_write(dev, AC97_NAM_EXT_AUDIO_CTRL, 0x0001);

    /* 设置 DAC 采样率 */
    ac97_codec_write(dev, AC97_NAM_PCM_FRONT_DAC_RATE, (uint16_t)rate);

    /* 验证设置 */
    uint16_t actual = ac97_codec_read(dev, AC97_NAM_PCM_FRONT_DAC_RATE);
    if (actual != (uint16_t)rate) {
        printk("ac97: 采样率设置失败 请求=%u 实际=%u", rate, actual);
        return -1;
    }

    return 0;
}

uint32_t ac97_get_sample_rate(void) {
    struct ac97_device *dev = &ac97_dev;
    if (!dev->codec_ready) return 0;
    return ac97_codec_read(dev, AC97_NAM_PCM_FRONT_DAC_RATE);
}

struct ac97_device *ac97_get_device(void) {
    return &ac97_dev;
}

/* ========== PCI 探测与初始化 ========== */

int ac97_probe(void) {
    printk("ac97: 扫描 PCI 总线...");

    for (uint8_t bus = 0; bus < 255; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor = pci_read_config32(bus, dev, func, 0x00);
                if (vendor == 0xFFFFFFFF) continue;

                uint16_t vid = vendor & 0xFFFF;
                uint16_t did = (vendor >> 16) & 0xFFFF;

                if (vid == AC97_VENDOR_ID && did == AC97_DEVICE_ID) {
                    printk("ac97: 发现设备 bus=%u dev=%u func=%u", bus, dev, func);
                    ac97_dev.pci_bus = bus;
                    ac97_dev.pci_dev = dev;
                    ac97_dev.pci_func = func;
                    return 0;
                }
            }
        }
    }

    printk("ac97: 未找到 AC97 设备");
    return -1;
}

int ac97_init(void) {
    printk("ac97: 初始化 Intel AC97 音频驱动...");

    memset(&ac97_dev, 0, sizeof(ac97_dev));
    spinlock_init(&ac97_dev.lock);

    /* 探测 PCI 设备 */
    if (ac97_probe() != 0) {
        return -1;
    }

    /* 获取 I/O 基址 */
    uint32_t bar0 = pci_get_bar(ac97_dev.pci_bus, ac97_dev.pci_dev, ac97_dev.pci_func, 0);
    uint32_t bar1 = pci_get_bar(ac97_dev.pci_bus, ac97_dev.pci_dev, ac97_dev.pci_func, 1);

    /* BAR0: Native Audio Mixer (NAM) - I/O 空间 */
    if (bar0 & 0x01) {
        ac97_dev.nam_base = (uint16_t)(bar0 & ~0x03);
    } else {
        printk("ac97: BAR0 不是 I/O 空间");
        return -1;
    }

    /* BAR1: Native Audio Bus Master (NABM) - I/O 空间 */
    if (bar1 & 0x01) {
        ac97_dev.nabm_base = (uint16_t)(bar1 & ~0x03);
    } else {
        printk("ac97: BAR1 不是 I/O 空间");
        return -1;
    }

    printk("ac97: NAM 基址=0x%04X NABM 基址=0x%04X", ac97_dev.nam_base, ac97_dev.nabm_base);

    /* 启用 PCI 总线主控和 I/O 空间 */
    pci_enable_bus_master(ac97_dev.pci_bus, ac97_dev.pci_dev, ac97_dev.pci_func);
    uint32_t cmd = pci_read_config32(ac97_dev.pci_bus, ac97_dev.pci_dev, ac97_dev.pci_func, 0x04);
    cmd |= 0x05;  /* I/O Space + Bus Master */
    pci_write_config32(ac97_dev.pci_bus, ac97_dev.pci_dev, ac97_dev.pci_func, 0x04, cmd);

    /* 获取 IRQ */
    ac97_dev.irq_line = pci_get_irq_line(ac97_dev.pci_bus, ac97_dev.pci_dev, ac97_dev.pci_func);
    printk("ac97: IRQ=%u", ac97_dev.irq_line);

    /* 冷复位控制器 */
    ac97_nabm_write32(&ac97_dev, AC97_GLOB_CNT, AC97_GLOB_CNT_COLD);
    /* 等待复位完成 */
    for (volatile int i = 0; i < 500000; i++);
    ac97_nabm_write32(&ac97_dev, AC97_GLOB_CNT, 0);

    /* 等待编解码器就绪 */
    for (int i = 0; i < 1000; i++) {
        uint32_t gs = ac97_nabm_read32(&ac97_dev, AC97_GLOB_STA);
        if (gs & (AC97_GLOB_STA_PCR | AC97_GLOB_STA_SCR)) {
            printk("ac97: 编解码器就绪 (状态=0x%08X)", gs);
            break;
        }
    }

    /* 初始化混音器 */
    ac97_mixer_init(&ac97_dev);
    if (!ac97_dev.codec_ready) {
        printk("ac97: 编解码器初始化失败");
        return -1;
    }

    /* 分配音频缓冲区 */
    if (ac97_alloc_buffers(&ac97_dev) != 0) {
        printk("ac97: 缓冲区分配失败");
        return -1;
    }

    /* 启用全局中断 */
    ac97_nabm_write32(&ac97_dev, AC97_GLOB_CNT, AC97_GLOB_CNT_GIE);

    /* 注册中断处理 */
    if (ac97_dev.irq_line != 0xFF && ac97_dev.irq_line != 0) {
        uint8_t vector = IRQ_BASE + ac97_dev.irq_line;
        isr_register_handler(vector, ac97_interrupt_handler);
        printk("ac97: 中断已注册 IRQ=%u 向量=%u", ac97_dev.irq_line, vector);
    }

    printk("ac97: Intel AC97 音频驱动初始化完成");
    return 0;
}
