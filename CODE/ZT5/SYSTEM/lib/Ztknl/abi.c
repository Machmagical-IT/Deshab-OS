#include "abi.h"
#include "console.h"
#include "vmm.h"
#include "pmm.h"
#include "task.h"
#include "vfs.h"
#include "string.h"
#include "ipc.h"
#include "exec.h"
#include "ntdll.h"
#include "socket.h"

/* ABI 检测: 从 ELF 头判断 */
abi_type_t abi_detect_from_elf(const uint8_t *elf_data) {
    if (elf_data == NULL) {
        return ABI_UNKNOWN;
    }
    
    /* 检查 ELF 魔数 */
    if (elf_data[0] != 0x7F || elf_data[1] != 'E' ||
        elf_data[2] != 'L' || elf_data[3] != 'F') {
        return ABI_UNKNOWN;
    }
    
    /* 检查 OSABI 字段 */
    uint8_t osabi = elf_data[EI_OSABI];
    
    switch (osabi) {
        case ELFOSABI_LINUX:
            return ABI_LINUX;
        case ELFOSABI_FREEBSD:
            return ABI_FREEBSD;
        case ELFOSABI_NONE:
            /* 默认 ELF 可能是 Linux 或 Zratro
             * 检查 PT_INTERP 段来区分 */
            /* 简化: 如果没有特殊标记，假设是 Zratro 原生 */
            return ABI_ZRATRO;
        default:
            return ABI_ZRATRO;
    }
}

/* ABI 名称 */
const char *abi_get_name(abi_type_t abi) {
    switch (abi) {
        case ABI_ZRATRO:    return "Zratro";
        case ABI_LINUX:     return "Linux";
        case ABI_FREEBSD:   return "FreeBSD";
        case ABI_WINDOWS:   return "Windows";
        default:            return "Unknown";
    }
}

/* ABI 初始化 */
int abi_init(void) {
    return 0;
}

/* ========================================
 * Linux 系统调用实现
 * ======================================== */

/* Linux: read */
static uint64_t linux_read(int fd, void *buf, uint64_t count) {
    /* TODO: 使用进程文件描述符表 */
    (void)fd;
    (void)buf;
    (void)count;
    return -1;  /* ENOSYS */
}

/* Linux: write */
static uint64_t linux_write(int fd, const void *buf, uint64_t count) {
    /* stdout/stderr -> 控制台 (使用串口输出) */
    if (fd == 1 || fd == 2) {
        /* 安全检查: 使用 copy_from_user */
        char kbuf[256];
        uint64_t written = 0;
        while (written < count) {
            uint64_t chunk = count - written;
            if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
            if (copy_from_user(kbuf, (const void *)((const char *)buf + written), chunk) != 0) {
                break;
            }
            for (uint64_t i = 0; i < chunk; i++) {
                putchar(kbuf[i]);
                serial_putchar(kbuf[i]);
            }
            written += chunk;
        }
        return written;
    }
    return -1;
}

/* Linux: open */
static uint64_t linux_open(const char *path, int flags, int mode) {
    /* 转换 Linux flags 到 Zratro flags */
    int zt_flags = 0;
    
    switch (flags & 3) {
        case LINUX_O_RDONLY: zt_flags = 0; break;
        case LINUX_O_WRONLY: zt_flags = 1; break;
        case LINUX_O_RDWR:   zt_flags = 2; break;
    }
    
    (void)mode;
    
    struct vfs_file *file = vfs_open(path, zt_flags);
    if (file == NULL) {
        return (uint64_t)-1;  /* 返回负数表示错误 */
    }
    
    return (uint64_t)file;  /* 简化: 返回指针作为 fd */
}

/* Linux: close */
static uint64_t linux_close(int fd) {
    struct vfs_file *file = (struct vfs_file *)(uint64_t)fd;
    return vfs_close(file);
}

