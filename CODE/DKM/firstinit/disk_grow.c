/* firstinit 磁盘扩张。
 *
 * 打包器把整盘切成 25GiB：ESP 7GiB、DATA 3GiB、WIN 拿走剩下的约 15GiB
 * （GPT 前后各留 1MiB）。镜像落到更大的物理盘时，三个分区按各自当前
 * 扇区数的比例占满可用空间，再把 FAT32 / ext4 / NTFS 扩到新分区尾部。
 * 比例取自盘上的 GPT，不把 7 和 3 写死；QEMU 里盘与镜像一样大时直接返回。
 *
 * ESP 镜像是 2KiB 簇。簇号是 28 位，超过约 512GiB 时把每簇扇区数成倍加大
 * （最大 128，目录项里的簇号不变）。BPB 总扇区是 32 位，FAT32 卷本身到 2TiB。
 * DATA 不是 64bit，块数到 2^32（16TiB）。组描述符放不下时，把挡在 GDT
 * 增长方向上的块搬进旧卷的空闲区，不依赖 mkfs 的 resize_inode 预留。
 *
 * 失败即 panic（FIRSTI-E10..E19）。已经扩满、或找不到 Deshab 三分区时
 * 只记日志，向导继续。
 */

#include "../dkm_shared.h"

void firstinit_disk_grow(const struct dkm_kernel_api *api,
                         const struct dkm_log_api *log,
                         void (*status)(const char *line));

static const struct dkm_kernel_api *g_api;
static const struct dkm_log_api *g_log;
static void (*g_status)(const char *line);

enum {
    DG_ALIGN = 2048,          /* 1MiB，与打包器 gpt_front 一致 */
    DG_TAIL  = 2048,          /* 尾部留给备份 GPT 的空隙 */
    DG_CHUNK = 32,            /* 每次搬运的扇区数 */
    DG_MAX_RUNS = 8
};

static u8 g_io[DG_CHUNK * 512];
static u8 g_blk[4096];
static u8 g_bm[4096];
static u8 g_ino[4096];
static u8 g_sb[1024];
static u8 *g_gdt;
static u32 g_gdt_cap;
static u8 g_ent[16384];
static u8 g_rec[4096];
static u8 g_fatsec[512];
static u32 g_fatsec_no = 0xffffffffu;

static void dg_panic(const char *sym) __attribute__((noreturn));
static void dg_panic(const char *sym) {
    if (g_status) g_status(sym);
    if (g_log && g_log->panic) g_log->panic(sym);
    for (;;) __asm__ volatile("cli; hlt");
}

static void dg_info(const char *s) {
    if (g_log && g_log->info) g_log->info(s);
}

static void dg_note(const char *s) {
    dg_info(s);
    if (g_status) g_status(s);
}

static u16 dg_r16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 dg_r32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 dg_r64(const u8 *p) { return (u64)dg_r32(p) | ((u64)dg_r32(p + 4) << 32); }
static void dg_w16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void dg_w32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static void dg_w64(u8 *p, u64 v) { dg_w32(p, (u32)v); dg_w32(p + 4, (u32)(v >> 32)); }

static void dg_zero(u8 *p, u32 n) { for (u32 i = 0; i < n; i++) p[i] = 0; }
static void dg_copy(u8 *d, const u8 *s, u32 n) { for (u32 i = 0; i < n; i++) d[i] = s[i]; }

static void dg_need_gdt(u32 bytes) {
    struct dkm_dma_buffer b;
    u64 pages;
    if (bytes < 4096u) bytes = 4096u;
    if (g_gdt && g_gdt_cap >= bytes) return;
    pages = ((u64)bytes + 4095ull) / 4096ull;
    if (!g_api->dma || !g_api->dma->alloc_pages ||
        g_api->dma->alloc_pages(pages, 4096, 0, &b) != 0 || !b.virt || b.size < bytes)
        dg_panic("FIRSTI-E15 EXT4 GDT ALLOC");
    g_gdt = b.virt;
    g_gdt_cap = b.size > 0xffffffffull ? 0xffffffffu : (u32)b.size;
    for (u32 i = 0; i < bytes; i++) g_gdt[i] = 0;
}

static int dg_rd(u32 dev, u64 lba, u32 n, void *buf) {
    return g_api->block->read(dev, lba, n, buf);
}
static int dg_wr(u32 dev, u64 lba, u32 n, const void *buf) {
    return g_api->block->write(dev, lba, n, buf);
}
static void dg_rd_or(u32 dev, u64 lba, u32 n, void *buf) {
    if (dg_rd(dev, lba, n, buf) != 0) dg_panic("FIRSTI-E10 GROW READ FAILED");
}
static void dg_wr_or(u32 dev, u64 lba, u32 n, const void *buf) {
    if (dg_wr(dev, lba, n, buf) != 0) dg_panic("FIRSTI-E17 GROW WRITE FAILED");
}

static u32 dg_crc32(const u8 *p, u32 n) {
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
    }
    return ~c;
}

/* Linux lib/crc16.c：反射多项式 0xA001，调用方传入初值。 */
static u16 dg_crc16(u16 crc, const u8 *p, u32 n) {
    for (u32 i = 0; i < n; i++) {
        crc = (u16)(crc ^ p[i]);
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (u16)((crc >> 1) ^ 0xA001) : (u16)(crc >> 1);
    }
    return crc;
}

/* (a * b) / d。商超出 u64 时返回全 1，调用方不得把它当扇区数用。 */
static void dg_mul64(u64 a, u64 b, u64 *hi, u64 *lo) {
    u64 a0 = a & 0xffffffffull, a1 = a >> 32;
    u64 b0 = b & 0xffffffffull, b1 = b >> 32;
    u64 p0 = a0 * b0;
    u64 p1 = a0 * b1;
    u64 p2 = a1 * b0;
    u64 p3 = a1 * b1;
    u64 mid = (p0 >> 32) + (p1 & 0xffffffffull) + (p2 & 0xffffffffull);
    *lo = (p0 & 0xffffffffull) | (mid << 32);
    *hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}
static u64 dg_mul_div(u64 a, u64 b, u64 d) {
    u64 hi, lo, q = 0, r;
    if (d == 0) return ~0ull;
    dg_mul64(a, b, &hi, &lo);
    if (hi >= d) return ~0ull;
    r = hi;
    for (int i = 63; i >= 0; i--) {
        u64 overflow = r >> 63;
        r = (r << 1) | ((lo >> i) & 1ull);
        q <<= 1;
        if (overflow || r >= d) {
            r -= d;
            q |= 1ull;
        }
    }
    return q;
}

static int dg_name_is(const u8 *ent, const char *s) {
    int n = 0;
    while (s[n]) {
        if (ent[0x38 + n * 2] != (u8)s[n] || ent[0x39 + n * 2] != 0) return 0;
        n++;
    }
    return ent[0x38 + n * 2] == 0 && ent[0x39 + n * 2] == 0;
}

static int dg_guid_eq(const u8 *a, const u8 *b) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* 与 fat32.drv 相同的 boot module 视图，只为取出启动盘 GPT UUID。 */
struct dg_mod_rsp { u64 revision; u64 module_count; void **modules; };
struct dg_mod_file {
    u64 revision; void *address; u64 size; char *path; char *cmdline;
    u32 media_type; u32 unused; u32 tftp_ip; u32 tftp_port;
    u32 partition_index; u32 mbr_disk_id;
    u8 gpt_disk_uuid[16]; u8 gpt_part_uuid[16]; u8 part_uuid[16];
};

static const u8 *dg_boot_uuid(void) {
    const struct dg_mod_rsp *rsp;
    if (!g_api->boot_modules_response) return 0;
    rsp = (const struct dg_mod_rsp *)g_api->boot_modules_response;
    if (!rsp->modules) return 0;
    for (u64 i = 0; i < rsp->module_count; i++) {
        const struct dg_mod_file *f = (const struct dg_mod_file *)rsp->modules[i];
        int any = 0;
        if (!f) continue;
        for (int k = 0; k < 16; k++) if (f->gpt_disk_uuid[k]) any = 1;
        if (any) return f->gpt_disk_uuid;
    }
    return 0;
}

