/*
 * example_algorithm.zts.c - 示例 .zts 算法模块
 * 
 * 编译方法:
 *   gcc -c -fPIC -o example_algorithm.o example_algorithm.zts.c
 *   ld -shared -o example_algorithm.zts example_algorithm.o
 * 
 * 这个文件展示如何编写一个 .zts 格式的算法模块
 */

#include <stdint.h>
#include <stddef.h>
#include "zts_algorithm.h"

/* ========== 算法元数据 ========== */

/* 声明算法元数据 */
ZTS_DECLARE(
    "example_algo",          /* 名称 */
    "示例算法模块",           /* 描述 */
    "ZratroOS Team",         /* 作者 */
    "GPL",                   /* 许可证 */
    ZTS_CAT_CUSTOM,          /* 分类 */
    1,                       /* 主版本 */
    0                        /* 次版本 */
);

/* ========== 算法私有数据 ========== */

struct example_data {
    int iteration_count;
    uint64_t total_processed;
};

/* ========== 算法实现 ========== */

ZTS_MODULE_INIT(example_init) {
    zts_info("Initializing example algorithm v1.0\n");
    
    /* 分配算法数据 */
    struct example_data *data = zts_alloc(sizeof(*data));
    if (!data) return -1;
    
    data->iteration_count = 0;
    data->total_processed = 0;
    
    alg->data = data;
    alg->data_size = sizeof(*data);
    
    zts_info("Example algorithm initialized\n");
    return 0;
}

ZTS_MODULE_CLEANUP(example_cleanup) {
    struct example_data *data = (struct example_data *)alg->data;
    
    zts_info("Cleanup: processed %lu iterations\n", data->iteration_count);
    
    /* 释放数据 */
    zts_free(data);
    alg->data = NULL;
    
    zts_info("Example algorithm cleaned up\n");
}

ZTS_MODULE_EXECUTE(example_execute) {
    struct example_data *data = (struct example_data *)alg->data;
    if (!data) return -1;
    
    /* 示例算法逻辑 */
    if (input && output) {
        /* 复制输入到输出 (示例) */
        __builtin_memcpy(output, input, 256);
        
        data->iteration_count++;
        data->total_processed++;
    }
    
    zts_info("Executed iteration %d\n", data->iteration_count);
    return 0;
}
