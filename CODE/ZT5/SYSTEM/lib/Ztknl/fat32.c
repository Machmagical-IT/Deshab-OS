#include "fat32.h"
#include "vfs.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "ktypes.h"
#include "task.h"

/* ========== 调试和验证宏 ========== */
#define FAT32_DEBUG 0  /* 生产环境关闭调试输出 */

#if FAT32_DEBUG
#define FAT32_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        println("[FAT32-ASSERT] " msg); \
        print("  File: "); println(__FILE__); \
        print("  Line: "); print_dec(__LINE__); println(""); \
    } \
} while(0)
#else
#define FAT32_ASSERT(cond, msg) ((void)0)
#endif

/* 有效簇号检查 */
#define IS_VALID_CLUSTER(c) ((c) >= 2 && (c) < fat32.total_clusters + 2)

/* EOF 簇检查 */
#define IS_EOF_CLUSTER(c) ((c) >= 0x0FFFFFF8)

/* 空闲簇检查 */
#define IS_FREE_CLUSTER(c) ((c) == 0)

/* 磁盘读取/写入函数 (需要由平台提供) */
extern int disk_read_sector(uint64_t lba, void *buf);
extern int disk_write_sector(uint64_t lba, const void *buf);

/* FAT32 文件系统实例 */
static struct fat32_fs fat32;

/* FAT 缓存锁 (保护静态缓冲区，SMP 安全) */
static volatile int fat_cache_lock = 0;

/* ========== 前向声明 ========== */
static uint32_t fat32_read_fat_entry(uint32_t cluster);
static int fat32_read_cluster(uint32_t cluster, void *buf);
static uint32_t fat32_next_cluster(uint32_t cluster);
static int fat32_write_fat_entry(uint32_t cluster, uint32_t value);
static int fat32_flush_fat_cache(void);
void fat32_cluster_bitmap_free(uint32_t cluster);

/* ========== FAT32 长文件名(LFN)支持 ========== */

/* LFN 目录项结构 */
struct lfn_dirent {
    uint8_t  seq;           /* 序号 (bit 6 = 最后一个条目, bit 5 = 0) */
    uint16_t name1[5];      /* 字符 1-5 (UTF-16LE) */
    uint8_t  attr;          /* 属性 = 0x0F */
    uint8_t  type;          /* 类型 = 0 */
    uint8_t  checksum;      /* 短文件名校验和 */
    uint16_t name2[6];      /* 字符 6-11 (UTF-16LE) */
    uint16_t reserved;      /* 保留 */
    uint16_t name3[2];      /* 字符 12-13 (UTF-16LE) */
} __attribute__((packed));

/* Forward declarations */
static int fat32_lfn_verify_checksum(const struct lfn_dirent *lfn, const struct fat32_dirent *short_entry);

/* 计算短文件名校验和 */
static uint8_t lfn_checksum(const uint8_t *short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i];
    }
    return sum;
}

/* UTF-16LE 转换为 ASCII (简化版，忽略非ASCII字符) */
static void utf16_to_ascii(char *dst, const uint16_t *src, int len) {
    for (int i = 0; i < len; i++) {
        uint16_t c = src[i];
        if (c == 0 || c == 0xFFFF) {
            dst[i] = '\0';
            return;
        }
        /* 简化处理：只保留低字节 */
        dst[i] = (c < 0x80) ? (char)c : '?';
    }
    dst[len] = '\0';
}

/* ASCII 转换为 UTF-16LE */
static void ascii_to_utf16(uint16_t *dst, const char *src, int len) {
    for (int i = 0; i < len; i++) {
        dst[i] = (uint8_t)src[i];
    }
}

/* 从 LFN 条目提取文件名 */
static void lfn_extract_name(char *name, int max_len, struct lfn_dirent *lfn) {
    char part1[6], part2[7], part3[3];
    
    utf16_to_ascii(part1, lfn->name1, 5);
    utf16_to_ascii(part2, lfn->name2, 6);
    utf16_to_ascii(part3, lfn->name3, 2);
    
    int pos = 0;
    for (int i = 0; part1[i] && pos < max_len - 1; i++) name[pos++] = part1[i];
    for (int i = 0; part2[i] && pos < max_len - 1; i++) name[pos++] = part2[i];
    for (int i = 0; part3[i] && pos < max_len - 1; i++) name[pos++] = part3[i];
    name[pos] = '\0';
}

/* 生成短文件名 (8.3格式) */
static void generate_short_name(const char *long_name, char *short_name, int *counter) {
    int i, j;
    
    /* 清空 */
    for (i = 0; i < 11; i++) short_name[i] = ' ';
    
    /* 复制主名 (最多6字符，留空间给序号) */
    for (i = 0, j = 0; long_name[i] && j < 6; i++) {
        char c = long_name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c >= 'A' && c <= 'Z') short_name[j++] = c;
        else if (c >= '0' && c <= '9') short_name[j++] = c;
        else if (c == '.') break;  /* 扩展名 */
    }
    
    /* 添加序号 */
    if (*counter > 1) {
        char num[8];
        int len = 0;
        int n = *counter;
        while (n > 0) { num[len++] = '0' + (n % 10); n /= 10; }
        /* 添加 ~ 和数字 */
        short_name[j++] = '~';
        while (len > 0 && j < 8) short_name[j++] = num[--len];
    }
    
    /* 查找扩展名 */
    const char *ext = long_name;
    while (*ext && *ext != '.') ext++;
    if (*ext == '.') {
        ext++;
        for (i = 8, j = 0; ext[j] && i < 11; j++) {
            char c = ext[j];
            if (c >= 'a' && c <= 'z') c -= 32;
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                short_name[i++] = c;
            }
        }
    }
    
    (*counter)++;
}

/* 写入 LFN 目录项 */
static int write_lfn_entries(uint8_t *buf, int offset, const char *name, 
                              uint8_t checksum, int max_entries) {
    int name_len = 0;
    while (name[name_len]) name_len++;
    
    /* 计算需要的 LFN 条目数 */
    int num_lfn = (name_len + 12) / 13;
    if (num_lfn > max_entries) num_lfn = max_entries;
    
    /* 从后向前写入 LFN 条目 */
    for (int entry = num_lfn - 1; entry >= 0; entry--) {
        struct lfn_dirent *lfn = (struct lfn_dirent *)(buf + offset + entry * 32);
        
        memset(lfn, 0, sizeof(*lfn));
        
        /* 序号 */
        lfn->seq = entry + 1;
        if (entry == num_lfn - 1) lfn->seq |= 0x40;  /* 最后一个条目标志 */
        
        lfn->attr = FAT_ATTR_LFN;
        lfn->type = 0;
        lfn->checksum = checksum;
        
        /* 填充字符 */
        int start = entry * 13;
        
        /* name1: 字符 0-4 */
        for (int i = 0; i < 5 && start + i < name_len; i++) {
            lfn->name1[i] = (uint8_t)name[start + i];
        }
        /* name2: 字符 5-10 */
        for (int i = 0; i < 6 && start + 5 + i < name_len; i++) {
            lfn->name2[i] = (uint8_t)name[start + 5 + i];
        }
        /* name3: 字符 11-12 */
        for (int i = 0; i < 2 && start + 11 + i < name_len; i++) {
            lfn->name3[i] = (uint8_t)name[start + 11 + i];
        }
    }
    
    return num_lfn;
}

/* ========== FAT32 写入支持 ========== */

/* FAT表缓存 - 统一用于读写操作 */
#define FAT_BUFFER_SIZE 8192
static uint8_t fat_buffer[FAT_BUFFER_SIZE];
static uint64_t fat_cached_sector = ~0ULL;
static int fat_buffer_dirty = 0;

/* 刷新 FAT 缓存 (内部函数，需要在锁保护下调用) */
static int fat32_flush_fat_cache_internal(void) {
    if (fat_buffer_dirty && fat_cached_sector != ~0ULL) {
        /* 写入所有 FAT 表副本 (镜像) */
        for (uint8_t i = 0; i < fat32.num_fats; i++) {
            uint64_t lba = fat32.partition_start + fat_cached_sector + (uint32_t)i * fat32.sectors_per_fat;
            if (disk_write_sector(lba, fat_buffer) != 0) {
                return -1;
            }
        }
        
        fat_buffer_dirty = 0;
    }
    return 0;
}

/* 刷新 FAT 缓存 (公共接口，自动获取锁) */
static int fat32_flush_fat_cache(void) {
    while (__sync_lock_test_and_set(&fat_cache_lock, 1)) {
        asm volatile("pause");
    }
    
    int ret = fat32_flush_fat_cache_internal();
    
    __sync_lock_release(&fat_cache_lock);
    
    return ret;
}

/* 写入 FAT 表项 */
static int fat32_write_fat_entry(uint32_t cluster, uint32_t value) {
    /* 检查簇号有效性 */
    if (cluster < 2 || cluster >= fat32.total_clusters + 2) {
        println("[FAT32] write_fat_entry: Invalid cluster number");
        return -1;
    }
    
    /* 使用 64 位计算防止溢出 */
    uint64_t fat_offset = (uint64_t)cluster * 4;
    
    /* 检查 FAT 表边界 */
    uint64_t fat_size = (uint64_t)fat32.sectors_per_fat * fat32.bytes_per_sector;
    if (fat_offset + 4 > fat_size) {
        println("[FAT32] write_fat_entry: FAT offset out of bounds");
        return -1;
    }
    
    uint32_t fat_sector = fat32.fat_start + (uint32_t)(fat_offset / fat32.bytes_per_sector);
    uint32_t offset = (uint32_t)(fat_offset % fat32.bytes_per_sector);
    
    /* 检查偏移是否会导致越界 (每个 FAT 项 4 字节) */
    if (offset + 4 > fat32.bytes_per_sector) {
        println("[FAT32] write_fat_entry: Offset exceeds sector size");
        return -1;
    }
    
    /* 获取锁保护静态变量 (与读操作共享同一把锁) */
    while (__sync_lock_test_and_set(&fat_cache_lock, 1)) {
        asm volatile("pause");
    }
    
    /* 如果缓存的是不同的扇区，先刷新 */
    if (fat_cached_sector != fat_sector) {
        if (fat32_flush_fat_cache_internal() != 0) {
            __sync_lock_release(&fat_cache_lock);
            return -1;
        }
        /* 读取新扇区到缓存 - 使用统一的缓冲区 */
        if (disk_read_sector(fat32.partition_start + fat_sector, fat_buffer) != 0) {
            __sync_lock_release(&fat_cache_lock);
            return -1;
        }
        fat_cached_sector = fat_sector;
    }
    
    /* 写入 FAT 表项 (保留高 4 位) */
    uint32_t old_entry = *(uint32_t *)(fat_buffer + offset);
    *(uint32_t *)(fat_buffer + offset) = (old_entry & 0xF0000000) | (value & 0x0FFFFFFF);
    fat_buffer_dirty = 1;
    
    __sync_lock_release(&fat_cache_lock);
    
    return 0;
}

/* 写入簇 */
static int fat32_write_cluster(uint32_t cluster, const void *buf) {
    /* 检查簇号有效性 (FAT32 簇从 2 开始) */
    if (cluster < 2 || cluster >= fat32.total_clusters + 2) {
        return -1;
    }
    
    /* 优先使用写回缓存 (延迟写入) */
    if (fat32_writeback_add(cluster, buf) == 0) {
        return 0;
    }
    
    /* 写回缓存不可用，直接写入磁盘 */
    uint64_t lba = fat32.data_start + (uint64_t)(cluster - 2) * fat32.sectors_per_cluster;
    lba += fat32.partition_start;
    
    for (uint32_t i = 0; i < fat32.sectors_per_cluster; i++) {
        if (disk_write_sector(lba + i, (const uint8_t *)buf + i * fat32.bytes_per_sector) != 0) {
            return -1;
        }
    }
    return 0;
}

/* 分配新簇 */
static uint32_t fat32_alloc_cluster(void) {
    static uint32_t last_alloc_hint = 2;  /* 上次分配位置提示，优化搜索 */
    
    /* 验证文件系统已初始化 */
    if (fat32.total_clusters == 0) {
        println("[FAT32] alloc_cluster: Filesystem not initialized");
        return 0;
    }
    
    /* 从上次分配位置开始搜索 (优化性能) */
    uint32_t start = last_alloc_hint;
    uint32_t searched = 0;
    
    while (searched < fat32.total_clusters) {
        uint32_t cluster = start + searched;
        if (cluster >= fat32.total_clusters + 2) {
            cluster = 2 + (cluster - fat32.total_clusters - 2);
        }
        
        if (cluster >= 2) {
            uint32_t entry = fat32_read_fat_entry(cluster);
            if (entry == 0) {
                /* 找到空闲簇，标记为 EOF */
                if (fat32_write_fat_entry(cluster, 0x0FFFFFFF) != 0) {
                    println("[FAT32] alloc_cluster: Failed to write FAT entry");
                    return 0;
                }
                if (fat32_flush_fat_cache() != 0) {
                    println("[FAT32] alloc_cluster: Failed to flush FAT cache");
                    return 0;
                }
                
                /* 更新提示位置 */
                last_alloc_hint = cluster + 1;
                if (last_alloc_hint >= fat32.total_clusters + 2) {
                    last_alloc_hint = 2;
                }
                
                print("[FAT32] alloc_cluster: Allocated cluster ");
                print_dec(cluster);
                println("");
                
                return cluster;
            }
        }
        searched++;
    }
    
    println("[FAT32] alloc_cluster: No free clusters available");
    return 0;  /* 没有空闲簇 */
}

/* 释放簇链 - 从指定簇开始释放整条链 */
static void fat32_free_cluster_chain(uint32_t start_cluster) {
    if (start_cluster == 0 || !IS_VALID_CLUSTER(start_cluster)) {
        return;
    }
    
    uint32_t cluster = start_cluster;
    uint32_t max_iterations = fat32.total_clusters + 2;  /* 防止无限循环 */
    uint32_t iterations = 0;
    
    while (IS_VALID_CLUSTER(cluster) && iterations < max_iterations) {
        uint32_t next = fat32_read_fat_entry(cluster);
        
        /* 标记当前簇为空闲 */
        fat32_write_fat_entry(cluster, 0);
        
        /* 更新位图 */
        fat32_cluster_bitmap_free(cluster);
        
        cluster = next;
        iterations++;
    }
    
    /* 刷新 FAT 缓存 */
    fat32_flush_fat_cache();
    
    print("[FAT32] free_cluster_chain: Freed chain starting at ");
    print_dec(start_cluster);
    print(" (");
    print_dec(iterations);
    println(" clusters)");
}

/* 扩展文件簇链 */
static int fat32_extend_cluster_chain(uint32_t last_cluster, uint32_t *new_cluster) {
    /* 验证参数 */
    if (new_cluster == NULL) {
        println("[FAT32] extend_chain: NULL output pointer");
        return -1;
    }
    
    /* 分配新簇 */
    uint32_t cluster = fat32_alloc_cluster();
    if (cluster == 0) {
        println("[FAT32] extend_chain: Failed to allocate new cluster");
        return -1;
    }
    
    /* 链接到现有链 (如果不是第一个簇) */
    if (last_cluster != 0 && IS_VALID_CLUSTER(last_cluster)) {
        if (fat32_write_fat_entry(last_cluster, cluster) != 0) {
            println("[FAT32] extend_chain: Failed to link cluster");
            /* 尝试释放新分配的簇 */
            fat32_write_fat_entry(cluster, 0);
            fat32_flush_fat_cache();
            return -1;
        }
    }
    
    *new_cluster = cluster;
    
    /* 刷新 FAT 表 */
    if (fat32_flush_fat_cache() != 0) {
        println("[FAT32] extend_chain: Warning - FAT flush failed");
        /* 不返回错误，因为簇已分配成功 */
    }
    
    return 0;
}

/* 创建目录项 (简化版: 只创建 8.3 格式) */
static int fat32_create_dirent(uint32_t parent_cluster, const char *name, 
                                uint32_t start_cluster, uint32_t file_size, 
                                uint8_t attr, uint32_t *out_cluster, uint32_t *out_offset) {
    /* 转换文件名为 8.3 格式 */
    char fat_name[11];
    int i;
    
    /* 清空 */
    for (i = 0; i < 11; i++) fat_name[i] = ' ';
    
    /* 复制主名 (最多 8 字符) */
    const char *dot = name;
    for (i = 0; i < 8 && *dot && *dot != '.'; i++, dot++) {
        char c = *dot;
        if (c >= 'a' && c <= 'z') c -= 32;  /* 转大写 */
        fat_name[i] = c;
    }
    
    /* 跳过点号，复制扩展名 */
    if (*dot == '.') {
        dot++;
        for (i = 8; i < 11 && *dot; i++, dot++) {
            char c = *dot;
            if (c >= 'a' && c <= 'z') c -= 32;
            fat_name[i] = c;
        }
    }
    
    /* 在父目录中查找空闲槽位 */
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) {
        println("[FAT32] create_dirent: Failed to allocate buffer");
        return -1;
    }
    
    print("[FAT32] create_dirent: parent_cluster=");
    print_dec(parent_cluster);
    print(", name=");
    println((char *)name);
    
    uint32_t cluster = parent_cluster;
    uint32_t prev_cluster = 0;
    
    while (cluster != 0 && cluster < 0x0FFFFFF8) {
        fat32_read_cluster(cluster, buf);
        struct fat32_dirent *dirent = (struct fat32_dirent *)buf;
        
        for (uint32_t j = 0; j < fat32.bytes_per_cluster / sizeof(struct fat32_dirent); j++) {
            /* 找到空闲槽位 (0x00 表示目录结束，0xE5 表示已删除) */
            int was_end_of_dir = (dirent[j].name[0] == 0x00);
            
            if (was_end_of_dir || dirent[j].name[0] == 0xE5) {
                /* 清空目录项 */
                for (int k = 0; k < sizeof(struct fat32_dirent); k++) {
                    ((uint8_t *)&dirent[j])[k] = 0;
                }
                
                /* 填写目录项 */
                for (i = 0; i < 11; i++) {
                    dirent[j].name[i] = fat_name[i];
                }
                dirent[j].attr = attr;
                dirent[j].cluster_high = (start_cluster >> 16) & 0xFFFF;
                dirent[j].cluster_low = start_cluster & 0xFFFF;
                dirent[j].file_size = file_size;
                
                /* 如果原来是目录结束标记，需要标记下一个为目录结束 */
                if (was_end_of_dir) {
                    if (j + 1 < fat32.bytes_per_cluster / sizeof(struct fat32_dirent)) {
                        dirent[j + 1].name[0] = 0x00;
                    }
                }
                
                /* 写回簇 */
                if (fat32_write_cluster(cluster, buf) != 0) {
                    kfree(buf);
                    return -1;
                }
                
                if (out_cluster) *out_cluster = cluster;
                if (out_offset) *out_offset = j * sizeof(struct fat32_dirent);
                
                kfree(buf);
                return 0;
            }
        }
        
        prev_cluster = cluster;
        cluster = fat32_next_cluster(cluster);
    }
    
    /* 需要扩展父目录 */
    print("[FAT32] create_dirent: No free slot, extending parent directory (prev_cluster=");
    print_dec(prev_cluster);
    println(")");
    
    if (prev_cluster != 0) {
        uint32_t new_cluster;
        if (fat32_extend_cluster_chain(prev_cluster, &new_cluster) != 0) {
            println("[FAT32] create_dirent: Failed to extend cluster chain");
            kfree(buf);
            return -1;
        }
        print("[FAT32] create_dirent: Extended to new cluster ");
        print_dec(new_cluster);
        println("");
        
        /* 清空新簇 */
        for (uint32_t i = 0; i < fat32.bytes_per_cluster; i++) {
            buf[i] = 0;
        }
        
        /* 在新簇开头创建目录项 */
        struct fat32_dirent *dirent = (struct fat32_dirent *)buf;
        for (i = 0; i < 11; i++) {
            dirent[0].name[i] = fat_name[i];
        }
        dirent[0].attr = attr;
        dirent[0].cluster_high = (start_cluster >> 16) & 0xFFFF;
        dirent[0].cluster_low = start_cluster & 0xFFFF;
        dirent[0].file_size = file_size;
        dirent[1].name[0] = 0x00;  /* 目录结束 */
        
        if (fat32_write_cluster(new_cluster, buf) != 0) {
            kfree(buf);
            return -1;
        }
        
        if (out_cluster) *out_cluster = new_cluster;
        if (out_offset) *out_offset = 0;
        
        kfree(buf);
        return 0;
    } else {
        println("[FAT32] create_dirent: Error - prev_cluster is 0, cannot extend directory");
    }
    
    kfree(buf);
    return -1;
}

