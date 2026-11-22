/*
 * Zratro OS - stdlib.h
 * 标准库头文件
 */

#ifndef _STDLIB_H
#define _STDLIB_H

#include "types.h"

/* 进程控制 */
#define EXIT_SUCCESS    0
#define EXIT_FAILURE    1

void exit(int status);
void abort(void);
int atexit(void (*func)(void));

/* 内存分配 */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

/* 字符串转换 */
int atoi(const char *nptr);
long atol(const char *nptr);
long long atoll(const char *nptr);
double atof(const char *nptr);

/* 随机数 */
#define RAND_MAX    32767

int rand(void);
void srand(unsigned int seed);

/* 环境 */
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *string);
int clearenv(void);

/* 排序和搜索 */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base,
              size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/* 绝对值 */
int abs(int j);
long labs(long j);
long long llabs(long long j);

/* 系统调用 */
int system(const char *command);

/* 数字转换 */
char *itoa(int value, char *str, int base);
char *ltoa(long value, char *str, int base);
char *lltoa(long long value, char *str, int base);
char *utoa(unsigned int value, char *str, int base);
char *ultoa(unsigned long value, char *str, int base);
char *ulltoa(unsigned long long value, char *str, int base);

/* 比较 */
int strverscmp(const char *s1, const char *s2);

#endif /* _STDLIB_H */
