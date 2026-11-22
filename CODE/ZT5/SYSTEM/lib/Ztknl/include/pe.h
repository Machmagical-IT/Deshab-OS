/*
 * Zratro OS - PE/COFF Loader Header
 * 
 * 完整的 Windows PE32+ 格式支持
 */

#ifndef PE_H
#define PE_H

#include <stdint.h>
#include <stddef.h>
#include "ktypes.h"

/* PE/COFF 签名 */
#define PE_SIGNATURE            0x00004550  /* "PE\0\0" */
#define DOS_SIGNATURE           0x5A4D      /* "MZ" */
#define PE32_MAGIC              0x10B       /* PE32 */
#define PE32_PLUS_MAGIC         0x20B       /* PE32+ (64-bit) */

/* ========== DOS 头 ========== */

struct dos_hdr {
    uint16_t e_magic;       /* MZ 签名 */
    uint16_t e_cblp;        /* 最后页字节数 */
    uint16_t e_cp;          /* 文件页数 */
    uint16_t e_crlc;        /* 重定位项数 */
    uint16_t e_cparhdr;     /* 段头大小 */
    uint16_t e_minalloc;    /* 最小额外内存 */
    uint16_t e_maxalloc;    /* 最大额外内存 */
    uint16_t e_ss;          /* 初始 SS 值 */
    uint16_t e_sp;          /* 初始 SP 值 */
    uint16_t e_csum;        /* 校验和 */
    uint16_t e_ip;          /* 初始 IP 值 */
    uint16_t e_cs;          /* 初始 CS 值 */
    uint16_t e_lfarlc;      /* 重定位表偏移 */
    uint16_t e_ovno;        /* 覆盖号 */
    uint16_t e_res[4];      /* 保留 */
    uint16_t e_oemid;       /* OEM 标识符 */
    uint16_t e_oeminfo;     /* OEM 信息 */
    uint16_t e_res2[10];    /* 保留 */
    uint32_t e_lfanew;      /* PE 头偏移 */
} __attribute__((packed));

/* ========== COFF 文件头 ========== */

struct coff_hdr {
    uint16_t Machine;               /* 机器类型 */
    uint16_t NumberOfSections;      /* 节数量 */
    uint32_t TimeDateStamp;         /* 时间戳 */
    uint32_t PointerToSymbolTable;  /* 符号表指针 */
    uint32_t NumberOfSymbols;       /* 符号数 */
    uint16_t SizeOfOptionalHeader;  /* 可选头大小 */
    uint16_t Characteristics;       /* 特征 */
} __attribute__((packed));

/* 机器类型 */
#define IMAGE_FILE_MACHINE_AMD64   0x8664
#define IMAGE_FILE_MACHINE_I386    0x014c

/* COFF 特征 */
#define IMAGE_FILE_RELOCS_STRIPPED     0x0001
#define IMAGE_FILE_EXECUTABLE_IMAGE    0x0002
#define IMAGE_FILE_LARGE_ADDRESS_AWARE 0x0020
#define IMAGE_FILE_32BIT_MACHINE       0x0100
#define IMAGE_FILE_DEBUG_STRIPPED      0x0200
#define IMAGE_FILE_DLL                 0x2000

/* ========== PE 可选头 (PE32+) ========== */

struct pe_opt_hdr {
    uint16_t Magic;                         /* 0x20b for PE32+ */
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;                    /* 代码段大小 */
    uint32_t SizeOfInitializedData;         /* 已初始化数据大小 */
    uint32_t SizeOfUninitializedData;       /* 未初始化数据大小 */
    uint32_t AddressOfEntryPoint;           /* 入口点 RVA */
    uint32_t BaseOfCode;                    /* 代码段基址 RVA */
    /* PE32+ 特有 (无 BaseOfData) */
    uint64_t ImageBase;                     /* 镜像基址 */
    uint32_t SectionAlignment;              /* 节对齐 */
    uint32_t FileAlignment;                 /* 文件对齐 */
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;                   /* 镜像大小 */
    uint32_t SizeOfHeaders;                 /* 头大小 */
    uint32_t CheckSum;
    uint16_t Subsystem;                     /* 子系统类型 */
    uint16_t DllCharacteristics;            /* DLL 特征 */
    uint64_t SizeOfStackReserve;            /* 栈保留大小 */
    uint64_t SizeOfStackCommit;             /* 栈提交大小 */
    uint64_t SizeOfHeapReserve;             /* 堆保留大小 */
    uint64_t SizeOfHeapCommit;              /* 堆提交大小 */
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;           /* 数据目录数 */
} __attribute__((packed));

/* 子系统类型 */
#define IMAGE_SUBSYSTEM_WINDOWS_GUI      2
#define IMAGE_SUBSYSTEM_WINDOWS_CUI      3

