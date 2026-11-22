#ifndef LOADER_H
#define LOADER_H

#include <stdint.h>
#include <stddef.h>
#include "abi.h"
#include "task.h"

/* 动态链接器配置 */
#define LD_LIBRARY_PATH     "/lib:/usr/lib"
#define LD_CACHE_PATH       "/etc/ld.so.cache"
#define MAX_LIBRARY_PATH    256
#define MAX_LIBRARIES       64
#define MAX_SYMBOLS         4096

/* ELF 动态节标记 */
#define DT_NULL             0
#define DT_NEEDED           1
#define DT_PLTRELSZ         2
#define DT_PLTGOT           3
#define DT_HASH             4
#define DT_STRTAB           5
#define DT_SYMTAB           6
#define DT_RELA             7
#define DT_RELASZ           8
#define DT_RELAENT          9
#define DT_STRSZ            10
#define DT_SYMENT           11
#define DT_INIT             12
#define DT_FINI             13
#define DT_SONAME           14
#define DT_RPATH            15
#define DT_SYMBOLIC         16
#define DT_RELCOUNT         0x6ffffff9
#define DT_FLAGS            0x6ffffffb
#define DT_RUNPATH          29
#define DT_GNU_HASH         0x6ffffef5
#define DT_VERSYM           0x6ffffff0
#define DT_RELACOUNT        0x6ffffff9
#define DT_INIT_ARRAY       0x6000000e
#define DT_FINI_ARRAY       0x6000000f
#define DT_INIT_ARRAYSZ     0x60000010
#define DT_FINI_ARRAYSZ     0x60000011
#define DT_JMPREL           0x00000017  /* PLT 重定位表 */

/* 重定位类型 (x86_64) */
#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_PC32       2
#define R_X86_64_GOT32      3
#define R_X86_64_PLT32      4
#define R_X86_64_COPY       5
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8
#define R_X86_64_GOTPCREL   9
#define R_X86_64_32         10
#define R_X86_64_IRELATIVE  37

/* 符号绑定 */
#define STB_LOCAL           0
#define STB_GLOBAL          1
#define STB_WEAK            2

/* 符号类型 */
#define STT_NOTYPE          0
#define STT_OBJECT          1
#define STT_FUNC            2
#define STT_SECTION         3
#define STT_FILE            4

/* 加载的共享库 */
struct loaded_library {
    char name[256];
    uint64_t base;              /* 加载基址 */
    uint64_t size;              /* 映射大小 */
    
    /* 动态链接信息 */
    uint64_t dynsym;            /* 符号表 */
    uint64_t dynstr;            /* 字符串表 */
    uint64_t hash;              /* 符号哈希表 */
    uint64_t gnu_hash;          /* GNU 哈希表 */
    uint64_t rela;              /* 重定位表 */
    uint64_t relasz;            /* 重定位表大小 */
    uint64_t relaent;           /* 重定位项大小 */
    
    /* 初始化/清理函数 */
    uint64_t init;
    uint64_t fini;
    uint64_t init_array;
    uint64_t fini_array;
    uint64_t init_arraysz;
    uint64_t fini_arraysz;
    
    /* 引用计数 */
    int ref_count;
    int global;                 /* 是否全局可见 */
};

/* 动态链接器上下文 */
struct link_map {
    struct loaded_library *lib;
    struct link_map *next;
    struct link_map *prev;
};

/* 全局偏移表项 */
struct got_entry {
    uint64_t addr;
    int resolved;
    char *symbol_name;
};

/* 符号查找结果 */
struct symbol_result {
    uint64_t addr;
    struct loaded_library *lib;
    int weak;
};

/* 初始化动态链接器 */
int loader_init(void);

/* 加载 ELF 解释器 */
int loader_load_interpreter(const char *path, struct process *proc);

/* 加载共享库 */
struct loaded_library *loader_load_library(const char *name, struct process *proc);

/* 卸载共享库 */
int loader_unload_library(struct loaded_library *lib);

/* 查找符号 */
struct symbol_result loader_find_symbol(const char *name, struct loaded_library *lib);

/* 执行重定位 */
int loader_relocate(struct loaded_library *lib);

/* 解析 PLT/GOT */
int loader_resolve_plt(struct loaded_library *lib, uint64_t plt_addr);

/* 获取符号地址 */
uint64_t loader_get_symbol_addr(struct loaded_library *lib, const char *name);

/* 调用初始化函数 */
int loader_call_init(struct loaded_library *lib);

/* 调用清理函数 */
void loader_call_fini(struct loaded_library *lib);

/* 构建初始 link_map */
struct link_map *loader_build_link_map(struct process *proc);

/* 获取 LD_LIBRARY_PATH */
const char *loader_get_library_path(void);

/* 设置 LD_LIBRARY_PATH */
void loader_set_library_path(const char *path);

#endif
