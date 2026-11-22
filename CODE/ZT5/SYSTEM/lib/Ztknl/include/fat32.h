#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include "vfs.h"

/* FAT32 引导扇区 */
struct fat32_bpb {
    uint8_t  jmp_boot[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;      /* FAT32 中为 0 */
    uint16_t total_sectors_16;  /* FAT32 中为 0 */
    uint8_t  media_type;
    uint16_t sectors_per_fat_16; /* FAT32 中为 0 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    /* FAT32 扩展 */
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_num;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
} __attribute__((packed));

/* FAT32 目录项 */
struct fat32_dirent {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t cluster_low;
    uint32_t file_size;
} __attribute__((packed));

/* FAT32 属性 */
#define FAT_ATTR_RDONLY     0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F   /* 长文件名标记 */

/* FAT 表项值 */
#define FAT32_FREE          0x00000000
#define FAT32_RESERVED      0x0FFFFFF0
#define FAT32_BAD_CLUSTER   0x0FFFFFF7
#define FAT32_EOC_MIN       0x0FFFFFF8   /* End of Chain 最小值 */
#define FAT32_EOC_MAX       0x0FFFFFFF   /* End of Chain 最大值 */
#define FAT32_EOC           0x0FFFFFFF   /* 标准 End of Chain 值 */

/* FAT32 目录项标记 */
#define FAT_DIRENT_DELETED  0xE5
#define FAT_DIRENT_END      0x00

/* FAT32 文件系统结构 */
struct fat32_fs {
    uint64_t partition_start;  /* 分区起始扇区 */
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t reserved_sectors;
    uint32_t sectors_per_fat;
    uint8_t  num_fats;         /* FAT 表数量 */
    uint32_t root_cluster;
    uint32_t fat_start;        /* FAT 表起始扇区 */
    uint32_t data_start;       /* 数据区起始扇区 */
    uint32_t total_clusters;
    uint32_t free_clusters;    /* 空闲簇数 */
    uint32_t next_free;        /* 下一个可用簇 */
    char volume_label[12];     /* 卷标 */
    uint32_t volume_id;        /* 卷序列号 */
};

/* FAT32 文件私有数据 */
struct fat32_file_data {
    uint32_t start_cluster;    /* 起始簇 */
    uint32_t current_cluster;  /* 当前簇 */
    uint32_t current_sector;   /* 当前扇区（簇内偏移） */
    uint64_t file_size;        /* 文件大小 */
    uint8_t  is_directory;     /* 是否为目录 */
    uint8_t  dirty;            /* 是否有修改 */
};

/* ========== 内联函数 ========== */

/* 检查是否为有效的 FAT32 簇号 */
static inline int fat32_valid_cluster(uint32_t cluster) {
    return cluster >= 2 && cluster < 0x0FFFFFF0;
}

/* 检查是否为文件结束标记 */
static inline int fat32_is_eoc(uint32_t cluster) {
    return cluster >= FAT32_EOC_MIN && cluster <= FAT32_EOC_MAX;
}

/* 检查是否为空闲簇 */
static inline int fat32_is_free(uint32_t cluster) {
    return cluster == FAT32_FREE;
}

/* 检查是否为坏簇 */
static inline int fat32_is_bad(uint32_t cluster) {
    return cluster == FAT32_BAD_CLUSTER;
}

/* 目录项是否被删除 */
static inline int fat32_dirent_deleted(const struct fat32_dirent *dirent) {
    return dirent->name[0] == FAT_DIRENT_DELETED;
}

/* 目录项是否为结束标记 */
static inline int fat32_dirent_end(const struct fat32_dirent *dirent) {
    return dirent->name[0] == FAT_DIRENT_END;
}

/* 目录项是否为长文件名 */
static inline int fat32_is_lfn(const struct fat32_dirent *dirent) {
    return (dirent->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN;
}

/* 目录项是否为卷标 */
static inline int fat32_is_volume_label(const struct fat32_dirent *dirent) {
    return (dirent->attr & FAT_ATTR_VOLUME_ID) != 0;
}

/* 获取目录项的起始簇号 */
static inline uint32_t fat32_get_cluster(const struct fat32_dirent *dirent) {
    return ((uint32_t)dirent->cluster_high << 16) | dirent->cluster_low;
}

/* 设置目录项的簇号 */
static inline void fat32_set_cluster(struct fat32_dirent *dirent, uint32_t cluster) {
    dirent->cluster_high = (cluster >> 16) & 0xFFFF;
    dirent->cluster_low = cluster & 0xFFFF;
}

/* 簇号转换为扇区号 */
static inline uint32_t fat32_cluster_to_sector(const struct fat32_fs *fs, uint32_t cluster) {
    return fs->data_start + (cluster - 2) * fs->sectors_per_cluster;
}

/* 扇区号转换为簇号 */
static inline uint32_t fat32_sector_to_cluster(const struct fat32_fs *fs, uint32_t sector) {
    return ((sector - fs->data_start) / fs->sectors_per_cluster) + 2;
}

/* 计算文件占用的簇数 */
static inline uint32_t fat32_calc_clusters(uint64_t file_size, uint32_t bytes_per_cluster) {
    if (file_size == 0) return 0;
    return (uint32_t)((file_size + bytes_per_cluster - 1) / bytes_per_cluster);
}

/* 初始化 FAT32 文件系统 */
int fat32_init(void);

/* 初始化 FAT32 文件系统 (带分区参数) */
int fat32_init_partition(uint64_t partition_start);

/* 获取 FAT32 文件系统操作 */
struct vfs_ops *fat32_get_ops(void);

/* 获取 FAT32 文件系统信息 */
struct fat32_fs *fat32_get_fs(void);

/* 刷新 FAT 表到磁盘 */
int fat32_flush_fat(void);

/* 获取空闲簇数 */
uint32_t fat32_get_free_clusters(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * FAT 缓存增强 - 多路组相联 FAT 表缓存
 * 缓存 FAT 表条目，避免重复磁盘读取
 * ═══════════════════════════════════════════════════════════════════════════ */

/* FAT 缓存配置 */
#define FAT_CACHE_SETS       64     /* 组数 */
#define FAT_CACHE_WAYS       4      /* 每组路数 (4路组相联) */
#define FAT_CACHE_SECTOR_U32 128    /* 每个缓存行包含的 uint32 条目数 (512字节) */

/* FAT 缓存行 */
struct fat_cache_line {
    uint32_t sector_num;                    /* 缓存的 FAT 扇区号 */
    uint32_t data[FAT_CACHE_SECTOR_U32];    /* 缓存数据 */
    uint8_t  valid;                         /* 是否有效 */
    uint8_t  dirty;                         /* 是否脏 */
    uint32_t lru_counter;                   /* LRU 计数器 */
};

/* FAT 缓存统计 */
struct fat_cache_stats {
    uint64_t reads;
    uint64_t hits;
    uint64_t misses;
    uint64_t writebacks;
    uint32_t evictions;
};

/* 初始化 FAT 缓存 */
void fat32_fat_cache_init(void);

/* 从 FAT 缓存读取扇区 */
int fat32_fat_cache_read(uint32_t fat_sector, uint32_t *data);

/* 向 FAT 缓存写入扇区 */
void fat32_fat_cache_write(uint32_t fat_sector, uint32_t *data);

/* 刷新 FAT 缓存 (写回所有脏行) */
int fat32_fat_cache_flush(void);

/* 获取 FAT 缓存统计 */
void fat32_fat_cache_get_stats(struct fat_cache_stats *stats);

/* ═══════════════════════════════════════════════════════════════════════════
 * 目录遍历哈希表 - 大目录加速查找
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 目录哈希表配置 */
#define DIR_HASH_SIZE       256     /* 哈希桶数量 */
#define DIR_HASH_MAX_CHAIN  16      /* 每个桶最大链长 */

/* 目录哈希条目 */
struct dir_hash_entry {
    char name[256];             /* 文件名 */
    uint32_t cluster;           /* 起始簇号 */
    uint32_t parent_cluster;    /* 父目录簇号 */
    uint32_t dir_offset;        /* 目录项偏移 */
    uint8_t  attr;              /* 文件属性 */
    uint32_t file_size;         /* 文件大小 */
    uint8_t  valid;
    struct dir_hash_entry *next;
};

/* 目录哈希表 */
struct dir_hash_table {
    struct dir_hash_entry buckets[DIR_HASH_SIZE];
    struct dir_hash_entry *overflow;     /* 溢出链表 */
    uint32_t count;                      /* 条目计数 */
    uint32_t parent_cluster;             /* 所属目录簇 */
    int valid;
};

/* 初始化目录哈希表 */
void fat32_dir_hash_init(struct dir_hash_table *table, uint32_t parent_cluster);

/* 在哈希表中查找 */
struct dir_hash_entry *fat32_dir_hash_lookup(struct dir_hash_table *table, const char *name);

/* 向哈希表添加条目 */
int fat32_dir_hash_add(struct dir_hash_table *table, const char *name,
                       uint32_t cluster, uint32_t dir_offset,
                       uint8_t attr, uint32_t file_size);

/* 构建目录哈希表 (从磁盘读取整个目录) */
int fat32_dir_hash_build(struct dir_hash_table *table, uint32_t dir_cluster);

/* ═══════════════════════════════════════════════════════════════════════════
 * LFN 长文件名完善 - 处理所有 LFN 边界情况
 * ═══════════════════════════════════════════════════════════════════════════ */

/* LFN 校验和验证 - 验证 LFN 条目与短文件名的匹配 (内部函数，fat32.c 中定义) */

/* LFN 跨簇处理 - 处理 LFN 条目跨越簇边界的情况 */
int fat32_lfn_read_cross_cluster(uint32_t start_cluster, int start_index,
                                  int num_lfn, char *name_out, int max_len);

/* LFN 名称规范化 - 处理尾随填充和终止符 */
void fat32_lfn_normalize(char *name, int len);

/* ═══════════════════════════════════════════════════════════════════════════
 * 写优化 - 延迟写入 / 写回缓存
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 延迟写入配置 */
#define FAT32_WRITEBACK_DELAY_MS   5000   /* 写回延迟 (毫秒) */
#define FAT32_DIRTY_CLUSTER_MAX    64     /* 最大脏簇数 */

/* 脏簇记录 */
struct fat32_dirty_cluster {
    uint32_t cluster;              /* 簇号 */
    uint8_t  *data;                /* 簇数据指针 */
    uint64_t dirty_time;           /* 变脏时间 */
    uint8_t  valid;
};

/* 写回缓存 */
struct fat32_writeback_cache {
    struct fat32_dirty_cluster entries[FAT32_DIRTY_CLUSTER_MAX];
    uint32_t count;                /* 脏簇计数 */
    uint64_t last_flush_time;      /* 上次刷新时间 */
    int enabled;                   /* 是否启用 */
};

/* 初始化写回缓存 */
void fat32_writeback_init(void);

/* 将簇加入写回缓存 (延迟写入) */
int fat32_writeback_add(uint32_t cluster, const uint8_t *data);

/* 刷新写回缓存 (将所有脏簇写入磁盘) */
int fat32_writeback_flush(void);

/* 定时刷新 (由内核定时器调用) */
void fat32_writeback_timer(void);

/* 查找写回缓存中的簇 */
int fat32_writeback_lookup(uint32_t cluster, uint8_t *data);

#endif
