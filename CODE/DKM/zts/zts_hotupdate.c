/* zts_hotupdate.c — 检查点 / 差分更新 / 事务 / 函数替换 / 版本回滚 / 统计
 *
 * 对应 ZT5 hotupdate.c 的全部功能面，并补全其 TODO：
 *   - checkpoint_create/restore 真实快照（槽位引用 + 注册表 + patch 表）
 *   - diff_apply 支持 modify/insert/delete
 *   - tx 支持内存/文件/模块三类 op
 *   - 更新包 .zup（全量/差分）解析与应用
 *   - 版本快照回滚真实恢复 patch 指令
 */
#include "zts.h"

/* ============================================================
 * 检查点
 * ============================================================ */
static zts_checkpoint g_ckpts[ZTS_MAX_CKPTS];
static u32 g_ckpt_count;
static u64 g_ckpt_next_id = 1;

/* 函数替换表 */
static zts_patch g_patches[ZTS_MAX_PATCHES];
static u32 g_patch_count;
static u32 g_patch_version;

/* 版本快照 */
typedef struct {
    u32 version;
    u64 timestamp_ms;
    char desc[64];
    u32 patch_count;
    zts_patch patches[ZTS_MAX_PATCHES];
    u8 valid;
} zts_version;
static zts_version g_versions[ZTS_MAX_VERSIONS];
static u32 g_version_count;
static u32 g_version_current;

static zts_stats g_stats;
static volatile int g_update_state = ZTS_UPDATE_IDLE;

void zts_hu_init(void) {
    zts_memset(g_ckpts, 0, sizeof(g_ckpts));
    g_ckpt_count = 0;
    g_ckpt_next_id = 1;
    zts_memset(g_patches, 0, sizeof(g_patches));
    g_patch_count = 0;
    g_patch_version = 0;
    zts_memset(g_versions, 0, sizeof(g_versions));
    g_version_count = 0;
    g_version_current = 0;
    zts_memset(&g_stats, 0, sizeof(g_stats));
    g_update_state = ZTS_UPDATE_IDLE;
}

/* 快照 patch 表到检查点（只记录 target/replacement；原始字节在影子表里，
 * 恢复时先全部 uninstall 再按快照重装） */
static void ckpt_capture_patches(zts_checkpoint *cp) {
    u32 n = g_patch_count < ZTS_MAX_PATCHES ? g_patch_count : ZTS_MAX_PATCHES;
    cp->patch_count = n;
    for (u32 i = 0; i < n; i++) {
        cp->patch_target[i] = g_patches[i].target_addr;
        cp->patch_replacement[i] = g_patches[i].replacement_addr;
        cp->patch_orig_len[i] = 0;
    }
}

u64 zts_ckpt_create(const char *desc) {
    if (g_ckpt_count >= ZTS_MAX_CKPTS) {
        /* 删除最旧检查点（会释放其槽位引用） */
        u64 old = g_ckpts[0].id;
        /* 前向声明由下方定义提供 */
        extern int zts_ckpt_delete(u64 id);
        zts_ckpt_delete(old);
    }
    zts_checkpoint *cp = &g_ckpts[g_ckpt_count];
    zts_memset(cp, 0, sizeof(*cp));
    cp->id = g_ckpt_next_id++;
    cp->timestamp_ms = zts_now_ms();
    zts_str_copy(cp->description, desc ? desc : "", 64);
    cp->version_before = g_version_current;

    int n = zts_module_count();
    if (n > ZTS_MAX_MODULES) n = ZTS_MAX_MODULES;   /* 不变量 */
    cp->module_count = (u32)n;
    for (int i = 0; i < n; i++) {
        zts_module *m = zts_module_at((u32)i);
        cp->modules[i] = *m;
        if (m->slot < ZTS_SLOT_COUNT) zts_slot_ref(m->slot);
    }

    ckpt_capture_patches(cp);
    g_ckpt_count++;
    zts_log("[zts] checkpoint created:");
    zts_log(cp->description);
    return cp->id;
}

