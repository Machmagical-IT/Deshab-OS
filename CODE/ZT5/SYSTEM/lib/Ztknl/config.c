#include "config.h"
#include "vfs.h"
#include "heap.h"
#include "console.h"
#include "string.h"

/* 跳过空白字符 */
static const char *skip_whitespace(const char *p) {
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

/* 跳过到行尾 */
static const char *skip_to_eol(const char *p) {
    while (*p && *p != '\n' && *p != '\r') {
        p++;
    }
    return p;
}

/* 解析一行 */
static int parse_line(const char *line, char *key, char *value) {
    const char *p = skip_whitespace(line);

    /* 跳过注释和空行 */
    if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') {
        return 0;
    }

    /* 解析 key */
    int i = 0;
    while (*p && *p != '=' && *p != ' ' && *p != '\t' && *p != '\n' && i < 63) {
        key[i++] = *p++;
    }
    key[i] = '\0';

    /* 关键修复：跳过 key 后的空白字符，再检查是否有 '=' */
    p = skip_whitespace(p);

    if (*p != '=') {
        /* 没有 '='，可能是只有值的行（程序路径） */
        i = 0;
        while (*p && *p != '\n' && *p != '\r' && i < 255) {
            value[i++] = *p++;
        }
        value[i] = '\0';
        /* 去除尾部空白 */
        while (i > 0 && (value[i-1] == ' ' || value[i-1] == '\t')) {
            value[--i] = '\0';
        }
        if (strlen(key) > 0 || strlen(value) > 0) {
            return 1;
        }
        return 0;
    }

    p++;  /* 跳过 '=' */

    /* 跳过 '=' 后的空白 */
    p = skip_whitespace(p);

    /* 解析 value */
    i = 0;
    while (*p && *p != '\n' && *p != '\r' && *p != '#' && i < 255) {
        value[i++] = *p++;
    }
    value[i] = '\0';

    /* 去除尾部空白 */
    while (i > 0 && (value[i-1] == ' ' || value[i-1] == '\t')) {
        value[--i] = '\0';
    }

    return 1;
}

struct config_file *config_load(const char *path) {
    size_t size;
    char *data = (char *)vfs_read_file(path, &size);
    if (data == NULL) {
        return NULL;
    }

    struct config_file *config = kmalloc(sizeof(struct config_file));
    if (config == NULL) {
        kfree(data);
        return NULL;
    }

    config->capacity = 32;
    config->entries = kmalloc(config->capacity * sizeof(struct config_entry));
    config->count = 0;

    if (config->entries == NULL) {
        kfree(config);
        kfree(data);
        return NULL;
    }

    /* 解析文件 */
    const char *p = data;
    int line_num = 0;
    while (*p && config->count < config->capacity) {
        char key[64] = {0};
        char value[256] = {0};

        const char *eol = skip_to_eol(p);
        size_t line_len = eol - p;

        /* 复制行到缓冲区 */
        char line[512];
        if (line_len >= sizeof(line)) {
            line_len = sizeof(line) - 1;
        }
        memcpy(line, p, line_len);
        line[line_len] = '\0';

        if (parse_line(line, key, value)) {
            strcpy(config->entries[config->count].key, key);
            strcpy(config->entries[config->count].value, value);
            config->entries[config->count].type = CONFIG_TYPE_STRING;
            config->count++;
        }

        p = eol;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
        line_num++;
        
        /* 防止无限循环 */
        if (line_num > 100) {
            break;
        }
    }

    kfree(data);

    return config;
}

void config_free(struct config_file *config) {
    if (config != NULL) {
        if (config->entries != NULL) {
            kfree(config->entries);
        }
        kfree(config);
    }
}

const char *config_get_string(struct config_file *config, const char *key, const char *default_value) {
    if (config == NULL) {
        return default_value;
    }

    for (int i = 0; i < config->count; i++) {
        if (strcmp(config->entries[i].key, key) == 0) {
            return config->entries[i].value;
        }
    }

    return default_value;
}

int config_get_int(struct config_file *config, const char *key, int default_value) {
    const char *value = config_get_string(config, key, NULL);
    if (value == NULL) {
        return default_value;
    }

    int result = 0;
    int negative = 0;
    const char *p = value;

    if (*p == '-') {
        negative = 1;
        p++;
    }

    while (*p >= '0' && *p <= '9') {
        result = result * 10 + (*p - '0');
        p++;
    }

    return negative ? -result : result;
}

int config_get_bool(struct config_file *config, const char *key, int default_value) {
    const char *value = config_get_string(config, key, NULL);
    if (value == NULL) {
        return default_value;
    }

    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
        strcmp(value, "yes") == 0 || strcmp(value, "on") == 0) {
        return 1;
    }

    return 0;
}

int config_get_programs(struct config_file *config, char programs[][256], int max_programs) {
    if (config == NULL || programs == NULL) {
        return 0;
    }

    int count = 0;

    /* 首先检查 programs 列表 */
    for (int i = 0; i < config->count && count < max_programs; i++) {
        if (strcmp(config->entries[i].key, "program") == 0 ||
            strcmp(config->entries[i].key, "exec") == 0 ||
            strcmp(config->entries[i].key, "run") == 0) {
            strcpy(programs[count], config->entries[i].value);
            count++;
        }
    }

    /* 如果没有明确的 program 项，检查没有 key 的行（纯路径） */
    if (count == 0) {
        for (int i = 0; i < config->count && count < max_programs; i++) {
            if (config->entries[i].key[0] == '\0' &&
                config->entries[i].value[0] != '\0') {
                strcpy(programs[count], config->entries[i].value);
                count++;
            }
        }
    }

    return count;
}