struct dg_part { u64 start, size; u32 slot; };
struct dg_disk {
    u32 dev;
    u32 entries, ent_size, ent_secs, hdr_size;
    u64 first_usable, last_usable, alternate, ent_lba;
    struct dg_part esp, data, win;
};

static int dg_parse_disk(u32 dev, struct dg_disk *out) {
    u8 hdr[512];
    u32 ent_secs, num, esz, slot;
    int saw_esp = 0, saw_data = 0, saw_win = 0;
    if (dg_rd(dev, 1, 1, hdr) != 0) return 0;
    if (hdr[0] != 'E' || hdr[1] != 'F' || hdr[2] != 'I' || hdr[3] != ' ') return 0;
    num = dg_r32(hdr + 0x50);
    esz = dg_r32(hdr + 0x54);
    if (num == 0 || num > 128 || esz != 128) return 0;
    ent_secs = (num * esz + 511u) / 512u;
    if (ent_secs == 0 || ent_secs > 32) return 0;
    if (dg_rd(dev, dg_r64(hdr + 0x48), ent_secs, g_ent) != 0) return 0;
    for (slot = 0; slot < num; slot++) {
        const u8 *e = g_ent + slot * esz;
        u64 start = dg_r64(e + 0x20);
        u64 end = dg_r64(e + 0x28);
        int empty = 1;
        if (start == 0 || end < start) continue;
        for (int i = 0; i < 16; i++) if (e[i]) empty = 0;
        if (empty) continue;
        if (dg_name_is(e, "DESHABBOOT")) {
            out->esp.start = start; out->esp.size = end - start + 1; out->esp.slot = slot;
            saw_esp = 1;
        } else if (dg_name_is(e, "DESHABWIN")) {
            out->win.start = start; out->win.size = end - start + 1; out->win.slot = slot;
            saw_win = 1;
        } else if (dg_name_is(e, "DESHAB")) {
            out->data.start = start; out->data.size = end - start + 1; out->data.slot = slot;
            saw_data = 1;
        }
    }
    if (!saw_esp || !saw_data || !saw_win) return 0;
    out->dev = dev;
    out->entries = num;
    out->ent_size = esz;
    out->ent_secs = ent_secs;
    out->hdr_size = dg_r32(hdr + 0x0C);
    out->first_usable = dg_r64(hdr + 0x28);
    out->last_usable = dg_r64(hdr + 0x30);
    out->alternate = dg_r64(hdr + 0x20);
    out->ent_lba = dg_r64(hdr + 0x48);
    if (out->ent_lba == 0 || out->hdr_size < 92 || out->hdr_size > 512) return 0;
    return 1;
}

static int dg_find_disk(struct dg_disk *out) {
    const u8 *uuid = dg_boot_uuid();
    u32 n, i;
    int found = 0;
    if (!g_api->block || !g_api->block->read || !g_api->block->write ||
        !g_api->block->device_count || !g_api->block->sector_size)
        return 0;
    n = g_api->block->device_count();
    for (i = 0; i < n; i++) {
        struct dg_disk cand;
        u8 hdr[512];
        if (g_api->block->sector_size(i) != 512) continue;
        if (!dg_parse_disk(i, &cand)) continue;
        if (uuid) {
            if (dg_rd(i, 1, 1, hdr) != 0) continue;
            if (!dg_guid_eq(hdr + 56, uuid)) continue;
        }
        *out = cand;
        found = 1;
        break;
    }
    return found;
}

static void dg_progress(const char *what, u64 done, u64 total) {
    char b[64];
    u32 i = 0, pct;
    const char *s = what;
    while (*s && i < 40) b[i++] = *s++;
    b[i++] = ' ';
    pct = total ? (u32)((done * 100ull) / total) : 100u;
    if (pct > 100u) pct = 100u;
    if (pct >= 100u) b[i++] = '1', b[i++] = '0', b[i++] = '0';
    else if (pct >= 10u) { b[i++] = (char)('0' + pct / 10u); b[i++] = (char)('0' + pct % 10u); }
    else b[i++] = (char)('0' + pct);
    b[i++] = '%';
    b[i] = 0;
    if (g_status) g_status(b);
}

static void dg_copy_range(u32 dev, u64 dst, u64 src, u64 sectors, const char *what) {
    u64 done = 0;
    if (sectors == 0 || dst == src) return;
    if (dst < src) dg_panic("FIRSTI-E18 PARTITION MOVE BACKWARD");
    while (done < sectors) {
        u64 left = sectors - done;
        u32 n = left > DG_CHUNK ? DG_CHUNK : (u32)left;
        u64 off = sectors - done - n;
        dg_rd_or(dev, src + off, n, g_io);
        dg_wr_or(dev, dst + off, n, g_io);
        done += n;
        if ((done & ~65535ull) != ((done - n) & ~65535ull) || done == sectors)
            dg_progress(what, done, sectors);
    }
}

static void dg_write_gpt(struct dg_disk *d, u64 esp_sz, u64 data_sz, u64 win_sz, u64 sectors) {
    u8 hdr[512];
    u32 crc;
    u64 ent_lba, backup_lba, last_usable;
    u64 esp_lba, data_lba, win_lba;
    dg_rd_or(d->dev, 1, 1, hdr);
    esp_lba = d->esp.start;
    data_lba = esp_lba + esp_sz;
    win_lba = data_lba + data_sz;
    backup_lba = sectors - 1;
    ent_lba = backup_lba - d->ent_secs;
    last_usable = ent_lba - 1;
    if (win_lba + win_sz - 1 > last_usable) dg_panic("FIRSTI-E12 PARTITION PAST DISK");
    dg_w64(g_ent + d->esp.slot * 128 + 0x20, esp_lba);
    dg_w64(g_ent + d->esp.slot * 128 + 0x28, esp_lba + esp_sz - 1);
    dg_w64(g_ent + d->data.slot * 128 + 0x20, data_lba);
    dg_w64(g_ent + d->data.slot * 128 + 0x28, data_lba + data_sz - 1);
    dg_w64(g_ent + d->win.slot * 128 + 0x20, win_lba);
    dg_w64(g_ent + d->win.slot * 128 + 0x28, win_lba + win_sz - 1);
    crc = dg_crc32(g_ent, d->entries * d->ent_size);

    dg_w64(hdr + 0x18, 1);
    dg_w64(hdr + 0x20, backup_lba);
    dg_w64(hdr + 0x28, d->first_usable);
    dg_w64(hdr + 0x30, last_usable);
    dg_w64(hdr + 0x48, d->ent_lba);
    dg_w32(hdr + 0x58, crc);
    dg_w32(hdr + 0x10, 0);
    dg_w32(hdr + 0x10, dg_crc32(hdr, d->hdr_size));
    dg_wr_or(d->dev, d->ent_lba, d->ent_secs, g_ent);
    dg_wr_or(d->dev, 1, 1, hdr);

    dg_w64(hdr + 0x18, backup_lba);
    dg_w64(hdr + 0x20, 1);
    dg_w64(hdr + 0x48, ent_lba);
    dg_w32(hdr + 0x10, 0);
    dg_w32(hdr + 0x10, dg_crc32(hdr, d->hdr_size));
    dg_wr_or(d->dev, ent_lba, d->ent_secs, g_ent);
    dg_wr_or(d->dev, backup_lba, 1, hdr);

    dg_rd_or(d->dev, 0, 1, g_blk);
    if (g_blk[510] == 0x55 && g_blk[511] == 0xAA && g_blk[450] == 0xEE) {
        u64 psz = sectors > 1 ? sectors - 1 : 0;
        if (psz > 0xffffffffull) psz = 0xffffffffull;
        dg_w32(g_blk + 454, 1);
        dg_w32(g_blk + 458, (u32)psz);
        dg_wr_or(d->dev, 0, 1, g_blk);
    }
}

/* ---------------- FAT32 ---------------- */

static u32 dg_fat_secs(u32 reserved, u32 fats, u32 spc, u32 total) {
    u32 fat = 1;
    for (int n = 0; n < 16; n++) {
        u32 data, clusters, need;
        if (reserved + fats * fat >= total) return 0;
        data = reserved + fats * fat;
        clusters = (total - data) / spc;
        need = ((clusters + 2u) * 4u + 511u) / 512u;
        if (need < 1) need = 1;
        if (need <= fat) return fat;
        fat = need;
    }
    return fat;
}

