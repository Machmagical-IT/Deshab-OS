/*
 * Zratro OS - Windows NTDLL/Kernel32 Compatibility Layer
 * 
 * 完整的 Windows API 模拟实现 (WPS 级别)
 */

#include "ntdll.h"
#include "console.h"
#include "task.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "pe.h"
#include "exec.h"
#include "scheduler.h"
#include "socket.h"
#include "net.h"

/* 前向声明 */
void *shell32_get_proc(const char *name);
void *comdlg32_get_proc(const char *name);

/* 避免 PAGE_SIZE 重定义警告 */
#ifdef PAGE_SIZE
#undef PAGE_SIZE
#endif
#define PAGE_SIZE 4096UL

/* ========== 全局变量 ========== */

static DWORD tls_last_error = 0;
static spinlock_t win_lock = SPINLOCK_INIT;

/* ========== Windows 句柄表 ========== */

#define MAX_WIN_HANDLES     1024
#define WIN_HANDLE_SHIFT    2

struct win_handle_entry {
    volatile int used;
    int type;
    uint64_t data;
    uint64_t extra;
    int ref_count;
    char name[256];
};

#define WIN_HANDLE_NONE      0
#define WIN_HANDLE_FILE      1
#define WIN_HANDLE_PROCESS   2
#define WIN_HANDLE_THREAD    3
#define WIN_HANDLE_MUTEX     4
#define WIN_HANDLE_EVENT     5
#define WIN_HANDLE_SEMAPHORE 6
#define WIN_HANDLE_HEAP      7
#define WIN_HANDLE_MODULE    8
#define WIN_HANDLE_FIND      9
#define WIN_HANDLE_REGKEY    10
#define WIN_HANDLE_SC        11
#define WIN_HANDLE_GDI       12
#define WIN_HANDLE_TIMER     13
#define WIN_HANDLE_HCRYPT    14
#define WIN_HANDLE_SOCKET    15
#define WIN_HANDLE_INET      16
#define WIN_HANDLE_INET_CONN 17
#define WIN_HANDLE_INET_REQ  18

static struct win_handle_entry win_handles[MAX_WIN_HANDLES];
static int win_handle_next = 4;

/* ========== TLS 存储表 ========== */

#define MAX_TLS_INDEX        64
#define WIN_MAX_THREADS      128

static int tls_in_use[MAX_TLS_INDEX];
static void *tls_values[WIN_MAX_THREADS][MAX_TLS_INDEX];
static spinlock_t tls_lock = SPINLOCK_INIT;

/* TLS 槽位占用跟踪 - 防止冲突 */
static int tls_slot_owner[WIN_MAX_THREADS];  /* 记录每个槽位被哪个 tid 占用 */
static int tls_slot_init = 0;

/* 初始化 TLS 槽位跟踪 */
static void tls_slot_ensure_init(void) {
    if (!tls_slot_init) {
        for (int i = 0; i < WIN_MAX_THREADS; i++) {
            tls_slot_owner[i] = 0;  /* 0 表示未占用 */
        }
        tls_slot_init = 1;
    }
}

/* 线程 ID 到 TLS 槽位的线程安全映射 - 带冲突检测 */
static int tls_tid_to_slot(int tid) {
    if (tid <= 0) return -1;
    
    spin_lock(&tls_lock);
    tls_slot_ensure_init();
    
    int slot = (tid - 1) % WIN_MAX_THREADS;
    
    /* 检查槽位是否已被其他线程占用 */
    if (tls_slot_owner[slot] != 0 && tls_slot_owner[slot] != tid) {
        /* 冲突：槽位已被占用，尝试找空闲槽位 */
        int found = -1;
        for (int i = 0; i < WIN_MAX_THREADS; i++) {
            if (tls_slot_owner[i] == 0) {
                found = i;
                break;
            }
        }
        
        if (found >= 0) {
            slot = found;
        } else {
            /* 没有空闲槽位，返回错误 */
            spin_unlock(&tls_lock);
            return -1;
        }
    }
    
    /* 标记槽位占用 */
    tls_slot_owner[slot] = tid;
    spin_unlock(&tls_lock);
    
    return slot;
}

/* 释放 TLS 槽位 (线程退出时调用) */
static void tls_release_slot(int tid) {
    if (tid <= 0) return;
    
    spin_lock(&tls_lock);
    
    for (int i = 0; i < WIN_MAX_THREADS; i++) {
        if (tls_slot_owner[i] == tid) {
            tls_slot_owner[i] = 0;
            /* 清除 TLS 值 */
            for (int j = 0; j < MAX_TLS_INDEX; j++) {
                tls_values[i][j] = NULL;
            }
            break;
        }
    }
    
    spin_unlock(&tls_lock);
}

/* 获取当前线程的 TLS 槽位，返回 -1 表示无效 */
static int tls_get_current_slot(void) {
    struct thread *t = thread_get_current();
    if (t == NULL) {
        /* 无法获取当前线程，这是一个严重错误 */
        return -1;
    }
    return tls_tid_to_slot(t->tid);
}

/* ========== SEH 异常处理链 ========== */

#define MAX_EXCEPTION_HANDLERS  256

struct seh_handler {
    int used;
    int vectored;
    ULONG first;
    PVECTORED_EXCEPTION_HANDLER handler;
};

static struct seh_handler seh_handlers[MAX_EXCEPTION_HANDLERS];
static LPTOP_LEVEL_EXCEPTION_FILTER unhandled_filter = NULL;
static spinlock_t seh_lock = SPINLOCK_INIT;

/* ========== 窗口/消息系统 ========== */

#define MAX_WINDOWS         256
#define MAX_MESSAGES        1024

struct win_window {
    int used;
    HWND hwnd;
    HWND parent;
    HINSTANCE instance;
    WNDPROC wndproc;
    DWORD style;
    DWORD exstyle;
    RECT rect;
    RECT client;
    char classname[64];
    char text[256];
    HMENU menu;
    int visible;
    int enabled;
    LONG_PTR userdata;
    LONG_PTR extra[8];
};

struct win_message {
    int used;
    HWND hwnd;
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
    DWORD time;
    POINT pt;
};

static struct win_window win_windows[MAX_WINDOWS];
static struct win_message win_messages[MAX_MESSAGES];
static int next_msg_read = 0;
static int next_msg_write = 0;
static HWND hwnd_next = (HWND)0x1000;
static spinlock_t win_lock_internal = SPINLOCK_INIT;

/* ========== GDI 对象表 ========== */

#define MAX_GDI_OBJECTS     512

struct gdi_object {
    int used;
    int type;
    int stock;
    COLORREF color;
    int width;
    int style;
    void *data;
    HDC hdc;
    struct {
        int width, height;
        int bpp;
        void *bits;
    } bitmap;
    struct {
        LOGFONTA lf;
    } font;
};

static struct gdi_object gdi_objects[MAX_GDI_OBJECTS];
static int next_gdi = (int)0x4000;
static spinlock_t gdi_lock = SPINLOCK_INIT;

/* 安全的 GDI 句柄生成，处理溢出回绕 */
static HGDIOBJ make_gdi_handle(int index) {
    /* next_gdi 使用低 16 位作为句柄唯一标识 */
    /* 确保在溢出后回绕，避免负数 */
    int handle_id = next_gdi & 0xFFFF;
    next_gdi = (next_gdi & 0xFFFF) + 1;
    if (next_gdi >= 0x10000) {
        next_gdi = 0x4000;  /* 回绕到初始值 */
    }
    return (HGDIOBJ)(handle_id + (index << 16));
}

/* ========== DC (设备上下文) 表 ========== */

#define MAX_DCS             64

struct win_dc {
    int used;
    HWND hwnd;
    int compatible;
    HRGN clip;
    HGDIOBJ cur_pen;
    HGDIOBJ cur_brush;
    HGDIOBJ cur_font;
    HGDIOBJ cur_bitmap;
    COLORREF text_color;
    COLORREF bk_color;
    int bk_mode;
    int map_mode;
    POINT pos;
};

static struct win_dc win_dcs[MAX_DCS];
static int next_dc = 1;
static spinlock_t dc_lock = SPINLOCK_INIT;

/* ========== 注册表模拟 ========== */

#define MAX_REG_KEYS        128
#define MAX_REG_VALUES      512
#define MAX_REG_DATA        4096

struct reg_value {
    int used;
    DWORD type;
    DWORD size;
    char name[64];
    BYTE data[1024];
};

struct reg_key {
    int used;
    HKEY hkey;
    HKEY parent;
    char name[64];
    int num_values;
    struct reg_value values[16];
};

static struct reg_key reg_keys[MAX_REG_KEYS];
static int next_reg_key = 0;
static spinlock_t reg_lock = SPINLOCK_INIT;

/* ========== 句柄分配函数 ========== */

static HANDLE alloc_handle(int type, uint64_t data, uint64_t extra) {
    spin_lock(&win_lock);
    
    for (int i = 0; i < MAX_WIN_HANDLES; i++) {
        int idx = (win_handle_next + i) % MAX_WIN_HANDLES;
        if (!win_handles[idx].used) {
            win_handles[idx].used = 1;
            win_handles[idx].type = type;
            win_handles[idx].data = data;
            win_handles[idx].extra = extra;
            win_handles[idx].ref_count = 1;
            win_handle_next = idx + 1;
            spin_unlock(&win_lock);
            return (HANDLE)(idx + 1);
        }
    }
    
    spin_unlock(&win_lock);
    return INVALID_HANDLE_VALUE;
}

static int lookup_handle(HANDLE h, int expected_type) {
    uint64_t idx = (uint64_t)h;
    if (idx == 0 || idx > MAX_WIN_HANDLES) return -1;
    
    int i = (int)(idx - 1);
    
    /* 添加锁保护，防止 SMP 环境下的竞态条件 */
    spin_lock(&win_lock);
    int used = win_handles[i].used;
    int type = win_handles[i].type;
    spin_unlock(&win_lock);
    
    if (!used) return -1;
    if (expected_type >= 0 && type != expected_type) return -1;
    
    return i;
}

/* ========== KERNEL32 文件操作实现 ========== */

DWORD WINAPI win_GetFileSize(HANDLE file, LPDWORD high) {
    int i = lookup_handle(file, WIN_HANDLE_FILE);
    if (i < 0) {
        win_SetLastError(ERROR_INVALID_HANDLE);
        return INVALID_FILE_SIZE;
    }
    
    struct vfs_file *vfile = (struct vfs_file *)win_handles[i].data;
    /* 简化：vfs 需要支持 size 查询 */
    if (high) *high = 0;
    return 0; /* 简化返回 */
}

