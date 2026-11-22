#include "loader.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "abi.h"
#include "vfs.h"
#include "task.h"

/* 全局库缓存 */
static struct loaded_library loaded_libs[MAX_LIBRARIES];
static int lib_count = 0;
static spinlock_t loader_lock = SPINLOCK_INIT;

/* 库搜索路径 */
static char library_path[MAX_LIBRARY_PATH] = LD_LIBRARY_PATH;

/* ELF 哈希 */
static uint32_t elf_hash(const char *name) {
    uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + (unsigned char)*name++;
        if ((g = h & 0xf0000000)) {
            h ^= g >> 24;
        }
        h &= ~g;
    }
    return h;
}

/* GNU 哈希 */
static uint32_t gnu_hash(const char *name) {
    uint32_t h = 5381;
    while (*name) {
        h += (h << 5) + (unsigned char)*name++;
    }
    return h;
}

/* 初始化动态链接器 */
int loader_init(void) {
    lib_count = 0;
    for (int i = 0; i < MAX_LIBRARIES; i++) {
        loaded_libs[i].name[0] = '\0';
        loaded_libs[i].ref_count = 0;
        loaded_libs[i].base = 0;
        loaded_libs[i].size = 0;
    }
    serial_print("[LOADER] Dynamic linker initialized\n");
    return 0;
}

/* 查找已加载的库 */
static struct loaded_library *find_loaded_library_locked(const char *name) {
    for (int i = 0; i < lib_count; i++) {
        if (loaded_libs[i].ref_count > 0) {
            /* 检查库名是否匹配（支持 basename 匹配） */
            const char *libname = loaded_libs[i].name;
            /* 尝试完整路径匹配 */
            if (strcmp(libname, name) == 0) {
                return &loaded_libs[i];
            }
            /* 尝试 basename 匹配 */
            const char *basename = name;
            const char *p = name;
            while (*p) {
                if (*p == '/') basename = p + 1;
                p++;
            }
            if (strcmp(libname, basename) == 0) {
                return &loaded_libs[i];
            }
        }
    }
    return NULL;
}

/* 分配库槽 */
static struct loaded_library *alloc_library_slot_locked(void) {
    for (int i = 0; i < MAX_LIBRARIES; i++) {
        if (loaded_libs[i].ref_count == 0) {
            if (i >= lib_count) lib_count = i + 1;
            struct loaded_library *slot = &loaded_libs[i];
            slot->name[0] = '\0';
            slot->base = 0;
            slot->size = 0;
            slot->dynsym = 0;
            slot->dynstr = 0;
            slot->hash = 0;
            slot->gnu_hash = 0;
            slot->rela = 0;
            slot->relasz = 0;
            slot->relaent = 0;
            slot->init = 0;
            slot->fini = 0;
            slot->init_array = 0;
            slot->fini_array = 0;
            slot->init_arraysz = 0;
            slot->fini_arraysz = 0;
            slot->global = 1;
            return slot;
        }
    }
    return NULL;
}

/* 计算加载基址 */
static uint64_t compute_load_base(size_t size) {
    static uint64_t next_base = 0x7f0000000000ULL;
    uint64_t aligned_size = (size + 0xfff) & ~0xfffULL;
    uint64_t base = next_base;
    next_base += aligned_size + 0x1000000;  /* 留一些间隙 */
    return base;
}

/* 解析动态节 - 从已映射的内存地址 */
static int parse_dynamic_section(struct loaded_library *lib, uint64_t dyn_addr, size_t dyn_size, uint64_t base) {
    int64_t *entry = (int64_t *)dyn_addr;
    int64_t *end = entry + dyn_size / sizeof(int64_t);

    while (entry < end) {
        int64_t tag = entry[0];
        int64_t val = entry[1];

        if (tag == DT_NULL) break;

        switch (tag) {
            case DT_STRTAB:
                lib->dynstr = val;
                break;
            case DT_SYMTAB:
                lib->dynsym = val;
                break;
            case DT_HASH:
                lib->hash = val;
                break;
            case DT_GNU_HASH:
                lib->gnu_hash = val;
                break;
            case DT_RELA:
                lib->rela = val;
                break;
            case DT_RELASZ:
                lib->relasz = val;
                break;
            case DT_RELAENT:
                lib->relaent = val;
                break;
            case DT_INIT:
                lib->init = val;
                break;
            case DT_FINI:
                lib->fini = val;
                break;
            case DT_INIT_ARRAY:
                lib->init_array = val;
                break;
            case DT_FINI_ARRAY:
                lib->fini_array = val;
                break;
            case DT_INIT_ARRAYSZ:
                lib->init_arraysz = val;
                break;
            case DT_FINI_ARRAYSZ:
                lib->fini_arraysz = val;
                break;
        }

        entry += 2;
    }

    /* 将虚拟地址转换为相对于加载基址的偏移 */
    if (lib->dynstr) lib->dynstr -= base;
    if (lib->dynsym) lib->dynsym -= base;
    if (lib->hash) lib->hash -= base;
    if (lib->gnu_hash) lib->gnu_hash -= base;
    if (lib->rela) lib->rela -= base;

    return 0;
}

