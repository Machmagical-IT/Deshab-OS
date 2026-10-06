/* taskbar.c — Win11 Fluent 居中悬浮任务栏 + 系统托盘（M2）
 *
 * 布局（深色主题 g_wf_dark）：
 *   主任务栏：48px 高，距底 8px，水平居中悬浮，acrylic 圆角 12
 *     [开始 40×40] [任务视图 40×40] [运行窗口按钮 44×44 ...]
 *     运行中 = 底部 3×16 accent 指示条；聚焦 = 指示条加宽 6px + 微亮蒙层
 *   托盘组（右下角独立悬浮，同视觉）：
 *     [网络 40] [音量 40] [电源 40] [两行时钟：HH:MM / M-D]
 *
 * 点击路由 taskbar_click() 由 shell.c 在 Fluent 分支调用。
 * 开始菜单 M3 接入；电源 M2 直接退出（M7 扩展菜单）。
 */
#include "desktop.h"
#include "ascii_font.h"          /* 顺序敏感：先于 fluent_ui.h */
#include "../UTSM/include/utsm/fluent_ui.h"
#include "zhfont.h"
#include "textures.h"

/* ---- 命中矩形（绘制时更新，点击时使用） ---- */
int g_tb2_start_rect[4];
int g_tb2_tv_rect[4];
int g_tb2_app_rect[MAX_WINDOWS][4];
int g_tb2_tray_rect[4];          /* 托盘组整体 */
int g_tb2_net_rect[4];
int g_tb2_vol_rect[4];
int g_tb2_pwr_rect[4];
int g_tb2_clk_rect[4];

#define TB_H      48
#define TB_BOTTOM_GAP 8
#define TB_BTN    40             /* 开始/任务视图/托盘钮边长 */
#define TB_APPBTN 44             /* 应用钮边长 */
#define TB_PAD    8              /* 条内边距 */

static const wf_theme *tb_theme(void) { return &g_wf_light; }
static wf_btn_state tb_hover_state(const int r[4]);

/* ============================================================
 *  WiFi 快捷卡片（托盘网络图标弹出：扫描列表 → 密码 → 连接）
 *
 *  net API 经 g_kernel_api + 0x48（DSB_KAPI_OFF_NET）取得，
 *  dkm_net_api 槽位按 UTSM include/utsm/net.h 布局（每槽 8 字节）：
 *    0x00 register_device   0x08 device_count    0x10 device_info
 *    0x18 tx                0x20 rx_poll
 *    0x28 scan_start        0x30 scan_count
 *    0x38 scan_result       0x40 is_wireless
 *    0x48 connect           0x50 disconnect      0x58 device_stats
 * ============================================================ */

#define WIFI_KAPI_OFF_NET        0x48
#define WIFI_NETAPI_SCAN_START   0x28
#define WIFI_NETAPI_SCAN_COUNT   0x30
#define WIFI_NETAPI_SCAN_RESULT  0x38
#define WIFI_NETAPI_IS_WIRELESS  0x40
#define WIFI_NETAPI_CONNECT      0x48
#define WIFI_NETAPI_DEVICE_COUNT 0x08

#define WIFI_MAX_ROWS   8       /* 卡片内最多显示行数 */
#define WIFI_PW_MAX     63
#define WIFI_ROW_H      36

/* dkm_net_scan_result 镜像（布局必须与 UTSM net.h 一致，对齐 1 无填充） */
typedef struct wifi_scan_item {
    char ssid[33];
    u8 bssid[6];
    u8 channel;
    signed char rssi;
    u8 security;
} wifi_scan_item;

typedef int (*wifi_fn_scan_start)(u32);
typedef int (*wifi_fn_scan_count)(u32);
typedef int (*wifi_fn_scan_result)(u32, u32, wifi_scan_item *);
typedef int (*wifi_fn_is_wireless)(u32);
typedef int (*wifi_fn_connect)(u32, const char *, const char *);
typedef u32 (*wifi_fn_device_count)(void);