BOOL WINAPI win_DeleteFileA(LPCSTR filename) {
    if (!filename) return FALSE;
    
    /* 使用 VFS 删除文件 */
    int ret = vfs_unlink(filename);
    if (ret < 0) {
        win_SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI win_MoveFileA(LPCSTR existing, LPCSTR newname) {
    if (!existing || !newname) return FALSE;
    
    int ret = vfs_rename(existing, newname);
    if (ret < 0) {
        win_SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI win_CopyFileA(LPCSTR existing, LPCSTR newname, BOOL failifexists) {
    if (!existing || !newname) return FALSE;
    
    /* 检查目标文件是否存在 */
    if (failifexists) {
        struct vfs_file *test = vfs_open(newname, VFS_MODE_READ);
        if (test) {
            vfs_close(test);
            win_SetLastError(ERROR_FILE_EXISTS);
            return FALSE;
        }
    }
    
    /* 打开源文件 */
    struct vfs_file *src = vfs_open(existing, VFS_MODE_READ);
    if (!src) {
        win_SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    
    /* 创建目标文件 */
    struct vfs_file *dst = vfs_open(newname, VFS_MODE_WRITE | 0x100);
    if (!dst) {
        vfs_close(src);
        win_SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    
    /* 复制内容 */
    char buffer[4096];
    ssize_t bytes_read;
    while ((bytes_read = vfs_read(src, buffer, sizeof(buffer))) > 0) {
        if (vfs_write(dst, buffer, bytes_read) < 0) {
            vfs_close(src);
            vfs_close(dst);
            win_SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
    }
    
    vfs_close(src);
    vfs_close(dst);
    return TRUE;
}

HANDLE WINAPI win_FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data) {
    (void)pattern;
    (void)data;
    /* 简化：需要实现目录遍历 */
    win_SetLastError(ERROR_NO_MORE_ITEMS);
    return INVALID_HANDLE_VALUE;
}

BOOL WINAPI win_FindNextFileA(HANDLE handle, LPWIN32_FIND_DATAA data) {
    (void)handle;
    (void)data;
    win_SetLastError(ERROR_NO_MORE_ITEMS);
    return FALSE;
}

BOOL WINAPI win_FindClose(HANDLE handle) {
    return win_CloseHandle(handle);
}

DWORD WINAPI win_GetFileAttributesA(LPCSTR filename) {
    if (!filename) return INVALID_FILE_ATTRIBUTES;
    
    struct vfs_file *file = vfs_open(filename, VFS_MODE_READ);
    if (!file) {
        return INVALID_FILE_ATTRIBUTES;
    }
    vfs_close(file);
    
    return FILE_ATTRIBUTE_NORMAL;
}

HANDLE WINAPI win_CreateFileA(LPCSTR filename, DWORD access, DWORD share,
                               LPSECURITY_ATTRIBUTES security, DWORD creation,
                               DWORD flags, HANDLE templ) {
    (void)security;
    (void)share;
    (void)flags;
    (void)templ;
    
    if (!filename) return INVALID_HANDLE_VALUE;
    
    /* 转换访问模式 */
    uint32_t mode = 0;
    if (access & GENERIC_READ) mode |= VFS_MODE_READ;
    if (access & GENERIC_WRITE) mode |= VFS_MODE_WRITE;
    if ((access & (GENERIC_READ | GENERIC_WRITE)) == (GENERIC_READ | GENERIC_WRITE)) {
        mode = VFS_MODE_READ | VFS_MODE_WRITE;
    }
    
    /* 转换创建模式 - 根据模式调整 */
    if (creation == CREATE_ALWAYS || creation == CREATE_NEW || creation == OPEN_ALWAYS) {
        mode |= 0x100; /* 创建标志 */
    }
    
    struct vfs_file *file = vfs_open(filename, mode);
    if (!file) {
        win_SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    
    HANDLE h = alloc_handle(WIN_HANDLE_FILE, (uint64_t)file, 0);
    if (h == INVALID_HANDLE_VALUE) {
        vfs_close(file);
        return INVALID_HANDLE_VALUE;
    }
    
    return h;
}

BOOL WINAPI win_ReadFile(HANDLE file, LPVOID buf, DWORD len, LPDWORD read, LPOVERLAPPED overlapped) {
    (void)overlapped;
    
    int i = lookup_handle(file, WIN_HANDLE_FILE);
    if (i < 0) {
        win_SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    
    struct vfs_file *vfile = (struct vfs_file *)win_handles[i].data;
    ssize_t n = vfs_read(vfile, buf, len);
    
    if (n < 0) {
        win_SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    
    if (read) *read = (DWORD)n;
    return TRUE;
}

BOOL WINAPI win_WriteFile(HANDLE file, LPCVOID buf, DWORD len, LPDWORD written, LPOVERLAPPED overlapped) {
    (void)overlapped;
    
    int i = lookup_handle(file, WIN_HANDLE_FILE);
    if (i < 0) {
        win_SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    
    struct vfs_file *vfile = (struct vfs_file *)win_handles[i].data;
    ssize_t n = vfs_write(vfile, (void*)buf, len);
    
    if (n < 0) {
        win_SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    
    if (written) *written = (DWORD)n;
    return TRUE;
}

BOOL WINAPI win_CloseHandle(HANDLE handle) {
    int i = lookup_handle(handle, -1);
    if (i < 0) {
        win_SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    
    switch (win_handles[i].type) {
        case WIN_HANDLE_FILE:
            vfs_close((struct vfs_file *)win_handles[i].data);
            break;
        case WIN_HANDLE_REGKEY:
            win_RegCloseKey((HKEY)handle);
            break;
        case WIN_HANDLE_SOCKET:
            win_closesocket((SOCKET)handle);
            return TRUE;
        case WIN_HANDLE_INET_REQ:
            if (win_handles[i].extra) win_closesocket((SOCKET)win_handles[i].extra);
            break;
        default:
            break;
    }

    win_handles[i].used = 0;
    win_handles[i].ref_count = 0;
    
    return TRUE;
}

/* ========== KERNEL32 进程/线程操作实现 ========== */

BOOL WINAPI win_CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES proc_attr,
                                LPSECURITY_ATTRIBUTES thread_attr, BOOL inherit,
                                DWORD flags, LPVOID env, LPCSTR cwd,
                                LPSTARTUPINFOA startup, LPPROCESS_INFORMATION info) {
    (void)proc_attr;
    (void)thread_attr;
    (void)inherit;
    (void)flags;
    (void)env;
    (void)cwd;
    (void)startup;
    
    if (!app && !cmd) return FALSE;
    
    /* 使用 app 或 cmd 的第一部分作为路径 */
    const char *path = app ? app : cmd;
    
    /* 使用 exec 子系统创建进程 */
    struct process_image image;
    memset(&image, 0, sizeof(image));
    
    if (build_process_image(path, &image) != 0) {
        win_SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    
    /* 创建进程 */
    struct process *proc = process_create(path, image.entry_point);
    if (!proc) {
        win_SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    
    proc->abi = image.abi;
    
    /* 创建主线程 */
    struct thread *thread = thread_create(proc, "main", image.entry_point, NULL);
    if (!thread) {
        win_SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    
    thread->user_stack = image.stack_top;
    thread->stack_size = image.stack_size;
    scheduler_add_thread(thread);
    
    if (info) {
        info->dwProcessId = (DWORD)proc->pid;
        info->dwThreadId = (DWORD)thread->tid;
        info->hProcess = alloc_handle(WIN_HANDLE_PROCESS, (uint64_t)proc, 0);
        info->hThread = alloc_handle(WIN_HANDLE_THREAD, (uint64_t)thread, 0);
    }
    
    return TRUE;
}

BOOL WINAPI win_TerminateProcess(HANDLE process, DWORD exit_code) {
    (void)process;
    (void)exit_code;
    /* 简化：终止当前进程 */
    win_ExitProcess(exit_code);
    return TRUE;
}

DWORD WINAPI win_GetCurrentProcessId(void) {
    struct thread *t = thread_get_current();
    return t ? (DWORD)t->pid : 0;
}

DWORD WINAPI win_GetCurrentThreadId(void) {
    struct thread *t = thread_get_current();
    return t ? (DWORD)t->tid : 0;
}

HANDLE WINAPI win_GetCurrentProcess(void) {
    return (HANDLE)0xFFFFFFFFFFFFFFFE;
}

HANDLE WINAPI win_GetCurrentThread(void) {
    return (HANDLE)0xFFFFFFFFFFFFFFFD;
}

void WINAPI win_ExitProcess(UINT exit_code) {
    process_exit((int)exit_code);
}

void WINAPI win_ExitThread(DWORD exit_code) {
    thread_exit((int)exit_code);
}

DWORD WINAPI win_GetLastError(void) {
    return tls_last_error;
}

void WINAPI win_SetLastError(DWORD error) {
    tls_last_error = error;
}

HANDLE WINAPI win_CreateThread(LPSECURITY_ATTRIBUTES attr, size_t stack,
                                void *start, void *param, DWORD flags, LPDWORD tid) {
    (void)attr;
    (void)stack;
    (void)flags;
    
    struct process *proc = process_get_current();
    if (!proc) return NULL_HANDLE;
    
    struct thread *thread = thread_create(proc, "win_thread", (uint64_t)start, param);
    if (!thread) return NULL_HANDLE;
    
    HANDLE h = alloc_handle(WIN_HANDLE_THREAD, (uint64_t)thread, 0);
    if (tid) *tid = (DWORD)thread->tid;
    
    return h;
}

DWORD WINAPI win_ResumeThread(HANDLE thread) {
    int i = lookup_handle(thread, WIN_HANDLE_THREAD);
    if (i < 0) return 0xFFFFFFFF;
    
    struct thread *t = (struct thread *)win_handles[i].data;
    if (t) {
        thread_set_state(t, THREAD_STATE_READY);
        return 0;
    }
    
    return 0xFFFFFFFF;
}

DWORD WINAPI win_SuspendThread(HANDLE thread) {
    int i = lookup_handle(thread, WIN_HANDLE_THREAD);
    if (i < 0) return 0xFFFFFFFF;
    
    struct thread *t = (struct thread *)win_handles[i].data;
    if (t) {
        thread_set_state(t, THREAD_STATE_BLOCKED);
        return 0;
    }
    
    return 0xFFFFFFFF;
}

/* ========== KERNEL32 内存操作实现 ========== */

/* 用户态虚拟地址分配器：递增式地址分配，避免固定地址冲突 */
static uint64_t next_user_virt = 0;
#define WIN_HEAP_BASE  (USER_BASE + 0x10000000ULL)
#define WIN_HEAP_LIMIT (USER_BASE + 0x70000000ULL)

LPVOID WINAPI win_VirtualAlloc(LPVOID addr, size_t size, DWORD type, DWORD protect) {
    (void)protect;
    
    if (!(type & MEM_COMMIT) && !(type & MEM_RESERVE)) {
        return NULL;
    }
    if (size == 0) return NULL;
    
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == 0) return NULL;
    
    struct address_space *space = vmm_get_current_space();
    if (!space) {
        pmm_free_pages(phys, pages);
        return NULL;
    }
    
    uint64_t virt;
    if (addr) {
        /* 用户指定地址 */
        virt = (uint64_t)addr;
    } else {
        /* 递增分配虚拟地址 */
        if (next_user_virt == 0) {
            next_user_virt = WIN_HEAP_BASE;
        }
        virt = next_user_virt;
        next_user_virt += pages * PAGE_SIZE;
        if (next_user_virt >= WIN_HEAP_LIMIT) {
            pmm_free_pages(phys, pages);
            return NULL;
        }
    }
    
    if (vmm_map_range(space, virt, phys, pages, VMM_FLAGS_USER) != 0) {
        pmm_free_pages(phys, pages);
        return NULL;
    }
    
    /* 零初始化 */
    memset((void *)virt, 0, pages * PAGE_SIZE);
    
    return (LPVOID)virt;
}

BOOL WINAPI win_VirtualFree(LPVOID addr, size_t size, DWORD type) {
    if (addr == NULL) return FALSE;
    (void)type;
    
    struct address_space *space = vmm_get_current_space();
    if (!space) return FALSE;
    
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;
    
    /* 解除映射并释放物理页 */
    uint64_t virt = (uint64_t)addr;
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = vmm_get_physical(space, virt + i * PAGE_SIZE);
        if (phys != 0) {
            vmm_unmap_page(space, virt + i * PAGE_SIZE);
            pmm_free_page(phys);
        }
    }
    
    return TRUE;
}

/*
 * HeapAlloc/HeapFree 使用 VirtualAlloc 而非 kmalloc，
 * 确保用户态 Windows 程序不直接操作内核堆。
 */
LPVOID WINAPI win_HeapAlloc(HANDLE heap, DWORD flags, size_t size) {
    (void)heap;
    DWORD alloc_type = MEM_COMMIT | MEM_RESERVE;
    LPVOID ptr = win_VirtualAlloc(NULL, size, alloc_type, 0);
    if (ptr && (flags & 0x00000008)) {  /* HEAP_ZERO_MEMORY */
        memset(ptr, 0, size);
    }
    return ptr;
}

BOOL WINAPI win_HeapFree(HANDLE heap, DWORD flags, LPVOID mem) {
    (void)heap;
    (void)flags;
    if (mem == NULL) return TRUE;
    /* 释放一页，VirtualFree 会处理实际解映射 */
    return win_VirtualFree(mem, PAGE_SIZE, 0);
}

LPVOID WINAPI win_HeapCreate(DWORD options, size_t init, size_t max) {
    (void)options;
    (void)init;
    (void)max;
    return (LPVOID)1; /* 返回伪句柄 */
}

/* ========== KERNEL32 模块操作实现 ========== */

HMODULE WINAPI win_LoadLibraryA(LPCSTR name) {
    if (!name) return NULL_HANDLE;
    
    /* 先尝试在已知 DLL 中查找 */
    void *proc = kernel32_get_proc(name);
    if (proc) return (HMODULE)0x77E40000; /* kernel32 基址 */
    
    proc = user32_get_proc(name);
    if (proc) return (HMODULE)0x77D10000; /* user32 基址 */
    
    proc = gdi32_get_proc(name);
    if (proc) return (HMODULE)0x77EF0000; /* gdi32 基址 */
    
    proc = advapi32_get_proc(name);
    if (proc) return (HMODULE)0x77DD0000; /* advapi32 基址 */
    
    win_SetLastError(ERROR_MOD_NOT_FOUND);
    return NULL_HANDLE;
}

HMODULE WINAPI win_LoadLibraryW(LPCWSTR name) {
    if (!name) return NULL_HANDLE;
    /* 简化：转成 ANSI */
    char buf[256];
    int i;
    for (i = 0; i < 255 && name[i]; i++) {
        buf[i] = (char)name[i];
    }
    buf[i] = '\0';
    return win_LoadLibraryA(buf);
}

BOOL WINAPI win_FreeLibrary(HMODULE module) {
    (void)module;
    return TRUE;
}

/* 前向声明 */
void *shell32_get_proc(const char *name);
void *comdlg32_get_proc(const char *name);
void *ws2_32_get_proc(const char *name);
void *iphlpapi_get_proc(const char *name);
void *dnsapi_get_proc(const char *name);
void *wininet_get_proc(const char *name);
void *winhttp_get_proc(const char *name);
void *mswsock_get_proc(const char *name);

FARPROC WINAPI win_GetProcAddress(HMODULE module, LPCSTR name) {
    (void)module;
    
    if ((uint64_t)name < 0x10000) {
        /* 按序号查找 */
        return NULL;
    }
    
    /* 在各个 DLL 导出表中查找 */
    void *proc = kernel32_get_proc(name);
    if (proc) return (FARPROC)proc;
    
    proc = user32_get_proc(name);
    if (proc) return (FARPROC)proc;
    
    proc = gdi32_get_proc(name);
    if (proc) return (FARPROC)proc;
    
    proc = advapi32_get_proc(name);
    if (proc) return (FARPROC)proc;
    
    proc = ntdll_get_proc(name);
    if (proc) return (FARPROC)proc;

    proc = ole32_get_proc(name);
    if (proc) return (FARPROC)proc;

    proc = shell32_get_proc(name);
    if (proc) return (FARPROC)proc;

    proc = comdlg32_get_proc(name);
    if (proc) return (FARPROC)proc;

    proc = ws2_32_get_proc(name);
    if (proc) return (FARPROC)proc;

    proc = iphlpapi_get_proc(name);
    if (proc) return (FARPROC)proc;

    proc = dnsapi_get_proc(name);
    if (proc) return (FARPROC)proc;

    proc = wininet_get_proc(name);
    if (proc) return (FARPROC)proc;

    proc = winhttp_get_proc(name);
    if (proc) return (FARPROC)proc;

    proc = mswsock_get_proc(name);
    if (proc) return (FARPROC)proc;

    return NULL;
}

HMODULE WINAPI win_GetModuleHandleA(LPCSTR name) {
    if (!name) return (HMODULE)0x10000000; /* 主模块伪地址 */
    return win_LoadLibraryA(name);
}

HMODULE WINAPI win_GetModuleHandleW(LPCWSTR name) {
    if (!name) return (HMODULE)0x10000000;
    return win_LoadLibraryW(name);
}

DWORD WINAPI win_GetModuleFileNameA(HMODULE module, LPSTR buf, DWORD size) {
    if (!buf || size == 0) return 0;
    
    if (module == NULL_HANDLE || module == (HMODULE)0x10000000) {
        const char *main = "C:\\Program Files\\App\\App.exe";
        size_t len = strlen(main);
        if (len >= size) len = size - 1;
        memcpy(buf, main, len);
        buf[len] = '\0';
        return (DWORD)len;
    }
    
    /* 返回一个默认路径 */
    const char *dll_path = "C:\\Windows\\System32\\unknown.dll";
    size_t len = strlen(dll_path);
    if (len >= size) len = size - 1;
    memcpy(buf, dll_path, len);
    buf[len] = '\0';
    
    return (DWORD)len;
}

/* ========== KERNEL32 同步操作实现 ========== */

void WINAPI win_Sleep(DWORD ms) {
    thread_sleep(ms);
}

HANDLE WINAPI win_CreateEventA(LPSECURITY_ATTRIBUTES attr, BOOL manual, BOOL init, LPCSTR name) {
    (void)attr;
    (void)manual;
    (void)init;
    (void)name;
    return alloc_handle(WIN_HANDLE_EVENT, init ? 1 : 0, manual ? 1 : 0);
}

BOOL WINAPI win_SetEvent(HANDLE event) {
    int i = lookup_handle(event, WIN_HANDLE_EVENT);
    if (i < 0) return FALSE;
    
    win_handles[i].data = 1;
    return TRUE;
}

BOOL WINAPI win_ResetEvent(HANDLE event) {
    int i = lookup_handle(event, WIN_HANDLE_EVENT);
    if (i < 0) return FALSE;
    
    win_handles[i].data = 0;
    return TRUE;
}

HANDLE WINAPI win_CreateMutexA(LPSECURITY_ATTRIBUTES attr, BOOL owner, LPCSTR name) {
    (void)attr;
    (void)owner;
    (void)name;
    return alloc_handle(WIN_HANDLE_MUTEX, 0, 0);
}

BOOL WINAPI win_ReleaseMutex(HANDLE mutex) {
    int i = lookup_handle(mutex, WIN_HANDLE_MUTEX);
    if (i < 0) return FALSE;
    
    win_handles[i].data = 0;
    return TRUE;
}

DWORD WINAPI win_WaitForSingleObject(HANDLE handle, DWORD ms) {
    int i = lookup_handle(handle, -1);
    if (i < 0) return WAIT_FAILED;
    
    uint64_t start = get_tick_count();
    
    while (1) {
        switch (win_handles[i].type) {
            case WIN_HANDLE_EVENT:
                if (win_handles[i].data) return WAIT_OBJECT_0;
                break;
            case WIN_HANDLE_MUTEX:
                if (win_handles[i].data == 0) {
                    win_handles[i].data = 1;
                    return WAIT_OBJECT_0;
                }
                break;
            case WIN_HANDLE_THREAD:
            case WIN_HANDLE_PROCESS: {
                struct thread *t = (struct thread *)win_handles[i].data;
                if (t && t->state == THREAD_STATE_TERMINATED) return WAIT_OBJECT_0;
                break;
            }
            default:
                return WAIT_FAILED;
        }
        
        if (ms != INFINITE) {
            uint64_t elapsed = get_tick_count() - start;
            if (elapsed >= ms) return WAIT_TIMEOUT;
        }
        
        thread_sleep(1);
    }
}

/* ========== KERNEL32 系统信息实现 ========== */

void WINAPI win_GetSystemInfo(LPSYSTEM_INFO info) {
    if (!info) return;
    
    info->dwPageSize = PAGE_SIZE;
    info->lpMinimumApplicationAddress = (LPVOID)0x10000;
    info->lpMaximumApplicationAddress = (LPVOID)0x7FFFFFFFFFFF;
    info->dwActiveProcessorMask = 1;
    info->dwNumberOfProcessors = 1;
    info->dwProcessorType = 8664;
    info->dwAllocationGranularity = 65536;
    info->wProcessorLevel = 1;
    info->wProcessorRevision = 0;
}

DWORD WINAPI win_GetTickCount(void) {
    return (DWORD)get_tick_count();
}

ULONGLONG WINAPI win_GetTickCount64(void) {
    return get_tick_count();
}

DWORD WINAPI win_GetCurrentDirectoryA(DWORD size, LPSTR buf) {
    /* 简化实现：返回根目录 */
    const char *cwd = "/";
    
    size_t len = strlen(cwd);
    if (len >= size) len = size - 1;
    memcpy(buf, cwd, len);
    buf[len] = '\0';
    
    return (DWORD)len;
}

BOOL WINAPI win_SetCurrentDirectoryA(LPCSTR path) {
    (void)path;
    /* 简化实现：总是成功 */
    return TRUE;
}

/* ========== KERNEL32 环境和命令行实现 ========== */

static char win_cmdline[1024] = "";
static char win_computername[64] = "ZRATRO";
static char win_username[64] = "user";

LPSTR WINAPI win_GetCommandLineA(void) {
    if (win_cmdline[0] == '\0') {
        win_cmdline[0] = 'a'; win_cmdline[1] = 'p'; win_cmdline[2] = 'p'; win_cmdline[3] = '\0';
    }
    return win_cmdline;
}

LPWSTR WINAPI win_GetCommandLineW(void) {
    static WCHAR wcmdline[1024];
    char *cmd = win_GetCommandLineA();
    for (int i = 0; cmd[i]; i++) {
        wcmdline[i] = (WCHAR)cmd[i];
    }
    return wcmdline;
}

BOOL WINAPI win_GetComputerNameA(LPSTR buf, LPDWORD size) {
    if (!buf || !size) return FALSE;
    
    size_t len = strlen(win_computername);
    if (*size < len + 1) {
        *size = (DWORD)(len + 1);
        return FALSE;
    }
    
    memcpy(buf, win_computername, len + 1);
    *size = (DWORD)len;
    return TRUE;
}

BOOL WINAPI win_GetComputerNameW(LPWSTR buf, LPDWORD size) {
    if (!buf || !size) return FALSE;
    
    size_t len = strlen(win_computername);
    if (*size < len + 1) {
        *size = (DWORD)(len + 1);
        return FALSE;
    }
    
    for (size_t i = 0; i <= len; i++) {
        buf[i] = (WCHAR)win_computername[i];
    }
    *size = (DWORD)len;
    return TRUE;
}

BOOL WINAPI win_GetUserNameA(LPSTR buf, LPDWORD size) {
    if (!buf || !size) return FALSE;
    
    size_t len = strlen(win_username);
    if (*size < len + 1) {
        *size = (DWORD)(len + 1);
        return FALSE;
    }
    
    memcpy(buf, win_username, len + 1);
    *size = (DWORD)len;
    return TRUE;
}

BOOL WINAPI win_GetUserNameW(LPWSTR buf, LPDWORD size) {
    if (!buf || !size) return FALSE;
    
    size_t len = strlen(win_username);
    if (*size < len + 1) {
        *size = (DWORD)(len + 1);
        return FALSE;
    }
    
    for (size_t i = 0; i <= len; i++) {
        buf[i] = (WCHAR)win_username[i];
    }
    *size = (DWORD)len;
    return TRUE;
}

void WINAPI win_GetStartupInfoA(LPSTARTUPINFOA info) {
    if (!info) return;
    
    memset(info, 0, sizeof(*info));
    info->cb = sizeof(STARTUPINFOA);
    info->hStdInput = (HANDLE)STD_INPUT_HANDLE;
    info->hStdOutput = (HANDLE)STD_OUTPUT_HANDLE;
    info->hStdError = (HANDLE)STD_ERROR_HANDLE;
    info->wShowWindow = SW_SHOWNORMAL;
}

void WINAPI win_GetStartupInfoW(LPSTARTUPINFOW info) {
    if (!info) return;
    
    memset(info, 0, sizeof(*info));
    info->cb = sizeof(STARTUPINFOW);
    info->hStdInput = (HANDLE)STD_INPUT_HANDLE;
    info->hStdOutput = (HANDLE)STD_OUTPUT_HANDLE;
    info->hStdError = (HANDLE)STD_ERROR_HANDLE;
    info->wShowWindow = SW_SHOWNORMAL;
}

/* ========== KERNEL32 内存查询实现 ========== */

SIZE_T WINAPI win_VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION info, SIZE_T len) {
    if (!info || len < sizeof(MEMORY_BASIC_INFORMATION)) return 0;
    
    /* 简化实现 */
    memset(info, 0, sizeof(MEMORY_BASIC_INFORMATION));
    info->BaseAddress = (PVOID)addr;
    info->AllocationBase = (PVOID)((uint64_t)addr & ~0xFFFFFULL);
    info->RegionSize = 0x10000;
    info->State = MEM_COMMIT;
    info->Protect = PAGE_READWRITE;
    info->Type = MEM_PRIVATE;
    
    return sizeof(MEMORY_BASIC_INFORMATION);
}

/*
 * TODO: 未实现 - VirtualProtect 页面保护功能尚未完成
 * 注意：此函数忽略所有参数，始终返回 TRUE。
 * Windows 程序调用此函数设置内存保护（如 PAGE_EXECUTE_READ）会"成功"，
 * 但实际页表保护位不会改变，W^X 策略无法实施。
 */
BOOL WINAPI win_VirtualProtect(LPVOID addr, SIZE_T size, DWORD new_prot, LPDWORD old_prot) {
    (void)addr;
    (void)size;
    (void)new_prot;
    if (old_prot) *old_prot = PAGE_READWRITE;
    return TRUE;  /* 始终返回成功但实际未改变任何保护 */
}

/* ========== KERNEL32 文件时间转换 ========== */

BOOL WINAPI win_FileTimeToLocalFileTime(const FILETIME *ft, LPFILETIME lft) {
    if (!ft || !lft) return FALSE;
    *lft = *ft;
    return TRUE;
}

BOOL WINAPI win_LocalFileTimeToFileTime(const FILETIME *lft, LPFILETIME ft) {
    if (!lft || !ft) return FALSE;
    *ft = *lft;
    return TRUE;
}

void WINAPI win_GetSystemTimeAsFileTime(LPFILETIME ft) {
    if (!ft) return;
    
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    
    /* 转换为 100 纳秒间隔 */
    uint64_t ns = tsc / 3;  /* 假设 3GHz TSC */
    uint64_t ft_val = ns / 100 + 0x01B21DD213814000ULL;  /* 从 1601 开始 */
    
    ft->dwLowDateTime = (DWORD)ft_val;
    ft->dwHighDateTime = (DWORD)(ft_val >> 32);
}

DWORD WINAPI win_GetTempPathA(DWORD size, LPSTR buf) {
    const char *temp = "/tmp";
    size_t len = strlen(temp);
    
    if (buf == NULL) return (DWORD)(len + 1);
    if (size < len + 1) return (DWORD)(len + 1);
    
    memcpy(buf, temp, len + 1);
    return (DWORD)len;
}

DWORD WINAPI win_GetTempFileNameA(LPCSTR path, LPCSTR prefix, UINT unique, LPSTR buf) {
    (void)path;
    (void)prefix;
    (void)unique;
    if (buf) {
        buf[0] = 't'; buf[1] = 'm'; buf[2] = 'p'; buf[3] = '\0';
    }
    return 1;
}

/* ========== KERNEL32 字符串操作实现 ========== */

int WINAPI win_lstrcmpA(LPCSTR s1, LPCSTR s2) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    return strcmp(s1, s2);
}

int WINAPI win_lstrcmpiA(LPCSTR s1, LPCSTR s2) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    
    /* 简单的不区分大小写比较 */
    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return (int)(unsigned char)c1 - (int)(unsigned char)c2;
        s1++;
        s2++;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

LPSTR WINAPI win_lstrcpyA(LPSTR dest, LPCSTR src) {
    if (!dest) return NULL;
    if (!src) return dest;
    return strcpy(dest, src);
}

int WINAPI win_lstrlenA(LPCSTR s) {
    if (!s) return 0;
    return (int)strlen(s);
}

/* ========== KERNEL32 控制台操作实现 ========== */

HANDLE WINAPI win_GetStdHandle(DWORD std) {
    switch (std) {
        case STD_INPUT_HANDLE:
            return (HANDLE)0; /* stdin */
        case STD_OUTPUT_HANDLE:
            return (HANDLE)1; /* stdout */
        case STD_ERROR_HANDLE:
            return (HANDLE)2; /* stderr */
        default:
            return INVALID_HANDLE_VALUE;
    }
}

BOOL WINAPI win_SetStdHandle(DWORD std, HANDLE handle) {
    (void)std;
    (void)handle;
    /* 简化：暂不支持重定向 */
    win_SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

DWORD WINAPI win_GetFileType(HANDLE file) {
    int i = lookup_handle(file, -1);
    if (i < 0) return 0; /* FILE_TYPE_UNKNOWN */
    
    switch (win_handles[i].type) {
        case WIN_HANDLE_FILE:
            return 1; /* FILE_TYPE_DISK */
        case WIN_HANDLE_EVENT:
        case WIN_HANDLE_MUTEX:
            return 3; /* FILE_TYPE_PIPE */
        default:
            return 2; /* FILE_TYPE_CHAR */
    }
}

BOOL WINAPI win_WriteConsoleA(HANDLE handle, LPCVOID buf, DWORD len, LPDWORD written, LPVOID reserved) {
    (void)reserved;
    
    if (!buf || len == 0) return FALSE;
    
    /* 简化：直接输出到串口 */
    char temp[256];
    DWORD to_write = (len < 255) ? len : 255;
    memcpy(temp, buf, to_write);
    temp[to_write] = '\0';
    
    serial_print(temp);
    
    if (written) *written = to_write;
    return TRUE;
}

BOOL WINAPI win_ReadConsoleA(HANDLE handle, LPVOID buf, DWORD len, LPDWORD read, LPVOID reserved) {
    (void)handle;
    (void)buf;
    (void)len;
    (void)read;
    (void)reserved;
    /* 简化：暂不支持控制台输入 */
    win_SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* ========== KERNEL32 环境变量实现 ========== */

#define MAX_ENV_VARS 256
#define MAX_ENV_NAME 64
#define MAX_ENV_VALUE 512

static struct {
    char name[MAX_ENV_NAME];
    char value[MAX_ENV_VALUE];
    int used;
} env_vars[MAX_ENV_VARS];

static spinlock_t env_lock = SPINLOCK_INIT;

DWORD WINAPI win_GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size) {
    if (!name) return 0;
    
    spin_lock(&env_lock);
    
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used && strcmp(env_vars[i].name, name) == 0) {
            DWORD len = (DWORD)strlen(env_vars[i].value);
            if (buf && size > len) {
                memcpy(buf, env_vars[i].value, len + 1);
            }
            spin_unlock(&env_lock);
            return len;
        }
    }
    
    spin_unlock(&env_lock);
    return 0; /* 未找到 */
}

BOOL WINAPI win_SetEnvironmentVariableA(LPCSTR name, LPCSTR value) {
    if (!name) return FALSE;
    
    spin_lock(&env_lock);
    
    /* 查找现有变量 */
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used && strcmp(env_vars[i].name, name) == 0) {
            if (value) {
                strncpy(env_vars[i].value, value, MAX_ENV_VALUE - 1);
            } else {
                /* 值为 NULL 则删除 */
                env_vars[i].used = 0;
            }
            spin_unlock(&env_lock);
            return TRUE;
        }
    }
    
    /* 添加新变量 */
    if (value) {
        for (int i = 0; i < MAX_ENV_VARS; i++) {
            if (!env_vars[i].used) {
                strncpy(env_vars[i].name, name, MAX_ENV_NAME - 1);
                strncpy(env_vars[i].value, value, MAX_ENV_VALUE - 1);
                env_vars[i].used = 1;
                spin_unlock(&env_lock);
                return TRUE;
            }
        }
    }
    
    spin_unlock(&env_lock);
    return FALSE;
}

/* ========== KERNEL32 其他实现 ========== */

DWORD WINAPI win_GetVersion(void) {
    return 0x0A00; /* Windows 10 */
}

void WINAPI win_OutputDebugStringA(LPCSTR str) {
    if (str) {
        serial_print("[DEBUG] ");
        serial_print(str);
        serial_print("\n");
    }
}

BOOL WINAPI win_IsDebuggerPresent(void) {
    return FALSE; /* 无调试器 */
}

void WINAPI win_DebugBreak(void) {
    asm volatile("int $3");
}

/* ========== KERNEL32 动态分配实现 ========== */

HGLOBAL WINAPI win_GlobalAlloc(UINT flags, size_t size) {
    (void)flags;
    return (HGLOBAL)kmalloc(size);
}

HGLOBAL WINAPI win_GlobalFree(HGLOBAL mem) {
    kfree((void*)mem);
    return NULL_HANDLE;
}

size_t WINAPI win_GlobalSize(HGLOBAL mem) {
    (void)mem;
    return 0; /* 无法查询 */
}

LPVOID WINAPI win_GlobalLock(HGLOBAL mem) {
    return (LPVOID)mem;
}

BOOL WINAPI win_GlobalUnlock(HGLOBAL mem) {
    (void)mem;
    return TRUE;
}

/* ========== SEH 异常处理实现 ========== */

void WINAPI win_RaiseException(DWORD code, DWORD flags, DWORD count, ULONG_PTR *args) {
    (void)flags;
    (void)count;
    (void)args;
    
    EXCEPTION_RECORD record = {0};
    record.ExceptionCode = code;
    
    /* 查找向量异常处理器 */
    spin_lock(&seh_lock);
    for (int i = 0; i < MAX_EXCEPTION_HANDLERS; i++) {
        if (seh_handlers[i].used && seh_handlers[i].vectored) {
            EXCEPTION_POINTERS ep = { &record, NULL };
            LONG result = seh_handlers[i].handler(&ep);
            if (result == EXCEPTION_CONTINUE_EXECUTION) {
                spin_unlock(&seh_lock);
                return;
            }
        }
    }
    spin_unlock(&seh_lock);
    
    /* 调用未处理异常过滤器 */
    if (unhandled_filter) {
        EXCEPTION_POINTERS ep = { &record, NULL };
        unhandled_filter(&ep);
    }
    
    /* 终止进程 */
    win_ExitProcess(code);
}

PVOID WINAPI win_AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler) {
    spin_lock(&seh_lock);
    
    for (int i = 0; i < MAX_EXCEPTION_HANDLERS; i++) {
        if (!seh_handlers[i].used) {
            seh_handlers[i].used = 1;
            seh_handlers[i].vectored = 1;
            seh_handlers[i].first = first;
            seh_handlers[i].handler = handler;
            spin_unlock(&seh_lock);
            return (PVOID)(i + 1);
        }
    }
    
    spin_unlock(&seh_lock);
    return NULL;
}

ULONG WINAPI win_RemoveVectoredExceptionHandler(PVOID handle) {
    uint64_t idx = (uint64_t)handle;
    if (idx == 0 || idx > MAX_EXCEPTION_HANDLERS) return 0;
    
    spin_lock(&seh_lock);
    seh_handlers[idx - 1].used = 0;
    spin_unlock(&seh_lock);
    
    return 1;
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI win_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    spin_lock(&seh_lock);
    LPTOP_LEVEL_EXCEPTION_FILTER old = unhandled_filter;
    unhandled_filter = filter;
    spin_unlock(&seh_lock);
    return old;
}

LONG WINAPI win_UnhandledExceptionFilter(PEXCEPTION_POINTERS info) {
    if (!info) return EXCEPTION_CONTINUE_SEARCH;
    
    char buf[64];
    print("[EXCEPTION] Code: 0x");
    print_hex(info->ExceptionRecord->ExceptionCode);
    println("");
    
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ========== TLS 线程局部存储实现 ========== */

DWORD WINAPI win_TlsAlloc(void) {
    spin_lock(&tls_lock);
    
    for (int i = 0; i < MAX_TLS_INDEX; i++) {
        if (!tls_in_use[i]) {
            tls_in_use[i] = 1;
            spin_unlock(&tls_lock);
            return (DWORD)i;
        }
    }
    
    spin_unlock(&tls_lock);
    return TLS_OUT_OF_INDEXES;
}

BOOL WINAPI win_TlsFree(DWORD index) {
    if (index >= MAX_TLS_INDEX) return FALSE;
    
    spin_lock(&tls_lock);
    tls_in_use[index] = 0;
    
    /* 清除所有线程的值 */
    for (int t = 0; t < WIN_MAX_THREADS; t++) {
        tls_values[t][index] = NULL;
    }
    
    spin_unlock(&tls_lock);
    return TRUE;
}

LPVOID WINAPI win_TlsGetValue(DWORD index) {
    if (index >= MAX_TLS_INDEX) return NULL;
    
    int slot = tls_get_current_slot();
    if (slot < 0) {
        /* 无法获取当前线程 - 返回 NULL 并设置最后错误 */
        return NULL;
    }
    
    /* 添加锁保护，防止并发修改 */
    spin_lock(&tls_lock);
    LPVOID value = tls_values[slot][index];
    spin_unlock(&tls_lock);
    
    return value;
}

BOOL WINAPI win_TlsSetValue(DWORD index, LPVOID value) {
    if (index >= MAX_TLS_INDEX) return FALSE;
    
    int slot = tls_get_current_slot();
    if (slot < 0) {
        /* 无法获取当前线程 - 返回失败 */
        return FALSE;
    }
    
    /* 添加锁保护，防止并发修改 */
    spin_lock(&tls_lock);
    tls_values[slot][index] = value;
    spin_unlock(&tls_lock);
    
    return TRUE;
}

/* ========== USER32 窗口管理实现 ========== */

HWND WINAPI win_CreateWindowExA(DWORD exstyle, LPCSTR classname, LPCSTR winname,
                                 DWORD style, int x, int y, int w, int h,
                                 HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    (void)exstyle;
    (void)param;
    
    spin_lock(&win_lock_internal);
    
    int idx = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!win_windows[i].used) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        spin_unlock(&win_lock_internal);
        return NULL_HANDLE;
    }
    
    HWND hwnd = (HWND)(hwnd_next++);
    
    win_windows[idx].used = 1;
    win_windows[idx].hwnd = hwnd;
    win_windows[idx].parent = parent;
    win_windows[idx].instance = inst;
    win_windows[idx].wndproc = NULL;
    win_windows[idx].style = style;
    win_windows[idx].exstyle = exstyle;
    win_windows[idx].rect.left = x;
    win_windows[idx].rect.top = y;
    win_windows[idx].rect.right = x + w;
    win_windows[idx].rect.bottom = y + h;
    win_windows[idx].client = win_windows[idx].rect;
    win_windows[idx].menu = menu;
    win_windows[idx].visible = (style & WS_VISIBLE) ? 1 : 0;
    win_windows[idx].enabled = 1;
    win_windows[idx].userdata = 0;
    
    if (classname) {
        strncpy(win_windows[idx].classname, classname, 63);
        win_windows[idx].classname[63] = '\0';
    }
    
    if (winname) {
        strncpy(win_windows[idx].text, winname, 255);
        win_windows[idx].text[255] = '\0';
    }
    
    spin_unlock(&win_lock_internal);
    
    return hwnd;
}

HWND WINAPI win_CreateWindowExW(DWORD exstyle, LPCWSTR classname, LPCWSTR winname,
                                 DWORD style, int x, int y, int w, int h,
                                 HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    /* 简化：转成 ANSI 调用 */
    char class_buf[64], win_buf[256];
    if (classname) {
        int i;
        for (i = 0; i < 63 && classname[i]; i++) class_buf[i] = (char)classname[i];
        class_buf[i] = '\0';
    }
    if (winname) {
        int i;
        for (i = 0; i < 255 && winname[i]; i++) win_buf[i] = (char)winname[i];
        win_buf[i] = '\0';
    }
    return win_CreateWindowExA(exstyle, classname ? class_buf : NULL, 
                                winname ? win_buf : NULL,
                                style, x, y, w, h, parent, menu, inst, param);
}

BOOL WINAPI win_DestroyWindow(HWND hwnd) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            win_windows[i].used = 0;
            spin_unlock(&win_lock_internal);
            return TRUE;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return FALSE;
}

BOOL WINAPI win_ShowWindow(HWND hwnd, int cmd) {
    (void)cmd;
    
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            win_windows[i].visible = (cmd != SW_HIDE);
            spin_unlock(&win_lock_internal);
            return TRUE;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return FALSE;
}

BOOL WINAPI win_GetWindowRect(HWND hwnd, LPRECT rect) {
    if (!rect) return FALSE;
    
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            *rect = win_windows[i].rect;
            spin_unlock(&win_lock_internal);
            return TRUE;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return FALSE;
}

BOOL WINAPI win_GetClientRect(HWND hwnd, LPRECT rect) {
    if (!rect) return FALSE;
    
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            *rect = win_windows[i].client;
            spin_unlock(&win_lock_internal);
            return TRUE;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return FALSE;
}

BOOL WINAPI win_SetWindowTextA(HWND hwnd, LPCSTR text) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            if (text) {
                strncpy(win_windows[i].text, text, 255);
                win_windows[i].text[255] = '\0';
            } else {
                win_windows[i].text[0] = '\0';
            }
            spin_unlock(&win_lock_internal);
            return TRUE;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return FALSE;
}

int WINAPI win_GetWindowTextA(HWND hwnd, LPSTR buf, int max) {
    if (!buf || max <= 0) return 0;
    
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            int len = strlen(win_windows[i].text);
            if (len >= max) len = max - 1;
            memcpy(buf, win_windows[i].text, len);
            buf[len] = '\0';
            spin_unlock(&win_lock_internal);
            return len;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return 0;
}

LONG_PTR WINAPI win_SetWindowLongPtrA(HWND hwnd, int index, LONG_PTR newval) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            LONG_PTR old = 0;
            
            switch (index) {
                case GWL_USERDATA:
                    old = win_windows[i].userdata;
                    win_windows[i].userdata = newval;
                    break;
                case GWL_WNDPROC:
                    old = (LONG_PTR)win_windows[i].wndproc;
                    win_windows[i].wndproc = (WNDPROC)newval;
                    break;
                case GWL_STYLE:
                    old = win_windows[i].style;
                    win_windows[i].style = (DWORD)newval;
                    break;
                case GWL_EXSTYLE:
                    old = win_windows[i].exstyle;
                    win_windows[i].exstyle = (DWORD)newval;
                    break;
                default:
                    if (index >= 0 && (index / 8) < 8) {
                        old = win_windows[i].extra[index / 8];
                        win_windows[i].extra[index / 8] = newval;
                    }
                    break;
            }
            
            spin_unlock(&win_lock_internal);
            return old;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return 0;
}

LONG_PTR WINAPI win_GetWindowLongPtrA(HWND hwnd, int index) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            LONG_PTR val = 0;
            
            switch (index) {
                case GWL_USERDATA:
                    val = win_windows[i].userdata;
                    break;
                case GWL_WNDPROC:
                    val = (LONG_PTR)win_windows[i].wndproc;
                    break;
                case GWL_STYLE:
                    val = win_windows[i].style;
                    break;
                case GWL_EXSTYLE:
                    val = win_windows[i].exstyle;
                    break;
                case GWL_HINSTANCE:
                    val = (LONG_PTR)win_windows[i].instance;
                    break;
                default:
                    if (index >= 0 && (index / 8) < 8) {
                        val = win_windows[i].extra[index / 8];
                    }
                    break;
            }
            
            spin_unlock(&win_lock_internal);
            return val;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return 0;
}

/* ========== 消息队列实现 ========== */

static void post_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    spin_lock(&win_lock_internal);
    
    /* 使用环形缓冲区：读写指针始终在 [0, MAX_MESSAGES) 范围内 */
    int queue_count = (next_msg_write - next_msg_read + MAX_MESSAGES) % MAX_MESSAGES;
    if (queue_count >= MAX_MESSAGES - 1) {
        /* 队列已满，丢弃最旧的消息 */
        next_msg_read = (next_msg_read + 1) % MAX_MESSAGES;
        /* 可选: 记录警告 */
    }
    
    struct win_message *m = &win_messages[next_msg_write];
    m->used = 1;
    m->hwnd = hwnd;
    m->message = msg;
    m->wparam = wparam;
    m->lparam = lparam;
    m->time = win_GetTickCount();
    m->pt.x = 0;
    m->pt.y = 0;
    
    next_msg_write = (next_msg_write + 1) % MAX_MESSAGES;
    
    spin_unlock(&win_lock_internal);
}

/* 获取消息队列状态 */
static int get_message_queue_count(void) {
    spin_lock(&win_lock_internal);
    int count = (next_msg_write - next_msg_read + MAX_MESSAGES) % MAX_MESSAGES;
    spin_unlock(&win_lock_internal);
    return count;
}

BOOL WINAPI win_GetMessageA(LPMSG msg, HWND hwnd, UINT filtermin, UINT filtermax) {
    (void)filtermin;
    (void)filtermax;
    
    while (1) {
        spin_lock(&win_lock_internal);
        
        int queue_count = (next_msg_write - next_msg_read + MAX_MESSAGES) % MAX_MESSAGES;
        if (queue_count > 0) {
            struct win_message *m = &win_messages[next_msg_read];
            
            if (hwnd == NULL_HANDLE || m->hwnd == hwnd) {
                msg->hwnd = m->hwnd;
                msg->message = m->message;
                msg->wParam = m->wparam;
                msg->lParam = m->lparam;
                msg->time = m->time;
                msg->pt = m->pt;
                
                m->used = 0;
                next_msg_read = (next_msg_read + 1) % MAX_MESSAGES;
                
                spin_unlock(&win_lock_internal);
                return (msg->message == WM_QUIT) ? 0 : 1;
            }
        }
        
        spin_unlock(&win_lock_internal);
        thread_sleep(1);
    }
}

BOOL WINAPI win_PeekMessageA(LPMSG msg, HWND hwnd, UINT filtermin, UINT filtermax, UINT remove) {
    (void)filtermin;
    (void)filtermax;
    
    spin_lock(&win_lock_internal);
    
    int i = next_msg_read;
    while (i != next_msg_write) {
        struct win_message *m = &win_messages[i];
        
        if (hwnd == NULL_HANDLE || m->hwnd == hwnd) {
            msg->hwnd = m->hwnd;
            msg->message = m->message;
            msg->wParam = m->wparam;
            msg->lParam = m->lparam;
            msg->time = m->time;
            msg->pt = m->pt;
            
            if (remove) {
                m->used = 0;
                next_msg_read = (i + 1) % MAX_MESSAGES;
            }
            
            spin_unlock(&win_lock_internal);
            return TRUE;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return FALSE;
}

BOOL WINAPI win_TranslateMessage(const MSG *msg) {
    (void)msg;
    return FALSE;
}

LRESULT WINAPI win_DispatchMessageA(const MSG *msg) {
    if (!msg) return 0;
    
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == msg->hwnd) {
            WNDPROC proc = win_windows[i].wndproc;
            spin_unlock(&win_lock_internal);
            
            if (proc) {
                return proc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
            }
            
            return win_DefWindowProcA(msg->hwnd, msg->message, msg->wParam, msg->lParam);
        }
    }
    
    spin_unlock(&win_lock_internal);
    return 0;
}

LRESULT WINAPI win_SendMessageA(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            WNDPROC proc = win_windows[i].wndproc;
            spin_unlock(&win_lock_internal);
            
            if (proc) {
                return proc(hwnd, msg, wparam, lparam);
            }
            
            return win_DefWindowProcA(hwnd, msg, wparam, lparam);
        }
    }
    
    spin_unlock(&win_lock_internal);
    return 0;
}

BOOL WINAPI win_PostMessageA(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    post_message(hwnd, msg, wparam, lparam);
    return TRUE;
}

BOOL WINAPI win_PostQuitMessage(int exitcode) {
    post_message(NULL_HANDLE, WM_QUIT, (WPARAM)exitcode, 0);
    return TRUE;
}

LRESULT WINAPI win_DefWindowProcA(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_DESTROY:
            win_PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            win_DestroyWindow(hwnd);
            return 0;
        case WM_PAINT:
            {
                PAINTSTRUCT ps;
                win_BeginPaint(hwnd, &ps);
                win_EndPaint(hwnd, &ps);
            }
            return 0;
        default:
            return 0;
    }
}

/* ========== 绘图函数实现 ========== */

HDC WINAPI win_GetDC(HWND hwnd) {
    spin_lock(&dc_lock);
    
    for (int i = 0; i < MAX_DCS; i++) {
        if (!win_dcs[i].used) {
            win_dcs[i].used = 1;
            win_dcs[i].hwnd = hwnd;
            win_dcs[i].compatible = 0;
            win_dcs[i].text_color = RGB(0, 0, 0);
            win_dcs[i].bk_color = RGB(255, 255, 255);
            win_dcs[i].bk_mode = OPAQUE;
            spin_unlock(&dc_lock);
            return (HDC)(next_dc++ + (i << 16));
        }
    }
    
    spin_unlock(&dc_lock);
    return NULL_HANDLE;
}

int WINAPI win_ReleaseDC(HWND hwnd, HDC hdc) {
    (void)hwnd;
    
    int idx = ((uint64_t)hdc >> 16) & 0xFFFF;
    if (idx >= MAX_DCS) return 0;
    
    spin_lock(&dc_lock);
    win_dcs[idx].used = 0;
    spin_unlock(&dc_lock);
    
    return 1;
}

HDC WINAPI win_CreateCompatibleDC(HDC hdc) {
    spin_lock(&dc_lock);
    
    for (int i = 0; i < MAX_DCS; i++) {
        if (!win_dcs[i].used) {
            win_dcs[i].used = 1;
            win_dcs[i].hwnd = NULL_HANDLE;
            win_dcs[i].compatible = 1;
            win_dcs[i].text_color = RGB(0, 0, 0);
            win_dcs[i].bk_color = RGB(255, 255, 255);
            win_dcs[i].bk_mode = OPAQUE;
            spin_unlock(&dc_lock);
            return (HDC)(next_dc++ + (i << 16));
        }
    }
    
    spin_unlock(&dc_lock);
    return NULL_HANDLE;
}

HDC WINAPI win_BeginPaint(HWND hwnd, LPPAINTSTRUCT ps) {
    if (!ps) return NULL_HANDLE;
    
    memset(ps, 0, sizeof(PAINTSTRUCT));
    ps->hdc = win_GetDC(hwnd);
    
    win_GetClientRect(hwnd, &ps->rcPaint);
    
    return ps->hdc;
}

BOOL WINAPI win_EndPaint(HWND hwnd, const PAINTSTRUCT *ps) {
    (void)hwnd;
    if (ps && ps->hdc) {
        win_ReleaseDC(hwnd, ps->hdc);
    }
    return TRUE;
}

BOOL WINAPI win_InvalidateRect(HWND hwnd, const RECT *rect, BOOL erase) {
    (void)rect;
    (void)erase;
    post_message(hwnd, WM_PAINT, 0, 0);
    return TRUE;
}

/* ========== GDI 对象实现 ========== */

