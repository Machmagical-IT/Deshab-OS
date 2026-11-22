/* example_module.c — ZTS 示例算法模块 v1
 *
 * 演示: API 表使用 / 导出符号（zts_export_* 进入宿主符号表）/ 热替换版本。
 */
#include "zts_module.h"

static unsigned long long g_calls;

int zts_module_init(zts_api_table *api) {
    zts_api = api;
    g_calls = 0;
    zts_logi("[example] init v1.0.0");
    return 0;
}

int zts_module_exec(void *input, void *output, void *ctx) {
    (void)input;
    (void)ctx;
    g_calls++;
    zts_logi("[example] exec v1");
    if (output) *(unsigned long long *)output = 0x5A5A000100000000ULL | (g_calls & 0xFFFFFFFFULL);
    return 0;
}

void zts_module_cleanup(void) {
    zts_logi("[example] cleanup v1");
}

/* 可被 srv.zts.patch.install 以 sym:zts_export_example_value 定位 */
unsigned long long zts_export_example_value(void) {
    return 0xA1B2C3D4ULL;
}
