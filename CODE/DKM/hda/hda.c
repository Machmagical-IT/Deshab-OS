/* DKM Intel HDA Audio Driver — stage3, optional, depends "pci"
 *
 * 母本：
 *   SYSTEMCOPY/OpenXJ380/driver/hda/hda.cpp（Apache-2.0）—— Immediate Command
 *   模式（无 CORB/RIRB DMA）+ AFG 子节点遍历 + 引脚选择 + 同步 PCM 回放；
 *   CODE/ZT5/SYSTEM/lib/Ztknl/hda.c —— 流描述符寄存器布局。
 * 本驱动按 DKM ABI 重写：DMA 走 kernel_api.dma（低 4G），MMIO 走
 * mm_map_mmio（失败回退 HHDM）；回放为**轮询同步**（BCIS），不注册 IRQ。
 *
 * 服务：
 *   srv.audio.play   int (*)(const void *pcm, u32 bytes, u32 rate,
 *                            u32 channels, u32 bits)
 *   srv.audio.stop   int (*)(void)
 *   srv.audio.info   const dkm_audio_info *(*)(void)
 * 消费契约见 CODE/DKM/dkm_audio.h。
 *
 * 错误码（零降级，仅当 FUCK [drivers] hda=1 且存在 HDA 控制器时生效）：
 *   HDA-E01 CONTROLLER RESET TIMEOUT
 *   HDA-E02 NO CODEC RESPONSE
 *   HDA-E03 NO OUTPUT PATH (无 DAC 或输出引脚)
 *   HDA-E04 DMA ALLOC FAILED
 *   HDA-E05 BDL/STREAM RESET FAILED
 * 可选驱动缺席（无控制器 / FUCK 关闭）走设计内回退：WARN + driver ready。
 */

#include "../dkm_shared.h"
#include "../dkm_audio.h"

/* ================================================================
 *  驱动描述符
 * ================================================================ */
static const char *const g_depends[]  = { "pci" };
static const char *const g_provides[] = { "audio", "hda" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "hda",
    .version        = "0.1.0",
    .vendor         = "Deshab",
    .driver_class   = DKM_CLASS_MISC,
    .stage          = 3,
    .flags          = 0,
    .priority       = 0,
    .depends        = g_depends,
    .depends_count  = 1,
    .provides       = g_provides,
    .provides_count = 2,
    .min_kernel_abi = 1,
};

static const struct dkm_log_api *g_log;
static const struct dkm_kernel_api *g_api;

/* ================================================================
 *  控制器寄存器（HDA 1.0a §3.3）
 * ================================================================ */
#define HDA_GCAP        0x00    /* 16: [11:8]=ISS [15:12]=OSS [7:3]=BSS */
#define HDA_GCTL        0x08    /* 32: bit0 CRST */
#define HDA_STATESTS    0x0E    /* 16: codec 存在位 */
#define HDA_INTCTL      0x20    /* 32: 关中断（本驱动轮询） */
#define HDA_ICOI        0x60    /* 32: Immediate Command Output */
#define HDA_ICII        0x64    /* 32: Immediate Response Input */
#define HDA_ICIS        0x68    /* 16: bit0 ICB, bit1 IRV */

#define HDA_SD_BASE     0x80
#define HDA_SD_STRIDE   0x20
#define SD_CTL          0x00    /* 32: [23:20]=tag [1]=RUN [0]=SRST */
#define SD_STS          0x03    /*  8: bit2 BCIS（W1C） */
#define SD_LPIB         0x04
#define SD_CBL          0x08    /* 32 */
#define SD_LVI          0x0C    /* 16 */
#define SD_FMT          0x12    /* 16 */
#define SD_BDPL         0x18    /* 32 */
#define SD_BDPU         0x1C    /* 32 */

#define SD_CTL_SRST     (1u << 0)
#define SD_CTL_RUN      (1u << 1)
#define SD_STS_BCIS     (1u << 2)

/* ================================================================
 *  状态
 * ================================================================ */
typedef struct {
    volatile u8 *mmio;
    u32 mmio_size;

    u8  codec;          /* codec 地址 0..15 */
    u8  afg_nid;
    u8  dac_nid;
    u8  pin_nid;
    u8  stream_tag;     /* 1..15 */
    u32 sd_off;         /* 输出流描述符在 MMIO 中的偏移 */
    u32 codec_vendor;
    u32 controller_vd;  /* vendor<<16 | device */

    struct dkm_dma_buffer bdl;
    struct dkm_dma_buffer pcm;
    u32 pcm_capacity;

    u32 cur_rate, cur_channels, cur_bits;
    u32 dac_amp_cap;
    u32 pin_amp_cap;
    int ready;
} hda_state;

