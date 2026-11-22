#include "vfs.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "task.h"

/* ========== 调试宏 ========== */
#define VFS_DEBUG 0
#include "smp.h"

#if VFS_DEBUG
#define VFS_LOG(msg) do { println("[VFS] " msg); } while(0)
#define VFS_LOG1(msg, val) do { print("[VFS] " msg); print_dec(val); println(""); } while(0)
#else
#define VFS_LOG(msg) ((void)0)
#define VFS_LOG1(msg, val) ((void)0)
#endif

/* 路径最大长度 */
#define VFS_MAX_PATH_LEN 255

/* 挂载点链表 */
static struct vfs_mount *mount_list = NULL;
static spinlock_t mount_list_lock = SPINLOCK_INIT;  /* SMP并发保护 */

/* 根挂载点静态结构 - 防止堆损坏 */
static struct vfs_mount root_mount_struct;

/* 根挂载点指针 */
static struct vfs_mount *root_mount = NULL;

void vfs_init(void) {
    mount_list = NULL;
    root_mount = NULL;
    memset(&root_mount_struct, 0, sizeof(root_mount_struct));
    VFS_LOG("Initialized");
}

int vfs_mount(const char *path, const char *device, struct vfs_ops *ops, void *data) {
    /* 参数验证 */
    if (path == NULL || device == NULL || ops == NULL) {
        println("[VFS] mount: Invalid parameters");
        return -1;
    }
    
    /* 检查路径长度 */
    if (strlen(path) >= VFS_MAX_PATH_LEN) {
        println("[VFS] mount: Path too long");
        return -1;
    }
    
    /* 对于根挂载点 "/"，使用静态分配的内存防止堆损坏 */
    struct vfs_mount *mount;
    if (strcmp(path, "/") == 0) {
        mount = &root_mount_struct;
        /* 清零初始化 */
        memset(mount, 0, sizeof(struct vfs_mount));
        println("[VFS] mount: using static root mount structure");
    } else {
        mount = kmalloc(sizeof(struct vfs_mount));
        if (mount == NULL) {
            println("[VFS] mount: Failed to allocate mount structure");
            return -1;
        }
        memset(mount, 0, sizeof(struct vfs_mount));
        println("[VFS] mount: kmalloc success");
    }

    strncpy(mount->path, path, sizeof(mount->path) - 1);
    mount->path[sizeof(mount->path) - 1] = '\0';
    
    strncpy(mount->device, device, sizeof(mount->device) - 1);
    mount->device[sizeof(mount->device) - 1] = '\0';
    
    mount->ops = ops;
    mount->fs_data = data;
    mount->next = NULL;

    print("[VFS] mount: ops=");
    print_hex((uint64_t)ops);
    print(" fs_data=");
    print_hex((uint64_t)data);
    print(" mount->ops=");
    print_hex((uint64_t)mount->ops);
    println("");

    /* 添加到链表 - 使用锁保护 */
    spin_lock(&mount_list_lock);
    println("[VFS] mount: acquired lock");
    if (mount_list == NULL) {
        mount_list = mount;
        println("[VFS] mount: set mount_list to new mount");
    } else {
        struct vfs_mount *m = mount_list;
        while (m->next != NULL) {
            m = m->next;
        }
        m->next = mount;
        println("[VFS] mount: appended to mount_list");
    }

    /* 根挂载点 */
    if (strcmp(path, "/") == 0) {
        root_mount = mount;
        println("[VFS] mount: set root_mount");
    }
    spin_unlock(&mount_list_lock);

    print("[VFS] Mounted ");
    print(device);
    print(" at ");
    println(path);
    print("[VFS] mount_list=");
    print_hex((uint64_t)mount_list);
    print(" root_mount=");
    print_hex((uint64_t)root_mount);
    putchar('\n');

    return 0;
}

/* 查找挂载点 */
static struct vfs_mount *find_mount(const char *path) {
    struct vfs_mount *best = NULL;
    size_t best_len = 0;

    /* 规范化路径: 确保以 '/' 开头 */
    char norm_path[VFS_MAX_PATH_LEN];
    if (path[0] != '/') {
        norm_path[0] = '/';
        size_t plen = strlen(path);
        if (plen >= VFS_MAX_PATH_LEN - 1) return NULL;
        memcpy(norm_path + 1, path, plen + 1);
        path = norm_path;
    }

    /* 限制调试输出 - disabled */
    int dbg = 0;

    /* 使用锁保护遍历 */
    spin_lock(&mount_list_lock);
    struct vfs_mount *m = mount_list;
    int count = 0;
    while (m != NULL) {
        count++;

        /* 检查指针有效性 */
        if ((uint64_t)m < 0xFFFFFFFF80000000ULL) {
            if (dbg) { print("[FIND_MOUNT] CORRUPTED pointer\n"); }
            break;
        }

        size_t len = strlen(m->path);
        int cmp_result = strncmp(path, m->path, len);

        if (cmp_result == 0) {
            /* 确保路径在挂载点前缀之后是 '/' 或 '\0'
             * 特殊情况: 挂载点为 "/" 时，len=1，匹配所有绝对路径 */
            if (len > 1 && path[len] != '/' && path[len] != '\0') {
                m = m->next;
                continue;
            }
            if (len > best_len) {
                best = m;
                best_len = len;
            }
        }

        m = m->next;
    }
    spin_unlock(&mount_list_lock);

    if (dbg) {
        print("[FIND_MOUNT] found=");
        if (best) println(best->path); else println("(null)");
    }

    return best;
}

/* 获取相对路径 - 从完整路径中剥离挂载点前缀 */
static const char *get_rel_path(const char *path, struct vfs_mount *mount) {
    size_t mlen = strlen(mount->path);
    
    /* 如果路径不以 '/' 开头，说明已经是相对路径，不需要剥离挂载点前缀 */
    if (path[0] != '/') {
        return path;
    }
    
    /* 路径以 '/' 开头，剥离挂载点前缀 */
    if (strlen(path) <= mlen) {
        return "/";
    }
    return path + mlen;
}

/* 权限检查 */
int vfs_permission_check(const char *path, uint32_t perm) {
    struct vfs_stat st;
    if (vfs_stat(path, &st) != 0) {
        /* stat 失败（如 ATA 读取错误），默认允许访问
         * FAT32 不支持 Unix 权限，不应因 I/O 错误阻止访问 */
        return 0;
    }

    uint32_t uid = process_get_current_uid();
    uint32_t gid = process_get_current_gid();

    /* root bypass */
    if (uid == 0) {
        return 0;
    }

    /* Path-based access control for non-root users:
     * - Can read/write under /usr/[username]
     * - Can read under /desktop/[username]
     * - Can execute programs under /prgm (but NOT /prgm/sys)
     * - Can read system files
     * - Cannot write to /prgm, /system, /desktop, /usr (except own dir)
     */
    {
        struct process *proc = process_get_current();
        const char *uname = proc ? proc->username : "";

        /* Writing check */
        if (perm & S_IWUSR) {
            /* Allow write to /usr/[username]/... */
            int allowed = 0;
            /* Check /usr/[username] prefix */
            if (uname[0]) {
                char usr_prefix[64];
                int pl = 0;
                const char *p = "/usr/";
                while (*p) usr_prefix[pl++] = *p++;
                const char *u = uname;
                while (*u) usr_prefix[pl++] = *u++;
                usr_prefix[pl++] = '/';
                usr_prefix[pl] = 0;

                if (strncmp(path, usr_prefix, pl - 1) == 0) {
                    allowed = 1;
                }
            }
            /* Allow write to /desktop/[username]/... */
            if (!allowed && uname[0]) {
                char desk_prefix[64];
                int pl = 0;
                const char *p = "/desktop/";
                while (*p) desk_prefix[pl++] = *p++;
                const char *u = uname;
                while (*u) desk_prefix[pl++] = *u++;
                desk_prefix[pl++] = '/';
                desk_prefix[pl] = 0;

                if (strncmp(path, desk_prefix, pl - 1) == 0) {
                    allowed = 1;
                }
            }
            /* Block write to /prgm/sys */
            if (!allowed && strncmp(path, "/prgm/sys", 9) == 0) {
                return -1;
            }
            /* Block write to /system */
            if (!allowed && strncmp(path, "/system", 7) == 0) {
                return -1;
            }
            if (!allowed) {
                return -1;
            }
        }

        /* Execute check: block exec from /prgm/sys for non-root */
        if (perm & S_IXUSR) {
            if (strncmp(path, "/prgm/sys", 9) == 0) {
                return -1;
            }
        }
    }

    /* Check owner permissions: shift perm >> 6 to match shifted mode bits */
    if (uid == st.uid) {
        if (((st.mode >> 6) & (perm >> 6)) == (perm >> 6)) {
            return 0;
        }
    }
    /* Check group permissions: shift perm >> 3 to match shifted mode bits */
    if (gid == st.gid) {
        if (((st.mode >> 3) & (perm >> 3)) == (perm >> 3)) {
            return 0;
        }
    }
    /* Check other permissions */
    if ((st.mode & perm) == perm) {
        return 0;
    }

    return -1;
}

/* 获取父目录路径 */
static int get_parent_path(const char *path, char *parent, size_t parent_len) {
    if (path == NULL || parent == NULL || parent_len == 0) {
        return -1;
    }
    size_t len = strlen(path);
    if (len == 0) {
        return -1;
    }
    /* 复制路径 */
    size_t copy_len = len < parent_len - 1 ? len : parent_len - 1;
    for (size_t i = 0; i < copy_len; i++) {
        parent[i] = path[i];
    }
    parent[copy_len] = '\0';
    /* 去掉末尾的 '/' */
    while (copy_len > 1 && parent[copy_len - 1] == '/') {
        parent[copy_len - 1] = '\0';
        copy_len--;
    }
    /* 找到最后一个 '/' */
    size_t last_slash = 0;
    for (size_t i = 1; i < copy_len; i++) {
        if (parent[i] == '/') {
            last_slash = i;
        }
    }
    parent[last_slash + 1] = '\0';
    return 0;
}

struct vfs_file *vfs_open(const char *path, uint32_t mode) {
    /* 参数验证 */
    if (path == NULL) {
        return NULL;
    }

    /* 检查路径长度 */
    size_t path_len = strlen(path);
    if (path_len == 0 || path_len >= VFS_MAX_PATH_LEN) {
        return NULL;
    }

    /* 查找挂载点 */
    struct vfs_mount *mount = find_mount(path);
    if (mount == NULL) {
        println("[VFS] open: No mount point found for path");
        print("[VFS] open: path=");
        print(path);
        putchar('\n');
        /* Print all mounted mounts with lock */
        print("[VFS] Mounts: ");
        spin_lock(&mount_list_lock);
        struct vfs_mount *dbg_m = mount_list;
        while (dbg_m != NULL) {
            print(dbg_m->path);
            print(" ");
            dbg_m = dbg_m->next;
        }
        print("root_mount=");
        print_hex((uint64_t)root_mount);
        putchar('\n');
        spin_unlock(&mount_list_lock);
        
        /* Also check if root_mount is set */
        if (root_mount != NULL) {
            println("[VFS] open: root_mount is SET but find_mount returned NULL!");
        }
        return NULL;
    }
    
    /* 分配文件结构 */
    struct vfs_file *file = kmalloc(sizeof(struct vfs_file));
    if (file == NULL) {
        // println("[VFS] open: Failed to allocate file structure");
        return NULL;
    }

    /* 安全复制路径 */
    strncpy(file->path, path, sizeof(file->path) - 1);
    file->path[sizeof(file->path) - 1] = '\0';
    
    file->offset = 0;
    file->mode = mode;
    file->flags = 0;
    file->mount = mount;
    file->fs_data = NULL;
    file->ref_count = 1;       /* 初始引用计数为 1 */
    file->is_dirty = 0;
    file->readdir_index = 0;

    /* 权限检查 - 使用非递归方式避免 vfs_stat -> vfs_open 死循环
     * 直接调用底层文件系统的 stat，而非 vfs_stat */
    {
        uint32_t perm_check = 0;
        if (mode & VFS_MODE_READ) perm_check |= S_IRUSR;
        if (mode & VFS_MODE_WRITE) perm_check |= S_IWUSR;
        if (mode & VFS_MODE_EXEC) perm_check |= S_IXUSR;

        if (perm_check != 0 && mount->ops != NULL && mount->ops->stat != NULL) {
            /* 直接调用底层 stat，避免递归 */
            struct vfs_stat st;
            const char *rel_path_check = get_rel_path(path, mount);
            if (mount->ops->stat(rel_path_check, &st) == 0) {
                /* stat 成功，执行权限检查 */
                uint32_t uid = process_get_current_uid();
                uint32_t gid = process_get_current_gid();

                if (uid != 0) {
                    /* 非root用户执行路径权限检查 */
                    if (vfs_permission_check(path, perm_check) != 0) {
                        kfree(file);
                        return NULL;
                    }
                }
            } else {
                /* stat 失败 (文件不存在)，如果是创建模式，检查父目录写权限 */
                if (mode & (VFS_MODE_CREATE | VFS_MODE_WRITE)) {
                    char parent_path[256];
                    if (get_parent_path(path, parent_path, sizeof(parent_path)) == 0) {
                        uint32_t uid = process_get_current_uid();
                        if (uid != 0) {
                            if (vfs_permission_check(parent_path, S_IWUSR) != 0) {
                                kfree(file);
                                return NULL;
                            }
                        }
                    }
                }
            }
        }
    }

    /* dentry 缓存查找 - 加速路径解析 */
    {
        struct dentry *dcache_ent = vfs_dcache_lookup(path, 0);
        if (dcache_ent != NULL) {
            /* 缓存命中，记录 inode 信息 */
            vfs_dcache_put(dcache_ent);
        }
    }

    /* 获取相对路径 */
    const char *rel_path = get_rel_path(path, mount);

    /* 调用文件系统特定的 open */
    if (mount->ops->open == NULL) {
        // println("[VFS] open: No open operation");
        kfree(file);
        return NULL;
    }
    
    if (mount->ops->open(file, rel_path, mode) != 0) {
        kfree(file);
        return NULL;
    }

    /* 打开成功后添加到 dentry 缓存 */
    {
        struct vfs_stat st;
        const char *rel = get_rel_path(path, mount);
        if (mount->ops->stat && mount->ops->stat(rel, &st) == 0) {
            vfs_dcache_add(path, 0, st.inode);
            vfs_icache_add(0, st.inode, st.size, st.mode);
        }
    }

    return file;
}

int vfs_close(struct vfs_file *file) {
    if (file == NULL) {
        return -1;
    }

    /* 减少引用计数，只有最后一个引用者才真正关闭和释放 */
    if (file->ref_count > 1) {
        file->ref_count--;
        return 0;
    }

    int ret = 0;
    if (file->mount != NULL && file->mount->ops != NULL && file->mount->ops->close != NULL) {
        ret = file->mount->ops->close(file);
    }

    kfree(file);
    return ret;
}