static int dg_is_pow2(u32 v) {
    return v && (v & (v - 1u)) == 0;
}

static u32 dg_fat_entry(u32 dev, u64 part, u32 reserved, u32 idx) {
    u32 sec = idx / 128u;
    if (g_fatsec_no != sec) {
        dg_rd_or(dev, part + reserved + sec, 1, g_fatsec);
        g_fatsec_no = sec;
    }
    return dg_r32(g_fatsec + (idx % 128u) * 4u) & 0x0FFFFFFFu;
}

/* dst >= src。从高地址往低地址搬，簇变大时新区间会盖住旧区间的尾巴。 */
static void dg_copy_secs(u32 dev, u64 dst, u64 src, u32 n) {
    u32 off = n;
    if (n == 0) return;
    if (n <= DG_CHUNK) {
        dg_rd_or(dev, src, n, g_io);
        dg_wr_or(dev, dst, n, g_io);
        return;
    }
    while (off > 0) {
        u32 c = off > DG_CHUNK ? DG_CHUNK : off;
        off -= c;
        dg_rd_or(dev, src + off, c, g_io);
        dg_wr_or(dev, dst + off, c, g_io);
    }
}

static void dg_zero_secs(u32 dev, u64 dst, u32 n) {
    u32 off = 0;
    while (off < n) {
        u32 c = n - off;
        if (c > DG_CHUNK) c = DG_CHUNK;
        dg_zero(g_io, c * 512u);
        dg_wr_or(dev, dst + off, c, g_io);
        off += c;
    }
}

/* 簇号不变，只把每个已用簇的数据挪到更大簇的开头，其余字节清零。
 * 从高簇往低簇搬：new_spc >= old_spc 且 new_data >= old_data 时，
 * 新位置不会压到还没搬的旧簇。 */
static void dg_stretch_clusters(u32 dev, u64 part, u32 reserved, u32 old_data, u32 new_data,
                                u32 old_spc, u32 new_spc, u32 old_clusters) {
    g_fatsec_no = 0xffffffffu;
    for (u32 n = old_clusters; n > 0; n--) {
        u32 c = n + 1u;
        u64 old_lba, new_lba;
        if (dg_fat_entry(dev, part, reserved, c) == 0) continue;
        old_lba = part + old_data + (u64)(c - 2u) * old_spc;
        new_lba = part + new_data + (u64)(c - 2u) * new_spc;
        dg_copy_secs(dev, new_lba, old_lba, old_spc);
        if (new_spc > old_spc) dg_zero_secs(dev, new_lba + old_spc, new_spc - old_spc);
        if ((n & 8191u) == 1u || n == old_clusters)
            dg_progress("Widening ESP clusters", old_clusters - n + 1u, old_clusters);
    }
}

static int dg_grow_fat32(u32 dev, u64 part, u64 part_secs) {
    u8 bpb[512];
    u32 bps, spc, spc_new, reserved, fats, old_fat, old_total, new_total;
    u32 old_data, new_fat, new_data, old_clusters, new_clusters, shift, keep;
    u32 fsinfo, backup, fs_total;
    u64 data_len;
    dg_rd_or(dev, part, 1, bpb);
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) dg_panic("FIRSTI-E13 FAT32 BPB BAD");
    bps = dg_r16(bpb + 11);
    spc = bpb[13];
    reserved = dg_r16(bpb + 14);
    fats = bpb[16];
    old_total = dg_r32(bpb + 32);
    old_fat = dg_r32(bpb + 36);
    if (bps != 512 || !dg_is_pow2(spc) || spc < 1 || spc > 128 || fats < 1 || fats > 2 ||
        reserved < 1 || old_fat == 0 || old_total < reserved)
        dg_panic("FIRSTI-E13 FAT32 GEOMETRY");
    if ((u32)spc * bps > 64u * 1024u) dg_panic("FIRSTI-E13 FAT32 CLUSTER TOO BIG");
    fs_total = part_secs > 0xffffffffull ? 0xffffffffu : (u32)part_secs;
    if (old_total >= fs_total) return 0;
    spc_new = spc;
    old_data = 0;
    new_fat = 0;
    new_data = 0;
    old_clusters = 0;
    new_clusters = 0;
    for (;;) {
        new_fat = dg_fat_secs(reserved, fats, spc_new, fs_total);
        if (new_fat < old_fat) new_fat = old_fat;
        if (new_fat == 0) dg_panic("FIRSTI-E13 FAT32 FAT SIZE");
        old_data = reserved + fats * old_fat;
        new_data = reserved + fats * new_fat;
        if (old_data > old_total || new_data >= fs_total) dg_panic("FIRSTI-E13 FAT32 LAYOUT");
        old_clusters = (old_total - old_data) / spc;
        new_clusters = (fs_total - new_data) / spc_new;
        if (new_clusters < 0x0FFFFFF0u - 2u && new_clusters >= old_clusters) break;
        if (spc_new >= 128u) dg_panic("FIRSTI-E13 FAT32 FORMAT MAX");
        spc_new *= 2u;
    }
    if (new_data < old_data) dg_panic("FIRSTI-E13 FAT32 FAT SHRANK");
    if ((u64)old_clusters * spc_new + new_data > fs_total) dg_panic("FIRSTI-E13 FAT32 SHIFT");
    new_total = fs_total;

    dg_note("Growing ESP");
    if (spc_new == spc) {
        shift = new_data - old_data;
        data_len = (u64)old_total - old_data;
        if ((u64)old_data + data_len + shift > new_total) dg_panic("FIRSTI-E13 FAT32 SHIFT");
        if (shift && data_len)
            dg_copy_range(dev, part + new_data, part + old_data, data_len, "Moving ESP data");
    } else {
        dg_note("Widening ESP clusters");
        dg_stretch_clusters(dev, part, reserved, old_data, new_data, spc, spc_new, old_clusters);
    }
    bpb[13] = (u8)spc_new;

    keep = old_clusters + 2u;
    for (u32 sec = new_fat; sec > 0; sec--) {
        u32 idx = sec - 1u;
        u32 ent0 = idx * 128u;
        if (idx < old_fat) dg_rd_or(dev, part + reserved + idx, 1, g_io);
        else dg_zero(g_io, 512);
        for (u32 k = 0; k < 128u; k++) {
            if (ent0 + k >= keep) dg_w32(g_io + k * 4u, 0);
        }
        for (u32 copy = 0; copy < fats; copy++)
            dg_wr_or(dev, part + reserved + (u64)copy * new_fat + idx, 1, g_io);
    }

    dg_w32(bpb + 32, new_total);
    dg_w32(bpb + 36, new_fat);
    dg_wr_or(dev, part, 1, bpb);
    backup = dg_r16(bpb + 50);
    if (backup && backup < new_total) dg_wr_or(dev, part + backup, 1, bpb);

    fsinfo = dg_r16(bpb + 48);
    if (fsinfo && fsinfo < new_total) {
        dg_rd_or(dev, part + fsinfo, 1, g_io);
        if (dg_r32(g_io) == 0x41615252u && dg_r32(g_io + 484) == 0x61417272u) {
            u32 free_c = dg_r32(g_io + 488);
            if (free_c != 0xffffffffu && new_clusters >= old_clusters) {
                u32 add = new_clusters - old_clusters;
                free_c = (free_c + add < free_c) ? 0xffffffffu : free_c + add;
            }
            dg_w32(g_io + 488, free_c);
            dg_w32(g_io + 492, old_clusters + 2u);
            dg_wr_or(dev, part + fsinfo, 1, g_io);
        }
    }
    dg_info("[firstinit] ESP filesystem grown");
    return 1;
}

/* ---------------- ext4 ---------------- */

static void dg_rd_blk(u32 dev, u64 part, u32 blk, u8 *dst) {
    dg_rd_or(dev, part + (u64)blk * 8u, 8, dst);
}
static void dg_wr_blk(u32 dev, u64 part, u32 blk, const u8 *src) {
    dg_wr_or(dev, part + (u64)blk * 8u, 8, src);
}

static int dg_pow_of(u32 g, u32 base) {
    u64 p = base;
    while (p < g) {
        if (p > (~0ull / base)) return 0;
        p *= base;
    }
    return p == g;
}
static int dg_has_super(u32 g, u32 ro) {
    if ((ro & 1u) == 0) return 1;
    if (g <= 1u) return 1;
    return dg_pow_of(g, 3) || dg_pow_of(g, 5) || dg_pow_of(g, 7);
}

