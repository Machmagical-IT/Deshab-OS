#include "exec.h"
#include "console.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "task.h"
#include "elf.h"
#include "pe.h"
#include "abi.h"
#include "ipc.h"
#include "heap.h"
#include "string.h"
#include "ntdll.h"

/* 用户栈配置 */
#define USER_STACK_TOP      0x00007FFFFFFFFFFFULL
#define USER_STACK_SIZE     (8 * 1024 * 1024)    /* 8 MB */
#define USER_ARG_MAX        256
#define USER_ENV_MAX        256

/* 初始化 exec 子系统 */
void exec_init(void) {
}

/* 构建辅助向量 */
int build_auxv(struct process_image *image) {
    if (image == NULL) return -1;
    
    int idx = 0;
    
    /* 程序头信息 */
    image->auxv[idx].type = AT_PHDR;
    image->auxv[idx].value = image->phdr_addr;
    idx++;
    
    image->auxv[idx].type = AT_PHENT;
    image->auxv[idx].value = image->phent;
    idx++;
    
    image->auxv[idx].type = AT_PHNUM;
    image->auxv[idx].value = image->phnum;
    idx++;
    
    /* 页大小 */
    image->auxv[idx].type = AT_PAGESZ;
    image->auxv[idx].value = PAGE_SIZE;
    idx++;
    
    /* 入口点 */
    image->auxv[idx].type = AT_ENTRY;
    image->auxv[idx].value = image->entry_point;
    idx++;
    
    /* 解释器基址 */
    if (image->interpreter != NULL) {
        image->auxv[idx].type = AT_BASE;
        image->auxv[idx].value = image->interpreter->entry;  /* 简化 */
        idx++;
    }
    
    /* 用户/组 ID (简化) */
    image->auxv[idx].type = AT_UID;
    image->auxv[idx].value = 0;
    idx++;
    
    image->auxv[idx].type = AT_EUID;
    image->auxv[idx].value = 0;
    idx++;
    
    image->auxv[idx].type = AT_GID;
    image->auxv[idx].value = 0;
    idx++;
    
    image->auxv[idx].type = AT_EGID;
    image->auxv[idx].value = 0;
    idx++;
    
    /* 时钟频率 */
    image->auxv[idx].type = AT_CLKTCK;
    image->auxv[idx].value = 100;
    idx++;
    
    /* 平台标识 */
    image->auxv[idx].type = AT_PLATFORM;
    image->auxv[idx].value = (uint64_t)"x86_64";
    idx++;
    
    /* 随数 (用于安全) */
    static uint64_t random_val = 0xDEADBEEFCAFEBABEULL;
    image->auxv[idx].type = AT_RANDOM;
    image->auxv[idx].value = (uint64_t)&random_val;
    idx++;
    
    /* 执行文件名 */
    image->auxv[idx].type = AT_EXECFN;
    image->auxv[idx].value = (uint64_t)image->path;
    idx++;
    
    /* 安全标志 */
    image->auxv[idx].type = AT_SECURE;
    image->auxv[idx].value = 0;
    idx++;
    
    /* 结束标记 */
    image->auxv[idx].type = AT_NULL;
    image->auxv[idx].value = 0;
    idx++;
    
    image->auxv_count = idx;
    
    return 0;
}

/* 在用户栈上压入字符串，返回指针 */
static uint64_t push_string(uint64_t *stack_top, const char *str) {
    if (str == NULL || stack_top == NULL) return 0;
    
    size_t len = 0;
    while (str[len]) len++;
    len++;  /* 包含 null 终止符 */
    
    /* 对齐 */
    *stack_top -= len;
    *stack_top &= ~0xF;  /* 16字节对齐 */
    
    /* 安全写入字符串到用户栈空间 */
    memcpy((void *)*stack_top, str, len);
    
    return *stack_top;
}

