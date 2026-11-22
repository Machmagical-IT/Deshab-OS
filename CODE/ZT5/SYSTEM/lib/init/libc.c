/*
 * Zratro OS - C 库实现
 */

#include "types.h"
#include "syscall.h"
#include "unistd.h"
#include "signal.h"
#include "dirent.h"

/* ========== 字符串函数 ========== */

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    while (n--) {
        if (*p == (uint8_t)c) return (void *)p;
        p++;
    }
    return NULL;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    while (n && (*d++ = *src++)) n--;
    if (n == 0) *d = '\0';
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(const uint8_t *)s1 - *(const uint8_t *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n > 0 && *s1 && *s1 == *s2) { s1++; s2++; n--; }
    return n == 0 ? 0 : *(const uint8_t *)s1 - *(const uint8_t *)s2;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return c == '\0' ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char *)(c == '\0' ? s : last);
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    size_t len = 0;
    while (*s && strchr(accept, *s)) {
        s++;
        len++;
    }
    return len;
}

size_t strcspn(const char *s, const char *reject) {
    size_t len = 0;
    while (*s && !strchr(reject, *s)) {
        s++;
        len++;
    }
    return len;
}

char *strtok(char *str, const char *delim) {
    static char *saved = NULL;
    if (str) saved = str;
    if (!saved) return NULL;
    
    /* 跳过前导分隔符 */
    saved += strspn(saved, delim);
    if (!*saved) return NULL;
    
    char *token = saved;
    saved += strcspn(saved, delim);
    if (*saved) *saved++ = '\0';
    
    return token;
}

/* 字符函数 */
int isdigit(int c) {
    return c >= '0' && c <= '9';
}

int isalpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

int isupper(int c) {
    return c >= 'A' && c <= 'Z';
}

int islower(int c) {
    return c >= 'a' && c <= 'z';
}

int toupper(int c) {
    return islower(c) ? c - 'a' + 'A' : c;
}

int tolower(int c) {
    return isupper(c) ? c - 'A' + 'a' : c;
}

/* 转换函数 */
int atoi(const char *s) {
    int val = 0;
    int sign = 1;
    
    while (isspace(*s)) s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    
    while (isdigit(*s)) {
        val = val * 10 + (*s - '0');
        s++;
    }
    
    return sign * val;
}

long atol(const char *s) {
    long val = 0;
    int sign = 1;
    
    while (isspace(*s)) s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    
    while (isdigit(*s)) {
        val = val * 10 + (*s - '0');
        s++;
    }
    
    return sign * val;
}

char *itoa(int value, char *str, int base) {
    char *p = str;
    char *p1, *p2;
    int neg = 0;
    unsigned int uv;
    
    if (value < 0 && base == 10) {
        neg = 1;
        uv = -value;
    } else {
        uv = (unsigned int)value;
    }
    
    if (uv == 0) {
        *p++ = '0';
    } else {
        while (uv) {
            int rem = uv % base;
            *p++ = (rem < 10) ? rem + '0' : rem - 10 + 'a';
            uv /= base;
        }
    }
    
    if (neg) *p++ = '-';
    *p = '\0';
    
    /* 反转字符串 */
    p1 = str;
    p2 = p - 1;
    while (p1 < p2) {
        char tmp = *p1;
        *p1++ = *p2;
        *p2-- = tmp;
    }
    
    return str;
}

char *ltoa(long value, char *str, int base) {
    char *p = str;
    char *p1, *p2;
    int neg = 0;
    unsigned long uv;
    
    if (value < 0 && base == 10) {
        neg = 1;
        uv = -value;
    } else {
        uv = (unsigned long)value;
    }
    
    if (uv == 0) {
        *p++ = '0';
    } else {
        while (uv) {
            int rem = uv % base;
            *p++ = (rem < 10) ? rem + '0' : rem - 10 + 'a';
            uv /= base;
        }
    }
    
    if (neg) *p++ = '-';
    *p = '\0';
    
    p1 = str;
    p2 = p - 1;
    while (p1 < p2) {
        char tmp = *p1;
        *p1++ = *p2;
        *p2-- = tmp;
    }
    
    return str;
}

/* ========== I/O 函数 ========== */

