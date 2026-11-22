/* blit.h — 帧缓冲矩形的定点缩放 + src-over
 *
 * 源是 0xAARRGGBB，8.8 双线性，再乘调用方 alpha（0..256，256=只用源 alpha）。
 * 放在桌面驱动内，不进 deshab_ui.h。
 */
#ifndef DESHAB_BLIT_H
#define DESHAB_BLIT_H

#include "ascii_font.h"
#include "../UTSM/include/utsm/deshab_ui.h"

void du_blit_scaled_alpha(du_context *ctx,
                          const du_u32 *src, int src_stride,
                          int sw, int sh,
                          int x, int y, int w, int h,
                          int alpha);

#endif /* DESHAB_BLIT_H */