/* 构建用户栈 */
uint64_t build_user_stack(struct process_image *image, uint64_t stack_top) {
    if (image == NULL) return 0;
    
    uint64_t sp = stack_top;
    
    /* 1. 压入字符串 (参数和环境变量) */
    uint64_t argv_ptrs[USER_ARG_MAX];
    uint64_t envp_ptrs[USER_ENV_MAX];
    
    /* 压入参数字符串 */
    for (int i = image->argc - 1; i >= 0; i--) {
        if (image->argv[i] != NULL) {
            argv_ptrs[i] = push_string(&sp, image->argv[i]);
        } else {
            argv_ptrs[i] = 0;
        }
    }
    
    /* 压入环境变量字符串 */
    for (int i = image->envc - 1; i >= 0; i--) {
        if (image->envp[i] != NULL) {
            envp_ptrs[i] = push_string(&sp, image->envp[i]);
        } else {
            envp_ptrs[i] = 0;
        }
    }
    
    /* 2. 压入辅助向量 (倒序) */
    for (int i = image->auxv_count - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64_t *)sp = image->auxv[i].value;
        sp -= 8;
        *(uint64_t *)sp = image->auxv[i].type;
    }
    
    /* 3. 16字节对齐 */
    sp &= ~0xF;
    
    /* 4. 压入 NULL 终止符 */
    sp -= 8;
    *(uint64_t *)sp = 0;  /* envp 终止符 */
    
    /* 5. 压入环境变量指针 (倒序) */
    for (int i = image->envc - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64_t *)sp = envp_ptrs[i];
    }
    
    /* 6. 压入 NULL 终止符 */
    sp -= 8;
    *(uint64_t *)sp = 0;  /* argv 终止符 */
    
    /* 7. 压入参数指针 (倒序) */
    for (int i = image->argc - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64_t *)sp = argv_ptrs[i];
    }
    
    /* 8. 压入 argc */
    sp -= 8;
    *(uint64_t *)sp = image->argc;
    
    return sp;
}

/* 处理 CLOEXEC 文件描述符 */
void handle_cloexec_fds(void) {
    struct process *proc = process_get_current();
    if (proc == NULL || proc->fd_table == NULL) return;
    
    /* 遍历关闭所有带 CLOEXEC 标志的 fd */
    for (int i = 0; i < MAX_FDS_PER_PROCESS; i++) {
        struct fd_entry *entry = fd_get(proc->fd_table, i);
        if (entry != NULL) {
            /* FD_CLOEXEC 标志通常是 flags 的第 0 位 */
            if (entry->flags & 0x80000000) {  /* 假设 CLOEXEC 标志 */
                fd_free(proc->fd_table, i);
            }
        }
    }
}

/* 加载 ELF 并检测 ABI */
int load_elf_with_abi(const char *path, struct process_image *image) {
    if (path == NULL || image == NULL) return -1;
    
    /* 读取文件 */
    size_t size;
    void *data = vfs_read_file(path, &size);
    if (data == NULL) {
        print("[EXEC] Failed to read: ");
        println(path);
        return -1;
    }
    
    /* 验证 ELF */
    if (elf_validate(data, size) != 0) {
        kfree(data);
        return -1;
    }
    
    /* 检测 ABI */
    image->abi = abi_detect_from_elf((const uint8_t *)data);
    print("[EXEC] Detected ABI: ");
    println(abi_get_name(image->abi));
    
    /* 复制路径 */
    int i = 0;
    while (path[i] && i < 255) {
        image->path[i] = path[i];
        i++;
    }
    image->path[i] = '\0';
    
    /* 获取 ELF 头信息 */
    struct elf64_hdr *hdr = (struct elf64_hdr *)data;
    image->entry_point = hdr->e_entry;
    image->phent = hdr->e_phentsize;
    image->phnum = hdr->e_phnum;
    
    /* 检查是否有解释器 (动态链接) */
    struct elf64_phdr *phdr = (struct elf64_phdr *)((uint8_t *)data + hdr->e_phoff);
    for (int j = 0; j < hdr->e_phnum; j++) {
        if (phdr[j].p_type == PT_INTERP) {
            /* 有解释器 - 这是动态链接程序 */
            print("[EXEC] Dynamic executable, interpreter needed");
            /* TODO: 加载解释器 */
            image->interpreter = NULL;  /* 简化 */
        }
    }
    
    kfree(data);
    
    return 0;
}

