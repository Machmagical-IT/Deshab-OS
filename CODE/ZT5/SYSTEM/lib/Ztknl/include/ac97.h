/*
 * Zratro OS - Intel AC97 音频控制器驱动
 *
 * 支持: Intel AC97 音频控制器 (PCI 8086:2415)
 * 功能: PCM 回放 (48kHz, 16-bit 立体声), 混音器控制, BDL 缓冲区管理
 * 中断: 驱动播放完成中断
 */

#ifndef AC97_H
#define AC97_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"
#include "idt.h"      /* struct interrupt_frame */

/* ========== PCI 标识 ========== */
#define AC97_VENDOR_ID      0x8086
#define AC97_DEVICE_ID      0x2415

/* ========== NATIVE AUDIO MIXER 寄存器 (端口偏移) ========== */
#define AC97_NAM_RESET          0x0000  /* 重置 */
#define AC97_NAM_MASTER_VOL    0x0002  /* 主音量 */
#define AC97_NAM_HEADPHONE_VOL 0x0004  /* 耳机音量 */
#define AC97_NAM_MONO_VOL      0x0006  /* 单声道音量 */
#define AC97_NAM_MASTER_TONE   0x0008  /* 音调控制 */
#define AC97_NAM_PCBEEP_VOL    0x000A  /* PC 蜂鸣器音量 */
#define AC97_NAM_PHONE_VOL     0x000C  /* 电话音量 */
#define AC97_NAM_MIC_VOL       0x000E  /* 麦克风音量 */
#define AC97_NAM_LINEIN_VOL    0x0010  /* 线路输入音量 */
#define AC97_NAM_CD_VOL        0x0012  /* CD 音量 */
#define AC97_NAM_VIDEO_VOL     0x0014  /* 视频音量 */
#define AC97_NAM_AUX_VOL       0x0016  /* 辅助音量 */
#define AC97_NAM_PCM_OUT_VOL   0x0018  /* PCM 输出音量 */
#define AC97_NAM_RECORD_SEL    0x001A  /* 录音选择 */
#define AC97_NAM_RECORD_GAIN   0x001C  /* 录音增益 */
#define AC97_NAM_RECORD_GAIN_MIC 0x001E /* 麦克风录音增益 */
#define AC97_NAM_GENERAL_PURPOSE 0x0020 /* 通用控制 */
#define AC97_NAM_3D_CONTROL    0x0022  /* 3D 控制 */
#define AC97_NAM_POWERDOWN     0x0026  /* 电源管理 */
#define AC97_NAM_EXT_AUDIO_ID  0x0028  /* 扩展音频 ID */
#define AC97_NAM_EXT_AUDIO_CTRL 0x002A /* 扩展音频控制 */
#define AC97_NAM_PCM_FRONT_DAC_RATE 0x002C /* PCM 前置 DAC 采样率 */
#define AC97_NAM_PCM_SURR_DAC_RATE  0x002E /* PCM 环绕 DAC 采样率 */
#define AC97_NAM_PCM_LFE_DAC_RATE   0x0030 /* PCM LFE DAC 采样率 */
#define AC97_NAM_PCM_LR_ADC_RATE    0x0032 /* PCM ADC 采样率 */

/* ========== NATIVE AUDIO BUS MASTER 寄存器 ========== */
#define AC97_NABM_PCM_OUT       0x00    /* PCM 输出通道基址 */
#define AC97_NABM_PCM_IN        0x10    /* PCM 输入通道基址 */
#define AC97_NABM_MIC_IN        0x20    /* 麦克风输入通道基址 */

/* 通道内偏移 (相对于通道基址) */
#define AC97_BDBAR      0x00    /* 缓冲区描述符列表基址 (32字节对齐) */
#define AC97_CIV        0x04    /* 当前索引值 */
#define AC97_LVI        0x05    /* 最后有效索引 */
#define AC97_SR         0x06    /* 状态寄存器 */
#define AC97_PICB       0x08    /* 已处理缓冲区位置 */
#define AC97_PIV        0x0A    /* 预取索引值 */
#define AC97_CR         0x0B    /* 控制寄存器 */
#define AC97_GLOB_CNT   0x2C    /* 全局控制 */
#define AC97_GLOB_STA   0x30    /* 全局状态 */
#define AC97_CAS        0x34    /* 编解码器访问语义 */

/* ========== 控制寄存器位 ========== */
#define AC97_CR_RPBM    0x01    /* 运行/暂停总线主控 */
#define AC97_CR_RR      0x02    /* 重置总线主控寄存器 */
#define AC97_CR_LVBIE   0x04    /* 最后有效缓冲区中断使能 */
#define AC97_CR_FEIE    0x08    /* FIFO 错误中断使能 */
#define AC97_CR_IOCE    0x10    /* 中断完成使能 */

/* ========== 状态寄存器位 ========== */
#define AC97_SR_DCH     0x0001  /* DMA 通道停止 */
#define AC97_SR_CELV    0x0002  /* 当前等于最后有效 */
#define AC97_SR_LVBCI   0x0004  /* 最后有效缓冲区完成中断 */
#define AC97_SR_BCIS    0x0008  /* 缓冲区完成中断状态 */
#define AC97_SR_FIFOE   0x0010  /* FIFO 错误 */

/* ========== 全局控制位 ========== */
#define AC97_GLOB_CNT_GIE   0x00000001  /* 全局中断使能 */
#define AC97_GLOB_CNT_COLD  0x00000002  /* 冷复位 */
#define AC97_GLOB_CNT_WARM  0x00000004  /* 热复位 */
#define AC97_GLOB_CNT_SHUT  0x00000008  /* 关闭 */
#define AC97_GLOB_CNT_PCLK  0x00000400  /* 主时钟分频 */

