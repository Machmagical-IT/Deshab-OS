/* DKM FAT32 Driver — full FAT32 read/write stack, exposed via dkm_bus
 * Stage 2, optional, depends on "vfs" and "block", provides "fat32" + "fs.fat32".
 *
 * P2 万物互联：DSK 自带的 FAT32 读写栈归位为独立 DKM 驱动
 * （母本 CODE/UTSM/kernel/dsk_loader.c）。通过 srv.fs.fat32.* 服务总线
 * 对外提供文件读写（块设备 index 固定 0，与母本一致）。
 * 读写全部经 driver_init 传入的 g_api->block，不直连任何内核符号。
 */

#include <stdint.h>

#include "../dkm_shared.h"
#include "../dkm_instr.h"
#include "../../tools/fat32_lfn.h"
#include "../../tools/fat32_part.h"

/* ---------------------------------------------------------------
 * FAT32 类型（照抄 dsk_loader.c；DKM ABI 类型来自 dkm_shared.h）
 * --------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    u8  jmp[3];
    char oem[8];
    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sector_count;
    u8  fat_count;
    u16 root_entry_count;
    u16 total_sectors_16;
    u8  media;
    u16 sectors_per_fat_16;
    u16 sectors_per_track;
    u16 heads;
    u32 hidden_sectors;
    u32 total_sectors_32;
    /* FAT32 extended */
    u32 sectors_per_fat;
    u16 flags;
    u16 version;
    u32 root_cluster;
    u16 fsinfo_sector;
    u16 backup_boot;
    u8  reserved[12];
    u8  drive;
    u8  nt_flags;
    u8  signature;
    u32 serial;
    char label[11];
    char type[8];
    u8  code[420];
    u16 boot_sig;
} fat32_bpb;

typedef struct __attribute__((packed)) {
    char name[11];
    u8  attr;
    u8  nt_reserved;
    u8  creation_tenth;
    u16 creation_time;
    u16 creation_date;
    u16 access_date;
    u16 cluster_high;
    u16 write_time;
    u16 write_date;
    u16 cluster_low;
    u32 file_size;
} fat32_dir_entry;

/* ---------------------------------------------------------------
 * Driver descriptor
 * --------------------------------------------------------------- */
static const char *const g_depends[] = { "vfs", "block" };
static const char *const g_provides[] = { "fat32", "fs.fat32" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic=DKM_DRIVER_MAGIC, .abi_version=DKM_ABI_VERSION,
    .desc_size=sizeof(struct dkm_driver_desc),
    .name="fat32", .version="0.2.0", .vendor="Deshab",
    .driver_class=7, .stage=2, .flags=0, .priority=0,
    .depends=g_depends, .depends_count=2,
    .provides=g_provides, .provides_count=2,
    .min_kernel_abi=1,
};

/* ---------------------------------------------------------------
 * Driver state + buffers（4 个静态缓冲照搬自 dsk_loader.c，进 .drv BSS）
 * --------------------------------------------------------------- */
static const struct dkm_log_api *g_log;
static const struct dkm_kernel_api *g_api;
static int g_ready;   /* 1 = 设备 0 上探测到合法 FAT32 卷，服务可注册 */

/* GPT 分区偏移：所选的块设备为 GPT 盘时，FAT32 ESP 在 p1（LBA 2048），
 * 不是 LBA0（LBA0 是保护性 MBR）。fat32_part_probe 定位 BPB 所在 LBA，
 * 所有卷内读写都加上 g_fat_lba。superfloppy（LBA0 即 FAT32）时为 0。 */
static u32 g_fat_lba;
/* 选中的块设备索引。实机上块设备 0 不一定是启动盘（AHCI/NVMe 会先于
 * xHCI 注册），因此不能硬编码 0——由 fat32_probe_and_seed 跨设备探测。 */
static u32 g_fat_dev;

static int fat32_rd(u64 lba, u32 count, void *buf) {
    return g_api->block->read(g_fat_dev, (u64)lba + g_fat_lba, count, buf);
}
static int fat32_wr(u64 lba, u32 count, const void *buf) {
    return g_api->block->write(g_fat_dev, (u64)lba + g_fat_lba, count, buf);
}

static u8 g_fat32_fat_sec[512];       /* BUG-019 按需 FAT 扇区读取缓冲 */

/* 单簇读写缓冲。上限见 FAT32_MAX_CLUSTER_BYTES（64KiB）。 */
static u8 g_fat32_cluster[FAT32_MAX_CLUSTER_BYTES];

/* ---------------------------------------------------------------
 * 卷参数（从 BPB 解析一次）
 *
 * 旧实现依赖"BPB + FAT 全在 128KiB 预读缓冲内"这个前提，只在 256MiB 级
 * ESP 上成立。分区扩到 12GiB 后 FAT 表本身就有 24MiB（-s 4），前提失效，
 * 那段"缓冲内直接读 FAT 条目"的快路径变成永远不会命中的死代码，而所有
 * 尺寸判断都围绕 sizeof(g_fat32_disk) 打转，难以推理。
 *
 * 现在改为：BPB 只读一个扇区并解析成 g_vol，FAT 条目一律按需读扇区
 * （带单扇区缓存）。逻辑不再随缓冲大小分叉，12GiB / 256MiB / superfloppy
 * 走同一条路径。
 * --------------------------------------------------------------- */
typedef struct {
    u32 cluster_bytes;   /* spc * bytes_per_sector */
    u32 spc;             /* sectors per cluster */
    u32 reserved;        /* reserved sectors before FAT */
    u32 fat_count;
    u32 fat_sectors;     /* sectors per FAT copy */
    u32 root_cluster;
    u32 data_lba;        /* 卷内第一个数据扇区 */
    u32 serial;
    u32 total_clusters;  /* 数据区簇数（上限校验用） */
} fat32_vol;

