/**
 * Limine 文件协议驱动
 * 
 * 使用 Limine 引导加载器提供的文件协议来访问文件系统。
 * 这在 UEFI 环境下特别有用，因为传统的 ATA 端口 I/O 可能不可用。
 */

#include "limine.h"
#include "console.h"
#include "string.h"
#include "heap.h"
#include "pmm.h"  /* For phys_to_virt */

/* Limine 内核文件请求 */
__attribute__((section(".limine_reqs"), used))
static volatile struct limine_kernel_file_request kernel_file_request = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0,
    .response = NULL
};

/* Limine 模块请求 */
__attribute__((section(".limine_reqs"), used))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
    .response = NULL
};

/* 文件缓存 */
#define MAX_CACHED_FILES 16

struct cached_file {
    char path[256];
    void *data;
    size_t size;
    int used;
};

static struct cached_file file_cache[MAX_CACHED_FILES];
static int limine_file_initialized = 0;

/* 前向声明 */
void *limine_find_module(const char *path, size_t *size_out);

/* 初始化 Limine 文件系统 */
void limine_file_init(void) {
    /* 清空缓存 */
    for (int i = 0; i < MAX_CACHED_FILES; i++) {
        file_cache[i].used = 0;
        file_cache[i].data = NULL;
        file_cache[i].size = 0;
    }
    
    limine_file_initialized = 1;
}

/* 从 Limine 获取文件 (从模块) */
void *limine_load_file(const char *path, size_t *size_out) {
    return limine_find_module(path, size_out);
}

/* 获取内核文件信息 */
void *limine_get_kernel_file(size_t *size_out) {
    if (kernel_file_request.response == NULL) {
        return NULL;
    }
    
    struct limine_file *kernel_file = kernel_file_request.response->kernel_file;
    if (kernel_file == NULL) {
        return NULL;
    }
    
    if (size_out) {
        *size_out = kernel_file->size;
    }

    /* Limine 返回的 address 已经是虚拟地址（HHDM 映射后）*/
    return kernel_file->address;
}

/* 获取模块数量 */
int limine_get_module_count(void) {
    if (module_request.response == NULL) {
        return 0;
    }
    
    return module_request.response->module_count;
}

/* 获取模块 */
void *limine_get_module(int index, size_t *size_out, const char **path_out) {
    if (module_request.response == NULL || index < 0) {
        return NULL;
    }
    
    if (index >= (int)module_request.response->module_count) {
        return NULL;
    }
    
    struct limine_file *module = module_request.response->modules[index];
    if (module == NULL) {
        return NULL;
    }
    
    if (size_out) {
        *size_out = module->size;
    }
    
    if (path_out) {
        *path_out = module->path;
    }

    /* Limine 返回的 address 已经是虚拟地址（HHDM 映射后）*/
    /* 不需要再用 phys_to_virt 转换！直接返回即可 */
    return module->address;
}

/* 辅助函数：跳过 boot(): 前缀 */
static const char *skip_boot_prefix(const char *path) {
    if (path == NULL) return NULL;
    /* 跳过 "boot():" 或类似前缀 */
    const char *p = path;
    while (*p && *p != ':' && *p != '/' && *p != '\\') p++;
    if (*p == ':') {
        p++;
        /* 跳过可能的前导斜杠 */
        while (*p == '/' || *p == '\\') p++;
        return p;
    }
    return path;
}

/* 辅助函数：检查路径是否以指定前缀结尾 */
static int path_ends_with(const char *full_path, const char *suffix) {
    if (full_path == NULL || suffix == NULL) return 0;
    
    size_t full_len = strlen(full_path);
    size_t suffix_len = strlen(suffix);
    
    if (suffix_len > full_len) return 0;
    
    return strcmp(full_path + full_len - suffix_len, suffix) == 0;
}

/* 按路径查找模块 */
void *limine_find_module(const char *path, size_t *size_out) {
    if (module_request.response == NULL || path == NULL) {
        serial_print("[LIMINE] module_request.response is NULL or path is NULL\n");
        return NULL;
    }
    
    serial_print("[LIMINE] Searching for module: ");
    serial_print(path);
    serial_print("\n");
    
    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file *module = module_request.response->modules[i];
        if (module != NULL && module->path != NULL) {
            const char *mod_path = module->path;
            const char *filename = mod_path;
            
            /* 提取文件名 */
            for (const char *p = mod_path; *p; p++) {
                if (*p == '/' || *p == '\\') {
                    filename = p + 1;
                }
            }
            
            const char *mod_path_clean = skip_boot_prefix(mod_path);
            
            serial_print("  Checking: ");
            serial_print(mod_path);
            serial_print(" (file: ");
            serial_print(filename);
            serial_print(")\n");
            
            /* 多种匹配方式 */
            int match = (strcmp(mod_path, path) == 0) ||
                        (strcmp(filename, path) == 0) ||
                        (strcmp(mod_path_clean, path) == 0) ||
                        (path_ends_with(mod_path_clean, path)) ||
                        (path[0] == '/' && strcmp(mod_path_clean, path + 1) == 0) ||
                        (path[0] == '/' && mod_path_clean[0] == '/' && strcmp(path + 1, mod_path_clean + 1) == 0) ||
                        (path_ends_with(mod_path, path));
            
            /* 特殊处理: 如果路径包含 shell, init, desk 等关键字 */
            if (!match) {
                /* 检查是否包含关键字 */
                int has_shell = (strstr(mod_path, "shell") != NULL) && (strstr(path, "shell") != NULL);
                int has_init = (strstr(mod_path, "init") != NULL) && (strstr(path, "init") != NULL);
                int has_desk = (strstr(mod_path, "desk") != NULL) && (strstr(path, "desk") != NULL);
                if (has_shell || has_init || has_desk) {
                    match = 1;
                    serial_print("  -> Matched by keyword!\n");
                }
            }
            
            if (match) {
                serial_print("  -> FOUND!\n");
                if (size_out) {
                    *size_out = module->size;
                }
                return module->address;
            }
        }
    }
    
    serial_print("[LIMINE] Module not found\n");
    return NULL;
}
