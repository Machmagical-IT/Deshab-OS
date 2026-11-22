/* zts_store.c — 模块与状态持久化
 *
 * 读取优先级:
 *   1) EFI 引导器预加载的 boot module（cmdline "zts:manifest" / "zts:module:<file>"）
 *   2) srv.fs.fat32.read_path（路径 "zts/<file>"）
 * 写入（仅 A/B 状态与模块副本）:
 *   srv.fs.fat32.find_dir_path("ZTS") + srv.fs.fat32.write_file_in_dir
 *
 * 状态采用 A/B 双槽 + SEQ + CRC32；读回校验通过才切换活动槽。
 */
#include "zts.h"

/* limine_file 前缀布局（与引导器/UTSM 一致），只使用前 5 个字段 */
typedef struct {
    u64 revision;
    void *address;
    u64 size;
    char *path;
    char *cmdline;
    u32 media_type;
    u32 unused;
    u32 tftp_ip;
    u32 tftp_port;
    u32 partition_index;
    u32 mbr_disk_id;
    u8 gpt_disk_uuid[16];
    u8 gpt_part_uuid[16];
    u8 part_uuid[16];
} zts_limine_file;

typedef struct {
    u64 revision;
    u64 module_count;
    zts_limine_file **modules;
} zts_limine_module_response;

#define ZTS_STORE_BUF_SIZE (256u * 1024u)
static u8 g_store_buf[4][ZTS_STORE_BUF_SIZE];
static u32 g_store_buf_idx;

static char g_manifest[8192];
static int g_manifest_loaded;

/* 状态表 */
#define ZTS_STATE_MAX 64

static zts_state_mod g_state[ZTS_STATE_MAX];
static int g_state_count;
static u32 g_state_seq;
static int g_state_side;      /* 0=A 1=B */

void zts_store_init(void) {
    g_store_buf_idx = 0;
    g_manifest_loaded = 0;
    zts_memset(g_state, 0, sizeof(g_state));
    g_state_count = 0;
    g_state_seq = 0;
    g_state_side = 0;
}

/* ============================================================
 * 基础工具
 * ============================================================ */
static int str_starts(const char *s, const char *p) {
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}

static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

static int name11(const char *in, char out[12]) {
    int n = 0, e = 0;
    const char *s = in;
    while (*s && *s != '.' && n < 8) out[n++] = *s++;
    while (n < 8) out[n++] = ' ';
    if (*s == '.') s++;
    while (*s && e < 3) out[8 + e++] = *s++;
    while (e < 3) out[8 + e++] = ' ';
    out[11] = 0;
    return 11;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void uuid_to_hex(const u8 u[16], char out[33]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[i * 2] = h[u[i] >> 4]; out[i * 2 + 1] = h[u[i] & 15]; }
    out[32] = 0;
}

static int uuid_from_hex(const char *s, u8 u[16]) {
    for (int i = 0; i < 16; i++) {
        int a = hex_val(s[i * 2]);
        int b = hex_val(s[i * 2 + 1]);
        if (a < 0 || b < 0) return -1;
        u[i] = (u8)((a << 4) | b);
    }
    return 0;
}

/* ============================================================
 * boot module 查找
 * ============================================================ */
static zts_limine_file *find_boot_file(const char *file) {
    const struct dkm_kernel_api *api = zts_kapi();
    if (!api || !api->boot_modules_response) return 0;
    const zts_limine_module_response *rsp =
        (const zts_limine_module_response *)api->boot_modules_response;
    const char *want_base = base_name(file);
    for (u64 i = 0; i < rsp->module_count; i++) {
        zts_limine_file *f = rsp->modules[i];
        if (!f || !f->path) continue;
        /* zts:manifest 精确匹配 */
        if (zts_str_eq(file, "ZTS.CNF") && f->cmdline && zts_str_eq(f->cmdline, "zts:manifest"))
            return f;
        if (f->cmdline && str_starts(f->cmdline, "zts:module:")) {
            const char *m = f->cmdline + 11;
            if (zts_str_eq(m, want_base)) return f;
        }
        if (zts_str_eq(base_name(f->path), want_base)) return f;
    }
    return 0;
}