static fat32_vol g_vol;
static u8 g_bpb_sec[512];            /* BPB 所在扇区（只读一个扇区） */
static u32 g_fat_sec_no;             /* 单扇区 FAT 缓存对应的卷内扇区号 */
static int g_fat_sec_valid;

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */
static void log_hex(const char *prefix, u64 val) {
    static const char h[] = "0123456789abcdef";
    char buf[19];
    u32 p = 0;
    buf[p++] = '0'; buf[p++] = 'x';
    for (int i = 15; i >= 0; i--) buf[p++] = h[(val >> (i * 4)) & 0xf];
    buf[p] = 0;
    g_log->info(prefix);
    g_log->info(buf);
}

static u32 fat32_read_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 fat32_read_u16(const u8 *p) {
    return (u16)p[0] | ((u16)p[1] << 8);
}

static int fat32_name11_eq(const char *n, const char *s) {
    for (int i = 0; i < 11; i++) {
        if (n[i] != s[i]) return 0;
    }
    return 1;
}

/* 母本 core helpers（dsk_loader.c）——当前 FAT32 栈未直接使用，
 * 保留以保持与母本一致性（ELF 装载路径复用）。 */
static __attribute__((unused)) int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static __attribute__((unused)) int range_ok(u64 offset, u64 length, u64 size) {
    if (offset > size) return 0;
    if (length > size - offset) return 0;
    return 1;
}

/* 读卷的 BPB 并解析成 g_vol。
 * 只读一个扇区，不依赖任何预读缓冲。返回 0 成功。
 * 校验失败返回负值（调用方据此跳过该候选设备）。 */
static int fat32_load_vol(void) {
    if (!g_api || !g_api->block || !g_api->block->read) return -1;
    if (fat32_rd(0, 1, g_bpb_sec) != 0) return -2;
    const fat32_bpb *bpb = (const fat32_bpb *)g_bpb_sec;
    if (bpb->boot_sig != 0xAA55 || bpb->bytes_per_sector != 512) return -3;
    if (bpb->sectors_per_cluster == 0 || bpb->sectors_per_fat == 0) return -4;
    if (bpb->root_cluster < 2) return -5;

    g_vol.spc = bpb->sectors_per_cluster;
    g_vol.cluster_bytes = g_vol.spc * bpb->bytes_per_sector;
    /* 簇大小必须能装进单簇缓冲，否则驱动无法按簇读写。
     * 12GiB + -s 4 是 2KiB；32G+ 的 mkfs 默认值会到 16/32/64KiB。 */
    if (g_vol.cluster_bytes > FAT32_MAX_CLUSTER_BYTES) return -6;
    g_vol.reserved = bpb->reserved_sector_count;
    g_vol.fat_count = bpb->fat_count;
    g_vol.fat_sectors = bpb->sectors_per_fat;
    g_vol.root_cluster = bpb->root_cluster;
    g_vol.data_lba = bpb->reserved_sector_count +
                     (u32)bpb->fat_count * bpb->sectors_per_fat;
    g_vol.serial = bpb->serial;

    /* 数据区扇区数 → 簇数。total_sectors_32 为 0 时退回 16 位字段。 */
    u32 total_sec = bpb->total_sectors_32;
    if (total_sec == 0) total_sec = bpb->total_sectors_16;
    g_vol.total_clusters = (total_sec > g_vol.data_lba)
                         ? (total_sec - g_vol.data_lba) / g_vol.spc : 0;
    g_fat_sec_valid = 0;
    return 0;
}

/* 簇号是否在数据区内（防止损坏的卷把 LBA 算到分区外）。 */
static int fat32_clus_ok(u32 clus) {
    return clus >= 2 && clus < 0x0FFFFFF8u &&
           (g_vol.total_clusters == 0 ||
            (clus - 2) < g_vol.total_clusters);
}

/* 数据簇 → 卷内起始扇区。 */
static u32 fat32_clus_lba(u32 clus) {
    return g_vol.data_lba + (clus - 2) * g_vol.spc;
}

/* 读取 FAT 条目（28 位有效）。按需读扇区，单扇区缓存。
 * 不依赖任何预读缓冲，因此卷大小无关。读失败一律返回 EOC（调用方终止链）。
 * copy: FAT 副本索引（0 = 第一份）。 */
static u32 fat32_fat_get_ex(u32 clus, u32 copy) {
    if (!fat32_clus_ok(clus)) return 0x0FFFFFFF;
    if (copy >= g_vol.fat_count) return 0x0FFFFFFF;
    u32 byte_off = clus * 4u;
    u32 sec_index = byte_off / 512u;
    u32 ent_off = byte_off % 512u;
    if (sec_index >= g_vol.fat_sectors) return 0x0FFFFFFF;
    u32 sec = g_vol.reserved + copy * g_vol.fat_sectors + sec_index;
    if (!g_fat_sec_valid || g_fat_sec_no != sec) {
        if (fat32_rd(sec, 1, g_fat32_fat_sec) != 0) {
            g_fat_sec_valid = 0;
            g_log->error("[fat32] FAT sector read failed");
            log_hex("[fat32] fat_sec=", sec);
            log_hex("[fat32] clus=", clus);
            g_log->panic("FAT32-E01 FAT SECTOR READ FAILED");
            return 0x0FFFFFFF;
        }
        g_fat_sec_no = sec;
        g_fat_sec_valid = 1;
    }
    return fat32_read_u32(g_fat32_fat_sec + ent_off) & 0x0FFFFFFF;
}

static u32 fat32_fat_get(u32 clus) {
    return fat32_fat_get_ex(clus, 0);
}

