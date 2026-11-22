#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"   /* 引入统一的 ssize_t 等类型定义 */

/* 文件类型 */
#define VFS_TYPE_FILE       0
#define VFS_TYPE_DIR        1
#define VFS_TYPE_DEVICE     2
#define VFS_TYPE_SYMLINK    3
#define VFS_TYPE_SOCKET     4
#define VFS_TYPE_FIFO       5

/* 文件打开模式 */
#define VFS_MODE_READ       0x01
#define VFS_MODE_WRITE      0x02
#define VFS_MODE_EXEC       0x04
#define VFS_MODE_APPEND     0x08    /* 追加模式 */
#define VFS_MODE_CREATE     0x10    /* 创建文件 */
#define VFS_MODE_TRUNC      0x20    /* 截断文件 */
#define VFS_MODE_EXCL       0x40    /* 排他创建 */
#define VFS_MODE_RDWR       (VFS_MODE_READ | VFS_MODE_WRITE)

/* 文件属性 */
#define VFS_ATTR_RDONLY     0x01
#define VFS_ATTR_HIDDEN     0x02
#define VFS_ATTR_SYSTEM     0x04
#define VFS_ATTR_ARCHIVE    0x08
#define VFS_ATTR_TEMPORARY  0x10
#define VFS_ATTR_COMPRESSED 0x20
#define VFS_ATTR_ENCRYPTED  0x40

/* 文件标志 */
#define VFS_FLAG_NONBLOCK   0x01
#define VFS_FLAG_APPEND     0x02
#define VFS_FLAG_SYNC       0x04
#define VFS_FLAG_DSYNC      0x08
#define VFS_FLAG_DIRECT     0x10

/* seek 命令 */
#define VFS_SEEK_SET        0
#define VFS_SEEK_CUR        1
#define VFS_SEEK_END        2

/* Linux-style permission bits */
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

/* 错误码 */
#define VFS_OK              0
#define VFS_ERR_NOMEM      -1
#define VFS_ERR_NOTFOUND   -2
#define VFS_ERR_EXISTS     -3
#define VFS_ERR_PERM       -4
#define VFS_ERR_NOTDIR     -5
#define VFS_ERR_ISDIR      -6
#define VFS_ERR_NOSPC      -7
#define VFS_ERR_ROFS       -8
#define VFS_ERR_BUSY       -9
#define VFS_ERR_INVAL      -10
#define VFS_ERR_NOTSUP     -11    /* 不支持的操作 */
#define VFS_ERR_CROSSDEV   -12    /* 跨设备操作 */
#define VFS_ERR_NOENT      -13    /* 无此条目 */
#define VFS_ERR_ACCESS     -14    /* 访问被拒绝 */
#define VFS_ERR_IO         -15    /* I/O 错误 */
#define VFS_ERR_NAMETOOLONG -16   /* 名字过长 */
#define VFS_ERR_NOTEMPTY   -17    /* 目录非空 */
#define VFS_ERR_LOOP       -18    /* 符号链接循环 */

/* 文件信息结构 */
struct vfs_stat {
    uint64_t size;          /* 文件大小 */
    uint32_t type;          /* 文件类型 */
    uint32_t attr;          /* 文件属性 */
    uint64_t ctime;         /* 创建时间 */
    uint64_t mtime;         /* 修改时间 */
    uint64_t atime;         /* 访问时间 */
    uint32_t inode;         /* inode 号 */
    uint32_t nlink;         /* 硬链接数 */
    uint32_t uid;           /* 用户 ID */
    uint32_t gid;           /* 组 ID */
    uint32_t mode;          /* 权限模式 */
    uint32_t blksize;       /* 块大小 */
    uint64_t blocks;        /* 块数 */
};

/* 目录项 */
struct vfs_dirent {
    char name[256];         /* 文件名 */
    uint32_t type;          /* 文件类型 */
    uint32_t inode;         /* inode 号 */
    uint64_t size;          /* 文件大小 */
};

/* 文件描述符 */
struct vfs_file {
    char path[256];         /* 文件路径 */
    uint64_t offset;        /* 当前偏移 */
    uint32_t mode;          /* 打开模式 */
    uint32_t flags;         /* 标志 */
    void *fs_data;          /* 文件系统私有数据 */
    struct vfs_mount *mount;/* 所属挂载点 */
    uint32_t ref_count;     /* 引用计数 */
    uint8_t  is_dirty;      /* 是否有未写入的修改 */
    uint32_t readdir_index; /* per-fd readdir position */
};