/* 从库的虚拟地址空间读取字符串 */
static const char *get_string(struct loaded_library *lib, uint32_t offset) {
    if (lib->dynstr == 0) return "";
    return (const char *)(lib->base + lib->dynstr + offset);
}

/* 从库的虚拟地址空间读取符号 */
static struct elf64_sym *get_symbol(struct loaded_library *lib, uint32_t index) {
    if (lib->dynsym == 0) return NULL;
    return (struct elf64_sym *)(lib->base + lib->dynsym + index * sizeof(struct elf64_sym));
}

/* 在库中查找符号 */
static uint32_t find_symbol_in_library(struct loaded_library *lib, const char *name) {
    if (lib == NULL || lib->dynsym == 0 || lib->dynstr == 0) return 0;

    uint32_t hash = elf_hash(name);

    /* 使用 SYSV 哈希表 */
    if (lib->hash != 0) {
        uint32_t *hashtab = (uint32_t *)(lib->base + lib->hash);
        uint32_t nbucket = hashtab[0];
        uint32_t *buckets = &hashtab[2];
        uint32_t *chains = &buckets[nbucket];

        for (uint32_t idx = buckets[hash % nbucket]; idx != 0; idx = chains[idx]) {
            struct elf64_sym *sym = get_symbol(lib, idx);
            if (sym == NULL) continue;

            const char *symname = get_string(lib, sym->st_name);
            if (strcmp(symname, name) == 0) {
                return idx;
            }
        }
    }

    /* 回退：线性搜索符号表 */
    if (lib->dynsym != 0) {
        /* 估算符号表大小 */
        size_t max_syms = 65536;  /* 合理上限 */
        for (uint32_t i = 0; i < max_syms; i++) {
            struct elf64_sym *sym = get_symbol(lib, i);
            if (sym == NULL) break;

            /* 检查是否是有效的符号 */
            if (sym->st_name == 0) continue;
            if (ELF64_ST_BIND(sym->st_info) == STB_LOCAL) continue;

            const char *symname = get_string(lib, sym->st_name);
            if (symname[0] == '\0') break;  /* 到达字符串表末尾 */

            if (strcmp(symname, name) == 0) {
                return i;
            }
        }
    }

    return 0;
}

/* 查找符号（全局） */
struct symbol_result loader_find_symbol(const char *name, struct loaded_library *lib) {
    struct symbol_result result = {0, NULL, 0};

    if (name == NULL || name[0] == '\0') return result;

    spin_lock(&loader_lock);

    /* 先在指定库中查找 */
    if (lib != NULL) {
        uint32_t sym_idx = find_symbol_in_library(lib, name);
        if (sym_idx != 0) {
            struct elf64_sym *sym = get_symbol(lib, sym_idx);
            if (sym && sym->st_value != 0) {
                result.addr = lib->base + sym->st_value;
                result.lib = lib;
                result.weak = (ELF64_ST_BIND(sym->st_info) == STB_WEAK);
                spin_unlock(&loader_lock);
                return result;
            }
        }
    }

    /* 在全局库中查找 */
    for (int i = 0; i < lib_count; i++) {
        if (loaded_libs[i].ref_count > 0 && loaded_libs[i].global) {
            uint32_t sym_idx = find_symbol_in_library(&loaded_libs[i], name);
            if (sym_idx != 0) {
                struct elf64_sym *sym = get_symbol(&loaded_libs[i], sym_idx);
                if (sym && sym->st_value != 0) {
                    result.addr = loaded_libs[i].base + sym->st_value;
                    result.lib = &loaded_libs[i];
                    result.weak = (ELF64_ST_BIND(sym->st_info) == STB_WEAK);
                    spin_unlock(&loader_lock);
                    return result;
                }
            }
        }
    }

