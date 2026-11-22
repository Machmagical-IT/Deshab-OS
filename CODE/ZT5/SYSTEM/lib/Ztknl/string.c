/*
 * Zratro OS - Safe String Operations
 * 
 * 增强容错性的字符串操作实现
 */

#include "string.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全内存操作 - 带空指针和边界检查
 * ═══════════════════════════════════════════════════════════════════════════ */

void *memset(void *s, int c, size_t n) {
    /* 空指针检查 */
    if (s == NULL) {
        return NULL;
    }
    
    /* 零长度优化 */
    if (n == 0) {
        return s;
    }
    
    uint8_t *p = (uint8_t *)s;
    
    /* 大块优化：按字填充 */
    if (n >= sizeof(uint64_t)) {
        /* 创建填充模式 */
        uint64_t pattern = (uint8_t)c;
        pattern |= pattern << 8;
        pattern |= pattern << 16;
        pattern |= pattern << 32;
        
        /* 按字填充 */
        while (n >= sizeof(uint64_t)) {
            *(uint64_t *)p = pattern;
            p += sizeof(uint64_t);
            n -= sizeof(uint64_t);
        }
    }
    
    /* 剩余字节 */
    while (n--) {
        *p++ = (uint8_t)c;
    }
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    /* 空指针检查 */
    if (dest == NULL || src == NULL) {
        return dest;
    }
    
    /* 零长度优化 */
    if (n == 0 || dest == src) {
        return dest;
    }
    
    /* 重叠检测 */
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    
    /* 如果源和目标重叠且源在前，需要从后向前复制 */
    if (d > s && d < s + n) {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
        return dest;
    }
    
    /* 大块优化：按字复制 */
    if (n >= sizeof(uint64_t) && ((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
        /* 对齐后按字复制 */
        while ((uintptr_t)d & 7) {
            *d++ = *s++;
            n--;
        }
        while (n >= sizeof(uint64_t)) {
            *(uint64_t *)d = *(const uint64_t *)s;
            d += sizeof(uint64_t);
            s += sizeof(uint64_t);
            n -= sizeof(uint64_t);
        }
    }
    
    /* 剩余字节 */
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    /* 空指针检查 */
    if (dest == NULL || src == NULL) {
        return dest;
    }
    
    /* 零长度或相同地址优化 */
    if (n == 0 || dest == src) {
        return dest;
    }
    
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    
    if (d < s) {
        /* 从前向后复制 */
        while (n--) {
            *d++ = *s++;
        }
    } else {
        /* 从后向前复制 */
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    /* 空指针检查 */
    if (s1 == NULL && s2 == NULL) {
        return 0;
    }
    if (s1 == NULL) {
        return -1;
    }
    if (s2 == NULL) {
        return 1;
    }
    
    /* 零长度 */
    if (n == 0) {
        return 0;
    }
    
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全字符串操作 - 带长度限制和空指针检查
 * ═══════════════════════════════════════════════════════════════════════════ */

size_t strlen(const char *s) {
    /* 空指针检查 */
    if (s == NULL) {
        return 0;
    }
    
    size_t len = 0;
    while (*s++) {
        len++;
        /* 防止无限循环 (最大 4GB) */
        if (len > 0xFFFFFFFFU) {
            return 0xFFFFFFFFU;
        }
    }
    return len;
}

size_t strnlen(const char *s, size_t maxlen) {
    /* 空指针检查 */
    if (s == NULL) {
        return 0;
    }
    
    size_t len = 0;
    while (len < maxlen && *s++) {
        len++;
    }
    return len;
}

char *strcpy(char *dest, const char *src) {
    /* 空指针检查 */
    if (dest == NULL || src == NULL) {
        return dest;
    }
    
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    /* 空指针检查 */
    if (dest == NULL || src == NULL || n == 0) {
        return dest;
    }
    
    char *d = dest;
    while (n > 0 && *src) {
        *d++ = *src++;
        n--;
    }
    while (n > 0) {
        *d++ = '\0';
        n--;
    }
    return dest;
}

/* 安全字符串复制 - 保证以 null 结尾 */
size_t strlcpy(char *dest, const char *src, size_t size) {
    /* 空指针检查 */
    if (dest == NULL || src == NULL) {
        return 0;
    }
    
    size_t src_len = strlen(src);
    
    if (size == 0) {
        return src_len;
    }
    
    size_t copy_len = (src_len < size - 1) ? src_len : size - 1;
    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
    
    return src_len;
}

/* 安全字符串连接 - 保证以 null 结尾 */
size_t strlcat(char *dest, const char *src, size_t size) {
    /* 空指针检查 */
    if (dest == NULL || src == NULL || size == 0) {
        return 0;
    }
    
    size_t dest_len = strnlen(dest, size);
    
    if (dest_len >= size) {
        return size + strlen(src);
    }
    
    size_t src_len = strlen(src);
    size_t copy_len = (src_len < size - dest_len - 1) ? src_len : size - dest_len - 1;
    
    memcpy(dest + dest_len, src, copy_len);
    dest[dest_len + copy_len] = '\0';
    
    return dest_len + src_len;
}

int strcmp(const char *s1, const char *s2) {
    /* 空指针检查 */
    if (s1 == NULL && s2 == NULL) {
        return 0;
    }
    if (s1 == NULL) {
        return -1;
    }
    if (s2 == NULL) {
        return 1;
    }
    
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *(const uint8_t *)s1 - *(const uint8_t *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    /* 空指针检查 */
    if (n == 0) {
        return 0;
    }
    if (s1 == NULL && s2 == NULL) {
        return 0;
    }
    if (s1 == NULL) {
        return -1;
    }
    if (s2 == NULL) {
        return 1;
    }
    
    while (n > 0 && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return *(const uint8_t *)s1 - *(const uint8_t *)s2;
}

/* 安全字符串比较（不区分大小写） */
int strcasecmp(const char *s1, const char *s2) {
    if (s1 == NULL && s2 == NULL) return 0;
    if (s1 == NULL) return -1;
    if (s2 == NULL) return 1;
    
    while (*s1 && *s2) {
        char c1 = *s1, c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return (uint8_t)c1 - (uint8_t)c2;
        s1++;
        s2++;
    }
    return (uint8_t)*s1 - (uint8_t)*s2;
}

char *strchr(const char *s, int c) {
    /* 空指针检查 */
    if (s == NULL) {
        return NULL;
    }
    
    while (*s != '\0') {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    if ((char)c == '\0') {
        return (char *)s;
    }
    return NULL;
}

char *strrchr(const char *s, int c) {
    /* 空指针检查 */
    if (s == NULL) {
        return NULL;
    }
    
    const char *last = NULL;
    while (*s != '\0') {
        if (*s == (char)c) {
            last = s;
        }
        s++;
    }
    if ((char)c == '\0') {
        return (char *)s;
    }
    return (char *)last;
}

char *strtok(char *str, const char *delim) {
    static char *saved = NULL;
    
    /* 空指针检查 */
    if (delim == NULL) {
        return NULL;
    }
    
    if (str != NULL) {
        saved = str;
    }
    
    if (saved == NULL || *saved == '\0') {
        return NULL;
    }
    
    /* 跳过前导分隔符 */
    while (*saved && strchr(delim, *saved)) {
        saved++;
    }
    
    if (*saved == '\0') {
        return NULL;
    }
    
    char *token = saved;
    
    /* 找到下一个分隔符 */
    while (*saved && !strchr(delim, *saved)) {
        saved++;
    }
    
    if (*saved) {
        *saved++ = '\0';
    }
    
    return token;
}

char *strcat(char *dest, const char *src) {
    /* 空指针检查 */
    if (dest == NULL || src == NULL) {
        return dest;
    }
    
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    /* 空指针检查 */
    if (dest == NULL || src == NULL || n == 0) {
        return dest;
    }
    
    char *d = dest;
    while (*d) d++;
    while (n-- > 0 && (*d++ = *src++));
    *d = '\0';
    return dest;
}

char *strstr(const char *haystack, const char *needle) {
    /* 空指针检查 */
    if (haystack == NULL) {
        return NULL;
    }
    if (needle == NULL || *needle == '\0') {
        return (char *)haystack;
    }
    
    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        
        if (*n == '\0') {
            return (char *)haystack;
        }
        
        haystack++;
    }
    
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 安全内存分配检查
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 安全的内存区域检查 */
int mem_is_readable(const void *ptr, size_t size) {
    if (ptr == NULL || size == 0) {
        return 0;
    }
    /* 简化实现：只检查空指针 */
    return 1;
}

int mem_is_writable(void *ptr, size_t size) {
    if (ptr == NULL || size == 0) {
        return 0;
    }
    /* 简化实现：只检查空指针 */
    return 1;
}

/* 安全内存比较（恒定时间） */
int memeq_ct(const void *a, const void *b, size_t n) {
    if (n == 0) return 1;
    if (a == NULL || b == NULL) return 0;
    
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    uint8_t result = 0;
    
    while (n--) {
        result |= *pa++ ^ *pb++;
    }
    
    return result == 0;
}
