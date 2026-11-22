/* appreg.c — 应用注册表 + 桌面图标（M0 自 main.c 平移）
 *
 * 注册来源：内置 / LINUXAPP.CNF / PEAPPS.CNF / user/desktop/*.lnk / STARTMENU.CNF
 * 启动台另外读 GUESTMENU.CNF（Linux .desktop + Windows 开始菜单 .lnk）
 */
#include "desktop.h"
#include "../UTSM/include/utsm/fluent_ui.h"   /* M3: desktop_app_icon 图标映射 */

/* ---- 全局状态 ---- */
app_descriptor g_apps[MAX_APPS];
int g_app_count = 0;

desktop_icon g_icons[MAX_ICONS];
int g_icon_count = 0;

/* ============================================================
 *  Winux-Kate 应用图标（40×40，青色几何图形）
 * ============================================================ */

void draw_circle_outline(du_context *ctx, int cx, int cy, int r, u32 color) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 > (r - 1) * (r - 1)) {
                du_pixel(ctx, cx + dx, cy + dy, color);
            }
        }
    }
}

static void draw_icon_terminal(du_context *ctx, int cx, int cy) {
    du_fill_rect(ctx, cx, cy, 40, 40, KS_BG_TERTIARY);
    du_rect_outline(ctx, cx, cy, 40, 40, KS_ACCENT, 2);
    du_fill_rect(ctx, cx + 8,  cy + 12, 2, 2, KS_ACCENT2);
    du_fill_rect(ctx, cx + 10, cy + 14, 2, 2, KS_ACCENT2);
    du_fill_rect(ctx, cx + 12, cy + 16, 2, 2, KS_ACCENT2);
    du_fill_rect(ctx, cx + 10, cy + 18, 2, 2, KS_ACCENT2);
    du_fill_rect(ctx, cx + 8,  cy + 20, 2, 2, KS_ACCENT2);
    du_fill_rect(ctx, cx + 16, cy + 22, 8, 2, KS_ACCENT);
}

static void draw_icon_editor(du_context *ctx, int cx, int cy) {
    du_fill_rect(ctx, cx, cy, 40, 40, KS_BG_TERTIARY);
    du_rect_outline(ctx, cx, cy, 40, 40, KS_ACCENT, 2);
    int widths[] = {24, 20, 22, 14, 20, 18};
    for (int i = 0; i < 6; i++) {
        du_fill_rect(ctx, cx + 8, cy + 8 + i * 4, widths[i], 2,
                     (i == 0) ? KS_ACCENT2 : KS_ACCENT);
    }
}

static void draw_icon_folder(du_context *ctx, int cx, int cy) {
    du_fill_rect(ctx, cx, cy, 40, 40, KS_BG_TERTIARY);
    du_rect_outline(ctx, cx, cy, 40, 40, KS_BORDER, 2);
    du_fill_rect(ctx, cx + 8, cy + 10, 12, 4, KS_ACCENT_DIM);
    du_rect_outline(ctx, cx + 8, cy + 10, 12, 4, KS_ACCENT, 1);
    du_fill_rect(ctx, cx + 6, cy + 14, 28, 16, KS_BG_SECONDARY);
    du_rect_outline(ctx, cx + 6, cy + 14, 28, 16, KS_ACCENT, 1);
    du_fill_rect(ctx, cx + 6, cy + 14, 28, 1, KS_ACCENT);
    du_fill_rect(ctx, cx + 10, cy + 19, 12, 1, KS_ACCENT2);
    du_fill_rect(ctx, cx + 10, cy + 23, 16, 1, KS_TEXT_DIM);
    du_fill_rect(ctx, cx + 10, cy + 26, 10, 1, KS_TEXT_DIM);
}

static void draw_icon_calc(du_context *ctx, int cx, int cy) {
    du_fill_rect(ctx, cx, cy, 40, 40, KS_BG_TERTIARY);
    du_rect_outline(ctx, cx, cy, 40, 40, KS_ACCENT, 2);
    du_fill_rect(ctx, cx + 6, cy + 6, 28, 8, KS_BG_PRIMARY);
    du_rect_outline(ctx, cx + 6, cy + 6, 28, 8, KS_ACCENT2, 1);
    du_fill_rect(ctx, cx + 28, cy + 8, 4, 4, KS_ACCENT2);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int bx = cx + 6 + c * 9;
            int by = cy + 18 + r * 7;
            du_fill_rect(ctx, bx, by, 7, 5, KS_ACCENT_DIM);
            du_rect_outline(ctx, bx, by, 7, 5, KS_ACCENT, 1);
        }
    }
}

static void draw_icon_browser(du_context *ctx, int cx, int cy) {
    du_fill_rect(ctx, cx, cy, 40, 40, KS_BG_TERTIARY);
    du_rect_outline(ctx, cx, cy, 40, 40, KS_ACCENT, 2);
    int ccx = cx + 20, ccy = cy + 20, r = 11;
    draw_circle_outline(ctx, ccx, ccy, r, KS_ACCENT);
    du_fill_rect(ctx, ccx - r, ccy, 2 * r + 1, 1, KS_ACCENT);
    du_fill_rect(ctx, ccx, ccy - r, 1, 2 * r + 1, KS_ACCENT);
    du_pixel(ctx, ccx, ccy, KS_ACCENT2);
}

