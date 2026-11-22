/*
 * string.c - 字符串和内存操作工具函数
 */

#include "zsh.h"

/* ============================================
 * 字符串函数
 * ============================================ */

size_t str_len(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

int str_cmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int str_ncmp(const char *s1, const char *s2, size_t n)
{
    if (n == 0) return 0;

    while (n-- && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }

    if (n == (size_t)-1) return 0;
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

char* str_cpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}

char* str_ncpy(char *dest, const char *src, size_t n)
{
    char *d = dest;
    size_t i = 0;

    while (i < n && *src) {
        *d++ = *src++;
        i++;
    }

    while (i < n) {
        *d++ = '\0';
        i++;
    }

    return dest;
}

char* str_cat(char *dest, const char *src)
{
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++) != '\0');
    return dest;
}

char* str_chr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) {
            return (char*)s;
        }
        s++;
    }
    return NULL;
}

int str_cmp_case(const char *s1, const char *s2)
{
    unsigned char c1, c2;

    while (*s1 && *s2) {
        c1 = (*s1 >= 'A' && *s1 <= 'Z') ? *s1 + 32 : *s1;
        c2 = (*s2 >= 'A' && *s2 <= 'Z') ? *s2 + 32 : *s2;

        if (c1 != c2) {
            return c1 - c2;
        }
        s1++;
        s2++;
    }

    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

/* ============================================
 * 内存函数
 * ============================================ */

void* mem_set(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char*)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

void* mem_copy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char*)dest;
    const unsigned char *s = (const unsigned char*)src;

    while (n--) {
        *d++ = *s++;
    }

    return dest;
}

int mem_cmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = (const unsigned char*)s1;
    const unsigned char *p2 = (const unsigned char*)s2;

    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }

    return 0;
}

/* ============================================
 * 工具函数
 * ============================================ */

void int_to_str(int num, char *buf, int base)
{
    int i = 0;
    int negative = 0;

    if (num < 0 && base == 10) {
        negative = 1;
        num = -num;
    }

    if (num == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    while (num > 0) {
        int rem = num % base;
        buf[i++] = (rem < 10) ? (rem + '0') : (rem - 10 + 'A');
        num /= base;
    }

    if (negative) {
        buf[i++] = '-';
    }

    buf[i] = '\0';

    /* 反转字符串 */
    int start = 0;
    int end = i - 1;
    while (start < end) {
        char temp = buf[start];
        buf[start] = buf[end];
        buf[end] = temp;
        start++;
        end--;
    }
}

const char* get_username(void)
{
    return g_shell.username;
}

const char* get_hostname(void)
{
    return "zratro";
}

/* ============================================
 * 路径工具函数
 * ============================================ */

int path_is_dir(const char *path)
{
    struct zsh_stat st;
    if (sys_stat(path, &st) != 0) {
        return 0;
    }
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

int path_join(char *dest, const char *dir, const char *name, int dest_size)
{
    int dlen = str_len(dir);
    int nlen = str_len(name);

    /* 复制目录 */
    if (dlen >= dest_size - 1) {
        str_ncpy(dest, dir, dest_size - 1);
        dest[dest_size - 1] = '\0';
        return -1;
    }
    str_cpy(dest, dir);

    /* 添加分隔符 */
    if (dlen > 0 && dir[dlen - 1] != '/') {
        if (dlen + 1 >= dest_size - 1) {
            dest[dlen] = '\0';
            return -1;
        }
        dest[dlen] = '/';
        dest[dlen + 1] = '\0';
        dlen++;
    }

    /* 添加文件名 */
    if (dlen + nlen >= dest_size) {
        str_ncpy(dest + dlen, name, dest_size - dlen - 1);
        dest[dest_size - 1] = '\0';
        return -1;
    }
    str_cat(dest, name);

    return 0;
}

const char* path_basename(const char *path)
{
    const char *last_slash = path;
    const char *p = path;

    while (*p) {
        if (*p == '/') {
            last_slash = p + 1;
        }
        p++;
    }

    return last_slash;
}

int path_dirname(char *dest, const char *path, int dest_size)
{
    const char *last_slash = NULL;
    const char *p = path;

    while (*p) {
        if (*p == '/') {
            last_slash = p;
        }
        p++;
    }

    if (!last_slash || last_slash == path) {
        str_ncpy(dest, "/", dest_size);
        return 0;
    }

    int len = (int)(last_slash - path);
    if (len >= dest_size) len = dest_size - 1;
    str_ncpy(dest, path, len);
    dest[len] = '\0';

    /* 如果结果为空，设为 / */
    if (dest[0] == '\0') {
        str_cpy(dest, "/");
    }

    return 0;
}
