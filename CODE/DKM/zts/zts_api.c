/* zts_api.c — ZTS 子系统基础设施：日志/内存/锁/API 表/符号表
 *
 * 模块通过 `extern zts_api_table *zts_api;` 访问宿主能力。API 表前 13 项与
 * ZT5 zts_api_table 前缀兼容；扩展项（bus_probe/fs_read_file/time_ms/crc32）
 * 在 ext_version >= 1 时可用。
 */
#include "zts.h"
#include <stdarg.h>

/* clang 会把大结构体赋值/清零降级为 memcpy/memset，把 __builtin_snprintf
 * 降级为 snprintf 调用。DKM 驱动没有 libc，必须在本驱动内提供这些符号，
 * 否则 DKM loader 的 PLT32 解析会落到 0/低地址（曾在 guest 测试中表现为
 * #UD rip=0xa0000）。 */
void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);
int snprintf(char *buf, unsigned long cap, const char *fmt, ...);

static const struct dkm_kernel_api *g_kapi;
static const struct dkm_log_api *g_log;
static zts_api_table g_api_tbl;
static volatile int g_lock;

/* 模块 ELF 的未定义符号 "zts_api" 解析到这里（指针变量）。 */
zts_api_table *zts_api;

/* ============================================================
 * 基础运行时
 * ============================================================ */
void *zts_memcpy(void *dst, const void *src, u64 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *zts_memset(void *dst, int c, u64 n) {
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < n; i++) d[i] = (u8)c;
    return dst;
}

int zts_strcmp(const char *a, const char *b) {
    if (!a || !b) return a ? 1 : (b ? -1 : 0);
    while (*a && *b) {
        if (*a != *b) return (u8)*a - (u8)*b;
        a++; b++;
    }
    return (u8)*a - (u8)*b;
}

