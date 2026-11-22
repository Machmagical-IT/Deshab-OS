/*
 * Zratro OS - Safe String Operations Header
 * 
 * 增强容错性的字符串操作接口
 */

#ifndef STRING_H
#define STRING_H

#include <stddef.h>
#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * 内存操作 - 带空指针和边界检查
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 设置内存 */
void *memset(void *s, int c, size_t n);

/* 复制内存 */
void *memcpy(void *dest, const void *src, size_t n);

/* 移动内存 (处理重叠区域) */
void *memmove(void *dest, const void *src, size_t n);

/* 比较内存 */
int memcmp(const void *s1, const void *s2, size_t n);

/* ═══════════════════════════════════════════════════════════════════════════
 * 字符串操作 - 带空指针检查
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 字符串长度 */
size_t strlen(const char *s);

/* 安全字符串长度 (带最大限制) */
size_t strnlen(const char *s, size_t maxlen);

/* 字符串复制 */
char *strcpy(char *dest, const char *src);

/* 字符串复制 (最多 n 个字符) */
char *strncpy(char *dest, const char *src, size_t n);

/* 安全字符串复制 - 保证以 null 结尾 */
size_t strlcpy(char *dest, const char *src, size_t size);

/* 安全字符串连接 - 保证以 null 结尾 */
size_t strlcat(char *dest, const char *src, size_t size);

/* 字符串比较 */
int strcmp(const char *s1, const char *s2);

/* 字符串比较 (前 n 个字符) */
int strncmp(const char *s1, const char *s2, size_t n);

/* 字符串比较 (不区分大小写) */
int strcasecmp(const char *s1, const char *s2);

/* 查找字符 */
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);

/* 字符串分割 */
char *strtok(char *str, const char *delim);

/* 字符串连接 */
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);

/* 字符串查找 */
char *strstr(const char *haystack, const char *needle);

/* 格式化输出 */
int snprintf(char *str, size_t size, const char *fmt, ...);

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全内存检查
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 检查内存区域是否可读 */
int mem_is_readable(const void *ptr, size_t size);

/* 检查内存区域是否可写 */
int mem_is_writable(void *ptr, size_t size);

/* 恒定时间内存比较 (防止时序攻击) */
int memeq_ct(const void *a, const void *b, size_t n);

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全宏
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 安全复制宏 - 自动计算目标大小 */
#define SAFE_COPY(dest, src) \
    strlcpy((dest), (src), sizeof(dest))

/* 安全连接宏 - 自动计算目标大小 */
#define SAFE_CAT(dest, src) \
    strlcat((dest), (src), sizeof(dest))

/* 安全长度检查 */
#define SAFE_STRLEN(s) \
    ((s) != NULL ? strlen(s) : 0)

#endif /* STRING_H */