/* 文件系统操作 */
struct vfs_ops {
    int (*open)(struct vfs_file *file, const char *path, uint32_t mode);
    int (*close)(struct vfs_file *file);
    ssize_t (*read)(struct vfs_file *file, void *buf, size_t count);
    ssize_t (*write)(struct vfs_file *file, const void *buf, size_t count);
    int (*seek)(struct vfs_file *file, uint64_t offset);
    int (*stat)(const char *path, struct vfs_stat *stat);
    int (*readdir)(const char *path, struct vfs_dirent *dirent, int index);
    int (*mkdir)(const char *path);
    int (*rmdir)(const char *path);
    int (*unlink)(const char *path);
    int (*rename)(const char *oldpath, const char *newpath);
    int (*fstat)(struct vfs_file *file, struct vfs_stat *stat);
    int (*sync)(struct vfs_file *file);
    int (*truncate)(const char *path, uint64_t length);
    int (*chmod)(const char *path, uint32_t mode);
    int (*chown)(const char *path, uint32_t uid, uint32_t gid);
};

/* 挂载点 */
struct vfs_mount {
    char path[256];         /* 挂载路径 */
    char device[64];        /* 设备名 */
    struct vfs_ops *ops;    /* 文件系统操作 */
    void *fs_data;          /* 文件系统私有数据 */
    struct vfs_mount *next; /* 下一个挂载点 */
    uint32_t flags;         /* 挂载标志 */
    uint32_t ref_count;     /* 引用计数 */
};

/* 初始化 VFS */
void vfs_init(void);

/* 获取挂载列表（调试用） */
struct vfs_mount *vfs_get_mount_list(void);

/* 挂载文件系统 */
int vfs_mount(const char *path, const char *device, struct vfs_ops *ops, void *data);

/* 卸载文件系统 */
int vfs_unmount(const char *path);

/* 打开文件 */
struct vfs_file *vfs_open(const char *path, uint32_t mode);

/* 关闭文件 */
int vfs_close(struct vfs_file *file);

/* 读取文件 */
ssize_t vfs_read(struct vfs_file *file, void *buf, size_t count);

/* 写入文件 */
ssize_t vfs_write(struct vfs_file *file, const void *buf, size_t count);

/* 定位文件 */
int vfs_seek(struct vfs_file *file, uint64_t offset);

/* 定位文件 (带命令) */
int vfs_seek_ext(struct vfs_file *file, int64_t offset, int whence);

/* 获取文件信息 */
int vfs_stat(const char *path, struct vfs_stat *stat);

/* 读取目录 */
int vfs_readdir(const char *path, struct vfs_dirent *dirent, int index);

/* 读取整个文件到内存 */
void *vfs_read_file(const char *path, size_t *size);

/* 创建目录 */
int vfs_mkdir(const char *path);

/* 删除目录 */
int vfs_rmdir(const char *path);

/* 删除文件 */
int vfs_unlink(const char *path);

/* 重命名文件 */
int vfs_rename(const char *oldpath, const char *newpath);

/* 获取文件信息 (通过文件描述符) */
int vfs_fstat(struct vfs_file *file, struct vfs_stat *stat);

/* 读取目录项 (通过文件描述符) */
ssize_t vfs_getdents(struct vfs_file *file, void *dirp, unsigned int count);

/* 同步文件到磁盘 */
int vfs_sync(struct vfs_file *file);

/* 截断文件 */
int vfs_truncate(const char *path, uint64_t length);

/* 修改权限 */
int vfs_chmod(const char *path, uint32_t mode);

/* 修改所有者 */
int vfs_chown(const char *path, uint32_t uid, uint32_t gid);

/* 权限检查 */
int vfs_permission_check(const char *path, uint32_t perm);

/* 检查文件是否存在 */
int vfs_exists(const char *path);

/* 检查路径是否为目录 */
int vfs_is_dir(const char *path);

/* 获取文件大小 */
int vfs_get_size(const char *path, uint64_t *size);