static void *fat32_service(const char *name) {
    const struct dkm_kernel_api *api = zts_kapi();
    if (!api || !api->bus) return 0;
    void *fn = 0;
    if (api->bus->probe(name, &fn, 0) != 0) return 0;
    return fn;
}

static int fat32_read_path(const char *path, u8 **out, u32 *size) {
    int (*read_path)(const char *, u8 **, u32 *) =
        (int (*)(const char *, u8 **, u32 *))fat32_service("srv.fs.fat32.read_path");
    if (!read_path) return -1;
    return read_path(path, out, size);
}

/* ============================================================
 * 读
 * ============================================================ */
int zts_store_read(const char *file, u8 **out, u32 *size) {
    if (!file || !out || !size) return -1;

    zts_limine_file *bf = find_boot_file(file);
    if (bf && bf->address && bf->size) {
        *out = (u8 *)bf->address;
        *size = (u32)bf->size;
        return 0;
    }

    char path[128];
    if (str_starts(file, "zts/") || str_starts(file, "ZTS/")) {
        zts_str_copy(path, file, sizeof(path));
    } else {
        zts_str_copy(path, "zts/", sizeof(path));
        u32 n = (u32)zts_strlen(path);
        zts_str_copy(path + n, file, (u32)sizeof(path) - n);
    }

    u8 *data = 0;
    u32 n = 0;
    if (fat32_read_path(path, &data, &n) != 0 || !data) return -1;
    if (n > ZTS_STORE_BUF_SIZE) return -1;
    u8 *buf = g_store_buf[g_store_buf_idx++ & 3];
    zts_memcpy(buf, data, n);
    *out = buf;
    *size = n;
    return 0;
}

/* ============================================================
 * 写
 * ============================================================ */
static u32 zts_dir_clus(void) {
    u32 (*find_dir)(const char *) = (u32 (*)(const char *))
        fat32_service("srv.fs.fat32.find_dir_path");
    if (!find_dir) return 0;
    return find_dir("ZTS");
}

int zts_store_write(const char *name, const u8 *data, u32 size) {
    if (!name || !data) return -1;
    int (*write_file)(u32, const char *, const u8 *, u32) =
        (int (*)(u32, const char *, const u8 *, u32))
        fat32_service("srv.fs.fat32.write_file_in_dir");
    if (!write_file) return -1;
    u32 clus = zts_dir_clus();
    if (!clus) return -1;
    char n11[12];
    name11(name, n11);
    static u8 wbuf[ZTS_STORE_BUF_SIZE];
    if (size > sizeof(wbuf)) return -1;
    zts_memcpy(wbuf, data, size);
    return write_file(clus, n11, wbuf, size);
}

/* ============================================================
 * 清单
 * ============================================================ */
const char *zts_store_manifest(void) {
    if (g_manifest_loaded) return g_manifest;
    u8 *data = 0;
    u32 size = 0;
    if (zts_store_read("ZTS.CNF", &data, &size) != 0 || !data) return 0;
    if (size >= sizeof(g_manifest)) size = sizeof(g_manifest) - 1;
    zts_memcpy(g_manifest, data, size);
    g_manifest[size] = 0;
    g_manifest_loaded = 1;
    return g_manifest;
}

/* ============================================================
 * 状态 A/B
 * ============================================================ */
static zts_state_mod *state_find(const char *name) {
    for (int i = 0; i < g_state_count; i++) {
        if (zts_str_eq(g_state[i].name, name)) return &g_state[i];
    }
    return 0;
}

const zts_state_mod *zts_store_state_find(const char *name) {
    return state_find(name);
}

const char *zts_store_state_file(const char *name) {
    zts_state_mod *m = state_find(name);
    return m ? m->file : 0;
}

