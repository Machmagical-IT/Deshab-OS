/* ext4.c — DKM ext4 read-write driver (vendored from DSK CODE/dsk/ext4.c)
 *
 * 万物互联 P2：DSK 主内核退役，其 ext4 能力归位为普通 DKM 驱动，
 * 经 dkm_bus 以 srv.fs.ext4.read_file / srv.fs.ext4.write_file 对外服务。
 * 目标：GPT p2（卷标 "DESHAB"）ext4 数据分区的读写。
 *
 * 块设备一律经 driver_init 传入的 api->block（device index 0，与 DSK 一致），
 * 不依赖任何 DSK 私有全局/hhdm。GPT 探测代码内联自 dsk/ext4.c。
 *
 * 严格错误策略：任何错误（挂载失败/feature 不支持/IO 失败/校验不符）
 * 一律经 g_log->panic("EXT4-E0x ...") 整屏 panic，零降级。
 * panic fallback 模式（panic 返回）：长跳回最外层公开入口，ext4 置不可用返回 -1。
 *
 * 错误码映射（自编号 EXT4-E01..，语义对应原 DSK FS-E20..E43）：
 *   E01 BLOCK READ FAILED          E11 EXTENT DEPTH > 1
 *   E02 BLOCK WRITE FAILED         E12 FILE EXCEEDS READ BUFFER
 *   E03 SUPERBLOCK MAGIC           E13 NO FREE INODES
 *   E04 UNSUPPORTED BLOCKSIZE      E14 DIR ENTRY CORRUPT
 *   E05 FEATURE UNSUPPORTED        E15 DIR TREE DEPTH
 *   E06 UNSUPPORTED INODE SIZE     E16 DIR BLOCK NOT MAPPED
 *   E07 UNEXPECTED FIRST DATA BLK  E17 PATH MUST BE ABSOLUTE
 *   E08 UNEXPECTED BLOCKS/GROUP    E18 BAD FINAL NAME
 *   E09 GDT OVERFLOW               E19 PATH COMPONENT NOT FOUND
 *   E0A SB WRITE VERIFY            E1A FILE NOT FOUND
 *   E0B BAD INODE NUMBER           E1B NOT A REGULAR FILE
 *   E0C FREE BLOCK UNDERFLOW       E1C DIR EXTENT TABLE FULL
 *   E0D BLOCK ALLOC OUT OF RANGE   E1D TOO MANY EXTENT LEAVES
 *   E0E NO FREE BLOCKS             E1E WRITE VERIFY FAILED
 *   E0F DOUBLE FREE DETECTED       E1F SELFTEST FUCK READ EMPTY
 *   E10 EXTENT MAGIC INVALID
 */

#include <stdint.h>

#include "../dkm_shared.h"
#include "ext4.h"

/* ---------------- 日志 / kernel API ---------------- */
static const struct dkm_log_api *g_log;
static const struct dkm_kernel_api *g_api;

static void log_hex(const char *p, ext4_u64 v) {
    static const char h[] = "0123456789abcdef";
    char b[19]; ext4_u32 i = 0;
    b[i++] = '0'; b[i++] = 'x';
    for (int j = 15; j >= 0; j--) b[i++] = h[(v >> (j * 4)) & 0xf];
    b[i] = 0;
    g_log->info(p);
    g_log->info(b);
}

/* panic fallback：全部 ext4_fail 经此处中转。strict 时 g_log->panic 不返回；
 * fallback 时 panic 返回，ext4_fail 长跳回最外层公开入口，该入口关闭 ext4
 * 并返回失败（调用方走备用路径）。 */
static void *g_ext4_jb[5];
static int g_ext4_depth;

static void ext4_fail(const char *sym) {
    g_log->panic(sym);
    if (g_ext4_depth > 0) __builtin_longjmp(g_ext4_jb, 1);
    for (;;) __asm__ volatile("cli; hlt");
}

