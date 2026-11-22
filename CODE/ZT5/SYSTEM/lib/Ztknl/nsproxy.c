/*
 * Zratro OS - Namespaces Implementation
 * 
 * 容器隔离机制实现
 */

#include "nsproxy.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "smp.h"
#include "task.h"

/* ========== 全局命名空间列表 ========== */

static struct list_head uts_ns_list;
static struct list_head ipc_ns_list;
static struct list_head pid_ns_list;
static struct list_head mnt_ns_list;
static struct list_head net_ns_list;
static struct list_head user_ns_list;
static struct list_head cgroup_ns_list;

static spinlock_t ns_lists_lock = SPINLOCK_INIT;

/* ========== 默认命名空间 ========== */

static struct uts_namespace default_uts_ns;
static struct ipc_namespace default_ipc_ns;
static struct pid_namespace default_pid_ns;
static struct mnt_namespace default_mnt_ns;
static struct net_namespace default_net_ns;
static struct user_namespace default_user_ns;
static struct nsproxy default_nsproxy;

/* ========== 初始化 ========== */

int nsproxy_init(void) {
    println("[NS] Initializing namespaces...");
    
    /* 初始化列表 */
    list_init(&uts_ns_list);
    list_init(&ipc_ns_list);
    list_init(&pid_ns_list);
    list_init(&mnt_ns_list);
    list_init(&net_ns_list);
    list_init(&user_ns_list);
    list_init(&cgroup_ns_list);
    
    /* 初始化默认用户命名空间 */
    default_user_ns.parent = NULL;
    default_user_ns.level = 0;
    default_user_ns.ref_count = 1;
    default_user_ns.owner = 0;
    default_user_ns.group = 0;
    default_user_ns.capabilities = 0xFFFFFFFFFFFFFFFFULL;
    list_init(&default_user_ns.children);
    list_init(&default_user_ns.list);
    
    /* 初始化默认 UTS 命名空间 */
    strcpy(default_uts_ns.nodename, "zratro");
    strcpy(default_uts_ns.domainname, "localdomain");
    strcpy(default_uts_ns.sysname, "Zratro OS");
    strcpy(default_uts_ns.release, "1.0.0");
    strcpy(default_uts_ns.version, "Zratro OS 1.0.0 x86_64");
    strcpy(default_uts_ns.machine, "x86_64");
    default_uts_ns.ref_count = 1;
    default_uts_ns.user_ns = &default_user_ns;
    list_init(&default_uts_ns.list);
    
    /* 初始化默认 IPC 命名空间 */
    default_ipc_ns.next_id = 1;
    default_ipc_ns.msg_max = 8192;
    default_ipc_ns.msgmnb = 16384;
    default_ipc_ns.msgmni = 32000;
    default_ipc_ns.shmmni = 4096;
    default_ipc_ns.shmmax = 0x2000000;
    default_ipc_ns.shmall = 0x4000000;
    default_ipc_ns.semmni = 32000;
    list_init(&default_ipc_ns.msg_queues);
    list_init(&default_ipc_ns.shm_segments);
    list_init(&default_ipc_ns.sem_arrays);
    default_ipc_ns.ref_count = 1;
    default_ipc_ns.user_ns = &default_user_ns;
    list_init(&default_ipc_ns.list);
    
    /* 初始化默认 PID 命名空间 */
    default_pid_ns.pid_next = 1;
    default_pid_ns.pid_max = 32768;
    default_pid_ns.level = 0;
    default_pid_ns.parent = NULL;
    default_pid_ns.nr_processes = 0;
    default_pid_ns.ref_count = 1;
    default_pid_ns.user_ns = &default_user_ns;
    list_init(&default_pid_ns.children);
    list_init(&default_pid_ns.list);
    
    /* 初始化默认挂载命名空间 */
    list_init(&default_mnt_ns.mount_list);
    default_mnt_ns.nr_mounts = 0;
    default_mnt_ns.ref_count = 1;
    default_mnt_ns.user_ns = &default_user_ns;
    list_init(&default_mnt_ns.list);
    
    /* 初始化默认网络命名空间 */
    list_init(&default_net_ns.dev_list);
    default_net_ns.dev_count = 0;
    list_init(&default_net_ns.route_table);
    list_init(&default_net_ns.filter_rules);
    list_init(&default_net_ns.nat_rules);
    list_init(&default_net_ns.mangle_rules);
    default_net_ns.ref_count = 1;
    default_net_ns.user_ns = &default_user_ns;
    list_init(&default_net_ns.list);
    
    /* 初始化默认 nsproxy */
    default_nsproxy.uts_ns = &default_uts_ns;
    default_nsproxy.ipc_ns = &default_ipc_ns;
    default_nsproxy.mnt_ns = &default_mnt_ns;
    default_nsproxy.pid_ns_for_children = &default_pid_ns;
    default_nsproxy.net_ns = &default_net_ns;
    default_nsproxy.cgroup_ns = NULL;
    default_nsproxy.ref_count = 1;
    
    /* 添加到全局列表 */
    list_add(&default_uts_ns.list, &uts_ns_list);
    list_add(&default_ipc_ns.list, &ipc_ns_list);
    list_add(&default_pid_ns.list, &pid_ns_list);
    list_add(&default_mnt_ns.list, &mnt_ns_list);
    list_add(&default_net_ns.list, &net_ns_list);
    list_add(&default_user_ns.list, &user_ns_list);
    
    println("[NS] Namespaces initialized");
    return 0;
}