/* 更新目录项的文件大小 */
static int fat32_update_file_size(uint32_t dir_cluster, const char *name, uint32_t new_size) {
    /* 转换文件名 */
    char fat_name[11];
    int i;
    for (i = 0; i < 11; i++) fat_name[i] = ' ';
    
    const char *dot = name;
    for (i = 0; i < 8 && *dot && *dot != '.'; i++, dot++) {
        char c = *dot;
        if (c >= 'a' && c <= 'z') c -= 32;
        fat_name[i] = c;
    }
    if (*dot == '.') {
        dot++;
        for (i = 8; i < 11 && *dot; i++, dot++) {
            char c = *dot;
            if (c >= 'a' && c <= 'z') c -= 32;
            fat_name[i] = c;
        }
    }
    
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) return -1;
    
    uint32_t cluster = dir_cluster;
    
    while (cluster != 0 && cluster < 0x0FFFFFF8) {
        fat32_read_cluster(cluster, buf);
        struct fat32_dirent *dirent = (struct fat32_dirent *)buf;
        
        for (uint32_t j = 0; j < fat32.bytes_per_cluster / sizeof(struct fat32_dirent); j++) {
            if (dirent[j].name[0] == 0x00) {
                kfree(buf);
                return -1;  /* 文件不存在 */
            }
            
            if (dirent[j].name[0] == 0xE5 || (dirent[j].attr & FAT_ATTR_LFN)) {
                continue;
            }
            
            /* 比较名称 */
            int match = 1;
            for (i = 0; i < 11 && match; i++) {
                if (dirent[j].name[i] != fat_name[i]) {
                    match = 0;
                }
            }
            
            if (match) {
                dirent[j].file_size = new_size;
                if (fat32_write_cluster(cluster, buf) != 0) {
                    kfree(buf);
                    return -1;
                }
                kfree(buf);
                return 0;
            }
        }
        
        cluster = fat32_next_cluster(cluster);
    }
    
    kfree(buf);
    return -1;
}

/* 读取 FAT 表项 */
static uint32_t fat32_read_fat_entry(uint32_t cluster) {
    /* 检查簇号有效性 */
    if (cluster < 2) {
        return 0x0FFFFFFF;  /* 返回 EOF 标记 */
    }
    
    /* 使用 64 位计算防止溢出 */
    uint64_t fat_offset = (uint64_t)cluster * 4;
    
    /* 检查 FAT 表边界 */
    uint64_t fat_size = (uint64_t)fat32.sectors_per_fat * fat32.bytes_per_sector;
    if (fat_offset + 4 > fat_size) {
        return 0x0FFFFFFF;  /* 返回 EOF 标记 */
    }
    
    uint32_t fat_sector = fat32.fat_start + (uint32_t)(fat_offset / fat32.bytes_per_sector);
    uint32_t offset = (uint32_t)(fat_offset % fat32.bytes_per_sector);

    /* 检查扇区大小是否超出缓冲区容量 */
    if (fat32.bytes_per_sector > FAT_BUFFER_SIZE) {
        println("[FAT32] Error: Sector size exceeds buffer capacity");
        return 0x0FFFFFFF;  /* 返回 EOF 标记 */
    }

    /* 检查偏移是否会导致越界 */
    if (offset + 4 > fat32.bytes_per_sector) {
        return 0x0FFFFFFF;  /* 返回 EOF 标记 */
    }

    /* 优先使用增强型 FAT 缓存 (多路组相联) */
    {
        uint32_t cached_data[FAT_CACHE_SECTOR_U32];
        if (fat32_fat_cache_read(fat_sector, cached_data) == 0) {
            return cached_data[offset / 4] & 0x0FFFFFFF;
        }
    }

    /* 回退到原有单扇区缓存 */
    /* 获取锁保护静态变量 - 使用全局统一的缓冲区 */
    while (__sync_lock_test_and_set(&fat_cache_lock, 1)) {
        asm volatile("pause");
    }

    if (fat_cached_sector != fat_sector) {
        if (fat_buffer_dirty) {
            fat32_flush_fat_cache_internal();
        }
        disk_read_sector(fat32.partition_start + fat_sector, fat_buffer);
        fat_cached_sector = fat_sector;
    }

    uint32_t entry = *(uint32_t *)(fat_buffer + offset);
    
    __sync_lock_release(&fat_cache_lock);
    
    return entry & 0x0FFFFFFF;
}

/* 读取簇 */
static int fat32_read_cluster(uint32_t cluster, void *buf) {
    /* 检查簇号有效性 (FAT32 簇从 2 开始) */
    if (cluster < 2 || cluster >= fat32.total_clusters + 2) {
        return -1;
    }

    uint64_t lba = fat32.data_start + (uint64_t)(cluster - 2) * fat32.sectors_per_cluster;
    lba += fat32.partition_start;

    for (uint32_t i = 0; i < fat32.sectors_per_cluster; i++) {
        int retries = 15;
        while (retries > 0) {
            if (disk_read_sector(lba + i, (uint8_t *)buf + i * fat32.bytes_per_sector) == 0) {
                break;
            }
            retries--;
            /* 延迟后重试 - 较长延迟提高成功率 */
            for (volatile int d = 0; d < 100000; d++);
        }
        if (retries == 0) {
            return -1;
        }
    }
    return 0;
}

/* FAT32 特殊簇号常量 */
#define FAT32_CLUSTER_EOF       0x0FFFFFFF   /* 文件结束标记 */
#define FAT32_CLUSTER_ERROR     0xFFFFFFFF   /* 错误标记 */
#define FAT32_CLUSTER_BAD       0x0FFFFFF7   /* 坏簇标记 */
#define FAT32_IS_VALID(c)       ((c) >= 2 && (c) < 0x0FFFFFF0)
#define FAT32_IS_EOF(c)         ((c) >= 0x0FFFFFF8)

/* 获取下一个簇
 * 返回值:
 *   2 ~ 0x0FFFFFF6: 有效的下一个簇号
 *   FAT32_CLUSTER_EOF (0x0FFFFFFF): 文件结束
 *   FAT32_CLUSTER_ERROR (0xFFFFFFFF): 读取错误
 */
static uint32_t fat32_next_cluster(uint32_t cluster) {
    uint32_t next = fat32_read_fat_entry(cluster);
    
    /* 检查簇号有效性 */
    if (next == 0) {
        /* FAT 条目为 0 表示空闲簇，不应该出现在链中 */
        return FAT32_CLUSTER_ERROR;
    }
    
    if (next == FAT32_CLUSTER_BAD) {
        /* 坏簇 */
        return FAT32_CLUSTER_ERROR;
    }
    
    if (FAT32_IS_EOF(next)) {
        return FAT32_CLUSTER_EOF;
    }
    
    /* 验证簇号在有效范围内 */
    if (next < 2 || next >= fat32.total_clusters + 2) {
        return FAT32_CLUSTER_ERROR;
    }
    
    return next;
}

