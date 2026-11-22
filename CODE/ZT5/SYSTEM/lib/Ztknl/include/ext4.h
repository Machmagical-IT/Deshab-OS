#ifndef EXT4_H
#define EXT4_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"

/* EXT4 魔数 */
#define EXT4_SUPER_MAGIC    0xEF53

/* EXT4 超级块偏移 */
#define EXT4_SB_OFFSET      1024

/* Inode 类型 */
#define EXT4_S_IFIFO        0x1000
#define EXT4_S_IFCHR        0x2000
#define EXT4_S_IFDIR        0x4000
#define EXT4_S_IFBLK        0x6000
#define EXT4_S_IFREG        0x8000
#define EXT4_S_IFLNK        0xA000
#define EXT4_S_IFSOCK       0xC000
#define EXT4_S_IFMT         0xF000

/* 超级块标志 */
#define EXT4_SIGNED_HASH    0x0001
#define EXT4_UNSIGNED_HASH  0x0002
#define EXT4_DEBUG          0x0004
#define EXT4_IS_PREALLOC    0x0008

/* 兼容性标志 */
#define EXT4_FEATURE_COMPAT_DIR_PREALLOC     0x0001
#define EXT4_FEATURE_COMPAT_IMAGIC_INODES    0x0002
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL      0x0004
#define EXT4_FEATURE_COMPAT_EXT_ATTR         0x0008
#define EXT4_FEATURE_COMPAT_RESIZE_INODE     0x0010
#define EXT4_FEATURE_COMPAT_DIR_INDEX        0x0020

/* 只读兼容性标志 */
#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER  0x0001
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE    0x0002
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR     0x0004
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE     0x0008
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM      0x0010
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK     0x0020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE   0x0040

/* 不兼容性标志 */
#define EXT4_FEATURE_INCOMPAT_COMPRESSION    0x0001
#define EXT4_FEATURE_INCOMPAT_FILETYPE       0x0002
#define EXT4_FEATURE_INCOMPAT_RECOVER        0x0004
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV    0x0008
#define EXT4_FEATURE_INCOMPAT_META_BG        0x0010
#define EXT4_FEATURE_INCOMPAT_EXTENTS        0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT          0x0080
#define EXT4_FEATURE_INCOMPAT_MMP            0x0100
#define EXT4_FEATURE_INCOMPAT_FLEX_BG        0x0200
#define EXT4_FEATURE_INCOMPAT_EA_INODE       0x0400

/* EXT4 超级块 */
struct ext4_super_block {
    uint32_t s_inodes_count;        /* Inode 总数 */
    uint32_t s_blocks_count_lo;     /* 块总数 (低32位) */
    uint32_t s_r_blocks_count_lo;   /* 保留块数 (低32位) */
    uint32_t s_free_blocks_count_lo;/* 空闲块数 (低32位) */
    uint32_t s_free_inodes_count;   /* 空闲 Inode 数 */
    uint32_t s_first_data_block;    /* 第一个数据块 */
    uint32_t s_log_block_size;      /* 块大小 = 1024 << s_log_block_size */
    uint32_t s_log_cluster_size;    /* 簇大小 */
    uint32_t s_blocks_per_group;    /* 每组块数 */
    uint32_t s_clusters_per_group;  /* 每组簇数 */
    uint32_t s_inodes_per_group;    /* 每组 Inode 数 */
    uint32_t s_mtime;               /* 最后挂载时间 */
    uint32_t s_wtime;               /* 最后写入时间 */
    uint16_t s_mnt_count;           /* 挂载计数 */
    uint16_t s_max_mnt_count;       /* 最大挂载计数 */
    uint16_t s_magic;               /* 魔数 */
    uint16_t s_state;               /* 文件系统状态 */
    uint16_t s_errors;              /* 错误处理方式 */
    uint16_t s_minor_rev_level;     /* 次版本号 */
    uint32_t s_lastcheck;           /* 最后检查时间 */
    uint32_t s_checkinterval;       /* 检查间隔 */
    uint32_t s_creator_os;          /* 创建者 OS */
    uint32_t s_rev_level;           /* 版本号 */
    uint16_t s_def_resuid;          /* 默认保留 UID */
    uint16_t s_def_resgid;          /* 默认保留 GID */
    
