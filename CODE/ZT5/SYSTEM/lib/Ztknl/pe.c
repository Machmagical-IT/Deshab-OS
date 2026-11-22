/*
 * Zratro OS - PE/COFF Loader Implementation
 * 
 * 完整的 Windows PE32+ 格式加载支持
 */

#include "pe.h"
#include "console.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"
#include "task.h"
#include "ntdll.h"

/* ========== 内置 DLL 导出表 ========== */

struct builtin_export {
    const char *name;
    void *func;
    uint16_t ordinal;
};

struct builtin_dll {
    const char *name;
    struct builtin_export *exports;
    int export_count;
    struct pe_module module;
};

/* ========== 调用约定转换 Thunk 声明 ========== */

/*
 * Windows x64 调用约定 (RCX,RDX,R8,R9) -> System V (RDI,RSI,RDX,RCX)
 * 这些 thunk 在 win_thunk.S 中实现，用于 IAT 填充时转换调用约定。
 * 使用方式：将实现函数地址加载到 RAX，然后调用对应的 thunk。
 */
extern void win_thunk_0args(void);
extern void win_thunk_1arg(void);
extern void win_thunk_2args(void);
extern void win_thunk_3args(void);
extern void win_thunk_4args(void);
extern void win_thunk_5args(void);
extern void win_thunk_6args(void);
extern void win_thunk_7args(void);

/* ========== 用户空间 Thunk 生成 ========== */

/*
 * 在用户空间生成调用约定转换 thunk。
 * 每个 thunk 包含：
 *   1. 寄存器重映射 (Windows x64 -> System V x64)
 *   2. 跳转到内核实现函数
 *
 * 生成的机器码示例 (4 参数):
 *   mov rdi, rcx       ; 48 89 CF
 *   mov rsi, rdx       ; 48 89 D6
 *   mov rdx, r8        ; 4C 89 C2
 *   mov rcx, r9        ; 4D 89 C9 -> 49 89 C1 (mov rcx, r9 = 49 89 C1)
 *   mov rax, <addr>    ; 48 B8 <8 bytes>
 *   jmp rax            ; FF E0
 *
 * 注意：Higher Half 内核地址映射对用户态可见（内核空间在 0xffffffff80000000 以上，
 * 用户态页表包含内核映射），因此用户态 PE 程序可以直接跳转到内核函数地址。
 */

