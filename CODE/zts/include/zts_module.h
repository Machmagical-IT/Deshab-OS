/* zts_module.h — ZTS 模块侧 ABI（编译为 ELF64 ET_REL）
 *
 * 构建方式（build.ps1 内置）：
 *   clang -target x86_64-unknown-none -ffreestanding -fno-builtin \
 *         -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel \
 *         -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -O2 \
 *         -I CODE/zts/include -c module.c -o module.o
 *   ld.lld -r -o module.rel module.o
 *   python CODE/zts/mkzts.py --in module.rel --out 0x00000001.zts ...
 */
#ifndef ZTS_MODULE_H
#define ZTS_MODULE_H

#include <stdint.h>
#include <stddef.h>

#define ZTS_MAGIC          0x53545A00u
#define ZTS_FORMAT_VERSION 2u
#define ZTS_API_VERSION    2u

#define ZTS_MAX_NAME       64
#define ZTS_MAX_DESC       256
#define ZTS_MAX_AUTHOR     64
#define ZTS_MAX_LICENSE    32
#define ZTS_UUID_LEN       16

#define ZTS_CAT_FILESYSTEM  0
#define ZTS_CAT_NETWORK     1
#define ZTS_CAT_CRYPTO      2
#define ZTS_CAT_COMPRESSION 3
#define ZTS_CAT_SCHEDULER   4
#define ZTS_CAT_MEMORY      5
#define ZTS_CAT_DRIVER      6
#define ZTS_CAT_PATCH       7
#define ZTS_CAT_INJECT      8
#define ZTS_CAT_CUSTOM      9

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t api_min_version;
    char name[ZTS_MAX_NAME];
    char description[ZTS_MAX_DESC];
    char author[ZTS_MAX_AUTHOR];
    char license[ZTS_MAX_LICENSE];
    uint8_t uuid[ZTS_UUID_LEN];
    uint32_t category;
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t patch_version;
    uint32_t num_dependencies;
    uint64_t init_func_offset;
    uint64_t cleanup_func_offset;
    uint64_t execute_func_offset;
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t checksum;
    uint8_t signature[256];
} zts_header;

typedef struct zts_api_table {
    uint32_t api_version;
    void *(*alloc)(uint64_t size);
    void (*free)(void *ptr);
    void *(*realloc)(void *ptr, uint64_t size);
    void (*log_print)(const char *level, const char *msg);
    uint64_t (*time_now)(void);
    int (*vfs_open)(const char *path, uint32_t mode);
    int (*vfs_close)(int fd);
    int (*vfs_read)(int fd, void *buf, uint64_t count);
    int (*vfs_write)(int fd, const void *buf, uint64_t count);
    int (*net_send)(const void *data, uint64_t len);
    int (*net_recv)(void *data, uint64_t len);
    int (*strcmp)(const char *s1, const char *s2);
    void *(*memcpy)(void *dst, const void *src, uint64_t n);
    void *(*memset)(void *s, int c, uint64_t n);
    uint32_t ext_version;
    int (*bus_probe)(const char *name, void **out_fn);
    int (*fs_read_file)(const char *path, uint8_t **out, uint32_t *size);
    uint64_t (*time_ms)(void);
    uint32_t (*crc32)(const void *data, uint64_t size);
} zts_api_table;

/* 宿主导出的 API 指针（ELF 未定义符号，由加载器解析）。 */
extern zts_api_table *zts_api;

#define zts_malloc(n)        (zts_api->alloc((uint64_t)(n)))
#define zts_free(p)          (zts_api->free((void *)(p)))
#define zts_logi(s)          (zts_api->log_print("info", (s)))
#define zts_logw(s)          (zts_api->log_print("warn", (s)))
#define zts_loge(s)          (zts_api->log_print("error", (s)))
#define zts_memcpy(d,s,n)    (zts_api->memcpy((d),(s),(uint64_t)(n)))
#define zts_memset(d,c,n)    (zts_api->memset((d),(c),(uint64_t)(n)))
#define zts_strcmp(a,b)      (zts_api->strcmp((a),(b)))
#define zts_now_ms()         (zts_api->time_ms ? zts_api->time_ms() : 0)
#define zts_crc32(p,n)       (zts_api->crc32 ? zts_api->crc32((p),(uint64_t)(n)) : 0)

/* 入口点符号名（加载器按名解析；同时 mkzts.py 写入头内偏移） */
#define ZTS_MODULE_INIT(name)    int name(zts_api_table *api)
#define ZTS_MODULE_EXEC(name)    int name(void *input, void *output, void *ctx)
#define ZTS_MODULE_CLEANUP(name) void name(void)

#endif /* ZTS_MODULE_H */
