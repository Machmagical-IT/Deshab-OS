/*
 * fs_proxy.c - 文件系统 RPC 代理客户端
 *
 * Ztknl 通过共享内存 + IPI 调用 Azknl (Linux) 的文件系统
 */

#include "vfs.h"
#include "inter_kernel.h"
#include "console.h"
#include "heap.h"
#include "string.h"
#include "cpu.h"

/* 调试开关: 0=关闭, 1=基本, 2=详细 */
#define FS_PROXY_DEBUG 2

#if FS_PROXY_DEBUG >= 1
#define FS_LOG(msg) println("[FS_PROXY] " msg)
#define FS_LOG1(msg, val) do { print("[FS_PROXY] " msg); print_dec(val); println(""); } while(0)
#define FS_LOG2(msg, v1, v2) do { print("[FS_PROXY] " msg); print_dec(v1); print(" / "); print_dec(v2); println(""); } while(0)
#else
#define FS_LOG(msg) ((void)0)
#define FS_LOG1(msg, val) ((void)0)
#define FS_LOG2(msg, v1, v2) ((void)0)
#endif

/* ========== RPC 协议定义 ========== */

/* 文件系统 RPC 请求类型 */
#define FS_RPC_OPEN       0x1001
#define FS_RPC_CLOSE      0x1002
#define FS_RPC_READ       0x1003
#define FS_RPC_WRITE      0x1004
#define FS_RPC_STAT       0x1005
#define FS_RPC_MKDIR      0x1006
#define FS_RPC_RMDIR      0x1007
#define FS_RPC_UNLINK     0x1008
#define FS_RPC_RENAME     0x1009
#define FS_RPC_READDIR    0x100A
#define FS_RPC_TRUNCATE   0x100B
#define FS_RPC_SYNC       0x100C
#define FS_RPC_FSTAT      0x100D
#define FS_RPC_SEEK       0x100E

/* RPC 请求头 */
struct fs_rpc_request {
    uint64_t syscall_id;     /* FS_RPC_* */
    uint64_t request_id;     /* 唯一请求 ID */
    uint64_t path_offset;    /* 路径在 data_area 中的偏移 */
    uint64_t path_len;       /* 路径长度 */
    uint64_t flags;          /* 打开模式/标志 */
    uint64_t data_len;       /* 数据长度 */
};

/* RPC 响应头 */
struct fs_rpc_response {
    int64_t  result;         /* 返回值 */
    int64_t  error_code;     /* 错误码 */
    uint64_t request_id;     /* 对应的请求 ID */
    uint64_t data_offset;    /* 返回数据偏移 */
    uint64_t data_len;       /* 返回数据长度 */
};

/* ========== 共享内存访问 ========== */

/* 共享内存虚拟地址 (通过 EPT 1:1 映射) */
static struct inter_kernel_shm *shm = NULL;

/* ========== 请求管理 ========== */

/* 请求上下文 */
struct rpc_request_ctx {
    uint64_t request_id;
    volatile int pending;
    uint64_t timeout_tsc;
    struct fs_rpc_response resp;
};

#define MAX_PENDING_REQUESTS 16
static struct rpc_request_ctx pending_requests[MAX_PENDING_REQUESTS];
static uint64_t next_request_id = 1;
static spinlock_t rpc_lock = SPINLOCK_INIT;

/* data_area 管理 */
#define DATA_AREA_OFFSET  0x2100
#define DATA_AREA_SIZE    (INTER_KERNEL_SHM_SIZE - 0x2100)  /* ~57KB */

/* 简单分配器 (单线程，由 rpc_lock 保护) */
static uint32_t data_area_used = 0;

static uint32_t data_area_alloc(uint32_t size) {
    if (size > DATA_AREA_SIZE) {
        return 0xFFFFFFFF;  /* 太大 */
    }
    if (data_area_used + size > DATA_AREA_SIZE) {
        data_area_used = 0;  /* 简单回收 */
    }
    uint32_t offset = data_area_used;
    data_area_used += size;
    return offset;
}

static void data_area_reset(void) {
    data_area_used = 0;
}

/* 获取 data_area 指针 */
static uint8_t *data_area_ptr(void) {
    if (shm == NULL) return NULL;
    return shm->data_area;
}

/* ========== 辅助函数 ========== */