static void dg_bit_set(u8 *bm, u32 bit) { bm[bit / 8u] = (u8)(bm[bit / 8u] | (u8)(1u << (bit % 8u))); }
static void dg_bit_clr(u8 *bm, u32 bit) { bm[bit / 8u] = (u8)(bm[bit / 8u] & (u8)~(1u << (bit % 8u))); }
static int dg_bit_get(const u8 *bm, u32 bit) { return (bm[bit / 8u] >> (bit % 8u)) & 1; }

static void dg_ext4_checksums(u32 groups, u32 ro) {
    if ((ro & 0x10u) == 0) return;
    for (u32 g = 0; g < groups; g++) {
        u8 *d = g_gdt + g * 32u;
        u8 gl[4];
        u16 c;
        dg_w16(d + 0x1E, 0);
        c = dg_crc16(0xFFFFu, g_sb + 0x68, 16);
        dg_w32(gl, g);
        c = dg_crc16(c, gl, 4);
        c = dg_crc16(c, d, 0x1E);
        dg_w16(d + 0x1E, c);
    }
}

static u32 dg_group_meta(u32 g, u32 ro, u32 gdt_area, u32 it_blocks) {
    u32 n = 2u + it_blocks;
    if (dg_has_super(g, ro)) n += 1u + gdt_area;
    return n;
}

/* 旧卷里、超级块组上，现有 GDT 区之后、新 GDT 要占用的那一段。 */
static u32 ev_bpg, ev_blocks, ev_groups, ev_ro, ev_area, ev_desc;
static u32 ev_bm_blk = 0xffffffffu;

static int ev_in(u32 blk) {
    u32 g, zone, zend, gend;
    if (blk >= ev_blocks || ev_desc <= ev_area) return 0;
    g = blk / ev_bpg;
    if (g >= ev_groups || !dg_has_super(g, ev_ro)) return 0;
    zone = g * ev_bpg + 1u + ev_area;
    zend = g * ev_bpg + 1u + ev_desc;
    gend = g * ev_bpg + ev_bpg;
    if (zend > ev_blocks) zend = ev_blocks;
    if (zend > gend) zend = gend;
    return zone < zend && blk >= zone && blk < zend;
}

static void ev_sync(u32 dev, u64 part) {
    if (ev_bm_blk != 0xffffffffu) dg_wr_blk(dev, part, ev_bm_blk, g_bm);
}

static void ev_load(u32 dev, u64 part, u32 bm) {
    if (ev_bm_blk == bm) return;
    ev_sync(dev, part);
    dg_rd_blk(dev, part, bm, g_bm);
    ev_bm_blk = bm;
}

static int ev_used(u32 dev, u64 part, u32 blk) {
    u32 g = blk / ev_bpg;
    ev_load(dev, part, dg_r32(g_gdt + g * 32u));
    return dg_bit_get(g_bm, blk % ev_bpg);
}

static void ev_touch(u32 dev, u64 part, u32 blk, int used) {
    u32 g = blk / ev_bpg;
    u32 bit = blk % ev_bpg;
    u16 fr;
    u32 sb;
    int cur;
    ev_load(dev, part, dg_r32(g_gdt + g * 32u));
    cur = dg_bit_get(g_bm, bit);
    if (cur == used) {
        if (!used) dg_panic("FIRSTI-E15 EXT4 DOUBLE FREE");
        return;
    }
    if (used) dg_bit_set(g_bm, bit);
    else dg_bit_clr(g_bm, bit);
    fr = dg_r16(g_gdt + g * 32u + 0x0C);
    sb = dg_r32(g_sb + 0x0C);
    if (used) {
        if (fr == 0 || sb == 0) dg_panic("FIRSTI-E15 EXT4 FREE COUNT");
        fr--;
        sb--;
    } else {
        if (fr == 65535u || sb == 0xffffffffu) dg_panic("FIRSTI-E15 EXT4 FREE COUNT");
        fr++;
        sb++;
    }
    dg_w16(g_gdt + g * 32u + 0x0C, fr);
    dg_w32(g_sb + 0x0C, sb);
}

static u32 ev_find(u32 dev, u64 part, u32 len) {
    u32 run = 0, start = 0;
    if (len == 0) dg_panic("FIRSTI-E15 EXT4 NO GDT ROOM");
    for (u32 blk = 0; blk < ev_blocks; blk++) {
        if (ev_in(blk) || ev_used(dev, part, blk)) {
            run = 0;
            continue;
        }
        if (run == 0) start = blk;
        run++;
        if (run == len) return start;
    }
    dg_panic("FIRSTI-E15 EXT4 NO GDT ROOM");
    return 0;
}

static int ev_run_hits(u32 blk, u32 len) {
    for (u32 i = 0; i < len; i++) {
        if (blk + i < blk) dg_panic("FIRSTI-E15 EXT4 BLOCK OWNER");
        if (ev_in(blk + i)) return 1;
    }
    return 0;
}

/* block_bm_field 非空：正在搬的就是这块块位图，标记必须写到新副本上。 */
static u32 ev_reloc(u32 dev, u64 part, u32 src, u32 len, u8 *block_bm_field) {
    u32 dst;
    ev_sync(dev, part);
    dst = ev_find(dev, part, len);
    for (u32 i = 0; i < len; i++) {
        dg_rd_blk(dev, part, src + i, g_io);
        dg_wr_blk(dev, part, dst + i, g_io);
    }
    if (block_bm_field) {
        dg_w32(block_bm_field, dst);
        ev_bm_blk = 0xffffffffu;
    }
    for (u32 i = 0; i < len; i++) ev_touch(dev, part, dst + i, 1);
    for (u32 i = 0; i < len; i++) ev_touch(dev, part, src + i, 0);
    ev_sync(dev, part);
    return dst;
}

static int ev_patch_extent(u32 dev, u64 part, u8 *e) {
    u16 raw = dg_r16(e + 4);
    u32 len = raw & 0x7FFFu;
    u32 phys;
    if (len == 0) return 0;
    if (dg_r16(e + 6) != 0) dg_panic("FIRSTI-E15 EXT4 BLOCK OWNER");
    phys = dg_r32(e + 8);
    if (!ev_run_hits(phys, len)) return 0;
    phys = ev_reloc(dev, part, phys, len, 0);
    dg_w32(e + 8, phys);
    dg_w16(e + 6, 0);
    return 1;
}

static int ev_leaf(u32 dev, u64 part, u32 leaf) {
    u16 n;
    int dirty = 0;
    dg_rd_blk(dev, part, leaf, g_blk);
    if (dg_r16(g_blk) != 0xF30A || dg_r16(g_blk + 6) != 0) dg_panic("FIRSTI-E15 EXT4 BLOCK OWNER");
    n = dg_r16(g_blk + 2);
    if (n > 340u) dg_panic("FIRSTI-E15 EXT4 BLOCK OWNER");
    for (u16 i = 0; i < n; i++)
        dirty |= ev_patch_extent(dev, part, g_blk + 12u + (u32)i * 12u);
    if (dirty) dg_wr_blk(dev, part, leaf, g_blk);
    return dirty;
}

static int ev_inode_extents(u32 dev, u64 part, u8 *ino) {
    u8 *root = ino + 0x28;
    u16 depth, n;
    int dirty = 0;
    if (dg_r16(root) != 0xF30A) dg_panic("FIRSTI-E15 EXT4 BLOCK OWNER");
    depth = dg_r16(root + 6);
    n = dg_r16(root + 2);
    if (n > 4u) dg_panic("FIRSTI-E15 EXT4 BLOCK OWNER");
    if (depth == 0) {
        for (u16 i = 0; i < n; i++)
            dirty |= ev_patch_extent(dev, part, root + 12u + (u32)i * 12u);
        return dirty;
    }
    if (depth != 1u) dg_panic("FIRSTI-E15 EXT4 BLOCK OWNER");
    for (u16 i = 0; i < n; i++) {
        u8 *e = root + 12u + (u32)i * 12u;
        u32 leaf = dg_r32(e + 4);
        if (dg_r16(e + 8) != 0) dg_panic("FIRSTI-E15 EXT4 BLOCK OWNER");
        if (ev_in(leaf)) {
            leaf = ev_reloc(dev, part, leaf, 1, 0);
            dg_w32(e + 4, leaf);
            dg_w16(e + 8, 0);
            dirty = 1;
        }
        ev_leaf(dev, part, leaf);
    }
    return dirty;
}