HGDIOBJ WINAPI win_GetStockObject(int index) {
    spin_lock(&gdi_lock);
    
    for (int i = 0; i < MAX_GDI_OBJECTS; i++) {
        if (!gdi_objects[i].used) {
            gdi_objects[i].used = 1;
            gdi_objects[i].stock = 1;
            gdi_objects[i].type = (index <= BLACK_PEN) ? OBJ_PEN : 
                                  (index <= NULL_BRUSH) ? OBJ_BRUSH : OBJ_FONT;
            spin_unlock(&gdi_lock);
            return make_gdi_handle(i);
        }
    }
    
    spin_unlock(&gdi_lock);
    return NULL_HANDLE;
}

HGDIOBJ WINAPI win_SelectObject(HDC hdc, HGDIOBJ obj) {
    int dc_idx = ((uint64_t)hdc >> 16) & 0xFFFF;
    int obj_idx = ((uint64_t)obj >> 16) & 0xFFFF;
    
    if (dc_idx >= MAX_DCS || obj_idx >= MAX_GDI_OBJECTS) return NULL_HANDLE;
    
    /* 使用双重锁确保锁顺序一致，避免死锁 */
    double_spin_lock(&dc_lock, &gdi_lock);
    
    HGDIOBJ old = NULL_HANDLE;
    
    switch (gdi_objects[obj_idx].type) {
        case OBJ_PEN:
            old = win_dcs[dc_idx].cur_pen;
            win_dcs[dc_idx].cur_pen = obj;
            break;
        case OBJ_BRUSH:
            old = win_dcs[dc_idx].cur_brush;
            win_dcs[dc_idx].cur_brush = obj;
            break;
        case OBJ_FONT:
            old = win_dcs[dc_idx].cur_font;
            win_dcs[dc_idx].cur_font = obj;
            break;
        case OBJ_BITMAP:
            old = win_dcs[dc_idx].cur_bitmap;
            win_dcs[dc_idx].cur_bitmap = obj;
            break;
    }
    
    double_spin_unlock(&gdi_lock, &dc_lock);
    
    return old;
}

BOOL WINAPI win_DeleteObject(HGDIOBJ obj) {
    int idx = ((uint64_t)obj >> 16) & 0xFFFF;
    if (idx >= MAX_GDI_OBJECTS) return FALSE;
    
    spin_lock(&gdi_lock);
    
    if (gdi_objects[idx].stock) {
        spin_unlock(&gdi_lock);
        return FALSE;
    }
    
    gdi_objects[idx].used = 0;
    
    if (gdi_objects[idx].bitmap.bits) {
        kfree(gdi_objects[idx].bitmap.bits);
        gdi_objects[idx].bitmap.bits = NULL;
    }
    
    spin_unlock(&gdi_lock);
    return TRUE;
}

HPEN WINAPI win_CreatePen(int style, int width, COLORREF color) {
    spin_lock(&gdi_lock);
    
    for (int i = 0; i < MAX_GDI_OBJECTS; i++) {
        if (!gdi_objects[i].used) {
            gdi_objects[i].used = 1;
            gdi_objects[i].stock = 0;
            gdi_objects[i].type = OBJ_PEN;
            gdi_objects[i].style = style;
            gdi_objects[i].width = width;
            gdi_objects[i].color = color;
            spin_unlock(&gdi_lock);
            return (HPEN)make_gdi_handle(i);
        }
    }
    
    spin_unlock(&gdi_lock);
    return NULL_HANDLE;
}

HBRUSH WINAPI win_CreateSolidBrush(COLORREF color) {
    spin_lock(&gdi_lock);
    
    for (int i = 0; i < MAX_GDI_OBJECTS; i++) {
        if (!gdi_objects[i].used) {
            gdi_objects[i].used = 1;
            gdi_objects[i].stock = 0;
            gdi_objects[i].type = OBJ_BRUSH;
            gdi_objects[i].color = color;
            spin_unlock(&gdi_lock);
            return (HBRUSH)make_gdi_handle(i);
        }
    }
    
    spin_unlock(&gdi_lock);
    return NULL_HANDLE;
}

HFONT WINAPI win_CreateFontA(int height, int width, int escapement, int orientation,
                              int weight, DWORD italic, DWORD underline, DWORD strikeout,
                              DWORD charset, DWORD outprecision, DWORD clipprecision,
                              DWORD quality, DWORD pitchfamily, LPCSTR face) {
    (void)escapement;
    (void)orientation;
    (void)outprecision;
    (void)clipprecision;
    (void)quality;
    
    spin_lock(&gdi_lock);
    
    for (int i = 0; i < MAX_GDI_OBJECTS; i++) {
        if (!gdi_objects[i].used) {
            gdi_objects[i].used = 1;
            gdi_objects[i].stock = 0;
            gdi_objects[i].type = OBJ_FONT;
            gdi_objects[i].font.lf.lfHeight = height;
            gdi_objects[i].font.lf.lfWidth = width;
            gdi_objects[i].font.lf.lfWeight = weight;
            gdi_objects[i].font.lf.lfItalic = (BYTE)italic;
            gdi_objects[i].font.lf.lfUnderline = (BYTE)underline;
            gdi_objects[i].font.lf.lfStrikeOut = (BYTE)strikeout;
            gdi_objects[i].font.lf.lfCharSet = (BYTE)charset;
            gdi_objects[i].font.lf.lfPitchAndFamily = (BYTE)pitchfamily;
            if (face) {
                strncpy(gdi_objects[i].font.lf.lfFaceName, face, 31);
            }
            spin_unlock(&gdi_lock);
            return (HFONT)make_gdi_handle(i);
        }
    }
    
    spin_unlock(&gdi_lock);
    return NULL_HANDLE;
}

COLORREF WINAPI win_SetTextColor(HDC hdc, COLORREF color) {
    int idx = ((uint64_t)hdc >> 16) & 0xFFFF;
    if (idx >= MAX_DCS) return 0;
    
    spin_lock(&dc_lock);
    COLORREF old = win_dcs[idx].text_color;
    win_dcs[idx].text_color = color;
    spin_unlock(&dc_lock);
    
    return old;
}

COLORREF WINAPI win_SetBkColor(HDC hdc, COLORREF color) {
    int idx = ((uint64_t)hdc >> 16) & 0xFFFF;
    if (idx >= MAX_DCS) return 0;
    
    spin_lock(&dc_lock);
    COLORREF old = win_dcs[idx].bk_color;
    win_dcs[idx].bk_color = color;
    spin_unlock(&dc_lock);
    
    return old;
}

int WINAPI win_SetBkMode(HDC hdc, int mode) {
    int idx = ((uint64_t)hdc >> 16) & 0xFFFF;
    if (idx >= MAX_DCS) return 0;
    
    spin_lock(&dc_lock);
    int old = win_dcs[idx].bk_mode;
    win_dcs[idx].bk_mode = mode;
    spin_unlock(&dc_lock);
    
    return old;
}

BOOL WINAPI win_Rectangle(HDC hdc, int x1, int y1, int x2, int y2) {
    (void)hdc;
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    /* 实际绘制需要在帧缓冲中实现 */
    return TRUE;
}

BOOL WINAPI win_TextOutA(HDC hdc, int x, int y, LPCSTR str, int count) {
    (void)hdc;
    (void)x;
    (void)y;
    (void)str;
    (void)count;
    return TRUE;
}

int WINAPI win_FillRect(HDC hdc, const RECT *rect, HBRUSH brush) {
    (void)hdc;
    (void)rect;
    (void)brush;
    return 1;
}

int WINAPI win_MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type) {
    (void)hwnd;
    (void)type;
    
    print("[MSGBOX] ");
    if (caption) print(caption);
    print(": ");
    if (text) print(text);
    println("");
    
    return IDOK;
}

HGLOBAL WINAPI win_LocalAlloc(UINT flags, size_t size) {
    (void)flags;
    return (HGLOBAL)kmalloc(size);
}

HGLOBAL WINAPI win_LocalFree(HGLOBAL mem) {
    kfree((void*)mem);
    return NULL_HANDLE;
}

/* ========== USER32 窗口类实现 ========== */

ATOM WINAPI win_RegisterClassA(const WNDCLASSA *wndclass) {
    (void)wndclass;
    /* 简化：返回伪 ATOM */
    return 0xC000;
}

ATOM WINAPI win_RegisterClassExA(const WNDCLASSEXA *wndclass) {
    (void)wndclass;
    return 0xC000;
}

BOOL WINAPI win_UnregisterClassA(LPCSTR classname, HINSTANCE inst) {
    (void)classname;
    (void)inst;
    return TRUE;
}

BOOL WINAPI win_UpdateWindow(HWND hwnd) {
    (void)hwnd;
    post_message(hwnd, WM_PAINT, 0, 0);
    return TRUE;
}

BOOL WINAPI win_MoveWindow(HWND hwnd, int x, int y, int w, int h, BOOL repaint) {
    (void)repaint;
    
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            win_windows[i].rect.left = x;
            win_windows[i].rect.top = y;
            win_windows[i].rect.right = x + w;
            win_windows[i].rect.bottom = y + h;
            win_windows[i].client = win_windows[i].rect;
            spin_unlock(&win_lock_internal);
            return TRUE;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return FALSE;
}

int WINAPI win_GetWindowTextLengthA(HWND hwnd) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            int len = (int)strlen(win_windows[i].text);
            spin_unlock(&win_lock_internal);
            return len;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return 0;
}

HWND WINAPI win_GetParent(HWND hwnd) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            HWND parent = win_windows[i].parent;
            spin_unlock(&win_lock_internal);
            return parent;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return NULL_HANDLE;
}

HWND WINAPI win_SetParent(HWND hwnd, HWND parent) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            HWND old = win_windows[i].parent;
            win_windows[i].parent = parent;
            spin_unlock(&win_lock_internal);
            return old;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return NULL_HANDLE;
}

LONG WINAPI win_SetWindowLongA(HWND hwnd, int index, LONG newval) {
    return (LONG)win_SetWindowLongPtrA(hwnd, index, (LONG_PTR)newval);
}

LONG WINAPI win_GetWindowLongA(HWND hwnd, int index) {
    return (LONG)win_GetWindowLongPtrA(hwnd, index);
}

BOOL WINAPI win_SetForegroundWindow(HWND hwnd) {
    (void)hwnd;
    return TRUE;
}

HWND WINAPI win_GetForegroundWindow(void) {
    spin_lock(&win_lock_internal);
    
    /* 返回第一个可见窗口 */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].visible) {
            HWND hwnd = win_windows[i].hwnd;
            spin_unlock(&win_lock_internal);
            return hwnd;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return NULL_HANDLE;
}

BOOL WINAPI win_EnableWindow(HWND hwnd, BOOL enable) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            win_windows[i].enabled = enable;
            spin_unlock(&win_lock_internal);
            return TRUE;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return FALSE;
}

BOOL WINAPI win_IsWindowEnabled(HWND hwnd) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            spin_unlock(&win_lock_internal);
            return win_windows[i].enabled;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return FALSE;
}

BOOL WINAPI win_IsWindowVisible(HWND hwnd) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            spin_unlock(&win_lock_internal);
            return win_windows[i].visible;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return FALSE;
}

BOOL WINAPI win_IsWindow(HWND hwnd) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            spin_unlock(&win_lock_internal);
            return TRUE;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return FALSE;
}

HWND WINAPI win_GetDesktopWindow(void) {
    return (HWND)0x1000; /* 伪桌面窗口句柄 */
}

HWND WINAPI win_GetFocus(void) {
    return win_GetForegroundWindow();
}

HWND WINAPI win_SetFocus(HWND hwnd) {
    (void)hwnd;
    return win_GetForegroundWindow();
}

HWND WINAPI win_GetActiveWindow(void) {
    return win_GetForegroundWindow();
}

LRESULT WINAPI win_CallWindowProcA(WNDPROC proc, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (proc) {
        return proc(hwnd, msg, wparam, lparam);
    }
    return win_DefWindowProcA(hwnd, msg, wparam, lparam);
}

BOOL WINAPI win_ValidateRect(HWND hwnd, const RECT *rect) {
    (void)hwnd;
    (void)rect;
    return TRUE;
}

/* ========== USER32 定时器实现 ========== */

#define MAX_TIMERS 64

static struct {
    int used;
    HWND hwnd;
    UINT_PTR id;
    UINT elapse;
    TIMERPROC callback;
    uint64_t last_tick;
} win_timers[MAX_TIMERS];

static spinlock_t timer_lock = SPINLOCK_INIT;

UINT_PTR WINAPI win_SetTimer(HWND hwnd, UINT_PTR idevent, UINT elapse, TIMERPROC callback) {
    spin_lock(&timer_lock);
    
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!win_timers[i].used) {
            win_timers[i].used = 1;
            win_timers[i].hwnd = hwnd;
            win_timers[i].id = idevent ? idevent : (UINT_PTR)(i + 1);
            win_timers[i].elapse = elapse;
            win_timers[i].callback = callback;
            win_timers[i].last_tick = win_GetTickCount();
            spin_unlock(&timer_lock);
            return win_timers[i].id;
        }
    }
    
    spin_unlock(&timer_lock);
    return 0;
}

BOOL WINAPI win_KillTimer(HWND hwnd, UINT_PTR idevent) {
    (void)hwnd;
    
    spin_lock(&timer_lock);
    
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (win_timers[i].used && win_timers[i].id == idevent) {
            win_timers[i].used = 0;
            spin_unlock(&timer_lock);
            return TRUE;
        }
    }
    
    spin_unlock(&timer_lock);
    return FALSE;
}

/* ========== USER32 光标实现 ========== */

BOOL WINAPI win_GetCursorPos(LPPOINT pt) {
    if (!pt) return FALSE;
    pt->x = 0;
    pt->y = 0;
    return TRUE;
}

BOOL WINAPI win_SetCursorPos(int x, int y) {
    (void)x;
    (void)y;
    return TRUE;
}

HWND WINAPI win_WindowFromPoint(POINT pt) {
    (void)pt;
    return win_GetForegroundWindow();
}

BOOL WINAPI win_SetCapture(HWND hwnd) {
    (void)hwnd;
    return TRUE;
}

HWND WINAPI win_ReleaseCapture(void) {
    return NULL_HANDLE;
}

HWND WINAPI win_GetCapture(void) {
    return NULL_HANDLE;
}

SHORT WINAPI win_GetKeyState(int vkey) {
    (void)vkey;
    return 0;
}

SHORT WINAPI win_GetAsyncKeyState(int vkey) {
    (void)vkey;
    return 0;
}

int WINAPI win_GetKeyboardState(BYTE *kbstate) {
    if (!kbstate) return 0;
    memset(kbstate, 0, 256);
    return 1;
}

HICON WINAPI win_LoadIconA(HINSTANCE inst, LPCSTR name) {
    (void)inst;
    (void)name;
    return (HICON)1; /* 伪句柄 */
}

HCURSOR WINAPI win_LoadCursorA(HINSTANCE inst, LPCSTR name) {
    (void)inst;
    (void)name;
    return (HCURSOR)1;
}

HCURSOR WINAPI win_SetCursor(HCURSOR cursor) {
    (void)cursor;
    return (HCURSOR)1;
}

/* ========== USER32 菜单实现 ========== */

HMENU WINAPI win_CreateMenu(void) {
    return (HMENU)(0x5000); /* 伪句柄 */
}

HMENU WINAPI win_CreatePopupMenu(void) {
    return (HMENU)(0x5001);
}

BOOL WINAPI win_DestroyMenu(HMENU menu) {
    (void)menu;
    return TRUE;
}

BOOL WINAPI win_AppendMenuA(HMENU menu, UINT flags, UINT_PTR id, LPCSTR data) {
    (void)menu;
    (void)flags;
    (void)id;
    (void)data;
    return TRUE;
}

HMENU WINAPI win_GetMenu(HWND hwnd) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            HMENU menu = win_windows[i].menu;
            spin_unlock(&win_lock_internal);
            return menu;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return NULL_HANDLE;
}

BOOL WINAPI win_SetMenu(HWND hwnd, HMENU menu) {
    spin_lock(&win_lock_internal);
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win_windows[i].used && win_windows[i].hwnd == hwnd) {
            win_windows[i].menu = menu;
            spin_unlock(&win_lock_internal);
            return TRUE;
        }
    }
    
    spin_unlock(&win_lock_internal);
    return FALSE;
}

/* ========== GDI32 图形设备接口实现 ========== */

HDC WINAPI win_GetWindowDC(HWND hwnd) {
    return win_GetDC(hwnd);
}

BOOL WINAPI win_DeleteDC(HDC hdc) {
    int idx = ((uint64_t)hdc >> 16) & 0xFFFF;
    if (idx >= MAX_DCS) return FALSE;
    
    spin_lock(&dc_lock);
    win_dcs[idx].used = 0;
    spin_unlock(&dc_lock);
    
    return TRUE;
}

HBRUSH WINAPI win_CreateHatchBrush(int style, COLORREF color) {
    spin_lock(&gdi_lock);
    
    for (int i = 0; i < MAX_GDI_OBJECTS; i++) {
        if (!gdi_objects[i].used) {
            gdi_objects[i].used = 1;
            gdi_objects[i].stock = 0;
            gdi_objects[i].type = OBJ_BRUSH;
            gdi_objects[i].style = style;
            gdi_objects[i].color = color;
            spin_unlock(&gdi_lock);
            return (HBRUSH)make_gdi_handle(i);
        }
    }
    
    spin_unlock(&gdi_lock);
    return NULL_HANDLE;
}

HFONT WINAPI win_CreateFontIndirectA(const LOGFONTA *lplf) {
    if (!lplf) return NULL_HANDLE;
    
    return win_CreateFontA(lplf->lfHeight, lplf->lfWidth, 0, 0,
                          lplf->lfWeight, lplf->lfItalic, lplf->lfUnderline,
                          lplf->lfStrikeOut, lplf->lfCharSet, 0, 0, 0, 0,
                          lplf->lfFaceName);
}

HBITMAP WINAPI win_CreateBitmap(int width, int height, UINT planes, UINT bpp, const void *data) {
    spin_lock(&gdi_lock);
    
    for (int i = 0; i < MAX_GDI_OBJECTS; i++) {
        if (!gdi_objects[i].used) {
            gdi_objects[i].used = 1;
            gdi_objects[i].stock = 0;
            gdi_objects[i].type = OBJ_BITMAP;
            gdi_objects[i].bitmap.width = width;
            gdi_objects[i].bitmap.height = height;
            gdi_objects[i].bitmap.bpp = bpp;
            
            /* 分配位图数据 */
            size_t size = width * height * (bpp / 8);
            gdi_objects[i].bitmap.bits = kmalloc(size);
            if (gdi_objects[i].bitmap.bits && data) {
                memcpy(gdi_objects[i].bitmap.bits, data, size);
            }
            
            spin_unlock(&gdi_lock);
            return (HBITMAP)make_gdi_handle(i);
        }
    }
    
    spin_unlock(&gdi_lock);
    return NULL_HANDLE;
}

HBITMAP WINAPI win_CreateCompatibleBitmap(HDC hdc, int width, int height) {
    (void)hdc;
    return win_CreateBitmap(width, height, 1, 32, NULL);
}

BOOL WINAPI win_MoveToEx(HDC hdc, int x, int y, LPPOINT pt) {
    int idx = ((uint64_t)hdc >> 16) & 0xFFFF;
    if (idx >= MAX_DCS) return FALSE;
    
    spin_lock(&dc_lock);
    if (pt) {
        pt->x = win_dcs[idx].pos.x;
        pt->y = win_dcs[idx].pos.y;
    }
    win_dcs[idx].pos.x = x;
    win_dcs[idx].pos.y = y;
    spin_unlock(&dc_lock);
    
    return TRUE;
}

BOOL WINAPI win_LineTo(HDC hdc, int x, int y) {
    (void)hdc;
    (void)x;
    (void)y;
    /* 实际绘制需要帧缓冲支持 */
    return TRUE;
}

BOOL WINAPI win_Ellipse(HDC hdc, int x1, int y1, int x2, int y2) {
    (void)hdc;
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    return TRUE;
}

BOOL WINAPI win_BitBlt(HDC hdcdest, int xdest, int ydest, int width, int height,
                        HDC hdcsrc, int xsrc, int ysrc, DWORD rop) {
    (void)hdcdest;
    (void)xdest;
    (void)ydest;
    (void)width;
    (void)height;
    (void)hdcsrc;
    (void)xsrc;
    (void)ysrc;
    (void)rop;
    /* 实际绘制需要帧缓冲支持 */
    return TRUE;
}

BOOL WINAPI win_StretchBlt(HDC hdcdest, int xdest, int ydest, int widthdest, int heightdest,
                            HDC hdcsrc, int xsrc, int ysrc, int widthsrc, int heightsrc, DWORD rop) {
    (void)hdcdest;
    (void)xdest;
    (void)ydest;
    (void)widthdest;
    (void)heightdest;
    (void)hdcsrc;
    (void)xsrc;
    (void)ysrc;
    (void)widthsrc;
    (void)heightsrc;
    (void)rop;
    return TRUE;
}

COLORREF WINAPI win_GetTextColor(HDC hdc) {
    int idx = ((uint64_t)hdc >> 16) & 0xFFFF;
    if (idx >= MAX_DCS) return 0;
    
    spin_lock(&dc_lock);
    COLORREF color = win_dcs[idx].text_color;
    spin_unlock(&dc_lock);
    
    return color;
}

BOOL WINAPI win_GetTextMetricsA(HDC hdc, LPTEXTMETRICA lptm) {
    (void)hdc;
    if (!lptm) return FALSE;
    
    memset(lptm, 0, sizeof(TEXTMETRICA));
    lptm->tmHeight = 16;
    lptm->tmAscent = 12;
    lptm->tmDescent = 4;
    lptm->tmAveCharWidth = 8;
    lptm->tmMaxCharWidth = 10;
    
    return TRUE;
}

BOOL WINAPI win_GetTextExtentPoint32A(HDC hdc, LPCSTR str, int count, LPSIZE size) {
    (void)hdc;
    if (!str || !size || count < 0) return FALSE;
    
    /* 简化：假设每个字符 8 像素宽 */
    size->cx = count * 8;
    size->cy = 16;
    
    return TRUE;
}

HRGN WINAPI win_CreateRectRgn(int x1, int y1, int x2, int y2) {
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    return (HRGN)1; /* 伪句柄 */
}

int WINAPI win_SelectClipRgn(HDC hdc, HRGN rgn) {
    (void)hdc;
    (void)rgn;
    return 1; /* SIMPLEREGION */
}

int WINAPI win_GetDeviceCaps(HDC hdc, int index) {
    (void)hdc;
    
    switch (index) {
        case HORZRES:
            return 1024; /* 默认宽度 */
        case VERTRES:
            return 768; /* 默认高度 */
        case BITSPIXEL:
            return 32; /* 32 位色 */
        case PLANES:
            return 1;
        case LOGPIXELSX:
        case LOGPIXELSY:
            return 96; /* 默认 DPI */
        default:
            return 0;
    }
}

/* ========== 注册表实现 ========== */

LONG WINAPI win_RegOpenKeyA(HKEY key, LPCSTR subkey, HKEY *result) {
    (void)key;
    (void)subkey;
    
    if (!result) return ERROR_INVALID_PARAMETER;
    
    spin_lock(&reg_lock);
    
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        if (!reg_keys[i].used) {
            reg_keys[i].used = 1;
            reg_keys[i].hkey = (HKEY)(i + 1);
            reg_keys[i].parent = key;
            if (subkey) {
                strncpy(reg_keys[i].name, subkey, 63);
            }
            *result = reg_keys[i].hkey;
            spin_unlock(&reg_lock);
            return ERROR_SUCCESS;
        }
    }
    
    spin_unlock(&reg_lock);
    return ERROR_NO_MORE_ITEMS;
}

LONG WINAPI win_RegOpenKeyExA(HKEY key, LPCSTR subkey, DWORD opts, REGSAM access, HKEY *result) {
    (void)opts;
    (void)access;
    return win_RegOpenKeyA(key, subkey, result);
}

LONG WINAPI win_RegCloseKey(HKEY key) {
    uint64_t idx = (uint64_t)key;
    if (idx == 0 || idx > MAX_REG_KEYS) return ERROR_INVALID_HANDLE;
    
    spin_lock(&reg_lock);
    reg_keys[idx - 1].used = 0;
    spin_unlock(&reg_lock);
    
    return ERROR_SUCCESS;
}

LONG WINAPI win_RegCreateKeyA(HKEY key, LPCSTR subkey, HKEY *result) {
    return win_RegOpenKeyA(key, subkey, result);
}

LONG WINAPI win_RegSetValueExA(HKEY key, LPCSTR value, DWORD reserved, DWORD type, const BYTE *data, DWORD len) {
    (void)reserved;
    
    uint64_t idx = (uint64_t)key;
    if (idx == 0 || idx > MAX_REG_KEYS) return ERROR_INVALID_HANDLE;
    
    spin_lock(&reg_lock);
    struct reg_key *k = &reg_keys[idx - 1];
    
    int vidx = -1;
    for (int i = 0; i < 16; i++) {
        if (!k->values[i].used || 
            (value && strcmp(k->values[i].name, value) == 0)) {
            vidx = i;
            break;
        }
    }
    
    if (vidx < 0) {
        spin_unlock(&reg_lock);
        return ERROR_NO_MORE_ITEMS;
    }
    
    k->values[vidx].used = 1;
    k->values[vidx].type = type;
    k->values[vidx].size = (len > 1024) ? 1024 : len;
    
    if (value) {
        strncpy(k->values[vidx].name, value, 63);
    } else {
        k->values[vidx].name[0] = '\0';
    }
    
    if (data && len > 0) {
        memcpy(k->values[vidx].data, data, k->values[vidx].size);
    }
    
    spin_unlock(&reg_lock);
    return ERROR_SUCCESS;
}

LONG WINAPI win_RegQueryValueExA(HKEY key, LPCSTR value, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size) {
    (void)reserved;
    
    uint64_t idx = (uint64_t)key;
    if (idx == 0 || idx > MAX_REG_KEYS) return ERROR_INVALID_HANDLE;
    
    spin_lock(&reg_lock);
    struct reg_key *k = &reg_keys[idx - 1];
    
    for (int i = 0; i < 16; i++) {
        if (k->values[i].used) {
            if (!value || strlen(k->values[i].name) == 0 || 
                strcmp(k->values[i].name, value) == 0) {
                
                if (type) *type = k->values[i].type;
                if (size) {
                    if (data && *size >= k->values[i].size) {
                        memcpy(data, k->values[i].data, k->values[i].size);
                    }
                    *size = k->values[i].size;
                }
                
                spin_unlock(&reg_lock);
                return ERROR_SUCCESS;
            }
        }
    }
    
    spin_unlock(&reg_lock);
    return ERROR_FILE_NOT_FOUND;
}

LONG WINAPI win_RegCreateKeyExA(HKEY key, LPCSTR subkey, DWORD reserved, LPSTR class, DWORD opts,
                                 REGSAM access, LPSECURITY_ATTRIBUTES security, HKEY *result, LPDWORD disposition) {
    (void)reserved;
    (void)class;
    (void)opts;
    (void)access;
    (void)security;
    
    LONG ret = win_RegCreateKeyA(key, subkey, result);
    if (ret == ERROR_SUCCESS && disposition) {
        *disposition = REG_OPENED_EXISTING_KEY;
    }
    return ret;
}

LONG WINAPI win_RegDeleteKeyA(HKEY key, LPCSTR subkey) {
    (void)key;
    (void)subkey;
    /* 简化：暂不支持删除键 */
    return ERROR_SUCCESS;
}

LONG WINAPI win_RegDeleteValueA(HKEY key, LPCSTR value) {
    (void)value;
    
    uint64_t idx = (uint64_t)key;
    if (idx == 0 || idx > MAX_REG_KEYS) return ERROR_INVALID_HANDLE;
    
    spin_lock(&reg_lock);
    struct reg_key *k = &reg_keys[idx - 1];
    
    for (int i = 0; i < 16; i++) {
        if (k->values[i].used && value && strcmp(k->values[i].name, value) == 0) {
            k->values[i].used = 0;
            spin_unlock(&reg_lock);
            return ERROR_SUCCESS;
        }
    }
    
    spin_unlock(&reg_lock);
    return ERROR_FILE_NOT_FOUND;
}

LONG WINAPI win_RegEnumKeyA(HKEY key, DWORD index, LPSTR name, DWORD size) {
    (void)key;
    (void)index;
    (void)name;
    (void)size;
    /* 简化：暂不支持枚举 */
    return ERROR_NO_MORE_ITEMS;
}

LONG WINAPI win_RegEnumValueA(HKEY key, DWORD index, LPSTR name, LPDWORD namesize, LPDWORD reserved,
                               LPDWORD type, LPBYTE data, LPDWORD datasize) {
    (void)reserved;
    
    uint64_t idx = (uint64_t)key;
    if (idx == 0 || idx > MAX_REG_KEYS) return ERROR_INVALID_HANDLE;
    
    spin_lock(&reg_lock);
    struct reg_key *k = &reg_keys[idx - 1];
    
    int count = 0;
    for (int i = 0; i < 16; i++) {
        if (k->values[i].used) {
            if (count == index) {
                /* 找到第 index 个值 */
                if (name && namesize) {
                    DWORD len = (DWORD)strlen(k->values[i].name);
                    if (*namesize > len) {
                        memcpy(name, k->values[i].name, len + 1);
                    }
                    *namesize = len;
                }
                if (type) *type = k->values[i].type;
                if (data && datasize) {
                    if (*datasize >= k->values[i].size) {
                        memcpy(data, k->values[i].data, k->values[i].size);
                    }
                    *datasize = k->values[i].size;
                }
                spin_unlock(&reg_lock);
                return ERROR_SUCCESS;
            }
            count++;
        }
    }
    
    spin_unlock(&reg_lock);
    return ERROR_NO_MORE_ITEMS;
}

/* ========== ADVAPI32 服务实现 ========== */

SC_HANDLE WINAPI win_OpenSCManagerA(LPCSTR machine, LPCSTR database, DWORD access) {
    (void)machine;
    (void)database;
    (void)access;
    return (SC_HANDLE)0x6000; /* 伪句柄 */
}

SC_HANDLE WINAPI win_OpenServiceA(SC_HANDLE manager, LPCSTR name, DWORD access) {
    (void)manager;
    (void)name;
    (void)access;
    return (SC_HANDLE)0x6001;
}

BOOL WINAPI win_CloseServiceHandle(SC_HANDLE handle) {
    (void)handle;
    return TRUE;
}

BOOL WINAPI win_DeleteService(SC_HANDLE service) {
    (void)service;
    return TRUE;
}

SC_HANDLE WINAPI win_CreateServiceA(SC_HANDLE manager, LPCSTR name, LPCSTR display, DWORD access,
                                     DWORD type, DWORD start, DWORD error, LPCSTR path,
                                     LPCSTR group, LPDWORD tag, LPCSTR deps, LPCSTR account, LPCSTR password) {
    (void)manager;
    (void)name;
    (void)display;
    (void)access;
    (void)type;
    (void)start;
    (void)error;
    (void)path;
    (void)group;
    (void)tag;
    (void)deps;
    (void)account;
    (void)password;
    return (SC_HANDLE)0x6002;
}

BOOL WINAPI win_StartServiceA(SC_HANDLE service, DWORD count, LPCSTR *args) {
    (void)service;
    (void)count;
    (void)args;
    return TRUE;
}

