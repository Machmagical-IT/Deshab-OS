/* host_main.c — DeshabOS 桌面演示的 Win32 宿主
 *
 * 目标：启动画面与桌面 UI 全部由原工程源码绘制（不是重绘近似）：
 *   1) CODE/boot/splash.c  —— 引导齿轮/铆钉/40 段进度条（boot_splash.c 编译）
 *   2) CODE/desktop/*.c    —— Aqua 桌面（壁纸/顶栏/Dock/窗口/内置应用）
 *
 * 宿主负责：固定物理地址区、帧缓冲、输入事件、虚拟 FAT32 卷、伪 kernel_api、
 *           线程编排（boot → desktop）与窗口呈现。
 *
 * 说明：外部 ELF / PE / Linux guest 应用依赖裸机地址空间，本进程内不执行，
 *       点击时只弹一条演示提示；内置应用（Editor/Files/Calculator）完整可用。
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../../CODE/DKM/dkm_shared.h"
#include "../../../CODE/DKM/dkm_input.h"
#include "../../../CODE/desktop/desktop.h"

/* ---- 原工程入口（main.c 提供） ---- */
int driver_init(const struct dkm_kernel_api *api, struct dkm_driver_handle *handle);

/* ---- 引导动画（boot_splash.c 提供） ---- */
void demo_splash_set_fb(void *fb, unsigned w, unsigned h, unsigned pitch);
void demo_splash_begin(void);
void demo_splash_load(unsigned q, const char *stage,
                      unsigned long long loaded, unsigned long long total,
                      unsigned files_done, unsigned files_total);
void demo_splash_finish(void);

#define DW 1280
#define DH 800
#define PITCH (DW * 4)
#define RES_ESP_IMG 301

/* ---- 全局 ---- */
static HINSTANCE g_inst;
static HWND g_hwnd;
static HDC  g_mdc;
static HBITMAP g_bmp;
static u32 *g_px;
static volatile LONG g_host_quit;
static volatile LONG g_desktop_started;

static u8 *g_img;             /* 可写 FAT32 卷映像 */
static u32 g_img_size;

static int (*g_desktop_run)(void);   /* 由伪总线捕获 srv.desktop.run */

/* ---- 串口日志 → demo_boot.log（桌面 slog 全部落到这里） ---- */
static FILE *g_logf;
static CRITICAL_SECTION g_log_lock;
static char g_log_line[1024];
static int g_log_len;

static void host_log_open(void) {
    char path[MAX_PATH];
    GetModuleFileNameA(0, path, MAX_PATH);
    char *p = strrchr(path, '\\');
    if (p) strcpy(p + 1, "demo_boot.log");
    g_logf = fopen(path, "w");
}

void host_serial_char(char c) {
    if (!g_logf) return;
    EnterCriticalSection(&g_log_lock);
    if (c == '\n' || g_log_len >= (int)sizeof(g_log_line) - 1) {
        g_log_line[g_log_len] = 0;
        fputs(g_log_line, g_logf);
        fputc('\n', g_logf);
        fflush(g_logf);
        g_log_len = 0;
    } else if (c != '\r') {
        g_log_line[g_log_len++] = c;
    }
    LeaveCriticalSection(&g_log_lock);
}

static void host_log_direct(const char *s) {
    if (!g_logf) return;
    EnterCriticalSection(&g_log_lock);
    fputs(s, g_logf);
    fputc('\n', g_logf);
    fflush(g_logf);
    LeaveCriticalSection(&g_log_lock);
}

void host_outb(unsigned short port, unsigned char value) { (void)port; (void)value; }
unsigned char host_inb(unsigned short port) { (void)port; return 0; }

void host_rtc_read(int *yy, int *mo, int *dd, int *hh, int *mi, int *ss) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    *yy = st.wYear % 100;
    *mo = st.wMonth;
    *dd = st.wDay;
    *hh = st.wHour;
    *mi = st.wMinute;
    *ss = st.wSecond;
}

int host_rtc_day_sec(void) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
}