ssize_t vfs_read(struct vfs_file *file, void *buf, size_t count) {
    /* 参数验证 */
    if (file == NULL) {
        return -1;
    }
    if (buf == NULL && count > 0) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    
    /* 检查读取权限 - 在 open 时已检查 */
    if (!(file->mode & VFS_MODE_READ)) {
        return -1;
    }

    /* 检查操作是否存在 */
    if (file->mount == NULL || file->mount->ops == NULL || file->mount->ops->read == NULL) {
        return -1;
    }

    ssize_t ret = file->mount->ops->read(file, buf, count);
    if (ret > 0) {
        file->offset += ret;
    }

    return ret;
}

ssize_t vfs_write(struct vfs_file *file, const void *buf, size_t count) {
    /* 参数验证 */
    if (file == NULL) {
        return -1;
    }
    if (buf == NULL && count > 0) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    
    /* 检查写入权限 - 在 open 时已检查 */
    if (!(file->mode & VFS_MODE_WRITE)) {
        return -1;
    }

    /* 检查操作是否存在 */
    if (file->mount == NULL || file->mount->ops == NULL || file->mount->ops->write == NULL) {
        return -1;
    }

    ssize_t ret = file->mount->ops->write(file, buf, count);
    if (ret > 0) {
        file->offset += ret;
    }

    return ret;
}

int vfs_seek(struct vfs_file *file, uint64_t offset) {
    if (file == NULL) {
        return -1;
    }

    if (file->mount->ops->seek) {
        return file->mount->ops->seek(file, offset);
    }

    file->offset = offset;
    return 0;
}

int vfs_stat(const char *path, struct vfs_stat *stat) {
    struct vfs_mount *mount = find_mount(path);
    if (mount == NULL || stat == NULL) {
        return -1;
    }

    if (mount->ops->stat == NULL) {
        return -1;
    }

    const char *rel_path = get_rel_path(path, mount);
    return mount->ops->stat(rel_path, stat);
}

int vfs_readdir(const char *path, struct vfs_dirent *dirent, int index) {
    struct vfs_mount *mount = find_mount(path);
    if (mount == NULL || dirent == NULL) {
        return -1;
    }

    if (mount->ops->readdir == NULL) {
        return -1;
    }

    /* 检查目录读权限 */
    if (vfs_permission_check(path, S_IRUSR) != 0) {
        return -1;
    }

    const char *rel_path = get_rel_path(path, mount);
    return mount->ops->readdir(rel_path, dirent, index);
}

void *vfs_read_file(const char *path, size_t *size) {
    // print("[VFS] vfs_read_file: path='");
    // print(path);
    // println("'");
    
    /* 首先尝试 VFS */
    struct vfs_stat stat;
    if (vfs_stat(path, &stat) == 0) {
        // print("[VFS] VFS stat OK, size=");
        // print_dec(stat.size);
        // println("");
        
        struct vfs_file *file = vfs_open(path, VFS_MODE_READ);
        if (file != NULL) {
            void *buf = kmalloc(stat.size + 1);
            if (buf != NULL) {
                ssize_t read = vfs_read(file, buf, stat.size);
                vfs_close(file);

                if (read >= 0) {
                    /* 仅当读取字节数与 stat 报告一致时才认为成功
                     * (规避 FAT32 簇链断裂等 bug 导致的截断) */
                    if ((size_t)read == stat.size) {
                        ((char *)buf)[read] = '\0';
                        if (size != NULL) {
                            *size = read;
                        }
                        return buf;
                    }
                    /* 读取被截断: 释放 buf, 回退到 Limine 模块 */
                    print("[VFS] WARN: truncated read for ");
                    print(path);
                    print(" got=");
                    {
                        char nb[24]; int p=0; uint64_t v=read;
                        if(v==0){nb[p++]='0';}else{char t[24];int tp=0;while(v>0){t[tp++]='0'+(v%10);v/=10;}while(tp)nb[p++]=t[--tp];} nb[p]=0;
                        print(nb);
                    }
                    print(" expected=");
                    {
                        char nb[24]; int p=0; uint64_t v=stat.size;
                        if(v==0){nb[p++]='0';}else{char t[24];int tp=0;while(v>0){t[tp++]='0'+(v%10);v/=10;}while(tp)nb[p++]=t[--tp];} nb[p]=0;
                        print(nb); print("\n");
                    }
                    kfree(buf);
                } else {
                    kfree(buf);
                }
            } else {
                vfs_close(file);
            }
        }
    } else {
        // print("[VFS] VFS stat failed, trying Limine modules...\n");
    }

    /* VFS 失败，回退到 Limine 模块查找 */
    extern void *limine_find_module(const char *path, size_t *size_out);
    void *module_data = limine_find_module(path, size);
    if (module_data != NULL) {
        /* Limine 返回的是直接指针，需要复制一份 */
        size_t module_size = size ? *size : 0;
        // print("[VFS] Limine module found, size=");
        // print_dec(module_size);
        // println(" bytes");
        
        void *buf = kmalloc(module_size + 1);
        if (buf != NULL) {
            memcpy(buf, module_data, module_size);
            ((char *)buf)[module_size] = '\0';
            // print("[VFS] Module copied to buffer 0x");
            // print_hex((uint64_t)buf);
            // println("");
            return buf;
        } else {
            // print("[VFS] ERROR: kmalloc failed for module buffer!\n");
        }
    } else {
        // print("[VFS] Limine module NOT found for: ");
        // println(path);
    }

    return NULL;
}

/* 创建目录 */
int vfs_mkdir(const char *path) {
    if (path == NULL) return -1;

    /* 检查父目录写权限 */
    char parent[256];
    if (get_parent_path(path, parent, sizeof(parent)) == 0) {
        if (vfs_permission_check(parent, S_IWUSR) != 0) {
            return -1;
        }
    }

    struct vfs_mount *mount = find_mount(path);
    if (mount == NULL) {
        // print("[VFS] mkdir: No mount point for ");
        // println(path);
        return -1;
    }

    if (mount->ops && mount->ops->mkdir) {
        /* 使用统一的相对路径计算函数 */
        const char *rel_path = get_rel_path(path, mount);

        // print("[VFS] mkdir: path='");
        // print(path);
        // print("' rel_path='");
        // print(rel_path);
        // println("'");

        return mount->ops->mkdir(rel_path);
    }

    // println("[VFS] mkdir: Operation not supported");
    return -1;
}

/* 删除目录 */
int vfs_rmdir(const char *path) {
    if (path == NULL) return -1;

    /* 检查父目录写权限 */
    char parent[256];
    if (get_parent_path(path, parent, sizeof(parent)) == 0) {
        if (vfs_permission_check(parent, S_IWUSR) != 0) {
            return -1;
        }
    }

    struct vfs_mount *mount = find_mount(path);
    if (mount == NULL) {
        return -1;
    }

    if (mount->ops && mount->ops->rmdir) {
        /* 计算相对路径 */
        const char *rel_path = path;
        int mount_len = 0;
        while (mount->path[mount_len]) mount_len++;
        if (strlen(path) > mount_len && path[mount_len] == '/') {
            rel_path = path + mount_len;
        }
        return mount->ops->rmdir(rel_path);
    }

    return -1;
}

/* 删除文件 */
int vfs_unlink(const char *path) {
    if (path == NULL) return -1;

    /* 检查父目录写权限 */
    char parent[256];
    if (get_parent_path(path, parent, sizeof(parent)) == 0) {
        if (vfs_permission_check(parent, S_IWUSR) != 0) {
            return -1;
        }
    }

    /* 检查文件本身写权限 (对只读文件应拒绝删除) */
    {
        uint32_t uid = process_get_current_uid();
        if (uid != 0) {
            /* 非root用户需要检查文件是否只读 */
            struct vfs_mount *mnt = find_mount(path);
            if (mnt != NULL && mnt->ops != NULL && mnt->ops->stat != NULL) {
                struct vfs_stat st;
                const char *rel = get_rel_path(path, mnt);
                if (mnt->ops->stat(rel, &st) == 0) {
                    /* 如果文件设置了只读属性，拒绝删除 */
                    if ((st.mode & S_IWUSR) == 0 && (st.attr & VFS_ATTR_RDONLY)) {
                        return VFS_ERR_PERM;
                    }
                }
            }
        }
    }

    struct vfs_mount *mount = find_mount(path);
    if (mount == NULL) {
        return -1;
    }

    if (mount->ops && mount->ops->unlink) {
        /* 使用统一的相对路径计算函数 */
        const char *rel_path = get_rel_path(path, mount);
        int ret = mount->ops->unlink(rel_path);
        /* 删除成功后使 dentry 缓存失效 */
        if (ret == 0) {
            vfs_dcache_invalidate(path, 0);
        }
        return ret;
    }

    return -1;
}

/* 重命名文件 */
int vfs_rename(const char *oldpath, const char *newpath) {
    if (oldpath == NULL || newpath == NULL) return -1;

    /* 检查源文件父目录写权限 */
    char old_parent[256];
    if (get_parent_path(oldpath, old_parent, sizeof(old_parent)) == 0) {
        if (vfs_permission_check(old_parent, S_IWUSR) != 0) {
            return VFS_ERR_PERM;
        }
    }

    /* 检查目标父目录写权限 */
    char new_parent[256];
    if (get_parent_path(newpath, new_parent, sizeof(new_parent)) == 0) {
        if (vfs_permission_check(new_parent, S_IWUSR) != 0) {
            return VFS_ERR_PERM;
        }
    }

    struct vfs_mount *mount = find_mount(oldpath);
    if (mount == NULL) {
        return -1;
    }

    if (mount->ops && mount->ops->rename) {
        /* 计算相对路径 */
        const char *rel_old = oldpath;
        const char *rel_new = newpath;
        int mount_len = 0;
        while (mount->path[mount_len]) mount_len++;

        if (strlen(oldpath) > mount_len && oldpath[mount_len] == '/') {
            rel_old = oldpath + mount_len;
        }
        if (strlen(newpath) > mount_len && newpath[mount_len] == '/') {
            rel_new = newpath + mount_len;
        }

        return mount->ops->rename(rel_old, rel_new);
    }

    return -1;
}

/* 获取文件信息 (通过文件描述符) */
int vfs_fstat(struct vfs_file *file, struct vfs_stat *stat) {
    if (file == NULL || stat == NULL) return -1;
    
    /* 从文件路径获取信息 */
    return vfs_stat(file->path, stat);
}