/* 解析路径为目录链 */
/* 比较文件名 (支持长文件名和8.3格式，不区分大小写) */
static int compare_filename(const char *name, const char *target) {
    int i = 0;
    while (name[i] && target[i]) {
        char c1 = name[i];
        char c2 = target[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
        i++;
    }
    return name[i] == '\0' && target[i] == '\0';
}

/* 解析目录项获取完整文件名 (支持LFN) */
static int get_dirent_name(uint8_t *cluster_buf, int dirent_index, 
                            char *name_out, int max_len,
                            uint32_t *out_cluster, uint32_t *out_offset) {
    struct fat32_dirent *dirent = (struct fat32_dirent *)cluster_buf;
    int entries_per_cluster = fat32.bytes_per_cluster / sizeof(struct fat32_dirent);
    
    /* 检查是否为目录结束 */
    if (dirent[dirent_index].name[0] == 0x00) {
        return -1;
    }
    
    /* 跳过已删除项 */
    if (dirent[dirent_index].name[0] == 0xE5) {
        return -1;
    }
    
    /* 检查是否为长文件名条目 */
    if ((dirent[dirent_index].attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
        /* LFN 条目 - 向前查找并组合名称 */
        struct lfn_dirent *lfn = (struct lfn_dirent *)&dirent[dirent_index];
        
        /* 检查是否为最后一个 LFN 条目 */
        if (!(lfn->seq & 0x40)) {
            return -1;  /* 不是最后一个，继续搜索 */
        }
        
        int num_lfn = lfn->seq & 0x1F;
        char lfn_name[256] = {0};
        int name_pos = 0;
        
        /* 从后向前读取 LFN 条目 */
        for (int i = num_lfn - 1; i >= 0; i--) {
            int idx = dirent_index - i;
            if (idx < 0) break;  /* 超出簇边界 */
            
            lfn = (struct lfn_dirent *)&dirent[idx];
            if ((lfn->attr & FAT_ATTR_LFN) != FAT_ATTR_LFN) break;
            
            /* 提取名称部分 */
            char part[14];
            lfn_extract_name(part, 14, lfn);
            
            /* 添加到名称开头 */
            int part_len = 0;
            while (part[part_len]) part_len++;
            
            for (int j = name_pos + part_len - 1; j >= part_len; j--) {
                lfn_name[j] = lfn_name[j - part_len];
            }
            for (int j = 0; j < part_len; j++) {
                lfn_name[j] = part[j];
            }
            name_pos += part_len;
        }
        lfn_name[name_pos] = '\0';
        
        /* 复制输出名称 */
        int copy_len = name_pos < max_len - 1 ? name_pos : max_len - 1;
        for (int i = 0; i < copy_len; i++) {
            name_out[i] = lfn_name[i];
        }
        name_out[copy_len] = '\0';
        
        /* 查找对应的短文件名条目 */
        int short_idx = dirent_index + 1;
        if (short_idx < entries_per_cluster) {
            struct fat32_dirent *short_ent = &dirent[short_idx];
            /* LFN 校验和验证 */
            if (fat32_lfn_verify_checksum((const struct lfn_dirent *)&dirent[dirent_index], short_ent)) {
                if (out_cluster) *out_cluster = ((uint32_t)short_ent->cluster_high << 16) | short_ent->cluster_low;
                if (out_offset) *out_offset = short_idx * sizeof(struct fat32_dirent);
            } else {
                /* 校验和不匹配，使用短文件名条目但标记可能不一致 */
                if (out_cluster) *out_cluster = ((uint32_t)short_ent->cluster_high << 16) | short_ent->cluster_low;
                if (out_offset) *out_offset = short_idx * sizeof(struct fat32_dirent);
            }
        }
        
        return 0;
    } else {
        /* 8.3 短文件名 */
        char fat_name[12];
        int k;
        for (k = 0; k < 8 && dirent[dirent_index].name[k] != ' '; k++) {
            fat_name[k] = dirent[dirent_index].name[k];
        }
        if (dirent[dirent_index].name[8] != ' ') {
            fat_name[k++] = '.';
            for (int l = 8; l < 11 && dirent[dirent_index].name[l] != ' '; l++) {
                fat_name[k++] = dirent[dirent_index].name[l];
            }
        }
        fat_name[k] = '\0';
        
        /* 转换为小写 */
        for (k = 0; fat_name[k]; k++) {
            if (fat_name[k] >= 'A' && fat_name[k] <= 'Z') {
                fat_name[k] += 32;
            }
        }
        
        /* 复制输出 */
        for (k = 0; fat_name[k] && k < max_len - 1; k++) {
            name_out[k] = fat_name[k];
        }
        name_out[k] = '\0';
        
        if (out_cluster) *out_cluster = ((uint32_t)dirent[dirent_index].cluster_high << 16) | dirent[dirent_index].cluster_low;
        if (out_offset) *out_offset = dirent_index * sizeof(struct fat32_dirent);
        
        return 0;
    }
}

static int parse_path_attr(const char *path, uint32_t *clusters, uint8_t *last_attr, uint32_t *file_size, int max_depth);

static int parse_path(const char *path, uint32_t *clusters, int max_depth) {
    return parse_path_attr(path, clusters, NULL, NULL, max_depth);
}

static int parse_path_attr(const char *path, uint32_t *clusters, uint8_t *last_attr, uint32_t *file_size, int max_depth) {
    char component[256];
    int depth = 0;

    clusters[depth++] = fat32.root_cluster;



    if (strcmp(path, "/") == 0) {
        return depth;
    }

    const char *p = path;
    while (*p == '/') p++;

    while (*p && depth < max_depth - 1) {
        /* 提取路径组件 */
        int i = 0;
        while (*p && *p != '/' && i < 255) {
            component[i++] = *p++;
        }
        component[i] = '\0';
        if (*p == '/') p++;

        if (component[0] == '\0') continue;

        /* 在当前目录中查找 */
        uint32_t cluster = clusters[depth - 1];
        uint8_t *cluster_buf = kmalloc(fat32.bytes_per_cluster);
        if (cluster_buf == NULL) {
            println("[FAT32] parse_path: Failed to allocate cluster buffer");
            return -1;
        }
        int found = 0;

        while (cluster != 0 && cluster < 0x0FFFFFF8) {
            int read_ret = fat32_read_cluster(cluster, cluster_buf);
            if (read_ret != 0) {
                /* 重试读取 - ATA 可能暂时失败 */
                int retry;
                for (retry = 0; retry < 3; retry++) {
                    for (volatile int d = 0; d < 500000; d++);
                    read_ret = fat32_read_cluster(cluster, cluster_buf);
                    if (read_ret == 0) break;
                }
                if (read_ret != 0) {
                    kfree(cluster_buf);
                    return -1;
                }
            }

            struct fat32_dirent *dirent = (struct fat32_dirent *)cluster_buf;
            int entries_per_cluster = fat32.bytes_per_cluster / sizeof(struct fat32_dirent);
            
            for (int j = 0; j < entries_per_cluster; j++) {
                if (dirent[j].name[0] == 0x00) {
                    /* 目录结束 */
                    kfree(cluster_buf);
                    return -1;
                }

                if (dirent[j].name[0] == 0xE5) {
                    continue;
                }

                /* 获取文件名 (支持LFN) */
                char entry_name[256];
                uint32_t file_cluster = 0;
                
                if ((dirent[j].attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
                    struct lfn_dirent *lfn = (struct lfn_dirent *)&dirent[j];
                    if (!(lfn->seq & 0x40)) continue;
                    
                    int num_lfn = lfn->seq & 0x1F;
                    char lfn_name[256] = {0};
                    int name_pos = 0;
                    int lfn_ok = 1;
                    
                    uint32_t lfn_cluster = cluster;
                    uint8_t *lfn_buf = cluster_buf;
                    int lfn_buf_is_ours = 0;
                    
                    for (int k = num_lfn - 1; k >= 0; k--) {
                        int idx = j + (num_lfn - 1 - k);
                        uint8_t *active_buf = cluster_buf;
                        uint32_t active_entries = entries_per_cluster;
                        uint32_t active_cluster = cluster;
                        
                        if (idx >= entries_per_cluster) {
                            uint32_t next_cl = cluster;
                            int skip = idx / entries_per_cluster;
                            for (int s = 0; s <= skip; s++) {
                                if (s > 0) next_cl = fat32_next_cluster(next_cl);
                                if (next_cl == 0 || next_cl >= 0x0FFFFFF8) { lfn_ok = 0; break; }
                            }
                            if (!lfn_ok) break;
                            
                            if (next_cl != lfn_cluster) {
                                if (lfn_buf_is_ours && lfn_buf && lfn_buf != cluster_buf) kfree(lfn_buf);
                                lfn_buf = kmalloc(fat32.bytes_per_cluster);
                                if (!lfn_buf) { lfn_ok = 0; break; }
                                lfn_buf_is_ours = 1;
                                if (fat32_read_cluster(next_cl, lfn_buf) != 0) { kfree(lfn_buf); lfn_ok = 0; break; }
                                lfn_cluster = next_cl;
                            }
                            active_buf = lfn_buf;
                            active_entries = fat32.bytes_per_cluster / sizeof(struct fat32_dirent);
                            active_cluster = next_cl;
                            idx = idx % entries_per_cluster;
                        }
                        
                        struct fat32_dirent *act_dirent = (struct fat32_dirent *)active_buf;
                        if (idx >= (int)active_entries) { lfn_ok = 0; break; }
                        lfn = (struct lfn_dirent *)&act_dirent[idx];
                        if ((lfn->attr & FAT_ATTR_LFN) != FAT_ATTR_LFN) { lfn_ok = 0; break; }
                        
                        char part[14];
                        lfn_extract_name(part, 14, lfn);
                        
                        int part_len = 0;
                        while (part[part_len]) part_len++;
                        
                        for (int m = name_pos + part_len - 1; m >= part_len; m--) {
                            lfn_name[m] = lfn_name[m - part_len];
                        }
                        for (int m = 0; m < part_len; m++) {
                            lfn_name[m] = part[m];
                        }
                        name_pos += part_len;
                    }
                    
                    if (lfn_buf_is_ours && lfn_buf && lfn_buf != cluster_buf) kfree(lfn_buf);
                    
                    if (!lfn_ok) {
                        strcpy(entry_name, "");
                        file_cluster = 0;
                    } else {
                        lfn_name[name_pos] = '\0';
                        
                        int short_idx = j + num_lfn;
                        if (short_idx < entries_per_cluster) {
                            file_cluster = ((uint32_t)dirent[short_idx].cluster_high << 16) | 
                                           dirent[short_idx].cluster_low;
                        } else {
                            uint32_t next_cl = cluster;
                            int skip = short_idx / entries_per_cluster;
                            for (int s = 0; s <= skip; s++) {
                                if (s > 0) next_cl = fat32_next_cluster(next_cl);
                                if (next_cl == 0 || next_cl >= 0x0FFFFFF8) { file_cluster = 0; break; }
                            }
                            if (next_cl != 0 && next_cl < 0x0FFFFFF8) {
                                uint8_t *sbuf = kmalloc(fat32.bytes_per_cluster);
                                if (sbuf) {
                                    if (fat32_read_cluster(next_cl, sbuf) == 0) {
                                        struct fat32_dirent *sd = (struct fat32_dirent *)sbuf;
                                        int si = short_idx % entries_per_cluster;
                                        file_cluster = ((uint32_t)sd[si].cluster_high << 16) | sd[si].cluster_low;
                                    }
                                    kfree(sbuf);
                                }
                            }
                        }
                        
                        strcpy(entry_name, lfn_name);
                    }
                } else {
                    /* 8.3 短文件名 */
                    int k;
                    for (k = 0; k < 8 && dirent[j].name[k] != ' '; k++) {
                        entry_name[k] = dirent[j].name[k];
                    }
                    if (dirent[j].name[8] != ' ') {
                        entry_name[k++] = '.';
                        for (int l = 8; l < 11 && dirent[j].name[l] != ' '; l++) {
                            entry_name[k++] = dirent[j].name[l];
                        }
                    }
                    entry_name[k] = '\0';
                    
                    /* 转小写 */
                    for (k = 0; entry_name[k]; k++) {
                        if (entry_name[k] >= 'A' && entry_name[k] <= 'Z') {
                            entry_name[k] += 32;
                        }
                    }
                    
                    file_cluster = ((uint32_t)dirent[j].cluster_high << 16) | 
                                   dirent[j].cluster_low;
                }
                
                /* 比较文件名 */
                if (compare_filename(component, entry_name)) {
                    clusters[depth++] = file_cluster;
                    int short_idx = j;
                    uint8_t short_attr = dirent[j].attr;
                    uint32_t short_fsize = dirent[j].file_size;
                    if ((dirent[j].attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
                        int num_lfn_attr = ((struct lfn_dirent *)&dirent[j])->seq & 0x1F;
                        short_idx = j + num_lfn_attr;
                        if (short_idx < entries_per_cluster) {
                            short_attr = dirent[short_idx].attr;
                            short_fsize = dirent[short_idx].file_size;
                        } else {
                            uint32_t next_cl = cluster;
                            int skip = short_idx / entries_per_cluster;
                            for (int s = 0; s <= skip; s++) {
                                if (s > 0) next_cl = fat32_next_cluster(next_cl);
                                if (next_cl == 0 || next_cl >= 0x0FFFFFF8) break;
                            }
                            if (next_cl != 0 && next_cl < 0x0FFFFFF8) {
                                uint8_t *abuf = kmalloc(fat32.bytes_per_cluster);
                                if (abuf) {
                                    if (fat32_read_cluster(next_cl, abuf) == 0) {
                                        struct fat32_dirent *ad = (struct fat32_dirent *)abuf;
                                        int ai = short_idx % entries_per_cluster;
                                        short_attr = ad[ai].attr;
                                        short_fsize = ad[ai].file_size;
                                    }
                                    kfree(abuf);
                                }
                            }
                        }
                    }
                    if (last_attr) {
                        *last_attr = short_attr;
                    }
                    if (file_size) {
                        *file_size = short_fsize;
                    }
                    found = 1;
                    break;
                }
            }

            if (found) break;
            cluster = fat32_next_cluster(cluster);
        }

        kfree(cluster_buf);
        
        if (!found) {
            return -1;
        }
    }

    return depth;
}

/* FAT32 文件结构 */
struct fat32_file {
    uint32_t start_cluster;
    uint32_t current_cluster;
    uint32_t file_size;
    uint32_t offset_in_cluster;
    uint8_t *cluster_buffer;
    uint32_t parent_cluster;    /* 父目录簇，用于更新文件大小 */
    char filename[256];         /* 原始文件名 */
};

static int fat32_open(struct vfs_file *file, const char *path, uint32_t mode) {
    uint32_t clusters[32];
    int depth = parse_path(path, clusters, 32);

    /* 调试输出 */
    print("[FAT32] open: path=");
    print(path);
    print(", mode=");
    print_hex(mode);
    print(", depth=");
    print_dec(depth);
    println("");

    /* Special case: opening root directory "/" */
    if (strcmp(path, "/") == 0) {
        println("[FAT32] Opening root directory");
        file->offset = 0;
        /* Allocate a proper fat32_file structure for root directory */
        struct fat32_file *ff = kmalloc(sizeof(struct fat32_file));
        if (ff == NULL) {
            return -1;
        }
        /* Initialize for root directory */
        for (int i = 0; i < 256; i++) {
            ff->filename[i] = '\0';
        }
        ff->filename[0] = '/';
        ff->start_cluster = fat32.root_cluster;
        ff->current_cluster = fat32.root_cluster;
        ff->file_size = 0;  /* Directories don't have a fixed size */
        ff->offset_in_cluster = 0;
        ff->parent_cluster = 0;
        ff->cluster_buffer = kmalloc(fat32.bytes_per_cluster);
        if (ff->cluster_buffer == NULL) {
            kfree(ff);
            return -1;
        }
        /* Clear the buffer */
        for (uint32_t i = 0; i < fat32.bytes_per_cluster; i++) {
            ff->cluster_buffer[i] = 0;
        }
        file->fs_data = ff;
        return 0;
    }

    /* 获取文件名 */
    const char *name = path + strlen(path);
    while (name > path && *(name - 1) != '/') name--;

    /* 获取父目录簇 */
    uint32_t parent_cluster = fat32.root_cluster;
    int parent_valid = 0;  /* 标记父目录是否有效 */
    
    if (depth > 1) {
        /* 文件存在，父目录在 clusters[depth-2] */
        parent_cluster = clusters[depth - 2];
        parent_valid = 1;
        print("[FAT32] File exists, parent cluster: ");
        print_dec(parent_cluster);
        println("");
    } else if (depth == 1) {
        /* 文件在根目录且存在 */
        parent_cluster = fat32.root_cluster;
        parent_valid = 1;
        println("[FAT32] File in root directory (exists)");
    } else if (depth < 0) {
        /* 路径不存在，尝试解析父目录 */
        println("[FAT32] File not found, trying to find parent directory...");
        
        /* 复制路径并提取父目录 */
        char parent_path[256];
        strcpy(parent_path, path);
        char *last_slash = parent_path + strlen(parent_path);
        while (last_slash > parent_path && *(last_slash - 1) != '/') {
            last_slash--;
        }
        if (last_slash > parent_path) {
            *(last_slash - 1) = '\0';  /* 截断文件名 */
        }
        
        /* 处理根目录情况 */
        if (parent_path[0] == '\0' || (parent_path[0] == '/' && parent_path[1] == '\0')) {
            parent_cluster = fat32.root_cluster;
            parent_valid = 1;
            println("[FAT32] Parent is root directory");
        } else {
            /* 解析父目录 */
            print("[FAT32] Looking for parent directory: ");
            println(parent_path);
            
            int parent_depth = parse_path(parent_path, clusters, 32);
            print("[FAT32] Parent parse result: depth=");
            print_dec(parent_depth);
            println("");
            
            if (parent_depth > 0) {
                parent_cluster = clusters[parent_depth - 1];
                parent_valid = 1;
                print("[FAT32] Found parent directory, cluster: ");
                print_dec(parent_cluster);
                println("");
            } else {
                println("[FAT32] Error: Parent directory not found");
            }
        }
    } else {
        println("[FAT32] Warning: depth=0, using root cluster as fallback");
    }

    uint32_t cluster = 0;
    uint32_t file_size = 0;
    int file_exists = 0;

    if (depth > 0) {
        cluster = clusters[depth - 1];
        {
            uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
            if (buf == NULL) {
                return -1;
            }
            uint32_t search_cluster = parent_cluster;
            
            while (search_cluster != 0 && search_cluster < 0x0FFFFFF8) {
                fat32_read_cluster(search_cluster, buf);
                struct fat32_dirent *dirent = (struct fat32_dirent *)buf;

                for (uint32_t i = 0; i < fat32.bytes_per_cluster / sizeof(struct fat32_dirent); i++) {
                    if (dirent[i].name[0] == 0x00) break;
                    if (dirent[i].name[0] == 0xE5 || (dirent[i].attr & FAT_ATTR_LFN)) continue;

                    uint32_t entry_cluster = ((uint32_t)dirent[i].cluster_high << 16) | dirent[i].cluster_low;
                    if (cluster == 0) {
                        if (entry_cluster == 0 && dirent[i].file_size == 0) {
                            char short_name[13];
                            int si;
                            for (si = 0; si < 8 && dirent[i].name[si] != ' '; si++)
                                short_name[si] = dirent[i].name[si];
                            if (dirent[i].name[8] != ' ') {
                                short_name[si++] = '.';
                                for (int l = 8; l < 11 && dirent[i].name[l] != ' '; l++)
                                    short_name[si++] = dirent[i].name[l];
                            }
                            short_name[si] = '\0';
                            for (si = 0; short_name[si]; si++)
                                if (short_name[si] >= 'A' && short_name[si] <= 'Z')
                                    short_name[si] += 32;
                            if (compare_filename(short_name, name) == 0) {
                                file_size = 0;
                                file_exists = 1;
                                search_cluster = 0x0FFFFFFF;
                                break;
                            }
                        }
                    } else if (entry_cluster == cluster) {
                        file_size = dirent[i].file_size;
                        file_exists = 1;
                        search_cluster = 0x0FFFFFFF;
                        break;
                    }
                }

                if (search_cluster != 0x0FFFFFFF) {
                    search_cluster = fat32_next_cluster(search_cluster);
                }
            }
            kfree(buf);
        }
    }

    /* 如果文件不存在且需要写入，创建新文件 */
    if (!file_exists && (mode & VFS_MODE_WRITE)) {
        /* 检查父目录是否有效 */
        if (!parent_valid) {
            println("[FAT32] Error: Parent directory does not exist, cannot create file");
            return -1;
        }
        
        println("[FAT32] Creating new file...");
        print("  Name: ");
        println((char *)name);
        print("  Parent cluster: ");
        print_dec(parent_cluster);
        println("");
        
        /* 分配起始簇 */
        cluster = fat32_alloc_cluster();
        if (cluster == 0) {
            println("[FAT32] Error: Failed to allocate cluster");
            return -1;
        }
        print("  Allocated cluster: ");
        print_dec(cluster);
        println("");
        
        /* 清空新簇 */
        uint8_t *empty_buf = kmalloc(fat32.bytes_per_cluster);
        if (empty_buf == NULL) {
            println("[FAT32] Error: Failed to allocate memory for cluster buffer");
            fat32_write_fat_entry(cluster, 0);
            fat32_flush_fat_cache();
            return -1;
        }
        for (uint32_t i = 0; i < fat32.bytes_per_cluster; i++) {
            empty_buf[i] = 0;
        }
        fat32_write_cluster(cluster, empty_buf);
        kfree(empty_buf);
        
        /* 在父目录中创建目录项 */
        print("[FAT32] Creating directory entry in parent cluster ");
        print_dec(parent_cluster);
        println("...");
        
        if (fat32_create_dirent(parent_cluster, name, cluster, 0, FAT_ATTR_ARCHIVE, NULL, NULL) != 0) {
            println("[FAT32] Error: Failed to create directory entry");
            fat32_write_fat_entry(cluster, 0);  /* 释放簇 */
            fat32_flush_fat_cache();
            return -1;
        }
        println("[FAT32] Directory entry created successfully");
        
        file_size = 0;
        file_exists = 1;
    }

    /* 如果文件仍不存在，返回错误 */
    if (!file_exists) {
        return -1;
    }

    struct fat32_file *ff = kmalloc(sizeof(struct fat32_file));
    if (ff == NULL) {
        return -1;
    }

    ff->start_cluster = cluster;
    ff->current_cluster = cluster;
    ff->file_size = file_size;
    ff->offset_in_cluster = 0;
    ff->parent_cluster = parent_cluster;  /* 保存父目录簇 */
    
    /* 保存原始文件名 */
    {
        int i;
        for (i = 0; name[i] && i < 255; i++) {
            ff->filename[i] = name[i];
        }
        ff->filename[i] = '\0';
    }
    
    ff->cluster_buffer = kmalloc(fat32.bytes_per_cluster);

    if (ff->cluster_buffer == NULL) {
        kfree(ff);
        return -1;
    }

    /* 清空缓冲区 */
    for (uint32_t i = 0; i < fat32.bytes_per_cluster; i++) {
        ff->cluster_buffer[i] = 0;
    }
    
    /* 只有当簇有效时才读取 */
    if (cluster != 0 && cluster < 0x0FFFFFF8) {
        fat32_read_cluster(cluster, ff->cluster_buffer);
    }
    file->fs_data = ff;

    return 0;
}

static int fat32_close(struct vfs_file *file) {
    struct fat32_file *ff = (struct fat32_file *)file->fs_data;
    if (ff != NULL) {
        /* 如果文件有修改，刷新写回缓存确保数据持久化 */
        if (file->is_dirty) {
            fat32_writeback_flush();
        }
        /* 刷新 FAT 缓存，确保所有元数据写入磁盘 */
        fat32_flush_fat_cache();
        
        if (ff->cluster_buffer != NULL) {
            kfree(ff->cluster_buffer);
        }
        kfree(ff);
    }
    return 0;
}

static ssize_t fat32_read(struct vfs_file *file, void *buf, size_t count) {
    struct fat32_file *ff = (struct fat32_file *)file->fs_data;
    
    /* 参数验证 */
    if (ff == NULL) {
        println("[FAT32] read: Invalid file handle");
        return -1;
    }
    if (buf == NULL && count > 0) {
        println("[FAT32] read: NULL buffer with non-zero count");
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    
    /* 验证簇缓冲区 */
    if (ff->cluster_buffer == NULL) {
        println("[FAT32] read: Cluster buffer is NULL");
        return -1;
    }

    /* 计算可读取的数据量 */
    if (file->offset >= ff->file_size) {
        return 0;  /* 已到文件末尾 */
    }
    
    size_t remaining = ff->file_size - file->offset;
    if (count > remaining) {
        count = remaining;
    }

    size_t total_read = 0;
    
    while (total_read < count) {
        /* 计算当前簇可读取的数据量 */
        size_t chunk = fat32.bytes_per_cluster - ff->offset_in_cluster;
        if (chunk > count - total_read) {
            chunk = count - total_read;
        }

        /* 从簇缓冲区复制数据 */
        memcpy((uint8_t *)buf + total_read, ff->cluster_buffer + ff->offset_in_cluster, chunk);
        total_read += chunk;
        ff->offset_in_cluster += chunk;
        file->offset += chunk;

        /* 如果需要读取下一个簇 */
        if (ff->offset_in_cluster >= fat32.bytes_per_cluster && total_read < count) {
            ff->offset_in_cluster = 0;
            ff->current_cluster = fat32_next_cluster(ff->current_cluster);
            
            /* 检查是否到达文件末尾 */
            if (IS_FREE_CLUSTER(ff->current_cluster) || IS_EOF_CLUSTER(ff->current_cluster)) {
                break;  /* 文件结束 */
            }
            
            /* 读取下一个簇 */
            if (fat32_read_cluster(ff->current_cluster, ff->cluster_buffer) != 0) {
                println("[FAT32] read: Failed to read cluster");
                break;  /* 读取失败 */
            }
        }
    }

    return total_read;
}

static ssize_t fat32_write(struct vfs_file *file, const void *buf, size_t count) {
    struct fat32_file *ff = (struct fat32_file *)file->fs_data;
    
    /* 参数验证 */
    if (ff == NULL) {
        println("[FAT32] write: Invalid file handle");
        return -1;
    }
    if (buf == NULL && count > 0) {
        println("[FAT32] write: NULL buffer with non-zero count");
        return -1;
    }
    if (count == 0) {
        return 0;  /* 无需写入 */
    }
    
    /* 验证簇缓冲区 */
    if (ff->cluster_buffer == NULL) {
        println("[FAT32] write: Cluster buffer is NULL");
        return -1;
    }

    /* 如果文件没有起始簇，需要分配 */
    if (ff->start_cluster == 0 || !IS_VALID_CLUSTER(ff->start_cluster)) {
        ff->start_cluster = fat32_alloc_cluster();
        if (ff->start_cluster == 0) {
            println("[FAT32] write: Failed to allocate start cluster");
            return -1;
        }
        ff->current_cluster = ff->start_cluster;
        ff->offset_in_cluster = 0;
        
        /* 清空簇缓冲区 */
        for (uint32_t i = 0; i < fat32.bytes_per_cluster; i++) {
            ff->cluster_buffer[i] = 0;
        }
        
        print("[FAT32] write: Allocated start cluster ");
        print_dec(ff->start_cluster);
        println("");
    }

    size_t written = 0;
    const uint8_t *src = (const uint8_t *)buf;

    while (written < count) {
        /* 计算当前簇可用空间 */
        size_t chunk = fat32.bytes_per_cluster - ff->offset_in_cluster;
        if (chunk > count - written) {
            chunk = count - written;
        }

        /* 写入数据到簇缓冲区 */
        memcpy(ff->cluster_buffer + ff->offset_in_cluster, src + written, chunk);
        written += chunk;
        ff->offset_in_cluster += chunk;
        file->offset += chunk;

        /* 更新文件大小 */
        if (file->offset > ff->file_size) {
            ff->file_size = file->offset;
        }

        /* 如果簇缓冲区满了，写入磁盘 */
        if (ff->offset_in_cluster >= fat32.bytes_per_cluster) {
            /* 写入当前簇 */
            if (fat32_write_cluster(ff->current_cluster, ff->cluster_buffer) != 0) {
                println("[FAT32] write: Failed to write cluster");
                break;
            }

            /* 检查是否还有数据要写 */
            if (written < count) {
                /* 获取或分配下一个簇 */
                uint32_t next = fat32_next_cluster(ff->current_cluster);
                if (IS_FREE_CLUSTER(next) || IS_EOF_CLUSTER(next)) {
                    /* 需要分配新簇 */
                    if (fat32_extend_cluster_chain(ff->current_cluster, &next) != 0) {
                        println("[FAT32] write: Failed to extend cluster chain");
                        break;
                    }
                    /* 清空新簇缓冲区 */
                    for (uint32_t i = 0; i < fat32.bytes_per_cluster; i++) {
                        ff->cluster_buffer[i] = 0;
                    }
                } else {
                    /* 读取下一个簇 */
                    if (fat32_read_cluster(next, ff->cluster_buffer) != 0) {
                        println("[FAT32] write: Failed to read next cluster");
                        break;
                    }
                }
                
                ff->current_cluster = next;
                ff->offset_in_cluster = 0;
            }
        }
    }

    /* 写入当前簇缓冲区 (部分写入或最后一块) */
    if (written > 0 && ff->offset_in_cluster > 0) {
        if (fat32_write_cluster(ff->current_cluster, ff->cluster_buffer) != 0) {
            println("[FAT32] write: Warning - failed to flush final cluster");
        }
    }

    /* 更新目录项中的文件大小 */
    if (written > 0 && ff->parent_cluster != 0 && ff->filename[0] != '\0') {
        if (fat32_update_file_size(ff->parent_cluster, ff->filename, ff->file_size) != 0) {
            println("[FAT32] write: Warning - failed to update file size in directory");
        }
    }

    return written;
}

static int fat32_seek(struct vfs_file *file, uint64_t offset) {
    struct fat32_file *ff = (struct fat32_file *)file->fs_data;
    if (ff == NULL) {
        return -1;
    }

    if (offset > ff->file_size) {
        offset = ff->file_size;
    }

    /* 特殊情况：空文件 (start_cluster = 0) */
    if (ff->start_cluster == 0 || ff->start_cluster >= 0x0FFFFFF8) {
        if (offset == 0) {
            file->offset = 0;
            ff->offset_in_cluster = 0;
            return 0;  /* 空文件 seek 到 0 是允许的 */
        }
        return -1;  /* 空文件不能 seek 到非零位置 */
    }

    /* 计算需要的簇 */
    uint64_t cluster_offset = 0;
    uint32_t cluster = ff->start_cluster;

    while (cluster_offset + fat32.bytes_per_cluster <= offset && cluster != 0) {
        cluster_offset += fat32.bytes_per_cluster;
        cluster = fat32_next_cluster(cluster);
    }

    if (cluster == 0 || cluster >= 0x0FFFFFF8) {
        return -1;
    }

    ff->current_cluster = cluster;
    ff->offset_in_cluster = offset - cluster_offset;
    file->offset = offset;

    fat32_read_cluster(cluster, ff->cluster_buffer);

    return 0;
}

static int fat32_stat(const char *path, struct vfs_stat *stat) {
    if (stat == NULL) {
        return -1;
    }

    uint32_t clusters[32];
    uint8_t last_attr = 0;
    uint32_t file_size = 0;
    int depth = parse_path_attr(path, clusters, &last_attr, &file_size, 32);

    if (depth < 1) {
        stat->type = VFS_TYPE_FILE;
        stat->size = 0;
        return -1;
    }

    /* 根据目录项属性判断类型，直接从目录项读取文件大小 */
    if (last_attr & FAT_ATTR_DIRECTORY) {
        stat->type = VFS_TYPE_DIR;
        stat->size = 0;
    } else {
        stat->type = VFS_TYPE_FILE;
        stat->size = file_size;
    }

    /* FAT32 没有 Unix 权限，设置默认权限: 0755 (rwxr-xr-x) */
    stat->mode = 0755;
    stat->uid = 0;
    stat->gid = 0;

    return 0;
}

static int fat32_readdir(const char *path, struct vfs_dirent *dirent, int index) {
    /* Validate parameters */
    if (path == NULL || dirent == NULL) {
        println("[FAT32] readdir: NULL parameters!");
        return -1;
    }
    
    print("[FAT32] readdir: path='");
    print(path);
    print("' dirent=");
    print_hex((uint64_t)dirent);
    print(" index=");
    print_dec(index);
    println("");
    
    uint32_t clusters[32];
    int depth = parse_path(path, clusters, 32);

    if (depth < 1) {
        println("[FAT32] readdir: invalid path or depth");
        return -1;
    }

    print("[FAT32] readdir: depth=");
    print_dec(depth);
    print(" cluster=");
    print_dec(clusters[depth - 1]);
    println("");
    
    uint32_t cluster = clusters[depth - 1];
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) {
        println("[FAT32] readdir: kmalloc failed!");
        return -1;
    }
    
    print("[FAT32] readdir: buf=");
    print_hex((uint64_t)buf);
    print(" bytes_per_cluster=");
    print_dec(fat32.bytes_per_cluster);
    println("");

    int current = 0;
    int entries_per_cluster = fat32.bytes_per_cluster / sizeof(struct fat32_dirent);
    
    while (cluster != 0 && cluster < 0x0FFFFFF8) {
        fat32_read_cluster(cluster, buf);
        struct fat32_dirent *fat_dirent = (struct fat32_dirent *)buf;

        for (int i = 0; i < entries_per_cluster; i++) {
            if (fat_dirent[i].name[0] == 0x00) {
                kfree(buf);
                return -1;  /* 目录结束 */
            }

            if (fat_dirent[i].name[0] == 0xE5) {
                continue;
            }

            /* 处理长文件名 */
            if ((fat_dirent[i].attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
                struct lfn_dirent *lfn = (struct lfn_dirent *)&fat_dirent[i];

                /* 只处理最后一个LFN条目 */
                if (!(lfn->seq & 0x40)) continue;

                int num_lfn = lfn->seq & 0x1F;

                /* 检查是否为请求的索引 */
                if (current == index) {
                    char lfn_name[256] = {0};
                    int name_pos = 0;
                    int lfn_ok = 1;
                    
                    uint32_t lfn_cluster = cluster;
                    uint8_t *lfn_buf = buf;
                    int lfn_buf_is_ours = 0;

                    for (int k = 0; k < num_lfn; k++) {
                        int idx = i + (num_lfn - 1 - k);
                        uint8_t *active_buf = buf;
                        uint32_t active_entries = entries_per_cluster;
                        
                        if (idx >= entries_per_cluster) {
                            uint32_t next_cl = cluster;
                            int skip = idx / entries_per_cluster;
                            for (int s = 0; s <= skip; s++) {
                                if (s > 0) next_cl = fat32_next_cluster(next_cl);
                                if (next_cl == 0 || next_cl >= 0x0FFFFFF8) { lfn_ok = 0; break; }
                            }
                            if (!lfn_ok) break;
                            
                            if (next_cl != lfn_cluster) {
                                if (lfn_buf_is_ours && lfn_buf && lfn_buf != buf) kfree(lfn_buf);
                                lfn_buf = kmalloc(fat32.bytes_per_cluster);
                                if (!lfn_buf) { lfn_ok = 0; break; }
                                lfn_buf_is_ours = 1;
                                if (fat32_read_cluster(next_cl, lfn_buf) != 0) { kfree(lfn_buf); lfn_ok = 0; break; }
                                lfn_cluster = next_cl;
                            }
                            active_buf = lfn_buf;
                            active_entries = fat32.bytes_per_cluster / sizeof(struct fat32_dirent);
                            idx = idx % entries_per_cluster;
                        }
                        
                        struct fat32_dirent *act_dirent = (struct fat32_dirent *)active_buf;
                        if (idx >= (int)active_entries) { lfn_ok = 0; break; }
                        lfn = (struct lfn_dirent *)&act_dirent[idx];
                        if ((lfn->attr & FAT_ATTR_LFN) != FAT_ATTR_LFN) { lfn_ok = 0; break; }

                        char part[14];
                        lfn_extract_name(part, 14, lfn);

                        int part_len = 0;
                        while (part[part_len]) part_len++;

                        if (name_pos + part_len >= 255) break;
                        for (int m = 0; m < part_len; m++) {
                            lfn_name[name_pos + m] = part[m];
                        }
                        name_pos += part_len;
                    }
                    
                    if (lfn_buf_is_ours && lfn_buf && lfn_buf != buf) kfree(lfn_buf);
                    
                    if (lfn_ok) {
                        lfn_name[name_pos] = '\0';

                        int copy_len = name_pos < 255 ? name_pos : 255;
                        for (int k = 0; k < copy_len; k++) {
                            dirent->name[k] = lfn_name[k];
                        }
                        dirent->name[copy_len] = '\0';

                        int short_idx = i + num_lfn;
                        if (short_idx < entries_per_cluster) {
                            dirent->type = (fat_dirent[short_idx].attr & FAT_ATTR_DIRECTORY) ?
                                           VFS_TYPE_DIR : VFS_TYPE_FILE;
                            dirent->inode = ((uint32_t)fat_dirent[short_idx].cluster_high << 16) |
                                            fat_dirent[short_idx].cluster_low;
                        } else {
                            uint32_t next_cl = cluster;
                            int skip = short_idx / entries_per_cluster;
                            for (int s = 0; s <= skip; s++) {
                                if (s > 0) next_cl = fat32_next_cluster(next_cl);
                                if (next_cl == 0 || next_cl >= 0x0FFFFFF8) break;
                            }
                            if (next_cl != 0 && next_cl < 0x0FFFFFF8) {
                                uint8_t *sbuf = kmalloc(fat32.bytes_per_cluster);
                                if (sbuf) {
                                    if (fat32_read_cluster(next_cl, sbuf) == 0) {
                                        struct fat32_dirent *sd = (struct fat32_dirent *)sbuf;
                                        int si = short_idx % entries_per_cluster;
                                        dirent->type = (sd[si].attr & FAT_ATTR_DIRECTORY) ?
                                                       VFS_TYPE_DIR : VFS_TYPE_FILE;
                                        dirent->inode = ((uint32_t)sd[si].cluster_high << 16) |
                                                        sd[si].cluster_low;
                                    }
                                    kfree(sbuf);
                                }
                            }
                        }
                    }

                    kfree(buf);
                    return 0;
                }

                current++;
                /* Skip remaining LFN entries and the 8.3 short name entry
                 * to avoid double-counting. LFN entries are at i+1..i+num_lfn-1,
                 * the 8.3 short name entry is at i+num_lfn.
                 * The for loop will i++, so we advance by num_lfn here. */
                i += num_lfn;
                continue;
            }

            /* Skip volume label entries */
            if (fat_dirent[i].attr & FAT_ATTR_VOLUME_ID) {
                continue;
            }

            /* 8.3 短文件名 */
            if (current == index) {
                int k;
                for (k = 0; k < 8 && fat_dirent[i].name[k] != ' '; k++) {
                    dirent->name[k] = fat_dirent[i].name[k];
                }
                if (fat_dirent[i].name[8] != ' ') {
                    dirent->name[k++] = '.';
                    for (int l = 8; l < 11 && fat_dirent[i].name[l] != ' '; l++) {
                        dirent->name[k++] = fat_dirent[i].name[l];
                    }
                }
                dirent->name[k] = '\0';

                /* 转小写 */
                for (k = 0; dirent->name[k]; k++) {
                    if (dirent->name[k] >= 'A' && dirent->name[k] <= 'Z') {
                        dirent->name[k] += 32;
                    }
                }

                dirent->type = (fat_dirent[i].attr & FAT_ATTR_DIRECTORY) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
                dirent->inode = ((uint32_t)fat_dirent[i].cluster_high << 16) | fat_dirent[i].cluster_low;

                kfree(buf);
                return 0;
            }

            current++;
        }

        cluster = fat32_next_cluster(cluster);
    }

    kfree(buf);
    return -1;
}

/* FAT32 创建目录 */
static int fat32_mkdir(const char *path) {
    if (path == NULL) return -1;
    
    /* 调试输出 */
    print("[FAT32] mkdir: path='");
    print(path);
    println("'");
    
    /* 处理相对路径可能没有前导 '/' 的情况 */
    const char *work_path = path;
    char path_buf[256];
    
    if (path[0] != '/') {
        /* 添加前导 '/' */
        path_buf[0] = '/';
        int i;
        for (i = 0; path[i] && i < 254; i++) {
            path_buf[i + 1] = path[i];
        }
        path_buf[i + 1] = '\0';
        work_path = path_buf;
        
        print("[FAT32] mkdir: normalized path='");
        print(work_path);
        println("'");
    }
    
    /* 解析路径获取父目录簇和新目录名 */
    char parent_path[256];
    char dir_name[256];
    
    /* 分离父目录和目录名 */
    int len = 0;
    while (work_path[len]) len++;
    
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (work_path[i] == '/') {
            last_slash = i;
            break;
        }
    }
    
    if (last_slash < 0) {
        println("[FAT32] mkdir: Invalid path - no slash found");
        return -1;  /* 无效路径 */
    }
    
    /* 复制父路径 */
    for (int i = 0; i < last_slash; i++) {
        parent_path[i] = work_path[i];
    }
    parent_path[last_slash] = '\0';
    if (parent_path[0] == '\0') {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    }
    
    /* 复制目录名 */
    int j = 0;
    for (int i = last_slash + 1; i < len; i++) {
        dir_name[j++] = work_path[i];
    }
    dir_name[j] = '\0';
    
    if (dir_name[0] == '\0') {
        println("[FAT32] mkdir: Empty directory name");
        return -1;  /* 空目录名 */
    }
    
    print("[FAT32] mkdir: parent='");
    print(parent_path);
    print("', dir='");
    print(dir_name);
    println("'");
    
    /* 解析父目录簇 */
    int32_t clusters[32];
    int depth = parse_path(parent_path, clusters, 32);
    if (depth <= 0) {
        println("[FAT32] mkdir: Parent directory not found");
        return -1;
    }
    
    uint32_t parent_cluster = clusters[depth - 1];
    print("[FAT32] mkdir: parent_cluster=");
    print_dec(parent_cluster);
    println("");
    
    /* 分配新簇 */
    uint32_t new_cluster = fat32_alloc_cluster();
    if (new_cluster == 0) {
        println("[FAT32] mkdir: Failed to allocate cluster");
        return -1;
    }
    
    /* 创建目录项 */
    int ret = fat32_create_dirent(parent_cluster, dir_name, new_cluster, 0,
                                   FAT_ATTR_DIRECTORY, NULL, NULL);
    if (ret != 0) {
        fat32_write_fat_entry(new_cluster, 0);  /* 释放簇 */
        println("[FAT32] mkdir: Failed to create directory entry");
        return -1;
    }
    
    /* 初始化新目录 (创建 . 和 .. 条目) */
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) {
        return -1;
    }
    
    /* 清空簇 */
    for (uint32_t i = 0; i < fat32.bytes_per_cluster; i++) {
        buf[i] = 0;
    }
    
    /* 创建 "." 条目 */
    struct fat32_dirent *dot = (struct fat32_dirent *)buf;
    for (int i = 0; i < 11; i++) dot->name[i] = ' ';
    dot->name[0] = '.';
    dot->attr = FAT_ATTR_DIRECTORY;
    dot->cluster_high = (new_cluster >> 16) & 0xFFFF;
    dot->cluster_low = new_cluster & 0xFFFF;
    dot->file_size = 0;
    
    /* 创建 ".." 条目 */
    struct fat32_dirent *dotdot = (struct fat32_dirent *)(buf + sizeof(struct fat32_dirent));
    for (int i = 0; i < 11; i++) dotdot->name[i] = ' ';
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT_ATTR_DIRECTORY;
    dotdot->cluster_high = (parent_cluster >> 16) & 0xFFFF;
    dotdot->cluster_low = parent_cluster & 0xFFFF;
    dotdot->file_size = 0;
    
    /* 标记目录结束 */
    struct fat32_dirent *end = (struct fat32_dirent *)(buf + 2 * sizeof(struct fat32_dirent));
    end->name[0] = 0x00;
    
    /* 写入簇 */
    fat32_write_cluster(new_cluster, buf);
    kfree(buf);
    
    fat32_flush_fat_cache();
    
    print("[FAT32] mkdir: Created directory ");
    println(path);
    
    return 0;
}

/* FAT32 删除目录 */
static int fat32_rmdir(const char *path) {
    if (path == NULL) return -1;
    
    /* 解析路径 */
    int32_t clusters[32];
    int depth = parse_path(path, clusters, 32);
    if (depth <= 0) {
        println("[FAT32] rmdir: Directory not found");
        return -1;
    }
    
    uint32_t dir_cluster = clusters[depth - 1];
    uint32_t parent_cluster = (depth > 1) ? clusters[depth - 2] : fat32.root_cluster;
    
    /* 检查目录是否为空 */
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) return -1;
    
    uint32_t cluster = dir_cluster;
    int is_empty = 1;
    
    while (cluster != 0 && cluster < 0x0FFFFFF8) {
        fat32_read_cluster(cluster, buf);
        struct fat32_dirent *dirent = (struct fat32_dirent *)buf;
        
        for (uint32_t i = 0; i < fat32.bytes_per_cluster / sizeof(struct fat32_dirent); i++) {
            if (dirent[i].name[0] == 0x00) break;
            if (dirent[i].name[0] == 0xE5) continue;
            if (dirent[i].attr & FAT_ATTR_LFN) continue;
            
            /* 跳过 . 和 .. */
            if (dirent[i].name[0] == '.') {
                if (dirent[i].name[1] == ' ' || dirent[i].name[1] == '\0' ||
                    (dirent[i].name[1] == '.' && dirent[i].name[2] == ' ')) {
                    continue;
                }
            }
            
            is_empty = 0;
            break;
        }
        
        if (!is_empty) break;
        cluster = fat32_next_cluster(cluster);
    }
    
    kfree(buf);
    
    if (!is_empty) {
        println("[FAT32] rmdir: Directory not empty");
        return -1;
    }
    
    /* 在父目录中标记为删除 */
    buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) return -1;
    
    /* 获取目录名 */
    char dir_name[256];
    int len = 0;
    while (path[len]) len++;
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') {
            last_slash = i;
            break;
        }
    }
    int j = 0;
    for (int i = last_slash + 1; i < len; i++) {
        dir_name[j++] = path[i];
    }
    dir_name[j] = '\0';
    
    /* 转换为 FAT 名称格式 */
    char fat_name[11];
    for (int i = 0; i < 11; i++) fat_name[i] = ' ';
    for (int i = 0, k = 0; i < 8 && dir_name[k]; k++) {
        if (dir_name[k] == '.') { k++; break; }
        fat_name[i++] = (dir_name[k] >= 'a' && dir_name[k] <= 'z') ?
                        (dir_name[k] - 32) : dir_name[k];
    }
    for (int i = 8, k = 0; dir_name[k]; k++) {
        if (dir_name[k] == '.') { k++; i = 8; continue; }
        if (i < 11) {
            fat_name[i++] = (dir_name[k] >= 'a' && dir_name[k] <= 'z') ?
                           (dir_name[k] - 32) : dir_name[k];
        }
    }
    
    /* 在父目录中查找并标记删除 */
    cluster = parent_cluster;
    while (cluster != 0 && cluster < 0x0FFFFFF8) {
        fat32_read_cluster(cluster, buf);
        struct fat32_dirent *dirent = (struct fat32_dirent *)buf;
        
        for (uint32_t i = 0; i < fat32.bytes_per_cluster / sizeof(struct fat32_dirent); i++) {
            if (dirent[i].name[0] == 0x00) break;
            
            int match = 1;
            for (int k = 0; k < 11 && match; k++) {
                if (dirent[i].name[k] != fat_name[k]) match = 0;
            }
            
            if (match) {
                /* 检查是否匹配目录簇 */
                uint32_t entry_cluster = ((uint32_t)dirent[i].cluster_high << 16) |
                                         dirent[i].cluster_low;
                if (entry_cluster == dir_cluster) {
                    /* 标记为删除 */
                    dirent[i].name[0] = 0xE5;
                    fat32_write_cluster(cluster, buf);
                    kfree(buf);
                    
                    /* 释放目录的簇链 */
                    fat32_free_cluster_chain(dir_cluster);
                    fat32_flush_fat_cache();
                    
                    print("[FAT32] rmdir: Removed directory ");
                    println(path);
                    return 0;
                }
            }
        }
        
        cluster = fat32_next_cluster(cluster);
    }
    
    kfree(buf);
    println("[FAT32] rmdir: Directory entry not found in parent");
    return -1;
}