/* 写 FAT 条目（28 位）。写到所有 FAT 副本。
 * 单扇区读-改-写；缓存同步，避免同一扇区连续写时重复读盘。 */
static int fat32_fat_set(u32 clus, u32 value) {
    if (!fat32_clus_ok(clus)) return -1;
    if (!g_api->block->write) return -2;
    u32 byte_off = clus * 4u;
    u32 sec_index = byte_off / 512u;
    u32 ent_off = byte_off % 512u;
    if (sec_index >= g_vol.fat_sectors) return -3;

    u32 copies = g_vol.fat_count;
    if (copies > 4) copies = 4;   /* 与旧实现一致的上限 */
    for (u32 f = 0; f < copies; f++) {
        u32 sec = g_vol.reserved + f * g_vol.fat_sectors + sec_index;
        /* 若正好是缓存中的扇区，直接在缓存上改，省一次读 */
        if (g_fat_sec_valid && g_fat_sec_no == sec) {
            u8 *p = g_fat32_fat_sec + ent_off;
            p[0] = (u8)(value & 0xFF);
            p[1] = (u8)((value >> 8) & 0xFF);
            p[2] = (u8)((value >> 16) & 0xFF);
            p[3] = (u8)((p[3] & 0xF0) | ((value >> 24) & 0x0F));
            if (fat32_wr(sec, 1, g_fat32_fat_sec) != 0) return -4;
            continue;
        }
        u8 buf[512];
        if (fat32_rd(sec, 1, buf) != 0) return -5;
        buf[ent_off]     = (u8)(value & 0xFF);
        buf[ent_off + 1] = (u8)((value >> 8) & 0xFF);
        buf[ent_off + 2] = (u8)((value >> 16) & 0xFF);
        buf[ent_off + 3] = (u8)((buf[ent_off + 3] & 0xF0) | ((value >> 24) & 0x0F));
        if (fat32_wr(sec, 1, buf) != 0) return -6;
    }
    return 0;
}

/* 顺序扫描 FAT 找空闲簇（按需读扇区，单扇区缓存）。
 * from_clus: 起始簇号（建议传入上次分配结果+1，避免反复扫描）。
 * 返回空闲簇号，失败返回 0。 */
static u32 fat32_find_free_cluster(u32 from_clus) {
    u32 c = from_clus < 2 ? 2 : from_clus;
    u32 limit = g_vol.total_clusters ? g_vol.total_clusters + 2
                                     : (g_vol.fat_sectors * 512u) / 4u;
    while (c < limit && c < 0x0FFFFFF8u) {
        if (fat32_fat_get(c) == 0) return c;
        c++;
    }
    return 0;
}

/* 分配并链接 needed 个簇，全部标记 EOC。scan_from 记录扫描游标。
 * 返回 0 成功；first/last 返回链首/链尾。 */
static int fat32_alloc_clusters(u32 needed, u32 *scan_from,
                                u32 *first, u32 *last) {
    u32 prev = 0;
    u32 fc = 0, lc = 0;
    u32 cur_scan = *scan_from;
    for (u32 i = 0; i < needed; i++) {
        u32 nc = fat32_find_free_cluster(cur_scan);
        if (nc == 0) return -1;
        cur_scan = nc + 1;
        if (fat32_fat_set(nc, 0x0FFFFFFF) != 0) return -2;
        if (prev) {
            if (fat32_fat_set(prev, nc) != 0) return -3;
        } else {
            fc = nc;
        }
        prev = nc;
        lc = nc;
    }
    *scan_from = cur_scan;
    *first = fc;
    *last = lc;
    return 0;
}

/* 在指定起始 cluster 的目录项中查找 name11（8.3）或 long_name（LFN 长名）。
 * long_name 为 NULL 时仅按 8.3 匹配。is_dir=1 只匹配子目录，0 只匹配文件。
 * 返回 0 成功，out_clus/out_size 返回匹配项的起始 cluster 和大小。
 * LFN 缓冲跨簇遍历持续存活，支持跨簇边界的 LFN 链。 */
static int fat32_find_in_dir(u32 start_clus, const char *name11, int is_dir,
                              u32 *out_clus, u32 *out_size, const char *long_name) {
    if (!g_api || !g_api->block || !g_api->block->device_count) return -1;

    /* LFN 缓冲必须在簇循环外声明：跨簇 LFN 链需要持续存活 */
    fat32_lfn_buf lfn;
    fat32_lfn_init(&lfn);

    u32 clus = start_clus;
    while (fat32_clus_ok(clus)) {
        u32 clus_lba = fat32_clus_lba(clus);
        u8 *cb = g_fat32_cluster;
        int st = fat32_rd(clus_lba, g_vol.spc, cb);
        if (st != 0) return -3;

        const u8 *entries = (const u8 *)cb;
        u32 entry_count = g_vol.cluster_bytes / 32;
        for (u32 e = 0; e < entry_count; e++) {
            const u8 *entry = entries + e * 32;
            if (entry[0] == 0) break;
            if ((u8)entry[0] == 0xE5) {
                fat32_lfn_init(&lfn);
                continue;
            }
            const fat32_dir_entry *de = (const fat32_dir_entry *)entry;
            int is_short = fat32_lfn_process(&lfn, entry);
            if (!is_short) continue;  /* LFN 片段 */
            if (de->attr & 0x08) {
                fat32_lfn_init(&lfn);
                continue;  /* 卷标 */
            }
            int entry_is_dir = (de->attr & 0x10) ? 1 : 0;
            if (entry_is_dir != is_dir) {
                fat32_lfn_init(&lfn);
                continue;
            }
            /* 匹配检查：先 8.3 短名，再 LFN 长名 */
            int matched = 0;
            if (name11 && fat32_name11_eq(de->name, name11)) {
                matched = 1;
            } else if (long_name) {
                if (lfn.valid) {
                    char ascii[FAT32_LFN_MAX];
                    int n = fat32_lfn_to_ascii(&lfn, ascii, sizeof(ascii));
                    if (n >= 0 && fat32_lfn_streq_ci(ascii, long_name)) matched = 1;
                }
                if (!matched) {
                    char short_disp[13];
                    fat32_lfn_short_to_str((const u8 *)de->name, short_disp);
                    if (fat32_lfn_streq_ci(short_disp, long_name)) matched = 1;
                }
            }
            if (matched) {
                *out_clus = fat32_read_u16((const u8 *)&de->cluster_low) |
                            ((u32)fat32_read_u16((const u8 *)&de->cluster_high) << 16);
                *out_size = de->file_size;
                return 0;
            }
            fat32_lfn_init(&lfn);
        }

        /* 跟随 FAT 链 — 按需读 FAT，与卷大小无关 */
        clus = fat32_fat_get(clus);
    }
    return -4;  /* 未找到 */
}