/* 读取目录项 (通过文件描述符) */
ssize_t vfs_getdents(struct vfs_file *file, void *dirp, unsigned int count) {
    if (file == NULL || dirp == NULL) return -1;
    
    /* 简化实现: 返回空 */
    (void)count;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * VFS 扩展功能实现
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 卸载文件系统 */
int vfs_unmount(const char *path) {
    if (path == NULL) {
        return VFS_ERR_INVAL;
    }
    
    /* 查找挂载点 - 加锁保护 */
    spin_lock(&mount_list_lock);
    
    struct vfs_mount *prev = NULL;
    struct vfs_mount *m = mount_list;
    
    while (m != NULL) {
        if (strcmp(m->path, path) == 0) {
            break;
        }
        prev = m;
        m = m->next;
    }
    
    if (m == NULL) {
        spin_unlock(&mount_list_lock);
        return VFS_ERR_NOTFOUND;
    }
    
    /* 检查引用计数 */
    if (m->ref_count > 0) {
        spin_unlock(&mount_list_lock);
        return VFS_ERR_BUSY;
    }
    
    /* 从链表移除 */
    if (prev == NULL) {
        mount_list = m->next;
    } else {
        prev->next = m->next;
    }
    
    /* 更新根挂载点 */
    if (m == root_mount) {
        root_mount = NULL;
    }
    
    spin_unlock(&mount_list_lock);
    
    /* 调用文件系统的清理函数（锁外执行，避免死锁） */
    if (m->ops != NULL && m->ops->sync != NULL) {
        m->ops->sync(NULL);  /* 同步所有数据 */
    }
    
    /* 不要释放静态分配的根挂载结构 */
    if (m != &root_mount_struct) {
        kfree(m);
    }
    
    // print("[VFS] Unmounted ");
    // println(path);
    
    return VFS_OK;
}

/* 定位文件 (带命令) */
int vfs_seek_ext(struct vfs_file *file, int64_t offset, int whence) {
    if (file == NULL) {
        return VFS_ERR_INVAL;
    }
    
    uint64_t new_offset;
    
    switch (whence) {
        case VFS_SEEK_SET:
            if (offset < 0) {
                return VFS_ERR_INVAL;
            }
            new_offset = (uint64_t)offset;
            break;
            
        case VFS_SEEK_CUR:
            if (offset < 0 && (uint64_t)(-offset) > file->offset) {
                return VFS_ERR_INVAL;
            }
            new_offset = file->offset + offset;
            break;
            
        case VFS_SEEK_END: {
            struct vfs_stat stat;
            if (vfs_fstat(file, &stat) != 0) {
                return VFS_ERR_INVAL;
            }
            if (offset < 0 && (uint64_t)(-offset) > stat.size) {
                return VFS_ERR_INVAL;
            }
            new_offset = stat.size + offset;
            break;
        }
        
        default:
            return VFS_ERR_INVAL;
    }
    
    return vfs_seek(file, new_offset);
}

/* 同步文件到磁盘 */
int vfs_sync(struct vfs_file *file) {
    if (file == NULL) {
        return VFS_ERR_INVAL;
    }
    
    if (file->mount == NULL || file->mount->ops == NULL) {
        return VFS_ERR_INVAL;
    }
    
    if (file->mount->ops->sync == NULL) {
        return VFS_OK;  /* 不支持同步，但不算错误 */
    }
    
    int ret = file->mount->ops->sync(file);
    if (ret == 0) {
        file->is_dirty = 0;
    }
    
    return ret;
}

/* 截断文件 */
int vfs_truncate(const char *path, uint64_t length) {
    if (path == NULL) {
        return VFS_ERR_INVAL;
    }
    
    struct vfs_mount *mount = find_mount(path);
    if (mount == NULL) {
        return VFS_ERR_NOTFOUND;
    }
    
    if (mount->ops == NULL || mount->ops->truncate == NULL) {
        return VFS_ERR_NOTSUP;
    }
    
    const char *rel_path = get_rel_path(path, mount);
    return mount->ops->truncate(rel_path, length);
}

/* 检查文件是否存在 */
int vfs_exists(const char *path) {
    if (path == NULL) {
        return 0;
    }
    
    struct vfs_stat stat;
    return vfs_stat(path, &stat) == 0 ? 1 : 0;
}

/* 检查路径是否为目录 */
int vfs_is_dir(const char *path) {
    if (path == NULL) {
        return 0;
    }
    
    struct vfs_stat stat;
    if (vfs_stat(path, &stat) != 0) {
        return 0;
    }
    
    return stat.type == VFS_TYPE_DIR ? 1 : 0;
}

/* 获取文件大小 */
int vfs_get_size(const char *path, uint64_t *size) {
    if (path == NULL || size == NULL) {
        return VFS_ERR_INVAL;
    }
    
    struct vfs_stat stat;
    if (vfs_stat(path, &stat) != 0) {
        return VFS_ERR_NOTFOUND;
    }
    
    *size = stat.size;
    return VFS_OK;
}

/* 完善 mkdir 实现 */
int vfs_mkdir_impl(const char *path) {
    if (path == NULL) return VFS_ERR_INVAL;
    
    struct vfs_mount *mount = find_mount(path);
    if (mount == NULL) {
        return VFS_ERR_NOTFOUND;
    }
    
    if (mount->ops == NULL || mount->ops->mkdir == NULL) {
        return VFS_ERR_NOTSUP;
    }
    
    const char *rel_path = get_rel_path(path, mount);
    return mount->ops->mkdir(rel_path);
}

/* 完善 rmdir 实现 */
int vfs_rmdir_impl(const char *path) {
    if (path == NULL) return VFS_ERR_INVAL;
    
    struct vfs_mount *mount = find_mount(path);
    if (mount == NULL) {
        return VFS_ERR_NOTFOUND;
    }
    
    if (mount->ops == NULL || mount->ops->rmdir == NULL) {
        return VFS_ERR_NOTSUP;
    }
    
    const char *rel_path = get_rel_path(path, mount);
    return mount->ops->rmdir(rel_path);
}

/* 完善 unlink 实现 */
int vfs_unlink_impl(const char *path) {
    if (path == NULL) return VFS_ERR_INVAL;
    
    struct vfs_mount *mount = find_mount(path);
    if (mount == NULL) {
        return VFS_ERR_NOTFOUND;
    }
    
    if (mount->ops == NULL || mount->ops->unlink == NULL) {
        return VFS_ERR_NOTSUP;
    }
    
    const char *rel_path = get_rel_path(path, mount);
    return mount->ops->unlink(rel_path);
}

/* 完善 rename 实现 */
int vfs_rename_impl(const char *oldpath, const char *newpath) {
    if (oldpath == NULL || newpath == NULL) {
        return VFS_ERR_INVAL;
    }
    
    struct vfs_mount *mount = find_mount(oldpath);
    if (mount == NULL) {
        return VFS_ERR_NOTFOUND;
    }
    
    /* 检查目标是否在同一挂载点 */
    struct vfs_mount *new_mount = find_mount(newpath);
    if (new_mount != mount) {
        return VFS_ERR_CROSSDEV;  /* 跨设备重命名不支持 */
    }
    
    if (mount->ops == NULL || mount->ops->rename == NULL) {
        return VFS_ERR_NOTSUP;
    }
    
    const char *old_rel = get_rel_path(oldpath, mount);
    const char *new_rel = get_rel_path(newpath, mount);
    return mount->ops->rename(old_rel, new_rel);
}

/* VFS 统计信息 */
/* 获取挂载点列表 (供诊断模块使用) */
struct vfs_mount *vfs_get_mounts(void) {
    return mount_list;
}

void vfs_stats(void) {
    // println("╔══════════════════════════════════════════════════════════════╗");
    // println("║                    VFS Statistics                             ║");
    // println("╠══════════════════════════════════════════════════════════════╣");
    
    /* 统计挂载点数量 */
    int mount_count = 0;
    struct vfs_mount *m = mount_list;
    while (m != NULL) {
        mount_count++;
        m = m->next;
    }
    
    // print("║ Mount points: ");
    // print_dec(mount_count);
    // println("                                             ║");
    
    if (root_mount != NULL) {
        // print("║ Root device:  ");
        // print(root_mount->device);
        // println("                                       ║");
    }
    
    // println("╚══════════════════════════════════════════════════════════════╝");
    
    /* 列出所有挂载点 */
    if (mount_count > 0) {
        // println("");
        // println("Mounted filesystems:");
        m = mount_list;
        while (m != NULL) {
            // print("  ");
            // print(m->device);
            // print(" on ");
            // println(m->path);
            m = m->next;
        }
    }
}

/* 修改文件权限 */
int vfs_chmod(const char *path, uint32_t mode) {
    if (path == NULL) return -1;

    struct vfs_mount *mount = find_mount(path);
    if (mount == NULL) return -1;

    if (mount->ops == NULL || mount->ops->chmod == NULL) {
        return -1;
    }

    const char *rel_path = get_rel_path(path, mount);
    return mount->ops->chmod(rel_path, mode);
}

/* 修改文件所有者 */
int vfs_chown(const char *path, uint32_t uid, uint32_t gid) {
    if (path == NULL) return -1;

    struct vfs_mount *mount = find_mount(path);
    if (mount == NULL) return -1;

    if (mount->ops == NULL || mount->ops->chown == NULL) {
        return -1;
    }

    const char *rel_path = get_rel_path(path, mount);
    return mount->ops->chown(rel_path, uid, gid);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：Buffer Cache 实现
 *
 * 高性能文件系统缓存
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "disk.h"
#include "task.h"

/* 缓存块池 */
static struct bcache_block bcache_pool[BCACHE_NUM_BLOCKS];

/* 哈希表 */
static struct bcache_block *bcache_hash[BCACHE_HASH_SIZE];

/* LRU 链表 */
static struct bcache_block *lru_head = NULL;  /* 最少使用 */
static struct bcache_block *lru_tail = NULL;  /* 最近使用 */

/* 统计 */
static struct bcache_stats bcache_stats;

/* 预读控制 */
static struct bcache_readahead readahead;

/* 缓存锁 */
static volatile int bcache_lock = 0;

/* 初始化 Buffer Cache */
void bcache_init(void) {
    /* 清空缓存池 */
    memset(bcache_pool, 0, sizeof(bcache_pool));
    
    /* 初始化哈希表 */
    for (int i = 0; i < BCACHE_HASH_SIZE; i++) {
        bcache_hash[i] = NULL;
    }
    
    /* 初始化 LRU 链表 */
    lru_head = NULL;
    lru_tail = NULL;
    
    /* 初始化统计 */
    memset(&bcache_stats, 0, sizeof(bcache_stats));
    
    /* 初始化预读 */
    readahead.enabled = 1;
    readahead.seq_count = 0;
    readahead.ra_size = 4;  /* 初始预读 4 块 */
    
    // println("[BCACHE] Buffer cache initialized");
}

/* 计算哈希值 */
static inline uint32_t bcache_hash_func(uint32_t dev_id, uint64_t block_num) {
    return (dev_id ^ (block_num & 0xFF) ^ ((block_num >> 8) & 0xFF)) % BCACHE_HASH_SIZE;
}

/* 计算块 ID */
static inline uint64_t bcache_make_block_id(uint32_t dev_id, uint64_t block_num) {
    return ((uint64_t)dev_id << 48) | (block_num & 0xFFFFFFFFFFFFULL);
}

/* 从哈希表查找块 */
static struct bcache_block *bcache_hash_lookup(uint32_t dev_id, uint64_t block_num) {
    uint32_t hash = bcache_hash_func(dev_id, block_num);
    uint64_t target_id = bcache_make_block_id(dev_id, block_num);
    
    struct bcache_block *block = bcache_hash[hash];
    while (block != NULL) {
        if (block->block_id == target_id) {
            return block;
        }
        block = block->hash_next;
    }
    
    return NULL;
}

/* 添加块到哈希表 */
static void bcache_hash_insert(struct bcache_block *block) {
    uint32_t hash = bcache_hash_func(
        (block->block_id >> 48) & 0xFFFF,
        block->block_id & 0xFFFFFFFFFFFFULL
    );
    
    block->hash_next = bcache_hash[hash];
    bcache_hash[hash] = block;
}

/* 从哈希表移除块 */
static void bcache_hash_remove(struct bcache_block *block) {
    uint32_t hash = bcache_hash_func(
        (block->block_id >> 48) & 0xFFFF,
        block->block_id & 0xFFFFFFFFFFFFULL
    );
    
    if (bcache_hash[hash] == block) {
        bcache_hash[hash] = block->hash_next;
        return;
    }
    
    struct bcache_block *prev = bcache_hash[hash];
    while (prev != NULL && prev->hash_next != block) {
        prev = prev->hash_next;
    }
    
    if (prev != NULL) {
        prev->hash_next = block->hash_next;
    }
}

/* 将块移到 LRU 链表尾部 (最近使用) */
static void bcache_lru_touch(struct bcache_block *block) {
    /* 如果已经在尾部，无需移动 */
    if (block == lru_tail) {
        return;
    }
    
    /* 从当前位置移除 */
    if (block->lru_prev != NULL) {
        block->lru_prev->lru_next = block->lru_next;
    } else if (lru_head == block) {
        lru_head = block->lru_next;
    }
    
    if (block->lru_next != NULL) {
        block->lru_next->lru_prev = block->lru_prev;
    }
    
    /* 添加到尾部 */
    block->lru_prev = lru_tail;
    block->lru_next = NULL;
    
    if (lru_tail != NULL) {
        lru_tail->lru_next = block;
    }
    lru_tail = block;
    
    if (lru_head == NULL) {
        lru_head = block;
    }
}

/* 获取一个空闲块 (可能需要淘汰) */
static struct bcache_block *bcache_get_free_block(void) {
    /* 首先查找未使用的块 */
    for (int i = 0; i < BCACHE_NUM_BLOCKS; i++) {
        if (bcache_pool[i].block_id == 0 && bcache_pool[i].ref_count == 0) {
            return &bcache_pool[i];
        }
    }
    
    /* 需要淘汰 LRU 块 */
    struct bcache_block *victim = lru_head;
    while (victim != NULL) {
        if (victim->ref_count == 0 && !(victim->state & BCACHE_STATE_LOCKED)) {
            /* 如果是脏块，需要写回 */
            if (victim->state & BCACHE_STATE_DIRTY) {
                uint64_t block_num = victim->block_id & 0xFFFFFFFFFFFFULL;
                disk_write_sector(block_num, victim->data);
                bcache_stats.writebacks++;
                victim->state &= ~BCACHE_STATE_DIRTY;
                bcache_stats.dirty_blocks--;
            }
            
            /* 从哈希表移除 */
            bcache_hash_remove(victim);
            
            /* 从 LRU 链表移除 */
            if (victim->lru_prev != NULL) {
                victim->lru_prev->lru_next = victim->lru_next;
            } else {
                lru_head = victim->lru_next;
            }
            if (victim->lru_next != NULL) {
                victim->lru_next->lru_prev = victim->lru_prev;
            } else {
                lru_tail = victim->lru_prev;
            }
            
            bcache_stats.evictions++;
            bcache_stats.used_blocks--;
            
            /* 重置块 */
            memset(victim, 0, sizeof(*victim));
            return victim;
        }
        victim = victim->lru_next;
    }
    
    return NULL;  /* 没有可用块 */
}

/* 读取缓存块 */
int bcache_read(uint32_t dev_id, uint64_t block_num, void *buf) {
    if (buf == NULL) return -1;
    
    /* 获取锁 */
    while (__sync_lock_test_and_set(&bcache_lock, 1)) {
        asm volatile("pause");
    }
    
    bcache_stats.reads++;
    
    /* 查找缓存 */
    struct bcache_block *block = bcache_hash_lookup(dev_id, block_num);
    
    if (block != NULL) {
        /* 缓存命中 */
        bcache_stats.hits++;
        block->hit_count++;
        block->access_time = get_tick_count();
        __sync_fetch_and_add(&block->ref_count, 1);  /* 原子增加引用计数 */
        bcache_lru_touch(block);
        
        /* 复制数据 */
        memcpy(buf, block->data, BCACHE_BLOCK_SIZE);
        
        __sync_fetch_and_sub(&block->ref_count, 1);  /* 原子减少引用计数 */
        __sync_lock_release(&bcache_lock);
        return 0;
    }
    
    /* 缓存未命中 */
    bcache_stats.misses++;
    
    /* 获取空闲块 */
    block = bcache_get_free_block();
    if (block == NULL) {
        __sync_lock_release(&bcache_lock);
        return -1;
    }
    
    /* 设置块信息 */
    block->block_id = bcache_make_block_id(dev_id, block_num);
    block->state = BCACHE_STATE_READING;
    __sync_lock_test_and_set(&block->ref_count, 1);  /* 设置初始引用计数为1 */
    block->access_time = get_tick_count();
    
    /* 添加到哈希表 */
    bcache_hash_insert(block);
    
    /* 添加到 LRU 链表 */
    bcache_lru_touch(block);
    
    bcache_stats.used_blocks++;
    
    /* 关键修复：分配临时缓冲区用于磁盘读取 */
    /* 这样在锁外读取时不会直接访问 block->data */
    uint8_t *temp_buf = (uint8_t *)kmalloc(BCACHE_BLOCK_SIZE);
    if (temp_buf == NULL) {
        /* 内存不足，回退：使用块数据但保持引用计数 */
        __sync_lock_release(&bcache_lock);
        
        int ret = disk_read_sector(block_num, block->data);
        
        /* 重新获取锁更新状态 */
        while (__sync_lock_test_and_set(&bcache_lock, 1)) {
            asm volatile("pause");
        }
        block->state = BCACHE_STATE_CLEAN;
        __sync_fetch_and_sub(&block->ref_count, 1);
        if (ret == 0) {
            memcpy(buf, block->data, BCACHE_BLOCK_SIZE);
        }
        __sync_lock_release(&bcache_lock);
        return ret;
    }
    
    /* 保存块ID用于后续验证 */
    uint64_t saved_block_id = block->block_id;
    
    __sync_lock_release(&bcache_lock);
    
    /* 从磁盘读取到临时缓冲区 (在锁外执行，安全) */
    int ret = disk_read_sector(block_num, temp_buf);
    
    /* 重新获取锁更新状态 */
    while (__sync_lock_test_and_set(&bcache_lock, 1)) {
        asm volatile("pause");
    }
    
    /* 验证块仍然有效 (防止期间被淘汰或替换) */
    block = bcache_hash_lookup(dev_id, block_num);
    if (block != NULL && block->block_id == saved_block_id) {
        /* 块仍然有效，更新数据 */
        memcpy(block->data, temp_buf, BCACHE_BLOCK_SIZE);
        block->state = BCACHE_STATE_CLEAN;
        __sync_fetch_and_sub(&block->ref_count, 1);
        
        if (ret == 0) {
            memcpy(buf, block->data, BCACHE_BLOCK_SIZE);
        }
    } else {
        /* 块已被替换，直接使用临时数据 */
        if (ret == 0) {
            memcpy(buf, temp_buf, BCACHE_BLOCK_SIZE);
        }
        /* 这不是错误，只是缓存竞争 */
    }
    
    kfree(temp_buf);
    
    /* 预读优化 - 必须在锁外执行，避免死锁（bcache_prefetch内部也会获取锁） */
    if (readahead.enabled) {
        /* 检测顺序访问 */
        if (block_num == readahead.last_block + 1) {
            readahead.seq_count++;
            if (readahead.seq_count >= 2) {
                /* 先更新预读状态，再释放锁 */
                readahead.last_block = block_num;
                readahead.ra_size = readahead.ra_size * 2;
                if (readahead.ra_size > 32) readahead.ra_size = 32;
                
                /* 释放锁再触发预读，避免死锁 */
                __sync_lock_release(&bcache_lock);
                bcache_prefetch(dev_id, block_num + 1, readahead.ra_size);
                return ret;  /* 已释放锁，直接返回 */
            }
        } else {
            readahead.seq_count = 0;
            readahead.ra_size = 4;
        }
        readahead.last_block = block_num;
    }
    
    __sync_lock_release(&bcache_lock);
    
    return ret;
}

/* 写入缓存块 */
int bcache_write(uint32_t dev_id, uint64_t block_num, const void *buf) {
    if (buf == NULL) return -1;
    
    /* 获取锁 */
    while (__sync_lock_test_and_set(&bcache_lock, 1)) {
        asm volatile("pause");
    }
    
    bcache_stats.writes++;
    
    /* 查找或创建缓存块 */
    struct bcache_block *block = bcache_hash_lookup(dev_id, block_num);
    
    if (block == NULL) {
        /* 需要分配新块 */
        block = bcache_get_free_block();
        if (block == NULL) {
            __sync_lock_release(&bcache_lock);
            return -1;
        }
        
        block->block_id = bcache_make_block_id(dev_id, block_num);
        block->state = BCACHE_STATE_DIRTY;
        block->ref_count = 1;
        block->access_time = get_tick_count();
        
        bcache_hash_insert(block);
        bcache_lru_touch(block);
        bcache_stats.used_blocks++;
        bcache_stats.dirty_blocks++;
    } else {
        block->ref_count++;
        block->access_time = get_tick_count();
        bcache_lru_touch(block);
    }
    
    /* 写入数据 */
    memcpy(block->data, buf, BCACHE_BLOCK_SIZE);
    block->state |= BCACHE_STATE_DIRTY;
    
    block->ref_count--;
    __sync_lock_release(&bcache_lock);
    
    return 0;
}

/* 标记块为脏 */
void bcache_mark_dirty(uint32_t dev_id, uint64_t block_num) {
    while (__sync_lock_test_and_set(&bcache_lock, 1)) {
        asm volatile("pause");
    }
    
    struct bcache_block *block = bcache_hash_lookup(dev_id, block_num);
    if (block != NULL && !(block->state & BCACHE_STATE_DIRTY)) {
        block->state |= BCACHE_STATE_DIRTY;
        bcache_stats.dirty_blocks++;
    }
    
    __sync_lock_release(&bcache_lock);
}

/* 同步脏块到磁盘 */
int bcache_sync(uint32_t dev_id) {
    int synced = 0;
    
    while (__sync_lock_test_and_set(&bcache_lock, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < BCACHE_NUM_BLOCKS; i++) {
        struct bcache_block *block = &bcache_pool[i];
        uint32_t block_dev = (block->block_id >> 48) & 0xFFFF;
        
        if (block_dev == dev_id && (block->state & BCACHE_STATE_DIRTY)) {
            uint64_t block_num = block->block_id & 0xFFFFFFFFFFFFULL;
            
            /* 写入磁盘 */
            disk_write_sector(block_num, block->data);
            
            block->state &= ~BCACHE_STATE_DIRTY;
            bcache_stats.writebacks++;
            bcache_stats.dirty_blocks--;
            synced++;
        }
    }
    
    __sync_lock_release(&bcache_lock);
    
    return synced;
}

/* 同步所有脏块 */
int bcache_sync_all(void) {
    int synced = 0;
    
    while (__sync_lock_test_and_set(&bcache_lock, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < BCACHE_NUM_BLOCKS; i++) {
        struct bcache_block *block = &bcache_pool[i];
        
        if (block->state & BCACHE_STATE_DIRTY) {
            uint64_t block_num = block->block_id & 0xFFFFFFFFFFFFULL;
            
            disk_write_sector(block_num, block->data);
            
            block->state &= ~BCACHE_STATE_DIRTY;
            bcache_stats.writebacks++;
            bcache_stats.dirty_blocks--;
            synced++;
        }
    }
    
    bcache_stats.dirty_blocks = 0;
    __sync_lock_release(&bcache_lock);
    
    return synced;
}

/* 使缓存失效 */
void bcache_invalidate(uint32_t dev_id, uint64_t block_num) {
    while (__sync_lock_test_and_set(&bcache_lock, 1)) {
        asm volatile("pause");
    }
    
    struct bcache_block *block = bcache_hash_lookup(dev_id, block_num);
    if (block != NULL) {
        bcache_hash_remove(block);
        
        if (block->lru_prev != NULL) {
            block->lru_prev->lru_next = block->lru_next;
        } else {
            lru_head = block->lru_next;
        }
        if (block->lru_next != NULL) {
            block->lru_next->lru_prev = block->lru_prev;
        } else {
            lru_tail = block->lru_prev;
        }
        
        if (block->state & BCACHE_STATE_DIRTY) {
            /* Write back dirty data before invalidating */
            uint64_t bn = block->block_id & 0xFFFFFFFFFFFFULL;
            disk_write_sector(bn, block->data);
            bcache_stats.writebacks++;
            bcache_stats.dirty_blocks--;
        }
        bcache_stats.used_blocks--;

        memset(block, 0, sizeof(*block));
    }

    __sync_lock_release(&bcache_lock);
}

/* 使设备所有缓存失效 */
void bcache_invalidate_dev(uint32_t dev_id) {
    while (__sync_lock_test_and_set(&bcache_lock, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < BCACHE_NUM_BLOCKS; i++) {
        struct bcache_block *block = &bcache_pool[i];
        uint32_t block_dev = (block->block_id >> 48) & 0xFFFF;
        
        if (block_dev == dev_id) {
            bcache_hash_remove(block);
            
            if (block->lru_prev != NULL) {
                block->lru_prev->lru_next = block->lru_next;
            } else {
                lru_head = block->lru_next;
            }
            if (block->lru_next != NULL) {
                block->lru_next->lru_prev = block->lru_prev;
            } else {
                lru_tail = block->lru_prev;
            }
            
            if (block->state & BCACHE_STATE_DIRTY) {
                /* Write back dirty data before invalidating */
                uint64_t bn = block->block_id & 0xFFFFFFFFFFFFULL;
                disk_write_sector(bn, block->data);
                bcache_stats.writebacks++;
                bcache_stats.dirty_blocks--;
            }
            bcache_stats.used_blocks--;
            
            memset(block, 0, sizeof(*block));
        }
    }
    
    __sync_lock_release(&bcache_lock);
}

/* 获取缓存统计 */
void bcache_get_stats(struct bcache_stats *stats) {
    if (stats != NULL) {
        *stats = bcache_stats;
    }
}

/* 预读 */
int bcache_prefetch(uint32_t dev_id, uint64_t start_block, uint32_t count) {
    int prefetched = 0;
    
    for (uint32_t i = 0; i < count; i++) {
        /* 获取锁 */
        while (__sync_lock_test_and_set(&bcache_lock, 1)) {
            asm volatile("pause");
        }
        
        /* 检查是否已在缓存中 */
        struct bcache_block *block = bcache_hash_lookup(dev_id, start_block + i);
        
        if (block == NULL) {
            /* 获取空闲块 */
            block = bcache_get_free_block();
            if (block != NULL) {
                block->block_id = bcache_make_block_id(dev_id, start_block + i);
                block->state = BCACHE_STATE_CLEAN;
                block->access_time = get_tick_count();
                
                bcache_hash_insert(block);
                bcache_lru_touch(block);
                bcache_stats.used_blocks++;
                bcache_stats.prefetches++;
                prefetched++;
                
                __sync_lock_release(&bcache_lock);
                
                /* 读取数据 */
                disk_read_sector(start_block + i, block->data);
                continue;
            }
        }
        
        __sync_lock_release(&bcache_lock);
    }
    
    return prefetched;
}

/* 设置预读参数 */
void bcache_set_readahead(int enabled, uint32_t size) {
    readahead.enabled = enabled;
    if (size > 0) {
        readahead.ra_size = size;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：Page Cache 页面缓存
 * 类似 Linux 的页面缓存，缓存文件数据到内存
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 页面缓存配置 */
#define PAGE_CACHE_SIZE         4096        /* 页面大小 */
#define PAGE_CACHE_NUM_PAGES    8192        /* 缓存页面数 (32MB) */
#define PAGE_CACHE_HASH_SIZE    8192        /* 哈希表大小 */

/* 页面缓存状态 */
#define PC_STATE_FREE           0
#define PC_STATE_CLEAN          1
#define PC_STATE_DIRTY          2
#define PC_STATE_LOCKED         4
#define PC_STATE_UPTODATE       8

/* 页面缓存条目 */
struct page_cache_entry {
    uint64_t page_id;          /* (dev_id << 48) | (inode << 16) | page_index */
    uint32_t state;
    uint32_t ref_count;
    uint64_t access_time;
    uint64_t modify_time;
    
    /* 链表 */
    struct page_cache_entry *hash_next;
    struct page_cache_entry *lru_prev;
    struct page_cache_entry *lru_next;
    
    /* 页面数据 */
    uint8_t data[PAGE_CACHE_SIZE];
};

/* 页面缓存统计 */
static struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t writebacks;
    uint32_t used_pages;
    uint32_t dirty_pages;
} page_cache_stats;

/* 页面缓存池 */
static struct page_cache_entry page_cache_pool[PAGE_CACHE_NUM_PAGES];

/* 哈希表 */
static struct page_cache_entry *page_cache_hash[PAGE_CACHE_HASH_SIZE];

/* LRU 链表 */
static struct page_cache_entry *page_cache_lru_head;
static struct page_cache_entry *page_cache_lru_tail;

/* 页面缓存锁 */
static volatile int page_cache_lock = 0;

/* 初始化页面缓存 */
void page_cache_init(void) {
    memset(page_cache_pool, 0, sizeof(page_cache_pool));
    memset(page_cache_hash, 0, sizeof(page_cache_hash));
    memset(&page_cache_stats, 0, sizeof(page_cache_stats));
    
    page_cache_lru_head = NULL;
    page_cache_lru_tail = NULL;
    
    // println("[VFS] Page cache initialized");
}

/* 哈希函数 */
static uint32_t page_cache_hash_fn(uint64_t page_id) {
    return (uint32_t)((page_id ^ (page_id >> 16) ^ (page_id >> 32)) % PAGE_CACHE_HASH_SIZE);
}

/* 创建页面 ID */
static uint64_t make_page_id(uint32_t dev_id, uint64_t inode, uint64_t page_idx) {
    return ((uint64_t)dev_id << 48) | ((inode & 0xFFFFFFFF) << 16) | (page_idx & 0xFFFF);
}

/* 哈希查找 */
static struct page_cache_entry *page_cache_hash_lookup(uint64_t page_id) {
    uint32_t idx = page_cache_hash_fn(page_id);
    struct page_cache_entry *entry = page_cache_hash[idx];
    
    while (entry != NULL) {
        if (entry->page_id == page_id) {
            return entry;
        }
        entry = entry->hash_next;
    }
    
    return NULL;
}

/* 哈希插入 */
static void page_cache_hash_insert(struct page_cache_entry *entry) {
    uint32_t idx = page_cache_hash_fn(entry->page_id);
    entry->hash_next = page_cache_hash[idx];
    page_cache_hash[idx] = entry;
}

/* 哈希移除 */
static void page_cache_hash_remove(struct page_cache_entry *entry) {
    uint32_t idx = page_cache_hash_fn(entry->page_id);
    
    if (page_cache_hash[idx] == entry) {
        page_cache_hash[idx] = entry->hash_next;
    } else {
        struct page_cache_entry *prev = page_cache_hash[idx];
        while (prev != NULL && prev->hash_next != entry) {
            prev = prev->hash_next;
        }
        if (prev != NULL) {
            prev->hash_next = entry->hash_next;
        }
    }
}

/* LRU 触摸 */
static void page_cache_lru_touch(struct page_cache_entry *entry) {
    /* 移到 LRU 头部 */
    if (entry == page_cache_lru_head) {
        return;
    }
    
    /* 从当前位置移除 */
    if (entry->lru_prev != NULL) {
        entry->lru_prev->lru_next = entry->lru_next;
    } else {
        page_cache_lru_head = entry->lru_next;
    }
    
    if (entry->lru_next != NULL) {
        entry->lru_next->lru_prev = entry->lru_prev;
    } else {
        page_cache_lru_tail = entry->lru_prev;
    }
    
    /* 插入头部 */
    entry->lru_prev = NULL;
    entry->lru_next = page_cache_lru_head;
    
    if (page_cache_lru_head != NULL) {
        page_cache_lru_head->lru_prev = entry;
    }
    page_cache_lru_head = entry;
    
    if (page_cache_lru_tail == NULL) {
        page_cache_lru_tail = entry;
    }
}

/* 获取空闲页面 */
static struct page_cache_entry *page_cache_get_free(void) {
    /* 先查找空闲页面 */
    for (int i = 0; i < PAGE_CACHE_NUM_PAGES; i++) {
        if (page_cache_pool[i].state == PC_STATE_FREE) {
            return &page_cache_pool[i];
        }
    }
    
    /* 没有空闲页面，从 LRU 尾部驱逐 */
    struct page_cache_entry *victim = page_cache_lru_tail;
    
    while (victim != NULL) {
        if (!(victim->state & PC_STATE_LOCKED) && victim->ref_count == 0) {
            /* 如果是脏页，先写回 */
            if (victim->state & PC_STATE_DIRTY) {
                /* TODO: 写回磁盘 */
                page_cache_stats.writebacks++;
                victim->state &= ~PC_STATE_DIRTY;
                page_cache_stats.dirty_pages--;
            }
            
            /* 从哈希表移除 */
            page_cache_hash_remove(victim);
            
            /* 更新统计 */
            page_cache_stats.evictions++;
            page_cache_stats.used_pages--;
            
            return victim;
        }
        victim = victim->lru_prev;
    }
    
    return NULL;
}

/* 读取页面 */
int page_cache_read(uint32_t dev_id, uint64_t inode, uint64_t page_idx, 
                     void *buf, size_t offset, size_t size) {
    if (buf == NULL || offset >= PAGE_CACHE_SIZE) return -1;
    if (offset + size > PAGE_CACHE_SIZE) size = PAGE_CACHE_SIZE - offset;
    
    uint64_t page_id = make_page_id(dev_id, inode, page_idx);
    
    while (__sync_lock_test_and_set(&page_cache_lock, 1)) {
        asm volatile("pause");
    }
    
    struct page_cache_entry *entry = page_cache_hash_lookup(page_id);
    
    if (entry != NULL) {
        /* 命中 */
        page_cache_stats.hits++;
        entry->ref_count++;
        entry->access_time = get_tick_count();
        page_cache_lru_touch(entry);
        
        /* 等待页面解锁 */
        while (entry->state & PC_STATE_LOCKED) {
            __sync_lock_release(&page_cache_lock);
            asm volatile("pause");
            while (__sync_lock_test_and_set(&page_cache_lock, 1)) {
                asm volatile("pause");
            }
        }
        
        /* 复制数据 */
        if (entry->state & PC_STATE_UPTODATE) {
            memcpy(buf, entry->data + offset, size);
            entry->ref_count--;
            __sync_lock_release(&page_cache_lock);
            return size;
        }
        
        entry->ref_count--;
    }
    
    /* 未命中 */
    page_cache_stats.misses++;
    
    /* 获取空闲页面 */
    entry = page_cache_get_free();
    if (entry == NULL) {
        __sync_lock_release(&page_cache_lock);
        return -1;
    }
    
    /* 初始化新页面 */
    entry->page_id = page_id;
    entry->state = PC_STATE_LOCKED;
    entry->ref_count = 1;
    entry->access_time = get_tick_count();
    
    page_cache_hash_insert(entry);
    page_cache_lru_touch(entry);
    page_cache_stats.used_pages++;
    
    __sync_lock_release(&page_cache_lock);
    
    /* 从磁盘读取 (简化实现) */
    /* TODO: 实际的磁盘读取 */
    memset(entry->data, 0, PAGE_CACHE_SIZE);
    
    /* 更新状态 */
    while (__sync_lock_test_and_set(&page_cache_lock, 1)) {
        asm volatile("pause");
    }
    entry->state = PC_STATE_UPTODATE;
    entry->ref_count--;
    __sync_lock_release(&page_cache_lock);
    
    memcpy(buf, entry->data + offset, size);
    return size;
}

/* 写入页面 */
int page_cache_write(uint32_t dev_id, uint64_t inode, uint64_t page_idx,
                      const void *buf, size_t offset, size_t size) {
    if (buf == NULL || offset >= PAGE_CACHE_SIZE) return -1;
    if (offset + size > PAGE_CACHE_SIZE) size = PAGE_CACHE_SIZE - offset;
    
    uint64_t page_id = make_page_id(dev_id, inode, page_idx);
    
    while (__sync_lock_test_and_set(&page_cache_lock, 1)) {
        asm volatile("pause");
    }
    
    struct page_cache_entry *entry = page_cache_hash_lookup(page_id);
    
    if (entry == NULL) {
        /* 创建新页面 */
        entry = page_cache_get_free();
        if (entry == NULL) {
            __sync_lock_release(&page_cache_lock);
            return -1;
        }
        
        entry->page_id = page_id;
        entry->state = 0;
        entry->ref_count = 0;
        entry->access_time = get_tick_count();
        memset(entry->data, 0, PAGE_CACHE_SIZE);
        
        page_cache_hash_insert(entry);
        page_cache_lru_touch(entry);
        page_cache_stats.used_pages++;
    }
    
    /* 等待页面解锁 */
    while (entry->state & PC_STATE_LOCKED) {
        __sync_lock_release(&page_cache_lock);
        asm volatile("pause");
        while (__sync_lock_test_and_set(&page_cache_lock, 1)) {
            asm volatile("pause");
        }
    }
    
    /* 写入数据 */
    memcpy(entry->data + offset, buf, size);
    entry->state |= PC_STATE_DIRTY | PC_STATE_UPTODATE;
    entry->modify_time = get_tick_count();
    
    page_cache_stats.dirty_pages++;
    
    __sync_lock_release(&page_cache_lock);
    return size;
}

/* 刷新脏页 */
void page_cache_flush(void) {
    while (__sync_lock_test_and_set(&page_cache_lock, 1)) {
        asm volatile("pause");
    }
    
    for (int i = 0; i < PAGE_CACHE_NUM_PAGES; i++) {
        struct page_cache_entry *entry = &page_cache_pool[i];
        
        if (entry->state & PC_STATE_DIRTY) {
            /* TODO: 实际的磁盘写入 */
            entry->state &= ~PC_STATE_DIRTY;
            page_cache_stats.writebacks++;
        }
    }
    
    page_cache_stats.dirty_pages = 0;
    __sync_lock_release(&page_cache_lock);
}

/* 获取页面缓存统计 */
void page_cache_get_stats(uint64_t *hits, uint64_t *misses, 
                          uint32_t *used, uint32_t *dirty) {
    if (hits) *hits = page_cache_stats.hits;
    if (misses) *misses = page_cache_stats.misses;
    if (used) *used = page_cache_stats.used_pages;
    if (dirty) *dirty = page_cache_stats.dirty_pages;
}

/* 打印页面缓存统计 */
void page_cache_print_stats(void) {
    // println("=== Page Cache Statistics ===");
    
    // print("Hits: ");
    // print_dec(page_cache_stats.hits);
    // print("  Misses: ");
    // print_dec(page_cache_stats.misses);
    // println("");
    
    uint64_t total = page_cache_stats.hits + page_cache_stats.misses;
    if (total > 0) {
        // print("Hit rate: ");
        // print_dec((page_cache_stats.hits * 100) / total);
        // println("%");
    }
    
    // print("Used pages: ");
    // print_dec(page_cache_stats.used_pages);
    // print(" / ");
    // print_dec(PAGE_CACHE_NUM_PAGES);
    // println("");
    
    // print("Dirty pages: ");
    // print_dec(page_cache_stats.dirty_pages);
    // println("");
    
    // print("Evictions: ");
    // print_dec(page_cache_stats.evictions);
    // print("  Writebacks: ");
    // print_dec(page_cache_stats.writebacks);
    // println("");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：自适应预读
 * 根据访问模式自动调整预读策略
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 预读状态机 */
struct readahead_state {
    uint64_t last_offset;       /* 上次读取偏移 */
    uint64_t start;             /* 预读起始 */
    uint32_t size;              /* 预读大小 */
    uint32_t ra_pages;          /* 预读页面数 */
    int sequential;             /* 顺序访问标志 */
    int hit_cache;              /* 缓存命中标志 */
};

static struct readahead_state ra_states[MAX_CPUS];

/* 自适应预读 */
int adaptive_readahead(uint32_t dev_id, uint64_t inode, uint64_t offset, 
                        uint32_t *ra_size) {
    int cpu = smp_processor_id();
    struct readahead_state *ra = &ra_states[cpu];
    
    /* 检测访问模式 */
    if (offset == ra->last_offset + 1 || 
        (ra->last_offset > 0 && offset == ra->last_offset + PAGE_CACHE_SIZE)) {
        /* 顺序访问 */
        ra->sequential++;
        if (ra->sequential > 3) {
            /* 增加预读窗口 */
            ra->ra_pages = ra->ra_pages * 2;
            if (ra->ra_pages > 64) ra->ra_pages = 64;
        }
    } else if (offset == ra->last_offset) {
        /* 重复访问，可能在等待 */
        ra->hit_cache = 1;
    } else {
        /* 随机访问 */
        ra->sequential = 0;
        ra->ra_pages = 4;
    }
    
    ra->last_offset = offset;
    ra->start = offset + PAGE_CACHE_SIZE;
    ra->size = ra->ra_pages * PAGE_CACHE_SIZE;
    
    if (ra_size != NULL) {
        *ra_size = ra->ra_pages;
    }
    
    return ra->sequential > 1 ? 1 : 0;
}

/* 执行预读 */
void perform_readahead(uint32_t dev_id, uint64_t inode, uint64_t start_page, 
                        uint32_t num_pages) {
    for (uint32_t i = 0; i < num_pages; i++) {
        /* 预读页面 */
        page_cache_read(dev_id, inode, start_page + i, NULL, 0, 0);
    }
}

/* ========== VFS 高级增强功能 ========== */

/* === 目录项缓存 (Dentry Cache) === */

#define DCACHE_HASH_SIZE    1024
#define DCACHE_MAX_ENTRIES  4096

struct dentry {
    char name[256];
    uint64_t inode;
    uint32_t dev_id;
    uint32_t flags;
    uint32_t ref_count;
    uint64_t access_time;
    struct dentry *parent;
    struct dentry *child;
    struct dentry *sibling;
    struct dentry *hash_next;
    struct dentry *lru_next;
    struct dentry *lru_prev;
};

static struct dentry *dcache_hash[DCACHE_HASH_SIZE];
static struct dentry dentry_pool[DCACHE_MAX_ENTRIES];
static struct dentry *dentry_free_list;
static struct dentry *dentry_lru_head;
static struct dentry *dentry_lru_tail;
static uint32_t dcache_count;
static spinlock_t dcache_lock = SPINLOCK_INIT;

/* 初始化目录缓存 */
void dcache_init(void) {
    dcache_count = 0;
    dentry_lru_head = NULL;
    dentry_lru_tail = NULL;
    
    for (int i = 0; i < DCACHE_HASH_SIZE; i++) {
        dcache_hash[i] = NULL;
    }
    
    /* 初始化空闲链表 */
    dentry_free_list = NULL;
    for (int i = 0; i < DCACHE_MAX_ENTRIES; i++) {
        dentry_pool[i].ref_count = 0;
        dentry_pool[i].hash_next = dentry_free_list;
        dentry_free_list = &dentry_pool[i];
    }
}

/* 哈希函数 */
static inline uint32_t dcache_hash_func(const char *name, uint32_t dev_id) {
    uint32_t hash = dev_id;
    while (*name) {
        hash = hash * 31 + *name++;
    }
    return hash % DCACHE_HASH_SIZE;
}

/* 查找目录项 */
struct dentry *dcache_lookup(const char *name, uint32_t dev_id) {
    if (name == NULL) return NULL;
    
    uint32_t hash = dcache_hash_func(name, dev_id);
    
    spin_lock(&dcache_lock);
    
    struct dentry *d = dcache_hash[hash];
    while (d != NULL) {
        if (d->dev_id == dev_id && strcmp(d->name, name) == 0) {
            d->ref_count++;
            d->access_time = get_tick_count();
            
            /* 移动到 LRU 头部 */
            if (d != dentry_lru_head) {
                if (d->lru_prev) d->lru_prev->lru_next = d->lru_next;
                if (d->lru_next) d->lru_next->lru_prev = d->lru_prev;
                if (d == dentry_lru_tail) dentry_lru_tail = d->lru_prev;
                
                d->lru_prev = NULL;
                d->lru_next = dentry_lru_head;
                if (dentry_lru_head) dentry_lru_head->lru_prev = d;
                dentry_lru_head = d;
            }
            
            spin_unlock(&dcache_lock);
            return d;
        }
        d = d->hash_next;
    }
    
    spin_unlock(&dcache_lock);
    return NULL;
}

/* 添加目录项 */
struct dentry *dcache_add(const char *name, uint32_t dev_id, uint64_t inode) {
    if (name == NULL) return NULL;
    
    spin_lock(&dcache_lock);
    
    /* 从空闲链表获取 */
    if (dentry_free_list == NULL) {
        /* LRU 淘汰 */
        if (dentry_lru_tail != NULL && dentry_lru_tail->ref_count == 0) {
            struct dentry *victim = dentry_lru_tail;
            
            /* 从哈希表移除 */
            uint32_t hash = dcache_hash_func(victim->name, victim->dev_id);
            struct dentry **pp = &dcache_hash[hash];
            while (*pp != NULL) {
                if (*pp == victim) {
                    *pp = victim->hash_next;
                    break;
                }
                pp = &(*pp)->hash_next;
            }
            
            /* 从 LRU 链表移除 */
            if (victim->lru_prev) victim->lru_prev->lru_next = victim->lru_next;
            if (victim->lru_next) victim->lru_next->lru_prev = victim->lru_prev;
            
            dentry_free_list = victim;
            dentry_lru_tail = victim->lru_prev;
            dcache_count--;
        }
    }
    
    if (dentry_free_list == NULL) {
        spin_unlock(&dcache_lock);
        return NULL;
    }
    
    struct dentry *d = dentry_free_list;
    dentry_free_list = d->hash_next;
    
    /* 初始化 */
    strncpy(d->name, name, sizeof(d->name) - 1);
    d->name[sizeof(d->name) - 1] = '\0';
    d->inode = inode;
    d->dev_id = dev_id;
    d->flags = 0;
    d->ref_count = 1;
    d->access_time = get_tick_count();
    
    /* 添加到哈希表 */
    uint32_t hash = dcache_hash_func(name, dev_id);
    d->hash_next = dcache_hash[hash];
    dcache_hash[hash] = d;
    
    /* 添加到 LRU 头部 */
    d->lru_prev = NULL;
    d->lru_next = dentry_lru_head;
    if (dentry_lru_head) dentry_lru_head->lru_prev = d;
    dentry_lru_head = d;
    if (dentry_lru_tail == NULL) dentry_lru_tail = d;
    
    dcache_count++;
    
    spin_unlock(&dcache_lock);
    return d;
}

/* 释放目录项 */
void dcache_put(struct dentry *d) {
    if (d == NULL) return;
    
    spin_lock(&dcache_lock);
    
    if (d->ref_count > 0) {
        d->ref_count--;
    }
    
    spin_unlock(&dcache_lock);
}

/* === Inode 缓存 === */

#define ICACHE_HASH_SIZE    512
#define ICACHE_MAX_ENTRIES  2048

struct inode_cache_entry {
    uint64_t inode;
    uint32_t dev_id;
    uint32_t ref_count;
    uint32_t flags;
    uint64_t size;
    uint64_t mode;
    uint64_t atime, mtime, ctime;
    struct inode_cache_entry *hash_next;
    struct inode_cache_entry *lru_next;
    struct inode_cache_entry *lru_prev;
};

static struct inode_cache_entry *icache_hash[ICACHE_HASH_SIZE];
static struct inode_cache_entry inode_pool[ICACHE_MAX_ENTRIES];
static struct inode_cache_entry *inode_free_list;
static struct inode_cache_entry *inode_lru_head;
static struct inode_cache_entry *inode_lru_tail;
static uint32_t icache_count;
static spinlock_t icache_lock = SPINLOCK_INIT;

/* 初始化 Inode 缓存 */
void icache_init(void) {
    icache_count = 0;
    inode_lru_head = NULL;
    inode_lru_tail = NULL;
    
    for (int i = 0; i < ICACHE_HASH_SIZE; i++) {
        icache_hash[i] = NULL;
    }
    
    inode_free_list = NULL;
    for (int i = 0; i < ICACHE_MAX_ENTRIES; i++) {
        inode_pool[i].ref_count = 0;
        inode_pool[i].hash_next = inode_free_list;
        inode_free_list = &inode_pool[i];
    }
}

/* 查找 Inode */
struct inode_cache_entry *icache_lookup(uint32_t dev_id, uint64_t inode) {
    uint32_t hash = (inode ^ dev_id) % ICACHE_HASH_SIZE;
    
    spin_lock(&icache_lock);
    
    struct inode_cache_entry *e = icache_hash[hash];
    while (e != NULL) {
        if (e->dev_id == dev_id && e->inode == inode) {
            e->ref_count++;
            spin_unlock(&icache_lock);
            return e;
        }
        e = e->hash_next;
    }
    
    spin_unlock(&icache_lock);
    return NULL;
}

/* === 文件锁机制 === */

#define FL_POSIX    1
#define FL_FLOCK    2
#define FL_LEASE    3

#define LOCK_SH     1   /* 共享锁 */
#define LOCK_EX     2   /* 排他锁 */
#define LOCK_UN     8   /* 解锁 */
#define LOCK_NB     4   /* 非阻塞 */

struct file_lock {
    uint64_t inode;
    uint32_t dev_id;
    uint32_t pid;
    uint32_t type;          /* FL_POSIX, FL_FLOCK */
    uint32_t flags;         /* LOCK_SH, LOCK_EX 等 */
    uint64_t start;         /* 锁范围起始 (POSIX 范围锁) */
    uint64_t end;           /* 锁范围结束 (0 表示文件末尾) */
    struct file_lock *next;
};

#define MAX_FILE_LOCKS    256
static struct file_lock file_locks[MAX_FILE_LOCKS];
static struct file_lock *file_lock_list;
static spinlock_t file_lock_lock = SPINLOCK_INIT;

/* 初始化文件锁 */
void file_lock_init(void) {
    file_lock_list = NULL;
    
    for (int i = 0; i < MAX_FILE_LOCKS; i++) {
        file_locks[i].inode = 0;
        file_locks[i].next = NULL;
    }
}

/* 检查锁冲突 */
static int lock_conflicts(struct file_lock *new_lock, struct file_lock *existing) {
    if (new_lock->inode != existing->inode) return 0;
    if (new_lock->dev_id != existing->dev_id) return 0;
    /* 同一进程的锁不冲突 */
    if (new_lock->pid == existing->pid) return 0;
    
    /* 检查范围重叠 */
    if (new_lock->end < existing->start || new_lock->start > existing->end) {
        return 0;
    }
    
    /* 检查锁类型冲突: 共享锁之间不冲突，排他锁与任何锁冲突 */
    if (new_lock->flags == LOCK_EX || existing->flags == LOCK_EX) {
        return 1;
    }
    
    return 0;
}

/* 获取文件锁 (内部实现) */
int vfs_lock_file(uint32_t dev_id, uint64_t inode, uint32_t flags, 
                  uint64_t start, uint64_t end, int nonblock) {
    struct file_lock *lock = NULL;
    uint32_t current_pid = 0;
    struct process *proc = process_get_current();
    if (proc) current_pid = proc->pid;
    
    /* 查找空闲锁结构 */
    spin_lock(&file_lock_lock);
    
    for (int i = 0; i < MAX_FILE_LOCKS; i++) {
        if (file_locks[i].inode == 0) {
            lock = &file_locks[i];
            break;
        }
    }
    
    if (lock == NULL) {
        spin_unlock(&file_lock_lock);
        return -1;
    }
    
    /* 临时填充锁信息用于冲突检测 */
    lock->inode = inode;
    lock->dev_id = dev_id;
    lock->pid = current_pid;
    lock->type = FL_POSIX;
    lock->flags = flags;
    lock->start = start;
    lock->end = end;
    
    /* 检查冲突 */
    struct file_lock *existing = file_lock_list;
    while (existing != NULL) {
        if (lock_conflicts(lock, existing)) {
            if (nonblock) {
                lock->inode = 0;  /* 归还锁结构 */
                spin_unlock(&file_lock_lock);
                return -1;  /* EAGAIN */
            }
            /* TODO: 阻塞等待 - 当前简化实现直接返回冲突 */
            lock->inode = 0;
            spin_unlock(&file_lock_lock);
            return -1;
        }
        existing = existing->next;
    }
    
    /* 同一进程升级/降级锁: 先移除旧锁 */
    {
        struct file_lock **pp = &file_lock_list;
        while (*pp != NULL) {
            if ((*pp)->inode == inode && (*pp)->dev_id == dev_id && 
                (*pp)->pid == current_pid && (*pp)->type == FL_POSIX) {
                /* 找到同进程同文件的锁，检查范围是否重叠 */
                if ((*pp)->start == start && (*pp)->end == end) {
                    /* 同范围，升级/降级 */
                    if (flags == LOCK_UN) {
                        /* 解锁: 移除 */
                        struct file_lock *old = *pp;
                        *pp = old->next;
                        old->inode = 0;
                    } else {
                        /* 修改锁类型 */
                        (*pp)->flags = flags;
                    }
                    lock->inode = 0;  /* 不需要新锁结构 */
                    spin_unlock(&file_lock_lock);
                    return 0;
                }
            }
            pp = &(*pp)->next;
        }
    }
    
    /* 设置新锁 */
    lock->inode = inode;
    lock->dev_id = dev_id;
    lock->pid = current_pid;
    lock->type = FL_POSIX;
    lock->flags = flags;
    lock->start = start;
    lock->end = end;
    lock->next = file_lock_list;
    file_lock_list = lock;
    
    spin_unlock(&file_lock_lock);
    
    return 0;
}

/* 解锁文件 */
int vfs_unlock_file(uint32_t dev_id, uint64_t inode) {
    uint32_t current_pid = 0;
    struct process *proc = process_get_current();
    if (proc) current_pid = proc->pid;
    
    spin_lock(&file_lock_lock);
    
    struct file_lock **pp = &file_lock_list;
    while (*pp != NULL) {
        if ((*pp)->inode == inode && (*pp)->dev_id == dev_id &&
            (*pp)->pid == current_pid) {
            struct file_lock *lock = *pp;
            *pp = lock->next;
            lock->inode = 0;
            /* 继续移除同进程同文件的所有锁 */
            continue;
        }
        pp = &(*pp)->next;
    }
    
    spin_unlock(&file_lock_lock);
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * flock 系统调用 - 整文件咨询锁
 * 支持共享锁、排他锁、解锁、非阻塞模式
 * ═══════════════════════════════════════════════════════════════════════════ */
int vfs_flock(struct vfs_file *file, uint32_t operation) {
    if (file == NULL || file->mount == NULL) {
        return VFS_ERR_INVAL;
    }
    
    /* 从文件路径获取 inode 信息 */
    struct vfs_stat st;
    if (vfs_fstat(file, &st) != 0) {
        return VFS_ERR_INVAL;
    }
    
    uint32_t dev_id = 0;  /* 简化: 使用挂载点指针作为设备ID */
    uint64_t inode_num = st.inode;
    uint32_t flags = operation & ~VFS_LOCK_NONBLOCK;
    int nonblock = (operation & VFS_LOCK_NONBLOCK) != 0;
    
    switch (flags) {
        case VFS_LOCK_SHARED:
            return vfs_lock_file(dev_id, inode_num, LOCK_SH, 0, 0xFFFFFFFFFFFFFFFFULL, nonblock);
        case VFS_LOCK_EXCLUSIVE:
            return vfs_lock_file(dev_id, inode_num, LOCK_EX, 0, 0xFFFFFFFFFFFFFFFFULL, nonblock);
        case VFS_LOCK_UNLOCK:
            return vfs_unlock_file(dev_id, inode_num);
        default:
            return VFS_ERR_INVAL;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * lockf 系统调用 - 范围咨询锁
 * 支持阻塞加锁、非阻塞加锁、解锁、测试锁
 * ═══════════════════════════════════════════════════════════════════════════ */
int vfs_lockf(struct vfs_file *file, int cmd, int64_t len) {
    if (file == NULL) {
        return VFS_ERR_INVAL;
    }
    
    struct vfs_stat st;
    if (vfs_fstat(file, &st) != 0) {
        return VFS_ERR_INVAL;
    }
    
    uint32_t dev_id = 0;
    uint64_t inode_num = st.inode;
    uint64_t start = file->offset;
    uint64_t end;
    
    /* 计算锁范围 */
    if (len > 0) {
        end = start + (uint64_t)len - 1;
    } else if (len < 0) {
        if ((uint64_t)(-len) > start) {
            start = 0;
        } else {
            start = start - (uint64_t)(-len);
        }
        end = file->offset;
    } else {
        /* len == 0: 锁从当前位置到文件末尾 */
        end = 0xFFFFFFFFFFFFFFFFULL;
    }
    
    switch (cmd) {
        case VFS_F_LOCK:
            /* 阻塞加锁 */
            return vfs_lock_file(dev_id, inode_num, LOCK_EX, start, end, 0);
        case VFS_F_TLOCK:
            /* 非阻塞加锁 */
            return vfs_lock_file(dev_id, inode_num, LOCK_EX, start, end, 1);
        case VFS_F_ULOCK:
            /* 解锁 */
            return vfs_unlock_file(dev_id, inode_num);
        case VFS_F_TEST: {
            /* 测试锁: 如果有冲突锁返回 -1，否则返回 0 */
            spin_lock(&file_lock_lock);
            struct file_lock *existing = file_lock_list;
            uint32_t current_pid = 0;
            struct process *proc = process_get_current();
            if (proc) current_pid = proc->pid;
            
            while (existing != NULL) {
                if (existing->inode == inode_num && existing->dev_id == dev_id &&
                    existing->pid != current_pid &&
                    existing->flags == LOCK_EX &&
                    !(end < existing->start || start > existing->end)) {
                    spin_unlock(&file_lock_lock);
                    return -1;  /* 有冲突锁 */
                }
                existing = existing->next;
            }
            spin_unlock(&file_lock_lock);
            return 0;  /* 无冲突 */
        }
        default:
            return VFS_ERR_INVAL;
    }
}

/* === 文件系统事件通知 (inotify 风格) === */

#define IN_ACCESS       0x00000001
#define IN_MODIFY       0x00000002
#define IN_ATTRIB       0x00000004
#define IN_CLOSE_WRITE  0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN         0x00000020
#define IN_MOVED_FROM   0x00000040
#define IN_MOVED_TO     0x00000080
#define IN_CREATE       0x00000100
#define IN_DELETE       0x00000200

struct fs_notify_watch {
    uint64_t inode;
    uint32_t dev_id;
    uint32_t mask;          /* 监控的事件 */
    uint32_t wd;            /* watch descriptor */
    void *callback;
    struct fs_notify_watch *next;
};

#define MAX_FS_WATCHES    64
static struct fs_notify_watch fs_watches[MAX_FS_WATCHES];
static struct fs_notify_watch *watch_list;
static uint32_t next_wd;
static spinlock_t fs_notify_lock = SPINLOCK_INIT;

/* 初始化文件系统通知 */
void fs_notify_init(void) {
    watch_list = NULL;
    next_wd = 1;
    
    for (int i = 0; i < MAX_FS_WATCHES; i++) {
        fs_watches[i].inode = 0;
        fs_watches[i].next = NULL;
    }
}

/* 添加监控 */
int fs_notify_add_watch(uint32_t dev_id, uint64_t inode, uint32_t mask) {
    spin_lock(&fs_notify_lock);
    
    struct fs_notify_watch *watch = NULL;
    for (int i = 0; i < MAX_FS_WATCHES; i++) {
        if (fs_watches[i].inode == 0) {
            watch = &fs_watches[i];
            break;
        }
    }
    
    if (watch == NULL) {
        spin_unlock(&fs_notify_lock);
        return -1;
    }
    
    watch->inode = inode;
    watch->dev_id = dev_id;
    watch->mask = mask;
    watch->wd = next_wd++;
    watch->next = watch_list;
    watch_list = watch;
    
    spin_unlock(&fs_notify_lock);
    
    return watch->wd;
}

/* 触发事件 */
void fs_notify_event(uint32_t dev_id, uint64_t inode, uint32_t event) {
    spin_lock(&fs_notify_lock);
    
    struct fs_notify_watch *watch = watch_list;
    while (watch != NULL) {
        if (watch->inode == inode && watch->dev_id == dev_id) {
            if (watch->mask & event) {
                /* 触发回调 */
                if (watch->callback != NULL) {
                    /* TODO: 调用回调 */
                }
            }
        }
        watch = watch->next;
    }
    
    spin_unlock(&fs_notify_lock);
}

/* === 写时复制 (COW) 支持 === */

struct cow_mapping {
    uint64_t src_inode;
    uint64_t dst_inode;
    uint32_t dev_id;
    uint32_t ref_count;
    uint64_t *copied_pages;     /* 位图：已复制的页面 */
    uint32_t total_pages;
    int active;
};

#define MAX_COW_MAPPINGS    16
static struct cow_mapping cow_mappings[MAX_COW_MAPPINGS];

/* 初始化 COW */
void vfs_cow_init(void) {
    for (int i = 0; i < MAX_COW_MAPPINGS; i++) {
        cow_mappings[i].active = 0;
        cow_mappings[i].copied_pages = NULL;
    }
}

/* 创建 COW 映射 */
int cow_create(uint32_t dev_id, uint64_t src_inode, uint64_t dst_inode, uint32_t total_pages) {
    for (int i = 0; i < MAX_COW_MAPPINGS; i++) {
        if (!cow_mappings[i].active) {
            cow_mappings[i].src_inode = src_inode;
            cow_mappings[i].dst_inode = dst_inode;
            cow_mappings[i].dev_id = dev_id;
            cow_mappings[i].ref_count = 1;
            cow_mappings[i].total_pages = total_pages;
            
            /* 分配位图 */
            uint32_t bitmap_size = (total_pages + 63) / 64 * sizeof(uint64_t);
            cow_mappings[i].copied_pages = kmalloc(bitmap_size);
            if (cow_mappings[i].copied_pages == NULL) {
                return -1;
            }
            
            /* 清零位图 */
            for (uint32_t j = 0; j < (total_pages + 63) / 64; j++) {
                cow_mappings[i].copied_pages[j] = 0;
            }
            
            cow_mappings[i].active = 1;
            return i;
        }
    }
    
    return -1;
}

/* 检查页面是否需要 COW */
int cow_check_page(int mapping_id, uint32_t page_idx) {
    if (mapping_id < 0 || mapping_id >= MAX_COW_MAPPINGS) return 0;
    if (!cow_mappings[mapping_id].active) return 0;
    
    struct cow_mapping *cow = &cow_mappings[mapping_id];
    
    if (page_idx >= cow->total_pages) return 0;
    
    uint32_t word = page_idx / 64;
    uint32_t bit = page_idx % 64;
    
    return (cow->copied_pages[word] & (1ULL << bit)) != 0;
}

/* 标记页面已复制 */
void cow_mark_copied(int mapping_id, uint32_t page_idx) {
    if (mapping_id < 0 || mapping_id >= MAX_COW_MAPPINGS) return;
    if (!cow_mappings[mapping_id].active) return;
    
    struct cow_mapping *cow = &cow_mappings[mapping_id];
    
    if (page_idx < cow->total_pages) {
        uint32_t word = page_idx / 64;
        uint32_t bit = page_idx % 64;
        cow->copied_pages[word] |= (1ULL << bit);
    }
}

/* === 文件系统快照 === */

struct fs_snapshot {
    uint64_t snap_id;
    uint64_t inode;
    uint32_t dev_id;
    uint64_t create_time;
    char description[64];
    uint64_t *block_bitmap;
    uint32_t block_count;
    int active;
};

#define MAX_SNAPSHOTS    8
static struct fs_snapshot snapshots[MAX_SNAPSHOTS];
static uint64_t next_snap_id;

/* 创建快照 */
int fs_create_snapshot(uint32_t dev_id, uint64_t inode, const char *desc) {
    for (int i = 0; i < MAX_SNAPSHOTS; i++) {
        if (!snapshots[i].active) {
            snapshots[i].snap_id = next_snap_id++;
            snapshots[i].inode = inode;
            snapshots[i].dev_id = dev_id;
            snapshots[i].create_time = get_tick_count();
            
            if (desc != NULL) {
                int j = 0;
                while (desc[j] && j < 63) {
                    snapshots[i].description[j] = desc[j];
                    j++;
                }
                snapshots[i].description[j] = '\0';
            } else {
                snapshots[i].description[0] = '\0';
            }
            
            snapshots[i].active = 1;
            
            return i;
        }
    }
    
    return -1;
}

/* 删除快照 */
int fs_delete_snapshot(uint64_t snap_id) {
    for (int i = 0; i < MAX_SNAPSHOTS; i++) {
        if (snapshots[i].active && snapshots[i].snap_id == snap_id) {
            if (snapshots[i].block_bitmap != NULL) {
                kfree(snapshots[i].block_bitmap);
            }
            snapshots[i].active = 0;
            return 0;
        }
    }
    
    return -1;
}

/* 列出快照 */
int fs_list_snapshots(struct fs_snapshot **list, int *count) {
    if (list != NULL) *list = snapshots;
    if (count != NULL) {
        *count = 0;
        for (int i = 0; i < MAX_SNAPSHOTS; i++) {
            if (snapshots[i].active) (*count)++;
        }
    }
    return 0;
}

/* === VFS 统计增强 === */

struct vfs_stats_enhanced {
    uint64_t dcache_hits;
    uint64_t dcache_misses;
    uint64_t icache_hits;
    uint64_t icache_misses;
    uint64_t lock_acquisitions;
    uint64_t lock_conflicts;
    uint64_t notify_events;
    uint64_t cow_copies;
    uint64_t snapshots_created;
};

static struct vfs_stats_enhanced vfs_stats_ext;

/* 获取增强统计 */
void vfs_get_enhanced_stats(struct vfs_stats_enhanced *stats) {
    if (stats != NULL) {
        *stats = vfs_stats_ext;
    }
}

/* 打印 VFS 状态 */
void vfs_print_status_enhanced(void) {
    // println("=== VFS Enhanced Status ===");
    
    // print("Dentry cache: ");
    // print_dec(dcache_count);
    // print(" / ");
    // print_dec(DCACHE_MAX_ENTRIES);
    // println(" entries");
    
    // print("Inode cache: ");
    // print_dec(icache_count);
    // print(" / ");
    // print_dec(ICACHE_MAX_ENTRIES);
    // println(" entries");
    
    /* 统计活跃文件锁 */
    int lock_count = 0;
    for (int i = 0; i < MAX_FILE_LOCKS; i++) {
        if (file_locks[i].inode != 0) lock_count++;
    }
    // print("Active file locks: ");
    // print_dec(lock_count);
    // println("");
    
    /* 统计监控 */
    int watch_count = 0;
    for (int i = 0; i < MAX_FS_WATCHES; i++) {
        if (fs_watches[i].inode != 0) watch_count++;
    }
    // print("FS watches: ");
    // print_dec(watch_count);
    // println("");
    
    /* 统计快照 */
    int snap_count = 0;
    for (int i = 0; i < MAX_SNAPSHOTS; i++) {
        if (snapshots[i].active) snap_count++;
    }
    // print("Snapshots: ");
    // print_dec(snap_count);
    // println("");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：VFS 高级增强功能
 * 
 * 特性：
 * - 文件系统配额管理
 * - 访问控制列表 (ACL)
 * - 文件系统日志
 * - 数据完整性校验
 * - 性能监控与诊断
 * ═══════════════════════════════════════════════════════════════════════════ */

/* === 文件系统配额管理 === */

/* 配额类型 */
#define QUOTA_USER     0
#define QUOTA_GROUP    1

/* 配额限制 */
struct quota_limit {
    uint64_t blocks_hard;       /* 块数硬限制 */
    uint64_t blocks_soft;       /* 块数软限制 */
    uint64_t inodes_hard;       /* inode 硬限制 */
    uint64_t inodes_soft;       /* inode 软限制 */
    uint64_t blocks_used;       /* 已用块数 */
    uint64_t inodes_used;       /* 已用 inode 数 */
    uint64_t grace_time;        /* 宽限期 */
};

#define MAX_QUOTA_ENTRIES    64

struct quota_entry {
    uint32_t id;                /* 用户/组 ID */
    uint32_t type;              /* QUOTA_USER / QUOTA_GROUP */
    uint32_t dev_id;
    struct quota_limit limit;
    int active;
};

static struct quota_entry quota_table[MAX_QUOTA_ENTRIES];
static spinlock_t quota_lock = SPINLOCK_INIT;

/* 初始化配额系统 */
void quota_init(void) {
    for (int i = 0; i < MAX_QUOTA_ENTRIES; i++) {
        quota_table[i].active = 0;
    }
}

/* 设置配额 */
int quota_set(uint32_t dev_id, uint32_t id, uint32_t type,
              uint64_t blocks_hard, uint64_t blocks_soft,
              uint64_t inodes_hard, uint64_t inodes_soft) {
    spin_lock(&quota_lock);
    
    /* 查找或创建配额条目 */
    struct quota_entry *entry = NULL;
    for (int i = 0; i < MAX_QUOTA_ENTRIES; i++) {
        if (!quota_table[i].active) {
            entry = &quota_table[i];
            break;
        }
    }
    
    if (entry == NULL) {
        spin_unlock(&quota_lock);
        return -1;
    }
    
    entry->dev_id = dev_id;
    entry->id = id;
    entry->type = type;
    entry->limit.blocks_hard = blocks_hard;
    entry->limit.blocks_soft = blocks_soft;
    entry->limit.inodes_hard = inodes_hard;
    entry->limit.inodes_soft = inodes_soft;
    entry->limit.blocks_used = 0;
    entry->limit.inodes_used = 0;
    entry->active = 1;
    
    spin_unlock(&quota_lock);
    return 0;
}

/* 检查配额 */
int quota_check(uint32_t dev_id, uint32_t id, uint32_t type,
                uint64_t blocks, uint64_t inodes) {
    spin_lock(&quota_lock);
    
    for (int i = 0; i < MAX_QUOTA_ENTRIES; i++) {
        if (quota_table[i].active && 
            quota_table[i].dev_id == dev_id &&
            quota_table[i].id == id && 
            quota_table[i].type == type) {
            
            struct quota_limit *limit = &quota_table[i].limit;
            
            /* 检查块限制 */
            if (limit->blocks_used + blocks > limit->blocks_hard) {
                spin_unlock(&quota_lock);
                return -1;  /* 超过硬限制 */
            }
            
            /* 检查 inode 限制 */
            if (limit->inodes_used + inodes > limit->inodes_hard) {
                spin_unlock(&quota_lock);
                return -1;
            }
            
            spin_unlock(&quota_lock);
            return 0;
        }
    }
    
    spin_unlock(&quota_lock);
    return 0;  /* 无配额限制 */
}

/* 更新配额使用 */
void quota_update(uint32_t dev_id, uint32_t id, uint32_t type,
                  int64_t blocks, int64_t inodes) {
    spin_lock(&quota_lock);
    
    for (int i = 0; i < MAX_QUOTA_ENTRIES; i++) {
        if (quota_table[i].active && 
            quota_table[i].dev_id == dev_id &&
            quota_table[i].id == id && 
            quota_table[i].type == type) {
            
            if (blocks > 0) {
                quota_table[i].limit.blocks_used += blocks;
            } else {
                if (quota_table[i].limit.blocks_used >= (uint64_t)(-blocks)) {
                    quota_table[i].limit.blocks_used += blocks;
                }
            }
            
            if (inodes > 0) {
                quota_table[i].limit.inodes_used += inodes;
            } else {
                if (quota_table[i].limit.inodes_used >= (uint64_t)(-inodes)) {
                    quota_table[i].limit.inodes_used += inodes;
                }
            }
            
            break;
        }
    }
    
    spin_unlock(&quota_lock);
}

/* === 访问控制列表 (ACL) === */

/* ACL 权限位 */
#define ACL_READ    (1 << 0)
#define ACL_WRITE   (1 << 1)
#define ACL_EXEC    (1 << 2)

/* ACL 条目 */
struct acl_entry_vfs {
    uint32_t tag;               /* USER, GROUP, OTHER */
    uint32_t id;                /* 用户/组 ID */
    uint32_t perm;              /* 权限位 */
};

#define ACL_TAG_USER_OBJ    1
#define ACL_TAG_USER        2
#define ACL_TAG_GROUP_OBJ   3
#define ACL_TAG_GROUP       4
#define ACL_TAG_MASK        5
#define ACL_TAG_OTHER       6

#define MAX_ACL_ENTRIES_VFS    32

struct acl_vfs {
    struct acl_entry_vfs entries[MAX_ACL_ENTRIES_VFS];
    int count;
};

/* 文件 ACL 存储 */
struct file_acl {
    uint64_t inode;
    uint32_t dev_id;
    struct acl_vfs acl;
    struct file_acl *next;
};

#define MAX_FILE_ACLS    256
static struct file_acl file_acls[MAX_FILE_ACLS];
static struct file_acl *file_acl_list;
static spinlock_t acl_lock = SPINLOCK_INIT;

/* 初始化 ACL 系统 */
void vfs_acl_init(void) {
    file_acl_list = NULL;
    for (int i = 0; i < MAX_FILE_ACLS; i++) {
        file_acls[i].inode = 0;
        file_acls[i].next = NULL;
    }
}

/* 设置文件 ACL */
int vfs_set_acl(uint32_t dev_id, uint64_t inode, struct acl_vfs *acl) {
    if (acl == NULL) return -1;
    
    spin_lock(&acl_lock);
    
    struct file_acl *fa = NULL;
    for (int i = 0; i < MAX_FILE_ACLS; i++) {
        if (file_acls[i].inode == 0) {
            fa = &file_acls[i];
            break;
        }
    }
    
    if (fa == NULL) {
        spin_unlock(&acl_lock);
        return -1;
    }
    
    fa->inode = inode;
    fa->dev_id = dev_id;
    fa->acl = *acl;
    fa->next = file_acl_list;
    file_acl_list = fa;
    
    spin_unlock(&acl_lock);
    return 0;
}

/* 获取文件 ACL */
int vfs_get_acl(uint32_t dev_id, uint64_t inode, struct acl_vfs *acl) {
    if (acl == NULL) return -1;
    
    spin_lock(&acl_lock);
    
    struct file_acl *fa = file_acl_list;
    while (fa != NULL) {
        if (fa->inode == inode && fa->dev_id == dev_id) {
            *acl = fa->acl;
            spin_unlock(&acl_lock);
            return 0;
        }
        fa = fa->next;
    }
    
    spin_unlock(&acl_lock);
    return -1;
}

/* 检查 ACL 权限 */
int vfs_check_acl(uint32_t dev_id, uint64_t inode, uint32_t uid, uint32_t gid, uint32_t perm) {
    struct acl_vfs acl;
    
    if (vfs_get_acl(dev_id, inode, &acl) != 0) {
        /* 无 ACL，使用传统权限 */
        return 0;
    }
    
    /* 遍历 ACL 条目 */
    for (int i = 0; i < acl.count; i++) {
        struct acl_entry_vfs *e = &acl.entries[i];
        
        if (e->tag == ACL_TAG_USER && e->id == uid) {
            return (e->perm & perm) == perm ? 0 : -1;
        }
        
        if (e->tag == ACL_TAG_GROUP && e->id == gid) {
            return (e->perm & perm) == perm ? 0 : -1;
        }
    }
    
    /* 检查 OTHER */
    for (int i = 0; i < acl.count; i++) {
        if (acl.entries[i].tag == ACL_TAG_OTHER) {
            return (acl.entries[i].perm & perm) == perm ? 0 : -1;
        }
    }
    
    return -1;  /* 默认拒绝 */
}

/* === 数据完整性校验 === */

/* 校验和类型 */
#define CHECKSUM_NONE       0
#define CHECKSUM_CRC32      1
#define CHECKSUM_CRC64      2
#define CHECKSUM_SHA256     3

/* 文件校验和 */
struct file_checksum {
    uint64_t inode;
    uint32_t dev_id;
    uint32_t type;
    union {
        uint32_t crc32;
        uint64_t crc64;
        uint8_t sha256[32];
    } checksum;
    uint64_t size;
    uint64_t mtime;
    int valid;
};

#define MAX_FILE_CHECKSUMS    128
static struct file_checksum file_checksums[MAX_FILE_CHECKSUMS];
static spinlock_t checksum_lock = SPINLOCK_INIT;

/* 计算 CRC32 */
static uint32_t crc32_table[256];
static int crc32_table_init = 0;

static void crc32_init_table(void) {
    if (crc32_table_init) return;
    
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_table_init = 1;
}

static uint32_t calc_crc32(const void *data, size_t len) {
    crc32_init_table();
    
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    
    return crc ^ 0xFFFFFFFF;
}

/* 初始化校验和系统 */
void checksum_init(void) {
    for (int i = 0; i < MAX_FILE_CHECKSUMS; i++) {
        file_checksums[i].valid = 0;
    }
    crc32_init_table();
}

/* 计算并存储文件校验和 */
int vfs_compute_checksum(uint32_t dev_id, uint64_t inode, 
                          const void *data, size_t len, uint32_t type) {
    if (data == NULL || len == 0) return -1;
    
    spin_lock(&checksum_lock);
    
    struct file_checksum *cs = NULL;
    for (int i = 0; i < MAX_FILE_CHECKSUMS; i++) {
        if (!file_checksums[i].valid) {
            cs = &file_checksums[i];
            break;
        }
    }
    
    if (cs == NULL) {
        spin_unlock(&checksum_lock);
        return -1;
    }
    
    cs->inode = inode;
    cs->dev_id = dev_id;
    cs->type = type;
    cs->size = len;
    cs->mtime = get_tick_count();
    
    switch (type) {
        case CHECKSUM_CRC32:
            cs->checksum.crc32 = calc_crc32(data, len);
            break;
        case CHECKSUM_CRC64:
            /* 简化：使用 CRC32 */
            cs->checksum.crc64 = calc_crc32(data, len);
            break;
        default:
            cs->checksum.crc32 = calc_crc32(data, len);
            break;
    }
    
    cs->valid = 1;
    
    spin_unlock(&checksum_lock);
    return 0;
}

/* 验证文件校验和 */
int vfs_verify_checksum(uint32_t dev_id, uint64_t inode,
                         const void *data, size_t len) {
    if (data == NULL) return -1;
    
    spin_lock(&checksum_lock);
    
    struct file_checksum *cs = NULL;
    for (int i = 0; i < MAX_FILE_CHECKSUMS; i++) {
        if (file_checksums[i].valid && 
            file_checksums[i].inode == inode &&
            file_checksums[i].dev_id == dev_id) {
            cs = &file_checksums[i];
            break;
        }
    }
    
    if (cs == NULL) {
        spin_unlock(&checksum_lock);
        return 0;  /* 无校验和记录 */
    }
    
    uint32_t calc;
    switch (cs->type) {
        case CHECKSUM_CRC32:
            calc = calc_crc32(data, len);
            spin_unlock(&checksum_lock);
            return calc == cs->checksum.crc32 ? 0 : -1;
        default:
            calc = calc_crc32(data, len);
            spin_unlock(&checksum_lock);
            return calc == cs->checksum.crc32 ? 0 : -1;
    }
}

/* === 文件系统性能监控 === */

struct vfs_perf_stats {
    uint64_t open_count;
    uint64_t close_count;
    uint64_t read_count;
    uint64_t write_count;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t avg_read_latency;
    uint64_t avg_write_latency;
    uint64_t max_read_latency;
    uint64_t max_write_latency;
};

static struct vfs_perf_stats vfs_perf;

/* 记录读操作 */
void vfs_record_read(uint64_t bytes, uint64_t latency_ns) {
    vfs_perf.read_count++;
    vfs_perf.read_bytes += bytes;
    vfs_perf.avg_read_latency = 
        (vfs_perf.avg_read_latency * (vfs_perf.read_count - 1) + latency_ns) / 
        vfs_perf.read_count;
    if (latency_ns > vfs_perf.max_read_latency) {
        vfs_perf.max_read_latency = latency_ns;
    }
}

/* 记录写操作 */
void vfs_record_write(uint64_t bytes, uint64_t latency_ns) {
    vfs_perf.write_count++;
    vfs_perf.write_bytes += bytes;
    vfs_perf.avg_write_latency = 
        (vfs_perf.avg_write_latency * (vfs_perf.write_count - 1) + latency_ns) / 
        vfs_perf.write_count;
    if (latency_ns > vfs_perf.max_write_latency) {
        vfs_perf.max_write_latency = latency_ns;
    }
}

/* 获取性能统计 */
void vfs_get_perf_stats(struct vfs_perf_stats *stats) {
    if (stats != NULL) {
        *stats = vfs_perf;
    }
}

/* === VFS 综合诊断报告 === */

void vfs_print_diagnostic_report(void) {
    // println("");
    // println("╔════════════════════════════════════════════════════════════╗");
    // println("║           Zratro OS VFS Diagnostic Report                  ║");
    // println("╚════════════════════════════════════════════════════════════╝");
    
    /* 挂载点信息 */
    // println("");
    // println("=== 挂载点信息 ===");
    
    int mount_count = 0;
    struct vfs_mount *m = mount_list;
    while (m != NULL) {
        mount_count++;
        m = m->next;
    }
    
    // print("挂载点数量: ");
    // print_dec(mount_count);
    // println("");
    
    m = mount_list;
    while (m != NULL) {
        // print("  ");
        // print(m->device);
        // print(" on ");
        // println(m->path);
        m = m->next;
    }
    
    /* 缓存统计 */
    // println("");
    // println("=== 缓存统计 ===");
    
    /* Buffer Cache */
    // print("Buffer Cache: ");
    // print_dec(bcache_stats.used_blocks);
    // print(" / ");
    // print_dec(BCACHE_NUM_BLOCKS);
    // print(" 块, ");
    // print_dec(bcache_stats.dirty_blocks);
    // println(" 脏块");
    
    // print("  命中: ");
    // print_dec(bcache_stats.hits);
    // print("  未命中: ");
    // print_dec(bcache_stats.misses);
    // println("");
    
    if (bcache_stats.hits + bcache_stats.misses > 0) {
        // print("  命中率: ");
        // print_dec((bcache_stats.hits * 100) / (bcache_stats.hits + bcache_stats.misses));
        // println("%");
    }
    
    /* Page Cache */
    // print("Page Cache: ");
    // print_dec(page_cache_stats.used_pages);
    // print(" / ");
    // print_dec(PAGE_CACHE_NUM_PAGES);
    // print(" 页, ");
    // print_dec(page_cache_stats.dirty_pages);
    // println(" 脏页");
    
    /* Dentry Cache */
    // print("Dentry Cache: ");
    // print_dec(dcache_count);
    // print(" / ");
    // print_dec(DCACHE_MAX_ENTRIES);
    // println(" 条目");
    
    /* Inode Cache */
    // print("Inode Cache: ");
    // print_dec(icache_count);
    // print(" / ");
    // print_dec(ICACHE_MAX_ENTRIES);
    // println(" 条目");
    
    /* 性能统计 */
    // println("");
    // println("=== 性能统计 ===");
    
    // print("读操作: ");
    // print_dec(vfs_perf.read_count);
    // print(" 次 (");
    // print_dec(vfs_perf.read_bytes / 1024);
    // println(" KB)");
    
    // print("写操作: ");
    // print_dec(vfs_perf.write_count);
    // print(" 次 (");
    // print_dec(vfs_perf.write_bytes / 1024);
    // println(" KB)");
    
    // print("平均读延迟: ");
    // print_dec(vfs_perf.avg_read_latency / 1000);
    // println(" us");
    
    // print("平均写延迟: ");
    // print_dec(vfs_perf.avg_write_latency / 1000);
    // println(" us");
    
    /* 安全与配额 */
    // println("");
    // println("=== 安全与配额 ===");
    
    /* 文件锁 */
    int lock_count = 0;
    for (int i = 0; i < MAX_FILE_LOCKS; i++) {
        if (file_locks[i].inode != 0) lock_count++;
    }
    // print("活跃文件锁: ");
    // print_dec(lock_count);
    // println("");
    
    /* ACL */
    int acl_count = 0;
    for (int i = 0; i < MAX_FILE_ACLS; i++) {
        if (file_acls[i].inode != 0) acl_count++;
    }
    // print("ACL 条目: ");
    // print_dec(acl_count);
    // println("");
    
    /* 配额 */
    int quota_count = 0;
    for (int i = 0; i < MAX_QUOTA_ENTRIES; i++) {
        if (quota_table[i].active) quota_count++;
    }
    // print("配额条目: ");
    // print_dec(quota_count);
    // println("");
    
    /* 校验和 */
    int cs_count = 0;
    for (int i = 0; i < MAX_FILE_CHECKSUMS; i++) {
        if (file_checksums[i].valid) cs_count++;
    }
    // print("校验和记录: ");
    // print_dec(cs_count);
    // println("");
    
    /* 快照 */
    int snap_count = 0;
    for (int i = 0; i < MAX_SNAPSHOTS; i++) {
        if (snapshots[i].active) snap_count++;
    }
    // print("文件系统快照: ");
    // print_dec(snap_count);
    // println("");
    
    // println("");
    // println("════════════════════════════════════════════════════════════");
}

/* Get mount list for debugging */
struct vfs_mount *vfs_get_mount_list(void) {
    return mount_list;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Dentry Cache 集成 - 路径解析加速
 * 将 dcache 与 VFS 操作集成，加速路径查找
 * ═══════════════════════════════════════════════════════════════════════════ */

/* dentry 缓存查找 - 公共接口 */
struct dentry *vfs_dcache_lookup(const char *name, uint32_t dev_id) {
    return dcache_lookup(name, dev_id);
}

/* dentry 缓存添加 - 公共接口 */
struct dentry *vfs_dcache_add(const char *name, uint32_t dev_id, uint64_t inode) {
    return dcache_add(name, dev_id, inode);
}

/* dentry 缓存释放 - 公共接口 */
void vfs_dcache_put(struct dentry *d) {
    dcache_put(d);
}

/* dentry 缓存使失效 - 当文件被删除/重命名时调用 */
void vfs_dcache_invalidate(const char *name, uint32_t dev_id) {
    if (name == NULL) return;
    
    spin_lock(&dcache_lock);
    
    uint32_t hash = dcache_hash_func(name, dev_id);
    struct dentry **pp = &dcache_hash[hash];
    
    while (*pp != NULL) {
        if ((*pp)->dev_id == dev_id && strcmp((*pp)->name, name) == 0) {
            struct dentry *victim = *pp;
            *pp = victim->hash_next;
            
            /* 从 LRU 链表移除 */
            if (victim->lru_prev) victim->lru_prev->lru_next = victim->lru_next;
            if (victim->lru_next) victim->lru_next->lru_prev = victim->lru_prev;
            if (victim == dentry_lru_head) dentry_lru_head = victim->lru_next;
            if (victim == dentry_lru_tail) dentry_lru_tail = victim->lru_prev;
            
            /* 归还到空闲链表 */
            memset(victim, 0, sizeof(*victim));
            victim->hash_next = dentry_free_list;
            dentry_free_list = victim;
            dcache_count--;
            break;
        }
        pp = &(*pp)->hash_next;
    }
    
    spin_unlock(&dcache_lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Inode Cache 回收 - 内存压力下回收未使用的 inode 缓存
 * ═══════════════════════════════════════════════════════════════════════════ */

/* inode 缓存添加 - 公共接口 */
void vfs_icache_add(uint32_t dev_id, uint64_t inode, uint64_t size, uint64_t mode) {
    spin_lock(&icache_lock);
    
    /* 检查是否已存在 */
    uint32_t hash = (inode ^ dev_id) % ICACHE_HASH_SIZE;
    struct inode_cache_entry *e = icache_hash[hash];
    while (e != NULL) {
        if (e->dev_id == dev_id && e->inode == inode) {
            /* 更新现有条目 */
            e->size = size;
            e->mode = mode;
            e->atime = get_tick_count();
            spin_unlock(&icache_lock);
            return;
        }
        e = e->hash_next;
    }
    
    /* 分配新条目 */
    if (inode_free_list == NULL) {
        /* 空闲链表为空，尝试从 LRU 尾部回收 */
        if (inode_lru_tail != NULL && inode_lru_tail->ref_count == 0) {
            struct inode_cache_entry *victim = inode_lru_tail;
            
            /* 从哈希表移除 */
            uint32_t vhash = (victim->inode ^ victim->dev_id) % ICACHE_HASH_SIZE;
            struct inode_cache_entry **vpp = &icache_hash[vhash];
            while (*vpp != NULL) {
                if (*vpp == victim) {
                    *vpp = victim->hash_next;
                    break;
                }
                vpp = &(*vpp)->hash_next;
            }
            
            /* 从 LRU 移除 */
            if (victim->lru_prev) victim->lru_prev->lru_next = victim->lru_next;
            if (victim->lru_next) victim->lru_next->lru_prev = victim->lru_prev;
            if (victim == inode_lru_head) inode_lru_head = victim->lru_next;
            if (victim == inode_lru_tail) inode_lru_tail = victim->lru_prev;
            
            inode_free_list = victim;
            victim->hash_next = NULL;
            victim->lru_prev = NULL;
            victim->lru_next = NULL;
            icache_count--;
        }
    }
    
    if (inode_free_list == NULL) {
        spin_unlock(&icache_lock);
        return;
    }
    
    e = inode_free_list;
    inode_free_list = e->hash_next;
    
    /* 初始化 */
    e->inode = inode;
    e->dev_id = dev_id;
    e->ref_count = 1;
    e->size = size;
    e->mode = mode;
    e->atime = get_tick_count();
    e->mtime = get_tick_count();
    e->ctime = get_tick_count();
    e->flags = 0;
    
    /* 添加到哈希表 */
    e->hash_next = icache_hash[hash];
    icache_hash[hash] = e;
    
    /* 添加到 LRU 头部 */
    e->lru_prev = NULL;
    e->lru_next = inode_lru_head;
    if (inode_lru_head) inode_lru_head->lru_prev = e;
    inode_lru_head = e;
    if (inode_lru_tail == NULL) inode_lru_tail = e;
    
    icache_count++;
    
    spin_unlock(&icache_lock);
}

/* inode 缓存回收 - 在内存压力下释放指定数量的未使用 inode 缓存 */
uint32_t vfs_icache_reclaim(uint32_t target_count) {
    uint32_t reclaimed = 0;
    
    spin_lock(&icache_lock);
    
    /* 从 LRU 尾部开始回收 */
    while (reclaimed < target_count && inode_lru_tail != NULL) {
        struct inode_cache_entry *victim = inode_lru_tail;
        
        /* 只回收引用计数为 0 的条目 */
        if (victim->ref_count > 0) {
            break;
        }
        
        /* 从哈希表移除 */
        uint32_t hash = (victim->inode ^ victim->dev_id) % ICACHE_HASH_SIZE;
        struct inode_cache_entry **pp = &icache_hash[hash];
        while (*pp != NULL) {
            if (*pp == victim) {
                *pp = victim->hash_next;
                break;
            }
            pp = &(*pp)->hash_next;
        }
        
        /* 从 LRU 移除 */
        inode_lru_tail = victim->lru_prev;
        if (inode_lru_tail) inode_lru_tail->lru_next = NULL;
        else inode_lru_head = NULL;
        
        /* 归还到空闲链表 */
        memset(victim, 0, sizeof(*victim));
        victim->hash_next = inode_free_list;
        inode_free_list = victim;
        
        icache_count--;
        reclaimed++;
    }
    
    spin_unlock(&icache_lock);
    
    return reclaimed;
}

/* inode 缓存使失效 */
void vfs_icache_invalidate(uint32_t dev_id, uint64_t inode) {
    spin_lock(&icache_lock);
    
    uint32_t hash = (inode ^ dev_id) % ICACHE_HASH_SIZE;
    struct inode_cache_entry **pp = &icache_hash[hash];
    
    while (*pp != NULL) {
        if ((*pp)->dev_id == dev_id && (*pp)->inode == inode) {
            struct inode_cache_entry *victim = *pp;
            *pp = victim->hash_next;
            
            /* 从 LRU 移除 */
            if (victim->lru_prev) victim->lru_prev->lru_next = victim->lru_next;
            if (victim->lru_next) victim->lru_next->lru_prev = victim->lru_prev;
            if (victim == inode_lru_head) inode_lru_head = victim->lru_next;
            if (victim == inode_lru_tail) inode_lru_tail = victim->lru_prev;
            
            /* 归还到空闲链表 */
            memset(victim, 0, sizeof(*victim));
            victim->hash_next = inode_free_list;
            inode_free_list = victim;
            icache_count--;
            break;
        }
        pp = &(*pp)->hash_next;
    }
    
    spin_unlock(&icache_lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 内存回收 Shrinker - 在内存压力下由 PMM 调用
 * 回收 dentry 缓存和 inode 缓存中未使用的条目
 * ═══════════════════════════════════════════════════════════════════════════ */

/* VFS 缓存 shrinker 回调 - 由内存管理器在内存压力下调用
 * target: 需要回收的目标页数 (近似)
 * 返回: 实际回收的条目数 */
uint32_t vfs_shrinker(uint32_t target) {
    uint32_t reclaimed = 0;

    /* 先回收 dentry 缓存 (dentry 较小，优先回收) */
    {
        spin_lock(&dcache_lock);
        struct dentry *victim = dentry_lru_tail;
        while (victim != NULL && reclaimed < target * 2) {
            struct dentry *prev = victim->lru_prev;
            if (victim->ref_count == 0) {
                /* 从哈希表移除 */
                uint32_t hash = dcache_hash_func(victim->name, victim->dev_id);
                struct dentry **pp = &dcache_hash[hash];
                while (*pp != NULL) {
                    if (*pp == victim) {
                        *pp = victim->hash_next;
                        break;
                    }
                    pp = &(*pp)->hash_next;
                }

                /* 从 LRU 链表移除 */
                if (victim->lru_prev) victim->lru_prev->lru_next = victim->lru_next;
                else dentry_lru_head = victim->lru_next;
                if (victim->lru_next) victim->lru_next->lru_prev = victim->lru_prev;
                else dentry_lru_tail = victim->lru_prev;

                /* 归还到空闲链表 */
                memset(victim, 0, sizeof(*victim));
                victim->hash_next = dentry_free_list;
                dentry_free_list = victim;
                dcache_count--;
                reclaimed++;
            }
            victim = prev;
        }
        spin_unlock(&dcache_lock);
    }

    /* 再回收 inode 缓存 */
    uint32_t icache_target = target > reclaimed ? (target - reclaimed) : 0;
    if (icache_target > 0) {
        reclaimed += vfs_icache_reclaim(icache_target);
    }

    return reclaimed;
}
