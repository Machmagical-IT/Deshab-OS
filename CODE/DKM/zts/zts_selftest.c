/* zts_selftest.c — ZTS 子系统启动自检（FUCK [zts] selftest=1）
 *
 * 覆盖: 模块执行 / 检查点 / 热替换 / 回滚 / 函数替换 / 差分 / 事务 / 状态持久化。
 * 失败只计数并打印 [zts-test] FAIL，不 panic；结果在串口与 [zts] selftest done 行。
 */
#include "zts.h"

static int g_fail;

static void st(const char *name, int ok) {
    zts_log(ok ? "[zts-test] PASS " : "[zts-test] FAIL ");
    zts_log(name);
    if (!ok) g_fail++;
}

static u64 exec_example(void) {
    u64 v = 0;
    zts_module_execute("example", 0, &v);
    return v;
}

static unsigned long long selftest_replacement(void) {
    return 0xDEADBEEFCAFEBABEULL;
}

static int diff_selftest(void) {
    u8 *a = 0, *b = 0;
    u32 as = 0, bs = 0;
    if (zts_store_read("0x00000001.zts", &a, &as) != 0) return 0;
    /* b 必须在 a 之后读，避免同一旋转缓冲被覆盖 */
    static u8 copy_a[4096];
    if (as > sizeof(copy_a)) return 0;
    zts_memcpy(copy_a, a, as);
    if (zts_store_read("0x00000002.zts", &b, &bs) != 0) return 0;
    static u8 copy_b[4096];
    if (bs > sizeof(copy_b)) return 0;
    zts_memcpy(copy_b, b, bs);

    static u8 diff[64 * 1024];
    static u8 work[4096];
    u32 dsize = 0;
    if (zts_diff_create(copy_a, as, copy_b, bs, diff, sizeof(diff), &dsize) != 0) return 0;
    zts_memcpy(work, copy_a, as);
    u32 wsize = as;
    if (zts_diff_apply(diff, dsize, work, &wsize, sizeof(work)) != 0) return 0;
    if (wsize != bs) return 0;
    for (u32 i = 0; i < bs; i++) if (work[i] != copy_b[i]) return 0;
    return 1;
}

static int tx_selftest(void) {
    u32 var = 1;
    u32 old1 = 1, new1 = 2;
    u64 tx = zts_tx_begin();
    if (!tx) return 0;
    if (zts_tx_add_op(tx, ZTS_TX_OP_MEM, &var, (const u8 *)&old1,
                      (const u8 *)&new1, sizeof(var), 0, 0) != 0) return 0;
    if (zts_tx_commit(tx) != 0 || var != 2) return 0;

    u32 old2 = 2, new2 = 3;
    tx = zts_tx_begin();
    if (!tx) return 0;
    if (zts_tx_add_op(tx, ZTS_TX_OP_MEM, &var, (const u8 *)&old2,
                      (const u8 *)&new2, sizeof(var), 0, 0) != 0) return 0;
    if (zts_tx_rollback(tx) != 0) return 0;
    return var == 2;
}

void zts_selftest(void) {
    g_fail = 0;
    zts_log("[zts-test] begin");

    /* 1. 模块自动加载 + 执行 */
    u64 v = exec_example();
    st("module execute v1", (v >> 32) == 0x5A5A0001u);

    /* 2. 检查点 + 热替换 v2 */
    u64 ckpt = zts_ckpt_create("selftest");
    int hu = zts_module_hotupdate("example", "0x00000002.zts");
    st("hotupdate v2", hu == 0);
    v = exec_example();
    st("execute v2 marker", (v >> 32) == 0x5A5A0002u);

    /* 3. 检查点回滚 v1 */
    int rb = zts_ckpt_restore(ckpt);
    st("checkpoint restore v1", rb == 0);
    v = exec_example();
    st("execute v1 marker", (v >> 32) == 0x5A5A0001u);

    /* 4. 函数替换：patch zts_export_example_value */
    u64 target = zts_symbol_lookup("zts_export_example_value");
    if (target) {
        unsigned long long (*fn)(void) = (unsigned long long (*)(void))target;
        u64 before = fn();
        int pi = zts_patch_install(target, (u64)selftest_replacement,
                                   "zts_export_example_value", "selftest_replacement");
        u64 during = fn();
        int pu = zts_patch_uninstall(target);
        u64 after = fn();
        st("patch install", pi == 0 && during == 0xDEADBEEFCAFEBABEULL);
        st("patch uninstall", pu == 0 && after == before);
    } else {
        st("patch target symbol", 0);
    }

    /* 5. 差分 create/apply */
    st("diff create/apply", diff_selftest());

    /* 6. 事务 commit/rollback */
    st("transaction commit/rollback", tx_selftest());

    /* 7. A/B 状态持久化 */
    int ws = zts_store_write_state();
    int ls = zts_store_load_state();
    st("state A/B write+reload", ws == 0 && ls == 0);

    zts_log_dec("[zts-test] failures=", (u64)g_fail);
    zts_log("[zts-test] done");
}
