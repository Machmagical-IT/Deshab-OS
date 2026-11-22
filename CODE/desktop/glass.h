/* glass.h — 壁纸 vibrancy（一次性盒式模糊）
 *
 * 壁纸缩放到帧缓冲尺寸后，做半径 8 的可分离盒式模糊，结果放在
 * GLASS_ADDR（0x9100000，上限 8MB）。与 ELF 窗口 0x8800000+8MB 隔 1MB，
 * 不覆盖字库 0x8000000/0x8300000 和开始菜单 logo 0x8700000。
 *
 * 暂存用 WALLPAPER_STAGING_ADDR。glass_init 必须在 crossfade / PE 占用
 * 该区之前调用。
 *
 * du_glass_wallpaper 按帧缓冲坐标采样这块缓存，再与 tint 做 du_blend。
 * 这是壁纸毛玻璃，不是每帧对窗口遮挡物做卷积。缓存未就绪时直接返回。
 * Aqua 控件经 aqua_glass（aqua_draw.c）读 glass_pixels，另叠透视与抗锯齿。
 */
#ifndef DESHAB_GLASS_H
#define DESHAB_GLASS_H

#include "ascii_font.h"
#include "../UTSM/include/utsm/deshab_ui.h"

#define GLASS_ADDR   0x9100000ULL
#define GLASS_MAX    (8u * 1024u * 1024u)
#define GLASS_RADIUS 8

/* 0 成功；负值失败（无壁纸 / 超出 8MB）。失败不 panic。 */
int glass_init(int fb_w, int fb_h);
int glass_ready(void);
/* 模糊缓存（帧缓冲尺寸，0xFFRRGGBB）。未就绪返回 0。 */
const du_u32 *glass_pixels(int *w, int *h);

void du_glass_wallpaper(du_context *ctx, int x, int y, int w, int h,
                        int radius, du_u32 tint, du_u32 alpha);

#endif /* DESHAB_GLASS_H */