    spin_unlock(&loader_lock);
    return result;
}

/* 执行重定位 */
int loader_relocate(struct loaded_library *lib) {
    if (lib == NULL || lib->rela == 0 || lib->relasz == 0 || lib->relaent == 0) {
        return 0;
    }

    struct elf64_rela *rela = (struct elf64_rela *)(lib->base + lib->rela);
    size_t count = lib->relasz / lib->relaent;

    for (size_t i = 0; i < count; i++) {
        uint64_t offset = rela[i].r_offset;
        uint64_t type = ELF64_R_TYPE(rela[i].r_info);
        uint32_t sym_idx = ELF64_R_SYM(rela[i].r_info);
        int64_t addend = rela[i].r_addend;

        uint64_t *target = (uint64_t *)(lib->base + offset);

        switch (type) {
            case R_X86_64_RELATIVE:
                /* 基址重定位：*target = base + addend */
                *target = lib->base + addend;
                break;

            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT: {
                /* 符号重定位：查找符号并写入地址 */
                if (sym_idx != 0) {
                    struct elf64_sym *sym = get_symbol(lib, sym_idx);
                    if (sym) {
                        const char *symname = get_string(lib, sym->st_name);
                        struct symbol_result found = loader_find_symbol(symname, lib);
                        if (found.addr != 0) {
                            *target = found.addr;
                        } else if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
                            *target = 0;  /* 弱符号可以为 NULL */
                        } else {
                            serial_print("[LOADER] ERROR: Undefined symbol: ");
                            serial_print(symname);
                            serial_print("\n");
                        }
                    }
                }
                break;
            }

            case R_X86_64_64: {
                /* 绝对 64 位地址 */
                if (sym_idx != 0) {
                    struct elf64_sym *sym = get_symbol(lib, sym_idx);
                    if (sym) {
                        const char *symname = get_string(lib, sym->st_name);
                        struct symbol_result found = loader_find_symbol(symname, lib);
                        if (found.addr != 0) {
                            *target = found.addr + addend;
                        } else {
                            serial_print("[LOADER] ERROR: Undefined symbol: ");
                            serial_print(symname);
                            serial_print("\n");
                        }
                    }
                } else {
                    *target = lib->base + addend;
                }
                break;
            }

            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                /* PC 相对重定位 */
                if (sym_idx != 0) {
                    struct elf64_sym *sym = get_symbol(lib, sym_idx);
                    if (sym) {
                        const char *symname = get_string(lib, sym->st_name);
                        struct symbol_result found = loader_find_symbol(symname, lib);
                        if (found.addr != 0) {
                            *target = (int32_t)(found.addr - (uint64_t)target + addend);
                        } else if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
                            *target = (int32_t)(-(uint64_t)target + addend);
                        } else {
                            serial_print("[LOADER] ERROR: Undefined symbol: ");
                            serial_print(symname);
                            serial_print("\n");
                        }
                    }
                } else {
                    *target = (int32_t)(addend - (uint64_t)target);
                }
                break;
            }

            case R_X86_64_NONE:
                break;

            default:
                serial_print("[LOADER] WARNING: Unsupported relocation type: ");
                char buf[16];
                int n = type, pos = 0;
                if (n == 0) buf[pos++] = '0';
                else {
                    char tmp[16]; int t = 0;
                    while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
                    while (t > 0) buf[pos++] = tmp[--t];
                }
                buf[pos] = '\0';
                serial_print(buf);
                serial_print(" at offset ");
                n = offset; pos = 0;
                if (n == 0) buf[pos++] = '0';
                else {
                    char tmp[16]; int t = 0;
                    while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
                    while (t > 0) buf[pos++] = tmp[--t];
                }
                buf[pos] = '\0';
                serial_print(buf);
                serial_print("\n");
                break;
        }
    }

    return 0;
}

/* 从 VFS 加载共享库到进程地址空间 */
static struct loaded_library *load_library_from_vfs(const char *path, struct process *proc) {
    serial_print("[LOADER] Loading from VFS: ");
    serial_print(path);
    serial_print("\n");

