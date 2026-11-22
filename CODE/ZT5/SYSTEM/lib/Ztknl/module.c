/*
 * Zratro OS - Kernel Module System Implementation
 * 
 * 可加载内核模块支持
 */

#include "module.h"
#include "elf.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "console.h"
#include "string.h"
#include "smp.h"
#include "rcu.h"

/* ========== 全局变量 ========== */

/* 模块链表 */
static struct list_head module_list;
static spinlock_t module_lock = SPINLOCK_INIT;

/* 内核符号表 */
static struct list_head kernel_symbols;

/* 模块计数 */
static uint64_t module_count = 0;

/* ========== 内置符号导出 ========== */

/* 导出内核符号 */
struct kernel_symbol *export_kernel_symbol(const char *name, void *addr) {
    struct kernel_symbol *sym = kmalloc(sizeof(*sym));
    if (sym == NULL) return NULL;
    
    sym->name = name;
    sym->value = (uint64_t)addr;
    sym->module = NULL;  /* 内核符号 */
    list_init(&sym->list);
    
    list_add(&sym->list, &kernel_symbols);
    
    return sym;
}

/* 初始化内核符号表 */
static void init_kernel_symbols(void) {
    list_init(&kernel_symbols);
    
    /* 导出常用内核符号 */
    export_kernel_symbol("kmalloc", kmalloc);
    export_kernel_symbol("kfree", kfree);
    export_kernel_symbol("print", print);
    export_kernel_symbol("println", println);
    export_kernel_symbol("memcpy", memcpy);
    export_kernel_symbol("memset", memset);
    export_kernel_symbol("strlen", strlen);
    export_kernel_symbol("strcmp", strcmp);
    export_kernel_symbol("strcpy", strcpy);
}

/* ========== 初始化 ========== */

int module_init_system(void) {
    println("[MODULE] Initializing module system...");
    
    list_init(&module_list);
    init_kernel_symbols();
    
    println("[MODULE] Module system initialized");
    return 0;
}

/* ========== ELF 解析 ========== */