    /* EXT4 动态版本字段 */
    uint32_t s_first_ino;           /* 第一个非保留 Inode */
    uint16_t s_inode_size;          /* Inode 大小 */
    uint16_t s_block_group_nr;      /* 块组号 */
    uint32_t s_feature_compat;      /* 兼容特性 */
    uint32_t s_feature_incompat;    /* 不兼容特性 */
    uint32_t s_feature_ro_compat;   /* 只读兼容特性 */
    uint8_t  s_uuid[16];            /* 卷 UUID */
    char     s_volume_name[16];     /* 卷名 */
    char     s_last_mounted[64];    /* 最后挂载路径 */
    uint32_t s_algorithm_usage_bitmap;
    
    /* 日志相关 */
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    
    /* 64位支持 */
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_checksum_type;
    uint8_t  s_encryption_level;
    uint8_t  s_reserved_pad;
    uint64_t s_kbytes_written;
    uint32_t s_snapshot_inum;
    uint32_t s_snapshot_id;
    uint64_t s_snapshot_r_blocks_count;
    uint32_t s_snapshot_list;
    uint32_t s_error_count;
    uint32_t s_first_error_time;
    uint32_t s_first_error_ino;
    uint64_t s_first_error_block;
    uint8_t  s_first_error_func[32];
    uint32_t s_first_error_line;
    uint32_t s_last_error_time;
    uint32_t s_last_error_ino;
    uint64_t s_last_error_block;
    uint8_t  s_last_error_func[32];
    uint32_t s_last_error_line;
    uint8_t  s_mount_opts[64];
    uint32_t s_usr_quota_inum;
    uint32_t s_grp_quota_inum;
    uint32_t s_overhead_blocks;
    uint32_t s_backup_bgs[2];
    uint8_t  s_encrypt_algos[4];
    uint8_t  s_encrypt_pw_salt[16];
    uint32_t s_lpf_ino;
    uint32_t s_prj_quota_inum;
    uint32_t s_checksum_seed;
    uint8_t  s_wtime_hi;
    uint8_t  s_mtime_hi;
    uint8_t  s_mkfs_time_hi;
    uint8_t  s_lastcheck_hi;
    uint8_t  s_first_error_time_hi;
    uint8_t  s_last_error_time_hi;
    uint8_t  s_first_error_errcode;
    uint8_t  s_last_error_errcode;
    uint8_t  s_pad[120];
    uint32_t s_reserved[96];
    uint32_t s_checksum;
} __attribute__((packed));

/* 块组描述符 */
struct ext4_group_desc {
    uint32_t bg_block_bitmap_lo;    /* 块位图地址 (低32位) */
    uint32_t bg_inode_bitmap_lo;    /* Inode 位图地址 (低32位) */
    uint32_t bg_inode_table_lo;     /* Inode 表地址 (低32位) */
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    
    /* 64位支持 */
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} __attribute__((packed));

