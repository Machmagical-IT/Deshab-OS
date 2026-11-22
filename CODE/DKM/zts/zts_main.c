/* zts_main.c — ZTS 子系统 DKM 驱动入口
 *
 * stage3 / optional / depends "fat32" / provides "zts"
 * driver_init = 读 FUCK [zts] 配置 → 载入 A/B 状态 → 按 ZTS.CNF 自动加载
 *               → 注册 srv.zts.* 服务。
 */
#include "zts.h"

#define ZTS_CONFIG_ENABLE_DEFAULT 1
#define ZTS_CONFIG_AUTOLOAD_DEFAULT 1

static const char *const g_depends[] = { "fat32" };
static const char *const g_provides[] = { "zts" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic = DKM_DRIVER_MAGIC,
    .abi_version = DKM_ABI_VERSION,
    .desc_size = sizeof(struct dkm_driver_desc),
    .name = "zts",
    .version = "1.0.0",
    .vendor = "Deshab",
    .driver_class = 11,          /* DKM_CLASS_MISC */
    .stage = 3,
    .flags = 0,
    .priority = 20,
    .depends = g_depends,
    .depends_count = 1,
    .provides = g_provides,
    .provides_count = 1,
    .min_kernel_abi = 1,
};

/* limine 前缀布局（与 zts_store.c 一致） */
typedef struct {
    u64 revision;
    void *address;
    u64 size;
    char *path;
    char *cmdline;
} zts_file_prefix;

typedef struct {
    u64 revision;
    u64 module_count;
    zts_file_prefix **modules;
} zts_mod_response_prefix;

static const char *g_fuck_text;

static const zts_file_prefix *find_cmdline_module(const char *cmdline) {
    const struct dkm_kernel_api *api = zts_kapi();
    if (!api || !api->boot_modules_response || !cmdline) return 0;
    const zts_mod_response_prefix *rsp =
        (const zts_mod_response_prefix *)api->boot_modules_response;
    for (u64 i = 0; i < rsp->module_count; i++) {
        zts_file_prefix *f = rsp->modules[i];
        if (f && f->cmdline && zts_str_eq(f->cmdline, cmdline)) return f;
    }
    return 0;
}

/* ============================================================
 * FUCK 配置
 * ============================================================ */
static void cfg_load(void) {
    const zts_file_prefix *f = find_cmdline_module("fuck:config");
    if (f && f->address && f->size) {
        g_fuck_text = (const char *)f->address;
        return;
    }
    /* boot module 缺失时经 FAT32 读 */
    u8 *data = 0;
    u32 size = 0;
    int (*read_path)(const char *, u8 **, u32 *) = 0;
    const struct dkm_kernel_api *api = zts_kapi();
    if (api && api->bus && api->bus->probe("srv.fs.fat32.read_path", (void **)&read_path, 0) == 0) {
        if (read_path("/system/deshab64/FUCK", &data, &size) == 0 && data) {
            static char buf[8192];
            if (size >= sizeof(buf)) size = sizeof(buf) - 1;
            zts_memcpy(buf, data, size);
            buf[size] = 0;
            g_fuck_text = buf;
        }
    }
}

/* 极简 INI 查询：返回 key 的值起点（非 NUL 终止，借助 out 复制） */
static int cfg_get(const char *section, const char *key, char *out, u32 cap, const char *def) {
    zts_str_copy(out, def ? def : "", cap);
    if (!g_fuck_text) return 0;
    const char *p = g_fuck_text;
    int in_sec = 0;
    char cur_sec[64] = {0};
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p == '\n') { p++; continue; }
        if (*p == '#' || *p == ';') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (*p == '[') {
            p++;
            u32 n = 0;
            while (*p && *p != ']' && n + 1 < sizeof(cur_sec)) cur_sec[n++] = *p++;
            cur_sec[n] = 0;
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            in_sec = zts_str_eq(cur_sec, section);
            continue;
        }
        const char *line = p;
        while (*p && *p != '\n') p++;
        const char *end = p;
        if (*p == '\n') p++;
        if (!in_sec || !line || end <= line) continue;
        const char *eq = line;
        while (eq < end && *eq != '=') eq++;
        if (eq >= end) continue;
        u32 klen = (u32)(eq - line);
        u32 wlen = (u32)zts_strlen(key);
        if (klen != wlen) continue;
        int match = 1;
        for (u32 i = 0; i < klen; i++) {
            if (line[i] != key[i]) { match = 0; break; }
        }
        if (!match) continue;
        const char *v = eq + 1;
        while (v < end && (*v == ' ' || *v == '\t')) v++;
        u32 n = 0;
        while (v + n < end && n + 1 < cap && v[n] != ' ' && v[n] != '\t') {
            out[n] = v[n];
            n++;
        }
        out[n] = 0;
        return 1;
    }
    return 0;
}