/* 按路径查找文件并读取内容（流式，长度不设上限）。
 *
 * path      : '/' 分隔路径，支持 8.3 与长名
 * buf/cap   : 调用方提供的目标缓冲
 * out_len   : 实际写入字节数
 * out_total : 文件真实大小（可为 NULL）
 *
 * 返回 0 成功；-5 = 目标缓冲不足（此时 out_len 仍给出需要的大小，
 * 便于调用方扩容重试）；-6 = 读盘失败。
 *
 * 旧接口返回 static 缓冲指针，隐含 10MB 上限、且强制调用方拷出（
 * pe_dll_manager / zts_store 都为此写显式拷贝）。流式版本把"读到哪"
 * 交给调用方，不再有隐含上限，也不再依赖共享静态缓冲。 */
int fat32_read_path_buf(const char *path, u8 *buf, u32 cap,
                        u32 *out_len, u32 *out_total) {
    if (!g_api || !g_api->block || !g_api->block->device_count) return -1;
    if (!path || !buf) return -2;
    if (fat32_load_vol() != 0) return -3;

    u32 pos = 0, path_len = 0;
    while (path[path_len]) path_len++;

    u32 cur_clus = g_vol.root_cluster;

    while (pos < path_len) {
        char comp[12];
        char orig[260];
        u32 ci = 0, oi = 0;
        while (pos < path_len && path[pos] != '/') {
            if (ci < 11) comp[ci++] = path[pos];
            if (oi + 1 < sizeof(orig)) orig[oi++] = path[pos];
            pos++;
        }
        while (ci < 11) comp[ci++] = ' ';
        comp[11] = 0;
        orig[oi] = 0;
        if (pos < path_len && path[pos] == '/') pos++;

        int is_last = (pos >= path_len);

        if (is_last) {
            u32 found_clus = 0, found_size = 0;
            if (fat32_find_in_dir(cur_clus, comp, 0, &found_clus, &found_size,
                                  (oi > 0) ? orig : 0) != 0)
                return -4;
            if (out_total) *out_total = found_size;
            if (found_size > cap) {
                if (out_len) *out_len = found_size;
                return -5;   /* 缓冲不足：报告所需大小 */
            }
            /* 逐簇读入调用方缓冲，不做整体中转 */
            u32 written = 0;
            u32 remaining = found_size;
            u32 fc = found_clus;
            while (fat32_clus_ok(fc) && remaining > 0) {
                u32 fc_lba = fat32_clus_lba(fc);
                u32 n = g_vol.cluster_bytes;
                if (n > remaining) n = remaining;
                if (n == g_vol.cluster_bytes) {
                    /* 整簇直接落到目标缓冲 */
                    if (fat32_rd(fc_lba, g_vol.spc, buf + written) != 0) return -6;
                } else {
                    if (g_vol.cluster_bytes > sizeof(g_fat32_cluster)) return -7;
                    if (fat32_rd(fc_lba, g_vol.spc, g_fat32_cluster) != 0) return -6;
                    for (u32 b = 0; b < n; b++) buf[written + b] = g_fat32_cluster[b];
                }
                written += n;
                remaining -= n;
                fc = fat32_fat_get(fc);
            }
            if (out_len) *out_len = written;
            return 0;
        } else {
            u32 sub_clus = 0, sub_size = 0;
            if (fat32_find_in_dir(cur_clus, comp, 1, &sub_clus, &sub_size,
                                  (oi > 0) ? orig : 0) != 0)
                return -7;
            cur_clus = sub_clus;
        }
    }
    return -8;
}

/* 兼容旧接口：静态缓冲 + 指针返回。
 *
 * 保留原因：pe_loader / pe_dll_manager / zts_store / firstinit / login /
 * session 共 6 处经总线按这个签名取数据，且部分调用方已明确按"返回共享
 * 静态缓冲、必须拷出"的方式写。直接改签名会一次性牵动全部调用点。
 *
 * 与旧版的区别：上限不再是 10MB 写死，而是按文件大小动态扩容（用
 * dma->alloc_pages 从内核页分配器要内存，只在文件变大时增长）。
 * 缓冲仍然被复用，所以"必须拷出后再递归读"的语义不变。 */
