/* dkm_audio.h — 音频服务总线客户端（hda.drv 提供 srv.audio.*）
 *
 * 消费者（shell 的 beep / 未来的音乐播放器）经服务总线取音频能力，
 * 不直接碰 HDA 寄存器。header-only，无静态缓冲。
 *
 * 服务：
 *   srv.audio.play  int (*)(const void *pcm, u32 bytes, u32 rate,
 *                           u32 channels, u32 bits)
 *                   同步播放一段 PCM。返回 0 = 播完；-1 = 参数不支持；
 *                   -2 = 设备错误（流复位失败/超时）。
 *   srv.audio.stop  int (*)(void)        停止当前流。
 *   srv.audio.info  const dkm_audio_info *(*)(void)   只读能力快照。
 *
 * 缺席策略：hda.drv 是可选驱动（FUCK [drivers] hda=0 / 无 HDA 控制器），
 * 消费方 Probe 失败时自行降级（shell beep → PC 喇叭），不得 panic。
 */
#ifndef DKM_AUDIO_H
#define DKM_AUDIO_H

#include "dkm_shared.h"

#define DKM_AUDIO_INFO_MAGIC 0x41554449u /* "AUDI" */
#define DKM_AUDIO_INFO_VERSION 1u

typedef struct dkm_audio_info {
    u32 magic;          /* DKM_AUDIO_INFO_MAGIC */
    u32 version;        /* DKM_AUDIO_INFO_VERSION */
    u32 ready;          /* 1 = 控制器 + 输出路径就绪 */
    u32 rate;           /* 当前流采样率（Hz） */
    u32 channels;       /* 当前流声道数 */
    u32 bits;           /* 当前流位深 */
    u32 codec_vendor;   /* codec vendor id（原样，调试用） */
    u32 codec_device;   /* codec device id */
    u32 dac_nid;        /* 输出转换器节点 */
    u32 pin_nid;        /* 输出引脚节点 */
    u32 stream_tag;     /* 使用的流标签（1..15） */
    u32 controller_vid; /* PCI vendor:device（高 16 位 = device） */
} dkm_audio_info;

typedef int (*dkm_audio_play_fn)(const void *pcm, u32 bytes, u32 rate,
                                 u32 channels, u32 bits);
typedef int (*dkm_audio_stop_fn)(void);
typedef const dkm_audio_info *(*dkm_audio_info_fn)(void);

typedef struct dkm_audio {
    const struct dkm_kernel_api *api;
    dkm_audio_play_fn play;
    dkm_audio_stop_fn stop;
    dkm_audio_info_fn info;
    const dkm_audio_info *snapshot;
} dkm_audio;

/* 0 = 服务齐全；-1 = play 缺席（音频不可用）；-2 = info 快照无效。 */
static __inline__ int dkm_audio_open(dkm_audio *a, const struct dkm_kernel_api *api) {
    void *fn = 0;
    if (!a) return -1;
    a->api = api;
    a->play = 0;
    a->stop = 0;
    a->info = 0;
    a->snapshot = 0;
    if (!api || !api->bus) return -1;
    if (api->bus->probe("srv.audio.play", &fn, 0) != 0 || !fn) return -1;
    a->play = (dkm_audio_play_fn)fn;
    fn = 0;
    if (api->bus->probe("srv.audio.stop", &fn, 0) == 0 && fn)
        a->stop = (dkm_audio_stop_fn)fn;
    fn = 0;
    if (api->bus->probe("srv.audio.info", &fn, 0) == 0 && fn) {
        a->info = (dkm_audio_info_fn)fn;
        a->snapshot = a->info();
        if (!a->snapshot || a->snapshot->magic != DKM_AUDIO_INFO_MAGIC) return -2;
    }
    return 0;
}

#endif /* DKM_AUDIO_H */