/* 参数数量推断表 - 常见 Windows API 函数参数数量 */
static int pe_infer_arg_count(const char *func_name) {
    if (func_name == NULL) return 4; /* 默认 4 参数 */

    /* 0 参数 */
    if (strcmp(func_name, "GetCurrentProcessId") == 0 ||
        strcmp(func_name, "GetCurrentThreadId") == 0 ||
        strcmp(func_name, "GetCurrentProcess") == 0 ||
        strcmp(func_name, "GetCurrentThread") == 0 ||
        strcmp(func_name, "GetLastError") == 0 ||
        strcmp(func_name, "IsDebuggerPresent") == 0 ||
        strcmp(func_name, "GetDesktopWindow") == 0 ||
        strcmp(func_name, "GetFocus") == 0 ||
        strcmp(func_name, "GetActiveWindow") == 0 ||
        strcmp(func_name, "WSACleanup") == 0 ||
        strcmp(func_name, "WSAGetLastError") == 0) return 0;

    /* 1 参数 */
    if (strcmp(func_name, "CloseHandle") == 0 ||
        strcmp(func_name, "ExitProcess") == 0 ||
        strcmp(func_name, "ExitThread") == 0 ||
        strcmp(func_name, "SetLastError") == 0 ||
        strcmp(func_name, "ShowWindow") == 0 ||
        strcmp(func_name, "UpdateWindow") == 0 ||
        strcmp(func_name, "DestroyWindow") == 0 ||
        strcmp(func_name, "GetDC") == 0 ||
        strcmp(func_name, "ReleaseDC") == 0 ||
        strcmp(func_name, "DeleteObject") == 0 ||
        strcmp(func_name, "DeleteDC") == 0 ||
        strcmp(func_name, "FreeLibrary") == 0 ||
        strcmp(func_name, "Sleep") == 0 ||
        strcmp(func_name, "SetEvent") == 0 ||
        strcmp(func_name, "ResetEvent") == 0 ||
        strcmp(func_name, "ReleaseMutex") == 0 ||
        strcmp(func_name, "ResumeThread") == 0 ||
        strcmp(func_name, "SuspendThread") == 0 ||
        strcmp(func_name, "GetModuleHandleA") == 0 ||
        strcmp(func_name, "GetModuleHandleW") == 0 ||
        strcmp(func_name, "closesocket") == 0 ||
        strcmp(func_name, "listen") == 0 ||
        strcmp(func_name, "PostQuitMessage") == 0 ||
        strcmp(func_name, "ValidateRect") == 0 ||
        strcmp(func_name, "KillTimer") == 0 ||
        strcmp(func_name, "SetFocus") == 0 ||
        strcmp(func_name, "SetForegroundWindow") == 0 ||
        strcmp(func_name, "GetForegroundWindow") == 0 ||
        strcmp(func_name, "IsWindowEnabled") == 0 ||
        strcmp(func_name, "IsWindowVisible") == 0 ||
        strcmp(func_name, "IsWindow") == 0 ||
        strcmp(func_name, "GetParent") == 0 ||
        strcmp(func_name, "EnableWindow") == 0 ||
        strcmp(func_name, "HeapDestroy") == 0) return 1;

    /* 2 参数 */
    if (strcmp(func_name, "HeapAlloc") == 0 ||
        strcmp(func_name, "HeapFree") == 0 ||
        strcmp(func_name, "LoadLibraryA") == 0 ||
        strcmp(func_name, "LoadLibraryW") == 0 ||
        strcmp(func_name, "GetProcAddress") == 0 ||
        strcmp(func_name, "GetFileSize") == 0 ||
        strcmp(func_name, "SetWindowTextA") == 0 ||
        strcmp(func_name, "GetWindowTextA") == 0 ||
        strcmp(func_name, "GetWindowTextLengthA") == 0 ||
        strcmp(func_name, "GetWindowRect") == 0 ||
        strcmp(func_name, "GetClientRect") == 0 ||
        strcmp(func_name, "InvalidateRect") == 0 ||
        strcmp(func_name, "SetWindowTextA") == 0 ||
        strcmp(func_name, "send") == 0 ||
        strcmp(func_name, "recv") == 0 ||
        strcmp(func_name, "bind") == 0 ||
        strcmp(func_name, "connect") == 0 ||
        strcmp(func_name, "accept") == 0 ||
        strcmp(func_name, "shutdown") == 0 ||
        strcmp(func_name, "TranslateMessage") == 0 ||
        strcmp(func_name, "DispatchMessageA") == 0 ||
        strcmp(func_name, "PostMessageA") == 0 ||
        strcmp(func_name, "SendMessageA") == 0 ||
        strcmp(func_name, "SetParent") == 0 ||
        strcmp(func_name, "GetStockObject") == 0 ||
        strcmp(func_name, "SelectObject") == 0 ||
        strcmp(func_name, "SetTextColor") == 0 ||
        strcmp(func_name, "SetBkColor") == 0 ||
        strcmp(func_name, "SetBkMode") == 0 ||
        strcmp(func_name, "TextOutA") == 0 ||
        strcmp(func_name, "FillRect") == 0 ||
        strcmp(func_name, "DeleteFileA") == 0 ||
        strcmp(func_name, "SetCursorPos") == 0 ||
        strcmp(func_name, "SetCapture") == 0 ||
        strcmp(func_name, "ReleaseCapture") == 0 ||
        strcmp(func_name, "GetCursorPos") == 0 ||
        strcmp(func_name, "GlobalFree") == 0 ||
        strcmp(func_name, "GlobalLock") == 0 ||
        strcmp(func_name, "GlobalUnlock") == 0 ||
        strcmp(func_name, "GetKeyboardState") == 0 ||
        strcmp(func_name, "DestroyMenu") == 0 ||
        strcmp(func_name, "SetMenu") == 0 ||
        strcmp(func_name, "RegCloseKey") == 0 ||
        strcmp(func_name, "freeaddrinfo") == 0 ||
        strcmp(func_name, "gethostbyname") == 0 ||
        strcmp(func_name, "gethostname") == 0 ||
        strcmp(func_name, "getsockname") == 0 ||
        strcmp(func_name, "getpeername") == 0 ||
        strcmp(func_name, "htons") == 0 ||
        strcmp(func_name, "htonl") == 0 ||
        strcmp(func_name, "ntohs") == 0 ||
        strcmp(func_name, "ntohl") == 0 ||
        strcmp(func_name, "inet_addr") == 0 ||
        strcmp(func_name, "inet_ntoa") == 0) return 2;

    /* 3 参数 */
    if (strcmp(func_name, "ReadFile") == 0 ||
        strcmp(func_name, "WriteFile") == 0 ||
        strcmp(func_name, "VirtualFree") == 0 ||
        strcmp(func_name, "HeapCreate") == 0 ||
        strcmp(func_name, "WaitForSingleObject") == 0 ||
        strcmp(func_name, "CreateEventA") == 0 ||
        strcmp(func_name, "CreateMutexA") == 0 ||
        strcmp(func_name, "GetModuleFileNameA") == 0 ||
        strcmp(func_name, "MoveFileA") == 0 ||
        strcmp(func_name, "GetFileAttributesA") == 0 ||
        strcmp(func_name, "FindFirstFileA") == 0 ||
        strcmp(func_name, "FindNextFileA") == 0 ||
        strcmp(func_name, "FindClose") == 0 ||
        strcmp(func_name, "CreateCompatibleDC") == 0 ||
        strcmp(func_name, "BeginPaint") == 0 ||
        strcmp(func_name, "EndPaint") == 0 ||
        strcmp(func_name, "Rectangle") == 0 ||
        strcmp(func_name, "MoveWindow") == 0 ||
        strcmp(func_name, "socket") == 0 ||
        strcmp(func_name, "sendto") == 0 ||
        strcmp(func_name, "recvfrom") == 0 ||
        strcmp(func_name, "getsockopt") == 0 ||
        strcmp(func_name, "setsockopt") == 0 ||
        strcmp(func_name, "ioctlsocket") == 0 ||
        strcmp(func_name, "RegOpenKeyA") == 0 ||
        strcmp(func_name, "RegOpenKeyExA") == 0 ||
        strcmp(func_name, "RegDeleteKeyA") == 0 ||
        strcmp(func_name, "RegDeleteValueA") == 0 ||
        strcmp(func_name, "RegEnumKeyA") == 0 ||
        strcmp(func_name, "RegEnumValueA") == 0 ||
        strcmp(func_name, "GlobalAlloc") == 0 ||
        strcmp(func_name, "GlobalSize") == 0 ||
        strcmp(func_name, "AppendMenuA") == 0 ||
        strcmp(func_name, "MoveToEx") == 0 ||
        strcmp(func_name, "LineTo") == 0 ||
        strcmp(func_name, "WSAStartup") == 0 ||
        strcmp(func_name, "WSASocketA") == 0 ||
        strcmp(func_name, "WSASocketW") == 0 ||
        strcmp(func_name, "WSAAsyncSelect") == 0 ||
        strcmp(func_name, "WSAEventSelect") == 0 ||
        strcmp(func_name, "getaddrinfo") == 0 ||
        strcmp(func_name, "getprotobyname") == 0 ||
        strcmp(func_name, "getservbyname") == 0) return 3;

    /* 4 参数 - 最常见的 Windows API 参数数量 */
    if (strcmp(func_name, "VirtualAlloc") == 0 ||
        strcmp(func_name, "VirtualProtect") == 0 ||
        strcmp(func_name, "CreateFileA") == 0 ||
        strcmp(func_name, "CopyFileA") == 0 ||
        strcmp(func_name, "CreateThread") == 0 ||
        strcmp(func_name, "CreateProcessA") == 0 ||
        strcmp(func_name, "GetMessageA") == 0 ||
        strcmp(func_name, "PeekMessageA") == 0 ||
        strcmp(func_name, "DefWindowProcA") == 0 ||
        strcmp(func_name, "CallWindowProcA") == 0 ||
        strcmp(func_name, "SetWindowLongPtrA") == 0 ||
        strcmp(func_name, "GetWindowLongPtrA") == 0 ||
        strcmp(func_name, "SetWindowLongA") == 0 ||
        strcmp(func_name, "GetWindowLongA") == 0 ||
        strcmp(func_name, "RegisterClassA") == 0 ||
        strcmp(func_name, "RegisterClassExA") == 0 ||
        strcmp(func_name, "UnregisterClassA") == 0 ||
        strcmp(func_name, "CreatePen") == 0 ||
        strcmp(func_name, "CreateSolidBrush") == 0 ||
        strcmp(func_name, "Ellipse") == 0 ||
        strcmp(func_name, "BitBlt") == 0 ||
        strcmp(func_name, "select") == 0 ||
        strcmp(func_name, "WSASend") == 0 ||
        strcmp(func_name, "WSARecv") == 0 ||
        strcmp(func_name, "WSAGetOverlappedResult") == 0 ||
        strcmp(func_name, "WSAEnumNetworkEvents") == 0 ||
        strcmp(func_name, "RegSetValueExA") == 0 ||
        strcmp(func_name, "RegQueryValueExA") == 0 ||
        strcmp(func_name, "RegCreateKeyA") == 0 ||
        strcmp(func_name, "RegCreateKeyExA") == 0 ||
        strcmp(func_name, "GetTextMetricsA") == 0 ||
        strcmp(func_name, "GetTextExtentPoint32A") == 0 ||
        strcmp(func_name, "SelectClipRgn") == 0 ||
        strcmp(func_name, "GetDeviceCaps") == 0 ||
        strcmp(func_name, "StretchBlt") == 0 ||
        strcmp(func_name, "TlsAlloc") == 0 ||
        strcmp(func_name, "TlsFree") == 0 ||
        strcmp(func_name, "TlsGetValue") == 0 ||
        strcmp(func_name, "TlsSetValue") == 0 ||
        strcmp(func_name, "SetTimer") == 0 ||
        strcmp(func_name, "VirtualQuery") == 0 ||
        strcmp(func_name, "GetStartupInfoA") == 0 ||
        strcmp(func_name, "GetStartupInfoW") == 0 ||
        strcmp(func_name, "GetVersion") == 0 ||
        strcmp(func_name, "OutputDebugStringA") == 0 ||
        strcmp(func_name, "DebugBreak") == 0 ||
        strcmp(func_name, "RaiseException") == 0 ||
        strcmp(func_name, "RemoveVectoredExceptionHandler") == 0 ||
        strcmp(func_name, "UnhandledExceptionFilter") == 0 ||
        strcmp(func_name, "GetStdHandle") == 0 ||
        strcmp(func_name, "SetStdHandle") == 0 ||
        strcmp(func_name, "GetFileType") == 0 ||
        strcmp(func_name, "WriteConsoleA") == 0 ||
        strcmp(func_name, "ReadConsoleA") == 0 ||
        strcmp(func_name, "GetEnvironmentVariableA") == 0 ||
        strcmp(func_name, "SetEnvironmentVariableA") == 0 ||
        strcmp(func_name, "GetComputerNameA") == 0 ||
        strcmp(func_name, "GetComputerNameW") == 0 ||
        strcmp(func_name, "GetUserNameA") == 0 ||
        strcmp(func_name, "GetUserNameW") == 0 ||
        strcmp(func_name, "GetSystemInfo") == 0 ||
        strcmp(func_name, "GetTickCount") == 0 ||
        strcmp(func_name, "GetCurrentDirectoryA") == 0 ||
        strcmp(func_name, "SetCurrentDirectoryA") == 0 ||
        strcmp(func_name, "CreateFontA") == 0) return 4;

    /* 5+ 参数 */
    if (strcmp(func_name, "CreateWindowExA") == 0 ||
        strcmp(func_name, "CreateWindowExW") == 0) return 12;

    if (strcmp(func_name, "ShellExecuteA") == 0) return 6;

    /* 默认 4 参数 */
    return 4;
}

