/*
 * ═══════════════════════════════════════════════════════════════════════════
 * Zratro OS - ISO9660 文件系统实现
 *
 * 只读文件系统，用于 CD-ROM 支持
 * - Primary Volume Descriptor 解析
 * - Rock Ridge 扩展 (POSIX 文件属性)
 * - Joliet 扩展 (UCS-2 Unicode 文件名)
 * - 目录记录解析与路径查找
 * - 文件读取支持
 * - struct vfs_ops 接口实现
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "iso9660.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "spinlock.h"
#include "disk.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 辅助函数 - 大端/小端转换
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 从 both-endian 字段读取小端值 (ISO9660 使用 both-endian 编码) */
static inline uint16_t iso9660_read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t iso9660_read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 验证 both-endian 字段 (小端和大端值应相等) */
static inline int iso9660_validate_both16(const uint8_t *p) {
    uint16_t le = iso9660_read_le16(p);
    uint16_t be = iso9660_read_le16(p + 2);
    return le == be;
}

static inline int iso9660_validate_both32(const uint8_t *p) {
    uint32_t le = iso9660_read_le32(p);
    uint32_t be = (iso9660_read_le16(p + 6) << 16) | iso9660_read_le16(p + 4);
    return le == be;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 全局状态
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 默认文件系统实例 (单设备场景) */
static struct iso9660_fs g_iso9660_fs;

/* 全局实例锁 */
static spinlock_t g_iso9660_lock;

/* ═══════════════════════════════════════════════════════════════════════════
 * 底层 I/O - 读取逻辑块
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 读取一个逻辑块 (2048 字节) */
int iso9660_read_block(struct iso9660_fs *fs, uint32_t lba, void *buf) {
    /* ISO9660 逻辑块 = 2048 字节, 磁盘扇区 = 512 字节
     * 一个逻辑块 = 4 个磁盘扇区 */
    uint64_t disk_lba = fs->partition_start + (uint64_t)lba * 4;

    /* 读取 4 个连续扇区 */
    for (int i = 0; i < 4; i++) {
        if (disk_read_sector(disk_lba + i, (uint8_t *)buf + i * 512) != 0) {
            return -1;
        }
    }

    fs->reads++;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 卷描述符解析
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 解析主卷描述符 */
static int iso9660_parse_pvd(struct iso9660_fs *fs, const struct iso9660_pvd *pvd) {
    /* 验证标准标识符 */
    if (pvd->std_identifier[0] != ISO9660_VD_STD_ID1 ||
        pvd->std_identifier[1] != ISO9660_VD_STD_ID2 ||
        pvd->std_identifier[2] != ISO9660_VD_STD_ID3 ||
        pvd->std_identifier[3] != ISO9660_VD_STD_ID4 ||
        pvd->std_identifier[4] != ISO9660_VD_STD_ID5) {
        println("[ISO9660] PVD 标准标识符无效");
        return -1;
    }

    /* 读取卷空间大小 */
    fs->volume_space_size = iso9660_read_le32((const uint8_t *)&pvd->volume_space_size_le);

    /* 读取逻辑块大小 */
    fs->logical_block_size = iso9660_read_le16((const uint8_t *)&pvd->logical_block_size_le);
    if (fs->logical_block_size != ISO9660_LB_SIZE) {
        println("[ISO9660] 不支持的逻辑块大小");
        return -1;
    }

    /* 读取卷标识符 */
    for (int i = 0; i < 32; i++) {
        fs->volume_id[i] = pvd->volume_id[i];
    }
    fs->volume_id[32] = '\0';
    /* 去除尾部空格 */
    for (int i = 31; i >= 0 && fs->volume_id[i] == ' '; i--) {
        fs->volume_id[i] = '\0';
    }

    /* 读取路径表信息 */
    fs->path_table_loc = iso9660_read_le32((const uint8_t *)&pvd->path_table_loc_le);
    fs->path_table_size = iso9660_read_le32((const uint8_t *)&pvd->path_table_size_le);

    /* 解析根目录记录 (34 字节) */
    const uint8_t *root_rec = pvd->root_dir_record;
    fs->root_extent = iso9660_read_le32(root_rec + 2);
    fs->root_data_length = iso9660_read_le32(root_rec + 10);

    /* 初始化扩展标志 */
    fs->has_rock_ridge = 0;
    fs->has_joliet = 0;

    print("[ISO9660] 卷标识: ");
    println(fs->volume_id);
    print("[ISO9660] 卷空间: ");
    print_hex(fs->volume_space_size);
    println(" 块");
    print("[ISO9660] 根目录: extent=");
    print_hex(fs->root_extent);
    print(" length=");
    print_hex(fs->root_data_length);
    println("");

    return 0;
}

/* 检查补充卷描述符是否为 Joliet */
static int iso9660_check_joliet(const struct iso9660_pvd *pvd) {
    /* Joliet 的转义序列在 unused3[32] 字段中
     * 标准转义序列: %/@ (UCS-2 Level 1), %/C (Level 2), %/E (Level 3) */
    const uint8_t *esc = (const uint8_t *)pvd + 88; /* unused3 字段偏移 */
    if (esc[0] == '%' && esc[1] == '/' &&
        (esc[2] == '@' || esc[2] == 'C' || esc[2] == 'E')) {
        return 1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * UCS-2 转 UTF-8 (Joliet 文件名支持)
 * ═══════════════════════════════════════════════════════════════════════════ */

int iso9660_ucs2_to_utf8(const uint8_t *ucs2, int ucs2_len,
                          char *utf8, int utf8_max) {
    int out = 0;

    for (int i = 0; i + 1 < ucs2_len && out < utf8_max - 1; i += 2) {
        /* 读取 UCS-2 字符 (大端序, ISO9660 Joliet 使用大端) */
        uint16_t ch = ((uint16_t)ucs2[i] << 8) | ucs2[i + 1];

        if (ch == 0) break;

        if (ch < 0x80) {
            /* ASCII 字符 */
            utf8[out++] = (char)ch;
        } else if (ch < 0x800) {
            /* 2 字节 UTF-8 */
            if (out + 2 >= utf8_max) break;
            utf8[out++] = (char)(0xC0 | (ch >> 6));
            utf8[out++] = (char)(0x80 | (ch & 0x3F));
        } else {
            /* 3 字节 UTF-8 */
            if (out + 3 >= utf8_max) break;
            utf8[out++] = (char)(0xE0 | (ch >> 12));
            utf8[out++] = (char)(0x80 | ((ch >> 6) & 0x3F));
            utf8[out++] = (char)(0x80 | (ch & 0x3F));
        }
    }

    utf8[out] = '\0';
    return out;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Rock Ridge 扩展解析
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 解析 Rock Ridge 系统使用区域 */
int iso9660_parse_rr(const uint8_t *data, int len,
                     struct iso9660_file_data *fdata) {
    int pos = 0;
    int rr_found = 0;
    char nm_buf[256];
    int nm_len = 0;
    int nm_continue = 0;

    while (pos + 4 <= len) {
        uint8_t sig0 = data[pos];
        uint8_t sig1 = data[pos + 1];
        uint8_t entry_len = data[pos + 2];
        uint8_t entry_ver = data[pos + 3];

        /* 检查条目有效性 */
        if (entry_len < 4 || pos + entry_len > len) {
            break;
        }

        /* PX - POSIX 属性 */
        if (sig0 == 'P' && sig1 == 'X') {
            if (entry_len >= 36 && entry_ver == 1) {
                fdata->mode = iso9660_read_le32(data + pos + 4);
                fdata->nlink = iso9660_read_le32(data + pos + 12);
                fdata->uid = iso9660_read_le32(data + pos + 20);
                fdata->gid = iso9660_read_le32(data + pos + 28);
                if (entry_len >= 44) {
                    fdata->ino = iso9660_read_le32(data + pos + 36);
                }
                rr_found = 1;
            }
        }
        /* NM - 备用名称 */
        else if (sig0 == 'N' && sig1 == 'M') {
            if (entry_len >= 5 && entry_ver == 1) {
                uint8_t nm_flags = data[pos + 4];
                int nm_part_len = entry_len - 5;
                if (nm_part_len > 0 && nm_len + nm_part_len < 255) {
                    for (int i = 0; i < nm_part_len; i++) {
                        nm_buf[nm_len++] = data[pos + 5 + i];
                    }
                }
                nm_continue = (nm_flags & RR_NM_CONTINUE) ? 1 : 0;
                rr_found = 1;
            }
        }
        /* TF - 时间戳 */
        else if (sig0 == 'T' && sig1 == 'F') {
            /* 时间戳信息已记录，暂不使用 */
            rr_found = 1;
        }
        /* SL - 符号链接 */
        else if (sig0 == 'S' && sig1 == 'L') {
            rr_found = 1;
        }
        /* CE - 续接区域 (暂不处理) */
        else if (sig0 == 'C' && sig1 == 'E') {
            /* 续接区域需要额外读取，简化实现中跳过 */
        }
        /* ER - 扩展引用 (标记 Rock Ridge 存在) */
        else if (sig0 == 'E' && sig1 == 'R') {
            rr_found = 1;
        }
        /* 未知或填充条目 */
        else if (sig0 == '\0' || sig1 == '\0') {
            /* 填充字节，跳过 */
            pos++;
            continue;
        }

        pos += entry_len;
    }

    /* NM 条目处理完成 */
    if (nm_len > 0 && !nm_continue) {
        nm_buf[nm_len] = '\0';
    }

    return rr_found;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 目录记录解析
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 从目录记录中提取文件名 (ISO9660 标准格式) */
static int iso9660_extract_name(const struct iso9660_dir_record *rec,
                                 char *name, int max_len, int is_joliet) {
    int id_len = rec->id_length;
    const char *id = rec->id;

    /* 跳过当前目录和父目录条目 */
    if (id_len == 1 && (id[0] == ISO9660_CURRENT_DIR || id[0] == ISO9660_PARENT_DIR)) {
        if (id[0] == ISO9660_CURRENT_DIR) {
            name[0] = '.'; name[1] = '\0';
        } else {
            name[0] = '.'; name[1] = '.'; name[2] = '\0';
        }
        return 0;
    }

    /* Joliet: UCS-2 编码的文件名 */
    if (is_joliet) {
        return iso9660_ucs2_to_utf8((const uint8_t *)id, id_len, name, max_len);
    }

    /* 标准 ISO9660 文件名: 8.3 格式 */
    int out = 0;
    int dot_added = 0;

    for (int i = 0; i < id_len && out < max_len - 1; i++) {
        char c = id[i];

        /* 遇到版本号分隔符 ';' 则停止 */
        if (c == ISO9660_FILE_SEP) {
            break;
        }

        /* 遇到 '.' 分隔符 */
        if (c == '.') {
            if (dot_added) {
                /* 第二个点，跳过 (如 .;1 的情况) */
                continue;
            }
            /* 检查后面是否还有非空字符 */
            int has_ext = 0;
            for (int j = i + 1; j < id_len; j++) {
                if (id[j] != '.' && id[j] != ';' && id[j] != ' ' && id[j] != '\0') {
                    has_ext = 1;
                    break;
                }
            }
            if (has_ext) {
                name[out++] = '.';
                dot_added = 1;
            }
            continue;
        }

        /* 跳过尾部空格 */
        if (c == ' ') {
            continue;
        }

        /* 转换为大写 (ISO9660 标准文件名大写) */
        if (c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        }

        name[out++] = c;
    }

    name[out] = '\0';
    return out;
}

/* 获取目录记录中的系统使用区域 (Rock Ridge 扩展) */
static const uint8_t *iso9660_get_sys_use(const struct iso9660_dir_record *rec,
                                           int *su_len) {
    int id_len = rec->id_length;
    /* 系统使用区域偏移 = 固定头部 (33) + id_length + 偶数对齐填充 */
    int su_offset = 33 + id_len;
    if (id_len % 2 == 0) {
        su_offset++;    /* 偶数长度 id 后有填充字节 */
    }

    *su_len = rec->length - su_offset;
    if (*su_len <= 0) {
        return NULL;
    }

    return (const uint8_t *)rec + su_offset;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 目录缓存
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 目录缓存哈希函数 */
static inline uint32_t iso9660_dcache_hash(const char *path) {
    uint32_t hash = 5381;
    while (*path) {
        hash = ((hash << 5) + hash) + (uint8_t)*path;
        path++;
    }
    return hash & 63;  /* 64 个桶 */
}

/* 目录缓存查找 */
static struct iso9660_dcache_entry *iso9660_dcache_lookup(struct iso9660_fs *fs,
                                                            const char *path) {
    uint32_t idx = iso9660_dcache_hash(path);
    spin_lock(&fs->dcache_lock);
    struct iso9660_dcache_entry *e = fs->dcache[idx];
    while (e) {
        if (e->valid && strcmp(e->path, path) == 0) {
            spin_unlock(&fs->dcache_lock);
            fs->cache_hits++;
            return e;
        }
        e = e->next;
    }
    spin_unlock(&fs->dcache_lock);
    return NULL;
}

/* 目录缓存添加 */
static void iso9660_dcache_add(struct iso9660_fs *fs, const char *path,
                                 uint32_t extent, uint32_t data_length,
                                 uint8_t is_dir) {
    /* 检查是否已存在 */
    struct iso9660_dcache_entry *existing = iso9660_dcache_lookup(fs, path);
    if (existing) {
        existing->extent = extent;
        existing->data_length = data_length;
        existing->is_dir = is_dir;
        return;
    }

    /* 分配新条目 */
    struct iso9660_dcache_entry *e = kmalloc(sizeof(struct iso9660_dcache_entry));
    if (!e) return;

    strncpy(e->path, path, 255);
    e->path[255] = '\0';
    e->extent = extent;
    e->data_length = data_length;
    e->is_dir = is_dir;
    e->valid = 1;

    /* 插入哈希表头部 */
    uint32_t idx = iso9660_dcache_hash(path);
    spin_lock(&fs->dcache_lock);
    e->next = fs->dcache[idx];
    fs->dcache[idx] = e;
    spin_unlock(&fs->dcache_lock);
}

/* 目录缓存清空 */
static void iso9660_dcache_flush(struct iso9660_fs *fs) {
    spin_lock(&fs->dcache_lock);
    for (int i = 0; i < 64; i++) {
        struct iso9660_dcache_entry *e = fs->dcache[i];
        while (e) {
            struct iso9660_dcache_entry *next = e->next;
            kfree(e);
            e = next;
        }
        fs->dcache[i] = NULL;
    }
    spin_unlock(&fs->dcache_lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 路径查找 - 在目录树中查找指定路径
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 在单个目录中查找指定名称的条目 */
static int iso9660_find_entry(struct iso9660_fs *fs,
                               uint32_t dir_extent, uint32_t dir_length,
                               const char *name,
                               struct iso9660_dir_record *result,
                               struct iso9660_file_data *fdata) {
    uint8_t *buf = kmalloc(dir_length);
    if (!buf) return -1;

    /* 读取整个目录 */
    uint32_t blocks = (dir_length + ISO9660_LB_SIZE - 1) / ISO9660_LB_SIZE;
    for (uint32_t i = 0; i < blocks; i++) {
        if (iso9660_read_block(fs, dir_extent + i,
                               buf + i * ISO9660_LB_SIZE) != 0) {
            kfree(buf);
            return -1;
        }
    }

    int found = 0;
    uint32_t pos = 0;

    while (pos < dir_length) {
        struct iso9660_dir_record *rec = (struct iso9660_dir_record *)(buf + pos);

        /* 记录长度为 0 表示块剩余部分为空，跳到下一个块 */
        if (rec->length == 0) {
            pos = (pos + ISO9660_LB_SIZE - 1) & ~(ISO9660_LB_SIZE - 1);
            if (pos >= dir_length) break;
            continue;
        }

        /* 跳过当前目录和父目录条目 */
        if (rec->id_length == 1 &&
            (rec->id[0] == ISO9660_CURRENT_DIR || rec->id[0] == ISO9660_PARENT_DIR)) {
            pos += rec->length;
            continue;
        }

        /* 提取文件名 */
        char entry_name[256];
        iso9660_extract_name(rec, entry_name, sizeof(entry_name), fs->has_joliet);

        /* 比较 (不区分大小写) */
        int match = 1;
        const char *n1 = name;
        const char *n2 = entry_name;
        while (*n1 && *n2) {
            char c1 = *n1;
            char c2 = *n2;
            if (c1 >= 'A' && c1 <= 'Z') c1 = c1 - 'A' + 'a';
            if (c2 >= 'A' && c2 <= 'Z') c2 = c2 - 'A' + 'a';
            if (c1 != c2) { match = 0; break; }
            n1++;
            n2++;
        }
        if (*n1 != '\0' || *n2 != '\0') match = 0;

        if (match) {
            /* 找到匹配条目 */
            if (result) {
                *result = *rec;
            }
            if (fdata) {
                fdata->extent = iso9660_read_le32((const uint8_t *)&rec->extent_le);
                fdata->data_length = iso9660_read_le32((const uint8_t *)&rec->data_length_le);
                fdata->is_dir = (rec->flags & ISO9660_FLAG_DIRECTORY) ? 1 : 0;
                fdata->mode = fdata->is_dir ? 0755 : 0644;
                fdata->uid = 0;
                fdata->gid = 0;
                fdata->nlink = 1;
                fdata->ino = fdata->extent;  /* 用 extent 作为 inode 号 */

                /* 解析 Rock Ridge 扩展 */
                if (fs->has_rock_ridge) {
                    int su_len;
                    const uint8_t *su = iso9660_get_sys_use(rec, &su_len);
                    if (su && su_len > 0) {
                        iso9660_parse_rr(su, su_len, fdata);
                    }
                }
            }
            found = 1;
            break;
        }

        pos += rec->length;
    }

    kfree(buf);
    return found ? 0 : -1;
}

/* 路径查找 - 逐级遍历目录树 */
int iso9660_path_lookup(struct iso9660_fs *fs, const char *path,
                        struct iso9660_dir_record *rec, uint8_t *rr_flags) {
    /* 检查目录缓存 */
    struct iso9660_dcache_entry *cached = iso9660_dcache_lookup(fs, path);
    if (cached) {
        if (rec) {
            memset(rec, 0, sizeof(*rec));
            /* 缓存中存储的是主机字节序值，需要写回 LE 格式 */
            rec->extent_le = cached->extent;       /* 小端机器上直接赋值即可 */
            rec->data_length_le = cached->data_length;
            rec->flags = cached->is_dir ? ISO9660_FLAG_DIRECTORY : 0;
        }
        if (rr_flags) *rr_flags = 0;
        return 0;
    }

    /* 跳过前导 '/' */
    const char *p = path;
    while (*p == '/') p++;
    if (*p == '\0') {
        /* 根目录 */
        if (rec) {
            memset(rec, 0, sizeof(*rec));
            rec->extent_le = fs->root_extent;
            rec->data_length_le = fs->root_data_length;
            rec->flags = ISO9660_FLAG_DIRECTORY;
        }
        return 0;
    }

    /* 从根目录开始逐级查找 */
    uint32_t dir_extent = fs->root_extent;
    uint32_t dir_length = fs->root_data_length;

    /* 如果有 Joliet 扩展，优先使用 Joliet 根目录 */
    if (fs->has_joliet) {
        dir_extent = fs->joliet_root_extent;
        dir_length = fs->joliet_root_data_length;
    }

    char component[256];

    while (*p) {
        /* 提取路径组件 */
        int len = 0;
        while (*p && *p != '/' && len < 255) {
            component[len++] = *p++;
        }
        component[len] = '\0';

        /* 跳过 '/' */
        while (*p == '/') p++;

        /* 在当前目录中查找 */
        struct iso9660_file_data fdata;
        memset(&fdata, 0, sizeof(fdata));

        if (iso9660_find_entry(fs, dir_extent, dir_length, component,
                               rec, &fdata) != 0) {
            return -1;  /* 未找到 */
        }

        /* 如果还有后续路径组件，当前条目必须是目录 */
        if (*p) {
            if (!fdata.is_dir) {
                return -1;  /* 不是目录 */
            }
            dir_extent = fdata.extent;
            dir_length = fdata.data_length;
        } else {
            /* 最终条目 */
            if (rr_flags) *rr_flags = 0;
        }
    }

    /* 添加到缓存 */
    if (rec) {
        uint32_t ext = iso9660_read_le32((const uint8_t *)&rec->extent_le);
        uint32_t dlen = iso9660_read_le32((const uint8_t *)&rec->data_length_le);
        uint8_t is_dir = (rec->flags & ISO9660_FLAG_DIRECTORY) ? 1 : 0;
        iso9660_dcache_add(fs, path, ext, dlen, is_dir);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 目录读取
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 读取目录中第 index 个条目 */
int iso9660_read_dir(struct iso9660_fs *fs, uint32_t extent, uint32_t length,
                     int index, struct vfs_dirent *dirent) {
    uint8_t *buf = kmalloc(length);
    if (!buf) return -1;

    /* 读取整个目录 */
    uint32_t blocks = (length + ISO9660_LB_SIZE - 1) / ISO9660_LB_SIZE;
    for (uint32_t i = 0; i < blocks; i++) {
        if (iso9660_read_block(fs, extent + i,
                               buf + i * ISO9660_LB_SIZE) != 0) {
            kfree(buf);
            return -1;
        }
    }

    int cur_index = 0;
    uint32_t pos = 0;

    while (pos < length) {
        struct iso9660_dir_record *rec = (struct iso9660_dir_record *)(buf + pos);

        if (rec->length == 0) {
            pos = (pos + ISO9660_LB_SIZE - 1) & ~(ISO9660_LB_SIZE - 1);
            if (pos >= length) break;
            continue;
        }

        /* 跳过当前目录和父目录条目 */
        if (rec->id_length == 1 &&
            (rec->id[0] == ISO9660_CURRENT_DIR || rec->id[0] == ISO9660_PARENT_DIR)) {
            pos += rec->length;
            continue;
        }

        if (cur_index == index) {
            /* 提取文件名 */
            iso9660_extract_name(rec, dirent->name, sizeof(dirent->name),
                                 fs->has_joliet);

            /* 如果有 Rock Ridge NM 条目，使用备用名称 */
            if (fs->has_rock_ridge) {
                int su_len;
                const uint8_t *su = iso9660_get_sys_use(rec, &su_len);
                if (su && su_len > 4) {
                    int rr_pos = 0;
                    while (rr_pos + 4 <= su_len) {
                        if (su[rr_pos] == 'N' && su[rr_pos + 1] == 'M') {
                            uint8_t nm_len = su[rr_pos + 2];
                            uint8_t nm_flags = su[rr_pos + 4];
                            int part_len = nm_len - 5;
                            if (part_len > 0 && !(nm_flags & (RR_NM_CURRENT | RR_NM_PARENT))) {
                                /* 用 Rock Ridge 名称覆盖 */
                                int copy_len = part_len < 255 ? part_len : 255;
                                for (int i = 0; i < copy_len; i++) {
                                    dirent->name[i] = su[rr_pos + 5 + i];
                                }
                                dirent->name[copy_len] = '\0';
                            }
                            break;
                        }
                        uint8_t entry_len = su[rr_pos + 2];
                        if (entry_len < 4) break;
                        rr_pos += entry_len;
                    }
                }
            }

            /* 填充目录项信息 */
            dirent->type = (rec->flags & ISO9660_FLAG_DIRECTORY) ?
                           VFS_TYPE_DIR : VFS_TYPE_FILE;
            dirent->inode = iso9660_read_le32((const uint8_t *)&rec->extent_le);
            dirent->size = iso9660_read_le32((const uint8_t *)&rec->data_length_le);

            kfree(buf);
            return 0;
        }

        cur_index++;
        pos += rec->length;
    }

    kfree(buf);
    return -1;  /* 超出范围 */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * VFS 接口实现
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ISO9660 VFS open */
static int iso9660_vfs_open(struct vfs_file *file, const char *path, uint32_t mode) {
    struct iso9660_fs *fs = &g_iso9660_fs;

    /* ISO9660 是只读文件系统，拒绝写模式 */
    if (mode & (VFS_MODE_WRITE | VFS_MODE_CREATE | VFS_MODE_TRUNC | VFS_MODE_APPEND)) {
        return VFS_ERR_ROFS;
    }

    /* 路径查找 */
    struct iso9660_dir_record rec;
    if (iso9660_path_lookup(fs, path, &rec, NULL) != 0) {
        return VFS_ERR_NOTFOUND;
    }

    /* 创建文件私有数据 */
    struct iso9660_file_data *fdata = kmalloc(sizeof(struct iso9660_file_data));
    if (!fdata) return VFS_ERR_NOMEM;

    fdata->extent = iso9660_read_le32((const uint8_t *)&rec.extent_le);
    fdata->data_length = iso9660_read_le32((const uint8_t *)&rec.data_length_le);
    fdata->is_dir = (rec.flags & ISO9660_FLAG_DIRECTORY) ? 1 : 0;
    fdata->offset = 0;
    fdata->mode = fdata->is_dir ? 0755 : 0644;
    fdata->uid = 0;
    fdata->gid = 0;
    fdata->nlink = 1;
    fdata->ino = fdata->extent;

    /* 解析 Rock Ridge 属性
     * 注意: 缓存命中时 rec 只有 extent/length/flags，无法解析 RR */
    if (fs->has_rock_ridge && rec.id_length > 0) {
        int su_len;
        const uint8_t *su = iso9660_get_sys_use(&rec, &su_len);
        if (su && su_len > 0) {
            iso9660_parse_rr(su, su_len, fdata);
        }
    }

    file->fs_data = fdata;
    file->offset = 0;
    file->mode = mode;
    file->is_dirty = 0;

    return 0;
}

/* ISO9660 VFS close */
static int iso9660_vfs_close(struct vfs_file *file) {
    if (file->fs_data) {
        kfree(file->fs_data);
        file->fs_data = NULL;
    }
    return 0;
}

/* ISO9660 VFS read */
static ssize_t iso9660_vfs_read(struct vfs_file *file, void *buf, size_t count) {
    struct iso9660_fs *fs = &g_iso9660_fs;
    struct iso9660_file_data *fdata = file->fs_data;

    if (!fdata) return VFS_ERR_INVAL;

    /* 不允许读目录 */
    if (fdata->is_dir) return VFS_ERR_ISDIR;

    /* 计算可读字节数 */
    uint64_t remaining = fdata->data_length - fdata->offset;
    if (remaining == 0) return 0;   /* EOF */
    if (count > remaining) count = (size_t)remaining;

    /* 按块读取 */
    size_t total_read = 0;
    uint8_t block_buf[ISO9660_LB_SIZE];

    while (total_read < count) {
        /* 计算当前偏移对应的逻辑块和块内偏移 */
        uint32_t block_idx = (uint32_t)(fdata->offset / ISO9660_LB_SIZE);
        uint32_t block_off = (uint32_t)(fdata->offset % ISO9660_LB_SIZE);

        /* 读取逻辑块 */
        if (iso9660_read_block(fs, fdata->extent + block_idx, block_buf) != 0) {
            return total_read > 0 ? (ssize_t)total_read : VFS_ERR_IO;
        }

        /* 计算本次可拷贝的字节数 */
        size_t block_remaining = ISO9660_LB_SIZE - block_off;
        size_t to_copy = count - total_read;
        if (to_copy > block_remaining) to_copy = block_remaining;

        /* 拷贝数据 */
        memcpy((uint8_t *)buf + total_read, block_buf + block_off, to_copy);

        total_read += to_copy;
        fdata->offset += to_copy;
    }

    file->offset = fdata->offset;
    return (ssize_t)total_read;
}

/* ISO9660 VFS write - 只读文件系统 */
static ssize_t iso9660_vfs_write(struct vfs_file *file, const void *buf, size_t count) {
    (void)file; (void)buf; (void)count;
    return VFS_ERR_ROFS;
}

/* ISO9660 VFS stat */
static int iso9660_vfs_stat(const char *path, struct vfs_stat *stat) {
    struct iso9660_fs *fs = &g_iso9660_fs;
    struct iso9660_dir_record rec;

    if (iso9660_path_lookup(fs, path, &rec, NULL) != 0) {
        return VFS_ERR_NOTFOUND;
    }

    memset(stat, 0, sizeof(*stat));
    stat->size = iso9660_read_le32((const uint8_t *)&rec.data_length_le);
    stat->type = (rec.flags & ISO9660_FLAG_DIRECTORY) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    stat->inode = iso9660_read_le32((const uint8_t *)&rec.extent_le);
    stat->blksize = ISO9660_LB_SIZE;
    stat->blocks = (stat->size + ISO9660_LB_SIZE - 1) / ISO9660_LB_SIZE;
    stat->nlink = 1;
    stat->mode = (rec.flags & ISO9660_FLAG_DIRECTORY) ? 0755 : 0644;

    /* 解析 Rock Ridge 属性
     * 注意: 缓存命中时 rec 只有 extent/length/flags，无法解析 RR */
    if (fs->has_rock_ridge && rec.id_length > 0) {
        struct iso9660_file_data fdata;
        memset(&fdata, 0, sizeof(fdata));
        int su_len;
        const uint8_t *su = iso9660_get_sys_use(&rec, &su_len);
        if (su && su_len > 0) {
            iso9660_parse_rr(su, su_len, &fdata);
            stat->mode = fdata.mode;
            stat->uid = fdata.uid;
            stat->gid = fdata.gid;
            stat->nlink = fdata.nlink;
            stat->inode = fdata.ino;
        }
    }

    return 0;
}

/* ISO9660 VFS readdir */
static int iso9660_vfs_readdir(const char *path, struct vfs_dirent *dirent, int index) {
    struct iso9660_fs *fs = &g_iso9660_fs;
    struct iso9660_dir_record rec;

    /* 查找目录 */
    if (iso9660_path_lookup(fs, path, &rec, NULL) != 0) {
        return VFS_ERR_NOTFOUND;
    }

    /* 必须是目录 */
    if (!(rec.flags & ISO9660_FLAG_DIRECTORY)) {
        return VFS_ERR_NOTDIR;
    }

    uint32_t extent = iso9660_read_le32((const uint8_t *)&rec.extent_le);
    uint32_t length = iso9660_read_le32((const uint8_t *)&rec.data_length_le);

    return iso9660_read_dir(fs, extent, length, index, dirent);
}

/* ISO9660 VFS fstat */
static int iso9660_vfs_fstat(struct vfs_file *file, struct vfs_stat *stat) {
    struct iso9660_file_data *fdata = file->fs_data;
    if (!fdata) return VFS_ERR_INVAL;

    memset(stat, 0, sizeof(*stat));
    stat->size = fdata->data_length;
    stat->type = fdata->is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    stat->inode = fdata->ino;
    stat->blksize = ISO9660_LB_SIZE;
    stat->blocks = (stat->size + ISO9660_LB_SIZE - 1) / ISO9660_LB_SIZE;
    stat->nlink = fdata->nlink;
    stat->mode = fdata->mode;
    stat->uid = fdata->uid;
    stat->gid = fdata->gid;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * VFS 操作表
 * ═══════════════════════════════════════════════════════════════════════════ */

static struct vfs_ops iso9660_vfs_ops = {
    .open = iso9660_vfs_open,
    .close = iso9660_vfs_close,
    .read = iso9660_vfs_read,
    .write = iso9660_vfs_write,
    .stat = iso9660_vfs_stat,
    .readdir = iso9660_vfs_readdir,
    .mkdir = NULL,           /* 只读文件系统不支持 */
    .rmdir = NULL,           /* 只读文件系统不支持 */
    .unlink = NULL,          /* 只读文件系统不支持 */
    .rename = NULL,          /* 只读文件系统不支持 */
    .fstat = iso9660_vfs_fstat,
    .sync = NULL,            /* 只读文件系统不需要同步 */
    .truncate = NULL,        /* 只读文件系统不支持 */
    .chmod = NULL,           /* 只读文件系统不支持 */
    .chown = NULL,           /* 只读文件系统不支持 */
};

/* 获取 ISO9660 VFS 操作接口 */
struct vfs_ops *iso9660_get_ops(void) {
    return &iso9660_vfs_ops;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 初始化与挂载
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 初始化 ISO9660 文件系统 */
int iso9660_init(void) {
    spin_lock_init(&g_iso9660_lock);
    memset(&g_iso9660_fs, 0, sizeof(g_iso9660_fs));
    spin_lock_init(&g_iso9660_fs.dcache_lock);
    println("[ISO9660] 文件系统已初始化");
    return 0;
}

/* 挂载 ISO9660 文件系统 */
int iso9660_mount_fs(struct iso9660_fs *fs, uint64_t partition_start) {
    memset(fs, 0, sizeof(*fs));
    fs->partition_start = partition_start;
    spin_lock_init(&fs->dcache_lock);

    /* 读取卷描述符序列 (从逻辑块 16 开始) */
    uint8_t vd_buf[ISO9660_VD_SIZE];
    int pvd_found = 0;

    for (uint32_t lba = 16; ; lba++) {
        if (iso9660_read_block(fs, lba, vd_buf) != 0) {
            println("[ISO9660] 读取卷描述符失败");
            return -1;
        }

        struct iso9660_vd_header *hdr = (struct iso9660_vd_header *)vd_buf;

        /* 验证标准标识符 */
        if (hdr->std_identifier[0] != 'C' || hdr->std_identifier[1] != 'D' ||
            hdr->std_identifier[2] != '0' || hdr->std_identifier[3] != '0' ||
            hdr->std_identifier[4] != '1') {
            println("[ISO9660] 卷描述符标识无效");
            return -1;
        }

        if (hdr->type == ISO9660_VD_PRIMARY) {
            /* 解析主卷描述符 */
            if (iso9660_parse_pvd(fs, (const struct iso9660_pvd *)vd_buf) != 0) {
                return -1;
            }
            pvd_found = 1;
        }
        else if (hdr->type == ISO9660_VD_SUPPLEMENT) {
            /* 检查是否为 Joliet 补充卷描述符 */
            if (iso9660_check_joliet((const struct iso9660_pvd *)vd_buf)) {
                fs->has_joliet = 1;
                const struct iso9660_pvd *spvd = (const struct iso9660_pvd *)vd_buf;
                const uint8_t *root_rec = spvd->root_dir_record;
                fs->joliet_root_extent = iso9660_read_le32(root_rec + 2);
                fs->joliet_root_data_length = iso9660_read_le32(root_rec + 10);
                println("[ISO9660] 检测到 Joliet 扩展");
            }
        }
        else if (hdr->type == ISO9660_VD_TERMINATOR) {
            break;  /* 卷描述符序列结束 */
        }

        /* 安全限制: 最多扫描 100 个卷描述符 */
        if (lba > 116) {
            println("[ISO9660] 卷描述符序列过长");
            break;
        }
    }

    if (!pvd_found) {
        println("[ISO9660] 未找到主卷描述符");
        return -1;
    }

    /* 检测 Rock Ridge 扩展 - 读取根目录并检查是否有 ER 条目 */
    {
        uint8_t *root_buf = kmalloc(fs->root_data_length);
        if (root_buf) {
            uint32_t blocks = (fs->root_data_length + ISO9660_LB_SIZE - 1) / ISO9660_LB_SIZE;
            int read_ok = 1;
            for (uint32_t i = 0; i < blocks; i++) {
                if (iso9660_read_block(fs, fs->root_extent + i,
                                       root_buf + i * ISO9660_LB_SIZE) != 0) {
                    read_ok = 0;
                    break;
                }
            }

            if (read_ok) {
                uint32_t pos = 0;
                while (pos < fs->root_data_length) {
                    struct iso9660_dir_record *rec =
                        (struct iso9660_dir_record *)(root_buf + pos);
                    if (rec->length == 0) {
                        pos = (pos + ISO9660_LB_SIZE - 1) & ~(ISO9660_LB_SIZE - 1);
                        if (pos >= fs->root_data_length) break;
                        continue;
                    }

                    /* 检查系统使用区域是否有 ER 条目 */
                    int su_len;
                    const uint8_t *su = iso9660_get_sys_use(rec, &su_len);
                    if (su && su_len >= 8) {
                        int rr_pos = 0;
                        while (rr_pos + 4 <= su_len) {
                            if (su[rr_pos] == 'E' && su[rr_pos + 1] == 'R') {
                                fs->has_rock_ridge = 1;
                                println("[ISO9660] 检测到 Rock Ridge 扩展");
                                break;
                            }
                            uint8_t entry_len = su[rr_pos + 2];
                            if (entry_len < 4) break;
                            rr_pos += entry_len;
                        }
                        if (fs->has_rock_ridge) break;
                    }

                    pos += rec->length;
                }
            }
            kfree(root_buf);
        }
    }

    print("[ISO9660] 挂载成功: ");
    println(fs->volume_id);
    return 0;
}

/* 卸载 ISO9660 文件系统 */
void iso9660_unmount_fs(struct iso9660_fs *fs) {
    /* 清空目录缓存 */
    iso9660_dcache_flush(fs);
    println("[ISO9660] 文件系统已卸载");
}
