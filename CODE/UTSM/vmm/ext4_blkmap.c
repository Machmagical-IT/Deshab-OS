/* ===================================================================
 *  ext4_blkmap.c — 把 ext4 分区里的一个大文件映射成块设备（只读元数据）
 *
 *  背景
 *  ----
 *  持久化卷（deshab-persist）原本是放在 FAT32 ESP 上的一个文件，由
 *  virtio-blk 把它的簇链映射成 /dev/vdc。FAT32 的簇链能直接枚举成扇区表，
 *  所以那块映射很轻。
 *
 *  把卷挪到 ext4 的 DATA 分区后，需要同样的能力：把 ext4 里一个文件的
 *  物理块列表映射成扇区表。不能退化成"把整个文件读进内存"——卷有数 GiB，
 *  而内核侧没有对应大小的常驻缓冲。
 *
 *  关键观察
 *  --------
 *  ext4 的 extent 本身就是「连续物理块区间」：
 *      leaf entry = { ee_block(file 起始块), ee_len(块数), ee_start(物理块) }
 *  这和 FAT32 的「连续簇 run」是同类结构。所以只读超级块 + inode +
 *  extent 树（几 KB 元数据），就能把文件映射成扇区表，正文完全按需读。
 *  ext_depth 最多再走一层索引块（depth 1），再多则明确拒绝。
 *
 *  只读约束
 *  --------
 *  本模块只读元数据，不修改 ext4。卷内的写入通过 virtio-blk 落到映射出的
 *  扇区上，即直接写文件正文区域——ext4 的元数据（块位图/inode 尺寸）因此
 *  与文件实际内容长度不一致。对"这个文件就是一整块裸卷"的用法是可接受
 *  的：卷内部有自己的文件系统，ext4 只当容器。这也是为什么卷大小在构建期
 *  固定，运行期不依赖 ext4 的 size 字段。
 * =================================================================== */

#include <utsm/types.h>
#include <utsm/log.h>
#include <utsm/block.h>
#include <utsm/panic.h>

#define EXMAP_MAX_RUNS      512u   /* 连续区间上限（超出即判定为碎片化不可映射） */
#define EXMAP_MAX_GROUP_DESC 65536u

#define EXT4_MAGIC          0xEF53u
#define EXT4_EXTENT_MAGIC   0xF30Au
#define EXT4_EXTENTS_FL     0x00080000u
#define EXT4_INCOMPAT_EXTENTS 0x0040u
#define EXT4_EXTENTS_FL_EXTENTS 0x00080000u

/* 已解析的卷元数据（模块内单实例：只服务一个持久卷） */
struct exmap_run {
    u64 file_blk;   /* 文件内起始块（以块为单位） */
    u64 phys_blk;   /* 分区内物理块号 */
    u32 nblk;       /* 连续块数 */
};

static struct {
    int valid;
    u32 dev;              /* 块设备索引 */
    u64 part_lba;         /* 分区起始扇区 */
    u32 blk_size;         /* 块大小（字节，通常 4096） */
    u32 sec_per_blk;      /* 每块扇区数 */
    u64 inode_size;
    u64 nr_runs;
    struct exmap_run runs[EXMAP_MAX_RUNS];
    u64 capacity_sectors; /* 映射出的块设备容量（扇区） */
} g_exmap;

static int exmap_read(u32 dev, u64 lba, u32 count, void *buf) {
    const dkm_block_api *b = block_get_api();
    if (!b || !b->read || !buf || count == 0) return -1;
    return b->read(dev, lba, count, buf);
}

