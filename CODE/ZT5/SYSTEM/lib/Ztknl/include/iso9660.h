/*
 * ═══════════════════════════════════════════════════════════════════════════
 * Zratro OS - ISO9660 文件系统
 *
 * 只读文件系统，用于 CD-ROM 支持
 * - Primary Volume Descriptor 解析
 * - Rock Ridge 扩展 (POSIX 文件属性)
 * - Joliet 扩展 (UCS-2 Unicode 文件名)
 * - 目录记录解析
 * - 文件读取支持
 * - struct vfs_ops 接口
 * ═══════════════════════════════════════════════════════════════════════════
 */

#ifndef ISO9660_H
#define ISO9660_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * ISO9660 标准定义
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 卷描述符类型 */
#define ISO9660_VD_PRIMARY      1       /* 主卷描述符 */
#define ISO9660_VD_SUPPLEMENT   2       /* 补充卷描述符 (Joliet) */
#define ISO9660_VD_PARTITION    3       /* 分区卷描述符 */
#define ISO9660_VD_TERMINATOR   255     /* 终止描述符 */

/* 卷描述符魔数 */
#define ISO9660_VD_STD_ID1      'C'
#define ISO9660_VD_STD_ID2      'D'
#define ISO9660_VD_STD_ID3      '0'
#define ISO9660_VD_STD_ID4      '0'
#define ISO9660_VD_STD_ID5      '1'

/* 标准标识符 "CD001" */
#define ISO9660_STD_ID_LEN      5

/* 卷描述符大小 (2048 字节，一个逻辑块) */
#define ISO9660_VD_SIZE         2048

/* 逻辑块大小 */
#define ISO9660_LB_SIZE         2048

/* 目录记录标志 */
#define ISO9660_FLAG_HIDDEN     0x01    /* 隐藏文件 */
#define ISO9660_FLAG_DIRECTORY  0x02    /* 目录 */
#define ISO9660_FLAG_ASSOCIATED 0x04    /* 关联文件 */
#define ISO9660_FLAG_RECORD     0x08    /* 记录格式 */
#define ISO9660_FLAG_PROTECTION 0x10    /* 保护 */
#define ISO9660_FLAG_MULTI_EXT  0x80    /* 多段扩展 */

/* 文件名特殊标记 */
#define ISO9660_FILE_SEP        ';'     /* 版本号分隔符 */
#define ISO9660_FILE_VER        '1'     /* 默认版本号 */
#define ISO9660_CURRENT_DIR     '\x00'  /* 当前目录 . */
#define ISO9660_PARENT_DIR      '\x01'  /* 父目录 .. */

/* ═══════════════════════════════════════════════════════════════════════════
 * Rock Ridge 扩展定义
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Rock Ridge 签名 */
#define RR_SIG_PX   0x5058    /* PX - POSIX 属性 */
#define RR_SIG_PN   0x504E    /* PN - POSIX 设备号 */
#define RR_SIG_SL   0x534C    /* SL - 符号链接 */
#define RR_SIG_NM   0x4E4D    /* NM - 备用名称 */
#define RR_SIG_CL   0x434C    /* CL - 子目录位置 */
#define RR_SIG_PL   0x504C    /* PL - 父目录位置 */
#define RR_SIG_RE   0x5245    /* RE - 重定位目录 */
#define RR_SIG_TF   0x5446    /* TF - 时间戳 */
#define RR_SIG_SF   0x5346    /* SF - 稀疏文件 */
#define RR_SIG_CE   0x4345    /* CE - 续接区域 */
#define RR_SIG_ER   0x4552    /* ER - 扩展引用 */

/* PX (POSIX 属性) 标志 */
#define RR_PX_MODE      (1 << 0)   /* 包含 mode */
#define RR_PX_NLINK     (1 << 1)   /* 包含 nlink */
#define RR_PX_UID       (1 << 2)   /* 包含 uid */
#define RR_PX_GID       (1 << 3)   /* 包含 gid */
#define RR_PX_INO       (1 << 4)   /* 包含 inode */

/* NM (备用名称) 标志 */
#define RR_NM_CONTINUE  0x01    /* 名称续接 */
#define RR_NM_CURRENT   0x02    /* 当前目录 */
#define RR_NM_PARENT    0x04    /* 父目录 */

/* TF (时间戳) 标志 */
#define RR_TF_CREATE    (1 << 0)
#define RR_TF_MODIFY    (1 << 1)
#define RR_TF_ACCESS    (1 << 2)
#define RR_TF_ATTRIBUTES (1 << 3)
#define RR_TF_LONG_FORM (1 << 7)    /* 使用 17 字节长格式 */

/* ═══════════════════════════════════════════════════════════════════════════
 * 数据结构
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 卷描述符 (通用头部) */
struct iso9660_vd_header {
    uint8_t  type;                           /* 卷描述符类型 */
    char     std_identifier[ISO9660_STD_ID_LEN]; /* 标准标识符 "CD001" */
    uint8_t  version;                        /* 版本号 */
};