static void ev_meta(u32 dev, u64 part, u32 it_blocks) {
    for (u32 g = 0; g < ev_groups; g++) {
        u8 *d = g_gdt + g * 32u;
        u32 bb = dg_r32(d + 0);
        u32 ib = dg_r32(d + 4);
        u32 it = dg_r32(d + 8);
        if (ev_in(bb)) ev_reloc(dev, part, bb, 1, d + 0);
        ib = dg_r32(d + 4);
        if (ev_in(ib)) dg_w32(d + 4, ev_reloc(dev, part, ib, 1, 0));
        it = dg_r32(d + 8);
        if (ev_run_hits(it, it_blocks)) dg_w32(d + 8, ev_reloc(dev, part, it, it_blocks, 0));
        if ((g & 15u) == 0) dg_progress("Moving DATA metadata", g, ev_groups);
    }
    ev_sync(dev, part);
}

static void ev_inodes(u32 dev, u64 part, u32 ipg) {
    u32 it_blocks = (ipg * 256u) / 4096u;
    for (u32 g = 0; g < ev_groups; g++) {
        u32 it = dg_r32(g_gdt + g * 32u + 8);
        for (u32 b = 0; b < it_blocks; b++) {
            int dirty = 0;
            dg_rd_blk(dev, part, it + b, g_ino);
            for (u32 s = 0; s < 16u; s++) {
                u8 *ino = g_ino + s * 256u;
                u16 mode = dg_r16(ino);
                u32 flags, blocks, acl;
                if (mode == 0) continue;
                flags = dg_r32(ino + 0x20);
                blocks = dg_r32(ino + 0x1C);
                acl = dg_r32(ino + 0x68);
                if (dg_r16(ino + 0x76) != 0) dg_panic("FIRSTI-E15 EXT4 BLOCK OWNER");
                if (acl && ev_in(acl)) {
                    dg_w32(ino + 0x68, ev_reloc(dev, part, acl, 1, 0));
                    dirty = 1;
                }
                if (flags & 0x80000u) dirty |= ev_inode_extents(dev, part, ino);
                else if (blocks != 0) dg_panic("FIRSTI-E15 EXT4 BLOCK OWNER");
            }
            if (dirty) dg_wr_blk(dev, part, it + b, g_ino);
        }
    }
}

static void ev_claim(u32 dev, u64 part) {
    for (u32 g = 0; g < ev_groups; g++) {
        u32 zone, zend, gend;
        if (!dg_has_super(g, ev_ro)) continue;
        zone = g * ev_bpg + 1u + ev_area;
        zend = g * ev_bpg + 1u + ev_desc;
        gend = g * ev_bpg + ev_bpg;
        if (zend > ev_blocks) zend = ev_blocks;
        if (zend > gend) zend = gend;
        for (u32 blk = zone; blk < zend; blk++) {
            if (ev_used(dev, part, blk)) dg_panic("FIRSTI-E15 EXT4 BLOCK OWNER");
            ev_touch(dev, part, blk, 1);
        }
    }
    ev_sync(dev, part);
}

static void dg_ext4_evac(u32 dev, u64 part, u32 bpg, u32 old_blocks, u32 old_groups,
                         u32 ro, u32 gdt_area, u32 desc_new, u32 it_blocks, u32 ipg) {
    ev_bpg = bpg;
    ev_blocks = old_blocks;
    ev_groups = old_groups;
    ev_ro = ro;
    ev_area = gdt_area;
    ev_desc = desc_new;
    ev_bm_blk = 0xffffffffu;
    dg_note("Moving DATA metadata");
    ev_meta(dev, part, it_blocks);
    ev_inodes(dev, part, ipg);
    ev_claim(dev, part);
}

static int dg_grow_ext4(u32 dev, u64 part, u64 part_secs) {
    u32 bpg, ipg, inode_sz, incompat, compat, ro, reserved;
    u32 old_blocks, new_blocks, old_groups, new_groups;
    u32 desc_old, desc_new, gdt_area, it_blocks, added_free = 0;
    u64 add_inodes;
    if (part_secs / 8u > 0xffffffffull) dg_panic("FIRSTI-E15 EXT4 TOO LARGE");
    new_blocks = (u32)(part_secs / 8u);
    dg_rd_or(dev, part + 2, 2, g_sb);
    if (dg_r16(g_sb + 0x38) != 0xEF53) dg_panic("FIRSTI-E15 EXT4 MAGIC");
    if (dg_r32(g_sb + 0x18) != 2 || dg_r32(g_sb + 0x20) != 32768 ||
        dg_r16(g_sb + 0x58) != 256 || dg_r32(g_sb + 0x14) != 0)
        dg_panic("FIRSTI-E15 EXT4 GEOMETRY");
    if (dg_r16(g_sb + 0xFE) != 0 && dg_r16(g_sb + 0xFE) != 32)
        dg_panic("FIRSTI-E15 EXT4 DESC SIZE");
    incompat = dg_r32(g_sb + 0x60);
    if (incompat & (0x80u | 0x10u | 0x400u)) dg_panic("FIRSTI-E15 EXT4 FEATURE");
    old_blocks = dg_r32(g_sb + 4);
    if (new_blocks <= old_blocks) return 0;
    bpg = 32768;
    ipg = dg_r32(g_sb + 0x28);
    inode_sz = 256;
    if (ipg == 0 || ipg > 32768 || (ipg * inode_sz) % 4096u) dg_panic("FIRSTI-E15 EXT4 INODES");
    it_blocks = (ipg * inode_sz) / 4096u;
    old_groups = (old_blocks + bpg - 1u) / bpg;
    new_groups = (new_blocks + bpg - 1u) / bpg;
    ro = dg_r32(g_sb + 0x64);
    compat = dg_r32(g_sb + 0x5C);
    reserved = (compat & 0x10u) ? dg_r16(g_sb + 0xCE) : 0;
    desc_old = (old_groups * 32u + 4095u) / 4096u;
    if (desc_old == 0) desc_old = 1;
    gdt_area = desc_old + reserved;

    desc_new = desc_old;
    for (;;) {
        u32 area, g, glen;
        if (new_blocks <= old_blocks) return 0;
        new_groups = (new_blocks + bpg - 1u) / bpg;
        desc_new = (new_groups * 32u + 4095u) / 4096u;
        if (desc_new == 0) desc_new = 1;
        area = gdt_area > desc_new ? gdt_area : desc_new;
        if (new_groups <= old_groups) break;
        g = new_groups - 1u;
        glen = new_blocks - g * bpg;
        if (glen >= dg_group_meta(g, ro, area, it_blocks)) break;
        new_groups--;
        new_blocks = new_groups * bpg;
    }
    if (new_blocks <= old_blocks) return 0;

    dg_need_gdt(desc_new * 4096u);
    dg_note("Growing DATA");
    for (u32 i = 0; i < desc_old; i++)
        dg_rd_blk(dev, part, 1u + i, g_gdt + i * 4096u);

    /* 旧的最后一组如果没占满 128MiB，先把尾部标成空闲。 */
    {
        u32 last = old_groups - 1u;
        u32 old_len = old_blocks - last * bpg;
        u32 new_len = (new_groups > old_groups) ? bpg : (new_blocks - last * bpg);
        u32 bm = dg_r32(g_gdt + last * 32u);
        u32 freed = 0;
        u32 slot_free;
        if (new_len > old_len) {
            dg_rd_blk(dev, part, bm, g_blk);
            for (u32 bit = old_len; bit < new_len; bit++) {
                if (dg_bit_get(g_blk, bit)) { dg_bit_clr(g_blk, bit); freed++; }
            }
            dg_wr_blk(dev, part, bm, g_blk);
            slot_free = dg_r16(g_gdt + last * 32u + 0x0C);
            if ((u32)slot_free + freed > 65535u) dg_panic("FIRSTI-E15 EXT4 FREE COUNT");
            dg_w16(g_gdt + last * 32u + 0x0C, (u16)(slot_free + freed));
            added_free += freed;
        }
    }

    if (desc_new > gdt_area) {
        dg_ext4_evac(dev, part, bpg, old_blocks, old_groups, ro, gdt_area, desc_new,
                     it_blocks, ipg);
        gdt_area = desc_new;
    }

    for (u32 g = old_groups; g < new_groups; g++) {
        u32 base = g * bpg;
        u32 glen = new_blocks - base;
        u32 cursor = 0, bb, ib, it, meta, free_b;
        if (glen > bpg) glen = bpg;
        if (dg_has_super(g, ro)) cursor += 1u + gdt_area;
        bb = cursor++;
        ib = cursor++;
        it = cursor;
        cursor += it_blocks;
        meta = cursor;
        if (meta > glen) dg_panic("FIRSTI-E15 EXT4 GROUP META");
        free_b = glen - meta;
        if (dg_has_super(g, ro)) {
            for (u32 b = 0; b < gdt_area; b++) {
                dg_zero(g_blk, 4096);
                dg_wr_blk(dev, part, base + 1u + b, g_blk);
            }
        }
        for (u32 i = 0; i < 4096u; i++) g_blk[i] = 0xFF;
        for (u32 bit = meta; bit < glen; bit++) dg_bit_clr(g_blk, bit);
        dg_wr_blk(dev, part, base + bb, g_blk);
        dg_zero(g_blk, 4096);
        for (u32 bit = ipg; bit < 32768u; bit++) dg_bit_set(g_blk, bit);
        dg_wr_blk(dev, part, base + ib, g_blk);
        dg_zero(g_blk, 4096);
        for (u32 b = 0; b < it_blocks; b++)
            dg_wr_blk(dev, part, base + it + b, g_blk);
        {
            u8 *d = g_gdt + g * 32u;
            dg_zero(d, 32);
            dg_w32(d + 0x00, base + bb);
            dg_w32(d + 0x04, base + ib);
            dg_w32(d + 0x08, base + it);
            dg_w16(d + 0x0C, (u16)free_b);
            dg_w16(d + 0x0E, (u16)ipg);
            dg_w16(d + 0x12, 0x0004);
            dg_w16(d + 0x1C, (u16)ipg);
        }
        added_free += free_b;
        if ((g & 15u) == 0) dg_progress("Formatting DATA", g - old_groups, new_groups - old_groups);
    }

    add_inodes = (u64)(new_groups - old_groups) * ipg;
    if (dg_r32(g_sb + 0x00) + add_inodes < dg_r32(g_sb + 0x00) ||
        dg_r32(g_sb + 0x10) + add_inodes < dg_r32(g_sb + 0x10) ||
        dg_r32(g_sb + 0x0C) + added_free < dg_r32(g_sb + 0x0C))
        dg_panic("FIRSTI-E15 EXT4 COUNT OVERFLOW");
    dg_w32(g_sb + 0x00, dg_r32(g_sb + 0x00) + (u32)add_inodes);
    dg_w32(g_sb + 0x10, dg_r32(g_sb + 0x10) + (u32)add_inodes);
    dg_w32(g_sb + 0x0C, dg_r32(g_sb + 0x0C) + added_free);
    dg_w32(g_sb + 0x04, new_blocks);
    dg_w16(g_sb + 0x3A, 1);
    dg_ext4_checksums(new_groups, ro);
    dg_wr_or(dev, part + 2, 2, g_sb);
    for (u32 i = 0; i < desc_new; i++)
        dg_wr_blk(dev, part, 1u + i, g_gdt + i * 4096u);

    for (u32 g = 1; g < new_groups; g++) {
        if (!dg_has_super(g, ro)) continue;
        dg_zero(g_blk, 4096);
        dg_copy(g_blk + 1024, g_sb, 1024);
        dg_w16(g_blk + 1024 + 0x5A, (u16)g);
        dg_wr_blk(dev, part, g * bpg, g_blk);
        for (u32 i = 0; i < desc_new; i++)
            dg_wr_blk(dev, part, g * bpg + 1u + i, g_gdt + i * 4096u);
    }
    dg_info("[firstinit] DATA filesystem grown");
    return 1;
}