/* EXT4 Inode */
struct ext4_inode {
    uint16_t i_mode;                /* 文件模式 */
    uint16_t i_uid;                 /* 所有者 UID (低16位) */
    uint32_t i_size_lo;             /* 文件大小 (低32位) */
    uint32_t i_atime;               /* 访问时间 */
    uint32_t i_ctime;               /* 创建时间 */
    uint32_t i_mtime;               /* 修改时间 */
    uint32_t i_dtime;               /* 删除时间 */
    uint16_t i_gid;                 /* 所有者 GID (低16位) */
    uint16_t i_links_count;         /* 链接数 */
    uint32_t i_blocks_lo;           /* 块数 (低32位) */
    uint32_t i_flags;               /* Inode 标志 */
    uint32_t i_osd1;                /* OS 依赖 1 */
    union {
        struct {
            uint32_t i_block[15];   /* 块指针 / Extent 头 */
        } legacy;
        struct {
            uint16_t extent_magic;
            uint16_t extent_entries;
            uint32_t extent_max;
            uint32_t extent_generation;
            uint8_t  extent_data[60];
        } extent;
    } i_data;
    uint32_t i_generation;          /* 文件版本 */
    uint32_t i_file_acl_lo;         /* ACL (低32位) */
    uint32_t i_size_hi;             /* 文件大小 (高32位) */
    uint32_t i_obso_faddr;          /* 已废弃 */
    union {
        struct {
            uint16_t l_i_reserved1;
            uint16_t i_uid_high;
            uint16_t i_gid_high;
            uint32_t l_i_reserved2;
        } linux2;
        struct {
            uint16_t h_i_reserved1;
            uint16_t h_i_mode_high;
            uint16_t h_i_uid_high;
            uint16_t h_i_gid_high;
            uint32_t h_i_author;
        } hurd2;
    } i_osd2;
    uint16_t i_extra_isize;         /* 额外 Inode 大小 */
    uint16_t i_checksum_hi;         /* 校验和高16位 */
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
} __attribute__((packed));

/* 目录项 */
struct ext4_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[255];
} __attribute__((packed));

/* 文件类型 */
#define EXT4_FT_UNKNOWN     0
#define EXT4_FT_REG_FILE    1
#define EXT4_FT_DIR         2
#define EXT4_FT_CHRDEV      3
#define EXT4_FT_BLKDEV      4
#define EXT4_FT_FIFO        5
#define EXT4_FT_SOCK        6
#define EXT4_FT_SYMLINK     7

/* EXT4 文件系统结构 */
struct ext4_fs {
    struct ext4_super_block sb;
    uint32_t block_size;
    uint32_t cluster_size;
    uint32_t groups_count;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inode_size;
    
    /* 块组描述符 */
    struct ext4_group_desc *group_descs;
    
    /* 设备 */
    void *device;
    int (*read_block)(void *dev, uint64_t block, void *buf);
    int (*write_block)(void *dev, uint64_t block, const void *buf);
};

/* 初始化 EXT4 */
int ext4_init(void);

/* 挂载 EXT4 文件系统 */
int ext4_mount(struct ext4_fs *fs, void *device,
               int (*read_block)(void *, uint64_t, void *),
               int (*write_block)(void *, uint64_t, const void *));

/* 卸载 EXT4 */
int ext4_unmount(struct ext4_fs *fs);

/* 读取 Inode */
int ext4_read_inode(struct ext4_fs *fs, uint32_t ino, struct ext4_inode *inode);

/* 写入 Inode */
int ext4_write_inode(struct ext4_fs *fs, uint32_t ino, struct ext4_inode *inode);

/* 读取文件数据 */
ssize_t ext4_read_file(struct ext4_fs *fs, struct ext4_inode *inode,
                       void *buf, size_t offset, size_t size);

/* 写入文件数据 */
ssize_t ext4_write_file(struct ext4_fs *fs, struct ext4_inode *inode,
                        const void *buf, size_t offset, size_t size);

/* 目录操作 */
int ext4_lookup(struct ext4_fs *fs, uint32_t dir_ino, const char *name,
                uint32_t *result_ino);
int ext4_create(struct ext4_fs *fs, uint32_t dir_ino, const char *name,
                uint16_t mode, uint32_t *result_ino);
int ext4_mkdir(struct ext4_fs *fs, uint32_t dir_ino, const char *name);
int ext4_unlink(struct ext4_fs *fs, uint32_t dir_ino, const char *name);
int ext4_rmdir(struct ext4_fs *fs, uint32_t dir_ino, const char *name);

/* VFS 接口 */
struct vfs_filesystem *ext4_create_vfs(void);

/* 获取 EXT4 VFS 操作接口 */
struct vfs_ops *ext4_get_ops(void);

#endif