static hda_state g_hda;
static dkm_audio_info g_info;

static inline u8 mm_r8(u32 off)  { return *(volatile u8  *)(g_hda.mmio + off); }
static inline u16 mm_r16(u32 off) { return *(volatile u16 *)(g_hda.mmio + off); }
static inline u32 mm_r32(u32 off) { return *(volatile u32 *)(g_hda.mmio + off); }
static inline void mm_w8(u32 off, u8 v)   { *(volatile u8  *)(g_hda.mmio + off) = v; }
static inline void mm_w16(u32 off, u16 v) { *(volatile u16 *)(g_hda.mmio + off) = v; }
static inline void mm_w32(u32 off, u32 v) { *(volatile u32 *)(g_hda.mmio + off) = v; }

static void hda_zero(void *p, u32 n) {
    u8 *b = (u8 *)p;
    for (u32 i = 0; i < n; i++) b[i] = 0;
}

static u64 hda_rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* "[prefix] 0x...." */
static void log_hex(const char *prefix, u64 val) {
    static const char hx[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) buf[2 + i] = hx[(val >> ((15 - i) * 4)) & 0xF];
    buf[18] = 0;
    g_log->info(prefix);
    g_log->info(buf);
}

/* ================================================================
 *  命令通道（Immediate Command）
 * ================================================================ */
static u32 hda_cmd(u32 cad, u32 nid, u32 verb, u32 payload) {
    if (verb < 0x10) {                       /* 16-bit payload 动词（0x2/0x3） */
        return (cad << 28) | (nid << 20) | (verb << 16) | (payload & 0xFFFF);
    }
    return (cad << 28) | (nid << 20) | (verb << 8) | (payload & 0xFF);
}

static int hda_wait_icb_clear(u32 timeout_ms) {
    u64 deadline = hda_rdtsc() + dkm_tsc_per_ms * timeout_ms;
    while (hda_rdtsc() < deadline) {
        if (!(mm_r16(HDA_ICIS) & 0x1)) return 0;      /* ICB=0 */
        __asm__ volatile("pause");
    }
    return -1;
}

/* 发一条 verb，返回 codec 响应。超时返回 0xFFFFFFFF。 */
static u32 hda_verb(u32 cad, u32 nid, u32 verb, u32 payload) {
    if (hda_wait_icb_clear(20) != 0) {
        g_log->warn("[hda] immediate command busy");
        return 0xFFFFFFFFu;
    }

    mm_w16(HDA_ICIS, 0x2);                 /* 清 IRV */
    mm_w32(HDA_ICOI, hda_cmd(cad, nid, verb, payload));
    mm_w16(HDA_ICIS, 0x1);                 /* 置 ICB */

    u64 deadline = hda_rdtsc() + dkm_tsc_per_ms * 20;
    while (hda_rdtsc() < deadline) {
        u16 st = mm_r16(HDA_ICIS);
        if ((st & 0x3) == 0x2) {           /* ICB=0 IRV=1 */
            u32 resp = mm_r32(HDA_ICII);
            mm_w16(HDA_ICIS, 0x2);         /* 清 IRV */
            return resp;
        }
        __asm__ __volatile__("pause");
    }

    mm_w16(HDA_ICIS, 0x2);
    g_log->warn("[hda] verb timeout");
    return 0xFFFFFFFFu;
}

/* ================================================================
 *  Widget / 参数
 * ================================================================ */
#define PARAM_VENDOR_ID    0x00
#define PARAM_SUB_NODES    0x04
#define PARAM_FGROUP_TYPE  0x05
#define PARAM_WIDGET_CAP   0x09
#define PARAM_CONN_LEN     0x0E
#define PARAM_PIN_CAP      0x0C
#define PARAM_OUT_AMP_CAP  0x12
#define PARAM_IN_AMP_CAP   0x0D
#define PARAM_CONFIG_DEF   0x1C
#define PARAM_POWER_STATE  0x0F

#define VERB_SET_STREAM_FMT      0x2
#define VERB_SET_AMP             0x3
#define VERB_SET_POWER_STATE     0x705
#define VERB_SET_CONV_STREAM     0x706
#define VERB_SET_PIN_CTL         0x707
#define VERB_SET_UNSOL           0x708
#define VERB_SET_EAPD            0x70C
#define VERB_GET_PARAM           0xF00