/* ========== nsproxy 操作 ========== */

struct nsproxy *nsproxy_create(void) {
    struct nsproxy *ns = kmalloc(sizeof(*ns));
    if (ns == NULL) return NULL;
    
    memset(ns, 0, sizeof(*ns));
    ns->ref_count = 1;
    
    return ns;
}

struct nsproxy *nsproxy_copy(struct nsproxy *orig, uint64_t flags) {
    if (orig == NULL) return NULL;
    
    struct nsproxy *ns = nsproxy_create();
    if (ns == NULL) return NULL;
    
    spin_lock(&ns_lists_lock);
    
    /* 复制或创建命名空间 */
    if (flags & CLONE_NEWUTS) {
        ns->uts_ns = uts_namespace_create(orig->uts_ns->user_ns);
    } else {
        ns->uts_ns = orig->uts_ns;
        ns->uts_ns->ref_count++;
    }
    
    if (flags & CLONE_NEWIPC) {
        ns->ipc_ns = ipc_namespace_create(orig->ipc_ns->user_ns);
    } else {
        ns->ipc_ns = orig->ipc_ns;
        ns->ipc_ns->ref_count++;
    }
    
    if (flags & CLONE_NEWPID) {
        ns->pid_ns_for_children = pid_namespace_create(orig->pid_ns_for_children->user_ns);
    } else {
        ns->pid_ns_for_children = orig->pid_ns_for_children;
        ns->pid_ns_for_children->ref_count++;
    }
    
    if (flags & CLONE_NEWNS) {
        ns->mnt_ns = mnt_namespace_create(orig->mnt_ns->user_ns);
    } else {
        ns->mnt_ns = orig->mnt_ns;
        ns->mnt_ns->ref_count++;
    }
    
    if (flags & CLONE_NEWNET) {
        ns->net_ns = net_namespace_create(orig->net_ns->user_ns);
    } else {
        ns->net_ns = orig->net_ns;
        ns->net_ns->ref_count++;
    }
    
    if (flags & CLONE_NEWUSER) {
        ns->pid_ns_for_children->user_ns = user_namespace_create(orig->pid_ns_for_children->user_ns);
    }
    
    spin_unlock(&ns_lists_lock);
    
    return ns;
}

void nsproxy_release(struct nsproxy *ns) {
    if (ns == NULL) return;
    
    spin_lock(&ns_lists_lock);
    
    if (ns->uts_ns && --ns->uts_ns->ref_count == 0) {
        list_del(&ns->uts_ns->list);
        kfree(ns->uts_ns);
    }
    
    if (ns->ipc_ns && --ns->ipc_ns->ref_count == 0) {
        list_del(&ns->ipc_ns->list);
        kfree(ns->ipc_ns);
    }
    
    if (ns->pid_ns_for_children && --ns->pid_ns_for_children->ref_count == 0) {
        list_del(&ns->pid_ns_for_children->list);
        kfree(ns->pid_ns_for_children);
    }
    
    if (ns->mnt_ns && --ns->mnt_ns->ref_count == 0) {
        list_del(&ns->mnt_ns->list);
        kfree(ns->mnt_ns);
    }
    
    if (ns->net_ns && --ns->net_ns->ref_count == 0) {
        list_del(&ns->net_ns->list);
        kfree(ns->net_ns);
    }
    
    spin_unlock(&ns_lists_lock);
    
    kfree(ns);
}

struct nsproxy *nsproxy_get_current(void) {
    struct process *proc = process_get_current();
    if (proc == NULL) return &default_nsproxy;
    
    /* TODO: 从进程获取 nsproxy */
    return &default_nsproxy;
}

/* ========== 创建命名空间 ========== */

struct uts_namespace *uts_namespace_create(struct user_namespace *user_ns) {
    struct uts_namespace *ns = kmalloc(sizeof(*ns));
    if (ns == NULL) return NULL;
    