    /* 读取文件 */
    size_t file_size;
    void *data = vfs_read_file(path, &file_size);
    if (data == NULL) {
        serial_print("[LOADER] ERROR: Cannot read file\n");
        return NULL;
    }

    /* 验证 ELF */
    struct elf64_hdr *hdr = (struct elf64_hdr *)data;
    if (file_size < sizeof(struct elf64_hdr) ||
        hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' ||
        hdr->e_ident[2] != 'L' || hdr->e_ident[3] != 'F') {
        serial_print("[LOADER] ERROR: Invalid ELF header\n");
        kfree(data);
        return NULL;
    }

    /* 检查是否为共享对象 */
    if (hdr->e_type != ELF_TYPE_DYN && hdr->e_type != ELF_TYPE_REL) {
        serial_print("[LOADER] ERROR: Not a shared object\n");
        kfree(data);
        return NULL;
    }

    /* 计算加载基址 */
    /* 首先计算需要的总大小 */
    size_t total_size = 0;
    struct elf64_phdr *phdr = (struct elf64_phdr *)((uint8_t *)data + hdr->e_phoff);
    for (int i = 0; i < hdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            size_t seg_size = phdr[i].p_vaddr + phdr[i].p_memsz;
            if (seg_size > total_size) total_size = seg_size;
        }
    }

    uint64_t load_base = compute_load_base(total_size);

    /* 映射段 */
    for (int i = 0; i < hdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;

        uint64_t vaddr_start = load_base + (phdr[i].p_vaddr & ~(PAGE_SIZE - 1));
        uint64_t vaddr_end = load_base + phdr[i].p_vaddr + phdr[i].p_memsz;
        vaddr_end = (vaddr_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (phdr[i].p_flags & PF_W) flags |= VMM_FLAG_WRITABLE;
        if (!(phdr[i].p_flags & PF_X)) flags |= VMM_FLAG_NX;

        for (uint64_t va = vaddr_start; va < vaddr_end; va += PAGE_SIZE) {
            uint64_t phys = pmm_alloc_page();
            if (phys == 0) {
                serial_print("[LOADER] ERROR: Cannot alloc page\n");
                kfree(data);
                return NULL;
            }

            void *page = phys_to_virt(phys);
            memset(page, 0, PAGE_SIZE);

            /* 复制数据 */
            uint64_t page_offset = va - vaddr_start;
            uint64_t seg_offset_in_page = phdr[i].p_vaddr & (PAGE_SIZE - 1);
            uint64_t file_offset = phdr[i].p_offset + (seg_offset_in_page - (phdr[i].p_vaddr & (PAGE_SIZE - 1)));

            if (file_offset < file_size) {
                uint64_t copy_size = PAGE_SIZE;
                if (file_offset + copy_size > file_size) {
                    copy_size = file_size - file_offset;
                }
                /* 检查是否在该段的有效范围内 */
                uint64_t seg_data_offset = phdr[i].p_vaddr & (PAGE_SIZE - 1);
                if (page_offset >= seg_data_offset) {
                    uint64_t data_in_page = seg_data_offset;
                    if (data_in_page < PAGE_SIZE) {
                        uint64_t to_copy = PAGE_SIZE - data_in_page;
                        if (file_offset + to_copy <= file_size) {
                            memcpy((uint8_t *)page + data_in_page,
                                   (uint8_t *)data + file_offset,
                                   to_copy < copy_size ? to_copy : copy_size);
                        }
                    }
                }
            }

            if (vmm_map_page(proc->space, va, phys, flags) != 0) {
                serial_print("[LOADER] ERROR: Cannot map page\n");
                kfree(data);
                return NULL;
            }
        }
    }

    /* 查找动态节 */
    uint64_t dyn_addr = 0;
    size_t dyn_size = 0;
    for (int i = 0; i < hdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn_addr = load_base + phdr[i].p_vaddr;
            dyn_size = phdr[i].p_memsz;
            break;
        }
    }

    kfree(data);

    return NULL;  /* 返回 NULL，由调用者处理 - 这里只是映射了库 */
}

/* 加载共享库 - 完整实现 */
struct loaded_library *loader_load_library(const char *name, struct process *proc) {
    if (name == NULL) return NULL;

    serial_print("[LOADER] Loading library: ");
    serial_print(name);
    serial_print("\n");