static int parse_state(const char *text, u32 len, int side) {
    u32 seq = 0;
    u32 crc_want = 0;
    int have_crc = 0;
    zts_state_mod mods[ZTS_STATE_MAX];
    int count = 0;
    zts_memset(mods, 0, sizeof(mods));

    u32 pos = 0, crc_off = 0;
    while (pos < len) {
        u32 eol = pos;
        while (eol < len && text[eol] != '\n') eol++;
        u32 start = pos;
        pos = (eol < len) ? eol + 1 : len;
        if (eol > start && text[eol - 1] == '\r') eol--;
        if (eol <= start) continue;
        if (str_starts(text + start, "SEQ=")) {
            u32 v = 0;
            for (u32 i = start + 4; i < eol; i++) {
                if (text[i] < '0' || text[i] > '9') break;
                v = v * 10 + (u32)(text[i] - '0');
            }
            seq = v;
        } else if (str_starts(text + start, "CRC=")) {
            crc_off = start;
            have_crc = 1;
            u32 v = 0;
            for (u32 i = start + 4; i < eol && i < start + 12; i++) {
                int h = hex_val(text[i]);
                if (h < 0) break;
                v = (v << 4) | (u32)h;
            }
            crc_want = v;
        } else if (str_starts(text + start, "MOD|")) {
            char fields[5][64];
            int nf = 0, fi = 0;
            zts_memset(fields, 0, sizeof(fields));
            for (u32 i = start + 4; i <= eol && nf < 5; i++) {
                char c = (i == eol) ? '|' : text[i];
                if (c == '|') {
                    fields[nf][fi] = 0;
                    nf++;
                    fi = 0;
                    if (nf >= 5) break;
                } else if (fi < 63) {
                    fields[nf][fi++] = c;
                }
            }
            if (nf >= 5 && count < ZTS_STATE_MAX) {
                zts_state_mod *m = &mods[count];
                zts_str_copy(m->name, fields[0], ZTS_MAX_NAME);
                uuid_from_hex(fields[1], m->uuid);
                m->version = 0;
                for (const char *s = fields[2]; *s; s++) {
                    if (*s < '0' || *s > '9') break;
                    m->version = m->version * 10 + (u32)(*s - '0');
                }
                zts_str_copy(m->file, fields[3], sizeof(m->file));
                m->source = fields[4][0] ? fields[4][0] : 'A';
                count++;
            }
        }
    }

    if (!have_crc || !seq) return -1;
    u32 crc = zts_crc32(text, crc_off);
    if (crc != crc_want) {
        zts_log(ZTS_E13_STATE);
        return -1;
    }
    (void)side;
    return (int)seq;
}

int zts_store_load_state(void) {
    zts_memset(g_state, 0, sizeof(g_state));
    g_state_count = 0;
    g_state_seq = 0;
    g_state_side = 0;

    int best_seq = 0;
    int best_side = -1;
    int best_count = 0;
    zts_state_mod best[ZTS_STATE_MAX];
    zts_memset(best, 0, sizeof(best));

    for (int side = 0; side < 2; side++) {
        const char *name = side == 0 ? "STATEA.CNF" : "STATEB.CNF";
        u8 *data = 0;
        u32 size = 0;
        if (zts_store_read(name, &data, &size) != 0 || !data) continue;
        /* 只解析前缀，确保 text 以 NUL 结尾由临时缓冲保证 */
        static char text[8192];
        if (size >= sizeof(text)) size = sizeof(text) - 1;
        zts_memcpy(text, data, size);
        text[size] = 0;
        int seq = parse_state(text, size, side);
        if (seq > 0 && seq > best_seq) {
            best_seq = seq;
            best_side = side;
            /* 重新解析到 best[] */
            zts_memset(best, 0, sizeof(best));
            /* 复用 parse_state 的副作用不可取，简单再扫一遍 MOD| 行 */
            best_count = 0;
            u32 pos = 0;
            while (pos < size && best_count < ZTS_STATE_MAX) {
                u32 eol = pos;
                while (eol < size && text[eol] != '\n') eol++;
                if (eol > pos && str_starts(text + pos, "MOD|")) {
                    char fields[5][64];
                    int nf = 0, fi = 0;
                    zts_memset(fields, 0, sizeof(fields));
                    for (u32 i = pos + 4; i <= eol && nf < 5; i++) {
                        char c = (i == eol) ? '|' : text[i];
                        if (c == '|') { fields[nf][fi] = 0; nf++; fi = 0; if (nf >= 5) break; }
                        else if (fi < 63) fields[nf][fi++] = c;
                    }
                    if (nf >= 5) {
                        zts_state_mod *m = &best[best_count++];
                        zts_str_copy(m->name, fields[0], ZTS_MAX_NAME);
                        uuid_from_hex(fields[1], m->uuid);
                        for (const char *s = fields[2]; *s; s++) {
                            if (*s < '0' || *s > '9') break;
                            m->version = m->version * 10 + (u32)(*s - '0');
                        }
                        zts_str_copy(m->file, fields[3], sizeof(m->file));
                        m->source = fields[4][0] ? fields[4][0] : 'A';
                    }
                }
                pos = (eol < size) ? eol + 1 : size;
            }
        }
    }

    if (best_side >= 0) {
        g_state_seq = (u32)best_seq;
        g_state_side = best_side;
        g_state_count = best_count;
        for (int i = 0; i < best_count; i++) g_state[i] = best[i];
        zts_log("[zts] state loaded:");
        zts_log(best_side == 0 ? "STATEA" : "STATEB");
        zts_log_dec("  seq=", g_state_seq);
        return 0;
    }
    return -1;
}