int g_wifi_open = 0;
static int g_wifi_mode;                 /* 0=列表 1=密码输入 */
static int g_wifi_dev = -1;             /* 无线设备索引，-1 = 无 */
static int g_wifi_scanning;
static int g_wifi_n;                    /* 缓存的扫描结果数 */
static int g_wifi_sel = -1;
static char g_wifi_sel_ssid[33];
static u8 g_wifi_sel_sec;
static char g_wifi_pw[WIFI_PW_MAX + 1];
static char g_wifi_status[64];
static wifi_scan_item g_wifi_items[32];

static int g_wifi_card_rect[4];
static int g_wifi_row_rect[WIFI_MAX_ROWS][4];
static int g_wifi_rescan_rect[4];
static int g_wifi_pwbox_rect[4];
static int g_wifi_ok_rect[4];
static int g_wifi_cancel_rect[4];

/* 取 net API 函数槽（kapi/netapi 均缺失时返回 0） */
static void *wifi_slot(u32 slot_off) {
    if (!g_kernel_api) return 0;
    u64 net = *(u64 *)(g_kernel_api + WIFI_KAPI_OFF_NET);
    if (!net) return 0;
    u64 fn = *(u64 *)(net + slot_off);
    return (void *)(unsigned long)fn;
}

/* 找无线设备；返回 0 = 已定位 g_wifi_dev */
static int wifi_dev_find(void) {
    wifi_fn_device_count cnt = (wifi_fn_device_count)wifi_slot(WIFI_NETAPI_DEVICE_COUNT);
    wifi_fn_is_wireless isw = (wifi_fn_is_wireless)wifi_slot(WIFI_NETAPI_IS_WIRELESS);
    g_wifi_dev = -1;
    if (!cnt || !isw) return -1;
    u32 n = cnt();
    for (u32 i = 0; i < n; i++) {
        if (isw(i) == 1) { g_wifi_dev = (int)i; return 0; }
    }
    return -1;
}

/* 拉取当前扫描结果到缓存 */
static void wifi_results_pull(void) {
    wifi_fn_scan_count scnt = (wifi_fn_scan_count)wifi_slot(WIFI_NETAPI_SCAN_COUNT);
    wifi_fn_scan_result sres = (wifi_fn_scan_result)wifi_slot(WIFI_NETAPI_SCAN_RESULT);
    if (!scnt || !sres || g_wifi_dev < 0) return;
    int n = scnt((u32)g_wifi_dev);
    if (n < 0) n = 0;
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++)
        sres((u32)g_wifi_dev, (u32)i, &g_wifi_items[i]);
    g_wifi_n = n;
}

/* 打开卡片并踢一脚扫描 */
static void wifi_card_open(void) {
    g_wifi_open = 1;
    g_wifi_mode = 0;
    g_wifi_sel = -1;
    g_wifi_pw[0] = 0;
    g_wifi_status[0] = 0;
    g_wifi_n = 0;
    g_wifi_scanning = 1;
    if (wifi_dev_find() != 0) {
        g_wifi_scanning = 0;
        const char *m = "未检测到无线网卡";
        int i = 0;
        for (; m[i]; i++) g_wifi_status[i] = m[i];
        g_wifi_status[i] = 0;
        slog("[wifi] no wireless device");
        return;
    }
    wifi_fn_scan_start ss = (wifi_fn_scan_start)wifi_slot(WIFI_NETAPI_SCAN_START);
    if (!ss || ss((u32)g_wifi_dev) != 0) {
        g_wifi_scanning = 0;
        const char *m = "扫描启动失败";
        int i = 0;
        for (; m[i]; i++) g_wifi_status[i] = m[i];
        g_wifi_status[i] = 0;
        slog("[wifi] scan_start failed");
    }
}