/* ========== 全局状态位 ========== */
#define AC97_GLOB_STA_GSCI  0x00000001  /* GPI 状态变化中断 */
#define AC97_GLOB_STA_MI    0x00000002  /* 麦克风输入中断 */
#define AC97_GLOB_STA_GPI   0x00000004  /* GPI 中断 */
#define AC97_GLOB_STA_GPE   0x00000008  /* GPI 使能 */
#define AC97_GLOB_STA_PI    0x00000010  /* PCM 输入中断 */
#define AC97_GLOB_STA_PO    0x00000020  /* PCM 输出中断 */
#define AC97_GLOB_STA_MO    0x00000040  /* 麦克风输出中断 */
#define AC97_GLOB_STA_RCS   0x00008000  /* 编解码器就绪 */
#define AC97_GLOB_STA_SLT   0x00010000  /* 插槽 12 中断 */
#define AC97_GLOB_STA_SCR   0x00020000  /* 二级编解码器就绪 */
#define AC97_GLOB_STA_PCR   0x00040000  /* 主编解码器就绪 */
#define AC97_GLOB_STA_MINT  0x01000000  /* 混音器中断 */
#define AC97_GLOB_STA_POINT 0x02000000  /* PCM 输出中断 */
#define AC97_GLOB_STA_PIINT 0x04000000  /* PCM 输入中断 */
#define AC97_GLOB_STA_MOINT 0x08000000  /* 麦克风输出中断 */

/* ========== 音频参数 ========== */
#define AC97_SAMPLE_RATE     48000   /* 默认采样率 48kHz */
#define AC97_SAMPLE_BITS     16      /* 16-bit */
#define AC97_CHANNELS        2       /* 立体声 */
#define AC97_BDL_ENTRIES     32      /* BDL 条目数 */
#define AC97_BUFFER_SIZE     4096    /* 每个缓冲区大小 (字节) */
#define AC97_NUM_BUFFERS     AC97_BDL_ENTRIES

/* ========== 缓冲区描述符 ========== */
struct ac97_bdl_entry {
    uint32_t buffer_addr;    /* 缓冲区物理地址 (32位) */
    uint16_t length;         /* 采样数 (以帧为单位) */
    uint16_t ctrl;           /* 控制位 */
} __attribute__((packed));

/* BDL 控制位 */
#define AC97_BDL_IOC    0x8000  /* 中断完成 */
#define AC97_BDL_BUP    0x4000  /* 缓冲区未完成 */

/* ========== PCM 音频帧格式 ========== */
struct ac97_pcm_frame {
    int16_t left;     /* 左声道采样 */
    int16_t right;    /* 右声道采样 */
} __attribute__((packed));

/* ========== 混音器控制结构 ========== */
struct ac97_mixer_state {
    uint16_t master_vol;      /* 主音量 (0-0x3F) */
    uint16_t pcm_vol;         /* PCM 音量 (0-0x1F) */
    uint16_t mic_vol;         /* 麦克风音量 */
    uint16_t linein_vol;      /* 线路输入音量 */
    uint16_t cd_vol;          /* CD 音量 */
    int muted;                /* 是否静音 */
};

/* ========== AC97 设备结构 ========== */
struct ac97_device {
    /* PCI 信息 */
    uint8_t pci_bus;
    uint8_t pci_dev;
    uint8_t pci_func;
    uint8_t irq_line;

    /* I/O 端口基址 */
    uint16_t nam_base;        /* Native Audio Mixer 基址 */
    uint16_t nabm_base;       /* Native Audio Bus Master 基址 */

    /* BDL 缓冲区描述符列表 (必须 32 字节对齐) */
    struct ac97_bdl_entry *bdl;
    uint64_t bdl_phys;        /* BDL 物理地址 */

    /* PCM 音频缓冲区 */
    uint8_t *buffers[AC97_NUM_BUFFERS];
    uint64_t buffer_phys[AC97_NUM_BUFFERS];

    /* 播放状态 */
    uint32_t play_civ;        /* 当前播放索引 */
    uint32_t play_lvi;        /* 最后有效索引 */
    int playing;              /* 是否正在播放 */
    spinlock_t lock;          /* 自旋锁 */

    /* 混音器状态 */
    struct ac97_mixer_state mixer;

    /* 编解码器信息 */
    uint16_t codec_vendor_id;
    uint16_t codec_revision;
    int codec_ready;

    /* 统计 */
    uint64_t bytes_played;
    uint64_t underruns;
};

/* ========== API 函数声明 ========== */

/* 初始化 AC97 驱动 */
int ac97_init(void);

/* 检测 AC97 设备 */
int ac97_probe(void);

/* 混音器控制 */
int ac97_set_master_volume(uint8_t left, uint8_t right);
int ac97_get_master_volume(uint8_t *left, uint8_t *right);
int ac97_set_pcm_volume(uint8_t left, uint8_t right);
int ac97_get_pcm_volume(uint8_t *left, uint8_t *right);
int ac97_mute(int mute);
int ac97_is_muted(void);

/* PCM 播放 */
int ac97_play(const void *data, size_t len);
int ac97_stop(void);
int ac97_pause(void);
int ac97_resume(void);
int ac97_is_playing(void);

/* 设置采样率 */
int ac97_set_sample_rate(uint32_t rate);
uint32_t ac97_get_sample_rate(void);

/* 中断处理 */
void ac97_interrupt_handler(struct interrupt_frame *frame);

/* 获取设备实例 */
struct ac97_device *ac97_get_device(void);

#endif /* AC97_H */
