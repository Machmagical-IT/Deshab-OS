/*
 * Zratro OS - Intel HD Audio 驱动
 *
 * 支持: Intel HDA 控制器 (PCI 8086:2668 等)
 * 功能: CORB/RIRB 命令环缓冲区, Widget 树解析, 流管理, PCM 回放
 */

#include "hda.h"
#include "console.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "idt.h"
#include "apic.h"
#include "string.h"

/* ========== MMIO 寄存器访问 ========== */

static inline uint16_t hda_reg_read16(struct hda_device *dev, uint32_t reg) {
    return *(volatile uint16_t *)(dev->mmio_virt + reg);
}

static inline uint32_t hda_reg_read32(struct hda_device *dev, uint32_t reg) {
    return *(volatile uint32_t *)(dev->mmio_virt + reg);
}

static inline void hda_reg_write16(struct hda_device *dev, uint32_t reg, uint16_t val) {
    *(volatile uint16_t *)(dev->mmio_virt + reg) = val;
}

static inline void hda_reg_write32(struct hda_device *dev, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(dev->mmio_virt + reg) = val;
}

/* ========== 全局设备实例 ========== */

static struct hda_device hda_dev;

/* ========== MMIO 8-bit 寄存器访问 ========== */

static inline void hda_reg_write8_at_offset(struct hda_device *dev, uint32_t reg, uint8_t val) {
    *(volatile uint8_t *)(dev->mmio_virt + reg) = val;
}

static inline uint8_t hda_reg_read8_at_offset(struct hda_device *dev, uint32_t reg) {
    return *(volatile uint8_t *)(dev->mmio_virt + reg);
}

/* ========== CORB/RIRB 操作 ========== */

/* 初始化 CORB (命令输出环缓冲区) */
static int hda_corb_init(struct hda_device *dev) {
    /* 分配 CORB 缓冲区 (256 条目 * 4 字节 = 1KB) */
    uint64_t corb_phys = pmm_alloc_zero_pages(1);
    if (corb_phys == 0) {
        printk("hda: CORB 内存分配失败");
        return -1;
    }
    dev->corb_phys = corb_phys;
    dev->corb = (uint32_t *)phys_to_virt(corb_phys);

    /* 设置 CORB 基地址 */
    hda_reg_write32(dev, HDA_CORBLBASE, (uint32_t)corb_phys);
    hda_reg_write32(dev, HDA_CORBUBASE, (uint32_t)(corb_phys >> 32));

    /* 设置 CORB 大小 (256 条目) */
    uint8_t corbsize = hda_reg_read16(dev, HDA_CORBSIZE) & 0x03;
    if (corbsize & 0x02) {
        hda_reg_write16(dev, HDA_CORBSIZE, 0x02);  /* 256 条目 */
    } else if (corbsize & 0x01) {
        hda_reg_write16(dev, HDA_CORBSIZE, 0x01);  /* 16 条目 */
    } else {
        hda_reg_write16(dev, HDA_CORBSIZE, 0x00);  /* 2 条目 */
    }

    /* 重置 CORB 指针 */
    hda_reg_write16(dev, HDA_CORBWP, 0);
    hda_reg_write16(dev, HDA_CORBRP, 0x8000);  /* 重置读指针 */
    for (int i = 0; i < 100; i++) {
        if (!(hda_reg_read16(dev, HDA_CORBRP) & 0x8000)) break;
    }
    hda_reg_write16(dev, HDA_CORBRP, 0);

    dev->corb_wp = 0;

    /* 启动 CORB DMA */
    hda_reg_write8_at_offset(dev, HDA_CORBCTL, HDA_CORBCTL_CORBRUN | HDA_CORBCTL_CMEIE);

    return 0;
}