/* 发起连接（当前选中网络 + 密码） */
static void wifi_connect_go(void) {
    wifi_fn_connect conn = (wifi_fn_connect)wifi_slot(WIFI_NETAPI_CONNECT);
    if (!conn || g_wifi_dev < 0) return;
    const char *m = "连接中...";
    int i = 0;
    for (; m[i]; i++) g_wifi_status[i] = m[i];
    g_wifi_status[i] = 0;
    int rc = conn((u32)g_wifi_dev, g_wifi_sel_ssid, g_wifi_pw);
    if (rc == 0) {
        m = "已连接";
        g_wifi_mode = 0;
        g_wifi_open = 0;    /* 连接成功收起卡片 */
    } else {
        m = "连接失败";
    }
    for (i = 0; m[i]; i++) g_wifi_status[i] = m[i];
    g_wifi_status[i] = 0;
    {
        char buf[72];
        buf[0] = '['; buf[1] = 'w'; buf[2] = 'i'; buf[3] = 'f'; buf[4] = 'i'; buf[5] = ']'; buf[6] = ' ';
        int p = 7;
        for (i = 0; g_wifi_sel_ssid[i] && p < 60; i++) buf[p++] = g_wifi_sel_ssid[i];
        buf[p++] = ' '; buf[p++] = rc == 0 ? 'o' : 'f'; buf[p++] = ' '; buf[p++] = '0' + (rc == 0 ? 0 : 1); buf[p] = 0;
        slog(buf);
    }
    g_wifi_pw[0] = 0;
}

