/* ===========================================================================
 *  Nutro+OS - kernel entry / start-up
 *     1. take over the boot information handed over by stage 2
 *     2. set up the heap, interrupts, timer, keyboard, mouse and disks
 *     3. show the boot splash
 *     4. hand control to the window manager
 * ===========================================================================
 */
#include "kernel.h"

static boot_info_t binfo;

const boot_info_t *boot_info_get(void) { return &binfo; }

u32 cpu_vendor(char *out13)
{
    u32 eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(u32 *)(out13 + 0) = ebx;
    *(u32 *)(out13 + 4) = edx;
    *(u32 *)(out13 + 8) = ecx;
    out13[12] = 0;
    return eax;
}

const char *kernel_build_date(void) { return __DATE__ " " __TIME__; }

/* ---------------------------------------------------------- app registry -- */
app_def_t g_apps[APP_COUNT_MAX];
int       g_app_count = 0;

static void register_apps(void)
{
    int i = 0;
    g_apps[i].name = "Terminal"; g_apps[i].open = app_terminal_open;       g_apps[i].accent = COL_ACCENT_GREEN;  i++;
    g_apps[i].name = "Files";    g_apps[i].open = app_files_open;          g_apps[i].accent = COL_ACCENT_ORANGE; i++;
    g_apps[i].name = "Clock";    g_apps[i].open = app_clock_open;          g_apps[i].accent = 0x6FB7FF;          i++;
    g_apps[i].name = "Calc";     g_apps[i].open = app_calc_open;           g_apps[i].accent = 0x4FE3D0;          i++;
    g_apps[i].name = "Snakes";   g_apps[i].open = app_snake_open;          g_apps[i].accent = 0x8CE07A;          i++;
    g_apps[i].name = "Mines";    g_apps[i].open = app_mines_open;          g_apps[i].accent = 0xFF8A5C;          i++;
    g_apps[i].name = "Music";    g_apps[i].open = app_music_open;          g_apps[i].accent = COL_ACCENT_PURPLE; i++;
    g_apps[i].name = "Sysmon";   g_apps[i].open = app_sysmon_open;         g_apps[i].accent = 0x54D98C;          i++;
    g_apps[i].name = "Paint";    g_apps[i].open = app_paint_open;          g_apps[i].accent = COL_ACCENT_PINK;   i++;
    g_apps[i].name = "Viewer";   g_apps[i].open = app_viewer_open_default; g_apps[i].accent = COL_ACCENT;        i++;
    g_apps[i].name = "About";    g_apps[i].open = app_about_open;          g_apps[i].accent = COL_TEXT_DIM;      i++;
    g_app_count = i;
}

/* ---------------------------------------------------------------- splash -- */
static void splash(void)
{
    /* same gradient as the desktop wallpaper so the transition is seamless */
    fb_set_target(fb_mem);
    int stops[4] = { COL_WALL_0, COL_WALL_1, COL_WALL_2, COL_WALL_3 };
    for (int y = 0; y < (int)fb_height; y++) {
        int seg = (y * 3) / (int)fb_height;
        if (seg > 2) seg = 2;
        u32 a = (u32)stops[seg], b = (u32)stops[seg + 1];
        int f = (y * 3) % (int)fb_height;
        u32 c = ((u32)(((a >> 16) & 0xFF) + (int)(((b >> 16) & 0xFF) - ((a >> 16) & 0xFF)) * f / (int)fb_height) << 16) |
                ((u32)(((a >> 8) & 0xFF) + (int)(((b >> 8) & 0xFF) - ((a >> 8) & 0xFF)) * f / (int)fb_height) << 8) |
                (u32)((a & 0xFF) + (int)((b & 0xFF) - (a & 0xFF)) * f / (int)fb_height);
        fill_rect(0, y, (int)fb_width, 1, c);
    }

    int cx = (int)fb_width / 2;
    int cy = (int)fb_height / 2;

    const char *title = "Nutro+OS";
    int tw = (int)strlen(title) * 8 * 5;
    draw_text_scaled(cx - tw / 2, cy - 100, title, 0xFFFFFF, 5);

    const char *ver = "version " NUTRO_VERSION "   -   32-bit protected mode   -   VESA   -   FAT32";
    int sw = (int)strlen(ver) * 8;
    draw_text(cx - sw / 2, cy - 26, ver, 0xE9ECF6);

    int bw = 420, bh = 10;
    int bx = cx - bw / 2, by = cy + 26;
    fill_round_rect(bx, by, bw, bh, 5, 0xFFFFFF);

    char line[96];
    ksnprintf(line, sizeof(line), "kernel %u KiB   %ux%u x %u bpp   %s",
              binfo.kernel_bytes / 1024, fb_width, fb_height, fb_bpp, kernel_build_date());
    int lw = (int)strlen(line) * 8;
    draw_text(cx - lw / 2, by + 34, line, 0xD8DDEE);

    const char *steps[4] = { "heap ready", "interrupts online", "drivers loaded", "starting desktop" };
    for (int i = 0; i < 4; i++) {
        int w = bw * (i + 1) / 4;
        fill_round_rect(bx, by, w, bh, 5, 0x0A84FF);
        const char *s = steps[i];
        int pw = (int)strlen(s) * 8;
        draw_text(cx - pw / 2, by + 52, s, 0xFFFFFF);
        timer_sleep_ms(240);
    }
    debug_write("kernel: splash done\n");
}

/* ------------------------------------------------------------------ main -- */
void kernel_main(boot_info_t *bi)
{
    /* reset the in-memory debug log (see string.c) */
    *(volatile u32 *)(0x50000u + 4096u) = 0;

    debug_write("Nutro+OS 1.0 kernel starting\n");
    memcpy(&binfo, bi, sizeof(boot_info_t));

    fb_init(&binfo);
    debug_printf("fb %ux%u %u bpp pitch %u at %x  (font=%u)\n",
                 fb_width, fb_height, fb_bpp, fb_pitch, binfo.fb_addr, binfo.font_valid);

    heap_init(&binfo);
    debug_printf("heap %u KiB at %x\n", heap_size() / 1024, (u32)(0));

    idt_init();
    interrupts_enable();
    timer_init(100);
    keyboard_init();

    rtc_time_t now;
    rtc_read(&now);
    debug_printf("rtc %04d-%02d-%02d %02d:%02d:%02d\n",
                 now.year, now.month, now.day, now.hour, now.minute, now.second);

    ata_init();
    mouse_init();

    splash();

    register_apps();            /* the wallpaper bakes the desktop icons in */
    gui_init();

    /* open a terminal so the desktop immediately shows something useful */
    gui_open_app(0);

    debug_write("kernel: entering the window manager\n");
    gui_run();
    for (;;) cpu_halt();
}