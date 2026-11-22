/* ===========================================================================
 *  Nutro+OS - freestanding string / memory / formatting helpers
 * ===========================================================================
 */
#include "kernel.h"

void *memset(void *dst, int c, u32 n)
{
    u8 *d = (u8 *)dst;
    u8  v = (u8)c;
    while (n >= 4) { d[0] = v; d[1] = v; d[2] = v; d[3] = v; d += 4; n -= 4; }
    while (n--) *d++ = v;
    return dst;
}

void *memcpy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n >= 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; d += 4; s += 4; n -= 4; }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n >= 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; d += 4; s += 4; n -= 4; }
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n >= 4) { d -= 4; s -= 4; d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; n -= 4; }
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, u32 n)
{
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

u32 strlen(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, u32 n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) ;
    return dst;
}

char *strncpy(char *dst, const char *src, u32 n)
{
    u32 i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);
    while ((*d++ = *src++)) ;
    return dst;
}

void itoa(s32 v, char *buf)
{
    char tmp[12];
    int i = 0, neg = 0;
    u32 uv;
    if (v < 0) { neg = 1; uv = (u32)(-v); } else uv = (u32)v;
    if (uv == 0) tmp[i++] = '0';
    while (uv) { tmp[i++] = (char)('0' + uv % 10); uv /= 10; }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
}

void utoa(u32 v, char *buf)
{
    char tmp[12];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
}

void utohex(u32 v, char *buf)
{
    const char *digits = "0123456789ABCDEF";
    char tmp[9];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = digits[v & 15]; v >>= 4; }
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
}

/* --------------------------------------------------------------- printf -- */
typedef struct {
    char *buf;
    int   size;
    int   pos;
} out_t;

static void o_char(out_t *o, char c)
{
    if (o->pos < o->size - 1 && o->size > 0) o->buf[o->pos] = c;
    o->pos++;
}

static void o_pad(out_t *o, char c, int n)
{
    while (n-- > 0) o_char(o, c);
}

static void o_str(out_t *o, const char *s)
{
    while (*s) o_char(o, *s++);
}

static void o_num(out_t *o, u32 v, int base, int upper, int width, char pad)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[33];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = digits[v % base]; v /= base; }
    while (i < width) tmp[i++] = pad;
    while (i) o_char(o, tmp[--i]);
}

int kvsnprintf(char *buf, int size, const char *fmt, __builtin_va_list ap)
{
    out_t o;
    o.buf = buf;
    o.size = size;
    o.pos = 0;

    while (*fmt) {
        if (*fmt != '%') { o_char(&o, *fmt++); continue; }
        fmt++;
        if (*fmt == '%') { o_char(&o, '%'); fmt++; continue; }

        char pad = ' ';
        int  width = 0;
        while (*fmt == '0' || *fmt == '-') { if (*fmt == '0') pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        switch (*fmt) {
        case 'd': {
            s32 v = __builtin_va_arg(ap, s32);
            char tmp[16];
            itoa(v, tmp);
            if (width > 0) {
                int len = (int)strlen(tmp);
                o_pad(&o, pad, width - len);
            }
            o_str(&o, tmp);
            break;
        }
        case 'u': {
            u32 v = __builtin_va_arg(ap, u32);
            o_num(&o, v, 10, 0, width, pad);
            break;
        }
        case 'x': {
            u32 v = __builtin_va_arg(ap, u32);
            o_num(&o, v, 16, 0, width, pad);
            break;
        }
        case 'X': {
            u32 v = __builtin_va_arg(ap, u32);
            o_num(&o, v, 16, 1, width, pad);
            break;
        }
        case 'c': {
            int c = __builtin_va_arg(ap, int);
            o_char(&o, (char)c);
            break;
        }
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            o_str(&o, s);
            break;
        }
        default:
            o_char(&o, '%');
            if (*fmt) o_char(&o, *fmt);
            break;
        }
        if (*fmt) fmt++;
    }
    if (o.size > 0) {
        int end = o.pos < o.size - 1 ? o.pos : o.size - 1;
        buf[end] = 0;
    }
    return o.pos;
}

int ksnprintf(char *buf, int size, const char *fmt, ...)
{
    __builtin_va_list ap;
    int r;
    __builtin_va_start(ap, fmt);
    r = kvsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

/* ------------------------------------------------------- debug console --- */
/* Every debug character goes to port 0xE9 (QEMU debugcon) *and* to a small
 * buffer at physical 0x50000, which the test runner reads back with the
 * QEMU monitor ("xp /256xb 0x50000") - that survives a crashed guest. */
#define DBGLOG_ADDR 0x00050000u
#define DBGLOG_MAX  4096u

void debug_putc(char c)
{
    volatile char *log = (volatile char *)DBGLOG_ADDR;
    volatile u32 *pos = (volatile u32 *)(DBGLOG_ADDR + DBGLOG_MAX);
    if (*pos < DBGLOG_MAX - 1) {
        log[*pos] = c;
        (*pos)++;
        log[*pos] = 0;
    }
    outb(0xE9, (u8)c);
}

void debug_write(const char *s)
{
    while (*s) debug_putc(*s++);
}

void debug_printf(const char *fmt, ...)
{
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    debug_write(buf);
}