/* Linux: mmap - 关键实现 */
static uint64_t linux_mmap(uint64_t addr, uint64_t length, int prot,
                           int flags, int fd, uint64_t offset) {
    struct process *proc = process_get_current();
    if (proc == NULL || proc->space == NULL) {
        return (uint64_t)-1;
    }
    
    /* 匿名映射 */
    if (flags & LINUX_MAP_ANONYMOUS) {
        /* 对齐长度 */
        length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        
        /* 计算需要的页数 */
        size_t pages = length / PAGE_SIZE;
        if (pages == 0) pages = 1;
        
        /* 寻找空闲虚拟地址区域 */
        /* 简化: 如果 addr 为 0 或 MAP_FIXED, 使用固定区域 */
        uint64_t vaddr;
        if (addr == 0 || !(flags & LINUX_MAP_FIXED)) {
            /* 使用 mmap 区域 (0x7F0000000000 开始) */
            /* 使用原子操作保证 SMP 安全 */
            static volatile uint64_t mmap_base = 0x7F0000000000ULL;
            vaddr = __sync_fetch_and_add((volatile uint64_t *)&mmap_base, length + PAGE_SIZE);
        } else {
            vaddr = addr & ~(PAGE_SIZE - 1);
        }
        
        /* 转换保护标志 */
        uint64_t vmm_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (prot & LINUX_PROT_WRITE) {
            vmm_flags |= VMM_FLAG_WRITABLE;
        }
        if (!(prot & LINUX_PROT_EXEC)) {
            vmm_flags |= VMM_FLAG_NX;
        }
        
        /* 分配并映射页面 */
        size_t mapped = 0;
        for (size_t i = 0; i < pages; i++) {
            uint64_t phys = pmm_alloc_page();
            if (phys == 0) {
                /* 分配失败, 回滚已映射的页面 */
                goto rollback;
            }
            
            /* 清零 */
            void *page = phys_to_virt(phys);
            if (page != NULL) {
                for (size_t j = 0; j < PAGE_SIZE; j++) {
                    ((uint8_t *)page)[j] = 0;
                }
            }
            
            if (vmm_map_page(proc->space, vaddr + i * PAGE_SIZE, phys, vmm_flags) != 0) {
                pmm_free_page(phys);
                goto rollback;
            }
            mapped++;
        }
        
        return vaddr;
        
rollback:
        /* 回滚已映射的页面 */
        for (size_t i = 0; i < mapped; i++) {
            uint64_t phys = vmm_get_physical(proc->space, vaddr + i * PAGE_SIZE);
            if (phys != 0) {
                vmm_unmap_page(proc->space, vaddr + i * PAGE_SIZE);
                pmm_free_page(phys);
            }
        }
        return (uint64_t)-1;
    }
    
    /* 文件映射 (简化实现) */
    (void)fd;
    (void)offset;
    return (uint64_t)-1;
}

/* Linux: munmap */
static uint64_t linux_munmap(uint64_t addr, uint64_t length) {
    struct process *proc = process_get_current();
    if (proc == NULL || proc->space == NULL) {
        return (uint64_t)-1;
    }
    
    /* 对齐 */
    addr &= ~(PAGE_SIZE - 1);
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    size_t pages = length / PAGE_SIZE;
    
    for (size_t i = 0; i < pages; i++) {
        /* 获取物理地址 */
        uint64_t phys = vmm_get_physical(proc->space, addr + i * PAGE_SIZE);
        if (phys != 0) {
            /* 解除映射 */
            vmm_unmap_page(proc->space, addr + i * PAGE_SIZE);
            /* 释放物理页 */
            pmm_free_page(phys);
        }
    }
    
    return 0;
}

