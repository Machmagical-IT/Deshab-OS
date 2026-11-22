#ifndef STDDEF_H
#define STDDEF_H

/* 标准类型定义 */
typedef long ptrdiff_t;
typedef unsigned long size_t;
typedef long ssize_t;

/* NULL 指针 */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* 容器宏 */
#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