/* DLL 特征 */
#define IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA    0x0020
#define IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE       0x0040
#define IMAGE_DLLCHARACTERISTICS_NX_COMPAT          0x0100
#define IMAGE_DLLCHARACTERISTICS_NO_SEH             0x0400
#define IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE  0x8000

/* ========== 数据目录 ========== */

struct data_dir {
    uint32_t VirtualAddress;    /* RVA */
    uint32_t Size;
} __attribute__((packed));

/* 数据目录索引 */
#define PE_DIR_EXPORT           0
#define PE_DIR_IMPORT           1
#define PE_DIR_RESOURCE         2
#define PE_DIR_EXCEPTION        3
#define PE_DIR_SECURITY         4
#define PE_DIR_BASERELOC        5
#define PE_DIR_DEBUG            6
#define PE_DIR_ARCHITECTURE     7
#define PE_DIR_GLOBALPTR        8
#define PE_DIR_TLS              9
#define PE_DIR_LOAD_CONFIG      10
#define PE_DIR_BOUND_IMPORT     11
#define PE_DIR_IAT              12
#define PE_DIR_DELAY_IMPORT     13
#define PE_DIR_CLR_RUNTIME      14

/* ========== 节头 ========== */

struct section_hdr {
    char     Name[8];              /* 节名 */
    uint32_t VirtualSize;          /* 虚拟大小 */
    uint32_t VirtualAddress;       /* 虚拟地址 (RVA) */
    uint32_t SizeOfRawData;        /* 原始数据大小 */
    uint32_t PointerToRawData;     /* 原始数据指针 */
    uint32_t PointerToRelocations; /* 重定位指针 */
    uint32_t PointerToLinenumbers; /* 行号指针 */
    uint16_t NumberOfRelocations;  /* 重定位数 */
    uint16_t NumberOfLinenumbers;  /* 行号数 */
    uint32_t Characteristics;      /* 特征 */
} __attribute__((packed));

/* 节特征 */
#define IMAGE_SCN_TYPE_NO_PAD           0x00000008
#define IMAGE_SCN_CNT_CODE              0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA  0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_SCN_LNK_INFO              0x00000200
#define IMAGE_SCN_LNK_REMOVE            0x00000800
#define IMAGE_SCN_LNK_COMDAT            0x00001000
#define IMAGE_SCN_NO_DEFER_SPEC_EXCPTS  0x00004000
#define IMAGE_SCN_GPREL                 0x00008000
#define IMAGE_SCN_MEM_PURGEABLE         0x00020000
#define IMAGE_SCN_MEM_LOCKED            0x00040000
#define IMAGE_SCN_MEM_PRELOAD           0x00080000
#define IMAGE_SCN_ALIGN_1BYTES          0x00100000
#define IMAGE_SCN_ALIGN_2BYTES          0x00200000
#define IMAGE_SCN_ALIGN_4BYTES          0x00300000
#define IMAGE_SCN_ALIGN_8BYTES          0x00400000
#define IMAGE_SCN_ALIGN_16BYTES         0x00500000
#define IMAGE_SCN_ALIGN_32BYTES         0x00600000
#define IMAGE_SCN_ALIGN_64BYTES         0x00700000
#define IMAGE_SCN_ALIGN_128BYTES        0x00800000
#define IMAGE_SCN_ALIGN_256BYTES        0x00900000
#define IMAGE_SCN_ALIGN_512BYTES        0x00A00000
#define IMAGE_SCN_ALIGN_1024BYTES       0x00B00000
#define IMAGE_SCN_ALIGN_2048BYTES       0x00C00000
#define IMAGE_SCN_ALIGN_4096BYTES       0x00D00000
#define IMAGE_SCN_ALIGN_8192BYTES       0x00E00000
#define IMAGE_SCN_LNK_NRELOC_OVFL       0x01000000
#define IMAGE_SCN_MEM_DISCARDABLE       0x02000000
#define IMAGE_SCN_MEM_NOT_CACHED        0x04000000
#define IMAGE_SCN_MEM_NOT_PAGED         0x08000000
#define IMAGE_SCN_MEM_SHARED            0x10000000
#define IMAGE_SCN_MEM_EXECUTE           0x20000000
#define IMAGE_SCN_MEM_READ              0x40000000
#define IMAGE_SCN_MEM_WRITE             0x80000000

/* ========== 导出表 ========== */

struct export_dir {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint32_t Name;                      /* DLL 名称 RVA */
    uint32_t Base;                      /* 导出序号基数 */
    uint32_t NumberOfFunctions;         /* 导出函数数 */
    uint32_t NumberOfNames;             /* 导出名称数 */
    uint32_t AddressOfFunctions;        /* 函数地址表 RVA */
    uint32_t AddressOfNames;            /* 名称表 RVA */
    uint32_t AddressOfNameOrdinals;     /* 序号表 RVA */
} __attribute__((packed));