/* ---------------- NTFS ---------------- */

struct dg_run { u64 lcn; u64 len; int sparse; };

static u32 dg_nbytes_u(u64 v) {
    u32 n = 1;
    while (n < 8 && (v >> (8u * n)) != 0) n++;
    return n;
}
static u32 dg_nbytes_s(i64 v) {
    for (u32 n = 1; n < 8; n++) {
        u32 bits = n * 8u;
        u64 mask = (1ull << bits) - 1ull;
        u64 u = (u64)v & mask;
        i64 back = (u & (1ull << (bits - 1u))) ? (i64)(u | ~mask) : (i64)u;
        if (back == v) return n;
    }
    return 8;
}

static int dg_run_decode(const u8 *p, u32 avail, struct dg_run *o, u32 max, u32 *n_out) {
    u32 i = 0, n = 0;
    u64 prev = 0;
    while (i < avail && p[i] != 0) {
        u8 h = p[i++];
        u32 lsz = h >> 4, osz = h & 15u;
        u64 len = 0;
        if (lsz == 0 || lsz > 8 || osz > 8 || i + lsz + osz > avail || n >= max) return -1;
        for (u32 b = 0; b < lsz; b++) len |= (u64)p[i++] << (8u * b);
        o[n].len = len;
        if (osz == 0) {
            o[n].sparse = 1;
            o[n].lcn = 0;
        } else {
            u64 raw = 0;
            for (u32 b = 0; b < osz; b++) raw |= (u64)p[i++] << (8u * b);
            if ((raw & (1ull << (osz * 8u - 1u))) && osz < 8)
                raw |= ~0ull << (osz * 8u);
            prev = (u64)((i64)prev + (i64)raw);
            o[n].sparse = 0;
            o[n].lcn = prev;
        }
        n++;
    }
    *n_out = n;
    return 0;
}

static u32 dg_run_encode(u8 *dst, u32 cap, const struct dg_run *o, u32 n) {
    u32 p = 0;
    u64 prev = 0;
    for (u32 i = 0; i < n; i++) {
        u32 lsz = dg_nbytes_u(o[i].len);
        u32 osz = 0;
        i64 delta = 0;
        if (!o[i].sparse) {
            delta = (i64)o[i].lcn - (i64)prev;
            osz = dg_nbytes_s(delta);
            prev = o[i].lcn;
        }
        if (p + 1u + lsz + osz + 1u > cap) return 0;
        dst[p++] = (u8)((lsz << 4) | osz);
        for (u32 b = 0; b < lsz; b++) dst[p++] = (u8)(o[i].len >> (8u * b));
        if (osz) {
            u64 u = (u64)delta;
            for (u32 b = 0; b < osz; b++) dst[p++] = (u8)(u >> (8u * b));
        }
    }
    if (p >= cap) return 0;
    dst[p++] = 0;
    return p;
}

static int dg_fixup_apply(u8 *rec, u32 rec_size) {
    u16 ofs, count, seq;
    u32 sectors;
    if (rec[0] != 'F' || rec[1] != 'I' || rec[2] != 'L' || rec[3] != 'E') return -1;
    ofs = dg_r16(rec + 4);
    count = dg_r16(rec + 6);
    if (count < 2 || (u32)ofs + (u32)count * 2u > rec_size) return -1;
    sectors = (u32)count - 1u;
    if (sectors * 512u != rec_size) return -1;
    seq = dg_r16(rec + ofs);
    for (u32 i = 0; i < sectors; i++) {
        u8 *end = rec + (i + 1u) * 512u - 2u;
        if (dg_r16(end) != seq) return -1;
        dg_w16(end, dg_r16(rec + ofs + 2u + i * 2u));
    }
    return 0;
}
static void dg_fixup_store(u8 *rec) {
    u16 ofs = dg_r16(rec + 4);
    u16 count = dg_r16(rec + 6);
    u32 sectors = (u32)count - 1u;
    u16 seq = (u16)(dg_r16(rec + ofs) + 1u);
    if (seq == 0) seq = 1;
    dg_w16(rec + ofs, seq);
    for (u32 i = 0; i < sectors; i++) {
        u8 *end = rec + (i + 1u) * 512u - 2u;
        dg_w16(rec + ofs + 2u + i * 2u, dg_r16(end));
        dg_w16(end, seq);
    }
}

