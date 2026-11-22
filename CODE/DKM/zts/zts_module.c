/* zts_module.c — ZTS 模块注册表 / 槽位 / 加载 / 热替换
 *
 * 槽位不可变：模块镜像一旦装入 slot 不再修改；热替换分配新 slot，旧 slot
 * 由检查点引用（zts_slot_ref），回滚时重新指向。引用归零才回收。
 */
#include "zts.h"

#define ZTS_SLOT_FREE 0xFFFFFFFFu

static u8 g_slots[ZTS_SLOT_COUNT][ZTS_SLOT_SIZE] __attribute__((aligned(64)));
static u32 g_slot_refs[ZTS_SLOT_COUNT];
static u8 g_slot_used[ZTS_SLOT_COUNT];

static zts_module g_modules[ZTS_MAX_MODULES];
static int g_module_count;
static u32 g_registry_lock;

/* manifest 提供的依赖（ZTS.CNF 每项一行；header 内只有数量） */
static char g_dep_csv[ZTS_MAX_DEPS][32];
static int g_dep_count;

void *zts_slot_base(u32 slot) {
    if (slot >= ZTS_SLOT_COUNT) return 0;
    return g_slots[slot];
}

int zts_slot_alloc(void) {
    u32 irq = zts_lock();
    for (u32 i = 0; i < ZTS_SLOT_COUNT; i++) {
        if (!g_slot_used[i]) {
            g_slot_used[i] = 1;
            g_slot_refs[i] = 1;
            zts_unlock();
            (void)irq;
            return (int)i;
        }
    }
    zts_unlock();
    (void)irq;
    return -1;
}

void zts_slot_ref(u32 slot) {
    if (slot >= ZTS_SLOT_COUNT) return;
    u32 irq = zts_lock();
    if (g_slot_used[slot]) g_slot_refs[slot]++;
    zts_unlock();
    (void)irq;
}

void zts_slot_unref(u32 slot) {
    if (slot >= ZTS_SLOT_COUNT) return;
    u32 irq = zts_lock();
    if (g_slot_used[slot] && g_slot_refs[slot]) {
        g_slot_refs[slot]--;
        if (!g_slot_refs[slot]) {
            g_slot_used[slot] = 0;
            zts_memset(g_slots[slot], 0, ZTS_SLOT_SIZE);
        }
    }
    zts_unlock();
    (void)irq;
}

void zts_set_pending_deps(const char *csv) {
    g_dep_count = 0;
    zts_memset(g_dep_csv, 0, sizeof(g_dep_csv));
    if (!csv) return;
    u32 i = 0;
    while (csv[i] && g_dep_count < ZTS_MAX_DEPS) {
        while (csv[i] == ' ' || csv[i] == ',') i++;
        if (!csv[i]) break;
        u32 n = 0;
        while (csv[i] && csv[i] != ',' && n + 1 < 32) g_dep_csv[g_dep_count][n++] = csv[i++];
        g_dep_csv[g_dep_count][n] = 0;
        if (n) g_dep_count++;
    }
}

static int dep_optional(const char *s) {
    return s && (s[0] == '?');
}

static int dep_satisfied(const char *name) {
    zts_module *m = zts_module_find(name);
    return m && m->loaded && m->initialized;
}

int zts_module_check_dependencies(const zts_header *hdr) {
    (void)hdr;
    int missing = 0;
    for (int i = 0; i < g_dep_count; i++) {
        const char *nm = g_dep_csv[i];
        if (dep_optional(nm)) nm++;
        if (!dep_satisfied(nm)) {
            if (dep_optional(nm)) {
                zts_log("[zts] optional dependency absent:");
                zts_log(nm);
            } else {
                zts_log("[zts] required dependency missing:");
                zts_log(nm);
                missing++;
            }
        }
    }
    if (missing) {
        zts_log(ZTS_E06_DEP);
        return -1;
    }
    return 0;
}