/* 卡片绘制（taskbar_draw 末尾调用；每次绘制顺带收割扫描结果） */
static void wifi_draw(void) {
    if (!g_wifi_open) return;
    const wf_theme *t = tb_theme();
    int W = (int)g_fb_w;

    /* 扫描收割：scanning 时每帧查 count，结果稳定（>0）即认为出网 */
    if (g_wifi_scanning && g_wifi_dev >= 0) {
        wifi_results_pull();
        if (g_wifi_n > 0) g_wifi_scanning = 0;
    }

    const int CW = 300;
    int rows = g_wifi_n;
    if (rows > WIFI_MAX_ROWS) rows = WIFI_MAX_ROWS;
    int ch;
    if (g_wifi_mode == 1) ch = 46 + 30 + 48 + 44 + 26 + 10;
    else ch = 46 + (rows ? rows * WIFI_ROW_H + 6 : 34) + 34 + 10;
    int cx = g_tb2_net_rect[0] + g_tb2_net_rect[2] - CW;
    if (cx < 8) cx = 8;
    if (cx + CW > W - 8) cx = W - 8 - CW;
    int cy = (int)g_fb_h - TB_H - TB_BOTTOM_GAP - ch - 8;

    g_wifi_card_rect[0] = cx; g_wifi_card_rect[1] = cy;
    g_wifi_card_rect[2] = CW; g_wifi_card_rect[3] = ch;
    du_wf_card(&g_fb, cx, cy, CW, ch, 235, t);

    /* 标题行 */
    zh_draw_string(&g_fb, "WLAN", cx + 14, cy + 12, t->text_primary, 0);
    /* 重新扫描按钮（右上） */
    int rx = cx + CW - 76, ry = cy + 8;
    g_wifi_rescan_rect[0] = rx; g_wifi_rescan_rect[1] = ry;
    g_wifi_rescan_rect[2] = 64; g_wifi_rescan_rect[3] = 32;
    du_wf_button(&g_fb, rx, ry, 64, "Scan", t, tb_hover_state(g_wifi_rescan_rect), 0);

    int y = cy + 46;

    if (g_wifi_mode == 1) {
        /* ---- 密码模式 ---- */
        zh_draw_string(&g_fb, g_wifi_sel_ssid, cx + 14, y, t->text_primary, 0);
        y += 30;
        g_wifi_pwbox_rect[0] = cx + 12; g_wifi_pwbox_rect[1] = y;
        g_wifi_pwbox_rect[2] = CW - 24; g_wifi_pwbox_rect[3] = 36;
        du_wf_textbox(&g_fb, cx + 12, y, CW - 24, g_wifi_pw, 1, t, 1);
        y += 48;
        int bw = (CW - 24 - 8) / 2;
        g_wifi_cancel_rect[0] = cx + 12; g_wifi_cancel_rect[1] = y;
        g_wifi_cancel_rect[2] = bw; g_wifi_cancel_rect[3] = 32;
        du_wf_button(&g_fb, cx + 12, y, bw, "Cancel", t,
                     tb_hover_state(g_wifi_cancel_rect), 0);
        g_wifi_ok_rect[0] = cx + 12 + bw + 8; g_wifi_ok_rect[1] = y;
        g_wifi_ok_rect[2] = bw; g_wifi_ok_rect[3] = 32;
        du_wf_button(&g_fb, cx + 12 + bw + 8, y, bw, "OK", t,
                     tb_hover_state(g_wifi_ok_rect), 1);
        y += 42;
    } else {
        /* ---- 列表模式 ---- */
        if (g_wifi_dev < 0) {
            zh_draw_string(&g_fb, g_wifi_status[0] ? g_wifi_status : "无无线设备",
                           cx + 14, y + 8, t->text_dim, 0);
            y += 34;
        } else if (g_wifi_scanning && g_wifi_n == 0) {
            zh_draw_string(&g_fb, "正在扫描...", cx + 14, y + 8, t->text_dim, 0);
            y += 34;
        } else if (g_wifi_n == 0) {
            zh_draw_string(&g_fb, "未发现网络", cx + 14, y + 8, t->text_dim, 0);
            y += 34;
        } else {
            for (int i = 0; i < rows; i++) {
                g_wifi_row_rect[i][0] = cx + 4; g_wifi_row_rect[i][1] = y;
                g_wifi_row_rect[i][2] = CW - 8; g_wifi_row_rect[i][3] = WIFI_ROW_H;
                int sel = (g_wifi_sel == i);
                du_wf_list_item(&g_fb, cx + 4, y, CW - 8,
                                g_wifi_items[i].ssid, t,
                                tb_hover_state(g_wifi_row_rect[i]) == WF_BTN_HOVER, sel);
                /* 右侧：加密标记 + 信号强度 */
                {
                    char tag[16];
                    int p = 0;
                    if (g_wifi_items[i].security != 0u) tag[p++] = '*';
                    int r = g_wifi_items[i].rssi;
                    int bars = (r >= -55) ? 3 : (r >= -70) ? 2 : 1;
                    tag[p++] = (char)('0' + bars);
                    tag[p] = 0;
                    du_draw_string(&g_fb, tag, cx + CW - 46, y + 10,
                                   t->text_secondary, 0, DU_ASCII_STEP);
                }
                y += WIFI_ROW_H;
            }
            y += 6;
        }
    }

    /* 状态行 */
    if (g_wifi_status[0])
        zh_draw_string(&g_fb, g_wifi_status, cx + 14, y + 2, t->accent, 0);
}

/* 卡片点击（taskbar_click 中调用；返回 1 = 已消费） */
static int wifi_click(int mx, int my) {
    if (!g_wifi_open) return 0;
    if (!rect_hit(g_wifi_card_rect, mx, my)) {
        g_wifi_open = 0;                /* 点外部收起 */
        return 0;
    }
    /* 重新扫描 */
    if (rect_hit(g_wifi_rescan_rect, mx, my)) {
        wifi_card_open();
        return 1;
    }
    if (g_wifi_mode == 1) {
        if (rect_hit(g_wifi_ok_rect, mx, my)) { wifi_connect_go(); return 1; }
        if (rect_hit(g_wifi_cancel_rect, mx, my)) {
            g_wifi_mode = 0;
            g_wifi_pw[0] = 0;
            return 1;
        }
        return 1;                       /* 卡片内其他点击吞掉 */
    }
    /* 列表行 */
    int rows = g_wifi_n;
    if (rows > WIFI_MAX_ROWS) rows = WIFI_MAX_ROWS;
    for (int i = 0; i < rows; i++) {
        if (!rect_hit(g_wifi_row_rect[i], mx, my)) continue;
        g_wifi_sel = i;
        int p = 0;
        for (; p < 32 && g_wifi_items[i].ssid[p]; p++)
            g_wifi_sel_ssid[p] = g_wifi_items[i].ssid[p];
        g_wifi_sel_ssid[p] = 0;
        g_wifi_sel_sec = g_wifi_items[i].security;
        if (g_wifi_sel_sec == 0u) {
            g_wifi_pw[0] = 0;
            wifi_connect_go();          /* 开放网络直连 */
        } else {
            g_wifi_mode = 1;            /* 加密网络进密码输入 */
            g_wifi_pw[0] = 0;
            g_wifi_status[0] = 0;
        }
        return 1;
    }
    return 1;
}

