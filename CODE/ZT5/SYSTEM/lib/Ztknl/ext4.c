#include "ext4.h"
#include "heap.h"
#include "console.h"
#include "string.h"

/* 初始化 EXT4 */
int ext4_init(void) {
    /* 检查磁盘是否可用 */
    extern int disk_is_available(void);
    if (!disk_is_available()) {
        return -1;
    }
    
    /* EXT4 支持暂时只是占位符，实际挂载在 VFS 挂载时处理 */
    return 0;
}

/* 读取超级块 */
static int read_superblock(struct ext4_fs *fs) {
    uint8_t buf[4096];
    
    /* 超级块在块 0 的偏移 1024 处 */
    if (fs->read_block(fs->device, 0, buf) != 0) {
        return -1;
    }
    
    /* 复制超级块 */
    for (size_t i = 0; i < sizeof(struct ext4_super_block); i++) {
        ((uint8_t *)&fs->sb)[i] = buf[EXT4_SB_OFFSET + i];
    }
    
    /* 验证魔数 */
    if (fs->sb.s_magic != EXT4_SUPER_MAGIC) {
        println("[EXT4] Invalid superblock magic");
        return -1;
    }
    
    /* 计算块大小 */
    fs->block_size = 1024 << fs->sb.s_log_block_size;
    fs->cluster_size = fs->block_size;
    fs->inode_size = fs->sb.s_inode_size;
    if (fs->inode_size == 0) {
        fs->inode_size = 128;
    }
    
    /* 计算块组数 */
    uint64_t blocks_count = fs->sb.s_blocks_count_lo |
                            ((uint64_t)fs->sb.s_blocks_count_hi << 32);
    fs->groups_count = (blocks_count + fs->sb.s_blocks_per_group - 1) /
                       fs->sb.s_blocks_per_group;
    
    fs->inodes_per_group = fs->sb.s_inodes_per_group;
    fs->blocks_per_group = fs->sb.s_blocks_per_group;
    
    print("[EXT4] Block size: ");
    print_hex(fs->block_size);
    println("");
    print("[EXT4] Groups: ");
    print_hex(fs->groups_count);
    println("");
    print("[EXT4] Volume: ");
    for (int i = 0; i < 16 && fs->sb.s_volume_name[i]; i++) {
        putchar(fs->sb.s_volume_name[i]);
    }
    println("");
    
    return 0;
}

/* 读取块组描述符 */
static int read_group_descs(struct ext4_fs *fs) {
    size_t gd_size = sizeof(struct ext4_group_desc);
    size_t gd_blocks = (fs->groups_count * gd_size + fs->block_size - 1) /
                       fs->block_size;
    
    fs->group_descs = kmalloc(gd_blocks * fs->block_size);
    if (fs->group_descs == NULL) {
        return -1;
    }
    
    /* 块组描述符在超级块之后 */
    uint32_t gd_block = (EXT4_SB_OFFSET + sizeof(struct ext4_super_block) + 
                         fs->block_size - 1) / fs->block_size;
    
    for (size_t i = 0; i < gd_blocks; i++) {
        if (fs->read_block(fs->device, gd_block + i,
                          (uint8_t *)fs->group_descs + i * fs->block_size) != 0) {
            kfree(fs->group_descs);
            return -1;
        }
    }
    
    return 0;
}

/* 挂载 EXT4 文件系统 */
int ext4_mount(struct ext4_fs *fs, void *device,
               int (*read_block)(void *, uint64_t, void *),
               int (*write_block)(void *, uint64_t, const void *)) {
    fs->device = device;
    fs->read_block = read_block;
    fs->write_block = write_block;
    
    /* 读取超级块 */
    if (read_superblock(fs) != 0) {
        return -1;
    }
    
    /* 读取块组描述符 */
    if (read_group_descs(fs) != 0) {
        return -1;
    }
    
    println("[EXT4] Mounted successfully");
    return 0;
}

/* 卸载 EXT4 */
int ext4_unmount(struct ext4_fs *fs) {
    if (fs->group_descs != NULL) {
        kfree(fs->group_descs);
        fs->group_descs = NULL;
    }
    return 0;
}

/* 获取 Inode 所在的块组 */
static uint32_t inode_to_group(struct ext4_fs *fs, uint32_t ino) {
    return (ino - 1) / fs->inodes_per_group;
}

/* 获取 Inode 在组内的索引 */
static uint32_t inode_to_index(struct ext4_fs *fs, uint32_t ino) {
    return (ino - 1) % fs->inodes_per_group;
}

/* 读取 Inode */
int ext4_read_inode(struct ext4_fs *fs, uint32_t ino, struct ext4_inode *inode) {
    if (ino < 1) {
        return -1;
    }
    
    uint32_t group = inode_to_group(fs, ino);
    uint32_t index = inode_to_index(fs, ino);
    
    if (group >= fs->groups_count) {
        return -1;
    }
    
    /* 获取块组描述符 */
    struct ext4_group_desc *gd = &fs->group_descs[group];
    uint64_t inode_table = gd->bg_inode_table_lo |
                           ((uint64_t)gd->bg_inode_table_hi << 32);
    
    /* 计算 Inode 所在的块和偏移 */
    uint64_t inode_offset = index * fs->inode_size;
    uint64_t block = inode_table + inode_offset / fs->block_size;
    uint64_t offset = inode_offset % fs->block_size;
    
    /* 读取块 */
    uint8_t *buf = kmalloc(fs->block_size);
    if (buf == NULL) {
        return -1;
    }
    
    if (fs->read_block(fs->device, block, buf) != 0) {
        kfree(buf);
        return -1;
    }
    
    /* 复制 Inode */
    for (size_t i = 0; i < sizeof(struct ext4_inode) && i < fs->inode_size; i++) {
        ((uint8_t *)inode)[i] = buf[offset + i];
    }
    
    kfree(buf);
    return 0;
}

/* 写入 Inode */
int ext4_write_inode(struct ext4_fs *fs, uint32_t ino, struct ext4_inode *inode) {
    if (ino < 1 || fs->write_block == NULL) {
        return -1;
    }
    
    uint32_t group = inode_to_group(fs, ino);
    uint32_t index = inode_to_index(fs, ino);
    
    if (group >= fs->groups_count) {
        return -1;
    }
    
    struct ext4_group_desc *gd = &fs->group_descs[group];
    uint64_t inode_table = gd->bg_inode_table_lo |
                           ((uint64_t)gd->bg_inode_table_hi << 32);
    
    uint64_t inode_offset = index * fs->inode_size;
    uint64_t block = inode_table + inode_offset / fs->block_size;
    uint64_t offset = inode_offset % fs->block_size;
    
    uint8_t *buf = kmalloc(fs->block_size);
    if (buf == NULL) {
        return -1;
    }
    
    /* 读取原块 */
    if (fs->read_block(fs->device, block, buf) != 0) {
        kfree(buf);
        return -1;
    }
    
    /* 修改 Inode */
    for (size_t i = 0; i < sizeof(struct ext4_inode) && i < fs->inode_size; i++) {
        buf[offset + i] = ((uint8_t *)inode)[i];
    }
    
    /* 写回 */
    int ret = fs->write_block(fs->device, block, buf);
    kfree(buf);
    
    return ret;
}