    memset(ns, 0, sizeof(*ns));
    
    /* 复制父命名空间的值 */
    strcpy(ns->nodename, default_uts_ns.nodename);
    strcpy(ns->domainname, default_uts_ns.domainname);
    strcpy(ns->sysname, default_uts_ns.sysname);
    strcpy(ns->release, default_uts_ns.release);
    strcpy(ns->version, default_uts_ns.version);
    strcpy(ns->machine, default_uts_ns.machine);
    
    ns->ref_count = 1;
    ns->user_ns = user_ns;
    list_init(&ns->list);
    
    list_add(&ns->list, &uts_ns_list);
    
    return ns;
}

struct ipc_namespace *ipc_namespace_create(struct user_namespace *user_ns) {
    struct ipc_namespace *ns = kmalloc(sizeof(*ns));
    if (ns == NULL) return NULL;
    
    memset(ns, 0, sizeof(*ns));
    
    /* 复制默认值 */
    ns->next_id = 1;
    ns->msg_max = default_ipc_ns.msg_max;
    ns->msgmnb = default_ipc_ns.msgmnb;
    ns->msgmni = default_ipc_ns.msgmni;
    ns->shmmni = default_ipc_ns.shmmni;
    ns->shmmax = default_ipc_ns.shmmax;
    ns->shmall = default_ipc_ns.shmall;
    ns->semmni = default_ipc_ns.semmni;
    
    list_init(&ns->msg_queues);
    list_init(&ns->shm_segments);
    list_init(&ns->sem_arrays);
    
    ns->ref_count = 1;
    ns->user_ns = user_ns;
    list_init(&ns->list);
    
    list_add(&ns->list, &ipc_ns_list);
    
    return ns;
}

struct pid_namespace *pid_namespace_create(struct user_namespace *user_ns) {
    struct pid_namespace *ns = kmalloc(sizeof(*ns));
    if (ns == NULL) return NULL;
    
    memset(ns, 0, sizeof(*ns));
    
    ns->pid_next = 1;
    ns->pid_max = 32768;
    
    /* 获取父命名空间 */
    struct nsproxy *current_ns = nsproxy_get_current();
    ns->parent = current_ns ? current_ns->pid_ns_for_children : NULL;
    ns->level = ns->parent ? ns->parent->level + 1 : 0;
    
    ns->ref_count = 1;
    ns->user_ns = user_ns;
    
    list_init(&ns->children);
    list_init(&ns->list);
    
    /* 添加到父的子列表 */
    if (ns->parent) {
        list_add(&ns->sibling, &ns->parent->children);
    }
    
    list_add(&ns->list, &pid_ns_list);
    
    return ns;
}

struct mnt_namespace *mnt_namespace_create(struct user_namespace *user_ns) {
    struct mnt_namespace *ns = kmalloc(sizeof(*ns));
    if (ns == NULL) return NULL;
    
    memset(ns, 0, sizeof(*ns));
    
    list_init(&ns->mount_list);
    ns->nr_mounts = 0;
    ns->ref_count = 1;
    ns->user_ns = user_ns;
    list_init(&ns->list);
    
    list_add(&ns->list, &mnt_ns_list);
    
    return ns;
}

struct net_namespace *net_namespace_create(struct user_namespace *user_ns) {
    struct net_namespace *ns = kmalloc(sizeof(*ns));
    if (ns == NULL) return NULL;
    
    memset(ns, 0, sizeof(*ns));
    
    list_init(&ns->dev_list);
    ns->dev_count = 0;
    
    list_init(&ns->route_table);
    list_init(&ns->filter_rules);
    list_init(&ns->nat_rules);
    list_init(&ns->mangle_rules);
    
    ns->ref_count = 1;
    ns->user_ns = user_ns;
    list_init(&ns->list);
    
    list_add(&ns->list, &net_ns_list);
    
    return ns;
}

struct user_namespace *user_namespace_create(struct user_namespace *parent) {
    struct user_namespace *ns = kmalloc(sizeof(*ns));
    if (ns == NULL) return NULL;
    
    memset(ns, 0, sizeof(*ns));
    
    ns->parent = parent;
    ns->level = parent ? parent->level + 1 : 0;
    ns->ref_count = 1;
    
    /* 子命名空间继承父的能力 */
    ns->capabilities = parent ? parent->capabilities : 0xFFFFFFFFFFFFFFFFULL;
    
    list_init(&ns->children);
    list_init(&ns->list);
    
    /* 添加到父的子列表 */
    if (parent) {
        list_add(&ns->sibling, &parent->children);
    }
    