void zts_modules_init(void) {
    zts_memset(g_modules, 0, sizeof(g_modules));
    zts_memset(g_slot_refs, 0, sizeof(g_slot_refs));
    zts_memset(g_slot_used, 0, sizeof(g_slot_used));
    g_module_count = 0;
    g_registry_lock = 0;
    g_dep_count = 0;
    zts_memset(g_dep_csv, 0, sizeof(g_dep_csv));
}

/* 注册表只由宿主控制路径访问（无 ISR/并发任务），因此不加锁；
 * 模块执行代码不在任何 zts 内部锁内运行。 */
zts_module *zts_module_find(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < g_module_count; i++) {
        if (zts_str_eq(g_modules[i].name, name)) return &g_modules[i];
    }
    return 0;
}

zts_module *zts_module_find_uuid(const u8 uuid[ZTS_UUID_LEN]) {
    if (!uuid) return 0;
    for (int i = 0; i < g_module_count; i++) {
        int eq = 1;
        for (int j = 0; j < ZTS_UUID_LEN; j++) {
            if (g_modules[i].uuid[j] != uuid[j]) { eq = 0; break; }
        }
        if (eq) return &g_modules[i];
    }
    return 0;
}

int zts_module_count(void) {
    return g_module_count;
}

zts_module *zts_module_at(u32 index) {
    if (index >= (u32)g_module_count) return 0;
    return &g_modules[index];
}

void zts_modules_restore(const zts_module *mods, u32 count) {
    if (count > ZTS_MAX_MODULES) count = ZTS_MAX_MODULES;
    zts_memset(g_modules, 0, sizeof(g_modules));
    for (u32 i = 0; i < count; i++) g_modules[i] = mods[i];
    g_module_count = (int)count;
}

static int header_validate(const zts_header *h) {
    if (h->magic != ZTS_MAGIC) return -1;
    if (h->version != ZTS_FORMAT_VERSION) return -1;
    if (h->api_min_version > ZTS_API_VERSION) return -1;
    int i = 0;
    while (i < ZTS_MAX_NAME && h->name[i]) i++;
    if (i == 0 || i >= ZTS_MAX_NAME) return -1;
    return 0;
}

/* 从完整 .zts 文件镜像加载。file 仅用于持久化记录。 */
int zts_module_load_buffer(const u8 *data, u32 size, const char *file,
                           int autoload, zts_module **out) {
    if (!data || size < ZTS_HEADER_SIZE + 16) {
        zts_log(ZTS_E01_HEADER);
        return -1;
    }
    zts_header hdr;
    zts_memcpy(&hdr, data, sizeof(hdr));
    if (header_validate(&hdr) != 0) {
        zts_log(ZTS_E01_HEADER);
        return -1;
    }
    /* body 由 16 字节对齐点开始 */
    u32 body_off = (ZTS_HEADER_SIZE + 15u) & ~15u;
    if (body_off >= size) return -1;
    const u8 *body = data + body_off;
    u32 body_size = size - body_off;

    if (hdr.checksum) {
        u32 want = zts_fnv1a32(body, body_size);
        if (want != hdr.checksum) {
            zts_log(ZTS_E05_CHECKSUM);
            zts_log_hex("  got=", want);
            zts_log_hex("  hdr=", hdr.checksum);
            return -1;
        }
    }

    if (zts_module_find(hdr.name)) {
        zts_log("[zts] module already loaded:");
        zts_log(hdr.name);
        return -1;
    }
    if (zts_module_check_dependencies(&hdr) != 0) return -1;

    int slot = zts_slot_alloc();
    if (slot < 0) {
        zts_log(ZTS_E07_SLOT);
        return -1;
    }

    zts_module mod;
    zts_memset(&mod, 0, sizeof(mod));
    zts_str_copy(mod.name, hdr.name, ZTS_MAX_NAME);
    zts_memcpy(mod.uuid, hdr.uuid, ZTS_UUID_LEN);
    mod.category = hdr.category;
    mod.major = hdr.major_version;
    mod.minor = hdr.minor_version;
    mod.patch = hdr.patch_version;
    mod.slot = (u32)slot;
    mod.state = ZTS_STATE_LOADED;
    zts_str_copy(mod.file, file ? file : "", ZTS_MAX_FILE);

    u64 io = hdr.init_func_offset;
    u64 eo = hdr.execute_func_offset;
    u64 co = hdr.cleanup_func_offset;
    int rc = zts_elf_load(body, body_size, (u32)slot, &mod, &io, &eo, &co);
    if (rc != 0) {
        if (rc == -4) zts_log(ZTS_E04_SYMBOL);
        else if (rc == -3) zts_log(ZTS_E03_RELOC);
        else if (rc == -7) zts_log(ZTS_E07_SLOT);
        else zts_log(ZTS_E02_ELF);
        zts_slot_unref((u32)slot);
        return -1;
    }
    mod.image_crc = zts_crc32(mod.code_base, mod.image_size);
    mod.loaded = 1;

    if (autoload) {
        if (mod.init) {
            int r = mod.init(zts_api_get());
            if (r < 0) {
                zts_log("[zts] module init failed:");
                zts_log(mod.name);
                zts_slot_unref((u32)slot);
                return -1;
            }
        }
        mod.initialized = 1;
        mod.state = ZTS_STATE_ACTIVE;
    }

    if (g_module_count >= ZTS_MAX_MODULES) {
        zts_log(ZTS_E07_SLOT);
        zts_slot_unref((u32)slot);
        return -1;
    }
    g_modules[g_module_count] = mod;
    g_module_count++;

    zts_log("[zts] loaded module:");
    zts_log(mod.name);
    zts_log_dec("  version=", ((u64)mod.major << 16) | mod.minor);
    zts_log_dec("  slot=", slot);
    if (out) *out = zts_module_find(mod.name);
    return 0;
}