/* 卡片键盘输入（shell.c handle_key 中调用；返回 1 = 已消费） */
int wifi_key(u8 sc, int shift) {
    if (!g_wifi_open) return 0;
    if (sc == 0x01) { g_wifi_open = 0; return 1; }        /* Esc 收起 */
    if (g_wifi_mode != 1) return 0;
    if (sc == 0x0E) {                                     /* Backspace */
        if (g_wifi_pw[0]) {
            int l = 0;
            while (g_wifi_pw[l]) l++;
            g_wifi_pw[l - 1] = 0;
        }
        return 1;
    }
    if (sc == 0x1C) { wifi_connect_go(); return 1; }      /* Enter 连接 */
    if (sc >= 58) return 0;
    {
        static const char normal[58] = {
            0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, '\t',
            'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
            'a','s','d','f','g','h','j','k','l',';','\'', '`',0,'\\',
            'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
        };
        static const char shifted[58] = {
            0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, '\t',
            'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
            'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
            'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
        };
        char c = shift ? shifted[sc] : normal[sc];
        if (c && c != '\n' && c != 27 && c != 8) {
            int l = 0;
            while (g_wifi_pw[l]) l++;
            if (l < WIFI_PW_MAX) {
                g_wifi_pw[l] = c;
                g_wifi_pw[l + 1] = 0;
            }
        }
    }
    return 1;
}

/* ---- 开始菜单 logo（textures/startMenuLogo.rgba，加载失败回退字形） ---- */
static int g_smlogo_ok = 0;

typedef int (*tb_read_fn)(const char *path, unsigned char *dst,
                          unsigned int cap, unsigned int *out_size);

int taskbar_init(tb_read_fn read_to) {
    u32 sz = 0;
    if (!read_to) return -1;
    if (read_to("system/deshab64/desktop/textures/startMenuLogo.rgba",
                (u8 *)SMLOGO_ADDR, SMLOGO_W * SMLOGO_H * 4u, &sz) != 0)
        return -2;
    if (sz < SMLOGO_W * SMLOGO_H * 4u) return -3;
    g_smlogo_ok = 1;
    return 0;
}

/* app_id → Fluent 图标 */
static wf_icon_id tb_app_icon(int app_id) {
    if (app_id < 0 || app_id >= g_app_count) return WF_ICON_TERMINAL;
    const char *n = g_apps[app_id].name;
    if (!n) return WF_ICON_FOLDER;
    if (n[0]=='s' && n[1]=='h') return WF_ICON_TERMINAL;      /* shell */
    if (n[0]=='c' && n[1]=='m' && n[2]=='d') return WF_ICON_TERMINAL;
    if (n[0]=='e' && n[1]=='d') return WF_ICON_EDITOR;        /* editor */
    if (n[0]=='p' && n[1]=='r') return WF_ICON_EDITOR;        /* proedit */
    if (n[0]=='f' && n[1]=='i') return WF_ICON_FOLDER;        /* fileman */
    if (n[0]=='c' && n[1]=='a') return WF_ICON_CALC;          /* calc */
    if (n[0]=='b' && n[1]=='r') return WF_ICON_BROWSER;       /* browser */
    if (n[0]=='s' && n[1]=='e') return WF_ICON_SETTINGS;      /* settings */
    if (n[0]=='v' || n[0]=='k') return WF_ICON_BROWSER;       /* vscode/kde */
    return WF_ICON_FOLDER;
}

