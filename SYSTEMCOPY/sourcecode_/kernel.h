/* ===========================================================================
 *  Nutro+OS - kernel common header
 *  32-bit protected mode, flat segmentation, VESA framebuffer GUI.
 * ===========================================================================
 */
#ifndef NUTRO_KERNEL_H
#define NUTRO_KERNEL_H

/* ---------------------------------------------------------------- types -- */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        s8;
typedef signed short       s16;
typedef signed int         s32;
typedef signed long long   s64;

#define NULL ((void *)0)
#define PACKED __attribute__((packed))
#define UNUSED(x) ((void)(x))

/* --------------------------------------------------------- boot info ------ */
typedef struct PACKED {
    u32 magic;          /* "NTR+" */
    u32 fb_addr;        /* linear framebuffer physical address */
    u32 fb_width;
    u32 fb_height;
    u32 fb_pitch;       /* bytes per scanline */
    u32 fb_bpp;
    u32 red_size, red_pos;
    u32 green_size, green_pos;
    u32 blue_size, blue_pos;
    u32 e820_count;
    u32 e820_addr;
    u32 part_lba;       /* FAT32 partition start LBA */
    u32 kernel_bytes;
    u32 boot_drive;
    u32 font_addr;      /* BIOS 8x16 font copy (256 glyphs) */
    u32 font_valid;
} boot_info_t;

const boot_info_t *boot_info_get(void);

typedef struct PACKED {
    u64 base;
    u64 length;
    u32 type;           /* 1 = usable RAM */
    u32 acpi;
} e820_entry_t;