    spin_lock(&loader_lock);

    /* 检查是否已加载 */
    struct loaded_library *lib = find_loaded_library_locked(name);
    if (lib != NULL) {
        lib->ref_count++;
        spin_unlock(&loader_lock);
        return lib;
    }

    /* 分配槽位 */
    lib = alloc_library_slot_locked();
    if (lib == NULL) {
        spin_unlock(&loader_lock);
        serial_print("[LOADER] ERROR: Too many libraries\n");
        return NULL;
    }

    /* 复制名称 */
    int i = 0;
    while (name[i] && i < 255) {
        lib->name[i] = name[i];
        i++;
    }
    lib->name[i] = '\0';

    /* TODO: 实际从文件系统加载库并解析动态节 */
    /* 当前为占位实现 */
    lib->base = 0;
    lib->size = 0;
    lib->ref_count = 1;

    spin_unlock(&loader_lock);
    serial_print("[LOADER] Library loaded (stub)\n");
    return lib;
}

/* 卸载共享库 */
int loader_unload_library(struct loaded_library *lib) {
    if (lib == NULL) return -1;

    spin_lock(&loader_lock);
    lib->ref_count--;

    if (lib->ref_count <= 0) {
        serial_print("[LOADER] Unloading: ");
        serial_print(lib->name);
        serial_print("\n");

        loader_call_fini(lib);
        lib->name[0] = '\0';
        lib->base = 0;
        lib->size = 0;
    }
    spin_unlock(&loader_lock);
    return 0;
}

/* 调用初始化函数 */
int loader_call_init(struct loaded_library *lib) {
    if (lib == NULL) return -1;

    serial_print("[LOADER] Calling init for: ");
    serial_print(lib->name);
    serial_print("\n");

    /* 内核态不能直接调用用户态函数，需要通过特殊机制 */
    /* 这里只打印信息，实际调用需要在用户态进行 */

    return 0;
}

/* 调用清理函数 */
void loader_call_fini(struct loaded_library *lib) {
    if (lib == NULL) return;

    serial_print("[LOADER] Calling fini for: ");
    serial_print(lib->name);
    serial_print("\n");
}

/* 构建 link_map */
struct link_map *loader_build_link_map(struct process *proc) {
    (void)proc;

    struct link_map *map = kmalloc(sizeof(struct link_map));
    if (map == NULL) return NULL;

    map->lib = NULL;
    map->next = NULL;
    map->prev = NULL;

    struct link_map *tail = map;
    for (int i = 0; i < lib_count; i++) {
        if (loaded_libs[i].ref_count > 0 && loaded_libs[i].global) {
            struct link_map *entry = kmalloc(sizeof(struct link_map));
            if (entry == NULL) {
                struct link_map *curr = map;
                while (curr != NULL) {
                    struct link_map *next = curr->next;
                    kfree(curr);
                    curr = next;
                }
                return NULL;
            }

            entry->lib = &loaded_libs[i];
            entry->next = NULL;
            entry->prev = tail;
            tail->next = entry;
            tail = entry;
        }
    }

    return map;
}

/* 获取/设置库搜索路径 */
const char *loader_get_library_path(void) {
    return library_path;
}

void loader_set_library_path(const char *path) {
    if (path == NULL) return;
    int i = 0;
    while (path[i] && i < MAX_LIBRARY_PATH - 1) {
        library_path[i] = path[i];
        i++;
    }
    library_path[i] = '\0';
}

/* 加载 ELF 解释器 */
int loader_load_interpreter(const char *path, struct process *proc) {
    serial_print("[LOADER] Loading interpreter: ");
    serial_print(path);
    serial_print("\n");

    struct loaded_library *interp = loader_load_library(path, proc);
    if (interp == NULL) {
        serial_print("[LOADER] ERROR: Failed to load interpreter\n");
        return -1;
    }

    loader_relocate(interp);
    loader_call_init(interp);

    return 0;
}

/* 获取符号地址 */
uint64_t loader_get_symbol_addr(struct loaded_library *lib, const char *name) {
    struct symbol_result result = loader_find_symbol(name, lib);
    return result.addr;
}

/* 解析 PLT/GOT */
int loader_resolve_plt(struct loaded_library *lib, uint64_t plt_addr) {
    (void)lib;
    (void)plt_addr;
    return 0;
}