/* Linux: brk */
static uint64_t linux_brk(uint64_t addr) {
    struct process *proc = process_get_current();
    if (proc == NULL) {
        return 0;
    }
    
    /* 使用进程级的堆管理，确保进程隔离 */
    uint64_t heap_end = proc->brk_end;
    
    /* 首次调用时初始化堆 */
    if (heap_end == 0) {
        proc->brk_start = 0x100000;  /* 1MB 起始 */
        proc->brk_end = proc->brk_start;
        proc->brk_limit = 0x800000;  /* 8MB 默认限制 */
        heap_end = proc->brk_end;
    }
    
    if (addr == 0) {
        return heap_end;
    }
    
    /* 检查是否超出限制 */
    if (addr > proc->brk_limit) {
        addr = proc->brk_limit;
    }
    
    if (addr > heap_end) {
        /* 扩展堆 */
        uint64_t old_end = (heap_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t new_end = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        
        for (uint64_t page = old_end; page < new_end; page += PAGE_SIZE) {
            uint64_t phys = pmm_alloc_page();
            if (phys == 0) break;
            
            void *p = phys_to_virt(phys);
            for (size_t j = 0; j < PAGE_SIZE; j++) {
                ((uint8_t *)p)[j] = 0;
            }
            
            vmm_map_page(proc->space, page, phys,
                        VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE);
        }
        
        proc->brk_end = addr;
    } else if (addr < heap_end) {
        /* 收缩堆 - 简化实现：只更新指针，不释放内存 */
        proc->brk_end = addr;
    }
    
    return proc->brk_end;
}

/* Linux: getpid */
static uint64_t linux_getpid(void) {
    struct process *proc = process_get_current();
    return proc ? proc->pid : 0;
}

/* Linux: exit */
static uint64_t linux_exit(int status) {
    process_exit(status);
    return 0;  /* 不会执行到这里 */
}

/* Linux: sched_yield */
static uint64_t linux_sched_yield(void) {
    /* 触发调度 */
    return 0;
}

/* Linux 系统调用分发器 */
uint64_t linux_syscall_handler(uint64_t num, uint64_t arg0, uint64_t arg1,
                               uint64_t arg2, uint64_t arg3, uint64_t arg4,
                               uint64_t arg5) {
    switch (num) {
        case LINUX_SYS_READ:
            return linux_read((int)arg0, (void *)arg1, arg2);
            
        case LINUX_SYS_WRITE:
            return linux_write((int)arg0, (const void *)arg1, arg2);
            
        case LINUX_SYS_OPEN:
            return linux_open((const char *)arg0, (int)arg1, (int)arg2);
            
        case LINUX_SYS_CLOSE:
            return linux_close((int)arg0);
            
        case LINUX_SYS_MMAP:
            return linux_mmap(arg0, arg1, (int)arg2, (int)arg3, (int)arg4, arg5);
            
        case LINUX_SYS_MUNMAP:
            return linux_munmap(arg0, arg1);
            
        case LINUX_SYS_BRK:
            return linux_brk(arg0);
            
        case LINUX_SYS_GETPID:
            return linux_getpid();
            
        case LINUX_SYS_EXIT:
            return linux_exit((int)arg0);
            
        case LINUX_SYS_FORK:
            /* Linux fork 使用 Zratro 的实现 */
            {
                struct process *parent = process_get_current();
                if (parent == NULL) return (uint64_t)-1;
                struct process *child = process_fork(parent);
                if (child == NULL) return (uint64_t)-1;
                /* 复制文件描述符表 */
                if (parent->fd_table != NULL) {
                    child->fd_table = fd_table_dup(parent->fd_table);
                }
                /* 子进程加入调度 */
                extern void scheduler_add_thread(struct thread *);
                struct thread *t = child->threads;
                while (t) { scheduler_add_thread(t); t = t->process_next; }
                return child->pid;
            }
            
        case LINUX_SYS_PIPE:
            return pipe_create((int *)arg0);
            
        case LINUX_SYS_DUP:
            {
                struct process *proc = process_get_current();
                if (proc == NULL || proc->fd_table == NULL) return (uint64_t)-1;
                return fd_dup(proc->fd_table, (int)arg0);
            }
            
        case LINUX_SYS_DUP2:
            {
                struct process *proc = process_get_current();
                if (proc == NULL || proc->fd_table == NULL) return (uint64_t)-1;
                return fd_dup2(proc->fd_table, (int)arg0, (int)arg1);
            }
            
        case LINUX_SYS_WAIT4:
            return sys_waitpid((pid_t)arg0, (int *)arg1, (int)arg2);
            
        case LINUX_SYS_EXECVE:
            return sys_execve((const char *)arg0, (char *const *)arg1, (char *const *)arg2);
            
        case LINUX_SYS_SCHED_YIELD:
            return linux_sched_yield();
            
        /* Socket 相关 */
        case LINUX_SYS_SOCKET:
            return sys_socket((int)arg0, (int)arg1, (int)arg2);
            
        case LINUX_SYS_BIND:
            return sys_bind((int)arg0, (const void *)arg1, (socklen_t)arg2);
            
        case LINUX_SYS_LISTEN:
            return sys_listen((int)arg0, (int)arg1);
            
        case LINUX_SYS_ACCEPT:
            return sys_accept((int)arg0, (void *)arg1, (socklen_t *)arg2);
            
        case LINUX_SYS_CONNECT:
            return sys_connect((int)arg0, (const void *)arg1, (socklen_t)arg2);
            
        case LINUX_SYS_SENDTO:
            return sys_sendto((int)arg0, (const void *)arg1, arg2, (int)arg3,
                              (const void *)arg4, (socklen_t)arg5);
            
        case LINUX_SYS_RECVFROM:
            return sys_recvfrom((int)arg0, (void *)arg1, arg2, (int)arg3,
                                (void *)arg4, (socklen_t *)arg5);
            
        case LINUX_SYS_SETSOCKOPT:
            return sys_setsockopt((int)arg0, (int)arg1, (int)arg2,
                                   (const void *)arg3, (socklen_t)arg4);
            
        case LINUX_SYS_GETSOCKOPT:
            return sys_getsockopt((int)arg0, (int)arg1, (int)arg2,
                                   (void *)arg3, (socklen_t *)arg4);
            
        case LINUX_SYS_GETSOCKNAME:
            return sys_getsockname((int)arg0, (void *)arg1, (socklen_t *)arg2);
            
        case LINUX_SYS_GETPEERNAME:
            return sys_getpeername((int)arg0, (void *)arg1, (socklen_t *)arg2);
            
        case LINUX_SYS_SOCKETPAIR:
            return sys_socketpair((int)arg0, (int)arg1, (int)arg2, (int *)arg3);
            
        /* ioctl */
        case LINUX_SYS_IOCTL:
            /* 简化: 返回成功 */
            return 0;
            
        /* fcntl */
        case LINUX_SYS_FCNTL:
            /* 简化: 返回成功 */
            return 0;
            
        /* Signal 相关 */
        case LINUX_SYS_RT_SIGACTION:
        case LINUX_SYS_RT_SIGPROCMASK:
            /* 简化: 返回成功 */
            return 0;
            
        /* uname */
        case LINUX_SYS_UNAME:
            {
                struct {
                    char sysname[65];
                    char nodename[65];
                    char release[65];
                    char version[65];
                    char machine[65];
                } *uts = (void *)arg0;
                if (uts) {
                    const char *s;
                    s = "Linux"; for (int i = 0; s[i]; i++) uts->sysname[i] = s[i];
                    s = "Zratro"; for (int i = 0; s[i]; i++) uts->nodename[i] = s[i];
                    s = "5.0.0"; for (int i = 0; s[i]; i++) uts->release[i] = s[i];
                    s = "Zratro Linux Compat"; for (int i = 0; s[i]; i++) uts->version[i] = s[i];
                    s = "x86_64"; for (int i = 0; s[i]; i++) uts->machine[i] = s[i];
                }
                return 0;
            }
            
        /* lseek */
        case LINUX_SYS_LSEEK:
            {
                struct vfs_file *file = (struct vfs_file *)arg0;
                if (file == NULL) return (uint64_t)-9;  /* EBADF */
                int64_t offset = (int64_t)arg1;
                /* whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END - 简化实现 */
                (void)arg2;  /* whence */
                return vfs_seek(file, offset);
            }
            
        /* access */
        case LINUX_SYS_ACCESS:
            {
                const char *path = (const char *)arg0;
                (void)arg1;  /* mode, 简化忽略 */
                struct vfs_stat st;
                return vfs_stat(path, &st);
            }
            
        /* getpid 相关 */
        case LINUX_SYS_GETPPID:
            {
                struct process *proc = process_get_current();
                if (proc == NULL) return 0;
                return proc->parent_pid;
            }
            
        case LINUX_SYS_GETUID:
        case LINUX_SYS_GETEUID:
            return 0;  /* root */
            
        case LINUX_SYS_GETGID:
        case LINUX_SYS_GETEGID:
            return 0;  /* root */
            
        case LINUX_SYS_SETUID:
        case LINUX_SYS_SETGID:
            return 0;  /* 成功 */
            
        case LINUX_SYS_GETTID:
            {
                struct thread *t = thread_get_current();
                return t ? t->tid : 0;
            }
            
        /* 时间相关 */
        case LINUX_SYS_GETTIMEOFDAY:
            {
                struct {
                    int64_t tv_sec;
                    int64_t tv_usec;
                } *tv = (void *)arg0;
                if (tv) {
                    /* 从 TSC 获取时间 */
                    uint64_t tsc;
                    asm volatile("rdtsc" : "=A"(tsc));
                    tv->tv_sec = tsc / 1000000000ULL;
                    tv->tv_usec = (tsc % 1000000000ULL) / 1000;
                }
                return 0;
            }
            
        case LINUX_SYS_CLOCK_GETTIME:
            {
                int clk_id = (int)arg0;
                struct {
                    int64_t tv_sec;
                    int64_t tv_nsec;
                } *tp = (void *)arg1;
                if (tp && clk_id == 0) {  /* CLOCK_REALTIME */
                    uint64_t tsc;
                    asm volatile("rdtsc" : "=A"(tsc));
                    tp->tv_sec = tsc / 1000000000ULL;
                    tp->tv_nsec = tsc % 1000000000ULL;
                }
                return 0;
            }
            
        case LINUX_SYS_NANOSLEEP:
            {
                const struct {
                    int64_t tv_sec;
                    int64_t tv_nsec;
                } *req = (void *)arg0;
                if (req) {
                    /* 简化: 转换为 tick */
                    uint64_t ticks = req->tv_sec * 1000 + req->tv_nsec / 1000000;
                    if (ticks > 0) thread_sleep(ticks);
                }
                return 0;
            }
            
        /* 文件操作 */
        case LINUX_SYS_MKDIR:
            {
                const char *path = (const char *)arg0;
                (void)arg1;  /* mode */
                return vfs_mkdir(path);
            }
            
        case LINUX_SYS_RMDIR:
            {
                const char *path = (const char *)arg0;
                return vfs_rmdir(path);
            }
            
        case LINUX_SYS_UNLINK:
            {
                const char *path = (const char *)arg0;
                return vfs_unlink(path);
            }
            
        case LINUX_SYS_RENAME:
            {
                const char *oldpath = (const char *)arg0;
                const char *newpath = (const char *)arg1;
                return vfs_rename(oldpath, newpath);
            }
            
        case LINUX_SYS_CHMOD:
        case LINUX_SYS_FCHMOD:
        case LINUX_SYS_CHOWN:
        case LINUX_SYS_FCHOWN:
        case LINUX_SYS_LCHOWN:
            return 0;  /* 简化: 成功 */
            
        case LINUX_SYS_UMASK:
            return 022;  /* 返回默认 umask */
            
        case LINUX_SYS_GETCWD:
            {
                char *buf = (char *)arg0;
                size_t size = arg1;
                if (buf && size > 0) {
                    const char *cwd = "/";
                    size_t len = 0;
                    while (cwd[len]) len++;
                    if (len < size) {
                        for (size_t i = 0; i <= len; i++) buf[i] = cwd[i];
                        return len;
                    }
                }
                return (uint64_t)-1;
            }
            
        case LINUX_SYS_CHDIR:
        case LINUX_SYS_FCHDIR:
            return 0;  /* 简化: 成功 */
            
        /* stat 系列 */
        case LINUX_SYS_STAT:
        case LINUX_SYS_LSTAT:
            return vfs_stat((const char *)arg0, (struct vfs_stat *)arg1);
            
        case LINUX_SYS_FSTAT:
            {
                struct vfs_file *file = (struct vfs_file *)arg0;
                if (file == NULL) return (uint64_t)-9;  /* EBADF */
                return vfs_fstat(file, (struct vfs_stat *)arg1);
            }
            
        /* 信号相关 */
        case LINUX_SYS_TGKILL:
        case LINUX_SYS_TKILL:
        case LINUX_SYS_KILL:
            return 0;  /* 简化: 成功 */
            
        /* prlimit */
        case LINUX_SYS_GETRLIMIT:
        case LINUX_SYS_SETRLIMIT:
        case LINUX_SYS_PRLIMIT64:
            return 0;  /* 简化: 成功 */
            
        /* arch_prctl */
        case LINUX_SYS_ARCH_PRCTL:
            {
                int code = (int)arg0;
                uint64_t *addr = (uint64_t *)arg1;
                if (code == 0x1001) {  /* ARCH_GET_FS */
                    uint64_t fs;
                    asm volatile("mov %%fs:0, %0" : "=r"(fs));
                    if (addr) *addr = fs;
                } else if (code == 0x1002) {  /* ARCH_GET_GS */
                    uint64_t gs;
                    asm volatile("mov %%gs:0, %0" : "=r"(gs));
                    if (addr) *addr = gs;
                }
                return 0;
            }
            
        /* set_tid_address */
        case LINUX_SYS_SET_TID_ADDRESS:
            {
                struct thread *t = thread_get_current();
                if (t) {
                    t->clear_child_tid = (int *)arg0;
                    return t->tid;
                }
                return (uint64_t)-1;
            }
            
        /* futex */
        case LINUX_SYS_FUTEX:
            {
                int *uaddr = (int *)arg0;
                int op = (int)arg1;
                (void)uaddr;
                if (op == 0) {  /* FUTEX_WAIT */
                    thread_sleep(1);  /* 简化: 短暂休眠 */
                }
                return 0;
            }
            
        /* getdents */
        case LINUX_SYS_GETDENTS:
        case LINUX_SYS_GETDENTS64:
            {
                int fd = (int)arg0;
                void *dirp = (void *)arg1;
                unsigned int count = (unsigned int)arg2;
                struct vfs_file *file = (struct vfs_file *)(uint64_t)fd;
                return vfs_getdents(file, dirp, count);
            }
            
        /* getrandom */
        case LINUX_SYS_GETRANDOM:
            {
                void *buf = (void *)arg0;
                size_t len = arg1;
                unsigned int flags = (unsigned int)arg2;
                (void)flags;
                /* 使用 TSC 作为随机源 */
                uint8_t *p = (uint8_t *)buf;
                for (size_t i = 0; i < len; i++) {
                    uint64_t tsc;
                    asm volatile("rdtsc" : "=A"(tsc));
                    p[i] = (uint8_t)(tsc >> (i % 8));
                }
                return len;
            }
            
        /* readlink */
        case LINUX_SYS_READLINK:
            {
                const char *path = (const char *)arg0;
                char *buf = (char *)arg1;
                size_t bufsize = arg2;
                (void)path;
                if (buf && bufsize > 0) {
                    buf[0] = '\0';  /* 简化: 返回空字符串 */
                }
                return 0;
            }
            
        /* splice/tee/vsplice */
        case LINUX_SYS_SPLICE:
        case LINUX_SYS_TEE:
        case LINUX_SYS_VSPLICE:
            return 0;  /* 简化 */
            
        /* memfd_create */
        case LINUX_SYS_MEMFD_CREATE:
            return (uint64_t)-38;  /* ENOSYS */
            
        /* eventfd/timerfd/signalfd */
        case LINUX_SYS_EVENTFD2:
        case LINUX_SYS_TIMERFD_CREATE:
        case LINUX_SYS_TIMERFD_SETTIME:
        case LINUX_SYS_TIMERFD_GETTIME:
        case LINUX_SYS_SIGNALFD4:
            return 0;  /* 简化: 返回假成功 */
            
        /* epoll */
        case LINUX_SYS_EPOLL_CREATE:
        case LINUX_SYS_EPOLL_CTL:
        case LINUX_SYS_EPOLL_WAIT:
            return 0;  /* 简化 */
            
        /* pipe2 */
        case LINUX_SYS_PIPE2:
            return pipe_create((int *)arg0);
            
        default:
            /* Fallback: 尝试 Zratro 原生系统调用 */
            {
                extern uint64_t zratro_syscall_dispatch(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
                uint64_t ret = zratro_syscall_dispatch(num, arg0, arg1, arg2, arg3, arg4, arg5);
                if (ret != (uint64_t)-1) return ret;
            }
            print("[ABI] Unimplemented Linux syscall: ");
            print_hex(num);
            println("");
            return (uint64_t)-38;  /* ENOSYS */
    }
}
