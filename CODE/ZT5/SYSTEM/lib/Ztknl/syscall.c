#include "syscall.h"
#include "exec.h"
#include "task.h"
#include "vfs.h"
#include "scheduler.h"
#include "console.h"
#include "heap.h"
#include "elf.h"
#include "abi.h"
#include "ipc.h"
#include "mp4_decode.h"
#include "exec.h"
#include "vmm.h"
#include "utx.h"
#include "pmem.h"
#include "pexlf.h"
#include "capability.h"
#include "signal.h"
#include "fb.h"
#include "pmm.h"
#include "ai.h"
#include "config.h"
#include "input.h"
#include "smp.h"
#include "ntdll.h"

/* Directory entry structure (matches user-space fm_dirent) */
struct fm_dirent {
    uint64_t ino;
    int64_t off;
    uint16_t reclen;
    char name[256];
    unsigned char type;
};

/* 全局变量：保存 int 0x80 入口时的寄存器值（由 idt_asm.S 设置） */
uint64_t syscall_debug_rdi = 0;
uint64_t syscall_debug_rsi = 0;
uint64_t syscall_debug_rdx = 0;
uint64_t syscall_debug_rax = 0;

/* 路径解析: 将用户提供的路径(可能是相对路径)解析为绝对路径
 * path: 用户提供的路径
 * out: 输出缓冲区 (至少 256 字节)
 * out_size: 输出缓冲区大小
 * 返回: 0 成功, -1 失败
 */
static int resolve_path(const char *path, char *out, size_t out_size) {
    if (path == NULL || out == NULL || out_size < 2) return -1;

    struct process *proc = process_get_current();
    const char *base;

    /* 确定基础路径 */
    if (path[0] == '/') {
        /* 绝对路径 */
        base = "/";
    } else {
        /* 相对路径: 基于进程 cwd */
        base = proc ? proc->cwd : "/";
    }

    /* 拼接基础路径和目标路径 */
    size_t base_len = 0;
    while (base_len < 255 && base[base_len]) base_len++;

    size_t path_len = 0;
    while (path_len < 255 && path[path_len]) path_len++;

    char combined[512];
    size_t combined_len = 0;

    if (path[0] != '/') {
        /* 复制基础路径 */
        if (base_len > 0) {
            for (size_t i = 0; i < base_len && combined_len < 511; i++)
                combined[combined_len++] = base[i];
        }
        /* 添加分隔符 */
        if (combined_len > 0 && combined[combined_len - 1] != '/' && combined_len < 511)
            combined[combined_len++] = '/';
    }

    /* 复制目标路径 */
    for (size_t i = 0; i < path_len && combined_len < 511; i++)
        combined[combined_len++] = path[i];
    combined[combined_len] = '\0';

    /* 规范化路径: 处理 ., .., 多余 / */
    size_t out_len = 0;
    size_t i = 0;

    while (i < combined_len && out_len < out_size - 1) {
        if (combined[i] == '/') {
            /* 跳过多余的 / */
            while (i < combined_len && combined[i] == '/') i++;
            if (out_len > 0 && out[out_len - 1] == '/') continue;
            out[out_len++] = '/';
            continue;
        }

        /* 检查 . 或 .. */
        if (combined[i] == '.') {
            if (i + 1 >= combined_len || combined[i + 1] == '/') {
                /* "." - 跳过 */
                i++;
                continue;
            }
            if (combined[i + 1] == '.' && (i + 2 >= combined_len || combined[i + 2] == '/')) {
                /* ".." - 回退一级 */
                i += 2;
                if (out_len > 1) {
                    out_len--;
                    while (out_len > 0 && out[out_len - 1] != '/') out_len--;
                }
                continue;
            }
        }

        /* 复制普通路径段 */
        while (i < combined_len && combined[i] != '/' && out_len < out_size - 1)
            out[out_len++] = combined[i++];
    }

    /* 确保路径以 / 开头且不以 / 结尾 (除非是根目录) */
    if (out_len == 0) {
        out[0] = '/';
        out[1] = '\0';
    } else {
        if (out_len > 1 && out[out_len - 1] == '/') out_len--;
        out[out_len] = '\0';
    }

    return 0;
}

/* 系统调用: exit */
static uint64_t sys_exit(int status) {
    process_exit(status);
    return 0;
}