BOOL WINAPI win_ControlService(SC_HANDLE service, DWORD ctrl, LPSERVICE_STATUS status) {
    (void)service;
    (void)ctrl;
    if (status) {
        status->dwCurrentState = SERVICE_STOPPED;
    }
    return TRUE;
}

BOOL WINAPI win_QueryServiceStatus(SC_HANDLE service, LPSERVICE_STATUS status) {
    (void)service;
    if (status) {
        status->dwCurrentState = SERVICE_RUNNING;
        status->dwWin32ExitCode = NO_ERROR;
    }
    return TRUE;
}

/* ========== ADVAPI32 安全实现 ========== */

BOOL WINAPI win_InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR desc, DWORD revision) {
    (void)desc;
    (void)revision;
    return TRUE;
}

BOOL WINAPI win_IsValidSecurityDescriptor(PSECURITY_DESCRIPTOR desc) {
    (void)desc;
    return TRUE;
}

BOOL WINAPI win_IsValidSid(void *sid) {
    (void)sid;
    return TRUE;
}

BOOL WINAPI win_IsValidAcl(void *acl) {
    (void)acl;
    return TRUE;
}

BOOL WINAPI win_AdjustTokenPrivileges(HANDLE token, BOOL disable, void *privs,
                                       DWORD len, void *oldprivs, LPDWORD oldlen) {
    (void)token;
    (void)disable;
    (void)privs;
    (void)len;
    (void)oldprivs;
    (void)oldlen;
    return TRUE;
}

BOOL WINAPI win_LookupPrivilegeValueA(LPCSTR system, LPCSTR name, void *luid) {
    (void)system;
    (void)name;
    (void)luid;
    return TRUE;
}

/* ========== ADVAPI32 加密实现 ========== */

BOOL WINAPI win_CryptAcquireContextA(HCRYPTPROV *prov, LPCSTR container, LPCSTR provider, DWORD type, DWORD flags) {
    (void)container;
    (void)provider;
    (void)type;
    (void)flags;
    if (prov) *prov = (HCRYPTPROV)0x7000;
    return TRUE;
}

BOOL WINAPI win_CryptReleaseContext(HCRYPTPROV prov, DWORD flags) {
    (void)prov;
    (void)flags;
    return TRUE;
}

BOOL WINAPI win_CryptGenKey(HCRYPTPROV prov, ALG_ID algid, DWORD flags, HCRYPTKEY *key) {
    (void)prov;
    (void)algid;
    (void)flags;
    if (key) *key = (HCRYPTKEY)0x7100;
    return TRUE;
}

BOOL WINAPI win_CryptDestroyKey(HCRYPTKEY key) {
    (void)key;
    return TRUE;
}

BOOL WINAPI win_CryptCreateHash(HCRYPTPROV prov, ALG_ID algid, HCRYPTKEY key, DWORD flags, HCRYPTHASH *hash) {
    (void)prov;
    (void)algid;
    (void)key;
    (void)flags;
    if (hash) *hash = (HCRYPTHASH)0x7200;
    return TRUE;
}

BOOL WINAPI win_CryptDestroyHash(HCRYPTHASH hash) {
    (void)hash;
    return TRUE;
}

BOOL WINAPI win_CryptHashData(HCRYPTHASH hash, const BYTE *data, DWORD len, DWORD flags) {
    (void)hash;
    (void)data;
    (void)len;
    (void)flags;
    return TRUE;
}

BOOL WINAPI win_CryptEncrypt(HCRYPTKEY key, HCRYPTHASH hash, BOOL final, DWORD flags, BYTE *data, DWORD *len, DWORD buflen) {
    (void)key;
    (void)hash;
    (void)final;
    (void)flags;
    (void)data;
    (void)len;
    (void)buflen;
    return TRUE;
}

BOOL WINAPI win_CryptDecrypt(HCRYPTKEY key, HCRYPTHASH hash, BOOL final, DWORD flags, BYTE *data, DWORD *len) {
    (void)key;
    (void)hash;
    (void)final;
    (void)flags;
    (void)data;
    (void)len;
    return TRUE;
}

/* ========== SHELL32 实现 ========== */

BOOL WINAPI win_ShellExecuteA(HWND hwnd, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, int show) {
    (void)hwnd;
    (void)op;
    (void)params;
    (void)dir;
    (void)show;
    
    if (file) {
        serial_print("[SHELL] Execute: ");
        serial_print(file);
        serial_print("\n");
    }
    
    return TRUE;
}

HRESULT WINAPI win_SHGetFolderPathA(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPSTR path) {
    (void)hwnd;
    (void)token;
    (void)flags;
    
    if (!path) return E_POINTER;
    
    const char *folder = "";
    switch (csidl) {
        case CSIDL_DESKTOP:
            folder = "C:\\Users\\user\\Desktop";
            break;
        case CSIDL_PROGRAMS:
            folder = "C:\\Program Files";
            break;
        case CSIDL_PERSONAL:
            folder = "C:\\Users\\user\\Documents";
            break;
        case CSIDL_WINDOWS:
            folder = "C:\\Windows";
            break;
        case CSIDL_SYSTEM:
            folder = "C:\\Windows\\System32";
            break;
        case CSIDL_APPDATA:
            folder = "C:\\Users\\user\\AppData\\Roaming";
            break;
        default:
            folder = "C:\\";
            break;
    }
    
    strcpy(path, folder);
    return S_OK;
}

BOOL WINAPI win_SHGetFileInfoA(LPCSTR path, DWORD attr, void *info, UINT size, UINT flags) {
    (void)path;
    (void)attr;
    (void)info;
    (void)size;
    (void)flags;
    return TRUE;
}

BOOL WINAPI win_Shell_NotifyIconA(DWORD msg, void *data) {
    (void)msg;
    (void)data;
    return TRUE;
}

/* ========== COMDLG32 实现 ========== */

BOOL WINAPI win_GetOpenFileNameA(void *info) {
    (void)info;
    return FALSE;
}

BOOL WINAPI win_GetSaveFileNameA(void *info) {
    (void)info;
    return FALSE;
}

DWORD WINAPI win_CommDlgExtendedError(void) {
    return 0;
}

BOOL WINAPI win_ChooseColorA(void *info) {
    (void)info;
    return FALSE;
}

BOOL WINAPI win_ChooseFontA(void *info) {
    (void)info;
    return FALSE;
}

BOOL WINAPI win_PrintDlgA(void *info) {
    (void)info;
    return FALSE;
}

BOOL WINAPI win_PageSetupDlgA(void *info) {
    (void)info;
    return FALSE;
}

/* ========== OLE32 COM/OLE 实现 ========== */

HRESULT WINAPI win_CoGetClassObject(REFCLSID rclsid, DWORD clsctx, void *server, REFIID riid, LPVOID *ppv) {
    (void)rclsid;
    (void)clsctx;
    (void)server;
    (void)riid;
    if (ppv) *ppv = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

BSTR WINAPI win_SysAllocStringLen(const OLECHAR *str, UINT len) {
    if (!str && len == 0) return NULL;
    
    BSTR bstr = (BSTR)kmalloc(len * sizeof(OLECHAR) + 4);
    if (!bstr) return NULL;
    
    *(DWORD*)bstr = len * sizeof(OLECHAR);
    if (str) {
        memcpy((BYTE*)bstr + 4, str, len * sizeof(OLECHAR));
    }
    ((OLECHAR*)((BYTE*)bstr + 4))[len] = 0;
    
    return bstr;
}

HRESULT WINAPI win_StringFromCLSID(REFCLSID rclsid, LPOLESTR *str) {
    (void)rclsid;
    if (!str) return E_POINTER;
    *str = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI win_CLSIDFromString(LPCOLESTR str, CLSID *clsid) {
    (void)str;
    (void)clsid;
    return E_NOTIMPL;
}

HRESULT WINAPI win_CoRegisterClassObject(REFCLSID rclsid, void *obj, DWORD clsctx, DWORD flags, LPDWORD reg) {
    (void)rclsid;
    (void)obj;
    (void)clsctx;
    (void)flags;
    (void)reg;
    return S_OK;
}

HRESULT WINAPI win_CoRevokeClassObject(DWORD reg) {
    (void)reg;
    return S_OK;
}

/* ========== 性能计数器实现 ========== */

BOOL WINAPI win_QueryPerformanceCounter(PLARGE_INTEGER count) {
    if (!count) return FALSE;
    
    uint64_t tsc;
    asm volatile("rdtsc" : "=A"(tsc));
    count->QuadPart = tsc;
    return TRUE;
}

BOOL WINAPI win_QueryPerformanceFrequency(PLARGE_INTEGER freq) {
    if (!freq) return FALSE;
    
    /* 假设 3GHz TSC */
    freq->QuadPart = 3000000000ULL;
    return TRUE;
}

/* ========== Heap 操作完善 ========== */

BOOL WINAPI win_HeapDestroy(HANDLE heap) {
    (void)heap;
    return TRUE;
}

LPVOID WINAPI win_HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, size_t size) {
    (void)heap;
    (void)flags;
    
    if (!mem) return win_HeapAlloc((HANDLE)1, flags, size);
    
    /* 简化：分配新内存并复制 */
    LPVOID new_mem = win_VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, 0);
    if (new_mem && mem) {
        /* 无法查询原大小，简化处理 */
        memcpy(new_mem, mem, size);
    }
    return new_mem;
}

size_t WINAPI win_HeapSize(HANDLE heap, DWORD flags, LPCVOID mem) {
    (void)heap;
    (void)flags;
    (void)mem;
    return 0; /* 无法查询 */
}

/* ========== COM/OLE 实现 ========== */

HRESULT WINAPI win_CoInitialize(LPVOID reserved) {
    (void)reserved;
    return S_OK;
}

void WINAPI win_CoUninitialize(void) {
}

HRESULT WINAPI win_CoInitializeEx(LPVOID reserved, DWORD coinit) {
    (void)reserved;
    (void)coinit;
    return S_OK;
}

HRESULT WINAPI win_CoCreateInstance(REFCLSID rclsid, void *outer, DWORD clsctx, REFIID riid, LPVOID *ppv) {
    (void)rclsid;
    (void)outer;
    (void)clsctx;
    (void)riid;
    
    if (ppv) *ppv = NULL;
    return E_NOINTERFACE;
}

LPVOID WINAPI win_CoTaskMemAlloc(size_t size) {
    return kmalloc(size);
}

LPVOID WINAPI win_CoTaskMemRealloc(LPVOID mem, size_t size) {
    (void)mem;
    return kmalloc(size);
}

void WINAPI win_CoTaskMemFree(LPVOID mem) {
    kfree(mem);
}

BSTR WINAPI win_SysAllocString(const OLECHAR *str) {
    if (!str) return NULL;
    
    size_t len = 0;
    const OLECHAR *p = str;
    while (*p++) len++;
    
    BSTR bstr = (BSTR)kmalloc((len + 1) * sizeof(OLECHAR) + 4);
    if (!bstr) return NULL;
    
    *(DWORD*)bstr = (DWORD)(len * sizeof(OLECHAR));
    memcpy((BYTE*)bstr + 4, str, (len + 1) * sizeof(OLECHAR));
    
    return bstr;
}

void WINAPI win_SysFreeString(BSTR bstr) {
    if (bstr) kfree(bstr);
}

UINT WINAPI win_SysStringLen(BSTR bstr) {
    if (!bstr) return 0;
    return *(DWORD*)bstr / sizeof(OLECHAR);
}

/* ========== 网络 API 实现 ========== */

/* 简单的 atoi 实现 */
static int win_atoi(const char *str) {
    if (!str) return 0;
    int result = 0;
    int sign = 1;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '-') { sign = -1; str++; }
    else if (*str == '+') str++;
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result * sign;
}

/* WSA 初始化状态 */
static int wsa_initialized = 0;
static DWORD wsa_last_error = 0;
static spinlock_t wsa_lock = SPINLOCK_INIT;

/* 套接字额外状态 */
struct win_socket_extra {
    int non_blocking;
    int wsa_error;
    HANDLE event_handle;
    LONG event_mask;
};

/* Windows <-> BSD 地址族映射 */
static int win_to_bsd_af(int win_af) {
    switch (win_af) {
        case WIN_AF_INET:  return AF_INET;
        case WIN_AF_INET6: return AF_INET6;
        case WIN_AF_UNSPEC: return AF_UNSPEC;
        default: return win_af;
    }
}

static int bsd_to_win_af(int bsd_af) {
    switch (bsd_af) {
        case AF_INET:  return WIN_AF_INET;
        case AF_INET6: return WIN_AF_INET6;
        case AF_UNSPEC: return WIN_AF_UNSPEC;
        default: return bsd_af;
    }
}

static int win_to_bsd_socktype(int win_type) {
    switch (win_type) {
        case WIN_SOCK_STREAM:  return SOCK_STREAM;
        case WIN_SOCK_DGRAM:   return SOCK_DGRAM;
        case WIN_SOCK_RAW:     return SOCK_RAW;
        default: return win_type;
    }
}

static int win_to_bsd_protocol(int win_proto) {
    switch (win_proto) {
        case WIN_IPPROTO_TCP: return IPPROTO_TCP;
        case WIN_IPPROTO_UDP: return IPPROTO_UDP;
        case WIN_IPPROTO_ICMP: return IPPROTO_ICMP;
        case WIN_IPPROTO_IP:  return IPPROTO_IP;
        default: return win_proto;
    }
}

/* sockaddr 转换 */
static void win_to_bsd_sockaddr(const win_sockaddr *win_addr, struct sockaddr *bsd_addr, int *bsd_len) {
    if (win_addr == NULL || bsd_addr == NULL) { *bsd_len = 0; return; }
    const win_sockaddr_in *win_in = (const win_sockaddr_in *)win_addr;
    struct sockaddr_in *bsd_in = (struct sockaddr_in *)bsd_addr;
    if (win_in->sin_family == WIN_AF_INET) {
        bsd_in->sin_family = AF_INET;
        bsd_in->sin_port = win_in->sin_port;
        bsd_in->sin_addr.s_addr = win_in->sin_addr.s_addr;
        for (int i = 0; i < 8; i++) bsd_in->sin_zero[i] = 0;
        *bsd_len = sizeof(struct sockaddr_in);
    } else {
        for (int i = 0; i < 16 && i < sizeof(struct sockaddr); i++)
            ((char *)bsd_addr)[i] = ((const char *)win_addr)[i];
        *bsd_len = 16;
    }
}

static void bsd_to_win_sockaddr(const struct sockaddr *bsd_addr, win_sockaddr *win_addr, int *win_len) {
    if (bsd_addr == NULL || win_addr == NULL) return;
    const struct sockaddr_in *bsd_in = (const struct sockaddr_in *)bsd_addr;
    win_sockaddr_in *win_in = (win_sockaddr_in *)win_addr;
    if (bsd_in->sin_family == AF_INET) {
        win_in->sin_family = WIN_AF_INET;
        win_in->sin_port = bsd_in->sin_port;
        win_in->sin_addr.s_addr = bsd_in->sin_addr.s_addr;
        for (int i = 0; i < 8; i++) win_in->sin_zero[i] = 0;
        if (win_len) *win_len = sizeof(win_sockaddr_in);
    } else {
        for (int i = 0; i < 16; i++)
            ((char *)win_addr)[i] = ((const char *)bsd_addr)[i];
        if (win_len) *win_len = 16;
    }
}

/* 内核 fd <-> Windows SOCKET */
static SOCKET fd_to_win_socket(int kfd) {
    struct win_socket_extra *extra = kmalloc(sizeof(struct win_socket_extra));
    if (extra == NULL) return INVALID_SOCKET;
    memset(extra, 0, sizeof(*extra));
    HANDLE h = alloc_handle(WIN_HANDLE_SOCKET, (uint64_t)kfd, (uint64_t)extra);
    if (h == INVALID_HANDLE_VALUE) { kfree(extra); return INVALID_SOCKET; }
    return (SOCKET)h;
}

static int win_socket_to_fd(SOCKET s) {
    int i = lookup_handle((HANDLE)s, WIN_HANDLE_SOCKET);
    if (i < 0) return -1;
    return (int)win_handles[i].data;
}

static struct win_socket_extra *win_socket_get_extra(SOCKET s) {
    int i = lookup_handle((HANDLE)s, WIN_HANDLE_SOCKET);
    if (i < 0) return NULL;
    return (struct win_socket_extra *)win_handles[i].extra;
}

/* DNS 解析器 */
static int dns_build_query(uint8_t *buf, const char *name, uint16_t type) {
    uint8_t *p = buf;
    uint16_t id = (uint16_t)((uint64_t)buf ^ (uint64_t)name);
    *p++ = (id >> 8) & 0xFF; *p++ = id & 0xFF;
    *p++ = 0x01; *p++ = 0x00;
    *p++ = 0x00; *p++ = 0x01;
    *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x00; *p++ = 0x00;
    const char *np = name;
    while (*np) {
        const char *dot = strchr(np, '.');
        int len = dot ? (int)(dot - np) : (int)strlen(np);
        if (len > 63) len = 63;
        *p++ = (uint8_t)len;
        for (int i = 0; i < len; i++) *p++ = np[i];
        if (dot) np = dot + 1; else break;
    }
    *p++ = 0;
    *p++ = 0; *p++ = (uint8_t)type;
    *p++ = 0; *p++ = 0x01;
    return (int)(p - buf);
}

static uint32_t dns_parse_response(const uint8_t *buf, int len) {
    if (len < 12) return 0;
    int questions = (buf[4] << 8) | buf[5];
    int answers = (buf[6] << 8) | buf[7];
    if (answers == 0) return 0;
    int pos = 12;
    for (int i = 0; i < questions; i++) {
        while (pos < len) {
            uint8_t c = buf[pos++];
            if (c == 0) break;
            if ((c & 0xC0) == 0xC0) { pos++; break; }
            pos += c;
        }
        pos += 4;
    }
    for (int i = 0; i < answers && pos < len; i++) {
        while (pos < len) {
            uint8_t c = buf[pos++];
            if (c == 0) break;
            if ((c & 0xC0) == 0xC0) { pos++; break; }
            pos += c;
        }
        if (pos + 10 > len) return 0;
        uint16_t type = (buf[pos] << 8) | buf[pos+1]; pos += 2;
        pos += 2;
        pos += 4;
        uint16_t rdlen = (buf[pos] << 8) | buf[pos+1]; pos += 2;
        if (type == DNS_TYPE_A && rdlen == 4 && pos + 4 <= len) {
            return ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos+1] << 16) |
                   ((uint32_t)buf[pos+2] << 8) | (uint32_t)buf[pos+3];
        }
        pos += rdlen;
    }
    return 0;
}

static uint32_t dns_resolve_name(const char *name) {
    if (!name) return 0;
    uint32_t dns_server = net_get_dns();
    if (dns_server == 0) return 0;

    int kfd = sys_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (kfd < 0) return 0;

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = 0;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    sys_bind(kfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    uint8_t query[512];
    int qlen = dns_build_query(query, name, DNS_TYPE_A);
    if (qlen <= 0) { sys_shutdown(kfd, SHUT_RDWR); return 0; }

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(53);
    srv_addr.sin_addr.s_addr = dns_server;
    sys_sendto(kfd, query, qlen, 0, (struct sockaddr *)&srv_addr, sizeof(srv_addr));

    uint8_t response[512];
    uint32_t result = 0;
    for (int timeout = 0; timeout < 300 && result == 0; timeout++) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int rlen = sys_recvfrom(kfd, response, sizeof(response), 0, (struct sockaddr *)&from, &fromlen);
        if (rlen > 0) result = dns_parse_response(response, rlen);
        if (result == 0) {
            for (int d = 0; d < 10; d++) net_poll();
        }
    }

    sys_shutdown(kfd, SHUT_RDWR);
    return result;
}

/* ========== WS2_32 函数实现 ========== */

int WINAPI win_WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData) {
    if (lpWSAData == NULL) return WSAEFAULT;
    BYTE major = wVersionRequested & 0xFF;
    BYTE minor = (wVersionRequested >> 8) & 0xFF;
    if (major < 1 || (major == 1 && minor < 1)) return WSAVERNOTSUPPORTED;
    memset(lpWSAData, 0, sizeof(*lpWSAData));
    lpWSAData->wVersion = 0x0202;
    lpWSAData->wHighVersion = 0x0202;
    memcpy(lpWSAData->szDescription, "ZratroOS Winsock 2.2", 21);
    memcpy(lpWSAData->szSystemStatus, "Running", 8);
    lpWSAData->iMaxSockets = 64;
    lpWSAData->iMaxUdpDg = 65507;
    lpWSAData->lpVendorInfo = NULL;
    spin_lock(&wsa_lock);
    wsa_initialized++;
    spin_unlock(&wsa_lock);
    return 0;
}

int WINAPI win_WSACleanup(void) {
    spin_lock(&wsa_lock);
    if (wsa_initialized > 0) wsa_initialized--;
    spin_unlock(&wsa_lock);
    return 0;
}

int WINAPI win_WSAGetLastError(void) { return (int)wsa_last_error; }
void WINAPI win_WSASetLastError(int iError) { wsa_last_error = (DWORD)iError; }

SOCKET WINAPI win_socket(int af, int type, int protocol) {
    int kfd = sys_socket(win_to_bsd_af(af), win_to_bsd_socktype(type), win_to_bsd_protocol(protocol));
    if (kfd < 0) { wsa_last_error = WSAENETDOWN; return INVALID_SOCKET; }
    SOCKET s = fd_to_win_socket(kfd);
    if (s == INVALID_SOCKET) { wsa_last_error = WSAEMFILE; }
    return s;
}

SOCKET WINAPI win_WSASocketA(int af, int type, int protocol, LPVOID lpProtocolInfo, DWORD g, DWORD dwFlags) {
    (void)lpProtocolInfo; (void)g; (void)dwFlags;
    return win_socket(af, type, protocol);
}

SOCKET WINAPI win_WSASocketW(int af, int type, int protocol, LPVOID lpProtocolInfo, DWORD g, DWORD dwFlags) {
    return win_WSASocketA(af, type, protocol, lpProtocolInfo, g, dwFlags);
}

int WINAPI win_closesocket(SOCKET s) {
    int i = lookup_handle((HANDLE)s, WIN_HANDLE_SOCKET);
    if (i < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    int kfd = (int)win_handles[i].data;
    struct win_socket_extra *extra = (struct win_socket_extra *)win_handles[i].extra;
    sys_shutdown(kfd, SHUT_RDWR);
    if (extra) kfree(extra);
    win_handles[i].used = 0;
    win_handles[i].ref_count = 0;
    return 0;
}

int WINAPI win_connect(SOCKET s, const win_sockaddr *name, int namelen) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    struct sockaddr bsd_addr; int bsd_len;
    win_to_bsd_sockaddr(name, &bsd_addr, &bsd_len);
    if (sys_connect(kfd, &bsd_addr, (socklen_t)bsd_len) < 0) {
        wsa_last_error = WSAECONNREFUSED; return SOCKET_ERROR;
    }
    return 0;
}

int WINAPI win_send(SOCKET s, const char *buf, int len, int flags) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    ssize_t ret = sys_send(kfd, buf, (size_t)len, flags);
    if (ret < 0) { wsa_last_error = WSAENETDOWN; return SOCKET_ERROR; }
    return (int)ret;
}

int WINAPI win_recv(SOCKET s, char *buf, int len, int flags) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    ssize_t ret = sys_recv(kfd, buf, (size_t)len, flags);
    if (ret < 0) { wsa_last_error = WSAENETDOWN; return SOCKET_ERROR; }
    return (int)ret;
}

int WINAPI win_sendto(SOCKET s, const char *buf, int len, int flags, const win_sockaddr *to, int tolen) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    struct sockaddr bsd_addr; int bsd_len;
    win_to_bsd_sockaddr(to, &bsd_addr, &bsd_len);
    ssize_t ret = sys_sendto(kfd, buf, (size_t)len, flags, &bsd_addr, (socklen_t)bsd_len);
    if (ret < 0) { wsa_last_error = WSAENETDOWN; return SOCKET_ERROR; }
    return (int)ret;
}

int WINAPI win_recvfrom(SOCKET s, char *buf, int len, int flags, win_sockaddr *from, int *fromlen) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    struct sockaddr bsd_addr;
    socklen_t bsd_len = sizeof(bsd_addr);
    ssize_t ret = sys_recvfrom(kfd, buf, (size_t)len, flags, &bsd_addr, &bsd_len);
    if (ret < 0) { wsa_last_error = WSAENETDOWN; return SOCKET_ERROR; }
    if (from && fromlen) bsd_to_win_sockaddr(&bsd_addr, from, fromlen);
    return (int)ret;
}

int WINAPI win_bind(SOCKET s, const win_sockaddr *addr, int namelen) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    struct sockaddr bsd_addr; int bsd_len;
    win_to_bsd_sockaddr(addr, &bsd_addr, &bsd_len);
    if (sys_bind(kfd, &bsd_addr, (socklen_t)bsd_len) < 0) { wsa_last_error = WSAEADDRINUSE; return SOCKET_ERROR; }
    return 0;
}

int WINAPI win_listen(SOCKET s, int backlog) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    if (sys_listen(kfd, backlog) < 0) { wsa_last_error = WSAEOPNOTSUPP; return SOCKET_ERROR; }
    return 0;
}

SOCKET WINAPI win_accept(SOCKET s, win_sockaddr *addr, int *addrlen) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return INVALID_SOCKET; }
    struct sockaddr bsd_addr;
    socklen_t bsd_len = sizeof(bsd_addr);
    int newkfd = sys_accept(kfd, &bsd_addr, &bsd_len);
    if (newkfd < 0) { wsa_last_error = WSAEWOULDBLOCK; return INVALID_SOCKET; }
    SOCKET newsock = fd_to_win_socket(newkfd);
    if (newsock == INVALID_SOCKET) { wsa_last_error = WSAEMFILE; return INVALID_SOCKET; }
    if (addr && addrlen) bsd_to_win_sockaddr(&bsd_addr, addr, addrlen);
    return newsock;
}

int WINAPI win_select(int nfds, win_fd_set *readfds, win_fd_set *writefds, win_fd_set *exceptfds, const win_timeval *timeout) {
    (void)nfds;
    struct pollfd pfds[WIN_FD_SETSIZE * 3];
    int pfd_count = 0;
    SOCKET win_to_pfd[WIN_FD_SETSIZE * 3];

    if (readfds) {
        for (uint32_t i = 0; i < readfds->fd_count && pfd_count < WIN_FD_SETSIZE * 3; i++) {
            int kfd = win_socket_to_fd(readfds->fd_array[i]);
            if (kfd >= 0) {
                pfds[pfd_count].fd = kfd; pfds[pfd_count].events = POLLIN; pfds[pfd_count].revents = 0;
                win_to_pfd[pfd_count] = readfds->fd_array[i]; pfd_count++;
            }
        }
    }
    if (writefds) {
        for (uint32_t i = 0; i < writefds->fd_count && pfd_count < WIN_FD_SETSIZE * 3; i++) {
            int kfd = win_socket_to_fd(writefds->fd_array[i]);
            if (kfd >= 0) {
                pfds[pfd_count].fd = kfd; pfds[pfd_count].events = POLLOUT; pfds[pfd_count].revents = 0;
                win_to_pfd[pfd_count] = writefds->fd_array[i]; pfd_count++;
            }
        }
    }
    if (exceptfds) {
        for (uint32_t i = 0; i < exceptfds->fd_count && pfd_count < WIN_FD_SETSIZE * 3; i++) {
            int kfd = win_socket_to_fd(exceptfds->fd_array[i]);
            if (kfd >= 0) {
                pfds[pfd_count].fd = kfd; pfds[pfd_count].events = POLLERR; pfds[pfd_count].revents = 0;
                win_to_pfd[pfd_count] = exceptfds->fd_array[i]; pfd_count++;
            }
        }
    }
    if (pfd_count == 0) return 0;

    int timeout_ms = -1;
    if (timeout) timeout_ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
    int ready = sys_poll(pfds, (nfds_t)pfd_count, timeout_ms);
    if (ready < 0) return SOCKET_ERROR;

    if (readfds) readfds->fd_count = 0;
    if (writefds) writefds->fd_count = 0;
    if (exceptfds) exceptfds->fd_count = 0;

    int total_ready = 0;
    for (int i = 0; i < pfd_count; i++) {
        SOCKET ws = win_to_pfd[i];
        if ((pfds[i].revents & POLLIN) && readfds && readfds->fd_count < WIN_FD_SETSIZE) {
            readfds->fd_array[readfds->fd_count++] = ws; total_ready++;
        }
        if ((pfds[i].revents & POLLOUT) && writefds && writefds->fd_count < WIN_FD_SETSIZE) {
            writefds->fd_array[writefds->fd_count++] = ws; total_ready++;
        }
        if ((pfds[i].revents & (POLLERR | POLLHUP)) && exceptfds && exceptfds->fd_count < WIN_FD_SETSIZE) {
            exceptfds->fd_array[exceptfds->fd_count++] = ws; total_ready++;
        }
    }
    return total_ready;
}

int WINAPI win_shutdown(SOCKET s, int how) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    int bsd_how;
    switch (how) { case WIN_SD_RECEIVE: bsd_how = SHUT_RD; break; case WIN_SD_SEND: bsd_how = SHUT_WR; break; default: bsd_how = SHUT_RDWR; break; }
    return sys_shutdown(kfd, bsd_how);
}

int WINAPI win_getsockname(SOCKET s, win_sockaddr *name, int *namelen) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    struct sockaddr bsd_addr; socklen_t bsd_len = sizeof(bsd_addr);
    if (sys_getsockname(kfd, &bsd_addr, &bsd_len) < 0) { wsa_last_error = WSAEFAULT; return SOCKET_ERROR; }
    bsd_to_win_sockaddr(&bsd_addr, name, namelen);
    return 0;
}

int WINAPI win_getpeername(SOCKET s, win_sockaddr *name, int *namelen) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    struct sockaddr bsd_addr; socklen_t bsd_len = sizeof(bsd_addr);
    if (sys_getpeername(kfd, &bsd_addr, &bsd_len) < 0) { wsa_last_error = WSAEFAULT; return SOCKET_ERROR; }
    bsd_to_win_sockaddr(&bsd_addr, name, namelen);
    return 0;
}

int WINAPI win_getsockopt(SOCKET s, int level, int optname, char *optval, int *optlen) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    int bsd_level = (level == WIN_SOL_SOCKET) ? SOL_SOCKET : level;
    return sys_getsockopt(kfd, bsd_level, optname, optval, (socklen_t *)optlen);
}

int WINAPI win_setsockopt(SOCKET s, int level, int optname, const char *optval, int optlen) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    int bsd_level = (level == WIN_SOL_SOCKET) ? SOL_SOCKET : level;
    return sys_setsockopt(kfd, bsd_level, optname, optval, (socklen_t)optlen);
}

int WINAPI win_ioctlsocket(SOCKET s, LONG cmd, ULONG *argp) {
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) { wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    switch (cmd) {
        case 0x8004667E: {
            struct win_socket_extra *extra = win_socket_get_extra(s);
            if (extra) extra->non_blocking = (*argp != 0) ? 1 : 0;
            return 0;
        }
        case 0x4004667F: {
            struct socket *sock = get_socket(kfd);
            if (sock) { *argp = (ULONG)sock->recv_buf_len; return 0; }
            wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR;
        }
        case 0x40047307: { *argp = 1; return 0; }
        default: wsa_last_error = WSAEINVAL; return SOCKET_ERROR;
    }
}

