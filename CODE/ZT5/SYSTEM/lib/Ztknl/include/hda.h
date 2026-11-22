/*
 * Zratro OS - Intel HD Audio 驱动
 *
 * 支持: Intel HDA 控制器 (PCI 8086:2668)
 * 功能: CORB/RIRB 命令环缓冲区, 流管理, Widget 树解析, PCM 回放
 */

#ifndef HDA_H
#define HDA_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== PCI 标识 ========== */
#define HDA_VENDOR_ID          0x8086
#define HDA_DEVICE_ID_2668     0x2668   /* ICH6 HDA */
#define HDA_DEVICE_ID_284B     0x284B   /* ICH7 HDA */
#define HDA_DEVICE_ID_293E     0x293E   /* ICH9 HDA */
#define HDA_DEVICE_ID_3A3E     0x3A3E   /* ICH10 HDA */
#define HDA_DEVICE_ID_3B56     0x3B56   /* PCH HDA */

/* ========== HDA 控制器 MMIO 寄存器偏移 ========== */
#define HDA_GCAP           0x0000   /* 全局能力 */
#define HDA_VMIN           0x0002   /* 次版本号 */
#define HDA_VMAJ           0x0003   /* 主版本号 */
#define HDA_OUTPAY         0x0004   /* 输出有效载荷能力 */
#define HDA_INPAY          0x0006   /* 输入有效载荷能力 */
#define HDA_GCTL           0x0008   /* 全局控制 */
#define HDA_WAKEEN         0x000C   /* 唤醒使能 */
#define HDA_STATESTS       0x000E   /* 状态更改状态 */
#define HDA_GSTS           0x0010   /* 全局状态 */
#define HDA_OUTSTRMPAY     0x0018   /* 输出流有效载荷 */
#define HDA_INSTRMPAY      0x001A   /* 输入流有效载荷 */
#define HDA_INTCTL         0x0020   /* 中断控制 */
#define HDA_INTSTS         0x0024   /* 中断状态 */
#define HDA_WALCLK         0x0030   /* 墙壁时钟计数器 */
#define HDA_SSYNC          0x0038   /* 流同步 */
#define HDA_CORBLBASE      0x0040   /* CORB 基地址低 */
#define HDA_CORBUBASE      0x0044   /* CORB 基地址高 */
#define HDA_CORBWP         0x0048   /* CORB 写指针 */
#define HDA_CORBRP         0x004A   /* CORB 读指针 */
#define HDA_CORBCTL        0x004C   /* CORB 控制 */
#define HDA_CORBSTS        0x004D   /* CORB 状态 */
#define HDA_CORBSIZE       0x004E   /* CORB 大小 */
#define HDA_RIRBLBASE      0x0050   /* RIRB 基地址低 */
#define HDA_RIRBUBASE      0x0054   /* RIRB 基地址高 */
#define HDA_RIRBWP         0x0058   /* RIRB 写指针 */
#define HDA_RINTCNT        0x005A   /* 响应中断计数 */
#define HDA_RIRBCTL        0x005C   /* RIRB 控制 */
#define HDA_RIRBSTS        0x005D   /* RIRB 状态 */
#define HDA_RIRBSIZE       0x005E   /* RIRB 大小 */
#define HDA_ICOI           0x0060   /* 立即命令输出 */
#define HDA_ICII           0x0064   /* 立即命令输入 */
#define HDA_ICIS           0x0068   /* 立即命令状态 */
#define HDA_DPLBASE        0x0070   /* DMA 位置缓冲区基址低 */
#define HDA_DPUBASE        0x0074   /* DMA 位置缓冲区基址高 */

/* 流描述符寄存器基址 (每个 0x20 字节) */
#define HDA_SD_BASE        0x0080
#define HDA_SD_SIZE        0x0020

/* 流描述符内偏移 */
#define HDA_SD_CTL         0x0000   /* 流描述符控制 */
#define HDA_SD_STS         0x0003   /* 流描述符状态 */
#define HDA_SD_LPIB        0x0004   /* 链接位置 */
#define HDA_SD_CBL         0x0008   /* 循环缓冲区长度 */
#define HDA_SD_LVI         0x000C   /* 最后有效索引 */
#define HDA_SD_FIFOW       0x000E   /* FIFO 水位线 */
#define HDA_SD_FIFOSIZE    0x0010   /* FIFO 大小 */
#define HDA_SD_FORMAT      0x0012   /* 流格式 */
#define HDA_SD_BDPL        0x0018   /* BDL 基地址低 */
#define HDA_SD_BDPU        0x001C   /* BDL 基地址高 */

/* ========== 寄存器位定义 ========== */

/* GCTL 位 */
#define HDA_GCTL_CRST      (1 << 0)   /* 控制器复位 */
#define HDA_GCTL_FCNTRL    (1 << 1)   /* 刷新控制 */
#define HDA_GCTL_UNSOL     (1 << 8)   /* 未经请求的响应使能 */