u64 zts_strlen(const char *s) {
    u64 n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

int zts_str_eq(const char *a, const char *b) {
    return zts_strcmp(a, b) == 0;
}

void zts_str_copy(char *dst, const char *src, u32 cap) {
    u32 i = 0;
    if (!cap) return;
    if (!src) { dst[0] = 0; return; }
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

u32 zts_crc32(const void *data, u64 size) {
    const u8 *p = (const u8 *)data;
    u32 crc = 0xFFFFFFFFu;
    for (u64 i = 0; i < size; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            u32 mask = (u32)-(int)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

u64 zts_fnv1a64(const void *data, u64 size) {
    const u8 *p = (const u8 *)data;
    u64 h = 14695981039346656037ULL;
    for (u64 i = 0; i < size; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

u32 zts_fnv1a32(const void *data, u64 size) {
    const u8 *p = (const u8 *)data;
    u32 h = 2166136261u;
    for (u64 i = 0; i < size; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* ============================================================
 * 日志
 * ============================================================ */
static u32 u64_to_dec(u64 v, char *out) {
    char tmp[24];
    u32 n = 0, i = 0;
    if (!v) { out[0] = '0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) out[i++] = tmp[--n];
    return i;
}

static void u64_to_hex(u64 v, char *out) {
    static const char h[] = "0123456789abcdef";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) out[2 + i] = h[(v >> (60 - i * 4)) & 0xF];
    out[18] = 0;
}

void zts_log(const char *msg) {
    if (g_log && msg) g_log->info(msg);
}

void zts_log_hex(const char *prefix, u64 v) {
    char buf[96];
    u32 n = 0;
    if (prefix) {
        while (prefix[n] && n < 70) { buf[n] = prefix[n]; n++; }
    }
    u64_to_hex(v, buf + n);
    if (g_log) g_log->info(buf);
}

void zts_log_dec(const char *prefix, u64 v) {
    char buf[96];
    u32 n = 0;
    if (prefix) {
        while (prefix[n] && n < 80) { buf[n] = prefix[n]; n++; }
    }
    n += u64_to_dec(v, buf + n);
    buf[n] = 0;
    if (g_log) g_log->info(buf);
}

u64 zts_now_ms(void) {
    if (g_kapi && g_kapi->sched && g_kapi->sched->uptime_ns) {
        return g_kapi->sched->uptime_ns() / 1000000ULL;
    }
    return 0;
}

/* ============================================================
 * 锁（单核 + IRQ 可能重入；用原子测试置位）
 * ============================================================ */
/* 短临界区自旋锁：只保护 zts 内部结构，不改变中断状态。
 * 模块执行路径不持有该锁，避免外部代码在锁内长时间运行。 */
u32 zts_lock(void) {
    while (__atomic_test_and_set(&g_lock, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
    return 0;
}

void zts_unlock(void) {
    __atomic_clear(&g_lock, __ATOMIC_RELEASE);
}

void zts_unlock_restore(u32 irq) {
    (void)irq;
    __atomic_clear(&g_lock, __ATOMIC_RELEASE);
}

/* ============================================================
 * 堆（1MB，模块运行期 alloc/free）
 * ============================================================ */
#define ZTS_HEAP_SIZE (1024u * 1024u)

typedef struct zts_block {
    u32 size;              /* 可用负载大小 */
    u32 free;
    struct zts_block *next;
    struct zts_block *prev;
} zts_block;

static u8 g_heap[ZTS_HEAP_SIZE] __attribute__((aligned(16)));
static zts_block *g_heap_head;

static void heap_init(void) {
    g_heap_head = (zts_block *)g_heap;
    g_heap_head->size = ZTS_HEAP_SIZE - (u32)sizeof(zts_block);
    g_heap_head->free = 1;
    g_heap_head->next = 0;
    g_heap_head->prev = 0;
}

void *zts_alloc(u64 size) {
    if (!size || size > ZTS_HEAP_SIZE) return 0;
    u32 want = (u32)size;
    want = (want + 15u) & ~15u;
    u32 irq = zts_lock();
    zts_block *b = g_heap_head;
    while (b) {
        if (b->free && b->size >= want) break;
        b = b->next;
    }
    if (!b) { zts_unlock_restore(irq); return 0; }
    if (b->size > want + sizeof(zts_block) + 16u) {
        zts_block *nb = (zts_block *)((u8 *)(b + 1) + want);
        nb->size = b->size - want - (u32)sizeof(zts_block);
        nb->free = 1;
        nb->next = b->next;
        nb->prev = b;
        if (b->next) b->next->prev = nb;
        b->next = nb;
        b->size = want;
    }
    b->free = 0;
    zts_unlock_restore(irq);
    return (void *)(b + 1);
}

void zts_free(void *ptr) {
    if (!ptr) return;
    zts_block *b = ((zts_block *)ptr) - 1;
    if ((u8 *)b < g_heap || (u8 *)b >= g_heap + ZTS_HEAP_SIZE) return;
    u32 irq = zts_lock();
    b->free = 1;
    /* 向后合并 */
    if (b->next && b->next->free) {
        b->size += (u32)sizeof(zts_block) + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    /* 向前合并 */
    if (b->prev && b->prev->free) {
        zts_block *p = b->prev;
        p->size += (u32)sizeof(zts_block) + b->size;
        p->next = b->next;
        if (p->next) p->next->prev = p;
    }
    zts_unlock_restore(irq);
}

void *zts_realloc(void *ptr, u64 size) {
    if (!ptr) return zts_alloc(size);
    if (!size) { zts_free(ptr); return 0; }
    zts_block *b = ((zts_block *)ptr) - 1;
    u64 old = b->size;
    if (old >= size) return ptr;
    void *n = zts_alloc(size);
    if (!n) return 0;
    zts_memcpy(n, ptr, old);
    zts_free(ptr);
    return n;
}

/* ============================================================
 * 模块 VFS 后端（fat32 只读 + ZTS 目录写）
 * ============================================================ */
#define ZTS_MAX_FD 8
typedef struct {
    u8 *data;
    u32 size;
    u32 pos;
    int used;
} zts_fd;
static zts_fd g_fds[ZTS_MAX_FD];

static int api_vfs_open(const char *path, u32 mode) {
    (void)mode;
    if (!path) return -1;
    u8 *data = 0;
    u32 size = 0;
    if (zts_store_read(path, &data, &size) != 0) return -1;
    u8 *copy = (u8 *)zts_alloc(size ? size : 1);
    if (!copy) return -1;
    zts_memcpy(copy, data, size);
    for (int i = 0; i < ZTS_MAX_FD; i++) {
        if (!g_fds[i].used) {
            g_fds[i].used = 1;
            g_fds[i].data = copy;
            g_fds[i].size = size;
            g_fds[i].pos = 0;
            return i;
        }
    }
    zts_free(copy);
    return -1;
}

static int api_vfs_close(int fd) {
    if (fd < 0 || fd >= ZTS_MAX_FD || !g_fds[fd].used) return -1;
    zts_free(g_fds[fd].data);
    g_fds[fd].used = 0;
    return 0;
}

static int api_vfs_read(int fd, void *buf, u64 count) {
    if (fd < 0 || fd >= ZTS_MAX_FD || !g_fds[fd].used || !buf) return -1;
    zts_fd *f = &g_fds[fd];
    u32 left = f->size - f->pos;
    u32 n = (count < left) ? (u32)count : left;
    zts_memcpy(buf, f->data + f->pos, n);
    f->pos += n;
    return (int)n;
}

static int api_vfs_write(int fd, const void *buf, u64 count) {
    (void)fd; (void)buf; (void)count;
    /* 写路径只开放给 srv.zts.*（ZTS 目录 A/B 槽）；模块自写盘属于设计外能力。 */
    return -1;
}

static void api_log_print(const char *level, const char *msg) {
    if (!msg) return;
    if (level && zts_str_eq(level, "error")) { if (g_log) g_log->error(msg); }
    else if (level && zts_str_eq(level, "warn")) { if (g_log) g_log->warn(msg); }
    else { if (g_log) g_log->info(msg); }
}

static int api_bus_probe(const char *name, void **out_fn) {
    if (!g_kapi || !g_kapi->bus || !name) return -1;
    return g_kapi->bus->probe(name, out_fn, 0);
}

static int api_fs_read(const char *path, u8 **out, u32 *size) {
    return zts_store_read(path, out, size);
}

static u32 api_crc32(const void *data, u64 size) {
    return zts_crc32(data, size);
}

void zts_core_init(const struct dkm_kernel_api *api, const struct dkm_log_api *log) {
    g_kapi = api;
    g_log = log;
    g_lock = 0;
    heap_init();
    zts_memset(g_fds, 0, sizeof(g_fds));

    g_api_tbl.api_version = ZTS_API_VERSION;
    g_api_tbl.alloc = zts_alloc;
    g_api_tbl.free = zts_free;
    g_api_tbl.realloc = zts_realloc;
    g_api_tbl.log_print = api_log_print;
    g_api_tbl.time_now = zts_now_ms;
    g_api_tbl.vfs_open = api_vfs_open;
    g_api_tbl.vfs_close = api_vfs_close;
    g_api_tbl.vfs_read = api_vfs_read;
    g_api_tbl.vfs_write = api_vfs_write;
    g_api_tbl.net_send = 0;
    g_api_tbl.net_recv = 0;
    g_api_tbl.strcmp = zts_strcmp;
    g_api_tbl.memcpy = zts_memcpy;
    g_api_tbl.memset = zts_memset;
    g_api_tbl.ext_version = 1;
    g_api_tbl.bus_probe = api_bus_probe;
    g_api_tbl.fs_read_file = api_fs_read;
    g_api_tbl.time_ms = zts_now_ms;
    g_api_tbl.crc32 = api_crc32;
    zts_api = &g_api_tbl;
}

const struct dkm_kernel_api *zts_kapi(void) { return g_kapi; }
const struct dkm_log_api *zts_log_api(void) { return g_log; }
zts_api_table *zts_api_get(void) { return &g_api_tbl; }

/* ============================================================
 * 符号表（模块 ELF 未定义符号解析）
 * ============================================================ */
#define ZTS_SYMS 128
typedef struct {
    char name[ZTS_MAX_NAME];
    u64 addr;
    int used;
} zts_sym;
static zts_sym g_syms[ZTS_SYMS];
static int g_sym_count;

int zts_symbol_register(const char *name, u64 addr) {
    if (!name || !addr) return -1;
    for (int i = 0; i < g_sym_count; i++) {
        if (zts_str_eq(g_syms[i].name, name)) {
            g_syms[i].addr = addr;
            return 0;
        }
    }
    if (g_sym_count >= ZTS_SYMS) return -1;
    zts_str_copy(g_syms[g_sym_count].name, name, ZTS_MAX_NAME);
    g_syms[g_sym_count].addr = addr;
    g_syms[g_sym_count].used = 1;
    g_sym_count++;
    return 0;
}

u64 zts_symbol_lookup(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < g_sym_count; i++) {
        if (zts_str_eq(g_syms[i].name, name)) return g_syms[i].addr;
    }
    return 0;
}

void zts_symbols_init(void);

/* ============================================================
 * libc 替代符号（DKM 驱动内定义，供编译器降级调用解析）
 * ============================================================ */
void *memcpy(void *dst, const void *src, unsigned long n) {
    return zts_memcpy(dst, src, n);
}

void *memset(void *dst, int c, unsigned long n) {
    return zts_memset(dst, c, n);
}

static int fmt_put(char *buf, unsigned long cap, unsigned long *pos, char c) {
    if (*pos + 1 < cap) buf[*pos] = c;
    (*pos)++;
    return 0;
}

static void fmt_uint(char *buf, unsigned long cap, unsigned long *pos,
                     u64 v, u32 base, int width, int zero, int upper) {
    char tmp[24];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    for (int i = n; i < width; i++) fmt_put(buf, cap, pos, zero ? '0' : ' ');
    while (n) fmt_put(buf, cap, pos, tmp[--n]);
}

int snprintf(char *buf, unsigned long cap, const char *fmt, ...) {
    unsigned long pos = 0;
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; p && *p; p++) {
        if (*p != '%') { fmt_put(buf, cap, &pos, *p); continue; }
        p++;
        if (!*p) break;
        int zero = 0, width = 0;
        if (*p == '0') { zero = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) fmt_put(buf, cap, &pos, *s++);
            break;
        }
        case 'u': {
            unsigned v = va_arg(ap, unsigned);
            fmt_uint(buf, cap, &pos, v, 10, width, zero, 0);
            break;
        }
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { fmt_put(buf, cap, &pos, '-'); fmt_uint(buf, cap, &pos, (u64)(-v), 10, width ? width - 1 : 0, zero, 0); }
            else fmt_uint(buf, cap, &pos, (u64)v, 10, width, zero, 0);
            break;
        }
        case 'x':
        case 'X': {
            unsigned v = va_arg(ap, unsigned);
            fmt_uint(buf, cap, &pos, v, 16, width, zero, *p == 'X');
            break;
        }
        case 'c': {
            int c = va_arg(ap, int);
            fmt_put(buf, cap, &pos, (char)c);
            break;
        }
        case '%':
            fmt_put(buf, cap, &pos, '%');
            break;
        default:
            fmt_put(buf, cap, &pos, '%');
            fmt_put(buf, cap, &pos, *p);
            break;
        }
    }
    va_end(ap);
    if (cap) buf[pos < cap ? pos : cap - 1] = 0;
    return (int)pos;
}

void zts_symbols_init(void) {
    zts_memset(g_syms, 0, sizeof(g_syms));
    g_sym_count = 0;
    zts_symbol_register("zts_api", (u64)&zts_api);
    zts_symbol_register("zts_alloc", (u64)zts_alloc);
    zts_symbol_register("zts_free", (u64)zts_free);
    zts_symbol_register("zts_realloc", (u64)zts_realloc);
    zts_symbol_register("zts_log", (u64)api_log_print);
    zts_symbol_register("zts_time_ms", (u64)zts_now_ms);
    zts_symbol_register("zts_memcpy", (u64)zts_memcpy);
    zts_symbol_register("zts_memset", (u64)zts_memset);
    zts_symbol_register("zts_strcmp", (u64)zts_strcmp);
    zts_symbol_register("zts_strlen", (u64)zts_strlen);
    zts_symbol_register("zts_crc32", (u64)zts_crc32);
}
