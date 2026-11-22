/*
 * ramfs.h - 内存文件系统
 * 
 * 用于在没有物理磁盘时提供基本的文件存储
 */

#ifndef RAMFS_H
#define RAMFS_H

#include "vfs.h"

/* RAMFS 初始化 */
int ramfs_init(void);

/* 挂载 ramfs 到指定路径 */
int ramfs_mount(const char *path);

/* 获取 ramfs 操作接口 */
struct vfs_ops *ramfs_get_ops(void);

#endif /* RAMFS_H */
