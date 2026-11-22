/*
 * ZTS7 Dynamic Algorithm System - Kernel Side Implementation
 *
 * Loads and executes .zts modules at boot time.
 * Supports API mode, Patch mode, and Inject mode.
 */

#include "zts_algorithm.h"
#include "console.h"
#include "heap.h"
#include "string.h"
#include "vfs.h"
#include "cpu.h"
#include "pmm.h"
#include "vmm.h"
#include <stdarg.h>

/* Global ZTS system state */
static struct zts_system zts_sys;

/* Forward declarations */
static void zts_setup_api(void);
static int zts_validate_header(struct zts_header *hdr);
static uint32_t zts_calculate_checksum(void *data, size_t size);
static int zts_parse_priority(const char *filename);
static int zts_load_from_file(const char *path, struct zts_module *mod);

/* ============================================================
 * API Implementation
 * ============================================================ */

static void *zts_api_alloc(size_t size) {
    return kzalloc(size);
}

static void zts_api_free(void *ptr) {
    kfree(ptr);
}

static void *zts_api_realloc(void *ptr, size_t size) {
    return krealloc(ptr, size);
}

static void zts_api_log(const char *level, const char *fmt, ...) {
    char buf[512];
    int pos = 0;

    if (level) {
        pos += __builtin_snprintf(buf + pos, sizeof(buf) - pos, "[ZTS][%s] ", level);
    }

    va_list args;
    va_start(args, fmt);
    /* Simple format processing - handle %s, %d, %x, %lu */
    while (*fmt && pos < 500) {
        if (*fmt == '%' && *(fmt+1)) {
            fmt++;
            switch (*fmt) {
            case 's': {
                const char *s = va_arg(args, const char *);
                if (s) {
                    while (*s && pos < 500) buf[pos++] = *s++;
                }
                break;
            }
            case 'd': case 'i': {
                int d = va_arg(args, int);
                if (d == 0) { buf[pos++] = '0'; }
                else {
                    int neg = 0;
                    if (d < 0) { neg = 1; d = -d; }
                    char tmp[20]; int ti = 0;
                    while (d > 0 && ti < 19) { tmp[ti++] = '0' + (d % 10); d /= 10; }
                    if (neg) buf[pos++] = '-';
                    while (ti > 0) buf[pos++] = tmp[--ti];
                }
                break;
            }
            case 'x': {
                unsigned int x = va_arg(args, unsigned int);
                buf[pos++] = '0'; buf[pos++] = 'x';
                for (int i = 7; i >= 0; i--) {
                    int nib = (x >> (i * 4)) & 0xF;
                    buf[pos++] = nib < 10 ? '0' + nib : 'a' + nib - 10;
                }
                break;
            }
            case 'l': {
                if (*(fmt+1) == 'u') {
                    fmt++;
                    unsigned long lu = va_arg(args, unsigned long);
                    if (lu == 0) { buf[pos++] = '0'; }
                    else {
                        char tmp[30]; int ti = 0;
                        while (lu > 0 && ti < 29) { tmp[ti++] = '0' + (lu % 10); lu /= 10; }
                        while (ti > 0) buf[pos++] = tmp[--ti];
                    }
                }
                break;
            }
            case 'p': {
                void *p = va_arg(args, void *);
                uint64_t addr = (uint64_t)p;
                buf[pos++] = '0'; buf[pos++] = 'x';
                for (int i = 15; i >= 0; i--) {
                    int nib = (addr >> (i * 4)) & 0xF;
                    buf[pos++] = nib < 10 ? '0' + nib : 'a' + nib - 10;
                }
                break;
            }
            default:
                buf[pos++] = '%';
                buf[pos++] = *fmt;
                break;
            }
        } else {
            buf[pos++] = *fmt;
        }
        fmt++;
    }
    va_end(args);
    buf[pos] = '\0';

    printk(buf);
}

static uint64_t zts_api_time(void) {
    return rdtsc();
}

static int zts_api_strcmp(const char *s1, const char *s2) {
    return strcmp(s1, s2);
}

static void *zts_api_memcpy(void *dst, const void *src, size_t n) {
    return memcpy(dst, src, n);
}

static void *zts_api_memset(void *s, int c, size_t n) {
    return memset(s, c, n);
}

static void zts_setup_api(void) {
    zts_sys.api.api_version = ZTS_API_VERSION;
    zts_sys.api.alloc = zts_api_alloc;
    zts_sys.api.free = zts_api_free;
    zts_sys.api.realloc = zts_api_realloc;
    zts_sys.api.log_print = zts_api_log;
    zts_sys.api.time_now = zts_api_time;
    zts_sys.api.strcmp = zts_api_strcmp;
    zts_sys.api.memcpy = zts_api_memcpy;
    zts_sys.api.memset = zts_api_memset;

    /* VFS and Network ops - optional, set to NULL if not available */
    zts_sys.api.vfs_open = NULL;
    zts_sys.api.vfs_close = NULL;
    zts_sys.api.vfs_read = NULL;
    zts_sys.api.vfs_write = NULL;
    zts_sys.api.net_send = NULL;
    zts_sys.api.net_recv = NULL;
}

