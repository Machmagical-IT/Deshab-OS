#include <utsm/dkm.h>
#include <utsm/log.h>
#include <utsm/panic.h>
#include <utsm/types.h>
#include "../arch/x86_64/limine.h"
#include "../kernel/ini_parser.h"

/* 前向声明：带 FUCK [drivers] 过滤的加载入口（定义见下文） */
void dsm_load_by_manifest_ex(const ini_config *cfg);

#define DSM_MAX_DRIVERS_PER_STAGE 12
#define DSM_MAX_STAGES 6
#define DSM_MAX_DEPENDS 4
#define DSM_MAX_PROVIDES 4
#define DSM_MAX_CAPS 128

typedef struct dsm_driver_entry {
    char name[32];
    char path[64];
    u32 class_id;
    u32 required;
    char depends[DSM_MAX_DEPENDS][32];
    u32 depend_count;
    char provides[DSM_MAX_PROVIDES][32];
    u32 provide_count;
    struct limine_file *boot_module;
    int loaded;
    /* P0（万物互联）：终态标志——已加载/被 FUCK 关闭/模块缺失/加载失败，
     * 不再是本轮不动点迭代的候选。resolved 但 !loaded 的驱动不登记 provides。 */
    int resolved;
} dsm_driver_entry;

typedef struct dsm_stage_entry {
    u32 id;
    char name[16];
    u32 required;
    dsm_driver_entry drivers[DSM_MAX_DRIVERS_PER_STAGE];
    u32 driver_count;
} dsm_stage_entry;

static dsm_stage_entry g_stages[DSM_MAX_STAGES];
static u32 g_stage_count;

static int streq_static(const char *a, const char *b);

/* ===================================================================
 *  P0（万物互联）：DSM 依赖图强制
 *
 *  capability 集合 = 已成功加载驱动的 provides token 并集。
 *  加载前校验 depends 全满足（miss → DKM-E44）；
 *  同 stage 内按依赖不动点迭代（环 → DKM-E47）。
 *  性能不是目标（多趟扫描），正确性第一——设计定稿见
 *  docs/RE/万物互联与服务总线设计.md §1.5。
 * =================================================================== */
static char g_caps[DSM_MAX_CAPS][32];
static u32 g_cap_count;

static int dsm_capability_satisfied(const char *token) {
    for (u32 i = 0; i < g_cap_count; i++) {
        if (streq_static(g_caps[i], token)) return 1;
    }
    return 0;
}

static void dsm_capability_add(const char *token) {
    if (dsm_capability_satisfied(token)) return;
    if (g_cap_count >= DSM_MAX_CAPS) {
        panic_full("DKM-E48 CAPABILITY TABLE FULL",
                   "P0 dependency graph: g_caps overflow (DSM_MAX_CAPS)", 0);
        return;
    }
    for (u32 i = 0; i + 1 < 32 && token[i]; i++) {
        g_caps[g_cap_count][i] = token[i];
    }
    g_caps[g_cap_count][31] = 0;
    g_cap_count++;
}

static void dsm_register_provides(const dsm_driver_entry *drv) {
    for (u32 i = 0; i < drv->provide_count; i++) {
        dsm_capability_add(drv->provides[i]);
    }
}

/* P1（万物互联）：stage>=4 的 provides 总线契约校验——driver_init 成功后，
 * 每个 provides token T 必须已有 "srv.T" 或 "srv.T.*" 服务注册到总线，
 * 否则 DKM-E45（零降级：provides 是对消费者/排序者的承诺，注册不到即违约）。
 * stage0-3 老 14 驱动豁免：其 provides 由内核注册表兑现（ahci→block
 * registry 等），不经总线。豁免清单 = manifest.json 现有 stage0-3 全部条目。 */
