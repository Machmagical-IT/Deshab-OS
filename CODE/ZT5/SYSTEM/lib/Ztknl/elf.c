#include "elf.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "console.h"
#include "string.h"
#include "abi.h"
#include "task.h"
#include "fb.h"

/* 用户栈配置 */
#define USER_STACK_TOP      0x00007FFFFFE00000ULL  /* 用户栈顶，留出空间避免边界问题 */
#define USER_STACK_SIZE     (8 * 1024 * 1024)     /* 8 MB */
#define USER_ARGV_MAX       16

int elf_validate(const void *data, size_t size) {
    if (data == NULL || size < sizeof(struct elf64_hdr)) {
        return -1;
    }

    struct elf64_hdr *hdr = (struct elf64_hdr *)data;

    /* 检查魔数 */
    if (hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' ||
        hdr->e_ident[2] != 'L' || hdr->e_ident[3] != 'F') {
        return -1;
    }

    /* 检查 64 位 */
    if (hdr->e_ident[4] != 2) {  /* ELFCLASS64 */
        return -1;
    }

    /* 检查小端 */
    if (hdr->e_ident[5] != 1) {  /* ELFDATA2LSB */
        return -1;
    }

    /* 检查机器类型 */
    if (hdr->e_machine != ELF_MACHINE_X86_64) {
        return -1;
    }

    /* 检查可执行类型 */
    if (hdr->e_type != ELF_TYPE_EXEC) {
        return -1;
    }

    return 0;
}

