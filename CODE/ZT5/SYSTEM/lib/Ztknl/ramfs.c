/*
 * ramfs.c - 内存文件系统实现
 * 
 * 简单的内存文件系统，用于日志存储和临时文件
 */

#include "ramfs.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"
#include "task.h"

/* 调试开关 */
#define RAMFS_DEBUG 0

#if RAMFS_DEBUG
#define RAMFS_LOG(msg) println("[RAMFS] " msg)
#define RAMFS_LOG1(msg, val) do { print("[RAMFS] " msg); print_dec(val); println(""); } while(0)
#else
#define RAMFS_LOG(msg) ((void)0)
#define RAMFS_LOG1(msg, val) ((void)0)
#endif

/* 最大文件数 */
#define RAMFS_MAX_FILES 64
/* 最大文件大小 */
#define RAMFS_MAX_FILE_SIZE (256 * 1024)
/* 最大路径长度 */
#define RAMFS_MAX_PATH 256

/* RAMFS 文件结构 */
struct ramfs_file {
    char path[RAMFS_MAX_PATH];
    uint8_t *data;
    size_t size;
    size_t capacity;
    int is_directory;
    int in_use;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
};

/* RAMFS 文件系统实例 */
static struct ramfs_file ramfs_files[RAMFS_MAX_FILES];
static int ramfs_initialized = 0;
static spinlock_t ramfs_lock = SPINLOCK_INIT;

/* 查找文件 */
static struct ramfs_file *ramfs_find_file(const char *path) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (ramfs_files[i].in_use && strcmp(ramfs_files[i].path, path) == 0) {
            return &ramfs_files[i];
        }
    }
    return NULL;
}

/* 查找空闲槽位 */
static struct ramfs_file *ramfs_find_slot(void) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!ramfs_files[i].in_use) {
            return &ramfs_files[i];
        }
    }
    return NULL;
}

/* 创建文件 */
static struct ramfs_file *ramfs_create_file(const char *path, int is_dir) {
    struct ramfs_file *file = ramfs_find_slot();
    if (!file) {
        return NULL;
    }
    
    strncpy(file->path, path, RAMFS_MAX_PATH - 1);
    file->path[RAMFS_MAX_PATH - 1] = '\0';
    file->is_directory = is_dir;
    file->size = 0;
    file->capacity = is_dir ? 0 : 4096;
    
    if (!is_dir) {
        file->data = kmalloc(file->capacity);
        if (!file->data) {
            return NULL;
        }
        memset(file->data, 0, file->capacity);
    } else {
        file->data = NULL;
    }
    
    file->in_use = 1;
    file->uid = process_get_current_uid();
    file->gid = process_get_current_gid();
    file->mode = is_dir ? 0755 : 0644;
    return file;
}

/* VFS open */
static int ramfs_vfs_open(struct vfs_file *vfile, const char *path, uint32_t mode) {
    RAMFS_LOG("open: ");
    RAMFS_LOG1("path=", path ? 1 : 0);
    
    /* 特殊处理：根目录总是存在 */
    if (path && path[0] == '/' && path[1] == '\0') {
        vfile->fs_data = NULL;  /* 根目录没有文件数据 */
        vfile->offset = 0;
        vfile->mode = mode;
        return 0;
    }
    
    spin_lock(&ramfs_lock);
    
    struct ramfs_file *file = ramfs_find_file(path);
    
    /* 如果文件不存在且需要写入，创建新文件 */
    if (!file && (mode & VFS_MODE_WRITE)) {
        file = ramfs_create_file(path, 0);
        if (!file) {
            spin_unlock(&ramfs_lock);
            // println("[RAMFS] Failed to create file");
            return -1;
        }
        // println("[RAMFS] Created new file");
    }
    
    if (!file) {
        spin_unlock(&ramfs_lock);
        return -1;
    }
    
    vfile->fs_data = file;
    /* 写入模式：offset 为 0（覆盖）；追加模式：offset 为文件大小 */
    if (mode & VFS_MODE_APPEND) {
        vfile->offset = file->size;
    } else {
        vfile->offset = 0;
    }
    vfile->mode = mode;  /* 保存模式以便后续检查 */
    
    spin_unlock(&ramfs_lock);
    return 0;
}

