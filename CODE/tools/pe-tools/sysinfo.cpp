/* sysinfo.cpp — Deshab 系统信息（GUI PE64 工具）
 *
 * 显示：CPUID 厂商/型号串/核心数、TSC 频率、RTC 实时时钟、
 *       开机时长（TSC）、宿主 surface 分辨率、PE 兼容层标识。
 * Esc 关闭（shim 合成 WM_CLOSE）。
 */
#include "pe_tool.h"

static char g_vendor[16];
static char g_brand[52];
static u32  g_cores;
static int  g_hv;
static u64  g_tsc_hz;

static void probe(void) {
    /* vendor */
    u32 a, b, c, d;
    pt_cpuid(0, 0, &a, &b, &c, &d);
    ((u32 *)g_vendor)[0] = b;
    ((u32 *)g_vendor)[1] = d;
    ((u32 *)g_vendor)[2] = c;
    g_vendor[12] = 0;

    /* brand string */
    if (a >= 0x80000004u) {
        for (u32 leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
            pt_cpuid(leaf, 0, &a, &b, &c, &d);
            int off = (int)(leaf - 0x80000002u) * 16;
            ((u32 *)g_brand)[off / 4 + 0] = a;
            ((u32 *)g_brand)[off / 4 + 1] = b;
            ((u32 *)g_brand)[off / 4 + 2] = c;
            ((u32 *)g_brand)[off / 4 + 3] = d;
        }
        g_brand[48] = 0;
    } else {
        g_brand[0] = 0;
    }

    pt_cpuid(1, 0, &a, &b, &c, &d);
    g_cores = (b >> 16) & 0xFF;
    g_hv = (int)((c >> 31) & 1);

    LARGE_INTEGER f;
    f.QuadPart = 0;
    QueryPerformanceFrequency(&f);
    g_tsc_hz = (u64)f.QuadPart;
}

/* 画一行 label: value，返回下一行 y */
static int row(PT_Canvas *cv, int y, const char *label, const char *value) {
    pt_text(cv, label, 28, y, PT_TEXT2);
    pt_text(cv, value, 170, y, PT_TEXT);
    return y + 30;
}

static void frame(PT_Canvas *cv) {
    pt_fill(cv, 0, 0, cv->w, cv->h, PT_BG);

    /* 顶部标题卡 */
    pt_round_fill(cv, 16, 16, cv->w - 32, 64, PT_CARD, 8);
    pt_text(cv, "Deshab SysInfo", 32, 22, PT_TEXT);
    pt_text(cv, "PE compat layer / native Ring0", 32, 44, PT_TEXT_DIM);

    /* 信息卡 */
    int ch = 0;
    int rows = 8;
    ch = 16 + rows * 30 + 24;
    pt_round_fill(cv, 16, 92, cv->w - 32, ch, PT_CARD, 8);

    int y = 108;
    y = row(cv, y, "CPU vendor", g_vendor);
    y = row(cv, y, "CPU brand", g_brand[0] ? g_brand : "(not supported)");
    {
        char b[16];
        pt_dec(g_cores, b);
        row(cv, y, "CPU cores (max APIC)", b);
        y += 30;
    }
    y = row(cv, y, "Hypervisor", g_hv ? "yes" : "no");
    {
        char b[24];
        u32 mhz = g_tsc_hz ? (u32)(g_tsc_hz / 1000000u) : 0;
        pt_dec(mhz, b);
        int n = 0;
        while (b[n]) n++;
        b[n] = ' '; b[n+1] = 'M'; b[n+2] = 'H'; b[n+3] = 'z'; b[n+4] = 0;
        y = row(cv, y, "TSC frequency", b);
    }
    {
        PT_Time t = pt_rtc_now();
        char b[16];
        pt_fmt_date(t, b);
        int n = 0;
        while (b[n]) n++;
        b[n] = ' ';
        pt_fmt_time(t, b + n + 1);
        y = row(cv, y, "RTC now", b);
    }
    {
        char b[16];
        pt_dec(GetTickCount() / 1000, b);
        int n = 0;
        while (b[n]) n++;
        b[n] = ' '; b[n+1] = 's'; b[n+2] = 0;
        y = row(cv, y, "Uptime (boot)", b);
    }
    {
        char b[16];
        int n = pt_dec((u32)cv->w, b);
        b[n] = 'x';
        pt_dec((u32)cv->h, b + n + 1);
        y = row(cv, y, "Surface", b);
    }

    /* 底部提示 */
    pt_text(cv, "Esc = close", 16, cv->h - 26, PT_TEXT_DIM);
}

int main(void) {
    probe();
    PT_Desc d;
    d.w = 560; d.h = 460;
    d.title = L"SysInfo";
    d.frame = frame;
    d.key = 0;
    d.mouse = 0;
    return pt_run(&d);
}