/* ------------------------------------------------------------- port I/O --- */
static inline void outb(u16 port, u8 val)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline void outw(u16 port, u16 val)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline u8 inb(u16 port)
{
    u8 r;
    __asm__ __volatile__("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline u16 inw(u16 port)
{
    u16 r;
    __asm__ __volatile__("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void io_wait(void) { outb(0x80, 0); }
static inline void cpu_halt(void) { __asm__ __volatile__("hlt"); }
static inline void interrupts_enable(void) { __asm__ __volatile__("sti"); }
static inline void interrupts_disable(void) { __asm__ __volatile__("cli"); }

/* -------------------------------------------------- debug console (0xE9) -- */
void debug_putc(char c);
void debug_write(const char *s);
void debug_printf(const char *fmt, ...);

/* -------------------------------------------------------------- strings --- */
void  *memset(void *dst, int c, u32 n);
void  *memcpy(void *dst, const void *src, u32 n);
void  *memmove(void *dst, const void *src, u32 n);
int    memcmp(const void *a, const void *b, u32 n);
u32    strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, u32 n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, u32 n);
char  *strcat(char *dst, const char *src);
void   itoa(s32 v, char *buf);
void   utoa(u32 v, char *buf);
void   utohex(u32 v, char *buf);
int    ksnprintf(char *buf, int size, const char *fmt, ...);

/* --------------------------------------------------------------- memory --- */
void  heap_init(const boot_info_t *bi);
void *kmalloc(u32 size);
void *kmalloc_aligned(u32 size, u32 align);
u32   heap_used(void);
u32   heap_size(void);
u32   mem_total_kb(const boot_info_t *bi);
u32   mem_usable_kb(const boot_info_t *bi);

/* ------------------------------------------------------------ interrupt --- */
void idt_init(void);
void irq_install(int irq, void (*handler)(void));
void irq_mask(int irq);
void irq_unmask(int irq);
u32  irq_disable_save(void);
void irq_restore(u32 flags);

/* --------------------------------------------------------------- timer ---- */
void timer_init(u32 hz);
u32  timer_ticks(void);
u32  timer_ms(void);
void timer_sleep_ms(u32 ms);

/* ------------------------------------------------------------ keyboard ---- */
#define KEY_UP      0x90
#define KEY_DOWN    0x91
#define KEY_LEFT    0x92
#define KEY_RIGHT   0x93
#define KEY_F1      0x94
#define KEY_F2      0x95
#define KEY_F3      0x96
#define KEY_F4      0x97
#define KEY_ESC     0x98

typedef struct {
    u8  ch;         /* ASCII or KEY_* value */
    u8  pressed;    /* 1 = press, 0 = release */
    u8  shift;
} key_event_t;

void keyboard_init(void);
int  keyboard_get_event(key_event_t *ev);

/* --------------------------------------------------------------- mouse ---- */
typedef struct {
    s16 dx, dy;
    u8  buttons;    /* change event: buttons after change */
    u8  is_button;
} mouse_event_t;

void mouse_init(void);
int  mouse_get_event(mouse_event_t *ev);
void mouse_get_pos(int *x, int *y);

/* ----------------------------------------------------------------- rtc ---- */
typedef struct {
    int year, month, day, hour, minute, second;
} rtc_time_t;

void rtc_read(rtc_time_t *t);

/* ----------------------------------------------------------------- ata ---- */
int ata_init(void);
int ata_read_sectors(u32 lba, u32 count, void *buf);
int ata_write_sectors(u32 lba, u32 count, const void *buf);
int ata_present(void);

/* --------------------------------------------------------------- fat32 ---- */
typedef struct {
    char name[64];      /* display name (8.3 with a dot) */
    u8   attr;
    int  is_dir;
    u32  size;
    u32  first_cluster;
} fat_entry_t;

typedef struct {
    int  valid;
    u32  part_lba;
    u32  spc;           /* sectors per cluster */
    u32  reserved;
    u32  nfats;
    u32  fat_size;      /* sectors per FAT */
    u32  root_cluster;
    u32  fat_lba;
    u32  data_lba;
    u32  cluster_bytes;
    u32  total_clusters;
} fat32_t;

int  fat32_mount(fat32_t *fs, u32 part_lba);
int  fat32_list(fat32_t *fs, u32 dir_cluster, fat_entry_t *out, int max);
u32  fat32_find_dir(fat32_t *fs, u32 dir_cluster, const char *name);
int  fat32_read_file(fat32_t *fs, u32 cluster, u32 size, void *buf, u32 max);
int  fat32_next_cluster(fat32_t *fs, u32 cluster);

/* ------------------------------------------------------------- graphics --- */
extern u32  fb_width, fb_height, fb_pitch, fb_bpp;
extern u8  *fb_mem;
extern u8  *fb_back;        /* back buffer (may be NULL before gui init) */

void fb_init(const boot_info_t *bi);
void fb_set_target(u8 *target);
void fb_set_target2(u8 *target, u32 pitch);
u8  *fb_get_target(void);
u32  fb_get_pitch(void);
void fb_blit_buffer(u8 *dst, u32 dst_pitch, int dx, int dy,
                    const u8 *src, u32 src_pitch, int w, int h);
void fb_blit_scaled(u8 *dst, u32 dst_pitch, int dx, int dy, int dw, int dh,
                    const u8 *src, u32 src_pitch, int sw, int sh);
void fb_set_clip(int x, int y, int w, int h);
void fb_reset_clip(void);
void fb_present(void);
u32  pack_color(u32 rgb);
u32  fb_get_pixel(int x, int y);
void put_pixel(int x, int y, u32 rgb);
void fill_rect(int x, int y, int w, int h, u32 rgb);
void fill_rect_blend(int x, int y, int w, int h, u32 rgb, u32 alpha);
void fill_hline(int x, int y, int w, u32 rgb);
void fill_vline(int x, int y, int h, u32 rgb);
void draw_rect(int x, int y, int w, int h, u32 rgb);
void fill_round_rect(int x, int y, int w, int h, int radius, u32 rgb);
void draw_round_rect(int x, int y, int w, int h, int radius, u32 rgb);
void fill_gradient_v(int x, int y, int w, int h, u32 top, u32 bottom);
void fill_circle(int cx, int cy, int r, u32 rgb);
void draw_circle(int cx, int cy, int r, u32 rgb, int thickness);
int  fsin_deg(int deg);
int  fcos_deg(int deg);
void draw_line(int x0, int y0, int x1, int y1, u32 rgb, int thickness);
void draw_char(int x, int y, char c, u32 rgb);
void draw_char_bold(int x, int y, char c, u32 rgb);
void draw_text(int x, int y, const char *s, u32 rgb);
void draw_text_bold(int x, int y, const char *s, u32 rgb);
void draw_char_scaled(int x, int y, char c, u32 rgb, int scale);
void draw_text_scaled(int x, int y, const char *s, u32 rgb, int scale);
int  text_width(const char *s);
void blit_u32(int dx, int dy, int w, int h, const u32 *src, u32 key);
void blit_u32_clip(int dx, int dy, int w, int h, const u32 *src, u32 key,
                   int cx, int cy, int cw, int ch);
void fb_copy_rect(u8 *dst, int dx, int dy, int w, int h, const u8 *src, int sx, int sy);
void fb_fill_screen(u32 rgb);

/* ----------------------------------------------------------------- gui ---- */
#define GUI_MAX_WINDOWS 16

typedef struct window window_t;

enum {
    EV_KEY = 1,          /* a = char (or KEY_*), b = pressed */
    EV_MOUSE_DOWN,       /* a = x, b = y, c = button */
    EV_MOUSE_UP,
    EV_MOUSE_MOVE,       /* a = x, b = y, c = buttons */
    EV_MOUSE_DBL,        /* double click */
    EV_CLOSE
};

struct window {
    int   x, y, w, h;
    char  title[48];
    u8    visible;
    u8    closable;
    u8    anim;             /* 0 = static, 1 = 1 Hz, 2 = 20 Hz redraw */
    u8    minimized;
    u8    zoomed;
    int   sx, sy, sw, sh;   /* saved geometry for the zoom toggle */
    u8   *cache;            /* content bitmap: w x (h - TITLEBAR_H) */
    u8    cache_dirty;
    /* transition animation (macOS style open/close/zoom/minimise) */
    u8    an_kind;          /* 0 none, 1 scale in/out, 2 rect lerp, 3 fly to dock */
    u8    an_flags;         /* 1 = finish by closing, 2 = finish by minimising */
    u8    dying;            /* drawn but no longer interactive */
    u32   an_start, an_dur;
    s16   an_fx, an_fy, an_fw, an_fh;
    s16   an_lx, an_ly, an_lw, an_lh;   /* rect used for the previous frame */
    int   app_id;           /* index into g_apps (-1 when opened by another app) */
    int   id;
    void (*draw)(window_t *w);
    int  (*event)(window_t *w, int type, int a, int b, int c);
    void *user;
    window_t *next;
    window_t *prev;
};

#define TITLEBAR_H 26
#define TASKBAR_H  38

void  gui_init(void);
void  gui_run(void);
void  gui_invalidate(void);
void  gui_invalidate_rect(int x, int y, int w, int h);
int   gui_mouse_over(int x, int y, int w, int h);
int   gui_window_count(void);
window_t *win_create(const char *title, int x, int y, int w, int h,
                     void (*draw)(window_t *), int (*event)(window_t *, int, int, int, int),
                     void *user);
void  win_close(window_t *w);
void  win_focus(window_t *w);
window_t *win_focused(void);
int   win_content_x(window_t *w);      /* drawing origin (0 while caching) */
int   win_content_y(window_t *w);
int   win_abs_x(window_t *w);          /* always the on-screen position */
int   win_abs_content_y(window_t *w);
void  win_set_dirty_content(window_t *w);
void  gui_open_app(int index);

/* ---------------------------------------------------------------- apps ---- */
typedef struct {
    const char *name;
    window_t  *(*open)(void);
    u32        accent;
} app_def_t;

#define APP_COUNT_MAX 12
extern app_def_t g_apps[APP_COUNT_MAX];
extern int       g_app_count;

window_t *app_terminal_open(void);
window_t *app_files_open(void);
window_t *app_viewer_open_default(void);
window_t *app_about_open(void);
window_t *app_paint_open(void);
window_t *viewer_open_text(const char *title, const char *text, u32 len);
window_t *app_clock_open(void);
window_t *app_calc_open(void);
window_t *app_snake_open(void);
window_t *app_mines_open(void);
window_t *app_music_open(void);
window_t *app_sysmon_open(void);

/* debug console counters (gui performance) */
void gui_perf(int *frames, int *busy_ms);
int  gui_mouse_buttons(void);

/* ------------------------------------------- SwiftUI style control kit ---- */
#define UI_FILLED    0
#define UI_TINTED    1
#define UI_PLAIN     2
#define UI_BORDERED  3
#define UI_DESTRUCT  4

int  ui_hit(int x, int y, int w, int h);
void ui_button(int x, int y, int w, int h, const char *label, int style);
void ui_toggle(int x, int y, int on);
void ui_checkbox(int x, int y, const char *label, int checked);
void ui_slider(int x, int y, int w, int value, int mn, int mx);
int  ui_slider_value(int x, int w, int mn, int mx, int mouse_x);
void ui_segment(int x, int y, int w, int h, const char *const *items, int n, int sel);
void ui_progress(int x, int y, int w, int h, int pct, u32 color);
void ui_row_bg(int x, int y, int w, int h, int selected, int hovered);
void ui_section(int x, int y, const char *title);

/* ------------------------------------------------- macOS 13 style theme --- */
#define NUTRO_VERSION       "1.1"
#define COL_WALL_0          0x121638      /* wallpaper gradient stops */
#define COL_WALL_1          0x3A1E63
#define COL_WALL_2          0x8E2E6B
#define COL_WALL_3          0xE0563F
#define COL_MENUBAR         0xEDEFF5
#define COL_MENUBAR_TEXT    0x1D1D22
#define COL_DOCK            0xE7E9F0
#define COL_WIN_BG          0xFAFBFD
#define COL_WIN_CONTENT     0xFFFFFF
#define COL_TITLE_ACTIVE    0xE7E9F0
#define COL_TITLE_INACTIVE  0xF1F2F6
#define COL_TEXT            0x1D1D22
#define COL_TEXT_DIM        0x74777F
#define COL_ACCENT          0x0A84FF
#define COL_ACCENT_GREEN    0x1FA34A
#define COL_ACCENT_ORANGE   0xE07C1F
#define COL_ACCENT_PINK     0xD6408C
#define COL_ACCENT_PURPLE   0x7A4FE0
#define COL_CLOSE           0xFF5F57
#define COL_MIN             0xFEBC2E
#define COL_ZOOM            0x28C840
#define COL_BORDER          0xC7CBD5
#define COL_SELECT          0xCFE5FF

void kmain_early_splash(void);
u32  cpu_vendor(char *out13);
const char *kernel_build_date(void);

#endif /* NUTRO_KERNEL_H */void fill_round_rect_blend(int x, int y, int w, int h, int radius, u32 rgb, int alpha);