/* 构建进程镜像 */
int build_process_image(const char *path, struct process_image *image) {
    if (path == NULL || image == NULL) return -1;
    
    /* 初始化 */
    for (int i = 0; i < 256; i++) image->path[i] = 0;
    image->abi = ABI_UNKNOWN;
    image->entry_point = 0;
    image->phdr_addr = 0;
    image->phent = 0;
    image->phnum = 0;
    image->stack_top = USER_STACK_TOP;
    image->stack_size = USER_STACK_SIZE;
    image->brk_start = 0;
    image->brk_end = 0;
    image->interpreter = NULL;
    image->argc = 0;
    image->argv = NULL;
    image->envc = 0;
    image->envp = NULL;
    image->auxv_count = 0;
    image->pe_mod = NULL;
    
    /* 读取文件头检测格式 */
    size_t file_size;
    void *data = vfs_read_file(path, &file_size);
    if (data == NULL) {
        print("[EXEC] Failed to read: ");
        println(path);
        return -1;
    }
    
    /* 检测文件格式 */
    int is_pe = 0;
    int is_elf = 0;
    
    /* 检查 ELF 签名 */
    if (file_size >= 4 && ((uint8_t*)data)[0] == 0x7f && ((uint8_t*)data)[1] == 'E' &&
        ((uint8_t*)data)[2] == 'L' && ((uint8_t*)data)[3] == 'F') {
        is_elf = 1;
    }
    
    /* 检查 PE 签名 (MZ -> PE) */
    if (file_size >= sizeof(struct dos_hdr) + 4) {
        struct dos_hdr *dos = (struct dos_hdr *)data;
        if (dos->e_magic == DOS_SIGNATURE) {
            uint32_t pe_offset = dos->e_lfanew;
            if (pe_offset + 4 <= file_size) {
                uint32_t pe_sig = *(uint32_t*)((uint8_t*)data + pe_offset);
                if (pe_sig == PE_SIGNATURE) {
                    is_pe = 1;
                }
            }
        }
    }
    
    kfree(data);
    
    /* 根据格式加载 */
    if (is_elf) {
        print("[EXEC] Loading ELF executable: ");
        println(path);
        return load_elf_with_abi(path, image);
    } else if (is_pe) {
        print("[EXEC] Loading PE executable: ");
        println(path);
        
        /* 使用 PE 加载器 */
        struct pe_module *mod = pe_load_file(path);
        if (mod == NULL) {
            print("[EXEC] PE load failed: ");
            println(path);
            return -1;
        }
        
        /* 设置进程镜像 */
        int i = 0;
        while (path[i] && i < 255) {
            image->path[i] = path[i];
            i++;
        }
        image->path[i] = '\0';
        
        image->entry_point = mod->entry_point;
        image->abi = ABI_WINDOWS;
        image->pe_mod = mod;

        print("[EXEC] PE loaded successfully\n");
        
        return 0;
    } else {
        print("[EXEC] Unknown executable format: ");
        println(path);
        return -1;
    }
}