static int cfg_bool(const char *key, int def) {
    char v[16];
    cfg_get("zts", key, v, sizeof(v), def ? "1" : "0");
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y';
}

/* ============================================================
 * 清单解析与自动加载
 * ============================================================ */
static int parse_and_load(const char *text) {
    const char *p = text;
    int loaded = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p == '\n') { p++; continue; }
        if (*p == '#' || *p == ';') { while (*p && *p != '\n') p++; continue; }
        const char *line = p;
        while (*p && *p != '\n') p++;
        const char *end = p;
        if (*p == '\n') p++;
        if (end <= line) continue;
        /* 行格式: file|deps|autoload|required */
        char fields[4][96];
        int nf = 0, fi = 0;
        zts_memset(fields, 0, sizeof(fields));
        for (const char *s = line; s <= end && nf < 4; s++) {
            char c = (s == end) ? '|' : *s;
            if (c == '\r' || c == ' ' || c == '\t') {
                if (c == '\r') continue;
            }
            if (c == '|') {
                fields[nf][fi] = 0;
                nf++;
                fi = 0;
                if (nf >= 4) break;
            } else if (fi < 95) {
                fields[nf][fi++] = c;
            }
        }
        if (nf < 1 || !fields[0][0]) continue;
        if (zts_str_eq(fields[0], "file")) continue;   /* 表头 */

        char file[96];
        zts_str_copy(file, fields[0], sizeof(file));
        const char *deps = nf >= 2 ? fields[1] : "";
        int autoload = nf >= 3 && fields[2][0] ? (fields[2][0] == '1') : 1;
        int required = nf >= 4 && fields[3][0] ? (fields[3][0] == '1') : 0;

        /* 若 A/B 状态指定了副本，优先加载该副本 */
        u8 *hdr_data = 0;
        u32 hdr_size = 0;
        char mod_name[ZTS_MAX_NAME] = {0};
        if (zts_store_read(file, &hdr_data, &hdr_size) == 0 && hdr_data &&
            hdr_size >= ZTS_HEADER_SIZE) {
            zts_header *h = (zts_header *)hdr_data;
            if (h->magic == ZTS_MAGIC) zts_str_copy(mod_name, h->name, ZTS_MAX_NAME);
        }
        if (mod_name[0]) {
            const zts_state_mod *st = zts_store_state_find(mod_name);
            if (st && st->file[0]) zts_str_copy(file, st->file, sizeof(file));
        }

        zts_set_pending_deps(deps);
        if (zts_module_load_file(file, autoload) != 0) {
            zts_log("[zts] autoload failed:");
            zts_log(file);
            if (required) {
                zts_log("[zts] required module failed; subsystem degraded");
            }
        } else {
            loaded++;
        }
        zts_set_pending_deps("");
    }
    return loaded;
}

/* ============================================================
 * 服务实现
 * ============================================================ */