/* Linux errno 到 VFS errno 映射 */
static int linux_errno_to_vfs(int linux_errno) {
    switch (linux_errno) {
        case 0:   return VFS_OK;
        case -2:  return VFS_ERR_NOENT;    /* ENOENT */
        case -13: return VFS_ERR_ACCESS;   /* EACCES */
        case -17: return VFS_ERR_EXISTS;   /* EEXIST */
        case -20: return VFS_ERR_NOTDIR;   /* ENOTDIR */
        case -21: return VFS_ERR_ISDIR;    /* EISDIR */
        case -22: return VFS_ERR_INVAL;    /* EINVAL */
        case -27: return VFS_ERR_NOSPC;    /* ENOSPC */
        case -30: return VFS_ERR_ROFS;     /* EROFS */
        case -36: return VFS_ERR_NAMETOOLONG; /* ENAMETOOLONG */
        case -39: return VFS_ERR_NOTEMPTY; /* ENOTEMPTY */
        case -40: return VFS_ERR_LOOP;     /* ELOOP */
        default:  return VFS_ERR_IO;
    }
}

/* 分配请求 ID */
static uint64_t alloc_request_id(void) {
    return next_request_id++;
}

/* 查找空闲请求槽位 */
static int find_free_slot(void) {
    for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
        if (!pending_requests[i].pending) {
            return i;
        }
    }
    return -1;
}

/* 等待响应 (带超时) */
static int wait_for_response(int slot, uint64_t timeout_ms) {
    uint64_t start = rdtsc();
    uint64_t timeout_ticks = timeout_ms * 2000000ULL;  /* 假设 2GHz */

    while (pending_requests[slot].pending) {
        uint64_t now = rdtsc();
        if (now - start > timeout_ticks) {
            FS_LOG("Request timeout");
            pending_requests[slot].pending = 0;
            return -1;  /* 超时 */
        }

        /* 检查是否有新消息 */
        if (inter_kernel_pending_msgs() > 0) {
            struct inter_kernel_msg msg;
            if (inter_kernel_recv_msg(&msg) == 0) {
                /* 检查是否是响应消息 */
                if (msg.type == MSG_TYPE_TASK_RESPONSE) {
                    struct fs_rpc_response *resp = (struct fs_rpc_response *)msg.data;
                    /* 匹配任意未完成的请求 (简化匹配) */
                    for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
                        if (pending_requests[i].pending &&
                            pending_requests[i].request_id == resp->request_id) {
                            memcpy(&pending_requests[i].resp, resp, sizeof(struct fs_rpc_response));
                            pending_requests[i].pending = 0;
                            if (i == slot) {
                                return 0;  /* 这是我们等待的响应 */
                            }
                            break;
                        }
                    }
                }
            }
        }

        /* 短暂让步 */
        for (volatile int i = 0; i < 1000; i++);
    }

    return 0;
}

/* ========== RPC 核心函数 ========== */

/* 发送 RPC 请求 */
static int64_t fs_rpc_send(uint64_t syscall_id, const char *path, uint64_t flags,
                           const void *in_data, uint64_t in_len,
                           void *out_data, uint64_t out_cap) {
    if (shm == NULL) {
        FS_LOG("Shared memory not initialized");
        return -VFS_ERR_IO;
    }

    if (!inter_kernel_is_peer_ready()) {
        FS_LOG("Peer not ready");
        return -VFS_ERR_IO;
    }

    spin_lock(&rpc_lock);

    /* 分配请求槽位 */
    int slot = find_free_slot();
    if (slot < 0) {
        spin_unlock(&rpc_lock);
        FS_LOG("No free request slot");
        return -VFS_ERR_BUSY;
    }

    /* 重置 data_area */
    data_area_reset();

    /* 分配请求 ID */
    uint64_t req_id = alloc_request_id();
    pending_requests[slot].request_id = req_id;
    pending_requests[slot].pending = 1;

    /* 复制路径到 data_area */
    uint64_t path_len = path ? strlen(path) + 1 : 0;
    uint32_t path_offset = 0xFFFFFFFF;
    if (path_len > 0) {
        path_offset = data_area_alloc(path_len);
        if (path_offset == 0xFFFFFFFF) {
            spin_unlock(&rpc_lock);
            return -VFS_ERR_NOMEM;
        }
        memcpy(data_area_ptr() + path_offset, path, path_len);
    }

    /* 复制输入数据到 data_area */
    uint32_t data_offset = 0xFFFFFFFF;
    if (in_data && in_len > 0) {
        data_offset = data_area_alloc(in_len);
        if (data_offset == 0xFFFFFFFF) {
            spin_unlock(&rpc_lock);
            return -VFS_ERR_NOMEM;
        }
        memcpy(data_area_ptr() + data_offset, in_data, in_len);
    }

    /* 构造请求 */
    struct fs_rpc_request req = {
        .syscall_id = syscall_id,
        .request_id = req_id,
        .path_offset = path_offset,
        .path_len = path_len,
        .flags = flags,
        .data_len = in_len
    };

    /* 发送消息 */
    uint64_t msg_data[5];
    memcpy(msg_data, &req, sizeof(msg_data));

    int ret = inter_kernel_send_msg(MSG_TYPE_SYSCALL_PROXY, msg_data, 5);
    if (ret != 0) {
        pending_requests[slot].pending = 0;
        spin_unlock(&rpc_lock);
        FS_LOG("Failed to send message");
        return -VFS_ERR_IO;
    }

    /* 发送 IPI 通知 */
    inter_kernel_notify_peer();

    spin_unlock(&rpc_lock);

    /* 等待响应 */
    int wait_ret = wait_for_response(slot, 5000);  /* 5 秒超时 */
    if (wait_ret != 0) {
        return -VFS_ERR_IO;
    }

    /* 处理响应 */
    struct fs_rpc_response *resp = &pending_requests[slot].resp;

    if (resp->result < 0) {
        return linux_errno_to_vfs((int)resp->error_code);
    }

    /* 复制输出数据 */
    if (out_data && resp->data_len > 0 && resp->data_len <= out_cap) {
        memcpy(out_data, data_area_ptr() + resp->data_offset, resp->data_len);
    }

    int64_t result = resp->result;
    pending_requests[slot].pending = 0;

    return result;
}