/* 替换进程镜像 */
int replace_process_image(struct process_image *image) {
    if (image == NULL) return -1;
    
    struct process *proc = process_get_current();
    if (proc == NULL) return -1;
    
    print("[EXEC] Replacing process image: ");
    println(image->path);
    
    /* 1. 清理旧的地址空间 (保留页表结构) */
    /* TODO: 遍历用户空间并释放 */
    
    /* 2. 更新进程 ABI */
    proc->abi = image->abi;
    print("[EXEC] Process ABI set to: ");
    println(abi_get_name(image->abi));

    /* 2.5 关联 PE 模块（Windows 进程） */
    if (image->pe_mod != NULL) {
        proc->pe_mod = image->pe_mod;
        print("[EXEC] PE module associated with process\n");
    }
    
    /* 3. 更新进程名 */
    for (int i = 0; image->path[i] && i < 63; i++) {
        proc->name[i] = image->path[i];
    }
    proc->name[63] = '\0';
    
    /* 4. 处理 CLOEXEC */
    handle_cloexec_fds();
    
    /* 5. 构建辅助向量 */
    build_auxv(image);

    /* 6. 构建用户栈 */
    uint64_t new_sp;

    if (image->abi == ABI_WINDOWS) {
        /* Windows PE 进程：初始化 TEB/PEB，构建 Windows 风格栈帧 */
        pe_init_teb_peb(image->entry_point - (image->pe_mod ? ((struct pe_module *)image->pe_mod)->opt_hdr->AddressOfEntryPoint : 0),
                        image->stack_top, image->stack_size, image->path);

        /* Windows 入口点调用约定：RCX = PEB 指针（exe），或 HINSTANCE（DLL） */
        /* 栈帧：Windows CRT 期望对齐的栈，不需要 argc/argv/auxv */
        new_sp = image->stack_top;
        new_sp &= ~0xF;  /* 16 字节对齐 */

        /* 在栈上放置返回地址 0（入口点不应返回） */
        new_sp -= 8;
        *(uint64_t *)new_sp = 0;
    } else {
        /* Linux/Zratro 进程：标准栈帧 */
        new_sp = build_user_stack(image, image->stack_top);
    }
    
    /* 7. 跳转到新程序入口 */
    struct thread *thread = thread_get_current();
    if (thread != NULL) {
        thread->context.rip = image->entry_point;
        thread->context.rsp = new_sp;
        thread->user_stack = image->stack_top;
        thread->stack_size = image->stack_size;
        
        /* 清除寄存器 */
        thread->context.rax = 0;
        thread->context.rbx = 0;
        thread->context.rdx = 0;
        thread->context.rsi = 0;
        thread->context.rbp = 0;
        thread->context.r8 = 0;
        thread->context.r9 = 0;
        thread->context.r10 = 0;
        thread->context.r11 = 0;
        thread->context.r12 = 0;
        thread->context.r13 = 0;
        thread->context.r14 = 0;
        thread->context.r15 = 0;

        if (image->abi == ABI_WINDOWS) {
            /* Windows PE 入口点调用约定：
             * RCX = PEB 指针（exe 的 main/WinMainCRTStartup）
             * gs 段寄存器指向 TEB */
            thread->context.rcx = pe_get_peb_address();
            thread->context.rdi = 0;
        } else {
            /* Linux/Zratro：RDI = argc */
            thread->context.rcx = 0;
            thread->context.rdi = image->argc;
        }
    }
    
    println("[EXEC] Image replaced successfully");
    
    return 0;
}

/* 标准 execve */
int sys_execve(const char *path, char *const argv[], char *const envp[]) {
    if (path == NULL) return -1;
    
    print("[EXEC] execve: ");
    println(path);
    
    /* 构建新镜像 */
    struct process_image image;
    if (build_process_image(path, &image) != 0) {
        return -1;
    }
    
    /* 设置参数 */
    if (argv != NULL) {
        int i = 0;
        while (argv[i] && i < USER_ARG_MAX) {
            i++;
        }
        image.argc = i;
        image.argv = (char **)argv;
    } else {
        image.argc = 1;
        static char *default_argv[2] = { (char*)"", NULL };
        image.argv = default_argv;
    }
    
    /* 设置环境变量 */
    if (envp != NULL) {
        int i = 0;
        while (envp[i] && i < USER_ENV_MAX) {
            i++;
        }
        image.envc = i;
        image.envp = (char **)envp;
    } else {
        static char *default_envp[1] = { NULL };
        image.envc = 0;
        image.envp = default_envp;
    }
    
    /* 替换镜像 */
    if (replace_process_image(&image) != 0) {
        return -1;
    }
    
    /* 不会返回到这里 - 新程序已加载 */
    return 0;
}

/* execveat */
int sys_execveat(int dirfd, const char *path, char *const argv[], 
                 char *const envp[], int flags) {
    (void)dirfd;
    (void)flags;
    
    /* 简化实现 - 忽略 dirfd 和 flags */
    return sys_execve(path, argv, envp);
}

/* fexecve */
int sys_fexecve(int fd, char *const argv[], char *const envp[]) {
    /* 通过 fd 执行 - 需要从 fd 获取文件路径 */
    (void)fd;
    (void)argv;
    (void)envp;
    
    println("[EXEC] fexecve: not implemented");
    return -1;
}
