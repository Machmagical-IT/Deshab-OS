/*
 * config.c - 配置文件读取和解析
 * 
 * 读取 zratro.conf 配置文件
 * 支持格式：key = value
 * 支持注释：# 开头的行
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_PATH    256
#define MAX_CONFIG_ENTRIES 64
#define MAX_KEY_LEN 64
#define MAX_VALUE_LEN 256

typedef struct {
    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];
} config_entry_t;

typedef struct {
    config_entry_t entries[MAX_CONFIG_ENTRIES];
    int count;
    char config_path[MAX_PATH];
} config_t;

static config_t g_config;

/* ============================================
 * 初始化配置
 * ============================================ */

void config_init(void)
{
    g_config.count = 0;
    
    /* 默认配置路径 */
    strcpy(g_config.config_path, "/system/zratro.conf");
}

/* ============================================
 * 去除字符串两端空格
 * ============================================ */

static char* trim(char *str)
{
    char *end;
    
    /* 去除前导空格 */
    while(*str == ' ' || *str == '\t') str++;
    
    if(*str == 0) return str;
    
    /* 去除尾部空格 */
    end = str + strlen(str) - 1;
    while(end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) 
        end--;
    
    end[1] = '\0';
    
    return str;
}

/* ============================================
 * 读取配置文件
 * 
 * 返回：0 成功，-1 失败
 * ============================================ */

int config_read(const char *path)
{
    if (!path) {
        path = g_config.config_path;
    } else {
        strncpy(g_config.config_path, path, MAX_PATH - 1);
    }
    
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "config: Cannot open %s\n", path);
        return -1;
    }
    
    char line[512];
    g_config.count = 0;
    
    while (fgets(line, sizeof(line), fp) && g_config.count < MAX_CONFIG_ENTRIES) {
        char *trimmed = trim(line);
        
        /* 跳过空行和注释 */
        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            continue;
        }
        
        /* 查找 = 分隔符 */
        char *eq = strchr(trimmed, '=');
        if (!eq) {
            continue;
        }
        
        /* 分离 key 和 value */
        *eq = '\0';
        char *key = trim(trimmed);
        char *value = trim(eq + 1);
        
        /* 存储配置项 */
        strncpy(g_config.entries[g_config.count].key, key, MAX_KEY_LEN - 1);
        strncpy(g_config.entries[g_config.count].value, value, MAX_VALUE_LEN - 1);
        g_config.count++;
    }
    
    fclose(fp);
    return 0;
}

/* ============================================
 * 获取配置值
 * 
 * 返回：值字符串，未找到返回 NULL
 * ============================================ */

const char* config_get(const char *key)
{
    for (int i = 0; i < g_config.count; i++) {
        if (strcmp(g_config.entries[i].key, key) == 0) {
            return g_config.entries[i].value;
        }
    }
    return NULL;
}

/* ============================================
 * 获取整数配置值
 * ============================================ */

int config_get_int(const char *key, int default_val)
{
    const char *val = config_get(key);
    if (!val) return default_val;
    
    return atoi(val);
}

/* ============================================
 * 获取布尔配置值
 * ============================================ */

int config_get_bool(const char *key, int default_val)
{
    const char *val = config_get(key);
    if (!val) return default_val;
    
    if (strcmp(val, "true") == 0 || strcmp(val, "yes") == 0 || strcmp(val, "1") == 0) {
        return 1;
    }
    if (strcmp(val, "false") == 0 || strcmp(val, "no") == 0 || strcmp(val, "0") == 0) {
        return 0;
    }
    
    return default_val;
}

/* ============================================
 * 获取程序列表
 * 
 * 返回以 NULL 结尾的字符串数组
 * 注意：返回的数组指向内部缓冲区
 * ============================================ */

#define MAX_PROGRAMS 32
static char* g_programs[MAX_PROGRAMS];
static char g_program_buf[MAX_PROGRAMS][MAX_PATH];

char** config_get_programs(int *count)
{
    *count = 0;
    
    for (int i = 0; i < g_config.count && *count < MAX_PROGRAMS; i++) {
        if (strcmp(g_config.entries[i].key, "program") == 0) {
            strncpy(g_program_buf[*count], g_config.entries[i].value, MAX_PATH - 1);
            g_programs[*count] = g_program_buf[*count];
            (*count)++;
        }
    }
    
    g_programs[*count] = NULL;  /* NULL 终止 */
    return g_programs;
}

/* ============================================
 * 获取路径列表（hook.cfg 中的 path 配置）
 * 
 * 返回以 NULL 结尾的字符串数组
 * ============================================ */

static char* g_paths[MAX_PROGRAMS];
static char g_path_buf[MAX_PROGRAMS][MAX_PATH];

char** config_get_paths(int *count)
{
    *count = 0;
    
    for (int i = 0; i < g_config.count && *count < MAX_PROGRAMS; i++) {
        if (strcmp(g_config.entries[i].key, "path") == 0) {
            strncpy(g_path_buf[*count], g_config.entries[i].value, MAX_PATH - 1);
            g_paths[*count] = g_path_buf[*count];
            (*count)++;
        }
    }
    
    g_paths[*count] = NULL;  /* NULL 终止 */
    return g_paths;
}

/* ============================================
 * 获取禁止路径列表（shn.cfg 中的 npath 配置）
 * 
 * 返回以 NULL 结尾的字符串数组
 * ============================================ */

static char* g_npaths[MAX_PROGRAMS];
static char g_npath_buf[MAX_PROGRAMS][MAX_PATH];

char** config_get_npaths(int *count)
{
    *count = 0;
    
    for (int i = 0; i < g_config.count && *count < MAX_PROGRAMS; i++) {
        if (strcmp(g_config.entries[i].key, "npath") == 0) {
            strncpy(g_npath_buf[*count], g_config.entries[i].value, MAX_PATH - 1);
            g_npaths[*count] = g_npath_buf[*count];
            (*count)++;
        }
    }
    
    g_npaths[*count] = NULL;  /* NULL 终止 */
    return g_npaths;
}

/* ============================================
 * 检查路径是否被禁止
 * 
 * 返回：1 禁止，0 允许
 * ============================================ */

int config_is_path_forbidden(const char *path)
{
    int count;
    char **npaths = config_get_npaths(&count);
    
    for (int i = 0; i < count; i++) {
        if (strcmp(path, npaths[i]) == 0) {
            return 1;
        }
    }
    
    return 0;
}

/* ============================================
 * 显示所有配置（调试用）
 * ============================================ */

void config_dump(void)
{
    printf("\n=== Configuration (%s) ===\n", g_config.config_path);
    
    for (int i = 0; i < g_config.count; i++) {
        printf("  %s = %s\n", g_config.entries[i].key, g_config.entries[i].value);
    }
    
    printf("\n=== Programs ===\n");
    int prog_count;
    char **progs = config_get_programs(&prog_count);
    
    for (int i = 0; i < prog_count; i++) {
        printf("  [%d] %s\n", i + 1, progs[i]);
    }
    
    printf("\n");
}
