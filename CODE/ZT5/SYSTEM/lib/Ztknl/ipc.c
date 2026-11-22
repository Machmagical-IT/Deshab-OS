#include "ipc.h"
#include "console.h"
#include "heap.h"
#include "vfs.h"
#include "string.h"
#include "scheduler.h"

/* 默认管道缓冲区大小 */
#define PIPE_BUF_SIZE   4096

/* 初始化 IPC 子系统 */
void ipc_init(void) {
    /* IPC 子系统已就绪 */
}

/* === 文件描述符表管理 === */

struct fd_table *fd_table_create(void) {
    struct fd_table *table = (struct fd_table *)kmalloc(sizeof(struct fd_table));
    if (table == NULL) {
        return NULL;
    }
    
    /* 清空所有条目 */
    for (int i = 0; i < MAX_FDS_PER_PROCESS; i++) {
        table->entries[i].type = FD_TYPE_NONE;
        table->entries[i].file = NULL;
        table->entries[i].offset = 0;
        table->entries[i].ref_count = 0;
        table->entries[i].flags = 0;
    }
    
    /* 初始化 stdin (fd 0), stdout (fd 1), stderr (fd 2) 为 TTY 类型 */
    table->entries[0].type = FD_TYPE_TTY;
    table->entries[0].file = NULL;
    table->entries[0].ref_count = 1;
    table->entries[0].flags = 1;  /* 读 */
    
    table->entries[1].type = FD_TYPE_TTY;
    table->entries[1].file = NULL;
    table->entries[1].ref_count = 1;
    table->entries[1].flags = 2;  /* 写 */
    
    table->entries[2].type = FD_TYPE_TTY;
    table->entries[2].file = NULL;
    table->entries[2].ref_count = 1;
    table->entries[2].flags = 2;  /* 写 */
    
    table->next_fd = 3;  /* 0=stdin, 1=stdout, 2=stderr 已占用 */
    
    return table;
}

void fd_table_destroy(struct fd_table *table) {
    if (table == NULL) {
        return;
    }
    
    /* 关闭所有文件描述符 */
    for (int i = 0; i < MAX_FDS_PER_PROCESS; i++) {
        if (table->entries[i].type != FD_TYPE_NONE) {
            fd_free(table, i);
        }
    }
    
    kfree(table);
}

struct fd_table *fd_table_dup(struct fd_table *src) {
    if (src == NULL) {
        return NULL;
    }
    
    struct fd_table *dst = fd_table_create();
    if (dst == NULL) {
        return NULL;
    }
    
    /* 复制所有条目 */
    for (int i = 0; i < MAX_FDS_PER_PROCESS; i++) {
        if (src->entries[i].type != FD_TYPE_NONE) {
            dst->entries[i] = src->entries[i];
            dst->entries[i].ref_count++;

            /* 如果是文件，增加 vfs_file 的引用计数 */
            if (src->entries[i].type == FD_TYPE_FILE && src->entries[i].file != NULL) {
                struct vfs_file *vfile = (struct vfs_file *)src->entries[i].file;
                vfile->ref_count++;
            }

            /* 如果是管道，增加引用计数 */
            if (src->entries[i].type == FD_TYPE_PIPE) {
                struct pipe *pipe = src->entries[i].pipe;
                if (src->entries[i].flags & 1) {  /* 读端 */
                    pipe->readers++;
                } else {  /* 写端 */
                    pipe->writers++;
                }
            }
        }
    }
    
    dst->next_fd = src->next_fd;
    
    return dst;
}

int fd_alloc(struct fd_table *table, fd_type_t type, void *obj, int flags) {
    if (table == NULL || obj == NULL) {
        return -1;
    }
    
    /* 查找空闲 fd */
    int fd = -1;
    for (int i = 0; i < MAX_FDS_PER_PROCESS; i++) {
        int candidate = (table->next_fd + i) % MAX_FDS_PER_PROCESS;
        if (table->entries[candidate].type == FD_TYPE_NONE) {
            fd = candidate;
            break;
        }
    }
    
    if (fd < 0) {
        return -1;  /* 表已满 */
    }
    
    table->entries[fd].type = type;
    table->entries[fd].file = obj;
    table->entries[fd].offset = 0;
    table->entries[fd].ref_count = 1;
    table->entries[fd].flags = flags;
    
    table->next_fd = fd + 1;
    if (table->next_fd >= MAX_FDS_PER_PROCESS) {
        table->next_fd = 0;
    }
    
    return fd;
}

int fd_free(struct fd_table *table, int fd) {
    if (table == NULL || fd < 0 || fd >= MAX_FDS_PER_PROCESS) {
        return -1;
    }
    
    struct fd_entry *entry = &table->entries[fd];
    if (entry->type == FD_TYPE_NONE) {
        return -1;
    }
    
    entry->ref_count--;
    
    if (entry->ref_count <= 0) {
        /* 根据类型释放资源 */
        switch (entry->type) {
            case FD_TYPE_FILE:
                if (entry->file != NULL) {
                    vfs_close((struct vfs_file *)entry->file);
                }
                break;
                
            case FD_TYPE_PIPE:
                if (entry->pipe != NULL) {
                    pipe_close(entry->pipe, (entry->flags & 2) != 0);
                }
                break;
                
            default:
                break;
        }
        
        entry->type = FD_TYPE_NONE;
        entry->file = NULL;
        entry->offset = 0;
        entry->flags = 0;
    }
    
    return 0;
}

struct fd_entry *fd_get(struct fd_table *table, int fd) {
    if (table == NULL || fd < 0 || fd >= MAX_FDS_PER_PROCESS) {
        return NULL;
    }
    
    if (table->entries[fd].type == FD_TYPE_NONE) {
        return NULL;
    }
    
    return &table->entries[fd];
}

int fd_dup(struct fd_table *table, int oldfd) {
    struct fd_entry *entry = fd_get(table, oldfd);
    if (entry == NULL) {
        return -1;
    }

    /* 分配新 fd */
    int newfd = fd_alloc(table, entry->type, entry->file, entry->flags);
    if (newfd < 0) {
        return -1;
    }

    /* 复制偏移 */
    table->entries[newfd].offset = entry->offset;

    /* 增加引用计数 */
    entry->ref_count++;

    /* 如果是文件，增加 vfs_file 的引用计数 */
    if (entry->type == FD_TYPE_FILE && entry->file != NULL) {
        struct vfs_file *vfile = (struct vfs_file *)entry->file;
        vfile->ref_count++;
    }

    /* 如果是管道 */
    if (entry->type == FD_TYPE_PIPE) {
        if (entry->flags & 1) {
            entry->pipe->readers++;
        } else {
            entry->pipe->writers++;
        }
    }

    return newfd;
}

int fd_dup2(struct fd_table *table, int oldfd, int newfd) {
    struct fd_entry *entry = fd_get(table, oldfd);
    if (entry == NULL) {
        return -1;
    }

    if (newfd < 0 || newfd >= MAX_FDS_PER_PROCESS) {
        return -1;
    }

    /* 如果 newfd 已打开，先关闭 */
    if (table->entries[newfd].type != FD_TYPE_NONE) {
        fd_free(table, newfd);
    }

    /* 复制 */
    table->entries[newfd] = *entry;
    table->entries[newfd].ref_count = 1;
    table->entries[newfd].offset = entry->offset;

    /* 增加原条目引用计数 */
    entry->ref_count++;

    /* 如果是文件，增加 vfs_file 的引用计数 */
    if (entry->type == FD_TYPE_FILE && entry->file != NULL) {
        struct vfs_file *vfile = (struct vfs_file *)entry->file;
        vfile->ref_count++;
    }

    /* 如果是管道 */
    if (entry->type == FD_TYPE_PIPE) {
        if (entry->flags & 1) {
            entry->pipe->readers++;
        } else {
            entry->pipe->writers++;
        }
    }

    return newfd;
}