/* 打印 VFS 状态 */
void vfs_stats(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：Buffer Cache (页面缓存)
 * 
 * 高性能文件系统缓存机制
 * - LRU 替换策略
 * - 预读优化
 * - 写回缓存
 * - 哈希索引
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 缓存块大小 (4KB) */
#define BCACHE_BLOCK_SIZE       4096

/* 缓存块数量 */
#define BCACHE_NUM_BLOCKS       1024

/* 哈希表大小 */
#define BCACHE_HASH_SIZE        256

/* 缓存块状态 */
#define BCACHE_STATE_CLEAN      0
#define BCACHE_STATE_DIRTY      1
#define BCACHE_STATE_LOCKED     2
#define BCACHE_STATE_READING    4
#define BCACHE_STATE_WRITING    8

/* 缓存块 */
struct bcache_block {
    uint64_t block_id;          /* 块 ID (设备号 + 块号) */
    uint32_t state;             /* 状态 */
    uint32_t ref_count;         /* 引用计数 */
    uint64_t access_time;       /* 访问时间 */
    uint32_t hit_count;         /* 命中计数 */
    struct bcache_block *hash_next;  /* 哈希链表 */
    struct bcache_block *lru_prev;   /* LRU 链表 */
    struct bcache_block *lru_next;
    uint8_t data[BCACHE_BLOCK_SIZE];
};

/* 缓存统计 */
struct bcache_stats {
    uint64_t reads;             /* 读取次数 */
    uint64_t writes;            /* 写入次数 */
    uint64_t hits;              /* 缓存命中 */
    uint64_t misses;            /* 缓存未命中 */
    uint64_t evictions;         /* 淘汰次数 */
    uint64_t writebacks;        /* 写回次数 */
    uint64_t prefetches;        /* 预读次数 */
    uint32_t used_blocks;       /* 已用块数 */
    uint32_t dirty_blocks;      /* 脏块数 */
};

/* 预读控制 */
struct bcache_readahead {
    uint64_t last_block;        /* 上次访问的块 */
    uint32_t seq_count;         /* 顺序访问计数 */
    uint32_t ra_size;           /* 预读大小 */
    int enabled;
};

/* 初始化 Buffer Cache */
void bcache_init(void);

/* 读取缓存块 */
int bcache_read(uint32_t dev_id, uint64_t block_num, void *buf);

/* 写入缓存块 */
int bcache_write(uint32_t dev_id, uint64_t block_num, const void *buf);

/* 标记块为脏 */
void bcache_mark_dirty(uint32_t dev_id, uint64_t block_num);

/* 同步脏块到磁盘 */
int bcache_sync(uint32_t dev_id);

/* 同步所有脏块 */
int bcache_sync_all(void);

/* 使缓存失效 */
void bcache_invalidate(uint32_t dev_id, uint64_t block_num);

/* 使设备所有缓存失效 */
void bcache_invalidate_dev(uint32_t dev_id);

/* 获取缓存统计 */
void bcache_get_stats(struct bcache_stats *stats);

/* 预读 */
int bcache_prefetch(uint32_t dev_id, uint64_t start_block, uint32_t count);

/* 设置预读参数 */
void bcache_set_readahead(int enabled, uint32_t size);

/* ═══════════════════════════════════════════════════════════════════════════
 * 文件锁 (flock/lockf) 支持 - POSIX 咨询锁
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 锁类型 */
#define VFS_LOCK_SHARED     1   /* 共享锁 (flock LOCK_SH) */
#define VFS_LOCK_EXCLUSIVE  2   /* 排他锁 (flock LOCK_EX) */
#define VFS_LOCK_UNLOCK     8   /* 解锁 (flock LOCK_UN) */
#define VFS_LOCK_NONBLOCK   4   /* 非阻塞 (flock LOCK_NB) */

/* POSIX lockf 模式 */
#define VFS_F_LOCK         0    /* 阻塞加锁 */
#define VFS_F_TLOCK        1    /* 非阻塞加锁 */
#define VFS_F_ULOCK        2    /* 解锁 */
#define VFS_F_TEST         3    /* 测试锁 */

/* flock 系统调用 - 整文件咨询锁 */
int vfs_flock(struct vfs_file *file, uint32_t operation);

/* lockf 系统调用 - 范围咨询锁 */
int vfs_lockf(struct vfs_file *file, int cmd, int64_t len);

/* ═══════════════════════════════════════════════════════════════════════════
 * Dentry Cache 目录项缓存 - 加速路径解析
 * ═══════════════════════════════════════════════════════════════════════════ */

/* dentry 缓存查找 - 用于路径解析加速 */
struct dentry *vfs_dcache_lookup(const char *name, uint32_t dev_id);

/* dentry 缓存添加 */
struct dentry *vfs_dcache_add(const char *name, uint32_t dev_id, uint64_t inode);

/* dentry 缓存释放 */
void vfs_dcache_put(struct dentry *d);

/* dentry 缓存使失效 */
void vfs_dcache_invalidate(const char *name, uint32_t dev_id);

/* ═══════════════════════════════════════════════════════════════════════════
 * Inode Cache 回收 - 内存压力下回收未使用 inode
 * ═══════════════════════════════════════════════════════════════════════════ */

/* inode 缓存添加 */
void vfs_icache_add(uint32_t dev_id, uint64_t inode, uint64_t size, uint64_t mode);

/* inode 缓存回收 - 在内存压力下释放未使用的 inode 缓存 */
uint32_t vfs_icache_reclaim(uint32_t target_count);

/* inode 缓存使失效 */
void vfs_icache_invalidate(uint32_t dev_id, uint64_t inode);

/* VFS 缓存 shrinker - 内存压力下回收缓存 */
uint32_t vfs_shrinker(uint32_t target);

#endif
