/* taskbar.c — Aqua 顶栏弹出卡片 + Apple logo 纹理
 *
 * 原 Win11 底部任务栏已删除，外壳改为 menubar.c（顶栏）+ dock.c。
 * 这里保留三张卡片，从顶栏图标下方弹出：
 *   Wi-Fi 卡片   锚点 g_tb2_net_rect（顶栏 Wi-Fi 图标），右对齐
 *   控制中心     锚点 g_tb2_vol_rect（顶栏控制中心图标），音量/亮度滑块
 *   关机确认     锚点 g_tb2_pwr_rect（Apple logo），左对齐
 * 锚点矩形由 menubar_draw 每帧写入。
 */
#include "desktop.h"
#include "aqua_draw.h"
#include "zhfont.h"
#include "textures.h"

/* ---- 卡片锚点（顶栏绘制时更新） ---- */
int g_tb2_net_rect[4];
int g_tb2_vol_rect[4];
int g_tb2_pwr_rect[4];

#define CARD_TOP (AQUA_MENUBAR_H + 6)

static const wf_theme *tb_theme(void) { return &g_wf_aqua; }
static wf_btn_state tb_hover_state(const int r[4]);

/* ============================================================
 *  WiFi 快捷卡片（顶栏 Wi-Fi 图标弹出：扫描列表 → 密码 → 连接）
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
#define WIFI_NETAPI_DEVICE_INFO  0x10
#define TRAY_NET_LINK_UP         (1u << 0)
#define TRAY_NET_WIRELESS        (1u << 3)

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
static int g_wifi_guest;                /* 1 = 结果来自 Linux 客户机 iw，不是原生网卡 */
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

/* 与 dkm_net_device_info 同布局：name 指针、6 字节 MAC、对齐后的 flags。 */
typedef struct tray_netdev {
    const char *name;
    u8 mac[6];
    u32 flags;
} tray_netdev;
typedef int (*tray_info_fn)(u32 index, tray_netdev *out);

static int g_tray_n, g_tray_up, g_tray_wl;