static wf_btn_state tb_hover_state(const int r[4]) {
    if (g_mouse_x >= r[0] && g_mouse_x < r[0] + r[2] &&
        g_mouse_y >= r[1] && g_mouse_y < r[1] + r[3])
        return WF_BTN_HOVER;
    return WF_BTN_NORMAL;
}

/* ============================================================
 *  绘制
 * ============================================================ */

void taskbar_draw(void) {
    const wf_theme *t = tb_theme();
    int W = (int)g_fb_w, H = (int)g_fb_h;
    int by = H - TB_H - TB_BOTTOM_GAP;

    /* ---- 主任务栏内容宽度 ---- */
    int nbtn = 0;
    for (int i = 0; i < g_win_count; i++)
        if (g_windows[i].visible && g_windows[i].ws == g_ws_cur) nbtn++;
    int content_w = TB_PAD + TB_BTN + 4 + TB_BTN + 6 + nbtn * (TB_APPBTN + 4) + TB_PAD - 4;
    int bx = (W - content_w) / 2;

    /* ---- 托盘组宽度 ---- */
    const char *time_s = "00:00";
    const char *date_s = "0/0";
    char tbuf[8], dbuf[10];
    {
        int yy, mo, dd, hh, mi, ss;
        rtc_read(&yy, &mo, &dd, &hh, &mi, &ss);
        tbuf[0] = '0' + hh / 10; tbuf[1] = '0' + hh % 10; tbuf[2] = ':';
        tbuf[3] = '0' + mi / 10; tbuf[4] = '0' + mi % 10; tbuf[5] = 0;
        time_s = tbuf;
        int dl = 0;
        if (mo >= 10) dbuf[dl++] = '0' + mo / 10;
        dbuf[dl++] = '0' + mo % 10; dbuf[dl++] = '/';
        if (dd >= 10) dbuf[dl++] = '0' + dd / 10;
        dbuf[dl++] = '0' + dd % 10; dbuf[dl] = 0;
        date_s = dbuf;
    }
    int clk_w = zh_string_width(time_s, 0) + 8;
    int tray_w = TB_PAD + TB_BTN + 2 + TB_BTN + 2 + TB_BTN + 6 + clk_w + TB_PAD;
    int tx = W - tray_w - 8;

    /* ---- 主任务栏面板 ---- */
    du_wf_acrylic(&g_fb, bx, by, content_w, TB_H, 216, t->radius_lg, t);
    int x = bx + TB_PAD;

    /* 开始按钮（logo：用户 textures/startMenuLogo，缺失回退 "D" 字形） */
    g_tb2_start_rect[0] = x; g_tb2_start_rect[1] = by + (TB_H - TB_BTN) / 2;
    g_tb2_start_rect[2] = TB_BTN; g_tb2_start_rect[3] = TB_BTN;
    du_wf_icon_button(&g_fb, x, by + (TB_H - TB_BTN) / 2, TB_BTN, t,
                      tb_hover_state(g_tb2_start_rect));
    if (g_smlogo_ok) {
        tex_draw_scaled(&g_fb, (const u8 *)SMLOGO_ADDR, SMLOGO_W, SMLOGO_H,
                        x + 6, by + (TB_H - TB_BTN) / 2 + 6, 28, 28, 256);
    } else {
        du_draw_string(&g_fb, "D", x + TB_BTN / 2 - 4,
                       by + (TB_H - TB_BTN) / 2 + TB_BTN / 2 - 8,
                       t->accent, 0, DU_ASCII_STEP * 2);
    }
    x += TB_BTN + 4;

    /* 任务视图按钮 */
    g_tb2_tv_rect[0] = x; g_tb2_tv_rect[1] = by + (TB_H - TB_BTN) / 2;
    g_tb2_tv_rect[2] = TB_BTN; g_tb2_tv_rect[3] = TB_BTN;
    du_wf_icon_button(&g_fb, x, by + (TB_H - TB_BTN) / 2, TB_BTN, t,
                      tb_hover_state(g_tb2_tv_rect));
    du_wf_icon(&g_fb, WF_ICON_TASKVIEW, x + (TB_BTN - 20) / 2,
               by + (TB_H - TB_BTN) / 2 + (TB_BTN - 20) / 2, 20, t->text_primary);
    x += TB_BTN + 6;

    /* 运行窗口按钮（当前工作区，数组序 = z 序） */
    for (int i = 0; i < g_win_count; i++) {
        desktop_window *w = &g_windows[i];
        if (!w->visible || w->ws != g_ws_cur) {
            g_tb2_app_rect[i][2] = 0;
            continue;
        }
        int byy = by + (TB_H - TB_APPBTN) / 2;
        g_tb2_app_rect[i][0] = x; g_tb2_app_rect[i][1] = byy;
        g_tb2_app_rect[i][2] = TB_APPBTN; g_tb2_app_rect[i][3] = TB_APPBTN;
        wf_btn_state st = tb_hover_state(g_tb2_app_rect[i]);
        if (w->focused) du_wf_overlay(&g_fb, x, byy, TB_APPBTN, TB_APPBTN, 0x22000000u);
        else if (st == WF_BTN_HOVER) du_wf_overlay(&g_fb, x, byy, TB_APPBTN, TB_APPBTN, 0x22000000u);
        du_wf_icon(&g_fb, tb_app_icon(w->app_id), x + (TB_APPBTN - 22) / 2,
                   byy + (TB_APPBTN - 22) / 2, 22, t->text_primary);
        /* 运行指示条：底部 accent，聚焦加宽 */
        int ind_w = w->focused ? 18 : 12;
        du_fill_rounded_rect(&g_fb, x + (TB_APPBTN - ind_w) / 2,
                             byy + TB_APPBTN - 6, ind_w, 3, t->accent, 1);
        x += TB_APPBTN + 4;
    }

    /* ---- 托盘组面板 ---- */
    du_wf_acrylic(&g_fb, tx, by, tray_w, TB_H, 216, t->radius_lg, t);
    x = tx + TB_PAD;
    int iy = by + (TB_H - TB_BTN) / 2;

    g_tb2_net_rect[0] = x; g_tb2_net_rect[1] = iy;
    g_tb2_net_rect[2] = TB_BTN; g_tb2_net_rect[3] = TB_BTN;
    du_wf_icon_button(&g_fb, x, iy, TB_BTN, t, tb_hover_state(g_tb2_net_rect));
    du_wf_icon(&g_fb, WF_ICON_NETWORK, x + (TB_BTN - 20) / 2, iy + (TB_BTN - 20) / 2, 20, t->text_primary);
    x += TB_BTN + 2;

    g_tb2_vol_rect[0] = x; g_tb2_vol_rect[1] = iy;
    g_tb2_vol_rect[2] = TB_BTN; g_tb2_vol_rect[3] = TB_BTN;
    du_wf_icon_button(&g_fb, x, iy, TB_BTN, t, tb_hover_state(g_tb2_vol_rect));
    du_wf_icon(&g_fb, WF_ICON_VOLUME, x + (TB_BTN - 20) / 2, iy + (TB_BTN - 20) / 2, 20, t->text_primary);
    x += TB_BTN + 2;

    g_tb2_pwr_rect[0] = x; g_tb2_pwr_rect[1] = iy;
    g_tb2_pwr_rect[2] = TB_BTN; g_tb2_pwr_rect[3] = TB_BTN;
    du_wf_icon_button(&g_fb, x, iy, TB_BTN, t, tb_hover_state(g_tb2_pwr_rect));
    du_wf_icon(&g_fb, WF_ICON_POWER, x + (TB_BTN - 20) / 2, iy + (TB_BTN - 20) / 2, 20, t->text_primary);
    x += TB_BTN + 6;

    /* 两行时钟（16px 中文位图，行高 18） */
    g_tb2_clk_rect[0] = x; g_tb2_clk_rect[1] = by;
    g_tb2_clk_rect[2] = clk_w; g_tb2_clk_rect[3] = TB_H;
    {
        int ty1 = by + (TB_H - 36) / 2;
        zh_draw_string(&g_fb, time_s, x, ty1, t->text_primary, 0);
        zh_draw_string(&g_fb, date_s, x, ty1 + 18, t->text_dim, 0);
    }
    g_tb2_tray_rect[0] = tx; g_tb2_tray_rect[1] = by;
    g_tb2_tray_rect[2] = tray_w; g_tb2_tray_rect[3] = TB_H;

    /* WiFi 快捷卡片（最顶） */
    wifi_draw();
}

