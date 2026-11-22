/* ===========================================================================
 *  Nutro+OS - read-only FAT32 driver on top of the ATA PIO driver
 * ===========================================================================
 */
#include "kernel.h"

#define FAT_EOC 0x0FFFFFF8u

static u8 secbuf[512];

static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8 *p) { return (u16)((u32)p[0] | ((u32)p[1] << 8)); }

int fat32_mount(fat32_t *fs, u32 part_lba)
{
    memset(fs, 0, sizeof(*fs));
    if (!ata_present()) return -1;

    if (ata_read_sectors(part_lba, 1, secbuf) != 0) return -1;

    u32 spc = secbuf[13];
    u32 reserved = rd16(secbuf + 14);
    u32 nfats = secbuf[16];
    u32 fatsz = rd32(secbuf + 36);
    u32 root = rd32(secbuf + 44);
    u32 totsec = rd32(secbuf + 32);

    if (spc == 0 || reserved == 0 || nfats == 0 || fatsz == 0) return -1;

    fs->valid = 1;
    fs->part_lba = part_lba;
    fs->spc = spc;
    fs->reserved = reserved;
    fs->nfats = nfats;
    fs->fat_size = fatsz;
    fs->root_cluster = root ? root : 2;
    fs->fat_lba = part_lba + reserved;
    fs->data_lba = fs->fat_lba + nfats * fatsz;
    fs->cluster_bytes = spc * 512;
    fs->total_clusters = (totsec - reserved - nfats * fatsz) / spc;
    return 0;
}

int fat32_next_cluster(fat32_t *fs, u32 cluster)
{
    u32 byteoff = cluster * 4;
    u32 lba = fs->fat_lba + byteoff / 512;
    u32 off = byteoff % 512;
    if (ata_read_sectors(lba, 1, secbuf) != 0) return -1;
    u32 v = rd32(secbuf + off) & 0x0FFFFFFFu;
    return (int)v;
}

static u32 cluster_lba(fat32_t *fs, u32 cluster)
{
    return fs->data_lba + (cluster - 2) * fs->spc;
}

int fat32_list(fat32_t *fs, u32 dir_cluster, fat_entry_t *out, int max)
{
    if (!fs->valid) return -1;
    u8 *cluster_buf = (u8 *)kmalloc(fs->cluster_bytes);
    if (!cluster_buf) return -1;

    int n = 0;
    u32 cluster = dir_cluster;
    int guard = 0;
    while (cluster >= 2 && cluster < FAT_EOC && guard++ < 4096) {
        if (ata_read_sectors(cluster_lba(fs, cluster), fs->spc, cluster_buf) != 0) break;
        for (u32 i = 0; i < fs->cluster_bytes / 32; i++) {
            u8 *e = cluster_buf + i * 32;
            if (e[0] == 0x00) { cluster = FAT_EOC; break; }   /* end of directory */
            if (e[0] == 0xE5) continue;
            if (e[11] == 0x0F) continue;                       /* long name entry */
            if (e[11] & 0x08) continue;                        /* volume label */
            if (e[0] == '.') continue;                         /* "." and ".." */
            if (n >= max) { cluster = FAT_EOC; break; }

            fat_entry_t *fe = &out[n];
            memset(fe, 0, sizeof(*fe));
            fe->attr = e[11];
            fe->is_dir = (e[11] & 0x10) ? 1 : 0;
            fe->size = fe->is_dir ? 0 : rd32(e + 28);
            fe->first_cluster = ((u32)rd16(e + 20) << 16) | rd16(e + 26);

            /* 8.3 name -> readable */
            char *dst = fe->name;
            int len = 0;
            for (int k = 0; k < 8 && e[k] != ' '; k++) dst[len++] = (char)e[k];
            int extlen = 0;
            for (int k = 0; k < 3 && e[8 + k] != ' '; k++) extlen++;
            if (extlen) {
                dst[len++] = '.';
                for (int k = 0; k < extlen; k++) dst[len++] = (char)e[8 + k];
            }
            dst[len] = 0;
            n++;
        }
        int next = fat32_next_cluster(fs, cluster);
        if (next < 0) break;
        cluster = (u32)next;
    }
    /* NOTE: cluster_buf is never freed - the bump allocator has no free() */
    return n;
}

u32 fat32_find_dir(fat32_t *fs, u32 dir_cluster, const char *name)
{
    fat_entry_t entries[64];
    int n = fat32_list(fs, dir_cluster, entries, 64);
    if (n < 0) return 0;
    for (int i = 0; i < n; i++) {
        if (!entries[i].is_dir) continue;
        if (strcmp(entries[i].name, name) == 0) return entries[i].first_cluster;
    }
    return 0;
}

int fat32_read_file(fat32_t *fs, u32 cluster, u32 size, void *buf, u32 max)
{
    if (!fs->valid) return -1;
    if (size > max) size = max;

    u8 *dst = (u8 *)buf;
    u32 done = 0;
    int guard = 0;
    while (cluster >= 2 && cluster < FAT_EOC && done < size && guard++ < 8192) {
        u32 lba = cluster_lba(fs, cluster);
        u32 chunk = fs->cluster_bytes;
        if (done + chunk > size) chunk = size - done;

        /* read the cluster through a bounce buffer when it is not 512 aligned */
        u8 *tmp = (u8 *)kmalloc(fs->cluster_bytes);
        if (!tmp) return -1;
        if (ata_read_sectors(lba, fs->spc, tmp) != 0) return -1;
        memcpy(dst + done, tmp, chunk);

        done += chunk;
        if (done >= size) break;
        int next = fat32_next_cluster(fs, cluster);
        if (next < 0) break;
        cluster = (u32)next;
    }
    return (int)done;
}