/* 读取文件数据 (简化实现) */
ssize_t ext4_read_file(struct ext4_fs *fs, struct ext4_inode *inode,
                       void *buf, size_t offset, size_t size) {
    uint64_t file_size = inode->i_size_lo | ((uint64_t)inode->i_size_hi << 32);
    
    if (offset >= file_size) {
        return 0;
    }
    
    if (offset + size > file_size) {
        size = file_size - offset;
    }
    
    /* 检查是否使用 Extent */
    if (inode->i_flags & 0x80000) {  /* EXT4_EXTENTS_FL */
        /* Extent 树 (简化: 只处理直接块) */
        print("[EXT4] Extent-based file");
    }
    
    /* 传统块指针 */
    uint32_t *blocks = inode->i_data.legacy.i_block;
    
    /* 计算起始块和偏移 */
    uint32_t first_block = offset / fs->block_size;
    uint32_t block_offset = offset % fs->block_size;
    uint32_t blocks_to_read = (block_offset + size + fs->block_size - 1) /
                              fs->block_size;
    
    uint8_t *block_buf = kmalloc(fs->block_size);
    if (block_buf == NULL) {
        return -1;
    }
    
    size_t read_bytes = 0;
    
    for (uint32_t i = 0; i < blocks_to_read && first_block + i < 12; i++) {
        uint32_t block_num = blocks[first_block + i];
        if (block_num == 0) {
            /* 稀疏文件: 返回零 */
            for (size_t j = 0; j < fs->block_size; j++) {
                block_buf[j] = 0;
            }
        } else {
            if (fs->read_block(fs->device, block_num, block_buf) != 0) {
                break;
            }
        }
        
        /* 计算复制范围 */
        size_t copy_start = (i == 0) ? block_offset : 0;
        size_t copy_end = fs->block_size;
        size_t copy_size = copy_end - copy_start;
        if (copy_size > size - read_bytes) {
            copy_size = size - read_bytes;
        }
        
        for (size_t j = 0; j < copy_size; j++) {
            ((uint8_t *)buf)[read_bytes + j] = block_buf[copy_start + j];
        }
        
        read_bytes += copy_size;
    }
    
    kfree(block_buf);
    return read_bytes;
}

/* 查找目录项 */
int ext4_lookup(struct ext4_fs *fs, uint32_t dir_ino, const char *name,
                uint32_t *result_ino) {
    struct ext4_inode dir_inode;
    
    if (ext4_read_inode(fs, dir_ino, &dir_inode) != 0) {
        return -1;
    }
    
    /* 检查是否为目录 */
    if ((dir_inode.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
        return -1;
    }
    
    /* 计算名称长度 */
    int name_len = 0;
    while (name[name_len]) name_len++;
    
    /* 读取目录数据 */
    uint8_t *dir_buf = kmalloc(fs->block_size);
    if (dir_buf == NULL) {
        return -1;
    }
    
    /* 遍历目录项 */
    uint64_t dir_size = dir_inode.i_size_lo;
    size_t offset = 0;
    
    while (offset < dir_size) {
        if (ext4_read_file(fs, &dir_inode, dir_buf, offset, fs->block_size) <= 0) {
            break;
        }
        
        struct ext4_dir_entry *entry = (struct ext4_dir_entry *)dir_buf;
        size_t pos = 0;
        
        while (pos < fs->block_size) {
            if (entry->rec_len == 0) {
                break;
            }
            
            if (entry->inode != 0 && entry->name_len == name_len) {
                /* 比较名称 */
                int match = 1;
                for (int i = 0; i < name_len; i++) {
                    if (entry->name[i] != name[i]) {
                        match = 0;
                        break;
                    }
                }
                
                if (match) {
                    *result_ino = entry->inode;
                    kfree(dir_buf);
                    return 0;
                }
            }
            
            pos += entry->rec_len;
            entry = (struct ext4_dir_entry *)((uint8_t *)entry + entry->rec_len);
        }
        
        offset += fs->block_size;
    }
    
    kfree(dir_buf);
    return -1;  /* 未找到 */
}

/* ========== EXT4 高级增强功能 ========== */

/* === 扩展属性支持 === */

/* 读取扩展属性 (简化版) */
int ext4_get_xattr(struct ext4_fs *fs, uint32_t ino, const char *name,
                   void *value, size_t *size) {
    struct ext4_inode inode;
    if (ext4_read_inode(fs, ino, &inode) != 0) return -1;
    
    /* 检查是否有扩展属性 */
    if (inode.i_file_acl_lo == 0) {
        return -1;  /* 无扩展属性 */
    }
    
    uint64_t acl_block = inode.i_file_acl_lo;
    
    uint8_t *block_buf = kmalloc(fs->block_size);
    if (block_buf == NULL) return -1;
    
    if (fs->read_block(fs->device, acl_block, block_buf) != 0) {
        kfree(block_buf);
        return -1;
    }
    
    /* 简化实现: 查找属性 */
    int name_len = 0;
    while (name[name_len]) name_len++;
    
    /* 扫描属性块 */
    size_t offset = 32;  /* 跳过头部 */
    while (offset < fs->block_size - 16) {
        uint8_t *entry = block_buf + offset;
        uint8_t entry_name_len = entry[0];
        uint32_t entry_value_size = *(uint32_t *)(entry + 4);
        
        if (entry_name_len == name_len && entry_name_len > 0) {
            if (memcmp(entry + 8, name, name_len) == 0) {
                /* 找到属性 */
                if (size != NULL) {
                    *size = entry_value_size;
                }
                if (value != NULL && size != NULL) {
                    size_t copy_size = entry_value_size < *size ? entry_value_size : *size;
                    memcpy(value, entry + 8 + name_len, copy_size);
                }
                kfree(block_buf);
                return 0;
            }
        }
        
        offset += 8 + entry_name_len + entry_value_size;
    }
    
    kfree(block_buf);
    return -1;
}

/* === 目录索引 (HTree) === */

/* HTree 哈希函数 */
static uint32_t ext4_htree_hash(const char *name, int len) {
    uint32_t hash = 0;
    for (int i = 0; i < len; i++) {
        hash = hash * 31 + (uint8_t)name[i];
    }
    return hash;
}

/* HTree 查找 (简化版) */
int ext4_htree_lookup(struct ext4_fs *fs, uint32_t dir_ino, const char *name,
                      uint32_t *result_ino) {
    struct ext4_inode dir_inode;
    if (ext4_read_inode(fs, dir_ino, &dir_inode) != 0) return -1;
    
    /* 检查是否有 HTree 索引 */
    if (!(dir_inode.i_flags & 0x1000)) {
        /* 回退到线性搜索 */
        return ext4_lookup(fs, dir_ino, name, result_ino);
    }
    
    int name_len = 0;
    while (name[name_len]) name_len++;
    (void)ext4_htree_hash(name, name_len);
    
    /* 简化: 回退到线性搜索 */
    return ext4_lookup(fs, dir_ino, name, result_ino);
}

/* === 文件写入 (内部版本) === */

/* 分配块 */
static uint64_t ext4_alloc_block_internal(struct ext4_fs *fs, uint32_t group) {
    if (group >= fs->groups_count) return 0;
    
    uint8_t *block_buf = kmalloc(fs->block_size);
    if (block_buf == NULL) return 0;
    
    /* 读取块位图 */
    uint64_t bitmap_block = fs->group_descs[group].bg_block_bitmap_lo;
    if (fs->read_block(fs->device, bitmap_block, block_buf) != 0) {
        kfree(block_buf);
        return 0;
    }
    
    /* 查找空闲位 */
    for (uint32_t byte = 0; byte < fs->block_size; byte++) {
        if (block_buf[byte] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (!(block_buf[byte] & (1 << bit))) {
                    uint32_t block = group * fs->blocks_per_group + 
                                    byte * 8 + bit;
                    
                    block_buf[byte] |= (1 << bit);
                    fs->write_block(fs->device, bitmap_block, block_buf);
                    
                    kfree(block_buf);
                    return block;
                }
            }
        }
    }
    
    kfree(block_buf);
    return 0;
}

