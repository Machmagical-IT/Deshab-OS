#ifndef UTSM_EXT4_BLKMAP_H
#define UTSM_EXT4_BLKMAP_H

/* ext4_blkmap.h — 把 ext4 分区里的一个大文件映射成块设备。
 *
 * 只读元数据（超级块 / inode / extent 树），正文按需读写。
 * 用于把持久化卷从 FAT32 ESP 搬到 ext4 DATA 分区（避开 FAT32 4GiB 单文件限制）。
 *
 * 用法：
 *   if (ext4_blkmap_open(dev, part_lba, "/persist/deshab-persist.img", &cap) == 0)
 *       ... ext4_blkmap_lookup(file_blk, &part_sector) ...
 */

#include <utsm/types.h>

/* 打开并解析。path 是分区内绝对路径。成功返回 0，*out_capacity 得字节数。 */
int ext4_blkmap_open(u32 dev, u64 part_lba, const char *path, u64 *out_capacity);

/* 文件块号 → 该块所在的分区绝对扇区。返回 0 成功。 */
int ext4_blkmap_lookup(u64 file_blk, u64 *out_part_sector);

int ext4_blkmap_ready(void);
u64 ext4_blkmap_capacity_sectors(void);
u64 ext4_blkmap_run_count(void);

#endif /* UTSM_EXT4_BLKMAP_H */