/* ========== VFS 操作实现 ========== */

/* RPC open */
static int rpc_vfs_open(struct vfs_file *file, const char *path, uint32_t mode) {
    FS_LOG1("open: ", (uint64_t)path);

    int64_t ret = fs_rpc_send(FS_RPC_OPEN, path, mode, NULL, 0, NULL, 0);
    if (ret < 0) {
        return (int)ret;
    }

    /* 远程文件描述符存储在 file->fs_data */
    file->fs_data = (void *)(uint64_t)ret;
    file->offset = 0;
    return VFS_OK;
}

/* RPC close */
static int rpc_vfs_close(struct vfs_file *file) {
    uint64_t remote_fd = (uint64_t)file->fs_data;

    int64_t ret = fs_rpc_send(FS_RPC_CLOSE, NULL, remote_fd, NULL, 0, NULL, 0);
    file->fs_data = NULL;
    return (ret < 0) ? (int)ret : VFS_OK;
}

/* RPC read */
static ssize_t rpc_vfs_read(struct vfs_file *file, void *buf, size_t count) {
    uint64_t remote_fd = (uint64_t)file->fs_data;

    /* 分配输出缓冲区 */
    int64_t ret = fs_rpc_send(FS_RPC_READ, NULL, remote_fd | (count << 32),
                              NULL, 0, buf, count);
    if (ret < 0) {
        return ret;
    }
    return (ssize_t)ret;
}

/* RPC write */
static ssize_t rpc_vfs_write(struct vfs_file *file, const void *buf, size_t count) {
    uint64_t remote_fd = (uint64_t)file->fs_data;

    int64_t ret = fs_rpc_send(FS_RPC_WRITE, NULL, remote_fd, buf, count, NULL, 0);
    if (ret < 0) {
        return ret;
    }
    return (ssize_t)ret;
}

/* RPC seek */
static int rpc_vfs_seek(struct vfs_file *file, uint64_t offset) {
    uint64_t remote_fd = (uint64_t)file->fs_data;

    /* seek 的偏移量通过 data_area 传递 */
    /* fs_rpc_send 内部会处理 data_area 的分配 */
    int64_t ret = fs_rpc_send(FS_RPC_SEEK, NULL, remote_fd,
                              &offset, sizeof(offset),
                              NULL, 0);
    if (ret < 0) {
        return (int)ret;
    }

    /* 响应中的 result 就是新的文件偏移 */
    file->offset = (uint64_t)ret;
    return VFS_OK;
}

/* RPC stat */
static int rpc_vfs_stat(const char *path, struct vfs_stat *stat) {
    struct vfs_stat stat_buf;
    int64_t ret = fs_rpc_send(FS_RPC_STAT, path, 0, NULL, 0, &stat_buf, sizeof(stat_buf));
    if (ret < 0) {
        return (int)ret;
    }
    memcpy(stat, &stat_buf, sizeof(stat_buf));
    return VFS_OK;
}

/* RPC fstat */
static int rpc_vfs_fstat(struct vfs_file *file, struct vfs_stat *stat) {
    uint64_t remote_fd = (uint64_t)file->fs_data;
    struct vfs_stat stat_buf;

    int64_t ret = fs_rpc_send(FS_RPC_FSTAT, NULL, remote_fd, NULL, 0, &stat_buf, sizeof(stat_buf));
    if (ret < 0) {
        return (int)ret;
    }
    memcpy(stat, &stat_buf, sizeof(stat_buf));
    return VFS_OK;
}

/* RPC mkdir */
static int rpc_vfs_mkdir(const char *path) {
    int64_t ret = fs_rpc_send(FS_RPC_MKDIR, path, 0, NULL, 0, NULL, 0);
    return (ret < 0) ? (int)ret : VFS_OK;
}