/* ============================================================
 * Validation
 * ============================================================ */

static int zts_validate_header(struct zts_header *hdr) {
    if (!hdr) return -1;

    if (hdr->magic != ZTS_MAGIC) {
        printk("zts: Invalid magic %x", hdr->magic);
        return -1;
    }

    if (hdr->version < hdr->api_min_version) {
        printk("zts: Version mismatch %d < %d", hdr->version, hdr->api_min_version);
        return -1;
    }

    if (hdr->api_min_version > ZTS_API_VERSION) {
        printk("zts: Module requires API %d, we have %d", hdr->api_min_version, ZTS_API_VERSION);
        return -1;
    }

    /* Checksum validation */
    uint32_t expected = zts_calculate_checksum(hdr, sizeof(*hdr));
    if (hdr->checksum != 0 && hdr->checksum != expected) {
        printk("zts: Checksum mismatch for %s", hdr->name);
        return -1;
    }

    return 0;
}

static uint32_t zts_calculate_checksum(void *data, size_t size) {
    uint32_t sum = 0;
    uint8_t *p = (uint8_t *)data;
    for (size_t i = 0; i < size; i++) {
        sum += p[i];
    }
    return sum;
}

/* ============================================================
 * Priority Parsing (from filename like 0x00000000.zts)
 * ============================================================ */

