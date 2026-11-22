/* ext4.h — DKM ext4 read-write driver (vendored from DSK CODE/dsk/ext4.h)
 *
 * 万物互联 P2：DSK 主内核退役，其 ext4 能力归位为普通 DKM 驱动，
 * 经 dkm_bus 以 srv.fs.ext4.read_file / srv.fs.ext4.write_file 对外服务。
 * 目标：GPT p2（卷标 "DESHAB"）ext4 数据分区的读写。
 *
 * 严格错误策略：任何错误（挂载失败/feature 不支持/IO 失败/校验不符）
 * 一律经 g_log->panic("EXT4-E0x ...") 整屏 panic 停机，零降级。
 * panic fallback 模式（panic 返回）：当前公开调用中止并返回 -1，ext4 置为
 * 不可用，后续只走 FAT32 ESP。
 *
 * 支持范围（mkfs.ext4 由 pack_system_image.sh 以 -O
 * ^has_journal,^metadata_csum,^64bit,^resize_inode -b 4096 -I 256 创建）：
 *   - 4K blocksize、inode 256B、无 journal、无 metadata_csum、无 64bit
 *   - extent 树（depth 0/1）读写
 *   - 块位图/inode 位图分配与回收
 *   - 线性目录项查找/插入/扩展
 *
 * 依赖（include）：仅 dkm_shared.h + stdint + 本目录 ext4.h。无 UTSM 头，
 * 无 DSK 私有实现（GPT 探测内联于 ext4.c）。
 */
#ifndef DKM_EXT4_H
#define DKM_EXT4_H

typedef unsigned char  ext4_u8;
typedef unsigned short ext4_u16;
typedef unsigned int   ext4_u32;
typedef unsigned long long ext4_u64;

/* 挂载 GPT p2 "DESHAB" ext4 分区（GPT 定位 device 0 + superblock/feature 校验）。
 * 成功返回 0；找不到 ext4 分区/卷非 DESHAB 返回 -1（设计内回退，不 panic）。
 * feature 不支持 / superblock 校验失败 / IO 失败 = panic（EXT4-E0x）。 */
int ext4_mount(void);

/* 是否已成功挂载。 */
int ext4_is_mounted(void);

/* 按路径读文件（"/system/deshab64/FUCK"），返回内部静态缓冲（驱动 BSS）。
 * 任何失败 panic。成功返回 0 并填充 out/size。 */
int ext4_read_file(const char *path, ext4_u8 **out, ext4_u32 *size);

/* 按路径写文件（存在则覆写，不存在则创建）。
 * 写后自动回读逐字节校验，不符 panic。成功返回 0。 */
int ext4_write_file(const char *path, const ext4_u8 *data, ext4_u32 size);

/* 启动自检：读 /system/deshab64/FUCK + 写读校验 /ext4_rw.test。 */
void ext4_boot_selftest(void);

#endif /* DKM_EXT4_H */