/*
 * 在用户空间 thunk 页中分配并生成一个调用约定转换 thunk。
 * 返回用户空间 thunk 地址，PE 程序通过 IAT 调用此地址。
 *
 * 生成的 thunk 代码执行：
 *   1. 将 Windows x64 寄存器 (RCX,RDX,R8,R9) 重映射到 System V (RDI,RSI,RDX,RCX)
 *   2. 跳转到内核实现函数
 */
static uint64_t pe_alloc_thunk(struct pe_module *mod, void *target_func, int arg_count) {
    if (mod->thunk_page == 0) return (uint64_t)target_func; /* 无 thunk 页，直接返回 */

    uint64_t thunk_addr = mod->thunk_page + mod->thunk_page_used;
    uint8_t *thunk = (uint8_t *)thunk_addr;
    int pos = 0;

    /* 限制参数数量 */
    if (arg_count > 4) arg_count = 4; /* 5+ 参数需要栈处理，暂简化为 4 */

    /*
     * 寄存器重映射顺序至关重要！必须先保存源寄存器再覆盖目标寄存器。
     * Windows x64: arg1=RCX, arg2=RDX, arg3=R8, arg4=R9
     * System V x64: arg1=RDI, arg2=RSI, arg3=RDX, arg4=RCX
     *
     * 映射: RCX->RDI, RDX->RSI, R8->RDX, R9->RCX
     *
     * 冲突分析: RDX 既是 Windows arg2 源，又是 System V arg3 目标。
     * 因此必须先保存 RDX 到 RSI，再覆盖 RDX。
     * RCX 既是 Windows arg1 源，又是 System V arg4 目标。
     * 因此必须先保存 RCX 到 RDI，再覆盖 RCX。
     *
     * 正确顺序:
     *   1. mov rdi, rcx  (保存 arg1)
     *   2. mov rsi, rdx  (保存 arg2)
     *   3. mov rdx, r8   (arg3 -> rdx)
     *   4. mov rcx, r9   (arg4 -> rcx)
     */

    /* 1. mov rdi, rcx  ->  48 89 CF */
    if (arg_count >= 1) {
        thunk[pos++] = 0x48;
        thunk[pos++] = 0x89;
        thunk[pos++] = 0xCF;
    }

    /* 2. mov rsi, rdx  ->  48 89 D6 */
    if (arg_count >= 2) {
        thunk[pos++] = 0x48;
        thunk[pos++] = 0x89;
        thunk[pos++] = 0xD6;
    }

    /* 3. mov rdx, r8  ->  4C 89 C2 */
    if (arg_count >= 3) {
        thunk[pos++] = 0x4C;
        thunk[pos++] = 0x89;
        thunk[pos++] = 0xC2;
    }

    /* 4. mov rcx, r9  ->  4C 89 C9 */
    if (arg_count >= 4) {
        thunk[pos++] = 0x4C;
        thunk[pos++] = 0x89;
        thunk[pos++] = 0xC9;
    }

    /* mov rax, <target_func>  ->  48 B8 <8 bytes little-endian> */
    thunk[pos++] = 0x48;
    thunk[pos++] = 0xB8;
    uint64_t addr = (uint64_t)target_func;
    thunk[pos++] = (uint8_t)(addr);
    thunk[pos++] = (uint8_t)(addr >> 8);
    thunk[pos++] = (uint8_t)(addr >> 16);
    thunk[pos++] = (uint8_t)(addr >> 24);
    thunk[pos++] = (uint8_t)(addr >> 32);
    thunk[pos++] = (uint8_t)(addr >> 40);
    thunk[pos++] = (uint8_t)(addr >> 48);
    thunk[pos++] = (uint8_t)(addr >> 56);

    /* jmp rax  ->  FF E0 */
    thunk[pos++] = 0xFF;
    thunk[pos++] = 0xE0;

    mod->thunk_page_used += pos;

    return thunk_addr;
}

/* 分配用户空间 thunk 页 */
static int pe_alloc_thunk_page(struct pe_module *mod) {
    struct process *proc = process_get_current();
    if (proc == NULL || proc->space == NULL) {
        mod->thunk_page = 0;
        return -1;
    }

    /* 在用户空间分配 thunk 页，位于 PE 镜像之后 */
    uint64_t thunk_phys = pmm_alloc_page();
    if (thunk_phys == 0) {
        mod->thunk_page = 0;
        return -1;
    }

    /* 使用镜像基址 + 镜像大小对齐后的地址 */
    uint64_t thunk_vaddr = (mod->image_base + mod->image_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    /* 如果地址已被映射，尝试更高地址 */
    for (int attempt = 0; attempt < 16; attempt++) {
        if (vmm_get_physical(proc->space, thunk_vaddr) == 0) {
            break;
        }
        thunk_vaddr += PAGE_SIZE;
    }

    if (vmm_map_page(proc->space, thunk_vaddr, thunk_phys,
                     VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE) != 0) {
        pmm_free_page(thunk_phys);
        mod->thunk_page = 0;
        return -1;
    }

    /* 清零 thunk 页 */
    for (uint32_t i = 0; i < PAGE_SIZE / 8; i++) {
        ((uint64_t *)thunk_vaddr)[i] = 0;
    }

    mod->thunk_page = thunk_vaddr;
    mod->thunk_page_used = 0;

    print("[PE] Thunk page at ");
    print_hex(thunk_vaddr);
    println("");

    return 0;
}

/* ========== 全局模块列表 ========== */

#define MAX_PE_MODULES  64

static struct pe_module pe_modules[MAX_PE_MODULES];
static int pe_module_count = 0;
static spinlock_t pe_lock = SPINLOCK_INIT;

/* 页大小 (使用 ktypes.h 中的定义) */

/* ========== 辅助函数 ========== */

/* 不区分大小写字符串比较 */
static int stricmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = *s1, c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return *s1 - *s2;
}

/* 字符串哈希 */
static uint32_t hash_string(const char *s) {
    uint32_t h = 0;
    while (*s) {
        h = h * 31 + (uint8_t)*s;
        s++;
    }
    return h;
}

/* ========== 初始化 ========== */

int pe_init(void) {
    for (int i = 0; i < MAX_PE_MODULES; i++) {
        pe_modules[i].image_base = 0;
        pe_modules[i].ref_count = 0;
        pe_modules[i].path = NULL;
    }
    pe_module_count = 0;
    return 0;
}

/* ========== 验证函数 ========== */

int pe_validate(const void *data, size_t size) {
    if (data == NULL || size < sizeof(struct dos_hdr)) {
        return -1;
    }
    
    /* 检查 DOS 签名 */
    struct dos_hdr *dos = (struct dos_hdr *)data;
    if (dos->e_magic != DOS_SIGNATURE) {
        return -1;
    }
    
    /* 检查 PE 头偏移 */
    if (dos->e_lfanew + 4 + sizeof(struct coff_hdr) > size) {
        return -1;
    }
    
    /* 检查 PE 签名 */
    uint32_t *pe_sig = (uint32_t *)((uint8_t *)data + dos->e_lfanew);
    if (*pe_sig != PE_SIGNATURE) {
        return -1;
    }
    
    return 0;
}