void draw_app_icon(du_context *ctx, int app_id, int cx, int cy) {
    const char *n = (app_id >= 0 && app_id < g_app_count) ? g_apps[app_id].name : 0;
    if (!n) { draw_icon_terminal(ctx, cx, cy); return; }
    if (n[0]=='e' && n[1]=='d') { draw_icon_editor(ctx, cx, cy); return; }
    if (n[0]=='p' && n[1]=='r') { draw_icon_editor(ctx, cx, cy); return; }
    if ((n[0]=='f' && n[1]=='i') || (n[0]=='f' && n[1]=='o')) {
        draw_icon_folder(ctx, cx, cy); return;
    }
    if (n[0]=='c' && n[1]=='a') { draw_icon_calc(ctx, cx, cy); return; }
    if (n[0]=='b' && n[1]=='r') { draw_icon_browser(ctx, cx, cy); return; }
    draw_icon_terminal(ctx, cx, cy);
}

/* M3: app_id → Fluent 线性图标（开始菜单瓦片 / 任务栏共用映射）。
 * 返回值即 wf_icon_id（int 传递避免 desktop.h 依赖 fluent_ui.h）。 */
int desktop_app_icon(int app_id) {
    const char *n = (app_id >= 0 && app_id < g_app_count) ? g_apps[app_id].name : 0;
    if (!n) return WF_ICON_TERMINAL;
    if (n[0]=='s' && n[1]=='h') return WF_ICON_TERMINAL;      /* shell */
    if (n[0]=='c' && n[1]=='m' && n[2]=='d') return WF_ICON_TERMINAL;
    if (n[0]=='e' && n[1]=='d') return WF_ICON_EDITOR;        /* editor */
    if (n[0]=='p' && n[1]=='r') return WF_ICON_EDITOR;        /* proedit */
    if (n[0]=='f' && n[1]=='i') return WF_ICON_FOLDER;        /* fileman */
    if (n[0]=='c' && n[1]=='a' && n[2]=='l') return WF_ICON_CALC;
    if (n[0]=='b' && n[1]=='r') return WF_ICON_BROWSER;       /* browser */
    if (n[0]=='s' && n[1]=='e') return WF_ICON_SETTINGS;      /* settings */
    if (n[0]=='v' || n[0]=='k') return WF_ICON_BROWSER;       /* vscode/kde */
    if (n[0]=='s' && n[1]=='y' && n[2]=='s') return WF_ICON_INFO;   /* sysinfo */
    if (n[0]=='c' && n[1]=='l') return WF_ICON_CLOCK;         /* clock */
    if (n[0]=='p' && n[1]=='a') return WF_ICON_PICTURE;       /* paint */
    if (n[0]=='n' && n[1]=='o') return WF_ICON_EDITOR;        /* notes */
    if (n[0]=='d' && n[1]=='i') return WF_ICON_DISK;          /* diskinfo */
    return WF_ICON_APPS;
}

/* ============================================================
 *  应用注册
 * ============================================================ */

/* ---- VSCode Phase 5: LINUXAPP.CNF Linux 应用注册表 ----
 * FAT32 根目录 LINUXAPP.CNF（8.3: LINUXAPPCNF），每行一个 Linux guest 应用：
 *   NAME|DISPLAY|/guest/path|args（args 可含空格，可省略）
 * '#' 开头为注释。解析结果存静态池，app_descriptor.linux_path 指入。 */
#define LINUXAPP_MAX 8
static char g_lapp_name[LINUXAPP_MAX][12];
static char g_lapp_disp[LINUXAPP_MAX][16];
static char g_lapp_path[LINUXAPP_MAX][96];
static char g_lapp_args[LINUXAPP_MAX][96];
int g_vscode_app_id = -1;   /* 第一个名为 "vscode" 的注册项 */

