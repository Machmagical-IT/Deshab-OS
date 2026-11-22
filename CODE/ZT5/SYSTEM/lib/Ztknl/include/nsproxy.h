/*
 * Zratro OS - Namespaces (容器隔离基础)
 * 
 * Linux 风格的命名空间隔离机制
 */

#ifndef NAMESPACE_H
#define NAMESPACE_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* ========== 命名空间类型 ========== */

/* 命名空间类型标志 */
#define CLONE_NEWNS        0x00020000  /* 挂载命名空间 */
#define CLONE_NEWUTS       0x04000000  /* UTS 命名空间 */
#define CLONE_NEWIPC       0x08000000  /* IPC 命名空间 */
#define CLONE_NEWUSER      0x10000000  /* 用户命名空间 */
#define CLONE_NEWPID       0x20000000  /* PID 命名空间 */
#define CLONE_NEWNET       0x40000000  /* 网络命名空间 */
#define CLONE_NEWCGROUP    0x02000000  /* cgroup 命名空间 */

/* ========== UTS 命名空间 (主机名和域名) ========== */

struct uts_namespace {
    /* 主机名 */
    char nodename[65];
    /* 域名 */
    char domainname[65];
    
    /* 版本信息 */
    char sysname[65];       /* 系统名称 */
    char release[65];       /* 内核版本 */
    char version[65];       /* 版本字符串 */
    char machine[65];       /* 硬件架构 */
    
    /* 引用计数 */
    atomic_t ref_count;
    
    /* 所属用户命名空间 */
    struct user_namespace *user_ns;
    
    struct list_head list;
};

/* ========== IPC 命名空间 (System V IPC 和 POSIX 消息队列) ========== */

struct ipc_namespace {
    /* System V IPC 标识符 */
    uint32_t next_id;
    
    /* IPC 资源限制 */
    uint32_t msg_max;           /* 消息最大长度 */
    uint32_t msgmnb;            /* 消息队列最大字节数 */
    uint32_t msgmni;            /* 消息队列最大数量 */
    uint32_t shmmni;            /* 共享内存最大数量 */
    uint64_t shmmax;            /* 共享内存最大大小 */
    uint64_t shmall;            /* 共享内存总页数 */
    uint32_t semmni;            /* 信号量最大数量 */
    
    /* IPC 对象 */
    struct list_head msg_queues;    /* 消息队列 */
    struct list_head shm_segments;  /* 共享内存 */
    struct list_head sem_arrays;    /* 信号量数组 */
    
    /* 引用计数 */
    atomic_t ref_count;
    
    /* 所属用户命名空间 */
    struct user_namespace *user_ns;
    
    struct list_head list;
};

/* ========== PID 命名空间 (进程 ID 隔离) ========== */

struct pid_namespace {
    /* ID 分配 */
    uint32_t pid_next;
    uint32_t pid_max;
    
    /* 层级 */
    int level;                  /* 嵌套层级 */
    struct pid_namespace *parent; /* 父命名空间 */
    
    /* 进程表 */
    struct process **pid_table;
    uint32_t pid_table_size;
    
    /* 统计 */
    uint32_t nr_processes;
    
    /* 引用计数 */
    atomic_t ref_count;
    
    /* 所属用户命名空间 */
    struct user_namespace *user_ns;
    
    /* 子命名空间链表 */
    struct list_head children;
    struct list_head sibling;
    
    struct list_head list;
};

/* ========== 挂载命名空间 (文件系统隔离) ========== */

struct mnt_namespace {
    /* 根挂载点 */
    struct mount *root;
    
    /* 挂载点列表 */
    struct list_head mount_list;
    uint32_t nr_mounts;
    
    /* 引用计数 */
    atomic_t ref_count;
    
    /* 所属用户命名空间 */
    struct user_namespace *user_ns;
    
    struct list_head list;
};

/* 挂载点 */
struct mount {
    uint64_t mount_id;
    uint64_t parent_id;
    
    struct vfs_node *mnt_root;      /* 挂载的根目录 */
    struct vfs_node *mnt_mountpoint; /* 挂载点 */
    struct vfs_filesystem *mnt_fs;  /* 文件系统 */
    
    struct mount *mnt_parent;       /* 父挂载 */
    struct list_head mnt_mounts;    /* 子挂载链表 */
    struct list_head mnt_child;     /* 父的子链表 */
    struct list_head mnt_list;      /* 命名空间挂载链表 */
    
    /* 挂载选项 */
    uint64_t mnt_flags;
    char mnt_devname[64];           /* 设备名 */
};

/* ========== 网络命名空间 (网络隔离) ========== */

struct net_namespace {
    /* 网络设备 */
    struct list_head dev_list;
    uint32_t dev_count;
    
    /* 回环设备 */
    struct nic_device *loopback;
    
    /* IP 地址 */
    uint32_t local_ip;
    uint32_t netmask;
    uint32_t broadcast;
    
    /* 路由表 */
    struct list_head route_table;
    
    /* Socket 统计 */
    uint32_t tcp_sockets;
    uint32_t udp_sockets;
    uint32_t unix_sockets;
    
    /* iptables 规则 */
    struct list_head filter_rules;
    struct list_head nat_rules;
    struct list_head mangle_rules;
    
    /* 引用计数 */
    atomic_t ref_count;
    