static int dg_find_attr(u8 *rec, u32 rec_size, u32 type, int named, u32 *off) {
    u32 ao = dg_r16(rec + 0x14);
    while (ao + 8u <= rec_size) {
        u32 t = dg_r32(rec + ao);
        u32 len;
        if (t == 0xffffffffu) return -1;
        len = dg_r32(rec + ao + 4);
        if (len < 8 || ao + len > rec_size) return -1;
        if (t == type && ((rec[ao + 9] != 0) == named)) { *off = ao; return 0; }
        ao += len;
    }
    return -1;
}

static int dg_attr_expand(u8 *rec, u32 off, u32 new_len) {
    u32 old = dg_r32(rec + off + 4);
    u32 grow, used, alloc, src;
    new_len = (new_len + 7u) & ~7u;
    if (new_len <= old) return 0;
    grow = new_len - old;
    used = dg_r32(rec + 0x18);
    alloc = dg_r32(rec + 0x1C);
    if (used < off + old || used + grow > alloc) return -1;
    src = used;
    while (src > off + old) {
        src--;
        rec[src + grow] = rec[src];
    }
    for (u32 i = 0; i < grow; i++) rec[off + old + i] = 0;
    dg_w32(rec + off + 4, new_len);
    dg_w32(rec + 0x18, used + grow);
    return 0;
}

static int dg_store_runs(u8 *rec, u32 rec_size, u32 off, const struct dg_run *runs, u32 n) {
    u8 tmp[160];
    u32 mp, enc, alen;
    if (rec[off + 8] != 1) return -1;
    mp = dg_r16(rec + off + 0x20);
    enc = dg_run_encode(tmp, sizeof tmp, runs, n);
    if (enc == 0) return -1;
    alen = dg_r32(rec + off + 4);
    if (mp + enc > alen) {
        if (dg_attr_expand(rec, off, mp + enc) != 0) return -1;
        alen = dg_r32(rec + off + 4);
    }
    if (mp + enc > alen || off + alen > rec_size) return -1;
    for (u32 i = 0; i < enc; i++) rec[off + mp + i] = tmp[i];
    for (u32 i = enc; mp + i < alen; i++) rec[off + mp + i] = 0;
    return 0;
}

static int dg_map_off(const struct dg_run *runs, u32 n, u64 off, u64 clus_bytes,
                      u64 *lcn, u64 *inn) {
    u64 cl = off / clus_bytes;
    u64 seen = 0;
    for (u32 i = 0; i < n; i++) {
        if (cl < seen + runs[i].len) {
            if (runs[i].sparse) return -1;
            *lcn = runs[i].lcn + (cl - seen);
            *inn = off % clus_bytes;
            return 0;
        }
        seen += runs[i].len;
    }
    return -1;
}

static void dg_zero_lcn(u32 dev, u64 part, u64 lcn, u32 spc) {
    u64 lba = part + lcn * spc;
    u32 left = spc;
    while (left) {
        u32 n = left > DG_CHUNK ? DG_CHUNK : left;
        dg_zero(g_io, n * 512u);
        dg_wr_or(dev, lba, n, g_io);
        lba += n;
        left -= n;
    }
}

static void dg_set_bit_file(u32 dev, u64 part, const struct dg_run *runs, u32 nruns,
                            u64 clus_bytes, u32 spc, u64 bit) {
    u64 lcn, inn, lba;
    u32 bit_in;
    if (dg_map_off(runs, nruns, bit / 8u, clus_bytes, &lcn, &inn) != 0)
        dg_panic("FIRSTI-E16 NTFS BITMAP MAP");
    lba = part + lcn * spc + inn / 512u;
    dg_rd_or(dev, lba, 1, g_io);
    bit_in = (u32)((inn % 512u) * 8u + (bit % 8u));
    g_io[bit_in / 8u] = (u8)(g_io[bit_in / 8u] | (u8)(1u << (bit_in % 8u)));
    dg_wr_or(dev, lba, 1, g_io);
}

static void dg_read_mft(u32 dev, u64 part, u64 mft_lcn, u32 spc, u32 rec_size, u32 index) {
    u64 off = mft_lcn * (u64)spc * 512ull + (u64)index * rec_size;
    if (rec_size > sizeof g_rec || (off & 511ull)) dg_panic("FIRSTI-E16 NTFS MFT ALIGN");
    dg_rd_or(dev, part + off / 512u, rec_size / 512u, g_rec);
    if (dg_fixup_apply(g_rec, rec_size) != 0) dg_panic("FIRSTI-E16 NTFS MFT FIXUP");
}
static void dg_write_mft(u32 dev, u64 part, u64 mft_lcn, u32 spc, u32 rec_size, u32 index) {
    u64 off = mft_lcn * (u64)spc * 512ull + (u64)index * rec_size;
    dg_fixup_store(g_rec);
    dg_wr_or(dev, part + off / 512u, rec_size / 512u, g_rec);
}