int pe_is_64bit(const void *data) {
    if (pe_validate(data, 0) != 0) {
        return -1;
    }
    
    struct dos_hdr *dos = (struct dos_hdr *)data;
    struct coff_hdr *coff = (struct coff_hdr *)((uint8_t *)data + dos->e_lfanew + 4);
    
    if (coff->SizeOfOptionalHeader < sizeof(struct pe_opt_hdr)) {
        return -1;
    }
    
    struct pe_opt_hdr *opt = (struct pe_opt_hdr *)((uint8_t *)coff + sizeof(struct coff_hdr));
    
    return (opt->Magic == PE32_PLUS_MAGIC) ? 1 : 0;
}

/* ========== 节操作 ========== */

struct section_hdr *pe_find_section(struct pe_module *mod, uint32_t rva) {
    for (int i = 0; i < mod->section_count; i++) {
        struct section_hdr *sec = &mod->sections[i];
        uint32_t sec_start = sec->VirtualAddress;
        uint32_t sec_end = sec_start + sec->VirtualSize;
        
        if (rva >= sec_start && rva < sec_end) {
            return sec;
        }
    }
    return NULL;
}

uint32_t pe_get_section_protection(struct section_hdr *sec) {
    uint32_t prot = 0;
    
    /* 默认可读 */
    prot |= VMM_FLAG_PRESENT;
    
    /* 可写 */
    if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) {
        prot |= VMM_FLAG_WRITABLE;
    }
    
    /* 用户空间 */
    prot |= VMM_FLAG_USER;
    
    /* 不可执行 */
    if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
        prot |= VMM_FLAG_NX;
    }
    
    return prot;
}

/* ========== 模块管理 ========== */

static struct pe_module *alloc_pe_module(void) {
    spin_lock(&pe_lock);
    
    for (int i = 0; i < MAX_PE_MODULES; i++) {
        if (pe_modules[i].image_base == 0) {
            struct pe_module *mod = &pe_modules[i];
            mod->ref_count = 1;
            mod->imports_resolved = 0;
            mod->reloc_processed = 0;
            if (i >= pe_module_count) {
                pe_module_count = i + 1;
            }
            spin_unlock(&pe_lock);
            return mod;
        }
    }
    
    spin_unlock(&pe_lock);
    return NULL;
}

static void free_pe_module(struct pe_module *mod) {
    spin_lock(&pe_lock);
    
    mod->image_base = 0;
    mod->ref_count = 0;
    mod->path = NULL;
    
    spin_unlock(&pe_lock);
}

struct pe_module *pe_find_module(const char *name) {
    spin_lock(&pe_lock);
    
    for (int i = 0; i < pe_module_count; i++) {
        if (pe_modules[i].image_base != 0 && pe_modules[i].path) {
            /* 比较文件名 */
            const char *p = pe_modules[i].path;
            const char *last = p;
            while (*p) {
                if (*p == '/' || *p == '\\') last = p + 1;
                p++;
            }
            if (stricmp(last, name) == 0) {
                struct pe_module *mod = &pe_modules[i];
                mod->ref_count++;
                spin_unlock(&pe_lock);
                return mod;
            }
        }
    }
    
    spin_unlock(&pe_lock);
    return NULL;
}

void *pe_get_module_handle(const char *name) {
    struct pe_module *mod = pe_find_module(name);
    return mod ? (void *)mod->image_base : NULL;
}

/* ========== 映射节到内存 ========== */

int pe_map_sections(struct pe_module *mod) {
    struct process *proc = process_get_current();
    if (proc == NULL || proc->space == NULL) {
        return -1;
    }
    
    /* 计算镜像大小并分配内存 */
    uint64_t image_size = mod->image_size;
    uint64_t image_base = mod->image_base;
    
    /* 分配镜像内存 */
    uint64_t pages = (image_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t allocated_pages = 0;  /* 跟踪已分配的页面，用于回滚 */
    
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            /* 分配失败，回滚已分配的页面 */
            for (uint64_t j = 0; j < allocated_pages; j++) {
                uint64_t vaddr = image_base + j * PAGE_SIZE;
                uint64_t p = vmm_get_physical(proc->space, vaddr);
                if (p != 0) {
                    vmm_unmap_page(proc->space, vaddr);
                    pmm_free_page(p);
                }
            }
            return -1;
        }
        
        /* 清零 */
        void *vaddr = phys_to_virt(phys);
        for (uint32_t j = 0; j < PAGE_SIZE; j++) {
            ((uint8_t *)vaddr)[j] = 0;
        }
        
        /* 映射页面 */
        if (vmm_map_page(proc->space, image_base + i * PAGE_SIZE, phys,
                     VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE) != 0) {
            /* 映射失败，释放此页面并回滚 */
            pmm_free_page(phys);
            for (uint64_t j = 0; j < allocated_pages; j++) {
                uint64_t vaddr = image_base + j * PAGE_SIZE;
                uint64_t p = vmm_get_physical(proc->space, vaddr);
                if (p != 0) {
                    vmm_unmap_page(proc->space, vaddr);
                    pmm_free_page(p);
                }
            }
            return -1;
        }
        allocated_pages++;
    }
    
    /* 复制头部 */
    void *dest = (void *)image_base;
    void *src = mod->file_data;
    uint32_t hdr_size = mod->opt_hdr->SizeOfHeaders;
    
    for (uint32_t i = 0; i < hdr_size; i++) {
        ((uint8_t *)dest)[i] = ((uint8_t *)src)[i];
    }
    
    /* 映射每个节 */
    for (int i = 0; i < mod->section_count; i++) {
        struct section_hdr *sec = &mod->sections[i];
        
        if (sec->SizeOfRawData == 0 && sec->VirtualSize == 0) {
            continue;
        }
        
        /* 计算节大小 */
        uint32_t sec_size = sec->VirtualSize;
        if (sec_size < sec->SizeOfRawData) {
            sec_size = sec->SizeOfRawData;
        }
        
        /* 复制节数据 */
        if (sec->SizeOfRawData > 0 && sec->PointerToRawData > 0) {
            /* 边界检查：防止越界访问 */
            if (sec->PointerToRawData + sec->SizeOfRawData > mod->file_size) {
                /* 超出文件边界，跳过此节 */
                continue;
            }
            /* 边界检查：防止写入超出镜像空间 */
            if (sec->VirtualAddress + sec->SizeOfRawData > mod->image_size) {
                continue;
            }
            
            uint8_t *src_sec = (uint8_t *)mod->file_data + sec->PointerToRawData;
            uint8_t *dst_sec = (uint8_t *)image_base + sec->VirtualAddress;
            
            for (uint32_t j = 0; j < sec->SizeOfRawData; j++) {
                dst_sec[j] = src_sec[j];
            }
        }
        
        /* 设置节保护属性 */
        uint32_t prot = pe_get_section_protection(sec);
        uint64_t sec_start = image_base + sec->VirtualAddress;
        uint64_t sec_pages = (sec_size + PAGE_SIZE - 1) / PAGE_SIZE;

        for (uint64_t p = 0; p < sec_pages; p++) {
            uint64_t vaddr = sec_start + p * PAGE_SIZE;
            /* 更新页保护属性 */
            vmm_protect(proc->space, vaddr, prot);
        }
    }
    
    return 0;
}

/* ========== 重定位处理 ========== */