/* 初始化 RIRB (响应输入环缓冲区) */
static int hda_rirb_init(struct hda_device *dev) {
    /* 分配 RIRB 缓冲区 (256 条目 * 8 字节 = 2KB) */
    uint64_t rirb_phys = pmm_alloc_zero_pages(1);
    if (rirb_phys == 0) {
        printk("hda: RIRB 内存分配失败");
        return -1;
    }
    dev->rirb_phys = rirb_phys;
    dev->rirb = (struct hda_rirb_entry *)phys_to_virt(rirb_phys);

    /* 设置 RIRB 基地址 */
    hda_reg_write32(dev, HDA_RIRBLBASE, (uint32_t)rirb_phys);
    hda_reg_write32(dev, HDA_RIRBUBASE, (uint32_t)(rirb_phys >> 32));

    /* 设置 RIRB 大小 */
    uint8_t rirbsize = hda_reg_read16(dev, HDA_RIRBSIZE) & 0x03;
    if (rirbsize & 0x02) {
        hda_reg_write16(dev, HDA_RIRBSIZE, 0x02);
    } else if (rirbsize & 0x01) {
        hda_reg_write16(dev, HDA_RIRBSIZE, 0x01);
    }

    /* 重置 RIRB 写指针 */
    hda_reg_write16(dev, HDA_RIRBWP, 0x8000);  /* 重置 */
    for (int i = 0; i < 100; i++) {
        if (!(hda_reg_read16(dev, HDA_RIRBWP) & 0x8000)) break;
    }

    dev->rirb_rp = 0;

    /* 设置响应中断计数 (每次响应都产生中断) */
    hda_reg_write16(dev, HDA_RINTCNT, 1);

    /* 启动 RIRB DMA */
    hda_reg_write8_at_offset(dev, HDA_RIRBCTL,
                              HDA_RIRBCTL_RIRBDMAEN | HDA_RIRBCTL_RINTCTL | HDA_RIRBCTL_RIRBOIC);

    return 0;
}

/* 发送编解码器命令并等待响应 */
uint32_t hda_codec_command(uint8_t codec_addr, uint32_t verb) {
    struct hda_device *dev = &hda_dev;

    /* 构造 CORB 条目: [31:28]=编解码器地址, [27:20]=节点ID, [19:0]=动词 */
    uint32_t cmd = ((uint32_t)codec_addr << 28) | verb;

    /* 等待 CORB 有空间 */
    uint16_t rp = hda_reg_read16(dev, HDA_CORBRP);
    uint16_t wp = dev->corb_wp;
    uint16_t next_wp = (wp + 1) % HDA_CORB_MAX_ENTRIES;

    if (next_wp == rp) {
        printk("hda: CORB 满出");
        return 0xFFFFFFFF;
    }

    /* 写入 CORB 条目 */
    dev->corb[wp] = cmd;
    dev->corb_wp = next_wp;

    /* 更新写指针 */
    hda_reg_write16(dev, HDA_CORBWP, next_wp);

    /* 等待 RIRB 响应 */
    for (int timeout = 0; timeout < 5000; timeout++) {
        uint16_t rirb_wp = hda_reg_read16(dev, HDA_RIRBWP) & 0xFF;
        if (rirb_wp != dev->rirb_rp) {
            /* 读取响应 */
            struct hda_rirb_entry *entry = &dev->rirb[dev->rirb_rp];
            uint32_t response = entry->response;
            dev->rirb_rp = (dev->rirb_rp + 1) % HDA_RIRB_MAX_ENTRIES;

            /* 清除 RIRB 中断状态 */
            hda_reg_write8_at_offset(dev, HDA_RIRBSTS, HDA_RIRBSTS_RINTFL);

            return response;
        }
        for (volatile int i = 0; i < 100; i++);
    }

    printk("hda: 编解码器命令超时 (addr=%u verb=0x%06X)", codec_addr, verb & 0xFFFFF);
    return 0xFFFFFFFF;
}

/* 获取编解码器参数 */
uint32_t hda_codec_get_param(uint8_t codec_addr, uint8_t nid, uint32_t param) {
    /* 参数获取动词: [19:8]=参数ID, [7:0]=0xF00 */
    uint32_t verb = ((uint32_t)nid << 20) | (param << 8) | 0xF00;
    return hda_codec_command(codec_addr, verb);
}