static u16 ex_r16(const u8 *p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
static u32 ex_r32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
/* 读分区内一个 4K 块到临时缓冲（块大小可能不是 4096，这里按块读）。 */
static u8 g_ex_blk[8192] __attribute__((aligned(16)));

static int ex_read_block(u64 blk_no) {
    u64 lba = g_exmap.part_lba + blk_no * g_exmap.sec_per_blk;
    return exmap_read(g_exmap.dev, lba, g_exmap.sec_per_blk, g_ex_blk);
}

/* 追加一个连续区间（file_blk 必须严格递增且首尾相接才合并）。
 * uninit: 该区间是 ext4 的"未初始化 extent"——内容未写过，ext4 语义规定
 * 读出来为零。这类区间不登记进 run 表（run 表只记录真实存在的磁盘块），
 * 因此 lookup 找不到时由调用方按"空洞"处理为零。 */
static int ex_add_run(u64 file_blk, u64 phys_blk, u32 nblk, int uninit) {
    if (nblk == 0) return 0;
    if (uninit) return 0;   /* 未初始化段不占磁盘内容，交给空洞语义 */

    if (g_exmap.nr_runs > 0) {
        struct exmap_run *last = &g_exmap.runs[g_exmap.nr_runs - 1];
        if (last->file_blk + last->nblk == file_blk &&
            last->phys_blk + last->nblk == phys_blk) {
            last->nblk += nblk;
            return 0;
        }
    }
    if (g_exmap.nr_runs >= EXMAP_MAX_RUNS) return -1;
    g_exmap.runs[g_exmap.nr_runs].file_blk = file_blk;
    g_exmap.runs[g_exmap.nr_runs].phys_blk = phys_blk;
    g_exmap.runs[g_exmap.nr_runs].nblk = nblk;
    g_exmap.nr_runs++;
    return 0;
}

static int ex_walk_extents(const u8 *node, int level) {
    if (ex_r16(node) != EXT4_EXTENT_MAGIC) return -2;
    u16 entries = ex_r16(node + 2);
    u16 node_depth = ex_r16(node + 6);

    if (level > 4) return -3;   /* 防御：ext4 实际最多 depth 5 */

    const u8 *e = node + 12;
    for (u16 i = 0; i < entries; i++, e += 12) {
        if (node_depth == 0) {
            u64 fb = ex_r32(e);
            u32 raw = ex_r16(e + 4);
            int uninit = (raw > 32768u) ? 1 : 0;
            u32 len = uninit ? (raw - 32768u) : raw;
            if (len == 0) continue;
            u64 phys = (u64)ex_r32(e + 8) | ((u64)ex_r16(e + 6) << 32);
            if (ex_add_run(fb, phys, len, uninit) != 0) return -4;
        } else {
            u64 leaf = (u64)ex_r32(e + 4) | ((u64)ex_r16(e + 8) << 32);
            if (level >= 4) return -3;
            if (ex_read_block(leaf) != 0) return -5;
            int rc = ex_walk_extents(g_ex_blk, level + 1);
            if (rc != 0) return rc;
        }
    }
    return 0;
}

/* --- 组描述符表：支持多块组 ---
 * 目标文件不一定落在组 0：/persist 目录的 inode 号可能远大于
 * s_inodes_per_group（实测 12G 镜像里是 524289，落在第 64 组）。
 * 必须按 inode 号算出组号，再取该组的 inode 表。 */
static u32 g_ex_ipg;        /* inodes per group */
static u32 g_ex_desc_size;  /* 组描述符大小（32 或 64） */
static u64 g_ex_gdt_blk;    /* 组描述符表起始块 */

static u8 g_ex_inode[512] __attribute__((aligned(16)));  /* 当前 inode */

static int ex_group_itable(u32 group, u64 *out_blk) {
    u64 off_bytes = (u64)group * g_ex_desc_size;
    u64 blk_off = off_bytes / g_exmap.blk_size;
    u32 in_blk = (u32)(off_bytes % g_exmap.blk_size);
    u32 need = (g_ex_desc_size >= 64) ? 0x2C : 0x0C;
    if (in_blk + need > g_exmap.blk_size) return -1;
    if (ex_read_block(g_ex_gdt_blk + blk_off) != 0) return -2;
    u32 lo = ex_r32(g_ex_blk + in_blk + 0x08);
    u32 hi = (g_ex_desc_size >= 64) ? ex_r32(g_ex_blk + in_blk + 0x28) : 0;
    u64 it = (u64)lo | ((u64)hi << 32);
    if (it == 0) return -3;
    *out_blk = it;
    return 0;
}

/* 读 inode 表项（任意块组）。inode 号从 1 开始。 */
static int ex_read_inode(u32 ino) {
    if (ino == 0 || g_ex_ipg == 0) return -2;
    u32 group = (ino - 1) / g_ex_ipg;
    u32 index = (ino - 1) % g_ex_ipg;
    u64 itable = 0;
    if (ex_group_itable(group, &itable) != 0) return -3;

    u64 per_blk = g_exmap.blk_size / g_exmap.inode_size;
    if (per_blk == 0) return -4;
    u64 blk = itable + index / per_blk;
    u32 within = (u32)((index % per_blk) * g_exmap.inode_size);
    if (within + g_exmap.inode_size > g_exmap.blk_size) return -5;
    if (ex_read_block(blk) != 0) return -6;
    for (u64 i = 0; i < g_exmap.inode_size; i++) g_ex_inode[i] = g_ex_blk[within + i];
    return 0;
}

/* 在目录的「多个 extent run」里按名字线性查找。
 * 目录数据可能由多个不连续 run 组成，必须逐个走，不能只看第一个。 */
static int ex_find_in_dir_runs(const struct exmap_run *runs, u64 nruns,
                               const char *name, u32 *out_ino) {
    u32 nlen = 0;
    while (name[nlen]) nlen++;

    for (u64 ri = 0; ri < nruns; ri++) {
        const struct exmap_run *r = &runs[ri];
        for (u32 b = 0; b < r->nblk; b++) {
            if (ex_read_block(r->phys_blk + b) != 0) return -1;
            u32 off = 0;
            while (off + 8 <= g_exmap.blk_size) {
                const u8 *ent = g_ex_blk + off;
                u32 ino = ex_r32(ent);
                u16 rec_len = ex_r16(ent + 4);
                u8 name_len = ent[6];
                if (rec_len < 8 || (off + rec_len) > g_exmap.blk_size) break;
                if (ino != 0 && name_len == nlen) {
                    int same = 1;
                    for (u32 k = 0; k < nlen; k++) {
                        if (ent[8 + k] != (u8)name[k]) { same = 0; break; }
                    }
                    if (same) { *out_ino = ino; return 0; }
                }
                off += rec_len;
            }
        }
    }
    return -2;
}


/* 入口：把 dev 上第 part_lba 扇区开始的 ext4 分区里的 path 映射成卷。
 * 成功返回 0，*out_capacity 为容量（字节）。失败返回负值。 */
int ext4_blkmap_open(u32 dev, u64 part_lba, const char *path, u64 *out_capacity) {
    g_exmap.valid = 0;
    g_exmap.nr_runs = 0;
    g_exmap.dev = dev;
    g_exmap.part_lba = part_lba;

    /* --- 超级块：分区内偏移 1024（即第 2 个扇区） --- */
    if (exmap_read(dev, part_lba + 2, 1, g_ex_blk) != 0) return -1;
    if (ex_r16(g_ex_blk + 0x38) != EXT4_MAGIC) return -2;   /* s_magic */

    u32 log_blk = ex_r32(g_ex_blk + 0x18);                  /* s_log_block_size */
    g_exmap.blk_size = 1024u << log_blk;
    if (g_exmap.blk_size < 1024 || g_exmap.blk_size > sizeof(g_ex_blk)) return -3;
    g_exmap.sec_per_blk = g_exmap.blk_size / 512u;

    u32 incompat = ex_r32(g_ex_blk + 0x60);
    if (!(incompat & EXT4_INCOMPAT_EXTENTS)) return -4;

    u32 inode_size = ex_r16(g_ex_blk + 0x58);
    if (inode_size < 128) inode_size = 128;
    if (inode_size > sizeof(g_ex_inode)) return -20;
    g_exmap.inode_size = inode_size;

    /* --- 组描述符表：按 inode 号定位组，需要 inodes_per_group 与描述符大小 --- */
    u32 ipg = ex_r32(g_ex_blk + 0x28);                       /* s_inodes_per_group */
    if (ipg == 0) return -6;
    g_ex_ipg = ipg;

    /* s_desc_size 为 0 表示用默认 32B（ext4 规范：0 或 <32 视为 32）。 */
    u32 ds = ex_r16(g_ex_blk + 0xFE);
    if (ds < 32) ds = 32;
    g_ex_desc_size = ds;

    /* 块大小 1024 时组描述符表在块 2，否则在块 1。 */
    g_ex_gdt_blk = (g_exmap.blk_size == 1024) ? 2 : 1;

    /* --- 逐段解析路径 --- */
    u32 cur_ino = 2;   /* 根目录固定 inode 2 */
    const char *p = path;
    char comp[256];

    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;

        /* 当前目录 inode → extent 表 → 目录数据 run 列表 */
        if (ex_read_inode(cur_ino) != 0) return -7;
        u32 flags = ex_r32(g_ex_inode + 0x20);
        if (!(flags & EXT4_EXTENTS_FL)) return -8;
        g_exmap.nr_runs = 0;
        if (ex_walk_extents(g_ex_inode + 0x28, 0) != 0) return -9;
        if (g_exmap.nr_runs == 0) return -10;

        /* 取一个路径分量，在当前目录的所有 run 里找 */
        u32 n = 0;
        while (*p && *p != '/' && n + 1 < sizeof(comp)) comp[n++] = *p++;
        comp[n] = 0;

        u32 child = 0;
        if (ex_find_in_dir_runs(g_exmap.runs, g_exmap.nr_runs, comp, &child) != 0)
            return -11;

        /* 还有下一段 → 下钻；否则 child 是目标文件 */
        const char *q = p;
        while (*q == '/') q++;
        if (*q) {
            cur_ino = child;
            continue;
        }

        if (ex_read_inode(child) != 0) return -12;
        u32 fflags = ex_r32(g_ex_inode + 0x20);
        if (!(fflags & EXT4_EXTENTS_FL)) return -16;
        u64 fsize = ex_r32(g_ex_inode + 0x04) |
                    ((u64)ex_r32(g_ex_inode + 0x6C) << 32);
        g_exmap.nr_runs = 0;
        if (ex_walk_extents(g_ex_inode + 0x28, 0) != 0) return -17;
        if (g_exmap.nr_runs == 0) return -18;
        g_exmap.capacity_sectors = fsize / 512u;
        if (out_capacity) *out_capacity = fsize;
        g_exmap.valid = 1;
        log_info("[EXMAP] ext4 file mapped as block device");
        log_hex64("[EXMAP] bytes=", fsize);
        log_hex64("[EXMAP] runs=", g_exmap.nr_runs);
        return 0;
    }
    return -19;
}

/* 查文件块 → 分区扇区。
 * 返回 0 = 命中真实磁盘块；1 = 该块是空洞/未初始化段（内容按零处理）；
 * 负值 = 未打开或块号越界。 */
int ext4_blkmap_lookup(u64 file_blk, u64 *out_part_sector) {
    if (!g_exmap.valid) return -1;
    for (u64 i = 0; i < g_exmap.nr_runs; i++) {
        const struct exmap_run *r = &g_exmap.runs[i];
        if (file_blk >= r->file_blk && file_blk < r->file_blk + r->nblk) {
            u64 off = file_blk - r->file_blk;
            *out_part_sector = g_exmap.part_lba +
                               (r->phys_blk + off) * g_exmap.sec_per_blk;
            return 0;
        }
    }
    /* 不在任何已初始化 run 内：ext4 语义为零（空洞 / 未初始化 extent） */
    if (file_blk < g_exmap.capacity_sectors / (g_exmap.blk_size / 512u))
        return 1;
    return -2;
}

int ext4_blkmap_ready(void) { return g_exmap.valid; }
u64 ext4_blkmap_capacity_sectors(void) { return g_exmap.capacity_sectors; }
u64 ext4_blkmap_run_count(void) { return g_exmap.nr_runs; }