int pe_process_relocations(struct pe_module *mod) {
    if (mod->reloc_dir == NULL || mod->reloc_processed) {
        return 0;
    }
    
    /* 计算重定位差值 */
    int64_t delta = (int64_t)mod->image_base - (int64_t)mod->preferred_base;
    
    if (delta == 0) {
        /* 加载到首选地址，无需重定位 */
        mod->reloc_processed = 1;
        return 0;
    }
    
    print("[PE] Processing relocations, delta = ");
    print_hex((uint64_t)delta);
    println("");
    
    /* 遍历重定位块 */
    struct reloc_block *block = mod->reloc_dir;
    
    while (block->VirtualAddress != 0 && block->SizeOfBlock != 0) {
        uint32_t block_rva = block->VirtualAddress;
        uint32_t block_size = block->SizeOfBlock;
        int entry_count = (block_size - 8) / 2;  /* 减去头部，每项 2 字节 */
        
        for (int i = 0; i < entry_count; i++) {
            uint16_t entry = block->TypeOffset[i];
            int type = RELOC_TYPE(entry);
            uint32_t offset = RELOC_OFFSET(entry);
            
            if (type == IMAGE_REL_BASED_ABSOLUTE) {
                /* 跳过填充项 */
                continue;
            }
            
            if (type == IMAGE_REL_BASED_DIR64) {
                /* 64 位重定位 */
                uint64_t rva = block_rva + offset;
                /* 边界检查：确保重定位目标在镜像范围内 */
                if (rva + 8 <= mod->image_size) {
                    uint64_t *target = (uint64_t *)(mod->image_base + rva);
                    *target += delta;
                }
            } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                /* 32 位重定位 (PE32) */
                uint64_t rva = block_rva + offset;
                /* 边界检查 */
                if (rva + 4 <= mod->image_size) {
                    uint32_t *target = (uint32_t *)(mod->image_base + rva);
                    *target += (uint32_t)delta;
                }
            }
        }
        
        /* 移动到下一个块 */
        block = (struct reloc_block *)((uint8_t *)block + block_size);
    }
    
    mod->reloc_processed = 1;
    return 0;
}

/* ========== 导入处理 ========== */

/* 内置 DLL 导出查找 */
static void *find_builtin_export(const char *dll_name, const char *func_name, 
                                  uint16_t ordinal, int by_ordinal) {
    /* 在 ntdll 中查找 */
    if (stricmp(dll_name, "ntdll.dll") == 0 || 
        stricmp(dll_name, "ntdll") == 0) {
        return ntdll_get_proc(func_name);
    }
    
    /* 在 kernel32 中查找 */
    if (stricmp(dll_name, "kernel32.dll") == 0 ||
        stricmp(dll_name, "kernel32") == 0 ||
        stricmp(dll_name, "kernelbase.dll") == 0) {
        return kernel32_get_proc(func_name);
    }
    
    /* 在 user32 中查找 */
    if (stricmp(dll_name, "user32.dll") == 0 ||
        stricmp(dll_name, "user32") == 0) {
        return user32_get_proc(func_name);
    }
    
    /* 在 gdi32 中查找 */
    if (stricmp(dll_name, "gdi32.dll") == 0 ||
        stricmp(dll_name, "gdi32") == 0) {
        return gdi32_get_proc(func_name);
    }
    
    /* 在 advapi32 中查找 */
    if (stricmp(dll_name, "advapi32.dll") == 0 ||
        stricmp(dll_name, "advapi32") == 0) {
        return advapi32_get_proc(func_name);
    }

    /* 在 ole32 中查找 */
    if (stricmp(dll_name, "ole32.dll") == 0 ||
        stricmp(dll_name, "ole32") == 0) {
        return ole32_get_proc(func_name);
    }

    /* 在 shell32 中查找 */
    if (stricmp(dll_name, "shell32.dll") == 0 ||
        stricmp(dll_name, "shell32") == 0) {
        return shell32_get_proc(func_name);
    }

    /* 在 comdlg32 中查找 */
    if (stricmp(dll_name, "comdlg32.dll") == 0 ||
        stricmp(dll_name, "comdlg32") == 0) {
        return comdlg32_get_proc(func_name);
    }

    /* 在 ws2_32/wsock32 中查找 */
    if (stricmp(dll_name, "ws2_32.dll") == 0 ||
        stricmp(dll_name, "ws2_32") == 0 ||
        stricmp(dll_name, "wsock32.dll") == 0 ||
        stricmp(dll_name, "wsock32") == 0) {
        return ws2_32_get_proc(func_name);
    }

    /* 在 iphlpapi 中查找 */
    if (stricmp(dll_name, "iphlpapi.dll") == 0 ||
        stricmp(dll_name, "iphlpapi") == 0) {
        return iphlpapi_get_proc(func_name);
    }

    /* 在 dnsapi 中查找 */
    if (stricmp(dll_name, "dnsapi.dll") == 0 ||
        stricmp(dll_name, "dnsapi") == 0) {
        return dnsapi_get_proc(func_name);
    }

    /* 在 wininet 中查找 */
    if (stricmp(dll_name, "wininet.dll") == 0 ||
        stricmp(dll_name, "wininet") == 0) {
        return wininet_get_proc(func_name);
    }

    /* 在 winhttp 中查找 */
    if (stricmp(dll_name, "winhttp.dll") == 0 ||
        stricmp(dll_name, "winhttp") == 0) {
        return winhttp_get_proc(func_name);
    }

    /* 在 mswsock 中查找 */
    if (stricmp(dll_name, "mswsock.dll") == 0 ||
        stricmp(dll_name, "mswsock") == 0) {
        return mswsock_get_proc(func_name);
    }

    return NULL;
}

int pe_process_imports(struct pe_module *mod) {
    if (mod->import_dir == NULL || mod->imports_resolved) {
        return 0;
    }
    
    print("[PE] Processing imports for: ");
    println(mod->path ? mod->path : "(unknown)");
    
    struct import_dir *import = mod->import_dir;
    
    while (import->Name != 0) {
        /* 获取 DLL 名称 */
        char *dll_name = (char *)(mod->image_base + import->Name);
        
        print("[PE] Importing from: ");
        println(dll_name);
        
        /* 查找或加载 DLL */
        struct pe_module *dll = pe_find_module(dll_name);
        if (dll == NULL) {
            /* 尝试加载 DLL */
            dll = pe_load_file(dll_name);
            if (dll == NULL) {
                /* 使用内置 DLL */
                print("[PE] Using builtin: ");
                println(dll_name);
            }
        }
        
        /* 获取导入地址表 (IAT) */
        uint64_t *thunk;
        if (import->OriginalFirstThunk != 0) {
            thunk = (uint64_t *)(mod->image_base + import->OriginalFirstThunk);
        } else {
            thunk = (uint64_t *)(mod->image_base + import->FirstThunk);
        }
        
        uint64_t *iat = (uint64_t *)(mod->image_base + import->FirstThunk);
        
        /* 解析每个导入 */
        int index = 0;
        while (*thunk != 0) {
            uint64_t lookup = *thunk;
            void *func_addr = NULL;
            const char *func_name = NULL; /* 用于参数推断 */

            if (lookup & IMPORT_BY_ORDINAL) {
                /* 按序号导入 */
                uint16_t ordinal = (uint16_t)(lookup & 0xFFFF);

                if (dll != NULL) {
                    func_addr = pe_get_export_ordinal(dll, ordinal);
                }

                if (func_addr == NULL) {
                    func_addr = find_builtin_export(dll_name, NULL, ordinal, 1);
                }

                print("[PE]   Import ordinal ");
                print_dec(ordinal);
                print(" -> ");
                print_hex((uint64_t)func_addr);
                println("");
            } else {
                /* 按名称导入 */
                struct import_hint_name *hint = (struct import_hint_name *)
                    (mod->image_base + (lookup & 0x7FFFFFFF));
                func_name = hint->Name;
                
                if (dll != NULL) {
                    func_addr = pe_get_export(dll, func_name);
                }
                
                if (func_addr == NULL) {
                    func_addr = find_builtin_export(dll_name, func_name, 0, 0);
                }
                
                print("[PE]   Import ");
                print(func_name);
                print(" -> ");
                print_hex((uint64_t)func_addr);
                println("");
            }
            
            /* 填充 IAT - 使用调用约定转换 thunk */
            if (func_addr != NULL) {
                int arg_count = pe_infer_arg_count(func_name);
                iat[index] = pe_alloc_thunk(mod, func_addr, arg_count);
            } else {
                iat[index] = 0;
            }
            
            thunk++;
            index++;
        }
        
        /* 移动到下一个 DLL 导入 */
        import++;
    }
    
    mod->imports_resolved = 1;
    return 0;
}