/* ============================================================
 *  点击路由（shell.c Fluent 分支调用；返回 1 = 已消费）
 * ============================================================ */

int taskbar_click(int mx, int my) {
    /* 主任务栏 */
    if (rect_hit(g_tb2_start_rect, mx, my)) {
        slog("[taskbar] start button (start menu: M3)");
        return 1;
    }
    if (rect_hit(g_tb2_tv_rect, mx, my)) {
        g_taskview = 1;
        return 1;
    }
    for (int i = 0; i < g_win_count; i++) {
        if (!g_tb2_app_rect[i][2] || !rect_hit(g_tb2_app_rect[i], mx, my))
            continue;
        /* 聚焦窗口；已聚焦则最小化（Win11 行为） */
        if (g_windows[i].focused && !g_windows[i].minimized) {
            g_windows[i].minimized = 1;
            g_focused_win = -1;
            g_windows[i].focused = 0;
        } else {
            g_windows[i].minimized = 0;
            win_focus(&g_windows[i]);
        }
        return 1;
    }
    /* WiFi 卡片（打开时优先消费；点外部收起并放行点击） */
    if (g_wifi_open) wifi_click(mx, my);

    /* 托盘 */
    if (rect_hit(g_tb2_pwr_rect, mx, my)) {
        g_quit = 1;                       /* M7：电源菜单（睡眠/重启/关机） */
        return 1;
    }
    if (rect_hit(g_tb2_net_rect, mx, my)) {
        /* 网络：弹出/收起 WiFi 快捷卡片（扫描 → 选网 → 密码 → 连接） */
        if (g_wifi_open) g_wifi_open = 0;
        else wifi_card_open();
        return 1;
    }
    if (rect_hit(g_tb2_vol_rect, mx, my)) {
        slog("[taskbar] volume (quick settings: M6)");
        return 1;
    }
    if (rect_hit(g_tb2_clk_rect, mx, my)) {
        slog("[taskbar] clock (notification center: M6)");
        return 1;
    }
    /* 命中面板空白处：吞掉点击（不落到桌面） */
    if (rect_hit(g_tb2_tray_rect, mx, my)) return 1;
    {
        int W = (int)g_fb_w, H = (int)g_fb_h;
        int by = H - TB_H - TB_BOTTOM_GAP;
        if (my >= by && my < by + TB_H) {
            int nbtn = 0;
            for (int i = 0; i < g_win_count; i++)
                if (g_windows[i].visible && g_windows[i].ws == g_ws_cur) nbtn++;
            int content_w = TB_PAD + TB_BTN + 4 + TB_BTN + 6 + nbtn * (TB_APPBTN + 4) + TB_PAD - 4;
            int bx = (W - content_w) / 2;
            if (mx >= bx && mx < bx + content_w) return 1;
        }
    }
    return 0;
}