/* 扫已注册网卡。返回 0 = 计数有效。链路起来的有线口在 QEMU e1000 上应为 1。 */
static int tray_net_query(void) {
    wifi_fn_device_count cnt = (wifi_fn_device_count)wifi_slot(WIFI_NETAPI_DEVICE_COUNT);
    tray_info_fn info = (tray_info_fn)wifi_slot(WIFI_NETAPI_DEVICE_INFO);
    if (!cnt) return -1;
    u32 n = cnt();
    if (n > 8) n = 8;
    int up = 0, wl = 0;
    for (u32 i = 0; i < n; i++) {
        tray_netdev d;
        d.flags = 0;
        if (info && info(i, &d) == 0) {
            if (d.flags & TRAY_NET_LINK_UP) up++;
            if (d.flags & TRAY_NET_WIRELESS) wl++;
        }
    }
    if (n != (u32)g_tray_n || up != g_tray_up || wl != g_tray_wl) {
        g_tray_n = (int)n;
        g_tray_up = up;
        g_tray_wl = wl;
        slog_num("[tray] net devices=", g_tray_n);
        slog_num("[tray] net link_up=", g_tray_up);
        slog_num("[tray] net wireless=", g_tray_wl);
    }
    return 0;
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

static void wifi_status_set(const char *m) {
    int i = 0;
    for (; m[i] && i < 63; i++) g_wifi_status[i] = m[i];
    g_wifi_status[i] = 0;
}

static int wifi_line_eq_prefix(const char *s, const char *p) {
    while (*p) {
        if (*s++ != *p++) return 0;
    }
    return 1;
}

static int wifi_parse_i(const char *s, int *out) {
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    if (*s < '0' || *s > '9') return -1;
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    *out = v * sign;
    return 0;
}

static int wifi_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 把一行 iw scan 文本并进当前 BSS。返回 1 表示遇到新的 BSS 头。 */
static int g_guest_have;
static char g_guest_ssid[33];
static u8 g_guest_bssid[6];
static u8 g_guest_ch;
static signed char g_guest_rssi;
static u8 g_guest_sec;

static void wifi_guest_commit(void) {
    if (!g_guest_have || !g_guest_ssid[0] || g_wifi_n >= 32) {
        g_guest_have = 0;
        return;
    }
    wifi_scan_item *it = &g_wifi_items[g_wifi_n];
    int i = 0;
    for (; i < 32 && g_guest_ssid[i]; i++) it->ssid[i] = g_guest_ssid[i];
    it->ssid[i] = 0;
    for (i = 0; i < 6; i++) it->bssid[i] = g_guest_bssid[i];
    it->channel = g_guest_ch;
    it->rssi = g_guest_rssi;
    it->security = g_guest_sec;
    g_wifi_n++;
    g_guest_have = 0;
}

static void wifi_guest_line(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (wifi_line_eq_prefix(s, "BSS ")) {
        wifi_guest_commit();
        g_guest_have = 1;
        g_guest_ssid[0] = 0;
        g_guest_ch = 0;
        g_guest_rssi = -90;
        g_guest_sec = 0;
        for (int i = 0; i < 6; i++) g_guest_bssid[i] = 0;
        const char *p = s + 4;
        for (int i = 0; i < 6; i++) {
            int hi = wifi_hex_nibble(p[0]);
            int lo = wifi_hex_nibble(p[1]);
            if (hi < 0 || lo < 0) { g_guest_have = 0; return; }
            g_guest_bssid[i] = (u8)((hi << 4) | lo);
            p += 2;
            if (i < 5) {
                if (*p != ':') { g_guest_have = 0; return; }
                p++;
            }
        }
        return;
    }
    if (!g_guest_have) return;
    if (wifi_line_eq_prefix(s, "SSID:")) {
        s += 5;
        while (*s == ' ') s++;
        int i = 0;
        for (; s[i] && s[i] != '\n' && i < 32; i++) g_guest_ssid[i] = s[i];
        g_guest_ssid[i] = 0;
        return;
    }
    if (wifi_line_eq_prefix(s, "signal:")) {
        s += 7;
        while (*s == ' ') s++;
        int dbm = 0;
        if (wifi_parse_i(s, &dbm) == 0) {
            if (dbm < -128) dbm = -128;
            if (dbm > 127) dbm = 127;
            g_guest_rssi = (signed char)dbm;
        }
        return;
    }
    if (wifi_line_eq_prefix(s, "freq:")) {
        s += 5;
        while (*s == ' ') s++;
        int freq = 0;
        if (wifi_parse_i(s, &freq) == 0) {
            int ch = 0;
            if (freq >= 2412 && freq <= 2472) ch = (freq - 2407) / 5;
            else if (freq >= 5180 && freq <= 5885) ch = (freq - 5000) / 5;
            if (ch > 0 && ch < 256) g_guest_ch = (u8)ch;
        }
        return;
    }
    if (wifi_line_eq_prefix(s, "RSN:")) g_guest_sec = 3;          /* WPA2 */
    else if (wifi_line_eq_prefix(s, "WPA:") && g_guest_sec == 0) g_guest_sec = 2;
}

/* 客户机里跑 iw。没有 wlan0 或客户机没起来时返回负值，不填假热点。 */
static int wifi_guest_scan(void) {
    g_wifi_guest = 0;
    g_wifi_n = 0;
    g_guest_have = 0;
    if (!g_lxc_svc || !g_lxc_svc->is_available)
        return -1;
    if (!g_lxc_svc->is_available()) return -1;

    /* guest 启动 wrapper 已用最小 nl80211 工具（/usr/local/bin/deshab_ap，零
     * OpenSSL）在 wlan1 起了开放 AP "Deshab-Net"，并执行 `iw dev wlan0 scan`
     * 把结果写入 /tmp/deshab-wifi.txt。这里经 lxc file_read 取回并解析。 */
    if (!g_lxc_svc->file_read) return -1;

    static char chunk[2048];
    char line[192];
    int ln = 0, any = 0;
    u64 off = 0;
    for (int part = 0; part < 32; part++) {
        u64 got = 0;
        int fr = g_lxc_svc->file_read("/tmp/deshab-wifi.txt", off, chunk,
                                      sizeof(chunk), &got, 0);
        if (fr != 0) { if (!any) { slog_num("[wifi] file_read rc=", fr); return -2; } break; }
        if (got == 0) break;
        any = 1;
        for (u64 i = 0; i < got; i++) {
            char c = chunk[i];
            if (c == '\n' || ln >= (int)sizeof(line) - 1) {
                line[ln] = 0;
                if (ln) wifi_guest_line(line);
                ln = 0;
            } else if (c != '\r') {
                line[ln++] = c;
            }
        }
        off += got;
    }
    if (ln) { line[ln] = 0; wifi_guest_line(line); }
    wifi_guest_commit();
    g_wifi_guest = 1;
    slog_num("[wifi] guest aps=", g_wifi_n);
    return any ? 0 : -2;
}

/* 打开卡片并踢一脚扫描 */
static void wifi_card_open(void) {
    g_wifi_open = 1;
    g_wifi_mode = 0;
    g_wifi_sel = -1;
    g_wifi_pw[0] = 0;
    g_wifi_status[0] = 0;
    g_wifi_n = 0;
    g_wifi_guest = 0;
    g_wifi_scanning = 1;
    tray_net_query();
    if (wifi_dev_find() != 0) {
        g_wifi_scanning = 0;
        int grc = wifi_guest_scan();
        if (grc == 0) {
            if (g_wifi_n == 0) wifi_status_set("未发现网络");
            else wifi_status_set("来自客户机");
            return;
        }
        if (grc == -3) {
            wifi_status_set("客户机无无线接口");
            slog("[wifi] guest has no wlan0");
            return;
        }
        if (grc == -2) {
            wifi_status_set("客户机扫描失败");
            return;
        }
        if (grc == -1) {
            /* Linux 客户机未运行（如 Windows/WHPX 或 FUCK linux_guest=0）：
             * 无线扫描必须经客户机，此处明确提示，避免误以为"就是有线"。 */
            if (g_tray_up > 0) wifi_status_set("有线已连接 / WiFi 需 Linux 客户机");
            else wifi_status_set("WiFi 需 Linux 客户机");
            slog("[wifi] guest unavailable (need Linux guest for WiFi scan)");
            return;
        }
        if (g_tray_up > 0) {
            wifi_status_set("有线已连接");
            slog("[wifi] wired link up, no wireless device");
            return;
        }
        wifi_status_set("未检测到无线网卡");
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

/* 经 Linux guest 关联无线：guest 内用 wpa_supplicant（有密码）或 iw（开放网络）
 * 关联 wlan0，随后回读 `iw dev wlan0 link` 判定是否连上。
 * 返回 0=已连接，<0=失败（-1 客户机不可用 / -2 exec 失败 / -3 未连上）。 */
static int wifi_guest_connect(const char *ssid, const char *pw) {
    if (!g_lxc_svc || !g_lxc_svc->exec || !g_lxc_svc->is_available) return -1;
    if (g_lxc_svc->is_available() != 1) return -1;

    /* 单引号不转义——出现即判失败，避免命令注入。 */
    for (const char *s = ssid; *s; s++) if (*s == '\'') return -1;
    for (const char *s = pw; s && *s; s++) if (*s == '\'') return -1;

    static char shcmd[400];
    int p = 0;
    #define WC_ADD(str) do { const char *_s=(str); while (*_s && p < 360) shcmd[p++]=*_s++; } while (0)
    WC_ADD("ip link set wlan0 up 2>/dev/null; pkill -f 'wpa_supplicant.*wlan0' 2>/dev/null; ");
    if (pw && pw[0]) {
        WC_ADD("wpa_passphrase '");
        while (*ssid && p < 360) shcmd[p++] = *ssid++;
        WC_ADD("' '");
        while (*pw && p < 360) shcmd[p++] = *pw++;
        WC_ADD("' >/tmp/w.conf 2>/dev/null; wpa_supplicant -B -i wlan0 -c /tmp/w.conf >/dev/null 2>&1; ");
    } else {
        WC_ADD("iw dev wlan0 connect '");
        while (*ssid && p < 360) shcmd[p++] = *ssid++;
        WC_ADD("'; ");
    }
    WC_ADD("sleep 5; iw dev wlan0 link");
    #undef WC_ADD
    shcmd[p] = 0;

    char out[256];
    u64 len = 0, ec = 0;
    const char *argv[] = { "sh", "-c", shcmd };
    int rc = g_lxc_svc->exec("/bin/sh", 3, argv, out, sizeof(out) - 1, &len, &ec);
    if (rc != 0) { slog_num("[wifi] guest connect rc=", rc); return -2; }
    if (len > sizeof(out) - 1) len = sizeof(out) - 1;
    out[len] = 0;
    slog("[wifi] guest link:");
    slog(out);
    for (u64 i = 0; i + 6 < len; i++)
        if (out[i] == 'C' && out[i+1] == 'o' && out[i+2] == 'n' && out[i+3] == 'n'
            && out[i+4] == 'e' && out[i+5] == 'c' && out[i+6] == 't')
            return 0;
    return -3;
}

/* 发起连接（当前选中网络 + 密码） */
static void wifi_connect_go(void) {
    if (g_wifi_guest) {
        wifi_status_set("客户机连接中...");
        int grc = wifi_guest_connect(g_wifi_sel_ssid, g_wifi_pw);
        if (grc == 0) {
            wifi_status_set("已连接");
            g_wifi_mode = 0;
            g_wifi_open = 0;    /* 连接成功收起卡片 */
        } else if (grc == -1) {
            wifi_status_set("客户机不可用");
            slog("[wifi] guest connect: service unavailable");
        } else {
            wifi_status_set("连接失败");
        }
        g_wifi_pw[0] = 0;
        return;
    }
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

/* 卡片绘制（taskbar_cards_draw 调用；每次绘制顺带收割扫描结果） */
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
    int cy = CARD_TOP;

    g_wifi_card_rect[0] = cx; g_wifi_card_rect[1] = cy;
    g_wifi_card_rect[2] = CW; g_wifi_card_rect[3] = ch;
    aqua_panel(&g_fb, cx, cy, CW, ch, t->radius_md);

    /* 标题行 */
    zh_draw_string(&g_fb, "Wi-Fi", cx + 14, cy + 12, t->text_primary, 0);
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
        if (g_wifi_dev < 0 && g_wifi_n == 0) {
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

/* 卡片点击（taskbar_cards_click 调用；返回 1 = 已消费） */
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

/* ============================================================
 *  控制中心卡片（顶栏控制中心图标弹出：音量/亮度滑块）
 *
 *  M6 提前落地最小版：du_wf_slider 组件实战。音量接 g_vol（状态），
 *  亮度接 g_bright（apply_brightness 真实调暗）。拖拽复用主循环
 *  g_drag_slider 通道（卡片绘制时把自己的轨道矩形写进 g_vol_rect/
 *  g_bri_rect，主循环 slider_set 即可作用到卡片滑块）。
 * ============================================================ */

int g_qs_open = 0;
static int g_qs_card_rect[4];
static int g_qs_vol_rect[4];      /* 音量滑块命中区（镜像到 g_vol_rect） */
static int g_qs_bri_rect[4];      /* 亮度滑块命中区（镜像到 g_bri_rect） */

static void qs_draw(void) {
    if (!g_qs_open) return;
    const wf_theme *t = tb_theme();
    int W = (int)g_fb_w;
    const int CW = 300, CH = 46 + 2 * 58 + 8;

    int cx = g_tb2_vol_rect[0] + g_tb2_vol_rect[2] - CW;
    if (cx < 8) cx = 8;
    if (cx + CW > W - 8) cx = W - 8 - CW;
    int cy = CARD_TOP;

    g_qs_card_rect[0] = cx; g_qs_card_rect[1] = cy;
    g_qs_card_rect[2] = CW; g_qs_card_rect[3] = CH;
    aqua_panel(&g_fb, cx, cy, CW, CH, t->radius_lg);

    zh_draw_string(&g_fb, "控制中心", cx + 14, cy + 12, t->text_primary, 0);

    int y = cy + 46;
    /* 音量行：标签 + 百分比 + 滑块 */
    {
        char vb[8];
        int n = u32dec((u32)g_vol, vb);
        vb[n] = '%'; vb[n + 1] = 0;
        zh_draw_string(&g_fb, "音量", cx + 14, y, t->text_secondary, 0);
        du_draw_string(&g_fb, vb, cx + CW - 14 - (n + 1) * 8, y,
                       t->text_primary, 0, DU_ASCII_STEP);
        du_wf_slider(&g_fb, cx + 14, y + 22, CW - 28, g_vol, t, g_qs_vol_rect);
        g_vol_rect[0] = g_qs_vol_rect[0]; g_vol_rect[1] = g_qs_vol_rect[1];
        g_vol_rect[2] = g_qs_vol_rect[2]; g_vol_rect[3] = g_qs_vol_rect[3];
        y += 58;
    }
    /* 亮度行（真实调暗：apply_brightness 每帧消费 g_bright） */
    {
        char vb[8];
        int n = u32dec((u32)g_bright, vb);
        vb[n] = '%'; vb[n + 1] = 0;
        zh_draw_string(&g_fb, "亮度", cx + 14, y, t->text_secondary, 0);
        du_draw_string(&g_fb, vb, cx + CW - 14 - (n + 1) * 8, y,
                       t->text_primary, 0, DU_ASCII_STEP);
        du_wf_slider(&g_fb, cx + 14, y + 22, CW - 28, g_bright, t, g_qs_bri_rect);
        g_bri_rect[0] = g_qs_bri_rect[0]; g_bri_rect[1] = g_qs_bri_rect[1];
        g_bri_rect[2] = g_qs_bri_rect[2]; g_bri_rect[3] = g_qs_bri_rect[3];
    }
}

/* 卡片点击（返回 1 = 已消费；点外部收起并放行） */
static int qs_click(int mx, int my) {
    if (!g_qs_open) return 0;
    if (!rect_hit(g_qs_card_rect, mx, my)) {
        g_qs_open = 0;
        return 0;
    }
    if (rect_hit(g_qs_vol_rect, mx, my)) {
        g_drag_slider = 1;
        slider_set(g_vol_rect, &g_vol, mx);
        return 1;
    }
    if (rect_hit(g_qs_bri_rect, mx, my)) {
        g_drag_slider = 2;
        slider_set(g_bri_rect, &g_bright, mx);
        return 1;
    }
    return 1;
}

/* ============================================================
 *  关机确认卡片（Apple 菜单"关机…/重新启动…"弹出：关机 / 重启 / 取消）
 *
 *  实际动作经 srv.acpi.poweroff / srv.acpi.reboot（acpi.drv 的
 *  FADT `_S5_` + reset_reg 路径）。服务缺席时 desktop_poweroff 退回
 *  g_quit，desktop_reboot 退回键盘控制器 + 0xCF9。
 *
 *  睡眠（S3）不在本菜单内：ACPI S3 需要设备状态保存/恢复，Deshab 尚无该
 *  能力，按插桩规约不提供"看起来能用"的假按钮。
 * ============================================================ */

static int g_pwr_open = 0;
static int g_pwr_card_rect[4];
static int g_pwr_off_rect[4];
static int g_pwr_reboot_rect[4];
static int g_pwr_cancel_rect[4];

/* 菜单行：悬停为系统蓝圆角底 + 白字 */
static void pwr_row(int r[4], int x, int y, int w, const char *label,
                    const wf_theme *t) {
    int hov;
    r[0] = x; r[1] = y; r[2] = w; r[3] = 28;
    hov = tb_hover_state(r) == WF_BTN_HOVER;
    if (hov) aqua_fill_round_alpha(&g_fb, x, y, w, 28, 5, t->accent, 255);
    zh_draw_string(&g_fb, label, x + 12, y + 5,
                   hov ? t->on_accent : t->text_primary, 0);
}

static void pwr_draw(void) {
    if (!g_pwr_open) return;
    const wf_theme *t = tb_theme();
    int W = (int)g_fb_w;
    const int CW = 220, CH = 34 + 3 * 30 + 10;

    int cx = g_tb2_pwr_rect[0];
    if (cx < 8) cx = 8;
    if (cx + CW > W - 8) cx = W - 8 - CW;
    int cy = CARD_TOP;

    g_pwr_card_rect[0] = cx; g_pwr_card_rect[1] = cy;
    g_pwr_card_rect[2] = CW; g_pwr_card_rect[3] = CH;
    aqua_panel(&g_fb, cx, cy, CW, CH, t->radius_md);

    zh_draw_string(&g_fb, "要关闭这台电脑吗？", cx + 14, cy + 10, t->text_secondary, 0);
    int y = cy + 34;
    pwr_row(g_pwr_off_rect, cx + 6, y, CW - 12, "关机", t);
    y += 30;
    pwr_row(g_pwr_reboot_rect, cx + 6, y, CW - 12, "重新启动", t);
    y += 30;
    pwr_row(g_pwr_cancel_rect, cx + 6, y, CW - 12, "取消", t);
}

/* 返回 1 = 已消费；点外部收起并放行 */
static int pwr_click(int mx, int my) {
    if (!g_pwr_open) return 0;
    if (!rect_hit(g_pwr_card_rect, mx, my)) {
        g_pwr_open = 0;
        return 0;
    }
    if (rect_hit(g_pwr_off_rect, mx, my)) {
        g_pwr_open = 0;
        slog("[taskbar] power: shut down");
        desktop_poweroff();
        return 1;
    }
    if (rect_hit(g_pwr_reboot_rect, mx, my)) {
        g_pwr_open = 0;
        slog("[taskbar] power: restart");
        desktop_reboot();
        return 1;
    }
    if (rect_hit(g_pwr_cancel_rect, mx, my)) {
        g_pwr_open = 0;
        return 1;
    }
    return 1;
}

/* Esc 收起电源菜单（shell.c handle_key 调用；返回 1 = 已消费） */
int taskbar_power_menu_key(int sc) {
    if (!g_pwr_open) return 0;
    if (sc == 0x01) { g_pwr_open = 0; return 1; }
    return 0;
}

/* ---- Apple 菜单 logo（textures/startMenuLogo.rgba） ---- */
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
static wf_btn_state tb_hover_state(const int r[4]) {
    if (g_mouse_x >= r[0] && g_mouse_x < r[0] + r[2] &&
        g_mouse_y >= r[1] && g_mouse_y < r[1] + r[3])
        return WF_BTN_HOVER;
    return WF_BTN_NORMAL;
}

/* 顶栏 Apple 菜单图标（startMenuLogo 纹理缩放到 size×size） */
void taskbar_logo_draw(int x, int y, int size) {
    if (!g_smlogo_ok) return;
    tex_draw_scaled(&g_fb, (const u8 *)SMLOGO_ADDR, SMLOGO_W, SMLOGO_H,
                    x, y, size, size, 256);
}

int taskbar_net_up(void) {
    tray_net_query();
    return g_tray_up > 0;
}

/* ============================================================
 *  卡片开关 / 绘制 / 点击
 * ============================================================ */

int taskbar_cards_open(void) { return g_wifi_open || g_qs_open || g_pwr_open; }

void taskbar_cards_close(void) {
    g_wifi_open = 0;
    g_qs_open = 0;
    g_pwr_open = 0;
}

void taskbar_wifi_toggle(void) {
    int was = g_wifi_open;
    taskbar_cards_close();
    if (!was) wifi_card_open();
}

void taskbar_cc_toggle(void) {
    int was = g_qs_open;
    taskbar_cards_close();
    g_qs_open = !was;
}

void taskbar_power_open(void) {
    taskbar_cards_close();
    g_pwr_open = 1;
}

void taskbar_cards_draw(void) {
    qs_draw();
    wifi_draw();
    pwr_draw();
}

/* 卡片打开时优先消费；点外部收起并放行 */
int taskbar_cards_click(int mx, int my) {
    if (g_wifi_open && wifi_click(mx, my)) return 1;
    if (g_qs_open && qs_click(mx, my)) return 1;
    if (g_pwr_open && pwr_click(mx, my)) return 1;
    return 0;
}