/* 解析 ELF 模块 */
static int parse_elf_module(struct module *mod, const void *data, size_t size) {
    struct elf64_ehdr *ehdr = (struct elf64_ehdr *)data;
    
    /* 验证 ELF 魔数 */
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        println("[MODULE] Invalid ELF magic");
        return -1;
    }
    
    /* 检查架构 */
    if (ehdr->e_machine != 0x3E) {  /* x86-64 */
        println("[MODULE] Invalid architecture");
        return -1;
    }
    
    /* 计算总大小 */
    size_t total_size = 0;
    struct elf64_phdr *phdr = (struct elf64_phdr *)((uint8_t *)data + ehdr->e_phoff);
    
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            total_size += phdr[i].p_memsz;
        }
    }
    
    /* 分配内存 */
    void *base = kmalloc(total_size);
    if (base == NULL) {
        return -1;
    }
    
    mod->mem[MOD_TEXT].base = base;
    mod->mem[MOD_TEXT].size = total_size;
    mod->core_size = total_size;
    
    /* 加载段 */
    uint64_t offset = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            memcpy((uint8_t *)base + offset,
                   (uint8_t *)data + phdr[i].p_offset,
                   phdr[i].p_filesz);
            
            /* 清零 BSS */
            if (phdr[i].p_memsz > phdr[i].p_filesz) {
                memset((uint8_t *)base + offset + phdr[i].p_filesz, 0,
                       phdr[i].p_memsz - phdr[i].p_filesz);
            }
            
            offset += phdr[i].p_memsz;
        }
    }
    
    /* 解析符号表 */
    struct elf64_shdr *shdr = (struct elf64_shdr *)((uint8_t *)data + ehdr->e_shoff);
    struct elf64_shdr *symtab = NULL;
    struct elf64_shdr *strtab = NULL;
    struct elf64_shdr *modinfo = NULL;
    
    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdr[i].sh_type == SHT_SYMTAB) {
            symtab = &shdr[i];
        } else if (shdr[i].sh_type == SHT_STRTAB) {
            strtab = &shdr[i];
        } else if (strcmp((char *)data + shdr[ehdr->e_shstrndx].sh_offset + shdr[i].sh_name,
                          ".modinfo") == 0) {
            modinfo = &shdr[i];
        }
    }
    
    /* 解析 modinfo */
    if (modinfo != NULL) {
        char *info = (char *)data + modinfo->sh_offset;
        char *end = info + modinfo->sh_size;
        
        while (info < end) {
            char *eq = strchr(info, '=');
            if (eq == NULL) break;
            
            *eq = '\0';
            char *key = info;
            char *value = eq + 1;
            
            if (strcmp(key, "license") == 0) {
                strncpy(mod->license, value, sizeof(mod->license) - 1);
            } else if (strcmp(key, "author") == 0) {
                strncpy(mod->author, value, sizeof(mod->author) - 1);
            } else if (strcmp(key, "description") == 0) {
                strncpy(mod->description, value, sizeof(mod->description) - 1);
            } else if (strcmp(key, "version") == 0) {
                strncpy(mod->version, value, sizeof(mod->version) - 1);
            } else if (strcmp(key, "init") == 0) {
                /* 查找 init 函数符号 */
                if (symtab != NULL && strtab != NULL) {
                    struct elf64_sym *syms = (struct elf64_sym *)((uint8_t *)data + symtab->sh_offset);
                    int nsyms = symtab->sh_size / sizeof(struct elf64_sym);
                    
                    for (int j = 0; j < nsyms; j++) {
                        char *sym_name = (char *)data + strtab->sh_offset + syms[j].st_name;
                        if (strcmp(sym_name, value) == 0) {
                            mod->init = (int (*)(void))((uint8_t *)base + syms[j].st_value);
                            break;
                        }
                    }
                }
            } else if (strcmp(key, "exit") == 0) {
                /* 查找 exit 函数符号 */
                if (symtab != NULL && strtab != NULL) {
                    struct elf64_sym *syms = (struct elf64_sym *)((uint8_t *)data + symtab->sh_offset);
                    int nsyms = symtab->sh_size / sizeof(struct elf64_sym);
                    
                    for (int j = 0; j < nsyms; j++) {
                        char *sym_name = (char *)data + strtab->sh_offset + syms[j].st_name;
                        if (strcmp(sym_name, value) == 0) {
                            mod->exit = (void (*)(void))((uint8_t *)base + syms[j].st_value);
                            break;
                        }
                    }
                }
            }
            
            info = value + strlen(value) + 1;
        }
    }
    
    /* 重定位 */
    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdr[i].sh_type == SHT_RELA) {
            struct elf64_rela *rela = (struct elf64_rela *)((uint8_t *)data + shdr[i].sh_offset);
            int nrela = shdr[i].sh_size / sizeof(struct elf64_rela);
            
            for (int j = 0; j < nrela; j++) {
                uint64_t *target = (uint64_t *)((uint8_t *)base + rela[j].r_offset);
                uint32_t sym_idx = rela[j].r_info >> 32;
                uint32_t type = rela[j].r_info & 0xFFFFFFFF;
                
                if (symtab != NULL && strtab != NULL) {
                    struct elf64_sym *syms = (struct elf64_sym *)((uint8_t *)data + symtab->sh_offset);
                    char *sym_name = (char *)data + strtab->sh_offset + syms[sym_idx].st_name;
                    
                    /* 查找符号 */
                    struct kernel_symbol *ksym = module_find_symbol(sym_name);
                    if (ksym != NULL) {
                        switch (type) {
                            case R_X86_64_64:
                                *target = ksym->value + rela[j].r_addend;
                                break;
                            case R_X86_64_PC32:
                                *(int32_t *)target = (int32_t)(ksym->value + rela[j].r_addend -
                                     (uint64_t)target);
                                break;
                            case R_X86_64_32:
                                *(uint32_t *)target = (uint32_t)(ksym->value + rela[j].r_addend);
                                break;
                            case R_X86_64_32S:
                                *(int32_t *)target = (int32_t)(ksym->value + rela[j].r_addend);
                                break;
                        }
                    }
                }
            }
        }
    }
    
    return 0;
}

