/* aqua_draw.h — macOS 外壳绘制原语（整数、无 SSE）
 *
 * 圆角覆盖率：角区像素 4x4 超采样，直边像素直接满覆盖。
 * 透明度参数一律 0..255；opacity（整体淡入淡出）0..256。
 *
 * 应用图标：每个 app_id 首次绘制时在 96x96 母版上渲染圆角方块 + 竖直渐变
 * + 白色 du_wf_icon 线性图标，同时盒式降采样出 48x48 一级 mip；之后只做
 * du_blit_scaled_alpha。Dock 放大、Launchpad 缩放都走这条路径。
 */
#ifndef DESHAB_AQUA_DRAW_H
#define DESHAB_AQUA_DRAW_H

#include "ascii_font.h"
#include "../UTSM/include/utsm/fluent_ui.h"
#include "aqua_theme.h"

/* Aqua 控件主题：系统蓝强调色，浅色材质，圆角 6/10/12。 */
static const wf_theme g_wf_aqua = {
    .acrylic_tint   = 0xFFF5F5F7u,
    .acrylic_stroke = 0xFFD8D8DCu,
    .acrylic_solid  = 0xFFFFFFFFu,
    .text_primary   = 0xFF1D1D1Fu,
    .text_secondary = 0xFF6E6E73u,
    .text_dim       = 0xFF8E8E93u,
    .accent         = 0xFF007AFFu,
    .accent_hover   = 0xFF0A84FFu,
    .accent_press   = 0xFF0062CCu,
    .on_accent      = 0xFFFFFFFFu,
    .control_stroke = 0xFFD1D1D6u,
    .window_border  = 0xFFD1D1D6u,
    .divider        = 0xFFE5E5EAu,
    .shadow         = 0x55000000u,
    .success        = 0xFF34C759u,
    .warning        = 0xFFFF9F0Au,
    .error          = 0xFFFF3B30u,
    .close_hover    = 0xFFFF5F57u,
    .radius_sm      = 6,
    .radius_md      = 10,
    .radius_lg      = 12,
};

/* 特殊图标槽（不对应 g_apps） */
#define AQUA_ICON_LAUNCHPAD (-2)
#define AQUA_ICON_MISSION   (-3)
#define AQUA_ICON_LINUX     (-4)
#define AQUA_ICON_WINDOWS   (-5)

/* 圆角矩形覆盖率 0..16（col,row 相对矩形左上角） */
int  aqua_round_cov(int col, int row, int w, int h, int r);

void aqua_fill_round_alpha(du_context *ctx, int x, int y, int w, int h,
                           int r, du_u32 color, int alpha);
/* 只画 [row0,row1) 行（相对矩形顶端），角区照常抗锯齿 */
void aqua_fill_round_rows(du_context *ctx, int x, int y, int w, int h,
                          int r, du_u32 color, int alpha, int row0, int row1);
/* 1px 抗锯齿圆角描边 */
void aqua_round_band(du_context *ctx, int x, int y, int w, int h,
                     int r, du_u32 color, int alpha);
/* 下偏 3px 的多层柔和投影；strength 0..255 */
void aqua_shadow(du_context *ctx, int x, int y, int w, int h, int r,
                 int strength, int layers);
/* d x d 外接框内切圆 */
void aqua_disc(du_context *ctx, int x, int y, int d, du_u32 color, int alpha);

/* 毛玻璃：模糊壁纸与当前像素按 see（0..256）混合，再叠 tint（tint_a 0..255），
 * 最后按 opacity（0..256）与原像素混合。模糊缓存缺失时只用当前像素。 */
void aqua_glass(du_context *ctx, int x, int y, int w, int h, int r,
                du_u32 tint, int tint_a, int see, int opacity);
/* 同上，tint 从左 tint0 到右 tint1 水平渐变 */
void aqua_glass_hgrad(du_context *ctx, int x, int y, int w, int h, int r,
                      du_u32 tint0, du_u32 tint1, int tint_a, int see, int opacity);

/* 弹出面板：投影 + 白色毛玻璃 + 描边（菜单、卡片、右键菜单共用） */
void aqua_panel(du_context *ctx, int x, int y, int w, int h, int r);

/* app_id 为 g_apps 下标或 AQUA_ICON_*；alpha 0..256 */
void aqua_app_icon(du_context *ctx, int app_id, int x, int y, int size,
                   int alpha);

/* du_wf_icon 线性图标的抗锯齿任意尺寸版：48px 母版按 (id,color) 缓存后缩放 */
void aqua_glyph(du_context *ctx, int id, int x, int y, int size, du_u32 color);

/* 两色按 t（0..256）混合，alpha 通道取 0xFF */
du_u32 aqua_mix(du_u32 a, du_u32 b, int t);

#endif /* DESHAB_AQUA_DRAW_H */