int zts_module_load_file(const char *file, int autoload) {
    u8 *data = 0;
    u32 size = 0;
    if (zts_store_read(file, &data, &size) != 0) {
        zts_log("[zts] module file not found:");
        zts_log(file);
        return -1;
    }
    return zts_module_load_buffer(data, size, file, autoload, 0);
}

int zts_module_unload(const char *name) {
    zts_module *m = zts_module_find(name);
    if (!m) return -1;
    if (m->initialized && m->cleanup) m->cleanup();
    u32 slot = m->slot;
    int idx = -1;
    for (int i = 0; i < g_module_count; i++) {
        if (&g_modules[i] == m) { idx = i; break; }
    }
    if (idx >= 0) {
        for (int i = idx; i < g_module_count - 1; i++) g_modules[i] = g_modules[i + 1];
        g_module_count--;
        zts_memset(&g_modules[g_module_count], 0, sizeof(zts_module));
    }
    zts_slot_unref(slot);
    zts_log("[zts] unloaded module:");
    zts_log(name);
    return 0;
}

int zts_module_execute(const char *name, void *in, void *out) {
    zts_module *m = zts_module_find(name);
    if (!m) {
        zts_log("[zts] execute: module not found:");
        zts_log(name);
        return -1;
    }
    if (!m->initialized) {
        if (m->init) {
            int r = m->init(zts_api_get());
            if (r < 0) {
                zts_log(ZTS_E12_EXEC);
                return -1;
            }
        }
        m->initialized = 1;
    }
    if (!m->execute) return 0;
    return m->execute(in, out, 0);
}