    /* 所属用户命名空间 */
    struct user_namespace *user_ns;
    
    struct list_head list;
};

/* ========== 用户命名空间 (UID/GID 隔离) ========== */

#define UID_GID_MAP_MAX  340

/* UID/GID 映射 */
struct uid_gid_map {
    uint32_t nr_extents;
    struct uid_gid_extent {
        uint32_t first;
        uint32_t lower_first;
        uint32_t count;
    } extent[UID_GID_MAP_MAX];
};

struct user_namespace {
    /* 父命名空间 */
    struct user_namespace *parent;
    int level;
    
    /* UID/GID 映射 */
    struct uid_gid_map uid_map;
    struct uid_gid_map gid_map;
    struct uid_gid_map projid_map;
    
    /* 能力 */
    uint64_t capabilities;
    uint64_t capabilities_bounding;
    
    /* 所有者 */
    uint32_t owner;
    uint32_t group;
    
    /* 引用计数 */
    atomic_t ref_count;
    
    /* 子命名空间 */
    struct list_head children;
    struct list_head sibling;
    
    struct list_head list;
};

/* ========== cgroup 命名空间 ========== */

struct cgroup_namespace {
    /* 根 cgroup */
    struct cgroup *root_cgroup;
    
    /* 引用计数 */
    atomic_t ref_count;
    
    /* 所属用户命名空间 */
    struct user_namespace *user_ns;
    
    struct list_head list;
};

/* ========== 命名空间代理 (nsproxy) ========== */

struct nsproxy {
    /* 各类命名空间指针 */
    struct uts_namespace *uts_ns;
    struct ipc_namespace *ipc_ns;
    struct mnt_namespace *mnt_ns;
    struct pid_namespace *pid_ns_for_children;
    struct net_namespace *net_ns;
    struct cgroup_namespace *cgroup_ns;
    
    /* 引用计数 */
    atomic_t ref_count;
};

/* ========== 命名空间操作 ========== */

/* 初始化 */
int nsproxy_init(void);

/* 创建空 nsproxy */
struct nsproxy *nsproxy_create(void);

/* 复制 nsproxy */
struct nsproxy *nsproxy_copy(struct nsproxy *orig, uint64_t flags);

/* 释放 nsproxy */
void nsproxy_release(struct nsproxy *ns);

/* 获取当前进程的 nsproxy */
struct nsproxy *nsproxy_get_current(void);

/* ========== 创建命名空间 ========== */

struct uts_namespace *uts_namespace_create(struct user_namespace *user_ns);
struct ipc_namespace *ipc_namespace_create(struct user_namespace *user_ns);
struct pid_namespace *pid_namespace_create(struct user_namespace *user_ns);
struct mnt_namespace *mnt_namespace_create(struct user_namespace *user_ns);
struct net_namespace *net_namespace_create(struct user_namespace *user_ns);
struct user_namespace *user_namespace_create(struct user_namespace *parent);
struct cgroup_namespace *cgroup_namespace_create(struct user_namespace *user_ns);

/* ========== 命名空间操作 ========== */

/* 进入命名空间 */
int ns_enter(int fd, int nstype);

/* 设置命名空间 */
int ns_setns(int fd, int nstype);

/* 取消共享 */
int ns_unshare(uint64_t flags);

/* ========== 命名空间查找 ========== */

/* 通过文件描述符获取命名空间 */
void *ns_get_by_fd(int fd, int nstype);

/* ========== UID/GID 映射 ========== */

/* 映射 UID */
int user_namespace_map_uid(struct user_namespace *ns, uint32_t uid);

/* 映射 GID */
int user_namespace_map_gid(struct user_namespace *ns, uint32_t gid);

/* 反向映射 UID */
uint32_t user_namespace_unmap_uid(struct user_namespace *ns, uint32_t uid);

/* 反向映射 GID */
uint32_t user_namespace_unmap_gid(struct user_namespace *ns, uint32_t gid);

/* ========== 辅助函数 ========== */

/* 获取当前 UTS 命名空间 */
static inline struct uts_namespace *uts_ns(void) {
    struct nsproxy *ns = nsproxy_get_current();
    return ns ? ns->uts_ns : NULL;
}

/* 获取当前 IPC 命名空间 */
static inline struct ipc_namespace *ipc_ns(void) {
    struct nsproxy *ns = nsproxy_get_current();
    return ns ? ns->ipc_ns : NULL;
}

/* 获取当前 PID 命名空间 */
static inline struct pid_namespace *pid_ns(void) {
    struct nsproxy *ns = nsproxy_get_current();
    return ns ? ns->pid_ns_for_children : NULL;
}

/* 获取当前挂载命名空间 */
static inline struct mnt_namespace *mnt_ns(void) {
    struct nsproxy *ns = nsproxy_get_current();
    return ns ? ns->mnt_ns : NULL;
}

/* 获取当前网络命名空间 */
static inline struct net_namespace *net_ns(void) {
    struct nsproxy *ns = nsproxy_get_current();
    return ns ? ns->net_ns : NULL;
}

/* 获取当前用户命名空间 */
static inline struct user_namespace *user_ns(void) {
    struct nsproxy *ns = nsproxy_get_current();
    return ns ? ns->pid_ns_for_children->user_ns : NULL;
}

#endif /* NAMESPACE_H */