#define WIDGET_TYPE_DAC     0x0
#define WIDGET_TYPE_PIN     0x4

static u32 hda_param(u32 nid, u32 param) {
    return hda_verb(g_hda.codec, nid, VERB_GET_PARAM, param);
}

static u32 hda_widget_type(u32 nid) {
    return (hda_param(nid, PARAM_WIDGET_CAP) >> 20) & 0xF;
}

/* 16-bit HDA 流格式：支持 8/16/20/24/32 位、1-2 声道、常用采样率。 */
static u16 hda_format(u32 rate, u32 channels, u32 bits) {
    u16 f = (u16)((channels - 1) & 0x0F);
    switch (bits) {
    case 8:  f |= (0x0u << 4); break;
    case 16: f |= (0x1u << 4); break;
    case 20: f |= (0x2u << 4); break;
    case 24: f |= (0x3u << 4); break;
    case 32: f |= (0x4u << 4); break;
    default: return 0;
    }
    switch (rate) {
    case 48000:  f |= (0x00u << 8); break;   /* base 48k, mult x1 */
    case 44100:  f |= ((0x40u << 8) | (0x1u << 14)); break;
    case 32000:  f |= ((0x0Au << 8) | (0x0u << 14)); break;
    case 22050:  f |= ((0x41u << 8) | (0x1u << 14)); break;
    case 16000:  f |= ((0x02u << 8) | (0x0u << 14)); break;
    case 11025:  f |= ((0x43u << 8) | (0x1u << 14)); break;
    case 8000:   f |= ((0x05u << 8) | (0x0u << 14)); break;
    default:     return 0;
    }
    return f;
}

/* SET_AMP_GAIN_MUTE：bit15=输出放大器，[14]=右静音，[13:8]=右增益，
 * [7]=左静音，[6:0]=左增益。index=0。 */
static void hda_set_amp(u32 nid, u32 cap, int volume_pct) {
    if (!cap) return;
    u32 gain_bits = (cap >> 8) & 0x7F;
    u32 gain = gain_bits * (u32)volume_pct / 100;
    u16 payload = (u16)((1u << 15) | (gain << 8) | gain);
    hda_verb(g_hda.codec, nid, VERB_SET_AMP, payload);
}

/* ================================================================
 *  初始化
 * ================================================================ */
static int hda_pci_find(u8 *out_bus, u8 *out_dev, u8 *out_func, u32 *out_vd) {
    for (u8 bus = 0; bus < 32; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            for (u8 func = 0; func < 8; func++) {
                u32 vd = dkm_pci_read(bus, dev, func, PCI_VENDOR_ID);
                if ((vd & 0xFFFF) == 0xFFFF) continue;

                u32 cls = dkm_pci_read(bus, dev, func, 0x08);
                u8 class_code = (u8)((cls >> 24) & 0xFF);
                u8 subclass   = (u8)((cls >> 16) & 0xFF);
                if (class_code != 0x04) continue;
                if (subclass != 0x03 && subclass != 0x01) continue;  /* HDA / Audio */

                *out_bus = bus; *out_dev = dev; *out_func = func; *out_vd = vd;
                return 0;
            }
        }
    }
    return -1;
}