/* 写文件内部版本 */
static int ext4_write_file_internal(struct ext4_fs *fs, struct ext4_inode *inode,
                    uint64_t offset, const void *buf, size_t size) {
    if (fs == NULL || inode == NULL || buf == NULL) return -1;
    
    uint8_t *block_buf = kmalloc(fs->block_size);
    if (block_buf == NULL) return -1;
    
    size_t written = 0;
    
    while (written < size) {
        uint64_t block_offset = offset + written;
        uint32_t block_idx = block_offset / fs->block_size;
        uint32_t offset_in_block = block_offset % fs->block_size;
        
        /* 获取或分配块 */
        uint64_t phys_block = 0;
        if (block_idx < 12) {
            phys_block = inode->i_data.legacy.i_block[block_idx];
            if (phys_block == 0) {
                phys_block = ext4_alloc_block_internal(fs, 0);
                inode->i_data.legacy.i_block[block_idx] = phys_block;
            }
        }
        
        if (phys_block == 0) {
            kfree(block_buf);
            return written > 0 ? written : -1;
        }
        
        /* 读取-修改-写入 */
        if (offset_in_block != 0 || size - written < fs->block_size) {
            if (fs->read_block(fs->device, phys_block, block_buf) != 0) {
                memset(block_buf, 0, fs->block_size);
            }
        }
        
        uint32_t write_size = fs->block_size - offset_in_block;
        if (write_size > size - written) {
            write_size = size - written;
        }
        
        memcpy(block_buf + offset_in_block, (uint8_t *)buf + written, write_size);
        
        if (fs->write_block(fs->device, phys_block, block_buf) != 0) {
            kfree(block_buf);
            return written > 0 ? written : -1;
        }
        
        written += write_size;
    }
    
    if (offset + written > inode->i_size_lo) {
        inode->i_size_lo = offset + written;
    }
    
    kfree(block_buf);
    return written;
}

/* === 创建文件 === */

/* 分配 inode */
static uint32_t ext4_alloc_inode_internal(struct ext4_fs *fs, uint32_t group) {
    if (group >= fs->groups_count) return 0;
    
    uint8_t *block_buf = kmalloc(fs->block_size);
    if (block_buf == NULL) return 0;
    
    uint64_t bitmap_block = fs->group_descs[group].bg_inode_bitmap_lo;
    if (fs->read_block(fs->device, bitmap_block, block_buf) != 0) {
        kfree(block_buf);
        return 0;
    }
    
    uint32_t inodes_per_group = fs->inodes_per_group;
    for (uint32_t byte = 0; byte < (inodes_per_group + 7) / 8; byte++) {
        if (block_buf[byte] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (!(block_buf[byte] & (1 << bit))) {
                    uint32_t ino = group * inodes_per_group + byte * 8 + bit + 1;
                    
                    block_buf[byte] |= (1 << bit);
                    fs->write_block(fs->device, bitmap_block, block_buf);
                    
                    kfree(block_buf);
                    return ino;
                }
            }
        }
    }
    
    kfree(block_buf);
    return 0;
}

/* 创建文件内部版本 */
__attribute__((unused)) static int ext4_create_internal(struct ext4_fs *fs, uint32_t dir_ino, const char *name,
                uint32_t mode, uint32_t *result_ino) {
    uint32_t new_ino = ext4_alloc_inode_internal(fs, 0);
    if (new_ino == 0) return -1;
    
    struct ext4_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = mode | 0x8000;  /* S_IFREG */
    inode.i_links_count = 1;
    inode.i_uid = 0;
    inode.i_gid = 0;
    inode.i_size_lo = 0;
    
    if (ext4_write_inode(fs, new_ino, &inode) != 0) {
        return -1;
    }
    
    struct ext4_inode dir_inode;
    if (ext4_read_inode(fs, dir_ino, &dir_inode) != 0) {
        return -1;
    }
    
    int name_len = 0;
    while (name[name_len]) name_len++;
    
    struct ext4_dir_entry new_entry;
    new_entry.inode = new_ino;
    new_entry.rec_len = 8 + name_len;
    new_entry.rec_len = (new_entry.rec_len + 3) & ~3;
    new_entry.name_len = name_len;
    new_entry.file_type = 1;
    memcpy(new_entry.name, name, name_len);
    
    uint64_t dir_size = dir_inode.i_size_lo;
    if (ext4_write_file_internal(fs, &dir_inode, dir_size, &new_entry, 
                        new_entry.rec_len) > 0) {
        if (result_ino != NULL) {
            *result_ino = new_ino;
        }
        return 0;
    }
    
    return -1;
}

/* === 删除文件 === */

int ext4_unlink(struct ext4_fs *fs, uint32_t dir_ino, const char *name) {
    struct ext4_inode dir_inode;
    if (ext4_read_inode(fs, dir_ino, &dir_inode) != 0) return -1;
    
    uint8_t *dir_buf = kmalloc(fs->block_size);
    if (dir_buf == NULL) return -1;
    
    int name_len = 0;
    while (name[name_len]) name_len++;
    
    uint64_t dir_size = dir_inode.i_size_lo;
    uint64_t offset = 0;
    
    while (offset < dir_size) {
        if (ext4_read_file(fs, &dir_inode, dir_buf, offset, fs->block_size) <= 0) {
            break;
        }
        
        struct ext4_dir_entry *entry = (struct ext4_dir_entry *)dir_buf;
        size_t pos = 0;
        
        while (pos < fs->block_size) {
            if (entry->rec_len == 0) break;
            
            if (entry->inode != 0 && entry->name_len == name_len) {
                int match = 1;
                for (int i = 0; i < name_len; i++) {
                    if (entry->name[i] != name[i]) {
                        match = 0;
                        break;
                    }
                }
                
                if (match) {
                    entry->inode = 0;
                    ext4_write_file_internal(fs, &dir_inode, offset + pos, 
                                   entry, entry->rec_len);
                    kfree(dir_buf);
                    return 0;
                }
            }
            
            pos += entry->rec_len;
            entry = (struct ext4_dir_entry *)((uint8_t *)entry + entry->rec_len);
        }
        
        offset += fs->block_size;
    }
    
    kfree(dir_buf);
    return -1;
}