/* 字段拷贝：src[0..n) → dst（NUL 结尾），返回拷贝长度 */
static int lapp_field_copy(char *dst, int dst_cap, const char *src, int n) {
    if (n >= dst_cap) n = dst_cap - 1;
    for (int i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = 0;
    return n;
}

#define UDESK_MAX 12
static char g_ud_name[UDESK_MAX][12];
static char g_ud_disp[UDESK_MAX][48];
static char g_ud_path[UDESK_MAX][96];
static char g_ud_elf[UDESK_MAX][12];
static char g_ud_elfpath[UDESK_MAX][96];
static char g_ud_files[UDESK_MAX][64];
static int  g_ud_nfiles;
/* 桌面图标按 user/desktop 目录枚举顺序排，不按内置表下标。 */
static int  g_desk_order[UDESK_MAX];
static int  g_desk_n;

static int udesk_name_taken(const char *n) {
    int i;
    for (i = 0; i < g_app_count; i++) {
        if (g_apps[i].name && fat32_lfn_streq_ci(g_apps[i].name, n)) return 1;
    }
    return 0;
}

static int udesk_collect_cb(const f32_entry *e, void *user) {
    (void)user;
    const char *nm = (e->has_lfn && e->long_name[0]) ? e->long_name : e->name;
    if (e->is_dir || !nm[0]) return 0;
    if (!fat32_lfn_ends_with_ci(nm, ".lnk")) return 0;
    if (g_ud_nfiles >= UDESK_MAX) return 1;
    lapp_field_copy(g_ud_files[g_ud_nfiles], 64, nm, kstrlen(nm));
    g_ud_nfiles++;
    return 0;
}

static int udesk_ascii_ok(const char *s) {
    if (!s || !s[0]) return 0;
    while (*s) {
        if ((unsigned char)*s > 0x7E) return 0;
        s++;
    }
    return 1;
}

static int kind_word(const char *kind, const char *word) {
    int i = 0;
    while (word[i]) {
        char c = kind[i];
        char w = word[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (w >= 'a' && w <= 'z') w = (char)(w - 32);
        if (c != w) return 0;
        i++;
    }
    return kind[i] == 0 || kind[i] == ' ';
}

static int find_app_by_name(const char *name) {
    int a, i;
    if (!name || !name[0]) return -1;
    for (a = 0; a < g_app_count; a++) {
        const char *nm = g_apps[a].name;
        if (!nm) continue;
        i = 0;
        while (name[i] && nm[i] && name[i] == nm[i]) i++;
        if (name[i] == 0 && nm[i] == 0) return a;
    }
    return -1;
}

static void desk_place(int app_id) {
    int i;
    if (app_id < 0 || app_id >= g_app_count) return;
    for (i = 0; i < g_desk_n; i++) {
        if (g_desk_order[i] == app_id) return;
    }
    if (g_desk_n >= UDESK_MAX) return;
    g_apps[app_id].user_desktop = 1;
    g_desk_order[g_desk_n++] = app_id;
}

/* 快捷方式种类的第一个词对应内置应用。路径以 '/' 开头不能改写种类。 */
static const char *kind_builtin_name(const char *kind) {
    if (kind_word(kind, "FILEMAN")) return "fileman";
    if (kind_word(kind, "EDITOR")) return "editor";
    if (kind_word(kind, "BROWSER")) return "browser";
    if (kind_word(kind, "SHELL")) return "shell";
    if (kind_word(kind, "CALC")) return "calc";
    if (kind_word(kind, "CMD")) return "cmd";
    if (kind_word(kind, "PROEDIT")) return "proedit";
    if (kind_word(kind, "SETTINGS")) return "settings";
    return 0;
}

static int bind_desktop_builtin(int slot, const char *name) {
    int a = find_app_by_name(name);
    if (a < 0) return 0;
    g_apps[a].display_name = g_ud_disp[slot];
    desk_place(a);
    slog("desktop shortcut -> builtin");
    slog(g_ud_disp[slot]);
    return 1;
}

static void udesk_parse_lnk(int slot, const u8 *data, u32 size) {
    const char *lines[4];
    int nlen[4];
    int n = 0, i;
    u32 pos = 0;
    char kind[16];
    if (slot < 0 || slot >= UDESK_MAX) return;
    while (pos < size && n < 4) {
        u32 eol = pos;
        while (eol < size && data[eol] != '\n' && data[eol] != '\r') eol++;
        lines[n] = (const char *)data + pos;
        nlen[n] = (int)(eol - pos);
        n++;
        while (eol < size && (data[eol] == '\n' || data[eol] == '\r')) eol++;
        pos = eol;
    }
    if (n < 2) return;
    if (n >= 4 && nlen[3] > 0)
        lapp_field_copy(g_ud_name[slot], 12, lines[3], nlen[3]);
    else
        lapp_field_copy(g_ud_name[slot], 12, lines[0], nlen[0]);
    if (udesk_ascii_ok(g_ud_name[slot]) == 0) {
        g_ud_name[slot][0] = 'a' + (char)slot;
        g_ud_name[slot][1] = 0;
    }
    /* 第一行是桌面标签，允许 UTF-8（文件管理 / 终端 等） */
    if (nlen[0] > 0)
        lapp_field_copy(g_ud_disp[slot], 48, lines[0], nlen[0]);
    else
        kstrcpy(g_ud_disp[slot], g_ud_name[slot], 48);
    lapp_field_copy(kind, 16, lines[1], nlen[1]);
    g_ud_path[slot][0] = 0;
    g_ud_elf[slot][0] = 0;
    g_ud_elfpath[slot][0] = 0;
    /* 种类优先。第三行路径经常以 '/' 开头，不能因此把 ELF / NATIVE 当成 Linux。 */
    if (kind_word(kind, "LINUX")) {
        if (n >= 3)
            lapp_field_copy(g_ud_path[slot], 96, lines[2], nlen[2]);
    } else if (kind_word(kind, "NATIVE")) {
        if (bind_desktop_builtin(slot, g_ud_name[slot])) return;
        /* 没有同名内置应用时，第三行是要启动的 ELF（native.lnk 的 desktop.elf）。 */
        if (n >= 3)
            lapp_field_copy(g_ud_elfpath[slot], 96, lines[2], nlen[2]);
    } else {
        const char *bn = kind_builtin_name(kind);
        if (bn && bind_desktop_builtin(slot, bn)) return;
        if (bind_desktop_builtin(slot, g_ud_name[slot])) return;
        if (nlen[1] >= 11)
            lapp_field_copy(g_ud_elf[slot], 12, lines[1], 11);
        else
            f32_name_to_83_w(kind, g_ud_elf[slot]);
        g_ud_elf[slot][11] = 0;
    }
    if (udesk_name_taken(g_ud_name[slot])) {
        /* LINUXAPP.CNF 先注册了同名项（KDE/VSCODE）。快捷方式仍要落在桌面上，
         * 复用已有描述符，保留 linux_path。 */
        int a;
        for (a = 0; a < g_app_count; a++) {
            if (g_apps[a].name && fat32_lfn_streq_ci(g_apps[a].name, g_ud_name[slot])) {
                g_apps[a].display_name = g_ud_disp[slot];
                desk_place(a);
                slog("desktop shortcut -> existing");
                slog(g_ud_disp[slot]);
                break;
            }
        }
        return;
    }
    if (!g_ud_path[slot][0] && !g_ud_elf[slot][0] && !g_ud_elfpath[slot][0]) return;
    if (g_app_count >= MAX_APPS) return;
    {
        int id = g_app_count;
        g_apps[id] = (app_descriptor){
            g_ud_name[slot], g_ud_disp[slot], 720, 480,
            0, 0, 0, 0,
            g_ud_elf[slot][0] ? g_ud_elf[slot] : 0,
            g_ud_path[slot][0] ? g_ud_path[slot] : 0, 0, 0, 0,
            0
        };
        g_app_count++;
        if (g_ud_elfpath[slot][0])
            g_apps[id].elf_path = g_ud_elfpath[slot];
        desk_place(id);
    }
    slog("user/desktop icon:");
    slog(g_ud_disp[slot]);
    (void)i;
}

static void register_user_desktop_icons(void) {
    u32 dclus = 0;
    int i;
    g_ud_nfiles = 0;
    g_desk_n = 0;
    if (f32_find_path_dir_lfn_w("user/desktop", &dclus) != 0) {
        slog("user/desktop missing, skip shared icons");
        return;
    }
    if (f32_list_dir_w(dclus, udesk_collect_cb, 0) != 0) return;
    for (i = 0; i < g_ud_nfiles && g_app_count < MAX_APPS; i++) {
        char path[96];
        u8 *data = 0;
        u32 size = 0;
        kstrcpy(path, "user/desktop/", 96);
        kstrcat(path, g_ud_files[i], 96);
        if (f32_read_path_lfn_w(path, &data, &size) != 0 || !data || !size) continue;
        udesk_parse_lnk(i, data, size);
    }
}

static void register_linux_apps(void) {
    u8 *data = 0;
    u32 size = 0;
    char n11[11];
    if (f32_name_to_83_w("LINUXAPP.CNF", n11) != 0) return;
    if (f32_read_root_file_w(n11, &data, &size) != 0) return;  /* 无配置文件：跳过 */

    int li = 0;                 /* 已注册数量 */
    u32 pos = 0;
    while (pos < size && li < LINUXAPP_MAX && g_app_count < MAX_APPS) {
        /* 取一行 */
        u32 eol = pos;
        while (eol < size && data[eol] != '\n' && data[eol] != '\r') eol++;
        u32 line_len = eol - pos;
        /* 跳过到下一行 */
        u32 next = eol;
        while (next < size && (data[next] == '\n' || data[next] == '\r')) next++;

        if (line_len == 0 || data[pos] == '#') { pos = next; continue; }

        /* 按 '|' 切 4 段：NAME|DISPLAY|PATH|ARGS */
        const char *line = (const char *)data + pos;
        int f0 = -1, f1 = -1, f2 = -1;   /* 三个分隔符位置 */
        for (u32 i = 0; i < line_len; i++) {
            if (line[i] == '|') {
                if (f0 < 0) f0 = (int)i;
                else if (f1 < 0) f1 = (int)i;
                else if (f2 < 0) { f2 = (int)i; break; }
            }
        }
        if (f0 > 0 && f1 > f0) {
            int path_beg = f1 + 1;
            int path_end = (f2 > path_beg) ? f2 : (int)line_len;
            int args_beg = (f2 > path_beg) ? f2 + 1 : (int)line_len;

            lapp_field_copy(g_lapp_name[li], 12, line, f0);
            lapp_field_copy(g_lapp_disp[li], 16, line + f0 + 1, f1 - f0 - 1);
            lapp_field_copy(g_lapp_path[li], 96, line + path_beg, path_end - path_beg);
            if (args_beg < (int)line_len)
                lapp_field_copy(g_lapp_args[li], 96, line + args_beg, (int)line_len - args_beg);
            else
                g_lapp_args[li][0] = 0;

            if (g_lapp_path[li][0] == '/') {
                g_apps[g_app_count] = (app_descriptor){
                    g_lapp_name[li], g_lapp_disp[li], 1024, 768,
                    0, 0, 0, 0, 0,
                    g_lapp_path[li], g_lapp_args[li]
                };
                /* 记录 VSCode app id（IDE attach 共享启动路径用）。
                 * P7.4 匹配收紧：精确匹配 "VSCODE"（第 7 字符须为 NUL），
                 * VSCODE2 等多实例条目不占用共享启动入口。 */
                if (g_vscode_app_id < 0 &&
                    g_lapp_name[li][0] == 'V' && g_lapp_name[li][1] == 'S' &&
                    g_lapp_name[li][2] == 'C' && g_lapp_name[li][3] == 'O' &&
                    g_lapp_name[li][4] == 'D' && g_lapp_name[li][5] == 'E' &&
                    g_lapp_name[li][6] == 0)
                    g_vscode_app_id = g_app_count;
                g_app_count++;
                li++;
            }
        }
        pos = next;
    }
}

/* ---- P5d: PEAPPS.CNF Windows PE 应用注册表 ----
 * FAT32 根目录 PEAPPS.CNF（8.3: PEAPPS CNF），每行一个 Windows PE 应用：
 *   NAME|DISPLAY|path/file.exe|args（'/' 分隔长路径，args 可含空格可省略）
 * '#' 开头为注释。解析结果存静态池，app_descriptor.pe_file 指入。
 * 仅适用于 GUI PE（消息循环驱动 pump）；纯控制台 PE 不会触发消息空转，
 * 窗口模式下桌面将无响应直到进程退出——控制台程序请走 CMD。 */
#define PEAPP_MAX 4
static char g_papp_name[PEAPP_MAX][12];
static char g_papp_disp[PEAPP_MAX][16];
static char g_papp_file[PEAPP_MAX][96];
static char g_papp_args[PEAPP_MAX][96];

static void register_pe_apps(void) {
    u8 *data = 0;
    u32 size = 0;
    char n11[11];
    if (f32_name_to_83_w("PEAPPS.CNF", n11) != 0) return;
    if (f32_read_root_file_w(n11, &data, &size) != 0) return;  /* 无配置文件：跳过 */

    int pi = 0;
    u32 pos = 0;
    while (pos < size && pi < PEAPP_MAX && g_app_count < MAX_APPS) {
        u32 eol = pos;
        while (eol < size && data[eol] != '\n' && data[eol] != '\r') eol++;
        u32 line_len = eol - pos;
        u32 next = eol;
        while (next < size && (data[next] == '\n' || data[next] == '\r')) next++;

        if (line_len == 0 || data[pos] == '#') { pos = next; continue; }

        /* 按 '|' 切 4 段：NAME|DISPLAY|FILE|ARGS */
        const char *line = (const char *)data + pos;
        int f0 = -1, f1 = -1, f2 = -1;
        for (u32 i = 0; i < line_len; i++) {
            if (line[i] == '|') {
                if (f0 < 0) f0 = (int)i;
                else if (f1 < 0) f1 = (int)i;
                else if (f2 < 0) { f2 = (int)i; break; }
            }
        }
        if (f0 > 0 && f1 > f0) {
            int file_beg = f1 + 1;
            int file_end = (f2 > file_beg) ? f2 : (int)line_len;
            int args_beg = (f2 > file_beg) ? f2 + 1 : (int)line_len;

            lapp_field_copy(g_papp_name[pi], 12, line, f0);
            lapp_field_copy(g_papp_disp[pi], 16, line + f0 + 1, f1 - f0 - 1);
            lapp_field_copy(g_papp_file[pi], 96, line + file_beg, file_end - file_beg);
            if (args_beg < (int)line_len)
                lapp_field_copy(g_papp_args[pi], 96, line + args_beg, (int)line_len - args_beg);
            else
                g_papp_args[pi][0] = 0;

            if (g_papp_file[pi][0]) {
                g_apps[g_app_count] = (app_descriptor){
                    g_papp_name[pi], g_papp_disp[pi], 720, 540,
                    0, 0, pe_win_on_draw, 0, 0, 0, 0,
                    g_papp_file[pi], g_papp_args[pi]
                };
                g_app_count++;
                pi++;
            }
        }
        pos = next;
    }
}

/* ============================================================
 *  M3: STARTMENU.CNF 开始菜单注册表
 *  FAT32 根目录 STARTMENU.CNF（8.3: STARTMENUCNF），每行一个菜单项：
 *    NAME|显示名|path|args（args 可省略）
 *  path 四种（目录约定：bin/ = ELF 工具，program/ = exe）：
 *    builtin:<name>   → 已注册内置应用（editor/calc/fileman/settings/...）
 *    program/xxx.exe  → PE 应用（run_windowed 桌面嵌入，g++ 编译；
 *                       CLI exe 同目录但请从 cmd 运行，不入菜单）
 *    bin/xxx.elf      → ELF 工具（按路径全屏加载）
 *    /guest/path      → Linux guest 应用（exec_async）
 *  显示名允许 UTF-8 中文（瓦片标签）；NAME 为 ASCII（窗口标题）。
 *  缺失/为空时回退：全部已注册应用进开始菜单。
 * ============================================================ */

int  g_sm_ids[SM_MAX_APPS];
char g_sm_labels[SM_MAX_APPS][48];
int  g_sm_count = 0;

static char g_smn_name[SM_MAX_APPS][12];
static char g_smn_path[SM_MAX_APPS][96];
static char g_smn_args[SM_MAX_APPS][96];

static void sm_add(int app_id, const char *label) {
    if (g_sm_count >= SM_MAX_APPS) return;
    if (app_id < 0 || app_id >= g_app_count) return;
    g_sm_ids[g_sm_count] = app_id;
    if (label && label[0])
        lapp_field_copy(g_sm_labels[g_sm_count], 48, label, kstrlen(label));
    else
        kstrcpy(g_sm_labels[g_sm_count], g_apps[app_id].display_name, 48);
    g_sm_count++;
}

static int str_eq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == 0 && b[i] == 0;
}

static int str_ends_with_ci(const char *s, const char *suffix) {
    int sl = kstrlen(s), fl = kstrlen(suffix);
    if (sl < fl) return 0;
    for (int i = 0; i < fl; i++) {
        char c = s[sl - fl + i], d = suffix[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (d >= 'a' && d <= 'z') d = (char)(d - 32);
        if (c != d) return 0;
    }
    return 1;
}

/* 已有同 pe_file / elf_path / linux_path / name 的注册项 → 复用 id */
static int sm_find_existing(const char *name, const char *path) {
    for (int i = 0; i < g_app_count; i++) {
        const app_descriptor *a = &g_apps[i];
        if (a->pe_file && str_eq(a->pe_file, path)) return i;
        if (a->elf_path && str_eq(a->elf_path, path)) return i;
        if (a->linux_path && str_eq(a->linux_path, path)) return i;
    }
    return find_app_by_name(name);
}

static void sm_register_entry(int slot) {
    const char *name = g_smn_name[slot];
    const char *path = g_smn_path[slot];
    const char *args = g_smn_args[slot];
    const char *label = g_sm_labels[slot];
    if (!name[0] || !path[0]) return;

    /* builtin:<name> → 绑定已注册应用 */
    if (path[0]=='b' && path[1]=='u' && path[2]=='i' && path[3]=='l' &&
        path[4]=='t' && path[5]=='i' && path[6]=='n' && path[7]==':') {
        int a = find_app_by_name(path + 8);
        if (a >= 0) {
            sm_add(a, label);
        } else {
            slog("[startmenu] builtin not found:");
            slog(path + 8);
        }
        return;
    }

    /* /guest → Linux guest 应用 */
    if (path[0] == '/') {
        int a = sm_find_existing(name, path);
        if (a >= 0) { sm_add(a, label); return; }
        if (g_app_count >= MAX_APPS) return;
        int id = g_app_count;
        g_apps[id] = (app_descriptor){
            name, name, 1024, 768,
            0, 0, 0, 0, 0,
            path, args[0] ? args : 0
        };
        g_app_count++;
        sm_add(id, label);
        return;
    }

    /* *.exe → PE 应用（GUI，run_windowed） */
    if (str_ends_with_ci(path, ".exe")) {
        int a = sm_find_existing(name, path);
        if (a >= 0) { sm_add(a, label); return; }
        if (g_app_count >= MAX_APPS) return;
        int id = g_app_count;
        g_apps[id] = (app_descriptor){
            name, name, 760, 560,
            0, 0, pe_win_on_draw, 0, 0, 0, 0,
            path, args[0] ? args : 0
        };
        g_app_count++;
        sm_add(id, label);
        return;
    }

    /* *.elf → 按路径加载的外部 ELF 工具（bin/xxx.elf） */
    if (str_ends_with_ci(path, ".elf")) {
        int a = sm_find_existing(name, path);
        if (a >= 0) { sm_add(a, label); return; }
        if (g_app_count >= MAX_APPS) return;
        int id = g_app_count;
        g_apps[id] = (app_descriptor){
            name, name, 800, 560,
            0, 0, 0, 0, 0, 0, 0, 0, 0
        };
        g_apps[id].elf_path = path;
        g_app_count++;
        sm_add(id, label);
        return;
    }

    /* 兜底：按 NAME 找已注册应用 */
    {
        int a = find_app_by_name(name);
        if (a >= 0) sm_add(a, label);
    }
}

static void register_startmenu_apps(void) {
    u8 *data = 0;
    u32 size = 0;
    char n11[11];
    int rc;

    g_sm_count = 0;
    /* STARTMENU.CNF 的 8 字符基名被 vfat 驱动存为短名 STARTM~1.CNF + LFN
     * （基名恰满 8 字符时驱动走 LFN 路径），所以必须用 LFN 路径读取；
     * 先试 LFN，失败再回退 8.3 根读（某些环境可能存为纯 8.3）。 */
    rc = f32_read_path_lfn_w("STARTMENU.CNF", &data, &size);
    if ((rc != 0 || !data || !size) &&
        f32_name_to_83_w("STARTMENU.CNF", n11) == 0) {
        rc = f32_read_root_file_w(n11, &data, &size);
    }
    if (rc == 0 && data && size) {
        int slot = 0;
        u32 pos = 0;
        while (pos < size && slot < SM_MAX_APPS) {
            u32 eol = pos;
            while (eol < size && data[eol] != '\n' && data[eol] != '\r') eol++;
            u32 line_len = eol - pos;
            u32 next = eol;
            while (next < size && (data[next] == '\n' || data[next] == '\r')) next++;

            if (line_len == 0 || data[pos] == '#') { pos = next; continue; }

            /* 按 '|' 切 4 段：NAME|DISPLAY|PATH|ARGS */
            const char *line = (const char *)data + pos;
            int f0 = -1, f1 = -1, f2 = -1;
            for (u32 i = 0; i < line_len; i++) {
                if (line[i] == '|') {
                    if (f0 < 0) f0 = (int)i;
                    else if (f1 < 0) f1 = (int)i;
                    else if (f2 < 0) { f2 = (int)i; break; }
                }
            }
            if (f0 > 0 && f1 > f0) {
                int path_beg = f1 + 1;
                int path_end = (f2 > path_beg) ? f2 : (int)line_len;
                int args_beg = (f2 > path_beg) ? f2 + 1 : (int)line_len;

                lapp_field_copy(g_smn_name[slot], 12, line, f0);
                lapp_field_copy(g_sm_labels[slot], 48, line + f0 + 1, f1 - f0 - 1);
                lapp_field_copy(g_smn_path[slot], 96, line + path_beg, path_end - path_beg);
                if (args_beg < (int)line_len)
                    lapp_field_copy(g_smn_args[slot], 96, line + args_beg, (int)line_len - args_beg);
                else
                    g_smn_args[slot][0] = 0;
                slot++;
            }
            pos = next;
        }
        for (int i = 0; i < slot; i++) sm_register_entry(i);
        slog_num("[startmenu] config entries=", g_sm_count);
        return;
    }

    /* 无配置文件：全部已注册应用入菜单 */
    for (int i = 0; i < g_app_count && g_sm_count < SM_MAX_APPS; i++)
        sm_add(i, g_apps[i].display_name);
    slog("[startmenu] no config, fallback to all apps");
}

/* GUESTMENU.CNF：L|name|label|/abs/path|args 或 W|name|label|C:\path|args
 * 由 tools/desktop/scan_guest_menus.ps1 从 Linux 应用菜单和 Windows
 * 「开始菜单」扫出来。已在本机菜单里的同一 Linux 路径不重复列入。 */
#define GUEST_PATH_CAP 180
#define GUEST_ARGS_CAP 96
int  g_guest_count = 0;
char g_guest_kind[GUEST_MENU_MAX];
char g_guest_name[GUEST_MENU_MAX][12];
char g_guest_label[GUEST_MENU_MAX][64];
static char g_guest_path[GUEST_MENU_MAX][GUEST_PATH_CAP];
static char g_guest_args[GUEST_MENU_MAX][GUEST_ARGS_CAP];

static int guest_path_taken(const char *path) {
    int i;
    for (i = 0; i < g_app_count; i++) {
        if (g_apps[i].linux_path && str_eq(g_apps[i].linux_path, path)) return 1;
    }
    for (i = 0; i < g_guest_count; i++) {
        if (str_eq(g_guest_path[i], path)) return 1;
    }
    return 0;
}

static void register_guest_menus(void) {
    u8 *data = 0;
    u32 size = 0;
    u32 pos = 0;
    int nL = 0, nW = 0;
    g_guest_count = 0;
    if (f32_read_path_lfn_w("GUESTMENU.CNF", &data, &size) != 0 || !data || !size) {
        slog("[startmenu] no guest menu");
        return;
    }
    while (pos < size && g_guest_count < GUEST_MENU_MAX) {
        u32 eol = pos;
        u32 line_len, next;
        const char *line;
        int f0 = -1, f1 = -1, f2 = -1, f3 = -1;
        int name_n, label_n, path_n, args_n, slot;
        char kind;
        while (eol < size && data[eol] != '\n' && data[eol] != '\r') eol++;
        line_len = eol - pos;
        next = eol;
        while (next < size && (data[next] == '\n' || data[next] == '\r')) next++;
        if (line_len == 0 || data[pos] == '#') { pos = next; continue; }
        line = (const char *)data + pos;
        for (u32 i = 0; i < line_len; i++) {
            if (line[i] != '|') continue;
            if (f0 < 0) f0 = (int)i;
            else if (f1 < 0) f1 = (int)i;
            else if (f2 < 0) f2 = (int)i;
            else if (f3 < 0) { f3 = (int)i; break; }
        }
        if (f0 != 1 || f1 <= f0 || f2 <= f1) { pos = next; continue; }
        kind = line[0];
        if (kind != 'L' && kind != 'W') { pos = next; continue; }
        name_n = f1 - f0 - 1;
        label_n = f2 - f1 - 1;
        path_n = (f3 > f2) ? (f3 - f2 - 1) : ((int)line_len - f2 - 1);
        args_n = (f3 > f2) ? ((int)line_len - f3 - 1) : 0;
        if (path_n <= 0 || path_n >= GUEST_PATH_CAP) { pos = next; continue; }
        if (kind == 'L' && line[f2 + 1] != '/') { pos = next; continue; }
        if (kind == 'W') {
            int colon = 0, k;
            for (k = 0; k < path_n; k++) if (line[f2 + 1 + k] == ':') colon = 1;
            if (!colon && line[f2 + 1] != '\\') { pos = next; continue; }
        }
        slot = g_guest_count;
        lapp_field_copy(g_guest_path[slot], GUEST_PATH_CAP, line + f2 + 1, path_n);
        if (guest_path_taken(g_guest_path[slot])) { pos = next; continue; }
        g_guest_kind[slot] = kind;
        if (name_n > 0)
            lapp_field_copy(g_guest_name[slot], 12, line + f0 + 1, name_n);
        else {
            g_guest_name[slot][0] = (kind == 'L') ? 'l' : 'w';
            g_guest_name[slot][1] = 0;
        }
        if (label_n > 0)
            lapp_field_copy(g_guest_label[slot], 64, line + f1 + 1, label_n);
        else
            kstrcpy(g_guest_label[slot], g_guest_name[slot], 64);
        if (args_n > 0)
            lapp_field_copy(g_guest_args[slot], GUEST_ARGS_CAP, line + f3 + 1, args_n);
        else
            g_guest_args[slot][0] = 0;
        g_guest_count++;
        if (kind == 'L') nL++; else nW++;
        pos = next;
    }
    slog_num("[startmenu] guest linux=", nL);
    slog_num("[startmenu] guest windows=", nW);
}

void launch_guest_entry(int idx) {
    if (idx < 0 || idx >= g_guest_count) {
        desktop_fatal("DESK-E16 GUEST MENU IDX");
        return;
    }
    if (g_guest_kind[idx] == 'L') {
        launch_linux_path(g_guest_name[idx], g_guest_label[idx],
                          g_guest_path[idx], g_guest_args[idx]);
        return;
    }
    if (g_guest_kind[idx] == 'W') {
        /* 开始菜单能列出 Windows 程序。逐个 exe 拉起 Windows guest 还没接线。 */
        slog("win start menu:");
        slog(g_guest_path[idx]);
        desktop_fatal("DESK-E15 WIN GUEST APP");
        return;
    }
    desktop_fatal("DESK-E16 GUEST MENU KIND");
}

/* ============================================================
 *  应用注册
 *  ============================================================ */

void register_apps(void) {
    /* 命令行1：shell.elf（原生命令行 + 可运行 Linux 程序）*/
    g_apps[0] = (app_descriptor){
        "shell", "Terminal", 840, 540, 0, 0, 0, 0, 0
    };
    /* 内置 ELF 应用用 elf_path（FS 路径），不用 elf_name（FS 根 8.3 名）——
     * 这些 .elf 实际在 system/deshab64/ 下，不是根目录，取根名必然 "not found"。
     * （此前经开始菜单/桌面图标点开一律静默失败。） */
    g_apps[0].elf_path = "system/deshab64/shell.elf";
    g_app_count++;
    /* 命令行2：cmd.elf（Windows 风格命令行 + PE/EXE 执行）*/
    g_apps[1] = (app_descriptor){
        "cmd", "CMD", 840, 540, 0, 0, 0, 0, 0
    };
    g_apps[1].elf_path = "system/deshab64/cmd.elf";
    g_app_count++;
    g_apps[2] = (app_descriptor){
        "editor", "Editor", 600, 450,
        editor_on_create, editor_on_event, editor_on_draw, editor_on_destroy, 0
    };
    g_app_count++;
    g_apps[3] = (app_descriptor){
        "fileman", "Files", 880, 560,
        fileman_on_create, fileman_on_event, fileman_on_draw, fileman_on_destroy, 0
    };
    g_app_count++;
    g_apps[4] = (app_descriptor){
        "calc", "Calculator", 280, 400,
        calc_on_create, calc_on_event, calc_on_draw, calc_on_destroy, 0
    };
    g_app_count++;
    g_apps[5] = (app_descriptor){
        "browser", "Browser", 1000, 660, 0, 0, 0, 0, 0
    };
    g_apps[5].elf_path = "system/deshab64/tools/browser.elf";
    g_app_count++;
    /* ProEdit — 增强版文本编辑器（语法高亮、设置、目录浏览）*/
    g_apps[6] = (app_descriptor){
        "proedit", "ProEdit", 880, 620, 0, 0, 0, 0, 0
    };
    g_apps[6].elf_path = "system/deshab64/tools/editor.elf";
    g_app_count++;
    /* Settings — 系统设置（内核 FUCK 配置 + 网络 NETCONF 配置）*/
    g_apps[7] = (app_descriptor){
        "settings", "Settings", 920, 640, 0, 0, 0, 0, 0
    };
    g_apps[7].elf_path = "system/deshab64/tools/settings.elf";
    g_app_count++;
    /* VSCode Phase 5: Linux guest 应用（VSCode 等），来自 LINUXAPP.CNF */
    register_linux_apps();
    /* P5d: Windows PE 应用，来自 PEAPPS.CNF */
    register_pe_apps();
    register_user_desktop_icons();
    /* M3: 开始菜单注册（STARTMENU.CNF；须在全部来源注册后跑以支持 builtin:/path 复用） */
    register_startmenu_apps();
    register_guest_menus();
}

/* M2: 桌面图标 = user/desktop/*.lnk 用户快捷方式（user_desktop 标记）。
 * 内置应用（editor/calc/settings…）不落桌面——经任务栏/开始菜单启动。
 * 布局沿用 canvas padding 24px，列宽 104px，gap 18px，左→右换行。 */
static void place_desktop_icon(int app_id, int *x, int *y, int pad, int max_x) {
    if (g_icon_count >= MAX_ICONS) return;
    if (app_id < 0 || app_id >= g_app_count) return;
    if (*x > pad && *x + KATE_ICON_W > max_x) {
        *x = pad;
        *y += KATE_ICON_H + KATE_ICON_GAP;
    }
    g_icons[g_icon_count].app_id = app_id;
    g_icons[g_icon_count].x = *x;
    g_icons[g_icon_count].y = *y;
    g_icons[g_icon_count].label = g_apps[app_id].display_name;
    g_icons[g_icon_count].selected = 0;
    g_icon_count++;
    *x += KATE_ICON_W + KATE_ICON_GAP;
}

/* Aqua：图标从右上角（顶栏下方）自上而下排一列，满列向左另起一列 */
static void place_desktop_icon_aqua(int app_id, int *x, int *y) {
    int top = AQUA_MENUBAR_H + 12;
    int bot = (int)g_fb_h - AQUA_DOCK_RESERVE;
    if (g_icon_count >= MAX_ICONS) return;
    if (app_id < 0 || app_id >= g_app_count) return;
    if (*y > top && *y + KATE_ICON_H > bot) {
        *y = top;
        *x -= KATE_ICON_W;
    }
    g_icons[g_icon_count].app_id = app_id;
    g_icons[g_icon_count].x = *x;
    g_icons[g_icon_count].y = *y;
    g_icons[g_icon_count].label = g_apps[app_id].display_name;
    g_icons[g_icon_count].selected = 0;
    g_icon_count++;
    *y += KATE_ICON_H + 6;
}

void setup_desktop_icons(void) {
    int pad = KATE_ICON_PAD;                        /* 24 */
    int x = pad;
    int y = KATE_TOPBAR_H + KATE_PAGE_PAD + pad;
    int max_x = (int)g_fb_w - pad;                  /* canvas 右缘 */
    int k;
    if (g_fluent) {
        x = (int)g_fb_w - 12 - KATE_ICON_W;
        y = AQUA_MENUBAR_H + 12;
        if (g_desk_n > 0) {
            for (k = 0; k < g_desk_n; k++)
                place_desktop_icon_aqua(g_desk_order[k], &x, &y);
            return;
        }
        for (int i = 0; i < g_app_count; i++)
            if (g_apps[i].user_desktop) place_desktop_icon_aqua(i, &x, &y);
        return;
    }
    if (g_desk_n > 0) {
        for (k = 0; k < g_desk_n; k++)
            place_desktop_icon(g_desk_order[k], &x, &y, pad, max_x);
        return;
    }
    for (int i = 0; i < g_app_count; i++) {
        if (g_icon_count >= MAX_ICONS) break;
        if (!g_apps[i].user_desktop) continue;      /* M2: 只显示 .lnk 快捷方式 */
        if (x > pad && x + KATE_ICON_W > max_x) {
            x = pad;
            y += KATE_ICON_H + KATE_ICON_GAP;
        }
        g_icons[g_icon_count].app_id = i;
        g_icons[g_icon_count].x = x;
        g_icons[g_icon_count].y = y;
        g_icons[g_icon_count].label = g_apps[i].display_name;
        g_icons[g_icon_count].selected = 0;
        g_icon_count++;
        x += KATE_ICON_W + KATE_ICON_GAP;
    }
}