/* 找出 AFG 下的 DAC 与输出引脚。返回 0 = 找到。 */
static int hda_find_output_path(void) {
    u32 root = hda_param(0, PARAM_SUB_NODES);
    if (root == 0xFFFFFFFFu) return -1;
    u32 start = (root >> 16) & 0xFF;
    u32 count = root & 0xFF;

    u32 afg = 0;
    for (u32 i = 0; i < count; i++) {
        u32 nid = start + i;
        if ((hda_param(nid, PARAM_FGROUP_TYPE) & 0xFF) == 0x01) { afg = nid; break; }
    }
    if (!afg) {
        g_log->warn("[hda] no audio function group");
        return -1;
    }
    g_hda.afg_nid = (u8)afg;

    u32 sub = hda_param(afg, PARAM_SUB_NODES);
    u32 nstart = (sub >> 16) & 0xFF;
    u32 ncount = sub & 0xFF;
    if (ncount > 64) ncount = 64;

    u32 first_dac = 0, speaker_pin = 0, hp_pin = 0, line_pin = 0;
    for (u32 i = 0; i < ncount; i++) {
        u32 nid = nstart + i;
        u32 type = hda_widget_type(nid);

        if (type == WIDGET_TYPE_DAC && !first_dac) first_dac = nid;

        if (type == WIDGET_TYPE_PIN) {
            u32 cfg = hda_param(nid, PARAM_CONFIG_DEF);
            u32 dev_type = (cfg >> 20) & 0xF;   /* 默认设备类型 */
            u32 conn     = (cfg >> 30) & 0x3;   /* 0=jack 1=none 2=fixed 3=both */
            if (conn == 1) continue;            /* 未连接 */

            if (dev_type == 0x1 && !speaker_pin) speaker_pin = nid;   /* Speaker */
            else if (dev_type == 0x2 && !hp_pin) hp_pin = nid;        /* HP out */
            else if (dev_type == 0x0 && !line_pin) line_pin = nid;    /* Line out */
        }
    }

    u32 pin = speaker_pin ? speaker_pin : (hp_pin ? hp_pin : line_pin);
    if (!first_dac || !pin) {
        g_log->warn("[hda] no DAC or no output pin");
        return -1;
    }

    g_hda.dac_nid = (u8)first_dac;
    g_hda.pin_nid = (u8)pin;

    g_hda.dac_amp_cap = hda_param(first_dac, PARAM_OUT_AMP_CAP);
    g_hda.pin_amp_cap = hda_param(pin, PARAM_OUT_AMP_CAP);
    return 0;
}

/* 配置输出路径（上电、解除静音、开启引脚输出放大器与 EAPD）。 */
static void hda_configure_output(void) {
    u32 cad = g_hda.codec;

    /* AFG + DAC + 引脚：D0 */
    hda_verb(cad, g_hda.afg_nid, VERB_SET_POWER_STATE, 0x00);
    hda_verb(cad, g_hda.dac_nid, VERB_SET_POWER_STATE, 0x00);
    hda_verb(cad, g_hda.pin_nid, VERB_SET_POWER_STATE, 0x00);

    /* 不做 unsolicited response（无 IRQ） */
    hda_verb(cad, g_hda.pin_nid, VERB_SET_UNSOL, 0x00);

    /* 引脚：输出放大器使能 + HP 放大器使能 */
    u32 pin_ctl = hda_param(g_hda.pin_nid, 0x0C);  /* PIN_CAP */
    (void)pin_ctl;
    hda_verb(cad, g_hda.pin_nid, VERB_SET_PIN_CTL, (1u << 7) | (1u << 6));

    /* EAPD：外部放大器上电 */
    hda_verb(cad, g_hda.pin_nid, VERB_SET_EAPD, 0x02);

    /* 输出放大器 100%（DAC 与引脚各一次） */
    hda_set_amp(g_hda.dac_nid, g_hda.dac_amp_cap, 100);
    hda_set_amp(g_hda.pin_nid, g_hda.pin_amp_cap, 100);
}

/* 复位输出流描述符并挂 BDL。 */
static int hda_stream_setup(u16 fmt, u32 bytes) {
    u32 off = g_hda.sd_off;

    mm_w32(off + SD_CTL, 0);
    dkm_delay_ms(1);
    mm_w32(off + SD_CTL, SD_CTL_SRST);
    u64 deadline = hda_rdtsc() + dkm_tsc_per_ms * 20;
    while (hda_rdtsc() < deadline) {
        if (mm_r32(off + SD_CTL) & SD_CTL_SRST) break;
    }
    mm_w32(off + SD_CTL, 0);
    for (volatile int i = 0; i < 1000; i++) __asm__ volatile("pause");

    mm_w32(off + SD_BDPL, (u32)g_hda.bdl.phys);
    mm_w32(off + SD_BDPU, (u32)(g_hda.bdl.phys >> 32));
    mm_w32(off + SD_CBL, bytes);
    mm_w16(off + SD_LVI, 0);
    mm_w16(off + SD_FMT, fmt);
    mm_w8(off + SD_STS, SD_STS_BCIS);       /* W1C */

    /* BDL 条目：地址 / 长度 / IOC */
    u32 *e = (u32 *)g_hda.bdl.virt;
    e[0] = (u32)g_hda.pcm.phys;
    e[1] = (u32)(g_hda.pcm.phys >> 32);
    e[2] = bytes;
    e[3] = 0x1;
    __asm__ volatile("wbinvd");
    return 0;
}