/* FAT32 删除文件 */
static int fat32_unlink(const char *path) {
    if (path == NULL) return -1;
    
    /* 解析路径 */
    int32_t clusters[32];
    int depth = parse_path(path, clusters, 32);
    if (depth <= 0) {
        println("[FAT32] unlink: File not found");
        return -1;
    }
    
    uint32_t file_cluster = clusters[depth - 1];
    uint32_t parent_cluster = (depth > 1) ? clusters[depth - 2] : fat32.root_cluster;
    
    /* 获取文件名 */
    char file_name[256];
    int len = 0;
    while (path[len]) len++;
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') {
            last_slash = i;
            break;
        }
    }
    int j = 0;
    for (int i = last_slash + 1; i < len; i++) {
        file_name[j++] = path[i];
    }
    file_name[j] = '\0';
    
    /* 转换为 FAT 名称格式 */
    char fat_name[11];
    for (int i = 0; i < 11; i++) fat_name[i] = ' ';
    for (int i = 0, k = 0; i < 8 && file_name[k]; k++) {
        if (file_name[k] == '.') { k++; break; }
        fat_name[i++] = (file_name[k] >= 'a' && file_name[k] <= 'z') ?
                        (file_name[k] - 32) : file_name[k];
    }
    for (int i = 8, k = 0; file_name[k]; k++) {
        if (file_name[k] == '.') { k++; i = 8; continue; }
        if (i < 11) {
            fat_name[i++] = (file_name[k] >= 'a' && file_name[k] <= 'z') ?
                           (file_name[k] - 32) : file_name[k];
        }
    }
    
    /* 在父目录中查找并标记删除 */
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) return -1;
    
    uint32_t cluster = parent_cluster;
    while (cluster != 0 && cluster < 0x0FFFFFF8) {
        fat32_read_cluster(cluster, buf);
        struct fat32_dirent *dirent = (struct fat32_dirent *)buf;
        
        for (uint32_t i = 0; i < fat32.bytes_per_cluster / sizeof(struct fat32_dirent); i++) {
            if (dirent[i].name[0] == 0x00) break;
            
            int match = 1;
            for (int k = 0; k < 11 && match; k++) {
                if (dirent[i].name[k] != fat_name[k]) match = 0;
            }
            
            if (match) {
                /* 检查是否匹配文件簇 */
                uint32_t entry_cluster = ((uint32_t)dirent[i].cluster_high << 16) |
                                         dirent[i].cluster_low;
                if (entry_cluster == file_cluster) {
                    /* 标记为删除 */
                    dirent[i].name[0] = 0xE5;
                    fat32_write_cluster(cluster, buf);
                    kfree(buf);
                    
                    /* 释放文件的簇链 */
                    fat32_free_cluster_chain(file_cluster);
                    fat32_flush_fat_cache();
                    
                    print("[FAT32] unlink: Removed file ");
                    println(path);
                    return 0;
                }
            }
        }
        
        cluster = fat32_next_cluster(cluster);
    }
    
    kfree(buf);
    println("[FAT32] unlink: File entry not found in parent");
    return -1;
}

/* FAT32 重命名 */
static int fat32_rename(const char *oldpath, const char *newpath) {
    if (oldpath == NULL || newpath == NULL) return -1;
    
    /* 简化实现：复制 + 删除 */
    /* TODO: 实现原子重命名 */
    
    println("[FAT32] rename: Not fully implemented");
    return -1;
}

static struct vfs_ops fat32_ops = {
    .open = fat32_open,
    .close = fat32_close,
    .read = fat32_read,
    .write = fat32_write,
    .seek = fat32_seek,
    .stat = fat32_stat,
    .readdir = fat32_readdir,
    .mkdir = fat32_mkdir,
    .rmdir = fat32_rmdir,
    .unlink = fat32_unlink,
    .rename = fat32_rename,
};

struct vfs_ops *fat32_get_ops(void) {
    return &fat32_ops;
}

int fat32_init(void) {
    /* 检查磁盘是否可用 */
    extern int disk_is_available(void);
    if (!disk_is_available()) {
        serial_print("[FAT32] Disk not available\n");
        return -1;
    }

    /* 读取第一个扇区 (MBR 或 FAT32 Boot Sector) */
    uint8_t sector[512];
    if (disk_read_sector(0, sector) != 0) {
        serial_print("[FAT32] Failed to read sector 0\n");
        return -1;
    }

    /* 检查是否为FAT32 Boot Sector (没有MBR的原始镜像)
     * FAT32 Boot Sector特征:
     * - 偏移0: 'EB' (跳转指令)
     * - 偏移82: 'FAT32' 字符串
     * - 偏移510-511: 0x55 0xAA
     */
    int is_fat32_boot_sector = 0;
    if (sector[0] == 0xEB && sector[510] == 0x55 && sector[511] == 0xAA) {
        /* 检查是否为FAT32 */
        if (sector[82] == 'F' && sector[83] == 'A' && sector[84] == 'T' &&
            sector[85] == '3' && sector[86] == '2') {
            is_fat32_boot_sector = 1;
            serial_print("[FAT32] Detected raw FAT32 boot sector (no MBR)\n");
        }
    }

    if (is_fat32_boot_sector) {
        /* 直接作为FAT32分区从扇区0开始 */
        serial_print("[FAT32] Mounting as raw FAT32 from sector 0\n");
        return fat32_init_partition(0);
    }

    /* 检查 MBR 签名 */
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        serial_print("[FAT32] No valid MBR signature found\n");
        return -1;
    }

    uint64_t partition_start = 0;

    /* 检查是否为 GPT 分区表 (保护MBR)
     * 保护MBR在偏移0x1BE(446)处有一个类型为0xEE的分区
     */
    if (sector[450] == 0xEE) {
        /* GPT 分区表，解析 GPT 头部 (LBA 1) */
        uint8_t gpt_header[512];
        if (disk_read_sector(1, gpt_header) != 0) {
            return -1;
        }

        /* 验证 GPT 签名 */
        if (gpt_header[0] != 'E' || gpt_header[1] != 'F' || gpt_header[2] != 'I' || gpt_header[3] != ' ' ||
            gpt_header[4] != 'P' || gpt_header[5] != 'A' || gpt_header[6] != 'R' || gpt_header[7] != 'T') {
            return -1;
        }

        /* 获取分区表起始 LBA (偏移 72-79) */
        uint64_t partition_entries_lba = 0;
        for (int i = 0; i < 8; i++) {
            partition_entries_lba |= ((uint64_t)gpt_header[72 + i] << (i * 8));
        }

        /* 读取第一个分区表项 */
        uint8_t partition_entry[128];
        if (disk_read_sector(partition_entries_lba, partition_entry) != 0) {
            return -1;
        }

        /* 获取分区起始 LBA (偏移 32-39) */
        for (int i = 0; i < 8; i++) {
            partition_start |= ((uint64_t)partition_entry[32 + i] << (i * 8));
        }

        serial_print("[FAT32] Found GPT partition at LBA ");
        char buf[20]; int bp = 0;
        uint64_t tmp = partition_start;
        if (tmp == 0) buf[bp++] = '0';
        else { char t[20]; int tp = 0; while (tmp > 0) { t[tp++] = '0' + (tmp % 10); tmp /= 10; } while (tp > 0) buf[bp++] = t[--tp]; }
        buf[bp] = '\n'; buf[bp+1] = '\0';
        serial_print(buf);
    } else {
        /* MBR 分区表，获取第一个分区 */
        partition_start = *(uint32_t *)(sector + 0x1BE + 8);

        serial_print("[FAT32] Found MBR partition at LBA ");
        char buf[20]; int bp = 0;
        uint64_t tmp = partition_start;
        if (tmp == 0) buf[bp++] = '0';
        else { char t[20]; int tp = 0; while (tmp > 0) { t[tp++] = '0' + (tmp % 10); tmp /= 10; } while (tp > 0) buf[bp++] = t[--tp]; }
        buf[bp] = '\n'; buf[bp+1] = '\0';
        serial_print(buf);
    }

    return fat32_init_partition(partition_start);
}

