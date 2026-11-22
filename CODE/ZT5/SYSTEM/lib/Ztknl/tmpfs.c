/*
 * tmpfs.c - 内存文件系统实现
 * 
 * 按照 Ztknl VFS API 重写
 */

#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "console.h"
#include "pmm.h"

#define TMPFS_LOG(fmt, ...)  printk("[TMPFS] " fmt, ##__VA_ARGS__)

#define TMPFS_MAX_FILES  256
#define TMPFS_MAX_PATH   256
#define TMPFS_MAX_NAME   64
#define TMPFS_BLOCK_SIZE 4096

/* 文件节点 */
struct tmpfs_node {
    char name[TMPFS_MAX_NAME];
    char path[TMPFS_MAX_PATH];
    int type;
    int mode;
    uint64_t size;
    int nlink;
    void *data;
    uint64_t capacity;
    int used;
};

static struct tmpfs_node tmpfs_nodes[TMPFS_MAX_FILES];

static struct tmpfs_node *tmpfs_alloc_node(void) {
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!tmpfs_nodes[i].used) {
            tmpfs_nodes[i].used = 1;
            return &tmpfs_nodes[i];
        }
    }
    return NULL;
}

static struct tmpfs_node *tmpfs_find(const char *path) {
    if (!path || !path[0]) return NULL;
    if (strcmp(path, "/") == 0) return &tmpfs_nodes[0];
    
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (tmpfs_nodes[i].used && strcmp(tmpfs_nodes[i].path, path) == 0) {
            return &tmpfs_nodes[i];
        }
    }
    return NULL;
}

/* ========== VFS 回调 ========== */

static int tmpfs_open(struct vfs_file *file, const char *path, uint32_t mode) {
    (void)mode;
    struct tmpfs_node *node = tmpfs_find(path);
    if (!node || node->type != VFS_TYPE_FILE) return -1;
    
    file->fs_data = node;
    file->offset = 0;
    return 0;
}

static ssize_t tmpfs_read(struct vfs_file *file, void *buf, size_t count) {
    struct tmpfs_node *node = (struct tmpfs_node *)file->fs_data;
    if (!node || !node->data) return 0;
    if (file->offset >= node->size) return 0;
    
    size_t avail = node->size - file->offset;
    if (avail > count) avail = count;
    
    memcpy(buf, (char *)node->data + file->offset, avail);
    file->offset += avail;
    return (ssize_t)avail;
}

static ssize_t tmpfs_write(struct vfs_file *file, const void *buf, size_t count) {
    struct tmpfs_node *node = (struct tmpfs_node *)file->fs_data;
    if (!node) return -1;
    
    uint64_t new_end = file->offset + count;
    if (new_end > node->capacity) {
        uint64_t new_cap = (new_end + TMPFS_BLOCK_SIZE - 1) & ~(TMPFS_BLOCK_SIZE - 1);
        void *new_data = krealloc(node->data, new_cap);
        if (!new_data) return -1;
        node->data = new_data;
        node->capacity = new_cap;
    }
    
    memcpy((char *)node->data + file->offset, buf, count);
    file->offset += count;
    if (file->offset > node->size) node->size = file->offset;
    return (ssize_t)count;
}

static int tmpfs_stat(const char *path, struct vfs_stat *stat) {
    struct tmpfs_node *node = tmpfs_find(path);
    if (!node) return -1;
    
    memset(stat, 0, sizeof(*stat));
    stat->type = node->type;
    stat->attr = node->mode;
    stat->size = node->size;
    stat->nlink = node->nlink;
    return 0;
}

static int tmpfs_readdir(const char *path, struct vfs_dirent *dirent, int index) {
    (void)path;
    if (index < 0) return -1;
    if (index == 0) {
        strncpy(dirent->name, ".", 255);
        dirent->type = VFS_TYPE_DIR;
        return 0;
    }
    if (index == 1) {
        strncpy(dirent->name, "..", 255);
        dirent->type = VFS_TYPE_DIR;
        return 0;
    }
    
    int count = 2;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!tmpfs_nodes[i].used) continue;
        if (tmpfs_nodes[i].path[0] == '/' && tmpfs_nodes[i].path[1] != '\0' &&
            strchr(tmpfs_nodes[i].path + 1, '/') == NULL) {
            if (count == index) {
                strncpy(dirent->name, tmpfs_nodes[i].name, 255);
                dirent->type = tmpfs_nodes[i].type;
                return 0;
            }
            count++;
        }
    }
    return -1;
}

static int tmpfs_mkdir(const char *path) {
    struct tmpfs_node *node = tmpfs_alloc_node();
    if (!node) return -1;
    
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    
    strncpy(node->name, name, TMPFS_MAX_NAME - 1);
    strncpy(node->path, path, TMPFS_MAX_PATH - 1);
    node->type = VFS_TYPE_DIR;
    node->mode = 0755;
    node->nlink = 2;
    node->data = NULL;
    node->capacity = 0;
    
    TMPFS_LOG("Created dir %s\n", path);
    return 0;
}

static int tmpfs_create(const char *path, uint32_t mode) {
    struct tmpfs_node *node = tmpfs_alloc_node();
    if (!node) return -1;
    
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    
    strncpy(node->name, name, TMPFS_MAX_NAME - 1);
    strncpy(node->path, path, TMPFS_MAX_PATH - 1);
    node->type = VFS_TYPE_FILE;
    node->mode = mode ? (mode & 0777) : 0644;
    node->nlink = 1;
    node->data = NULL;
    node->capacity = 0;
    node->size = 0;
    
    TMPFS_LOG("Created file %s\n", path);
    return 0;
}

/* ========== 挂载 ========== */

static struct vfs_ops tmpfs_ops = {
    .open = tmpfs_open,
    .read = tmpfs_read,
    .write = tmpfs_write,
    .stat = tmpfs_stat,
    .readdir = tmpfs_readdir,
    .mkdir = tmpfs_mkdir,
};

int tmpfs_mount(const char *path, uint64_t max_size) {
    (void)max_size;
    TMPFS_LOG("Mounting tmpfs at %s\n", path);
    
    memset(tmpfs_nodes, 0, sizeof(tmpfs_nodes));
    
    struct tmpfs_node *root = tmpfs_alloc_node();
    if (!root) return -1;
    
    strncpy(root->name, "/", TMPFS_MAX_NAME - 1);
    strncpy(root->path, "/", TMPFS_MAX_PATH - 1);
    root->type = VFS_TYPE_DIR;
    root->mode = 0755;
    root->nlink = 2;
    
    return vfs_mount(path, "tmpfs", &tmpfs_ops, NULL);
}