static int zts_parse_priority(const char *filename) {
    /* Extract hex value from filename prefix like "0x00000000" */
    if (!filename || filename[0] != '0' || filename[1] != 'x') return 0;

    uint64_t val = 0;
    int i = 2;
    while (filename[i]) {
        char c = filename[i];
        if (c >= '0' && c <= '9') {
            val = (val << 4) | (c - '0');
        } else if (c >= 'a' && c <= 'f') {
            val = (val << 4) | (c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            val = (val << 4) | (c - 'A' + 10);
        } else if (c == '.') {
            break;
        } else {
            break;
        }
        i++;
    }

    return (int)val;
}

/* ============================================================
 * Module Loading
 * ============================================================ */

static int zts_load_from_file(const char *path, struct zts_module *mod) {
    printk("zts: Loading module from %s", path);

    /* Open the .zts file */
    struct vfs_file file;
    memset(&file, 0, sizeof(file));

    /* Use VFS to open the file */
    /* Note: This requires VFS to be initialized before ZTS */
    int fd = -1;
    /* Try to open via VFS if available */
    /* For now, we'll use a simplified approach */

    /* Map the file into memory using VFS */
    /* Since VFS read may not be fully available, we use a simple approach */

    printk("zts: Reading module file...");

    /* Read file header first */
    struct zts_header hdr;
    memset(&hdr, 0, sizeof(hdr));

    /* For now, assume the module is already loaded into memory */
    /* In a full implementation, we would read from the filesystem */

    return -1;  /* Not yet implemented - needs VFS integration */
}

/* Simplified module loader - works with pre-loaded binary data */
int zts_register_module(void *binary, size_t size, int priority) {
    if (!binary || size < sizeof(struct zts_header)) {
        printk("zts: Invalid module binary");
        return -1;
    }

    if (zts_sys.module_count >= ZTS_MAX_MODULES) {
        printk("zts: Too many modules");
        return -1;
    }

    struct zts_header *hdr = (struct zts_header *)binary;

    if (zts_validate_header(hdr) < 0) {
        printk("zts: Invalid module header");
        return -1;
    }

    /* Allocate module slot */
    struct zts_module *mod = &zts_sys.modules[zts_sys.module_count];
    memset(mod, 0, sizeof(*mod));

    /* Copy metadata */
    strncpy(mod->name, hdr->name, ZTS_MAX_NAME - 1);
    memcpy(mod->uuid, hdr->uuid, ZTS_UUID_LEN);
    mod->category = hdr->category;
    mod->major_version = hdr->major_version;
    mod->minor_version = hdr->minor_version;
    mod->priority = priority;

    /* Copy the entire binary to executable memory */
    mod->code_base = binary;
    mod->code_size = size;

    /* Resolve entry points */
    if (hdr->init_func_offset > 0 && hdr->init_func_offset < size) {
        mod->init = (int (*)(struct zts_api_table *))((uint8_t *)binary + hdr->init_func_offset);
    }
    if (hdr->execute_func_offset > 0 && hdr->execute_func_offset < size) {
        mod->execute = (int (*)(void *, void *, void *))((uint8_t *)binary + hdr->execute_func_offset);
    }
    if (hdr->cleanup_func_offset > 0 && hdr->cleanup_func_offset < size) {
        mod->cleanup = (void (*)(void))((uint8_t *)binary + hdr->cleanup_func_offset);
    }

    mod->loaded = 1;
    zts_sys.module_count++;

    printk("zts: Module '%s' v%d.%d loaded (pri=%d)",
           mod->name, mod->major_version, mod->minor_version, mod->priority);

    return 0;
}

/* ============================================================
 * Module Execution
 * ============================================================ */

static int zts_init_module(struct zts_module *mod) {
    if (!mod || !mod->loaded) return -1;
    if (mod->initialized) return 0;

    printk("zts: Initializing module '%s'...", mod->name);

    if (mod->init) {
        int ret = mod->init(&zts_sys.api);
        if (ret < 0) {
            printk("zts: Module '%s' init failed: %d", mod->name, ret);
            return ret;
        }
    }

    mod->initialized = 1;
    printk("zts: Module '%s' initialized", mod->name);
    return 0;
}

int zts_execute_module(const char *name, void *input, void *output) {
    struct zts_module *mod = zts_find_by_name(name);
    if (!mod) {
        printk("zts: Module '%s' not found", name);
        return -1;
    }

    if (!mod->initialized) {
        int ret = zts_init_module(mod);
        if (ret < 0) return ret;
    }

    if (mod->execute) {
        return mod->execute(input, output, NULL);
    }

    return 0;
}

/* ============================================================
 * Module Management
 * ============================================================ */

struct zts_module *zts_find_by_name(const char *name) {
    for (int i = 0; i < zts_sys.module_count; i++) {
        if (strcmp(zts_sys.modules[i].name, name) == 0) {
            return &zts_sys.modules[i];
        }
    }
    return NULL;
}

struct zts_module *zts_find_by_uuid(const uint8_t uuid[ZTS_UUID_LEN]) {
    for (int i = 0; i < zts_sys.module_count; i++) {
        if (memcmp(zts_sys.modules[i].uuid, uuid, ZTS_UUID_LEN) == 0) {
            return &zts_sys.modules[i];
        }
    }
    return NULL;
}

int zts_unload_module(const char *name) {
    struct zts_module *mod = zts_find_by_name(name);
    if (!mod) return -1;

    printk("zts: Unloading module '%s'...", name);

    if (mod->cleanup) {
        mod->cleanup();
    }

    /* Free module data */
    if (mod->data) {
        kfree(mod->data);
    }

    /* Note: We don't free code_base as it may be part of a larger allocation */

    mod->loaded = 0;
    mod->initialized = 0;

    printk("zts: Module '%s' unloaded", name);
    return 0;
}

void zts_list_modules(void) {
    printk("=== ZTS7 Loaded Modules ===");
    printk("Module count: %d", zts_sys.module_count);

    for (int i = 0; i < zts_sys.module_count; i++) {
        struct zts_module *mod = &zts_sys.modules[i];
        const char *cat_str = "UNKNOWN";
        switch (mod->category) {
        case ZTS_CAT_FILESYSTEM: cat_str = "FILESYSTEM"; break;
        case ZTS_CAT_NETWORK: cat_str = "NETWORK"; break;
        case ZTS_CAT_CRYPTO: cat_str = "CRYPTO"; break;
        case ZTS_CAT_SCHEDULER: cat_str = "SCHEDULER"; break;
        case ZTS_CAT_DRIVER: cat_str = "DRIVER"; break;
        case ZTS_CAT_PATCH: cat_str = "PATCH"; break;
        case ZTS_CAT_INJECT: cat_str = "INJECT"; break;
        case ZTS_CAT_CUSTOM: cat_str = "CUSTOM"; break;
        }

        printk("  [%d] %s v%d.%d [%s] %s",
               i, mod->name, mod->major_version, mod->minor_version,
               cat_str, mod->initialized ? "(initialized)" : "(loaded)");
    }
}

/* ============================================================
 * Patch Mode Support
 * ============================================================ */

void zts_restore_patch(struct zts_module *mod) {
    if (!mod || !mod->patch_address || !mod->patch_original) return;

    printk("zts: Restoring patch for '%s'...", mod->name);
    memcpy(mod->patch_address, mod->patch_original, mod->patch_size);
    kfree(mod->patch_original);
    mod->patch_original = NULL;
}

/* ============================================================
 * Directory Loading (simplified)
 * ============================================================ */

int zts_load_directory(const char *dir_path) {
    printk("zts: Loading modules from %s", dir_path);

    /* In a full implementation, this would:
     * 1. Use VFS to list files in the directory
     * 2. Filter for .zts files
     * 3. Parse priority from filename
     * 4. Sort by priority
     * 5. Load each module
     *
     * For now, modules must be registered manually via zts_register_module()
     */

    printk("zts: Directory loading not yet implemented (needs VFS readdir)");
    return -1;
}

/* ============================================================
 * Main Initialization
 * ============================================================ */

int zts_init(void) {
    printk("=== ZTS7 Dynamic Algorithm System ===");

    memset(&zts_sys, 0, sizeof(zts_sys));

    /* Setup API table */
    zts_setup_api();

    /* Set default load path */
    strcpy(zts_sys.load_path, "/zrr64/zts/p.zts6/");

    zts_sys.initialized = 1;

    printk("zts: System initialized, API version %d", ZTS_API_VERSION);
    printk("zts: Default load path: %s", zts_sys.load_path);

    return 0;
}

struct zts_api_table *zts_get_api(void) {
    return &zts_sys.api;
}