/* 设置放大器增益 */
uint32_t hda_codec_set_amp(uint8_t codec_addr, uint8_t nid, uint32_t payload) {
    uint32_t verb = ((uint32_t)nid << 20) | (0x3000) | (payload & 0xFFF);
    return hda_codec_command(codec_addr, verb);
}

/* ========== Widget 树解析 ========== */

/* 获取 Widget 类型 */
static uint8_t hda_get_widget_type(uint8_t codec_addr, uint8_t nid) {
    uint32_t amp_caps = hda_codec_get_param(codec_addr, nid, 0x09);
    return (uint8_t)((amp_caps >> 20) & 0xF);
}

/* 解析编解码器 Widget 树 */
int hda_parse_widgets(struct hda_codec *codec) {
    printk("hda: 解析编解码器 %u 的 Widget 树...", codec->addr);

    /* 获取功能组类型 */
    uint32_t fgroup_type = hda_codec_get_param(codec->addr, codec->afg_nid, 0x05);
    if ((fgroup_type & 0xFF) != 0x01) {
        printk("hda: 非音频功能组 (类型=0x%02X)", fgroup_type & 0xFF);
        return -1;
    }

    /* 获取子节点数量和起始节点 */
    uint32_t node_info = hda_codec_get_param(codec->addr, codec->afg_nid, 0x04);
    uint8_t start_node = node_info & 0xFF;
    uint8_t num_nodes = (node_info >> 8) & 0xFF;

    printk("hda: 起始节点=%u 节点数=%u", start_node, num_nodes);

    codec->num_widgets = 0;
    codec->dac_nid = 0;
    codec->adc_nid = 0;
    codec->pin_nid = 0;

    for (uint8_t i = 0; i < num_nodes && codec->num_widgets < 64; i++) {
        uint8_t nid = start_node + i;
        struct hda_widget *w = &codec->widgets[codec->num_widgets];

        w->nid = nid;
        w->type = hda_get_widget_type(codec->addr, nid);

        /* 获取放大器能力 */
        w->amp_out_caps = hda_codec_get_param(codec->addr, nid, 0x0C);
        w->amp_in_caps = hda_codec_get_param(codec->addr, nid, 0x0D);

        /* 获取引脚能力 (仅引脚 Widget) */
        if (w->type == HDA_WIDGET_TYPE_PIN) {
            w->pin_caps = hda_codec_get_param(codec->addr, nid, 0x0C);

            /* 获取引脚默认配置 */
            uint32_t config = hda_codec_get_param(codec->addr, nid, 0x1C);
            w->config_default = config;

            uint8_t device = (config >> 20) & 0xF;
            uint8_t conn_type = (config >> 16) & 0xF;

            /* 寻找输出引脚 */
            if (device == HDA_PIN_DEV_LINE_OUT || device == HDA_PIN_DEV_SPEAKER ||
                device == HDA_PIN_DEV_HP_OUT) {
                if (codec->pin_nid == 0) {
                    codec->pin_nid = nid;
                    printk("hda: 发现输出引脚 NID=%u 设备=%u 连接=%u", nid, device, conn_type);
                }
            }
        }

        /* 获取连接列表 */
        if (w->type == HDA_WIDGET_TYPE_DAC && codec->dac_nid == 0) {
            codec->dac_nid = nid;
            printk("hda: 发现 DAC NID=%u", nid);
        }
        if (w->type == HDA_WIDGET_TYPE_ADC && codec->adc_nid == 0) {
            codec->adc_nid = nid;
            printk("hda: 发现 ADC NID=%u", nid);
        }

        codec->num_widgets++;
    }

    printk("hda: Widget 树解析完成: %d 个 Widget, DAC=%u, ADC=%u, PIN=%u",
           codec->num_widgets, codec->dac_nid, codec->adc_nid, codec->pin_nid);
    return 0;
}

/* ========== 流管理 ========== */

