/* boot_splash.c — 直接编译真实引导界面 CODE/boot/splash.c
 *
 * splash.c 原本由 MEDIA/CODE/boot/main.c #include，依赖 EFI 环境。
 * 本文件提供最小宿主垫片（端口 I/O / rdtsc / EFI Stall / 内存分配 / 日志），
 * 让同一份齿轮 + 铆钉 + 40 段进度条代码在 Windows 进程里逐像素原样运行。
 *
 * 不修改 splash.c 源码；所有绘制参数/动画节奏均来自原文件。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          s64;
typedef unsigned long long usize;
typedef unsigned int       bool32;

/* ---- 端口 I/O / EFI 垫片 ---- */
static void outb(u16 port, u8 value) { (void)port; (void)value; }
static u8 inb(u16 port) {
    /* PIT OUT2 状态位：直接报告已到计数，逼校准走 Stall 回退路径 */
    if (port == 0x61) return 0x20;
    return 0;
}
static void spl_host_stall(unsigned long microseconds) {
    Sleep((DWORD)(microseconds / 1000));
}

typedef struct { void (*Stall)(unsigned long); } spl_efi_bs;
static spl_efi_bs g_spl_bs = { spl_host_stall };
#define BS (&g_spl_bs)

static void *alloc_pages(u64 bytes) {
    return VirtualAlloc(0, (SIZE_T)bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void log2(const char *a, const void *b) { (void)a; (void)b; }
static void log_hex(const char *a, u64 b) { (void)a; (void)b; }
static void log_hex2(const char *a, u64 b) { (void)a; (void)b; }
static int  str_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void str_copy(char *d, const char *s, int cap) {
    int i = 0;
    for (; i < cap - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

/* ---- 帧缓冲（宿主 DIB） ---- */
static u8  *g_fb;
static u64  g_fb_w, g_fb_h, g_fb_pitch;
static u32  g_fb_rsh = 16, g_fb_gsh = 8, g_fb_bsh = 0;

/* 原文件（#include 后其 static 变量在本编译单元可见） */
#include "../../../CODE/boot/splash.c"

/* ---- 供宿主调用的薄接口 ---- */
void demo_splash_set_fb(void *fb, unsigned w, unsigned h, unsigned pitch) {
    g_fb = (u8 *)fb;
    g_fb_w = w; g_fb_h = h; g_fb_pitch = pitch;
}

void demo_splash_begin(void) {
    splash_begin();
}

void demo_splash_load(unsigned q, const char *stage,
                      unsigned long long loaded, unsigned long long total,
                      unsigned files_done, unsigned files_total) {
    g_bytes_loaded = loaded;
    g_bytes_total = total;
    g_files_done = files_done;
    g_files_total = files_total;
    splash_phase(q, stage);
}

void demo_splash_finish(void) {
    splash_finish();
}
