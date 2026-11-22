/*
 * fs_proxy.h - 文件系统 RPC 代理客户端
 *
 * Ztknl 通过共享内存 + IPI 调用 Azknl (Linux) 的文件系统
 */

#ifndef FS_PROXY_H
#define FS_PROXY_H

#include "vfs.h"

/* 获取 RPC VFS 操作接口 */
struct vfs_ops *fs_proxy_get_ops(void);

/* 初始化 FS Proxy */
int fs_proxy_init(void);

/* 检查代理是否可用 */
int fs_proxy_available(void);

#endif /* FS_PROXY_H */