int puts(const char *s) {
    size_t len = strlen(s);
    sys_write(1, s, len);
    sys_write(1, "\n", 1);
    return len + 1;
}

int putchar(int c) {
    char ch = (char)c;
    sys_write(1, &ch, 1);
    return c;
}

int fputs(const char *s, int fd) {
    return sys_write(fd, s, strlen(s));
}

int fputc(int c, int fd) {
    char ch = (char)c;
    return sys_write(fd, &ch, 1);
}

int fputs_stdout(const char *s) {
    return sys_write(STDOUT_FILENO, s, strlen(s));
}

int fputs_stderr(const char *s) {
    return sys_write(STDERR_FILENO, s, strlen(s));
}

void print_int(int64_t val) {
    char buf[21];
    int i = 20;
    int neg = val < 0;
    if (neg) val = -val;
    buf[i] = 0;
    if (val == 0) buf[--i] = '0';
    else while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    if (neg) buf[--i] = '-';
    sys_write(1, &buf[i], 20 - i);
}

void print_uint(uint64_t val) {
    char buf[21];
    int i = 20;
    buf[i] = 0;
    if (val == 0) buf[--i] = '0';
    else while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    sys_write(1, &buf[i], 20 - i);
}

void print_hex(uint64_t val) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
    for (int i = 15; i >= 0; i--) { buf[i] = hex[val & 0xF]; val >>= 4; }
    buf[16] = 0;
    sys_write(1, "0x", 2);
    sys_write(1, buf, 16);
}

void print_oct(uint64_t val) {
    char buf[23];
    int i = 22;
    buf[i] = 0;
    if (val == 0) buf[--i] = '0';
    else while (val > 0) { buf[--i] = '0' + (val % 8); val /= 8; }
    sys_write(1, "0", 1);
    sys_write(1, &buf[i], 22 - i);
}

int printf(const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int count = 0;
    
    while (*fmt) {
        if (*fmt == '%' && *++fmt) {
            /* 处理长度修饰符 */
            int lmod = 0;
            if (*fmt == 'l') { lmod = 1; fmt++; }
            if (*fmt == 'l') { lmod = 2; fmt++; }
            
            switch (*fmt) {
                case 'd':
                case 'i': {
                    int64_t val = lmod ? __builtin_va_arg(args, int64_t) : __builtin_va_arg(args, int);
                    char buf[21];
                    int i = 20;
                    int neg = val < 0;
                    if (neg) val = -val;
                    buf[i] = 0;
                    if (val == 0) buf[--i] = '0';
                    else while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
                    if (neg) buf[--i] = '-';
                    sys_write(1, &buf[i], 20 - i);
                    count += 20 - i;
                    break;
                }
                case 'u': {
                    uint64_t val = lmod ? __builtin_va_arg(args, uint64_t) : __builtin_va_arg(args, unsigned int);
                    char buf[21];
                    int i = 20;
                    buf[i] = 0;
                    if (val == 0) buf[--i] = '0';
                    else while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
                    sys_write(1, &buf[i], 20 - i);
                    count += 20 - i;
                    break;
                }
                case 'x':
                case 'X': {
                    uint64_t val = __builtin_va_arg(args, uint64_t);
                    static const char hex_lower[] = "0123456789abcdef";
                    static const char hex_upper[] = "0123456789ABCDEF";
                    const char *hex = (*fmt == 'x') ? hex_lower : hex_upper;
                    char buf[17];
                    for (int j = 15; j >= 0; j--) { buf[j] = hex[val & 0xF]; val >>= 4; }
                    buf[16] = 0;
                    sys_write(1, buf, 16);
                    count += 16;
                    break;
                }
                case 'p': {
                    uint64_t val = (uint64_t)__builtin_va_arg(args, void*);
                    static const char hex[] = "0123456789abcdef";
                    char buf[17];
                    sys_write(1, "0x", 2);
                    for (int j = 15; j >= 0; j--) { buf[j] = hex[val & 0xF]; val >>= 4; }
                    buf[16] = 0;
                    sys_write(1, buf, 16);
                    count += 18;
                    break;
                }
                case 'o': {
                    uint64_t val = __builtin_va_arg(args, uint64_t);
                    char buf[23];
                    int i = 22;
                    buf[i] = 0;
                    if (val == 0) buf[--i] = '0';
                    else while (val > 0) { buf[--i] = '0' + (val % 8); val /= 8; }
                    sys_write(1, &buf[i], 22 - i);
                    count += 22 - i;
                    break;
                }
                case 's': {
                    const char *s = __builtin_va_arg(args, const char*);
                    if (!s) s = "(null)";
                    size_t len = strlen(s);
                    sys_write(1, s, len);
                    count += len;
                    break;
                }
                case 'c': {
                    char c = (char)__builtin_va_arg(args, int);
                    sys_write(1, &c, 1);
                    count++;
                    break;
                }
                case '%':
                    sys_write(1, "%", 1);
                    count++;
                    break;
                default:
                    sys_write(1, "%", 1);
                    sys_write(1, fmt, 1);
                    count += 2;
                    break;
            }
        } else {
            sys_write(1, fmt, 1);
            count++;
        }
        fmt++;
    }
    
    __builtin_va_end(args);
    return count;
}