static zts_checkpoint *ckpt_find(u64 id) {
    for (u32 i = 0; i < g_ckpt_count; i++) {
        if (g_ckpts[i].id == id) return &g_ckpts[i];
    }
    return 0;
}

int zts_ckpt_delete(u64 id) {
    for (u32 i = 0; i < g_ckpt_count; i++) {
        if (g_ckpts[i].id != id) continue;
        for (u32 j = 0; j < g_ckpts[i].module_count; j++) {
            zts_slot_unref(g_ckpts[i].modules[j].slot);
        }
        for (u32 k = i; k + 1 < g_ckpt_count; k++) g_ckpts[k] = g_ckpts[k + 1];
        g_ckpt_count--;
        zts_memset(&g_ckpts[g_ckpt_count], 0, sizeof(zts_checkpoint));
        return 0;
    }
    return -1;
}

/* 原始字节影子表（install 时保存，uninstall/rollback 恢复） */
static u8 g_patch_orig[ZTS_MAX_PATCHES][16];
static u8 g_patch_orig_valid[ZTS_MAX_PATCHES];

static void patch_write_jump(u64 target, u64 replacement) {
    i64 rel = (i64)replacement - (i64)(target + 5);
    if (rel < -2147483648LL || rel > 2147483647LL) {
        u8 code[14] = {0x48, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
        *(u64 *)&code[7] = replacement;
        zts_memcpy((void *)target, code, 14);
    } else {
        u8 code[5] = {0xE9, 0, 0, 0, 0};
        *(i32 *)&code[1] = (i32)rel;
        zts_memcpy((void *)target, code, 5);
    }
    __builtin___clear_cache((char *)target, (char *)target + 16);
}

int zts_patch_install(u64 target, u64 replacement, const char *target_name,
                      const char *replacement_name) {
    if (!target || !replacement) return -1;
    for (u32 i = 0; i < g_patch_count; i++) {
        if (g_patches[i].target_addr == target) {
            g_patches[i].replacement_addr = replacement;
            g_patches[i].version = ++g_patch_version;
            g_patches[i].active = 1;
            zts_str_copy(g_patches[i].replacement_name,
                         replacement_name ? replacement_name : "", ZTS_MAX_MOD_NAME);
            patch_write_jump(target, replacement);
            return 0;
        }
    }
    if (g_patch_count >= ZTS_MAX_PATCHES) return -1;
    zts_patch *p = &g_patches[g_patch_count];
    zts_memset(p, 0, sizeof(*p));
    p->target_addr = target;
    p->replacement_addr = replacement;
    p->version = ++g_patch_version;
    p->active = 1;
    zts_str_copy(p->target_name, target_name ? target_name : "", ZTS_MAX_MOD_NAME);
    zts_str_copy(p->replacement_name, replacement_name ? replacement_name : "", ZTS_MAX_MOD_NAME);
    zts_memcpy(g_patch_orig[g_patch_count], (const void *)target, 16);
    g_patch_orig_valid[g_patch_count] = 1;
    g_patch_count++;
    patch_write_jump(target, replacement);
    zts_log("[zts] function replaced:");
    zts_log(p->target_name);
    return 0;
}

int zts_patch_uninstall(u64 target) {
    for (u32 i = 0; i < g_patch_count; i++) {
        if (g_patches[i].target_addr != target) continue;
        if (g_patch_orig_valid[i]) {
            zts_memcpy((void *)target, g_patch_orig[i], 16);
            __builtin___clear_cache((char *)target, (char *)target + 16);
        }
        g_patches[i].active = 0;
        for (u32 k = i; k + 1 < g_patch_count; k++) {
            g_patches[k] = g_patches[k + 1];
            zts_memcpy(g_patch_orig[k], g_patch_orig[k + 1], 16);
            g_patch_orig_valid[k] = g_patch_orig_valid[k + 1];
        }
        g_patch_count--;
        zts_memset(&g_patches[g_patch_count], 0, sizeof(zts_patch));
        g_patch_orig_valid[g_patch_count] = 0;
        return 0;
    }
    return -1;
}

static void patches_restore_snapshot(const zts_checkpoint *cp) {
    /* 先卸载现有全部 patch */
    while (g_patch_count) {
        zts_patch_uninstall(g_patches[0].target_addr);
    }
    for (u32 i = 0; i < cp->patch_count; i++) {
        zts_patch_install(cp->patch_target[i], cp->patch_replacement[i], "ckpt", "ckpt");
    }
}

int zts_ckpt_restore(u64 id) {
    zts_checkpoint *cp = ckpt_find(id);
    if (!cp) {
        zts_log(ZTS_E08_CKPT);
        return -1;
    }
    zts_log("[zts] checkpoint restore:");

    /* 当前注册表里不在快照中的模块：cleanup 后释放引用 */
    int cur_n = zts_module_count();
    for (int i = 0; i < cur_n; i++) {
        zts_module *m = zts_module_at((u32)i);
        int in_snap = 0;
        for (u32 j = 0; j < cp->module_count; j++) {
            if (zts_str_eq(cp->modules[j].name, m->name)) { in_snap = 1; break; }
        }
        if (!in_snap) {
            if (m->initialized && m->cleanup) m->cleanup();
            if (m->slot < ZTS_SLOT_COUNT) zts_slot_unref(m->slot);
        }
    }
    zts_modules_restore(cp->modules, cp->module_count);

    patches_restore_snapshot(cp);
    g_version_current = cp->version_before;
    zts_log("[zts] checkpoint restore complete");
    return 0;
}

int zts_ckpt_latest_for_module(const char *name, u64 *out_id) {
    if (!name || !out_id) return -1;
    for (u32 i = g_ckpt_count; i > 0; i--) {
        zts_checkpoint *cp = &g_ckpts[i - 1];
        for (u32 j = 0; j < cp->module_count; j++) {
            if (zts_str_eq(cp->modules[j].name, name)) {
                *out_id = cp->id;
                return 0;
            }
        }
    }
    return -1;
}

int zts_ckpt_list(char *buf, u32 cap) {
    if (!buf || !cap) return -1;
    u32 n = 0;
    for (u32 i = 0; i < g_ckpt_count && n + 64 < cap; i++) {
        n += (u32)__builtin_snprintf(buf + n, cap - n, "%u|%s|%u\n",
                                     (unsigned)g_ckpts[i].id, g_ckpts[i].description,
                                     (unsigned)g_ckpts[i].module_count);
    }
    buf[n < cap ? n : cap - 1] = 0;
    return (int)g_ckpt_count;
}

/* ============================================================
 * 差分更新
 * ============================================================ */
static int diff_emit(zts_diff_header *hdr, u8 *out, u32 out_cap, u32 *pos,
                     u64 offset, u32 size, u32 type, const u8 *data) {
    u32 hdr_size = (u32)sizeof(zts_diff_block);
    u32 need = hdr_size + ((type == ZTS_DIFF_DELETE) ? 0u : size);
    if (*pos + need > out_cap) return -1;
    zts_diff_block *b = (zts_diff_block *)(out + *pos);
    b->offset = offset;
    b->size = size;
    b->type = type;
    if (type != ZTS_DIFF_DELETE && size) {
        zts_memcpy(out + *pos + hdr_size, data, size);
    }
    *pos += need;
    hdr->block_count++;
    return 0;
}

int zts_diff_create(const u8 *old_data, u32 old_size, const u8 *new_data, u32 new_size,
                    u8 *out, u32 out_cap, u32 *out_size) {
    if (!old_data || !new_data || !out || !out_size ||
        out_cap < sizeof(zts_diff_header)) return -1;
    zts_diff_header *hdr = (zts_diff_header *)out;
    zts_memset(hdr, 0, sizeof(*hdr));
    hdr->magic = ZTS_DIFF_MAGIC;
    hdr->version = 1;
    hdr->old_checksum = zts_fnv1a64(old_data, old_size);
    hdr->new_checksum = zts_fnv1a64(new_data, new_size);
    u32 pos = (u32)sizeof(zts_diff_header);

    u32 min = old_size < new_size ? old_size : new_size;
    u32 i = 0;
    while (i < min) {
        while (i < min && old_data[i] == new_data[i]) i++;
        if (i >= min) break;
        u32 start = i;
        while (i < min && old_data[i] != new_data[i]) i++;
        u32 total = i - start;
        u32 done = 0;
        while (done < total) {
            u32 chunk = total - done;
            if (chunk > ZTS_DIFF_BLOCK_SIZE) chunk = ZTS_DIFF_BLOCK_SIZE;
            if (diff_emit(hdr, out, out_cap, &pos, start + done, chunk,
                          ZTS_DIFF_MODIFY, new_data + start + done) != 0) return -1;
            done += chunk;
        }
    }
    if (new_size > old_size) {
        u32 total = new_size - old_size;
        u32 done = 0;
        while (done < total) {
            u32 chunk = total - done;
            if (chunk > ZTS_DIFF_BLOCK_SIZE) chunk = ZTS_DIFF_BLOCK_SIZE;
            if (diff_emit(hdr, out, out_cap, &pos, old_size + done, chunk,
                          ZTS_DIFF_INSERT, new_data + old_size + done) != 0) return -1;
            done += chunk;
        }
    } else if (old_size > new_size) {
        if (diff_emit(hdr, out, out_cap, &pos, new_size, old_size - new_size,
                      ZTS_DIFF_DELETE, 0) != 0) return -1;
    }
    *out_size = pos;
    return 0;
}

int zts_diff_apply(const u8 *diff, u32 diff_size, u8 *data, u32 *size, u32 cap) {
    if (!diff || diff_size < sizeof(zts_diff_header) || !data || !size) return -1;
    const zts_diff_header *hdr = (const zts_diff_header *)diff;
    if (hdr->magic != ZTS_DIFF_MAGIC) {
        zts_log(ZTS_E09_DIFF);
        return -1;
    }
    if (zts_fnv1a64(data, *size) != hdr->old_checksum) {
        zts_log(ZTS_E09_DIFF);
        zts_log("  old checksum mismatch");
        return -1;
    }
    u32 off = (u32)sizeof(zts_diff_header);
    u32 cur = *size;
    for (u32 i = 0; i < hdr->block_count; i++) {
        u32 hdr_size = (u32)sizeof(zts_diff_block);
        if (off + hdr_size > diff_size) {
            zts_log(ZTS_E09_DIFF);
            return -1;
        }
        const zts_diff_block *b = (const zts_diff_block *)(diff + off);
        const u8 *payload = diff + off + hdr_size;
        u32 data_len = (b->type == ZTS_DIFF_DELETE) ? 0u : b->size;
        if (off + hdr_size + data_len > diff_size) {
            zts_log(ZTS_E09_DIFF);
            return -1;
        }
        switch (b->type) {
        case ZTS_DIFF_MODIFY:
            if (b->offset + b->size > cur || b->offset + b->size > cap) return -1;
            zts_memcpy(data + b->offset, payload, b->size);
            break;
        case ZTS_DIFF_INSERT: {
            if (b->offset > cur || cur + b->size > cap) return -1;
            /* 在 offset 处插入：后移尾部 */
            for (u32 k = cur; k > b->offset; k--) data[k + b->size - 1] = data[k - 1];
            zts_memcpy(data + b->offset, payload, b->size);
            cur += b->size;
            break;
        }
        case ZTS_DIFF_DELETE:
            if (b->offset > cur) return -1;
            cur = b->offset;
            break;
        default:
            return -1;
        }
        off += hdr_size + data_len;
    }
    if (zts_fnv1a64(data, cur) != hdr->new_checksum) {
        zts_log(ZTS_E05_CHECKSUM);
        return -1;
    }
    *size = cur;
    return 0;
}

/* ============================================================
 * 事务
 * ============================================================ */
static zts_transaction g_txs[ZTS_TX_MAX];

u64 zts_tx_begin(void) {
    for (int i = 0; i < ZTS_TX_MAX; i++) {
        if (g_txs[i].state == 0) {
            g_txs[i].id = (u64)(i + 1);
            g_txs[i].state = 1;
            g_txs[i].ops = 0;
            g_txs[i].ops_tail = 0;
            g_txs[i].op_count = 0;
            g_txs[i].checkpoint_id = zts_ckpt_create("transaction");
            return g_txs[i].id;
        }
    }
    return 0;
}

static zts_transaction *tx_get(u64 id) {
    if (id == 0 || id > ZTS_TX_MAX) return 0;
    zts_transaction *tx = &g_txs[id - 1];
    return tx->state ? tx : 0;
}

int zts_tx_add_op(u64 id, int type, void *target, const u8 *old_value,
                  const u8 *new_value, u32 size, const char *file, const char *arg) {
    zts_transaction *tx = tx_get(id);
    if (!tx || tx->state != 1) return -1;
    zts_tx_op *op = (zts_tx_op *)zts_alloc(sizeof(*op));
    if (!op) return -1;
    zts_memset(op, 0, sizeof(*op));
    op->type = type;
    op->target = target;
    op->size = size;
    if (old_value && size) {
        op->old_value = (u8 *)zts_alloc(size);
        if (!op->old_value) { zts_free(op); return -1; }
        zts_memcpy(op->old_value, old_value, size);
    }
    op->new_value = new_value;
    zts_str_copy(op->file, file ? file : "", ZTS_MAX_FILE);
    zts_str_copy(op->arg, arg ? arg : "", ZTS_MAX_MOD_NAME);
    if (!tx->ops) tx->ops = op;
    else tx->ops_tail->next = op;
    tx->ops_tail = op;
    tx->op_count++;
    return 0;
}

static void tx_free_ops(zts_transaction *tx) {
    zts_tx_op *op = tx->ops;
    while (op) {
        zts_tx_op *next = op->next;
        if (op->old_value) zts_free(op->old_value);
        zts_free(op);
        op = next;
    }
    tx->ops = 0;
    tx->ops_tail = 0;
    tx->op_count = 0;
}

int zts_tx_commit(u64 id) {
    zts_transaction *tx = tx_get(id);
    if (!tx || tx->state != 1) return -1;
    tx->state = 3;
    for (zts_tx_op *op = tx->ops; op; op = op->next) {
        switch (op->type) {
        case ZTS_TX_OP_MEM:
            if (op->target && op->new_value && op->size)
                zts_memcpy(op->target, op->new_value, op->size);
            break;
        case ZTS_TX_OP_FILE:
            if (zts_store_write(op->file, op->new_value, op->size) != 0) {
                zts_log(ZTS_E10_STORE);
                tx->state = 5;
                return -1;
            }
            break;
        case ZTS_TX_OP_MODULE:
            if (zts_module_load_file(op->arg, 1) != 0) {
                zts_log("[zts] tx module load failed:");
                zts_log(op->arg);
                tx->state = 5;
                return -1;
            }
            break;
        }
    }
    tx->state = 4;
    tx_free_ops(tx);
    return 0;
}

int zts_tx_rollback(u64 id) {
    zts_transaction *tx = tx_get(id);
    if (!tx) return -1;
    tx->state = 5;
    /* 逆序恢复 */
    u32 n = tx->op_count;
    zts_tx_op **arr = (zts_tx_op **)zts_alloc(sizeof(zts_tx_op *) * (n ? n : 1));
    if (arr) {
        u32 i = 0;
        for (zts_tx_op *op = tx->ops; op && i < n; op = op->next) arr[i++] = op;
        while (i > 0) {
            zts_tx_op *op = arr[--i];
            if (op->type == ZTS_TX_OP_MEM && op->target && op->old_value && op->size) {
                zts_memcpy(op->target, op->old_value, op->size);
            } else if (op->type == ZTS_TX_OP_FILE && op->old_value) {
                zts_store_write(op->file, op->old_value, op->size);
            } else if (op->type == ZTS_TX_OP_MODULE && op->arg[0]) {
                zts_module_unload(op->arg);
            }
        }
        zts_free(arr);
    }
    tx_free_ops(tx);
    if (tx->checkpoint_id) zts_ckpt_restore(tx->checkpoint_id);
    tx->state = 6;
    return 0;
}

/* ============================================================
 * 版本快照
 * ============================================================ */
u32 zts_version_snapshot(const char *desc) {
    zts_version *v = 0;
    for (u32 i = 0; i < ZTS_MAX_VERSIONS; i++) {
        if (!g_versions[i].valid) { v = &g_versions[i]; break; }
    }
    if (!v) {
        /* 覆盖最旧版本 */
        v = &g_versions[0];
    }
    zts_memset(v, 0, sizeof(*v));
    v->version = ++g_version_current;
    v->timestamp_ms = zts_now_ms();
    zts_str_copy(v->desc, desc ? desc : "", 64);
    v->patch_count = g_patch_count < ZTS_MAX_PATCHES ? g_patch_count : ZTS_MAX_PATCHES;
    for (u32 i = 0; i < v->patch_count; i++) v->patches[i] = g_patches[i];
    v->valid = 1;
    if (g_version_count < ZTS_MAX_VERSIONS) g_version_count++;
    return v->version;
}

int zts_version_rollback(u32 version) {
    zts_version *v = 0;
    for (u32 i = 0; i < ZTS_MAX_VERSIONS; i++) {
        if (g_versions[i].valid && g_versions[i].version == version) { v = &g_versions[i]; break; }
    }
    if (!v) return -1;
    while (g_patch_count) zts_patch_uninstall(g_patches[0].target_addr);
    for (u32 i = 0; i < v->patch_count; i++) {
        zts_patch_install(v->patches[i].target_addr, v->patches[i].replacement_addr,
                          v->patches[i].target_name, v->patches[i].replacement_name);
    }
    g_version_current = version;
    return 0;
}

u32 zts_version_current(void) { return g_version_current; }

/* ============================================================
 * 更新包 .zup
 * ============================================================ */
#define ZTS_MAX_IMAGE (ZTS_SLOT_SIZE)

int zts_update_apply_package(const char *path) {
    g_update_state = ZTS_UPDATE_VERIFYING;
    u8 *pkg = 0;
    u32 pkg_size = 0;
    if (zts_store_read(path, &pkg, &pkg_size) != 0) {
        zts_log(ZTS_E09_DIFF);
        g_update_state = ZTS_UPDATE_FAILED;
        return -1;
    }
    zts_update_header hdr;
    if (pkg_size < sizeof(hdr)) {
        zts_log(ZTS_E09_DIFF);
        g_update_state = ZTS_UPDATE_FAILED;
        return -1;
    }
    zts_memcpy(&hdr, pkg, sizeof(hdr));
    if (hdr.magic != ZTS_UPD_MAGIC || hdr.payload_size != pkg_size - sizeof(hdr)) {
        zts_log(ZTS_E09_DIFF);
        g_update_state = ZTS_UPDATE_FAILED;
        return -1;
    }
    g_update_state = ZTS_UPDATE_PREPARING;
    static u8 image[ZTS_MAX_IMAGE];
    u32 image_size = 0;

    if (hdr.mode == 1) {   /* 差分 */
        zts_module *m = zts_module_find(hdr.name);
        if (!m) {
            zts_log("[zts] update target not loaded:");
            zts_log(hdr.name);
            g_update_state = ZTS_UPDATE_FAILED;
            return -1;
        }
        u8 *base = 0;
        u32 base_size = 0;
        if (zts_store_read(m->file, &base, &base_size) != 0 || base_size > ZTS_MAX_IMAGE) {
            zts_log(ZTS_E09_DIFF);
            g_update_state = ZTS_UPDATE_FAILED;
            return -1;
        }
        zts_memcpy(image, base, base_size);
        image_size = base_size;
        if (zts_diff_apply(pkg + sizeof(hdr), hdr.payload_size, image, &image_size,
                           ZTS_MAX_IMAGE) != 0) {
            g_update_state = ZTS_UPDATE_FAILED;
            return -1;
        }
        if (zts_crc32(image, image_size) != hdr.new_crc) {
            zts_log(ZTS_E05_CHECKSUM);
            g_update_state = ZTS_UPDATE_FAILED;
            return -1;
        }
    } else {                /* 全量 */
        image_size = hdr.payload_size;
        if (image_size > ZTS_MAX_IMAGE) {
            g_update_state = ZTS_UPDATE_FAILED;
            return -1;
        }
        zts_memcpy(image, pkg + sizeof(hdr), image_size);
        if (hdr.new_crc && zts_crc32(image, image_size) != hdr.new_crc) {
            zts_log(ZTS_E05_CHECKSUM);
            g_update_state = ZTS_UPDATE_FAILED;
            return -1;
        }
    }

    /* 写入临时模块副本，再走热替换（保证 A/B 持久化与回滚） */
    if (zts_store_write("ZTSNEW.ZTS", image, image_size) != 0) {
        zts_log(ZTS_E10_STORE);
        g_update_state = ZTS_UPDATE_FAILED;
        return -1;
    }
    g_update_state = ZTS_UPDATE_APPLYING;
    if (zts_module_hotupdate(hdr.name, "ZTSNEW.ZTS") != 0) {
        g_stats.failed_updates++;
        g_update_state = ZTS_UPDATE_FAILED;
        return -1;
    }
    g_stats.total_updates++;
    g_stats.successful_updates++;
    g_stats.total_bytes_updated += image_size;
    g_update_state = ZTS_UPDATE_COMPLETED;
    return 0;
}

/* ============================================================
 * 统计/状态文本
 * ============================================================ */
void zts_stats_get(zts_stats *out) {
    if (out) *out = g_stats;
}

int zts_update_get_state(void) { return g_update_state; }

int zts_stats_text(char *buf, u32 cap) {
    if (!buf || !cap) return -1;
    u32 n = (u32)__builtin_snprintf(buf, cap,
        "updates=%u ok=%u fail=%u rollback=%u bytes=%u state=%u patches=%u versions=%u slots_used\n",
        (unsigned)g_stats.total_updates, (unsigned)g_stats.successful_updates,
        (unsigned)g_stats.failed_updates, (unsigned)g_stats.rollbacks,
        (unsigned)g_stats.total_bytes_updated, (unsigned)g_update_state,
        (unsigned)g_patch_count, (unsigned)g_version_current);
    return (int)n;
}

int zts_state_text(char *buf, u32 cap) {
    if (!buf || !cap) return -1;
    u32 n = (u32)__builtin_snprintf(buf, cap,
        "modules=%u ckpts=%u patches=%u version=%u update_state=%u\n",
        (unsigned)zts_module_count(), (unsigned)g_ckpt_count,
        (unsigned)g_patch_count, (unsigned)g_version_current,
        (unsigned)g_update_state);
    return (int)n;
}