/* VFS close */
static int ramfs_vfs_close(struct vfs_file *vfile) {
    vfile->fs_data = NULL;
    return 0;
}

/* VFS read */
static ssize_t ramfs_vfs_read(struct vfs_file *vfile, void *buf, size_t count) {
    struct ramfs_file *file = vfile->fs_data;
    if (!file || !buf) return -1;
    
    spin_lock(&ramfs_lock);
    
    if (vfile->offset >= file->size) {
        spin_unlock(&ramfs_lock);
        return 0;
    }
    
    /* 检查数据指针有效性 */
    if (file->data == NULL) {
        spin_unlock(&ramfs_lock);
        return -1;
    }
    
    size_t to_read = count;
    if (vfile->offset + to_read > file->size) {
        to_read = file->size - vfile->offset;
    }
    
    memcpy(buf, file->data + vfile->offset, to_read);
    /* Note: offset is managed by VFS layer, so we don't increment it here */
    
    spin_unlock(&ramfs_lock);
    return to_read;
}

/* VFS write */
static ssize_t ramfs_vfs_write(struct vfs_file *vfile, const void *buf, size_t count) {
    struct ramfs_file *file = vfile->fs_data;
    if (!file || !buf) return -1;
    
    spin_lock(&ramfs_lock);
    
    /* 检查数据指针有效性 - 如果为 NULL 则分配初始内存 */
    if (file->data == NULL && !file->is_directory) {
        file->data = kmalloc(4096);
        if (!file->data) {
            spin_unlock(&ramfs_lock);
            return -1;
        }
        memset(file->data, 0, 4096);
        file->capacity = 4096;
    }
    
    /* 检查是否需要扩展 */
    size_t new_size = vfile->offset + count;
    if (new_size > file->capacity) {
        size_t new_capacity = new_size * 2;
        if (new_capacity > RAMFS_MAX_FILE_SIZE) {
            new_capacity = RAMFS_MAX_FILE_SIZE;
        }
        if (new_size > new_capacity) {
            spin_unlock(&ramfs_lock);
            return -1;  /* 文件太大 */
        }
        
        uint8_t *new_data = kmalloc(new_capacity);
        if (!new_data) {
            spin_unlock(&ramfs_lock);
            return -1;
        }
        
        if (file->data) {
            memcpy(new_data, file->data, file->size);
            kfree(file->data);
        }
        file->data = new_data;
        file->capacity = new_capacity;
    }
    
    if (file->data) {
        memcpy(file->data + vfile->offset, buf, count);
    } else {
        spin_unlock(&ramfs_lock);
        return -1;
    }
    
    /* Update file size if needed */
    size_t current_end = vfile->offset + count;
    if (current_end > file->size) {
        file->size = current_end;
    }
    
    spin_unlock(&ramfs_lock);
    return count;
}

/* VFS seek */
static int ramfs_vfs_seek(struct vfs_file *vfile, uint64_t offset) {
    struct ramfs_file *file = vfile->fs_data;
    if (!file) return -1;
    
    if (offset > file->size) {
        /* 扩展文件 */
        if (offset > RAMFS_MAX_FILE_SIZE) {
            return -1;
        }
        /* 对于简单实现，只允许在文件大小内 seek */
        offset = file->size;
    }
    
    vfile->offset = offset;
    return 0;
}

/* VFS mkdir */
static int ramfs_vfs_mkdir(const char *path) {
    spin_lock(&ramfs_lock);
    
    if (ramfs_find_file(path)) {
        spin_unlock(&ramfs_lock);
        return -1;  /* 已存在 */
    }
    
    struct ramfs_file *dir = ramfs_create_file(path, 1);
    spin_unlock(&ramfs_lock);
    
    return dir ? 0 : -1;
}

/* VFS readdir — 只返回指定目录的直接子项，名称为basename
 * 注意：path是相对于挂载点的路径（由VFS的get_rel_path转换）：
 *   根目录: path="/"
 *   /system: path="system"
 *   /system/dbg: path="system/dbg"
 * 但ramfs内部存储的path是绝对路径："/system", "/system/dbg"等
 */