int fat32_read_path(const char *path, u8 **out_data, u32 *out_size) {
    static u8 *g_rbuf;
    static u32 g_rcap;

    if (!out_data || !out_size) return -2;

    /* 第一趟：先解析大小（cap=0 时若文件非空必返回 -5 且给出 total） */
    u32 need = 0, total = 0;
    u8 one;
    int rc = fat32_read_path_buf(path, &one, 0, &need, &total);
    if (rc == 0) {
        /* 空文件：仍需一个可返回的非空指针 */
        total = 0;
    } else if (rc != -5) {
        return rc;
    }

    u32 want = total ? total : 1;
    if (want > g_rcap) {
        u32 pages = (want + 4095u) / 4096u;
        if (!g_api->dma || !g_api->dma->alloc_pages) {
            g_log->error("[fat32] no dma api; cannot size read buffer");
            return -10;
        }
        struct dkm_dma_buffer nb;
        if (g_api->dma->alloc_pages(pages, 4096, 0x100000000ULL, &nb) != 0) {
            g_log->error("[fat32] read buffer alloc failed");
            log_hex("[fat32] need_bytes=", want);
            return -10;
        }
        /* 旧缓冲不释放：DKM 无 free 接口，且容量单调增长，不会反复泄漏 */
        g_rbuf = (u8 *)nb.virt;
        g_rcap = pages * 4096u;
    }

    /* 第二趟：真正读进（可能刚扩容的）缓冲 */
    u32 got = 0;
    int r2 = fat32_read_path_buf(path, g_rbuf, g_rcap, &got, &total);
    if (r2 != 0) return r2;
    *out_data = g_rbuf;
    *out_size = got;
    return 0;
}

/* ---------------------------------------------------------------
 * FAT32 写入栈（实机启动日志落盘，母本 dsk_loader.c 移植）
 *   1) FAT 条目支持按需读-改-写（超出 256 扇区预读缓冲时逐扇区读写，
 *      并同步镜像到所有 FAT 副本），不受小分区限制。
 *   2) 目录扫描跨簇 + 目录满时自动扩展，文件链支持多簇。
 * 注意：与母本一致，不更新 FSInfo（free count 会过期）。 */


/* 在指定目录（dir_clus 起始簇）写入文件（8.3 名，11 字符）。
 * 存在则覆盖（复用旧链/扩展/截断），否则新建。支持多簇目录与多簇文件。
 * data 必须指向可寻址静态内存。返回 0 成功。总线服务 srv.fs.fat32.write_file_in_dir。 */
int fat32_write_file_in_dir(u32 dir_clus, const char *name11,
                            const u8 *data, u32 size) {
    if (!g_api || !g_api->block || !g_api->block->read || !g_api->block->write) return -1;
    if (fat32_load_vol() != 0) return -2;

    /* ---- 1. 扫描目录链：找同名条目或空闲槽 ---- */
    u32 cur = dir_clus;
    u32 last_dir_clus = dir_clus;
    u32 entry_clus = 0;      /* 目录项所在目录簇 */
    u32 entry_slot = 0;
    int found = 0;           /* 同名文件已存在 */
    u32 old_first_clus = 0;
    int done = 0;
    while (fat32_clus_ok(cur)) {
        last_dir_clus = cur;
        if (fat32_rd(fat32_clus_lba(cur), g_vol.spc, g_fat32_cluster) != 0) return -4;
        u32 me = g_vol.cluster_bytes / 32;
        const fat32_dir_entry *d = (const fat32_dir_entry *)g_fat32_cluster;
        for (u32 e = 0; e < me; e++) {
            if (d[e].name[0] == 0) {           /* 目录结束标记 */
                if (entry_clus == 0) { entry_clus = cur; entry_slot = e; }
                done = 1;
                break;
            }
            if ((u8)d[e].name[0] == 0xE5) {    /* 已删除项，可复用 */
                if (entry_clus == 0) { entry_clus = cur; entry_slot = e; }
                continue;
            }
            if (d[e].attr == 0x0F || (d[e].attr & 0x08)) continue;
            if (fat32_name11_eq(d[e].name, name11)) {
                found = 1;
                entry_clus = cur;
                entry_slot = e;
                old_first_clus = fat32_read_u16((const u8 *)&d[e].cluster_low) |
                                 ((u32)fat32_read_u16((const u8 *)&d[e].cluster_high) << 16);
                done = 1;
                break;
            }
        }
        if (done) break;
        cur = fat32_fat_get(cur);
    }

    /* ---- 2. 目录满时扩展一个簇 ---- */
    if (entry_clus == 0) {
        u32 scan_from = 2;
        u32 nc = fat32_find_free_cluster(scan_from);
        if (nc == 0) return -9;
        if (fat32_fat_set(nc, 0x0FFFFFFF) != 0) return -10;
        if (fat32_fat_set(last_dir_clus, nc) != 0) return -11;
        for (u32 b = 0; b < g_vol.cluster_bytes; b++) g_fat32_cluster[b] = 0;
        if (fat32_wr(fat32_clus_lba(nc), g_vol.spc, g_fat32_cluster) != 0) return -12;
        entry_clus = nc;
        entry_slot = 0;
    }

    /* ---- 3. 文件簇链：复用/扩展/截断 ---- */
    u32 needed = (size + g_vol.cluster_bytes - 1) / g_vol.cluster_bytes;
    if (needed == 0) needed = 1;
    u32 first = 0;
    u32 scan_from = 2;

    if (found && old_first_clus >= 2) {
        /* 走旧链，收集前 needed 个簇 */
        u32 c = old_first_clus;
        u32 cnt = 0;
        u32 last = 0;
        u32 tail = 0;
        while (fat32_clus_ok(c) && cnt < needed) {
            last = c;
            cnt++;
            c = fat32_fat_get(c);
        }
        first = old_first_clus;
        if (cnt == needed) {
            /* 截断：last 指向 EOC，释放旧链尾部 */
            tail = fat32_fat_get(last);
            if (fat32_fat_set(last, 0x0FFFFFFF) != 0) return -13;
            while (fat32_clus_ok(tail)) {
                u32 nn = fat32_fat_get(tail);
                if (fat32_fat_set(tail, 0) != 0) return -14;
                tail = nn;
            }
        } else {
            /* 旧链不足：last 之后补簇 */
            u32 need_more = needed - cnt;
            u32 nf = 0, nl = 0;
            if (fat32_alloc_clusters(need_more, &scan_from, &nf, &nl) != 0)
                return -15;
            if (fat32_fat_set(last, nf) != 0) return -16;
        }
    } else {
        u32 nf = 0, nl = 0;
        if (fat32_alloc_clusters(needed, &scan_from, &nf, &nl) != 0)
            return -17;
        first = nf;
    }

    /* ---- 4. 写数据簇 ---- */
    {
        u32 c = first;
        u32 remaining = size;
        u32 off = 0;
        while (fat32_clus_ok(c) && remaining > 0) {
            u32 lba = fat32_clus_lba(c);
            if (remaining >= g_vol.cluster_bytes) {
                if (fat32_wr(lba, g_vol.spc, data + off) != 0) return -18;
            } else {
                /* 尾簇：补零到整簇再写，避免把上一版本残留写进文件 */
                u8 *cb = g_fat32_cluster;
                for (u32 b = 0; b < g_vol.cluster_bytes; b++) cb[b] = 0;
                for (u32 b = 0; b < remaining; b++) cb[b] = data[off + b];
                if (fat32_wr(lba, g_vol.spc, cb) != 0) return -19;
            }
            if (remaining >= g_vol.cluster_bytes) {
                off += g_vol.cluster_bytes;
                remaining -= g_vol.cluster_bytes;
            } else {
                off += remaining;
                remaining = 0;
            }
            c = fat32_fat_get(c);
        }
    }

    /* ---- 5. 更新目录项 ---- */
    {
        u32 elba = fat32_clus_lba(entry_clus);
        if (fat32_rd(elba, g_vol.spc, g_fat32_cluster) != 0) return -20;
        fat32_dir_entry *dir = (fat32_dir_entry *)g_fat32_cluster;
        fat32_dir_entry *e = &dir[entry_slot];
        if (!found) {
            for (int i = 0; i < 11; i++) e->name[i] = name11[i];
            e->attr = 0x20;                 /* archive */
            e->nt_reserved = 0;
            e->creation_tenth = 0;
            e->creation_time = 0;
            e->creation_date = 0;
            e->access_date = 0;
            e->write_time = 0;
            e->write_date = 0;
        }
        e->cluster_high = (u16)((first >> 16) & 0xFFFF);
        e->cluster_low  = (u16)(first & 0xFFFF);
        e->file_size = size;
        if (fat32_wr(elba, g_vol.spc, g_fat32_cluster) != 0) return -21;
    }

    /* FAT 条目已在第 2/3 步按扇区即时写盘（fat32_fat_set 直接落所有副本），
     * 这里不再需要"把预读缓冲里的 FAT 刷回"那一步。 */

    g_log->info("[fat32] write ok");
    return 0;
}