int fat32_init_partition(uint64_t partition_start) {
    uint8_t sector[512];
    serial_print("[FAT32] Reading partition start sector...\n");
    if (disk_read_sector(partition_start, sector) != 0) {
        serial_print("[FAT32] Failed to read partition start sector\n");
        return -1;
    }
    serial_print("[FAT32] Partition start sector read OK\n");

    struct fat32_bpb *bpb = (struct fat32_bpb *)sector;

    /* 验证 FAT32 签名 */
    if (sector[82] != 'F' || sector[83] != 'A' || sector[84] != 'T' ||
        sector[85] != '3' || sector[86] != '2') {
        serial_print("[FAT32] Not a FAT32 filesystem (missing FAT32 signature)\n");
        return -1;
    }

    /* 验证 FAT32 */
    serial_print("[FAT32] BPB: bytes_per_sector=");
    char buf[60]; int bp = 0;
    uint16_t tmp16 = bpb->bytes_per_sector;
    if (tmp16 == 0) { buf[bp++] = '0'; } else { char t[10]; int tp = 0; while (tmp16 > 0) { t[tp++] = '0' + (tmp16 % 10); tmp16 /= 10; } while (tp > 0) buf[bp++] = t[--tp]; }
    buf[bp++] = ' '; buf[bp++] = 's'; buf[bp++] = 'p'; buf[bp++] = 'c'; buf[bp++] = '=';
    uint8_t tmp8 = bpb->sectors_per_cluster;
    if (tmp8 == 0) { buf[bp++] = '0'; } else { char t[10]; int tp = 0; while (tmp8 > 0) { t[tp++] = '0' + (tmp8 % 10); tmp8 /= 10; } while (tp > 0) buf[bp++] = t[--tp]; }
    buf[bp++] = ' '; buf[bp++] = 'n'; buf[bp++] = 'f'; buf[bp++] = 'a'; buf[bp++] = 't'; buf[bp++] = '=';
    tmp8 = bpb->num_fats;
    if (tmp8 == 0) { buf[bp++] = '0'; } else { char t[10]; int tp = 0; while (tmp8 > 0) { t[tp++] = '0' + (tmp8 % 10); tmp8 /= 10; } while (tp > 0) buf[bp++] = t[--tp]; }
    buf[bp++] = ' '; buf[bp++] = 'r'; buf[bp++] = 'o'; buf[bp++] = 'o'; buf[bp++] = 't'; buf[bp++] = '=';
    uint32_t tmp32 = bpb->root_cluster;
    if (tmp32 == 0) { buf[bp++] = '0'; } else { char t[15]; int tp = 0; while (tmp32 > 0) { t[tp++] = '0' + (tmp32 % 10); tmp32 /= 10; } while (tp > 0) buf[bp++] = t[--tp]; }
    buf[bp] = '\n'; buf[bp+1] = '\0';
    serial_print(buf);

    if (bpb->bytes_per_sector == 0 || bpb->bytes_per_sector != 512) {
        serial_print("[FAT32] Invalid bytes_per_sector (must be 512)\n");
        return -1;
    }
    if (bpb->sectors_per_cluster == 0) {
        serial_print("[FAT32] Invalid sectors_per_cluster\n");
        return -1;
    }
    if (bpb->num_fats == 0 || bpb->num_fats > 4) {
        serial_print("[FAT32] Invalid num_fats\n");
        return -1;
    }
    if (bpb->sectors_per_fat_32 == 0) {
        serial_print("[FAT32] Invalid sectors_per_fat\n");
        return -1;
    }
    if (bpb->root_cluster < 2) {
        serial_print("[FAT32] Invalid root_cluster\n");
        return -1;
    }

    fat32.partition_start = partition_start;
    fat32.bytes_per_sector = bpb->bytes_per_sector;
    fat32.sectors_per_cluster = bpb->sectors_per_cluster;
    fat32.bytes_per_cluster = fat32.bytes_per_sector * fat32.sectors_per_cluster;
    fat32.reserved_sectors = bpb->reserved_sectors;
    fat32.sectors_per_fat = bpb->sectors_per_fat_32;
    fat32.num_fats = bpb->num_fats;
    fat32.root_cluster = bpb->root_cluster;

    fat32.fat_start = fat32.reserved_sectors;
    fat32.data_start = fat32.fat_start + fat32.sectors_per_fat * fat32.num_fats;
    fat32.total_clusters = (bpb->total_sectors_32 - fat32.data_start) / fat32.sectors_per_cluster;
    
    /* 初始化增强型 FAT 缓存 */
    fat32_fat_cache_init();
    
    /* 初始化写回缓存 */
    fat32_writeback_init();
    
    /* 挂载 FAT32 到 VFS 根目录 */
    println("[FAT32] About to call vfs_mount...");
    struct vfs_ops *ops = fat32_get_ops();
    print("[FAT32] fat32_ops address: ");
    print_hex((uint64_t)ops);
    println("");
    print("[FAT32] ops->open: ");
    print_hex((uint64_t)ops->open);
    println("");
    print("[FAT32] ops->stat: ");
    print_hex((uint64_t)ops->stat);
    println("");
    
    if (vfs_mount("/", "fat32", ops, NULL) != 0) {
        println("[FAT32] vfs_mount failed!");
        return -1;
    }
    println("[FAT32] vfs_mount succeeded");

    return 0;
}

/* 获取 FAT32 文件系统实例 */
struct fat32_fs *fat32_get_fs(void) {
    if (fat32.total_clusters == 0) {
        return NULL;
    }
    return &fat32;
}

/* 刷新 FAT 表到磁盘 */
int fat32_flush_fat(void) {
    return fat32_flush_fat_cache();
}

/* 获取空闲簇数 */
uint32_t fat32_get_free_clusters(void) {
    if (fat32.total_clusters == 0) {
        return 0;
    }
    
    uint32_t free_count = 0;
    /* 扫描 FAT 表统计空闲簇 */
    for (uint32_t cluster = 2; cluster < fat32.total_clusters + 2; cluster++) {
        uint32_t entry = fat32_read_fat_entry(cluster);
        if (entry == 0) {
            free_count++;
        }
    }
    
    return free_count;
}

/* ========== FAT32 高性能优化 ========== */

/* 目录项缓存 */
#define DIR_CACHE_SIZE      64
#define DIR_CACHE_NAME_LEN  256

struct dir_cache_entry {
    uint32_t cluster;                   /* 目录起始簇 */
    char name[DIR_CACHE_NAME_LEN];      /* 文件名 */
    uint32_t file_cluster;              /* 文件起始簇 */
    uint64_t file_size;                 /* 文件大小 */
    uint32_t attributes;                /* 文件属性 */
    uint32_t dir_entry_offset;          /* 目录项偏移 */
    uint64_t access_time;               /* 访问时间 */
    uint32_t hits;                      /* 命中次数 */
    uint8_t valid;
    uint8_t pad[3];
};

static struct dir_cache_entry dir_cache[DIR_CACHE_SIZE];
static volatile int dir_cache_lock2 = 0;

/* 初始化目录缓存 */
void fat32_dir_cache_init(void) {
    for (int i = 0; i < DIR_CACHE_SIZE; i++) {
        dir_cache[i].valid = 0;
        dir_cache[i].hits = 0;
    }
}

/* 查找目录缓存 */
static struct dir_cache_entry *dir_cache_lookup(uint32_t cluster, const char *name) {
    for (int i = 0; i < DIR_CACHE_SIZE; i++) {
        if (dir_cache[i].valid && 
            dir_cache[i].cluster == cluster) {
            /* 比较文件名 */
            int j = 0;
            while (name[j] && dir_cache[i].name[j] && 
                   name[j] == dir_cache[i].name[j]) {
                j++;
            }
            if (name[j] == '\0' && dir_cache[i].name[j] == '\0') {
                dir_cache[i].hits++;
                return &dir_cache[i];
            }
        }
    }
    return NULL;
}

/* 添加目录缓存项 */
__attribute__((unused)) static void dir_cache_add(uint32_t cluster, const char *name, 
                          uint32_t file_cluster, uint64_t file_size,
                          uint32_t attributes, uint32_t offset) {
    /* 查找空闲槽或替换最少使用的项 */
    int slot = -1;
    uint32_t min_hits = 0xFFFFFFFF;
    
    for (int i = 0; i < DIR_CACHE_SIZE; i++) {
        if (!dir_cache[i].valid) {
            slot = i;
            break;
        }
        if (dir_cache[i].hits < min_hits) {
            min_hits = dir_cache[i].hits;
            slot = i;
        }
    }
    
    if (slot >= 0) {
        /* 添加缓存项 */
        dir_cache[slot].cluster = cluster;
        int j = 0;
        while (name[j] && j < DIR_CACHE_NAME_LEN - 1) {
            dir_cache[slot].name[j] = name[j];
            j++;
        }
        dir_cache[slot].name[j] = '\0';
        dir_cache[slot].file_cluster = file_cluster;
        dir_cache[slot].file_size = file_size;
        dir_cache[slot].attributes = attributes;
        dir_cache[slot].dir_entry_offset = offset;
        dir_cache[slot].hits = 1;
        dir_cache[slot].valid = 1;
    }
}

/* 预读优化 */
#define READAHEAD_SIZE      8       /* 预读簇数 */
#define READAHEAD_THRESHOLD 2       /* 触发预读的顺序读取阈值 */

struct readahead_state {
    uint32_t last_cluster;
    uint32_t seq_count;
    uint32_t readahead_cluster;
    int enabled;
};

static struct readahead_state readahead;

/* 更新预读状态 */
__attribute__((unused)) static void update_readahead(uint32_t cluster) {
    if (cluster == readahead.last_cluster + 1) {
        readahead.seq_count++;
        
        /* 触发预读 */
        if (readahead.seq_count >= READAHEAD_THRESHOLD && readahead.enabled) {
            uint32_t ra_cluster = cluster + 1;
            for (int i = 0; i < READAHEAD_SIZE; i++) {
                uint32_t next = fat32_read_fat_entry(ra_cluster);
                if (next >= 0x0FFFFFF8) break;  /* 文件结束 */
                
                /* 预读到缓存 */
                uint64_t lba = fat32.data_start + (uint64_t)(ra_cluster - 2) * fat32.sectors_per_cluster; (void)lba;
                /* disk_read_sector 会自动缓存 */
                
                ra_cluster = next;
            }
            readahead.readahead_cluster = ra_cluster;
        }
    } else {
        readahead.seq_count = 1;
    }
    readahead.last_cluster = cluster;
}

/* 簇位图 - 快速空闲簇查找 */
#define CLUSTER_BITMAP_SIZE  4096
static uint32_t cluster_bitmap[CLUSTER_BITMAP_SIZE];
static int cluster_bitmap_valid = 0;

/* 初始化簇位图 */
void fat32_cluster_bitmap_init(void) {
    for (int i = 0; i < CLUSTER_BITMAP_SIZE; i++) {
        cluster_bitmap[i] = 0;
    }
    cluster_bitmap_valid = 1;
}

/* 更新簇位图 */
__attribute__((unused)) static void cluster_bitmap_set(uint32_t cluster, int used) {
    if (cluster < 2 || cluster >= fat32.total_clusters + 2) return;
    cluster -= 2;  /* 簇号从2开始 */
    
    uint32_t index = cluster / 32;
    uint32_t bit = cluster % 32;
    
    if (index < CLUSTER_BITMAP_SIZE) {
        if (used) {
            cluster_bitmap[index] |= (1 << bit);
        } else {
            cluster_bitmap[index] &= ~(1 << bit);
        }
    }
}

/* 快速查找空闲簇 */
static uint32_t cluster_bitmap_find_free(void) {
    for (uint32_t i = 0; i < CLUSTER_BITMAP_SIZE && i < (fat32.total_clusters / 32 + 1); i++) {
        if (cluster_bitmap[i] != 0xFFFFFFFF) {
            /* 找到有空闲位的字 */
            for (int bit = 0; bit < 32; bit++) {
                if (!(cluster_bitmap[i] & (1 << bit))) {
                    uint32_t cluster = i * 32 + bit + 2;
                    if (cluster < fat32.total_clusters + 2) {
                        return cluster;
                    }
                }
            }
        }
    }
    return 0;  /* 没有空闲簇 */
}

/* 文件分配表缓存增强 */
#define FAT_CACHE_WAYS   4        /* 每个 set 的关联路数 */

struct fat_cache_set {
    uint32_t tags[FAT_CACHE_WAYS];
    uint32_t lru[FAT_CACHE_WAYS];
    uint32_t sectors[FAT_CACHE_WAYS][128];
    uint8_t valid[FAT_CACHE_WAYS];
    uint8_t dirty[FAT_CACHE_WAYS];
};

#define FAT_CACHE_SETS   32
static struct fat_cache_set fat_cache_sets[FAT_CACHE_SETS];

/* 计算 FAT 缓存索引 */
static inline uint32_t fat_cache_hash(uint32_t fat_sector) {
    return fat_sector % FAT_CACHE_SETS;
}

/* 从增强 FAT 缓存读取 */
__attribute__((unused)) static int fat_cache_read_enhanced(uint32_t fat_sector, uint32_t *data) {
    uint32_t set_idx = fat_cache_hash(fat_sector);
    struct fat_cache_set *set = &fat_cache_sets[set_idx];
    
    for (int way = 0; way < FAT_CACHE_WAYS; way++) {
        if (set->valid[way] && set->tags[way] == fat_sector) {
            /* 命中 */
            for (int i = 0; i < 128; i++) {
                data[i] = set->sectors[way][i];
            }
            set->lru[way] = 0;
            return 1;
        }
    }
    
    return 0;  /* 未命中 */
}

/* 写入增强 FAT 缓存 */
static void fat_cache_write_enhanced(uint32_t fat_sector, uint32_t *data) {
    uint32_t set_idx = fat_cache_hash(fat_sector);
    struct fat_cache_set *set = &fat_cache_sets[set_idx];
    
    /* 查找空闲路或替换 LRU */
    int way = -1;
    uint32_t max_lru = 0;
    
    for (int i = 0; i < FAT_CACHE_WAYS; i++) {
        if (!set->valid[i]) {
            way = i;
            break;
        }
        if (set->lru[i] > max_lru) {
            max_lru = set->lru[i];
            way = i;
        }
    }
    
    if (way >= 0) {
        set->tags[way] = fat_sector;
        for (int i = 0; i < 128; i++) {
            set->sectors[way][i] = data[i];
        }
        set->valid[way] = 1;
        set->dirty[way] = 1;
        set->lru[way] = 0;
    }
}

/* ========== FAT32 统计信息 ========== */

struct fat32_stats {
    uint64_t reads;
    uint64_t writes;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cluster_allocs;
    uint64_t cluster_frees;
    uint64_t dir_cache_hits;
    uint64_t readahead_count;
};

static struct fat32_stats fat32_stats;

/* 获取 FAT32 统计 */
void fat32_get_stats(struct fat32_stats *stats) {
    if (stats != NULL) {
        *stats = fat32_stats;
    }
}

/* 重置 FAT32 统计 */
void fat32_reset_stats(void) {
    fat32_stats.reads = 0;
    fat32_stats.writes = 0;
    fat32_stats.cache_hits = 0;
    fat32_stats.cache_misses = 0;
    fat32_stats.cluster_allocs = 0;
    fat32_stats.cluster_frees = 0;
    fat32_stats.dir_cache_hits = 0;
    fat32_stats.readahead_count = 0;
}

/* 打印 FAT32 统计 */
void fat32_print_stats(void) {
    println("=== FAT32 Statistics ===");
    print("Reads:         ");
    print_dec(fat32_stats.reads);
    println("");
    print("Writes:        ");
    print_dec(fat32_stats.writes);
    println("");
    print("Cache Hits:    ");
    print_dec(fat32_stats.cache_hits);
    println("");
    print("Cache Misses:  ");
    print_dec(fat32_stats.cache_misses);
    println("");
    print("Cluster Allocs:");
    print_dec(fat32_stats.cluster_allocs);
    println("");
    print("Cluster Frees: ");
    print_dec(fat32_stats.cluster_frees);
    println("");
    print("Dir Cache Hits:");
    print_dec(fat32_stats.dir_cache_hits);
    println("");
    print("Readahead:     ");
    print_dec(fat32_stats.readahead_count);
    println("");
}

/* ========== 文件系统完整性检查 ========== */

/* 检查 FAT 表一致性 */
int fat32_check_fat_consistency(void) {
    println("[FAT32] Checking FAT consistency...");
    
    int errors = 0;
    uint32_t lost_clusters = 0;
    uint32_t cross_links = 0;
    
    /* 扫描所有簇 */
    for (uint32_t cluster = 2; cluster < fat32.total_clusters + 2; cluster++) {
        uint32_t entry = fat32_read_fat_entry(cluster);
        
        /* 检查无效值 */
        if (entry != 0 && entry < 0x0FFFFFF8 && 
            (entry < 2 || entry >= fat32.total_clusters + 2)) {
            println("[FAT32] Invalid FAT entry");
            errors++;
        }
    }
    
    print("FAT check complete. Errors: ");
    print_dec(errors);
    println("");
    
    return errors;
}

/* 修复 FAT 表 */
int fat32_repair_fat(void) {
    println("[FAT32] Attempting FAT repair...");
    
    int repaired = 0;
    
    /* 修复无效条目 */
    for (uint32_t cluster = 2; cluster < fat32.total_clusters + 2; cluster++) {
        uint32_t entry = fat32_read_fat_entry(cluster);
        
        if (entry != 0 && entry < 0x0FFFFFF8 && 
            (entry < 2 || entry >= fat32.total_clusters + 2)) {
            /* 标记为坏簇 */
            fat32_write_fat_entry(cluster, 0x0FFFFFF7);
            repaired++;
        }
    }
    
    /* 刷新 FAT */
    fat32_flush_fat();
    
    print("Repaired ");
    print_dec(repaired);
    println(" entries");
    
    return repaired;
}

/* ========== FAT32 高级缓存增强功能 ========== */

/* === 簇位图缓存 (快速分配) === */

struct fat32_cluster_bitmap {
    uint64_t *allocated;        /* 已分配位图 */
    uint64_t *free_map;         /* 空闲位图 */
    uint32_t last_alloc;        /* 最后分配位置 */
    uint32_t total_clusters;
    int initialized;
};

static struct fat32_cluster_bitmap cluster_bmap;

/* 初始化簇位图 */
int fat32_cluster_bitmap_init2(uint32_t total_clusters) {
    uint32_t bitmap_words = (total_clusters + 63) / 64;
    
    cluster_bmap.allocated = kmalloc(bitmap_words * sizeof(uint64_t));
    cluster_bmap.free_map = kmalloc(bitmap_words * sizeof(uint64_t));
    
    if (cluster_bmap.allocated == NULL || cluster_bmap.free_map == NULL) {
        if (cluster_bmap.allocated) kfree(cluster_bmap.allocated);
        if (cluster_bmap.free_map) kfree(cluster_bmap.free_map);
        return -1;
    }
    
    for (uint32_t i = 0; i < bitmap_words; i++) {
        cluster_bmap.allocated[i] = 0;
        cluster_bmap.free_map[i] = ~0ULL;
    }
    
    cluster_bmap.total_clusters = total_clusters;
    cluster_bmap.last_alloc = 2;
    cluster_bmap.initialized = 1;
    
    return 0;
}

/* 从位图快速分配簇 */
int fat32_cluster_bitmap_alloc(void) {
    if (!cluster_bmap.initialized) return -1;
    
    uint32_t start = cluster_bmap.last_alloc;
    uint32_t current = start;
    
    do {
        uint32_t word = current / 64;
        uint32_t bit = current % 64;
        
        if (cluster_bmap.free_map[word] & (1ULL << bit)) {
            cluster_bmap.free_map[word] &= ~(1ULL << bit);
            cluster_bmap.allocated[word] |= (1ULL << bit);
            cluster_bmap.last_alloc = current + 1;
            return current;
        }
        
        current++;
        if (current >= cluster_bmap.total_clusters + 2) {
            current = 2;
        }
    } while (current != start);
    
    return -1;
}

/* 释放簇到位图 */
void fat32_cluster_bitmap_free(uint32_t cluster) {
    if (!cluster_bmap.initialized || cluster < 2) return;
    
    uint32_t word = cluster / 64;
    uint32_t bit = cluster % 64;
    
    cluster_bmap.allocated[word] &= ~(1ULL << bit);
    cluster_bmap.free_map[word] |= (1ULL << bit);
}

/* === 预读缓冲区 === */

#define FAT32_RA_BUFFERS       8
#define FAT32_RA_SIZE          16384

struct fat32_ra_buffer {
    uint32_t start_cluster;
    uint32_t count;
    uint8_t data[FAT32_RA_SIZE];
    uint64_t access_time;
    int valid;
};

static struct fat32_ra_buffer ra_buffers[FAT32_RA_BUFFERS];

/* 初始化预读缓冲区 */
void fat32_readahead_init(void) {
    for (int i = 0; i < FAT32_RA_BUFFERS; i++) {
        ra_buffers[i].valid = 0;
    }
}