/* CORBCTL 位 */
#define HDA_CORBCTL_CORBRUN  (1 << 1)  /* CORB 运行 */
#define HDA_CORBCTL_CMEIE    (1 << 0)  /* 内存错误中断使能 */

/* RIRBCTL 位 */
#define HDA_RIRBCTL_RIRBDMAEN (1 << 1)  /* RIRB DMA 使能 */
#define HDA_RIRBCTL_RINTCTL   (1 << 0)  /* 响应中断控制 */
#define HDA_RIRBCTL_RIRBOIC   (1 << 2)  /* RIRB 溢出中断控制 */

/* CORBSTS 位 */
#define HDA_CORBSTS_CMEI   (1 << 0)   /* 内存错误指示 */

/* RIRBSTS 位 */
#define HDA_RIRBSTS_RINTFL (1 << 0)   /* 响应中断 */
#define HDA_RIRBSTS_RIRBOIS (1 << 2)  /* 溢出中断 */

/* SD_CTL 位 */
#define HDA_SD_CTL_SRST    (1 << 0)   /* 流复位 */
#define HDA_SD_CTL_RUN     (1 << 1)   /* 流运行 */
#define HDA_SD_CTL_IOCE    (1 << 2)   /* 中断完成使能 */
#define HDA_SD_CTL_DEIE    (1 << 3)   /* 描述符错误中断使能 */
#define HDA_SD_CTL_FIFIE   (1 << 4)   /* FIFO 错误中断使能 */

/* SD_STS 位 */
#define HDA_SD_STS_BCIS    (1 << 2)   /* 缓冲区完成中断状态 */
#define HDA_SD_STS_DESE    (1 << 4)   /* 描述符错误 */
#define HDA_SD_STS_FIFOE   (1 << 3)   /* FIFO 错误 */
#define HDA_SD_STS_DCH     (1 << 6)   /* DMA 停止 */

/* INTCTL 位 */
#define HDA_INTCTL_GIE     (1 << 0)   /* 全局中断使能 */
#define HDA_INTCTL_CIE     (1 << 1)   /* 控制器中断使能 */

/* ========== 流格式定义 ========== */
#define HDA_FORMAT_48KHZ_16BIT_STEREO  0x0041
#define HDA_FORMAT_44KHZ_16BIT_STEREO  0x0031
#define HDA_FORMAT_96KHZ_16BIT_STEREO  0x0051
#define HDA_FORMAT_BASE_48KHZ  0

/* ========== CORB/RIRB 条目 ========== */
#define HDA_CORB_ENTRY_SIZE   4     /* 每条 CORB 条目 4 字节 */
#define HDA_RIRB_ENTRY_SIZE   8     /* 每条 RIRB 条目 8 字节 */
#define HDA_CORB_MAX_ENTRIES  256
#define HDA_RIRB_MAX_ENTRIES  256

/* RIRB 响应格式 */
struct hda_rirb_entry {
    uint32_t response;       /* 编解码器响应 */
    uint32_t response_flags; /* 响应标志 (bit0=未经请求) */
} __attribute__((packed));

/* ========== BDL 缓冲区描述符 ========== */
struct hda_bdl_entry {
    uint64_t buffer_addr;    /* 缓冲区物理地址 */
    uint32_t length;         /* 缓冲区长度 (字节) */
    uint32_t ctrl;           /* 控制位 (bit0=IOC 中断) */
} __attribute__((packed));

/* ========== Widget 类型 ========== */
#define HDA_WIDGET_TYPE_AFG       0x01   /* 音频功能组 */
#define HDA_WIDGET_TYPE_DAC       0x03   /* DAC */
#define HDA_WIDGET_TYPE_ADC       0x04   /* ADC */
#define HDA_WIDGET_TYPE_MIXER     0x05   /* 混音器 */
#define HDA_WIDGET_TYPE_SELECTOR  0x06   /* 选择器 */
#define HDA_WIDGET_TYPE_PIN       0x07   /* 引脚 */
#define HDA_WIDGET_TYPE_POWER     0x08   /* 电源 */
#define HDA_WIDGET_TYPE_VOLUME    0x09   /* 音量旋钮 */
#define HDA_WIDGET_TYPE_BEEP      0x0A   /* 蜂鸣器 */

/* 引脚连接类型 */
#define HDA_PIN_CONN_JACK        0x00
#define HDA_PIN_CONN_NONE        0x01
#define HDA_PIN_CONN_FIXED       0x02
#define HDA_PIN_CONN_BOTH        0x03

/* 引脚默认设备 */
#define HDA_PIN_DEV_LINE_OUT     0x00
#define HDA_PIN_DEV_SPEAKER      0x01
#define HDA_PIN_DEV_HP_OUT       0x02
#define HDA_PIN_DEV_CD           0x03
#define HDA_PIN_DEV_SPDIF_OUT    0x04
#define HDA_PIN_DEV_DIGIT_OUT    0x05
#define HDA_PIN_DEV_MODEM_LINE   0x06
#define HDA_PIN_DEV_MODEM_HP     0x07
#define HDA_PIN_DEV_LINE_IN      0x08
#define HDA_PIN_DEV_AUX          0x09
#define HDA_PIN_DEV_MIC_IN       0x0A
#define HDA_PIN_DEV_SPDIF_IN     0x0B