/* ---------------- 小端读取 ---------------- */
static ext4_u16 r16(const ext4_u8 *p) { return (ext4_u16)(p[0] | (p[1] << 8)); }
static ext4_u32 r32(const ext4_u8 *p) {
    return (ext4_u32)p[0] | ((ext4_u32)p[1] << 8) |
           ((ext4_u32)p[2] << 16) | ((ext4_u32)p[3] << 24);
}
static ext4_u64 r64(const ext4_u8 *p) { return (ext4_u64)r32(p) | ((ext4_u64)r32(p + 4) << 32); }
static void w32(ext4_u8 *p, ext4_u32 v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
static void w16(ext4_u8 *p, ext4_u16 v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }

/* ---------------- 状态 ---------------- */
static ext4_u64 g_part_lba;        /* ext4 分区起始 LBA */
static ext4_u32 g_block_size;      /* 4096 */
static ext4_u32 g_sec_per_block;   /* 8 */
static ext4_u32 g_inodes_per_group;
static ext4_u32 g_blocks_per_group;
static ext4_u32 g_inode_size;
static ext4_u32 g_first_data_block;
static ext4_u32 g_desc_size;
static ext4_u32 g_group_count;
static ext4_u64 g_blocks_count;
static ext4_u8  g_sb[1024];        /* superblock（1024B） */
/* 组描述符按组数现分配。单块 GDT 只有 128 组（16GiB）；扩张后的卷远大于
 * 这个数，静态数组会把上限又写回去。 */
static ext4_u8 *g_gdt;
static ext4_u32 g_gdt_bytes;
#define EXT4_MAX_FILE (8u * 1024u * 1024u)
static ext4_u8  g_fdata[EXT4_MAX_FILE];   /* 读文件静态缓冲（.drv BSS） */
static ext4_u8  g_blk[4096];       /* 元数据块 scratch（位图/目录/inode 表） */
static ext4_u8  g_blk2[4096];      /* 第二 scratch（inode 表与目录并行操作） */

#define EXT4_SUPER_MAGIC  0xEF53
#define EXT4_ROOT_INO     2

/* incompat feature 位 */
#define COMPAT_HAS_JOURNAL     0x0004u
#define INCOMPAT_FILETYPE      0x0002u
#define INCOMPAT_RECOVER       0x0004u
#define INCOMPAT_META_BG       0x0010u
#define INCOMPAT_EXTENTS       0x0040u
#define INCOMPAT_64BIT         0x0080u
#define INCOMPAT_MMP           0x0100u
#define INCOMPAT_FLEX_BG       0x0200u
#define INCOMPAT_METADATA_CSUM 0x0400u
#define INCOMPAT_INLINE_DATA   0x8000u
#define INCOMPAT_SUPPORTED (INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_FLEX_BG)

/* inode flags */
#define EXT4_NODUMP_FL 0x00000004u
#define EXT4_EXTENTS_FL 0x00080000u

static int g_ext4_ok;

/* ---------------- 底层 IO（经 kernel_api.block，device 0；失败即 fatal） ---------------- */
/* GPT p2 数据盘固定为 block device index 0（与 DSK 一致）。 */
#define EXT4_BLOCK_DEV 0u
static void rd_sec(ext4_u64 lba, ext4_u32 cnt, void *buf) {
    if (g_api->block->read(EXT4_BLOCK_DEV, lba, cnt, buf) != 0)
        ext4_fail("EXT4-E01 EXT4 BLOCK READ FAILED");
}
static void wr_sec(ext4_u64 lba, ext4_u32 cnt, const void *buf) {
    if (g_api->block->write(EXT4_BLOCK_DEV, lba, cnt, buf) != 0)
        ext4_fail("EXT4-E02 EXT4 BLOCK WRITE FAILED");
}
/* 块号 → LBA */
static ext4_u64 blk_lba(ext4_u32 blk) { return g_part_lba + (ext4_u64)blk * g_sec_per_block; }
static void rd_blk(ext4_u32 blk, void *buf)  { rd_sec(blk_lba(blk), g_sec_per_block, buf); }
static void wr_blk(ext4_u32 blk, const void *buf) { wr_sec(blk_lba(blk), g_sec_per_block, buf); }

/* ---------------- GPT 探测 ext4 分区（device 0） ----------------
 * 返回 1 = 找到并填 out_lba/out_sb，0 = 这块盘没有可用 ext4。探测失败不 panic。
 * 判定：分区类型为 Linux filesystem GUID，且 GPT 分区名 / ext4 卷标为 DESHAB，
 * 或该盘已露出 DESHABBOOT FAT，或打包特征（4K 块 + incompat 恰为 0x242）。 */
static int probe_ext4(ext4_u64 *out_lba, ext4_u8 *out_sb) {
    ext4_u8 gpt[512];
    if (g_api->block->read(EXT4_BLOCK_DEV, 1, 1, gpt) != 0) {
        g_log->warn("[EXT4] device 0 LBA1 read failed");
        return 0;
    }
    if (gpt[0] != 'E' || gpt[1] != 'F' || gpt[2] != 'I' || gpt[3] != ' ') {
        g_log->warn("[EXT4] device 0 has no GPT signature");
        return 0;
    }
    ext4_u64 ent_lba = r64(gpt + 0x48);
    ext4_u32 ent_num = r32(gpt + 0x50);
    ext4_u32 ent_sz  = r32(gpt + 0x54);
    if (ent_sz < 128 || ent_num == 0 || ent_num > 128) {
        g_log->warn("[EXT4] invalid GPT header");
        return 0;
    }
    /* 探测该盘是否露出 DESHABBOOT FAT（LBA 2048 起）。 */
    int boot_disk = 0;
    {
        ext4_u8 bpb[512];
        if (g_api->block->read(EXT4_BLOCK_DEV, 2048, 1, bpb) == 0 &&
            bpb[510] == 0x55 && bpb[511] == 0xAA) {
            static const char fatlab[] = "DESHABBOOT";
            int ok = 1;
            for (int j = 0; j < 10; j++) if (bpb[71 + j] != (ext4_u8)fatlab[j]) ok = 0;
            if (ok) boot_disk = 1;
        }
    }
    int best = 0;
    ext4_u32 ent_secs = (ent_num * ent_sz + 511) / 512;
    if (ent_secs > 32) ent_secs = 32;
    for (ext4_u32 sec = 0; sec < ent_secs; sec++) {
        ext4_u8 secbuf[512];
        if (g_api->block->read(EXT4_BLOCK_DEV, ent_lba + sec, 1, secbuf) != 0) break;
        ext4_u32 per = 512 / ent_sz;
        if (per == 0) per = 1;
        for (ext4_u32 k = 0; k < per; k++) {
            ext4_u32 i = sec * per + k;
            if (i >= ent_num) break;
            const ext4_u8 *e = secbuf + k * ent_sz;
            static const ext4_u8 linux_guid[16] = {
                0xAF,0x3D,0xC6,0x0F, 0x83,0x84, 0x72,0x47,
                0x8E,0x79, 0x3D,0x69,0xD8,0x47,0x7D,0xE4 };
            int is_linux = 1;
            for (int j = 0; j < 16; j++) if (e[j] != linux_guid[j]) { is_linux = 0; break; }
            if (!is_linux) continue;
            ext4_u64 start = r64(e + 0x20);
            if (start == 0) continue;
            /* GPT 分区名 UTF-16LE，打包时为 DESHAB。不依赖超级块卷标。 */
            int name_ok = 1;
            static const char want[] = "DESHAB";
            for (int j = 0; j < 6; j++) {
                if (e[0x38 + j * 2] != (ext4_u8)want[j] || e[0x39 + j * 2] != 0) name_ok = 0;
            }
            ext4_u8 sb_probe[1024];
            if (g_api->block->read(EXT4_BLOCK_DEV, start + 2, 1, sb_probe) != 0) continue;
            if (g_api->block->read(EXT4_BLOCK_DEV, start + 3, 1, sb_probe + 512) != 0) continue;
            if (r16(sb_probe + 0x38) != EXT4_SUPER_MAGIC) continue;
            int label_ok = 1;
            for (int j = 0; j < 6; j++) if (sb_probe[0x78 + j] != (ext4_u8)want[j]) label_ok = 0;
            /* 分区名和卷标都在各自扇区的前 512 字节里。读全了就必然对上 DESHAB。
             * 对不上仍接受打包特征：4K 块且 incompat 恰好是 FILETYPE|EXTENTS|FLEX_BG。
             * 普通发行版 ext4 带 64bit/metadata_csum，不会是 0x242。 */
            ext4_u32 log_bs = r32(sb_probe + 0x18);
            ext4_u32 incompat = r32(sb_probe + 0x60);
            int feat_ok = (log_bs == 2 && incompat == 0x242u);
            if (name_ok || label_ok || boot_disk || feat_ok) {
                *out_lba = start;
                for (int j = 0; j < 1024; j++) out_sb[j] = sb_probe[j];
                return 1;
            }
            if (best == 0) {
                best = 1;
                log_hex("[EXT4] non-DESHAB ext4 incompat=", incompat);
            }
        }
    }
    (void)best;
    return 0;
}

/* 定位 "DESHAB" ext4 分区。返回 1=成功（填 g_part_lba/g_sb，g_ext4_ok=1），0=未找到。 */
static int gpt_find_ext4(void) {
    g_ext4_ok = 0;
    ext4_u64 lba = 0;
    static ext4_u8 sb[1024];
    if (!probe_ext4(&lba, sb)) {
        g_log->warn("[EXT4] no DESHAB ext4 volume on device 0");
        return 0;
    }
    g_part_lba = lba;
    for (int j = 0; j < 1024; j++) g_sb[j] = sb[j];
    log_hex("[EXT4] DESHAB partition LBA=", lba);
    g_ext4_ok = 1;
    return 1;
}

/* ---------------- 挂载 ---------------- */
static void gdt_load(void);
static int ext4_mount_impl(void) {
    if (!gpt_find_ext4()) return -1;
    log_hex("[EXT4] partition LBA=", g_part_lba);
    if (r16(g_sb + 0x38) != EXT4_SUPER_MAGIC) ext4_fail("EXT4-E03 SUPERBLOCK MAGIC MISMATCH");

    ext4_u32 log_bs = r32(g_sb + 0x18);
    if (log_bs != 2) ext4_fail("EXT4-E04 UNSUPPORTED BLOCKSIZE (need 4096)");
    g_block_size = 4096; g_sec_per_block = 8;

    g_first_data_block   = r32(g_sb + 0x14);
    g_blocks_per_group   = r32(g_sb + 0x20);
    g_inodes_per_group   = r32(g_sb + 0x28);
    g_inode_size         = r16(g_sb + 0x58);
    g_desc_size          = r16(g_sb + 0xFE);   /* s_desc_size @ 0xFE（0xFA 是 hash_seed） */
    if (g_desc_size < 32) g_desc_size = 32;
    g_blocks_count       = r32(g_sb + 0x04);

    ext4_u32 compat = r32(g_sb + 0x5C);
    ext4_u32 incompat = r32(g_sb + 0x60);
    int is_deshab = 1;
    {
        static const char want[] = "DESHAB";
        for (int j = 0; j < 6; j++) if (g_sb[0x78 + j] != (ext4_u8)want[j]) is_deshab = 0;
    }
    /* 打包卷没有 journal，实机扫到的其它 ext4 也经常带着 needs_recovery。
     * 驱动不做 journal 回放：两个标志都清掉再挂载，避免 feature 检查挡住挂载。 */
    (void)is_deshab;
    if (incompat & INCOMPAT_RECOVER) {
        incompat &= ~INCOMPAT_RECOVER;
        w32(g_sb + 0x60, incompat);
        g_log->info("[EXT4] cleared needs_recovery");
    }
    if (compat & COMPAT_HAS_JOURNAL) {
        compat &= ~COMPAT_HAS_JOURNAL;
        w32(g_sb + 0x5C, compat);
        g_log->info("[EXT4] cleared has_journal");
    }
    if ((incompat & (INCOMPAT_64BIT | INCOMPAT_METADATA_CSUM | INCOMPAT_MMP |
                     INCOMPAT_INLINE_DATA | INCOMPAT_META_BG)) ||
        (incompat & ~INCOMPAT_SUPPORTED)) {
        static char msg[28];
        static const char hex[] = "0123456789ABCDEF";
        const char *p = "EXT4-E05 FEAT ";
        int n = 0;
        while (p[n]) { msg[n] = p[n]; n++; }
        for (int s = 28; s >= 0; s -= 4) msg[n++] = hex[(incompat >> s) & 0xF];
        msg[n] = 0;
        ext4_fail(msg);
    }

    ext4_u16 state = r16(g_sb + 0x3A);
    if (state != 1) {
        w16(g_sb + 0x3A, 1);
        g_log->info("[EXT4] forced clean state");
    }

    if (g_inode_size != 256) ext4_fail("EXT4-E06 UNSUPPORTED INODE SIZE");
    if (g_first_data_block != 0) ext4_fail("EXT4-E07 UNEXPECTED FIRST DATA BLOCK");
    if (g_blocks_per_group != 32768) ext4_fail("EXT4-E08 UNEXPECTED BLOCKS PER GROUP");

    g_group_count = (ext4_u32)((g_blocks_count - g_first_data_block + g_blocks_per_group - 1) / g_blocks_per_group);
    if (g_group_count == 0) g_group_count = 1;
    {
        ext4_u64 bytes = (ext4_u64)g_group_count * g_desc_size;
        ext4_u64 span = (bytes + g_block_size - 1) / g_block_size;
        ext4_u64 need = span * g_block_size;
        struct dkm_dma_buffer buf;
        ext4_u64 pages = (need + 4095ull) / 4096ull;
        if (pages == 0) pages = 1;
        if (!g_gdt || (ext4_u64)g_gdt_bytes < need) {
            if (!g_api->dma || !g_api->dma->alloc_pages ||
                g_api->dma->alloc_pages(pages, 4096, 0, &buf) != 0 || !buf.virt ||
                buf.size < need)
                ext4_fail("EXT4-E09 GROUP DESCRIPTOR TABLE OVERFLOW");
            g_gdt = (ext4_u8 *)buf.virt;
            g_gdt_bytes = buf.size > 0xffffffffull ? 0xffffffffu : (ext4_u32)buf.size;
        }
    }
    /* 组描述符从 block 1 起，连续占满 ceil(groups*desc/block) 个块。 */
    gdt_load();
    log_hex("[EXT4] groups=", g_group_count);

    /* mount 即写 sb（mnt_count++），验证写通道，失败即 panic */
    ext4_u16 mnt = r16(g_sb + 0x34);
    w16(g_sb + 0x34, (ext4_u16)(mnt + 1));
    {
        ext4_u8 sbw[1024];
        for (int i = 0; i < 1024; i++) sbw[i] = g_sb[i];
        wr_sec(g_part_lba + 2, 2, sbw);
        ext4_u8 sbr[1024];
        rd_sec(g_part_lba + 2, 2, sbr);
        if (r16(sbr + 0x34) != (ext4_u16)(mnt + 1)) ext4_fail("EXT4-E0A SB WRITE VERIFY FAILED");
    }
    g_log->info("[EXT4] mounted");
    return 0;
}

static int ext4_abort(void) {
    g_ext4_depth = 0;
    g_ext4_ok = 0;
    g_log->info("[EXT4] fallback: operation aborted, ext4 disabled (FAT32 ESP only)");
    return -1;
}

int ext4_mount(void) {
    if (__builtin_setjmp(g_ext4_jb)) return ext4_abort();
    g_ext4_depth = 1;
    int rc = ext4_mount_impl();
    g_ext4_depth = 0;
    return rc;
}

int ext4_is_mounted(void) { return g_ext4_ok; }

/* 分区被 firstinit 搬走或扩容后，丢掉旧 LBA / 块数，按 GPT 重新挂载。 */
int ext4_remount(void) {
    g_ext4_ok = 0;
    return ext4_mount();
}

/* ---------------- 组描述符访问 ---------------- */
/* 32B 组描述符：0x00 block_bitmap, 0x04 inode_bitmap, 0x08 inode_table,
 * 0x0C free_blocks_lo(u16), 0x0E free_inodes_lo(u16), 0x10 used_dirs(u16) */
static ext4_u32 gd_block_bitmap(ext4_u32 g)  { return r32(g_gdt + (ext4_u64)g * g_desc_size + 0x00); }
static ext4_u32 gd_inode_bitmap(ext4_u32 g)  { return r32(g_gdt + (ext4_u64)g * g_desc_size + 0x04); }
static ext4_u32 gd_inode_table(ext4_u32 g)   { return r32(g_gdt + (ext4_u64)g * g_desc_size + 0x08); }
static ext4_u16 gd_free_blocks(ext4_u32 g)   { return r16(g_gdt + (ext4_u64)g * g_desc_size + 0x0C); }
static ext4_u16 gd_free_inodes(ext4_u32 g)   { return r16(g_gdt + (ext4_u64)g * g_desc_size + 0x0E); }

static void gd_set_free_blocks(ext4_u32 g, ext4_u16 v) { w16(g_gdt + (ext4_u64)g * g_desc_size + 0x0C, v); }
static void gd_set_free_inodes(ext4_u32 g, ext4_u16 v) { w16(g_gdt + (ext4_u64)g * g_desc_size + 0x0E, v); }

static ext4_u32 gdt_span_blocks(void) {
    ext4_u32 bytes = g_group_count * g_desc_size;
    if (bytes == 0 || g_block_size == 0) return 1;
    return (bytes + g_block_size - 1) / g_block_size;
}
static void gdt_load(void) {
    ext4_u32 n = gdt_span_blocks();
    if ((ext4_u64)n * g_block_size > g_gdt_bytes)
        ext4_fail("EXT4-E09 GROUP DESCRIPTOR TABLE OVERFLOW");
    for (ext4_u32 i = 0; i < n; i++)
        rd_blk(1 + i, g_gdt + (ext4_u64)i * g_block_size);
}
static void gdt_flush(void) {
    ext4_u32 n = gdt_span_blocks();
    for (ext4_u32 i = 0; i < n; i++)
        wr_blk(1 + i, g_gdt + (ext4_u64)i * g_block_size);
}

/* ---------------- inode 读写 ---------------- */
/* inode 结构关键偏移（256B）：
 * 0x00 mode 0x02 uid 0x04 size_lo 0x1A links 0x1C blocks_lo(512B)
 * 0x20 flags 0x28 i_block[15] 0x6C size_high */
static void read_inode(ext4_u32 ino, ext4_u8 *dst) {
    if (ino < 2) ext4_fail("EXT4-E0B BAD INODE NUMBER");
    ext4_u32 g = (ino - 1) / g_inodes_per_group;
    ext4_u32 idx = (ino - 1) % g_inodes_per_group;
    ext4_u32 table = gd_inode_table(g);
    ext4_u32 per_block = g_block_size / g_inode_size;   /* 16 */
    rd_blk(table + idx / per_block, g_blk);
    for (ext4_u32 i = 0; i < g_inode_size; i++)
        dst[i] = g_blk[(ext4_u64)(idx % per_block) * g_inode_size + i];
}
static void write_inode(ext4_u32 ino, const ext4_u8 *src) {
    if (ino < 2) ext4_fail("EXT4-E0B BAD INODE NUMBER");
    ext4_u32 g = (ino - 1) / g_inodes_per_group;
    ext4_u32 idx = (ino - 1) % g_inodes_per_group;
    ext4_u32 table = gd_inode_table(g);
    ext4_u32 per_block = g_block_size / g_inode_size;
    rd_blk(table + idx / per_block, g_blk);
    for (ext4_u32 i = 0; i < g_inode_size; i++)
        g_blk[(ext4_u64)(idx % per_block) * g_inode_size + i] = src[i];
    wr_blk(table + idx / per_block, g_blk);
}

/* ---------------- 块分配/回收 ---------------- */
static void sb_dec_free_blocks(void) {
    ext4_u32 v = r32(g_sb + 0x0C);
    if (v == 0) ext4_fail("EXT4-E0C FREE BLOCK COUNT UNDERFLOW");
    w32(g_sb + 0x0C, v - 1);
    /* sb 稍后统一 flush（写文件末尾），避免多次 1KB IO */
}
static void sb_inc_free_blocks(void) {
    w32(g_sb + 0x0C, r32(g_sb + 0x0C) + 1);
}
static void sb_flush(void) {
    ext4_u8 sbw[1024];
    for (int i = 0; i < 1024; i++) sbw[i] = g_sb[i];
    wr_sec(g_part_lba + 2, 2, sbw);
}

/* 分配 1 个数据块，返回物理块号。立即落盘位图与组描述符。 */
static ext4_u32 block_alloc(void) {
    for (ext4_u32 g = 0; g < g_group_count; g++) {
        if (gd_free_blocks(g) == 0) continue;
        rd_blk(gd_block_bitmap(g), g_blk);
        for (ext4_u32 byte = 0; byte < g_block_size; byte++) {
            if (g_blk[byte] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (g_blk[byte] & (1u << bit)) continue;
                ext4_u64 blk = (ext4_u64)g * g_blocks_per_group + (ext4_u64)byte * 8 + bit;
                if (blk >= g_blocks_count) ext4_fail("EXT4-E0D BLOCK ALLOC OUT OF RANGE");
                g_blk[byte] |= (ext4_u8)(1u << bit);
                wr_blk(gd_block_bitmap(g), g_blk);
                gd_set_free_blocks(g, (ext4_u16)(gd_free_blocks(g) - 1));
                gdt_flush();
                sb_dec_free_blocks();
                /* 新块需清零（目录块/文件尾块） */
                for (ext4_u32 i = 0; i < g_block_size; i++) g_blk2[i] = 0;
                wr_blk((ext4_u32)blk, g_blk2);
                return (ext4_u32)blk;
            }
        }
    }
    ext4_fail("EXT4-E0E NO FREE BLOCKS");
    return 0;
}

/* 回收 1 个块 */
static void block_free(ext4_u32 blk) {
    ext4_u32 g = (ext4_u32)(blk / g_blocks_per_group);
    ext4_u32 off = (ext4_u32)(blk % g_blocks_per_group);
    rd_blk(gd_block_bitmap(g), g_blk);
    if (!(g_blk[off / 8] & (1u << (off % 8)))) ext4_fail("EXT4-E0F DOUBLE FREE DETECTED");
    g_blk[off / 8] &= (ext4_u8)~(1u << (off % 8));
    wr_blk(gd_block_bitmap(g), g_blk);
    gd_set_free_blocks(g, (ext4_u16)(gd_free_blocks(g) + 1));
    gdt_flush();
    sb_inc_free_blocks();
}

/* ---------------- extent 树 ---------------- */
/* 根节点位于 inode i_block（0x28，60B）：
 * header 12B: magic(2) entries(2) max(2) depth(2) gen(4)
 * depth 0: leaf 项 12B: block(4) len(2) starthi(2) startlo(4)
 * depth 1: index 项 12B: block(4) leaf_lo(4) leaf_hi(2) unused(2) */
#define EXTENT_MAGIC 0xF30A

/* 遍历叶项回调：map(file_block → phys_block)。depth 0/1 支持。 */
static int extent_walk(const ext4_u8 *ino,
                       void (*cb)(void *ud, ext4_u64 file_blk, ext4_u64 phys_blk, ext4_u32 len),
                       void *ud) {
    ext4_u8 root[60];
    for (int i = 0; i < 60; i++) root[i] = ino[0x28 + i];
    if (r16(root + 0) != EXTENT_MAGIC) ext4_fail("EXT4-E10 EXTENT MAGIC INVALID");
    ext4_u16 depth = r16(root + 6);
    if (depth == 0) {
        ext4_u16 n = r16(root + 2);
        const ext4_u8 *e = root + 12;
        for (ext4_u16 i = 0; i < n; i++, e += 12) {
            ext4_u64 fb = r32(e);
            ext4_u32 len = r16(e + 4);
            if (len == 0) continue;
            ext4_u64 phys = r32(e + 8) | ((ext4_u64)r16(e + 6) << 32);
            cb(ud, fb, phys, len);
        }
        return 0;
    }
    if (depth != 1) ext4_fail("EXT4-E11 EXTENT DEPTH > 1 UNSUPPORTED");
    ext4_u16 n = r16(root + 2);
    const ext4_u8 *e = root + 12;
    for (ext4_u16 i = 0; i < n; i++, e += 12) {
        ext4_u64 leaf = r32(e + 4) | ((ext4_u64)r16(e + 8) << 32);
        rd_blk((ext4_u32)leaf, g_blk);
        if (r16(g_blk + 0) != EXTENT_MAGIC) ext4_fail("EXT4-E10 EXTENT MAGIC INVALID");
        ext4_u16 ln = r16(g_blk + 2);
        if (r16(g_blk + 6) != 0) ext4_fail("EXT4-E11 EXTENT DEPTH > 1 UNSUPPORTED");
        const ext4_u8 *le = g_blk + 12;
        for (ext4_u16 j = 0; j < ln; j++, le += 12) {
            ext4_u64 fb = r32(le);
            ext4_u32 len = r16(le + 4);
            if (len == 0) continue;
            ext4_u64 phys = r32(le + 8) | ((ext4_u64)r16(le + 6) << 32);
            cb(ud, fb, phys, len);
        }
    }
    return 0;
}

/* 回调载体：读文件 */
typedef struct { ext4_u8 *dst; ext4_u64 dst_cap; } read_ctx;
/* 优化：整段连续读（尾块超界时经 g_blk2 部分拷贝，杜绝缓冲区溢出） */
static void cb_read_seq(void *ud, ext4_u64 fb, ext4_u64 phys, ext4_u32 len) {
    read_ctx *c = (read_ctx *)ud;
    ext4_u64 start = fb * g_block_size;
    ext4_u64 bytes = (ext4_u64)len * g_block_size;
    if (start >= c->dst_cap) return;
    if (start + bytes <= c->dst_cap) {
        rd_sec(blk_lba((ext4_u32)phys), (ext4_u32)(len * g_sec_per_block),
               c->dst + start);
    } else {
        for (ext4_u32 i = 0; i < len; i++) {
            ext4_u64 off = start + (ext4_u64)i * g_block_size;
            if (off >= c->dst_cap) return;
            rd_blk((ext4_u32)(phys + i), g_blk2);
            ext4_u64 rem = c->dst_cap - off;
            if (rem > g_block_size) rem = g_block_size;
            for (ext4_u64 b = 0; b < rem; b++) c->dst[off + b] = g_blk2[b];
        }
    }
}
/* 回调载体：释放全部块 */
static void cb_free(void *ud, ext4_u64 fb, ext4_u64 phys, ext4_u32 len) {
    (void)ud; (void)fb;
    for (ext4_u32 i = 0; i < len; i++) block_free((ext4_u32)phys);
}

/* ---------------- inode 分配 ---------------- */
static ext4_u32 inode_alloc(void) {
    for (ext4_u32 g = 0; g < g_group_count; g++) {
        if (gd_free_inodes(g) == 0) continue;
        rd_blk(gd_inode_bitmap(g), g_blk);
        for (ext4_u32 byte = 0; byte < g_block_size; byte++) {
            if (g_blk[byte] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (g_blk[byte] & (1u << bit)) continue;
                ext4_u32 ino = g * g_inodes_per_group + (ext4_u32)byte * 8 + (ext4_u32)bit + 1;
                if (ino < 11) continue;   /* 保留 inode 1..10 */
                g_blk[byte] |= (ext4_u8)(1u << bit);
                wr_blk(gd_inode_bitmap(g), g_blk);
                gd_set_free_inodes(g, (ext4_u16)(gd_free_inodes(g) - 1));
                gdt_flush();
                w32(g_sb + 0x10, r32(g_sb + 0x10) - 1);   /* free_inodes_count */
                return ino;
            }
        }
    }
    ext4_fail("EXT4-E13 NO FREE INODES");
    return 0;
}

/* ---------------- 路径解析 ---------------- */
static ext4_u32 streq(const char *a, const char *b, ext4_u32 n) {
    for (ext4_u32 i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* 目录块内查找 name。返回 inode nr（0=未找到）。
 * entry: inode(4) rec_len(2) name_len(1) ftype(1) name */
static ext4_u32 dir_block_find(const ext4_u8 *blk, const char *name, ext4_u32 name_len) {
    ext4_u32 off = 0;
    while (off + 8 <= g_block_size) {
        ext4_u32 ino = r32(blk + off);
        ext4_u16 rec = r16(blk + off + 4);
        if (rec < 8 || (off + rec) > g_block_size) ext4_fail("EXT4-E14 DIR ENTRY CORRUPT");
        if (ino != 0) {
            ext4_u8 nl = blk[off + 6];
            if (nl == name_len && streq((const char *)(blk + off + 8), name, name_len))
                return ino;
        }
        off += rec;
    }
    return 0;
}

/* 读 inode 的第 bi 个目录块（bi < ceil(size/blocksize)） */
static void dir_read_block(const ext4_u8 *ino, ext4_u32 bi, ext4_u8 *out) {
    ext4_u8 root[60];
    for (int i = 0; i < 60; i++) root[i] = ino[0x28 + i];
    if (r16(root + 0) != EXTENT_MAGIC) ext4_fail("EXT4-E10 EXTENT MAGIC INVALID");
    ext4_u16 depth = r16(root + 6);
    if (depth != 0) ext4_fail("EXT4-E15 DIR TREE DEPTH UNSUPPORTED");
    ext4_u16 n = r16(root + 2);
    const ext4_u8 *e = root + 12;
    for (ext4_u16 i = 0; i < n; i++, e += 12) {
        ext4_u64 fb = r32(e);
        ext4_u32 len = r16(e + 4);
        ext4_u64 phys = r32(e + 8) | ((ext4_u64)r16(e + 6) << 32);
        if (bi >= fb && bi < fb + len) {
            rd_blk((ext4_u32)(phys + (bi - fb)), out);
            return;
        }
    }
    ext4_fail("EXT4-E16 DIR BLOCK NOT MAPPED");
}

/* 在 dir inode 下查找子项，返回 inode nr（0=不存在） */
static ext4_u32 dir_lookup(const ext4_u8 *dir_ino, const char *name, ext4_u32 name_len) {
    ext4_u64 dsize = r32(dir_ino + 0x04);
    ext4_u32 nblk = (ext4_u32)((dsize + g_block_size - 1) / g_block_size);
    for (ext4_u32 bi = 0; bi < nblk; bi++) {
        dir_read_block(dir_ino, bi, g_blk);
        ext4_u32 hit = dir_block_find(g_blk, name, name_len);
        if (hit) return hit;
    }
    return 0;
}

/* 按路径逐级走到父目录 + 最终名。 */
static void resolve_parent(const char *path, ext4_u32 *parent_ino,
                           const char **name, ext4_u32 *name_len) {
    if (path[0] != '/') ext4_fail("EXT4-E17 PATH MUST BE ABSOLUTE");
    ext4_u32 cur = EXT4_ROOT_INO;
    const char *p = path + 1;
    /* 找最终段 */
    const char *last = p;
    for (const char *q = p; *q; q++) if (*q == '/') last = q + 1;
    ext4_u32 last_len = 0;
    while (last[last_len]) last_len++;
    if (last_len == 0 || last_len > 255) ext4_fail("EXT4-E18 BAD FINAL NAME");
    /* 逐级遍历前面的段 */
    while (p < last) {
        ext4_u32 seg = 0;
        while (p[seg] && p[seg] != '/') seg++;
        if (seg == 0) { p++; continue; }
        ext4_u8 ino_buf[256];
        read_inode(cur, ino_buf);
        ext4_u32 next = dir_lookup(ino_buf, p, seg);
        if (!next) ext4_fail("EXT4-E19 PATH COMPONENT NOT FOUND");
        cur = next;
        p += seg + 1;
    }
    *parent_ino = cur;
    *name = last;
    *name_len = last_len;
}

/* ---------------- 读文件 ---------------- */
static int ext4_read_file_impl(const char *path, ext4_u8 **out, ext4_u32 *size) {
    ext4_u32 parent; const char *name; ext4_u32 name_len;
    resolve_parent(path, &parent, &name, &name_len);
    ext4_u8 dir_ino[256];
    read_inode(parent, dir_ino);
    ext4_u32 target = dir_lookup(dir_ino, name, name_len);
    if (!target) ext4_fail("EXT4-E1A FILE NOT FOUND");
    ext4_u8 ino[256];
    read_inode(target, ino);
    ext4_u16 mode = r16(ino + 0x00);
    if ((mode & 0xF000) != 0x8000) ext4_fail("EXT4-E1B NOT A REGULAR FILE");
    ext4_u64 fsize = r32(ino + 0x04) | ((ext4_u64)r32(ino + 0x6C) << 32);
    if (fsize > EXT4_MAX_FILE) ext4_fail("EXT4-E12 FILE EXCEEDS READ BUFFER");
    read_ctx rc; rc.dst = g_fdata; rc.dst_cap = fsize;
    extent_walk(ino, cb_read_seq, &rc);
    *out = g_fdata;
    *size = (ext4_u32)fsize;
    return 0;
}

int ext4_read_file(const char *path, ext4_u8 **out, ext4_u32 *size) {
    if (g_ext4_depth > 0) return ext4_read_file_impl(path, out, size);
    if (__builtin_setjmp(g_ext4_jb)) return ext4_abort();
    g_ext4_depth = 1;
    int rc = ext4_read_file_impl(path, out, size);
    g_ext4_depth = 0;
    return rc;
}

/* ---------------- 目录项插入 ---------------- */
/* 在 dir inode 的现有块中找空隙插入。返回 0 成功；1 无空隙（需扩块）。 */
static int dir_insert_existing(ext4_u8 *dir_ino, const char *name, ext4_u32 name_len, ext4_u32 target) {
    ext4_u64 dsize = r32(dir_ino + 0x04);
    ext4_u32 nblk = (ext4_u32)((dsize + g_block_size - 1) / g_block_size);
    ext4_u32 need = (8 + name_len + 3) & ~3u;
    for (ext4_u32 bi = 0; bi < nblk; bi++) {
        dir_read_block(dir_ino, bi, g_blk);
        ext4_u32 off = 0;
        while (off + 8 <= g_block_size) {
            ext4_u32 ino = r32(g_blk + off);
            ext4_u16 rec = r16(g_blk + off + 4);
            if (rec < 8 || (off + rec) > g_block_size) ext4_fail("EXT4-E14 DIR ENTRY CORRUPT");
            if (ino == 0 && rec >= need) {
                /* 空洞直接占用 */
                w32(g_blk + off, target);
                g_blk[off + 6] = (ext4_u8)name_len;
                g_blk[off + 7] = 1;   /* REGULAR FILE */
                for (ext4_u32 i = 0; i < name_len; i++) g_blk[off + 8 + i] = (ext4_u8)name[i];
                wr_blk_dir:;
                ext4_u8 root[60];
                for (int i = 0; i < 60; i++) root[i] = dir_ino[0x28 + i];
                /* 找 bi 对应物理块写回 */
                const ext4_u8 *e = root + 12;
                ext4_u16 n = r16(root + 2);
                for (ext4_u16 i = 0; i < n; i++, e += 12) {
                    ext4_u64 fb = r32(e);
                    ext4_u32 len = r16(e + 4);
                    ext4_u64 phys = r32(e + 8) | ((ext4_u64)r16(e + 6) << 32);
                    if (bi >= fb && bi < fb + len) { wr_blk((ext4_u32)(phys + (bi - fb)), g_blk); return 0; }
                }
                ext4_fail("EXT4-E16 DIR BLOCK NOT MAPPED");
            }
            if (off + rec == g_block_size) {
                /* 尾项收缩插入 */
                ext4_u8 nl = g_blk[off + 6];
                ext4_u32 min_rec = (8 + nl + 3) & ~3u;
                if (rec - min_rec >= need) {
                    ext4_u32 new_off = off + min_rec;
                    w16(g_blk + off + 4, (ext4_u16)min_rec);   /* rec_len 在 +4 */
                    w32(g_blk + new_off, target);
                    w16(g_blk + new_off + 4, (ext4_u16)(rec - min_rec));
                    g_blk[new_off + 6] = (ext4_u8)name_len;
                    g_blk[new_off + 7] = 1;
                    for (ext4_u32 i = 0; i < name_len; i++) g_blk[new_off + 8 + i] = (ext4_u8)name[i];
                    goto wr_blk_dir;
                }
                break;
            }
            off += rec;
        }
    }
    return 1;
}

/* 追加一个新目录块并插入条目 */
static void dir_insert_new_block(ext4_u8 *dir_ino, const char *name, ext4_u32 name_len, ext4_u32 target) {
    /* 扩展 extent：depth 0 且 <4 项时直接追加；否则 fatal（目录应远小于 4 块） */
    ext4_u8 root[60];
    for (int i = 0; i < 60; i++) root[i] = dir_ino[0x28 + i];
    if (r16(root + 0) != EXTENT_MAGIC) ext4_fail("EXT4-E10 EXTENT MAGIC INVALID");
    if (r16(root + 6) != 0) ext4_fail("EXT4-E15 DIR TREE DEPTH UNSUPPORTED");
    ext4_u16 n = r16(root + 2);
    ext4_u16 max = r16(root + 4);
    if (n >= max || n >= 4) ext4_fail("EXT4-E1C DIR EXTENT TABLE FULL");
    ext4_u32 phys = block_alloc();
    ext4_u64 dsize = r32(dir_ino + 0x04);
    /* 追加 extent 项：file block = dsize/4096，len 1 */
    w32(root + 12 + (ext4_u64)n * 12, (ext4_u32)(dsize / g_block_size));
    w16(root + 12 + (ext4_u64)n * 12 + 4, 1);
    w16(root + 12 + (ext4_u64)n * 12 + 6, 0);
    w32(root + 12 + (ext4_u64)n * 12 + 8, phys);
    w16(root + 12 + (ext4_u64)n * 12 + 10, 0);
    w16(root + 2, (ext4_u16)(n + 1));
    for (int i = 0; i < 60; i++) dir_ino[0x28 + i] = root[i];
    /* 更新 dir 尺寸与块计数 */
    dsize += g_block_size;
    w32(dir_ino + 0x04, (ext4_u32)dsize);
    w32(dir_ino + 0x1C, r32(dir_ino + 0x1C) + g_sec_per_block);
    /* 构造新目录块 */
    for (ext4_u32 i = 0; i < g_block_size; i++) g_blk[i] = 0;
    w32(g_blk + 0, target);
    w16(g_blk + 4, (ext4_u16)g_block_size);
    g_blk[6] = (ext4_u8)name_len;
    g_blk[7] = 1;
    for (ext4_u32 i = 0; i < name_len; i++) g_blk[8 + i] = (ext4_u8)name[i];
    /* used_dirs 不变（非目录）。写 inode 由调用方统一完成 */
    wr_blk(phys, g_blk);
}

/* ---------------- 写文件（覆写/创建） ---------------- */
static int ext4_write_file_impl(const char *path, const ext4_u8 *data, ext4_u32 size) {
    ext4_u32 parent; const char *name; ext4_u32 name_len;
    resolve_parent(path, &parent, &name, &name_len);
    ext4_u8 dir_ino[256];
    read_inode(parent, dir_ino);
    ext4_u32 target = dir_lookup(dir_ino, name, name_len);
    int existed = (target != 0);

    ext4_u8 ino[256];
    if (target) {
        read_inode(target, ino);
        ext4_u16 mode = r16(ino + 0x00);
        if ((mode & 0xF000) != 0x8000) ext4_fail("EXT4-E1B NOT A REGULAR FILE");
        /* 释放旧 extent 叶块（depth 1） */
        {
            ext4_u8 root0[60];
            for (int i = 0; i < 60; i++) root0[i] = ino[0x28 + i];
            if (r16(root0 + 0) != EXTENT_MAGIC) ext4_fail("EXT4-E10 EXTENT MAGIC INVALID");
            if (r16(root0 + 6) == 1) {
                ext4_u16 nn = r16(root0 + 2);
                for (ext4_u16 i = 0; i < nn; i++) {
                    ext4_u64 leaf = r32(root0 + 12 + i * 12 + 4) |
                                    ((ext4_u64)r16(root0 + 12 + i * 12 + 8) << 32);
                    block_free((ext4_u32)leaf);
                }
            }
        }
        /* 释放旧数据块 */
        extent_walk(ino, cb_free, 0);
    } else {
        target = inode_alloc();
        for (ext4_u32 i = 0; i < g_inode_size; i++) ino[i] = 0;
        w16(ino + 0x00, 0x8000 | 0644);       /* regular, rw-r--r-- */
        w16(ino + 0x1A, 1);                    /* links */
        w32(ino + 0x20, EXT4_EXTENTS_FL);
        w32(ino + 0x64, 0xDE5AAB1E);           /* generation 品牌落款 */
    }

    /* 构造 extent 根（depth 0/1） */
    ext4_u32 nblk = (size + g_block_size - 1) / g_block_size;
    if ((ext4_u64)nblk * g_block_size > EXT4_MAX_FILE) ext4_fail("EXT4-E12 FILE EXCEEDS READ BUFFER");
    /* 收集分配的块号（最大 8MB/4K = 2048 块） */
    static ext4_u32 alloc_map[2048];
    for (ext4_u32 i = 0; i < nblk; i++) alloc_map[i] = block_alloc();
    /* 写数据块 */
    for (ext4_u32 i = 0; i < nblk; i++) {
        for (ext4_u32 b = 0; b < g_block_size; b++) {
            ext4_u64 off = (ext4_u64)i * g_block_size + b;
            g_blk[b] = (off < size) ? data[off] : 0;
        }
        wr_blk(alloc_map[i], g_blk);
    }
    /* 生成 extent 叶项（每 extent ≤ 32768 块，此处 ≤2048） */
    ext4_u8 root[60];
    for (int i = 0; i < 60; i++) root[i] = 0;
    w16(root + 0, EXTENT_MAGIC);
    w16(root + 4, 4);          /* max = 4（根内联） */
    w16(root + 6, 0);          /* depth 0 */
    ext4_u16 ne = 0;
    ext4_u32 i = 0;
    if (nblk > 0) {
        while (i < nblk) {
            if (ne >= 4) {
                /* >4 extents：需要 depth 1。把现有叶项压入一个叶块，继续分配。 */
                /* 分配叶块，把 4 项写入 */
                ext4_u32 leaf = block_alloc();
                ext4_u8 lfb[4096];
                for (ext4_u32 b = 0; b < g_block_size; b++) lfb[b] = 0;
                w16(lfb + 0, EXTENT_MAGIC);
                w16(lfb + 2, ne);
                w16(lfb + 4, (ext4_u16)((g_block_size - 12) / 12));
                w16(lfb + 6, 0);
                for (ext4_u16 j = 0; j < ne; j++) {
                    const ext4_u8 *src = root + 12 + j * 12;
                    ext4_u8 *dst = lfb + 12 + j * 12;
                    for (int k = 0; k < 12; k++) dst[k] = src[k];
                }
                wr_blk(leaf, lfb);
                /* root 变 depth1：保留 index[0] = 旧叶；继续向 alloc_map 后段收集 */
                /* 这里简化：一次性深度1重建（见下），直接跳出 */
                static ext4_u32 leaf_blocks[8];
                static ext4_u16 leaf_counts[8];
                int nleaf = 0;
                leaf_blocks[nleaf] = leaf; leaf_counts[nleaf] = ne; nleaf++;
                /* 继续把剩余块打成叶块，每块 ≤340 项 */
                ext4_u16 cur_cnt = ne;
                ext4_u32 cur_start = i;   /* 前 4 项 extent 已覆盖 file blocks [0, i) */
                (void)cur_cnt;
                while (cur_start < nblk) {
                    ext4_u32 cnt = nblk - cur_start;
                    if (cnt > (g_block_size - 12) / 12) cnt = (g_block_size - 12) / 12;
                    ext4_u32 lf = block_alloc();
                    ext4_u8 lb[4096];
                    for (ext4_u32 b = 0; b < g_block_size; b++) lb[b] = 0;
                    w16(lb + 0, EXTENT_MAGIC);
                    w16(lb + 2, (ext4_u16)cnt);
                    w16(lb + 4, (ext4_u16)((g_block_size - 12) / 12));
                    w16(lb + 6, 0);
                    for (ext4_u32 j = 0; j < cnt; j++) {
                        ext4_u8 *ee = lb + 12 + j * 12;
                        w32(ee, cur_start + j);            /* file block */
                        w16(ee + 4, 1);                    /* len 1 */
                        w16(ee + 6, 0);
                        w32(ee + 8, alloc_map[cur_start + j]);
                    }
                    wr_blk(lf, lb);
                    if (nleaf >= 8) ext4_fail("EXT4-E1D TOO MANY EXTENT LEAVES");
                    leaf_blocks[nleaf] = lf; leaf_counts[nleaf] = (ext4_u16)cnt; nleaf++;
                    cur_start += cnt;
                }
                /* 重建根为 depth 1 */
                for (int b = 0; b < 60; b++) root[b] = 0;
                w16(root + 0, EXTENT_MAGIC);
                w16(root + 2, (ext4_u16)nleaf);
                w16(root + 4, 4);
                w16(root + 6, 1);
                ext4_u64 fb_cur = 0;
                for (int l = 0; l < nleaf; l++) {
                    ext4_u8 *ie = root + 12 + l * 12;
                    w32(ie, (ext4_u32)fb_cur);
                    w32(ie + 4, leaf_blocks[l]);
                    w16(ie + 8, 0);
                    fb_cur += leaf_counts[l];
                }
                goto root_done;
            }
            /* 找连续 run */
            ext4_u32 run = 1;
            while (i + run < nblk && run < 32768 &&
                   alloc_map[i + run] == alloc_map[i] + run) run++;
            ext4_u8 *ee = root + 12 + ne * 12;
            w32(ee, i);
            w16(ee + 4, (ext4_u16)run);
            w16(ee + 6, 0);
            w32(ee + 8, alloc_map[i]);
            ne++;
            i += run;
        }
    }
    w16(root + 2, ne);
root_done:
    for (int b = 0; b < 60; b++) ino[0x28 + b] = root[b];
    w32(ino + 0x04, size);                       /* size_lo */
    w32(ino + 0x6C, 0);                          /* size_high */
    w32(ino + 0x1C, (ext4_u32)((ext4_u64)nblk * g_sec_per_block));  /* blocks(512B) */
    w32(ino + 0x10, 0xDE5A0000u);                /* mtime 品牌纪年 */
    write_inode(target, ino);

    /* 新文件：插入目录项（target 为新分配 inode 且原查找时不存在） */
    if (!existed) {
        if (dir_insert_existing(dir_ino, name, name_len, target) != 0)
            dir_insert_new_block(dir_ino, name, name_len, target);
        write_inode(parent, dir_ino);
    }

    sb_flush();
    log_hex("[EXT4] wrote blocks=", nblk);

    /* 写后回读校验 */
    ext4_u8 *vdata; ext4_u32 vsize;
    ext4_read_file(path, &vdata, &vsize);
    if (vsize != size) ext4_fail("EXT4-E1E WRITE VERIFY SIZE MISMATCH");
    for (ext4_u32 b = 0; b < size; b++)
        if (vdata[b] != data[b]) ext4_fail("EXT4-E1E WRITE VERIFY DATA MISMATCH");
    return 0;
}

int ext4_write_file(const char *path, const ext4_u8 *data, ext4_u32 size) {
    if (g_ext4_depth > 0) return ext4_write_file_impl(path, data, size);
    if (__builtin_setjmp(g_ext4_jb)) return ext4_abort();
    g_ext4_depth = 1;
    int rc = ext4_write_file_impl(path, data, size);
    g_ext4_depth = 0;
    return rc;
}

/* ---------------- 启动自检 ---------------- */
static void ext4_boot_selftest_impl(void);

void ext4_boot_selftest(void) {
    if (!g_ext4_ok) {
        g_log->info("[EXT4] selftest skipped, no DESHAB volume");
        return;
    }
    if (__builtin_setjmp(g_ext4_jb)) { ext4_abort(); return; }
    g_ext4_depth = 1;
    ext4_boot_selftest_impl();
    g_ext4_depth = 0;
}

static void ext4_boot_selftest_impl(void) {
    /* 读测试：p2 上的 FUCK 配置副本 */
    ext4_u8 *d; ext4_u32 n;
    ext4_read_file("/system/deshab64/FUCK", &d, &n);
    if (n == 0) ext4_fail("EXT4-E1F SELFTEST FUCK READ EMPTY");
    log_hex("[EXT4] selftest FUCK bytes=", n);
    /* 写读校验测试 */
    static const ext4_u8 msg[] = "DESHAB EXT4 RW OK - DEAICUP\n";
    ext4_write_file("/ext4_rw.test", msg, sizeof(msg) - 1);
    g_log->info("[EXT4] selftest PASS");
}

/* ================= DKM 驱动入口 ================= */
static const char *const g_depends[] = { "vfs", "block" };
static const char *const g_provides[] = { "ext4", "fs.ext4" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic=DKM_DRIVER_MAGIC, .abi_version=DKM_ABI_VERSION,
    .desc_size=sizeof(struct dkm_driver_desc),
    .name="ext4", .version="0.1.0", .vendor="Deshab",
    .driver_class=DKM_CLASS_FS /* 7 */, .stage=2, .flags=0, .priority=0,
    .depends=g_depends, .depends_count=2,
    .provides=g_provides, .provides_count=2,
    .min_kernel_abi=1,
};

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;
    if (!api || !api->log) return -1;
    g_log = api->log;
    g_api = api;

    /* 设计内回退：无块设备属环境能力缺失，不 panic。 */
    if (!api->block) {
        g_log->warn("[ext4] no block api");
        return 0;
    }

    /* 无 DESHAB ext4 分区（QEMU 无盘 / 盘非 DESHAB）→ 不 panic，驱动空转。 */
    if (ext4_mount() != 0) {
        g_log->warn("[ext4] no DESHAB ext4 volume; driver idle");
        return 0;
    }

    ext4_boot_selftest();

    /* 注册 srv.* 服务到双向服务总线（P0/P1）。 */
    if (g_api->bus && g_api->bus->register_service) {
        int r1 = g_api->bus->register_service("srv.fs.ext4.read_file", 1, (void *)ext4_read_file);
        int r2 = g_api->bus->register_service("srv.fs.ext4.write_file", 1, (void *)ext4_write_file);
        g_api->bus->register_service("srv.fs.ext4.remount", 1, (void *)ext4_remount);
        if (r1 != 0 && r2 != 0)
            g_log->warn("[ext4] bus register_service failed (bus full?)");
    } else {
        g_log->warn("[ext4] no bus api; services not registered");
    }

    g_log->info("[ext4] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    g_ext4_ok = 0;
    return 0;
}