/* 执行预读 */
void fat32_do_readahead(uint32_t start_cluster, uint32_t count) {
    if (count == 0 || count > FAT32_RA_SIZE / 512) return;
    
    struct fat32_ra_buffer *buf = NULL;
    for (int i = 0; i < FAT32_RA_BUFFERS; i++) {
        if (!ra_buffers[i].valid) {
            buf = &ra_buffers[i];
            break;
        }
    }
    
    if (buf == NULL) {
        buf = &ra_buffers[0];
        for (int i = 1; i < FAT32_RA_BUFFERS; i++) {
            if (ra_buffers[i].access_time < buf->access_time) {
                buf = &ra_buffers[i];
            }
        }
    }
    
    buf->start_cluster = start_cluster;
    buf->count = count;
    buf->access_time = get_tick_count();
    buf->valid = 1;
    
    for (uint32_t i = 0; i < count; i++) {
        fat32_read_cluster(start_cluster + i, buf->data + i * 512);
    }
}

/* === 写入合并优化 === */

#define FAT32_WRITE_MERGE_MAX  16

struct fat32_write_entry {
    uint32_t cluster;
    uint32_t size;
    uint8_t *data;
    uint64_t submit_time;
    int pending;
};

static struct fat32_write_entry write_merge_q[FAT32_WRITE_MERGE_MAX];
static uint32_t write_merge_cnt;
static volatile int write_merge_lk = 0;

/* 初始化写入合并 */
void fat32_write_merge_init(void) {
    write_merge_cnt = 0;
    for (int i = 0; i < FAT32_WRITE_MERGE_MAX; i++) {
        write_merge_q[i].pending = 0;
    }
}

/* 提交写入 */
int fat32_submit_write_merged(uint32_t cluster, const void *data, uint32_t size) {
    while (__sync_lock_test_and_set(&write_merge_lk, 1)) {
        asm volatile("pause");
    }
    
    if (write_merge_cnt < FAT32_WRITE_MERGE_MAX) {
        write_merge_q[write_merge_cnt].cluster = cluster;
        write_merge_q[write_merge_cnt].size = size;
        write_merge_q[write_merge_cnt].data = (uint8_t *)data;
        write_merge_q[write_merge_cnt].submit_time = get_tick_count();
        write_merge_q[write_merge_cnt].pending = 1;
        write_merge_cnt++;
    }
    
    __sync_lock_release(&write_merge_lk);
    
    return fat32_write_cluster(cluster, data);
}

/* 刷新写入队列 */
void fat32_write_merge_flush(void) {
    while (__sync_lock_test_and_set(&write_merge_lk, 1)) {
        asm volatile("pause");
    }
    
    for (uint32_t i = 0; i < write_merge_cnt; i++) {
        if (write_merge_q[i].pending) {
            fat32_write_cluster(write_merge_q[i].cluster, write_merge_q[i].data);
            write_merge_q[i].pending = 0;
        }
    }
    
    write_merge_cnt = 0;
    
    __sync_lock_release(&write_merge_lk);
}

/* === 延迟写入 === */

#define FAT32_LAZY_WRITE_MAX   32
#define FAT32_LAZY_DELAY_NS    5000000000ULL

struct fat32_lazy_write {
    uint32_t cluster;
    uint8_t data[512];
    uint64_t write_time;
    int pending;
};

static struct fat32_lazy_write lazy_writes[FAT32_LAZY_WRITE_MAX];
static volatile int lazy_write_lk = 0;

/* 初始化延迟写入 */
void fat32_lazy_write_init(void) {
    for (int i = 0; i < FAT32_LAZY_WRITE_MAX; i++) {
        lazy_writes[i].pending = 0;
    }
}

/* 添加延迟写入 */
int fat32_add_lazy_write(uint32_t cluster, const void *data) {
    while (__sync_lock_test_and_set(&lazy_write_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < FAT32_LAZY_WRITE_MAX; i++) {
        if (lazy_writes[i].pending && lazy_writes[i].cluster == cluster) {
            for (int j = 0; j < 512; j++) {
                lazy_writes[i].data[j] = ((uint8_t *)data)[j];
            }
            lazy_writes[i].write_time = get_tick_count();
            __sync_lock_release(&lazy_write_lk);
            return 0;
        }
    }
    
    for (int i = 0; i < FAT32_LAZY_WRITE_MAX; i++) {
        if (!lazy_writes[i].pending) {
            lazy_writes[i].cluster = cluster;
            for (int j = 0; j < 512; j++) {
                lazy_writes[i].data[j] = ((uint8_t *)data)[j];
            }
            lazy_writes[i].write_time = get_tick_count();
            lazy_writes[i].pending = 1;
            __sync_lock_release(&lazy_write_lk);
            return 0;
        }
    }
    
    __sync_lock_release(&lazy_write_lk);
    return fat32_write_cluster(cluster, data);
}

/* 处理延迟写入 */
void fat32_lazy_write_process(void) {
    uint64_t now = get_tick_count();
    
    while (__sync_lock_test_and_set(&lazy_write_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < FAT32_LAZY_WRITE_MAX; i++) {
        if (lazy_writes[i].pending && 
            now - lazy_writes[i].write_time >= FAT32_LAZY_DELAY_NS) {
            fat32_write_cluster(lazy_writes[i].cluster, lazy_writes[i].data);
            lazy_writes[i].pending = 0;
        }
    }
    
    __sync_lock_release(&lazy_write_lk);
}

/* === 性能统计增强 === */

struct fat32_perf_stats {
    uint64_t cluster_allocs_fast;
    uint64_t readahead_hits;
    uint64_t write_merges;
    uint64_t lazy_writes;
    uint64_t avg_read_latency;
    uint64_t avg_write_latency;
};

static struct fat32_perf_stats fat32_perf;

/* 记录延迟 */
void fat32_record_latency(int is_write, uint64_t latency) {
    if (is_write) {
        fat32_perf.avg_write_latency = 
            (fat32_perf.avg_write_latency * 7 + latency) / 8;
    } else {
        fat32_perf.avg_read_latency = 
            (fat32_perf.avg_read_latency * 7 + latency) / 8;
    }
}

/* 打印性能统计 */
void fat32_print_perf_stats(void) {
    println("=== FAT32 Performance ===");
    
    print("Fast Cluster Allocs: ");
    print_dec(fat32_perf.cluster_allocs_fast);
    println("");
    
    print("Readahead Hits: ");
    print_dec(fat32_perf.readahead_hits);
    println("");
    
    print("Write Merges: ");
    print_dec(fat32_perf.write_merges);
    println("");
    
    print("Lazy Writes: ");
    print_dec(fat32_perf.lazy_writes);
    println("");
    
    print("Avg Read Latency: ");
    print_dec(fat32_perf.avg_read_latency);
    println(" ns");
    
    print("Avg Write Latency: ");
    print_dec(fat32_perf.avg_write_latency);
    println(" ns");
}

/* ========== FAT32 高级缓存增强功能 ========== */

/* === 多级 FAT 缓存 === */

/* FAT 缓存行 */
#define FAT_CACHE2_WAYS          4
#define FAT_CACHE2_SETS          64

struct fat_cache2_entry {
    uint32_t sector;
    uint8_t data[512];
    uint64_t access_time;
    uint32_t access_count;
    int valid;
    int dirty;
};

static struct fat_cache2_entry fat_cache2[FAT_CACHE2_SETS][FAT_CACHE2_WAYS];
static volatile int fat_cache2_lk = 0;

/* 初始化多级 FAT 缓存 */
void fat_cache2_init(void) {
    for (int s = 0; s < FAT_CACHE2_SETS; s++) {
        for (int w = 0; w < FAT_CACHE2_WAYS; w++) {
            fat_cache2[s][w].valid = 0;
            fat_cache2[s][w].dirty = 0;
            fat_cache2[s][w].access_count = 0;
        }
    }
}

/* 从多级缓存读取 FAT 扇区 */
int fat_cache2_read(uint32_t sector, void *buf) {
    uint32_t set = sector % FAT_CACHE2_SETS;
    
    while (__sync_lock_test_and_set(&fat_cache2_lk, 1)) {
        asm volatile("pause");
    }
    
    /* 查找缓存 */
    for (int w = 0; w < FAT_CACHE2_WAYS; w++) {
        if (fat_cache2[set][w].valid && fat_cache2[set][w].sector == sector) {
            for (int i = 0; i < 512; i++) {
                ((uint8_t *)buf)[i] = fat_cache2[set][w].data[i];
            }
            fat_cache2[set][w].access_time = get_tick_count();
            fat_cache2[set][w].access_count++;
            __sync_lock_release(&fat_cache2_lk);
            return 0;
        }
    }
    
    __sync_lock_release(&fat_cache2_lk);
    return -1;
}

/* 写入多级缓存 */
void fat_cache2_write(uint32_t sector, const void *buf) {
    uint32_t set = sector % FAT_CACHE2_SETS;
    
    while (__sync_lock_test_and_set(&fat_cache2_lk, 1)) {
        asm volatile("pause");
    }
    
    /* 查找空闲槽或 LRU 替换 */
    int victim = 0;
    uint64_t oldest = ~0ULL;
    
    for (int w = 0; w < FAT_CACHE2_WAYS; w++) {
        if (!fat_cache2[set][w].valid) {
            victim = w;
            break;
        }
        if (fat_cache2[set][w].access_time < oldest) {
            oldest = fat_cache2[set][w].access_time;
            victim = w;
        }
    }
    
    /* 如果替换的条目是脏的，写回磁盘 */
    if (fat_cache2[set][victim].valid && fat_cache2[set][victim].dirty) {
        disk_write_sector(fat32.partition_start + fat_cache2[set][victim].sector, 
                          fat_cache2[set][victim].data);
    }
    
    fat_cache2[set][victim].sector = sector;
    for (int i = 0; i < 512; i++) {
        fat_cache2[set][victim].data[i] = ((uint8_t *)buf)[i];
    }
    fat_cache2[set][victim].access_time = get_tick_count();
    fat_cache2[set][victim].access_count = 1;
    fat_cache2[set][victim].valid = 1;
    fat_cache2[set][victim].dirty = 0;
    
    __sync_lock_release(&fat_cache2_lk);
}

/* 刷新多级缓存 */
void fat_cache2_flush(void) {
    while (__sync_lock_test_and_set(&fat_cache2_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int s = 0; s < FAT_CACHE2_SETS; s++) {
        for (int w = 0; w < FAT_CACHE2_WAYS; w++) {
            if (fat_cache2[s][w].valid && fat_cache2[s][w].dirty) {
                disk_write_sector(fat32.partition_start + fat_cache2[s][w].sector,
                                  fat_cache2[s][w].data);
                fat_cache2[s][w].dirty = 0;
            }
        }
    }
    
    __sync_lock_release(&fat_cache2_lk);
}

/* ========== 文件碎片整理系统 ========== */

/* 碎片信息结构 */
struct fat32_fragment_info {
    uint32_t start_cluster;
    uint32_t fragment_count;
    uint32_t total_clusters;
    uint32_t contiguous_runs;
    uint64_t fragmentation_score;
};

/* 分析文件碎片 */
int fat32_analyze_fragmentation(uint32_t start_cluster, struct fat32_fragment_info *info) {
    if (info == NULL || start_cluster < 2) return -1;
    
    info->start_cluster = start_cluster;
    info->fragment_count = 0;
    info->total_clusters = 0;
    info->contiguous_runs = 1;
    info->fragmentation_score = 0;
    
    uint32_t prev_cluster = 0;
    uint32_t current = start_cluster;
    
    while (IS_VALID_CLUSTER(current) && !IS_EOF_CLUSTER(current)) {
        info->total_clusters++;
        
        uint32_t next = fat32_read_fat_entry(current);
        
        /* 检查是否连续 */
        if (prev_cluster != 0 && next != current + 1) {
            info->fragment_count++;
            info->contiguous_runs++;
        }
        
        prev_cluster = current;
        current = next;
    }
    
    /* 计算碎片分数 (0-100, 0 = 完全连续) */
    if (info->total_clusters > 1) {
        info->fragmentation_score = 
            (uint64_t)info->fragment_count * 100 / (info->total_clusters - 1);
    }
    
    return 0;
}

/* 碎片整理单个文件 */
int fat32_defragment_file(uint32_t start_cluster) {
    struct fat32_fragment_info info;
    if (fat32_analyze_fragmentation(start_cluster, &info) != 0) {
        return -1;
    }
    
    /* 如果碎片率低于 10%，不需要整理 */
    if (info.fragmentation_score < 10) {
        return 0;
    }
    
    println("[FAT32] Defragmenting file...");
    print("  Clusters: ");
    print_dec(info.total_clusters);
    print(", Fragments: ");
    print_dec(info.fragment_count);
    println("");
    
    /* 分配连续簇块 */
    uint32_t new_start = 0;
    uint32_t contiguous = 0;
    
    /* 寻找足够大的连续空闲区域 */
    for (uint32_t c = 2; c < fat32.total_clusters + 2 - info.total_clusters; c++) {
        int is_free = 1;
        for (uint32_t j = 0; j < info.total_clusters; j++) {
            if (fat32_read_fat_entry(c + j) != 0) {
                is_free = 0;
                c += j;
                break;
            }
        }
        if (is_free) {
            new_start = c;
            contiguous = 1;
            break;
        }
    }
    
    if (!contiguous) {
        println("[FAT32] No contiguous space available for defragmentation");
        return -1;
    }
    
    /* 复制数据到新位置 */
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) return -1;
    
    uint32_t old_cluster = start_cluster;
    uint32_t new_cluster = new_start;
    uint32_t prev_new = 0;
    
    while (IS_VALID_CLUSTER(old_cluster) && !IS_EOF_CLUSTER(old_cluster)) {
        /* 读取旧数据 */
        fat32_read_cluster(old_cluster, buf);
        
        /* 写入新位置 */
        fat32_write_cluster(new_cluster, buf);
        
        /* 更新 FAT 链 */
        if (prev_new != 0) {
            fat32_write_fat_entry(prev_new, new_cluster);
        }
        prev_new = new_cluster;
        
        /* 保存下一个旧簇 */
        uint32_t next_old = fat32_read_fat_entry(old_cluster);
        
        /* 标记旧簇为空闲 */
        fat32_write_fat_entry(old_cluster, 0);
        
        old_cluster = next_old;
        new_cluster++;
    }
    
    /* 标记新链的最后一个簇为 EOF */
    if (prev_new != 0) {
        fat32_write_fat_entry(prev_new, 0x0FFFFFFF);
    }
    
    fat32_flush_fat_cache();
    kfree(buf);
    
    println("[FAT32] Defragmentation complete");
    return (int)new_start;  /* 返回新的起始簇 */
}

/* 整个卷碎片整理 */
int fat32_defragment_volume(void) {
    println("[FAT32] Starting volume defragmentation...");
    
    uint32_t defragged = 0;
    uint32_t failed = 0;
    
    /* 遍历根目录 */
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) return -1;
    
    uint32_t cluster = fat32.root_cluster;
    
    while (IS_VALID_CLUSTER(cluster) && !IS_EOF_CLUSTER(cluster)) {
        fat32_read_cluster(cluster, buf);
        struct fat32_dirent *dirent = (struct fat32_dirent *)buf;
        
        for (uint32_t i = 0; i < fat32.bytes_per_cluster / sizeof(struct fat32_dirent); i++) {
            if (dirent[i].name[0] == 0x00) break;
            if (dirent[i].name[0] == 0xE5 || (dirent[i].attr & FAT_ATTR_LFN)) continue;
            if (dirent[i].attr & FAT_ATTR_DIRECTORY) continue;  /* 跳过目录 */
            
            uint32_t file_cluster = ((uint32_t)dirent[i].cluster_high << 16) | dirent[i].cluster_low;
            if (file_cluster >= 2) {
                int result = fat32_defragment_file(file_cluster);
                if (result > 0) {
                    /* 更新目录项 */
                    dirent[i].cluster_high = (result >> 16) & 0xFFFF;
                    dirent[i].cluster_low = result & 0xFFFF;
                    fat32_write_cluster(cluster, buf);
                    defragged++;
                } else if (result < 0) {
                    failed++;
                }
            }
        }
        
        cluster = fat32_next_cluster(cluster);
    }
    
    kfree(buf);
    
    print("[FAT32] Defragmentation complete: ");
    print_dec(defragged);
    print(" files, ");
    print_dec(failed);
    println(" failed");
    
    return defragged;
}

/* ========== 长文件名 (LFN) 优化支持 ========== */

#define LFN_MAX_ENTRIES         20
#define LFN_CHARS_PER_ENTRY     13

/* LFN 校验和计算 */
static uint8_t fat32_lfn_checksum(const uint8_t *name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + name[i];
    }
    return sum;
}

/* 创建 LFN 目录项 */
int fat32_create_lfn_entries(uint32_t parent_cluster, const char *long_name,
                              const char *short_name, uint32_t start_cluster,
                              uint32_t file_size, uint8_t attr) {
    size_t name_len = strlen(long_name);
    uint8_t lfn_entries_needed = (name_len + LFN_CHARS_PER_ENTRY - 1) / LFN_CHARS_PER_ENTRY;
    
    if (lfn_entries_needed > LFN_MAX_ENTRIES) {
        return -1;  /* 名字太长 */
    }
    
    /* 计算 8.3 校验和 */
    char fat_short[11];
    for (int i = 0; i < 11; i++) fat_short[i] = ' ';
    const char *dot = short_name;
    int j;
    for (j = 0; j < 8 && *dot && *dot != '.'; j++, dot++) {
        fat_short[j] = (*dot >= 'a' && *dot <= 'z') ? *dot - 32 : *dot;
    }
    if (*dot == '.') {
        dot++;
        for (j = 8; j < 11 && *dot; j++, dot++) {
            fat_short[j] = (*dot >= 'a' && *dot <= 'z') ? *dot - 32 : *dot;
        }
    }
    uint8_t checksum = fat32_lfn_checksum((uint8_t *)fat_short);
    
    /* 分配缓冲区 */
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) return -1;
    
    /* LFN 目录项结构 */
    struct fat32_lfn_entry {
        uint8_t seq;
        uint16_t name1[5];
        uint8_t attr;
        uint8_t type;
        uint8_t checksum;
        uint16_t name2[6];
        uint16_t cluster;
        uint16_t name3[2];
    } __attribute__((packed));
    
    /* 查找空闲槽位 */
    uint32_t cluster = parent_cluster;
    uint32_t entry_offset = 0;
    int found_slot = 0;
    
    while (IS_VALID_CLUSTER(cluster) && !IS_EOF_CLUSTER(cluster)) {
        fat32_read_cluster(cluster, buf);
        struct fat32_dirent *dirent = (struct fat32_dirent *)buf;
        
        for (uint32_t i = 0; i < fat32.bytes_per_cluster / sizeof(struct fat32_dirent); i++) {
            if (dirent[i].name[0] == 0x00 || dirent[i].name[0] == 0xE5) {
                entry_offset = i;
                found_slot = 1;
                break;
            }
        }
        if (found_slot) break;
        cluster = fat32_next_cluster(cluster);
    }
    
    if (!found_slot) {
        kfree(buf);
        return -1;
    }
    
    /* 从后往前创建 LFN 条目 */
    uint32_t name_pos = 0;
    for (int entry = lfn_entries_needed - 1; entry >= 0; entry--) {
        struct fat32_lfn_entry *lfn = (struct fat32_lfn_entry *)&buf[entry_offset * sizeof(struct fat32_dirent)];
        
        lfn->seq = entry + 1;
        if (entry == lfn_entries_needed - 1) {
            lfn->seq |= 0x40;  /* 最后一条 LFN 条目标记 */
        }
        lfn->attr = FAT_ATTR_LFN;
        lfn->type = 0;
        lfn->checksum = checksum;
        lfn->cluster = 0;
        
        /* 填充名称字符 (UTF-16) */
        int pos;
        for (pos = 0; pos < 5 && name_pos < name_len; pos++, name_pos++) {
            lfn->name1[pos] = (uint16_t)long_name[name_pos];
        }
        for (; pos < 5; pos++) lfn->name1[pos] = 0xFFFF;
        
        for (pos = 0; pos < 6 && name_pos < name_len; pos++, name_pos++) {
            lfn->name2[pos] = (uint16_t)long_name[name_pos];
        }
        for (; pos < 6; pos++) lfn->name2[pos] = 0xFFFF;
        
        for (pos = 0; pos < 2 && name_pos < name_len; pos++, name_pos++) {
            lfn->name3[pos] = (uint16_t)long_name[name_pos];
        }
        for (; pos < 2; pos++) lfn->name3[pos] = 0xFFFF;
        
        entry_offset++;
    }
    
    /* 创建 8.3 短目录项 */
    struct fat32_dirent *dirent = (struct fat32_dirent *)&buf[entry_offset * sizeof(struct fat32_dirent)];
    for (int i = 0; i < 11; i++) {
        dirent->name[i] = fat_short[i];
    }
    dirent->attr = attr;
    dirent->cluster_high = (start_cluster >> 16) & 0xFFFF;
    dirent->cluster_low = start_cluster & 0xFFFF;
    dirent->file_size = file_size;
    
    /* 标记目录结束 */
    if (entry_offset + 1 < fat32.bytes_per_cluster / sizeof(struct fat32_dirent)) {
        struct fat32_dirent *next = (struct fat32_dirent *)&buf[(entry_offset + 1) * sizeof(struct fat32_dirent)];
        next->name[0] = 0x00;
    }
    
    /* 写回 */
    int result = fat32_write_cluster(cluster, buf);
    kfree(buf);
    
    return result;
}