static int dg_grow_ntfs(u32 dev, u64 part, u64 part_secs) {
    u8 boot[512];
    u32 bps, spc, rec_size;
    u64 old_total, new_total, mft_lcn, clus_bytes;
    u64 old_clusters, new_clusters, bytes_need, bmp_clusters, old_bmp, need;
    u64 alloc_lcn, old_data_sz;
    struct dg_run runs[DG_MAX_RUNS];
    u32 nruns = 0, aoff = 0;
    i8 cpr;
    dg_rd_or(dev, part, 1, boot);
    if (boot[3] != 'N' || boot[4] != 'T' || boot[5] != 'F' || boot[6] != 'S')
        dg_panic("FIRSTI-E16 NTFS MAGIC");
    bps = dg_r16(boot + 11);
    spc = boot[13];
    old_total = dg_r64(boot + 0x28);
    mft_lcn = dg_r64(boot + 0x30);
    cpr = (i8)boot[0x40];
    if (bps != 512 || !dg_is_pow2(spc) || spc > 128) dg_panic("FIRSTI-E16 NTFS GEOMETRY");
    if (part > 0xffffffffull) dg_panic("FIRSTI-E16 NTFS HIDDEN LBA");
    new_total = (part_secs / spc) * spc;
    if (new_total <= old_total) return 0;
    if (cpr > 0) rec_size = (u32)cpr * spc * bps;
    else if (cpr < 0 && (u32)(-cpr) < 13u) rec_size = 1u << (u32)(-cpr);
    else dg_panic("FIRSTI-E16 NTFS RECORD SIZE");
    if (rec_size < 512 || rec_size > sizeof g_rec) dg_panic("FIRSTI-E16 NTFS RECORD SIZE");
    clus_bytes = (u64)spc * bps;
    old_clusters = old_total / spc;
    new_clusters = new_total / spc;
    bytes_need = (new_clusters + 7ull) / 8ull;

    dg_note("Growing WIN");
    dg_read_mft(dev, part, mft_lcn, spc, rec_size, 6);
    if (dg_find_attr(g_rec, rec_size, 0x80, 0, &aoff) != 0) dg_panic("FIRSTI-E16 NTFS BITMAP ATTR");
    if (g_rec[aoff + 8] != 1) dg_panic("FIRSTI-E16 NTFS BITMAP RESIDENT");
    if (dg_run_decode(g_rec + aoff + dg_r16(g_rec + aoff + 0x20),
                      dg_r32(g_rec + aoff + 4) - dg_r16(g_rec + aoff + 0x20),
                      runs, DG_MAX_RUNS, &nruns) != 0 || nruns == 0)
        dg_panic("FIRSTI-E16 NTFS BITMAP RUNS");
    old_bmp = 0;
    for (u32 i = 0; i < nruns; i++) {
        if (runs[i].sparse) dg_panic("FIRSTI-E16 NTFS BITMAP SPARSE");
        old_bmp += runs[i].len;
    }
    bmp_clusters = (bytes_need + clus_bytes - 1ull) / clus_bytes;
    if (bmp_clusters > old_bmp) {
        need = bmp_clusters - old_bmp;
        if (need >= DG_MAX_RUNS || nruns + 1u > DG_MAX_RUNS) dg_panic("FIRSTI-E16 NTFS BITMAP RUNS");
        if (new_clusters < old_clusters + need) dg_panic("FIRSTI-E16 NTFS BITMAP SPACE");
        alloc_lcn = new_clusters - need;
        runs[nruns].sparse = 0;
        runs[nruns].lcn = alloc_lcn;
        runs[nruns].len = need;
        nruns++;
        for (u64 c = 0; c < need; c++) dg_zero_lcn(dev, part, alloc_lcn + c, spc);
        for (u64 c = 0; c < need; c++)
            dg_set_bit_file(dev, part, runs, nruns, clus_bytes, spc, alloc_lcn + c);
    }
    old_data_sz = dg_r64(g_rec + aoff + 0x30);
    dg_w64(g_rec + aoff + 0x18, bmp_clusters - 1ull);
    dg_w64(g_rec + aoff + 0x28, bmp_clusters * clus_bytes);
    dg_w64(g_rec + aoff + 0x30, bytes_need);
    dg_w64(g_rec + aoff + 0x38, bytes_need);
    if (dg_store_runs(g_rec, rec_size, aoff, runs, nruns) != 0)
        dg_panic("FIRSTI-E16 NTFS BITMAP RUNS");
    /* 旧位图尾部到新长度之间的字节必须是 0（空闲簇）。新簇已整体清零。 */
    if (old_data_sz < bytes_need && old_data_sz < old_bmp * clus_bytes) {
        u64 end = bytes_need;
        if (end > old_bmp * clus_bytes) end = old_bmp * clus_bytes;
        for (u64 off = old_data_sz; off < end; ) {
            u64 lcn, inn, lba;
            u32 room, n;
            if (dg_map_off(runs, nruns, off, clus_bytes, &lcn, &inn) != 0)
                dg_panic("FIRSTI-E16 NTFS BITMAP MAP");
            lba = part + lcn * spc + inn / 512u;
            dg_rd_or(dev, lba, 1, g_io);
            room = 512u - (u32)(inn % 512u);
            n = (u32)((end - off) > room ? room : (end - off));
            dg_zero(g_io + (u32)(inn % 512u), n);
            dg_wr_or(dev, lba, 1, g_io);
            off += n;
        }
    }
    dg_write_mft(dev, part, mft_lcn, spc, rec_size, 6);

    dg_read_mft(dev, part, mft_lcn, spc, rec_size, 8);
    if (dg_find_attr(g_rec, rec_size, 0x80, 1, &aoff) != 0) dg_panic("FIRSTI-E16 NTFS BADCLUS");
    if (g_rec[aoff + 8] != 1) dg_panic("FIRSTI-E16 NTFS BADCLUS");
    if (dg_run_decode(g_rec + aoff + dg_r16(g_rec + aoff + 0x20),
                      dg_r32(g_rec + aoff + 4) - dg_r16(g_rec + aoff + 0x20),
                      runs, DG_MAX_RUNS, &nruns) != 0 || nruns != 1 || !runs[0].sparse)
        dg_panic("FIRSTI-E16 NTFS BADCLUS");
    runs[0].len = new_clusters;
    dg_w64(g_rec + aoff + 0x18, new_clusters - 1ull);
    dg_w64(g_rec + aoff + 0x30, new_clusters * clus_bytes);
    if (dg_r64(g_rec + aoff + 0x38) != 0)
        dg_w64(g_rec + aoff + 0x38, new_clusters * clus_bytes);
    if (dg_store_runs(g_rec, rec_size, aoff, runs, nruns) != 0)
        dg_panic("FIRSTI-E16 NTFS BADCLUS");
    dg_write_mft(dev, part, mft_lcn, spc, rec_size, 8);

    dg_w64(boot + 0x28, new_total);
    dg_w32(boot + 0x1C, (u32)part);
    dg_wr_or(dev, part, 1, boot);
    dg_wr_or(dev, part + part_secs - 1ull, 1, boot);
    if (new_total != part_secs) dg_wr_or(dev, part + new_total - 1ull, 1, boot);
    dg_info("[firstinit] WIN filesystem grown");
    return 1;
}

static void dg_reload(int fat, int ext4) {
    void *fn;
    u32 ver;
    if (!fat && !ext4) return;
    if (!g_api->bus || !g_api->bus->probe) dg_panic("FIRSTI-E19 RELOAD MISSING");
    if (fat) {
        fn = 0; ver = 0;
        if (g_api->bus->probe("srv.fs.fat32.reload", &fn, &ver) != 0 || !fn)
            dg_panic("FIRSTI-E19 FAT32 RELOAD MISSING");
        if (((int (*)(void))fn)() != 0) dg_panic("FIRSTI-E19 FAT32 RELOAD FAILED");
    }
    if (ext4) {
        fn = 0; ver = 0;
        if (g_api->bus->probe("srv.fs.ext4.remount", &fn, &ver) != 0 || !fn)
            dg_panic("FIRSTI-E19 EXT4 REMOUNT MISSING");
        if (((int (*)(void))fn)() != 0) dg_panic("FIRSTI-E19 EXT4 REMOUNT FAILED");
    }
}

void firstinit_disk_grow(const struct dkm_kernel_api *api,
                         const struct dkm_log_api *log,
                         void (*status)(const char *line)) {
    struct dg_disk disk;
    u64 sectors, usable, sum, n1, n2, n3;
    u64 p2_new, p3_new;
    int moved = 0, fat = 0, ext4 = 0, ntfs = 0;
    g_api = api;
    g_log = log;
    g_status = status;
    if (!api || !log) return;
    dg_info("[firstinit] disk grow");
    if (!dg_find_disk(&disk)) {
        dg_info("[firstinit] disk grow: Deshab GPT not found, skip");
        return;
    }
    if (!api->block->sector_count) {
        dg_info("[firstinit] disk grow: sector_count missing, skip");
        return;
    }
    sectors = api->block->sector_count(disk.dev);
    if (sectors < disk.esp.start + disk.esp.size + DG_TAIL) {
        dg_info("[firstinit] disk grow: disk not larger than image, skip");
        return;
    }
    if (disk.esp.start + disk.esp.size != disk.data.start ||
        disk.data.start + disk.data.size != disk.win.start)
        dg_panic("FIRSTI-E12 PARTITION GAP");
    sum = disk.esp.size + disk.data.size + disk.win.size;
    usable = sectors - DG_TAIL - disk.esp.start;
    if (usable <= sum) {
        n1 = disk.esp.size;
        n2 = disk.data.size;
        n3 = disk.win.size;
    } else {
        n1 = dg_mul_div(usable, disk.esp.size, sum) & ~(u64)(DG_ALIGN - 1);
        n2 = dg_mul_div(usable, disk.data.size, sum) & ~(u64)(DG_ALIGN - 1);
        if (n1 < disk.esp.size) n1 = disk.esp.size;
        if (n2 < disk.data.size) n2 = disk.data.size;
        if (n1 == ~0ull || n2 == ~0ull || n1 + n2 >= usable) {
            n1 = disk.esp.size;
            n2 = disk.data.size;
            n3 = disk.win.size;
        } else {
            n3 = usable - n1 - n2;
            if (n3 < disk.win.size) {
                n1 = disk.esp.size;
                n2 = disk.data.size;
                n3 = disk.win.size;
            }
        }
    }
    p2_new = disk.esp.start + n1;
    p3_new = p2_new + n2;
    if (p2_new != disk.data.start || p3_new != disk.win.start ||
        n1 != disk.esp.size || n3 != disk.win.size) {
        dg_note("Expanding partitions");
        if (p3_new != disk.win.start)
            dg_copy_range(disk.dev, p3_new, disk.win.start, disk.win.size, "Moving WIN");
        if (p2_new != disk.data.start)
            dg_copy_range(disk.dev, p2_new, disk.data.start, disk.data.size, "Moving DATA");
        dg_write_gpt(&disk, n1, n2, n3, sectors);
        moved = 1;
        dg_info("[firstinit] GPT expanded");
    }
    fat = dg_grow_fat32(disk.dev, disk.esp.start, n1);
    ext4 = dg_grow_ext4(disk.dev, p2_new, n2);
    ntfs = dg_grow_ntfs(disk.dev, p3_new, n3);
    if (moved || fat || ext4) dg_reload(fat || moved, ext4 || moved);
    if (moved || fat || ext4 || ntfs) dg_note("Partition expand done");
    else dg_info("[firstinit] disk grow: already full");
}