/* ========== 导出查找 ========== */

void *pe_get_export(struct pe_module *mod, const char *name) {
    if (mod == NULL || mod->export_dir == NULL || name == NULL) {
        return NULL;
    }
    
    struct export_dir *exp = mod->export_dir;
    
    /* 计算名称哈希 */
    uint32_t name_hash = hash_string(name);
    
    /* 遍历导出名称表 */
    uint32_t *name_table = (uint32_t *)(mod->image_base + exp->AddressOfNames);
    uint16_t *ordinal_table = (uint16_t *)(mod->image_base + exp->AddressOfNameOrdinals);
    uint32_t *func_table = (uint32_t *)(mod->image_base + exp->AddressOfFunctions);
    
    for (uint32_t i = 0; i < exp->NumberOfNames; i++) {
        char *export_name = (char *)(mod->image_base + name_table[i]);
        
        /* 快速哈希比较 */
        if (hash_string(export_name) == name_hash) {
            /* 精确字符串比较 */
            int j = 0;
            while (name[j] && export_name[j] && name[j] == export_name[j]) {
                j++;
            }
            if (name[j] == '\0' && export_name[j] == '\0') {
                /* 找到匹配 */
                uint16_t ordinal = ordinal_table[i];
                uint32_t func_rva = func_table[ordinal];
                
                /* 检查是否为转发导出 */
                if (func_rva >= exp->AddressOfFunctions &&
                    func_rva < exp->AddressOfFunctions + exp->NumberOfFunctions * 4) {
                    /* 转发导出，格式: "DLLName.FuncName" */
                    char *forward_str = (char *)(mod->image_base + func_rva);

                    /* 解析 "DLLName.FuncName" 格式 */
                    char dll_name[256];
                    char func_name[256];
                    int dot_pos = -1;

                    /* 查找点号位置 */
                    for (int k = 0; forward_str[k] && k < 255; k++) {
                        if (forward_str[k] == '.') {
                            dot_pos = k;
                            break;
                        }
                    }

                    if (dot_pos > 0) {
                        /* 复制 DLL 名 */
                        for (int k = 0; k < dot_pos && k < 255; k++) {
                            dll_name[k] = forward_str[k];
                        }
                        dll_name[dot_pos] = '\0';

                        /* 添加 .dll 后缀（如果没有） */
                        int dll_len = dot_pos;
                        if (dll_len + 4 < 255 &&
                            stricmp(dll_name + dll_len - 4, ".dll") != 0) {
                            dll_name[dll_len] = '.';
                            dll_name[dll_len + 1] = 'd';
                            dll_name[dll_len + 2] = 'l';
                            dll_name[dll_len + 3] = 'l';
                            dll_name[dll_len + 4] = '\0';
                        }

                        /* 复制函数名 */
                        int f = 0;
                        for (int k = dot_pos + 1; forward_str[k] && f < 255; k++, f++) {
                            func_name[f] = forward_str[k];
                        }
                        func_name[f] = '\0';

                        print("[PE] Forward export: ");
                        print(dll_name);
                        print(".");
                        println(func_name);

                        /* 查找目标 DLL */
                        struct pe_module *target_dll = pe_find_module(dll_name);
                        if (target_dll == NULL) {
                            target_dll = pe_load_file(dll_name);
                        }

                        if (target_dll != NULL) {
                            return pe_get_export(target_dll, func_name);
                        }

                        /* 尝试从内置 DLL 查找 */
                        return find_builtin_export(dll_name, func_name, 0, 0);
                    }

                    return NULL;
                }
                
                return (void *)(mod->image_base + func_rva);
            }
        }
    }
    
    return NULL;
}

void *pe_get_export_ordinal(struct pe_module *mod, uint32_t ordinal) {
    if (mod == NULL || mod->export_dir == NULL) {
        return NULL;
    }
    
    struct export_dir *exp = mod->export_dir;
    
    /* 调整序号 */
    if (ordinal < exp->Base || ordinal >= exp->Base + exp->NumberOfFunctions) {
        return NULL;
    }
    
    uint32_t index = ordinal - exp->Base;
    uint32_t *func_table = (uint32_t *)(mod->image_base + exp->AddressOfFunctions);
    uint32_t func_rva = func_table[index];
    
    return (void *)(mod->image_base + func_rva);
}

/* ========== 完整加载流程 ========== */

struct pe_module *pe_load_module(const void *data, size_t size, const char *path) {
    /* 验证 PE */
    if (pe_validate(data, size) != 0) {
        println("[PE] Invalid PE file");
        return NULL;
    }
    
    /* 检查是否为 64 位 */
    if (pe_is_64bit(data) != 1) {
        println("[PE] Only PE32+ (64-bit) is supported");
        return NULL;
    }
    
    /* 分配模块结构 */
    struct pe_module *mod = alloc_pe_module();
    if (mod == NULL) {
        println("[PE] Too many modules");
        return NULL;
    }
    
    /* 解析 PE 头 */
    mod->dos_hdr = (struct dos_hdr *)data;
    mod->coff_hdr = (struct coff_hdr *)((uint8_t *)data + mod->dos_hdr->e_lfanew + 4);
    mod->opt_hdr = (struct pe_opt_hdr *)((uint8_t *)mod->coff_hdr + sizeof(struct coff_hdr));
    mod->sections = (struct section_hdr *)((uint8_t *)mod->opt_hdr + mod->coff_hdr->SizeOfOptionalHeader);
    mod->section_count = mod->coff_hdr->NumberOfSections;
    
    mod->preferred_base = mod->opt_hdr->ImageBase;
    mod->image_size = mod->opt_hdr->SizeOfImage;
    mod->entry_point = mod->preferred_base + mod->opt_hdr->AddressOfEntryPoint;
    mod->path = path;
    mod->file_data = (void *)data;
    mod->file_size = size;
    
    /* 数据目录：紧跟在 Optional Header 之后 */
    /* 注意：SizeOfOptionalHeader 包含了数据目录的大小，数据目录从 opt_hdr 结束位置开始 */
    mod->data_dirs = (struct data_dir *)((uint8_t *)mod->opt_hdr + sizeof(struct pe_opt_hdr));
    mod->data_dir_count = mod->opt_hdr->NumberOfRvaAndSizes;
    
    /* 数据目录指针暂设为 NULL，在节映射后根据 RVA 转换为 VA */
    mod->import_dir = NULL;
    mod->export_dir = NULL;
    mod->reloc_dir = NULL;
    mod->tls_dir = NULL;
    mod->exception_dir = NULL;
    mod->exception_count = 0;
    
    print("[PE] Loading: ");
    println(path ? path : "(memory)");
    print("  ImageBase: ");
    print_hex(mod->preferred_base);
    print(", Size: ");
    print_hex(mod->image_size);
    println("");
    print("  EntryPoint: ");
    print_hex(mod->entry_point);
    println("");
    print("  Sections: ");
    print_dec(mod->section_count);
    println("");
    