int elf_load(const void *data, size_t size, struct process *proc,
             struct elf_load_result *result) {
    /* 用于跟踪已分配的资源，便于错误时清理 */
    uint64_t *allocated_pages = NULL;
    size_t allocated_count = 0;
    size_t allocated_capacity = 0;
    
    if (elf_validate(data, size) != 0) {
        if (result) result->error = -1;
        return -1;
    }

    struct elf64_hdr *hdr = (struct elf64_hdr *)data;

    /* 检查程序头 */
    if (hdr->e_phoff + hdr->e_phnum * hdr->e_phentsize > size) {
        // println("[ELF] Error: Invalid program headers");
        if (result) result->error = -2;
        return -1;
    }
    
    /* 整数溢出检查 */
    if (hdr->e_phnum > 65536 / hdr->e_phentsize) {
        // println("[ELF] Error: Program header count overflow");
        if (result) result->error = -2;
        return -1;
    }

    /* 预分配页面跟踪数组 */
    allocated_capacity = 1024;  /* 初始容量 */
    allocated_pages = kmalloc(allocated_capacity * sizeof(uint64_t));
    if (allocated_pages == NULL) {
        if (result) result->error = -4;
        return -1;
    }

    /* 加载程序段 */
    struct elf64_phdr *phdr = (struct elf64_phdr *)((uint8_t *)data + hdr->e_phoff);

    for (int i = 0; i < hdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) {
            continue;
        }

        if (phdr[i].p_filesz > phdr[i].p_memsz) {
            if (result) result->error = -3;
            goto cleanup;
        }
        
        /* 检查偏移是否超出文件大小 */
        if (phdr[i].p_offset + phdr[i].p_filesz > size) {
            // println("[ELF] Error: Segment extends beyond file");
            if (result) result->error = -3;
            goto cleanup;
        }

        /* 计算需要的页数 */
        uint64_t vaddr_start = phdr[i].p_vaddr & ~(PAGE_SIZE - 1);
        uint64_t vaddr_end = phdr[i].p_vaddr + phdr[i].p_memsz;
        vaddr_end = (vaddr_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        size_t pages = (vaddr_end - vaddr_start) / PAGE_SIZE;

        /* 检查地址是否在用户空间范围内 */
        if (vaddr_end > 0x0000800000000000ULL) {
            // println("[ELF] Error: Segment address exceeds user space");
            if (result) result->error = -3;
            goto cleanup;
        }

        /* 设置页权限 */
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (phdr[i].p_flags & PF_W) {
            flags |= VMM_FLAG_WRITABLE;
        }
        if (!(phdr[i].p_flags & PF_X)) {
            flags |= VMM_FLAG_NX;
        }

        /* 分配并映射页面 */
        for (size_t j = 0; j < pages; j++) {
            uint64_t vaddr = vaddr_start + j * PAGE_SIZE;
            uint64_t phys = pmm_alloc_page();

            if (phys == 0) {
                if (result) result->error = -4;
                goto cleanup;
            }
            
            /* 记录分配的页面 */
            if (allocated_count >= allocated_capacity) {
                /* 扩展数组 */
                size_t new_capacity = allocated_capacity * 2;
                uint64_t *new_array = krealloc(allocated_pages, new_capacity * sizeof(uint64_t));
                if (new_array == NULL) {
                    pmm_free_page(phys);
                    if (result) result->error = -4;
                    goto cleanup;
                }
                allocated_pages = new_array;
                allocated_capacity = new_capacity;
            }
            allocated_pages[allocated_count++] = phys;

            void *page = phys_to_virt(phys);
            
            /* 安全检查：page 是否有效且在内核空间 */
            if (page == NULL || (uint64_t)page < 0xFFFF800000000000ULL) {
                if (result) result->error = -4;
                goto cleanup;
            }

            /* 清零 */
            for (size_t k = 0; k < PAGE_SIZE; k++) {
                ((uint8_t *)page)[k] = 0;
            }

            /* 复制文件内容 */
            uint64_t file_start = phdr[i].p_offset;
            uint64_t file_end = file_start + phdr[i].p_filesz;
            uint64_t page_offset = j * PAGE_SIZE;
            uint64_t seg_offset = phdr[i].p_vaddr - vaddr_start;

            if (page_offset >= seg_offset && page_offset < seg_offset + phdr[i].p_memsz) {
                uint64_t copy_start = 0;
                uint64_t copy_end = PAGE_SIZE;

                /* 计算复制范围 */
                if (page_offset < seg_offset) {
                    copy_start = seg_offset - page_offset;
                }
                if (page_offset + PAGE_SIZE > seg_offset + phdr[i].p_memsz) {
                    copy_end = seg_offset + phdr[i].p_memsz - page_offset;
                }

                /* 从文件复制 */
                /* 计算文件偏移：p_offset + (页面内起始位置在段内的偏移) */
                uint64_t src_offset;
                if (page_offset >= seg_offset) {
                    /* 页面开始位置在段内 */
                    src_offset = file_start + (page_offset - seg_offset);
                } else {
                    /* 页面开始位置在段之前，段数据从页面中间开始 */
                    src_offset = file_start;
                }
                
                if (src_offset < file_end) {
                    uint64_t copy_size = copy_end - copy_start;
                    if (src_offset + copy_size > file_end) {
                        copy_size = file_end - src_offset;
                    }
                    
                    /* 复制数据：使用 memcpy */
                    memcpy((uint8_t *)page + copy_start,
                           (uint8_t *)data + src_offset,
                           copy_size);
                }
            }

            /* 映射页面 */
            if (vmm_map_page(proc->space, vaddr, phys, flags) != 0) {
                if (result) result->error = -5;
                goto cleanup;
            }
        }
    }

    /* 分配用户栈 */
    uint64_t stack_top = USER_STACK_TOP;
    uint64_t stack_bottom = stack_top - USER_STACK_SIZE;
    stack_bottom &= ~(PAGE_SIZE - 1);

    size_t stack_pages = USER_STACK_SIZE / PAGE_SIZE;
    for (size_t i = 0; i < stack_pages; i++) {
        uint64_t vaddr = stack_bottom + i * PAGE_SIZE;
        uint64_t phys = pmm_alloc_page();

        if (phys == 0) {
            if (result) result->error = -6;
            goto cleanup;
        }
        
        /* 记录分配的页面 */
        if (allocated_count >= allocated_capacity) {
            size_t new_capacity = allocated_capacity * 2;
            uint64_t *new_array = krealloc(allocated_pages, new_capacity * sizeof(uint64_t));
            if (new_array == NULL) {
                pmm_free_page(phys);
                if (result) result->error = -6;
                goto cleanup;
            }
            allocated_pages = new_array;
            allocated_capacity = new_capacity;
        }
        allocated_pages[allocated_count++] = phys;

        /* 清零 */
        void *page = phys_to_virt(phys);
        for (size_t j = 0; j < PAGE_SIZE; j++) {
            ((uint8_t *)page)[j] = 0;
        }

        /* 映射用户栈 */
        if (vmm_map_page(proc->space, vaddr, phys,
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER | VMM_FLAG_NX) != 0) {
            if (result) result->error = -7;
            goto cleanup;
        }
    }

    /* 成功，释放跟踪数组但保留页面 */
    kfree(allocated_pages);

    if (result) {
        result->entry = hdr->e_entry;
        result->stack_top = stack_top;
        result->error = 0;
    }

    return 0;

cleanup:
    /* 清理已分配的页面 */
    for (size_t i = 0; i < allocated_count; i++) {
        pmm_free_page(allocated_pages[i]);
    }
    kfree(allocated_pages);
    return -1;
}