/* === 管道操作 === */

int pipe_create(int pipefd[2]) {
    /* 分配管道结构 */
    struct pipe *pipe = (struct pipe *)kmalloc(sizeof(struct pipe));
    if (pipe == NULL) {
        return -1;
    }
    
    /* 分配缓冲区 */
    pipe->buffer = (uint8_t *)kmalloc(PIPE_BUF_SIZE);
    if (pipe->buffer == NULL) {
        kfree(pipe);
        return -1;
    }
    
    pipe->size = PIPE_BUF_SIZE;
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->count = 0;
    pipe->readers = 1;
    pipe->writers = 1;
    pipe->read_wait = NULL;
    pipe->write_wait = NULL;
    pipe->lock = 0;         /* 初始化自旋锁 */
    pipe->flags = 0;        /* 初始化标志 */
    
    /* 获取当前进程的 fd 表 */
    struct process *proc = process_get_current();
    if (proc == NULL) {
        kfree(pipe->buffer);
        kfree(pipe);
        return -1;
    }
    
    struct fd_table *table = process_get_fd_table(proc);
    if (table == NULL) {
        table = fd_table_create();
        if (table == NULL) {
            kfree(pipe->buffer);
            kfree(pipe);
            return -1;
        }
        /* TODO: 将 table 关联到进程 */
    }
    
    /* 分配读端 fd (flags: 1=只读) */
    pipefd[0] = fd_alloc(table, FD_TYPE_PIPE, pipe, 1);
    if (pipefd[0] < 0) {
        kfree(pipe->buffer);
        kfree(pipe);
        return -1;
    }
    pipe->read_fd = pipefd[0];
    
    /* 分配写端 fd (flags: 2=只写) */
    pipefd[1] = fd_alloc(table, FD_TYPE_PIPE, pipe, 2);
    if (pipefd[1] < 0) {
        fd_free(table, pipefd[0]);
        kfree(pipe->buffer);
        kfree(pipe);
        return -1;
    }
    pipe->write_fd = pipefd[1];
    
    return 0;
}