int WINAPI win_WSASend(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesSent, DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped, void *lpCompletionRoutine) {
    (void)dwBufferCount; (void)lpOverlapped; (void)lpCompletionRoutine;
    if (lpBuffers == NULL || lpNumberOfBytesSent == NULL) { wsa_last_error = WSAEFAULT; return SOCKET_ERROR; }
    int ret = win_send(s, lpBuffers->buf, (int)lpBuffers->len, (int)dwFlags);
    if (ret == SOCKET_ERROR) return SOCKET_ERROR;
    *lpNumberOfBytesSent = (DWORD)ret;
    return 0;
}

int WINAPI win_WSARecv(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount, LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped, void *lpCompletionRoutine) {
    (void)dwBufferCount; (void)lpOverlapped; (void)lpCompletionRoutine;
    if (lpBuffers == NULL || lpNumberOfBytesRecvd == NULL) { wsa_last_error = WSAEFAULT; return SOCKET_ERROR; }
    int ret = win_recv(s, lpBuffers->buf, (int)lpBuffers->len, (int)*lpFlags);
    if (ret == SOCKET_ERROR) return SOCKET_ERROR;
    *lpNumberOfBytesRecvd = (DWORD)ret;
    return 0;
}

int WINAPI win_WSAAsyncSelect(SOCKET s, HWND hWnd, unsigned int wMsg, LONG lEvent) {
    (void)s; (void)hWnd; (void)wMsg; (void)lEvent;
    return 0;
}

int WINAPI win_WSAEventSelect(SOCKET s, HANDLE hEventObject, LONG lNetworkEvents) {
    int i = lookup_handle((HANDLE)s, WIN_HANDLE_SOCKET);
    if (i < 0) return SOCKET_ERROR;
    struct win_socket_extra *extra = (struct win_socket_extra *)win_handles[i].extra;
    if (extra) { extra->event_handle = hEventObject; extra->event_mask = lNetworkEvents; }
    return 0;
}

int WINAPI win_WSAEnumNetworkEvents(SOCKET s, HANDLE hEventObject, WSANETWORKEVENTS *lpNetworkEvents) {
    (void)hEventObject;
    if (lpNetworkEvents == NULL) return SOCKET_ERROR;
    int kfd = win_socket_to_fd(s);
    if (kfd < 0) return SOCKET_ERROR;
    memset(lpNetworkEvents, 0, sizeof(*lpNetworkEvents));
    struct socket *sock = get_socket(kfd);
    if (sock && sock->recv_buf_len > 0) lpNetworkEvents->lNetworkEvents |= WIN_FD_READ;
    if (sock && sock->send_buf_len < sock->send_buf_size) lpNetworkEvents->lNetworkEvents |= WIN_FD_WRITE;
    return 0;
}

BOOL WINAPI win_WSAGetOverlappedResult(SOCKET s, LPWSAOVERLAPPED lpOverlapped, LPDWORD lpcbTransfer, BOOL fWait, LPDWORD lpdwFlags) {
    (void)s; (void)lpOverlapped; (void)fWait;
    if (lpcbTransfer) *lpcbTransfer = 0;
    if (lpdwFlags) *lpdwFlags = 0;
    return TRUE;
}

int WINAPI win_getaddrinfo(const char *nodename, const char *servname, const win_addrinfo *hints, win_addrinfo **result) {
    if (result == NULL) return WSAEINVAL;
    *result = NULL;
    int ai_family = WIN_AF_INET, ai_socktype = 0, ai_protocol = 0, ai_flags = 0;
    if (hints) { ai_family = hints->ai_family; ai_socktype = hints->ai_socktype; ai_protocol = hints->ai_protocol; ai_flags = hints->ai_flags; }

    uint16_t port = 0;
    if (servname) {
        port = (uint16_t)win_atoi(servname);
        if (port == 0) {
            if (strcmp(servname, "http") == 0) port = 80;
            else if (strcmp(servname, "https") == 0) port = 443;
            else if (strcmp(servname, "ftp") == 0) port = 21;
            else if (strcmp(servname, "ssh") == 0) port = 22;
            else if (strcmp(servname, "smtp") == 0) port = 25;
            else if (strcmp(servname, "dns") == 0) port = 53;
        }
    }

    uint32_t ip_addr = 0;
    if (nodename == NULL) {
        ip_addr = (ai_flags & WIN_AI_PASSIVE) ? 0 : 0x7F000001;
    } else {
        ip_addr = net_aton(nodename);
        if (ip_addr == 0) {
            ip_addr = dns_resolve_name(nodename);
            if (ip_addr == 0) return WSAHOST_NOT_FOUND;
        }
    }

    win_addrinfo *ai = kmalloc(sizeof(win_addrinfo));
    if (ai == NULL) return WSAENOMEM;
    memset(ai, 0, sizeof(*ai));
    win_sockaddr_in *sa = kmalloc(sizeof(win_sockaddr_in));
    if (sa == NULL) { kfree(ai); return WSAENOMEM; }
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = WIN_AF_INET;
    sa->sin_port = htons(port);
    sa->sin_addr.s_addr = ip_addr;
    ai->ai_family = WIN_AF_INET;
    ai->ai_socktype = ai_socktype ? ai_socktype : WIN_SOCK_STREAM;
    ai->ai_protocol = ai_protocol ? ai_protocol : WIN_IPPROTO_TCP;
    ai->ai_addrlen = sizeof(win_sockaddr_in);
    ai->ai_addr = (win_sockaddr *)sa;
    ai->ai_next = NULL;
    *result = ai;
    return 0;
}

void WINAPI win_freeaddrinfo(win_addrinfo *pAddrInfo) {
    while (pAddrInfo) {
        win_addrinfo *next = pAddrInfo->ai_next;
        if (pAddrInfo->ai_addr) kfree(pAddrInfo->ai_addr);
        if (pAddrInfo->ai_canonname) kfree(pAddrInfo->ai_canonname);
        kfree(pAddrInfo);
        pAddrInfo = next;
    }
}

win_hostent *WINAPI win_gethostbyname(const char *name) {
    static win_hostent hostent;
    static char *h_aliases[2] = {NULL, NULL};
    static char *h_addr_list[2] = {NULL, NULL};
    static uint32_t h_addr_val;
    static char h_name_buf[256];
    if (name == NULL) return NULL;
    uint32_t ip = net_aton(name);
    if (ip == 0) { ip = dns_resolve_name(name); if (ip == 0) return NULL; }
    strncpy(h_name_buf, name, 255); h_name_buf[255] = '\0';
    h_addr_val = ip;
    h_addr_list[0] = (char *)&h_addr_val; h_addr_list[1] = NULL;
    hostent.h_name = h_name_buf;
    hostent.h_aliases = h_aliases;
    hostent.h_addrtype = WIN_AF_INET;
    hostent.h_length = 4;
    hostent.h_addr_list = h_addr_list;
    return &hostent;
}

int WINAPI win_gethostname(char *name, int namelen) {
    if (name == NULL || namelen <= 0) { wsa_last_error = WSAEFAULT; return SOCKET_ERROR; }
    const char *hostname = "zratroos";
    int len = 8;
    if (len >= namelen) len = namelen - 1;
    memcpy(name, hostname, len);
    name[len] = '\0';
    return 0;
}

win_protoent *WINAPI win_getprotobyname(const char *name) {
    static win_protoent protoent;
    static char *p_aliases[2] = {NULL, NULL};
    static char p_name[32];
    if (strcmp(name, "tcp") == 0) { strncpy(p_name, "tcp", 31); protoent.p_name = p_name; protoent.p_aliases = p_aliases; protoent.p_proto = WIN_IPPROTO_TCP; return &protoent; }
    if (strcmp(name, "udp") == 0) { strncpy(p_name, "udp", 31); protoent.p_name = p_name; protoent.p_aliases = p_aliases; protoent.p_proto = WIN_IPPROTO_UDP; return &protoent; }
    if (strcmp(name, "ip") == 0) { strncpy(p_name, "ip", 31); protoent.p_name = p_name; protoent.p_aliases = p_aliases; protoent.p_proto = WIN_IPPROTO_IP; return &protoent; }
    if (strcmp(name, "icmp") == 0) { strncpy(p_name, "icmp", 31); protoent.p_name = p_name; protoent.p_aliases = p_aliases; protoent.p_proto = WIN_IPPROTO_ICMP; return &protoent; }
    return NULL;
}

win_servent *WINAPI win_getservbyname(const char *name, const char *proto) {
    static win_servent servent;
    static char *s_aliases[2] = {NULL, NULL};
    static char s_name[32], s_proto[8];
    struct { const char *name; int port; } services[] = {
        {"http", 80}, {"https", 443}, {"ftp", 21}, {"ssh", 22},
        {"smtp", 25}, {"dns", 53}, {"pop3", 110}, {"imap", 143},
        {"ntp", 123}, {"telnet", 23}, {NULL, 0}
    };
    for (int i = 0; services[i].name; i++) {
        if (strcmp(name, services[i].name) == 0) {
            strncpy(s_name, name, 31); s_name[31] = '\0';
            strncpy(s_proto, proto ? proto : "tcp", 7); s_proto[7] = '\0';
            servent.s_name = s_name; servent.s_aliases = s_aliases;
            servent.s_port = htons(services[i].port); servent.s_proto = s_proto;
            return &servent;
        }
    }
    return NULL;
}

uint32_t WINAPI win_ntohl(uint32_t netlong) { return ntohl(netlong); }
uint16_t WINAPI win_ntohs(uint16_t netshort) { return ntohs(netshort); }
uint32_t WINAPI win_htonl(uint32_t hostlong) { return htonl(hostlong); }
uint16_t WINAPI win_htons(uint16_t hostshort) { return htons(hostshort); }
uint32_t WINAPI win_inet_addr(const char *cp) { return net_aton(cp); }
char *WINAPI win_inet_ntoa(uint32_t in_addr) { return net_ntoa(in_addr); }

/* ========== IPHLPAPI 函数实现 ========== */

DWORD WINAPI win_GetAdaptersInfo(IP_ADAPTER_INFO *pAdapterInfo, ULONG *pOutBufLen) {
    if (pOutBufLen == NULL) return ERROR_INVALID_PARAMETER;
    ULONG needed_size = sizeof(IP_ADAPTER_INFO);
    if (pAdapterInfo == NULL || *pOutBufLen < needed_size) { *pOutBufLen = needed_size; return ERROR_BUFFER_OVERFLOW; }
    memset(pAdapterInfo, 0, sizeof(*pAdapterInfo));
    struct nic_device *nic = net_get_first_nic();
    if (nic == NULL) return ERROR_NO_DATA;
    pAdapterInfo->Next = NULL;
    pAdapterInfo->ComboIndex = 0;
    strncpy(pAdapterInfo->AdapterName, nic->name, MAX_ADAPTER_NAME_LENGTH);
    strncpy(pAdapterInfo->Description, "ZratroOS Network Adapter", MAX_ADAPTER_DESCRIPTION_LENGTH);
    pAdapterInfo->AddressLength = 6;
    for (int i = 0; i < 6; i++) pAdapterInfo->Address[i] = nic->mac.addr[i];
    pAdapterInfo->Index = nic->id;
    pAdapterInfo->Type = MIB_IF_TYPE_ETHERNET;
    pAdapterInfo->DhcpEnabled = 1;
    pAdapterInfo->Mtu = nic->mtu ? nic->mtu : 1500;
    pAdapterInfo->Speed = 1000000000;
    return NO_ERROR;
}

DWORD WINAPI win_GetAdaptersAddresses(ULONG Family, DWORD Flags, void *Reserved, IP_ADAPTER_ADDRESSES *pAdapterAddresses, ULONG *pOutBufLen) {
    (void)Family; (void)Flags; (void)Reserved;
    if (pOutBufLen == NULL) return ERROR_INVALID_PARAMETER;
    ULONG needed = sizeof(IP_ADAPTER_ADDRESSES);
    if (pAdapterAddresses == NULL || *pOutBufLen < needed) { *pOutBufLen = needed; return ERROR_BUFFER_OVERFLOW; }
    memset(pAdapterAddresses, 0, sizeof(*pAdapterAddresses));
    struct nic_device *nic = net_get_first_nic();
    if (nic == NULL) return ERROR_NO_DATA;
    pAdapterAddresses->Length = sizeof(IP_ADAPTER_ADDRESSES);
    pAdapterAddresses->IfIndex = nic->id;
    pAdapterAddresses->Next = NULL;
    pAdapterAddresses->AdapterName = nic->name;
    pAdapterAddresses->PhysicalAddressLength = 6;
    for (int i = 0; i < 6; i++) pAdapterAddresses->PhysicalAddress[i] = nic->mac.addr[i];
    pAdapterAddresses->Mtu = nic->mtu ? nic->mtu : 1500;
    pAdapterAddresses->IfType = IF_TYPE_ETHERNET_CSMACD;
    pAdapterAddresses->OperStatus = 1;
    return NO_ERROR;
}

DWORD WINAPI win_GetBestInterface(uint32_t dwDestAddr, DWORD *pdwBestIfIndex) {
    (void)dwDestAddr;
    if (pdwBestIfIndex == NULL) return ERROR_INVALID_PARAMETER;
    struct nic_device *nic = net_get_first_nic();
    if (nic == NULL) return ERROR_NO_DATA;
    *pdwBestIfIndex = nic->id;
    return NO_ERROR;
}

DWORD WINAPI win_GetIpAddrTable(MIB_IPADDRTABLE *pIpAddrTable, ULONG *pdwSize, BOOL bOrder) {
    (void)bOrder;
    if (pdwSize == NULL) return ERROR_INVALID_PARAMETER;
    ULONG needed = sizeof(MIB_IPADDRTABLE);
    if (pIpAddrTable == NULL || *pdwSize < needed) { *pdwSize = needed; return ERROR_INSUFFICIENT_BUFFER; }
    memset(pIpAddrTable, 0, sizeof(*pIpAddrTable));
    struct nic_device *nic = net_get_first_nic();
    if (nic) {
        pIpAddrTable->dwNumEntries = 1;
        pIpAddrTable->table[0].dwAddr = nic->ip_addr;
        pIpAddrTable->table[0].dwIndex = nic->id;
        pIpAddrTable->table[0].dwMask = nic->netmask;
    }
    return NO_ERROR;
}

DWORD WINAPI win_GetIfTable(void *pIfTable, ULONG *pdwSize, BOOL bOrder) {
    (void)bOrder;
    if (pdwSize == NULL) return ERROR_INVALID_PARAMETER;
    *pdwSize = 4;
    if (pIfTable == NULL) return ERROR_INSUFFICIENT_BUFFER;
    *(DWORD *)pIfTable = 0;
    return NO_ERROR;
}

DWORD WINAPI win_GetNumberOfInterfaces(DWORD *lpdwNumIf) {
    if (lpdwNumIf == NULL) return ERROR_INVALID_PARAMETER;
    *lpdwNumIf = 2;
    return NO_ERROR;
}