int elf_load_file(const char *path, struct process **proc_out, uint64_t *entry_out) {
    /* 读取文件 */
    size_t size;
    void *data = vfs_read_file(path, &size);
    if (data == NULL) {
        return -1;
    }

    /* 检测 ABI 类型 */
    abi_type_t abi = abi_detect_from_elf((const uint8_t *)data);

    /* 创建进程 */
    struct process *proc = process_create(path, 0);
    if (proc == NULL) {
        kfree(data);
        return -1;
    }

    /* 设置进程 ABI */
    proc->abi = abi;

    /* 加载 ELF */
    struct elf_load_result result;
    if (elf_load(data, size, proc, &result) != 0) {
        process_destroy(proc);
        kfree(data);
        return -1;
    }

    kfree(data);

    /* 创建主线程 */
    struct thread *thread = thread_create(proc, "main", result.entry, NULL);
    if (thread == NULL) {
        process_destroy(proc);
        return -1;
    }

    /* 设置用户栈 */
    thread->user_stack = result.stack_top;
    thread->stack_size = USER_STACK_SIZE;

    /* 在用户栈上设置 argc 和 argv
     * 栈布局 (从高地址到低地址):
     *   stack_top
     *   ...
     *   argv字符串区域
     *   ...
     *   NULL (argv 终止符)
     *   argv[0] 指针
     *   argc
     *   <- RSP (16字节对齐)
     */
    
    /* 从路径提取程序名 */
    const char *prog_name = path;
    const char *slash = path;
    while (*slash) {
        if (*slash == '/') prog_name = slash + 1;
        slash++;
    }
    
    /* 在用户栈上设置 argc 和 argv
     * x86-64 System V ABI 栈布局 (RSP 指向 argc):
     *   高地址
     *   ... (alignment padding)
     *   envp[0] (NULL)
     *   argv[1] (NULL)
     *   argv[0] pointer -> points to string data below
     *   argc = 1
     *   string data "init\0"
     *   低地址 <- RSP after program pushes data
     */
    
    extern uint64_t vmm_get_physical(struct address_space *space, uint64_t virt);
    extern void *phys_to_virt(uint64_t phys);
    
    /* 从栈顶向下分配空间:
     * stack_top - 16:  对齐填充
     * stack_top - 24:  envp终止符(NULL)
     * stack_top - 32:  argv终止符(NULL)  
     * stack_top - 40:  argv[0]指针
     * stack_top - 48:  argc
     * stack_top - 64:  字符串区域 (程序名)
     */
    
    /* 首先计算字符串区域 - 放在更低地址 */
    uint64_t string_area = result.stack_top - 128;  /* 留足够空间 */
    uint64_t argv0_addr = string_area;
    
    /* 计算 argc/argv/envp 区域 - 在字符串之上 */
    uint64_t metadata_area = result.stack_top - 64;  /* argc(8) + argv[0](8) + argv[1](8) + envp[0](8) + padding(32) */
    
    /* 确保 16 字节对齐 */
    metadata_area &= ~15ULL;
    
    /* 栈指针指向 argc 位置 */
    uint64_t stack_ptr = metadata_area;
    
    /* 计算各元数据地址 */
    uint64_t argc_addr = stack_ptr;
    uint64_t argv0_ptr_addr = stack_ptr + 8;
    uint64_t argv1_null_addr = stack_ptr + 16;
    uint64_t envp0_null_addr = stack_ptr + 24;
    
    /* 写入程序名字符串 */
    uint64_t argv0_phys = vmm_get_physical(proc->space, argv0_addr);
    
    if (argv0_phys) {
        char *argv0_kernel = (char *)phys_to_virt(argv0_phys);
        size_t name_len = 0;
        for (const char *p = prog_name; *p && name_len < 31; p++, name_len++) {
            argv0_kernel[name_len] = *p;
        }
        argv0_kernel[name_len] = '\0';
    }
    
    /* 写入 argc = 1 */
    uint64_t argc_phys = vmm_get_physical(proc->space, argc_addr);
    
    if (argc_phys) {
        *(uint64_t *)phys_to_virt(argc_phys) = 1;
    }
    
    /* 写入 argv[0] 指针 - 指向字符串地址 */
    uint64_t argv0_ptr_phys = vmm_get_physical(proc->space, argv0_ptr_addr);
    if (argv0_ptr_phys) {
        *(uint64_t *)phys_to_virt(argv0_ptr_phys) = argv0_addr;
    }
    
    /* 写入 argv[1] = NULL (终止符) */
    uint64_t argv1_phys = vmm_get_physical(proc->space, argv1_null_addr);
    if (argv1_phys) {
        *(uint64_t *)phys_to_virt(argv1_phys) = 0;
    }
    
    /* 写入 envp[0] = NULL (无环境变量) */
    uint64_t envp0_phys = vmm_get_physical(proc->space, envp0_null_addr);
    if (envp0_phys) {
        *(uint64_t *)phys_to_virt(envp0_phys) = 0;
    }

    /* 设置用户态上下文 */
    thread->context.cs = 0x18 | 3;       /* 用户代码段 + RPL3 = 0x1B */
    thread->context.ss = 0x20 | 3;       /* 用户数据段 + RPL3 = 0x23 */
    thread->context.user_rsp = stack_ptr;  /* 用户栈指针 */
    thread->context.rflags = 0x202;      /* IF = 1, standard user mode flags */
    
    /* 在内核栈上构建 iretq 帧 */
    /*
     * iretq 帧布局 (从 RSP 向上):
     *   RSP+0x00: RIP
     *   RSP+0x08: CS
     *   RSP+0x10: RFLAGS
     *   RSP+0x18: 用户 RSP
     *   RSP+0x20: SS
     */
    /* 注意：thread->kernel_stack 已经是栈顶地址，不需要再加 stack_size */
    uint64_t *stack_top = (uint64_t *)thread->kernel_stack;
    
    /* 构建 iretq 帧 (从高地址开始，向下生长) */
    *(--stack_top) = thread->context.ss;        /* SS = 0x23 */
    *(--stack_top) = thread->context.user_rsp;  /* 用户 RSP */
    *(--stack_top) = thread->context.rflags;    /* RFLAGS = 0x202 */
    *(--stack_top) = thread->context.cs;        /* CS = 0x1B */
    *(--stack_top) = result.entry;              /* RIP = ELF 入口点 */
    
    /* 设置内核栈指针指向 iretq 帧顶 (RIP) */
    thread->context.rsp = (uint64_t)stack_top;
    
    /* 设置返回地址为 iretq 帧中的 RIP（用户态入口） */
    /* 实际上 context.rip 在用户线程切换中不会被用到 */
    thread->context.rip = result.entry;

    if (proc_out) *proc_out = proc;
    if (entry_out) *entry_out = result.entry;

    return 0;
}

