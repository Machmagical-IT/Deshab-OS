#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* 配置项类型 */
#define CONFIG_TYPE_STRING  0
#define CONFIG_TYPE_INT     1
#define CONFIG_TYPE_BOOL    2

/* 配置项 */
struct config_entry {
    char key[64];
    char value[256];
    int type;
};

/* 配置文件 */
struct config_file {
    struct config_entry *entries;
    int count;
    int capacity;
};

/* 加载配置文件 */
struct config_file *config_load(const char *path);

/* 释放配置文件 */
void config_free(struct config_file *config);

/* 获取字符串值 */
const char *config_get_string(struct config_file *config, const char *key, const char *default_value);

/* 获取整数值 */
int config_get_int(struct config_file *config, const char *key, int default_value);

/* 获取布尔值 */
int config_get_bool(struct config_file *config, const char *key, int default_value);

/* 获取程序列表 */
int config_get_programs(struct config_file *config, char programs[][256], int max_programs);

#endif