/* 主卷描述符 (Primary Volume Descriptor) */
struct iso9660_pvd {
    uint8_t  type;                           /* 类型 = 1 */
    char     std_identifier[5];              /* "CD001" */
    uint8_t  version;                        /* 版本 = 1 */
    char     unused1[1];
    char     system_id[32];                  /* 系统标识符 */
    char     volume_id[32];                  /* 卷标识符 */
    char     unused2[8];
    uint32_t volume_space_size_le;           /* 卷空间大小 (LE) */
    uint32_t volume_space_size_be;           /* 卷空间大小 (BE) */
    char     unused3[32];
    uint16_t volume_set_size_le;             /* 卷集大小 (LE) */
    uint16_t volume_set_size_be;             /* 卷集大小 (BE) */
    uint16_t volume_sequence_le;             /* 卷序号 (LE) */
    uint16_t volume_sequence_be;             /* 卷序号 (BE) */
    uint16_t logical_block_size_le;          /* 逻辑块大小 (LE) */
    uint16_t logical_block_size_be;          /* 逻辑块大小 (BE) */
    uint32_t path_table_size_le;             /* 路径表大小 (LE) */
    uint32_t path_table_size_be;             /* 路径表大小 (BE) */
    uint32_t path_table_loc_le;              /* L型路径表位置 (LE) */
    uint32_t path_table_loc_opt_le;          /* L型可选路径表位置 */
    uint32_t path_table_loc_be;              /* M型路径表位置 (BE) */
    uint32_t path_table_loc_opt_be;          /* M型可选路径表位置 */
    uint8_t  root_dir_record[34];            /* 根目录记录 */
    char     volume_set_id[128];             /* 卷集标识符 */
    char     publisher_id[128];              /* 出版商标识符 */
    char     data_preparer_id[128];          /* 数据准备者标识符 */
    char     application_id[128];            /* 应用标识符 */
    char     copyright_file_id[37];          /* 版权文件标识符 */
    char     abstract_file_id[37];           /* 摘要文件标识符 */
    char     bibliographic_file_id[37];      /* 书目文件标识符 */
    char     creation_date[17];              /* 创建日期 */
    char     modification_date[17];          /* 修改日期 */
    char     expiration_date[17];            /* 过期日期 */
    char     effective_date[17];             /* 生效日期 */
    uint8_t  file_structure_version;         /* 文件结构版本 */
    char     unused4[1];
    char     application_use[512];           /* 应用使用区 */
    char     unused5[653];
} __attribute__((packed));

/* 目录记录 (Directory Record) */
struct iso9660_dir_record {
    uint8_t  length;                         /* 记录长度 */
    uint8_t  ext_attr_length;                /* 扩展属性记录长度 */
    uint32_t extent_le;                      /* 数据起始块号 (LE) */
    uint32_t extent_be;                      /* 数据起始块号 (BE) */
    uint32_t data_length_le;                 /* 数据长度 (LE) */
    uint32_t data_length_be;                 /* 数据长度 (BE) */
    uint8_t  recording_date[7];              /* 记录日期时间 */
    uint8_t  flags;                          /* 文件标志 */
    uint8_t  file_unit_size;                 /* 文件单元大小 */
    uint8_t  interleave_gap;                 /* 交错间隔大小 */
    uint16_t volume_sequence_le;             /* 卷序号 (LE) */
    uint16_t volume_sequence_be;             /* 卷序号 (BE) */
    uint8_t  id_length;                      /* 文件标识符长度 */
    char     id[1];                          /* 文件标识符 (变长) */
} __attribute__((packed));

/* 路径表记录 */
struct iso9660_path_table_entry {
    uint8_t  name_length;                    /* 目录标识符长度 */
    uint8_t  ext_attr_length;                /* 扩展属性记录长度 */
    uint32_t extent;                         /* 目录数据起始块号 */
    uint16_t parent_index;                   /* 父目录在路径表中的索引 */
    char     name[1];                        /* 目录标识符 (变长) */
} __attribute__((packed));

/* Rock Ridge 系统使用区域头部 */
struct rr_sys_use_header {
    uint8_t  signature[2];                   /* 两个字符签名 */
    uint8_t  length;                         /* 条目长度 */
    uint8_t  version;                        /* 版本 = 1 */
} __attribute__((packed));

/* Rock Ridge PX 条目 (POSIX 属性) */
struct rr_px_entry {
    uint8_t  signature[2];                   /* 'P','X' */
    uint8_t  length;                         /* 条目长度 */
    uint8_t  version;                        /* 版本 = 1 */
    uint32_t mode_le;                        /* 文件模式 (LE) */
    uint32_t mode_be;                        /* 文件模式 (BE) */
    uint32_t nlink_le;                       /* 链接数 (LE) */
    uint32_t nlink_be;                       /* 链接数 (BE) */
    uint32_t uid_le;                         /* 用户 ID (LE) */
    uint32_t uid_be;                         /* 用户 ID (BE) */
    uint32_t gid_le;                         /* 组 ID (LE) */
    uint32_t gid_be;                         /* 组 ID (BE) */
    uint32_t ino_le;                         /* Inode 号 (LE) */
    uint32_t ino_be;                         /* Inode 号 (BE) */
} __attribute__((packed));