/* === 文件系统检查 === */

int ext4_check_superblock(struct ext4_fs *fs) {
    if (fs->sb.s_magic != EXT4_SUPER_MAGIC) {
        println("[EXT4] Invalid magic");
        return -1;
    }
    
    if (fs->sb.s_state & 0x0002) {  /* EXT4_ERROR_FS */
        println("[EXT4] Filesystem has errors");
        return -1;
    }
    
    return 0;
}

/* === 统计信息 === */

void ext4_get_stats(struct ext4_fs *fs, uint64_t *total_blocks,
                    uint64_t *free_blocks, uint64_t *total_inodes,
                    uint64_t *free_inodes) {
    if (total_blocks) {
        *total_blocks = fs->sb.s_blocks_count_lo |
                       ((uint64_t)fs->sb.s_blocks_count_hi << 32);
    }
    if (free_blocks) {
        *free_blocks = fs->sb.s_free_blocks_count_lo |
                      ((uint64_t)fs->sb.s_free_blocks_count_hi << 32);
    }
    if (total_inodes) {
        *total_inodes = fs->sb.s_inodes_count;
    }
    if (free_inodes) {
        *free_inodes = fs->sb.s_free_inodes_count;
    }
}

/* 打印 EXT4 信息 */
void ext4_print_info(struct ext4_fs *fs) {
    println("╔══════════════════════════════════════════╗");
    println("║         EXT4 Filesystem Info             ║");
    println("╚══════════════════════════════════════════╝");
    
    print("Block size: ");
    print_dec(fs->block_size);
    println(" bytes");
    
    print("Block groups: ");
    print_dec(fs->groups_count);
    println("");
    
    print("Inodes per group: ");
    print_dec(fs->inodes_per_group);
    println("");
    
    uint64_t total, free;
    ext4_get_stats(fs, &total, &free, NULL, NULL);
    
    print("Total blocks: ");
    print_dec(total);
    println("");
    print("Free blocks: ");
    print_dec(free);
    println("");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * EXT4 Extent 树 - 替代传统间接块映射
 * 支持深度 0-2 的 extent 树遍历
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Extent 头部魔数 */
#define EXT4_EXT_MAGIC      0xA30E

/* Extent 结构 */
struct ext4_extent {
    uint32_t ee_block;      /* 逻辑块号 */
    uint16_t ee_len;        /* 长度 (块数) */
    uint16_t ee_start_hi;   /* 物理块号高16位 */
    uint32_t ee_start_lo;   /* 物理块号低32位 */
} __attribute__((packed));

/* Extent 索引结构 */
struct ext4_extent_idx {
    uint32_t ei_block;      /* 逻辑块号 */
    uint32_t ei_leaf_lo;    /* 子节点块号低32位 */
    uint16_t ei_leaf_hi;    /* 子节点块号高16位 */
    uint16_t ei_unused;
} __attribute__((packed));

/* Extent 头部 */
struct ext4_extent_header {
    uint16_t eh_magic;      /* 魔数 0xA30E */
    uint16_t eh_entries;    /* 条目数 */
    uint16_t eh_max;        /* 最大条目数 */
    uint16_t eh_depth;      /* 树深度 (0 = 叶子节点) */
    uint32_t eh_generation; /* 生成号 */
} __attribute__((packed));

/* 获取 extent 头部 */
static struct ext4_extent_header *ext4_get_extent_header(struct ext4_inode *inode) {
    return (struct ext4_extent_header *)inode->i_data.legacy.i_block;
}

/* 在叶子节点中查找 extent */
static int ext4_find_extent_in_leaf(struct ext4_fs *fs,
                                     struct ext4_extent_header *eh,
                                     uint32_t logical_block,
                                     uint64_t *physical_block,
                                     uint32_t *length) {
    if (eh->eh_magic != EXT4_EXT_MAGIC) return -1;
    
    int entries = eh->eh_entries;
    struct ext4_extent *ext = (struct ext4_extent *)(eh + 1);
    
    /* 线性搜索 (extent 数组通常很短) */
    for (int i = 0; i < entries; i++) {
        if (logical_block < ext[i].ee_block) {
            /* 逻辑块在此 extent 之前，未找到 */
            if (i == 0) return -1;
            /* 使用前一个 extent */
            i--;
            uint64_t start = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;
            uint32_t offset = logical_block - ext[i].ee_block;
            if (offset >= ext[i].ee_len) return -1;
            *physical_block = start + offset;
            *length = ext[i].ee_len - offset;
            return 0;
        }
        if (logical_block < ext[i].ee_block + ext[i].ee_len) {
            /* 命中 */
            uint64_t start = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;
            uint32_t offset = logical_block - ext[i].ee_block;
            *physical_block = start + offset;
            *length = ext[i].ee_len - offset;
            return 0;
        }
    }
    
    return -1;  /* 未找到 */
}

/* 递归遍历 extent 树 */
static int ext4_extent_tree_lookup(struct ext4_fs *fs,
                                    struct ext4_extent_header *eh,
                                    uint32_t logical_block,
                                    uint64_t *physical_block,
                                    uint32_t *length,
                                    int max_depth) {
    if (eh->eh_magic != EXT4_EXT_MAGIC) return -1;
    if (max_depth < 0) return -1;
    
    if (eh->eh_depth == 0) {
        /* 叶子节点: 直接查找 extent */
        return ext4_find_extent_in_leaf(fs, eh, logical_block, physical_block, length);
    }
    
    /* 内部节点: 查找索引条目 */
    int entries = eh->eh_entries;
    struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(eh + 1);
    
    /* 找到合适的索引 */
    int target = 0;
    for (int i = 0; i < entries; i++) {
        if (logical_block >= idx[i].ei_block) {
            target = i;
        } else {
            break;
        }
    }
    
    /* 读取子节点 */
    uint64_t child_block = ((uint64_t)idx[target].ei_leaf_hi << 32) | idx[target].ei_leaf_lo;
    uint8_t *buf = kmalloc(fs->block_size);
    if (buf == NULL) return -1;
    
    if (fs->read_block(fs->device, child_block, buf) != 0) {
        kfree(buf);
        return -1;
    }
    
    struct ext4_extent_header *child_eh = (struct ext4_extent_header *)buf;
    int ret = ext4_extent_tree_lookup(fs, child_eh, logical_block, physical_block, length, max_depth - 1);
    
    kfree(buf);
    return ret;
}

/* EXT4 extent 读取文件 - 替代直接块指针方式 */
ssize_t ext4_read_file_extent(struct ext4_fs *fs, struct ext4_inode *inode,
                               void *buf, size_t offset, size_t size) {
    uint64_t file_size = inode->i_size_lo | ((uint64_t)inode->i_size_hi << 32);
    
    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;
    
    struct ext4_extent_header *eh = ext4_get_extent_header(inode);
    if (eh->eh_magic != EXT4_EXT_MAGIC) {
        /* 不是 extent 格式，回退到传统方式 */
        return ext4_read_file(fs, inode, buf, offset, size);
    }
    
    uint8_t *block_buf = kmalloc(fs->block_size);
    if (block_buf == NULL) return -1;
    
    size_t read_bytes = 0;
    uint32_t first_block = offset / fs->block_size;
    uint32_t block_offset = offset % fs->block_size;
    
    while (read_bytes < size) {
        uint32_t logical_block = first_block + (read_bytes + block_offset) / fs->block_size;
        if (read_bytes > 0) block_offset = 0;
        
        uint64_t physical_block = 0;
        uint32_t extent_len = 0;
        
        if (ext4_extent_tree_lookup(fs, eh, logical_block, &physical_block, &extent_len, 2) != 0) {
            /* 稀疏文件或查找失败: 返回零 */
            memset(block_buf, 0, fs->block_size);
        } else {
            if (fs->read_block(fs->device, physical_block, block_buf) != 0) {
                break;
            }
        }
        
        size_t copy_start = (read_bytes == 0) ? block_offset : 0;
        size_t copy_size = fs->block_size - copy_start;
        if (copy_size > size - read_bytes) copy_size = size - read_bytes;
        
        memcpy((uint8_t *)buf + read_bytes, block_buf + copy_start, copy_size);
        read_bytes += copy_size;
    }
    
    kfree(block_buf);
    return read_bytes;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * EXT4 Htree 目录索引 - DX 树索引查找
 * 替代线性目录扫描，加速大目录查找
 * ═══════════════════════════════════════════════════════════════════════════ */

/* DX 根节点结构 */
struct dx_root {
    struct ext4_dir_entry dot;
    struct ext4_dir_entry dotdot;
    struct dx_root_info {
        uint32_t reserved_zero;
        uint8_t  hash_version;
        uint8_t  info_length;
        uint8_t  indirect_levels;
        uint8_t  unused_flags;
    } __attribute__((packed)) info;
    struct dx_entry {
        uint32_t hash;
        uint32_t block;
    } __attribute__((packed)) entries[0];
} __attribute__((packed));

/* DX 节点结构 */
struct dx_node {
    struct ext4_dir_entry fake;
    struct dx_entry entries[0];
} __attribute__((packed));

/* 半字节偏移哈希 (与 Linux ext4 兼容) */
#define DX_HASH_LEGACY      0
#define DX_HASH_HALF_MD4    1
#define DX_HASH_TEA         2

static uint32_t ext4_dx_hash_legacy(const char *name, int len) {
    uint32_t hash = 0;
    for (int i = 0; i < len; i++) {
        hash = (hash << 4) | (hash >> 28);
        hash ^= (uint8_t)name[i];
    }
    return hash;
}

/* DX 树查找 - 在索引中定位目录块 */
static int ext4_dx_lookup(struct ext4_fs *fs, uint32_t dir_ino,
                           const char *name, int name_len,
                           uint32_t *result_block) {
    struct ext4_inode dir_inode;
    if (ext4_read_inode(fs, dir_ino, &dir_inode) != 0) return -1;
    
    /* 读取第一个目录块 (DX 根节点) */
    uint8_t *buf = kmalloc(fs->block_size);
    if (buf == NULL) return -1;
    
    /* 获取第一个数据块 */
    uint64_t first_block = 0;
    if (dir_inode.i_flags & 0x80000) {
        /* Extent 格式 */
        struct ext4_extent_header *eh = ext4_get_extent_header(&dir_inode);
        uint32_t extent_len = 0;
        if (ext4_extent_tree_lookup(fs, eh, 0, &first_block, &extent_len, 2) != 0) {
            kfree(buf);
            return -1;
        }
    } else {
        first_block = dir_inode.i_data.legacy.i_block[0];
    }
    
    if (first_block == 0 || fs->read_block(fs->device, first_block, buf) != 0) {
        kfree(buf);
        return -1;
    }
    
    struct dx_root *root = (struct dx_root *)buf;
    
    /* 验证 DX 根节点 */
    if (root->dot.inode != dir_ino || root->info.hash_version > DX_HASH_TEA) {
        kfree(buf);
        return -1;  /* 不是有效的 DX 树 */
    }
    
    /* 计算文件名哈希 */
    uint32_t hash = ext4_dx_hash_legacy(name, name_len);
    uint32_t minor_hash = 0;
    
    int indirect_levels = root->info.indirect_levels;
    struct dx_entry *entries = root->entries;
    int num_entries = (fs->block_size - sizeof(struct dx_root)) / sizeof(struct dx_entry);
    
    /* 逐层查找 */
    for (int level = 0; level <= indirect_levels; level++) {
        /* 在当前节点的条目中二分查找 */
        int low = 0, high = 0;
        /* 计算有效条目数 */
        while (high < num_entries && entries[high].block != 0) high++;
        if (high == 0) { kfree(buf); return -1; }
        high--;
        
        while (low < high) {
            int mid = (low + high + 1) / 2;
            if (entries[mid].hash <= hash) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        
        *result_block = entries[low].block;
        
        /* 如果不是最后一层，读取子节点 */
        if (level < indirect_levels) {
            if (fs->read_block(fs->device, *result_block, buf) != 0) {
                kfree(buf);
                return -1;
            }
            struct dx_node *node = (struct dx_node *)buf;
            entries = node->entries;
            num_entries = (fs->block_size - 12) / sizeof(struct dx_entry);  /* 减去 fake dirent */
        }
    }
    
    (void)minor_hash;
    kfree(buf);
    return 0;
}

/* HTree 查找 - 完整实现 */
int ext4_htree_lookup_full(struct ext4_fs *fs, uint32_t dir_ino, const char *name,
                            uint32_t *result_ino) {
    int name_len = 0;
    while (name[name_len]) name_len++;
    
    /* 尝试 DX 树索引查找 */
    uint32_t target_block = 0;
    if (ext4_dx_lookup(fs, dir_ino, name, name_len, &target_block) == 0) {
        /* 在目标块中线性搜索 */
        uint8_t *buf = kmalloc(fs->block_size);
        if (buf == NULL) return ext4_lookup(fs, dir_ino, name, result_ino);
        
        if (fs->read_block(fs->device, target_block, buf) != 0) {
            kfree(buf);
            return ext4_lookup(fs, dir_ino, name, result_ino);
        }
        
        struct ext4_dir_entry *entry = (struct ext4_dir_entry *)buf;
        size_t pos = 0;
        
        while (pos < fs->block_size) {
            if (entry->rec_len == 0) break;
            if (entry->inode != 0 && entry->name_len == name_len) {
                int match = 1;
                for (int i = 0; i < name_len; i++) {
                    if (entry->name[i] != name[i]) { match = 0; break; }
                }
                if (match) {
                    *result_ino = entry->inode;
                    kfree(buf);
                    return 0;
                }
            }
            pos += entry->rec_len;
            entry = (struct ext4_dir_entry *)((uint8_t *)entry + entry->rec_len);
        }
        
        kfree(buf);
    }
    
    /* 回退到线性搜索 */
    return ext4_lookup(fs, dir_ino, name, result_ino);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * EXT4 简化日志 (JBD2) - 写前日志
 * 实现 descriptor → data → commit 块流程
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 日志块类型 */
#define JBD2_DESCRIPTOR_BLOCK   1
#define JBD2_COMMIT_BLOCK       2
#define JBD2_SUPERBLOCK_V1      3
#define JBD2_SUPERBLOCK_V2      4
#define JBD2_REVOKE_BLOCK       5

/* 日志魔数 */
#define JBD2_MAGIC_NUMBER       0xC03B3998

/* 日志头部 */
struct journal_header_s {
    uint32_t h_magic;
    uint32_t h_blocktype;
    uint32_t h_sequence;
} __attribute__((packed));

/* 日志描述符块标签 */
struct journal_block_tag_s {
    uint32_t t_blocknr;         /* 数据块号 */
    uint32_t t_flags;
} __attribute__((packed));

/* 日志超级块 */
struct journal_superblock_s {
    struct journal_header_s s_header;
    uint32_t s_blocksize;
    uint32_t s_maxlen;
    uint32_t s_first;
    uint32_t s_sequence;
    uint32_t s_start;
    uint32_t s_errno;
} __attribute__((packed));

/* 简化日志状态 */
#define JOURNAL_MAX_TXN_BLOCKS  32

struct ext4_journal {
    uint64_t journal_inode;     /* 日志 inode 号 */
    uint64_t first_block;       /* 日志起始块 */
    uint32_t total_blocks;      /* 日志总块数 */
    uint32_t sequence;          /* 当前序列号 */
    uint32_t head;              /* 日志头位置 */
    uint32_t tail;              /* 日志尾位置 */
    int enabled;                /* 是否启用 */
    struct ext4_fs *fs;
};

/* 全局日志实例 */
static struct ext4_journal ext4_journal;

/* 初始化日志 */
static int ext4_journal_init(struct ext4_fs *fs) {
    memset(&ext4_journal, 0, sizeof(ext4_journal));
    ext4_journal.fs = fs;
    
    /* 检查超级块是否指定了日志 inode */
    if (fs->sb.s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) {
        ext4_journal.journal_inode = fs->sb.s_journal_inum;
        ext4_journal.enabled = 1;
        ext4_journal.sequence = 1;
        
        /* 读取日志 inode 获取日志块范围 */
        if (ext4_journal.journal_inode > 0) {
            struct ext4_inode journal_inode;
            if (ext4_read_inode(fs, ext4_journal.journal_inode, &journal_inode) == 0) {
                /* 获取日志的第一个块 */
                if (journal_inode.i_flags & 0x80000) {
                    struct ext4_extent_header *eh = ext4_get_extent_header(&journal_inode);
                    uint32_t extent_len = 0;
                    ext4_extent_tree_lookup(fs, eh, 0, &ext4_journal.first_block, &extent_len, 2);
                } else {
                    ext4_journal.first_block = journal_inode.i_data.legacy.i_block[0];
                }
                /* 估算日志大小 */
                uint64_t j_size = journal_inode.i_size_lo | ((uint64_t)journal_inode.i_size_hi << 32);
                ext4_journal.total_blocks = j_size / fs->block_size;
                ext4_journal.head = 0;
                ext4_journal.tail = 0;
            }
        }
    }
    
    return 0;
}

/* 日志: 写入描述符块 */
static int ext4_journal_write_descriptor(struct ext4_fs *fs,
                                          uint32_t sequence,
                                          const uint32_t *block_list,
                                          int num_blocks) {
    if (!ext4_journal.enabled || ext4_journal.total_blocks == 0) return -1;
    
    uint8_t *buf = kmalloc(fs->block_size);
    if (buf == NULL) return -1;
    memset(buf, 0, fs->block_size);
    
    /* 写入头部 */
    struct journal_header_s *header = (struct journal_header_s *)buf;
    header->h_magic = JBD2_MAGIC_NUMBER;
    header->h_blocktype = JBD2_DESCRIPTOR_BLOCK;
    header->h_sequence = sequence;
    
    /* 写入块标签 */
    struct journal_block_tag_s *tag = (struct journal_block_tag_s *)(header + 1);
    for (int i = 0; i < num_blocks && 
         (uint8_t *)(tag + 1) < buf + fs->block_size - sizeof(struct journal_block_tag_s); i++) {
        tag->t_blocknr = block_list[i];
        tag->t_flags = 0;
        tag++;
    }
    
    /* 写入日志块 */
    uint64_t journal_block = ext4_journal.first_block + ext4_journal.head;
    int ret = fs->write_block(fs->device, journal_block, buf);
    
    ext4_journal.head = (ext4_journal.head + 1) % ext4_journal.total_blocks;
    kfree(buf);
    return ret;
}

/* 日志: 写入提交块 */
static int ext4_journal_write_commit(struct ext4_fs *fs, uint32_t sequence) {
    if (!ext4_journal.enabled || ext4_journal.total_blocks == 0) return -1;
    
    uint8_t *buf = kmalloc(fs->block_size);
    if (buf == NULL) return -1;
    memset(buf, 0, fs->block_size);
    
    struct journal_header_s *header = (struct journal_header_s *)buf;
    header->h_magic = JBD2_MAGIC_NUMBER;
    header->h_blocktype = JBD2_COMMIT_BLOCK;
    header->h_sequence = sequence;
    
    uint64_t journal_block = ext4_journal.first_block + ext4_journal.head;
    int ret = fs->write_block(fs->device, journal_block, buf);
    
    ext4_journal.head = (ext4_journal.head + 1) % ext4_journal.total_blocks;
    ext4_journal.sequence++;
    kfree(buf);
    return ret;
}

/* 日志: 事务提交 - descriptor → data → commit */
int ext4_journal_commit_transaction(struct ext4_fs *fs,
                                     const uint32_t *block_list,
                                     const void **data_list,
                                     int num_blocks) {
    if (!ext4_journal.enabled) return 0;
    if (num_blocks > JOURNAL_MAX_TXN_BLOCKS) return -1;
    
    uint32_t sequence = ext4_journal.sequence;
    
    /* 1. 写入描述符块 */
    if (ext4_journal_write_descriptor(fs, sequence, block_list, num_blocks) != 0) {
        return -1;
    }
    
    /* 2. 写入数据块到日志 */
    for (int i = 0; i < num_blocks; i++) {
        uint64_t journal_block = ext4_journal.first_block + ext4_journal.head;
        if (fs->write_block(fs->device, journal_block, data_list[i]) != 0) {
            return -1;
        }
        ext4_journal.head = (ext4_journal.head + 1) % ext4_journal.total_blocks;
    }
    
    /* 3. 写入提交块 */
    if (ext4_journal_write_commit(fs, sequence) != 0) {
        return -1;
    }
    
    /* 4. 写入实际数据块到文件系统 */
    for (int i = 0; i < num_blocks; i++) {
        if (fs->write_block(fs->device, block_list[i], data_list[i]) != 0) {
            /* 数据写入失败，但日志已提交，可以恢复 */
            return -1;
        }
    }
    
    return 0;
}

/* 日志: 挂载时恢复 */
int ext4_journal_recover(struct ext4_fs *fs) {
    if (!ext4_journal.enabled || ext4_journal.total_blocks == 0) return 0;
    
    uint8_t *buf = kmalloc(fs->block_size);
    if (buf == NULL) return -1;
    
    /* 扫描日志，查找未完成的事务 */
    for (uint32_t i = 0; i < ext4_journal.total_blocks; i++) {
        uint64_t block = ext4_journal.first_block + i;
        if (fs->read_block(fs->device, block, buf) != 0) continue;
        
        struct journal_header_s *header = (struct journal_header_s *)buf;
        if (header->h_magic != JBD2_MAGIC_NUMBER) continue;
        
        if (header->h_blocktype == JBD2_DESCRIPTOR_BLOCK) {
            /* 找到描述符，检查是否有对应的提交块 */
            uint32_t seq = header->h_sequence;
            int num_tags = 0;
            struct journal_block_tag_s *tag = (struct journal_block_tag_s *)(header + 1);
            uint32_t data_blocks[JOURNAL_MAX_TXN_BLOCKS];
            
            while ((uint8_t *)(tag + 1) < buf + fs->block_size - sizeof(struct journal_block_tag_s) &&
                   num_tags < JOURNAL_MAX_TXN_BLOCKS) {
                if (tag->t_blocknr == 0) break;
                data_blocks[num_tags++] = tag->t_blocknr;
                tag++;
            }
            
            /* 查找提交块 */
            int found_commit = 0;
            uint32_t commit_pos = i + num_tags + 1;
            if (commit_pos < ext4_journal.total_blocks) {
                uint64_t commit_block = ext4_journal.first_block + commit_pos;
                uint8_t *commit_buf = kmalloc(fs->block_size);
                if (commit_buf) {
                    if (fs->read_block(fs->device, commit_block, commit_buf) == 0) {
                        struct journal_header_s *ch = (struct journal_header_s *)commit_buf;
                        if (ch->h_magic == JBD2_MAGIC_NUMBER &&
                            ch->h_blocktype == JBD2_COMMIT_BLOCK &&
                            ch->h_sequence == seq) {
                            found_commit = 1;
                        }
                    }
                    kfree(commit_buf);
                }
            }
            
            if (found_commit) {
                /* 有提交块: 重放事务 (将日志数据写入实际位置) */
                for (int j = 0; j < num_tags; j++) {
                    uint64_t data_block = ext4_journal.first_block + i + 1 + j;
                    uint8_t *data_buf = kmalloc(fs->block_size);
                    if (data_buf) {
                        if (fs->read_block(fs->device, data_block, data_buf) == 0) {
                            fs->write_block(fs->device, data_blocks[j], data_buf);
                        }
                        kfree(data_buf);
                    }
                }
            }
            /* 无提交块: 丢弃不完整事务 */
            
            i += num_tags + 1;  /* 跳过已处理的数据块和提交块 */
        }
    }
    
    kfree(buf);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * EXT4 VFS 接口 - ext4_get_ops()
 * 返回 struct vfs_ops* 供 VFS 挂载使用
 * ═══════════════════════════════════════════════════════════════════════════ */

/* EXT4 全局文件系统实例 */
static struct ext4_fs ext4_global_fs;

/* EXT4 文件私有数据 */
struct ext4_file_data {
    uint32_t inode_num;
    struct ext4_inode inode;
    uint64_t file_size;
    uint8_t *block_buf;
};

/* EXT4 VFS open */
static int ext4_vfs_open(struct vfs_file *file, const char *path, uint32_t mode) {
    if (path == NULL) return -1;
    
    /* 根目录 */
    if (strcmp(path, "/") == 0 || path[0] == '\0') {
        struct ext4_file_data *fd = kmalloc(sizeof(struct ext4_file_data));
        if (fd == NULL) return -1;
        memset(fd, 0, sizeof(*fd));
        fd->inode_num = 2;  /* EXT4 根目录 inode */
        ext4_read_inode(&ext4_global_fs, 2, &fd->inode);
        fd->file_size = fd->inode.i_size_lo | ((uint64_t)fd->inode.i_size_hi << 32);
        fd->block_buf = kmalloc(ext4_global_fs.block_size);
        file->fs_data = fd;
        return 0;
    }
    
    /* 路径解析: 逐级查找 */
    uint32_t current_ino = 2;  /* 从根目录开始 */
    char component[256];
    const char *p = path;
    
    while (*p == '/') p++;
    
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < 255) component[i++] = *p++;
        component[i] = '\0';
        if (*p == '/') p++;
        if (component[0] == '\0') continue;
        
        /* 在当前目录中查找 */
        uint32_t found_ino = 0;
        if (ext4_htree_lookup_full(&ext4_global_fs, current_ino, component, &found_ino) != 0) {
            /* 文件不存在 */
            if (mode & VFS_MODE_CREATE) {
                /* TODO: 创建文件 */
                return -1;
            }
            return -1;
        }
        current_ino = found_ino;
    }
    
    /* 读取 inode */
    struct ext4_file_data *fd = kmalloc(sizeof(struct ext4_file_data));
    if (fd == NULL) return -1;
    memset(fd, 0, sizeof(*fd));
    fd->inode_num = current_ino;
    
    if (ext4_read_inode(&ext4_global_fs, current_ino, &fd->inode) != 0) {
        kfree(fd);
        return -1;
    }
    
    fd->file_size = fd->inode.i_size_lo | ((uint64_t)fd->inode.i_size_hi << 32);
    fd->block_buf = kmalloc(ext4_global_fs.block_size);
    if (fd->block_buf == NULL) {
        kfree(fd);
        return -1;
    }
    
    file->fs_data = fd;
    return 0;
}

/* EXT4 VFS close */
static int ext4_vfs_close(struct vfs_file *file) {
    struct ext4_file_data *fd = (struct ext4_file_data *)file->fs_data;
    if (fd != NULL) {
        if (fd->block_buf != NULL) kfree(fd->block_buf);
        kfree(fd);
    }
    return 0;
}

/* EXT4 VFS read */
static ssize_t ext4_vfs_read(struct vfs_file *file, void *buf, size_t count) {
    struct ext4_file_data *fd = (struct ext4_file_data *)file->fs_data;
    if (fd == NULL || buf == NULL) return -1;
    if (count == 0) return 0;
    
    if (file->offset >= fd->file_size) return 0;
    if (file->offset + count > fd->file_size) {
        count = fd->file_size - file->offset;
    }
    
    /* 使用 extent 或传统方式读取 */
    ssize_t ret;
    if (fd->inode.i_flags & 0x80000) {
        ret = ext4_read_file_extent(&ext4_global_fs, &fd->inode, buf, file->offset, count);
    } else {
        ret = ext4_read_file(&ext4_global_fs, &fd->inode, buf, file->offset, count);
    }
    
    if (ret > 0) file->offset += ret;
    return ret;
}

/* EXT4 VFS write */
static ssize_t ext4_vfs_write(struct vfs_file *file, const void *buf, size_t count) {
    struct ext4_file_data *fd = (struct ext4_file_data *)file->fs_data;
    if (fd == NULL || buf == NULL) return -1;
    if (count == 0) return 0;
    
    ssize_t ret = ext4_write_file_internal(&ext4_global_fs, &fd->inode,
                                            file->offset, buf, count);
    if (ret > 0) {
        file->offset += ret;
        file->is_dirty = 1;
        if (file->offset > fd->file_size) {
            fd->file_size = file->offset;
        }
    }
    return ret;
}

/* EXT4 VFS stat */
static int ext4_vfs_stat(const char *path, struct vfs_stat *stat) {
    if (path == NULL || stat == NULL) return -1;
    
    /* 根目录 */
    if (strcmp(path, "/") == 0 || path[0] == '\0') {
        stat->type = VFS_TYPE_DIR;
        stat->size = 0;
        stat->mode = 0755;
        stat->uid = 0;
        stat->gid = 0;
        stat->inode = 2;
        return 0;
    }
    
    /* 路径解析 */
    uint32_t current_ino = 2;
    char component[256];
    const char *p = path;
    
    while (*p == '/') p++;
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < 255) component[i++] = *p++;
        component[i] = '\0';
        if (*p == '/') p++;
        if (component[0] == '\0') continue;
        
        uint32_t found_ino = 0;
        if (ext4_htree_lookup_full(&ext4_global_fs, current_ino, component, &found_ino) != 0) {
            return -1;
        }
        current_ino = found_ino;
    }
    
    struct ext4_inode inode;
    if (ext4_read_inode(&ext4_global_fs, current_ino, &inode) != 0) return -1;
    
    uint64_t file_size = inode.i_size_lo | ((uint64_t)inode.i_size_hi << 32);
    
    stat->inode = current_ino;
    stat->size = file_size;
    stat->uid = inode.i_uid;
    stat->gid = inode.i_gid;
    stat->mode = inode.i_mode & 0xFFF;
    stat->nlink = inode.i_links_count;
    
    if ((inode.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) {
        stat->type = VFS_TYPE_DIR;
    } else if ((inode.i_mode & EXT4_S_IFMT) == EXT4_S_IFREG) {
        stat->type = VFS_TYPE_FILE;
    } else if ((inode.i_mode & EXT4_S_IFMT) == EXT4_S_IFLNK) {
        stat->type = VFS_TYPE_SYMLINK;
    } else {
        stat->type = VFS_TYPE_FILE;
    }
    
    return 0;
}

/* EXT4 VFS readdir */
static int ext4_vfs_readdir(const char *path, struct vfs_dirent *dirent, int index) {
    if (path == NULL || dirent == NULL) return -1;
    
    /* 获取目录 inode */
    uint32_t dir_ino = 2;
    if (strcmp(path, "/") != 0 && path[0] != '\0') {
        char component[256];
        const char *p = path;
        while (*p == '/') p++;
        while (*p) {
            int i = 0;
            while (*p && *p != '/' && i < 255) component[i++] = *p++;
            component[i] = '\0';
            if (*p == '/') p++;
            if (component[0] == '\0') continue;
            
            uint32_t found_ino = 0;
            if (ext4_htree_lookup_full(&ext4_global_fs, dir_ino, component, &found_ino) != 0) {
                return -1;
            }
            dir_ino = found_ino;
        }
    }
    
    struct ext4_inode dir_inode;
    if (ext4_read_inode(&ext4_global_fs, dir_ino, &dir_inode) != 0) return -1;
    
    uint64_t dir_size = dir_inode.i_size_lo;
    uint8_t *dir_buf = kmalloc(ext4_global_fs.block_size);
    if (dir_buf == NULL) return -1;
    
    int current = 0;
    size_t offset = 0;
    
    while (offset < dir_size) {
        ssize_t read_ret;
        if (dir_inode.i_flags & 0x80000) {
            read_ret = ext4_read_file_extent(&ext4_global_fs, &dir_inode, dir_buf, offset, ext4_global_fs.block_size);
        } else {
            read_ret = ext4_read_file(&ext4_global_fs, &dir_inode, dir_buf, offset, ext4_global_fs.block_size);
        }
        if (read_ret <= 0) break;
        
        struct ext4_dir_entry *entry = (struct ext4_dir_entry *)dir_buf;
        size_t pos = 0;
        
        while (pos < (size_t)read_ret) {
            if (entry->rec_len == 0) break;
            
            if (entry->inode != 0) {
                if (current == index) {
                    int copy_len = entry->name_len < 255 ? entry->name_len : 255;
                    memcpy(dirent->name, entry->name, copy_len);
                    dirent->name[copy_len] = '\0';
                    dirent->inode = entry->inode;
                    
                    switch (entry->file_type) {
                        case EXT4_FT_DIR: dirent->type = VFS_TYPE_DIR; break;
                        case EXT4_FT_REG_FILE: dirent->type = VFS_TYPE_FILE; break;
                        case EXT4_FT_SYMLINK: dirent->type = VFS_TYPE_SYMLINK; break;
                        default: dirent->type = VFS_TYPE_FILE; break;
                    }
                    
                    kfree(dir_buf);
                    return 0;
                }
                current++;
            }
            
            pos += entry->rec_len;
            entry = (struct ext4_dir_entry *)((uint8_t *)entry + entry->rec_len);
        }
        
        offset += ext4_global_fs.block_size;
    }
    
    kfree(dir_buf);
    return -1;
}

/* EXT4 VFS 操作表 */
static struct vfs_ops ext4_vfs_ops = {
    .open = ext4_vfs_open,
    .close = ext4_vfs_close,
    .read = ext4_vfs_read,
    .write = ext4_vfs_write,
    .stat = ext4_vfs_stat,
    .readdir = ext4_vfs_readdir,
    .mkdir = NULL,
    .rmdir = NULL,
    .unlink = NULL,
    .rename = NULL,
};

/* 获取 EXT4 VFS 操作接口 */
struct vfs_ops *ext4_get_ops(void) {
    return &ext4_vfs_ops;
}
