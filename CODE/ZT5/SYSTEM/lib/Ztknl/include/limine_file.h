/**
 * Limine 文件协议接口
 */

#ifndef LIMINE_FILE_H
#define LIMINE_FILE_H

#include <stdint.h>
#include <stddef.h>

/* 初始化 Limine 文件系统 */
void limine_file_init(void);

/* 加载文件 (从模块) */
void *limine_load_file(const char *path, size_t *size_out);

/* 获取内核文件 */
void *limine_get_kernel_file(size_t *size_out);

/* 获取模块数量 */
int limine_get_module_count(void);

/* 获取指定索引的模块 */
void *limine_get_module(int index, size_t *size_out, const char **path_out);

/* 按路径查找模块 */
void *limine_find_module(const char *path, size_t *size_out);

#endif
