#ifndef IPC_H
#define IPC_H

#include <stdint.h>
#include <stddef.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int64_t ssize_t;
#endif

#include "task.h"

/* 文件描述符类型 */
typedef enum {
    FD_TYPE_NONE = 0,
    FD_TYPE_FILE,      /* 普通文件 */
    FD_TYPE_PIPE,      /* 管道 */
    FD_TYPE_SOCKET,    /* 套接字 */
    FD_TYPE_TTY,       /* 终端 */
    FD_TYPE_DIR,       /* 目录 */
} fd_type_t;

/* 管道结构 */
struct pipe {
    uint8_t *buffer;        /* 环形缓冲区 */
    size_t size;            /* 缓冲区大小 */
    size_t read_pos;        /* 读位置 */
    size_t write_pos;       /* 写位置 */
    size_t count;           /* 当前数据量 */
    
    int read_fd;            /* 读端 fd */
    int write_fd;           /* 写端 fd */
    
    uint32_t readers;       /* 读进程数 */
    uint32_t writers;       /* 写进程数 */
    
    struct thread *read_wait;  /* 等待读的线程 */
    struct thread *write_wait; /* 等待写的线程 */
    
    volatile int lock;      /* 自旋锁保护并发访问 */
    int flags;              /* 管道标志 (O_NONBLOCK 等) */
};

/* 文件描述符表项 */
struct fd_entry {
    fd_type_t type;
    int flags;              /* O_RDONLY, O_WRONLY, O_RDWR, O_CLOEXEC... */
    
    union {
        void *file;         /* vfs_file 指针 */
        struct pipe *pipe;  /* 管道指针 */
        void *socket;       /* 套接字指针 */
        void *dir;          /* 目录指针 */
    };
    
    uint64_t offset;        /* 文件偏移 */
    int ref_count;          /* 引用计数 */
};

/* 进程文件描述符表 */
#define MAX_FDS_PER_PROCESS  256

struct fd_table {
    struct fd_entry entries[MAX_FDS_PER_PROCESS];
    int next_fd;            /* 下一个可用 fd */
};

/* 初始化 IPC 子系统 */
void ipc_init(void);

/* === 文件描述符表管理 === */

/* 创建文件描述符表 */
struct fd_table *fd_table_create(void);

/* 销毁文件描述符表 */
void fd_table_destroy(struct fd_table *table);

/* 复制文件描述符表 (fork用) */
struct fd_table *fd_table_dup(struct fd_table *src);

/* 分配文件描述符 */
int fd_alloc(struct fd_table *table, fd_type_t type, void *obj, int flags);

/* 释放文件描述符 */
int fd_free(struct fd_table *table, int fd);

/* 获取文件描述符 */
struct fd_entry *fd_get(struct fd_table *table, int fd);

/* 复制文件描述符 (dup) */
int fd_dup(struct fd_table *table, int oldfd);

/* 复制到指定文件描述符 (dup2) */
int fd_dup2(struct fd_table *table, int oldfd, int newfd);

/* === 管道操作 === */

/* 创建管道 */
int pipe_create(int pipefd[2]);

/* 管道读 */
ssize_t pipe_read(struct pipe *pipe, void *buf, size_t count);

/* 管道写 */
ssize_t pipe_write(struct pipe *pipe, const void *buf, size_t count);

/* 关闭管道端 */
int pipe_close(struct pipe *pipe, int is_write);

/* === 进程等待 === */

/* 等待进程状态改变 */
pid_t sys_waitpid(pid_t pid, int *status, int options);

/* 等待任意子进程 */
pid_t sys_wait_impl(int *status);

/* 唤醒等待的父进程 */
void wakeup_parent(pid_t child_pid, int status);

/* === 获取进程的 fd 表 === */
struct fd_table *process_get_fd_table(struct process *proc);

#endif