/* 读取 LFN 名称 */
int fat32_read_lfn_name(uint32_t cluster, uint32_t offset, char *name, size_t max_len) {
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) return -1;
    
    fat32_read_cluster(cluster, buf);
    
    /* 向后查找 8.3 条目 */
    struct fat32_dirent *dirent = (struct fat32_dirent *)buf;
    while (offset > 0) {
        offset -= sizeof(struct fat32_dirent);
        if ((dirent[offset / sizeof(struct fat32_dirent)].attr & FAT_ATTR_LFN) == 0) {
            break;
        }
    }
    
    /* 向前读取 LFN 条目 */
    size_t name_pos = 0;
    uint32_t pos = offset + sizeof(struct fat32_dirent);
    
    while (pos < fat32.bytes_per_cluster) {
        struct fat32_lfn_entry {
            uint8_t seq;
            uint16_t name1[5];
            uint8_t attr;
            uint8_t type;
            uint8_t checksum;
            uint16_t name2[6];
            uint16_t cluster;
            uint16_t name3[2];
        } __attribute__((packed));
        
        struct fat32_lfn_entry *lfn = (struct fat32_lfn_entry *)&buf[pos];
        
        if ((lfn->attr & FAT_ATTR_LFN) == 0 || lfn->attr != FAT_ATTR_LFN) {
            break;
        }
        
        /* 提取名称字符 */
        for (int i = 0; i < 5 && name_pos < max_len - 1; i++) {
            if (lfn->name1[i] != 0xFFFF && lfn->name1[i] != 0) {
                name[name_pos++] = (char)lfn->name1[i];
            }
        }
        for (int i = 0; i < 6 && name_pos < max_len - 1; i++) {
            if (lfn->name2[i] != 0xFFFF && lfn->name2[i] != 0) {
                name[name_pos++] = (char)lfn->name2[i];
            }
        }
        for (int i = 0; i < 2 && name_pos < max_len - 1; i++) {
            if (lfn->name3[i] != 0xFFFF && lfn->name3[i] != 0) {
                name[name_pos++] = (char)lfn->name3[i];
            }
        }
        
        pos += sizeof(struct fat32_dirent);
    }
    
    name[name_pos] = '\0';
    kfree(buf);
    
    return (int)name_pos;
}

/* ========== 文件系统快照功能 ========== */

#define FAT32_SNAPSHOT_MAX      8

struct fat32_snapshot {
    uint32_t id;
    char name[32];
    uint64_t create_time;
    uint32_t root_cluster_copy;
    uint32_t *fat_copy;          /* FAT 表副本 */
    uint32_t fat_entries;
    int active;
};

static struct fat32_snapshot snapshots[FAT32_SNAPSHOT_MAX];
static volatile int snapshot_lk = 0;