/* ========== 导入表 ========== */

struct import_dir {
    uint32_t OriginalFirstThunk;    /* 导入名称表 RVA */
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;                  /* DLL 名称 RVA */
    uint32_t FirstThunk;            /* 导入地址表 RVA */
} __attribute__((packed));

/* 导入查找项 (PE32+ 64位) */
struct import_lookup {
    union {
        uint64_t OrdinalName;       /* 最高位=1: 按序号导入 */
        struct {
            uint64_t OrdinalNumber : 16;
            uint64_t Reserved : 47;
            uint64_t Flag : 1;      /* 0=按名称, 1=按序号 */
        };
    };
} __attribute__((packed));

#define IMPORT_BY_ORDINAL   0x8000000000000000ULL

/* 导入名称提示/名称项 */
struct import_hint_name {
    uint16_t Hint;                  /* 导出名称表索引 */
    char     Name[1];               /* 以 NULL 结尾的函数名 */
} __attribute__((packed));

/* ========== 重定位表 ========== */

struct reloc_block {
    uint32_t VirtualAddress;        /* 页面基址 RVA */
    uint32_t SizeOfBlock;           /* 块大小 */
    uint16_t TypeOffset[1];         /* 类型/偏移数组 */
} __attribute__((packed));

/* 重定位类型 */
#define IMAGE_REL_BASED_ABSOLUTE    0   /* 忽略 */
#define IMAGE_REL_BASED_HIGH        1   /* 高 16 位 */
#define IMAGE_REL_BASED_LOW         2   /* 低 16 位 */
#define IMAGE_REL_BASED_HIGHLOW     3   /* 32 位 */
#define IMAGE_REL_BASED_DIR64       10  /* 64 位 (PE32+) */

/* 重定位类型/偏移解析 */
#define RELOC_TYPE(x)   ((x) >> 12)
#define RELOC_OFFSET(x) ((x) & 0xFFF)

/* ========== TLS 表 ========== */

struct tls_dir {
    uint64_t StartAddressOfRawData;     /* TLS 数据起始 RVA */
    uint64_t EndAddressOfRawData;       /* TLS 数据结束 RVA */
    uint64_t AddressOfIndex;            /* TLS 索引 RVA */
    uint64_t AddressOfCallBacks;        /* 回调函数数组 RVA */
    uint32_t SizeOfZeroFill;            /* 零填充大小 */
    uint32_t Characteristics;           /* 特征 */
} __attribute__((packed));

/* ========== 异常表 (SEH) ========== */

struct runtime_function {
    uint32_t BeginAddress;          /* 函数起始 RVA */
    uint32_t EndAddress;            /* 函数结束 RVA */
    uint32_t UnwindInfoAddress;     /* 展开信息 RVA */
} __attribute__((packed));

struct unwind_info {
    uint8_t Version : 3;            /* 版本 */
    uint8_t Flags : 5;              /* 标志 */
    uint8_t SizeOfProlog;           /* 序言大小 */
    uint8_t CountOfUnwindCodes;     /* 展开代码数 */
    uint8_t FrameRegister : 4;      /* 帧寄存器 */
    uint8_t FrameOffset : 4;        /* 帧偏移 */
    uint16_t UnwindCode[1];         /* 展开代码数组 */
} __attribute__((packed));

/* 展开标志 */
#define UNW_FLAG_NHANDLER   0x0
#define UNW_FLAG_EHANDLER   0x1
#define UNW_FLAG_UHANDLER   0x2
#define UNW_FLAG_CHAININFO  0x4

/* ========== 加载配置 ========== */

struct load_config {
    uint32_t Size;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint32_t GlobalFlagsClear;
    uint32_t GlobalFlagsSet;
    uint32_t CriticalSectionDefaultTimeout;
    uint32_t DeCommitFreeBlockThreshold;
    uint32_t DeCommitTotalFreeThreshold;
    uint32_t LockPrefixTable;
    uint32_t MaximumAllocationSize;
    uint32_t VirtualMemoryThreshold;
    uint32_t ProcessAffinityMask;
    uint32_t ProcessHeapFlags;
    uint16_t CSDVersion;
    uint16_t Reserved1;
    uint32_t EditList;
    uint64_t SecurityCookie;
    uint64_t SEHandlerTable;
    uint64_t SEHandlerCount;
    uint64_t GuardCFCheckFunctionPointer;
    uint64_t GuardCFDispatchFunctionPointer;
    uint64_t GuardCFFunctionTable;
    uint64_t GuardCFFunctionCount;
    uint32_t GuardFlags;
} __attribute__((packed));

/* ========== PE 加载结果 ========== */