/* 分配流的 BDL 和缓冲区 */
static int hda_stream_alloc_buffers(struct hda_stream *stream) {
    /* 分配 BDL (32 条目 * 16 字节 = 512 字节) */
    uint64_t bdl_phys = pmm_alloc_zero_pages(1);
    if (bdl_phys == 0) return -1;
    stream->bdl_phys = bdl_phys;
    stream->bdl = (struct hda_bdl_entry *)phys_to_virt(bdl_phys);

    /* 分配音频缓冲区 */
    for (int i = 0; i < HDA_BDL_ENTRIES; i++) {
        uint64_t buf_phys = pmm_alloc_zero_pages(1);
        if (buf_phys == 0) {
            for (int j = 0; j < i; j++) pmm_free_page(stream->buffer_phys[j]);
            pmm_free_page(bdl_phys);
            return -1;
        }
        stream->buffer_phys[i] = buf_phys;
        stream->buffers[i] = (uint8_t *)phys_to_virt(buf_phys);

        /* 填充 BDL */
        stream->bdl[i].buffer_addr = buf_phys;
        stream->bdl[i].length = HDA_STREAM_BUF_SIZE;
        stream->bdl[i].ctrl = 0x01;  /* IOC 中断 */
    }

    return 0;
}

/* 获取流描述符寄存器基址 */
static uint32_t hda_sd_base(int stream_index, int direction) {
    /* 输出流从 SD_BASE 开始, 输入流在输出流之后 */
    return HDA_SD_BASE + (stream_index * HDA_SD_SIZE) +
           (direction ? (8 * HDA_SD_SIZE) : 0);
}

int hda_stream_start(int stream_index, int direction) {
    struct hda_device *dev = &hda_dev;
    struct hda_stream *stream = direction ?
        &dev->input_streams[stream_index] :
        &dev->output_streams[stream_index];

    uint32_t base = hda_sd_base(stream_index, direction);

    /* 重置流 */
    hda_reg_write32(dev, base + HDA_SD_CTL, HDA_SD_CTL_SRST);
    for (int i = 0; i < 100; i++) {
        if (hda_reg_read32(dev, base + HDA_SD_CTL) & HDA_SD_CTL_SRST) break;
    }
    hda_reg_write32(dev, base + HDA_SD_CTL, 0);
    for (int i = 0; i < 100; i++) {
        if (!(hda_reg_read32(dev, base + HDA_SD_CTL) & HDA_SD_CTL_SRST)) break;
    }

    /* 设置 BDL 地址 */
    hda_reg_write32(dev, base + HDA_SD_BDPL, (uint32_t)stream->bdl_phys);
    hda_reg_write32(dev, base + HDA_SD_BDPU, (uint32_t)(stream->bdl_phys >> 32));

    /* 设置循环缓冲区长度 */
    hda_reg_write32(dev, base + HDA_SD_CBL, HDA_STREAM_BUF_SIZE * HDA_BDL_ENTRIES);

    /* 设置最后有效索引 */
    hda_reg_write16(dev, base + HDA_SD_LVI, HDA_BDL_ENTRIES - 1);

    /* 设置流格式 (48kHz, 16-bit, 立体声) */
    hda_reg_write16(dev, base + HDA_SD_FORMAT, HDA_FORMAT_48KHZ_16BIT_STEREO);

    /* 清除状态位 */
    hda_reg_write8_at_offset(dev, base + HDA_SD_STS,
                              HDA_SD_STS_BCIS | HDA_SD_STS_DESE | HDA_SD_STS_FIFOE);

    /* 启动流 */
    uint32_t ctl = HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE | HDA_SD_CTL_DEIE | HDA_SD_CTL_FIFIE;
    hda_reg_write32(dev, base + HDA_SD_CTL, ctl);

    stream->running = 1;
    return 0;
}