static void dsm_verify_provides(const dsm_stage_entry *stage, const dsm_driver_entry *drv) {
    if (stage->id < 4) return;
    for (u32 i = 0; i < drv->provide_count; i++) {
        char srv_name[40];
        const char *token = drv->provides[i];
        srv_name[0] = 's'; srv_name[1] = 'r'; srv_name[2] = 'v'; srv_name[3] = '.';
        u32 p = 4;
        while (token[p - 4] && p + 1 < sizeof(srv_name)) {
            srv_name[p] = token[p - 4];
            p++;
        }
        srv_name[p] = 0;
        if (!dkm_bus_prefix_registered(srv_name)) {
            log_error("[DSM] provides token not registered on bus: ");
            log_info(drv->name);
            log_info(token);
            panic_full("DKM-E45 PROVIDES NOT REGISTERED",
                       "stage>=4 driver provides token has no matching srv.* bus service", 0);
        }
    }
}

/* depends 全满足返回 1；否则返回 0 并（可选）写出第一个未满足 token 的下标 */
static int dsm_deps_satisfied(const dsm_driver_entry *drv, u32 *missing_idx) {
    for (u32 i = 0; i < drv->depend_count; i++) {
        if (!dsm_capability_satisfied(drv->depends[i])) {
            if (missing_idx) *missing_idx = i;
            return 0;
        }
    }
    return 1;
}

/* token 是否由本 stage 尚未 resolved 的候选提供（用于区分缺依赖 vs 成环） */
static int dsm_dep_pending_in_stage(const dsm_stage_entry *stage, const char *token) {
    for (u32 di = 0; di < stage->driver_count; di++) {
        const dsm_driver_entry *drv = &stage->drivers[di];
        if (drv->resolved) continue;
        for (u32 i = 0; i < drv->provide_count; i++) {
            if (streq_static(drv->provides[i], token)) return 1;
        }
    }
    return 0;
}

/* 不动点停滞：先判真缺依赖（DKM-E44），全在剩余候选里则判环（DKM-E47）。
 * 零降级：两者都是编程/manifest 错误，莲花屏。 */
static void dsm_report_dependency_stall(const dsm_stage_entry *stage) {
    for (u32 di = 0; di < stage->driver_count; di++) {
        const dsm_driver_entry *drv = &stage->drivers[di];
        if (drv->resolved) continue;
        for (u32 i = 0; i < drv->depend_count; i++) {
            const char *dep = drv->depends[i];
            if (!dsm_capability_satisfied(dep) && !dsm_dep_pending_in_stage(stage, dep)) {
                log_error("[DSM] dependency not satisfied: ");
                log_info(drv->name);
                log_info(dep);
                panic_full("DKM-E44 DEPENDENCY NOT SATISFIED",
                           "manifest depends token not provided by any loaded/pending driver", 0);
                return;
            }
        }
    }
    panic_full("DKM-E47 DEPENDENCY CYCLE",
               "manifest stage dependency graph has a cycle (no progress)", 0);
}

extern volatile struct limine_module_request g_module_request;

static int dsm_skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') { (*p)++; }
    return 0;
}

static int dsm_expect(const char **p, char c) {
    dsm_skip_ws(p);
    if (**p == c) { (*p)++; return 0; }
    return -1;
}

static int dsm_read_string(const char **p, char *out, u32 max_len) {
    dsm_skip_ws(p);
    if (**p != '"') return -1;
    (*p)++;
    u32 i = 0;
    while (**p && **p != '"' && i + 1 < max_len) {
        out[i++] = **p;
        (*p)++;
    }
    out[i] = 0;
    if (**p != '"') return -1;
    (*p)++;
    return 0;
}

static int dsm_skip_value(const char **p) {
    dsm_skip_ws(p);
    if (**p == '"') {
        char tmp[256];
        return dsm_read_string(p, tmp, sizeof(tmp));
    }
    if (**p == '{') {
        (*p)++;
        int depth = 1;
        while (depth > 0 && **p) {
            if (**p == '{') depth++;
            else if (**p == '}') depth--;
            else if (**p == '"') { (*p)++; while (**p && **p != '"') (*p)++; if (**p) (*p)++; continue; }
            if (depth > 0) (*p)++;
        }
        return 0;
    }
    if (**p == '[') {
        (*p)++;
        int depth = 1;
        while (depth > 0 && **p) {
            if (**p == '[') depth++;
            else if (**p == ']') depth--;
            else if (**p == '"') { (*p)++; while (**p && **p != '"') (*p)++; if (**p) (*p)++; continue; }
            if (depth > 0) (*p)++;
        }
        return 0;
    }
    while (**p && **p != ',' && **p != '}' && **p != ']') (*p)++;
    return 0;
}