/* ========== 模块加载/卸载 ========== */

struct module *module_load(const char *name, const void *data, size_t size) {
    if (name == NULL || data == NULL || size == 0) return NULL;
    
    /* 分配模块结构 */
    struct module *mod = kmalloc(sizeof(*mod));
    if (mod == NULL) return NULL;
    
    memset(mod, 0, sizeof(*mod));
    strncpy(mod->name, name, sizeof(mod->name) - 1);
    mod->state = MODULE_STATE_LOADING;
    mod->flags = 0;
    mod->ref_count = 1;
    mod->lock = SPINLOCK_INIT;
    list_init(&mod->dependencies);
    list_init(&mod->dependents);
    list_init(&mod->params);
    list_init(&mod->attrs);
    
    /* 解析 ELF */
    if (parse_elf_module(mod, data, size) != 0) {
        kfree(mod);
        return NULL;
    }
    
    spin_lock(&module_lock);
    
    /* 检查是否已加载 */
    struct module *existing = module_find(name);
    if (existing != NULL) {
        spin_unlock(&module_lock);
        kfree(mod->mem[MOD_TEXT].base);
        kfree(mod);
        return NULL;
    }
    
    /* 添加到模块链表 */
    list_add(&mod->list, &module_list);
    module_count++;
    
    spin_unlock(&module_lock);
    
    /* 调用初始化函数 */
    if (mod->init != NULL) {
        int ret = mod->init();
        if (ret != 0) {
            println("[MODULE] Module init failed: ");
            println(mod->name);
            
            spin_lock(&module_lock);
            list_del(&mod->list);
            module_count--;
            spin_unlock(&module_lock);
            
            kfree(mod->mem[MOD_TEXT].base);
            kfree(mod);
            return NULL;
        }
    }
    
    mod->state = MODULE_STATE_LIVE;
    mod->flags |= MODULE_FLAG_LIVE;
    
    print("[MODULE] Loaded: ");
    println(mod->name);
    
    return mod;
}

int module_unload(struct module *mod) {
    if (mod == NULL) return -1;
    
    spin_lock(&mod->lock);
    
    /* 检查引用计数 */
    if (mod->ref_count > 1) {
        spin_unlock(&mod->lock);
        return -1;  /* 还有其他引用 */
    }
    
    /* 检查依赖 */
    if (!list_empty(&mod->dependents)) {
        spin_unlock(&mod->lock);
        return -1;  /* 有其他模块依赖此模块 */
    }
    
    mod->state = MODULE_STATE_GOING;
    
    spin_unlock(&mod->lock);
    
    /* 调用清理函数 */
    if (mod->exit != NULL) {
        mod->exit();
    }
    
    spin_lock(&module_lock);
    list_del(&mod->list);
    module_count--;
    spin_unlock(&module_lock);
    
    /* 释放内存 */
    if (mod->mem[MOD_TEXT].base != NULL) {
        kfree(mod->mem[MOD_TEXT].base);
    }
    
    print("[MODULE] Unloaded: ");
    println(mod->name);
    
    kfree(mod);
    
    return 0;
}

int module_unload_name(const char *name) {
    struct module *mod = module_find(name);
    if (mod == NULL) return -1;
    return module_unload(mod);
}

/* ========== 查找 ========== */

struct module *module_find(const char *name) {
    struct module *mod;
    
    list_for_each_entry(mod, &module_list, list) {
        if (strcmp(mod->name, name) == 0) {
            return mod;
        }
    }
    
    return NULL;
}

struct module *module_find_by_addr(uint64_t addr) {
    struct module *mod;
    
    list_for_each_entry(mod, &module_list, list) {
        uint64_t base = (uint64_t)mod->mem[MOD_TEXT].base;
        uint64_t end = base + mod->mem[MOD_TEXT].size;
        
        if (addr >= base && addr < end) {
            return mod;
        }
    }
    