/* 从内存加载 ELF (用于 Limine 模块) */
int elf_load_from_memory_ex(void *data, size_t size, const char *prog_name, struct process **proc_out, uint64_t *entry_out) {
    if (data == NULL || size == 0) {
        return -1;
    }

    /* 检测 ABI 类型 */
    abi_type_t abi = abi_detect_from_elf((const uint8_t *)data);

    /* 创建进程 */
    struct process *proc = process_create(prog_name ? prog_name : "limine_module", 0);
    if (proc == NULL) {
        return -1;
    }

    /* 设置进程 ABI */
    proc->abi = abi;

    /* 加载 ELF */
    struct elf_load_result result;
    if (elf_load(data, size, proc, &result) != 0) {
        process_destroy(proc);
        return -1;
    }

    /* 创建主线程 */
    struct thread *thread = thread_create(proc, "main", result.entry, NULL);
    if (thread == NULL) {
        process_destroy(proc);
        return -1;
    }

    /* 设置用户栈 */
    thread->user_stack = result.stack_top;
    thread->stack_size = USER_STACK_SIZE;

    /* 在用户栈上设置 argc 和 argv
     * x86-64 System V ABI 栈布局 (RSP 指向 argc):
     *   高地址
     *   ... (alignment padding)
     *   envp[0] (NULL)
     *   argv[1] (NULL)
     *   argv[0] pointer -> points to string data below
     *   argc = 1
     *   string data
     *   低地址
     */
    
    /* 使用传入的程序名或默认值 */
    const char *name = prog_name ? prog_name : "program";
    
    extern uint64_t vmm_get_physical(struct address_space *space, uint64_t virt);
    extern void *phys_to_virt(uint64_t phys);
    
    /* 从栈顶向下分配空间 */
    uint64_t string_area = result.stack_top - 128;
    uint64_t argv0_addr = string_area;
    
    uint64_t metadata_area = result.stack_top - 64;
    metadata_area &= ~15ULL;
    
    uint64_t stack_ptr = metadata_area;
    
    uint64_t argc_addr = stack_ptr;
    uint64_t argv0_ptr_addr = stack_ptr + 8;
    uint64_t argv1_null_addr = stack_ptr + 16;
    uint64_t envp0_null_addr = stack_ptr + 24;
    
    /* 写入程序名字符串 */
    uint64_t argv0_phys = vmm_get_physical(proc->space, argv0_addr);
    
    if (argv0_phys) {
        char *argv0_kernel = (char *)phys_to_virt(argv0_phys);
        size_t name_len = 0;
        for (const char *p = name; *p && name_len < 31; p++, name_len++) {
            argv0_kernel[name_len] = *p;
        }
        argv0_kernel[name_len] = '\0';
    }
    
    /* 写入 argc = 1 */
    uint64_t argc_phys = vmm_get_physical(proc->space, argc_addr);
    if (argc_phys) {
        *(uint64_t *)phys_to_virt(argc_phys) = 1;
    }
    
    /* 写入 argv[0] 指针 */
    uint64_t argv0_ptr_phys = vmm_get_physical(proc->space, argv0_ptr_addr);
    if (argv0_ptr_phys) {
        *(uint64_t *)phys_to_virt(argv0_ptr_phys) = argv0_addr;
    }
    
    /* 写入 argv[1] = NULL */
    uint64_t argv1_phys = vmm_get_physical(proc->space, argv1_null_addr);
    if (argv1_phys) {
        *(uint64_t *)phys_to_virt(argv1_phys) = 0;
    }
    
    /* 写入 envp[0] = NULL */
    uint64_t envp0_phys = vmm_get_physical(proc->space, envp0_null_addr);
    if (envp0_phys) {
        *(uint64_t *)phys_to_virt(envp0_phys) = 0;
    }

    /* 设置用户态上下文 */
    thread->context.cs = 0x18 | 3;       /* 用户代码段 + RPL3 */
    thread->context.ss = 0x20 | 3;       /* 用户数据段 + RPL3 */
    thread->context.user_rsp = stack_ptr;  /* 用户栈指针 */
    thread->context.rflags = 0x202;      /* IF = 1 */
    
    /* 在内核栈上构建 iretq 帧 */
    /* 注意：thread->kernel_stack 已经是栈顶地址，不需要再加 stack_size */
    uint64_t *stack_top = (uint64_t *)thread->kernel_stack;
    
    /* 先清零一些空间 */
    for (int i = 0; i < 10; i++) {
        *(--stack_top) = 0;
    }
    
    /* 构建 iretq 帧 */
    *(--stack_top) = thread->context.ss;        /* SS = 0x23 */
    *(--stack_top) = thread->context.user_rsp;  /* user_rsp */
    *(--stack_top) = thread->context.rflags;    /* RFLAGS = 0x202 */
    *(--stack_top) = thread->context.cs;        /* CS = 0x1B */
    *(--stack_top) = result.entry;              /* RIP = ELF 入口点 */
    
    /* 设置内核栈指针指向 iretq 帧顶 */
    thread->context.rsp = (uint64_t)stack_top;
    thread->context.rip = result.entry;

    if (proc_out) *proc_out = proc;
    if (entry_out) *entry_out = result.entry;

    return 0;
}

