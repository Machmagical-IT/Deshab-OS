/*
 * procfs.c - 进程文件系统实现
 * 
 * 按照 Ztknl VFS API 重写
 */

#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "console.h"
#include "pmm.h"
#include "task.h"
#include "cpu.h"

#define PROC_LOG(fmt, ...)  printk("[PROCFS] " fmt, ##__VA_ARGS__)
#define PROC_BUF_SIZE  4096

/* 外部声明 */
extern uint64_t pmm_total_free_pages;
extern uint64_t pmm_total_pages;

/* proc 条目 */
struct proc_entry {
    const char *name;
    int type;
    int (*show)(char *buf, int size);
    char buf[PROC_BUF_SIZE];
    int buf_len;
};

/* ========== 读取函数 ========== */

static int proc_show_meminfo(char *buf, int size) {
    extern uint64_t pmm_get_total_memory(void);
    extern uint64_t pmm_get_free_memory(void);
    
    uint64_t total_kb = pmm_get_total_memory() / 1024;
    uint64_t free_kb = pmm_get_free_memory() / 1024;
    uint64_t used_kb = total_kb - free_kb;
    
    return snprintf(buf, size,
        "MemTotal:       %10lu kB\n"
        "MemFree:        %10lu kB\n"
        "MemAvailable:   %10lu kB\n"
        "MemUsed:        %10lu kB\n",
        total_kb, free_kb, free_kb, used_kb);
}

static int proc_show_version(char *buf, int size) {
    return snprintf(buf, size, "ZratroOS version 1.1.1 (Ztknl x86_64)\n");
}

static int proc_show_uptime(char *buf, int size) {
    uint64_t ticks = rdtsc();
    uint64_t seconds = ticks / 2400000000ULL;
    return snprintf(buf, size, "%lu.00 %lu.00\n", seconds, seconds);
}

static int proc_show_stat(char *buf, int size) {
    return snprintf(buf, size,
        "cpu  1000 0 2000 50000 100 200 300 0 0 0\n"
        "ctxt 1000000\n"
        "btime 1609459200\n"
        "processes 1\n"
        "procs_running 1\n");
}

static int proc_show_self(char *buf, int size) {
    struct process *curr = get_current_task();
    if (!curr) return 0;
    return snprintf(buf, size, "%u\n", curr->pid);
}

/* 条目缓冲区 */
static struct proc_entry proc_files[] = {
    {"meminfo",  VFS_TYPE_FILE, proc_show_meminfo, {}, 0},
    {"version",  VFS_TYPE_FILE, proc_show_version, {}, 0},
    {"uptime",   VFS_TYPE_FILE, proc_show_uptime,  {}, 0},
    {"stat",     VFS_TYPE_FILE, proc_show_stat,    {}, 0},
    {"self",     VFS_TYPE_FILE, proc_show_self,    {}, 0},
    {NULL, 0, NULL, {}, 0}
};

/* ========== VFS 回调 ========== */

static int procfs_open(struct vfs_file *file, const char *path, uint32_t mode) {
    (void)mode;
    
    const char *name = path;
    if (name[0] == '/') name++;
    
    for (int i = 0; proc_files[i].name; i++) {
        if (strcmp(name, proc_files[i].name) == 0) {
            /* 填充缓冲区 */
            proc_files[i].buf_len = proc_files[i].show(proc_files[i].buf, PROC_BUF_SIZE);
            file->offset = 0;
            return 0;
        }
    }
    return -1;
}

static ssize_t procfs_read(struct vfs_file *file, void *buf, size_t count) {
    /* 从绝对路径中提取 proc 文件名 (例如 "/proc/meminfo" -> "meminfo") */
    const char *path = file->path;
    if (path[0] == '/') path++;

    /* 跳过挂载点前缀 (例如 "proc/") */
    const char *slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') slash = p;
    }
    const char *name = slash ? slash + 1 : path;
    
    for (int i = 0; proc_files[i].name; i++) {
        if (strcmp(name, proc_files[i].name) == 0) {
            if (file->offset >= (uint64_t)proc_files[i].buf_len) {
                return 0;  /* EOF */
            }
            
            size_t avail = proc_files[i].buf_len - file->offset;
            if (avail > count) avail = count;
            
            memcpy(buf, proc_files[i].buf + file->offset, avail);
            file->offset += avail;
            return (ssize_t)avail;
        }
    }
    return -1;
}

static int procfs_stat(const char *path, struct vfs_stat *stat) {
    memset(stat, 0, sizeof(*stat));
    
    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        stat->type = VFS_TYPE_DIR;
        stat->attr = 0755;
        return 0;
    }
    
    const char *name = path;
    if (name[0] == '/') name++;
    
    for (int i = 0; proc_files[i].name; i++) {
        if (strcmp(name, proc_files[i].name) == 0) {
            stat->type = VFS_TYPE_FILE;
            stat->attr = 0444;
            stat->size = proc_files[i].buf_len;
            return 0;
        }
    }
    return -1;
}

static int procfs_readdir(const char *path, struct vfs_dirent *dirent, int index) {
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
    
    int file_idx = index - 2;
    /* 计算 proc_files 数组大小 (不含 NULL 终止符) */
    int num_proc_files = 0;
    while (proc_files[num_proc_files].name) num_proc_files++;

    if (file_idx >= 0 && file_idx < num_proc_files && proc_files[file_idx].name) {
        strncpy(dirent->name, proc_files[file_idx].name, 255);
        dirent->type = VFS_TYPE_FILE;
        return 0;
    }
    return -1;
}

/* ========== 挂载 ========== */

static struct vfs_ops procfs_ops = {
    .open = procfs_open,
    .read = procfs_read,
    .stat = procfs_stat,
    .readdir = procfs_readdir,
};

int procfs_mount(const char *path) {
    PROC_LOG("Mounting procfs at %s\n", path);
    return vfs_mount(path, "procfs", &procfs_ops, NULL);
}