/* 在根目录写入文件（8.3 名）。总线服务 srv.fs.fat32.write_root_file。 */
int fat32_write_root_file(const char *name11, const u8 *data, u32 size) {
    if (!g_api || !g_api->block || !g_api->block->read) return -1;
    if (fat32_load_vol() != 0) return -2;
    return fat32_write_file_in_dir(g_vol.root_cluster, name11, data, size);
}

/* 按 8.3 路径（每段 11 字符，'/' 分隔）定位子目录，返回起始簇，失败返回 0。
 * 总线服务 srv.fs.fat32.find_dir_path。 */
u32 fat32_find_dir_path(const char *path) {
    if (!g_api || !g_api->block || !g_api->block->read) return 0;
    if (fat32_load_vol() != 0) return 0;

    u32 cur = g_vol.root_cluster;
    u32 pos = 0;
    while (path[pos]) {
        char comp[12];
        u32 ci = 0;
        while (path[pos] && path[pos] != '/') {
            if (ci < 11) comp[ci++] = path[pos];
            pos++;
        }
        while (ci < 11) comp[ci++] = ' ';
        comp[11] = 0;
        if (path[pos] == '/') pos++;
        if (comp[0] == ' ') break;   /* 空段防呆 */
        u32 sub = 0, sz = 0;
        if (fat32_find_in_dir(cur, comp, 1, &sub, &sz, 0) != 0) return 0;
        cur = sub;
    }
    return cur;
}

/* 实机启动日志落盘：写 FAT32 文件。
 * 优先写 SYSTEM/DESHAB64/DEV/BOOTLOG.TXT；DEV 目录缺失时回退根目录 BOOTLOG.TXT。
 * 总线服务 srv.fs.fat32.bootlog。返回 0 成功。 */
int utsm_bootlog_write(const u8 *data, u32 size) {
    if (!g_api || !g_api->block || !g_api->block->device_count) return -1;
    if (g_api->block->device_count() == 0) return -2;
    if (!g_api->block->write) return -3;
    if (!data || size == 0) return -4;

    static const char name11[11] = {'B','O','O','T','L','O','G',' ','T','X','T'};
    u32 dev_clus = fat32_find_dir_path("SYSTEM  /DESHAB64 /DEV     ");
    if (dev_clus >= 2)
        return fat32_write_file_in_dir(dev_clus, name11, data, size);
    return fat32_write_root_file(name11, data, size);
}

/* ---------------------------------------------------------------
 * 块设备探测：读一次 BPB 并校验。成功则 g_ready=1 允许注册服务。
 * 无块设备 / 非 FAT32 卷 → warn 后正常返回（设计内回退：环境能力缺失，
 * 与「块设备缺失回 boot module」白名单同源，不 panic）。 */