/* 热替换：新模块先 init，成功才切换；失败保留旧模块。 */
int zts_module_hotupdate(const char *name, const char *file) {
    zts_module *old = zts_module_find(name);
    if (!old) {
        zts_log("[zts] hotupdate: module not found:");
        zts_log(name);
        return -1;
    }
    old->state = ZTS_STATE_UPDATING;
    u64 ckpt = zts_ckpt_create("pre-hotupdate");

    u8 *data = 0;
    u32 size = 0;
    if (zts_store_read(file, &data, &size) != 0) {
        zts_log("[zts] hotupdate: read failed:");
        zts_log(file);
        old->state = ZTS_STATE_ACTIVE;
        return -1;
    }

    zts_header hdr;
    zts_memcpy(&hdr, data, sizeof(hdr));
    if (header_validate(&hdr) != 0) {
        zts_log(ZTS_E01_HEADER);
        old->state = ZTS_STATE_ACTIVE;
        return -1;
    }
    if (!zts_str_eq(hdr.name, name)) {
        zts_log("[zts] hotupdate: module name mismatch");
        old->state = ZTS_STATE_ACTIVE;
        return -1;
    }
    if (zts_module_check_dependencies(&hdr) != 0) {
        old->state = ZTS_STATE_ACTIVE;
        return -1;
    }

    zts_module fresh;
    zts_memset(&fresh, 0, sizeof(fresh));
    int slot = zts_slot_alloc();
    if (slot < 0) {
        zts_log(ZTS_E07_SLOT);
        old->state = ZTS_STATE_ACTIVE;
        return -1;
    }
    u32 body_off = (ZTS_HEADER_SIZE + 15u) & ~15u;
    if (size <= body_off || (hdr.checksum && zts_fnv1a32(data + body_off, size - body_off) != hdr.checksum)) {
        zts_log(ZTS_E05_CHECKSUM);
        zts_slot_unref((u32)slot);
        old->state = ZTS_STATE_ACTIVE;
        return -1;
    }
    zts_str_copy(fresh.name, hdr.name, ZTS_MAX_NAME);
    zts_memcpy(fresh.uuid, hdr.uuid, ZTS_UUID_LEN);
    fresh.category = hdr.category;
    fresh.major = hdr.major_version;
    fresh.minor = hdr.minor_version;
    fresh.patch = hdr.patch_version;
    fresh.slot = (u32)slot;
    fresh.loaded = 1;
    fresh.state = ZTS_STATE_ACTIVE;
    zts_str_copy(fresh.file, file ? file : "", ZTS_MAX_FILE);
    u64 io = hdr.init_func_offset, eo = hdr.execute_func_offset, co = hdr.cleanup_func_offset;
    int rc = zts_elf_load(data + body_off, size - body_off, (u32)slot, &fresh, &io, &eo, &co);
    if (rc != 0) {
        if (rc == -4) zts_log(ZTS_E04_SYMBOL);
        else if (rc == -3) zts_log(ZTS_E03_RELOC);
        else zts_log(ZTS_E02_ELF);
        zts_slot_unref((u32)slot);
        old->state = ZTS_STATE_ACTIVE;
        return -1;
    }
    fresh.image_crc = zts_crc32(fresh.code_base, fresh.image_size);
    if (fresh.init) {
        if (fresh.init(zts_api_get()) < 0) {
            zts_log("[zts] hotupdate: new module init failed");
            zts_slot_unref((u32)slot);
            old->state = ZTS_STATE_ACTIVE;
            return -1;
        }
    }
    fresh.initialized = 1;

    /* 切换注册表条目（保留旧 slot，由检查点引用） */
    zts_module old_copy = *old;
    *old = fresh;

    /* 持久化 A/B 状态；失败则回退到旧版本（仍保留检查点用于诊断） */
    if (zts_store_write_state() != 0) {
        zts_log(ZTS_E10_STORE);
        *old = old_copy;
        if (fresh.cleanup) fresh.cleanup();
        zts_slot_unref((u32)slot);
        old->state = ZTS_STATE_ACTIVE;
        if (ckpt) zts_ckpt_restore(ckpt);
        return -1;
    }

    /* 旧模块 cleanup（切换后调用） */
    if (old_copy.initialized && old_copy.cleanup) old_copy.cleanup();

    zts_log("[zts] hotupdate complete:");
    zts_log(name);
    return 0;
}

void zts_modules_shutdown(void) {
    for (int i = g_module_count - 1; i >= 0; i--) {
        zts_module *m = &g_modules[i];
        if (m->initialized && m->cleanup) m->cleanup();
        m->loaded = 0;
        m->initialized = 0;
    }
    g_module_count = 0;
}