/* RPC rmdir */
static int rpc_vfs_rmdir(const char *path) {
    int64_t ret = fs_rpc_send(FS_RPC_RMDIR, path, 0, NULL, 0, NULL, 0);
    return (ret < 0) ? (int)ret : VFS_OK;
}

/* RPC unlink */
static int rpc_vfs_unlink(const char *path) {
    int64_t ret = fs_rpc_send(FS_RPC_UNLINK, path, 0, NULL, 0, NULL, 0);
    return (ret < 0) ? (int)ret : VFS_OK;
}

/* RPC rename */
static int rpc_vfs_rename(const char *oldpath, const char *newpath) {
    /* 简单实现：传递两个路径 */
    struct {
        char old[128];
        char new[128];
    } rename_data;

    strncpy(rename_data.old, oldpath, 127);
    rename_data.old[127] = '\0';
    strncpy(rename_data.new, newpath, 127);
    rename_data.new[127] = '\0';

    int64_t ret = fs_rpc_send(FS_RPC_RENAME, oldpath, 0, &rename_data, sizeof(rename_data), NULL, 0);
    return (ret < 0) ? (int)ret : VFS_OK;
}

/* RPC readdir */
static int rpc_vfs_readdir(const char *path, struct vfs_dirent *dirent, int index) {
    struct {
        int index;
    } req = { .index = index };

    int64_t ret = fs_rpc_send(FS_RPC_READDIR, path, 0, &req, sizeof(req), dirent, sizeof(struct vfs_dirent));
    return (ret < 0) ? (int)ret : VFS_OK;
}

/* RPC truncate */
static int rpc_vfs_truncate(const char *path, uint64_t length) {
    int64_t ret = fs_rpc_send(FS_RPC_TRUNCATE, path, length, NULL, 0, NULL, 0);
    return (ret < 0) ? (int)ret : VFS_OK;
}

/* RPC sync */
static int rpc_vfs_sync(struct vfs_file *file) {
    uint64_t remote_fd = (uint64_t)file->fs_data;
    int64_t ret = fs_rpc_send(FS_RPC_SYNC, NULL, remote_fd, NULL, 0, NULL, 0);
    return (ret < 0) ? (int)ret : VFS_OK;
}

/* ========== VFS 操作接口 ========== */

static struct vfs_ops rpc_vfs_ops = {
    .open = rpc_vfs_open,
    .close = rpc_vfs_close,
    .read = rpc_vfs_read,
    .write = rpc_vfs_write,
    .seek = rpc_vfs_seek,
    .stat = rpc_vfs_stat,
    .readdir = rpc_vfs_readdir,
    .mkdir = rpc_vfs_mkdir,
    .rmdir = rpc_vfs_rmdir,
    .unlink = rpc_vfs_unlink,
    .rename = rpc_vfs_rename,
    .fstat = rpc_vfs_fstat,
    .sync = rpc_vfs_sync,
    .truncate = rpc_vfs_truncate,
};

/* ========== 公共 API ========== */

/* 获取 RPC VFS 操作接口 */
struct vfs_ops *fs_proxy_get_ops(void) {
    return &rpc_vfs_ops;
}

/* 初始化 FS Proxy */
int fs_proxy_init(void) {
    /* 获取共享内存地址 */
    uint64_t base, size;
    inter_kernel_get_shm_info(&base, &size);
    shm = (struct inter_kernel_shm *)(uint64_t)base;

    FS_LOG1("Shared memory at: 0x", base);

    /* 验证共享内存 */
    if (shm->header.magic != INTER_KERNEL_MAGIC) {
        FS_LOG("Invalid shared memory magic");
        print_hex(shm->header.magic);
        println("");
        return -1;
    }

    /* 验证队列配置 */
    if (shm->ztknl_to_azknl.capacity != INTER_KERNEL_QUEUE_MSG_COUNT ||
        shm->azknl_to_ztknl.capacity != INTER_KERNEL_QUEUE_MSG_COUNT) {
        FS_LOG("Invalid queue capacity");
        FS_LOG2("Z->A:", shm->ztknl_to_azknl.capacity, INTER_KERNEL_QUEUE_MSG_COUNT);
        FS_LOG2("A->Z:", shm->azknl_to_ztknl.capacity, INTER_KERNEL_QUEUE_MSG_COUNT);
    }

    /* 初始化请求上下文 */
    memset(pending_requests, 0, sizeof(pending_requests));
    data_area_used = 0;

    FS_LOG1("Initialized, data_area size: ", DATA_AREA_SIZE);
    return 0;
}

/* 检查代理是否可用 */
int fs_proxy_available(void) {
    if (shm == NULL) return 0;
    return inter_kernel_is_peer_ready();
}