int sprintf(char *str, const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int count = 0;
    
    while (*fmt) {
        if (*fmt == '%' && *++fmt) {
            int lmod = 0;
            if (*fmt == 'l') { lmod = 1; fmt++; }
            if (*fmt == 'l') { lmod = 2; fmt++; }
            
            switch (*fmt) {
                case 'd':
                case 'i': {
                    int64_t val = lmod ? __builtin_va_arg(args, int64_t) : __builtin_va_arg(args, int);
                    char buf[21];
                    int i = 20;
                    int neg = val < 0;
                    if (neg) val = -val;
                    buf[i] = 0;
                    if (val == 0) buf[--i] = '0';
                    else while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
                    if (neg) buf[--i] = '-';
                    strcpy(str + count, &buf[i]);
                    count += 20 - i;
                    break;
                }
                case 'u': {
                    uint64_t val = lmod ? __builtin_va_arg(args, uint64_t) : __builtin_va_arg(args, unsigned int);
                    char buf[21];
                    int i = 20;
                    buf[i] = 0;
                    if (val == 0) buf[--i] = '0';
                    else while (val > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
                    strcpy(str + count, &buf[i]);
                    count += 20 - i;
                    break;
                }
                case 'x':
                case 'X': {
                    uint64_t val = __builtin_va_arg(args, uint64_t);
                    static const char hex_lower[] = "0123456789abcdef";
                    static const char hex_upper[] = "0123456789ABCDEF";
                    const char *hex = (*fmt == 'x') ? hex_lower : hex_upper;
                    for (int j = 0; j < 16; j++) {
                        str[count + 15 - j] = hex[val & 0xF];
                        val >>= 4;
                    }
                    str[count + 16] = 0;
                    count += 16;
                    break;
                }
                case 's': {
                    const char *s = __builtin_va_arg(args, const char*);
                    if (!s) s = "(null)";
                    strcpy(str + count, s);
                    count += strlen(s);
                    break;
                }
                case 'c': {
                    str[count++] = (char)__builtin_va_arg(args, int);
                    break;
                }
                case '%':
                    str[count++] = '%';
                    break;
            }
        } else {
            str[count++] = *fmt;
        }
        fmt++;
    }
    str[count] = '\0';
    
    __builtin_va_end(args);
    return count;
}

char *gets(char *s, int size) {
    int i = 0;
    char c;
    
    while (i < size - 1) {
        if (sys_read(STDIN_FILENO, &c, 1) <= 0) break;
        if (c == '\n' || c == '\r') break;
        s[i++] = c;
    }
    
    s[i] = '\0';
    return s;
}

/* ========== 信号函数 ========== */

sighandler_t signal(int sig, sighandler_t handler) {
    return (sighandler_t)syscall2(SYS_SIGNAL, sig, (uint64_t)handler);
}

int raise(int sig) {
    return kill(getpid(), sig);
}

/* ========== 目录函数 ========== */

DIR *opendir(const char *name) {
    int fd = sys_open(name, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return NULL;
    
    DIR *dir = (DIR *)sys_mmap(NULL, sizeof(DIR), PROT_READ | PROT_WRITE, 
                                MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (dir == NULL || dir == (void *)-1) {
        sys_close(fd);
        return NULL;
    }
    
    dir->fd = fd;
    dir->eof = 0;
    return dir;
}

struct dirent *readdir(DIR *dir) {
    if (dir == NULL || dir->eof) return NULL;
    
    int ret = (int)syscall2(SYS_READDIR, dir->fd, (uint64_t)&dir->entry);
    if (ret <= 0) {
        dir->eof = 1;
        return NULL;
    }
    
    return &dir->entry;
}

int closedir(DIR *dir) {
    if (dir == NULL) return -1;
    int ret = sys_close(dir->fd);
    sys_munmap(dir, sizeof(DIR));
    return ret;
}

/* ========== 环境变量 ========== */

#define MAX_ENV_VARS    64
#define MAX_ENV_LEN     256

static char env_vars[MAX_ENV_VARS][MAX_ENV_LEN];
static char *env_ptrs[MAX_ENV_VARS + 1];
static int env_count = 0;
static int env_initialized = 0;

/* 全局环境变量指针 */
char **environ = NULL;

static void init_environ(void) {
    if (env_initialized) return;
    
    /* 设置默认环境变量 */
    env_ptrs[0] = "PATH=/bin:/usr/bin";
    env_ptrs[1] = "HOME=/root";
    env_ptrs[2] = "USER=root";
    env_ptrs[3] = "SHELL=/bin/shell";
    env_ptrs[4] = NULL;
    env_count = 4;
    environ = env_ptrs;
    env_initialized = 1;
}

char *getenv(const char *name) {
    init_environ();
    
    size_t len = strlen(name);
    for (int i = 0; i < env_count; i++) {
        if (strncmp(env_ptrs[i], name, len) == 0 && env_ptrs[i][len] == '=') {
            return env_ptrs[i] + len + 1;
        }
    }
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite) {
    init_environ();
    
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    
    /* 查找现有变量 */
    for (int i = 0; i < env_count; i++) {
        if (strncmp(env_ptrs[i], name, name_len) == 0 && env_ptrs[i][name_len] == '=') {
            if (!overwrite) return 0;
            /* 更新现有变量 */
            if (name_len + 1 + value_len < MAX_ENV_LEN) {
                strcpy(env_vars[i], name);
                strcat(env_vars[i], "=");
                strcat(env_vars[i], value);
                env_ptrs[i] = env_vars[i];
                return 0;
            }
            return -1;
        }
    }
    
    /* 添加新变量 */
    if (env_count >= MAX_ENV_VARS - 1) return -1;
    
    if (name_len + 1 + value_len < MAX_ENV_LEN) {
        strcpy(env_vars[env_count], name);
        strcat(env_vars[env_count], "=");
        strcat(env_vars[env_count], value);
        env_ptrs[env_count] = env_vars[env_count];
        env_count++;
        env_ptrs[env_count] = NULL;
        return 0;
    }
    
    return -1;
}

int unsetenv(const char *name) {
    init_environ();
    
    size_t len = strlen(name);
    for (int i = 0; i < env_count; i++) {
        if (strncmp(env_ptrs[i], name, len) == 0 && env_ptrs[i][len] == '=') {
            /* 移动后续变量 */
            for (int j = i; j < env_count - 1; j++) {
                env_ptrs[j] = env_ptrs[j + 1];
            }
            env_ptrs[--env_count] = NULL;
            return 0;
        }
    }
    return 0;
}

/* ========== 其他工具函数 ========== */

/* 错误信息 */
static const char *error_msgs[] = {
    "Success",
    "Operation not permitted",
    "No such file or directory",
    "No such process",
    "Interrupted system call",
    "I/O error",
    "No such device or address",
    "Argument list too long",
    "Exec format error",
    "Bad file number",
    "No child processes",
    "Try again",
    "Out of memory",
    "Permission denied",
    "Bad address",
    "Block device required",
    "Device or resource busy",
    "File exists",
    "No such device",
    "Not a directory",
    "Is a directory",
    "Invalid argument",
    "File table overflow",
    "Too many open files",
    "Not a typewriter",
    "Text file busy",
    "File too large",
    "No space left on device",
    "Illegal seek",
    "Read-only file system",
    "Too many links",
    "Broken pipe"
};

static int errno = 0;

const char *strerror(int errnum) {
    if (errnum >= 0 && errnum < (int)(sizeof(error_msgs) / sizeof(error_msgs[0]))) {
        return error_msgs[errnum];
    }
    return "Unknown error";
}

/* ========== 栈保护 (Stack Canary) - 与内核 __stack_chk_fail 对接 ========== */

/* 全局栈保护符 - 由内核在进程创建时设置 */
static unsigned long __stack_chk_guard = 0;

/* 获取内核设置的 canary 值（弱符号，可由内核覆盖） */
unsigned long __get_stack_chk_guard(void) {
    /* TODO: 从内核 syscall 获取随机 canary 值 */
    if (__stack_chk_guard == 0) {
        /* 简化：使用固定值 */
        __stack_chk_guard = 0xDEADBEEFCAFEBABEUL;
    }
    return __stack_chk_guard;
}

/* 栈溢出检测失败处理 - 调用内核 sys_exit */
__attribute__((noreturn))
void __stack_chk_fail(void) {
    /* 栈溢出！立即终止进程 */
    extern void sys_exit(int status);
    sys_exit(127);  /* 特殊退出码表示栈溢出 */
    
    /* 永不返回 */
    for (;;);
}

/* 本地版本（用于某些编译器优化） */
__attribute__((noreturn))
void __stack_chk_fail_local(void) {
    __stack_chk_fail();
}

void perror(const char *s) {
    if (s && *s) {
        sys_write(STDERR_FILENO, s, strlen(s));
        sys_write(STDERR_FILENO, ": ", 2);
    }
    sys_write(STDERR_FILENO, strerror(errno), strlen(strerror(errno)));
    sys_write(STDERR_FILENO, "\n", 1);
}

/* 堆管理 (简化实现) */
static uint8_t *heap_brk = NULL;
static uint8_t *heap_end = NULL;

void *malloc(size_t size) {
    if (size == 0) return NULL;
    
    /* 对齐到 16 字节 */
    size = (size + 15) & ~15;
    
    /* 初始化堆 */
    if (heap_brk == NULL) {
        heap_brk = (uint8_t *)sys_mmap(NULL, 4096 * 1024, PROT_READ | PROT_WRITE,
                                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (heap_brk == (void *)-1) return NULL;
        heap_end = heap_brk + 4096 * 1024;
    }
    
    /* 简单线性分配 */
    if (heap_brk + size > heap_end) {
        /* 需要扩展堆 */
        size_t expand = (size + 4095) & ~4095;
        void *new_mem = sys_mmap(heap_end, expand, PROT_READ | PROT_WRITE,
                                  MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0);
        if (new_mem == (void *)-1) return NULL;
        heap_end += expand;
    }
    
    void *ptr = heap_brk;
    heap_brk += size;
    return ptr;
}

void free(void *ptr) {
    /* 简化实现：不释放内存 */
    (void)ptr;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    
    /* 简化实现：总是分配新内存 */
    void *new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, size);  /* 可能复制过多，但安全 */
        free(ptr);
    }
    return new_ptr;
}

/* 文件状态 (简化) */
struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
};

int stat(const char *path, struct stat *buf) {
    return (int)syscall2(SYS_STAT, (uint64_t)path, (uint64_t)buf);
}

int fstat(int fd, struct stat *buf) {
    return -1;  /* 未实现 */
}

/* 文件类型检查宏 */
#define S_ISDIR(mode)   (((mode) & 0170000) == 0040000)
#define S_ISREG(mode)   (((mode) & 0170000) == 0100000)
#define S_ISCHR(mode)   (((mode) & 0170000) == 0020000)
#define S_ISBLK(mode)   (((mode) & 0170000) == 0060000)
#define S_ISFIFO(mode)  (((mode) & 0170000) == 0010000)
#define S_ISLNK(mode)   (((mode) & 0170000) == 0120000)
#define S_ISSOCK(mode)  (((mode) & 0170000) == 0140000)

/* 权限位 */
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

/* 时间函数 */
#include "time.h"

time_t time(time_t *tloc) {
    time_t t = (time_t)sys_time();
    if (tloc) *tloc = t;
    return t;
}

clock_t clock(void) {
    return (clock_t)sys_uptime();
}