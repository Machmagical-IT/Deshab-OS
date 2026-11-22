/* example_module_v2.c — ZTS 示例算法模块 v2（同名/同 UUID，热替换演示）
 *
 * `zts update example 0x00000002.zts` 后执行结果从 0x...0001 变为 0x...0002。
 */
#include "zts_module.h"

static unsigned long long g_calls;

int zts_module_init(zts_api_table *api) {
    zts_api = api;
    g_calls = 0;
    zts_logi("[example] init v2.0.0");
    return 0;
}

int zts_module_exec(void *input, void *output, void *ctx) {
    (void)input;
    (void)ctx;
    g_calls++;
    zts_logi("[example] exec v2");
    if (output) *(unsigned long long *)output = 0x5A5A000200000000ULL | (g_calls & 0xFFFFFFFFULL);
    return 0;
}

void zts_module_cleanup(void) {
    zts_logi("[example] cleanup v2");
}

unsigned long long zts_export_example_value(void) {
    return 0x00C0FFEEULL;
}