struct pe_module {
    /* 镜像信息 */
    uint64_t image_base;            /* 实际加载基址 */
    uint64_t preferred_base;        /* 首选基址 */
    uint64_t image_size;            /* 镜像大小 */
    uint64_t entry_point;           /* 入口点 */
    
    /* 文件信息 */
    const char *path;               /* 文件路径 */
    void *file_data;                /* 原始文件数据 */
    size_t file_size;               /* 文件大小 */
    
    /* PE 头 */
    struct dos_hdr *dos_hdr;
    struct coff_hdr *coff_hdr;
    struct pe_opt_hdr *opt_hdr;
    struct section_hdr *sections;
    int section_count;
    
    /* 数据目录 */
    struct data_dir *data_dirs;
    int data_dir_count;
    
    /* 导入信息 */
    struct import_dir *import_dir;
    int import_count;
    int imports_resolved;           /* 导入是否已解析 */
    
    /* 导出信息 */
    struct export_dir *export_dir;
    
    /* 重定位信息 */
    struct reloc_block *reloc_dir;
    int reloc_processed;            /* 重定位是否已处理 */
    
    /* TLS 信息 */
    struct tls_dir *tls_dir;
    
    /* 异常信息 */
    struct runtime_function *exception_dir;
    int exception_count;
    
    /* 加载配置 */
    struct load_config *load_config;
    
    /* 引用计数 */
    int ref_count;

    /* 模块句柄 */
    void *handle;

    /* 用户空间 thunk 页（调用约定转换） */
    uint64_t thunk_page;           /* thunk 代码页虚拟地址 */
    uint64_t thunk_page_used;      /* 已使用的偏移量 */

    /* 链表 */
    struct list_head list;
};

/* ========== PE 加载器接口 ========== */

/* 初始化 PE 加载器 */
int pe_init(void);

/* 验证 PE 文件 */
int pe_validate(const void *data, size_t size);

/* 检测 PE 是 32 位还是 64 位 */
int pe_is_64bit(const void *data);

/* 加载 PE 模块 (完整流程) */
struct pe_module *pe_load_module(const void *data, size_t size, const char *path);

/* 从文件加载 PE 模块 */
struct pe_module *pe_load_file(const char *path);

/* 映射 PE 节到内存 */
int pe_map_sections(struct pe_module *mod);

/* 处理重定位 */
int pe_process_relocations(struct pe_module *mod);

/* 处理导入表 */
int pe_process_imports(struct pe_module *mod);

/* 查找导出函数 */
void *pe_get_export(struct pe_module *mod, const char *name);
void *pe_get_export_ordinal(struct pe_module *mod, uint32_t ordinal);

/* 卸载 PE 模块 */
int pe_unload_module(struct pe_module *mod);

/* RVA 转 VA */
static inline void *pe_rva_to_va(struct pe_module *mod, uint32_t rva) {
    return (void *)(mod->image_base + rva);
}

/* VA 转 RVA */
static inline uint32_t pe_va_to_rva(struct pe_module *mod, void *va) {
    return (uint32_t)((uint64_t)va - mod->image_base);
}

/* ========== 模块管理 ========== */

/* 查找已加载模块 */
struct pe_module *pe_find_module(const char *name);

/* 获取模块句柄 */
void *pe_get_module_handle(const char *name);

/* 获取模块文件名 */
int pe_get_module_filename(void *handle, char *buf, size_t size);

/* ========== 节操作 ========== */

/* 查找包含 RVA 的节 */
struct section_hdr *pe_find_section(struct pe_module *mod, uint32_t rva);

/* 获取节保护属性 */
uint32_t pe_get_section_protection(struct section_hdr *sec);

/* ========== DLL 导入/导出 ========== */

/* 解析 DLL 导入 */
int pe_resolve_import(struct pe_module *mod, const char *dll_name,
                      const char *func_name, void **addr);

/* 注册内置 DLL */
int pe_register_builtin_dll(const char *name, void *exports);

/* ========== SEH 异常处理 ========== */

/* 注册异常表 */
int pe_register_exception_table(struct pe_module *mod);

/* 在 PE 异常表中查找给定 RIP 的运行时函数 */
struct runtime_function *pe_find_runtime_function(struct pe_module *mod, uint64_t rip);

/* 展开 SEH 帧
 * 返回: -1 未找到函数, 0 无处理器继续展开, 1 找到异常处理器
 */
int pe_unwind_exception(struct pe_module *mod, uint64_t rip,
                        uint32_t exception_code, uint64_t *rsp_ptr, uint64_t *rip_ptr);

/* ========== TLS 支持 ========== */

/* 初始化线程 TLS */
int pe_init_thread_tls(struct pe_module *mod, void **tls_index);

/* 获取 TLS 数据 */
void *pe_get_tls_data(struct pe_module *mod, int tls_index);

#endif /* PE_H */