    list_add(&ns->list, &user_ns_list);
    
    return ns;
}

struct cgroup_namespace *cgroup_namespace_create(struct user_namespace *user_ns) {
    struct cgroup_namespace *ns = kmalloc(sizeof(*ns));
    if (ns == NULL) return NULL;
    
    memset(ns, 0, sizeof(*ns));
    
    ns->ref_count = 1;
    ns->user_ns = user_ns;
    list_init(&ns->list);
    
    list_add(&ns->list, &cgroup_ns_list);
    
    return ns;
}

/* ========== UID/GID 映射 ========== */

int user_namespace_map_uid(struct user_namespace *ns, uint32_t uid) {
    if (ns == NULL) return uid;
    
    /* 查找映射 */
    for (uint32_t i = 0; i < ns->uid_map.nr_extents; i++) {
        struct uid_gid_extent *ext = &ns->uid_map.extent[i];
        if (uid >= ext->first && uid < ext->first + ext->count) {
            return ext->lower_first + (uid - ext->first);
        }
    }
    
    /* 未找到映射，返回溢出 UID */
    return 65534;  /* overflowuid */
}

int user_namespace_map_gid(struct user_namespace *ns, uint32_t gid) {
    if (ns == NULL) return gid;
    
    /* 查找映射 */
    for (uint32_t i = 0; i < ns->gid_map.nr_extents; i++) {
        struct uid_gid_extent *ext = &ns->gid_map.extent[i];
        if (gid >= ext->first && gid < ext->first + ext->count) {
            return ext->lower_first + (gid - ext->first);
        }
    }
    
    /* 未找到映射，返回溢出 GID */
    return 65534;  /* overflowgid */
}

uint32_t user_namespace_unmap_uid(struct user_namespace *ns, uint32_t uid) {
    if (ns == NULL) return uid;
    
    /* 反向查找映射 */
    for (uint32_t i = 0; i < ns->uid_map.nr_extents; i++) {
        struct uid_gid_extent *ext = &ns->uid_map.extent[i];
        if (uid >= ext->lower_first && uid < ext->lower_first + ext->count) {
            return ext->first + (uid - ext->lower_first);
        }
    }
    
    return uid;
}

uint32_t user_namespace_unmap_gid(struct user_namespace *ns, uint32_t gid) {
    if (ns == NULL) return gid;
    
    /* 反向查找映射 */
    for (uint32_t i = 0; i < ns->gid_map.nr_extents; i++) {
        struct uid_gid_extent *ext = &ns->gid_map.extent[i];
        if (gid >= ext->lower_first && gid < ext->lower_first + ext->count) {
            return ext->first + (gid - ext->lower_first);
        }
    }
    
    return gid;
}

/* ========== 命名空间操作 ========== */

int ns_setns(int fd, int nstype) {
    /* TODO: 实现 setns */
    (void)fd;
    (void)nstype;
    return 0;
}

int ns_unshare(uint64_t flags) {
    struct nsproxy *old_ns = nsproxy_get_current();
    if (old_ns == NULL) return -1;
    
    struct nsproxy *new_ns = nsproxy_copy(old_ns, flags);
    if (new_ns == NULL) return -1;
    
    /* TODO: 设置到当前进程 */
    
    return 0;
}

int ns_enter(int fd, int nstype) {
    (void)fd;
    (void)nstype;
    return 0;
}

/* ========== 主机名操作 ========== */

int sys_sethostname(const char *name, size_t len) {
    struct uts_namespace *ns = uts_ns();
    if (ns == NULL) return -1;
    
    if (len >= sizeof(ns->nodename)) {
        return -1;
    }
    
    memcpy(ns->nodename, name, len);
    ns->nodename[len] = '\0';
    
    return 0;
}

int sys_gethostname(char *name, size_t len) {
    struct uts_namespace *ns = uts_ns();
    if (ns == NULL) return -1;
    
    size_t name_len = strlen(ns->nodename);
    if (len <= name_len) {
        return -1;
    }
    
    strcpy(name, ns->nodename);
    return 0;
}

int sys_setdomainname(const char *name, size_t len) {
    struct uts_namespace *ns = uts_ns();
    if (ns == NULL) return -1;
    
    if (len >= sizeof(ns->domainname)) {
        return -1;
    }
    
    memcpy(ns->domainname, name, len);
    ns->domainname[len] = '\0';
    
    return 0;
}

/* uname 系统调用 */
int sys_uname(void *buf) {
    struct uts_namespace *ns = uts_ns();
    if (ns == NULL) return -1;
    
    /* 复制到用户缓冲区 */
    memcpy(buf, ns, sizeof(*ns));
    return 0;
}