static struct limine_file *dsm_find_boot_module(const char *path) {
    struct limine_module_response *rsp = g_module_request.response;
    if (!rsp) return 0;
    for (u64 i = 0; i < rsp->module_count; i++) {
        struct limine_file *f = rsp->modules[i];
        if (f && f->path) {
            const char *fp = f->path;
            while (*fp == '/') fp++;
            usize j = 0;
            usize k = 0;
            while (fp[j]) j++;
            while (path[k]) k++;
            if (j == k) {
                int match = 1;
                for (usize x = 0; x < j; x++) {
                    if (fp[x] != path[x]) { match = 0; break; }
                }
                if (match) return f;
            }
        }
    }
    return 0;
}

static int streq_static(const char *a, const char *b) {
    usize i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static void dsm_parse_driver(const char **p, dsm_driver_entry *drv) {
    drv->depend_count = 0;
    drv->provide_count = 0;
    drv->required = 1;
    drv->boot_module = 0;
    drv->loaded = 0;
    drv->resolved = 0;
    drv->class_id = 0;

    if (dsm_expect(p, '{') != 0) return;
    while (**p && **p != '}') {
        dsm_skip_ws(p);
        if (**p == '}') break;
        char key[32];
        if (dsm_read_string(p, key, sizeof(key)) != 0) return;
        dsm_expect(p, ':');

        if (streq_static(key, "name")) {
            dsm_read_string(p, drv->name, sizeof(drv->name));
        } else if (streq_static(key, "path")) {
            dsm_read_string(p, drv->path, sizeof(drv->path));
        } else if (streq_static(key, "class")) {
            char cls[32];
            dsm_read_string(p, cls, sizeof(cls));
            if (streq_static(cls, "platform")) drv->class_id = 1;
            else if (streq_static(cls, "bus")) drv->class_id = 2;
            else if (streq_static(cls, "interrupt")) drv->class_id = 3;
            else if (streq_static(cls, "timer")) drv->class_id = 4;
            else if (streq_static(cls, "console")) drv->class_id = 5;
            else if (streq_static(cls, "storage")) drv->class_id = 6;
            else if (streq_static(cls, "fs")) drv->class_id = 7;
            else if (streq_static(cls, "fs-core")) drv->class_id = 8;
            else if (streq_static(cls, "net")) drv->class_id = 9;
            else if (streq_static(cls, "input")) drv->class_id = 10;
        } else if (streq_static(key, "required")) {
            dsm_skip_ws(p);
            if (**p == 't') { drv->required = 1; (*p) += 4; }
            else if (**p == 'f') { drv->required = 0; (*p) += 5; }
        } else if (streq_static(key, "depends")) {
            if (dsm_expect(p, '[') != 0) return;
            while (**p && **p != ']') {
                dsm_skip_ws(p);
                if (**p == ']') break;
                if (drv->depend_count < DSM_MAX_DEPENDS) {
                    dsm_read_string(p, drv->depends[drv->depend_count], 32);
                    drv->depend_count++;
                } else { dsm_skip_value(p); }
                dsm_skip_ws(p);
                if (**p == ',') { (*p)++; continue; }
                if (**p == ']') break;
            }
            dsm_expect(p, ']');
        } else if (streq_static(key, "provides")) {
            if (dsm_expect(p, '[') != 0) return;
            while (**p && **p != ']') {
                dsm_skip_ws(p);
                if (**p == ']') break;
                if (drv->provide_count < DSM_MAX_PROVIDES) {
                    dsm_read_string(p, drv->provides[drv->provide_count], 32);
                    drv->provide_count++;
                } else { dsm_skip_value(p); }
                dsm_skip_ws(p);
                if (**p == ',') { (*p)++; continue; }
                if (**p == ']') break;
            }
            dsm_expect(p, ']');
        } else {
            dsm_skip_value(p);
        }
        dsm_skip_ws(p);
        if (**p == ',') (*p)++;
    }
    dsm_expect(p, '}');
}

static void dsm_parse_stage(const char **p, dsm_stage_entry *stage) {
    stage->driver_count = 0;
    if (dsm_expect(p, '{') != 0) return;
    while (**p && **p != '}') {
        dsm_skip_ws(p);
        if (**p == '}') break;
        char key[32];
        if (dsm_read_string(p, key, sizeof(key)) != 0) return;
        dsm_expect(p, ':');

        if (streq_static(key, "id")) {
            dsm_skip_ws(p);
            u32 val = 0;
            while (**p >= '0' && **p <= '9') { val = val * 10 + (u32)(**p - '0'); (*p)++; }
            stage->id = val;
        } else if (streq_static(key, "name")) {
            dsm_read_string(p, stage->name, sizeof(stage->name));
        } else if (streq_static(key, "required")) {
            dsm_skip_ws(p);
            if (**p == 't') { stage->required = 1; (*p) += 4; }
            else if (**p == 'f') { stage->required = 0; (*p) += 5; }
        } else if (streq_static(key, "drivers")) {
            if (dsm_expect(p, '[') != 0) return;
            while (**p && **p != ']') {
                dsm_skip_ws(p);
                if (**p == ']') break;
                if (stage->driver_count < DSM_MAX_DRIVERS_PER_STAGE) {
                    dsm_parse_driver(p, &stage->drivers[stage->driver_count]);
                    stage->driver_count++;
                } else {
                    dsm_skip_value(p);
                }
                dsm_skip_ws(p);
                if (**p == ',') { (*p)++; continue; }
                if (**p == ']') break;
            }
            dsm_expect(p, ']');
        } else {
            dsm_skip_value(p);
        }
        dsm_skip_ws(p);
        if (**p == ',') (*p)++;
    }
    dsm_expect(p, '}');
}

static int dsm_parse_manifest(const char *text, u64 size) {
    g_stage_count = 0;
    const char *p = text;
    const char *end = text + size;

    while (p < end && *p) {
        dsm_skip_ws(&p);
        if (p >= end || !*p) break;
        if (*p == '{' || *p == '}') { p++; continue; }
        if (*p == ',' || *p == ':') { p++; continue; }
        if (*p == '[' || *p == ']') { p++; continue; }

        char key[32];
        if (dsm_read_string(&p, key, sizeof(key)) != 0) break;
        dsm_expect(&p, ':');

        if (streq_static(key, "stages")) {
            if (dsm_expect(&p, '[') != 0) {
                /* 严格错误策略插桩：stage 解析失败零降级（DKM-E08） */
                panic_full("DKM-E08 MANIFEST STAGES PARSE FAILED",
                           "dsm_parse_manifest: stages array malformed", 0);
                return -1;
            }
            while (p < end && *p) {
                dsm_skip_ws(&p);
                if (*p == ']') { p++; break; }
                if (g_stage_count < DSM_MAX_STAGES) {
                    dsm_parse_stage(&p, &g_stages[g_stage_count]);
                    g_stage_count++;
                }
                dsm_skip_ws(&p);
                if (*p == ',') p++;
            }
        } else {
            dsm_skip_value(&p);
        }
        dsm_skip_ws(&p);
        if (*p == ',' || *p == '}' || *p == ']') p++;
    }
    return 0;
}

void dsm_load_by_manifest(void) {
    dsm_load_by_manifest_ex(NULL);
}

static int dsm_ensure_manifest(void) {
    struct limine_module_response *rsp;
    struct limine_file *manifest_file;
    u64 i;

    if (g_stage_count) return 0;
    rsp = g_module_request.response;
    if (!rsp) {
        log_warn("[DSM] no Limine module response");
        /* 严格错误策略插桩：manifest 缺失零降级（DKM-E09） */
        panic_full("DKM-E09 NO LIMINE MODULE RESPONSE",
                   "dsm_ensure_manifest: no Limine module response", 0);
        return -1;
    }

    manifest_file = 0;
    for (i = 0; i < rsp->module_count; i++) {
        struct limine_file *f = rsp->modules[i];
        if (f && f->path && f->cmdline && streq_static(f->cmdline, "dkm:manifest")) {
            manifest_file = f;
            break;
        }
    }

    if (!manifest_file) {
        log_warn("[DSM] manifest not found in boot modules");
        /* 严格错误策略插桩：manifest 缺失零降级（DKM-E10） */
        panic_full("DKM-E10 MANIFEST NOT FOUND",
                   "dsm_ensure_manifest: dkm:manifest boot module missing", 0);
        return -1;
    }

    log_info("[DSM] parsing manifest");
    dsm_parse_manifest((const char *)manifest_file->address, manifest_file->size);
    log_info("[DSM] manifest parsed");
    log_hex64("[DSM] stages=", (u64)g_stage_count);
    return 0;
}

/* P3（万物互联）：模块从文件系统加载——boot module 未命中时的回退源。
 * 经服务总线取 fat32.drv 的 srv.fs.fat32.read_path（stage2 已加载，stage4
 * 模块经 FS 加载时可用）。返回 0 成功，out_buf 指向 fat32.drv 的静态缓冲。
 * 调用方须在下一次读文件前完成本次加载（本加载器单线程、逐个加载，满足）。 */
static int dsm_read_module_fs(const char *path, const void **out_buf, u64 *out_size) {
    const dkm_kernel_api *api = dkm_get_kernel_api();
    if (!api || !api->bus) return -1;
    void *fn = 0;
    if (api->bus->probe("srv.fs.fat32.read_path", &fn, 0) != 0) return -1;
    int (*read_path)(const char *, u8 **, u32 *) =
        (int (*)(const char *, u8 **, u32 *))fn;
    u8 *data = 0;
    u32 size = 0;
    if (read_path(path, &data, &size) != 0) return -2;
    if (out_buf) *out_buf = data;
    if (out_size) *out_size = (u64)size;
    return 0;
}

static void dsm_load_stage_range(const ini_config *cfg, int limit_stage, u32 max_stage_id) {
    if (dsm_ensure_manifest() != 0) return;

    for (u32 si = 0; si < g_stage_count; si++) {
        dsm_stage_entry *stage = &g_stages[si];
        if (limit_stage && stage->id > max_stage_id) continue;
        log_info("[DSM] stage begin");
        log_info(stage->name);
        log_hex64("[DSM] driver count=", stage->driver_count);

        /* P0：同 stage 内按依赖不动点迭代——每趟加载 deps 已满足的候选，
         * 直到全部 resolved；停滞即 DKM-E44/E47（见 dsm_report_dependency_stall）。
         * 多趟扫描 O(n²·趟数)，性能刻意不管，正确性第一。 */
        u32 pending = 0;
        for (u32 di = 0; di < stage->driver_count; di++) {
            if (!stage->drivers[di].resolved) pending++;
        }

        while (pending > 0) {
            u32 pending_before = pending;

            for (u32 di = 0; di < stage->driver_count; di++) {
                dsm_driver_entry *drv = &stage->drivers[di];
                const void *mod_addr;
                u64 mod_size;
                struct dkm_symbol_scan symbols;
                int result;

                if (drv->resolved) continue;

                /* === FUCK [drivers] 驱动开关检查 ===
                 * 关闭即退出候选（provides 不登记，依赖它的模块将 DKM-E44）。 */
                if (cfg) {
                    int enabled = ini_get_bool(cfg, "drivers", drv->name, 1);
                    if (!enabled) {
                        log_info("[DSM] driver disabled by FUCK: ");
                        log_info(drv->name);
                        drv->resolved = 1;
                        pending--;
                        continue;
                    }
                }

                /* P0：depends 校验——暂不满足先跳过，等本 stage 后续候选登记
                 * provides 后下一趟重试；整体停滞时统一判罚（零降级）。 */
                if (!dsm_deps_satisfied(drv, 0)) continue;

                /* 模块源解析（P3）：boot module 命中优先；否则经服务总线
                 * 从 FAT32 读取（stage≥4 模块不必预置为 boot module）。
                 * 双源皆无且 required → DKM-E46（零降级）。 */
                drv->boot_module = dsm_find_boot_module(drv->path);
                if (drv->boot_module) {
                    mod_addr = drv->boot_module->address;
                    mod_size = drv->boot_module->size;
                } else if (dsm_read_module_fs(drv->path, &mod_addr, &mod_size) == 0) {
                    log_info("[DSM] module from FS");
                    log_info(drv->name);
                } else {
                    log_info("[DSM] module not found (boot module + FS)");
                    log_info(drv->name);
                    drv->resolved = 1;
                    pending--;
                    if (drv->required && stage->required) {
                        /* P3：模块 boot module 与 FS 双源皆无零降级（DKM-E46） */
                        panic_full("DKM-E46 MODULE SOURCE MISSING",
                                   "module not found as boot module nor via srv.fs.fat32", 0);
                        break;
                    }
                    continue;
                }

                log_info("[DSM] loading driver");
                log_info(drv->name);

                if (dkm_check_elf64(mod_addr, mod_size) != 0) {
                    log_error("[DSM] ELF check failed");
                    if (drv->required && stage->required) {
                        panic_full("DKM-E05 REQUIRED DRIVER ELF INVALID",
                                   "required driver module failed ELF64 check", 0);
                    }
                    drv->resolved = 1;
                    pending--;
                    continue;
                }

                if (dkm_scan_symbols(mod_addr, mod_size, &symbols) != 0) {
                    log_error("[DSM] symbol scan failed");
                    if (drv->required && stage->required) {
                        panic_full("DKM-E06 REQUIRED DRIVER SYMBOL SCAN FAILED",
                                   "required driver symbol scan failed", 0);
                    }
                    drv->resolved = 1;
                    pending--;
                    continue;
                }

                result = dkm_load_elf_rel(mod_addr, mod_size, &symbols);
                if (result != 0) {
                    log_error("[DSM] load failed");
                    if (drv->required && stage->required) {
                        /* 严格错误策略插桩：required 驱动加载失败零降级（DKM-E07） */
                        panic_full("DKM-E07 REQUIRED DRIVER LOAD FAILED",
                                   "required driver relocation/load failed", 0);
                    }
                    drv->resolved = 1;
                    pending--;
                } else {
                    drv->loaded = 1;
                    drv->resolved = 1;
                    pending--;
                    /* P0：加载成功才登记 provides（disabled/missing/failed 一律不登记） */
                    dsm_register_provides(drv);
                    /* P1：stage>=4 provides 总线契约校验（DKM-E45） */
                    dsm_verify_provides(stage, drv);
                }
            }

            if (pending > 0 && pending == pending_before) {
                /* 一整趟零进展：真缺依赖（DKM-E44）或依赖环（DKM-E47） */
                dsm_report_dependency_stall(stage);
                /* fallback：停滞的候选全部放弃（不登记 provides），结束本 stage */
                for (u32 di = 0; di < stage->driver_count; di++) {
                    if (stage->drivers[di].resolved) continue;
                    log_warn("[DSM] fallback: driver skipped (unresolved deps): ");
                    log_info(stage->drivers[di].name);
                    stage->drivers[di].resolved = 1;
                }
                pending = 0;
            }
        }
        log_info("[DSM] stage end");
    }
}

void dsm_load_by_manifest_upto(const ini_config *cfg, u32 max_stage_id) {
    dsm_load_stage_range(cfg, 1, max_stage_id);
}

/* 按名加载公共实现。strict_deps=1：depends 校验强制（DKM-E44）——供
 * kern.dkm.load_by_name（万物互联 P3）等常规路径使用；
 * strict_deps=0：顺序豁免版——唯一调用方是 kernel/main.c VMM 路径的
 * pci+ahci 预加载（必须先于 apic/stage0，否则 guest 陷入 IDTR=0
 * VM-exit 风暴，见 main.c 注释）。设计内顺序豁免，登记于设计文档 §6。 */
static void dsm_load_named_impl(const ini_config *cfg, const char *name, int strict_deps) {
    if (!name || dsm_ensure_manifest() != 0) {
        /* 严格错误策略插桩：按名加载 manifest 缺失零降级（DKM-E11） */
        panic_full("DKM-E11 NAMED LOAD MANIFEST MISSING",
                   "dsm_load_named: null name or manifest unavailable", 0);
        return;
    }

    for (u32 si = 0; si < g_stage_count; si++) {
        dsm_stage_entry *stage = &g_stages[si];
        for (u32 di = 0; di < stage->driver_count; di++) {
            dsm_driver_entry *drv = &stage->drivers[di];
            const void *mod_addr;
            u64 mod_size;
            struct dkm_symbol_scan symbols;
            int result;

            if (!streq_static(drv->name, name)) continue;
            if (drv->loaded) return;
            if (cfg && !ini_get_bool(cfg, "drivers", drv->name, 1)) {
                log_info("[DSM] driver disabled by FUCK: ");
                log_info(drv->name);
                return;
            }
            /* P0：按名加载同样强制依赖（此时无同 stage 重试机会，miss 即判罚） */
            if (strict_deps && !dsm_deps_satisfied(drv, 0)) {
                log_error("[DSM] named driver dependency not satisfied: ");
                log_info(drv->name);
                panic_full("DKM-E44 DEPENDENCY NOT SATISFIED",
                           "dsm_load_named: depends token not provided by any loaded driver", 0);
                return;
            }
            /* P3：boot module 优先，回退服务总线 FS 读取。 */
            drv->boot_module = dsm_find_boot_module(drv->path);
            if (drv->boot_module) {
                mod_addr = drv->boot_module->address;
                mod_size = drv->boot_module->size;
            } else if (dsm_read_module_fs(drv->path, &mod_addr, &mod_size) == 0) {
                log_info("[DSM] named module from FS");
                log_info(drv->name);
            } else {
                log_warn("[DSM] named module not found (boot module + FS)");
                log_info(name);
                /* 严格错误策略插桩：named 驱动缺模块零降级（DKM-E12） */
                panic_full("DKM-E12 NAMED DRIVER MODULE MISSING",
                           "dsm_load_named: named driver not found as boot module nor via FS", 0);
                return;
            }
            log_info("[DSM] loading named driver");
            log_info(drv->name);
            if (dkm_check_elf64(mod_addr, mod_size) != 0) {
                log_error("[DSM] ELF check failed");
                /* 严格错误策略插桩：named 驱动 ELF 校验失败零降级（DKM-E13） */
                panic_full("DKM-E13 NAMED DRIVER ELF INVALID",
                           "dsm_load_named: named driver failed ELF64 check", 0);
                return;
            }
            if (dkm_scan_symbols(mod_addr, mod_size, &symbols) != 0) {
                log_error("[DSM] symbol scan failed");
                /* 严格错误策略插桩：named 驱动符号扫描失败零降级（DKM-E14） */
                panic_full("DKM-E14 NAMED DRIVER SYMBOL SCAN FAILED",
                           "dsm_load_named: named driver symbol scan failed", 0);
                return;
            }
            result = dkm_load_elf_rel(mod_addr, mod_size, &symbols);
            if (result != 0) {
                log_error("[DSM] load failed");
                /* 严格错误策略插桩：named 驱动加载失败零降级（DKM-E15） */
                panic_full("DKM-E15 NAMED DRIVER LOAD FAILED",
                           "dsm_load_named: named driver relocation/load failed", 0);
                return;
            }
            drv->loaded = 1;
            drv->resolved = 1;
            /* P0：按名加载成功同样登记 provides */
            dsm_register_provides(drv);
            /* P1：stage>=4 provides 总线契约校验（DKM-E45） */
            dsm_verify_provides(stage, drv);
            return;
        }
    }
    log_warn("[DSM] named driver not in manifest");
    log_info(name);
    /* 严格错误策略插桩：named 驱动不在 manifest 零降级（DKM-E16） */
    panic_full("DKM-E16 NAMED DRIVER NOT IN MANIFEST",
               "dsm_load_named: named driver not found in manifest", 0);
}

void dsm_load_named(const ini_config *cfg, const char *name) {
    dsm_load_named_impl(cfg, name, 1);
}

void dsm_load_named_boot(const ini_config *cfg, const char *name) {
    dsm_load_named_impl(cfg, name, 0);
}

void dsm_load_by_manifest_ex(const ini_config *cfg) {
    dsm_load_stage_range(cfg, 0, 0);
}