/* 创建快照 */
int fat32_create_snapshot(const char *name) {
    if (name == NULL) return -1;
    
    while (__sync_lock_test_and_set(&snapshot_lk, 1)) {
        asm volatile("pause");
    }
    
    /* 查找空闲槽位 */
    int slot = -1;
    for (int i = 0; i < FAT32_SNAPSHOT_MAX; i++) {
        if (!snapshots[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        __sync_lock_release(&snapshot_lk);
        println("[FAT32] No free snapshot slots");
        return -1;
    }
    
    /* 分配 FAT 副本 */
    uint32_t fat_entries = fat32.total_clusters + 2;
    snapshots[slot].fat_copy = kmalloc(fat_entries * sizeof(uint32_t));
    if (snapshots[slot].fat_copy == NULL) {
        __sync_lock_release(&snapshot_lk);
        return -1;
    }
    
    /* 复制 FAT 表 */
    for (uint32_t i = 0; i < fat_entries; i++) {
        snapshots[slot].fat_copy[i] = fat32_read_fat_entry(i);
    }
    
    /* 设置快照信息 */
    static uint32_t next_id = 1;
    snapshots[slot].id = next_id++;
    strncpy(snapshots[slot].name, name, 31);
    snapshots[slot].name[31] = '\0';
    snapshots[slot].create_time = get_tick_count();
    snapshots[slot].root_cluster_copy = fat32.root_cluster;
    snapshots[slot].fat_entries = fat_entries;
    snapshots[slot].active = 1;
    
    __sync_lock_release(&snapshot_lk);
    
    print("[FAT32] Created snapshot: ");
    println((char *)name);
    
    return snapshots[slot].id;
}

/* 恢复快照 */
int fat32_restore_snapshot(uint32_t id) {
    while (__sync_lock_test_and_set(&snapshot_lk, 1)) {
        asm volatile("pause");
    }
    
    /* 查找快照 */
    int found = -1;
    for (int i = 0; i < FAT32_SNAPSHOT_MAX; i++) {
        if (snapshots[i].active && snapshots[i].id == id) {
            found = i;
            break;
        }
    }
    
    if (found < 0) {
        __sync_lock_release(&snapshot_lk);
        println("[FAT32] Snapshot not found");
        return -1;
    }
    
    /* 恢复 FAT 表 */
    for (uint32_t i = 0; i < snapshots[found].fat_entries; i++) {
        fat32_write_fat_entry(i, snapshots[found].fat_copy[i]);
    }
    
    fat32_flush_fat_cache();
    
    print("[FAT32] Restored snapshot: ");
    println(snapshots[found].name);
    
    __sync_lock_release(&snapshot_lk);
    
    return 0;
}

/* 删除快照 */
int fat32_delete_snapshot(uint32_t id) {
    while (__sync_lock_test_and_set(&snapshot_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < FAT32_SNAPSHOT_MAX; i++) {
        if (snapshots[i].active && snapshots[i].id == id) {
            if (snapshots[i].fat_copy) {
                kfree(snapshots[i].fat_copy);
                snapshots[i].fat_copy = NULL;
            }
            snapshots[i].active = 0;
            
            __sync_lock_release(&snapshot_lk);
            
            print("[FAT32] Deleted snapshot ");
            print_dec(id);
            println("");
            
            return 0;
        }
    }
    
    __sync_lock_release(&snapshot_lk);
    return -1;
}

/* 列出快照 */
void fat32_list_snapshots(void) {
    println("=== FAT32 Snapshots ===");
    
    for (int i = 0; i < FAT32_SNAPSHOT_MAX; i++) {
        if (snapshots[i].active) {
            print("  ID: ");
            print_dec(snapshots[i].id);
            print(", Name: ");
            print(snapshots[i].name);
            print(", Created: ");
            print_dec(snapshots[i].create_time);
            println("");
        }
    }
}

/* ========== 数据恢复工具 ========== */

/* 查找已删除文件 */
int fat32_find_deleted_files(uint32_t parent_cluster) {
    println("[FAT32] Scanning for deleted files...");
    
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) return -1;
    
    uint32_t found = 0;
    uint32_t cluster = parent_cluster;
    
    while (IS_VALID_CLUSTER(cluster) && !IS_EOF_CLUSTER(cluster)) {
        fat32_read_cluster(cluster, buf);
        struct fat32_dirent *dirent = (struct fat32_dirent *)buf;
        
        for (uint32_t i = 0; i < fat32.bytes_per_cluster / sizeof(struct fat32_dirent); i++) {
            /* 0xE5 表示已删除 */
            if (dirent[i].name[0] == 0xE5) {
                uint32_t file_cluster = ((uint32_t)dirent[i].cluster_high << 16) | dirent[i].cluster_low;
                
                /* 检查簇是否仍有效 */
                if (file_cluster >= 2 && fat32_read_fat_entry(file_cluster) != 0) {
                    print("  Found deleted file at cluster ");
                    print_dec(file_cluster);
                    print(", size: ");
                    print_dec(dirent[i].file_size);
                    println(" bytes");
                    found++;
                }
            }
        }
        
        cluster = fat32_next_cluster(cluster);
    }
    
    kfree(buf);
    
    print("[FAT32] Found ");
    print_dec(found);
    println(" potentially recoverable files");
    
    return found;
}

/* 恢复已删除文件 */
int fat32_recover_deleted_file(uint32_t parent_cluster, uint32_t file_cluster,
                                const char *new_name) {
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) return -1;
    
    uint32_t cluster = parent_cluster;
    
    while (IS_VALID_CLUSTER(cluster) && !IS_EOF_CLUSTER(cluster)) {
        fat32_read_cluster(cluster, buf);
        struct fat32_dirent *dirent = (struct fat32_dirent *)buf;
        
        for (uint32_t i = 0; i < fat32.bytes_per_cluster / sizeof(struct fat32_dirent); i++) {
            if (dirent[i].name[0] == 0xE5) {
                uint32_t fc = ((uint32_t)dirent[i].cluster_high << 16) | dirent[i].cluster_low;
                
                if (fc == file_cluster) {
                    /* 恢复文件名 */
                    if (new_name != NULL) {
                        /* 转换新名称为 8.3 格式 */
                        char fat_name[11];
                        for (int j = 0; j < 11; j++) fat_name[j] = ' ';
                        const char *dot = new_name;
                        int j;
                        for (j = 0; j < 8 && *dot && *dot != '.'; j++, dot++) {
                            fat_name[j] = (*dot >= 'a' && *dot <= 'z') ? *dot - 32 : *dot;
                        }
                        if (*dot == '.') {
                            dot++;
                            for (j = 8; j < 11 && *dot; j++, dot++) {
                                fat_name[j] = (*dot >= 'a' && *dot <= 'z') ? *dot - 32 : *dot;
                            }
                        }
                        for (int j = 0; j < 11; j++) {
                            dirent[i].name[j] = fat_name[j];
                        }
                    } else {
                        /* 恢复原名称 (将 0xE5 替换为 '_' 或原字符) */
                        dirent[i].name[0] = '_';
                    }
                    
                    /* 写回目录 */
                    int result = fat32_write_cluster(cluster, buf);
                    kfree(buf);
                    
                    if (result == 0) {
                        println("[FAT32] File recovered successfully");
                        return 0;
                    }
                    return -1;
                }
            }
        }
        
        cluster = fat32_next_cluster(cluster);
    }
    
    kfree(buf);
    println("[FAT32] Deleted file entry not found");
    return -1;
}

/* ========== 事务性写入支持 ========== */

#define FAT32_TX_MAX_OPS        64

enum fat32_tx_op_type {
    TX_OP_WRITE_FAT,
    TX_OP_WRITE_CLUSTER,
    TX_OP_WRITE_DIRENT
};

struct fat32_tx_op {
    enum fat32_tx_op_type type;
    uint32_t cluster;
    uint32_t value;          /* FAT 值 */
    uint8_t *data;           /* 簇数据 */
    uint32_t data_size;
    uint8_t *old_data;       /* 原始数据 (用于回滚) */
};

struct fat32_transaction {
    struct fat32_tx_op ops[FAT32_TX_MAX_OPS];
    uint32_t op_count;
    int active;
    int committed;
};

static struct fat32_transaction current_tx;
static volatile int tx_lk = 0;

/* 开始事务 */
int fat32_begin_transaction(void) {
    while (__sync_lock_test_and_set(&tx_lk, 1)) {
        asm volatile("pause");
    }
    
    if (current_tx.active) {
        __sync_lock_release(&tx_lk);
        println("[FAT32] Transaction already active");
        return -1;
    }
    
    current_tx.op_count = 0;
    current_tx.active = 1;
    current_tx.committed = 0;
    
    __sync_lock_release(&tx_lk);
    
    println("[FAT32] Transaction started");
    return 0;
}

/* 添加 FAT 写入操作 */
int fat32_tx_write_fat(uint32_t cluster, uint32_t value) {
    if (!current_tx.active || current_tx.op_count >= FAT32_TX_MAX_OPS) {
        return -1;
    }
    
    struct fat32_tx_op *op = &current_tx.ops[current_tx.op_count];
    op->type = TX_OP_WRITE_FAT;
    op->cluster = cluster;
    op->value = value;
    op->old_data = kmalloc(4);
    if (op->old_data) {
        *(uint32_t *)op->old_data = fat32_read_fat_entry(cluster);
    }
    
    current_tx.op_count++;
    return 0;
}

/* 添加簇写入操作 */
int fat32_tx_write_cluster(uint32_t cluster, const void *data, uint32_t size) {
    if (!current_tx.active || current_tx.op_count >= FAT32_TX_MAX_OPS) {
        return -1;
    }
    
    struct fat32_tx_op *op = &current_tx.ops[current_tx.op_count];
    op->type = TX_OP_WRITE_CLUSTER;
    op->cluster = cluster;
    op->data = kmalloc(size);
    op->data_size = size;
    
    if (op->data) {
        for (uint32_t i = 0; i < size; i++) {
            op->data[i] = ((uint8_t *)data)[i];
        }
    }
    
    op->old_data = kmalloc(size);
    if (op->old_data) {
        fat32_read_cluster(cluster, op->old_data);
    }
    
    current_tx.op_count++;
    return 0;
}

/* 提交事务 */
int fat32_commit_transaction(void) {
    while (__sync_lock_test_and_set(&tx_lk, 1)) {
        asm volatile("pause");
    }
    
    if (!current_tx.active) {
        __sync_lock_release(&tx_lk);
        return -1;
    }
    
    /* 执行所有操作 */
    for (uint32_t i = 0; i < current_tx.op_count; i++) {
        struct fat32_tx_op *op = &current_tx.ops[i];
        
        switch (op->type) {
            case TX_OP_WRITE_FAT:
                fat32_write_fat_entry(op->cluster, op->value);
                break;
            case TX_OP_WRITE_CLUSTER:
                if (op->data) {
                    fat32_write_cluster(op->cluster, op->data);
                    kfree(op->data);
                    op->data = NULL;
                }
                break;
            default:
                break;
        }
        
        if (op->old_data) {
            kfree(op->old_data);
            op->old_data = NULL;
        }
    }
    
    fat32_flush_fat_cache();
    
    current_tx.active = 0;
    current_tx.committed = 1;
    current_tx.op_count = 0;
    
    __sync_lock_release(&tx_lk);
    
    println("[FAT32] Transaction committed");
    return 0;
}

/* 回滚事务 */
int fat32_rollback_transaction(void) {
    while (__sync_lock_test_and_set(&tx_lk, 1)) {
        asm volatile("pause");
    }
    
    if (!current_tx.active) {
        __sync_lock_release(&tx_lk);
        return -1;
    }
    
    /* 按逆序回滚操作 */
    for (int i = (int)current_tx.op_count - 1; i >= 0; i--) {
        struct fat32_tx_op *op = &current_tx.ops[i];
        
        if (op->old_data) {
            switch (op->type) {
                case TX_OP_WRITE_FAT:
                    fat32_write_fat_entry(op->cluster, *(uint32_t *)op->old_data);
                    break;
                case TX_OP_WRITE_CLUSTER:
                    fat32_write_cluster(op->cluster, op->old_data);
                    break;
                default:
                    break;
            }
            kfree(op->old_data);
            op->old_data = NULL;
        }
        
        if (op->data) {
            kfree(op->data);
            op->data = NULL;
        }
    }
    
    fat32_flush_fat_cache();
    
    current_tx.active = 0;
    current_tx.committed = 0;
    current_tx.op_count = 0;
    
    __sync_lock_release(&tx_lk);
    
    println("[FAT32] Transaction rolled back");
    return 0;
}

/* ========== FAT32 综合诊断报告 ========== */

void fat32_print_diagnostic_report(void) {
    println("");
    println("========================================");
    println("        FAT32 Filesystem Report        ");
    println("========================================");
    println("");
    
    /* 基本信息 */
    println("--- Basic Information ---");
    print("Bytes per Sector: ");
    print_dec(fat32.bytes_per_sector);
    println("");
    
    print("Sectors per Cluster: ");
    print_dec(fat32.sectors_per_cluster);
    println("");
    
    print("Bytes per Cluster: ");
    print_dec(fat32.bytes_per_cluster);
    println("");
    
    print("Total Clusters: ");
    print_dec(fat32.total_clusters);
    println("");
    
    print("Total Size: ");
    print_dec(fat32.total_clusters * fat32.bytes_per_cluster / (1024 * 1024));
    println(" MB");
    
    print("Root Cluster: ");
    print_dec(fat32.root_cluster);
    println("");
    
    print("FAT Start: ");
    print_dec(fat32.fat_start);
    println("");
    
    print("Data Start: ");
    print_dec(fat32.data_start);
    println("");
    
    /* 统计信息 */
    println("");
    println("--- Statistics ---");
    
    uint32_t used_clusters = 0;
    uint32_t free_clusters = 0;
    uint32_t bad_clusters = 0;
    
    for (uint32_t c = 2; c < fat32.total_clusters + 2; c++) {
        uint32_t entry = fat32_read_fat_entry(c);
        if (entry == 0) {
            free_clusters++;
        } else if (entry == 0x0FFFFFF7) {
            bad_clusters++;
        } else {
            used_clusters++;
        }
    }
    
    print("Used Clusters: ");
    print_dec(used_clusters);
    println("");
    
    print("Free Clusters: ");
    print_dec(free_clusters);
    println("");
    
    print("Bad Clusters: ");
    print_dec(bad_clusters);
    println("");
    
    print("Used Space: ");
    print_dec(used_clusters * fat32.bytes_per_cluster / (1024 * 1024));
    println(" MB");
    
    print("Free Space: ");
    print_dec(free_clusters * fat32.bytes_per_cluster / (1024 * 1024));
    println(" MB");
    
    /* 碎片分析 */
    println("");
    println("--- Fragmentation Analysis ---");
    
    struct fat32_fragment_info frag_info;
    if (fat32_analyze_fragmentation(fat32.root_cluster, &frag_info) == 0) {
        print("Root Dir Clusters: ");
        print_dec(frag_info.total_clusters);
        println("");
        
        print("Fragmentation Score: ");
        print_dec(frag_info.fragmentation_score);
        println("%");
    }
    
    /* 缓存状态 */
    println("");
    println("--- Cache Status ---");
    
    int cache2_valid = 0;
    int cache2_dirty = 0;
    for (int s = 0; s < FAT_CACHE2_SETS; s++) {
        for (int w = 0; w < FAT_CACHE2_WAYS; w++) {
            if (fat_cache2[s][w].valid) cache2_valid++;
            if (fat_cache2[s][w].dirty) cache2_dirty++;
        }
    }
    
    print("L2 Cache Entries: ");
    print_dec(cache2_valid);
    print("/");
    print_dec(FAT_CACHE2_SETS * FAT_CACHE2_WAYS);
    println("");
    
    print("L2 Cache Dirty: ");
    print_dec(cache2_dirty);
    println("");
    
    /* 快照信息 */
    println("");
    println("--- Snapshots ---");
    
    int active_snapshots = 0;
    for (int i = 0; i < FAT32_SNAPSHOT_MAX; i++) {
        if (snapshots[i].active) active_snapshots++;
    }
    
    print("Active Snapshots: ");
    print_dec(active_snapshots);
    print("/");
    print_dec(FAT32_SNAPSHOT_MAX);
    println("");
    
    /* 性能统计 */
    println("");
    println("--- Performance ---");
    fat32_print_perf_stats();
    
    println("");
    println("========================================");
    println("          End of Report");
    println("========================================");
    println("");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * FAT 缓存增强 - 多路组相联 FAT 表缓存
 * 替代原有的单扇区缓存，提供更高的缓存命中率
 * ═══════════════════════════════════════════════════════════════════════════ */

/* FAT 缓存实例 */
static struct fat_cache_line fat_cache_lines[FAT_CACHE_SETS][FAT_CACHE_WAYS];
static volatile int fat_cache_enhanced_lk = 0;
static uint32_t fat_cache_lru_counter = 0;
static struct fat_cache_stats fat_cache_enhanced_stats;

/* 初始化 FAT 缓存 */
void fat32_fat_cache_init(void) {
    for (int s = 0; s < FAT_CACHE_SETS; s++) {
        for (int w = 0; w < FAT_CACHE_WAYS; w++) {
            fat_cache_lines[s][w].valid = 0;
            fat_cache_lines[s][w].dirty = 0;
            fat_cache_lines[s][w].sector_num = 0;
            fat_cache_lines[s][w].lru_counter = 0;
        }
    }
    memset(&fat_cache_enhanced_stats, 0, sizeof(fat_cache_enhanced_stats));
}

/* 从 FAT 缓存读取扇区 */
int fat32_fat_cache_read(uint32_t fat_sector, uint32_t *data) {
    uint32_t set_idx = fat_sector % FAT_CACHE_SETS;
    
    while (__sync_lock_test_and_set(&fat_cache_enhanced_lk, 1)) {
        asm volatile("pause");
    }
    
    fat_cache_enhanced_stats.reads++;
    
    /* 在组内查找 */
    for (int w = 0; w < FAT_CACHE_WAYS; w++) {
        if (fat_cache_lines[set_idx][w].valid && 
            fat_cache_lines[set_idx][w].sector_num == fat_sector) {
            /* 缓存命中 */
            fat_cache_enhanced_stats.hits++;
            fat_cache_lines[set_idx][w].lru_counter = ++fat_cache_lru_counter;
            memcpy(data, fat_cache_lines[set_idx][w].data, 
                   FAT_CACHE_SECTOR_U32 * sizeof(uint32_t));
            __sync_lock_release(&fat_cache_enhanced_lk);
            return 0;
        }
    }
    
    /* 缓存未命中 - 从磁盘读取 */
    fat_cache_enhanced_stats.misses++;
    
    /* 查找空闲路或 LRU 替换 */
    int victim = 0;
    uint32_t min_lru = 0xFFFFFFFF;
    
    for (int w = 0; w < FAT_CACHE_WAYS; w++) {
        if (!fat_cache_lines[set_idx][w].valid) {
            victim = w;
            break;
        }
        if (fat_cache_lines[set_idx][w].lru_counter < min_lru) {
            min_lru = fat_cache_lines[set_idx][w].lru_counter;
            victim = w;
        }
    }
    
    /* 如果替换脏行，先写回 */
    if (fat_cache_lines[set_idx][victim].valid && 
        fat_cache_lines[set_idx][victim].dirty) {
        uint64_t lba = fat32.partition_start + fat32.fat_start + 
                       fat_cache_lines[set_idx][victim].sector_num;
        disk_write_sector(lba, fat_cache_lines[set_idx][victim].data);
        fat_cache_enhanced_stats.writebacks++;
        fat_cache_lines[set_idx][victim].dirty = 0;
    }
    
    /* 从磁盘读取到缓存行 */
    uint64_t lba = fat32.partition_start + fat32.fat_start + fat_sector;
    if (disk_read_sector(lba, fat_cache_lines[set_idx][victim].data) != 0) {
        __sync_lock_release(&fat_cache_enhanced_lk);
        return -1;
    }
    
    fat_cache_lines[set_idx][victim].sector_num = fat_sector;
    fat_cache_lines[set_idx][victim].valid = 1;
    fat_cache_lines[set_idx][victim].dirty = 0;
    fat_cache_lines[set_idx][victim].lru_counter = ++fat_cache_lru_counter;
    
    /* 复制到输出 */
    memcpy(data, fat_cache_lines[set_idx][victim].data,
           FAT_CACHE_SECTOR_U32 * sizeof(uint32_t));
    
    __sync_lock_release(&fat_cache_enhanced_lk);
    return 0;
}

/* 向 FAT 缓存写入扇区 */
void fat32_fat_cache_write(uint32_t fat_sector, uint32_t *data) {
    uint32_t set_idx = fat_sector % FAT_CACHE_SETS;
    
    while (__sync_lock_test_and_set(&fat_cache_enhanced_lk, 1)) {
        asm volatile("pause");
    }
    
    /* 查找现有条目 */
    for (int w = 0; w < FAT_CACHE_WAYS; w++) {
        if (fat_cache_lines[set_idx][w].valid && 
            fat_cache_lines[set_idx][w].sector_num == fat_sector) {
            /* 更新现有缓存行 */
            memcpy(fat_cache_lines[set_idx][w].data, data,
                   FAT_CACHE_SECTOR_U32 * sizeof(uint32_t));
            fat_cache_lines[set_idx][w].dirty = 1;
            fat_cache_lines[set_idx][w].lru_counter = ++fat_cache_lru_counter;
            __sync_lock_release(&fat_cache_enhanced_lk);
            return;
        }
    }
    
    /* 查找空闲路或 LRU 替换 */
    int victim = 0;
    uint32_t min_lru = 0xFFFFFFFF;
    
    for (int w = 0; w < FAT_CACHE_WAYS; w++) {
        if (!fat_cache_lines[set_idx][w].valid) {
            victim = w;
            break;
        }
        if (fat_cache_lines[set_idx][w].lru_counter < min_lru) {
            min_lru = fat_cache_lines[set_idx][w].lru_counter;
            victim = w;
        }
    }
    
    /* 替换脏行先写回 */
    if (fat_cache_lines[set_idx][victim].valid && 
        fat_cache_lines[set_idx][victim].dirty) {
        uint64_t lba = fat32.partition_start + fat32.fat_start + 
                       fat_cache_lines[set_idx][victim].sector_num;
        disk_write_sector(lba, fat_cache_lines[set_idx][victim].data);
        fat_cache_enhanced_stats.writebacks++;
    }
    
    fat_cache_lines[set_idx][victim].sector_num = fat_sector;
    memcpy(fat_cache_lines[set_idx][victim].data, data,
           FAT_CACHE_SECTOR_U32 * sizeof(uint32_t));
    fat_cache_lines[set_idx][victim].valid = 1;
    fat_cache_lines[set_idx][victim].dirty = 1;
    fat_cache_lines[set_idx][victim].lru_counter = ++fat_cache_lru_counter;
    
    __sync_lock_release(&fat_cache_enhanced_lk);
}

/* 刷新 FAT 缓存 (写回所有脏行) */
int fat32_fat_cache_flush(void) {
    int flushed = 0;
    
    while (__sync_lock_test_and_set(&fat_cache_enhanced_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int s = 0; s < FAT_CACHE_SETS; s++) {
        for (int w = 0; w < FAT_CACHE_WAYS; w++) {
            if (fat_cache_lines[s][w].valid && fat_cache_lines[s][w].dirty) {
                uint64_t lba = fat32.partition_start + fat32.fat_start + 
                               fat_cache_lines[s][w].sector_num;
                /* 写入所有 FAT 表副本 */
                for (uint8_t i = 0; i < fat32.num_fats; i++) {
                    disk_write_sector(lba + (uint32_t)i * fat32.sectors_per_fat,
                                      fat_cache_lines[s][w].data);
                }
                fat_cache_lines[s][w].dirty = 0;
                fat_cache_enhanced_stats.writebacks++;
                flushed++;
            }
        }
    }
    
    __sync_lock_release(&fat_cache_enhanced_lk);
    return flushed;
}

/* 获取 FAT 缓存统计 */
void fat32_fat_cache_get_stats(struct fat_cache_stats *stats) {
    if (stats != NULL) {
        *stats = fat_cache_enhanced_stats;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 目录遍历哈希表 - 大目录加速查找
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 目录哈希溢出池 */
#define DIR_HASH_OVERFLOW_SIZE  512
static struct dir_hash_entry dir_hash_overflow_pool[DIR_HASH_OVERFLOW_SIZE];
static int dir_hash_overflow_initialized = 0;

/* 初始化溢出池 */
static void dir_hash_overflow_init(void) {
    if (dir_hash_overflow_initialized) return;
    for (int i = 0; i < DIR_HASH_OVERFLOW_SIZE; i++) {
        dir_hash_overflow_pool[i].valid = 0;
        dir_hash_overflow_pool[i].next = NULL;
    }
    dir_hash_overflow_initialized = 1;
}

/* 目录名哈希函数 */
static uint32_t dir_name_hash(const char *name) {
    uint32_t hash = 5381;
    while (*name) {
        hash = ((hash << 5) + hash) + (uint8_t)*name;
        name++;
    }
    return hash;
}

/* 初始化目录哈希表 */
void fat32_dir_hash_init(struct dir_hash_table *table, uint32_t parent_cluster) {
    if (table == NULL) return;
    
    dir_hash_overflow_init();
    
    for (int i = 0; i < DIR_HASH_SIZE; i++) {
        table->buckets[i].valid = 0;
        table->buckets[i].next = NULL;
    }
    table->overflow = NULL;
    table->count = 0;
    table->parent_cluster = parent_cluster;
    table->valid = 1;
}

/* 在哈希表中查找 */
struct dir_hash_entry *fat32_dir_hash_lookup(struct dir_hash_table *table, const char *name) {
    if (table == NULL || name == NULL || !table->valid) return NULL;
    
    uint32_t hash = dir_name_hash(name) % DIR_HASH_SIZE;
    struct dir_hash_entry *entry = &table->buckets[hash];
    
    /* 检查桶内第一个条目 */
    if (entry->valid && compare_filename(name, entry->name)) {
        return entry;
    }
    
    /* 检查链表 */
    entry = table->buckets[hash].next;
    while (entry != NULL) {
        if (entry->valid && compare_filename(name, entry->name)) {
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

/* 向哈希表添加条目 */
int fat32_dir_hash_add(struct dir_hash_table *table, const char *name,
                       uint32_t cluster, uint32_t dir_offset,
                       uint8_t attr, uint32_t file_size) {
    if (table == NULL || name == NULL || !table->valid) return -1;
    
    uint32_t hash = dir_name_hash(name) % DIR_HASH_SIZE;
    
    /* 如果桶为空，直接使用 */
    if (!table->buckets[hash].valid) {
        strncpy(table->buckets[hash].name, name, 255);
        table->buckets[hash].name[255] = '\0';
        table->buckets[hash].cluster = cluster;
        table->buckets[hash].parent_cluster = table->parent_cluster;
        table->buckets[hash].dir_offset = dir_offset;
        table->buckets[hash].attr = attr;
        table->buckets[hash].file_size = file_size;
        table->buckets[hash].valid = 1;
        table->buckets[hash].next = NULL;
        table->count++;
        return 0;
    }
    
    /* 从溢出池分配 */
    struct dir_hash_entry *new_entry = NULL;
    for (int i = 0; i < DIR_HASH_OVERFLOW_SIZE; i++) {
        if (!dir_hash_overflow_pool[i].valid) {
            new_entry = &dir_hash_overflow_pool[i];
            break;
        }
    }
    
    if (new_entry == NULL) return -1;  /* 溢出池满 */
    
    strncpy(new_entry->name, name, 255);
    new_entry->name[255] = '\0';
    new_entry->cluster = cluster;
    new_entry->parent_cluster = table->parent_cluster;
    new_entry->dir_offset = dir_offset;
    new_entry->attr = attr;
    new_entry->file_size = file_size;
    new_entry->valid = 1;
    
    /* 添加到链表头部 */
    new_entry->next = table->buckets[hash].next;
    table->buckets[hash].next = new_entry;
    table->count++;
    
    return 0;
}

/* 构建目录哈希表 (从磁盘读取整个目录) */
int fat32_dir_hash_build(struct dir_hash_table *table, uint32_t dir_cluster) {
    if (table == NULL) return -1;
    
    fat32_dir_hash_init(table, dir_cluster);
    
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) return -1;
    
    uint32_t cluster = dir_cluster;
    uint32_t dir_offset = 0;
    
    while (IS_VALID_CLUSTER(cluster) && !IS_EOF_CLUSTER(cluster)) {
        if (fat32_read_cluster(cluster, buf) != 0) {
            kfree(buf);
            return -1;
        }
        
        struct fat32_dirent *dirent = (struct fat32_dirent *)buf;
        int entries_per_cluster = fat32.bytes_per_cluster / sizeof(struct fat32_dirent);
        
        for (int i = 0; i < entries_per_cluster; i++) {
            if (dirent[i].name[0] == 0x00) {
                kfree(buf);
                return 0;  /* 目录结束 */
            }
            if (dirent[i].name[0] == 0xE5) continue;
            if (dirent[i].attr & FAT_ATTR_LFN) continue;
            if (dirent[i].attr & FAT_ATTR_VOLUME_ID) continue;
            
            /* 提取文件名 */
            char name[256];
            int k;
            for (k = 0; k < 8 && dirent[i].name[k] != ' '; k++) {
                name[k] = dirent[i].name[k];
            }
            if (dirent[i].name[8] != ' ') {
                name[k++] = '.';
                for (int l = 8; l < 11 && dirent[i].name[l] != ' '; l++) {
                    name[k++] = dirent[i].name[l];
                }
            }
            name[k] = '\0';
            
            /* 转小写 */
            for (k = 0; name[k]; k++) {
                if (name[k] >= 'A' && name[k] <= 'Z') {
                    name[k] += 32;
                }
            }
            
            uint32_t file_cluster = ((uint32_t)dirent[i].cluster_high << 16) | 
                                     dirent[i].cluster_low;
            
            fat32_dir_hash_add(table, name, file_cluster, dir_offset,
                              dirent[i].attr, dirent[i].file_size);
            dir_offset++;
        }
        
        cluster = fat32_next_cluster(cluster);
    }
    
    kfree(buf);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LFN 长文件名完善 - 处理所有 LFN 边界情况
 * ═══════════════════════════════════════════════════════════════════════════ */

/* LFN 校验和验证 - 验证 LFN 条目与短文件名的匹配 */
static int fat32_lfn_verify_checksum(const struct lfn_dirent *lfn, const struct fat32_dirent *short_entry) {
    if (lfn == NULL || short_entry == NULL) return 0;
    
    uint8_t expected = lfn_checksum(short_entry->name);
    return (lfn->checksum == expected) ? 1 : 0;
}

/* LFN 跨簇处理 - 处理 LFN 条目跨越簇边界的情况 */
int fat32_lfn_read_cross_cluster(uint32_t start_cluster, int start_index,
                                  int num_lfn, char *name_out, int max_len) {
    if (name_out == NULL || max_len <= 0) return -1;
    
    int entries_per_cluster = fat32.bytes_per_cluster / sizeof(struct fat32_dirent);
    uint8_t *buf = kmalloc(fat32.bytes_per_cluster);
    if (buf == NULL) return -1;
    
    int name_pos = 0;
    uint32_t cluster = start_cluster;
    int current_index = start_index;
    
    /* 定位到起始簇 */
    while (current_index >= entries_per_cluster) {
        cluster = fat32_next_cluster(cluster);
        if (!IS_VALID_CLUSTER(cluster) || IS_EOF_CLUSTER(cluster)) {
            kfree(buf);
            return -1;
        }
        current_index -= entries_per_cluster;
    }
    
    /* 从最后一个 LFN 条目开始读取 (序号最小) */
    for (int entry = 0; entry < num_lfn && name_pos < max_len - 1; entry++) {
        /* 读取当前簇 */
        if (fat32_read_cluster(cluster, buf) != 0) {
            kfree(buf);
            return -1;
        }
        
        struct lfn_dirent *lfn = (struct lfn_dirent *)buf + current_index;
        
        /* 验证是 LFN 条目 */
        if ((lfn->attr & FAT_ATTR_LFN) != FAT_ATTR_LFN) {
            kfree(buf);
            return -1;
        }
        
        /* 提取名称字符 */
        for (int i = 0; i < 5 && name_pos < max_len - 1; i++) {
            uint16_t c = lfn->name1[i];
            if (c == 0 || c == 0xFFFF) continue;
            name_out[name_pos++] = (c < 0x80) ? (char)c : '?';
        }
        for (int i = 0; i < 6 && name_pos < max_len - 1; i++) {
            uint16_t c = lfn->name2[i];
            if (c == 0 || c == 0xFFFF) continue;
            name_out[name_pos++] = (c < 0x80) ? (char)c : '?';
        }
        for (int i = 0; i < 2 && name_pos < max_len - 1; i++) {
            uint16_t c = lfn->name3[i];
            if (c == 0 || c == 0xFFFF) continue;
            name_out[name_pos++] = (c < 0x80) ? (char)c : '?';
        }
        
        /* 移动到下一个条目 */
        current_index++;
        if (current_index >= entries_per_cluster) {
            cluster = fat32_next_cluster(cluster);
            if (!IS_VALID_CLUSTER(cluster) || IS_EOF_CLUSTER(cluster)) {
                break;
            }
            current_index = 0;
        }
    }
    
    name_out[name_pos] = '\0';
    kfree(buf);
    return name_pos;
}

/* LFN 名称规范化 - 处理尾随填充和终止符 */
void fat32_lfn_normalize(char *name, int len) {
    if (name == NULL || len <= 0) return;
    
    /* 从末尾去除 0xFFFF 填充和空格 */
    int i = len - 1;
    while (i >= 0) {
        if ((uint8_t)name[i] == 0xFF || name[i] == ' ' || name[i] == '\0') {
            i--;
        } else {
            break;
        }
    }
    name[i + 1] = '\0';
    
    /* 去除前导空格 */
    int start = 0;
    while (name[start] == ' ') start++;
    if (start > 0) {
        int j = 0;
        while (name[start + j]) {
            name[j] = name[start + j];
            j++;
        }
        name[j] = '\0';
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 写优化 - 延迟写入 / 写回缓存
 * 将簇写入操作缓存，延迟到定时器触发或缓存满时写回磁盘
 * ═══════════════════════════════════════════════════════════════════════════ */

static struct fat32_writeback_cache writeback_cache;
static volatile int writeback_lk = 0;

/* 初始化写回缓存 */
void fat32_writeback_init(void) {
    memset(&writeback_cache, 0, sizeof(writeback_cache));
    writeback_cache.enabled = 1;
    writeback_cache.last_flush_time = get_tick_count();
    
    for (int i = 0; i < FAT32_DIRTY_CLUSTER_MAX; i++) {
        writeback_cache.entries[i].data = NULL;
        writeback_cache.entries[i].valid = 0;
    }
}

/* 将簇加入写回缓存 (延迟写入) */
int fat32_writeback_add(uint32_t cluster, const uint8_t *data) {
    if (data == NULL || !writeback_cache.enabled) {
        /* 缓存未启用，直接写入磁盘 */
        return fat32_write_cluster(cluster, data);
    }
    
    while (__sync_lock_test_and_set(&writeback_lk, 1)) {
        asm volatile("pause");
    }
    
    /* 检查是否已在缓存中 */
    for (uint32_t i = 0; i < writeback_cache.count; i++) {
        if (writeback_cache.entries[i].valid && 
            writeback_cache.entries[i].cluster == cluster) {
            /* 更新现有条目 */
            if (writeback_cache.entries[i].data != NULL) {
                memcpy(writeback_cache.entries[i].data, data, fat32.bytes_per_cluster);
            }
            writeback_cache.entries[i].dirty_time = get_tick_count();
            __sync_lock_release(&writeback_lk);
            return 0;
        }
    }
    
    /* 缓存满时先刷新 */
    if (writeback_cache.count >= FAT32_DIRTY_CLUSTER_MAX) {
        __sync_lock_release(&writeback_lk);
        fat32_writeback_flush();
        
        while (__sync_lock_test_and_set(&writeback_lk, 1)) {
            asm volatile("pause");
        }
    }
    
    /* 查找空闲槽位 */
    int slot = -1;
    for (int i = 0; i < FAT32_DIRTY_CLUSTER_MAX; i++) {
        if (!writeback_cache.entries[i].valid) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        __sync_lock_release(&writeback_lk);
        return fat32_write_cluster(cluster, data);
    }
    
    /* 分配数据缓冲区 */
    if (writeback_cache.entries[slot].data == NULL) {
        writeback_cache.entries[slot].data = kmalloc(fat32.bytes_per_cluster);
        if (writeback_cache.entries[slot].data == NULL) {
            __sync_lock_release(&writeback_lk);
            return fat32_write_cluster(cluster, data);
        }
    }
    
    writeback_cache.entries[slot].cluster = cluster;
    memcpy(writeback_cache.entries[slot].data, data, fat32.bytes_per_cluster);
    writeback_cache.entries[slot].dirty_time = get_tick_count();
    writeback_cache.entries[slot].valid = 1;
    writeback_cache.count++;
    
    __sync_lock_release(&writeback_lk);
    return 0;
}

/* 刷新写回缓存 (将所有脏簇写入磁盘) */
int fat32_writeback_flush(void) {
    int flushed = 0;
    
    while (__sync_lock_test_and_set(&writeback_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < FAT32_DIRTY_CLUSTER_MAX; i++) {
        if (writeback_cache.entries[i].valid) {
            if (writeback_cache.entries[i].data != NULL) {
                fat32_write_cluster(writeback_cache.entries[i].cluster,
                                     writeback_cache.entries[i].data);
                flushed++;
            }
            writeback_cache.entries[i].valid = 0;
        }
    }
    
    writeback_cache.count = 0;
    writeback_cache.last_flush_time = get_tick_count();
    
    __sync_lock_release(&writeback_lk);
    return flushed;
}

/* 定时刷新 (由内核定时器调用) */
void fat32_writeback_timer(void) {
    if (!writeback_cache.enabled) return;
    
    uint64_t now = get_tick_count();
    /* 简化: 假设 tick 约为毫秒级 */
    if (now - writeback_cache.last_flush_time >= FAT32_WRITEBACK_DELAY_MS) {
        fat32_writeback_flush();
    }
}

/* 查找写回缓存中的簇 */
int fat32_writeback_lookup(uint32_t cluster, uint8_t *data) {
    if (data == NULL) return -1;
    
    while (__sync_lock_test_and_set(&writeback_lk, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < FAT32_DIRTY_CLUSTER_MAX; i++) {
        if (writeback_cache.entries[i].valid && 
            writeback_cache.entries[i].cluster == cluster &&
            writeback_cache.entries[i].data != NULL) {
            memcpy(data, writeback_cache.entries[i].data, fat32.bytes_per_cluster);
            __sync_lock_release(&writeback_lk);
            return 0;
        }
    }
    
    __sync_lock_release(&writeback_lk);
    return -1;  /* 未在缓存中找到 */
}
