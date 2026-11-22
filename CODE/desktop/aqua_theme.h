/* aqua_theme.h — macOS 外壳令牌。只有颜色和度量，没有绘制函数。
 *
 * g_fluent=1 时整套桌面外壳（顶栏 / Dock / Launchpad / 窗口框）读这张表；
 * g_fluent=0 是 KATE 回退路径，不读它。
 */
#ifndef DESHAB_AQUA_THEME_H
#define DESHAB_AQUA_THEME_H

typedef struct aqua_theme {
    unsigned menu_bar;          /* 顶栏 tint */
    unsigned dock;              /* Dock tint */
    unsigned window;            /* 窗口底 */
    unsigned text_primary;
    unsigned text_secondary;
    unsigned traffic_close;     /* 红 */
    unsigned traffic_min;       /* 黄 */
    unsigned traffic_zoom;      /* 绿 */
    int radius_window;
    int radius_dock;
    int menu_bar_h;             /* 24 */
    int dock_h;                 /* 64 */
    int dock_icon;              /* 48 */
    int traffic;                /* 直径 */
    int traffic_gap;
    int traffic_inset_x;
    int traffic_inset_y;
} aqua_theme;

static const aqua_theme g_aqua_theme = {
    0xF2F6F6F6u,
    0xCCF5F5F7u,
    0xFFF6F6F6u,
    0xFF1D1D1Fu,
    0xFF6E6E73u,
    0xFFFF5F57u,
    0xFFFEBC2Eu,
    0xFF28C840u,
    12,
    18,
    24,
    64,
    48,
    12,
    8,
    12,
    8,
};

/* 编译期常量版本（数组尺寸 / 布局宏用）。须与上表一致。 */
#define AQUA_MENUBAR_H     24
#define AQUA_TITLEBAR_H    28
#define AQUA_DOCK_ICON     48
#define AQUA_DOCK_ICON_MAX 72
#define AQUA_DOCK_BOTTOM   6
#define AQUA_DOCK_PAD_T    8
#define AQUA_DOCK_PAD_B    10
#define AQUA_DOCK_PANEL_H  (AQUA_DOCK_PAD_T + AQUA_DOCK_ICON + AQUA_DOCK_PAD_B)
/* 窗口缩放 / 新窗口放置时给 Dock 让出的底部高度 */
#define AQUA_DOCK_RESERVE  (AQUA_DOCK_PANEL_H + AQUA_DOCK_BOTTOM + 8)

#endif /* DESHAB_AQUA_THEME_H */