    /* 分配内存并映射节 */
    /* 尝试在首选地址加载 */
    struct process *proc = process_get_current();
    
    /* Windows 程序通常在 0x140000000 附近 */
    uint64_t try_base = mod->preferred_base;
    if (try_base == 0) {
        try_base = 0x140000000ULL;
    }
    
    /* 检查地址是否可用 */
    /* 简化: 直接使用首选地址或重新分配 */
    static uint64_t next_pe_base = 0x7FF000000000ULL;
    
    mod->image_base = try_base;
    
    /* 如果首选地址不可用，使用新地址 */
    /* TODO: 实现地址空间检查 */
    
    /* 映射节到内存 */
    if (pe_map_sections(mod) != 0) {
        println("[PE] Failed to map sections");
        free_pe_module(mod);
        return NULL;
    }
    
    /* 节映射完成后，将数据目录 RVA 转换为 VA（始终需要，不论是否重定位） */
    if (mod->data_dirs[PE_DIR_IMPORT].VirtualAddress != 0) {
        mod->import_dir = (struct import_dir *)
            (mod->image_base + mod->data_dirs[PE_DIR_IMPORT].VirtualAddress);
    }
    if (mod->data_dirs[PE_DIR_EXPORT].VirtualAddress != 0) {
        mod->export_dir = (struct export_dir *)
            (mod->image_base + mod->data_dirs[PE_DIR_EXPORT].VirtualAddress);
    }
    if (mod->data_dirs[PE_DIR_BASERELOC].VirtualAddress != 0) {
        mod->reloc_dir = (struct reloc_block *)
            (mod->image_base + mod->data_dirs[PE_DIR_BASERELOC].VirtualAddress);
    }
    if (mod->data_dirs[PE_DIR_TLS].VirtualAddress != 0) {
        mod->tls_dir = (struct tls_dir *)
            (mod->image_base + mod->data_dirs[PE_DIR_TLS].VirtualAddress);
    }
    if (mod->data_dirs[PE_DIR_EXCEPTION].VirtualAddress != 0) {
        mod->exception_dir = (struct runtime_function *)
            (mod->image_base + mod->data_dirs[PE_DIR_EXCEPTION].VirtualAddress);
        mod->exception_count = mod->data_dirs[PE_DIR_EXCEPTION].Size / sizeof(struct runtime_function);
    }

    /* 处理重定位 */
    if (mod->image_base != mod->preferred_base) {
        pe_process_relocations(mod);
    }

    /* 分配用户空间 thunk 页（用于 IAT 调用约定转换） */
    pe_alloc_thunk_page(mod);

    /* 更新入口点 */
    mod->entry_point = mod->image_base + mod->opt_hdr->AddressOfEntryPoint;
    
    print("[PE] Loaded at ");
    print_hex(mod->image_base);
    print(", entry = ");
    print_hex(mod->entry_point);
    println("");
    
    return mod;
}

struct pe_module *pe_load_file(const char *path) {
    /* 检查是否已加载 */
    struct pe_module *existing = pe_find_module(path);
    if (existing != NULL) {
        return existing;
    }
    
    /* 读取文件 */
    size_t size;
    void *data = vfs_read_file(path, &size);
    if (data == NULL) {
        print("[PE] Failed to read: ");
        println(path);
        return NULL;
    }
    
    /* 加载模块 */
    struct pe_module *mod = pe_load_module(data, size, path);
    
    kfree(data);
    
    return mod;
}

int pe_unload_module(struct pe_module *mod) {
    if (mod == NULL) {
        return -1;
    }
    
    spin_lock(&pe_lock);
    
    mod->ref_count--;
    
    if (mod->ref_count <= 0) {
        /* 释放 PE 模块占用的内存页面 */
        struct process *proc = process_get_current();
        if (proc != NULL && proc->space != NULL && mod->image_base != 0) {
            uint64_t pages = (mod->image_size + PAGE_SIZE - 1) / PAGE_SIZE;
            for (uint64_t i = 0; i < pages; i++) {
                uint64_t vaddr = mod->image_base + i * PAGE_SIZE;
                uint64_t phys = vmm_get_physical(proc->space, vaddr);
                if (phys != 0) {
                    vmm_unmap_page(proc->space, vaddr);
                    pmm_free_page(phys);
                }
            }
        }

        /* 清除进程关联 */
        if (proc != NULL && proc->pe_mod == mod) {
            proc->pe_mod = NULL;
        }

        /* 释放 thunk 页 */
        if (proc != NULL && proc->space != NULL && mod->thunk_page != 0) {
            uint64_t thunk_phys = vmm_get_physical(proc->space, mod->thunk_page);
            if (thunk_phys != 0) {
                vmm_unmap_page(proc->space, mod->thunk_page);
                pmm_free_page(thunk_phys);
            }
        }

        mod->image_base = 0;
        mod->ref_count = 0;
        mod->path = NULL;
        mod->import_dir = NULL;
        mod->export_dir = NULL;
        mod->reloc_dir = NULL;
        mod->tls_dir = NULL;
        mod->exception_dir = NULL;
        mod->imports_resolved = 0;
        mod->reloc_processed = 0;
        mod->thunk_page = 0;
        mod->thunk_page_used = 0;
    }
    
    spin_unlock(&pe_lock);
    
    return 0;
}

/* ========== DLL 导入解析 ========== */

int pe_resolve_import(struct pe_module *mod, const char *dll_name,
                      const char *func_name, void **addr) {
    if (mod == NULL || dll_name == NULL || func_name == NULL || addr == NULL) {
        return -1;
    }
    
    /* 查找 DLL */
    struct pe_module *dll = pe_find_module(dll_name);
    if (dll == NULL) {
        dll = pe_load_file(dll_name);
    }
    
    /* 从 DLL 导出表查找 */
    if (dll != NULL) {
        *addr = pe_get_export(dll, func_name);
        if (*addr != NULL) {
            return 0;
        }
    }
    
    /* 从内置 DLL 查找 */
    *addr = find_builtin_export(dll_name, func_name, 0, 0);
    
    return (*addr != NULL) ? 0 : -1;
}

/* ========== TLS 支持 ========== */

int pe_init_thread_tls(struct pe_module *mod, void **tls_index) {
    if (mod == NULL || mod->tls_dir == NULL) {
        return -1;
    }
    
    struct tls_dir *tls = mod->tls_dir;
    
    /* 分配 TLS 索引 */
    /* TODO: 实现 TLS 索引分配 */
    
    /* 分配 TLS 数据块 */
    size_t tls_size = tls->EndAddressOfRawData - tls->StartAddressOfRawData;
    void *tls_data = kmalloc(tls_size + tls->SizeOfZeroFill);
    
    if (tls_data == NULL) {
        return -1;
    }
    
    /* 复制初始 TLS 数据 */
    void *src = (void *)(mod->image_base + tls->StartAddressOfRawData);
    for (size_t i = 0; i < tls_size; i++) {
        ((uint8_t *)tls_data)[i] = ((uint8_t *)src)[i];
    }
    
    /* 清零剩余部分 */
    for (size_t i = 0; i < tls->SizeOfZeroFill; i++) {
        ((uint8_t *)tls_data)[tls_size + i] = 0;
    }
    
    *tls_index = tls_data;
    
    return 0;
}

/* ========== SEH 异常处理 ========== */

int pe_register_exception_table(struct pe_module *mod) {
    if (mod == NULL || mod->exception_dir == NULL) {
        return -1;
    }

    /* 注册异常处理表到内核 */
    print("[PE] Registered ");
    print_dec(mod->exception_count);
    println(" exception handlers");

    /* 将异常表信息存储到进程结构中，供内核异常处理路径使用 */
    struct process *proc = process_get_current();
    if (proc != NULL && proc->pe_mod == mod) {
        /* 异常表已在 mod->exception_dir 和 mod->exception_count 中 */
        /* 内核异常处理路径（isr.c）可通过 proc->pe_mod 访问 */
        print("[PE] Exception table available at ");
        print_hex((uint64_t)mod->exception_dir);
        println("");
    }

    return 0;
}