static int ramfs_vfs_readdir(const char *path, struct vfs_dirent *dirent, int index) {
    int count = 0;

    /* 将相对path转换为绝对路径前缀以便匹配 */
    /* path="/": 需要匹配"/xxx"（一个'/'后无嵌套） */
    /* path="system": 需要匹配"/system/xxx"（system/后无嵌套） */
    int is_root = (path[0] == '/' && path[1] == '\0');
    int path_empty = (path[0] == '\0');

    /* 构造路径前缀以匹配子项。
     * get_rel_path 在根挂载时移除前导 "/"，因此 ramfs 条目
     * 的存储形式为 "system/file"（而非 "/system/file"）。
     */
    char abs_prefix[256];
    int prefix_len = 0;
    if (is_root || path_empty) {
        /* 根目录：无前缀；直接子项为 "name" */
        prefix_len = 0;
    } else {
        /* 非根目录：前缀为 "path/" */
        int j = 0;
        for (int k = 0; path[k] && j < 254; k++, j++) {
            abs_prefix[j] = path[k];
        }
        abs_prefix[j] = '/';
        abs_prefix[j + 1] = '\0';
        prefix_len = j + 1;
    }

    spin_lock(&ramfs_lock);

    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!ramfs_files[i].in_use) continue;

        const char *fpath = ramfs_files[i].path;
        int flen = 0;
        while (fpath[flen]) flen++;

        /* 检查fpath是否是path的直接子项：
         * 1. fpath长度 > prefix_len（必须有basename部分）
         * 2. fpath前缀匹配abs_prefix
         * 3. prefix之后的部分（basename）不包含'/' */
        if (flen <= prefix_len) continue;
        if (strncmp(fpath, abs_prefix, prefix_len) != 0) continue;

        /* 检查basename部分没有嵌套'/' */
        const char *basename = fpath + prefix_len;
        int has_nested_slash = 0;
        for (int j = 0; basename[j]; j++) {
            if (basename[j] == '/') { has_nested_slash = 1; break; }
        }
        if (has_nested_slash) continue;

        if (count == index) {
            strncpy(dirent->name, basename, 255);
            dirent->name[255] = '\0';
            dirent->type = ramfs_files[i].is_directory ? 2 : 1;
            dirent->size = ramfs_files[i].size;
            spin_unlock(&ramfs_lock);
            return 0;
        }
        count++;
    }

    spin_unlock(&ramfs_lock);
    return -1;
}

/* VFS stat */
static int ramfs_vfs_stat(const char *path, struct vfs_stat *stat) {
    spin_lock(&ramfs_lock);
    struct ramfs_file *file = ramfs_find_file(path);
    
    if (!file) {
        spin_unlock(&ramfs_lock);
        return -1;
    }
    
    stat->size = file->size;
    stat->type = file->is_directory ? 1 : 0;
    stat->attr = 0;
    stat->ctime = 0;
    stat->mtime = 0;
    stat->atime = 0;
    stat->inode = (uint32_t)(file - ramfs_files);
    stat->nlink = 1;
    stat->uid = file->uid;
    stat->gid = file->gid;
    stat->mode = file->mode;
    stat->blksize = 4096;
    stat->blocks = (file->size + 4095) / 4096;

    spin_unlock(&ramfs_lock);
    return 0;
}

/* VFS unlink */
static int ramfs_vfs_unlink(const char *path) {
    spin_lock(&ramfs_lock);
    struct ramfs_file *file = ramfs_find_file(path);
    if (!file) {
        spin_unlock(&ramfs_lock);
        return -1;
    }
    
    if (file->data) {
        kfree(file->data);
    }
    file->in_use = 0;
    file->data = NULL;
    file->size = 0;
    
    spin_unlock(&ramfs_lock);
    return 0;
}

/* VFS rename */
static int ramfs_vfs_rename(const char *oldpath, const char *newpath) {
    spin_lock(&ramfs_lock);
    struct ramfs_file *file = ramfs_find_file(oldpath);
    if (!file) {
        spin_unlock(&ramfs_lock);
        return -1;
    }
    
    /* 检查新路径是否已存在 */
    if (ramfs_find_file(newpath)) {
        spin_unlock(&ramfs_lock);
        return -1;
    }
    
    strncpy(file->path, newpath, RAMFS_MAX_PATH - 1);
    file->path[RAMFS_MAX_PATH - 1] = '\0';
    
    spin_unlock(&ramfs_lock);
    return 0;
}