int hda_stream_stop(int stream_index, int direction) {
    struct hda_device *dev = &hda_dev;
    uint32_t base = hda_sd_base(stream_index, direction);

    /* 停止流 */
    hda_reg_write32(dev, base + HDA_SD_CTL, 0);

    struct hda_stream *stream = direction ?
        &dev->input_streams[stream_index] :
        &dev->output_streams[stream_index];
    stream->running = 0;

    return 0;
}

/* ========== PCM 播放 ========== */

int hda_play(const void *data, size_t len) {
    struct hda_device *dev = &hda_dev;
    struct hda_stream *stream = &dev->output_streams[0];

    if (stream->bdl == NULL) return -1;

    unsigned long flags = spin_lock_irqsave(&dev->lock);

    /* 复制音频数据到缓冲区 */
    const uint8_t *src = (const uint8_t *)data;
    size_t remaining = len;
    int buf_idx = 0;

    while (remaining > 0 && buf_idx < HDA_BDL_ENTRIES) {
        size_t chunk = MIN(remaining, HDA_STREAM_BUF_SIZE);
        memcpy(stream->buffers[buf_idx], src, chunk);
        if (chunk < HDA_STREAM_BUF_SIZE) {
            memset(stream->buffers[buf_idx] + chunk, 0, HDA_STREAM_BUF_SIZE - chunk);
        }
        stream->bdl[buf_idx].length = chunk;
        stream->bdl[buf_idx].ctrl = 0x01;  /* IOC */

        src += chunk;
        remaining -= chunk;
        buf_idx++;
    }

    /* 更新 LVI */
    uint32_t base = hda_sd_base(0, 0);
    hda_reg_write16(dev, base + HDA_SD_LVI, buf_idx - 1);

    spin_unlock_irqrestore(&dev->lock, flags);
    return (int)(len - remaining);
}

int hda_stop(void) {
    return hda_stream_stop(0, 0);
}

/* ========== 音量控制 ========== */

int hda_set_volume(uint8_t nid, int left_db, int right_db) {
    struct hda_device *dev = &hda_dev;
    if (dev->num_codecs == 0) return -1;

    struct hda_codec *codec = &dev->codecs[0];
    /* 放大器设置: 输出放大器, 左右声道, 索引0 */
    uint32_t payload = 0xB000;  /* 输出, 左右声道, 索引0 */
    payload |= (uint32_t)(left_db & 0x7F);
    payload |= ((uint32_t)(right_db & 0x7F) << 8);

    hda_codec_set_amp(codec->addr, nid, payload);
    return 0;
}

int hda_set_mute(uint8_t nid, int mute) {
    struct hda_device *dev = &hda_dev;
    if (dev->num_codecs == 0) return -1;

    struct hda_codec *codec = &dev->codecs[0];
    uint32_t payload = 0xB000;  /* 输出, 左右声道 */
    if (mute) payload |= 0x8080;  /* 静音位 */

    hda_codec_set_amp(codec->addr, nid, payload);
    return 0;
}

/* ========== 中断处理 ========== */

static void hda_interrupt_handler(struct interrupt_frame *frame) {
    (void)frame;
    struct hda_device *dev = &hda_dev;

    /* 读取中断状态 */
    uint32_t intsts = hda_reg_read32(dev, HDA_INTSTS);
    if (intsts == 0) {
        apic_eoi();
        return;
    }

    /* 检查各流的中断状态 */
    for (int i = 0; i < dev->num_output_streams; i++) {
        uint32_t base = hda_sd_base(i, 0);
        uint8_t sts = hda_reg_read8_at_offset(dev, base + HDA_SD_STS);

        if (sts & HDA_SD_STS_BCIS) {
            /* 缓冲区完成 */
            dev->bytes_played += HDA_STREAM_BUF_SIZE;
            hda_reg_write8_at_offset(dev, base + HDA_SD_STS, HDA_SD_STS_BCIS);
        }
        if (sts & HDA_SD_STS_FIFOE) {
            dev->underruns++;
            hda_reg_write8_at_offset(dev, base + HDA_SD_STS, HDA_SD_STS_FIFOE);
        }
    }

    /* 清除 RIRB 中断 */
    uint8_t rirb_sts = hda_reg_read8_at_offset(dev, HDA_RIRBSTS);
    if (rirb_sts) {
        hda_reg_write8_at_offset(dev, HDA_RIRBSTS, rirb_sts);
    }

    apic_eoi();
}