/* 在 PE 异常表中查找给定 RIP 的运行时函数 */
struct runtime_function *pe_find_runtime_function(struct pe_module *mod, uint64_t rip) {
    if (mod == NULL || mod->exception_dir == NULL) {
        return NULL;
    }

    /* 计算 RVA */
    uint32_t rva = (uint32_t)(rip - mod->image_base);

    /* 二分查找运行时函数表（表按 BeginAddress 排序） */
    int low = 0, high = mod->exception_count - 1;
    while (low <= high) {
        int mid = (low + high) / 2;
        struct runtime_function *func = &mod->exception_dir[mid];

        if (rva < func->BeginAddress) {
            high = mid - 1;
        } else if (rva >= func->EndAddress) {
            low = mid + 1;
        } else {
            /* 找到匹配的函数 */
            return func;
        }
    }

    return NULL;
}

/* ========== SEH 异常展开 ========== */

/*
 * 展开码操作类型 (UWOP_*)
 * 参考: https://docs.microsoft.com/en-us/cpp/build/exception-handling-x64
 */
#define UWOP_PUSH_NONVOL        0   /* push 非易失寄存器 */
#define UWOP_ALLOC_LARGE        1   /* 大栈分配 */
#define UWOP_ALLOC_SMALL        2   /* 小栈分配 */
#define UWOP_SET_FPREG          3   /* 设置帧指针 */
#define UWOP_SAVE_NONVOL        4   /* 保存非易失寄存器 (带偏移) */
#define UWOP_SAVE_NONVOL_FAR    5   /* 保存非易失寄存器 (远偏移) */
#define UWOP_SAVE_XMM128        6   /* 保存 XMM128 */
#define UWOP_SAVE_XMM128_FAR    7   /* 保存 XMM128 (远偏移) */
#define UWOP_PUSH_MACHFRAME     8   /* push 机器帧 */

/*
 * pe_unwind_exception - 对 PE 程序执行 SEH 异常展开
 *
 * @mod: PE 模块
 * @rip: 异常发生的地址
 * @exception_code: 异常代码
 * @rsp_ptr: 指向当前 RSP 的指针 (输入/输出)
 * @rip_ptr: 指向当前 RIP 的指针 (输入/输出)
 *
 * 返回: 0 如果异常被处理, -1 如果未处理
 *
 * 此函数执行简化的 SEH 栈展开：
 * 1. 在异常表中查找 RIP 对应的运行时函数
 * 2. 解析展开码，计算栈帧大小
 * 3. 如果函数有异常处理器 (EHANDLER)，返回让调用者调用处理器
 * 4. 如果没有处理器，展开栈帧并继续搜索
 */
int pe_unwind_exception(struct pe_module *mod, uint64_t rip,
                        uint32_t exception_code, uint64_t *rsp_ptr, uint64_t *rip_ptr) {
    if (mod == NULL || mod->exception_dir == NULL) {
        return -1;
    }

    /* 查找 RIP 对应的运行时函数 */
    struct runtime_function *rf = pe_find_runtime_function(mod, rip);
    if (rf == NULL) {
        return -1; /* 没有找到对应的函数，无法展开 */
    }

    /* 获取展开信息 */
    struct unwind_info *ui = (struct unwind_info *)(mod->image_base + rf->UnwindInfoAddress);

    print("[PE] SEH unwind at RIP=");
    print_hex(rip);
    print(", func=[");
    print_hex(mod->image_base + rf->BeginAddress);
    print("-");
    print_hex(mod->image_base + rf->EndAddress);
    print("], flags=");
    print_dec(ui->Flags);
    println("");

    /* 计算栈帧调整量 */
    uint64_t rsp_adjust = 0;
    int has_frame_ptr = 0;

    /* 遍历展开码（从序言末尾到开头，即逆序） */
    for (int i = ui->CountOfUnwindCodes - 1; i >= 0; i--) {
        uint16_t code = ui->UnwindCode[i];
        uint8_t op = code & 0x0F;       /* 低 4 位: 操作码 */
        uint8_t info = (code >> 4) & 0x0F; /* 高 4 位: 操作信息 */

        switch (op) {
        case UWOP_PUSH_NONVOL:
            /* push 非易失寄存器: RSP -= 8 */
            rsp_adjust += 8;
            break;

        case UWOP_ALLOC_LARGE: {
            /* 大栈分配 */
            if (info == 0) {
                /* 下一个展开码包含分配大小 / 8 */
                if (i > 0) {
                    rsp_adjust += (uint64_t)ui->UnwindCode[i - 1] * 8;
                    i--; /* 跳过额外操作数 */
                }
            } else {
                /* 下两个展开码包含分配大小 */
                if (i > 1) {
                    uint64_t size = (uint64_t)ui->UnwindCode[i - 1] |
                                    ((uint64_t)ui->UnwindCode[i - 2] << 16);
                    rsp_adjust += size;
                    i -= 2;
                }
            }
            break;
        }

        case UWOP_ALLOC_SMALL:
            /* 小栈分配: 分配大小 = (info + 1) * 8 */
            rsp_adjust += (uint64_t)(info + 1) * 8;
            break;

        case UWOP_SET_FPREG:
            /* 设置帧指针: RSP = RBP - FrameOffset * 16 */
            has_frame_ptr = 1;
            break;

        case UWOP_SAVE_NONVOL:
            /* 保存非易失寄存器 (带偏移): 不调整 RSP，只跳过操作数 */
            i--; /* 跳过偏移操作数 */
            break;

        case UWOP_SAVE_NONVOL_FAR:
            /* 保存非易失寄存器 (远偏移) */
            i -= 2; /* 跳过两个偏移操作数 */
            break;

        case UWOP_SAVE_XMM128:
            /* 保存 XMM128 */
            i--; /* 跳过偏移操作数 */
            break;

        case UWOP_SAVE_XMM128_FAR:
            /* 保存 XMM128 (远偏移) */
            i -= 2;
            break;

        case UWOP_PUSH_MACHFRAME:
            /* push 机器帧: RSP -= 40 (5 * 8) */
            rsp_adjust += 40;
            break;
        }
    }

    /* 更新 RSP: 恢复到函数入口时的 RSP + 返回地址 */
    if (rsp_ptr != NULL) {
        *rsp_ptr = *rsp_ptr + rsp_adjust + 8; /* +8 跳过返回地址 */
    }

    /* 读取返回地址作为新的 RIP */
    if (rip_ptr != NULL && rsp_ptr != NULL) {
        /* 返回地址在当前 RSP 指向的位置 */
        uint64_t ret_addr_pos = *rsp_ptr - rsp_adjust - 8;
        *rip_ptr = *(uint64_t *)ret_addr_pos;
    }

    /* 检查是否有异常处理器 */
    if (ui->Flags & UNW_FLAG_EHANDLER) {
        /* 计算语言特定处理器地址 */
        /* 处理器地址在展开码数组之后，对齐到 4 字节 */
        int code_count = ui->CountOfUnwindCodes;
        int aligned_count = (code_count + 1) & ~1; /* 对齐到偶数 */
        uint32_t handler_rva = *(uint32_t *)&ui->UnwindCode[aligned_count];

        print("[PE] SEH handler at RVA=");
        print_hex(handler_rva);
        println("");

        /* 返回 1 表示找到了处理器，调用者负责调用它 */
        return 1;
    }

    /* 没有异常处理器，继续展开 */
    return 0;
}