unsigned long long host_tsc_calibrate(void) {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    u64 c0 = __rdtsc();
    Sleep(60);
    u64 c1 = __rdtsc();
    QueryPerformanceCounter(&b);
    double sec = (double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart;
    if (sec <= 0.0) sec = 0.001;
    return (unsigned long long)((double)(c1 - c0) / sec);
}

/* ---- 演示提示条（外部应用不可用） ---- */
static char g_toast[256];
static volatile LONG g_toast_until;

static void toast_set(const char *fmt, const char *arg) {
    char buf[256];
    _snprintf(buf, sizeof(buf) - 1, fmt, arg ? arg : "");
    buf[sizeof(buf) - 1] = 0;
    EnterCriticalSection(&g_log_lock);
    strcpy(g_toast, buf);
    g_toast_until = (LONG)(GetTickCount() + 2800);
    LeaveCriticalSection(&g_log_lock);
}

void host_demo_unapp(const char *what) {
    toast_set("演示环境：外部应用 %s 需要裸机运行，已跳过", what);
    host_log_direct("[host] external app skipped");
}

void host_demo_quit(void) {
    InterlockedExchange(&g_host_quit, 1);
    PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
}

void host_fatal_halt(const char *sym) {
    host_log_direct("[host] desktop_fatal:");
    host_log_direct(sym ? sym : "(null)");
    toast_set("演示遇到 fatal：%s", sym ? sym : "unknown");
    for (;;) Sleep(1000);
}

/* ============================== 伪 kernel_api / bus ============================== */

static struct dkm_kernel_api g_kapi;
static struct dkm_log_api   g_log_api;
static struct dkm_block_api g_block_api;
static struct utsm_sched_api g_sched_api;
static struct dkm_bus_service g_bus_svc[DKM_BUS_MAX_SERVICES];
static struct dkm_bus_api g_bus_api;
static int g_bus_n;
static CRITICAL_SECTION g_bus_lock;

static void log_info(const char *m)  { host_log_direct(m); }
static void log_warn(const char *m)  { host_log_direct(m); }
static void log_error(const char *m) { host_log_direct(m); }
static void log_panic(const char *m) { host_log_direct(m); }

static int blk_read(u32 index, u64 lba, u32 count, void *buffer) {
    (void)index;
    u64 off = lba * 512ULL;
    u64 bytes = (u64)count * 512ULL;
    if (!g_img || off + bytes > g_img_size) return -1;
    memcpy(buffer, g_img + off, (size_t)bytes);
    return 0;
}

static int blk_write(u32 index, u64 lba, u32 count, const void *buffer) {
    (void)index;
    u64 off = lba * 512ULL;
    u64 bytes = (u64)count * 512ULL;
    if (!g_img || off + bytes > g_img_size) return -1;
    memcpy(g_img + off, buffer, (size_t)bytes);
    return 0;
}

static u64 blk_sector_size(u32 index) { (void)index; return 512; }
static const char *blk_name(u32 index) { (void)index; return "demo0"; }
static u64 blk_sector_count(u32 index) { (void)index; return g_img_size / 512; }
static int blk_set_write(u32 i, dkm_block_write_fn fn) { (void)i; (void)fn; return 0; }
static int blk_register(const struct dkm_block_device_desc *d) { (void)d; return 0; }
static u32 blk_count(void) { return 1; }

static int bus_register(const char *name, u32 version, void *fn) {
    if (!name || !fn) return -1;
    if (!strcmp(name, "srv.desktop.run")) g_desktop_run = (int (*)(void))fn;
    EnterCriticalSection(&g_bus_lock);
    for (int i = 0; i < g_bus_n; i++) {
        if (!strcmp(g_bus_svc[i].name, name)) {
            g_bus_svc[i].version = version;
            g_bus_svc[i].fn = fn;
            LeaveCriticalSection(&g_bus_lock);
            return 0;
        }
    }
    if (g_bus_n >= DKM_BUS_MAX_SERVICES) { LeaveCriticalSection(&g_bus_lock); return -1; }
    strncpy(g_bus_svc[g_bus_n].name, name, DKM_BUS_MAX_NAME - 1);
    g_bus_svc[g_bus_n].version = version;
    g_bus_svc[g_bus_n].flags = DKM_BUS_F_NO_UNLOAD;
    g_bus_svc[g_bus_n].fn = fn;
    g_bus_n++;
    LeaveCriticalSection(&g_bus_lock);
    return 0;
}

static int bus_lookup(const char *name, void **out, u32 *out_ver) {
    if (out) *out = 0;
    if (out_ver) *out_ver = 0;
    if (!name) return -1;
    EnterCriticalSection(&g_bus_lock);
    for (int i = 0; i < g_bus_n; i++) {
        if (!strcmp(g_bus_svc[i].name, name)) {
            if (out) *out = g_bus_svc[i].fn;
            if (out_ver) *out_ver = g_bus_svc[i].version;
            LeaveCriticalSection(&g_bus_lock);
            return 0;
        }
    }
    LeaveCriticalSection(&g_bus_lock);
    return -1;
}

static int bus_probe(const char *name, void **out, u32 *out_ver) {
    return bus_lookup(name, out, out_ver);
}
static u32 bus_count(void) { return (u32)g_bus_n; }
static const struct dkm_bus_service *bus_at(u32 i) {
    return (i < (u32)g_bus_n) ? &g_bus_svc[i] : 0;
}

/* ---- 输入服务 ---- */
static volatile int g_mx = 400, g_my = 300;
static volatile int g_mbtn = 0;
static volatile int g_mpkt = 0;
static CRITICAL_SECTION g_mouse_lock;

static int svc_mouse_poll(dkm_input_mouse *out) {
    if (!out) return -1;
    EnterCriticalSection(&g_mouse_lock);
    out->magic = DKM_INPUT_MOUSE_MAGIC;
    out->version = 1;
    out->x = g_mx;
    out->y = g_my;
    out->buttons = (u32)g_mbtn;
    out->packet_count = (u32)g_mpkt;
    out->overflow_count = 0;
    out->max_x = DW - CURSOR_SIZE;
    out->max_y = DH - CURSOR_SIZE;
    LeaveCriticalSection(&g_mouse_lock);
    return 0;
}

#define KBQ 256
static volatile unsigned char g_kbq[KBQ];
static volatile int g_kbr, g_kbw;

static int svc_kbd_poll(u8 *out, u32 cap) {
    int n = 0;
    while (n < (int)cap && g_kbr != g_kbw) {
        out[n++] = g_kbq[g_kbr];
        g_kbr = (g_kbr + 1) % KBQ;
    }
    return n;
}

static const char *svc_config_get(const char *section, const char *key) {
    (void)section; (void)key;
    return 0;
}

static void sched_sleep_ms(u32 ms) { Sleep(ms ? ms : 1); }
static u64 sched_uptime_ns(void) { return (u64)GetTickCount64() * 1000000ULL; }
static void sched_yield(void) { Sleep(0); }
static u32 sched_current(void) { return 0; }
static int sched_task_create(u32 p, u32 prio, void (*entry)(void *), void *arg) {
    (void)p; (void)prio; (void)entry; (void)arg; return -1;
}

static void build_fake_api(void) {
    memset(&g_kapi, 0, sizeof(g_kapi));
    memset(&g_log_api, 0, sizeof(g_log_api));
    memset(&g_block_api, 0, sizeof(g_block_api));
    memset(&g_sched_api, 0, sizeof(g_sched_api));

    g_log_api.info = log_info;
    g_log_api.warn = log_warn;
    g_log_api.error = log_error;
    g_log_api.panic = log_panic;

    g_block_api.register_device = blk_register;
    g_block_api.device_count = blk_count;
    g_block_api.read = blk_read;
    g_block_api.write = blk_write;
    g_block_api.sector_size = blk_sector_size;
    g_block_api.device_name = blk_name;
    g_block_api.set_write_fn = blk_set_write;
    g_block_api.sector_count = blk_sector_count;

    g_sched_api.uptime_ns = sched_uptime_ns;
    g_sched_api.yield = sched_yield;
    g_sched_api.current_task_slot = sched_current;
    g_sched_api.task_create = sched_task_create;
    g_sched_api.sleep_ms = sched_sleep_ms;

    static struct dkm_bus_api bus;
    memset(&bus, 0, sizeof(bus));
    bus.magic = DKM_BUS_MAGIC;
    bus.version = DKM_BUS_VERSION;
    bus.register_service = bus_register;
    bus.unregister_service = 0;
    bus.lookup = bus_lookup;
    bus.probe = bus_probe;
    bus.count = bus_count;
    bus.at = bus_at;
    g_bus_api = bus;

    g_kapi.version = 1;
    g_kapi.size = sizeof(g_kapi);
    g_kapi.log = &g_log_api;
    g_kapi.fb_address = (const void *)g_px;
    g_kapi.fb_width = DW;
    g_kapi.fb_height = DH;
    g_kapi.fb_pitch = PITCH;
    g_kapi.fb_bpp = 32;
    g_kapi.block = &g_block_api;
    g_kapi.sched = &g_sched_api;
    g_kapi.bus = &g_bus_api;

    bus_register("srv.input.kbd.poll", 1, (void *)svc_kbd_poll);
    bus_register("srv.input.mouse.poll", 1, (void *)svc_mouse_poll);
    bus_register("kern.config.get", 1, (void *)svc_config_get);
}

/* ============================== 键盘扫描码 ============================== */

static void kb_push(u8 b) {
    int nxt = (g_kbw + 1) % KBQ;
    if (nxt == g_kbr) return;
    g_kbq[g_kbw] = b;
    g_kbw = nxt;
}

static int vk_to_set1(WPARAM vk, int *e0) {
    *e0 = 0;
    switch (vk) {
    case VK_LEFT:   *e0 = 1; return 0x4B;
    case VK_RIGHT:  *e0 = 1; return 0x4D;
    case VK_UP:     *e0 = 1; return 0x48;
    case VK_DOWN:   *e0 = 1; return 0x50;
    case VK_DELETE: *e0 = 1; return 0x53;
    case VK_HOME:   *e0 = 1; return 0x47;
    case VK_END:    *e0 = 1; return 0x4F;
    case VK_PRIOR:  *e0 = 1; return 0x49;
    case VK_NEXT:   *e0 = 1; return 0x51;
    case VK_INSERT: *e0 = 1; return 0x52;
    case VK_RCONTROL:*e0 = 1; return 0x1D;
    case VK_RMENU:  *e0 = 1; return 0x38;
    default: break;
    }
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    return (int)(sc & 0xFF);
}

static void key_event(WPARAM vk, int down) {
    int e0 = 0;
    int sc = vk_to_set1(vk, &e0);
    if (!sc) return;
    if (e0) kb_push(0xE0);
    kb_push(down ? (u8)sc : (u8)(sc | 0x80));
}

/* ============================== 线程 ============================== */

static DWORD WINAPI desktop_thread(LPVOID param) {
    (void)param;
    host_log_direct("[host] desktop thread start");
    int rc = driver_init(&g_kapi, 0);
    host_log_direct(rc == 0 ? "[host] driver_init OK" : "[host] driver_init FAILED");
    if (rc == 0 && g_desktop_run) {
        InterlockedExchange(&g_desktop_started, 1);
        g_desktop_run();      /* 永不返回 */
    }
    return 0;
}

static DWORD WINAPI boot_thread(LPVOID param) {
    (void)param;
    /* 与 dev 镜像真实装载计划一致（串口 [boot] payload files/bytes 实测值）：
     * 54 个文件、1,105,533,649 字节（含 Linux rootfs）。 */
    const unsigned long long total_bytes = 1105533649ULL;
    const unsigned total_files = 54;
    struct stage { unsigned q; const char *label; unsigned end_ms; } st[] = {
        {  800, "SCANNING VOLUME",       300 },
        { 1400, "LOADING DRIVERS",       800 },
        { 4600, "LOADING DESKTOP ASSETS", 1450 },
        { 7000, "LOADING ZTS MODULES",   1950 },
        { 8200, "LOADING LINUX GUEST",   2700 },
        { 9000, "LOADING UTSM KERNEL",   3600 },
    };
    const int nst = (int)(sizeof(st) / sizeof(st[0]));

    demo_splash_begin();
    DWORD t0 = GetTickCount();
    for (;;) {
        DWORD el = GetTickCount() - t0;
        int k = 0;
        while (k < nst - 1 && el > st[k].end_ms) k++;
        unsigned long long loaded = total_bytes;
        unsigned files = total_files;
        unsigned q = st[k].q;
        if (el < st[nst - 1].end_ms) {
            loaded = total_bytes * (unsigned long long)el / st[nst - 1].end_ms;
            files = (unsigned)((unsigned long long)total_files * el / st[nst - 1].end_ms);
            q = 300u + (unsigned)((unsigned long long)8700u * el / st[nst - 1].end_ms);
        }
        demo_splash_load(q, st[k].label, loaded, total_bytes, files, total_files);
        if (el >= st[nst - 1].end_ms) break;
        Sleep(15);
    }
    demo_splash_finish();          /* 100% + 淡入深蓝 + 内核转圈图 */
    Sleep(900);                    /* 内核/驱动初始化窗口 */
    CreateThread(0, 0, desktop_thread, 0, 0, 0);
    return 0;
}

/* ============================== 呈现 ============================== */

static HFONT g_toast_font;

static void present(void) {
    HDC dc = GetDC(g_hwnd);
    BitBlt(dc, 0, 0, DW, DH, g_mdc, 0, 0, SRCCOPY);
    if (g_toast_until && (LONG)GetTickCount() < g_toast_until) {
        EnterCriticalSection(&g_log_lock);
        char text[256];
        strcpy(text, g_toast);
        LeaveCriticalSection(&g_log_lock);
        if (text[0]) {
            HFONT old = (HFONT)SelectObject(dc, g_toast_font);
            SetBkMode(dc, TRANSPARENT);
            wchar_t w[512];
            int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 512);
            SIZE sz = {0, 0};
            if (n > 0) GetTextExtentPoint32W(dc, w, n - 1, &sz);
            int tw = sz.cx + 32, th = 30;
            int x = (DW - tw) / 2, y = DH - 110;
            HBRUSH br = CreateSolidBrush(RGB(0x2B, 0x2B, 0x2F));
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x4A, 0x4A, 0x50));
            HBRUSH ob = (HBRUSH)SelectObject(dc, br);
            HPEN op = (HPEN)SelectObject(dc, pen);
            RoundRect(dc, x, y, x + tw, y + th, 12, 12);
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(br); DeleteObject(pen);
            SetTextColor(dc, RGB(0xF2, 0xF2, 0xF4));
            if (n > 0) TextOutW(dc, x + 16, y + 6, w, n - 1);
            SelectObject(dc, old);
        }
    }
    ReleaseDC(g_hwnd, dc);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(0);          /* 桌面自己绘制 24x24 光标 */
        return TRUE;
    case WM_MOUSEMOVE:
        EnterCriticalSection(&g_mouse_lock);
        g_mx = (int)(short)LOWORD(lp);
        g_my = (int)(short)HIWORD(lp);
        g_mpkt++;
        LeaveCriticalSection(&g_mouse_lock);
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
        int down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
        int bit = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 1 :
                  (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 2 : 4;
        EnterCriticalSection(&g_mouse_lock);
        if (down) g_mbtn |= bit; else g_mbtn &= ~bit;
        g_mx = (int)(short)LOWORD(lp);
        g_my = (int)(short)HIWORD(lp);
        g_mpkt++;
        LeaveCriticalSection(&g_mouse_lock);
        SetCapture(hwnd);
        if (!down) ReleaseCapture();
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        key_event(wp, 1);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        key_event(wp, 0);
        return 0;
    case WM_CLOSE:
        InterlockedExchange(&g_host_quit, 1);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ============================== 启动 ============================== */

static int reserve_fixed_region(void) {
    /* ADDR_TERM1 0x4000000 ... GLASS_ADDR 0x9100000 + 模糊缓存；预留 96MiB */
    void *p = VirtualAlloc((void *)(uintptr_t)0x04000000, 0x06000000,
                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p) return 0;
    p = VirtualAlloc(0, 0x06000000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return -1;
    host_log_direct("[host] WARN fixed region unavailable; fallback allocation not identity-based");
    return -2;
}

static int load_esp_image(void) {
    HRSRC r = FindResourceA(g_inst, MAKEINTRESOURCEA(RES_ESP_IMG), RT_RCDATA);
    if (!r) return -1;
    HGLOBAL h = LoadResource(g_inst, r);
    if (!h) return -1;
    DWORD sz = SizeofResource(g_inst, r);
    g_img_size = sz;
    g_img = (u8 *)malloc(sz);
    if (!g_img) return -1;
    memcpy(g_img, LockResource(h), sz);
    return 0;
}

static void gfx_init(void) {
    HDC screen = GetDC(0);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = DW;
    bi.bmiHeader.biHeight = -DH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&g_px, 0, 0);
    g_mdc = CreateCompatibleDC(screen);
    SelectObject(g_mdc, g_bmp);
    ReleaseDC(0, screen);
    g_toast_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev; (void)cmd;
    g_inst = hInst;
    SetProcessDPIAware();
    InitializeCriticalSection(&g_log_lock);
    InitializeCriticalSection(&g_mouse_lock);
    InitializeCriticalSection(&g_bus_lock);
    host_log_open();

    if (reserve_fixed_region() != 0) host_log_direct("[host] fixed region reserve failed");
    gfx_init();
    if (!g_px) return 1;
    if (load_esp_image() != 0) {
        MessageBoxA(0, "内嵌 FAT32 卷缺失（DEMO-E01）", "DeshabOS Demo", MB_ICONERROR);
        return 1;
    }
    build_fake_api();
    demo_splash_set_fb(g_px, DW, DH, PITCH);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = 0;
    wc.lpszClassName = L"DeshabDemoWnd";
    wc.hIcon = LoadIconA(hInst, MAKEINTRESOURCEA(200));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    RECT rc = {0, 0, DW, DH};
    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    RECT work;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    int wx = work.left + ((work.right - work.left) - ww) / 2;
    int wy = work.top + ((work.bottom - work.top) - wh) / 2;

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"DeshabOS Demo — 直接运行，无需安装",
                             style, wx, wy, ww, wh, 0, 0, hInst, 0);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd, show ? SW_SHOW : SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    SetFocus(g_hwnd);

    CreateThread(0, 0, boot_thread, 0, 0, 0);

    DWORD last = 0;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_host_quit) goto done;
        DWORD now = GetTickCount();
        if (now - last >= 15) {
            last = now;
            present();
        }
        Sleep(4);
    }
done:
    ExitProcess(0);
    return 0;
}