/* ========== 编解码器枚举 ========== */

static int hda_enum_codecs(struct hda_device *dev) {
    /* 读取状态寄存器获取已连接的编解码器 */
    uint16_t statests = hda_reg_read16(dev, HDA_STATESTS);
    dev->num_codecs = 0;

    for (int i = 0; i < 16; i++) {
        if (statests & (1 << i)) {
            struct hda_codec *codec = &dev->codecs[dev->num_codecs];
            codec->addr = i;
            codec->ready = 1;

            /* 读取厂商 ID */
            uint32_t vendor_id = hda_codec_get_param(i, 0, 0x00);
            codec->vendor_id = vendor_id;

            /* 读取修订 ID */
            uint32_t rev_id = hda_codec_get_param(i, 0, 0x02);
            codec->revision_id = rev_id;

            /* 查找音频功能组 */
            uint32_t node_info = hda_codec_get_param(i, 0, 0x04);
            uint8_t start_node = node_info & 0xFF;
            uint8_t num_nodes = (node_info >> 8) & 0xFF;

            codec->afg_nid = 0;
            for (int n = 0; n < num_nodes; n++) {
                uint8_t nid = start_node + n;
                uint32_t fgroup = hda_codec_get_param(i, nid, 0x05);
                if ((fgroup & 0xFF) == 0x01) {
                    codec->afg_nid = nid;
                    break;
                }
            }

            if (codec->afg_nid == 0) {
                codec->afg_nid = start_node;  /* 使用第一个节点 */
            }

            printk("hda: 编解码器 %u: 厂商=0x%08X 修订=0x%08X AFG=%u",
                   i, vendor_id, rev_id, codec->afg_nid);

            /* 解析 Widget 树 */
            hda_parse_widgets(codec);

            dev->num_codecs++;
            if (dev->num_codecs >= 4) break;
        }
    }

    return dev->num_codecs > 0 ? 0 : -1;
}

/* ========== PCI 探测与初始化 ========== */

int hda_probe(void) {
    printk("hda: 扫描 PCI 总线...");

    static const uint16_t supported_devices[] = {
        HDA_DEVICE_ID_2668, HDA_DEVICE_ID_284B,
        HDA_DEVICE_ID_293E, HDA_DEVICE_ID_3A3E, HDA_DEVICE_ID_3B56
    };

    for (uint8_t bus = 0; bus < 255; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor = pci_read_config32(bus, dev, func, 0x00);
                if (vendor == 0xFFFFFFFF) continue;

                uint16_t vid = vendor & 0xFFFF;
                uint16_t did = (vendor >> 16) & 0xFFFF;

                if (vid != HDA_VENDOR_ID) continue;

                for (int i = 0; i < (int)ARRAY_SIZE(supported_devices); i++) {
                    if (did == supported_devices[i]) {
                        printk("hda: 发现设备 bus=%u dev=%u func=%u did=0x%04X",
                               bus, dev, func, did);
                        hda_dev.pci_bus = bus;
                        hda_dev.pci_dev = dev;
                        hda_dev.pci_func = func;
                        return 0;
                    }
                }
            }
        }
    }

    printk("hda: 未找到 HDA 设备");
    return -1;
}