ssize_t pipe_read(struct pipe *pipe, void *buf, size_t count) {
    if (pipe == NULL || buf == NULL) {
        return -1;
    }
    
    /* 获取锁 */
    while (__sync_lock_test_and_set(&pipe->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 如果缓冲区为空且没有写者 */
    if (pipe->count == 0 && pipe->writers == 0) {
        __sync_lock_release(&pipe->lock);
        return 0;  /* EOF */
    }
    
    /* 等待数据 (简化: 返回 0) */
    if (pipe->count == 0) {
        __sync_lock_release(&pipe->lock);
        return 0;
    }
    
    /* 读取数据 */
    size_t to_read = count < pipe->count ? count : pipe->count;
    uint8_t *dst = (uint8_t *)buf;
    
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = pipe->buffer[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1) % pipe->size;
    }
    
    pipe->count -= to_read;
    
    __sync_lock_release(&pipe->lock);
    return to_read;
}

ssize_t pipe_write(struct pipe *pipe, const void *buf, size_t count) {
    if (pipe == NULL || buf == NULL) {
        return -1;
    }
    
    /* 获取锁 */
    while (__sync_lock_test_and_set(&pipe->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 如果没有读者 */
    if (pipe->readers == 0) {
        __sync_lock_release(&pipe->lock);
        return -1;  /* EPIPE */
    }
    
    /* 检查缓冲区空间 */
    size_t available = pipe->size - pipe->count;
    if (available == 0) {
        __sync_lock_release(&pipe->lock);
        return 0;  /* 阻塞 */
    }
    
    /* 写入数据 */
    size_t to_write = count < available ? count : available;
    const uint8_t *src = (const uint8_t *)buf;
    
    for (size_t i = 0; i < to_write; i++) {
        pipe->buffer[pipe->write_pos] = src[i];
        pipe->write_pos = (pipe->write_pos + 1) % pipe->size;
    }
    
    pipe->count += to_write;
    
    __sync_lock_release(&pipe->lock);
    return to_write;
}

int pipe_close(struct pipe *pipe, int is_write) {
    if (pipe == NULL) {
        return -1;
    }
    
    /* 获取锁 */
    while (__sync_lock_test_and_set(&pipe->lock, 1)) {
        asm volatile("pause");
    }
    
    if (is_write) {
        pipe->writers--;
    } else {
        pipe->readers--;
    }
    
    /* 如果两端都关闭，释放资源 */
    if (pipe->readers == 0 && pipe->writers == 0) {
        __sync_lock_release(&pipe->lock);
        if (pipe->buffer != NULL) {
            kfree(pipe->buffer);
        }
        kfree(pipe);
    } else {
        __sync_lock_release(&pipe->lock);
    }
    
    return 0;
}

/* === 进程等待 === */

pid_t sys_waitpid(pid_t pid, int *status, int options) {
    struct process *parent = process_get_current();
    if (parent == NULL) {
        return -1;
    }

    /* 非阻塞模式: 只检查一次 */
    if (options & 1) {  /* WNOHANG */
        struct process *child = NULL;
        if (pid == -1) {
            child = process_find_zombie_child(parent->pid);
        } else if (pid > 0) {
            child = process_get_by_pid(pid);
            if (child == NULL || child->parent_pid != parent->pid) {
                return -1;
            }
            if (child->state != PROCESS_STATE_ZOMBIE) {
                return 0;
            }
        }
        if (child == NULL) return 0;
        if (status != NULL) *status = child->exit_code;
        pid_t cpid = child->pid;
        process_destroy(child);
        return cpid;
    }

    /* 阻塞模式: 循环等待子进程退出, 期间 yield CPU */
    for (int iter = 0;; iter++) {
        struct process *child = NULL;
        if (pid == -1) {
            child = process_find_zombie_child(parent->pid);
        } else if (pid > 0) {
            child = process_get_by_pid(pid);
            if (child == NULL || child->parent_pid != parent->pid) {
                return -1;
            }
            if (child->state != PROCESS_STATE_ZOMBIE) {
                child = NULL;
            }
        } else {
            return -1;
        }

        if (child != NULL) {
            if (status != NULL) *status = child->exit_code;
            pid_t cpid = child->pid;
            process_destroy(child);
            return cpid;
        }

        /* 检查是否还存在子进程 */
        if (pid == -1) {
            /* 首次检查: 确认至少有一个子进程, 否则返回 -1 */
            if (iter == 0) {
                struct process *c = process_find_zombie_child(parent->pid);
                (void)c;
                /* 找任意非僵尸子进程: 用 process_get_children 接口 */
                uint32_t children[32];
                uint32_t child_count = 0;
                if (process_get_children(parent->pid, children, &child_count) == 0) {
                    int alive = 0;
                    for (uint32_t ci = 0; ci < child_count && ci < 32; ci++) {
                        struct process *pc = process_get_by_pid(children[ci]);
                        if (pc && pc->state != PROCESS_STATE_ZOMBIE &&
                            pc->state != PROCESS_STATE_UNUSED) {
                            alive = 1;
                            break;
                        }
                    }
                    if (!alive) {
                        return -1;  /* 没有在运行的子进程 */
                    }
                }
            }
        }

        /* yield CPU 给其他线程 (包括子进程) 运行 */
        yield();
    }
}

pid_t sys_wait_impl(int *status) {
    return sys_waitpid(-1, status, 0);
}

void wakeup_parent(pid_t child_pid, int status) {
    (void)child_pid;
    (void)status;
    /* TODO: 唤醒等待该子进程的父进程 */
}

/* 获取进程的 fd 表 (简化实现) */
struct fd_table *process_get_fd_table(struct process *proc) {
    (void)proc;
    /* TODO: 在进程结构中添加 fd_table 字段 */
    return NULL;
}

/* ========== 高性能 IPC 优化 ========== */

/* === 快速消息队列 === */

#define MSG_QUEUE_SIZE      64
#define MSG_MAX_SIZE        4096

struct msg_header {
    uint32_t type;
    uint32_t size;
    uint32_t sender;
    uint32_t flags;
};

struct msg_queue {
    struct msg_header headers[MSG_QUEUE_SIZE];
    uint8_t data[MSG_QUEUE_SIZE][MSG_MAX_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t count;
    volatile int lock;
};

/* 全局消息队列表 */
#define MAX_MSG_QUEUES  16
static struct msg_queue msg_queues[MAX_MSG_QUEUES];
static int msg_queue_used[MAX_MSG_QUEUES];

/* 初始化消息队列 */
void msg_queue_init(void) {
    for (int i = 0; i < MAX_MSG_QUEUES; i++) {
        msg_queues[i].head = 0;
        msg_queues[i].tail = 0;
        msg_queues[i].count = 0;
        msg_queues[i].lock = 0;
        msg_queue_used[i] = 0;
    }
}

/* 创建消息队列 */
int msg_queue_create(void) {
    for (int i = 0; i < MAX_MSG_QUEUES; i++) {
        if (!msg_queue_used[i]) {
            msg_queue_used[i] = 1;
            msg_queues[i].head = 0;
            msg_queues[i].tail = 0;
            msg_queues[i].count = 0;
            return i;
        }
    }
    return -1;
}

/* 发送消息 (无锁快速路径) */
int msg_queue_send(int qid, uint32_t type, const void *data, uint32_t size) {
    if (qid < 0 || qid >= MAX_MSG_QUEUES) return -1;
    if (size > MSG_MAX_SIZE) return -1;
    
    struct msg_queue *q = &msg_queues[qid];
    
    /* 检查队列是否已满 */
    if (q->count >= MSG_QUEUE_SIZE) {
        return -1;  /* 队列已满 */
    }
    
    /* 获取锁 */
    while (__sync_lock_test_and_set(&q->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 写入消息头 */
    uint32_t idx = q->tail;
    q->headers[idx].type = type;
    q->headers[idx].size = size;
    q->headers[idx].sender = 0;  /* 当前进程 PID */
    q->headers[idx].flags = 0;
    
    /* 复制数据 */
    if (data != NULL && size > 0) {
        for (uint32_t i = 0; i < size; i++) {
            q->data[idx][i] = ((const uint8_t *)data)[i];
        }
    }
    
    /* 更新尾指针 */
    q->tail = (q->tail + 1) % MSG_QUEUE_SIZE;
    q->count++;
    
    __sync_lock_release(&q->lock);
    
    return 0;
}

/* 接收消息 */
int msg_queue_recv(int qid, uint32_t *type, void *data, uint32_t *size) {
    if (qid < 0 || qid >= MAX_MSG_QUEUES) return -1;
    
    struct msg_queue *q = &msg_queues[qid];
    
    /* 检查队列是否为空 */
    if (q->count == 0) {
        return -1;  /* 队列为空 */
    }
    
    /* 获取锁 */
    while (__sync_lock_test_and_set(&q->lock, 1)) {
        asm volatile("pause");
    }
    
    /* 读取消息头 */
    uint32_t idx = q->head;
    if (type != NULL) {
        *type = q->headers[idx].type;
    }
    if (size != NULL) {
        *size = q->headers[idx].size;
    }
    
    /* 复制数据 */
    uint32_t msg_size = q->headers[idx].size;
    if (data != NULL && msg_size > 0) {
        for (uint32_t i = 0; i < msg_size; i++) {
            ((uint8_t *)data)[i] = q->data[idx][i];
        }
    }
    
    /* 更新头指针 */
    q->head = (q->head + 1) % MSG_QUEUE_SIZE;
    q->count--;
    
    __sync_lock_release(&q->lock);
    
    return 0;
}

/* === 共享内存段 === */

#define SHM_MAX_SEGMENTS   32
#define SHM_MAX_SIZE       (16 * 1024 * 1024)  /* 16MB */

struct shm_segment {
    uint64_t phys_addr;     /* 物理地址 */
    uint64_t size;          /* 大小 */
    uint32_t key;           /* 键值 */
    uint32_t creator;       /* 创建者 PID */
    uint32_t ref_count;     /* 引用计数 */
    uint32_t flags;         /* 标志 */
    int in_use;
};

static struct shm_segment shm_segments[SHM_MAX_SEGMENTS];

/* 初始化共享内存 */
void shm_init(void) {
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        shm_segments[i].in_use = 0;
        shm_segments[i].ref_count = 0;
    }
}

/* 创建共享内存段 */
int shm_create(uint32_t key, size_t size, int flags) {
    if (size == 0 || size > SHM_MAX_SIZE) return -1;
    
    /* 检查是否已存在 */
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (shm_segments[i].in_use && shm_segments[i].key == key) {
            if (flags & 0x0800) {  /* IPC_EXCL */
                return -1;  /* 已存在 */
            }
            return i;  /* 返回已存在的段 */
        }
    }
    
    /* 找空闲槽 */
    int slot = -1;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (!shm_segments[i].in_use) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) return -1;
    
    /* 分配物理内存 (简化实现) */
    extern uint64_t pmm_alloc_pages(size_t count);
    size_t pages = (size + 4095) / 4096;
    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == 0) return -1;
    
    shm_segments[slot].phys_addr = phys;
    shm_segments[slot].size = size;
    shm_segments[slot].key = key;
    shm_segments[slot].creator = 0;  /* 当前进程 PID */
    shm_segments[slot].ref_count = 1;
    shm_segments[slot].flags = flags;
    shm_segments[slot].in_use = 1;
    
    return slot;
}

/* 映射共享内存到进程地址空间 */
void *shm_attach(int shmid, void *addr, int flags) {
    if (shmid < 0 || shmid >= SHM_MAX_SEGMENTS) return NULL;
    if (!shm_segments[shmid].in_use) return NULL;
    
    struct shm_segment *shm = &shm_segments[shmid];
    
    /* 增加引用计数 */
    shm->ref_count++;
    
    /* 映射到虚拟地址空间 */
    /* 简化: 返回物理地址的直接映射 */
    extern uint64_t hhdm_offset;
    return (void *)(shm->phys_addr + hhdm_offset);
}

/* 分离共享内存 */
int shm_detach(int shmid) {
    if (shmid < 0 || shmid >= SHM_MAX_SEGMENTS) return -1;
    if (!shm_segments[shmid].in_use) return -1;
    
    shm_segments[shmid].ref_count--;
    
    return 0;
}

/* === 快速信号量 === */

#define SEM_MAX_COUNT   64

struct semaphore {
    volatile int count;
    volatile int waiting;
    volatile int lock;
};

static struct semaphore semaphores[SEM_MAX_COUNT];
static int sem_used[SEM_MAX_COUNT];

/* 初始化信号量 */
void sem_init(void) {
    for (int i = 0; i < SEM_MAX_COUNT; i++) {
        semaphores[i].count = 0;
        semaphores[i].waiting = 0;
        semaphores[i].lock = 0;
        sem_used[i] = 0;
    }
}

/* 创建信号量 */
int sem_create(int initial_value) {
    for (int i = 0; i < SEM_MAX_COUNT; i++) {
        if (!sem_used[i]) {
            sem_used[i] = 1;
            semaphores[i].count = initial_value;
            semaphores[i].waiting = 0;
            return i;
        }
    }
    return -1;
}

/* 等待信号量 */
int sem_wait(int semid) {
    if (semid < 0 || semid >= SEM_MAX_COUNT) return -1;
    
    struct semaphore *sem = &semaphores[semid];
    
    /* 快速路径: 直接递减 */
    if (sem->count > 0) {
        if (__sync_sub_and_fetch(&sem->count, 1) >= 0) {
            return 0;
        }
        __sync_add_and_fetch(&sem->count, 1);
    }
    
    /* 慢速路径: 需要等待 */
    __sync_add_and_fetch(&sem->waiting, 1);
    
    while (sem->count <= 0) {
        asm volatile("pause");
    }
    
    __sync_sub_and_fetch(&sem->waiting, 1);
    __sync_sub_and_fetch(&sem->count, 1);
    
    return 0;
}

/* 释放信号量 */
int sem_post(int semid) {
    if (semid < 0 || semid >= SEM_MAX_COUNT) return -1;
    
    __sync_add_and_fetch(&semaphores[semid].count, 1);
    
    return 0;
}

/* === IPC 统计 === */

struct ipc_stats {
    uint64_t msg_sent;
    uint64_t msg_recv;
    uint64_t shm_creates;
    uint64_t shm_attaches;
    uint64_t sem_ops;
};

static struct ipc_stats ipc_stats;

void ipc_get_stats(struct ipc_stats *stats) {
    if (stats != NULL) {
        *stats = ipc_stats;
    }
}

/* ========== 高级 IPC 增强功能 ========== */

/* === 快速 IPC (零拷贝消息传递) === */

/* 快速 IPC 槽 - 直接共享内存通信 */
#define FAST_IPC_SLOTS     32
#define FAST_IPC_DATA_SIZE 256

struct fast_ipc_slot {
    volatile uint32_t state;      /* 0=空闲, 1=有数据, 2=处理中 */
    uint32_t sender;
    uint32_t receiver;
    uint32_t msg_type;
    uint32_t data_size;
    uint8_t data[FAST_IPC_DATA_SIZE];
    volatile int lock;
    uint64_t timestamp;
};

static struct fast_ipc_slot fast_ipc_slots[FAST_IPC_SLOTS];

/* 快速 IPC 查找表 - 按 sender/receiver 索引 */
#define FAST_IPC_HASH_SIZE 64
static int fast_ipc_by_sender[FAST_IPC_HASH_SIZE];
static int fast_ipc_by_receiver[FAST_IPC_HASH_SIZE];

/* 初始化快速 IPC */
void fast_ipc_init(void) {
    for (int i = 0; i < FAST_IPC_SLOTS; i++) {
        fast_ipc_slots[i].state = 0;
        fast_ipc_slots[i].lock = 0;
    }
    for (int i = 0; i < FAST_IPC_HASH_SIZE; i++) {
        fast_ipc_by_sender[i] = -1;
        fast_ipc_by_receiver[i] = -1;
    }
}

/* 哈希函数 */
static inline uint32_t fast_ipc_hash(uint32_t key) {
    key = ((key >> 16) ^ key) * 0x45d9f3b;
    key = ((key >> 16) ^ key) * 0x45d9f3b;
    key = (key >> 16) ^ key;
    return key % FAST_IPC_HASH_SIZE;
}

/* 快速发送 - 零拷贝 */
int fast_ipc_send(uint32_t receiver, uint32_t type, const void *data, uint32_t size) {
    if (size > FAST_IPC_DATA_SIZE) return -1;
    
    /* 查找或创建槽 */
    int slot = -1;
    uint32_t hash = fast_ipc_hash(receiver);
    
    /* 首先查找接收者已有的槽 - 需要获取锁避免竞态 */
    int idx = fast_ipc_by_receiver[hash];
    if (idx >= 0 && fast_ipc_slots[idx].receiver == receiver) {
        /* 尝试获取锁，避免竞态条件 */
        if (!__sync_lock_test_and_set(&fast_ipc_slots[idx].lock, 1)) {
            /* 获取锁后再次验证状态 */
            if (fast_ipc_slots[idx].state == 0) {
                slot = idx;
            } else {
                /* 状态已改变，释放锁 */
                __sync_lock_release(&fast_ipc_slots[idx].lock);
            }
        }
    }
    
    /* 没找到则分配新槽 */
    if (slot < 0) {
        for (int i = 0; i < FAST_IPC_SLOTS; i++) {
            if (fast_ipc_slots[i].state == 0) {
                /* 尝试获取锁 */
                if (!__sync_lock_test_and_set(&fast_ipc_slots[i].lock, 1)) {
                    if (fast_ipc_slots[i].state == 0) {
                        slot = i;
                        break;
                    }
                    __sync_lock_release(&fast_ipc_slots[i].lock);
                }
            }
        }
    }
    
    if (slot < 0) return -1;
    
    struct fast_ipc_slot *s = &fast_ipc_slots[slot];
    
    /* 写入数据 */
    s->sender = 0;  /* 当前进程 PID */
    s->receiver = receiver;
    s->msg_type = type;
    s->data_size = size;
    
    if (data != NULL && size > 0) {
        for (uint32_t i = 0; i < size; i++) {
            s->data[i] = ((const uint8_t *)data)[i];
        }
    }
    
    /* 更新索引 */
    fast_ipc_by_sender[fast_ipc_hash(s->sender)] = slot;
    fast_ipc_by_receiver[fast_ipc_hash(receiver)] = slot;
    
    /* 设置时间戳 */
    uint64_t tsc_val; asm volatile("rdtsc" : "=A"(tsc_val));
    s->timestamp = tsc_val;
    
    /* 标记为有数据 (释放锁并更新状态) */
    __sync_synchronize();
    s->state = 1;
    __sync_lock_release(&s->lock);
    
    ipc_stats.msg_sent++;
    
    return 0;
}

/* 快速接收 - 零拷贝 */
int fast_ipc_recv(uint32_t *sender, uint32_t *type, void *data, uint32_t *size) {
    uint32_t receiver = 0;  /* 当前进程 PID */
    uint32_t hash = fast_ipc_hash(receiver);
    
    int slot = fast_ipc_by_receiver[hash];
    if (slot < 0) return -1;
    
    struct fast_ipc_slot *s = &fast_ipc_slots[slot];
    
    /* 检查状态 */
    if (s->state != 1) return -1;
    if (s->receiver != receiver) return -1;
    
    /* 获取锁 */
    while (__sync_lock_test_and_set(&s->lock, 1)) {
        asm volatile("pause");
    }
    
    if (s->state != 1) {
        __sync_lock_release(&s->lock);
        return -1;
    }
    
    /* 标记为处理中 */
    s->state = 2;
    
    /* 复制数据 */
    if (sender != NULL) *sender = s->sender;
    if (type != NULL) *type = s->msg_type;
    if (size != NULL) *size = s->data_size;
    
    uint32_t data_size = s->data_size;
    if (data != NULL && data_size > 0) {
        for (uint32_t i = 0; i < data_size; i++) {
            ((uint8_t *)data)[i] = s->data[i];
        }
    }
    
    /* 标记为空闲 */
    s->state = 0;
    __sync_lock_release(&s->lock);
    
    ipc_stats.msg_recv++;
    
    return 0;
}

/* === IPC 权限控制 === */

struct ipc_permission {
    uint32_t owner;
    uint32_t group;
    uint32_t mode;      /* 类 Unix 权限位 */
    uint32_t capabilities;
};

#define IPC_PERM_READ    0x01
#define IPC_PERM_WRITE   0x02
#define IPC_PERM_EXEC    0x04
#define IPC_PERM_ADMIN   0x08

/* 检查 IPC 权限 */
int ipc_check_permission(uint32_t subject, uint32_t object_owner, uint32_t mode) {
    /* 简化实现: owner 有所有权限 */
    if (subject == object_owner) {
        return 1;
    }
    
    /* 检查 capabilities */
    /* TODO: 实现 capability 检查 */
    
    return 0;  /* 默认拒绝 */
}

/* === IPC 命名空间 === */

#define IPC_NS_MAX       8

struct ipc_namespace {
    uint32_t ns_id;
    uint32_t creator;
    uint32_t msg_queues[MAX_MSG_QUEUES];   /* 该命名空间的消息队列 */
    uint32_t shm_segments[SHM_MAX_SEGMENTS]; /* 该命名空间的共享内存 */
    uint32_t semaphores[SEM_MAX_COUNT];    /* 该命名空间的信号量 */
    int in_use;
};

static struct ipc_namespace ipc_namespaces[IPC_NS_MAX];

/* 初始化 IPC 命名空间 */
void ipc_ns_init(void) {
    for (int i = 0; i < IPC_NS_MAX; i++) {
        ipc_namespaces[i].in_use = 0;
    }
}

/* 创建 IPC 命名空间 */
int ipc_ns_create(void) {
    for (int i = 0; i < IPC_NS_MAX; i++) {
        if (!ipc_namespaces[i].in_use) {
            ipc_namespaces[i].ns_id = i;
            ipc_namespaces[i].creator = 0;  /* 当前进程 PID */
            ipc_namespaces[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

/* 销毁 IPC 命名空间 */
void ipc_ns_destroy(uint32_t ns_id) {
    if (ns_id >= IPC_NS_MAX) return;
    
    struct ipc_namespace *ns = &ipc_namespaces[ns_id];
    
    /* 清理该命名空间的所有 IPC 资源 */
    for (int i = 0; i < MAX_MSG_QUEUES; i++) {
        if (ns->msg_queues[i]) {
            msg_queue_used[i] = 0;
        }
    }
    
    ns->in_use = 0;
}

/* === 高性能环形缓冲区 IPC === */

/* 无锁单生产者单消费者环形缓冲区 */
struct ipc_ring {
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t size;
    uint32_t mask;
    uint8_t *buffer;
    uint32_t elem_size;
};

#define IPC_RING_CACHE_ALIGNED __attribute__((aligned(64)))

/* 创建环形缓冲区 */
struct ipc_ring *ipc_ring_create(uint32_t size, uint32_t elem_size) {
    struct ipc_ring *ring = (struct ipc_ring *)kmalloc(sizeof(struct ipc_ring));
    if (ring == NULL) return NULL;
    
    /* 大小必须是 2 的幂 */
    uint32_t actual_size = 1;
    while (actual_size < size) actual_size <<= 1;
    
    ring->buffer = (uint8_t *)kmalloc(actual_size * elem_size);
    if (ring->buffer == NULL) {
        kfree(ring);
        return NULL;
    }
    
    ring->head = 0;
    ring->tail = 0;
    ring->size = actual_size;
    ring->mask = actual_size - 1;
    ring->elem_size = elem_size;
    
    return ring;
}

/* 环形缓冲区写入 (单生产者) */
int ipc_ring_push(struct ipc_ring *ring, const void *elem) {
    if (ring == NULL || elem == NULL) return -1;
    
    uint32_t next = (ring->head + 1) & ring->mask;
    if (next == ring->tail) {
        return -1;  /* 满 */
    }
    
    uint8_t *dst = ring->buffer + (ring->head * ring->elem_size);
    const uint8_t *src = (const uint8_t *)elem;
    for (uint32_t i = 0; i < ring->elem_size; i++) {
        dst[i] = src[i];
    }
    
    __sync_synchronize();
    ring->head = next;
    
    return 0;
}

/* 环形缓冲区读取 (单消费者) */
int ipc_ring_pop(struct ipc_ring *ring, void *elem) {
    if (ring == NULL || elem == NULL) return -1;
    
    if (ring->tail == ring->head) {
        return -1;  /* 空 */
    }
    
    uint8_t *src = ring->buffer + (ring->tail * ring->elem_size);
    uint8_t *dst = (uint8_t *)elem;
    for (uint32_t i = 0; i < ring->elem_size; i++) {
        dst[i] = src[i];
    }
    
    __sync_synchronize();
    ring->tail = (ring->tail + 1) & ring->mask;
    
    return 0;
}

/* 环形缓冲区容量查询 */
uint32_t ipc_ring_count(struct ipc_ring *ring) {
    if (ring == NULL) return 0;
    return (ring->head - ring->tail) & ring->mask;
}

/* 销毁环形缓冲区 */
void ipc_ring_destroy(struct ipc_ring *ring) {
    if (ring == NULL) return;
    if (ring->buffer != NULL) {
        kfree(ring->buffer);
    }
    kfree(ring);
}

/* === RPC (远程过程调用) 支持 === */

#define RPC_MAX_SERVICES    32
#define RPC_MAX_ARGS        8

struct rpc_service {
    uint32_t service_id;
    uint32_t owner;
    char name[32];
    void *handler;              /* 服务处理函数 */
    uint32_t arg_types[RPC_MAX_ARGS];
    uint32_t ret_type;
    int in_use;
};

static struct rpc_service rpc_services[RPC_MAX_SERVICES];

/* 初始化 RPC */
void rpc_init(void) {
    for (int i = 0; i < RPC_MAX_SERVICES; i++) {
        rpc_services[i].in_use = 0;
    }
}

/* 注册 RPC 服务 */
int rpc_register(const char *name, void *handler, uint32_t *arg_types, 
                 uint32_t ret_type) {
    if (name == NULL || handler == NULL) return -1;
    
    /* 查找空闲槽 */
    int slot = -1;
    for (int i = 0; i < RPC_MAX_SERVICES; i++) {
        if (!rpc_services[i].in_use) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) return -1;
    
    struct rpc_service *svc = &rpc_services[slot];
    
    /* 复制名称 */
    int i;
    for (i = 0; i < 31 && name[i]; i++) {
        svc->name[i] = name[i];
    }
    svc->name[i] = '\0';
    
    svc->service_id = slot;
    svc->owner = 0;  /* 当前进程 PID */
    svc->handler = handler;
    svc->ret_type = ret_type;
    
    /* 复制参数类型 */
    if (arg_types != NULL) {
        for (int j = 0; j < RPC_MAX_ARGS; j++) {
            svc->arg_types[j] = arg_types[j];
        }
    }
    
    svc->in_use = 1;
    
    return slot;
}

/* 注销 RPC 服务 */
int rpc_unregister(uint32_t service_id) {
    if (service_id >= RPC_MAX_SERVICES) return -1;
    if (!rpc_services[service_id].in_use) return -1;
    
    rpc_services[service_id].in_use = 0;
    return 0;
}

/* 查找 RPC 服务 */
int rpc_lookup(const char *name) {
    if (name == NULL) return -1;
    
    for (int i = 0; i < RPC_MAX_SERVICES; i++) {
        if (rpc_services[i].in_use) {
            int match = 1;
            for (int j = 0; j < 32 && name[j]; j++) {
                if (rpc_services[i].name[j] != name[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) return i;
        }
    }
    
    return -1;
}

/* 执行 RPC 调用 */
int rpc_call(uint32_t service_id, uint64_t *args, int arg_count, 
             uint64_t *ret_value) {
    if (service_id >= RPC_MAX_SERVICES) return -1;
    if (!rpc_services[service_id].in_use) return -1;
    
    struct rpc_service *svc = &rpc_services[service_id];
    
    if (svc->handler == NULL) return -1;
    
    /* 简化实现: 直接调用处理函数 */
    typedef uint64_t (*rpc_handler_t)(uint64_t *, int);
    rpc_handler_t handler = (rpc_handler_t)svc->handler;
    
    uint64_t result = handler(args, arg_count);
    
    if (ret_value != NULL) {
        *ret_value = result;
    }
    
    return 0;
}

/* === IPC 性能监控 === */

struct ipc_perf_stats {
    uint64_t fast_ipc_sends;
    uint64_t fast_ipc_recvs;
    uint64_t fast_ipc_latency_sum;
    uint64_t fast_ipc_latency_max;
    uint64_t ring_pushes;
    uint64_t ring_pops;
    uint64_t rpc_calls;
    uint64_t permission_denied;
};

static struct ipc_perf_stats ipc_perf;

/* 记录 IPC 延迟 */
void ipc_record_latency(uint64_t latency_ns) {
    ipc_perf.fast_ipc_latency_sum += latency_ns;
    if (latency_ns > ipc_perf.fast_ipc_latency_max) {
        ipc_perf.fast_ipc_latency_max = latency_ns;
    }
}

/* 获取 IPC 性能统计 */
void ipc_get_perf_stats(struct ipc_perf_stats *stats) {
    if (stats != NULL) {
        *stats = ipc_perf;
    }
}

/* 打印 IPC 状态 */
void ipc_print_status(void) {
    println("=== IPC Subsystem Status ===");
    
    print("Message Queues: ");
    int mq_count = 0;
    for (int i = 0; i < MAX_MSG_QUEUES; i++) {
        if (msg_queue_used[i]) mq_count++;
    }
    print_dec(mq_count);
    print(" / ");
    print_dec(MAX_MSG_QUEUES);
    println(" used");
    
    print("Shared Memory: ");
    int shm_count = 0;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (shm_segments[i].in_use) shm_count++;
    }
    print_dec(shm_count);
    print(" / ");
    print_dec(SHM_MAX_SEGMENTS);
    println(" segments");
    
    print("Semaphores: ");
    int sem_count = 0;
    for (int i = 0; i < SEM_MAX_COUNT; i++) {
        if (sem_used[i]) sem_count++;
    }
    print_dec(sem_count);
    print(" / ");
    print_dec(SEM_MAX_COUNT);
    println(" used");
    
    print("Fast IPC Slots: ");
    int fast_count = 0;
    for (int i = 0; i < FAST_IPC_SLOTS; i++) {
        if (fast_ipc_slots[i].state != 0) fast_count++;
    }
    print_dec(fast_count);
    print(" / ");
    print_dec(FAST_IPC_SLOTS);
    println(" active");
    
    print("RPC Services: ");
    int rpc_count = 0;
    for (int i = 0; i < RPC_MAX_SERVICES; i++) {
        if (rpc_services[i].in_use) rpc_count++;
    }
    print_dec(rpc_count);
    print(" / ");
    print_dec(RPC_MAX_SERVICES);
    println(" registered");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 原创优化：高级 IPC 增强功能
 * 
 * 特性：
 * - IPC 事件通知系统
 * - 批量 IPC 操作
 * - IPC 流量控制
 * - IPC 安全审计
 * - 高级诊断报告
 * ═══════════════════════════════════════════════════════════════════════════ */

/* === IPC 事件通知系统 === */

#define IPC_EVENT_MAX       64

enum ipc_event_type {
    IPC_EVENT_MSG_ARRIVAL,      /* 消息到达 */
    IPC_EVENT_SHM_ATTACH,       /* 共享内存附加 */
    IPC_EVENT_SHM_DETACH,       /* 共享内存分离 */
    IPC_EVENT_SEM_ACQUIRE,      /* 信号量获取 */
    IPC_EVENT_SEM_RELEASE,      /* 信号量释放 */
    IPC_EVENT_RPC_CALL,         /* RPC 调用 */
    IPC_EVENT_ERROR,            /* IPC 错误 */
};

struct ipc_event {
    enum ipc_event_type type;
    uint32_t pid;
    uint32_t resource_id;
    uint64_t timestamp;
    uint32_t data;
    int handled;
};

static struct ipc_event ipc_events[IPC_EVENT_MAX];
static volatile uint32_t ipc_event_head;
static volatile uint32_t ipc_event_tail;
static spinlock_t ipc_event_lock = SPINLOCK_INIT;

/* 订阅者 */
#define IPC_EVENT_SUBSCRIBERS    8
typedef void (*ipc_event_callback_t)(struct ipc_event *event);

static ipc_event_callback_t event_subscribers[IPC_EVENT_SUBSCRIBERS];
static int subscriber_count;

/* 注册事件回调 */
int ipc_event_subscribe(ipc_event_callback_t callback) {
    if (subscriber_count >= IPC_EVENT_SUBSCRIBERS) return -1;
    
    event_subscribers[subscriber_count++] = callback;
    return 0;
}

/* 发布事件 */
void ipc_event_publish(enum ipc_event_type type, uint32_t pid, 
                       uint32_t resource_id, uint32_t data) {
    spin_lock(&ipc_event_lock);
    
    uint32_t idx = ipc_event_head % IPC_EVENT_MAX;
    ipc_events[idx].type = type;
    ipc_events[idx].pid = pid;
    ipc_events[idx].resource_id = resource_id;
    ipc_events[idx].timestamp = get_tick_count();
    ipc_events[idx].data = data;
    ipc_events[idx].handled = 0;
    
    ipc_event_head++;
    
    if (ipc_event_head - ipc_event_tail > IPC_EVENT_MAX) {
        ipc_event_tail++;
    }
    
    spin_unlock(&ipc_event_lock);
    
    /* 通知订阅者 */
    for (int i = 0; i < subscriber_count; i++) {
        if (event_subscribers[i] != NULL) {
            event_subscribers[i](&ipc_events[idx]);
        }
    }
}

/* 获取事件 */
int ipc_event_get(struct ipc_event *event) {
    if (event == NULL) return -1;
    
    spin_lock(&ipc_event_lock);
    
    if (ipc_event_tail >= ipc_event_head) {
        spin_unlock(&ipc_event_lock);
        return -1;  /* 无事件 */
    }
    
    uint32_t idx = ipc_event_tail % IPC_EVENT_MAX;
    *event = ipc_events[idx];
    ipc_events[idx].handled = 1;
    ipc_event_tail++;
    
    spin_unlock(&ipc_event_lock);
    return 0;
}

/* === 批量 IPC 操作 === */

#define IPC_BATCH_MAX       16

enum ipc_op_type {
    IPC_OP_MSG_SEND,
    IPC_OP_MSG_RECV,
    IPC_OP_SEM_WAIT,
    IPC_OP_SEM_POST,
    IPC_OP_SHM_ATTACH,
    IPC_OP_SHM_DETACH,
};

struct ipc_batch_op {
    enum ipc_op_type type;
    uint32_t resource_id;
    uint32_t flags;
    void *data;
    uint32_t size;
    int result;
};

struct ipc_batch {
    struct ipc_batch_op ops[IPC_BATCH_MAX];
    int count;
};

/* 创建批量操作 */
struct ipc_batch *ipc_batch_create(void) {
    struct ipc_batch *batch = (struct ipc_batch *)kmalloc(sizeof(struct ipc_batch));
    if (batch != NULL) {
        batch->count = 0;
    }
    return batch;
}

/* 添加操作到批量 */
int ipc_batch_add(struct ipc_batch *batch, enum ipc_op_type type, 
                  uint32_t resource_id, void *data, uint32_t size, uint32_t flags) {
    if (batch == NULL || batch->count >= IPC_BATCH_MAX) return -1;
    
    struct ipc_batch_op *op = &batch->ops[batch->count];
    op->type = type;
    op->resource_id = resource_id;
    op->flags = flags;
    op->data = data;
    op->size = size;
    op->result = -1;
    
    batch->count++;
    return batch->count - 1;
}

/* 执行批量操作 */
int ipc_batch_execute(struct ipc_batch *batch) {
    if (batch == NULL) return -1;
    
    int success_count = 0;
    
    for (int i = 0; i < batch->count; i++) {
        struct ipc_batch_op *op = &batch->ops[i];
        
        switch (op->type) {
            case IPC_OP_MSG_SEND:
                op->result = msg_queue_send(op->resource_id, 0, op->data, op->size);
                break;
                
            case IPC_OP_MSG_RECV:
                op->result = msg_queue_recv(op->resource_id, NULL, op->data, &op->size);
                break;
                
            case IPC_OP_SEM_WAIT:
                op->result = sem_wait(op->resource_id);
                break;
                
            case IPC_OP_SEM_POST:
                op->result = sem_post(op->resource_id);
                break;
                
            case IPC_OP_SHM_ATTACH:
                {
                    void *addr = shm_attach(op->resource_id, NULL, op->flags);
                    op->result = addr != NULL ? 0 : -1;
                }
                break;
                
            case IPC_OP_SHM_DETACH:
                op->result = shm_detach(op->resource_id);
                break;
        }
        
        if (op->result == 0) {
            success_count++;
        }
    }
    
    return success_count;
}

/* 释放批量操作 */
void ipc_batch_free(struct ipc_batch *batch) {
    if (batch != NULL) {
        kfree(batch);
    }
}

/* === IPC 流量控制 === */

struct ipc_rate_limit {
    uint64_t window_start;
    uint32_t ops_in_window;
    uint32_t max_ops_per_window;
    uint64_t window_size_ns;
    uint32_t dropped_ops;
};

#define IPC_RATE_LIMIT_WINDOWS    4

static struct ipc_rate_limit rate_limits[IPC_RATE_LIMIT_WINDOWS];

/* 初始化流量控制 */
void ipc_rate_limit_init(void) {
    for (int i = 0; i < IPC_RATE_LIMIT_WINDOWS; i++) {
        rate_limits[i].window_start = 0;
        rate_limits[i].ops_in_window = 0;
        rate_limits[i].max_ops_per_window = 1000;
        rate_limits[i].window_size_ns = 1000000000ULL;  /* 1秒 */
        rate_limits[i].dropped_ops = 0;
    }
}

/* 检查是否允许操作 */
int ipc_rate_limit_check(int limit_id) {
    if (limit_id < 0 || limit_id >= IPC_RATE_LIMIT_WINDOWS) return 0;
    
    struct ipc_rate_limit *rl = &rate_limits[limit_id];
    uint64_t now = get_tick_count();
    
    /* 检查是否需要重置窗口 */
    if (now - rl->window_start >= rl->window_size_ns) {
        rl->window_start = now;
        rl->ops_in_window = 0;
    }
    
    /* 检查是否超过限制 */
    if (rl->ops_in_window >= rl->max_ops_per_window) {
        rl->dropped_ops++;
        return 0;  /* 拒绝 */
    }
    
    rl->ops_in_window++;
    return 1;  /* 允许 */
}

/* 设置速率限制 */
void ipc_rate_limit_set(int limit_id, uint32_t max_ops, uint64_t window_ns) {
    if (limit_id >= 0 && limit_id < IPC_RATE_LIMIT_WINDOWS) {
        rate_limits[limit_id].max_ops_per_window = max_ops;
        rate_limits[limit_id].window_size_ns = window_ns;
    }
}

/* === IPC 安全审计 === */

struct ipc_audit_record {
    uint64_t timestamp;
    uint32_t pid;
    enum ipc_op_type op_type;
    uint32_t resource_id;
    uint32_t flags;
    int result;
    char comm[16];
};

#define IPC_AUDIT_BUFFER_SIZE    256

static struct ipc_audit_record ipc_audit_buffer[IPC_AUDIT_BUFFER_SIZE];
static uint32_t ipc_audit_head;
static uint32_t ipc_audit_tail;
static spinlock_t ipc_audit_lock = SPINLOCK_INIT;
static int ipc_audit_enabled;

/* 启用/禁用审计 */
void ipc_audit_enable(int enable) {
    ipc_audit_enabled = enable;
}

/* 记录审计 */
void ipc_audit_log(uint32_t pid, enum ipc_op_type op_type, 
                   uint32_t resource_id, int result) {
    if (!ipc_audit_enabled) return;
    
    spin_lock(&ipc_audit_lock);
    
    uint32_t idx = ipc_audit_head % IPC_AUDIT_BUFFER_SIZE;
    
    ipc_audit_buffer[idx].timestamp = get_tick_count();
    ipc_audit_buffer[idx].pid = pid;
    ipc_audit_buffer[idx].op_type = op_type;
    ipc_audit_buffer[idx].resource_id = resource_id;
    ipc_audit_buffer[idx].flags = 0;
    ipc_audit_buffer[idx].result = result;
    
    ipc_audit_head++;
    
    if (ipc_audit_head - ipc_audit_tail > IPC_AUDIT_BUFFER_SIZE) {
        ipc_audit_tail++;
    }
    
    spin_unlock(&ipc_audit_lock);
}

/* 获取审计记录 */
int ipc_audit_get(struct ipc_audit_record *records, int max_count) {
    if (records == NULL) return 0;
    
    spin_lock(&ipc_audit_lock);
    
    int count = 0;
    uint32_t idx = ipc_audit_tail;
    
    while (idx < ipc_audit_head && count < max_count) {
        records[count] = ipc_audit_buffer[idx % IPC_AUDIT_BUFFER_SIZE];
        idx++;
        count++;
    }
    
    spin_unlock(&ipc_audit_lock);
    
    return count;
}

/* === IPC 死锁检测 === */

struct ipc_wait_graph {
    uint32_t pid;
    uint32_t waiting_on;    /* 正在等待的资源 */
    enum ipc_op_type wait_type;
    uint64_t wait_start;
};

#define IPC_WAIT_GRAPH_SIZE    32

static struct ipc_wait_graph wait_graph[IPC_WAIT_GRAPH_SIZE];
static int wait_graph_count;

/* 记录等待 */
void ipc_wait_record(uint32_t pid, uint32_t resource_id, enum ipc_op_type type) {
    if (wait_graph_count >= IPC_WAIT_GRAPH_SIZE) return;
    
    wait_graph[wait_graph_count].pid = pid;
    wait_graph[wait_graph_count].waiting_on = resource_id;
    wait_graph[wait_graph_count].wait_type = type;
    wait_graph[wait_graph_count].wait_start = get_tick_count();
    wait_graph_count++;
}

/* 清除等待记录 */
void ipc_wait_clear(uint32_t pid) {
    for (int i = 0; i < wait_graph_count; i++) {
        if (wait_graph[i].pid == pid) {
            /* 移除该条目 */
            for (int j = i; j < wait_graph_count - 1; j++) {
                wait_graph[j] = wait_graph[j + 1];
            }
            wait_graph_count--;
            i--;
        }
    }
}

/* 检测死锁 */
int ipc_detect_deadlock(void) {
    /* 简化实现：检测循环等待 */
    for (int i = 0; i < wait_graph_count; i++) {
        uint32_t pid1 = wait_graph[i].pid;
        uint32_t resource1 = wait_graph[i].waiting_on;
        
        for (int j = 0; j < wait_graph_count; j++) {
            if (i == j) continue;
            
            uint32_t pid2 = wait_graph[j].pid;
            uint32_t resource2 = wait_graph[j].waiting_on;
            
            /* 如果 pid2 等待 pid1 持有的资源 */
            if (pid2 == resource1 && pid1 == resource2) {
                return 1;  /* 检测到死锁 */
            }
        }
    }
    
    return 0;  /* 无死锁 */
}

/* === IPC 高级诊断报告 === */

struct ipc_diagnostic {
    struct ipc_stats stats;
    struct ipc_perf_stats perf;
    int msg_queues_used;
    int shm_used;
    int sems_used;
    int fast_ipc_active;
    int rpc_services;
    uint32_t rate_limit_drops;
    int deadlock_detected;
    int audit_records;
};

/* 获取 IPC 诊断信息 */
void ipc_get_diagnostic(struct ipc_diagnostic *diag) {
    if (diag == NULL) return;
    
    memset(diag, 0, sizeof(*diag));
    
    /* 统计 */
    diag->stats = ipc_stats;
    diag->perf = ipc_perf;
    
    /* 消息队列 */
    for (int i = 0; i < MAX_MSG_QUEUES; i++) {
        if (msg_queue_used[i]) diag->msg_queues_used++;
    }
    
    /* 共享内存 */
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (shm_segments[i].in_use) diag->shm_used++;
    }
    
    /* 信号量 */
    for (int i = 0; i < SEM_MAX_COUNT; i++) {
        if (sem_used[i]) diag->sems_used++;
    }
    
    /* 快速 IPC */
    for (int i = 0; i < FAST_IPC_SLOTS; i++) {
        if (fast_ipc_slots[i].state != 0) diag->fast_ipc_active++;
    }
    
    /* RPC 服务 */
    for (int i = 0; i < RPC_MAX_SERVICES; i++) {
        if (rpc_services[i].in_use) diag->rpc_services++;
    }
    
    /* 速率限制丢弃 */
    for (int i = 0; i < IPC_RATE_LIMIT_WINDOWS; i++) {
        diag->rate_limit_drops += rate_limits[i].dropped_ops;
    }
    
    /* 死锁检测 */
    diag->deadlock_detected = ipc_detect_deadlock();
    
    /* 审计记录数 */
    diag->audit_records = ipc_audit_head - ipc_audit_tail;
}

/* 打印 IPC 综合诊断报告 */
void ipc_print_diagnostic_report(void) {
    println("");
    println("╔════════════════════════════════════════════════════════════╗");
    println("║           Zratro OS IPC Diagnostic Report                  ║");
    println("╚════════════════════════════════════════════════════════════╝");
    
    /* 基本统计 */
    println("");
    println("=== 基本统计 ===");
    
    print("消息发送: ");
    print_dec(ipc_stats.msg_sent);
    println("");
    
    print("消息接收: ");
    print_dec(ipc_stats.msg_recv);
    println("");
    
    print("共享内存创建: ");
    print_dec(ipc_stats.shm_creates);
    println("");
    
    print("共享内存附加: ");
    print_dec(ipc_stats.shm_attaches);
    println("");
    
    /* 资源使用 */
    println("");
    println("=== 资源使用 ===");
    
    int mq_count = 0, shm_count = 0, sem_count = 0;
    for (int i = 0; i < MAX_MSG_QUEUES; i++) if (msg_queue_used[i]) mq_count++;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) if (shm_segments[i].in_use) shm_count++;
    for (int i = 0; i < SEM_MAX_COUNT; i++) if (sem_used[i]) sem_count++;
    
    print("消息队列: ");
    print_dec(mq_count);
    print(" / ");
    print_dec(MAX_MSG_QUEUES);
    println("");
    
    print("共享内存段: ");
    print_dec(shm_count);
    print(" / ");
    print_dec(SHM_MAX_SEGMENTS);
    println("");
    
    print("信号量: ");
    print_dec(sem_count);
    print(" / ");
    print_dec(SEM_MAX_COUNT);
    println("");
    
    /* 性能统计 */
    println("");
    println("=== 性能统计 ===");
    
    print("快速 IPC 发送: ");
    print_dec(ipc_perf.fast_ipc_sends);
    println("");
    
    print("快速 IPC 接收: ");
    print_dec(ipc_perf.fast_ipc_recvs);
    println("");
    
    if (ipc_perf.fast_ipc_sends > 0) {
        print("平均延迟: ");
        print_dec(ipc_perf.fast_ipc_latency_sum / ipc_perf.fast_ipc_sends / 1000);
        println(" us");
    }
    
    print("最大延迟: ");
    print_dec(ipc_perf.fast_ipc_latency_max / 1000);
    println(" us");
    
    print("RPC 调用: ");
    print_dec(ipc_perf.rpc_calls);
    println("");
    
    /* 流量控制 */
    println("");
    println("=== 流量控制 ===");
    
    uint32_t total_drops = 0;
    for (int i = 0; i < IPC_RATE_LIMIT_WINDOWS; i++) {
        total_drops += rate_limits[i].dropped_ops;
    }
    
    print("丢弃操作: ");
    print_dec(total_drops);
    println("");
    
    print("权限拒绝: ");
    print_dec(ipc_perf.permission_denied);
    println("");
    
    /* 安全状态 */
    println("");
    println("=== 安全状态 ===");
    
    if (ipc_detect_deadlock()) {
        println("** 警告: 检测到潜在死锁! **");
    } else {
        println("无死锁");
    }
    
    print("审计记录: ");
    print_dec(ipc_audit_head - ipc_audit_tail);
    println("");
    
    /* 建议 */
    println("");
    println("=== 优化建议 ===");
    
    if (mq_count > MAX_MSG_QUEUES * 3 / 4) {
        println("- 消息队列使用率较高，考虑增加容量");
    }
    
    if (ipc_perf.permission_denied > 10) {
        println("- 存在多次权限拒绝，检查访问控制配置");
    }
    
    if (total_drops > 100) {
        println("- 流量控制丢弃较多，考虑调整速率限制");
    }
    
    println("");
    println("════════════════════════════════════════════════════════════");
}