    return NULL;
}

/* ========== 符号操作 ========== */

struct kernel_symbol *module_find_symbol(const char *name) {
    /* 先查找内核符号 */
    struct kernel_symbol *sym;
    list_for_each_entry(sym, &kernel_symbols, list) {
        if (strcmp(sym->name, name) == 0) {
            return sym;
        }
    }
    
    /* 查找模块符号 */
    struct module *mod;
    list_for_each_entry(mod, &module_list, list) {
        for (uint32_t i = 0; i < mod->num_syms; i++) {
            if (strcmp(mod->syms[i].name, name) == 0) {
                return &mod->syms[i];
            }
        }
    }
    
    return NULL;
}

int module_add_symbol(struct module *mod, const char *name, uint64_t value) {
    if (mod == NULL || name == NULL) return -1;
    
    /* 扩展符号表 */
    uint32_t new_count = mod->num_syms + 1;
    struct kernel_symbol *new_syms = kmalloc(sizeof(*new_syms) * new_count);
    if (new_syms == NULL) return -1;
    
    /* 复制旧符号 */
    if (mod->syms != NULL) {
        memcpy(new_syms, mod->syms, sizeof(*new_syms) * mod->num_syms);
        kfree(mod->syms);
    }
    
    /* 添加新符号 */
    new_syms[mod->num_syms].name = name;
    new_syms[mod->num_syms].value = value;
    new_syms[mod->num_syms].module = mod;
    
    mod->syms = new_syms;
    mod->num_syms = new_count;
    
    return 0;
}

void *module_get_symbol(struct module *mod, const char *name) {
    struct kernel_symbol *sym = module_find_symbol(name);
    if (sym == NULL) return NULL;
    
    return (void *)sym->value;
}

/* ========== 引用计数 ========== */

struct module *module_get(struct module *mod) {
    if (mod == NULL) return NULL;
    
    spin_lock(&mod->lock);
    mod->ref_count++;
    spin_unlock(&mod->lock);
    
    return mod;
}

void module_put(struct module *mod) {
    if (mod == NULL) return;
    
    spin_lock(&mod->lock);
    if (mod->ref_count > 0) {
        mod->ref_count--;
    }
    spin_unlock(&mod->lock);
}

int module_ref_count(struct module *mod) {
    if (mod == NULL) return 0;
    return mod->ref_count;
}

/* ========== 系统调用 ========== */

int sys_init_module(void *module_image, size_t len, const char *param_values) {
    if (module_image == NULL || len == 0) return -1;
    
    /* 从模块镜像解析名称 */
    /* 简化：使用默认名称 */
    char name[64];
    strcpy(name, "mod_");
    /* 将指针转为十六进制字符串追加 */
    uint64_t ptr = (uint64_t)module_image;
    for (int i = 0; i < 16; i++) {
        int digit = (ptr >> (60 - i * 4)) & 0xF;
        name[4 + i] = digit < 10 ? '0' + digit : 'a' + digit - 10;
    }
    name[20] = '\0';
    
    struct module *mod = module_load(name, module_image, len);
    if (mod == NULL) return -1;
    
    /* 处理参数 */
    if (param_values != NULL) {
        /* TODO: 解析参数 */
    }
    
    return 0;
}

int sys_finit_module(int fd, const char *param_values, int flags) {
    /* TODO: 从文件描述符加载 */
    (void)fd;
    (void)param_values;
    (void)flags;
    return -1;
}

int sys_delete_module(const char *name, int flags) {
    if (name == NULL) return -1;
    (void)flags;
    
    return module_unload_name(name);
}

/* ========== 列表访问 ========== */

struct list_head *module_get_list(void) {
    return &module_list;
}

/* ========== 模块信息打印 ========== */

void module_print_list(void) {
    println("[MODULE] Loaded modules:");
    
    struct module *mod;
    list_for_each_entry(mod, &module_list, list) {
        print("  ");
        print(mod->name);
        print(" (");
        print(mod->version);
        print(", refs: ");
        print_dec(mod->ref_count);
        println(")");
    }
}