/* 启动盘的 GPT 磁盘 UUID（来自 Limine boot module 的 gpt_disk_uuid）。
 * 用于在多个块设备中精确锁定与引导器同一块盘；ESP 分区表里 p1 是
 * FAT32。EFI stub 路径下该字段可能为全 0（未知），此时退回目录特征打分。 */
typedef struct {
    u64 revision;
    u64 module_count;
    void **modules;
} fat32_mod_response;

typedef struct {
    u64 revision;
    void *address;
    u64 size;
    char *path;
    char *cmdline;
    u32 media_type;
    u32 unused;
    u32 tftp_ip;
    u32 tftp_port;
    u32 partition_index;
    u32 mbr_disk_id;
    u8  gpt_disk_uuid[16];
    u8  gpt_part_uuid[16];
    u8  part_uuid[16];
} fat32_mod_file;

static int fat32_uuid_nonzero(const u8 *u) {
    for (int i = 0; i < 16; i++) if (u[i]) return 1;
    return 0;
}

static const u8 *fat32_boot_disk_uuid(void) {
    const fat32_mod_response *rsp =
        (const fat32_mod_response *)g_api->boot_modules_response;
    if (!rsp || !rsp->modules) return 0;
    for (u64 i = 0; i < rsp->module_count; i++) {
        const fat32_mod_file *f = (const fat32_mod_file *)rsp->modules[i];
        if (f && fat32_uuid_nonzero(f->gpt_disk_uuid)) return f->gpt_disk_uuid;
    }
    return 0;
}

/* 引导器传来的启动卷 FAT32 卷序列号（cmdline "boot:volid"，4 字节载荷）。
 * EFI stub 从 UEFI BlockIo 的 BPB 读取；Limine 路径下不存在（返回 0）。 */
static u32 fat32_boot_vol_serial(void) {
    const fat32_mod_response *rsp =
        (const fat32_mod_response *)g_api->boot_modules_response;
    if (!rsp || !rsp->modules) return 0;
    for (u64 i = 0; i < rsp->module_count; i++) {
        const fat32_mod_file *f = (const fat32_mod_file *)rsp->modules[i];
        if (!f || !f->cmdline || !f->address || f->size < 4) continue;
        const char *a = f->cmdline, *b = "boot:volid";
        int match = 1;
        for (int k = 0; ; k++) {
            if (a[k] != b[k]) { match = 0; break; }
            if (!a[k]) break;
        }
        if (match) return *(const u32 *)f->address;
    }
    return 0;
}

/* 设备 dev 的 GPT 磁盘 UUID 是否等于 boot UUID。
 * 非 GPT 盘 / 读失败一律视为不匹配。 */
static int fat32_dev_uuid_match(u32 dev, const u8 *uuid, u8 *lba1_out) {
    u8 sec[512];
    if (!uuid) return 0;
    if (g_api->block->read(dev, 1, 1, sec) != 0) return 0;
    if (!(sec[0]=='E'&&sec[1]=='F'&&sec[2]=='I'&&sec[3]==' ')) return 0;
    if (lba1_out) {
        for (int i = 0; i < 512; i++) lba1_out[i] = sec[i];
    }
    for (int i = 0; i < 16; i++) {
        if (sec[56 + i] != uuid[i]) return 0;
    }
    return 1;
}

/* 卷特征打分：Deshab ESP 根目录同时含 DRIVER 与 BOOT 目录。
 * 返回 0..2；用于 boot UUID 未知（EFI stub / 非 GPT）时的选择。 */
static u32 fat32_root_score(void) {
    u32 s = 0;
    if (fat32_find_dir_path("DRIVER  ") >= 2) s++;
    if (fat32_find_dir_path("BOOT    ") >= 2) s++;
    return s;
}