int hda_init(void) {
    printk("hda: 初始化 Intel HD Audio 驱动...");

    memset(&hda_dev, 0, sizeof(hda_dev));
    spinlock_init(&hda_dev.lock);

    /* 探测 PCI 设备 */
    if (hda_probe() != 0) return -1;

    /* 获取 MMIO 基址 (BAR0) */
    uint32_t bar0 = pci_get_bar(hda_dev.pci_bus, hda_dev.pci_dev, hda_dev.pci_func, 0);
    if (bar0 & 0x01) {
        printk("hda: BAR0 是 I/O 空间, 不支持");
        return -1;
    }
    hda_dev.mmio_phys = bar0 & ~0xF;
    hda_dev.mmio_virt = (uint64_t)phys_to_virt(hda_dev.mmio_phys);

    /* 启用 PCI 总线主控和内存空间 */
    pci_enable_bus_master(hda_dev.pci_bus, hda_dev.pci_dev, hda_dev.pci_func);
    uint32_t cmd = pci_read_config32(hda_dev.pci_bus, hda_dev.pci_dev, hda_dev.pci_func, 0x04);
    cmd |= 0x06;  /* Memory Space + Bus Master */
    pci_write_config32(hda_dev.pci_bus, hda_dev.pci_dev, hda_dev.pci_func, 0x04, cmd);

    /* 获取 IRQ */
    hda_dev.irq_line = pci_get_irq_line(hda_dev.pci_bus, hda_dev.pci_dev, hda_dev.pci_func);

    /* 读取控制器能力 */
    uint16_t gcap = hda_reg_read16(&hda_dev, HDA_GCAP);
    hda_dev.num_output_streams = (gcap >> 12) & 0x0F;
    hda_dev.num_input_streams = (gcap >> 8) & 0x0F;
    hda_dev.num_bidir_streams = (gcap >> 3) & 0x0F;
    printk("hda: 输出流=%u 输入流=%u 双向流=%u",
           hda_dev.num_output_streams, hda_dev.num_input_streams, hda_dev.num_bidir_streams);

    /* 控制器复位 */
    hda_reg_write32(&hda_dev, HDA_GCTL, 0);
    for (volatile int i = 0; i < 500000; i++);
    hda_reg_write32(&hda_dev, HDA_GCTL, HDA_GCTL_CRST);

    /* 等待复位完成 */
    for (int i = 0; i < 1000; i++) {
        if (hda_reg_read32(&hda_dev, HDA_GCTL) & HDA_GCTL_CRST) break;
    }

    /* 等待编解码器就绪 */
    for (int i = 0; i < 1000; i++) {
        uint16_t statests = hda_reg_read16(&hda_dev, HDA_STATESTS);
        if (statests & 0x0F) break;
    }

    /* 初始化 CORB/RIRB */
    if (hda_corb_init(&hda_dev) != 0) {
        printk("hda: CORB 初始化失败");
        return -1;
    }
    if (hda_rirb_init(&hda_dev) != 0) {
        printk("hda: RIRB 初始化失败");
        return -1;
    }

    /* 枚举编解码器 */
    if (hda_enum_codecs(&hda_dev) != 0) {
        printk("hda: 未找到编解码器");
        return -1;
    }

    /* 分配输出流缓冲区 */
    for (int i = 0; i < hda_dev.num_output_streams && i < 8; i++) {
        hda_dev.output_streams[i].index = i;
        hda_dev.output_streams[i].direction = 0;
        hda_dev.output_streams[i].format = HDA_FORMAT_48KHZ_16BIT_STEREO;
        if (hda_stream_alloc_buffers(&hda_dev.output_streams[i]) != 0) {
            printk("hda: 输出流 %d 缓冲区分配失败", i);
        }
    }

    /* 启用全局中断 */
    hda_reg_write32(&hda_dev, HDA_INTCTL,
                     HDA_INTCTL_GIE | HDA_INTCTL_CIE | 0xFF00);  /* 使能所有流中断 */

    /* 注册中断处理 */
    if (hda_dev.irq_line != 0xFF && hda_dev.irq_line != 0) {
        uint8_t vector = IRQ_BASE + hda_dev.irq_line;
        isr_register_handler(vector, hda_interrupt_handler);
        printk("hda: 中断已注册 IRQ=%u", hda_dev.irq_line);
    }

    printk("hda: Intel HD Audio 驱动初始化完成");
    return 0;
}

struct hda_device *hda_get_device(void) {
    return &hda_dev;
}