DWORD WINAPI win_NotifyAddrChange(HANDLE *Handle, LPOVERLAPPED overlapped) {
    (void)Handle; (void)overlapped; return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI win_NotifyRouteChange(HANDLE *Handle, LPOVERLAPPED overlapped) {
    (void)Handle; (void)overlapped; return ERROR_NOT_SUPPORTED;
}

/* ========== DNSAPI 函数实现 ========== */

int WINAPI win_DnsQuery_A(const char *pszName, WORD wType, DWORD Options, void *pExtra, DNS_RECORDA **ppQueryResults, void *pReserved) {
    (void)pExtra; (void)pReserved; (void)Options;
    if (pszName == NULL || ppQueryResults == NULL) return DNS_ERROR_RCODE_NAME_ERROR;
    if (wType != DNS_TYPE_A) return DNS_ERROR_RCODE_NAME_ERROR;
    uint32_t ip = net_aton(pszName);
    if (ip == 0) { ip = dns_resolve_name(pszName); if (ip == 0) return DNS_ERROR_RCODE_NAME_ERROR; }
    DNS_RECORDA *record = kmalloc(sizeof(DNS_RECORDA));
    if (record == NULL) return DNS_ERROR_RCODE_NO_ERROR;
    memset(record, 0, sizeof(*record));
    record->pNext = NULL;
    record->pName = (char *)pszName;
    record->wType = DNS_TYPE_A;
    record->wDataLength = sizeof(DNS_A_DATA);
    record->dwTtl = 300;
    record->Data.A.IpAddress = ip;
    *ppQueryResults = record;
    return DNS_ERROR_RCODE_NO_ERROR;
}

int WINAPI win_DnsQuery_W(const WCHAR *pszName, WORD wType, DWORD Options, void *pExtra, DNS_RECORDA **ppQueryResults, void *pReserved) {
    if (pszName == NULL) return DNS_ERROR_RCODE_NAME_ERROR;
    char ansi[256]; int i;
    for (i = 0; i < 255 && pszName[i]; i++) ansi[i] = (char)pszName[i];
    ansi[i] = '\0';
    return win_DnsQuery_A(ansi, wType, Options, pExtra, ppQueryResults, pReserved);
}

void WINAPI win_DnsRecordListFree(DNS_RECORDA *pRecordList, DWORD FreeType) {
    (void)FreeType;
    while (pRecordList) {
        DNS_RECORDA *next = pRecordList->pNext;
        kfree(pRecordList);
        pRecordList = next;
    }
}

/* ========== WININET 函数实现 ========== */

HINTERNET WINAPI win_InternetOpenA(const char *lpszAgent, DWORD dwAccessType, const char *lpszProxy, const char *lpszProxyBypass, DWORD dwFlags) {
    (void)lpszAgent; (void)dwAccessType; (void)lpszProxy; (void)lpszProxyBypass; (void)dwFlags;
    HANDLE h = alloc_handle(WIN_HANDLE_INET, 0, 0);
    return (HINTERNET)h;
}

HINTERNET WINAPI win_InternetConnectA(HINTERNET hInternet, const char *lpszServerName, INTERNET_PORT nServerPort, const char *lpszUserName, const char *lpszPassword, DWORD dwService, DWORD dwFlags, ULONG_PTR dwContext) {
    (void)hInternet; (void)lpszUserName; (void)lpszPassword; (void)dwService; (void)dwFlags; (void)dwContext;
    uint32_t ip = net_aton(lpszServerName);
    if (ip == 0) { ip = dns_resolve_name(lpszServerName); if (ip == 0) return 0; }
    HANDLE h = alloc_handle(WIN_HANDLE_INET_CONN, ip, (uint64_t)nServerPort);
    return (HINTERNET)h;
}

BOOL WINAPI win_InternetCloseHandle(HINTERNET hInternet) {
    return win_CloseHandle((HANDLE)hInternet);
}

HINTERNET WINAPI win_HttpOpenRequestA(HINTERNET hConnect, const char *lpszVerb, const char *lpszObjectName, const char *lpszVersion, const char *lpszReferrer, const char **lplpszAcceptTypes, DWORD dwFlags, ULONG_PTR dwContext) {
    (void)lpszVersion; (void)lpszReferrer; (void)lplpszAcceptTypes; (void)dwFlags; (void)dwContext;
    int i = lookup_handle((HANDLE)hConnect, WIN_HANDLE_INET_CONN);
    if (i < 0) return 0;
    uint32_t server_ip = (uint32_t)win_handles[i].data;
    uint16_t server_port = (uint16_t)win_handles[i].extra;
    SOCKET http_sock = win_socket(WIN_AF_INET, WIN_SOCK_STREAM, WIN_IPPROTO_TCP);
    if (http_sock == INVALID_SOCKET) return 0;
    win_sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = WIN_AF_INET;
    addr.sin_port = htons(server_port);
    addr.sin_addr.s_addr = server_ip;
    if (win_connect(http_sock, (win_sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        win_closesocket(http_sock); return 0;
    }
    HANDLE h = alloc_handle(WIN_HANDLE_INET_REQ, (uint64_t)i, (uint64_t)http_sock);
    return (HINTERNET)h;
}

BOOL WINAPI win_HttpSendRequestA(HINTERNET hRequest, const char *lpszHeaders, DWORD dwHeadersLength, LPVOID lpOptional, DWORD dwOptionalLength) {
    (void)lpszHeaders; (void)dwHeadersLength; (void)lpOptional; (void)dwOptionalLength;
    int ri = lookup_handle((HANDLE)hRequest, WIN_HANDLE_INET_REQ);
    if (ri < 0) return FALSE;
    SOCKET sock = (SOCKET)win_handles[ri].extra;
    const char *req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    int ret = win_send(sock, req, strlen(req), 0);
    return (ret != SOCKET_ERROR);
}

BOOL WINAPI win_InternetReadFile(HINTERNET hFile, LPVOID lpBuffer, DWORD dwNumberOfBytesToRead, LPDWORD lpdwNumberOfBytesRead) {
    int ri = lookup_handle((HANDLE)hFile, WIN_HANDLE_INET_REQ);
    if (ri < 0) return FALSE;
    SOCKET sock = (SOCKET)win_handles[ri].extra;
    int ret = win_recv(sock, (char *)lpBuffer, (int)dwNumberOfBytesToRead, 0);
    if (ret == SOCKET_ERROR) { if (lpdwNumberOfBytesRead) *lpdwNumberOfBytesRead = 0; return FALSE; }
    if (lpdwNumberOfBytesRead) *lpdwNumberOfBytesRead = (DWORD)ret;
    return TRUE;
}

BOOL WINAPI win_InternetSetOptionA(HINTERNET hInternet, DWORD dwOption, LPVOID lpBuffer, DWORD dwBufferLength) {
    (void)hInternet; (void)dwOption; (void)lpBuffer; (void)dwBufferLength; return TRUE;
}

BOOL WINAPI win_InternetGetLastResponseInfoA(LPDWORD lpdwError, LPSTR lpszBuffer, LPDWORD lpdwBufferLength) {
    if (lpdwError) *lpdwError = 0;
    if (lpszBuffer && lpdwBufferLength && *lpdwBufferLength > 0) lpszBuffer[0] = '\0';
    return TRUE;
}

BOOL WINAPI win_HttpQueryInfoA(HINTERNET hRequest, DWORD dwInfoLevel, LPVOID lpBuffer, LPDWORD lpdwBufferLength, LPDWORD lpdwIndex) {
    (void)hRequest; (void)dwInfoLevel; (void)lpdwIndex;
    if (lpBuffer && lpdwBufferLength && *lpdwBufferLength > 0) { ((char *)lpBuffer)[0] = '\0'; *lpdwBufferLength = 0; }
    return TRUE;
}

BOOL WINAPI win_HttpAddRequestHeadersA(HINTERNET hRequest, LPCSTR lpszHeaders, DWORD dwHeadersLength, DWORD dwModifiers) {
    (void)hRequest; (void)lpszHeaders; (void)dwHeadersLength; (void)dwModifiers; return TRUE;
}

DWORD WINAPI win_InternetErrorDlg(HWND hWndParent, HINTERNET hInternet, DWORD dwError, DWORD dwFlags, LPVOID lppvData) {
    (void)hWndParent; (void)hInternet; (void)dwError; (void)dwFlags; (void)lppvData;
    return ERROR_SUCCESS;
}

/* ========== WINHTTP 函数实现 ========== */

HINTERNET WINAPI win_WinHttpOpen(LPCWSTR pszAgentW, DWORD dwAccessType, LPCWSTR pszProxyW, LPCWSTR pszProxyBypassW, DWORD dwFlags) {
    (void)pszAgentW; (void)dwAccessType; (void)pszProxyW; (void)pszProxyBypassW; (void)dwFlags;
    HANDLE h = alloc_handle(WIN_HANDLE_INET, 0, 0);
    return (HINTERNET)h;
}

HINTERNET WINAPI win_WinHttpConnect(HINTERNET hSession, LPCWSTR pszServerNameW, INTERNET_PORT nServerPort, DWORD dwReserved) {
    (void)hSession; (void)dwReserved;
    char server_name[256]; int i;
    for (i = 0; i < 255 && pszServerNameW && pszServerNameW[i]; i++) server_name[i] = (char)pszServerNameW[i];
    server_name[i] = '\0';
    uint32_t ip = net_aton(server_name);
    if (ip == 0) { ip = dns_resolve_name(server_name); if (ip == 0) return 0; }
    HANDLE h = alloc_handle(WIN_HANDLE_INET_CONN, ip, (uint64_t)nServerPort);
    return (HINTERNET)h;
}

HINTERNET WINAPI win_WinHttpOpenRequest(HINTERNET hConnect, LPCWSTR pszVerbW, LPCWSTR pszObjectNameW, LPCWSTR pszVersionW, LPCWSTR pszReferrerW, LPCWSTR *ppszAcceptTypesW, DWORD dwFlags) {
    (void)pszVerbW; (void)pszObjectNameW; (void)pszVersionW; (void)pszReferrerW; (void)ppszAcceptTypesW; (void)dwFlags;
    int i = lookup_handle((HANDLE)hConnect, WIN_HANDLE_INET_CONN);
    if (i < 0) return 0;
    uint32_t server_ip = (uint32_t)win_handles[i].data;
    uint16_t server_port = (uint16_t)win_handles[i].extra;
    SOCKET http_sock = win_socket(WIN_AF_INET, WIN_SOCK_STREAM, WIN_IPPROTO_TCP);
    if (http_sock == INVALID_SOCKET) return 0;
    win_sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = WIN_AF_INET;
    addr.sin_port = htons(server_port);
    addr.sin_addr.s_addr = server_ip;
    if (win_connect(http_sock, (win_sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        win_closesocket(http_sock); return 0;
    }
    HANDLE h = alloc_handle(WIN_HANDLE_INET_REQ, (uint64_t)i, (uint64_t)http_sock);
    return (HINTERNET)h;
}

BOOL WINAPI win_WinHttpSendRequest(HINTERNET hRequest, LPCWSTR lpszHeadersW, DWORD dwHeadersLength, LPVOID lpOptional, DWORD dwOptionalLength, DWORD dwTotalLength, ULONG_PTR dwContext) {
    (void)lpszHeadersW; (void)dwHeadersLength; (void)lpOptional; (void)dwOptionalLength; (void)dwTotalLength; (void)dwContext;
    int ri = lookup_handle((HANDLE)hRequest, WIN_HANDLE_INET_REQ);
    if (ri < 0) return FALSE;
    SOCKET sock = (SOCKET)win_handles[ri].extra;
    const char *req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    int ret = win_send(sock, req, strlen(req), 0);
    return (ret != SOCKET_ERROR);
}

BOOL WINAPI win_WinHttpReceiveResponse(HINTERNET hRequest, LPVOID lpReserved) {
    (void)hRequest; (void)lpReserved; return TRUE;
}

BOOL WINAPI win_WinHttpReadData(HINTERNET hRequest, LPVOID lpBuffer, DWORD dwNumberOfBytesToRead, LPDWORD lpdwNumberOfBytesRead) {
    return win_InternetReadFile(hRequest, lpBuffer, dwNumberOfBytesToRead, lpdwNumberOfBytesRead);
}

BOOL WINAPI win_WinHttpCloseHandle(HINTERNET hInternet) {
    return win_CloseHandle((HANDLE)hInternet);
}

BOOL WINAPI win_WinHttpSetOption(HINTERNET hInternet, DWORD dwOption, LPVOID lpBuffer, DWORD dwBufferLength) {
    (void)hInternet; (void)dwOption; (void)lpBuffer; (void)dwBufferLength; return TRUE;
}

BOOL WINAPI win_WinHttpQueryHeaders(HINTERNET hRequest, DWORD dwInfoLevel, LPCWSTR pszNameW, LPVOID lpBuffer, LPDWORD lpdwBufferLength, LPDWORD lpdwIndex) {
    (void)hRequest; (void)dwInfoLevel; (void)pszNameW; (void)lpdwIndex;
    if (lpBuffer && lpdwBufferLength && *lpdwBufferLength > 0) { ((char *)lpBuffer)[0] = '\0'; *lpdwBufferLength = 0; }
    return TRUE;
}

BOOL WINAPI win_WinHttpQueryDataAvailable(HINTERNET hRequest, LPDWORD lpdwNumberOfBytesAvailable) {
    (void)hRequest;
    if (lpdwNumberOfBytesAvailable) *lpdwNumberOfBytesAvailable = 4096;
    return TRUE;
}

/* ========== MSWSOCK 函数实现 ========== */

int WINAPI win_WSARecvMsg(SOCKET s, void *lpMsg, DWORD *lpdwNumberOfBytesRecvd, LPWSAOVERLAPPED lpOverlapped, void *lpCompletionRoutine) {
    (void)s; (void)lpMsg; (void)lpOverlapped; (void)lpCompletionRoutine;
    if (lpdwNumberOfBytesRecvd) *lpdwNumberOfBytesRecvd = 0;
    return SOCKET_ERROR;
}

BOOL WINAPI win_TransmitFile(SOCKET hSocket, HANDLE hFile, DWORD nNumberOfBytesToWrite, DWORD nNumberOfBytesPerSend, LPOVERLAPPED lpOverlapped, LPVOID lpTransmitBuffers, DWORD dwReserved) {
    (void)hSocket; (void)hFile; (void)nNumberOfBytesToWrite; (void)nNumberOfBytesPerSend; (void)lpOverlapped; (void)lpTransmitBuffers; (void)dwReserved;
    return FALSE;
}

BOOL WINAPI win_AcceptEx(SOCKET sListenSocket, SOCKET sAcceptSocket, PVOID lpOutputBuffer, DWORD dwReceiveDataLength, DWORD dwLocalAddressLength, DWORD dwRemoteAddressLength, LPDWORD lpdwBytesReceived, LPOVERLAPPED lpOverlapped) {
    (void)sListenSocket; (void)sAcceptSocket; (void)lpOutputBuffer; (void)dwReceiveDataLength; (void)dwLocalAddressLength; (void)dwRemoteAddressLength; (void)lpdwBytesReceived; (void)lpOverlapped;
    return FALSE;
}

void WINAPI win_GetAcceptExSockaddrs(PVOID lpOutputBuffer, DWORD dwReceiveDataLength, DWORD dwLocalAddressLength, DWORD dwRemoteAddressLength, win_sockaddr **LocalSockaddr, LPINT LocalSockaddrLength, win_sockaddr **RemoteSockaddr, LPINT RemoteSockaddrLength) {
    (void)lpOutputBuffer; (void)dwReceiveDataLength; (void)dwLocalAddressLength; (void)dwRemoteAddressLength;
    if (LocalSockaddr) *LocalSockaddr = NULL;
    if (LocalSockaddrLength) *LocalSockaddrLength = 0;
    if (RemoteSockaddr) *RemoteSockaddr = NULL;
    if (RemoteSockaddrLength) *RemoteSockaddrLength = 0;
}

/* ========== 导出函数表 ========== */

struct dll_export {
    const char *name;
    void *func;
};

static const struct dll_export kernel32_exports[] = {
    {"CreateFileA", win_CreateFileA},
    {"ReadFile", win_ReadFile},
    {"WriteFile", win_WriteFile},
    {"CloseHandle", win_CloseHandle},
    {"GetFileSize", win_GetFileSize},
    {"DeleteFileA", win_DeleteFileA},
    {"MoveFileA", win_MoveFileA},
    {"CopyFileA", win_CopyFileA},
    {"FindFirstFileA", win_FindFirstFileA},
    {"FindNextFileA", win_FindNextFileA},
    {"FindClose", win_FindClose},
    {"GetFileAttributesA", win_GetFileAttributesA},
    {"GetCurrentProcessId", win_GetCurrentProcessId},
    {"GetCurrentThreadId", win_GetCurrentThreadId},
    {"GetCurrentProcess", win_GetCurrentProcess},
    {"GetCurrentThread", win_GetCurrentThread},
    {"ExitProcess", win_ExitProcess},
    {"ExitThread", win_ExitThread},
    {"GetLastError", win_GetLastError},
    {"SetLastError", win_SetLastError},
    {"CreateThread", win_CreateThread},
    {"ResumeThread", win_ResumeThread},
    {"SuspendThread", win_SuspendThread},
    {"CreateProcessA", win_CreateProcessA},
    {"TerminateProcess", win_TerminateProcess},
    {"VirtualAlloc", win_VirtualAlloc},
    {"VirtualFree", win_VirtualFree},
    {"VirtualProtect", win_VirtualProtect},
    {"VirtualQuery", win_VirtualQuery},
    {"HeapAlloc", win_HeapAlloc},
    {"HeapFree", win_HeapFree},
    {"HeapCreate", win_HeapCreate},
    {"HeapDestroy", win_HeapDestroy},
    {"HeapReAlloc", win_HeapReAlloc},
    {"HeapSize", win_HeapSize},
    {"LoadLibraryA", win_LoadLibraryA},
    {"LoadLibraryW", win_LoadLibraryW},
    {"FreeLibrary", win_FreeLibrary},
    {"GetProcAddress", win_GetProcAddress},
    {"GetModuleHandleA", win_GetModuleHandleA},
    {"GetModuleHandleW", win_GetModuleHandleW},
    {"GetModuleFileNameA", win_GetModuleFileNameA},
    {"Sleep", win_Sleep},
    {"CreateEventA", win_CreateEventA},
    {"SetEvent", win_SetEvent},
    {"ResetEvent", win_ResetEvent},
    {"CreateMutexA", win_CreateMutexA},
    {"ReleaseMutex", win_ReleaseMutex},
    {"WaitForSingleObject", win_WaitForSingleObject},
    {"GetSystemInfo", win_GetSystemInfo},
    {"GetTickCount", win_GetTickCount},
    {"GetTickCount64", win_GetTickCount64},
    {"QueryPerformanceCounter", win_QueryPerformanceCounter},
    {"QueryPerformanceFrequency", win_QueryPerformanceFrequency},
    {"GetCurrentDirectoryA", win_GetCurrentDirectoryA},
    {"SetCurrentDirectoryA", win_SetCurrentDirectoryA},
    {"GetCommandLineA", win_GetCommandLineA},
    {"GetCommandLineW", win_GetCommandLineW},
    {"GetComputerNameA", win_GetComputerNameA},
    {"GetComputerNameW", win_GetComputerNameW},
    {"GetUserNameA", win_GetUserNameA},
    {"GetUserNameW", win_GetUserNameW},
    {"GetStartupInfoA", win_GetStartupInfoA},
    {"GetStartupInfoW", win_GetStartupInfoW},
    {"GetTempPathA", win_GetTempPathA},
    {"GetTempFileNameA", win_GetTempFileNameA},
    {"FileTimeToLocalFileTime", win_FileTimeToLocalFileTime},
    {"LocalFileTimeToFileTime", win_LocalFileTimeToFileTime},
    {"GetSystemTimeAsFileTime", win_GetSystemTimeAsFileTime},
    {"GetEnvironmentVariableA", win_GetEnvironmentVariableA},
    {"SetEnvironmentVariableA", win_SetEnvironmentVariableA},
    {"lstrcmpA", win_lstrcmpA},
    {"lstrcmpiA", win_lstrcmpiA},
    {"lstrcpyA", win_lstrcpyA},
    {"lstrlenA", win_lstrlenA},
    {"GlobalAlloc", win_GlobalAlloc},
    {"GlobalFree", win_GlobalFree},
    {"GlobalSize", win_GlobalSize},
    {"GlobalLock", win_GlobalLock},
    {"GlobalUnlock", win_GlobalUnlock},
    {"LocalAlloc", win_LocalAlloc},
    {"LocalFree", win_LocalFree},
    {"GetStdHandle", win_GetStdHandle},
    {"SetStdHandle", win_SetStdHandle},
    {"GetFileType", win_GetFileType},
    {"WriteConsoleA", win_WriteConsoleA},
    {"ReadConsoleA", win_ReadConsoleA},
    {"GetVersion", win_GetVersion},
    {"OutputDebugStringA", win_OutputDebugStringA},
    {"IsDebuggerPresent", win_IsDebuggerPresent},
    {"DebugBreak", win_DebugBreak},
    {"RaiseException", win_RaiseException},
    {"AddVectoredExceptionHandler", win_AddVectoredExceptionHandler},
    {"RemoveVectoredExceptionHandler", win_RemoveVectoredExceptionHandler},
    {"SetUnhandledExceptionFilter", win_SetUnhandledExceptionFilter},
    {"UnhandledExceptionFilter", win_UnhandledExceptionFilter},
    {"TlsAlloc", win_TlsAlloc},
    {"TlsFree", win_TlsFree},
    {"TlsGetValue", win_TlsGetValue},
    {"TlsSetValue", win_TlsSetValue},
    {NULL, NULL}
};

static const struct dll_export user32_exports[] = {
    {"CreateWindowExA", win_CreateWindowExA},
    {"CreateWindowExW", win_CreateWindowExW},
    {"DestroyWindow", win_DestroyWindow},
    {"ShowWindow", win_ShowWindow},
    {"UpdateWindow", win_UpdateWindow},
    {"MoveWindow", win_MoveWindow},
    {"GetWindowRect", win_GetWindowRect},
    {"GetClientRect", win_GetClientRect},
    {"SetWindowTextA", win_SetWindowTextA},
    {"GetWindowTextA", win_GetWindowTextA},
    {"GetWindowTextLengthA", win_GetWindowTextLengthA},
    {"SetWindowLongA", win_SetWindowLongA},
    {"SetWindowLongPtrA", win_SetWindowLongPtrA},
    {"GetWindowLongA", win_GetWindowLongA},
    {"GetWindowLongPtrA", win_GetWindowLongPtrA},
    {"GetParent", win_GetParent},
    {"SetParent", win_SetParent},
    {"SetForegroundWindow", win_SetForegroundWindow},
    {"GetForegroundWindow", win_GetForegroundWindow},
    {"EnableWindow", win_EnableWindow},
    {"IsWindowEnabled", win_IsWindowEnabled},
    {"IsWindowVisible", win_IsWindowVisible},
    {"IsWindow", win_IsWindow},
    {"GetDesktopWindow", win_GetDesktopWindow},
    {"GetFocus", win_GetFocus},
    {"SetFocus", win_SetFocus},
    {"GetActiveWindow", win_GetActiveWindow},
    {"RegisterClassA", win_RegisterClassA},
    {"RegisterClassExA", win_RegisterClassExA},
    {"UnregisterClassA", win_UnregisterClassA},
    {"GetMessageA", win_GetMessageA},
    {"PeekMessageA", win_PeekMessageA},
    {"TranslateMessage", win_TranslateMessage},
    {"DispatchMessageA", win_DispatchMessageA},
    {"SendMessageA", win_SendMessageA},
    {"PostMessageA", win_PostMessageA},
    {"PostQuitMessage", win_PostQuitMessage},
    {"DefWindowProcA", win_DefWindowProcA},
    {"CallWindowProcA", win_CallWindowProcA},
    {"MessageBoxA", win_MessageBoxA},
    {"GetDC", win_GetDC},
    {"GetWindowDC", win_GetWindowDC},
    {"ReleaseDC", win_ReleaseDC},
    {"BeginPaint", win_BeginPaint},
    {"EndPaint", win_EndPaint},
    {"InvalidateRect", win_InvalidateRect},
    {"ValidateRect", win_ValidateRect},
    {"SetTimer", win_SetTimer},
    {"KillTimer", win_KillTimer},
    {"GetCursorPos", win_GetCursorPos},
    {"SetCursorPos", win_SetCursorPos},
    {"WindowFromPoint", win_WindowFromPoint},
    {"SetCapture", win_SetCapture},
    {"ReleaseCapture", win_ReleaseCapture},
    {"GetCapture", win_GetCapture},
    {"GetKeyState", win_GetKeyState},
    {"GetAsyncKeyState", win_GetAsyncKeyState},
    {"GetKeyboardState", win_GetKeyboardState},
    {"LoadIconA", win_LoadIconA},
    {"LoadCursorA", win_LoadCursorA},
    {"SetCursor", win_SetCursor},
    {"CreateMenu", win_CreateMenu},
    {"CreatePopupMenu", win_CreatePopupMenu},
    {"DestroyMenu", win_DestroyMenu},
    {"AppendMenuA", win_AppendMenuA},
    {"GetMenu", win_GetMenu},
    {"SetMenu", win_SetMenu},
    {NULL, NULL}
};

static const struct dll_export gdi32_exports[] = {
    {"GetStockObject", win_GetStockObject},
    {"SelectObject", win_SelectObject},
    {"DeleteObject", win_DeleteObject},
    {"CreatePen", win_CreatePen},
    {"CreateSolidBrush", win_CreateSolidBrush},
    {"CreateHatchBrush", win_CreateHatchBrush},
    {"CreateFontA", win_CreateFontA},
    {"CreateFontIndirectA", win_CreateFontIndirectA},
    {"CreateBitmap", win_CreateBitmap},
    {"CreateCompatibleBitmap", win_CreateCompatibleBitmap},
    {"SetTextColor", win_SetTextColor},
    {"GetTextColor", win_GetTextColor},
    {"SetBkColor", win_SetBkColor},
    {"SetBkMode", win_SetBkMode},
    {"Rectangle", win_Rectangle},
    {"Ellipse", win_Ellipse},
    {"TextOutA", win_TextOutA},
    {"GetTextMetricsA", win_GetTextMetricsA},
    {"GetTextExtentPoint32A", win_GetTextExtentPoint32A},
    {"FillRect", win_FillRect},
    {"MoveToEx", win_MoveToEx},
    {"LineTo", win_LineTo},
    {"BitBlt", win_BitBlt},
    {"StretchBlt", win_StretchBlt},
    {"GetDC", win_GetDC},
    {"ReleaseDC", win_ReleaseDC},
    {"CreateCompatibleDC", win_CreateCompatibleDC},
    {"GetWindowDC", win_GetWindowDC},
    {"DeleteDC", win_DeleteDC},
    {"CreateRectRgn", win_CreateRectRgn},
    {"SelectClipRgn", win_SelectClipRgn},
    {"GetDeviceCaps", win_GetDeviceCaps},
    {NULL, NULL}
};

static const struct dll_export advapi32_exports[] = {
    {"RegOpenKeyA", win_RegOpenKeyA},
    {"RegOpenKeyExA", win_RegOpenKeyExA},
    {"RegCloseKey", win_RegCloseKey},
    {"RegCreateKeyA", win_RegCreateKeyA},
    {"RegCreateKeyExA", win_RegCreateKeyExA},
    {"RegDeleteKeyA", win_RegDeleteKeyA},
    {"RegDeleteValueA", win_RegDeleteValueA},
    {"RegEnumKeyA", win_RegEnumKeyA},
    {"RegEnumValueA", win_RegEnumValueA},
    {"RegSetValueExA", win_RegSetValueExA},
    {"RegQueryValueExA", win_RegQueryValueExA},
    {"OpenSCManagerA", win_OpenSCManagerA},
    {"OpenServiceA", win_OpenServiceA},
    {"CloseServiceHandle", win_CloseServiceHandle},
    {"DeleteService", win_DeleteService},
    {"CreateServiceA", win_CreateServiceA},
    {"StartServiceA", win_StartServiceA},
    {"ControlService", win_ControlService},
    {"QueryServiceStatus", win_QueryServiceStatus},
    {"InitializeSecurityDescriptor", win_InitializeSecurityDescriptor},
    {"IsValidSecurityDescriptor", win_IsValidSecurityDescriptor},
    {"IsValidSid", win_IsValidSid},
    {"IsValidAcl", win_IsValidAcl},
    {"AdjustTokenPrivileges", win_AdjustTokenPrivileges},
    {"LookupPrivilegeValueA", win_LookupPrivilegeValueA},
    {"CryptAcquireContextA", win_CryptAcquireContextA},
    {"CryptReleaseContext", win_CryptReleaseContext},
    {"CryptGenKey", win_CryptGenKey},
    {"CryptDestroyKey", win_CryptDestroyKey},
    {"CryptCreateHash", win_CryptCreateHash},
    {"CryptDestroyHash", win_CryptDestroyHash},
    {"CryptHashData", win_CryptHashData},
    {"CryptEncrypt", win_CryptEncrypt},
    {"CryptDecrypt", win_CryptDecrypt},
    {NULL, NULL}
};

static const struct dll_export ole32_exports[] = {
    {"CoInitialize", win_CoInitialize},
    {"CoUninitialize", win_CoUninitialize},
    {"CoInitializeEx", win_CoInitializeEx},
    {"CoCreateInstance", win_CoCreateInstance},
    {"CoGetClassObject", win_CoGetClassObject},
    {"CoTaskMemAlloc", win_CoTaskMemAlloc},
    {"CoTaskMemRealloc", win_CoTaskMemRealloc},
    {"CoTaskMemFree", win_CoTaskMemFree},
    {"CoRegisterClassObject", win_CoRegisterClassObject},
    {"CoRevokeClassObject", win_CoRevokeClassObject},
    {"SysAllocString", win_SysAllocString},
    {"SysAllocStringLen", win_SysAllocStringLen},
    {"SysFreeString", win_SysFreeString},
    {"SysStringLen", win_SysStringLen},
    {"StringFromCLSID", win_StringFromCLSID},
    {"CLSIDFromString", win_CLSIDFromString},
    {NULL, NULL}
};

static const struct dll_export shell32_exports[] = {
    {"ShellExecuteA", win_ShellExecuteA},
    {"SHGetFolderPathA", win_SHGetFolderPathA},
    {"SHGetFileInfoA", win_SHGetFileInfoA},
    {"Shell_NotifyIconA", win_Shell_NotifyIconA},
    {NULL, NULL}
};

static const struct dll_export comdlg32_exports[] = {
    {"GetOpenFileNameA", win_GetOpenFileNameA},
    {"GetSaveFileNameA", win_GetSaveFileNameA},
    {"CommDlgExtendedError", win_CommDlgExtendedError},
    {"ChooseColorA", win_ChooseColorA},
    {"ChooseFontA", win_ChooseFontA},
    {"PrintDlgA", win_PrintDlgA},
    {"PageSetupDlgA", win_PageSetupDlgA},
    {NULL, NULL}
};

static const struct dll_export ws2_32_exports[] = {
    {"WSAStartup", win_WSAStartup},
    {"WSACleanup", win_WSACleanup},
    {"WSAGetLastError", win_WSAGetLastError},
    {"WSASetLastError", win_WSASetLastError},
    {"socket", win_socket},
    {"closesocket", win_closesocket},
    {"connect", win_connect},
    {"send", win_send},
    {"recv", win_recv},
    {"sendto", win_sendto},
    {"recvfrom", win_recvfrom},
    {"bind", win_bind},
    {"listen", win_listen},
    {"accept", win_accept},
    {"select", win_select},
    {"shutdown", win_shutdown},
    {"getsockname", win_getsockname},
    {"getpeername", win_getpeername},
    {"getsockopt", win_getsockopt},
    {"setsockopt", win_setsockopt},
    {"ioctlsocket", win_ioctlsocket},
    {"WSASocketA", win_WSASocketA},
    {"WSASocketW", win_WSASocketW},
    {"WSASend", win_WSASend},
    {"WSARecv", win_WSARecv},
    {"WSAAsyncSelect", win_WSAAsyncSelect},
    {"WSAEventSelect", win_WSAEventSelect},
    {"WSAEnumNetworkEvents", win_WSAEnumNetworkEvents},
    {"WSAGetOverlappedResult", win_WSAGetOverlappedResult},
    {"getaddrinfo", win_getaddrinfo},
    {"freeaddrinfo", win_freeaddrinfo},
    {"gethostbyname", win_gethostbyname},
    {"gethostname", win_gethostname},
    {"getprotobyname", win_getprotobyname},
    {"getservbyname", win_getservbyname},
    {"ntohl", win_ntohl},
    {"ntohs", win_ntohs},
    {"htonl", win_htonl},
    {"htons", win_htons},
    {"inet_addr", win_inet_addr},
    {"inet_ntoa", win_inet_ntoa},
    {NULL, NULL}
};

static const struct dll_export iphlpapi_exports[] = {
    {"GetAdaptersInfo", win_GetAdaptersInfo},
    {"GetAdaptersAddresses", win_GetAdaptersAddresses},
    {"GetBestInterface", win_GetBestInterface},
    {"GetIpAddrTable", win_GetIpAddrTable},
    {"GetIfTable", win_GetIfTable},
    {"GetNumberOfInterfaces", win_GetNumberOfInterfaces},
    {"NotifyAddrChange", win_NotifyAddrChange},
    {"NotifyRouteChange", win_NotifyRouteChange},
    {NULL, NULL}
};

static const struct dll_export dnsapi_exports[] = {
    {"DnsQuery_A", win_DnsQuery_A},
    {"DnsQuery_W", win_DnsQuery_W},
    {"DnsQuery_UTF8", win_DnsQuery_A},
    {"DnsRecordListFree", win_DnsRecordListFree},
    {NULL, NULL}
};

static const struct dll_export wininet_exports[] = {
    {"InternetOpenA", win_InternetOpenA},
    {"InternetConnectA", win_InternetConnectA},
    {"InternetCloseHandle", win_InternetCloseHandle},
    {"HttpOpenRequestA", win_HttpOpenRequestA},
    {"HttpSendRequestA", win_HttpSendRequestA},
    {"InternetReadFile", win_InternetReadFile},
    {"InternetSetOptionA", win_InternetSetOptionA},
    {"InternetGetLastResponseInfoA", win_InternetGetLastResponseInfoA},
    {"HttpQueryInfoA", win_HttpQueryInfoA},
    {"HttpAddRequestHeadersA", win_HttpAddRequestHeadersA},
    {"InternetErrorDlg", win_InternetErrorDlg},
    {NULL, NULL}
};

static const struct dll_export winhttp_exports[] = {
    {"WinHttpOpen", win_WinHttpOpen},
    {"WinHttpConnect", win_WinHttpConnect},
    {"WinHttpOpenRequest", win_WinHttpOpenRequest},
    {"WinHttpSendRequest", win_WinHttpSendRequest},
    {"WinHttpReceiveResponse", win_WinHttpReceiveResponse},
    {"WinHttpReadData", win_WinHttpReadData},
    {"WinHttpCloseHandle", win_WinHttpCloseHandle},
    {"WinHttpSetOption", win_WinHttpSetOption},
    {"WinHttpQueryHeaders", win_WinHttpQueryHeaders},
    {"WinHttpQueryDataAvailable", win_WinHttpQueryDataAvailable},
    {NULL, NULL}
};

static const struct dll_export mswsock_exports[] = {
    {"WSARecvMsg", win_WSARecvMsg},
    {"TransmitFile", win_TransmitFile},
    {"AcceptEx", win_AcceptEx},
    {"GetAcceptExSockaddrs", win_GetAcceptExSockaddrs},
    {NULL, NULL}
};

/* ========== DLL 导出查找 ========== */

void *kernel32_get_proc(const char *name) {
    for (int i = 0; kernel32_exports[i].name; i++) {
        if (strcmp(kernel32_exports[i].name, name) == 0) {
            return kernel32_exports[i].func;
        }
    }
    return NULL;
}

void *user32_get_proc(const char *name) {
    for (int i = 0; user32_exports[i].name; i++) {
        if (strcmp(user32_exports[i].name, name) == 0) {
            return user32_exports[i].func;
        }
    }
    return NULL;
}

void *gdi32_get_proc(const char *name) {
    for (int i = 0; gdi32_exports[i].name; i++) {
        if (strcmp(gdi32_exports[i].name, name) == 0) {
            return gdi32_exports[i].func;
        }
    }
    return NULL;
}

void *advapi32_get_proc(const char *name) {
    for (int i = 0; advapi32_exports[i].name; i++) {
        if (strcmp(advapi32_exports[i].name, name) == 0) {
            return advapi32_exports[i].func;
        }
    }
    return NULL;
}

/* ========== NTDLL 类型定义 ========== */

/* Windows NT 字符串结构 */
typedef struct _ANSI_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    char *Buffer;
} ANSI_STRING, *PANSI_STRING;

typedef struct _UNICODE_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    uint16_t *Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

/* Windows NT 临界区 (简化) */
typedef struct _RTL_CRITICAL_SECTION {
    void *DebugInfo;
    volatile int LockCount;
    volatile int RecursionCount;
    void *OwningThread;
    void *LockSemaphore;
    uint64_t SpinCount;
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION;

/* Windows NT 对象属性 (简化) */
typedef struct _OBJECT_ATTRIBUTES {
    uint64_t Length;
    void *RootDirectory;
    UNICODE_STRING *ObjectName;
    uint32_t Attributes;
    void *SecurityDescriptor;
    void *SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

/* Windows NT IO 状态块 */
typedef struct _IO_STATUS_BLOCK {
    uint64_t Status;
    uint64_t Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

/* ========== NTDLL 函数实现 ========== */

/* ----- Rtl 内存/堆操作 ----- */

static void *win_RtlAllocateHeap(void *heap, uint32_t flags, size_t size) {
    (void)flags;
    return win_HeapAlloc((HANDLE)heap, 0, size);
}

static int win_RtlFreeHeap(void *heap, uint32_t flags, void *ptr) {
    (void)flags;
    return win_HeapFree((HANDLE)heap, 0, ptr);
}

static void *win_RtlReAllocateHeap(void *heap, uint32_t flags, void *ptr, size_t size) {
    (void)flags;
    if (!ptr) return win_HeapAlloc((HANDLE)heap, 0, size);
    void *new_ptr = win_HeapAlloc((HANDLE)heap, 0, size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, size); /* 简化: 复制 size 字节 */
    win_HeapFree((HANDLE)heap, 0, ptr);
    return new_ptr;
}

static size_t win_RtlSizeHeap(void *heap, uint32_t flags, void *ptr) {
    (void)heap; (void)flags; (void)ptr;
    return PAGE_SIZE; /* 简化: 返回页大小 */
}

static void win_RtlZeroMemory(void *ptr, size_t len) {
    memset(ptr, 0, len);
}

static void win_RtlFillMemory(void *ptr, size_t len, uint8_t value) {
    memset(ptr, value, len);
}

static void win_RtlMoveMemory(void *dst, const void *src, size_t len) {
    memmove(dst, src, len);
}

static size_t win_RtlCompareMemory(const void *ptr1, const void *ptr2, size_t len) {
    const uint8_t *p1 = (const uint8_t *)ptr1;
    const uint8_t *p2 = (const uint8_t *)ptr2;
    size_t i;
    for (i = 0; i < len; i++) {
        if (p1[i] != p2[i]) break;
    }
    return i;
}

static void win_RtlCopyMemory(void *dst, const void *src, size_t len) {
    memcpy(dst, src, len);
}

/* ----- Rtl 字符串操作 ----- */

static void win_RtlInitAnsiString(PANSI_STRING str, const char *source) {
    if (!str) return;
    if (source) {
        str->Length = (uint16_t)strlen(source);
        str->MaximumLength = str->Length + 1;
        str->Buffer = (char *)source;
    } else {
        str->Length = 0;
        str->MaximumLength = 0;
        str->Buffer = NULL;
    }
}

static void win_RtlInitUnicodeString(PUNICODE_STRING str, const uint16_t *source) {
    if (!str) return;
    if (source) {
        uint16_t len = 0;
        while (source[len]) len++;
        str->Length = len * 2;
        str->MaximumLength = str->Length + 2;
        str->Buffer = (uint16_t *)source;
    } else {
        str->Length = 0;
        str->MaximumLength = 0;
        str->Buffer = NULL;
    }
}

static uint64_t win_RtlAnsiStringToUnicodeString(PUNICODE_STRING dest, PANSI_STRING src, int alloc) {
    if (!dest || !src) return STATUS_INVALID_PARAMETER;
    uint16_t len = src->Length;
    dest->Length = len * 2;
    dest->MaximumLength = dest->Length + 2;
    if (alloc) {
        dest->Buffer = (uint16_t *)win_HeapAlloc((HANDLE)NULL, 0, dest->MaximumLength);
        if (!dest->Buffer) return STATUS_NO_MEMORY;
    }
    if (src->Buffer && dest->Buffer) {
        for (uint16_t i = 0; i < len; i++) {
            dest->Buffer[i] = (uint16_t)(uint8_t)src->Buffer[i];
        }
        dest->Buffer[len] = 0;
    }
    return STATUS_SUCCESS;
}

static uint64_t win_RtlUnicodeStringToAnsiString(PANSI_STRING dest, PUNICODE_STRING src, int alloc) {
    if (!dest || !src) return STATUS_INVALID_PARAMETER;
    uint16_t len = src->Length / 2;
    dest->Length = len;
    dest->MaximumLength = len + 1;
    if (alloc) {
        dest->Buffer = (char *)win_HeapAlloc((HANDLE)NULL, 0, dest->MaximumLength);
        if (!dest->Buffer) return STATUS_NO_MEMORY;
    }
    if (src->Buffer && dest->Buffer) {
        for (uint16_t i = 0; i < len; i++) {
            dest->Buffer[i] = (char)(uint8_t)src->Buffer[i];
        }
        dest->Buffer[len] = '\0';
    }
    return STATUS_SUCCESS;
}

static void win_RtlFreeAnsiString(PANSI_STRING str) {
    if (!str || !str->Buffer) return;
    win_HeapFree((HANDLE)NULL, 0, str->Buffer);
    str->Buffer = NULL;
    str->Length = 0;
    str->MaximumLength = 0;
}

static void win_RtlFreeUnicodeString(PUNICODE_STRING str) {
    if (!str || !str->Buffer) return;
    win_HeapFree((HANDLE)NULL, 0, str->Buffer);
    str->Buffer = NULL;
    str->Length = 0;
    str->MaximumLength = 0;
}

static int win_RtlCompareString(PANSI_STRING s1, PANSI_STRING s2, int case_insensitive) {
    if (!s1 || !s2) return 0;
    uint16_t len = s1->Length < s2->Length ? s1->Length : s2->Length;
    for (uint16_t i = 0; i < len; i++) {
        char c1 = s1->Buffer[i];
        char c2 = s2->Buffer[i];
        if (case_insensitive) {
            if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
            if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
        }
        if (c1 != c2) return (uint8_t)c1 - (uint8_t)c2;
    }
    return s1->Length - s2->Length;
}

static int win_RtlCompareUnicodeString(PUNICODE_STRING s1, PUNICODE_STRING s2, int case_insensitive) {
    if (!s1 || !s2) return 0;
    uint16_t len = s1->Length < s2->Length ? s1->Length : s2->Length;
    for (uint16_t i = 0; i < len / 2; i++) {
        uint16_t c1 = s1->Buffer[i];
        uint16_t c2 = s2->Buffer[i];
        if (case_insensitive) {
            if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
            if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
        }
        if (c1 != c2) return c1 - c2;
    }
    return s1->Length - s2->Length;
}

static char win_RtlUpperChar(char c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

static void win_RtlUpperString(PANSI_STRING dest, PANSI_STRING src) {
    if (!dest || !src) return;
    dest->Length = src->Length;
    dest->MaximumLength = src->MaximumLength;
    dest->Buffer = src->Buffer;
    if (dest->Buffer) {
        for (uint16_t i = 0; i < dest->Length; i++) {
            if (dest->Buffer[i] >= 'a' && dest->Buffer[i] <= 'z')
                dest->Buffer[i] -= 32;
        }
    }
}

static uint64_t win_RtlAppendStringToString(PANSI_STRING dest, PANSI_STRING src) {
    if (!dest || !src) return STATUS_INVALID_PARAMETER;
    if (dest->Length + src->Length > dest->MaximumLength)
        return STATUS_ERROR;
    if (src->Buffer && dest->Buffer) {
        memcpy(dest->Buffer + dest->Length, src->Buffer, src->Length);
        dest->Length += src->Length;
        dest->Buffer[dest->Length] = '\0';
    }
    return STATUS_SUCCESS;
}

static uint64_t win_RtlAppendUnicodeStringToString(PUNICODE_STRING dest, PUNICODE_STRING src) {
    if (!dest || !src) return STATUS_INVALID_PARAMETER;
    if (dest->Length + src->Length > dest->MaximumLength)
        return STATUS_ERROR;
    if (src->Buffer && dest->Buffer) {
        memcpy((uint8_t *)dest->Buffer + dest->Length, src->Buffer, src->Length);
        dest->Length += src->Length;
        dest->Buffer[dest->Length / 2] = 0;
    }
    return STATUS_SUCCESS;
}

static uint64_t win_RtlAppendUnicodeToString(PUNICODE_STRING dest, const uint16_t *src) {
    if (!dest || !src) return STATUS_INVALID_PARAMETER;
    uint16_t slen = 0;
    while (src[slen]) slen++;
    uint16_t src_bytes = slen * 2;
    if (dest->Length + src_bytes > dest->MaximumLength)
        return STATUS_ERROR;
    if (dest->Buffer) {
        memcpy((uint8_t *)dest->Buffer + dest->Length, src, src_bytes);
        dest->Length += src_bytes;
        dest->Buffer[dest->Length / 2] = 0;
    }
    return STATUS_SUCCESS;
}

/* ----- Nt 文件操作 ----- */

static uint64_t win_NtClose(HANDLE handle) {
    win_CloseHandle(handle);
    return STATUS_SUCCESS;
}

static uint64_t win_NtCreateFile(HANDLE *handle_out, uint32_t access, OBJECT_ATTRIBUTES *attrs,
                                  IO_STATUS_BLOCK *iosb, uint64_t *alloc_size,
                                  uint32_t file_attrs, uint32_t share,
                                  uint32_t disposition, uint32_t options,
                                  void *ea_buf, uint32_t ea_len) {
    (void)iosb; (void)alloc_size; (void)file_attrs; (void)options;
    (void)ea_buf; (void)ea_len;
    if (!handle_out || !attrs || !attrs->ObjectName) return STATUS_INVALID_PARAMETER;
    /* 将 UNICODE_STRING 转为 ANSI 路径 */
    char path[MAX_PATH] = {0};
    if (attrs->ObjectName->Buffer) {
        uint16_t len = attrs->ObjectName->Length / 2;
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        for (uint16_t i = 0; i < len; i++)
            path[i] = (char)attrs->ObjectName->Buffer[i];
        path[len] = '\0';
    }
    DWORD create_disp = 0;
    switch (disposition) {
        case 1: create_disp = 1; break; /* FILE_SUPERSEDE -> CREATE_ALWAYS */
        case 2: create_disp = 2; break; /* FILE_OPEN -> OPEN_EXISTING */
        case 3: create_disp = 3; break; /* FILE_CREATE -> CREATE_NEW */
        case 4: create_disp = 4; break; /* FILE_OPEN_IF -> OPEN_ALWAYS */
        case 5: create_disp = 5; break; /* FILE_OVERWRITE -> TRUNCATE_EXISTING */
        default: create_disp = 2; break;
    }
    HANDLE h = win_CreateFileA(path, access, share, NULL, create_disp, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return STATUS_ERROR;
    *handle_out = h;
    return STATUS_SUCCESS;
}

static uint64_t win_NtOpenFile(HANDLE *handle_out, uint32_t access, OBJECT_ATTRIBUTES *attrs,
                                IO_STATUS_BLOCK *iosb, uint32_t share, uint32_t options) {
    (void)iosb; (void)options;
    if (!handle_out || !attrs || !attrs->ObjectName) return STATUS_INVALID_PARAMETER;
    char path[MAX_PATH] = {0};
    if (attrs->ObjectName->Buffer) {
        uint16_t len = attrs->ObjectName->Length / 2;
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        for (uint16_t i = 0; i < len; i++)
            path[i] = (char)attrs->ObjectName->Buffer[i];
        path[len] = '\0';
    }
    HANDLE h = win_CreateFileA(path, access, share, NULL, 3, 0, 0); /* OPEN_EXISTING */
    if (h == INVALID_HANDLE_VALUE) return STATUS_ERROR;
    *handle_out = h;
    return STATUS_SUCCESS;
}

static uint64_t win_NtReadFile(HANDLE handle, void *event, void *apc, void *apc_ctx,
                                IO_STATUS_BLOCK *iosb, void *buf, uint32_t len,
                                uint64_t *offset, void *key) {
    (void)event; (void)apc; (void)apc_ctx; (void)offset; (void)key;
    DWORD read_bytes = 0;
    BOOL ret = win_ReadFile(handle, buf, len, &read_bytes, NULL);
    if (iosb) {
        iosb->Status = ret ? STATUS_SUCCESS : STATUS_ERROR;
        iosb->Information = read_bytes;
    }
    return ret ? STATUS_SUCCESS : STATUS_ERROR;
}

static uint64_t win_NtWriteFile(HANDLE handle, void *event, void *apc, void *apc_ctx,
                                 IO_STATUS_BLOCK *iosb, const void *buf, uint32_t len,
                                 uint64_t *offset, void *key) {
    (void)event; (void)apc; (void)apc_ctx; (void)offset; (void)key;
    DWORD written = 0;
    BOOL ret = win_WriteFile(handle, buf, len, &written, NULL);
    if (iosb) {
        iosb->Status = ret ? STATUS_SUCCESS : STATUS_ERROR;
        iosb->Information = written;
    }
    return ret ? STATUS_SUCCESS : STATUS_ERROR;
}

static uint64_t win_NtDeleteFile(OBJECT_ATTRIBUTES *attrs) {
    if (!attrs || !attrs->ObjectName) return STATUS_INVALID_PARAMETER;
    char path[MAX_PATH] = {0};
    if (attrs->ObjectName->Buffer) {
        uint16_t len = attrs->ObjectName->Length / 2;
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        for (uint16_t i = 0; i < len; i++)
            path[i] = (char)attrs->ObjectName->Buffer[i];
        path[len] = '\0';
    }
    return win_DeleteFileA(path) ? STATUS_SUCCESS : STATUS_ERROR;
}

static uint64_t win_NtQueryInformationFile(HANDLE handle, IO_STATUS_BLOCK *iosb,
                                            void *info, uint32_t len, uint32_t class_) {
    (void)handle; (void)iosb; (void)info; (void)len; (void)class_;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtSetInformationFile(HANDLE handle, IO_STATUS_BLOCK *iosb,
                                          void *info, uint32_t len, uint32_t class_) {
    (void)handle; (void)iosb; (void)info; (void)len; (void)class_;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtQueryVolumeInformationFile(HANDLE handle, IO_STATUS_BLOCK *iosb,
                                                   void *info, uint32_t len, uint32_t class_) {
    (void)handle; (void)iosb; (void)info; (void)len; (void)class_;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtDeviceIoControlFile(HANDLE handle, void *event, void *apc, void *apc_ctx,
                                            IO_STATUS_BLOCK *iosb, uint32_t code,
                                            void *in_buf, uint32_t in_len,
                                            void *out_buf, uint32_t out_len) {
    (void)handle; (void)event; (void)apc; (void)apc_ctx; (void)iosb;
    (void)code; (void)in_buf; (void)in_len; (void)out_buf; (void)out_len;
    return STATUS_NOT_IMPLEMENTED;
}

/* ----- Nt 内存操作 ----- */

static uint64_t win_NtAllocateVirtualMemory(void *proc, void **base, uint32_t zero_bits,
                                             size_t *size, uint32_t alloc_type, uint32_t protect) {
    (void)proc; (void)zero_bits;
    if (!base || !size) return STATUS_INVALID_PARAMETER;
    void *ptr = win_VirtualAlloc(*base, *size, alloc_type, protect);
    if (!ptr) return STATUS_NO_MEMORY;
    *base = ptr;
    *size = (*size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    return STATUS_SUCCESS;
}

static uint64_t win_NtFreeVirtualMemory(void *proc, void **base, size_t *size, uint32_t free_type) {
    (void)proc;
    if (!base || !size) return STATUS_INVALID_PARAMETER;
    BOOL ret = win_VirtualFree(*base, *size, free_type);
    return ret ? STATUS_SUCCESS : STATUS_ERROR;
}

static uint64_t win_NtProtectVirtualMemory(void *proc, void **base, size_t *size,
                                            uint32_t new_prot, uint32_t *old_prot) {
    (void)proc;
    if (!base || !size) return STATUS_INVALID_PARAMETER;
    BOOL ret = win_VirtualProtect(*base, *size, new_prot, old_prot);
    return ret ? STATUS_SUCCESS : STATUS_ERROR;
}

static uint64_t win_NtQueryVirtualMemory(void *proc, void *base, uint32_t info_class,
                                          void *info, size_t len, size_t *return_len) {
    (void)proc; (void)info_class; (void)return_len;
    SIZE_T ret = win_VirtualQuery(base, info, len);
    return ret ? STATUS_SUCCESS : STATUS_ERROR;
}

static uint64_t win_NtCreateSection(void *handle_out, uint32_t access, OBJECT_ATTRIBUTES *attrs,
                                     uint64_t *max_size, uint32_t page_prot,
                                     uint32_t alloc_prot, uint32_t disposition,
                                     HANDLE file) {
    (void)handle_out; (void)access; (void)attrs; (void)max_size;
    (void)page_prot; (void)alloc_prot; (void)disposition; (void)file;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtMapViewOfSection(HANDLE section, void *proc, void **base,
                                        uint32_t zero_bits, size_t commit_size,
                                        uint64_t *offset, size_t *view_size,
                                        uint32_t inherit, uint32_t alloc_type, uint32_t protect) {
    (void)section; (void)proc; (void)base; (void)zero_bits; (void)commit_size;
    (void)offset; (void)view_size; (void)inherit; (void)alloc_type; (void)protect;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtUnmapViewOfSection(void *proc, void *base) {
    (void)proc; (void)base;
    return STATUS_NOT_IMPLEMENTED;
}

/* ----- Nt 进程/线程操作 ----- */

static uint64_t win_NtTerminateProcess(HANDLE handle, uint64_t exit_status) {
    (void)handle;
    win_ExitProcess((UINT)exit_status);
    return STATUS_SUCCESS;
}

static uint64_t win_NtTerminateThread(HANDLE handle, uint64_t exit_status) {
    (void)handle;
    win_ExitThread((DWORD)exit_status);
    return STATUS_SUCCESS;
}

static uint64_t win_NtCreateProcess(void *handle_out, uint32_t access, OBJECT_ATTRIBUTES *attrs,
                                     HANDLE parent, int inherit, uint32_t section,
                                     uint32_t debug, uint32_t exception) {
    (void)handle_out; (void)access; (void)attrs; (void)parent;
    (void)inherit; (void)section; (void)debug; (void)exception;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtCreateThread(void *handle_out, uint32_t access, OBJECT_ATTRIBUTES *attrs,
                                    HANDLE proc, void *start, void *param, uint32_t flags,
                                    size_t stack, size_t stack_reserve) {
    (void)handle_out; (void)access; (void)attrs; (void)proc;
    (void)start; (void)param; (void)flags; (void)stack; (void)stack_reserve;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtSuspendThread(HANDLE handle, uint32_t *prev_count) {
    DWORD ret = win_SuspendThread(handle);
    if (prev_count) *prev_count = ret;
    return STATUS_SUCCESS;
}

static uint64_t win_NtResumeThread(HANDLE handle, uint32_t *prev_count) {
    DWORD ret = win_ResumeThread(handle);
    if (prev_count) *prev_count = ret;
    return STATUS_SUCCESS;
}

static uint64_t win_NtGetContextThread(HANDLE handle, void *context) {
    (void)handle; (void)context;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtSetContextThread(HANDLE handle, void *context) {
    (void)handle; (void)context;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtQueryInformationProcess(HANDLE handle, uint32_t class_,
                                               void *info, uint32_t len, uint32_t *return_len) {
    (void)handle; (void)class_; (void)info; (void)len; (void)return_len;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtSetInformationProcess(HANDLE handle, uint32_t class_,
                                             void *info, uint32_t len) {
    (void)handle; (void)class_; (void)info; (void)len;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtQueryInformationThread(HANDLE handle, uint32_t class_,
                                              void *info, uint32_t len, uint32_t *return_len) {
    (void)handle; (void)class_; (void)info; (void)len; (void)return_len;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtSetInformationThread(HANDLE handle, uint32_t class_,
                                            void *info, uint32_t len) {
    (void)handle; (void)class_; (void)info; (void)len;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtGetCurrentProcessorNumber(void) {
    return 0;
}

/* ----- Nt 同步操作 ----- */

static uint64_t win_NtCreateEvent(HANDLE *handle_out, uint32_t access, OBJECT_ATTRIBUTES *attrs,
                                   uint32_t type, int state) {
    (void)access;
    const char *name = NULL;
    if (attrs && attrs->ObjectName && attrs->ObjectName->Buffer) {
        /* 简化: 忽略 unicode 名称 */
        name = "";
    }
    HANDLE h = win_CreateEventA(NULL, type, state, name);
    if (h == INVALID_HANDLE_VALUE) return STATUS_ERROR;
    if (handle_out) *handle_out = h;
    return STATUS_SUCCESS;
}

static uint64_t win_NtOpenEvent(HANDLE *handle_out, uint32_t access, OBJECT_ATTRIBUTES *attrs) {
    (void)handle_out; (void)access; (void)attrs;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtSetEvent(HANDLE handle, uint32_t *prev_state) {
    (void)prev_state;
    win_SetEvent(handle);
    return STATUS_SUCCESS;
}

static uint64_t win_NtResetEvent(HANDLE handle, uint32_t *prev_state) {
    (void)prev_state;
    win_ResetEvent(handle);
    return STATUS_SUCCESS;
}

static uint64_t win_NtClearEvent(HANDLE handle) {
    win_ResetEvent(handle);
    return STATUS_SUCCESS;
}

static uint64_t win_NtCreateMutant(HANDLE *handle_out, uint32_t access, OBJECT_ATTRIBUTES *attrs,
                                    int initial) {
    (void)access;
    const char *name = NULL;
    if (attrs && attrs->ObjectName && attrs->ObjectName->Buffer) {
        name = "";
    }
    HANDLE h = win_CreateMutexA(NULL, initial, name);
    if (h == INVALID_HANDLE_VALUE) return STATUS_ERROR;
    if (handle_out) *handle_out = h;
    return STATUS_SUCCESS;
}

static uint64_t win_NtReleaseMutant(HANDLE handle, uint32_t *prev_count) {
    (void)prev_count;
    win_ReleaseMutex(handle);
    return STATUS_SUCCESS;
}

static uint64_t win_NtCreateSemaphore(HANDLE *handle_out, uint32_t access, OBJECT_ATTRIBUTES *attrs,
                                       uint32_t initial, uint32_t max) {
    (void)access; (void)attrs; (void)max;
    /* 简化: 使用事件句柄模拟信号量 */
    if (!handle_out) return STATUS_INVALID_PARAMETER;
    HANDLE h = win_CreateEventA(NULL, 1, initial > 0 ? 1 : 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return STATUS_ERROR;
    *handle_out = h;
    return STATUS_SUCCESS;
}

static uint64_t win_NtReleaseSemaphore(HANDLE handle, uint32_t count, uint32_t *prev_count) {
    (void)prev_count;
    /* 简化: 释放 count 次 */
    for (uint32_t i = 0; i < count; i++) {
        win_SetEvent(handle);
    }
    return STATUS_SUCCESS;
}

static uint64_t win_NtCreateTimer(HANDLE *handle_out, uint32_t access, OBJECT_ATTRIBUTES *attrs,
                                   uint32_t type) {
    (void)handle_out; (void)access; (void)attrs; (void)type;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtSetTimer(HANDLE handle, uint64_t *due_time, void *apc, void *apc_ctx,
                                int resume, int period, uint32_t *prev_state) {
    (void)handle; (void)due_time; (void)apc; (void)apc_ctx;
    (void)resume; (void)period; (void)prev_state;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtCancelTimer(HANDLE handle, int *prev_state) {
    (void)handle; (void)prev_state;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtWaitForSingleObject(HANDLE handle, int alertable, uint64_t *timeout) {
    (void)alertable;
    DWORD ms = INFINITE;
    if (timeout) {
        /* Windows 使用 100ns 单位，负值表示相对时间 */
        int64_t t = *(int64_t *)timeout;
        if (t < 0) {
            ms = (DWORD)(-t / 10000); /* 转换为毫秒 */
        } else {
            ms = 0;
        }
    }
    DWORD ret = win_WaitForSingleObject(handle, ms);
    if (ret == 0) return STATUS_SUCCESS; /* WAIT_OBJECT_0 */
    if (ret == 258) return STATUS_TIMEOUT; /* WAIT_TIMEOUT */
    return STATUS_ERROR;
}

static uint64_t win_NtWaitForMultipleObjects(uint32_t count, HANDLE *handles, uint32_t wait_type,
                                               int alertable, uint64_t *timeout) {
    (void)wait_type; (void)alertable;
    /* 简化: 逐个等待 */
    DWORD ms = INFINITE;
    if (timeout) {
        int64_t t = *(int64_t *)timeout;
        if (t < 0) {
            ms = (DWORD)(-t / 10000);
        } else {
            ms = 0;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        DWORD ret = win_WaitForSingleObject(handles[i], ms);
        if (ret == 0) return (uint64_t)i; /* 返回触发对象的索引 */
    }
    return STATUS_TIMEOUT;
}

static uint64_t win_NtDelayExecution(int alertable, uint64_t *delay) {
    (void)alertable;
    if (!delay) {
        win_Sleep(0);
        return STATUS_SUCCESS;
    }
    int64_t t = *(int64_t *)delay;
    if (t < 0) {
        DWORD ms = (DWORD)(-t / 10000);
        win_Sleep(ms);
    } else {
        win_Sleep(0);
    }
    return STATUS_SUCCESS;
}

static uint64_t win_NtSleep(uint64_t delay) {
    win_Sleep((DWORD)delay);
    return STATUS_SUCCESS;
}

/* ----- Nt 系统信息 ----- */

static uint64_t win_NtQuerySystemInformation(uint32_t class_, void *info, uint32_t len, uint32_t *return_len) {
    (void)class_; (void)info; (void)len; (void)return_len;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtSetSystemInformation(uint32_t class_, void *info, uint32_t len) {
    (void)class_; (void)info; (void)len;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtQueryObject(HANDLE handle, uint32_t class_, void *info, uint32_t len, uint32_t *return_len) {
    (void)handle; (void)class_; (void)info; (void)len; (void)return_len;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_NtQueryPerformanceCounter(uint64_t *counter, uint64_t *freq) {
    if (counter) {
        LARGE_INTEGER li;
        win_QueryPerformanceCounter(&li);
        *counter = (uint64_t)li.QuadPart;
    }
    if (freq) {
        LARGE_INTEGER li;
        win_QueryPerformanceFrequency(&li);
        *freq = (uint64_t)li.QuadPart;
    }
    return STATUS_SUCCESS;
}

static uint64_t win_NtQueryTimerResolution(uint32_t *min, uint32_t *max, uint32_t *current) {
    if (min) *min = 1000;     /* 1ms = 1000 * 100ns */
    if (max) *max = 156250;   /* ~15.6ms */
    if (current) *current = 1000; /* 1ms */
    return STATUS_SUCCESS;
}

/* ----- Rtl 其他 ----- */

static void win_RtlEnterCriticalSection(PRTL_CRITICAL_SECTION cs) {
    if (!cs) return;
    while (!__sync_bool_compare_and_swap(&cs->LockCount, 0, 1)) {
        /* 自旋等待 */
    }
    cs->OwningThread = (void *)(uint64_t)1; /* 简化 */
    cs->RecursionCount = 1;
}

static void win_RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION cs) {
    if (!cs) return;
    cs->RecursionCount = 0;
    cs->OwningThread = NULL;
    cs->LockCount = 0;
    __sync_synchronize();
}

static int win_RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION cs) {
    if (!cs) return 0;
    if (__sync_bool_compare_and_swap(&cs->LockCount, 0, 1)) {
        cs->OwningThread = (void *)(uint64_t)1;
        cs->RecursionCount = 1;
        return 1;
    }
    return 0;
}

static void win_RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION cs) {
    if (!cs) return;
    memset(cs, 0, sizeof(RTL_CRITICAL_SECTION));
}

static void win_RtlDeleteCriticalSection(PRTL_CRITICAL_SECTION cs) {
    if (!cs) return;
    memset(cs, 0, sizeof(RTL_CRITICAL_SECTION));
}

static void win_RtlSetLastWin32Error(DWORD err) {
    win_SetLastError(err);
}

static DWORD win_RtlGetLastWin32Error(void) {
    return win_GetLastError();
}

static void win_RtlRaiseException(void *record) {
    win_RaiseException(0, 0, 0, NULL);
    (void)record;
}

static void win_RtlUnwind(void *target_frame, void *target_ip, void *record, uint64_t return_value) {
    (void)target_frame; (void)target_ip; (void)record; (void)return_value;
    /* 简化: 直接返回 */
}

static void *win_RtlAddVectoredExceptionHandler(uint32_t first, void *handler) {
    return win_AddVectoredExceptionHandler(first, handler);
}

static uint32_t win_RtlRemoveVectoredExceptionHandler(void *handle) {
    return win_RemoveVectoredExceptionHandler(handle);
}

static uint64_t win_RtlGetVersion(void *version_info) {
    if (!version_info) return STATUS_INVALID_PARAMETER;
    /* 填充 Windows 10 版本信息 */
    uint32_t *vi = (uint32_t *)version_info;
    vi[0] = sizeof(uint32_t) * 6; /* dwOSVersionInfoSize */
    vi[1] = 10;   /* dwMajorVersion */
    vi[2] = 0;    /* dwMinorVersion */
    vi[3] = 19041; /* dwBuildNumber */
    vi[4] = 2;    /* dwPlatformId (VER_PLATFORM_WIN32_NT) */
    return STATUS_SUCCESS;
}

static void *win_RtlLookupFunctionEntry(uint64_t control_pc, uint64_t *image_base, void *history_table) {
    (void)history_table;
    if (image_base) *image_base = 0;
    /* 简化: 无法查找, 返回 NULL */
    (void)control_pc;
    return NULL;
}

static uint64_t win_RtlVirtualUnwind(uint32_t frame_type, uint64_t image_base,
                                      uint64_t control_pc, void *entry, void *context,
                                      void **handler_data, uint64_t *establisher_frame,
                                      void *pointers) {
    (void)frame_type; (void)image_base; (void)control_pc; (void)entry;
    (void)context; (void)handler_data; (void)establisher_frame; (void)pointers;
    return STATUS_NOT_IMPLEMENTED;
}

static uint64_t win_RtlRestoreContext(void *context, void *record) {
    (void)context; (void)record;
    return STATUS_NOT_IMPLEMENTED;
}

/* SID 简化结构 - PSID 已在 ntdll.h 中定义为 void*，这里只定义 SID 结构体 */
typedef struct _SID {
    uint8_t Revision;
    uint8_t SubAuthorityCount;
    uint8_t IdentifierAuthority[6];
    uint32_t SubAuthority[1]; /* 变长 */
} SID;

static uint64_t win_RtlAllocateAndInitializeSid(uint8_t *auth, uint8_t sub_count,
                                                 uint32_t sub0, uint32_t sub1, uint32_t sub2,
                                                 uint32_t sub3, uint32_t sub4, uint32_t sub5,
                                                 uint32_t sub6, uint32_t sub7, void **sid) {
    if (!sid) return STATUS_INVALID_PARAMETER;
    size_t size = 8 + sub_count * 4;
    SID *s = (SID *)win_HeapAlloc((HANDLE)NULL, 0, size);
    if (!s) return STATUS_NO_MEMORY;
    s->Revision = 1;
    s->SubAuthorityCount = sub_count;
    if (auth) memcpy(s->IdentifierAuthority, auth, 6);
    uint32_t subs[8] = {sub0, sub1, sub2, sub3, sub4, sub5, sub6, sub7};
    for (uint8_t i = 0; i < sub_count && i < 8; i++)
        s->SubAuthority[i] = subs[i];
    *sid = s;
    return STATUS_SUCCESS;
}

static uint64_t win_RtlFreeSid(void *sid) {
    if (sid) win_HeapFree((HANDLE)NULL, 0, sid);
    return STATUS_SUCCESS;
}

static uint32_t win_RtlLengthSid(void *sid) {
    if (!sid) return 0;
    SID *s = (SID *)sid;
    return 8 + s->SubAuthorityCount * 4;
}

static int win_RtlEqualSid(void *s1, void *s2) {
    if (!s1 || !s2) return 0;
    uint32_t len1 = win_RtlLengthSid(s1);
    uint32_t len2 = win_RtlLengthSid(s2);
    if (len1 != len2) return 0;
    return memcmp(s1, s2, len1) == 0;
}

static void win_RtlInitializeSid(void *sid, uint8_t *auth, uint8_t sub_count) {
    if (!sid) return;
    SID *s = (SID *)sid;
    s->Revision = 1;
    s->SubAuthorityCount = sub_count;
    if (auth) memcpy(s->IdentifierAuthority, auth, 6);
}

static uint32_t *win_RtlSubAuthoritySid(void *sid, uint32_t sub) {
    if (!sid) return NULL;
    SID *s = (SID *)sid;
    if (sub >= s->SubAuthorityCount) return NULL;
    return &s->SubAuthority[sub];
}

static uint8_t *win_RtlSubAuthorityCountSid(void *sid) {
    if (!sid) return NULL;
    return &((SID *)sid)->SubAuthorityCount;
}

static uint64_t win_RtlCreateUserStack(void *proc, size_t commit, size_t reserve,
                                        uint32_t zero_bits, void **stack_base, void **stack_limit) {
    (void)proc; (void)zero_bits;
    if (!stack_base || !stack_limit) return STATUS_INVALID_PARAMETER;
    size_t size = reserve ? reserve : 0x100000; /* 默认 1MB */
    void *ptr = win_VirtualAlloc(NULL, size, 0x3000, 0x04); /* MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE */
    if (!ptr) return STATUS_NO_MEMORY;
    *stack_base = (uint8_t *)ptr + size;
    *stack_limit = ptr;
    return STATUS_SUCCESS;
}

static void win_RtlFreeUserStack(void *proc, void *stack_base) {
    (void)proc;
    if (stack_base) win_VirtualFree(stack_base, 0, 0x8000); /* MEM_RELEASE */
}

static void *win_RtlPcToFileHeader(uint64_t pc, uint64_t *base) {
    if (!base) return NULL;
    *base = 0;
    (void)pc;
    /* 简化: 返回 NULL */
    return NULL;
}

/* ----- LDR 操作 ----- */

static uint64_t win_LdrLoadDll(UNICODE_STRING *path, uint32_t *flags, UNICODE_STRING *name, HANDLE *handle) {
    (void)path; (void)flags;
    if (!name || !handle) return STATUS_INVALID_PARAMETER;
    /* 将 UNICODE_STRING 转为 ANSI */
    char ansi_name[MAX_PATH] = {0};
    if (name->Buffer) {
        uint16_t len = name->Length / 2;
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        for (uint16_t i = 0; i < len; i++)
            ansi_name[i] = (char)name->Buffer[i];
        ansi_name[len] = '\0';
    }
    HMODULE h = win_LoadLibraryA(ansi_name);
    if (!h) return STATUS_OBJECT_NAME_NOT_FOUND;
    *handle = (HANDLE)h;
    return STATUS_SUCCESS;
}

static uint64_t win_LdrGetDllHandle(UNICODE_STRING *path, uint32_t *flags, UNICODE_STRING *name, HANDLE *handle) {
    (void)path; (void)flags;
    if (!name || !handle) return STATUS_INVALID_PARAMETER;
    char ansi_name[MAX_PATH] = {0};
    if (name->Buffer) {
        uint16_t len = name->Length / 2;
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        for (uint16_t i = 0; i < len; i++)
            ansi_name[i] = (char)name->Buffer[i];
        ansi_name[len] = '\0';
    }
    HMODULE h = win_GetModuleHandleA(ansi_name);
    if (!h) return STATUS_OBJECT_NAME_NOT_FOUND;
    *handle = (HANDLE)h;
    return STATUS_SUCCESS;
}

static uint64_t win_LdrGetProcedureAddress(HANDLE handle, ANSI_STRING *name, uint32_t ordinal, void **func) {
    if (!handle || !func) return STATUS_INVALID_PARAMETER;
    const char *proc_name = NULL;
    if (name && name->Buffer) proc_name = name->Buffer;
    FARPROC p = win_GetProcAddress((HMODULE)handle, proc_name);
    if (!p) return STATUS_OBJECT_NAME_NOT_FOUND;
    *func = (void *)p;
    return STATUS_SUCCESS;
}

static uint64_t win_LdrGetDllHandleByName(UNICODE_STRING *name, HANDLE *handle) {
    if (!name || !handle) return STATUS_INVALID_PARAMETER;
    char ansi_name[MAX_PATH] = {0};
    if (name->Buffer) {
        uint16_t len = name->Length / 2;
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        for (uint16_t i = 0; i < len; i++)
            ansi_name[i] = (char)name->Buffer[i];
        ansi_name[len] = '\0';
    }
    HMODULE h = win_GetModuleHandleA(ansi_name);
    if (!h) return STATUS_OBJECT_NAME_NOT_FOUND;
    *handle = (HANDLE)h;
    return STATUS_SUCCESS;
}

void *ntdll_get_proc(const char *name) {
    /* ntdll.dll 核心导出函数表 */
    static const struct { const char *name; void *func; } ntdll_exports[] = {
        /* Rtl 内存/堆操作 */
        { "RtlAllocateHeap", (void *)win_RtlAllocateHeap },
        { "RtlFreeHeap", (void *)win_RtlFreeHeap },
        { "RtlReAllocateHeap", (void *)win_RtlReAllocateHeap },
        { "RtlSizeHeap", (void *)win_RtlSizeHeap },
        { "RtlZeroMemory", (void *)win_RtlZeroMemory },
        { "RtlFillMemory", (void *)win_RtlFillMemory },
        { "RtlMoveMemory", (void *)win_RtlMoveMemory },
        { "RtlCompareMemory", (void *)win_RtlCompareMemory },
        { "RtlCopyMemory", (void *)win_RtlCopyMemory },

        /* Rtl 字符串操作 */
        { "RtlInitAnsiString", (void *)win_RtlInitAnsiString },
        { "RtlInitUnicodeString", (void *)win_RtlInitUnicodeString },
        { "RtlAnsiStringToUnicodeString", (void *)win_RtlAnsiStringToUnicodeString },
        { "RtlUnicodeStringToAnsiString", (void *)win_RtlUnicodeStringToAnsiString },
        { "RtlFreeAnsiString", (void *)win_RtlFreeAnsiString },
        { "RtlFreeUnicodeString", (void *)win_RtlFreeUnicodeString },
        { "RtlCompareString", (void *)win_RtlCompareString },
        { "RtlCompareUnicodeString", (void *)win_RtlCompareUnicodeString },
        { "RtlUpperChar", (void *)win_RtlUpperChar },
        { "RtlUpperString", (void *)win_RtlUpperString },
        { "RtlAppendStringToString", (void *)win_RtlAppendStringToString },
        { "RtlAppendUnicodeStringToString", (void *)win_RtlAppendUnicodeStringToString },
        { "RtlAppendUnicodeToString", (void *)win_RtlAppendUnicodeToString },

        /* Nt 文件操作 */
        { "NtClose", (void *)win_NtClose },
        { "NtCreateFile", (void *)win_NtCreateFile },
        { "NtOpenFile", (void *)win_NtOpenFile },
        { "NtReadFile", (void *)win_NtReadFile },
        { "NtWriteFile", (void *)win_NtWriteFile },
        { "NtDeleteFile", (void *)win_NtDeleteFile },
        { "NtQueryInformationFile", (void *)win_NtQueryInformationFile },
        { "NtSetInformationFile", (void *)win_NtSetInformationFile },
        { "NtQueryVolumeInformationFile", (void *)win_NtQueryVolumeInformationFile },
        { "NtDeviceIoControlFile", (void *)win_NtDeviceIoControlFile },

        /* Nt 内存操作 */
        { "NtAllocateVirtualMemory", (void *)win_NtAllocateVirtualMemory },
        { "NtFreeVirtualMemory", (void *)win_NtFreeVirtualMemory },
        { "NtProtectVirtualMemory", (void *)win_NtProtectVirtualMemory },
        { "NtQueryVirtualMemory", (void *)win_NtQueryVirtualMemory },
        { "NtCreateSection", (void *)win_NtCreateSection },
        { "NtMapViewOfSection", (void *)win_NtMapViewOfSection },
        { "NtUnmapViewOfSection", (void *)win_NtUnmapViewOfSection },

        /* Nt 进程/线程操作 */
        { "NtCreateProcess", (void *)win_NtCreateProcess },
        { "NtCreateThread", (void *)win_NtCreateThread },
        { "NtTerminateProcess", (void *)win_NtTerminateProcess },
        { "NtTerminateThread", (void *)win_NtTerminateThread },
        { "NtSuspendThread", (void *)win_NtSuspendThread },
        { "NtResumeThread", (void *)win_NtResumeThread },
        { "NtGetContextThread", (void *)win_NtGetContextThread },
        { "NtSetContextThread", (void *)win_NtSetContextThread },
        { "NtQueryInformationProcess", (void *)win_NtQueryInformationProcess },
        { "NtSetInformationProcess", (void *)win_NtSetInformationProcess },
        { "NtQueryInformationThread", (void *)win_NtQueryInformationThread },
        { "NtSetInformationThread", (void *)win_NtSetInformationThread },
        { "NtGetCurrentProcessorNumber", (void *)win_NtGetCurrentProcessorNumber },

        /* Nt 同步操作 */
        { "NtCreateEvent", (void *)win_NtCreateEvent },
        { "NtOpenEvent", (void *)win_NtOpenEvent },
        { "NtSetEvent", (void *)win_NtSetEvent },
        { "NtResetEvent", (void *)win_NtResetEvent },
        { "NtClearEvent", (void *)win_NtClearEvent },
        { "NtCreateMutant", (void *)win_NtCreateMutant },
        { "NtReleaseMutant", (void *)win_NtReleaseMutant },
        { "NtCreateSemaphore", (void *)win_NtCreateSemaphore },
        { "NtReleaseSemaphore", (void *)win_NtReleaseSemaphore },
        { "NtCreateTimer", (void *)win_NtCreateTimer },
        { "NtSetTimer", (void *)win_NtSetTimer },
        { "NtCancelTimer", (void *)win_NtCancelTimer },
        { "NtWaitForSingleObject", (void *)win_NtWaitForSingleObject },
        { "NtWaitForMultipleObjects", (void *)win_NtWaitForMultipleObjects },
        { "NtDelayExecution", (void *)win_NtDelayExecution },
        { "NtSleep", (void *)win_NtSleep },

        /* Nt 系统信息 */
        { "NtQuerySystemInformation", (void *)win_NtQuerySystemInformation },
        { "NtSetSystemInformation", (void *)win_NtSetSystemInformation },
        { "NtQueryObject", (void *)win_NtQueryObject },
        { "NtQueryPerformanceCounter", (void *)win_NtQueryPerformanceCounter },
        { "NtQueryTimerResolution", (void *)win_NtQueryTimerResolution },

        /* Rtl 其他 */
        { "RtlEnterCriticalSection", (void *)win_RtlEnterCriticalSection },
        { "RtlLeaveCriticalSection", (void *)win_RtlLeaveCriticalSection },
        { "RtlTryEnterCriticalSection", (void *)win_RtlTryEnterCriticalSection },
        { "RtlInitializeCriticalSection", (void *)win_RtlInitializeCriticalSection },
        { "RtlDeleteCriticalSection", (void *)win_RtlDeleteCriticalSection },
        { "RtlSetLastWin32Error", (void *)win_RtlSetLastWin32Error },
        { "RtlGetLastWin32Error", (void *)win_RtlGetLastWin32Error },
        { "RtlRaiseException", (void *)win_RtlRaiseException },
        { "RtlUnwind", (void *)win_RtlUnwind },
        { "RtlAddVectoredExceptionHandler", (void *)win_RtlAddVectoredExceptionHandler },
        { "RtlRemoveVectoredExceptionHandler", (void *)win_RtlRemoveVectoredExceptionHandler },
        { "RtlAllocateAndInitializeSid", (void *)win_RtlAllocateAndInitializeSid },
        { "RtlFreeSid", (void *)win_RtlFreeSid },
        { "RtlLengthSid", (void *)win_RtlLengthSid },
        { "RtlEqualSid", (void *)win_RtlEqualSid },
        { "RtlInitializeSid", (void *)win_RtlInitializeSid },
        { "RtlSubAuthoritySid", (void *)win_RtlSubAuthoritySid },
        { "RtlSubAuthorityCountSid", (void *)win_RtlSubAuthorityCountSid },
        { "RtlGetVersion", (void *)win_RtlGetVersion },
        { "RtlCreateUserStack", (void *)win_RtlCreateUserStack },
        { "RtlFreeUserStack", (void *)win_RtlFreeUserStack },
        { "RtlPcToFileHeader", (void *)win_RtlPcToFileHeader },
        { "RtlLookupFunctionEntry", (void *)win_RtlLookupFunctionEntry },
        { "RtlVirtualUnwind", (void *)win_RtlVirtualUnwind },
        { "RtlRestoreContext", (void *)win_RtlRestoreContext },

        /* LDR 操作 */
        { "LdrLoadDll", (void *)win_LdrLoadDll },
        { "LdrGetDllHandle", (void *)win_LdrGetDllHandle },
        { "LdrGetProcedureAddress", (void *)win_LdrGetProcedureAddress },
        { "LdrGetDllHandleByName", (void *)win_LdrGetDllHandleByName },

        { NULL, NULL }
    };

    for (int i = 0; ntdll_exports[i].name; i++) {
        if (strcmp(ntdll_exports[i].name, name) == 0) {
            return ntdll_exports[i].func;
        }
    }
    return NULL;
}

void *ole32_get_proc(const char *name) {
    for (int i = 0; ole32_exports[i].name; i++) {
        if (strcmp(ole32_exports[i].name, name) == 0) {
            return ole32_exports[i].func;
        }
    }
    return NULL;
}

void *shell32_get_proc(const char *name) {
    for (int i = 0; shell32_exports[i].name; i++) {
        if (strcmp(shell32_exports[i].name, name) == 0) {
            return shell32_exports[i].func;
        }
    }
    return NULL;
}

void *comdlg32_get_proc(const char *name) {
    for (int i = 0; comdlg32_exports[i].name; i++) {
        if (strcmp(comdlg32_exports[i].name, name) == 0) {
            return comdlg32_exports[i].func;
        }
    }
    return NULL;
}

void *ws2_32_get_proc(const char *name) {
    for (int i = 0; ws2_32_exports[i].name; i++) {
        if (strcmp(ws2_32_exports[i].name, name) == 0) {
            return ws2_32_exports[i].func;
        }
    }
    return NULL;
}

void *iphlpapi_get_proc(const char *name) {
    for (int i = 0; iphlpapi_exports[i].name; i++) {
        if (strcmp(iphlpapi_exports[i].name, name) == 0) {
            return iphlpapi_exports[i].func;
        }
    }
    return NULL;
}

void *dnsapi_get_proc(const char *name) {
    for (int i = 0; dnsapi_exports[i].name; i++) {
        if (strcmp(dnsapi_exports[i].name, name) == 0) {
            return dnsapi_exports[i].func;
        }
    }
    return NULL;
}

void *wininet_get_proc(const char *name) {
    for (int i = 0; wininet_exports[i].name; i++) {
        if (strcmp(wininet_exports[i].name, name) == 0) {
            return wininet_exports[i].func;
        }
    }
    return NULL;
}

void *winhttp_get_proc(const char *name) {
    for (int i = 0; winhttp_exports[i].name; i++) {
        if (strcmp(winhttp_exports[i].name, name) == 0) {
            return winhttp_exports[i].func;
        }
    }
    return NULL;
}

void *mswsock_get_proc(const char *name) {
    for (int i = 0; mswsock_exports[i].name; i++) {
        if (strcmp(mswsock_exports[i].name, name) == 0) {
            return mswsock_exports[i].func;
        }
    }
    return NULL;
}

/* ========== Windows NT 系统调用处理器 ========== */

/* ========== TEB/PEB 结构定义 ========== */

/*
 * TEB (Thread Environment Block) - Windows 线程环境块
 * 通过 gs 段寄存器指向 TEB，gs:0x60 获取 PEB
 */
struct win_teb {
    uint64_t exception_list;        /* +0x000: SEH 链头 */
    uint64_t stack_base;            /* +0x008: 栈基址 */
    uint64_t stack_limit;           /* +0x010: 栈限制 */
    uint64_t sub_system_tib;        /* +0x018: 子系统 TIB */
    uint64_t fiber_data;            /* +0x020: Fiber 数据 */
    uint64_t arbitrary_user;        /* +0x028: 任意用户指针 */
    uint64_t tls_array;             /* +0x030: TLS 数组 */
    uint64_t peb;                   /* +0x038: PEB 指针 (gs:0x60) */
    uint64_t last_error;            /* +0x040: LastErrorValue */
    uint64_t count_of_owned;        /* +0x048: 临界区计数 */
    uint64_t activation_context;    /* +0x050: 激活上下文栈 */
    uint8_t  reserved[104];         /* +0x058: 保留 */
    uint64_t tls_slots[64];         /* +0x0C0: TLS 槽位 */
    uint8_t  reserved2[96];         /* +0x2C0: 保留 */
    uint64_t tls_expansion;         /* +0x320: TLS 扩展槽位 */
};

/*
 * PEB (Process Environment Block) - Windows 进程环境块
 */
struct win_peb {
    uint8_t  inherited;             /* +0x000: 继承标志 */
    uint8_t  read_image;            /* +0x001: 读取映像标志 */
    uint8_t  spare1;                /* +0x002: 保留 */
    uint8_t  being_debugged;        /* +0x003: 调试标志 */
    uint8_t  spare2[4];             /* +0x004: 保留 */
    uint64_t mutant;                /* +0x008: 互斥体 */
    uint64_t image_base_address;    /* +0x010: 镜像基址 */
    uint64_t ldr;                   /* +0x018: PEB_LDR_DATA */
    uint64_t process_parameters;    /* +0x020: 进程参数 (RTL_USER_PROCESS_PARAMETERS) */
    uint64_t sub_system_data;       /* +0x028: 子系统数据 */
    uint64_t process_heap;          /* +0x030: 进程默认堆 */
    uint64_t fast_peb_lock;         /* +0x038: 快速锁 */
    uint64_t atl_thunk_slist;       /* +0x040: ATL thunk */
    uint64_t ifeo_key;              /* +0x048: IFEO 键 */
    uint64_t cross_process_flags;   /* +0x050: 跨进程标志 */
    uint64_t kernel_callback_table; /* +0x058: 内核回调表 */
    uint64_t system_reserved;       /* +0x060: 系统保留 */
    uint64_t atl_thunk_slist_ptr;   /* +0x068: ATL thunk 指针 */
    uint64_t api_set_map;           /* +0x070: API Set 映射 */
    uint64_t tls_expansion_counter; /* +0x078: TLS 扩展计数 */
    uint64_t tls_bitmap;            /* +0x080: TLS 位图 */
    uint64_t tls_bitmap_bits[2];    /* +0x088: TLS 位图位 */
    uint64_t read_only_shared_mem;  /* +0x098: 只读共享内存 */
    uint64_t read_only_static_data; /* +0x0A0: 只读静态数据 */
    uint64_t code_page_data;        /* +0x0A8: 代码页数据 */
    uint64_t code_page;             /* +0x0B0: 代码页 */
    uint64_t os_major_version;      /* +0x0B8: OS 主版本号 */
    uint64_t os_minor_version;      /* +0x0C0: OS 次版本号 */
    uint64_t os_build_number;       /* +0x0C8: OS 构建号 */
    uint64_t os_platform_id;        /* +0x0D0: OS 平台 ID */
};

/* 进程参数结构 (RTL_USER_PROCESS_PARAMETERS) */
struct win_process_params {
    uint32_t length;                /* 结构大小 */
    uint32_t flags;                 /* 标志 */
    uint32_t console_flags;         /* 控制台标志 */
    uint32_t reserved1;             /* 保留 */
    uint64_t stdin_handle;          /* 标准输入 */
    uint64_t stdout_handle;         /* 标准输出 */
    uint64_t stderr_handle;         /* 标准错误 */
    uint16_t current_directory_length;
    uint16_t reserved2;
    uint64_t current_directory;     /* 当前目录字符串 */
    uint64_t dll_path;              /* DLL 搜索路径 */
    uint64_t image_path_name;       /* 可执行文件路径 */
    uint64_t command_line;          /* 命令行字符串 */
    uint64_t environment;           /* 环境变量块 */
};

/* 当前进程的 TEB/PEB（内核空间分配，映射到用户空间） */
static struct win_teb *current_teb = NULL;
static struct win_peb *current_peb = NULL;
static struct win_process_params *current_params = NULL;

/* 为 PE 进程初始化 TEB 和 PEB */
int pe_init_teb_peb(uint64_t image_base, uint64_t stack_top, uint64_t stack_size,
                    const char *cmdline) {
    struct process *proc = process_get_current();
    if (proc == NULL || proc->space == NULL) return -1;

    /* 分配 TEB（用户空间可访问） */
    uint64_t teb_phys = pmm_alloc_page();
    if (teb_phys == 0) return -1;
    uint64_t teb_vaddr = 0x7FFE0000ULL;  /* Windows 标准 TEB 位置 */
    if (vmm_map_page(proc->space, teb_vaddr, teb_phys,
                     VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE) != 0) {
        pmm_free_page(teb_phys);
        return -1;
    }
    current_teb = (struct win_teb *)teb_vaddr;

    /* 分配 PEB */
    uint64_t peb_phys = pmm_alloc_page();
    if (peb_phys == 0) return -1;
    uint64_t peb_vaddr = 0x7FFD0000ULL;
    if (vmm_map_page(proc->space, peb_vaddr, peb_phys,
                     VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE) != 0) {
        pmm_free_page(peb_phys);
        return -1;
    }
    current_peb = (struct win_peb *)peb_vaddr;

    /* 分配进程参数 */
    uint64_t params_phys = pmm_alloc_page();
    if (params_phys == 0) return -1;
    uint64_t params_vaddr = 0x7FFC0000ULL;
    if (vmm_map_page(proc->space, params_vaddr, params_phys,
                     VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE) != 0) {
        pmm_free_page(params_phys);
        return -1;
    }
    current_params = (struct win_process_params *)params_vaddr;

    /* 清零 TEB */
    for (uint32_t i = 0; i < PAGE_SIZE / 8; i++) {
        ((uint64_t *)current_teb)[i] = 0;
    }

    /* 清零 PEB */
    for (uint32_t i = 0; i < PAGE_SIZE / 8; i++) {
        ((uint64_t *)current_peb)[i] = 0;
    }

    /* 清零进程参数 */
    for (uint32_t i = 0; i < PAGE_SIZE / 8; i++) {
        ((uint64_t *)current_params)[i] = 0;
    }

    /* 填充 TEB */
    current_teb->stack_base = stack_top;
    current_teb->stack_limit = stack_top - stack_size;
    current_teb->peb = peb_vaddr;
    current_teb->last_error = 0;

    /* 填充 PEB */
    current_peb->being_debugged = 0;
    current_peb->image_base_address = image_base;
    current_peb->process_heap = 0;  /* TODO: 创建默认堆 */
    current_peb->process_parameters = params_vaddr;
    current_peb->os_major_version = 10;  /* Windows 10 */
    current_peb->os_minor_version = 0;
    current_peb->os_build_number = 19041;
    current_peb->os_platform_id = 2;  /* VER_PLATFORM_WIN32_NT */

    /* 填充进程参数 */
    current_params->length = sizeof(struct win_process_params);
    current_params->stdin_handle = 0;   /* 标准句柄索引 */
    current_params->stdout_handle = 1;
    current_params->stderr_handle = 2;
    current_params->image_path_name = 0;  /* TODO: 填入实际路径 */
    current_params->command_line = 0;     /* TODO: 填入命令行 */

    print("[PE] TEB at ");
    print_hex(teb_vaddr);
    print(", PEB at ");
    print_hex(peb_vaddr);
    println("");

    return 0;
}

/* 获取当前 PEB 地址 */
uint64_t pe_get_peb_address(void) {
    return current_peb ? (uint64_t)current_peb : 0;
}

/* 获取当前 TEB 地址 */
uint64_t pe_get_teb_address(void) {
    return current_teb ? (uint64_t)current_teb : 0;
}

/*
 * Windows NT syscall 号 (x64)
 * PE 程序通常通过 IAT 调用兼容层函数，不直接执行 syscall。
 * 但 ntdll.dll 导出函数（如 NtCreateFile）内部会执行 syscall，
 * 此处理器作为备用路径处理这些调用。
 */

/* Windows NT 系统调用号定义 (Windows 10/11 x64) */
#define WIN_SYS_NTCLOSE                     0x000F
#define WIN_SYS_NTREADFILE                  0x0006
#define WIN_SYS_NTWRITEFILE                 0x0008
#define WIN_SYS_NTCREATEFILE                0x0055
#define WIN_SYS_NTOPENFILE                  0x0034
#define WIN_SYS_NTDEVICEIOCONTROLFILE       0x0007
#define WIN_SYS_NTQUERYINFORMATIONFILE      0x0023
#define WIN_SYS_NTSETINFORMATIONFILE        0x0024
#define WIN_SYS_NTQUERYVOLUMEINFORMATIONFILE 0x0040
#define WIN_SYS_NTSETVOLUMEINFORMATIONFILE  0x0041
#define WIN_SYS_NTCREATESECTION             0x004D
#define WIN_SYS_NTMAPVIEWOFSECTION          0x0028
#define WIN_SYS_NTUNMAPVIEWOFSECTION        0x002A
#define WIN_SYS_NTCREATEPROCESS             0x004B
#define WIN_SYS_NTCREATETHREAD              0x004E
#define WIN_SYS_NTCREATEEVENT               0x004C
#define WIN_SYS_NTSETEVENT                   0x0014
#define WIN_SYS_NTCREATEMUTANT              0x001B
#define WIN_SYS_NTRELEASEMUTANT             0x001D
#define WIN_SYS_NTCREATESEMAPHORE           0x004F
#define WIN_SYS_NTRELEASESEMAPHORE          0x0051
#define WIN_SYS_NTWAITFORSINGLEOBJECT       0x0004
#define WIN_SYS_NTWAITFORMULTIPLEOBJECTS    0x0003
#define WIN_SYS_NTSLEEP                      0x0005
#define WIN_SYS_NTCREATEKEYEDEVENT          0x0052
#define WIN_SYS_NTALLOCVIRTUALMEMORY        0x0018
#define WIN_SYS_NTFREEVIRTUALMEMORY         0x0019
#define WIN_SYS_NTPROTECTVIRTUALMEMORY      0x0050
#define WIN_SYS_NTQUERYVIRTUALMEMORY        0x0025
#define WIN_SYS_NTQUERYSYSTEMINFORMATION    0x0036
#define WIN_SYS_NTQUERYOBJECT               0x0010
#define WIN_SYS_NTGETCONTEXT                0x0038
#define WIN_SYS_NTSETCONTEXT                0x0039
#define WIN_SYS_NTSUSPENDTHREAD             0x003B
#define WIN_SYS_NTRESUMETHREAD              0x003C
#define WIN_SYS_NTTerminateProcess          0x002C
#define WIN_SYS_NTTerminateThread           0x002D
#define WIN_SYS_NTQUERYPERFORMANCECOUNTER   0x0031
#define WIN_SYS_NTALPCCREATEPORT            0x0044

uint64_t windows_syscall_handler(uint64_t num, uint64_t arg0, uint64_t arg1,
                                  uint64_t arg2, uint64_t arg3, uint64_t arg4,
                                  uint64_t arg5, uint64_t arg6, uint64_t arg7) {
    (void)arg6; (void)arg7;

    switch (num) {
    /* 文件操作 */
    case WIN_SYS_NTCLOSE:
        return (uint64_t)win_CloseHandle((HANDLE)arg0);

    case WIN_SYS_NTREADFILE: {
        /* NtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key) */
        DWORD read = 0;
        BOOL ret = win_ReadFile((HANDLE)arg0, (LPVOID)arg5, (DWORD)arg6, &read, NULL);
        if (!ret) return (uint64_t)STATUS_ERROR;
        return (uint64_t)STATUS_SUCCESS;
    }

    case WIN_SYS_NTWRITEFILE: {
        /* NtWriteFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key) */
        DWORD written = 0;
        BOOL ret = win_WriteFile((HANDLE)arg0, (LPCVOID)arg5, (DWORD)arg6, &written, NULL);
        if (!ret) return (uint64_t)STATUS_ERROR;
        return (uint64_t)STATUS_SUCCESS;
    }

    case WIN_SYS_NTCREATEFILE: {
        /* NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength) */
        HANDLE *handle_out = (HANDLE *)arg0;
        if (!handle_out) return STATUS_INVALID_PARAMETER;
        /* 简化: 忽略 ObjectAttributes 中的路径，使用空路径 */
        /* 实际 PE 程序很少直接通过 syscall 调用 NtCreateFile，通常通过 kernel32.CreateFileA */
        *handle_out = INVALID_HANDLE_VALUE;
        return STATUS_NOT_IMPLEMENTED;
    }

    case WIN_SYS_NTOPENFILE: {
        HANDLE *handle_out = (HANDLE *)arg0;
        if (!handle_out) return STATUS_INVALID_PARAMETER;
        *handle_out = INVALID_HANDLE_VALUE;
        return STATUS_NOT_IMPLEMENTED;
    }

    /* 内存操作 */
    case WIN_SYS_NTALLOCVIRTUALMEMORY: {
        /* NtAllocateVirtualMemory(ProcessHandle, BaseAddress, ZeroBits, RegionSize, AllocationType, Protect) */
        LPVOID *base_addr = (LPVOID *)arg1;
        SIZE_T *region_size = (SIZE_T *)arg3;
        if (base_addr == NULL || region_size == NULL) return (uint64_t)STATUS_INVALID_PARAMETER;
        LPVOID result = win_VirtualAlloc(*base_addr, *region_size, (DWORD)arg4, (DWORD)arg5);
        if (result == NULL) return (uint64_t)STATUS_NO_MEMORY;
        *base_addr = result;
        return (uint64_t)STATUS_SUCCESS;
    }

    case WIN_SYS_NTFREEVIRTUALMEMORY: {
        /* NtFreeVirtualMemory(ProcessHandle, BaseAddress, RegionSize, FreeType) */
        LPVOID *base_addr = (LPVOID *)arg1;
        SIZE_T *region_size = (SIZE_T *)arg3;
        if (base_addr == NULL) return (uint64_t)STATUS_INVALID_PARAMETER;
        BOOL ret = win_VirtualFree(*base_addr, region_size ? *region_size : 0, (DWORD)arg4);
        return ret ? (uint64_t)STATUS_SUCCESS : (uint64_t)STATUS_ERROR;
    }

    case WIN_SYS_NTPROTECTVIRTUALMEMORY: {
        /* NtProtectVirtualMemory(ProcessHandle, BaseAddress, RegionSize, NewProtect, OldProtect) */
        LPVOID *base_addr = (LPVOID *)arg1;
        SIZE_T *region_size = (SIZE_T *)arg3;
        if (base_addr == NULL || region_size == NULL) return (uint64_t)STATUS_INVALID_PARAMETER;
        DWORD old_prot = 0;
        BOOL ret = win_VirtualProtect(*base_addr, *region_size, (DWORD)arg4, &old_prot);
        if (arg5 != 0) {
            *((DWORD *)arg5) = old_prot;
        }
        return ret ? (uint64_t)STATUS_SUCCESS : (uint64_t)STATUS_ERROR;
    }

    /* 同步对象 */
    case WIN_SYS_NTCREATEEVENT:
        return win_NtCreateEvent((HANDLE *)arg0, (uint32_t)arg1,
                                 (OBJECT_ATTRIBUTES *)arg2, (uint32_t)arg3, (int)arg4);

    case WIN_SYS_NTSETEVENT:
        return win_NtSetEvent((HANDLE)arg0, (uint32_t *)arg1);

    case WIN_SYS_NTCREATEMUTANT:
        return win_NtCreateMutant((HANDLE *)arg0, (uint32_t)arg1,
                                  (OBJECT_ATTRIBUTES *)arg2, (int)arg3);

    case WIN_SYS_NTRELEASEMUTANT:
        return win_NtReleaseMutant((HANDLE)arg0, (uint32_t *)arg1);

    case WIN_SYS_NTCREATESEMAPHORE:
        return win_NtCreateSemaphore((HANDLE *)arg0, (uint32_t)arg1,
                                     (OBJECT_ATTRIBUTES *)arg2, (uint32_t)arg3, (uint32_t)arg4);

    case WIN_SYS_NTRELEASESEMAPHORE:
        return win_NtReleaseSemaphore((HANDLE)arg0, (uint32_t)arg1, (uint32_t *)arg2);

    /* 等待 */
    case WIN_SYS_NTWAITFORSINGLEOBJECT: {
        /* NtWaitForSingleObject(Handle, Alertable, Timeout) */
        DWORD timeout_ms = INFINITE;
        LARGE_INTEGER *timeout = (LARGE_INTEGER *)arg2;
        if (timeout != NULL && timeout->QuadPart < 0) {
            /* 负值表示相对时间（100ns 单位） */
            timeout_ms = (DWORD)(-timeout->QuadPart / 10000);
        }
        DWORD result = win_WaitForSingleObject((HANDLE)arg0, timeout_ms);
        if (result == WAIT_OBJECT_0) return (uint64_t)STATUS_SUCCESS;
        if (result == WAIT_TIMEOUT) return (uint64_t)STATUS_TIMEOUT;
        return (uint64_t)STATUS_ERROR;
    }

    case WIN_SYS_NTWAITFORMULTIPLEOBJECTS:
        return win_NtWaitForMultipleObjects((uint32_t)arg0, (HANDLE *)arg1,
                                            (uint32_t)arg2, (int)arg3, (uint64_t *)arg4);

    /* 线程/进程 */
    case WIN_SYS_NTSLEEP: {
        /* NtSleep(DelayInterval) */
        LARGE_INTEGER *delay = (LARGE_INTEGER *)arg0;
        if (delay != NULL) {
            if (delay->QuadPart < 0) {
                DWORD ms = (DWORD)(-delay->QuadPart / 10000);
                win_Sleep(ms);
            } else {
                /* 绝对时间 - 计算差值 */
                win_Sleep(INFINITE);
            }
        }
        return (uint64_t)STATUS_SUCCESS;
    }

    case WIN_SYS_NTSUSPENDTHREAD:
        return win_NtSuspendThread((HANDLE)arg0, (uint32_t *)arg1);

    case WIN_SYS_NTRESUMETHREAD:
        return win_NtResumeThread((HANDLE)arg0, (uint32_t *)arg1);

    case WIN_SYS_NTTerminateProcess:
        win_ExitProcess((UINT)arg1);
        return (uint64_t)STATUS_SUCCESS;

    case WIN_SYS_NTTerminateThread:
        win_ExitThread((DWORD)arg1);
        return (uint64_t)STATUS_SUCCESS;

    /* 信息查询 */
    case WIN_SYS_NTQUERYVIRTUALMEMORY:
        return win_NtQueryVirtualMemory((void *)(uintptr_t)arg0, (void *)arg1,
                                         (uint32_t)arg2, (void *)arg3,
                                         (size_t)arg4, (size_t *)arg5);

    case WIN_SYS_NTQUERYSYSTEMINFORMATION:
        return win_NtQuerySystemInformation((uint32_t)arg0, (void *)arg1,
                                            (uint32_t)arg2, (uint32_t *)arg3);

    case WIN_SYS_NTQUERYOBJECT:
        return win_NtQueryObject((HANDLE)arg0, (uint32_t)arg1,
                                 (void *)arg2, (uint32_t)arg3, (uint32_t *)arg4);

    case WIN_SYS_NTQUERYPERFORMANCECOUNTER: {
        PLARGE_INTEGER counter = (PLARGE_INTEGER)arg0;
        if (counter) win_QueryPerformanceCounter(counter);
        PLARGE_INTEGER freq = (PLARGE_INTEGER)arg1;
        if (freq) win_QueryPerformanceFrequency(freq);
        return (uint64_t)STATUS_SUCCESS;
    }

    /* 其他 */
    case WIN_SYS_NTDEVICEIOCONTROLFILE:
        return (uint64_t)STATUS_NOT_IMPLEMENTED;

    case WIN_SYS_NTQUERYINFORMATIONFILE:
        return (uint64_t)STATUS_NOT_IMPLEMENTED;

    case WIN_SYS_NTSETINFORMATIONFILE:
        return (uint64_t)STATUS_NOT_IMPLEMENTED;

    case WIN_SYS_NTCREATESECTION:
    case WIN_SYS_NTMAPVIEWOFSECTION:
    case WIN_SYS_NTUNMAPVIEWOFSECTION:
        return STATUS_NOT_IMPLEMENTED;

    case WIN_SYS_NTCREATEPROCESS:
        return win_NtCreateProcess((void *)arg0, (uint32_t)arg1, (OBJECT_ATTRIBUTES *)arg2,
                                   (HANDLE)arg3, (int)arg4, (uint32_t)arg5,
                                   (uint32_t)arg6, (uint32_t)arg7);

    case WIN_SYS_NTCREATETHREAD:
        return win_NtCreateThread((void *)arg0, (uint32_t)arg1, (OBJECT_ATTRIBUTES *)arg2,
                                  (HANDLE)arg3, (void *)arg4, (void *)arg5,
                                  (uint32_t)arg6, (size_t)arg7, 0);
    case WIN_SYS_NTGETCONTEXT:
        return win_NtGetContextThread((HANDLE)arg0, (void *)arg1);

    case WIN_SYS_NTSETCONTEXT:
        return win_NtSetContextThread((HANDLE)arg0, (void *)arg1);

    case WIN_SYS_NTCREATEKEYEDEVENT:
    case WIN_SYS_NTALPCCREATEPORT:
        return STATUS_NOT_IMPLEMENTED;

    default:
        print("[WIN32] Unhandled NT syscall: 0x");
        print_hex(num);
        println("");
        return (uint64_t)STATUS_NOT_IMPLEMENTED;
    }
}

/* ========== DLL 初始化 ========== */

void dll_init(void) {
    /* 初始化句柄表 */
    for (int i = 0; i < MAX_WIN_HANDLES; i++) {
        win_handles[i].used = 0;
        win_handles[i].type = WIN_HANDLE_NONE;
        win_handles[i].data = 0;
        win_handles[i].extra = 0;
        win_handles[i].ref_count = 0;
    }
    
    /* 预分配标准句柄 */
    win_handles[0].used = 1;
    win_handles[0].type = WIN_HANDLE_FILE;
    win_handles[0].data = 0;
    
    win_handles[1].used = 1;
    win_handles[1].type = WIN_HANDLE_FILE;
    win_handles[1].data = 1;
    
    win_handles[2].used = 1;
    win_handles[2].type = WIN_HANDLE_FILE;
    win_handles[2].data = 2;
    
    win_handle_next = 4;
    
    /* 初始化 TLS */
    for (int i = 0; i < MAX_TLS_INDEX; i++) {
        tls_in_use[i] = 0;
    }
    
    /* 初始化 SEH */
    for (int i = 0; i < MAX_EXCEPTION_HANDLERS; i++) {
        seh_handlers[i].used = 0;
    }
    
    /* 初始化窗口系统 */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        win_windows[i].used = 0;
    }
    
    for (int i = 0; i < MAX_MESSAGES; i++) {
        win_messages[i].used = 0;
    }
    
    /* 初始化 GDI */
    for (int i = 0; i < MAX_GDI_OBJECTS; i++) {
        gdi_objects[i].used = 0;
    }
    
    /* 初始化 DC */
    for (int i = 0; i < MAX_DCS; i++) {
        win_dcs[i].used = 0;
    }
    
    /* 初始化注册表 */
    for (int i = 0; i < MAX_REG_KEYS; i++) {
        reg_keys[i].used = 0;
    }
    
    /* 初始化环境变量 */
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        env_vars[i].used = 0;
    }
    
    /* 设置默认环境变量 */
    win_SetEnvironmentVariableA("PATH", "C:\\Windows\\System32;C:\\Windows");
    win_SetEnvironmentVariableA("SYSTEMROOT", "C:\\Windows");
    win_SetEnvironmentVariableA("USERNAME", "user");
    win_SetEnvironmentVariableA("COMPUTERNAME", "ZRATRO");
    win_SetEnvironmentVariableA("TEMP", "/tmp");
    win_SetEnvironmentVariableA("TMP", "/tmp");

    /* 初始化 WSA */
    wsa_initialized = 0;
    wsa_last_error = 0;

    /* 初始化定时器 */
    for (int i = 0; i < MAX_TIMERS; i++) {
        win_timers[i].used = 0;
    }
}