static u64 hex_to_u64(const char *s) {
    u64 v = 0;
    if (s && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (s && *s) {
        int h;
        if (*s >= '0' && *s <= '9') h = *s - '0';
        else if (*s >= 'a' && *s <= 'f') h = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') h = *s - 'A' + 10;
        else break;
        v = (v << 4) | (u64)h;
        s++;
    }
    return v;
}

static u64 resolve_addr(const char *s) {
    if (!s || !s[0]) return 0;
    if (s[0] == 's' && s[1] == 'y' && s[2] == 'm' && s[3] == ':') return zts_symbol_lookup(s + 4);
    return hex_to_u64(s);
}

int zts_svc_list(char *buf, u32 cap) {
    if (!buf || !cap) return -1;
    u32 n = 0;
    int count = zts_module_count();
    for (int i = 0; i < count && n + 160 < cap; i++) {
        zts_module *m = zts_module_at((u32)i);
        char u[9];
        static const char hx[] = "0123456789abcdef";
        for (int k = 0; k < 4; k++) { u[k * 2] = hx[m->uuid[k] >> 4]; u[k * 2 + 1] = hx[m->uuid[k] & 15]; }
        u[8] = 0;
        n += (u32)__builtin_snprintf(buf + n, cap - n,
            "%s|%s|%u.%u.%u|%u|%u|%s\n", m->name, u,
            (unsigned)m->major, (unsigned)m->minor, (unsigned)m->patch,
            (unsigned)m->state, (unsigned)m->slot, m->file);
    }
    if (cap) buf[n < cap ? n : cap - 1] = 0;
    return count;
}

int zts_svc_load(const char *file) {
    if (!file) return -1;
    return zts_module_load_file(file, 1);
}

int zts_svc_unload(const char *name) {
    if (!name) return -1;
    return zts_module_unload(name);
}

int zts_svc_execute(const char *args) {
    if (!args) return -1;
    char name[ZTS_MAX_MOD_NAME];
    zts_str_copy(name, args, sizeof(name));
    for (u32 i = 0; name[i]; i++) {
        if (name[i] == '|') { name[i] = 0; break; }
    }
    return zts_module_execute(name, 0, 0);
}

int zts_svc_hotupdate(const char *args) {
    if (!args) return -1;
    char name[ZTS_MAX_MOD_NAME];
    const char *file = 0;
    u32 i = 0;
    while (args[i] && args[i] != '|' && i + 1 < sizeof(name)) { name[i] = args[i]; i++; }
    name[i] = 0;
    if (args[i] == '|') file = args + i + 1;
    if (!file || !file[0]) return -1;
    return zts_module_hotupdate(name, file);
}

int zts_svc_rollback(const char *args) {
    if (!args) return -1;
    if (args[0] == 'c' && args[1] == 'k' && args[2] == 'p' && args[3] == 't' && args[4] == ':')
        return zts_ckpt_restore(hex_to_u64(args + 5));
    if (args[0] == 'v' && args[1] == 'e' && args[2] == 'r' && args[3] == ':')
        return zts_version_rollback((u32)hex_to_u64(args + 4));
    /* 按模块名：恢复包含该模块的最新检查点 */
    char name[ZTS_MAX_MOD_NAME];
    zts_str_copy(name, args, sizeof(name));
    for (u32 i = 0; name[i]; i++) if (name[i] == '|') { name[i] = 0; break; }
    if (!zts_module_find(name)) return -1;
    u64 id = 0;
    if (zts_ckpt_latest_for_module(name, &id) != 0) return -1;
    return zts_ckpt_restore(id);
}

int zts_svc_ckpt_create(const char *desc) {
    return (int)zts_ckpt_create(desc ? desc : "manual");
}

int zts_svc_ckpt_restore(u64 id) {
    return zts_ckpt_restore(id);
}

int zts_svc_diff_apply(const char *path) {
    if (!path) return -1;
    return zts_update_apply_package(path);
}

int zts_svc_patch_install(const char *args) {
    if (!args) return -1;
    char f[4][ZTS_MAX_MOD_NAME];
    int nf = 0, fi = 0;
    zts_memset(f, 0, sizeof(f));
    for (u32 i = 0; args[i] && nf < 4; i++) {
        if (args[i] == '|') {
            f[nf][fi] = 0;
            nf++;
            fi = 0;
            if (nf >= 4) break;
        } else if (fi < ZTS_MAX_MOD_NAME - 1) {
            f[nf][fi++] = args[i];
        }
    }
    if (nf < 4) {
        f[nf][fi] = 0;
        nf++;
    }
    if (nf < 2) return -1;
    u64 target = resolve_addr(f[0]);
    u64 repl = resolve_addr(f[1]);
    if (!target || !repl) return -1;
    return zts_patch_install(target, repl, f[2], f[3]);
}

int zts_svc_patch_uninstall(const char *args) {
    if (!args) return -1;
    u64 target = resolve_addr(args);
    return target ? zts_patch_uninstall(target) : -1;
}

int zts_svc_state(char *buf, u32 cap) { return zts_state_text(buf, cap); }
int zts_svc_stats(char *buf, u32 cap) { return zts_stats_text(buf, cap); }

int zts_svc_guest_status(char *buf, u32 cap) {
    if (!buf || !cap) return -1;
    const struct dkm_kernel_api *api = zts_kapi();
    if (!api || !api->bus) return -1;
    int (*status)(char *, u32) = 0;
    if (api->bus->probe("kern.zt5.status", (void **)&status, 0) != 0 || !status) {
        zts_str_copy(buf, "zt5: not present\n", cap);
        return 0;
    }
    return status(buf, cap);
}

/* ============================================================
 * 服务注册
 * ============================================================ */
static int register_services(void) {
    const struct dkm_kernel_api *api = zts_kapi();
    if (!api || !api->bus) {
        zts_log("[zts] no service bus; srv.zts.* not registered");
        return -1;
    }
    struct { const char *name; void *fn; } svc[] = {
        { "srv.zts.list",            (void *)zts_svc_list },
        { "srv.zts.load",            (void *)zts_svc_load },
        { "srv.zts.unload",          (void *)zts_svc_unload },
        { "srv.zts.execute",         (void *)zts_svc_execute },
        { "srv.zts.hotupdate",       (void *)zts_svc_hotupdate },
        { "srv.zts.rollback",        (void *)zts_svc_rollback },
        { "srv.zts.checkpoint.create", (void *)zts_svc_ckpt_create },
        { "srv.zts.checkpoint.restore", (void *)zts_svc_ckpt_restore },
        { "srv.zts.diff.apply",      (void *)zts_svc_diff_apply },
        { "srv.zts.patch.install",   (void *)zts_svc_patch_install },
        { "srv.zts.patch.uninstall", (void *)zts_svc_patch_uninstall },
        { "srv.zts.state",           (void *)zts_svc_state },
        { "srv.zts.stats",           (void *)zts_svc_stats },
        { "srv.zts.guest.status",    (void *)zts_svc_guest_status },
    };
    for (u32 i = 0; i < sizeof(svc) / sizeof(svc[0]); i++) {
        if (api->bus->register_service(svc[i].name, 1, svc[i].fn) != 0) {
            zts_log("[zts] service register failed:");
            zts_log(svc[i].name);
            return -1;
        }
    }
    zts_log("[zts] srv.zts.* registered");
    return 0;
}

/* ============================================================
 * driver_init / driver_exit
 * ============================================================ */
int driver_init(const struct dkm_kernel_api *api, struct dkm_driver_handle *handle) {
    (void)handle;
    zts_core_init(api, api->log);
    zts_symbols_init();
    zts_modules_init();
    zts_hu_init();
    zts_store_init();

    zts_log("[zts] subsystem init");
    cfg_load();

    if (!cfg_bool("enable", ZTS_CONFIG_ENABLE_DEFAULT)) {
        zts_log("[zts] disabled by FUCK [zts] enable=0");
        return 0;
    }

    /* A/B 状态先于模块加载 */
    zts_store_load_state();

    if (cfg_bool("autoload", ZTS_CONFIG_AUTOLOAD_DEFAULT)) {
        const char *manifest = zts_store_manifest();
        if (manifest && manifest[0]) {
            int n = parse_and_load(manifest);
            zts_log_dec("[zts] autoload modules=", (u64)n);
        } else {
            zts_log("[zts] ZTS.CNF not found; no autoload");
        }
    }

    if (cfg_bool("selftest", 0)) {
        zts_selftest();
    }

    if (register_services() != 0) {
        /* 无总线时仍可完成模块加载；返回非零会让 DKM 标记 FAILED，
         * 因此这里只记录并继续（services 缺席可由调用方 probe 探测）。 */
        zts_log("[zts] running without service bus");
    }
    zts_log("[zts] driver ready");
    return 0;
}

int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    zts_modules_shutdown();
    zts_log("[zts] driver exit");
    return 0;
}