/* 兼容旧接口 */
int elf_load_from_memory(void *data, size_t size, struct process **proc_out, uint64_t *entry_out) {
    return elf_load_from_memory_ex(data, size, NULL, proc_out, entry_out);
}

uint64_t elf_lookup_symbol(const void *elf_data, size_t elf_size, const char *name) {
    if (elf_data == NULL || name == NULL || elf_size < sizeof(struct elf64_hdr)) {
        return 0;
    }

    struct elf64_hdr *hdr = (struct elf64_hdr *)elf_data;

    if (hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' ||
        hdr->e_ident[2] != 'L' || hdr->e_ident[3] != 'F') {
        return 0;
    }

    if (hdr->e_shoff == 0 || hdr->e_shnum == 0) {
        return 0;
    }

    if (hdr->e_shoff + (uint64_t)hdr->e_shnum * hdr->e_shentsize > elf_size) {
        return 0;
    }

    struct elf64_shdr *shdrs = (struct elf64_shdr *)((uint8_t *)elf_data + hdr->e_shoff);

    struct elf64_shdr *symtab_sh = NULL;
    struct elf64_shdr *strtab_sh = NULL;

    for (int i = 0; i < hdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            symtab_sh = &shdrs[i];
            if (symtab_sh->sh_link < hdr->e_shnum) {
                strtab_sh = &shdrs[symtab_sh->sh_link];
            }
            break;
        }
    }

    if (symtab_sh == NULL || strtab_sh == NULL) {
        return 0;
    }

    if (symtab_sh->sh_offset + symtab_sh->sh_size > elf_size ||
        strtab_sh->sh_offset + strtab_sh->sh_size > elf_size) {
        return 0;
    }

    struct elf64_sym *syms = (struct elf64_sym *)((uint8_t *)elf_data + symtab_sh->sh_offset);
    const char *strtab = (const char *)((uint8_t *)elf_data + strtab_sh->sh_offset);
    size_t sym_count = symtab_sh->sh_size / symtab_sh->sh_entsize;

    for (size_t i = 0; i < sym_count; i++) {
        if (syms[i].st_name == 0) continue;
        if (ELF64_ST_TYPE(syms[i].st_info) == STT_SECTION) continue;

        const char *sym_name = strtab + syms[i].st_name;
        if (sym_name + strlen(name) > strtab + strtab_sh->sh_size) continue;

        if (strcmp(sym_name, name) == 0) {
            if (syms[i].st_value != 0 && syms[i].st_shndx != 0) {
                return syms[i].st_value;
            }
        }
    }

    return 0;
}

uint64_t elf_lookup_symbol_from_file(const char *path, const char *name) {
    if (path == NULL || name == NULL) return 0;

    size_t size;
    void *data = vfs_read_file(path, &size);
    if (data == NULL) return 0;

    uint64_t addr = elf_lookup_symbol(data, size, name);
    kfree(data);
    return addr;
}