/* VFS fstat */
static int ramfs_vfs_fstat(struct vfs_file *vfile, struct vfs_stat *stat) {
    struct ramfs_file *file = vfile->fs_data;
    if (!file || !stat) return -1;
    
    stat->size = file->size;
    stat->type = file->is_directory ? 1 : 0;
    stat->attr = 0;
    stat->ctime = 0;
    stat->mtime = 0;
    stat->atime = 0;
    stat->inode = (uint32_t)(file - ramfs_files);
    stat->nlink = 1;
    stat->uid = file->uid;
    stat->gid = file->gid;
    stat->mode = file->mode;
    stat->blksize = 4096;
    stat->blocks = (file->size + 4095) / 4096;

    return 0;
}

/* VFS sync */
static int ramfs_vfs_sync(struct vfs_file *vfile) {
    /* RAMFS 所有数据都在内存中，无需同步 */
    return 0;
}

/* VFS truncate */
static int ramfs_vfs_truncate(const char *path, uint64_t length) {
    spin_lock(&ramfs_lock);
    struct ramfs_file *file = ramfs_find_file(path);
    if (!file) {
        spin_unlock(&ramfs_lock);
        return -1;
    }

    if (length > file->capacity) {
        /* 需要扩展 */
        uint8_t *new_data = kmalloc(length);
        if (!new_data) {
            spin_unlock(&ramfs_lock);
            return -1;
        }
        memset(new_data, 0, length);  /* 清零以防止信息泄露 */
        if (file->data) {
            memcpy(new_data, file->data, file->size);
            kfree(file->data);
        }
        file->data = new_data;
        file->capacity = length;
    }

    file->size = length;
    spin_unlock(&ramfs_lock);
    return 0;
}

/* VFS chmod */
static int ramfs_vfs_chmod(const char *path, uint32_t mode) {
    spin_lock(&ramfs_lock);
    struct ramfs_file *file = ramfs_find_file(path);
    if (!file) {
        spin_unlock(&ramfs_lock);
        return -1;
    }
    file->mode = mode;
    spin_unlock(&ramfs_lock);
    return 0;
}

/* VFS chown */
static int ramfs_vfs_chown(const char *path, uint32_t uid, uint32_t gid) {
    spin_lock(&ramfs_lock);
    struct ramfs_file *file = ramfs_find_file(path);
    if (!file) {
        spin_unlock(&ramfs_lock);
        return -1;
    }
    file->uid = uid;
    file->gid = gid;
    spin_unlock(&ramfs_lock);
    return 0;
}

/* VFS 操作接口 */
static struct vfs_ops ramfs_ops = {
    .open = ramfs_vfs_open,
    .close = ramfs_vfs_close,
    .read = ramfs_vfs_read,
    .write = ramfs_vfs_write,
    .seek = ramfs_vfs_seek,
    .stat = ramfs_vfs_stat,
    .readdir = ramfs_vfs_readdir,
    .mkdir = ramfs_vfs_mkdir,
    .rmdir = ramfs_vfs_unlink,
    .unlink = ramfs_vfs_unlink,
    .rename = ramfs_vfs_rename,
    .fstat = ramfs_vfs_fstat,
    .sync = ramfs_vfs_sync,
    .truncate = ramfs_vfs_truncate,
    .chmod = ramfs_vfs_chmod,
    .chown = ramfs_vfs_chown,
};

/* 获取 ramfs 操作接口 */
struct vfs_ops *ramfs_get_ops(void) {
    return &ramfs_ops;
}

/* 初始化 RAMFS */
int ramfs_init(void) {
    if (ramfs_initialized) {
        return 0;
    }
    
    // println("[RAMFS] Initializing memory filesystem...");
    
    memset(ramfs_files, 0, sizeof(ramfs_files));
    ramfs_initialized = 1;
    
    // println("[RAMFS] Memory filesystem initialized");
    return 0;
}

/* 挂载 ramfs 到指定路径 */
int ramfs_mount(const char *path) {
    if (!ramfs_initialized) {
        ramfs_init();
    }
    
    return vfs_mount(path, "ramfs", &ramfs_ops, NULL);
}
