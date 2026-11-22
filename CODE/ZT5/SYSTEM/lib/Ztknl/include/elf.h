#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include "task.h"

/* ELF 魔数 */
#define ELF_MAGIC      0x464C457F  /* 0x7F 'E' 'L' 'F' */

/* ELF 类型 */
#define ELF_TYPE_NONE   0
#define ELF_TYPE_REL    1   /* 可重定位 */
#define ELF_TYPE_EXEC   2   /* 可执行 */
#define ELF_TYPE_DYN    3   /* 共享对象 */

/* ELF 机器类型 */
#define ELF_MACHINE_X86_64  0x3E

/* 程序头类型 */
#define PT_NULL     0
#define PT_LOAD     1   /* 可加载段 */
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_PHDR     6

/* 程序头标志 */
#define PF_X    0x1     /* 可执行 */
#define PF_W    0x2     /* 可写 */
#define PF_R    0x4     /* 可读 */

/* ELF64 文件头 */
struct elf64_hdr {
    uint8_t  e_ident[16];       /* ELF 标识 */
    uint16_t e_type;            /* 目标文件类型 */
    uint16_t e_machine;         /* 机器类型 */
    uint32_t e_version;         /* 版本 */
    uint64_t e_entry;           /* 入口点 */
    uint64_t e_phoff;           /* 程序头偏移 */
    uint64_t e_shoff;           /* 节头偏移 */
    uint32_t e_flags;           /* 标志 */
    uint16_t e_ehsize;          /* ELF 头大小 */
    uint16_t e_phentsize;       /* 程序头条目大小 */
    uint16_t e_phnum;           /* 程序头条目数 */
    uint16_t e_shentsize;       /* 节头条目大小 */
    uint16_t e_shnum;           /* 节头条目数 */
    uint16_t e_shstrndx;        /* 节字符串表索引 */
} __attribute__((packed));

/* 别名 */
#define elf64_ehdr elf64_hdr

/* ELF64 程序头 */
struct elf64_phdr {
    uint32_t p_type;            /* 段类型 */
    uint32_t p_flags;           /* 段标志 */
    uint64_t p_offset;          /* 文件偏移 */
    uint64_t p_vaddr;           /* 虚拟地址 */
    uint64_t p_paddr;           /* 物理地址 */
    uint64_t p_filesz;          /* 文件大小 */
    uint64_t p_memsz;           /* 内存大小 */
    uint64_t p_align;           /* 对齐 */
} __attribute__((packed));

/* ELF64 节头 */
struct elf64_shdr {
    uint32_t sh_name;           /* 节名称索引 */
    uint32_t sh_type;           /* 节类型 */
    uint64_t sh_flags;          /* 节标志 */
    uint64_t sh_addr;           /* 虚拟地址 */
    uint64_t sh_offset;         /* 文件偏移 */
    uint64_t sh_size;           /* 节大小 */
    uint32_t sh_link;           /* 链接索引 */
    uint32_t sh_info;           /* 附加信息 */
    uint64_t sh_addralign;      /* 对齐 */
    uint64_t sh_entsize;        /* 条目大小 */
} __attribute__((packed));

/* 节类型 */
#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_HASH        5
#define SHT_DYNAMIC     6
#define SHT_NOTE        7
#define SHT_NOBITS      8
#define SHT_REL         9
#define SHT_DYNSYM      11

/* ELF64 符号表项 */
struct elf64_sym {
    uint32_t st_name;           /* 符号名称索引 */
    uint8_t  st_info;           /* 类型和绑定属性 */
    uint8_t  st_other;          /* 可见性 */
    uint16_t st_shndx;          /* 所属节索引 */
    uint64_t st_value;          /* 符号值 */
    uint64_t st_size;           /* 符号大小 */
} __attribute__((packed));

/* 符号类型 */
#define STT_NOTYPE      0
#define STT_OBJECT      1
#define STT_FUNC        2
#define STT_SECTION     3
#define STT_FILE        4

/* 符号绑定 */
#define STB_LOCAL       0
#define STB_GLOBAL      1
#define STB_WEAK        2

/* ELF64 重定位项 (带加数) */
struct elf64_rela {
    uint64_t r_offset;          /* 重定位位置 */
    uint64_t r_info;            /* 符号索引和类型 */
    int64_t  r_addend;          /* 加数 */
} __attribute__((packed));

/* ELF64 重定位项 (无加数) */
struct elf64_rel {
    uint64_t r_offset;          /* 重定位位置 */
    uint64_t r_info;            /* 符号索引和类型 */
} __attribute__((packed));

/* 重定位类型 (x86-64) */
#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_PC32       2
#define R_X86_64_GOT32      3
#define R_X86_64_PLT32      4
#define R_X86_64_32         10
#define R_X86_64_32S        11
#define R_X86_64_16         12
#define R_X86_64_PC16       13
#define R_X86_64_8          14
#define R_X86_64_PC8        15
#define R_X86_64_64PCREL    24

/* 获取重定位符号索引 */
#define ELF64_R_SYM(i)      ((i) >> 32)
#define ELF64_R_TYPE(i)     ((i) & 0xffffffff)

/* 获取符号类型和绑定 */
#define ELF64_ST_TYPE(i)    ((i) & 0xf)
#define ELF64_ST_BIND(i)    ((i) >> 4)

/* ELF 加载结果 */
struct elf_load_result {
    uint64_t entry;             /* 入口点 */
    uint64_t stack_top;         /* 栈顶 */
    int error;                  /* 错误码 */
};

/* 加载 ELF 文件 */
int elf_load(const void *data, size_t size, struct process *proc, struct elf_load_result *result);

/* 从文件加载 ELF */
int elf_load_file(const char *path, struct process **proc_out, uint64_t *entry_out);

/* 从内存加载 ELF (用于 Limine 模块) */
int elf_load_from_memory(void *data, size_t size, struct process **proc_out, uint64_t *entry_out);

/* 验证 ELF 文件 */
int elf_validate(const void *data, size_t size);

uint64_t elf_lookup_symbol(const void *elf_data, size_t elf_size, const char *name);

uint64_t elf_lookup_symbol_from_file(const char *path, const char *name);

#endif
