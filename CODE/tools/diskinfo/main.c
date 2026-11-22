/* diskinfo — Deshab 磁盘工具（diskpart 风格，ELF 全屏工具）
 *
 * 展示：block 设备清单、GPT 分区表（类型/起始/大小）、ESP FAT32
 * 容量统计（总簇/空闲簇/已用%）。只读，不写盘。
 *
 * 构建: cd CODE/tools/diskinfo ; make
 * 输出: SYSTEM/bin/diskinfo.elf（开始菜单 bin/xxx.elf 条目启动）
 */
#include "../../firstInit/ascii_bitmaps.c"
#include "../../sdk/include/deshab/deshab.h"
#include "../../sdk/include/deshab/font.h"
#include "../../tools/fat32_part.h"

static dsb_app_context g_ac;

/* ---- 简易格式化 ---- */
static char *fmt_dec(u64 v, char *buf) {
    char tmp[24]; int n = 0;
    if (!v) { buf[0] = '0'; buf[1] = 0; return buf; }
    while (v && n < 23) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return buf;
}
/* 拼接：a " / " b */
static char *fmt_pair(u64 a, u64 b, char *buf) {
    char t1[24], t2[24];
    int i = 0;
    const char *s = fmt_dec(a, t1);
    while (s[i] && i < 40) { buf[i] = s[i]; i++; }
    buf[i++] = ' '; buf[i++] = '/'; buf[i++] = ' ';
    const char *s2 = fmt_dec(b, t2);
    int j = 0;
    while (s2[j] && i < 44) buf[i++] = s2[j++];
    buf[i] = 0;
    return buf;
}

#define INFO_X        12
#define INFO_Y_START  (DSB_TITLEBAR_H + 8)
#define LINE_H        20
#define COL_LABEL_W   190

static int g_line = 0;

static void info_line(const char *label, const char *value, u32 val_color) {
    i64 y = INFO_Y_START + (i64)g_line * LINE_H;
    dsb_draw_string(&g_ac, label, INFO_X, y, DSB_TEXT_DIM, DSB_BG_PRIMARY, 12);
    dsb_draw_string(&g_ac, value, INFO_X + COL_LABEL_W, y, val_color, DSB_BG_PRIMARY, 12);
    g_line++;
}
static void info_section(const char *title) {
    i64 y = INFO_Y_START + (i64)g_line * LINE_H;
    dsb_draw_string(&g_ac, title, INFO_X, y, DSB_ACCENT_LIGHT, DSB_BG_PRIMARY, 12);
    dsb_fill_rect(&g_ac, INFO_X, y + 16, (i64)g_ac.fb_w - INFO_X * 2, 1, DSB_BORDER);
    g_line += 2;
}

/* ---- 静态读缓冲 ---- */
static u8 g_sec[512];          /* MBR / GPT 头 / BPB */
static u8 g_ptes[16384];       /* GPT 分区项（128 项 × 128B = 16KB） */
static u8 g_fat[65536];        /* FAT 扫描块（128 扇区） */

static u64 rd64(const u8 *p) {
    return (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) | ((u64)p[3] << 24)
         | ((u64)p[4] << 32) | ((u64)p[5] << 40) | ((u64)p[6] << 48)
         | ((u64)p[7] << 56);
}
static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* block read 直通（fat32_part_probe 回调签名） */
static int blk_read(u32 index, u64 lba, u32 count, void *buf) {
    return g_ac.block.read ? g_ac.block.read(index, lba, count, buf) : -1;
}