/* 系统调用: read */
static uint64_t sys_read(int fd, void *buf, size_t count) {
    /* 安全检查: 验证用户空间缓冲区 */
    if (buf == NULL) {
        return (uint64_t)-1;
    }
    if (!access_ok(buf, count)) {
        println("[SYSCALL] sys_read: Invalid user buffer");
        return (uint64_t)-1;
    }
    
    /* stdin (fd 0) 从串口/TYY 读取 */
    if (fd == 0) {
        char kbuf[256];
        size_t read_size = count < sizeof(kbuf) ? count : sizeof(kbuf);
        size_t idx = 0;
        
        /* 从串口轮询读取 */
        while (idx < read_size) {
            int c = serial_getchar();
            if (c >= 0) {
                kbuf[idx++] = (char)c;
                /* 遇到换行符返回 */
                if ((char)c == '\n') break;
            } else {
                /* 无输入 */
                if (idx > 0) break;  /* 已有数据，返回 */
                /* 短暂忙等待 */
                for (volatile int i = 0; i < 100000; i++);
                /* 再次检查 */
                c = serial_getchar();
                if (c < 0) return 0;  /* 仍无数据，返回 0 */
            }
        }
        
        if (idx > 0) {
            if (copy_to_user(buf, kbuf, idx) != 0) {
                return (uint64_t)-1;
            }
            return idx;
        }
        return 0;
    }
    
    struct process *proc = process_get_current();
    if (proc == NULL || proc->fd_table == NULL) {
        return (uint64_t)-1;
    }
    
    struct fd_entry *entry = fd_get(proc->fd_table, fd);
    if (entry == NULL) {
        return (uint64_t)-1;
    }
    
    /* 根据类型分发 */
    switch (entry->type) {
    case FD_TYPE_FILE: {
        char kbuf[4096];
        size_t to_read = count < sizeof(kbuf) ? count : sizeof(kbuf);
        ssize_t ret = vfs_read((struct vfs_file *)entry->file, kbuf, to_read);
        if (ret > 0) {
            if (fd != 0 && fd != 1 && fd != 2 && ret < 1024) {
                static int _read_dbg_cnt = 0;
                if (_read_dbg_cnt < 8) {
                    _read_dbg_cnt++;
                    serial_print("[SYS_READ] fd=");
                    { char _nb[16]; int _np=0; long _v=(long)fd; if(_v<0){serial_print("-");_v=-_v;} if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
                    serial_print(" ret=");
                    { char _nb[16]; int _np=0; long _v=(long)ret; if(_v<0){serial_print("-");_v=-_v;} if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
                    serial_print(" hex=");
                    int hd = ret > 32 ? 32 : (int)ret;
                    for (int i = 0; i < hd; i++) {
                        char hx[3];
                        hx[0] = "0123456789abcdef"[(kbuf[i] >> 4) & 0xF];
                        hx[1] = "0123456789abcdef"[kbuf[i] & 0xF];
                        hx[2] = 0;
                        serial_print(hx);
                    }
                    serial_print("\n");
                }
            }
            if (copy_to_user(buf, kbuf, ret) != 0) {
                return (uint64_t)-1;
            }
        }
        return (uint64_t)ret;
    }
    case FD_TYPE_PIPE:
        return pipe_read((struct pipe *)entry->pipe, buf, count);
    case FD_TYPE_TTY:
        return 0;
    default:
        return (uint64_t)-1;
    }
}

/* 系统调用: write */
static uint64_t sys_write(int fd, const void *buf, size_t count) {
    /* 安全检查: 验证用户空间缓冲区 */
    if (buf == NULL) {
        return (uint64_t)-1;
    }
    
    if (!access_ok(buf, count)) {
        return (uint64_t)-1;
    }
    
    /* stdin (fd 0) 只读 */
    if (fd == 0) {
        return (uint64_t)-1;
    }
    
    /* stdout/stderr 写入控制台 */
    if (fd == 1 || fd == 2) {
        /* 使用安全的用户空间读取方式 */
        char kbuf[256];
        size_t written = 0;
        
        while (written < count) {
            size_t chunk = count - written;
            if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
            
            /* 使用 SMAP 安全读取 */
            if (copy_from_user(kbuf, (const void *)((const char *)buf + written), chunk) != 0) {
                break;  /* 读取失败 */
            }
            
            /* 输出到串口 (不输出到屏幕帧缓冲区，避免GUI程序闪烁) */
            for (size_t i = 0; i < chunk; i++) {
                serial_putchar(kbuf[i]);
            }
            written += chunk;
        }
        return written;
    }
    
    struct process *proc = process_get_current();
    if (proc == NULL || proc->fd_table == NULL) {
        return (uint64_t)-1;
    }
    
    struct fd_entry *entry = fd_get(proc->fd_table, fd);
    if (entry == NULL) {
        return (uint64_t)-1;
    }
    
    /* 根据类型分发 */
    switch (entry->type) {
    case FD_TYPE_FILE: {
        char kbuf[4096];
        size_t written = 0;
        while (written < count) {
            size_t chunk = count - written;
            if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
            if (copy_from_user(kbuf, (const void *)((const char *)buf + written), chunk) != 0) {
                break;
            }
            ssize_t ret = vfs_write((struct vfs_file *)entry->file, kbuf, chunk);
            if (ret <= 0) break;
            written += ret;
            if ((size_t)ret < chunk) break;
        }
        return written > 0 ? written : (uint64_t)-1;
    }
    case FD_TYPE_PIPE:
        return pipe_write((struct pipe *)entry->pipe, buf, count);
    default:
        return (uint64_t)-1;
    }
}

/* 系统调用: open */
static uint64_t sys_open(const char *path, int flags, int mode) {
    (void)mode;

    /* 安全检查: 验证用户空间路径字符串 */
    if (path == NULL) {
        println("[OPEN] NULL path");
        return (uint64_t)-1;
    }

    char upath[256];
    if (copy_string_from_user(upath, path, sizeof(upath)) != 0) {
        return (uint64_t)-1;
    }

    /* 解析路径 (支持相对路径) */
    char kpath[256];
    if (resolve_path(upath, kpath, sizeof(kpath)) != 0) {
        return (uint64_t)-1;
    }

    struct process *proc = process_get_current();
    if (proc == NULL) {
        return (uint64_t)-1;
    }

    /* 确保进程有文件描述符表 */
    if (proc->fd_table == NULL) {
        proc->fd_table = fd_table_create();
        if (proc->fd_table == NULL) {
            return (uint64_t)-1;
        }
    }

    /* 转换 flags 到 VFS 模式 */
    int vfs_mode = 0;
    if ((flags & 0x3) == 0) vfs_mode = VFS_MODE_READ;       /* O_RDONLY */
    else if (flags & 0x1) vfs_mode = VFS_MODE_WRITE;        /* O_WRONLY */
    else if (flags & 0x2) vfs_mode = VFS_MODE_READ | VFS_MODE_WRITE; /* O_RDWR */

    /* O_CREAT: 如果文件不存在则创建 */
    if (flags & 0x40) vfs_mode |= VFS_MODE_CREATE;

    struct vfs_file *file = vfs_open(kpath, vfs_mode);
    if (file == NULL) {
        return (uint64_t)-1;
    }

    /* 分配文件描述符，隐藏内核地址 */
    int fd = fd_alloc(proc->fd_table, FD_TYPE_FILE, file, flags);
    if (fd < 0) {
        vfs_close(file);
        return (uint64_t)-1;
    }

    return (uint64_t)fd;
}

/* 系统调用: close */
static uint64_t sys_close(int fd) {
    struct process *proc = process_get_current();
    if (proc == NULL || proc->fd_table == NULL) {
        return (uint64_t)-1;
    }

    struct fd_entry *entry = fd_get(proc->fd_table, fd);
    if (entry == NULL) {
        return (uint64_t)-1;
    }

    /* fd_free 会根据 ref_count 决定是否调用 vfs_close */
    int ret = fd_free(proc->fd_table, fd);
    return (uint64_t)ret;
}

/* 系统调用: seek */
static uint64_t sys_seek(int fd, uint64_t offset) {
    struct process *proc = process_get_current();
    if (proc == NULL || proc->fd_table == NULL) {
        return (uint64_t)-1;
    }
    
    struct fd_entry *entry = fd_get(proc->fd_table, fd);
    if (entry == NULL || entry->type != FD_TYPE_FILE) {
        return (uint64_t)-1;
    }
    
    return vfs_seek((struct vfs_file *)entry->file, offset);
}

/* 系统调用: stat */
static uint64_t sys_stat(const char *path, void *stat_buf) {
    if (path == NULL || stat_buf == NULL) {
        return (uint64_t)-1;
    }
    char upath[256];
    if (copy_string_from_user(upath, path, sizeof(upath)) != 0) {
        return (uint64_t)-1;
    }
    /* 解析路径 (支持相对路径) */
    char kpath[256];
    if (resolve_path(upath, kpath, sizeof(kpath)) != 0) {
        return (uint64_t)-1;
    }
    if (!access_ok(stat_buf, sizeof(struct vfs_stat))) {
        return (uint64_t)-1;
    }
    return vfs_stat(kpath, (struct vfs_stat *)stat_buf);
}

/* 系统调用: 获取当前工作目录 */
static uint64_t sys_getcwd(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        return (uint64_t)-1;
    }
    if (!access_ok(buf, size)) {
        return (uint64_t)-1;
    }
    struct process *proc = process_get_current();
    if (proc == NULL) {
        return (uint64_t)-1;
    }
    size_t len = 0;
    while (len < 255 && proc->cwd[len]) len++;
    if (len + 1 > size) {
        return (uint64_t)-1;
    }
    if (copy_to_user(buf, proc->cwd, len + 1) != 0) {
        return (uint64_t)-1;
    }
    return len + 1;
}

/* 系统调用: 内存映射 */
static uint64_t sys_mmap(void *addr, size_t length, int prot, int flags, int fd, uint64_t offset) {
    /* 简化实现: 使用堆分配 */
    if (length == 0) {
        return (uint64_t)-1;
    }
    /* TODO: 实现完整的 mmap 逻辑 */
    void *ret = kmalloc(length);
    if (ret == NULL) {
        return (uint64_t)-1;
    }
    /* 如果是匿名映射，清零内存 */
    if (flags & 0x20) {  /* MAP_ANONYMOUS */
        memset(ret, 0, length);
    }
    return (uint64_t)ret;
}

/* 系统调用: 取消内存映射 */
static uint64_t sys_munmap(void *addr, size_t length) {
    /* 简化实现: 直接释放 */
    if (addr == NULL || length == 0) {
        return (uint64_t)-1;
    }
    /* TODO: 实现完整的 munmap 逻辑 */
    kfree(addr);
    return 0;
}

/* 系统调用: 设备控制 */
static uint64_t sys_ioctl(int fd, unsigned long request, void *argp) {
    /* 简化实现: 返回 ENOTTY */
    (void)fd;
    (void)request;
    (void)argp;
    return (uint64_t)-25;  /* ENOTTY */
}

/* 系统调用: 读取目录 */
/* Note: Each call to readdir reads one entry.
 * The readdir index is stored per-file-descriptor (vfs_file.readdir_index)
 * so that multiple directories can be read concurrently without interference. */

static uint64_t sys_readdir(int fd, void *dirp, unsigned int count) {
    if (dirp == NULL || count == 0) {
        return (uint64_t)-1;
    }
    if (!access_ok(dirp, count)) {
        println("[SYSCALL] sys_readdir: Invalid buffer");
        return (uint64_t)-1;
    }
    
    /* Get the current process and fd table */
    struct process *proc = process_get_current();
    if (proc == NULL || proc->fd_table == NULL) {
        println("[SYSCALL] sys_readdir: No process or fd_table");
        return (uint64_t)-1;
    }
    
    struct fd_entry *entry = fd_get(proc->fd_table, fd);
    if (entry == NULL || entry->type == FD_TYPE_NONE) {
        print("[SYSCALL] sys_readdir: Invalid fd fd=");
        print_dec(fd);
        println("");
        return (uint64_t)-1;
    }
    
    /* Get the path from the file descriptor */
    struct vfs_file *file = (struct vfs_file *)entry->file;
    if (file == NULL) {
        return (uint64_t)-1;
    }

    /* Read directory entry at current index */
    struct vfs_dirent kern_dirent;
    memset(&kern_dirent, 0, sizeof(kern_dirent));

    int ret = vfs_readdir(file->path, &kern_dirent, file->readdir_index);
    if (ret < 0) {
        /* End of directory or error */
        return (uint64_t)-1;
    }
    
    /* Copy to user space - convert vfs_dirent to fm_dirent format */
    struct fm_dirent kern_fm;
    memset(&kern_fm, 0, sizeof(kern_fm));
    
    int name_len;
    for (name_len = 0; name_len < 255 && kern_dirent.name[name_len]; name_len++) {
        kern_fm.name[name_len] = kern_dirent.name[name_len];
    }
    kern_fm.name[name_len] = '\0';
    
    kern_fm.type = (kern_dirent.type == VFS_TYPE_DIR) ? 2 : 
                   (kern_dirent.type == VFS_TYPE_SYMLINK) ? 3 : 1;
    kern_fm.ino = kern_dirent.inode;
    kern_fm.off = file->readdir_index;
    kern_fm.reclen = sizeof(struct fm_dirent);

    if (copy_to_user(dirp, &kern_fm, sizeof(kern_fm)) != 0) {
        return (uint64_t)-1;
    }

    /* Increment index for next call */
    file->readdir_index++;

    return 1;  /* One entry read */
}

/* 系统调用: 创建目录 */
static uint64_t sys_mkdir(const char *path, int mode) {
    (void)mode;
    if (path == NULL) return (uint64_t)-1;
    char upath[256];
    if (copy_string_from_user(upath, path, sizeof(upath)) != 0) {
        return (uint64_t)-1;
    }
    /* 解析路径 (支持相对路径) */
    char kpath[256];
    if (resolve_path(upath, kpath, sizeof(kpath)) != 0) {
        return (uint64_t)-1;
    }
    int ret = vfs_mkdir(kpath);
    return (ret < 0) ? (uint64_t)-1 : 0;
}

/* 系统调用: 删除目录 */
static uint64_t sys_rmdir(const char *path) {
    if (path == NULL) return (uint64_t)-1;
    char upath[256];
    if (copy_string_from_user(upath, path, sizeof(upath)) != 0) {
        return (uint64_t)-1;
    }
    char kpath[256];
    if (resolve_path(upath, kpath, sizeof(kpath)) != 0) {
        return (uint64_t)-1;
    }
    int ret = vfs_rmdir(kpath);
    return (ret < 0) ? (uint64_t)-1 : 0;
}

/* 系统调用: 删除文件 */
static uint64_t sys_unlink(const char *path) {
    if (path == NULL) return (uint64_t)-1;
    char upath[256];
    if (copy_string_from_user(upath, path, sizeof(upath)) != 0) {
        return (uint64_t)-1;
    }
    char kpath[256];
    if (resolve_path(upath, kpath, sizeof(kpath)) != 0) {
        return (uint64_t)-1;
    }
    int ret = vfs_unlink(kpath);
    return (ret < 0) ? (uint64_t)-1 : 0;
}

/* 系统调用: 打开目录 */
static uint64_t sys_opendir(const char *path) {
    if (!access_ok_string((const char *)path, 256)) {
        return (uint64_t)-1;
    }
    /* 从用户空间复制路径 */
    char upath[256];
    if (copy_string_from_user(upath, path, sizeof(upath)) != 0) {
        return (uint64_t)-1;
    }
    /* 解析路径 (支持相对路径) */
    char kpath[256];
    if (resolve_path(upath, kpath, sizeof(kpath)) != 0) {
        return (uint64_t)-1;
    }

    struct process *proc = process_get_current();
    if (proc == NULL) return (uint64_t)-1;
    if (proc->fd_table == NULL) {
        proc->fd_table = fd_table_create();
        if (proc->fd_table == NULL) return (uint64_t)-1;
    }

    /* 打开目录 */
    struct vfs_file *file = vfs_open(kpath, VFS_MODE_READ);
    if (file == NULL) return (uint64_t)-1;

    int fd = fd_alloc(proc->fd_table, FD_TYPE_FILE, file, 0x1000);
    if (fd < 0) {
        vfs_close(file);
        return (uint64_t)-1;
    }

    /* 重置 readdir 索引 */
    file->readdir_index = 0;
    return (uint64_t)fd;
}

/* 系统调用: 关闭目录 */
static uint64_t sys_closedir(int fd) {
    return sys_close(fd);
}

/* 系统调用: 改变工作目录 */
static uint64_t sys_chdir(const char *path) {
    if (path == NULL) return (uint64_t)-1;
    struct process *proc = process_get_current();
    if (proc == NULL) return (uint64_t)-1;

    char upath[256];
    if (copy_string_from_user(upath, path, sizeof(upath)) != 0) {
        return (uint64_t)-1;
    }
    /* 解析路径 (支持相对路径, 基于 proc->cwd) */
    char kpath[256];
    if (resolve_path(upath, kpath, sizeof(kpath)) != 0) {
        return (uint64_t)-1;
    }
    /* 验证目标路径存在且为目录 */
    struct vfs_stat st;
    if (vfs_stat(kpath, &st) != 0) {
        return (uint64_t)-1;
    }
    if (st.type != VFS_TYPE_DIR) {
        return (uint64_t)-1;
    }
    /* 更新进程工作目录 */
    int i = 0;
    while (i < 255 && kpath[i]) {
        proc->cwd[i] = kpath[i];
        i++;
    }
    proc->cwd[i] = '\0';
    return 0;
}

/* 系统调用: 设置断点 */
static uint64_t sys_brk(void *addr) {
    /* 简化实现: 返回当前地址 */
    return 0;
}

/* 系统调用: exec */
static void map_fb_for_process(struct process *proc) {
    struct fb_info *fb = fb_get_info();
    if (!fb || !fb->buffer || !fb->width || !fb->height) {
        serial_print("[MAP-FB] SKIP: fb info invalid\n");
        return;
    }

    uint64_t user_fb_virt = 0x08000000;
    uint64_t fb_phys = fb->physical;
    uint64_t fb_size = fb->size;
    uint64_t fb_pages = (fb_size + 4095) / 4096;
    if (fb_pages < 1024) fb_pages = 1024;

    serial_print("[MAP-FB] phys=0x");
    { char h[20]; int p=0; for(int i=60;i>=0;i-=4) h[p++]="0123456789ABCDEF"[(fb_phys>>i)&0xF]; h[p]='\n'; h[p+1]=0; serial_print(h); }
    serial_print("[MAP-FB] size=");
    { char b[16]; int p=0; uint64_t v=fb_size; if(!v){b[p++]='0';}else{char t[16];int tp=0;while(v){t[tp++]='0'+(v%10);v/=10;}while(tp)b[p++]=t[--tp];} b[p]='\n'; b[p+1]=0; serial_print(b); }
    serial_print("[MAP-FB] pages=");
    { char b[16]; int p=0; uint64_t v=fb_pages; if(!v){b[p++]='0';}else{char t[16];int tp=0;while(v){t[tp++]='0'+(v%10);v/=10;}while(tp)b[p++]=t[--tp];} b[p]='\n'; b[p+1]=0; serial_print(b); }

    /* 关键: 实机 GPU framebuffer 是 PCI MMIO, 必须使用 Write-Combining 或
     * Write-Through 缓存模式, 否则写入会被 CPU 缓存吞掉, GPU 永远看不到更新.
     * 这里使用 PWT (Write-Through), 兼容 QEMU RAM framebuffer 和实机 PCI MMIO.
     * PWT=1 (bit 3), PCD=0 (bit 4 不设): Write-Through, 写入立即穿透到 GPU. */
    uint64_t fb_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER
                      | VMM_FLAG_WRITETHROUGH;
    int map_ret = vmm_map_range(proc->space, user_fb_virt, fb_phys, fb_pages, fb_flags);
    if (map_ret == 0) {
        serial_print("[MAP-FB] OK for pid=");
        char pbuf[16]; int pp = 0; uint64_t pv = proc->pid; if(pv==0){pbuf[pp++]='0';}else{char t[16];int tp=0;while(pv>0){t[tp++]='0'+(pv%10);pv/=10;}while(tp)pbuf[pp++]=t[--tp];} pbuf[pp]='\n'; pbuf[pp+1]=0;
        serial_print(pbuf);
        /* 验证映射: 读回物理地址确认映射正确 */
        uint64_t verify = vmm_get_physical(proc->space, user_fb_virt);
        serial_print("[MAP-FB] verify: 0x08000000 -> phys 0x");
        { char h[20]; int p=0; for(int i=60;i>=0;i-=4) h[p++]="0123456789ABCDEF"[(verify>>i)&0xF]; h[p]='\n'; h[p+1]=0; serial_print(h); }
    } else {
        serial_print("[MAP-FB] FAILED ret=");
        char rbuf[16]; rbuf[0] = '0' + map_ret; rbuf[1] = '\n'; rbuf[2] = '\0';
        serial_print(rbuf);
    }
}

static uint64_t sys_exec(const char *path) {
    if (path == NULL || !access_ok_string(path, 256)) {
        return (uint64_t)-1;
    }

    struct process *caller = process_get_current();
    uint32_t caller_pid = caller ? caller->pid : 0;

    /* 使用 elf_load_file 加载 ELF 并映射段到进程地址空间
     * (与 load_program 在 main.c 中加载 desk/fm/shell 的路径一致) */
    struct process *proc;
    uint64_t entry;

    if (elf_load_file(path, &proc, &entry) == 0) {
        map_fb_for_process(proc);
        /* 设置父进程为调用者, 以便父进程能 sys_wait */
        if (caller_pid != 0) proc->parent_pid = caller_pid;
        struct thread *thread = proc->threads;
        while (thread != NULL) {
            scheduler_add_thread(thread);
            thread = thread->process_next;
        }
        return proc->pid;
    }

    serial_print("[EXEC] ERROR: Failed to load executable: ");
    serial_print(path);
    serial_print("\n");
    return (uint64_t)-1;
}

/* 系统调用: execve */
static uint64_t sys_execve_wrapper(const char *path, char *const argv[], char *const envp[]) {
    /* 安全检查: 验证用户空间路径字符串 */
    if (path == NULL || !access_ok_string(path, 256)) {
        return (uint64_t)-1;
    }
    /* 验证 argv 和 envp 指针数组 (至少第一页可访问) */
    if (argv != NULL && !access_ok(argv, sizeof(char *))) {
        return (uint64_t)-1;
    }
    if (envp != NULL && !access_ok(envp, sizeof(char *))) {
        return (uint64_t)-1;
    }
    return sys_execve(path, argv, envp);
}

/* 系统调用: execveat */
static uint64_t sys_execveat_wrapper(int dirfd, const char *path, 
                                       char *const argv[], char *const envp[], int flags) {
    /* 安全检查: 验证用户空间路径字符串 */
    if (path == NULL || !access_ok_string(path, 256)) {
        return (uint64_t)-1;
    }
    if (argv != NULL && !access_ok(argv, sizeof(char *))) {
        return (uint64_t)-1;
    }
    if (envp != NULL && !access_ok(envp, sizeof(char *))) {
        return (uint64_t)-1;
    }
    return sys_execveat(dirfd, path, argv, envp, flags);
}

/* 系统调用: yield */
static uint64_t sys_yield(void) {
    schedule();
    return 0;
}

/* 系统调用: sleep */
static uint64_t sys_sleep(uint64_t ticks) {
    thread_sleep(ticks);
    return 0;
}

/* 系统调用: getpid */
static uint64_t sys_getpid(void) {
    struct process *proc = process_get_current();
    if (proc != NULL) {
        return proc->pid;
    }
    return 0;
}

/* 系统调用: getppid - 获取父进程 PID */
static uint64_t sys_getppid(void) {
    struct process *proc = process_get_current();
    if (proc != NULL) {
        return proc->parent_pid;
    }
    return 0;  /* 根进程无父进程 */
}

/* 系统调用: uname - 获取系统信息 */
static uint64_t sys_uname(struct utsname *buf) {
    if (buf == NULL) {
        return (uint64_t)-1;
    }
    if (!access_ok(buf, sizeof(struct utsname))) {
        return (uint64_t)-1;
    }
    
    /* 填充 UTS 信息 */
    const char *sysname = "ZratroOS";
    const char *nodename = "zratroos";
    const char *release = "1.1.1";
    const char *version = "1.1.1-g" __DATE__;
    const char *machine = "x86_64";
    
    /* 安全复制字符串 */
    for (int i = 0; i < 64 && sysname[i]; i++) buf->sysname[i] = sysname[i];
    buf->sysname[64] = '\0';
    
    for (int i = 0; i < 64 && nodename[i]; i++) buf->nodename[i] = nodename[i];
    buf->nodename[64] = '\0';
    
    for (int i = 0; i < 64 && release[i]; i++) buf->release[i] = release[i];
    buf->release[64] = '\0';
    
    for (int i = 0; i < 64 && version[i]; i++) buf->version[i] = version[i];
    buf->version[64] = '\0';
    
    for (int i = 0; i < 64 && machine[i]; i++) buf->machine[i] = machine[i];
    buf->machine[64] = '\0';
    
    return 0;
}

/* 系统调用: sysinfo - 获取系统详细信息 */
static uint64_t sys_sysinfo(struct sysinfo *info) {
    if (info == NULL) {
        return (uint64_t)-1;
    }
    if (!access_ok(info, sizeof(struct sysinfo))) {
        return (uint64_t)-1;
    }
    
    extern uint64_t pmm_get_free_memory(void);
    extern uint64_t pmm_get_total_memory(void);
    
    /* 填充系统信息 */
    info->uptime = 0;  /* TODO: 实现 uptime 计数 */
    info->loads[0] = 0;
    info->loads[1] = 0;
    info->loads[2] = 0;
    info->totalram = pmm_get_total_memory();
    info->freeram = pmm_get_free_memory();
    info->sharedram = 0;
    info->bufferram = 0;
    info->totalswap = 0;
    info->freeswap = 0;
    info->procs = 0;  /* TODO: 计数进程数 */
    info->totalhigh = 0;
    info->freehigh = 0;
    info->mem_unit = 1;
    
    return 0;
}

/* 系统调用: gettimeofday - 获取时间 */
static uint64_t sys_gettimeofday(struct timeval *tv, struct timezone *tz) {
    if (tv == NULL) {
        return (uint64_t)-1;
    }
    if (!access_ok(tv, sizeof(struct timeval))) {
        return (uint64_t)-1;
    }
    
    /* 读取 TSC 并转换为时间 */
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    
    /* 假设 2GHz CPU，转换为微秒 */
    uint64_t microseconds = tsc / 2000;
    
    tv->tv_sec = microseconds / 1000000;
    tv->tv_usec = microseconds % 1000000;
    
    /* 时区信息（可选） */
    if (tz != NULL && access_ok(tz, sizeof(struct timezone))) {
        tz->tz_minuteswest = 0;  /* UTC */
        tz->tz_dsttime = 0;
    }
    
    return 0;
}

/* 系统调用: time - 获取时间戳 */
static uint64_t g_time_base = 0;
static uint64_t g_time_tsc_base = 0;
static int g_time_initialized = 0;

static volatile int g_sntp_got_response = 0;
static uint64_t g_sntp_transmit_sec = 0;
static uint32_t g_sntp_transmit_frac = 0;

#define SNTP_PORT       123
#define NTP_EPOCH_DIFF  2208988800ULL
#define SNTP_TIMEOUT_MS 3000

static uint8_t cmos_read(uint8_t reg) {
    asm volatile("outb %0, $0x70" : : "a"(reg));
    uint8_t val;
    asm volatile("inb $0x71, %0" : "=a"(val));
    return val;
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static uint64_t rtc_to_epoch(int year, int month, int day, int hour, int min, int sec) {
    if (year < 70) year += 2000;
    else if (year < 100) year += 1900;
    int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if ((year % 400 == 0) || (year % 4 == 0 && year % 100 != 0)) mdays[1] = 29;
    uint64_t days = 0;
    for (int y = 1970; y < year; y++) {
        days += 365 + ((y % 400 == 0) || (y % 4 == 0 && y % 100 != 0) ? 1 : 0);
    }
    for (int m = 1; m < month; m++) days += mdays[m - 1];
    days += day - 1;
    return days * 86400 + hour * 3600 + min * 60 + sec;
}

static void time_init_from_rtc(void) {
    uint8_t sec = bcd_to_bin(cmos_read(0x00));
    uint8_t min = bcd_to_bin(cmos_read(0x02));
    uint8_t hour = bcd_to_bin(cmos_read(0x04));
    uint8_t day = bcd_to_bin(cmos_read(0x07));
    uint8_t month = bcd_to_bin(cmos_read(0x08));
    uint8_t year = bcd_to_bin(cmos_read(0x09));

    g_time_base = rtc_to_epoch(year + 2000, month, day, hour, min, sec);
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    g_time_tsc_base = tsc;
    g_time_initialized = 1;

    print("[TIME] RTC: 20");
    print_dec(year); print("/");
    print_dec(month); print("/");
    print_dec(day); print(" ");
    print_dec(hour); print(":");
    print_dec(min); print(":");
    print_dec(sec); print(" epoch=");
    print_dec(g_time_base);
    print("\n");
}

void sntp_handle_response(const uint8_t *data, size_t len) {
    if (len < 48) return;
    uint8_t mode = data[0] & 0x07;
    if (mode != 4 && mode != 5) return;
    g_sntp_transmit_sec = ((uint64_t)data[40] << 24) | ((uint64_t)data[41] << 16) |
                          ((uint64_t)data[42] << 8) | (uint64_t)data[43];
    g_sntp_transmit_frac = ((uint32_t)data[44] << 24) | ((uint32_t)data[45] << 16) |
                           ((uint32_t)data[46] << 8) | (uint32_t)data[47];
    g_sntp_got_response = 1;
}

void time_sync_ntp(void) {
    extern int net_is_connected(void);
    extern uint32_t net_get_ip(void);
    extern struct udp_pcb *udp_new(void);
    extern int udp_bind(struct udp_pcb *pcb, uint32_t addr, uint16_t port);
    extern int udp_sendto(struct udp_pcb *pcb, const void *data, size_t len,
                          uint32_t dst_ip, uint16_t dst_port);
    extern void udp_free(struct udp_pcb *pcb);
    extern void net_poll(void);
    extern uint64_t get_tick_count(void);

    if (!net_is_connected() || net_get_ip() == 0) {
        print("[TIME] No network for NTP, using RTC\n");
        if (!g_time_initialized) time_init_from_rtc();
        return;
    }

    print("[TIME] Network available, trying NTP sync...\n");

    struct udp_pcb *pcb = udp_new();
    if (pcb == NULL) {
        print("[TIME] UDP alloc failed, using RTC\n");
        if (!g_time_initialized) time_init_from_rtc();
        return;
    }
    udp_bind(pcb, 0, SNTP_PORT);

    uint8_t sntp_req[48];
    for (int i = 0; i < 48; i++) sntp_req[i] = 0;
    sntp_req[0] = 0x1B;

    uint32_t ntp_servers[] = {
        0xCB6B8458,
        0xD4C1B200,
        0x816B4858,
    };
    int ntp_count = sizeof(ntp_servers) / sizeof(ntp_servers[0]);

    g_sntp_got_response = 0;

    for (int s = 0; s < ntp_count && !g_sntp_got_response; s++) {
        uint32_t dst_ip = ntp_servers[s];
        print("[TIME] Sending SNTP to ");
        print_dec((dst_ip >> 24) & 0xFF); print(".");
        print_dec((dst_ip >> 16) & 0xFF); print(".");
        print_dec((dst_ip >> 8) & 0xFF); print(".");
        print_dec(dst_ip & 0xFF); print("\n");

        udp_sendto(pcb, sntp_req, 48, dst_ip, SNTP_PORT);

        uint64_t deadline = get_tick_count() + SNTP_TIMEOUT_MS;
        while (get_tick_count() < deadline && !g_sntp_got_response) {
            net_poll();
            for (volatile int d = 0; d < 10000; d++);
        }

        if (g_sntp_got_response) break;
        print("[TIME] NTP server timeout, trying next...\n");
    }

    udp_free(pcb);

    if (g_sntp_got_response) {
        if (g_sntp_transmit_sec > NTP_EPOCH_DIFF) {
            uint64_t unix_epoch = g_sntp_transmit_sec - NTP_EPOCH_DIFF;
            g_time_base = unix_epoch;
            uint64_t tsc;
            asm volatile("rdtsc" : "=A"(tsc));
            g_time_tsc_base = tsc;
            g_time_initialized = 1;
            print("[TIME] NTP sync OK, epoch=");
            print_dec(unix_epoch);
            print("\n");
        } else {
            print("[TIME] NTP response invalid, using RTC\n");
            if (!g_time_initialized) time_init_from_rtc();
        }
    } else {
        print("[TIME] NTP sync failed, using RTC\n");
        if (!g_time_initialized) time_init_from_rtc();
    }
}

static uint64_t sys_time(uint64_t *tloc) {
    if (!g_time_initialized) {
        time_init_from_rtc();
    }
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    uint64_t elapsed = (tsc - g_time_tsc_base) / 2000000000ULL;
    uint64_t result = g_time_base + elapsed;

    if (tloc != NULL && access_ok(tloc, sizeof(uint64_t))) {
        *tloc = result;
    }

    return result;
}

/* 系统调用: kill - 发送信号 */
static uint64_t sys_kill(pid_t pid, int sig) {
    if (sig < 0 || sig >= 64) {
        return (uint64_t)-1;  /* EINVAL */
    }
    
    int ret = signal_send(pid, sig);
    if (ret < 0) {
        return (uint64_t)-1;  /* ESRCH or EPERM */
    }
    
    return 0;
}

/* 系统调用: signal - 注册信号处理器 */
static uint64_t sys_signal(int signum, void (*handler)(int)) {
    struct process *proc = process_get_current();
    if (proc == NULL) {
        return (uint64_t)-1;
    }
    
    if (signum < 0 || signum >= 64) {
        return (uint64_t)-1;  /* EINVAL */
    }
    
    /* SIGKILL 和 SIGSTOP 不能被捕获或忽略 */
    if (signum == SIGKILL || signum == SIGSTOP) {
        return (uint64_t)-1;  /* EINVAL */
    }
    
    /* 确保信号状态已初始化 */
    if (proc->signals == NULL) {
        proc->signals = kmalloc(sizeof(struct signal_state));
        if (proc->signals == NULL) {
            return (uint64_t)-1;  /* ENOMEM */
        }
        signal_state_init(proc->signals);
    }
    
    /* 保存旧的处理函数 */
    void (*old_handler)(int) = proc->signals->actions[signum].sa_handler;
    
    /* 设置新的处理函数 */
    proc->signals->actions[signum].sa_handler = handler;
    sigemptyset(&proc->signals->actions[signum].sa_mask);
    proc->signals->actions[signum].sa_flags = 0;
    
    return (uint64_t)old_handler;
}

/* 系统调用: fork */
static uint64_t sys_fork(void) {
    struct process *parent = process_get_current();
    if (parent == NULL) {
        return (uint64_t)-1;
    }
    
    struct process *child = process_fork(parent);
    if (child == NULL) {
        return (uint64_t)-1;
    }
    
    /* 复制文件描述符表 */
    if (parent->fd_table != NULL) {
        child->fd_table = fd_table_dup(parent->fd_table);
    }
    
    /* 将子进程加入调度 */
    struct thread *thread = child->threads;
    while (thread != NULL) {
        scheduler_add_thread(thread);
        thread = thread->process_next;
    }
    
    /* 父进程返回子进程 PID */
    return child->pid;
}

/* 系统调用: pipe */
static uint64_t sys_pipe(int pipefd[2]) {
    /* 验证用户空间指针 */
    if (pipefd == NULL) {
        return (uint64_t)-1;
    }
    if (!access_ok(pipefd, sizeof(int) * 2)) {
        println("[SYSCALL] sys_pipe: Invalid pipefd pointer");
        return (uint64_t)-1;
    }
    return pipe_create(pipefd);
}

/* 系统调用: dup */
static uint64_t sys_dup(int oldfd) {
    struct process *proc = process_get_current();
    if (proc == NULL || proc->fd_table == NULL) {
        return (uint64_t)-1;
    }
    return fd_dup(proc->fd_table, oldfd);
}

/* 系统调用: dup2 */
static uint64_t sys_dup2(int oldfd, int newfd) {
    struct process *proc = process_get_current();
    if (proc == NULL || proc->fd_table == NULL) {
        return (uint64_t)-1;
    }
    return fd_dup2(proc->fd_table, oldfd, newfd);
}

/* 系统调用: wait */
static uint64_t sys_wait(int *status) {
    /* 安全检查: 验证用户空间指针 */
    if (status != NULL && !access_ok(status, sizeof(int))) {
        return (uint64_t)-1;
    }
    return sys_wait_impl(status);
}


/* ========== 用户态事务系统调用 (Zratro 原创增强) ========== */

/* 系统调用: tx_begin - 开始事务 */
static uint64_t sys_tx_begin(uint32_t flags, struct utx_handle_user *handle) {
    struct process *proc = process_get_current();
    if (proc == NULL) {
        return (uint64_t)-1;
    }
    
    /* 验证用户空间句柄指针 */
    if (handle == NULL || !access_ok(handle, sizeof(struct utx_handle_user))) {
        println("[SYSCALL] sys_tx_begin: Invalid handle pointer");
        return (uint64_t)-1;
    }
    
    return (uint64_t)utx_begin_kernel(proc->pid, flags, handle);
}

/* 系统调用: tx_commit - 提交事务 */
static uint64_t sys_tx_commit(uint64_t tx_id) {
    return (uint64_t)utx_commit_kernel(tx_id);
}

/* 系统调用: tx_abort - 中止事务 */
static uint64_t sys_tx_abort(uint64_t tx_id) {
    return (uint64_t)utx_abort_kernel(tx_id);
}

/* 系统调用: tx_write - 事务写入 */
static uint64_t sys_tx_write(uint64_t tx_id, int fd, uint64_t offset,
                              const void *data, size_t size) {
    /* 验证用户空间数据指针 */
    if (data == NULL || !access_ok(data, size)) {
        println("[SYSCALL] sys_tx_write: Invalid data pointer");
        return (uint64_t)-1;
    }
    
    return (uint64_t)utx_add_write_kernel(tx_id, fd, offset, data, size);
}

/* 系统调用: tx_read - 事务读取 */
static uint64_t sys_tx_read(uint64_t tx_id, int fd, uint64_t offset,
                            void *buf, size_t size) {
    /* 验证用户空间缓冲区指针 */
    if (buf == NULL || !access_ok(buf, size)) {
        println("[SYSCALL] sys_tx_read: Invalid buffer pointer");
        return (uint64_t)-1;
    }
    
    return (uint64_t)utx_add_read_kernel(tx_id, fd, offset, buf, size);
}

/* 系统调用: tx_snapshot - 创建快照 */
static uint64_t sys_tx_snapshot(uint64_t *snapshot_id) {
    struct process *proc = process_get_current();
    if (proc == NULL) {
        return (uint64_t)-1;
    }
    
    /* 验证用户空间指针 */
    if (snapshot_id == NULL || !access_ok(snapshot_id, sizeof(uint64_t))) {
        return (uint64_t)-1;
    }
    
    return (uint64_t)utx_snapshot_create_kernel(proc->pid, snapshot_id);
}

/* 系统调用: tx_status - 查询事务状态 */
static uint64_t sys_tx_status(uint64_t tx_id, struct utx_handle_user *handle) {
    /* 验证用户空间句柄指针 */
    if (handle == NULL || !access_ok(handle, sizeof(struct utx_handle_user))) {
        return (uint64_t)-1;
    }
    
    return (uint64_t)utx_get_status_kernel(tx_id, handle);
}

/* ========== 持久内存系统调用 (Zratro 原创增强) ========== */

/* 系统调用: 创建持久段 */
static uint64_t sys_pmem_create(const char *name, uint64_t size, uint32_t flags,
                                 struct pmem_handle_user *handle) {
    if (name == NULL || !access_ok_string(name, PMEM_MAX_NAME_LEN)) {
        return (uint64_t)-1;
    }
    if (handle == NULL || !access_ok(handle, sizeof(struct pmem_handle_user))) {
        return (uint64_t)-1;
    }
    
    struct pmem_manager *pm = pmem_get_global();
    if (pm == NULL) {
        return (uint64_t)-1;
    }
    
    struct pmem_segment *seg = pmem_create_segment(pm, name, size, flags);
    if (seg == NULL) {
        return (uint64_t)-1;
    }
    
    handle->seg_id = seg->seg_id;
    handle->size = seg->size;
    handle->flags = seg->flags;
    handle->state = seg->state;
    handle->base_addr = 0;  /* 不暴露内核地址，用户通过 pmem_alloc 获取偏移量访问 */
    
    return 0;
}

/* 系统调用: 打开持久段 */
static uint64_t sys_pmem_open(const char *name, uint32_t flags,
                               struct pmem_handle_user *handle) {
    if (name == NULL || !access_ok_string(name, PMEM_MAX_NAME_LEN)) {
        return (uint64_t)-1;
    }
    if (handle == NULL || !access_ok(handle, sizeof(struct pmem_handle_user))) {
        return (uint64_t)-1;
    }
    
    struct pmem_manager *pm = pmem_get_global();
    if (pm == NULL) {
        return (uint64_t)-1;
    }
    
    struct pmem_segment *seg = pmem_open_segment(pm, name, flags);
    if (seg == NULL) {
        return (uint64_t)-1;
    }
    
    handle->seg_id = seg->seg_id;
    handle->size = seg->size;
    handle->flags = seg->flags;
    handle->state = seg->state;
    handle->base_addr = 0;  /* 不暴露内核地址，用户通过 pmem_alloc 获取偏移量访问 */
    
    return 0;
}

/* 系统调用: 关闭持久段 */
static uint64_t sys_pmem_close(uint64_t seg_id) {
    struct pmem_manager *pm = pmem_get_global();
    if (pm == NULL) {
        return (uint64_t)-1;
    }
    
    struct pmem_segment *seg = pmem_find_segment(pm, seg_id);
    if (seg == NULL) {
        return (uint64_t)-1;
    }
    
    int ret = pmem_close_segment(pm, seg);
    return (uint64_t)ret;
}

/* 系统调用: 持久分配 */
static uint64_t sys_pmem_alloc(uint64_t seg_id, size_t size) {
    struct pmem_manager *pm = pmem_get_global();
    if (pm == NULL) {
        return (uint64_t)-1;
    }
    
    struct pmem_segment *seg = pmem_find_segment(pm, seg_id);
    if (seg == NULL) {
        return (uint64_t)-1;
    }
    
    void *ptr = pmem_alloc(seg, size);
    if (ptr == NULL) {
        pmem_seg_put(seg);
        return (uint64_t)-1;
    }
    
    /* 返回段内偏移而非内核地址 */
    uint64_t offset = (uint64_t)((uint8_t *)ptr - (uint8_t *)pmem_get_data_area(seg));
    pmem_seg_put(seg);
    
    return offset;
}

/* 系统调用: 持久释放 */
static uint64_t sys_pmem_free(uint64_t seg_id, uint64_t offset) {
    struct pmem_manager *pm = pmem_get_global();
    if (pm == NULL) {
        return (uint64_t)-1;
    }
    
    struct pmem_segment *seg = pmem_find_segment(pm, seg_id);
    if (seg == NULL) {
        return (uint64_t)-1;
    }
    
    int ret = pmem_free_offset(seg, offset);
    pmem_seg_put(seg);
    
    return (uint64_t)ret;
}

/* 系统调用: 创建快照 */
static uint64_t sys_pmem_snapshot(uint64_t seg_id, int incremental, uint64_t *snap_id) {
    if (snap_id == NULL || !access_ok(snap_id, sizeof(uint64_t))) {
        return (uint64_t)-1;
    }
    
    struct pmem_manager *pm = pmem_get_global();
    if (pm == NULL) {
        return (uint64_t)-1;
    }
    
    struct pmem_segment *seg = pmem_find_segment(pm, seg_id);
    if (seg == NULL) {
        return (uint64_t)-1;
    }
    
    struct pmem_snapshot *snap = pmem_snapshot_create(pm, seg, incremental);
    if (snap == NULL) {
        pmem_seg_put(seg);
        return (uint64_t)-1;
    }
    
    *snap_id = snap->snapshot_id;  /* access_ok 已在上方验证 */
    pmem_seg_put(seg);
    
    return 0;
}

/* 系统调用: 恢复快照 */
static uint64_t sys_pmem_restore(uint64_t snapshot_id) {
    struct pmem_manager *pm = pmem_get_global();
    if (pm == NULL) {
        return (uint64_t)-1;
    }
    
    /* 查找快照 */
    struct pmem_snapshot *snap = NULL;
    spin_lock(&pm->snap_lock);
    list_for_each_entry(snap, &pm->snapshots, list) {
        if (snap->snapshot_id == snapshot_id) {
            break;
        }
    }
    spin_unlock(&pm->snap_lock);
    
    if (snap == NULL) {
        return (uint64_t)-1;
    }
    
    return (uint64_t)pmem_snapshot_restore(pm, snap);
}

/* 系统调用: 同步持久内存 */
static uint64_t sys_pmem_sync(uint64_t seg_id) {
    struct pmem_manager *pm = pmem_get_global();
    if (pm == NULL) {
        return (uint64_t)-1;
    }
    
    struct pmem_segment *seg = pmem_find_segment(pm, seg_id);
    if (seg == NULL) {
        return (uint64_t)-1;
    }
    
    int ret = pmem_sync(seg);
    pmem_seg_put(seg);
    
    return (uint64_t)ret;
}

/* 系统调用: 获取统计信息 */
static uint64_t sys_pmem_stat(uint64_t *allocated, uint64_t *freed, uint64_t *segments) {
    if (allocated != NULL && !access_ok(allocated, sizeof(uint64_t))) {
        return (uint64_t)-1;
    }
    if (freed != NULL && !access_ok(freed, sizeof(uint64_t))) {
        return (uint64_t)-1;
    }
    if (segments != NULL && !access_ok(segments, sizeof(uint64_t))) {
        return (uint64_t)-1;
    }
    
    struct pmem_manager *pm = pmem_get_global();
    if (pm == NULL) {
        return (uint64_t)-1;
    }
    
    pmem_get_stats(pm, allocated, freed, segments);
    return 0;
}

/* ========== AI 自开发系统调用 ========== */

/* 系统调用: ai_send - 发送消息给 AI */
static uint64_t sys_ai_send(struct ai_message *msg) {
    if (msg == NULL || !access_ok(msg, sizeof(struct ai_message))) {
        return (uint64_t)-1;
    }

    struct ai_message kmsg;
    if (copy_from_user(&kmsg, msg, sizeof(kmsg)) != 0) {
        return (uint64_t)-1;
    }

    return (uint64_t)ai_send_message(&kmsg);
}

/* 系统调用: ai_recv - 接收 AI 响应 */
static uint64_t sys_ai_recv(struct ai_response *resp) {
    if (resp == NULL || !access_ok(resp, sizeof(struct ai_response))) {
        return (uint64_t)-1;
    }

    struct ai_response kresp;
    int ret = ai_recv_response(&kresp);
    if (ret <= 0)
        return (uint64_t)ret;

    if (copy_to_user(resp, &kresp, sizeof(kresp)) != 0) {
        return (uint64_t)-1;
    }

    return kresp.content_len;
}

/* 系统调用: ai_status - 查询 AI 服务状态 */
static uint64_t sys_ai_status(struct ai_status *status) {
    if (status == NULL || !access_ok(status, sizeof(struct ai_status))) {
        return (uint64_t)-1;
    }

    struct ai_status kstatus;
    int ret = ai_get_status(&kstatus);
    if (ret != 0)
        return (uint64_t)-1;

    if (copy_to_user(status, &kstatus, sizeof(kstatus)) != 0) {
        return (uint64_t)-1;
    }

    return 0;
}

/* 系统调用: ai_cancel - 取消请求 */
static uint64_t sys_ai_cancel(void) {
    return (uint64_t)ai_cancel_request();
}

/* 系统调用: ai_log_write - 写入对话日志 */
static uint64_t sys_ai_log_write(const void *data, uint32_t size, uint32_t role) {
    if (data == NULL || size == 0 || !access_ok(data, size)) {
        return (uint64_t)-1;
    }

    char *kbuf = kmalloc(size);
    if (!kbuf)
        return (uint64_t)-1;

    if (copy_from_user(kbuf, data, size) != 0) {
        kfree(kbuf);
        return (uint64_t)-1;
    }

    int ret = ai_log_add_entry(0, (enum ai_role)role, kbuf, size);
    kfree(kbuf);
    return (uint64_t)ret;
}

/* 系统调用: ai_log_list - 列出对话日志 */
static uint64_t sys_ai_log_list(uint64_t *ids, int max_count) {
    if (ids == NULL || max_count <= 0 || !access_ok(ids, sizeof(uint64_t) * max_count)) {
        return (uint64_t)-1;
    }

    uint64_t *kids = kmalloc(sizeof(uint64_t) * max_count);
    if (!kids)
        return (uint64_t)-1;

    int ret = ai_log_list_sessions(kids, max_count);
    if (ret > 0) {
        if (copy_to_user(ids, kids, sizeof(uint64_t) * ret) != 0) {
            kfree(kids);
            return (uint64_t)-1;
        }
    }

    kfree(kids);
    return (uint64_t)ret;
}

/* 系统调用: input_poll - 轮询键盘和鼠标事件
 * 参数: events - 事件数组指针, max_events - 最大事件数
 * 返回: 实际返回的事件数
 * 事件格式 (uint32_t):
 *   高16位: 事件类型 (0=无, 1=键盘按下, 2=键盘释放, 3=鼠标移动, 4=鼠标左键, 5=鼠标右键)
 *   低16位: 事件数据 (键盘=键码, 鼠标=坐标或按钮)
 */
static uint64_t sys_input_poll(uint32_t *events, int max_events) {
    if (events == NULL || max_events <= 0) {
        return (uint64_t)-1;
    }

    int count = 0;

    /* 获取键盘设备 — 边缘触发：只在键状态变化时发送事件 */
    struct input_dev *kbd = input_get_keyboard();
    if (kbd) {
        extern int kbd_evt_pop(uint32_t *evt);
        while (count < max_events) {
            uint32_t evt;
            if (!kbd_evt_pop(&evt)) break;
            if (copy_to_user(&events[count], &evt, sizeof(uint32_t)) != 0) {
                return (uint64_t)-1;
            }
            count++;
        }
    }

    /* 轮询 PS/2 鼠标（中断可能不工作时使用轮询备用方案） */
    extern void ps2_mouse_poll(void);
    extern struct input_dev *ps2_get_mouse_dev(void);
    ps2_mouse_poll();

    /* 使用 PS/2 鼠标设备 */
    struct input_dev *mouse = ps2_get_mouse_dev();
    if (!mouse) {
        mouse = input_get_mouse();
    }

    if (mouse && count < max_events) {
        int32_t mx = mouse->abs[ABS_X];
        int32_t my = mouse->abs[ABS_Y];

        /* 获取实际屏幕尺寸 */
        extern struct fb_info *fb_get_info(void);
        struct fb_info *fbi = fb_get_info();
        int32_t screen_w = (fbi && fbi->width > 0) ? (int32_t)fbi->width : 1280;
        int32_t screen_h = (fbi && fbi->height > 0) ? (int32_t)fbi->height : 800;

        /* 限制坐标在屏幕范围内 */
        if (mx < 0) mx = 0;
        if (mx >= screen_w) mx = screen_w - 1;
        if (my < 0) my = 0;
        if (my >= screen_h) my = screen_h - 1;

        /* 将 clamped 值写回设备，防止相对移动无限累积 */
        mouse->abs[ABS_X] = mx;
        mouse->abs[ABS_Y] = my;

        /* 只在鼠标实际移动时发送坐标事件 */
        static int32_t prev_mx = -1, prev_my = -1;
        if (mx != prev_mx || my != prev_my) {
            uint32_t evt_x = (3 << 16) | (uint16_t)mx;
            uint32_t evt_y = (4 << 16) | (uint16_t)my;

            if (count + 1 < max_events) {
                if (copy_to_user(&events[count], &evt_x, sizeof(uint32_t)) != 0) {
                    return (uint64_t)-1;
                }
                count++;
                if (copy_to_user(&events[count], &evt_y, sizeof(uint32_t)) != 0) {
                    return (uint64_t)-1;
                }
                count++;
            }
            prev_mx = mx;
            prev_my = my;
        }

        /* 鼠标按键 — 边缘触发：只在按下/释放变化时发送 */
        {
            static uint8_t prev_btn_left = 0, prev_btn_right = 0;
            uint8_t cur_left = input_key_is_pressed(mouse, BTN_LEFT) ? 1 : 0;
            uint8_t cur_right = input_key_is_pressed(mouse, BTN_RIGHT) ? 1 : 0;

            if (cur_left != prev_btn_left && count < max_events) {
                uint32_t evt = (5 << 16) | cur_left;
                if (copy_to_user(&events[count], &evt, sizeof(uint32_t)) != 0) {
                    return (uint64_t)-1;
                }
                count++;
                prev_btn_left = cur_left;
            }
            if (cur_right != prev_btn_right && count < max_events) {
                uint32_t evt = (6 << 16) | cur_right;
                if (copy_to_user(&events[count], &evt, sizeof(uint32_t)) != 0) {
                    return (uint64_t)-1;
                }
                count++;
                prev_btn_right = cur_right;
            }
        }
    }

    return (uint64_t)count;
}

/* ========== 权限系统调用 ========== */

static uint64_t sys_getuid(void) {
    struct process *proc = process_get_current();
    if (proc == NULL) {
        return (uint64_t)-1;
    }
    return proc->uid;
}

static uint64_t sys_getgid(void) {
    struct process *proc = process_get_current();
    if (proc == NULL) {
        return (uint64_t)-1;
    }
    return proc->gid;
}

static uint64_t sys_setuid(uint32_t uid) {
    struct process *proc = process_get_current();
    if (proc == NULL) {
        return (uint64_t)-1;
    }
    /* 只有 root 可以设置任意 UID，普通用户只能设置为自己的 UID */
    if (proc->uid != 0 && proc->uid != uid) {
        return (uint64_t)-1;
    }
    proc->uid = uid;
    return 0;
}

static uint64_t sys_setgid(uint32_t gid) {
    struct process *proc = process_get_current();
    if (proc == NULL) {
        return (uint64_t)-1;
    }
    if (proc->uid != 0 && proc->gid != gid) {
        return (uint64_t)-1;
    }
    proc->gid = gid;
    return 0;
}

static uint64_t sys_chmod(const char *path, uint32_t mode) {
    if (path == NULL || !access_ok_string(path, 256)) {
        return (uint64_t)-1;
    }
    return (uint64_t)vfs_chmod(path, mode);
}

static uint64_t sys_chown(const char *path, uint32_t uid, uint32_t gid) {
    if (path == NULL || !access_ok_string(path, 256)) {
        return (uint64_t)-1;
    }
    return (uint64_t)vfs_chown(path, uid, gid);
}

/* sudo: verify root password and elevate to root */
static uint64_t sys_sudo(const char *password_hash) {
    if (password_hash == NULL || !access_ok_string(password_hash, 64)) {
        return (uint64_t)-1;
    }

    char khash[64];
    if (copy_string_from_user(khash, password_hash, sizeof(khash)) != 0) {
        return (uint64_t)-1;
    }

    struct process *proc = process_get_current();
    if (proc == NULL) return (uint64_t)-1;

    if (proc->uid == 0) return 0;

    struct config_file *cfg = config_load("/ini/usrroot/user.cfg");
    if (cfg == NULL) return (uint64_t)-1;

    const char *root_hash = config_get_string(cfg, "passwd", NULL);
    uint32_t cfg_uid = (uint32_t)config_get_int(cfg, "uid", 1000);

    if (cfg_uid != 0) {
        config_free(cfg);
        return (uint64_t)-1;
    }

    if (root_hash == NULL) {
        config_free(cfg);
        return (uint64_t)-1;
    }

    int match = 1;
    for (int i = 0; i < 32; i++) {
        if (khash[i] != root_hash[i]) { match = 0; break; }
        if (khash[i] == 0 && root_hash[i] == 0) break;
    }
    config_free(cfg);

    if (!match) return (uint64_t)-1;

    /* Elevate to root */
    proc->uid = 0;
    proc->gid = 0;
    proc->username[0] = 'r'; proc->username[1] = 'o'; proc->username[2] = 'o'; proc->username[3] = 't'; proc->username[4] = 0;
    return 0;
}

/* ========== 统一系统调用分发 (消除代码重复) ========== */

/*
 * zratro_syscall_dispatch - Zratro 原生系统调用统一分发函数
 *
 * 中断路径 (syscall_handler) 和快速路径 (syscall_dispatch) 共用此函数，
 * 避免两份 switch-case 的维护隐患。
 */
uint64_t zratro_syscall_dispatch(uint64_t num, uint64_t arg0, uint64_t arg1,
                                         uint64_t arg2, uint64_t arg3, uint64_t arg4,
                                         uint64_t arg5) {
    (void)arg5;

    switch (num) {
        case SYS_EXIT:
            return sys_exit((int)arg0);
        case SYS_READ:
            return sys_read((int)arg0, (void *)arg1, arg2);
        case SYS_WRITE:
            return sys_write((int)arg0, (const void *)arg1, arg2);
        case SYS_OPEN:
            return sys_open((const char *)arg0, (int)arg1, (int)arg2);
        case SYS_CLOSE:
            return sys_close((int)arg0);
        case SYS_SEEK:
            return sys_seek((int)arg0, arg1);
        case SYS_STAT:
            return sys_stat((const char *)arg0, (void *)arg1);
        case SYS_GETCWD:
            return sys_getcwd((char *)arg0, (size_t)arg1);
        case SYS_MMAP:
            return sys_mmap((void *)arg0, (size_t)arg1, (int)arg2, (int)arg3, (int)arg4, arg5);
        case SYS_MUNMAP:
            return sys_munmap((void *)arg0, (size_t)arg1);
        case SYS_BRK:
            return sys_brk((void *)arg0);
        case SYS_CHDIR:
            return sys_chdir((const char *)arg0);
        case SYS_MKDIR:
            return sys_mkdir((const char *)arg0, (int)arg1);
        case SYS_RMDIR:
            return sys_rmdir((const char *)arg0);
        case SYS_UNLINK:
            return sys_unlink((const char *)arg0);
        case SYS_OPENDIR:
            return sys_opendir((const char *)arg0);
        case SYS_READDIR:
            return sys_readdir((int)arg0, (void *)arg1, (unsigned int)arg2);
        case SYS_CLOSEDIR:
            return sys_closedir((int)arg0);
        case SYS_IOCTL:
            return sys_ioctl((int)arg0, (unsigned long)arg1, (void *)arg2);
        case SYS_EXEC:
            return sys_exec((const char *)arg0);
        case SYS_YIELD:
            return sys_yield();
        case SYS_SLEEP:
            return sys_sleep(arg0);
        case SYS_GETPID:
            return sys_getpid();
        case SYS_GETPPID:
            return sys_getppid();
        case SYS_UNAME:
            return sys_uname((struct utsname *)arg0);
        case SYS_SYSINFO:
            return sys_sysinfo((struct sysinfo *)arg0);
        case SYS_GETTIMEOFDAY:
            return sys_gettimeofday((struct timeval *)arg0, (struct timezone *)arg1);
        case SYS_TIME:
            return sys_time((uint64_t *)arg0);
        case SYS_KILL:
            return sys_kill((pid_t)arg0, (int)arg1);
        case SYS_SIGNAL:
            return sys_signal((int)arg0, (void (*)(int))arg1);
        case SYS_FORK:
            return sys_fork();
        case SYS_PIPE:
            return sys_pipe((int *)arg0);
        case SYS_DUP:
            return sys_dup((int)arg0);
        case SYS_DUP2:
            return sys_dup2((int)arg0, (int)arg1);
        case SYS_WAIT:
            return sys_wait((int *)arg0);
        case SYS_EXECVE:
            return sys_execve_wrapper((const char *)arg0,
                                       (char *const *)arg1,
                                       (char *const *)arg2);
        case SYS_EXECVEAT:
            return sys_execveat_wrapper((int)arg0,
                                         (const char *)arg1,
                                         (char *const *)arg2,
                                         (char *const *)arg3,
                                         (int)arg4);
        /* 用户态事务系统调用 */
        case SYS_TX_BEGIN:
            return sys_tx_begin((uint32_t)arg0, (struct utx_handle_user *)arg1);
        case SYS_TX_COMMIT:
            return sys_tx_commit(arg0);
        case SYS_TX_ABORT:
            return sys_tx_abort(arg0);
        case SYS_TX_WRITE:
            return sys_tx_write(arg0, (int)arg1, arg2, (const void *)arg3, arg4);
        case SYS_TX_READ:
            return sys_tx_read(arg0, (int)arg1, arg2, (void *)arg3, arg4);
        case SYS_TX_SNAPSHOT:
            return sys_tx_snapshot((uint64_t *)arg0);
        case SYS_TX_STATUS:
            return sys_tx_status(arg0, (struct utx_handle_user *)arg1);
        /* 持久内存系统调用 */
        case SYS_PMEM_CREATE:
            return sys_pmem_create((const char *)arg0, arg1, (uint32_t)arg2,
                                    (struct pmem_handle_user *)arg3);
        case SYS_PMEM_OPEN:
            return sys_pmem_open((const char *)arg0, (uint32_t)arg1,
                                  (struct pmem_handle_user *)arg2);
        case SYS_PMEM_CLOSE:
            return sys_pmem_close(arg0);
        case SYS_PMEM_ALLOC:
            return sys_pmem_alloc(arg0, (size_t)arg1);
        case SYS_PMEM_FREE:
            return sys_pmem_free(arg0, arg1);
        case SYS_PMEM_SNAPSHOT:
            return sys_pmem_snapshot(arg0, (int)arg1, (uint64_t *)arg2);
        case SYS_PMEM_RESTORE:
            return sys_pmem_restore(arg0);
        case SYS_PMEM_SYNC:
            return sys_pmem_sync(arg0);
        case SYS_PMEM_STAT:
            return sys_pmem_stat((uint64_t *)arg0, (uint64_t *)arg1,
                                  (uint64_t *)arg2);
        case SYS_PEXLF_INIT:
            return pexlf_init();
        case SYS_PEXLF_CREATE:
            if (!arg0 || !access_ok_string((const char *)arg0, 32)) return (uint64_t)-1;
            return pexlf_channel_create((const char *)arg0);
        case SYS_PEXLF_JOIN:
            if (!arg0 || !access_ok_string((const char *)arg0, 32)) return (uint64_t)-1;
            return pexlf_channel_join((const char *)arg0);
        case SYS_PEXLF_LEAVE:
            return pexlf_channel_leave(arg0);
        case SYS_PEXLF_SEND:
            if (!arg0 || !arg2 || arg3 == 0) return (uint64_t)-1;
            if (!access_ok_string((const char *)arg0, 32) || !access_ok((const void *)arg2, arg3))
                return (uint64_t)-1;
            return pexlf_send((const char *)arg0, (uint32_t)arg1, (const void *)arg2, arg3);
        case SYS_PEXLF_RECV:
            if (!arg0 || !arg1 || arg2 == 0) return (uint64_t)-1;
            if (!access_ok_string((const char *)arg0, 32) || !access_ok((void *)arg1, arg2))
                return (uint64_t)-1;
            return pexlf_recv((const char *)arg0, (void *)arg1, arg2, arg3);
        case SYS_PEXLF_BROADCAST:
            if (!arg1 || arg2 == 0) return (uint64_t)-1;
            if (!access_ok((const void *)arg1, arg2)) return (uint64_t)-1;
            return pexlf_broadcast((uint32_t)arg0, (const void *)arg1, arg2);
        case SYS_PEXLF_LIST:
            if (!arg0 || arg1 <= 0) return (uint64_t)-1;
            if (!access_ok((char (*)[32])arg0, arg1 * 32)) return (uint64_t)-1;
            return pexlf_list_channels((char (*)[32])arg0, (int)arg1);
        case SYS_PEXLF_PEER_INFO:
            if (!arg1 || !access_ok((struct pexlf_endpoint *)arg1, sizeof(struct pexlf_endpoint)))
                return (uint64_t)-1;
            return pexlf_get_peer_info(arg0, (struct pexlf_endpoint *)arg1);
        case SYS_FB_SYNC:
            /* 刷新 CPU 缓存，确保 framebuffer 写入可见 */
            {
                extern uint64_t g_user_fb_virt;
                extern struct fb_info *fb_get_info(void);
                struct fb_info *fb = fb_get_info();

                /* 从用户空间复制到内核 framebuffer
                 * 必须使用 copy_from_user 因为 SMAP 阻止内核直接访问用户空间 */
                if (g_user_fb_virt && fb && fb->buffer) {
                    uint64_t fb_size = (uint64_t)fb->height * (uint64_t)fb->pitch;
                    if (copy_from_user(fb->buffer, (void *)g_user_fb_virt, fb_size) != 0) {
                        /* copy_from_user failed, try stac/clac approach */
                        asm volatile("stac" ::: "memory");
                        memcpy(fb->buffer, (void *)g_user_fb_virt, fb_size);
                        asm volatile("clac" ::: "memory");
                    }

                    /* 写屏障确保写入完成 */
                    asm volatile("sfence" ::: "memory");
                }
            }
            return 0;
        case SYS_GETFBINFO:
            /* 获取 framebuffer 信息
             * arg0: 指向用户空间 struct fb_info 的指针
             * struct fb_info { uint64_t base; uint32_t width; uint32_t height; uint32_t pitch; };
             */
            {
                extern struct fb_info *fb_get_info(void);
                
                struct fb_info_user {
                    uint64_t base;
                    uint32_t width;
                    uint32_t height;
                    uint32_t pitch;
                    uint32_t format;  /* 像素格式: 0=unknown, 1=XRGB8888, 2=XBGR8888 */
                } __attribute__((packed));

                if (!arg0 || !access_ok((void *)arg0, sizeof(struct fb_info_user))) {
                    serial_print("[GETFBINFO] Invalid pointer\n");
                    return (uint64_t)-1;
                }

                struct fb_info *fb = fb_get_info();
                if (!fb) {
                    serial_print("[GETFBINFO] fb_get_info returned NULL\n");
                    return (uint64_t)-1;
                }

                extern uint64_t g_user_fb_virt;

                /* 返回用户空间 framebuffer 映射地址
                 * 内核在 load_program 时已将 Limine framebuffer 物理页面
                 * 映射到用户进程的虚拟地址空间 (0x08000000)
                 * 用户进程直接写入该地址即写入 Limine framebuffer，无需 sys_fb_sync */
                struct fb_info_user info;
                info.base = g_user_fb_virt ? g_user_fb_virt : 0x08000000;
                info.width = fb->width;
                info.height = fb->height;
                info.pitch = fb->pitch;
                info.format = (uint32_t)fb->format;
                
                serial_print("[GETFBINFO] returning base=");
                { char bhex[20]; int bp = 0; for (int i = 60; i >= 0; i -= 4) bhex[bp++] = "0123456789ABCDEF"[(info.base >> i) & 0xF]; bhex[bp] = '\n'; bhex[bp+1] = '\0'; serial_print(bhex); }
                serial_print("[GETFBINFO] width=");
                char wbuf[16]; int wp2 = 0;
                uint32_t wv2 = fb->width;
                if (wv2 == 0) wbuf[wp2++] = '0';
                else { char wt2[16]; int tp2 = 0; while (wv2) { wt2[tp2++] = '0' + (wv2%10); wv2/=10; } while (tp2) wbuf[wp2++] = wt2[--tp2]; }
                wbuf[wp2] = '\n'; wbuf[wp2+1] = '\0';
                serial_print(wbuf);
                serial_print("[GETFBINFO] height=");
                char hbuf[16]; int hp2 = 0;
                uint32_t hv2 = fb->height;
                if (hv2 == 0) hbuf[hp2++] = '0';
                else { char ht2[16]; int tp3 = 0; while (hv2) { ht2[tp3++] = '0' + (hv2%10); hv2/=10; } while (tp3) hbuf[hp2++] = ht2[--tp3]; }
                hbuf[hp2] = '\n'; hbuf[hp2+1] = '\0';
                serial_print(hbuf);
                serial_print("[GETFBINFO] pitch=");
                char pbuf[16]; int pp = 0;
                uint32_t pv = fb->pitch;
                if (pv == 0) pbuf[pp++] = '0';
                else { char pt[16]; int tp4 = 0; while (pv) { pt[tp4++] = '0' + (pv%10); pv/=10; } while (tp4) pbuf[pp++] = pt[--tp4]; }
                pbuf[pp] = '\n'; pbuf[pp+1] = '\0';
                serial_print(pbuf);
                
                serial_print("[GETFBINFO] about to copy_to_user\n");
                
                if (copy_to_user((void *)arg0, &info, sizeof(info)) != 0) {
                    serial_print("[GETFBINFO] copy_to_user failed\n");
                    return (uint64_t)-1;
                }
                
                serial_print("[GETFBINFO] copy_to_user success\n");
                
                serial_print("[GETFBINFO] returned fb info\n");
                return 0;
            }
        
        /* ========== Capability 安全模型系统调用 ========== */
        case SYS_CAPABILITY_BASE + 0:  /* SYS_CAP_ALLOC */
            return cap_allocate(arg0, arg1, arg2, 0);
        case SYS_CAPABILITY_BASE + 1:  /* SYS_CAP_REVOKE */
            return cap_revoke(arg0);
        case SYS_CAPABILITY_BASE + 2:  /* SYS_CAP_CHECK */
            return cap_check(arg0, arg1, arg2);
        case SYS_CAPABILITY_BASE + 3:  /* SYS_CAP_LIST */
            if (!arg1 || !access_ok((void *)arg1, arg2))
                return (uint64_t)-1;
            return cap_list(arg0, (struct capability_internal *)arg1, arg2);
        case SYS_CAPABILITY_BASE + 4:  /* SYS_CAP_DELEGATE */
            return cap_delegate(arg0, arg1, arg2, arg3);

        /* ========== AI 自开发系统调用 ========== */
        case SYS_AI_SEND:
            return sys_ai_send((struct ai_message *)arg0);
        case SYS_AI_RECV:
            return sys_ai_recv((struct ai_response *)arg0);
        case SYS_AI_STATUS:
            return sys_ai_status((struct ai_status *)arg0);
        case SYS_AI_CANCEL:
            return sys_ai_cancel();
        case SYS_AI_LOG_WRITE:
            return sys_ai_log_write((const void *)arg0, (uint32_t)arg1, (uint32_t)arg2);
        case SYS_AI_LOG_LIST:
            return sys_ai_log_list((uint64_t *)arg0, (int)arg1);
        case SYS_INPUT_POLL:
            return sys_input_poll((uint32_t *)arg0, (int)arg1);
        case SYS_GETUID:
            return sys_getuid();
        case SYS_GETGID:
            return sys_getgid();
        case SYS_SETUID:
            return sys_setuid((uint32_t)arg0);
        case SYS_SETGID:
            return sys_setgid((uint32_t)arg0);
        case SYS_CHMOD:
            return sys_chmod((const char *)arg0, (uint32_t)arg1);
        case SYS_CHOWN:
            return sys_chown((const char *)arg0, (uint32_t)arg1, (uint32_t)arg2);
        case SYS_SUDO:
            return sys_sudo((const char *)arg0);

        case SYS_MP4_OPEN: {
            struct mp4_info *info = (struct mp4_info *)kmalloc(sizeof(struct mp4_info));
            if (!info) return (uint64_t)-1;
            if (mp4_parse((const char *)arg0, info) != 0) {
                kfree(info);
                return (uint64_t)-1;
            }
            return (uint64_t)info;
        }
        case SYS_MP4_DECODE: {
            struct mp4_info *info = (struct mp4_info *)arg0;
            if (!info) return (uint64_t)-1;
            if (mp4_decode_frame(info, (uint32_t)arg1, (uint32_t *)arg2, (int)arg3, (int)arg4) != 0)
                return (uint64_t)-1;
            return 0;
        }
        case SYS_MP4_CLOSE: {
            struct mp4_info *info = (struct mp4_info *)arg0;
            if (info) {
                mp4_close(info);
                kfree(info);
            }
            return 0;
        }

        default:
            {
                serial_print("[SYSCALL] Unknown Zratro syscall num=");
                char nbuf[12]; int p = 0;
                uint64_t n = num;
                if (n == 0) nbuf[p++] = '0';
                else { char t[20]; int tp = 0; while (n > 0) { t[tp++] = '0' + (n % 10); n /= 10; } while (tp > 0) nbuf[p++] = t[--tp]; }
                nbuf[p] = '\n'; serial_print(nbuf);
            }
            return (uint64_t)-1;
    }
}

/* ========== 中断路径入口 (int 0x80) ========== */

void syscall_handler(struct interrupt_frame *frame) {
    uint64_t syscall_num = frame->rax;
    uint64_t arg0 = frame->rdi;
    uint64_t arg1 = frame->rsi;
    uint64_t arg2 = frame->rdx;
    uint64_t arg3 = frame->r10;
    uint64_t arg4 = frame->r8;
    uint64_t arg5 = frame->r9;

    /* 获取当前进程的 ABI 类型 */
    struct process *proc = process_get_current();
    abi_type_t abi = proc ? proc->abi : ABI_ZRATRO;

    uint64_t ret;
    if (abi == ABI_LINUX) {
        ret = linux_syscall_handler(syscall_num, arg0, arg1, arg2, arg3, arg4, arg5);
    } else if (abi == ABI_WINDOWS) {
        ret = windows_syscall_handler(syscall_num, arg0, arg1, arg2, arg3, arg4, arg5, 0, 0);
    } else {
        ret = zratro_syscall_dispatch(syscall_num, arg0, arg1, arg2, arg3, arg4, arg5);
    }

    frame->rax = ret;
    
    /* 不再将线程设为 READY 并加入运行队列
     * 线程即将通过 iretq 返回用户态继续运行，应保持 RUNNING 状态
     * 如果线程需要睡眠（如 sys_sleep），thread_sleep 已经设置了 SLEEPING 状态
     * 调度由 timer interrupt → scheduler_tick → NEED_RESCHED → schedule() 处理 */
}

void syscall_init(void) {
    /* 初始化快速系统调用 (syscall/sysret) */
    extern void syscall_fast_init(void);
    syscall_fast_init();

    /* 注册传统系统调用处理程序 */
    isr_register_handler(SYSCALL_VECTOR, syscall_handler);
}

/* ========== 快速系统调用入口 (SYSCALL/SYSRET) ========== */

/* 从汇编调用 */
uint64_t syscall_dispatch(uint64_t num, uint64_t arg0, uint64_t arg1,
                          uint64_t arg2, uint64_t arg3, uint64_t arg4,
                          uint64_t arg5) {
    /* 更新统计 */
    struct cpu_local *cpu = this_cpu();
    if (cpu) {
        cpu->syscalls++;
    }

    /* 获取当前进程的 ABI 类型 */
    struct process *proc = process_get_current();
    abi_type_t abi = proc ? proc->abi : ABI_ZRATRO;

    if (abi == ABI_LINUX) {
        return linux_syscall_handler(num, arg0, arg1, arg2, arg3, arg4, arg5);
    } else if (abi == ABI_WINDOWS) {
        return windows_syscall_handler(num, arg0, arg1, arg2, arg3, arg4, arg5, 0, 0);
    }

    return zratro_syscall_dispatch(num, arg0, arg1, arg2, arg3, arg4, arg5);
}