/* Rock Ridge NM 条目 (备用名称) */
struct rr_nm_entry {
    uint8_t  signature[2];                   /* 'N','M' */
    uint8_t  length;                         /* 条目长度 */
    uint8_t  version;                        /* 版本 = 1 */
    uint8_t  flags;                          /* 标志 */
    char     name[1];                        /* 名称 (变长) */
} __attribute__((packed));

/* Rock Ridge TF 条目 (时间戳) */
struct rr_tf_entry {
    uint8_t  signature[2];                   /* 'T','F' */
    uint8_t  length;                         /* 条目长度 */
    uint8_t  version;                        /* 版本 = 1 */
    uint8_t  flags;                          /* 时间戳类型标志 */
    uint8_t  data[1];                        /* 时间戳数据 (变长) */
} __attribute__((packed));

/* ═══════════════════════════════════════════════════════════════════════════
 * ISO9660 文件系统运行时结构
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 目录缓存条目 - 加速重复目录解析 */
struct iso9660_dcache_entry {
    char     path[256];                      /* 完整路径 */
    uint32_t extent;                         /* 数据起始块号 */
    uint32_t data_length;                    /* 数据长度 */
    uint8_t  is_dir;                         /* 是否为目录 */
    uint8_t  valid;                          /* 是否有效 */
    struct iso9660_dcache_entry *next;       /* 哈希链表 */
};

/* 文件打开私有数据 */
struct iso9660_file_data {
    uint32_t extent;                         /* 数据起始块号 */
    uint32_t data_length;                    /* 数据长度 */
    uint64_t offset;                         /* 当前偏移 */
    uint8_t  is_dir;                         /* 是否为目录 */
    uint32_t mode;                           /* POSIX mode (来自 Rock Ridge) */
    uint32_t uid;                            /* POSIX uid (来自 Rock Ridge) */
    uint32_t gid;                            /* POSIX gid (来自 Rock Ridge) */
    uint32_t nlink;                          /* 链接数 (来自 Rock Ridge) */
    uint32_t ino;                            /* Inode 号 (来自 Rock Ridge) */
};

/* ISO9660 文件系统实例 */
struct iso9660_fs {
    /* 卷描述符信息 */
    char     volume_id[33];                  /* 卷标识符 (以 \0 结尾) */
    uint32_t volume_space_size;              /* 卷空间大小 (块数) */
    uint16_t logical_block_size;             /* 逻辑块大小 */

    /* 根目录 */
    uint32_t root_extent;                    /* 根目录起始块号 */
    uint32_t root_data_length;               /* 根目录数据长度 */

    /* 路径表 */
    uint32_t path_table_loc;                 /* 路径表位置 */
    uint32_t path_table_size;                /* 路径表大小 */

    /* 扩展支持标志 */
    uint8_t  has_rock_ridge;                 /* 是否有 Rock Ridge 扩展 */
    uint8_t  has_joliet;                     /* 是否有 Joliet 扩展 */

    /* Joliet 补充卷描述符 */
    uint32_t joliet_root_extent;             /* Joliet 根目录起始块号 */
    uint32_t joliet_root_data_length;        /* Joliet 根目录数据长度 */

    /* 设备信息 */
    uint64_t partition_start;                /* 分区起始扇区 */

    /* 目录缓存 */
    struct iso9660_dcache_entry *dcache[64]; /* 目录缓存哈希表 */
    spinlock_t dcache_lock;                  /* 目录缓存锁 */

    /* 统计 */
    uint64_t reads;                          /* 读取次数 */
    uint64_t cache_hits;                     /* 缓存命中次数 */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 函数声明
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 初始化 ISO9660 文件系统 */
int iso9660_init(void);

/* 挂载 ISO9660 文件系统 */
int iso9660_mount_fs(struct iso9660_fs *fs, uint64_t partition_start);

/* 卸载 ISO9660 文件系统 */
void iso9660_unmount_fs(struct iso9660_fs *fs);

/* 获取 ISO9660 VFS 操作接口 */
struct vfs_ops *iso9660_get_ops(void);

/* 读取逻辑块 */
int iso9660_read_block(struct iso9660_fs *fs, uint32_t lba, void *buf);

/* 解析路径到目录记录 */
int iso9660_path_lookup(struct iso9660_fs *fs, const char *path,
                        struct iso9660_dir_record *rec, uint8_t *rr_flags);

/* 读取目录内容 */
int iso9660_read_dir(struct iso9660_fs *fs, uint32_t extent, uint32_t length,
                     int index, struct vfs_dirent *dirent);

/* UCS-2 转 UTF-8 (Joliet 文件名) */
int iso9660_ucs2_to_utf8(const uint8_t *ucs2, int ucs2_len,
                          char *utf8, int utf8_max);

/* 解析 Rock Ridge 扩展属性 */
int iso9660_parse_rr(const uint8_t *data, int len,
                     struct iso9660_file_data *fdata);

#endif /* ISO9660_H */