void dsk_entry(const dsk_boot_context *ctx) {
    if (ctx->magic != DSK_BOOT_MAGIC) return;
    dsb_app_init(&g_ac, ctx);
    char buf[64];

    /* 输入经服务总线（P6b）：ps2kbd.drv/ps2mouse.drv 的 IRQ handler 已读走
     * 端口 0x60，直接轮询必然饿死（鼠标不动 / Esc 失灵）。改用
     * srv.input.kbd.poll / srv.input.mouse.poll（与 shell/cmd 同款）。 */
    static dsb_input g_in;
    static dsb_cursor g_cur;
    if (dsb_input_open(&g_in, (const dkm_kernel_api *)ctx->dkm_kernel_api) != 0) {
        /* kbd 服务缺席 = 无法交互；画提示后归还桌面（零降级：不碰端口）。 */
        dsb_fill_bg(&g_ac, DSB_BG_PRIMARY);
        dsb_draw_titlebar(&g_ac, "DiskInfo - input unavailable", (i64)g_ac.fb_w);
        dsb_draw_string(&g_ac, "srv.input.kbd.poll missing; press any key to exit",
                        12, 60, DSB_ERROR, DSB_BG_PRIMARY, 12);
        return;
    }
    dsb_cursor_init(&g_cur, (i64)g_ac.fb_w, (i64)g_ac.fb_h);

    dsb_fill_bg(&g_ac, DSB_BG_PRIMARY);
    dsb_draw_titlebar(&g_ac, "DiskInfo - diskpart style", (i64)g_ac.fb_w);
    dsb_draw_statusbar(&g_ac, "read-only tool, Esc to exit",
                       (i64)g_ac.fb_w, (i64)g_ac.fb_h);

    /* ---- Block 设备 ---- */
    const dkm_kernel_api *api = g_ac.api;
    if (!api || !api->block || !api->block->device_count) {
        info_section("== Block Devices ==");
        info_line("block api", "unavailable", DSB_ERROR);
        goto wait_esc;
    }
    {
        u32 ndev = api->block->device_count();
        info_section("== Block Devices ==");
        info_line("device_count", fmt_dec(ndev, buf), DSB_TEXT_PRIMARY);
        for (u32 i = 0; i < ndev && i < 4; i++) {
            char label[16];
            label[0] = 'd'; label[1] = 'e'; label[2] = 'v'; label[3] = '[';
            label[4] = (char)('0' + i); label[5] = ']'; label[6] = 0;
            const char *name = api->block->device_name
                               ? api->block->device_name(i) : "?";
            info_line(label, name ? name : "?", DSB_ACCENT_LIGHT);
            if (api->block->sector_size)
                info_line("  sector_size", fmt_dec(api->block->sector_size(i), buf),
                          DSB_TEXT_PRIMARY);
            if (api->block->sector_count)
                info_line("  sector_count", fmt_dec(api->block->sector_count(i), buf),
                          DSB_TEXT_PRIMARY);
        }
    }

    /* ---- GPT 分区表 ---- */
    info_section("== GPT Partitions (disk 0) ==");
    if (blk_read(0, 1, 1, g_sec) != 0) {
        info_line("read LBA1", "failed", DSB_ERROR);
        goto wait_esc;
    }
    if (!(g_sec[0]=='E' && g_sec[1]=='F' && g_sec[2]=='I' && g_sec[3]==' ' &&
          g_sec[4]=='P' && g_sec[5]=='A' && g_sec[6]=='R' && g_sec[7]=='T')) {
        info_line("signature", "not GPT (MBR raw?)", DSB_WARNING);
        goto wait_esc;
    }
    {
        u64 pte_lba = rd64(g_sec + 72);
        u32 nent = rd32(g_sec + 80);
        u32 esz = rd32(g_sec + 84);
        if (esz == 0 || esz > 512) esz = 128;
        if (nent > 128) nent = 128;
        info_line("entry_lba", fmt_dec(pte_lba, buf), DSB_TEXT_PRIMARY);
        info_line("entries", fmt_dec(nent, buf), DSB_TEXT_PRIMARY);
        /* 读 128 项 × 128B = 32 扇区 */
        if (blk_read(0, pte_lba, 32, g_ptes) != 0) {
            info_line("read PTEs", "failed", DSB_ERROR);
            goto wait_esc;
        }
        int shown = 0;
        for (u32 i = 0; i < nent; i++) {
            const u8 *e = g_ptes + (u64)i * esz;
            int used = 0;
            for (int k = 0; k < 16; k++) if (e[k]) { used = 1; break; }
            if (!used) continue;
            u64 first = rd64(e + 32), last = rd64(e + 40);
            int is_esp = 1;
            for (int k = 0; k < 16; k++)
                if (e[k] != fat32_part_esp_guid[k]) { is_esp = 0; break; }
            char lb[24];
            lb[0] = 'p'; lb[1] = (char)('1' + i);
            lb[2] = is_esp ? '*' : ' ';
            lb[3] = 0;
            /* 类型 GUID 前 4 字节 + 起始 LBA */
            char tv[20];
            static const char hx[] = "0123456789ABCDEF";
            tv[0] = 't'; tv[1] = 'y'; tv[2] = 'p'; tv[3] = 'e'; tv[4] = '=';
            for (int k = 0; k < 4; k++) {
                tv[5 + k * 2] = hx[e[k] >> 4];
                tv[6 + k * 2] = hx[e[k] & 0xF];
            }
            tv[13] = 0;
            info_line(lb, tv, is_esp ? DSB_ACCENT_LIGHT : DSB_TEXT_PRIMARY);
            info_line("  first_lba", fmt_dec(first, buf), DSB_TEXT_DIM);
            info_line("  sectors", fmt_dec(last - first + 1, buf), DSB_TEXT_DIM);
            shown++;
            if (shown >= 8) break;
        }
        if (!shown) info_line("(no partitions)", "", DSB_TEXT_DIM);
    }

    /* ---- ESP FAT32 容量 ---- */
    info_section("== FAT32 Volume (ESP) ==");
    {
        u64 base = fat32_part_probe(blk_read, 0);
        /* probe 语义：0 = superfloppy（BPB 在 LBA0）或失败；>0 = ESP LBA */
        if (blk_read(0, base, 1, g_sec) != 0) {
            info_line("read BPB", "failed", DSB_ERROR);
            goto wait_esc;
        }
        if (!fat32_part_bpb_ok(g_sec)) {
            info_line("BPB", "invalid", DSB_ERROR);
            goto wait_esc;
        }
        u32 spc = g_sec[13];
        u32 rsv = (u32)g_sec[14] | ((u32)g_sec[15] << 8);
        u32 nfat = g_sec[16];
        u32 spf = rd32(g_sec + 36);          /* BPB_FATSz32 */
        u32 rootc = rd32(g_sec + 44);        /* BPB_RootClus */
        u64 tot = rd32(g_sec + 32);          /* BPB_TotSec32 */
        u64 data_sec = rsv + (u64)nfat * spf;
        u64 nclus = (tot > data_sec) ? (tot - data_sec) / spc : 0;

        {
            char lb[16];
            for (int i = 0; i < 11; i++) lb[i] = (char)g_sec[71 + i];
            lb[11] = 0;
            info_line("label", lb, DSB_ACCENT_LIGHT);
        }
        info_line("bytes/sector", "512", DSB_TEXT_PRIMARY);
        info_line("sectors/cluster", fmt_dec(spc, buf), DSB_TEXT_PRIMARY);
        info_line("FAT size(sec x n)", fmt_pair(spf, nfat, buf), DSB_TEXT_PRIMARY);
        info_line("root_cluster", fmt_dec(rootc, buf), DSB_TEXT_PRIMARY);
        info_line("total clusters", fmt_dec(nclus, buf), DSB_TEXT_PRIMARY);

        /* 空闲簇扫描：FAT 区按 64KB 块读，统计 (i>=2 && entry==0) */
        if (spf) {
            u64 fat_lba = base + rsv;
            u64 fat_ent = (u64)spf * 512 / 4;
            if (fat_ent > nclus + 2) fat_ent = nclus + 2;
            u64 free_c = 0;
            u32 chunk_sec = sizeof(g_fat) / 512;       /* 128 扇区/块 */
            u32 ents_per_chunk = chunk_sec * 128;      /* FAT 项/块 */
            u64 chunks = (fat_ent + ents_per_chunk - 1) / ents_per_chunk;
            for (u64 ch = 0; ch < chunks; ch++) {
                if (blk_read(0, fat_lba + ch * chunk_sec, chunk_sec, g_fat) != 0)
                    break;
                for (u32 k = 0; k < ents_per_chunk; k++) {
                    u64 idx = ch * ents_per_chunk + k;
                    if (idx < 2 || idx >= fat_ent) continue;
                    u32 v = rd32(g_fat + k * 4) & 0x0FFFFFFFu;
                    if (v == 0) free_c++;
                }
            }
            info_line("free clusters", fmt_dec(free_c, buf), DSB_SUCCESS);
            if (nclus) {
                u64 used_pct = (nclus - free_c) * 100 / nclus;
                info_line("used", fmt_pair(used_pct, 100, buf), DSB_WARNING);
            }
        }
    }

wait_esc:
    /* 光标就位（UI 绘完后保存背景 + 画一次） */
    dsb_cursor_save(&g_ac, &g_cur);
    dsb_cursor_draw(&g_ac, &g_cur, DSB_ACCENT);

    /* 输入循环：鼠标经总线快照（驱动已按 fb 钳位、Y 方向正确）；
     * 键盘 Esc 退出。绝不碰端口 0x60 —— 与 IRQ handler 抢字节会错位。 */
    for (;;) {
        if (dsb_input_mouse(&g_in, &g_cur, (i64)g_ac.fb_w, (i64)g_ac.fb_h)) {
            dsb_cursor_restore(&g_ac, &g_cur);
            dsb_cursor_save(&g_ac, &g_cur);
            dsb_cursor_draw(&g_ac, &g_cur, DSB_ACCENT);
        }
        u8 sc;
        if (dsb_input_kbd(&g_in, &sc)) {
            if (sc == 1) break;           /* Esc make code */
            continue;
        }
        __asm__ volatile("pause");
    }
}
