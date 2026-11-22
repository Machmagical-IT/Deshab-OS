#ifndef EXEC_H
#define EXEC_H

#include <stdint.h>
#include <stddef.h>
#include "abi.h"

/* 前向声明 */
struct pe_module;

/* 初始化 exec 子系统 */
void exec_init(void);

/* exec 创新特性标志 */
#define EXEC_FLAG_NONE          0
#define EXEC_FLAG_KEEP_ENV      (1 << 0)    /* 保留环境变量 */
#define EXEC_FLAG_KEEP_CWD      (1 << 1)    /* 保留工作目录 */
#define EXEC_FLAG_KEEP_FDS      (1 << 2)    /* 保留所有文件描述符 */
#define EXEC_FLAG_SETUID        (1 << 3)    /* 设置用户ID */
#define EXEC_FLAG_SETGID        (1 << 4)    /* 设置组ID */
#define EXEC_FLAG_NO_ASLR       (1 << 5)    /* 禁用ASLR */
#define EXEC_FLAG_TRACE         (1 << 6)    /* 启用跟踪模式 */
#define EXEC_FLAG_SANDBOX       (1 << 7)    /* 沙箱模式 */

/* ELF 解释器链 */
struct elf_interpreter {
    char path[256];             /* 解释器路径 */
    abi_type_t abi;             /* 解释器 ABI */
    uint64_t entry;             /* 入口点 */
    uint64_t phdr;              /* 程序头地址 */
    uint64_t phnum;             /* 程序头数量 */
    
    struct elf_interpreter *next;  /* 链式解释器 (如: /lib/ld-linux.so -> 程序) */
};

/* 进程镜像信息 */
struct process_image {
    /* 基本信息 */
    char path[256];             /* 可执行文件路径 */
    abi_type_t abi;             /* 检测到的 ABI */
    
    /* 内存布局 */
    uint64_t entry_point;       /* 入口点 */
    uint64_t phdr_addr;         /* 程序头地址 */
    uint64_t phent;             /* 程序头条目大小 */
    uint64_t phnum;             /* 程序头数量 */
    
    /* 栈信息 */
    uint64_t stack_top;
    uint64_t stack_size;
    
    /* 堆信息 */
    uint64_t brk_start;
    uint64_t brk_end;
    
    /* 解释器链 */
    struct elf_interpreter *interpreter;
    
    /* 参数/环境 */
    int argc;
    char **argv;
    int envc;
    char **envp;
    
    /* 辅助向量 (auxv) */
    struct {
        uint64_t type;
        uint64_t value;
    } auxv[16];
    int auxv_count;

    /* PE 模块 (Windows 进程) */
    struct pe_module *pe_mod;
};

/* 辅助向量类型 (Linux 兼容) */
#define AT_NULL     0
#define AT_IGNORE   1
#define AT_EXECFD   2
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_CLKTCK   17
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_EXECFN   31

/* execve 创新接口 */

/* 标准 execve */
int sys_execve(const char *path, char *const argv[], char *const envp[]);

/* 扩展 execveat (支持目录fd和标志) */
int sys_execveat(int dirfd, const char *path, char *const argv[], 
                 char *const envp[], int flags);

/* fexecve (通过文件描述符执行) */
int sys_fexecve(int fd, char *const argv[], char *const envp[]);

/* === 内部函数 === */

/* 构建进程镜像 */
int build_process_image(const char *path, struct process_image *image);

/* 加载 ELF 并检测 ABI */
int load_elf_with_abi(const char *path, struct process_image *image);

/* 加载 ELF 解释器链 */
int load_interpreter_chain(struct process_image *image);

/* 构建用户栈 (参数、环境、auxv) */
uint64_t build_user_stack(struct process_image *image, uint64_t stack_top);

/* 替换进程镜像 */
int replace_process_image(struct process_image *image);

/* 清理旧进程镜像 */
void cleanup_old_image(void);

/* 处理 CLOEXEC 文件描述符 */
void handle_cloexec_fds(void);

/* 初始化 exec 子系统 */
void exec_init(void);

/* 构建辅助向量 */
int build_auxv(struct process_image *image);

#endif