static int hda_play(const void *pcm, u32 bytes, u32 rate, u32 channels, u32 bits) {
    if (!g_hda.ready) return -1;
    if (!pcm || !bytes) return -1;
    if (bytes > g_hda.pcm_capacity) return -1;
    if (channels < 1 || channels > 2) return -1;

    u16 fmt = hda_format(rate, channels, bits);
    if (!fmt) return -1;

    /* 拷入低 4G DMA 缓冲（控制器只能访问物理地址）。 */
    const u8 *src = (const u8 *)pcm;
    u8 *dst = (u8 *)g_hda.pcm.virt;
    for (u32 i = 0; i < bytes; i++) dst[i] = src[i];

    hda_stream_setup(fmt, bytes);

    /* 转码器：流标签 + 声道；并设置格式 */
    hda_verb(g_hda.codec, g_hda.dac_nid, VERB_SET_CONV_STREAM,
             (u32)((1u << 4) | g_hda.stream_tag));
    hda_verb(g_hda.codec, g_hda.dac_nid, VERB_SET_STREAM_FMT, fmt);
    hda_verb(g_hda.codec, g_hda.pin_nid, VERB_SET_STREAM_FMT, fmt);

    g_hda.cur_rate = rate;
    g_hda.cur_channels = channels;
    g_hda.cur_bits = bits;

    mm_w32(g_hda.sd_off + SD_CTL, ((u32)g_hda.stream_tag << 20) | SD_CTL_RUN);

    /* 同步等待 BCIS；超时按播放时长 + 2s 估算。 */
    u32 frame_bytes = (u32)(channels * (bits / 8));
    u32 ms = bytes / (rate * frame_bytes) * 1000u + 2000u;
    u64 deadline = hda_rdtsc() + dkm_tsc_per_ms * ms;
    while (hda_rdtsc() < deadline) {
        if (mm_r8(g_hda.sd_off + SD_STS) & SD_STS_BCIS) {
            mm_w8(g_hda.sd_off + SD_STS, SD_STS_BCIS);
            mm_w32(g_hda.sd_off + SD_CTL, 0);
            return 0;
        }
        __asm__ __volatile__("pause");
    }

    g_log->warn("[hda] playback timeout");
    mm_w32(g_hda.sd_off + SD_CTL, 0);
    return -2;
}

static int hda_stop(void) {
    if (!g_hda.ready) return -1;
    mm_w32(g_hda.sd_off + SD_CTL, 0);
    return 0;
}

/* ---- 自检：FUCK [drivers] hda_selftest=1 时在 init 末尾播 440Hz 0.25s。
 * 串口给出结论，便于无音频输出的自动化环境判定 DMA/流路径是否成立。 ---- */
static int hda_selftest(void) {
    const u32 rate = 48000, frames = rate / 4;
    u32 bytes = frames * 2u * 2u;                 /* stereo, 16-bit */
    if (bytes > g_hda.pcm_capacity) return -1;

    i16 *p = (i16 *)g_hda.pcm.virt;
    u32 phase = 0, step = (u32)((440ull << 32) / rate);
    for (u32 i = 0; i < frames; i++) {
        /* 三角波：无 FPU，相位高 16 位映射到 [-32767, 32767] */
        u32 t = (phase >> 16) & 0xFFFF;
        i32 s = (t < 32768) ? (i32)t * 2 - 32768 : 65535 - (i32)t * 2;
        i16 v = (i16)(s >> 1);
        p[i * 2] = v;
        p[i * 2 + 1] = v;
        phase += step;
    }
    int rc = hda_play(g_hda.pcm.virt, bytes, rate, 2, 16);
    log_hex("[hda] selftest rc=", (u64)(i64)rc);
    return rc;
}

/* FUCK [drivers] hda_selftest 读取（经 kern.config.get；缺席按 0） */
static int hda_cfg_bool(const char *key) {
    if (!g_api || !g_api->bus) return 0;
    void *fn = 0;
    if (g_api->bus->probe("kern.config.get", &fn, 0) != 0 || !fn) return 0;
    const char *(*get)(const char *, const char *) =
        (const char *(*)(const char *, const char *))fn;
    const char *v = get("drivers", key);
    return v && v[0] == '1';
}