static void fat32_probe_and_seed(void) {
    if (!g_api->block || !g_api->block->read || !g_api->block->device_count
        || !g_api->block->sector_size) {
        g_log->warn("[fat32] no block provider; srv.fs.fat32.* unavailable");
        return;
    }
    u32 count = g_api->block->device_count();
    if (count == 0) {
        g_log->warn("[fat32] no block devices; srv.fs.fat32.* unavailable");
        return;
    }
    log_hex("[fat32] block device count=", count);

    /* 实机多盘场景：AHCI/NVMe 通常先于 xHCI 注册，块设备 0 往往是内置
     * 系统盘而不是启动 U 盘。逐个探测，优先 boot module 的 GPT 磁盘 UUID
     * 精确匹配；UUID 未知时按根目录 DRIVER+BOOT 特征打分。 */
    const u8 *boot_uuid = fat32_boot_disk_uuid();
    u32 boot_vol = fat32_boot_vol_serial();
    if (boot_vol) log_hex("[fat32] boot volume serial=", boot_vol);
    int best_score = -1;
    u32 best_dev = 0;
    u32 best_lba = 0;

    for (u32 dev = 0; dev < count; dev++) {
        u64 ss = g_api->block->sector_size(dev);
        if (ss != 512) {
            log_hex("[fat32] skip device sector_size=", ss);
            continue;
        }
        u32 lba = (u32)fat32_part_probe((fat32_part_read_fn)g_api->block->read, dev);
        {
            /* 诊断：GPT 头与 ESP 候选扇区的原始可读性（实机多盘定位用） */
            u8 dbg[512];
            int r1 = g_api->block->read(dev, 1, 1, dbg);
            log_hex("[fat32] dev gpt hdr rc=", (u64)(i64)r1);
            if (r1 == 0) {
                log_hex("[fat32]   sig0=", *(const u32 *)dbg);
                log_hex("[fat32]   sig4=", *(const u32 *)(dbg + 4));
            }
            int r2 = g_api->block->read(dev, 2048, 1, dbg);
            log_hex("[fat32] dev lba2048 rc=", (u64)(i64)r2);
            if (r2 == 0) {
                log_hex("[fat32]   boot_sig=", (u64)(dbg[510] | (dbg[511] << 8)));
                log_hex("[fat32]   bps=", (u64)(dbg[11] | (dbg[12] << 8)));
            }
            log_hex("[fat32] probe lba=", lba);
        }
        /* 临时切到候选设备，供 BPB 校验与目录打分使用。 */
        u32 saved_dev = g_fat_dev, saved_lba = g_fat_lba;
        g_fat_dev = dev;
        g_fat_lba = lba;
        if (fat32_load_vol() != 0) {
            g_fat_dev = saved_dev; g_fat_lba = saved_lba;
            log_hex("[fat32] device BPB read failed, dev=", dev);
            continue;
        }
        int ok = (g_vol.spc != 0 && g_vol.fat_sectors != 0 && g_vol.root_cluster >= 2);
        if (!ok) {
            log_hex("[fat32] candidate bad BPB dev=", dev);
            log_hex("[fat32]   lba=", lba);
            g_fat_dev = saved_dev; g_fat_lba = saved_lba;
            continue;
        }
        int score;
        if (boot_vol && g_vol.serial == boot_vol) {
            score = 200;   /* 卷序列号与引导器一致：决定性 */
        } else if (boot_uuid && fat32_dev_uuid_match(dev, boot_uuid, 0)) {
            score = 100;   /* GPT 磁盘 UUID 与引导器一致 */
        } else {
            score = (int)fat32_root_score();
        }
        log_hex("[fat32] candidate dev=", dev);
        log_hex("[fat32] candidate lba=", lba);
        log_hex("[fat32] candidate score=", (u64)score);
        if (score > best_score) {
            best_score = score;
            best_dev = dev;
            best_lba = lba;
        }
        g_fat_dev = saved_dev; g_fat_lba = saved_lba;
    }

    if (best_score < 0) {
        g_log->warn("[fat32] no FAT32 volume found; srv.fs.fat32.* unavailable");
        return;
    }

    g_fat_dev = best_dev;
    g_fat_lba = best_lba;
    log_hex("[fat32] selected dev=", g_fat_dev);
    log_hex("[fat32] volume lba=", g_fat_lba);

    if (fat32_load_vol() != 0) {
        g_log->warn("[fat32] BPB read failed; srv.fs.fat32.* unavailable");
        return;
    }
    g_ready = 1;
    g_log->info("[fat32] FAT32 volume live");
    log_hex("[fat32] root_cluster=", g_vol.root_cluster);
    log_hex("[fat32] sectors_per_cluster=", g_vol.spc);
    log_hex("[fat32] sectors_per_fat=", g_vol.fat_sectors);
    log_hex("[fat32] total_clusters=", g_vol.total_clusters);
}

/* srv.fs.fat32.volume_info：公布启动卷所在块设备索引。
 * desktop/cmd 的 fat32_io.h 硬编码设备 0，实机多盘时会读错盘。 */
static struct dkm_fat32_volume_info g_volinfo;

static const struct dkm_fat32_volume_info *fat32_volume_info_get(void) {
    return &g_volinfo;
}

/* firstinit 扩大 ESP 后重读 BPB。卷起始 LBA 不变，簇大小/FAT 长度会变。 */
static int fat32_reload(void) {
    if (!g_ready) return -1;
    g_fat_sec_valid = 0;
    if (fat32_load_vol() != 0) {
        g_ready = 0;
        g_volinfo.valid = 0;
        g_log->panic("FAT32-E02 RELOAD FAILED");
        return -1;
    }
    g_volinfo.lba_base = g_fat_lba;
    g_volinfo.vol_serial = g_vol.serial;
    g_log->info("[fat32] volume reloaded");
    log_hex("[fat32] sectors_per_fat=", g_vol.fat_sectors);
    log_hex("[fat32] total_clusters=", g_vol.total_clusters);
    return 0;
}

/* ---------------------------------------------------------------
 * Driver entry
 * --------------------------------------------------------------- */
__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;
    if (!api || !api->log) return -1;
    g_log = api->log;
    g_api = api;
    dkm_instr_init(api);

    fat32_probe_and_seed();

    g_volinfo.valid = (u32)g_ready;
    g_volinfo.dev = g_fat_dev;
    g_volinfo.lba_base = g_fat_lba;
    g_volinfo.vol_serial = g_ready ? g_vol.serial : 0;

    /* 通过服务总线暴露 FAT32 能力（srv.* 命名空间）。 */
    if (g_ready && g_api->bus) {
        g_api->bus->register_service("srv.fs.fat32.read_path", 1, (void *)fat32_read_path);
        g_api->bus->register_service("srv.fs.fat32.write_root_file", 1, (void *)fat32_write_root_file);
        g_api->bus->register_service("srv.fs.fat32.find_dir_path", 1, (void *)fat32_find_dir_path);
        g_api->bus->register_service("srv.fs.fat32.write_file_in_dir", 1, (void *)fat32_write_file_in_dir);
        g_api->bus->register_service("srv.fs.fat32.bootlog", 1, (void *)utsm_bootlog_write);
        g_api->bus->register_service("srv.fs.fat32.volume_info", 1, (void *)fat32_volume_info_get);
        g_api->bus->register_service("srv.fs.fat32.reload", 1, (void *)fat32_reload);
        g_log->info("[fat32] srv.fs.fat32.* registered");
    } else if (g_ready) {
        g_log->warn("[fat32] no service bus; srv.fs.fat32.* not registered");
    }

    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle; return 0;
}