/* ========== Widget 信息结构 ========== */
struct hda_widget {
    uint8_t nid;              /* 节点 ID */
    uint8_t type;             /* Widget 类型 */
    uint8_t nconn;            /* 输入连接数 */
    uint32_t conn_list[16];   /* 连接列表 */
    uint32_t amp_in_caps;     /* 输入放大器能力 */
    uint32_t amp_out_caps;    /* 输出放大器能力 */
    uint32_t pin_caps;        /* 引脚能力 */
    uint32_t config_default;  /* 引脚默认配置 */
    uint16_t power_state;     /* 电源状态 */
};

/* ========== HDA 流结构 ========== */
#define HDA_MAX_STREAMS      16
#define HDA_BDL_ENTRIES      32
#define HDA_STREAM_BUF_SIZE  8192

struct hda_stream {
    int index;                /* 流索引 */
    int direction;            /* 0=输出, 1=输入 */
    int running;              /* 是否运行中 */
    uint32_t format;          /* 流格式 */

    /* BDL */
    struct hda_bdl_entry *bdl;
    uint64_t bdl_phys;

    /* 音频缓冲区 */
    uint8_t *buffers[HDA_BDL_ENTRIES];
    uint64_t buffer_phys[HDA_BDL_ENTRIES];

    /* 播放状态 */
    uint32_t position;        /* 当前播放位置 */
    uint64_t bytes_played;    /* 已播放字节数 */
};

/* ========== HDA 编解码器结构 ========== */
struct hda_codec {
    uint8_t addr;             /* 编解码器地址 (0-15) */
    uint32_t vendor_id;       /* 厂商 ID */
    uint32_t revision_id;     /* 修订 ID */
    int ready;                /* 是否就绪 */

    /* Widget 树 */
    struct hda_widget widgets[64];  /* 最多 64 个 Widget */
    int num_widgets;          /* Widget 数量 */
    uint8_t afg_nid;          /* 音频功能组节点 ID */
    uint8_t root_nid;         /* 根节点 ID */

    /* DAC/ADC/Pin 映射 */
    uint8_t dac_nid;          /* 主 DAC 节点 */
    uint8_t adc_nid;          /* 主 ADC 节点 */
    uint8_t pin_nid;          /* 输出引脚节点 */
};

/* ========== HDA 控制器设备结构 ========== */
struct hda_device {
    /* PCI 信息 */
    uint8_t pci_bus;
    uint8_t pci_dev;
    uint8_t pci_func;
    uint8_t irq_line;

    /* MMIO */
    uint64_t mmio_phys;
    uint64_t mmio_virt;       /* MMIO 映射虚拟地址 */

    /* 控制器能力 */
    uint16_t num_output_streams;
    uint16_t num_input_streams;
    uint16_t num_bidir_streams;

    /* CORB/RIRB */
    uint32_t *corb;           /* CORB 缓冲区虚拟地址 */
    uint64_t corb_phys;       /* CORB 物理地址 */
    uint16_t corb_wp;         /* CORB 写指针 */

    struct hda_rirb_entry *rirb;  /* RIRB 缓冲区 */
    uint64_t rirb_phys;
    uint16_t rirb_rp;         /* RIRB 读指针 */

    /* 编解码器 */
    struct hda_codec codecs[4];    /* 最多 4 个编解码器 */
    int num_codecs;

    /* 流管理 */
    struct hda_stream output_streams[8];
    struct hda_stream input_streams[4];

    /* 锁 */
    spinlock_t lock;

    /* 统计 */
    uint64_t bytes_played;
    uint64_t underruns;
};

/* ========== API 函数声明 ========== */

/* 初始化 HDA 驱动 */
int hda_init(void);

/* 探测 HDA 设备 */
int hda_probe(void);

/* 编解码器命令 */
uint32_t hda_codec_command(uint8_t codec_addr, uint32_t verb);
uint32_t hda_codec_get_param(uint8_t codec_addr, uint8_t nid, uint32_t param);
uint32_t hda_codec_set_amp(uint8_t codec_addr, uint8_t nid, uint32_t payload);

/* Widget 树 */
int hda_parse_widgets(struct hda_codec *codec);

/* 流管理 */
int hda_stream_start(int stream_index, int direction);
int hda_stream_stop(int stream_index, int direction);
int hda_stream_play(const void *data, size_t len);

/* PCM 播放 */
int hda_play(const void *data, size_t len);
int hda_stop(void);

/* 音量控制 */
int hda_set_volume(uint8_t nid, int left_db, int right_db);
int hda_set_mute(uint8_t nid, int mute);

/* 中断处理 (内部函数，hda.c 中定义) */

/* 获取设备实例 */
struct hda_device *hda_get_device(void);

#endif /* HDA_H */