static const dkm_audio_info *hda_info(void) {
    g_info.magic        = DKM_AUDIO_INFO_MAGIC;
    g_info.version      = DKM_AUDIO_INFO_VERSION;
    g_info.ready        = (u32)g_hda.ready;
    g_info.rate         = g_hda.cur_rate;
    g_info.channels     = g_hda.cur_channels;
    g_info.bits         = g_hda.cur_bits;
    g_info.codec_vendor = g_hda.codec_vendor;
    g_info.codec_device = g_hda.codec_vendor;
    g_info.dac_nid      = g_hda.dac_nid;
    g_info.pin_nid      = g_hda.pin_nid;
    g_info.stream_tag   = g_hda.stream_tag;
    g_info.controller_vid = g_hda.controller_vd;
    return &g_info;
}

/* ================================================================
 *  driver_init
 * ================================================================ */
__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;
    dkm_tsc_calibrate();

    if (!api || !api->log) return -1;
    g_api = api;
    g_log = api->log;
    hda_zero(&g_hda, sizeof(g_hda));

    g_log->info("[hda] init begin");

    /* ---- PCI 探测 ---- */
    u8 bus = 0, dev = 0, func = 0;
    u32 vd = 0;
    if (hda_pci_find(&bus, &dev, &func, &vd) != 0) {
        g_log->info("[hda] HDA controller not found");
        g_log->info("[hda] driver ready");
        return 0;                      /* 可选驱动：无控制器即静默就绪 */
    }
    g_hda.controller_vd = vd;
    log_hex("[hda] pci bus=", bus);
    log_hex("[hda] vendor:device=", vd);

    /* MEM + BUSM；关 INTx（轮询模式） */
    u32 cmd = dkm_pci_read(bus, dev, func, PCI_COMMAND) & 0xFFFF;
    cmd |= PCI_CMD_MEM | PCI_CMD_BUSM | (1u << 10);
    dkm_pci_write(bus, dev, func, PCI_COMMAND, cmd);

    /* BAR0（64 位 / 32 位均可） */
    u32 bar0 = dkm_pci_read(bus, dev, func, PCI_BAR0);
    u32 bar1 = dkm_pci_read(bus, dev, func, PCI_BAR1);
    u64 bar_phys;
    if (bar0 & 1u) {
        g_log->warn("[hda] BAR0 is IO space; unsupported");
        g_log->info("[hda] driver ready");
        return 0;
    }
    if ((bar0 & 0x6u) == 0x4u) bar_phys = ((u64)bar1 << 32) | (u64)(bar0 & 0xFFFFFFF0u);
    else                       bar_phys = (u64)(bar0 & 0xFFFFFFF0u);
    if (!bar_phys) {
        g_log->warn("[hda] BAR0 invalid");
        g_log->info("[hda] driver ready");
        return 0;
    }
    log_hex("[hda] BAR0 phys=", bar_phys);

    /* MMIO 映射：优先独立窗口，回退 HHDM */
    g_hda.mmio_size = 0x4000;
    if (api->mm_map_mmio) {
        void *m = api->mm_map_mmio(bar_phys, g_hda.mmio_size);
        if (m) {
            g_hda.mmio = (volatile u8 *)m;
            g_log->info("[hda] BAR mapped via mm_map_mmio");
        }
    }
    if (!g_hda.mmio && api->hhdm_offset) {
        g_hda.mmio = (volatile u8 *)(api->hhdm_offset + bar_phys);
        g_log->info("[hda] BAR mapped via HHDM");
    }
    if (!g_hda.mmio) {
        g_log->warn("[hda] BAR mapping failed");
        g_log->info("[hda] driver ready");
        return 0;
    }

    /* ---- 控制器复位 ---- */
    mm_w32(HDA_GCTL, 0);
    for (volatile int i = 0; i < 20000; i++) __asm__ volatile("pause");
    if (mm_r32(HDA_GCTL) & 0x1) {
        g_log->panic("HDA-E01 CONTROLLER RESET TIMEOUT");
        g_log->info("[hda] driver ready");
        return 0;
    }
    mm_w32(HDA_GCTL, 0x1);
    {
        u64 deadline = hda_rdtsc() + dkm_tsc_per_ms * 100;
        int ok = 0;
        while (hda_rdtsc() < deadline) {
            if (mm_r32(HDA_GCTL) & 0x1) { ok = 1; break; }
            __asm__ volatile("pause");
        }
        if (!ok) {
            g_log->panic("HDA-E01 CONTROLLER RESET TIMEOUT");
            g_log->info("[hda] driver ready");
            return 0;
        }
    }

    /* 关中断：全程轮询 */
    mm_w32(HDA_INTCTL, 0);

    u16 gcap = mm_r16(HDA_GCAP);
    u32 iss = (gcap >> 8) & 0xF;
    u32 oss = (gcap >> 12) & 0xF;
    log_hex("[hda] GCAP=", gcap);
    log_hex("[hda] input streams=", iss);
    log_hex("[hda] output streams=", oss);
    if (oss == 0) {
        g_log->warn("[hda] controller has no output stream");
        g_log->info("[hda] driver ready");
        return 0;
    }
    /* 输出流描述符位于输入流之后 */
    g_hda.sd_off = HDA_SD_BASE + iss * HDA_SD_STRIDE;
    g_hda.stream_tag = 1;

    /* 等 codec 上电 */
    {
        u64 deadline = hda_rdtsc() + dkm_tsc_per_ms * 500;
        while (hda_rdtsc() < deadline) {
            if (mm_r16(HDA_STATESTS) & 0x7) break;
            __asm__ volatile("pause");
        }
    }

    u16 statests = mm_r16(HDA_STATESTS);
    log_hex("[hda] STATESTS=", statests);

    int found = 0;
    for (u32 cad = 0; cad < 15; cad++) {
        if (!(statests & (1u << cad))) continue;
        g_hda.codec = (u8)cad;
        u32 vendor = hda_param(0, PARAM_VENDOR_ID);
        if (vendor == 0xFFFFFFFFu || vendor == 0) continue;
        g_hda.codec_vendor = vendor;
        log_hex("[hda] codec vendor=", vendor);
        if (hda_find_output_path() == 0) { found = 1; break; }
    }
    if (!found) {
        g_log->panic("HDA-E03 NO OUTPUT PATH");
        g_log->info("[hda] driver ready");
        return 0;
    }
    log_hex("[hda] DAC nid=", g_hda.dac_nid);
    log_hex("[hda] PIN nid=", g_hda.pin_nid);
    log_hex("[hda] DAC amp cap=", g_hda.dac_amp_cap);
    log_hex("[hda] PIN amp cap=", g_hda.pin_amp_cap);

    /* ---- DMA 缓冲（BDL 1 页 + PCM 64 页 = 256KiB） ---- */
    if (!api->dma || !api->dma->alloc_pages) {
        g_log->panic("HDA-E04 DMA ALLOC FAILED");
        g_log->info("[hda] driver ready");
        return 0;
    }
    if (api->dma->alloc_pages(1, 128, 0x100000000ULL, &g_hda.bdl) != 0 ||
        api->dma->alloc_pages(64, 128, 0x100000000ULL, &g_hda.pcm) != 0) {
        g_log->panic("HDA-E04 DMA ALLOC FAILED");
        g_log->info("[hda] driver ready");
        return 0;
    }
    g_hda.pcm_capacity = (u32)g_hda.pcm.size;
    hda_zero(g_hda.bdl.virt, (u32)g_hda.bdl.size);
    log_hex("[hda] BDL phys=", g_hda.bdl.phys);
    log_hex("[hda] PCM phys=", g_hda.pcm.phys);

    hda_configure_output();

    /* 试配 48kHz 立体声，确认流寄存器可写 */
    if (hda_stream_setup(hda_format(48000, 2, 16), 4096) != 0) {
        g_log->panic("HDA-E05 BDL/STREAM RESET FAILED");
        g_log->info("[hda] driver ready");
        return 0;
    }
    g_hda.cur_rate = 48000;
    g_hda.cur_channels = 2;
    g_hda.cur_bits = 16;

    if (api->bus && api->bus->register_service) {
        api->bus->register_service("srv.audio.play", 1, (void *)hda_play);
        api->bus->register_service("srv.audio.stop", 1, (void *)hda_stop);
        api->bus->register_service("srv.audio.info", 1, (void *)hda_info);
        g_log->info("[hda] srv.audio.play / stop / info registered");
    }

    g_hda.ready = 1;

    if (hda_cfg_bool("hda_selftest")) {
        g_log->info("[hda] selftest: 440Hz 0.25s");
        hda_selftest();
    }

    g_log->info("[hda] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    if (g_hda.mmio && g_hda.ready) {
        mm_w32(g_hda.sd_off + SD_CTL, 0);
        mm_w32(HDA_GCTL, 0);
    }
    g_hda.ready = 0;
    return 0;
}