/* 把当前模块注册表 + 镜像副本写入非活动状态槽 */
int zts_store_write_state(void) {
    int side = g_state_side ? 0 : 1;    /* 非活动槽 */
    char side_name = side ? 'B' : 'A';
    const char *state_name = side ? "STATEB.CNF" : "STATEA.CNF";

    static char text[8192];
    u32 n = 0;
    n += (u32)__builtin_snprintf(text + n, sizeof(text) - n, "ZTSSTATE=1\nSEQ=%u\n", (unsigned)(g_state_seq + 1));

    int count = zts_module_count();
    for (int i = 0; i < count; i++) {
        zts_module *m = zts_module_at((u32)i);
        /* 读当前源文件字节，写到本侧副本；读不到则沿用现有文件名 */
        u8 *data = 0;
        u32 size = 0;
        char copy_name[16] = {0};
        if (zts_store_read(m->file, &data, &size) == 0 && data) {
            u32 h = zts_fnv1a32(m->name, zts_strlen(m->name)) & 0xFFFFFFu;
            __builtin_snprintf(copy_name, sizeof(copy_name), "M%06X%c.ZTS",
                               (unsigned)h, side_name);
            if (zts_store_write(copy_name, data, size) != 0) {
                zts_log(ZTS_E10_STORE);
                return -1;
            }
            zts_str_copy(m->file, copy_name, ZTS_MAX_FILE);
            m->source = side_name;
        }
        char uhex[33];
        uuid_to_hex(m->uuid, uhex);
        n += (u32)__builtin_snprintf(text + n, sizeof(text) - n,
            "MOD|%s|%s|%u|%s|%c\n", m->name, uhex,
            (unsigned)(m->major * 10000u + m->minor * 100u + m->patch),
            m->file, side_name);
        if (n >= sizeof(text) - 128) break;
    }

    u32 crc = zts_crc32(text, n);
    n += (u32)__builtin_snprintf(text + n, sizeof(text) - n, "CRC=%08x\n", crc);

    if (zts_store_write(state_name, (const u8 *)text, n) != 0) {
        zts_log(ZTS_E10_STORE);
        return -1;
    }
    /* 读回校验 */
    u8 *back = 0;
    u32 back_size = 0;
    if (zts_store_read(state_name, &back, &back_size) != 0 || !back ||
        parse_state((const char *)back, back_size, side) <= 0) {
        zts_log(ZTS_E10_STORE);
        return -1;
    }
    g_state_side = side;
    g_state_seq++;
    zts_log("[zts] state committed:");
    zts_log(state_name);
    return 0;
}
