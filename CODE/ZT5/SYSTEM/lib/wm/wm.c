/*
 * wm.c - ZratroOS Window Manager (zrdemage)
 *
 * Win11 + KDE Plasma fusion style window manager
 * Features:
 *   - Acrylic/Mica style translucent effects
 *   - Rounded corners with shadow
 *   - Snap layouts (Win11 style)
 *   - KDE-style window decorations
 *   - Compositor with double buffering
 *   - IPC with desktop and file manager
 *
 * Compiles to: SYSTEM/system/zrdemage
 */

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;
typedef int                int32_t;
#define NULL ((void*)0)

/* ========== System calls ========== */
#define SYS_EXIT        0
#define SYS_READ        1
#define SYS_WRITE       2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_MMAP        11
#define SYS_MUNMAP      12
#define SYS_OPENDIR     20
#define SYS_READDIR     21
#define SYS_CLOSEDIR    22
#define SYS_STAT        6
#define SYS_TIME        114
#define SYS_FB_SYNC     155
#define SYS_GETFBINFO   156
#define SYS_INPUT_POLL  188
#define SYS_EXEC        7
#define SYS_FORK        23
#define SYS_GETPID      10
#define SYS_WAITPID     24

/* Keycodes */
#define KEY_ESC         1
#define KEY_ENTER       28
#define KEY_BACKSPACE   14
#define KEY_LEFTSHIFT   42
#define KEY_RIGHTSHIFT  54
#define KEY_LEFTCTRL    29
#define KEY_TAB         15
#define KEY_F11         87

/* Syscall wrappers */
static inline long sc1(long n, long a0) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a0) : "rcx", "r11", "memory");
    return r;
}
static inline long sc2(long n, long a0, long a1) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a0), "S"(a1) : "rcx", "r11", "memory");
    return r;
}
static inline long sc3(long n, long a0, long a1, long a2) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a0), "S"(a1), "d"(a2) : "rcx", "r11", "memory");
    return r;
}

static long sys_write(int fd, const void *buf, long count) {
    extern long sys_write_asm(int fd, const void *buf, long count);
    return sys_write_asm(fd, buf, count);
}
static long sys_open(const char *path, int flags) { return sc2(SYS_OPEN, (long)path, (long)flags); }
static long sys_read(int fd, void *buf, long count) { return sc3(SYS_READ, (long)fd, (long)buf, (long)count); }
static long sys_close(int fd) { return sc1(SYS_CLOSE, (long)fd); }
static long sys_fb_sync(void *fb) { return sc1(SYS_FB_SYNC, (long)fb); }
static long sys_input_poll(uint32_t *keycodes, int max_keys) { return sc2(SYS_INPUT_POLL, (long)keycodes, (long)max_keys); }

/* Serial debug */
static inline unsigned char inb(unsigned short port) {
    unsigned char v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline void outb(unsigned char v, unsigned short port) {
    asm volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static void serial_putc(char c) { while ((inb(0x3FD) & 0x20) == 0); outb(c, 0x3F8); }
static void serial_puts(const char *s) { while (*s) serial_putc(*s++); }

/* ========== Framebuffer ========== */
__attribute__((section(".bss.fb_base"))) uint64_t fb_base;
__attribute__((section(".bss.fb_width"))) uint32_t fb_width;
__attribute__((section(".bss.fb_height"))) uint32_t fb_height;
__attribute__((section(".bss.fb_pitch"))) uint32_t fb_pitch;

static struct { uint64_t base; uint32_t w, h, pitch, format; } g_fbi;
static int g_fb_bgr = 0;

/* ========== Color System - Win11 + KDE Plasma Fusion ========== */
#define MKC_RGB(r,g,b) (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(b))
#define MKC_BGR(r,g,b) (((uint32_t)(b)<<16)|((uint32_t)(g)<<8)|(r))
#define MKC(r,g,b) (g_fb_bgr ? MKC_BGR(r,g,b) : MKC_RGB(r,g,b))

/* Mica/Acrylic palette - dark theme */
#define C_MICA_BG       MKC(32, 32, 38)       /* Mica base */
#define C_MICA_BG2      MKC(44, 44, 52)       /* Mica layer 2 */
#define C_MICA_LIGHT    MKC(55, 55, 65)       /* Mica highlight */
#define C_SURFACE       MKC(40, 40, 48)       /* Surface color */
#define C_SURFACE_VAR   MKC(49, 49, 57)       /* Surface variant */
#define C_ACCENT        MKC(96, 165, 250)     /* Win11 blue accent */
#define C_ACCENT_HOVER  MKC(118, 179, 252)    /* Accent hover */
#define C_ACCENT_PRESSED MKC(76, 145, 228)    /* Accent pressed */
#define C_ACCENT2       MKC(180, 130, 255)    /* KDE purple */
#define C_TEXT          MKC(235, 235, 242)     /* Primary text */
#define C_TEXT_SEC      MKC(154, 154, 170)     /* Secondary text */
#define C_TEXT_DIS      MKC(104, 104, 120)     /* Disabled text */
#define C_BORDER        MKC(62, 62, 72)        /* Border */
#define C_BORDER_LIGHT  MKC(78, 78, 90)        /* Light border */
#define C_TASKBAR_BG    MKC(24, 24, 30)        /* Taskbar (Mica) */
#define C_TASKBAR_HOVER MKC(48, 48, 58)        /* Taskbar hover */
#define C_SHADOW        MKC(10, 10, 14)        /* Window shadow */
#define C_DOCK_BG       MKC(28, 28, 36)        /* KDE dock */
#define C_RED           MKC(255, 85, 85)       /* Close button */
#define C_GREEN         MKC(80, 220, 120)      /* Maximize button */
#define C_YELLOW        MKC(255, 200, 60)      /* Minimize button */
#define C_ORANGE        MKC(247, 179, 112)     /* Folder icon */
#define C_WHITE         MKC(255, 255, 255)
#define C_BLACK         MKC(0, 0, 0)

/* ========== Memory Pool ========== */
static char g_pool[12 * 1024 * 1024];
static int g_pool_pos = 0;

static void *wm_alloc(int size) {
    size = (size + 15) & ~15;
    if (g_pool_pos + size > (int)sizeof(g_pool)) return 0;
    void *p = &g_pool[g_pool_pos];
    g_pool_pos += size;
    for (int i = 0; i < size; i++) ((char*)p)[i] = 0;
    return p;
}

/* ========== String utilities ========== */
static int d_strlen(const char *s) { int n=0; while(s[n])n++; return n; }
static void d_strcpy(char *d, const char *s) { while((*d++=*s++)); }
static void d_strncpy(char *d, const char *s, int n) {
    int i; for(i=0;i<n&&s[i];i++) d[i]=s[i]; for(;i<n;i++) d[i]=0;
}
static int d_strcmp(const char *a, const char *b) {
    while(*a&&*a==*b){a++;b++;} return *(unsigned char*)a-*(unsigned char*)b;
}

/* ========== 8x8 Font ========== */
static const uint8_t font8[128][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x5F,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x07,0x00,0x07,0x00,0x00,0x00},
    {0x00,0x14,0x7F,0x14,0x7F,0x14,0x00,0x00},
    {0x00,0x24,0x2A,0x7F,0x2A,0x12,0x00,0x00},
    {0x00,0x23,0x13,0x08,0x64,0x62,0x00,0x00},
    {0x00,0x36,0x49,0x55,0x22,0x50,0x00,0x00},
    {0x00,0x00,0x05,0x03,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1C,0x22,0x41,0x00,0x00,0x00},
    {0x00,0x00,0x41,0x22,0x1C,0x00,0x00,0x00},
    {0x00,0x14,0x08,0x3E,0x08,0x14,0x00,0x00},
    {0x00,0x08,0x08,0x3E,0x08,0x08,0x00,0x00},
    {0x00,0x00,0x50,0x30,0x00,0x00,0x00,0x00},
    {0x00,0x08,0x08,0x08,0x08,0x08,0x00,0x00},
    {0x00,0x00,0x60,0x60,0x00,0x00,0x00,0x00},
    {0x00,0x20,0x10,0x08,0x04,0x02,0x00,0x00},
    {0x00,0x3E,0x51,0x49,0x45,0x3E,0x00,0x00},
    {0x00,0x00,0x42,0x7F,0x40,0x00,0x00,0x00},
    {0x00,0x42,0x61,0x51,0x49,0x46,0x00,0x00},
    {0x00,0x21,0x41,0x45,0x4B,0x31,0x00,0x00},
    {0x00,0x18,0x14,0x12,0x7F,0x10,0x00,0x00},
    {0x00,0x27,0x45,0x45,0x45,0x39,0x00,0x00},
    {0x00,0x3C,0x4A,0x49,0x49,0x30,0x00,0x00},
    {0x00,0x01,0x71,0x09,0x05,0x03,0x00,0x00},
    {0x00,0x36,0x49,0x49,0x49,0x36,0x00,0x00},
    {0x00,0x06,0x49,0x49,0x29,0x1E,0x00,0x00},
    {0x00,0x00,0x36,0x36,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x56,0x36,0x00,0x00,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41,0x00,0x00,0x00},
    {0x00,0x14,0x14,0x14,0x14,0x14,0x00,0x00},
    {0x00,0x00,0x41,0x22,0x14,0x08,0x00,0x00},
    {0x00,0x02,0x01,0x51,0x09,0x06,0x00,0x00},
    {0x00,0x32,0x49,0x79,0x41,0x3E,0x00,0x00},
    {0x00,0x7E,0x11,0x11,0x11,0x7E,0x00,0x00},
    {0x00,0x7F,0x49,0x49,0x49,0x36,0x00,0x00},
    {0x00,0x3E,0x41,0x41,0x41,0x22,0x00,0x00},
    {0x00,0x7F,0x41,0x41,0x22,0x1C,0x00,0x00},
    {0x00,0x7F,0x49,0x49,0x49,0x41,0x00,0x00},
    {0x00,0x7F,0x09,0x09,0x09,0x01,0x00,0x00},
    {0x00,0x3E,0x41,0x49,0x49,0x7A,0x00,0x00},
    {0x00,0x7F,0x08,0x08,0x08,0x7F,0x00,0x00},
    {0x00,0x00,0x41,0x7F,0x41,0x00,0x00,0x00},
    {0x00,0x20,0x40,0x41,0x3F,0x01,0x00,0x00},
    {0x00,0x7F,0x08,0x14,0x22,0x41,0x00,0x00},
    {0x00,0x7F,0x40,0x40,0x40,0x40,0x00,0x00},
    {0x00,0x7F,0x02,0x0C,0x02,0x7F,0x00,0x00},
    {0x00,0x7F,0x04,0x08,0x10,0x7F,0x00,0x00},
    {0x00,0x3E,0x41,0x41,0x41,0x3E,0x00,0x00},
    {0x00,0x7F,0x09,0x09,0x09,0x06,0x00,0x00},
    {0x00,0x3E,0x41,0x51,0x21,0x5E,0x00,0x00},
    {0x00,0x7F,0x09,0x19,0x29,0x46,0x00,0x00},
    {0x00,0x46,0x49,0x49,0x49,0x31,0x00,0x00},
    {0x00,0x01,0x01,0x7F,0x01,0x01,0x00,0x00},
    {0x00,0x3F,0x40,0x40,0x40,0x3F,0x00,0x00},
    {0x00,0x1F,0x20,0x40,0x20,0x1F,0x00,0x00},
    {0x00,0x3F,0x40,0x38,0x40,0x3F,0x00,0x00},
    {0x00,0x63,0x14,0x08,0x14,0x63,0x00,0x00},
    {0x00,0x07,0x08,0x70,0x08,0x07,0x00,0x00},
    {0x00,0x61,0x51,0x49,0x45,0x43,0x00,0x00},
    {0x00,0x00,0x7F,0x41,0x41,0x00,0x00,0x00},
    {0x00,0x02,0x04,0x08,0x10,0x20,0x00,0x00},
    {0x00,0x41,0x41,0x7F,0x00,0x00,0x00,0x00},
    {0x00,0x04,0x02,0x01,0x02,0x04,0x00,0x00},
    {0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40},
    {0x00,0x00,0x01,0x02,0x04,0x00,0x00,0x00},
    {0x00,0x20,0x54,0x54,0x54,0x78,0x00,0x00},
    {0x00,0x7F,0x48,0x44,0x44,0x38,0x00,0x00},
    {0x00,0x38,0x44,0x44,0x44,0x20,0x00,0x00},
    {0x00,0x38,0x44,0x44,0x48,0x7F,0x00,0x00},
    {0x00,0x38,0x54,0x54,0x54,0x18,0x00,0x00},
    {0x00,0x08,0x7E,0x09,0x01,0x02,0x00,0x00},
    {0x00,0x18,0xA4,0xA4,0xA4,0x7C,0x00,0x00},
    {0x00,0x7F,0x08,0x04,0x04,0x78,0x00,0x00},
    {0x00,0x00,0x44,0x7D,0x40,0x00,0x00,0x00},
    {0x00,0x40,0x80,0x84,0x7D,0x00,0x00,0x00},
    {0x00,0x7F,0x10,0x28,0x44,0x00,0x00,0x00},
    {0x00,0x00,0x41,0x7F,0x40,0x00,0x00,0x00},
    {0x00,0x7C,0x04,0x18,0x04,0x78,0x00,0x00},
    {0x00,0x7C,0x08,0x04,0x04,0x78,0x00,0x00},
    {0x00,0x38,0x44,0x44,0x44,0x38,0x00,0x00},
    {0x00,0xFC,0x24,0x24,0x24,0x18,0x00,0x00},
    {0x00,0x18,0x24,0x24,0x18,0xFC,0x00,0x00},
    {0x00,0x7C,0x08,0x04,0x04,0x08,0x00,0x00},
    {0x00,0x48,0x54,0x54,0x54,0x20,0x00,0x00},
    {0x00,0x04,0x3F,0x44,0x40,0x20,0x00,0x00},
    {0x00,0x3C,0x40,0x40,0x20,0x7C,0x00,0x00},
    {0x00,0x1C,0x20,0x40,0x20,0x1C,0x00,0x00},
    {0x00,0x3C,0x40,0x30,0x40,0x3C,0x00,0x00},
    {0x00,0x44,0x28,0x10,0x28,0x44,0x00,0x00},
    {0x00,0x1C,0xA0,0xA0,0xA0,0x7C,0x00,0x00},
    {0x00,0x44,0x64,0x54,0x4C,0x44,0x00,0x00},
};

/* ========== Drawing Primitives ========== */
static uint32_t *g_fb;
static int g_w, g_h, g_stride;

static void fb_flip(void) {
    uint32_t *real_fb = (uint32_t*)(uint64_t)g_fbi.base;
    if (!real_fb) { sys_fb_sync(g_fb); return; }
    uint64_t total = (uint64_t)g_h * g_stride;
    for (uint64_t i = 0; i < total; i++) real_fb[i] = g_fb[i];
    sys_fb_sync(real_fb);
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (x >= g_w || y >= g_h || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_w ? g_w : x + w, y1 = y + h > g_h ? g_h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &g_fb[yy * g_stride + x0];
        for (int xx = x1 - x0; xx > 0; xx--) *row++ = color;
    }
}

static void fill_rect_vgrad(int x, int y, int w, int h, uint32_t c1, uint32_t c2) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_w ? g_w : x + w, y1 = y + h > g_h ? g_h : y + h;
    int r1=(c1>>16)&0xFF, g1v=(c1>>8)&0xFF, b1=c1&0xFF;
    int r2=(c2>>16)&0xFF, g2v=(c2>>8)&0xFF, b2=c2&0xFF;
    for (int yy = y0; yy < y1; yy++) {
        int t = (y1 > y0) ? (yy - y0) * 256 / (y1 - y0) : 0;
        int r = r1 + (r2 - r1) * t / 256;
        int gv = g1v + (g2v - g1v) * t / 256;
        int b = b1 + (b2 - b1) * t / 256;
        uint32_t c = ((uint32_t)r << 16) | ((uint32_t)gv << 8) | (uint32_t)b;
        uint32_t *row = &g_fb[yy * g_stride];
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

static void fill_rounded_rect(int x, int y, int w, int h, int r, uint32_t color) {
    if (r < 0) r = 0;
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_w ? g_w : x + w, y1 = y + h > g_h ? g_h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            int in = 1;
            if (xx < x+r && yy < y+r) {
                int dx=(x+r-1)-xx, dy=(y+r-1)-yy;
                if (dx*dx+dy*dy > r*r) in = 0;
            } else if (xx >= x+w-r && yy < y+r) {
                int dx=xx-(x+w-r), dy=(y+r-1)-yy;
                if (dx*dx+dy*dy > r*r) in = 0;
            } else if (xx < x+r && yy >= y+h-r) {
                int dx=(x+r-1)-xx, dy=yy-(y+h-r);
                if (dx*dx+dy*dy > r*r) in = 0;
            } else if (xx >= x+w-r && yy >= y+h-r) {
                int dx=xx-(x+w-r), dy=yy-(y+h-r);
                if (dx*dx+dy*dy > r*r) in = 0;
            }
            if (in) g_fb[yy*g_stride+xx] = color;
        }
    }
}

static void draw_text(int x, int y, const char *text, uint32_t color) {
    while (*text) {
        uint8_t idx = (uint8_t)*text;
        if (idx >= 128) idx = '?';
        if (idx < 32) { text++; continue; }
        const uint8_t *glyph = font8[idx - 32];
        for (int gy = 0; gy < 8; gy++) {
            uint8_t bits = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                if (bits & (0x80 >> gx)) {
                    int px = x + gx, py = y + gy;
                    if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                        g_fb[py * g_stride + px] = color;
                }
            }
        }
        x += 8;
        text++;
    }
}

static int text_width(const char *text) {
    return d_strlen(text) * 8;
}

/* ========== Window System ========== */
#define MAX_WINDOWS     32
#define WIN_TITLE_MAX   128
#define TITLEBAR_H      32
#define SNAP_MARGIN     8

typedef enum {
    WIN_HIDDEN = 0,
    WIN_VISIBLE,
    WIN_MINIMIZED,
    WIN_MAXIMIZED,
    WIN_SNAP_LEFT,
    WIN_SNAP_RIGHT,
    WIN_SNAP_TOP,
} win_state_t;

#define WIN_FLAG_TITLEBAR   0x01
#define WIN_FLAG_BORDER     0x02
#define WIN_FLAG_RESIZABLE  0x04
#define WIN_FLAG_CLOSE      0x08
#define WIN_FLAG_MAX        0x10
#define WIN_FLAG_MIN        0x20
#define WIN_FLAG_SHADOW     0x40
#define WIN_FLAG_SNAP       0x80
#define WIN_FLAG_DEFAULT    (WIN_FLAG_TITLEBAR|WIN_FLAG_BORDER|WIN_FLAG_RESIZABLE|WIN_FLAG_CLOSE|WIN_FLAG_MAX|WIN_FLAG_MIN|WIN_FLAG_SHADOW|WIN_FLAG_SNAP)

typedef struct {
    uint32_t id;
    char title[WIN_TITLE_MAX];
    int x, y, w, h;
    int prev_x, prev_y, prev_w, prev_h;
    uint32_t flags;
    win_state_t state;
    int has_focus;
    int z_order;
    uint32_t titlebar_color;
    uint32_t bg_color;
    uint32_t *content_fb;
    int content_w, content_h;
    int is_dragging;
    int drag_off_x, drag_off_y;
    int is_resizing;
    int resize_edge; /* 0=right, 1=bottom, 2=corner */
    int corner_radius;
} wm_window_t;

static wm_window_t g_windows[MAX_WINDOWS];
static int g_win_count = 0;
static uint32_t g_next_win_id = 1;
static wm_window_t *g_focused = 0;

/* Mouse state */
static struct {
    int x, y;
    int left, right;
    int last_click_x, last_click_y;
    uint64_t last_click_time;
} g_mouse;

/* Keyboard state */
static uint8_t g_key_state[256] = {0};

/* Taskbar state */
static struct {
    int height;
    int win_btn_count;
    struct { uint32_t win_id; char title[32]; int active; } win_btns[MAX_WINDOWS];
    int clock_hour, clock_min;
    int start_open;
} g_taskbar = { .height = 44 };

/* ========== Window Management ========== */

static wm_window_t *wm_create(const char *title, int x, int y, int w, int h, uint32_t flags) {
    if (g_win_count >= MAX_WINDOWS) return 0;
    wm_window_t *win = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].id == 0) { win = &g_windows[i]; break; }
    }
    if (!win) return 0;

    win->id = g_next_win_id++;
    d_strncpy(win->title, title ? title : "Window", WIN_TITLE_MAX-1);
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->flags = flags;
    win->state = WIN_HIDDEN;
    win->has_focus = 0;
    win->z_order = g_win_count;
    win->titlebar_color = C_SURFACE;
    win->bg_color = C_SURFACE;
    win->content_fb = 0;
    win->content_w = 0;
    win->content_h = 0;
    win->is_dragging = 0;
    win->is_resizing = 0;
    win->corner_radius = 8;
    g_win_count++;
    return win;
}

static void wm_set_focus(wm_window_t *win) {
    for (int i = 0; i < MAX_WINDOWS; i++) g_windows[i].has_focus = 0;
    if (win) {
        win->has_focus = 1;
        int max_z = 0;
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (g_windows[i].id && g_windows[i].z_order > max_z)
                max_z = g_windows[i].z_order;
        win->z_order = max_z + 1;
        g_focused = win;
    }
}

static wm_window_t *wm_hit_test(int x, int y) {
    wm_window_t *best = 0;
    int best_z = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm_window_t *w = &g_windows[i];
        if (w->id == 0 || (w->state != WIN_VISIBLE && w->state != WIN_MAXIMIZED
            && w->state != WIN_SNAP_LEFT && w->state != WIN_SNAP_RIGHT)) continue;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) {
            if (w->z_order > best_z) { best = w; best_z = w->z_order; }
        }
    }
    return best;
}

/* Snap layout detection (Win11 style) */
static void wm_check_snap(wm_window_t *win, int mx, int my) {
    if (!win || !(win->flags & WIN_FLAG_SNAP)) return;
    int screen_h = g_h - g_taskbar.height;

    if (mx <= SNAP_MARGIN) {
        /* Snap left half */
        win->prev_x = win->x; win->prev_y = win->y;
        win->prev_w = win->w; win->prev_h = win->h;
        win->x = 0; win->y = 0;
        win->w = g_w / 2; win->h = screen_h;
        win->state = WIN_SNAP_LEFT;
    } else if (mx >= g_w - SNAP_MARGIN) {
        /* Snap right half */
        win->prev_x = win->x; win->prev_y = win->y;
        win->prev_w = win->w; win->prev_h = win->h;
        win->x = g_w / 2; win->y = 0;
        win->w = g_w / 2; win->h = screen_h;
        win->state = WIN_SNAP_RIGHT;
    } else if (my <= SNAP_MARGIN) {
        /* Maximize */
        win->prev_x = win->x; win->prev_y = win->y;
        win->prev_w = win->w; win->prev_h = win->h;
        win->x = 0; win->y = 0;
        win->w = g_w; win->h = screen_h;
        win->state = WIN_MAXIMIZED;
    }
}

/* ========== Compositor ========== */

static void draw_window(wm_window_t *win) {
    if (!win || (win->state != WIN_VISIBLE && win->state != WIN_MAXIMIZED
        && win->state != WIN_SNAP_LEFT && win->state != WIN_SNAP_RIGHT)) return;

    int wx = win->x, wy = win->y, ww = win->w, wh = win->h;
    int r = win->corner_radius;

    /* Shadow (Win11 style soft shadow) */
    if (win->flags & WIN_FLAG_SHADOW) {
        for (int s = 0; s < 8; s++) {
            int alpha = 30 - s * 3;
            if (alpha < 5) alpha = 5;
            uint32_t shadow_c = ((uint32_t)alpha << 24) | C_SHADOW;
            fill_rounded_rect(wx + s, wy + s, ww, wh, r + 2, shadow_c);
        }
    }

    /* Window body with rounded corners */
    fill_rounded_rect(wx, wy, ww, wh, r, win->bg_color);

    /* Titlebar - KDE Breeze style with Mica effect */
    if (win->flags & WIN_FLAG_TITLEBAR) {
        /* Titlebar background - gradient for Mica effect */
        fill_rounded_rect(wx, wy, ww, TITLEBAR_H, r, C_SURFACE_VAR);
        fill_rect(wx, wy + r, ww, TITLEBAR_H - r, C_SURFACE_VAR);

        /* Active focus indicator - thin accent line at top */
        if (win->has_focus) {
            fill_rect(wx + r, wy, ww - 2 * r, 2, C_ACCENT);
        }

        /* Window title */
        {
            int maxc = (ww - 140) / 8; if (maxc < 1) maxc = 1; if (maxc > 60) maxc = 60;
            char tmp[64]; d_strncpy(tmp, win->title, maxc); tmp[maxc] = 0;
            draw_text(wx + 14, wy + 12, tmp, win->has_focus ? C_TEXT : C_TEXT_SEC);
        }

        /* Window control buttons - KDE Breeze style (circle) */
        int btn_y = wy + 8;
        int btn_r = 7;

        /* Close button - right side */
        if (win->flags & WIN_FLAG_CLOSE) {
            int bx = wx + ww - 36;
            int hov = (g_mouse.x >= bx-btn_r && g_mouse.x <= bx+btn_r
                    && g_mouse.y >= btn_y-btn_r && g_mouse.y <= btn_y+btn_r);
            fill_rounded_rect(bx-btn_r, btn_y-btn_r, btn_r*2, btn_r*2, btn_r,
                              hov ? C_RED : C_MICA_LIGHT);
            /* X mark */
            draw_text(bx-3, btn_y-4, "x", hov ? C_WHITE : C_TEXT_SEC);
        }

        /* Maximize button */
        if (win->flags & WIN_FLAG_MAX) {
            int bx = wx + ww - 62;
            int hov = (g_mouse.x >= bx-btn_r && g_mouse.x <= bx+btn_r
                    && g_mouse.y >= btn_y-btn_r && g_mouse.y <= btn_y+btn_r);
            fill_rounded_rect(bx-btn_r, btn_y-btn_r, btn_r*2, btn_r*2, btn_r,
                              hov ? C_GREEN : C_MICA_LIGHT);
            draw_text(bx-3, btn_y-4, "M", hov ? C_WHITE : C_TEXT_SEC);
        }

        /* Minimize button */
        if (win->flags & WIN_FLAG_MIN) {
            int bx = wx + ww - 88;
            int hov = (g_mouse.x >= bx-btn_r && g_mouse.x <= bx+btn_r
                    && g_mouse.y >= btn_y-btn_r && g_mouse.y <= btn_y+btn_r);
            fill_rounded_rect(bx-btn_r, btn_y-btn_r, btn_r*2, btn_r*2, btn_r,
                              hov ? C_YELLOW : C_MICA_LIGHT);
            draw_text(bx-3, btn_y-4, "-", hov ? C_BLACK : C_TEXT_SEC);
        }
    }

    /* Blit content framebuffer if available */
    if (win->content_fb) {
        int cx = wx + 2, cy = wy + TITLEBAR_H;
        int cw = ww - 4, ch = wh - TITLEBAR_H - 2;
        if (cw > 0 && ch > 0) {
            for (int yy = 0; yy < ch && yy < win->content_h; yy++) {
                for (int xx = 0; xx < cw && xx < win->content_w; xx++) {
                    int px = cx + xx, py = cy + yy;
                    if (px >= 0 && px < g_w && py >= 0 && py < g_h) {
                        g_fb[py * g_stride + px] = win->content_fb[yy * win->content_w + xx];
                    }
                }
            }
        }
    }
}

/* Draw KDE Plasma style dock/taskbar */
static void draw_taskbar(void) {
    int tb_y = g_h - g_taskbar.height;
    int tb_h = g_taskbar.height;

    /* Mica background with subtle gradient */
    fill_rect_vgrad(0, tb_y, g_w, tb_h, C_TASKBAR_BG, MKC(28, 28, 36));

    /* Top border - accent line (KDE style) */
    fill_rect(0, tb_y, g_w, 1, C_ACCENT);

    /* Start button - KDE Kickoff style */
    int start_w = 44;
    int start_hov = (g_mouse.x < start_w && g_mouse.y >= tb_y);
    fill_rounded_rect(4, tb_y + 6, start_w - 4, tb_h - 12, 6,
                      start_hov ? C_ACCENT : C_SURFACE_VAR);
    /* Z logo */
    draw_text(16, tb_y + 14, "Z", C_WHITE);

    /* Window buttons - KDE task manager style */
    int btn_x = start_w + 8;
    g_taskbar.win_btn_count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm_window_t *w = &g_windows[i];
        if (w->id == 0 || w->state == WIN_HIDDEN || w->state == WIN_MINIMIZED) continue;
        int bw = 140;
        if (btn_x + bw > g_w - 100) break;

        int hov = (g_mouse.x >= btn_x && g_mouse.x < btn_x+bw &&
                   g_mouse.y >= tb_y && g_mouse.y < g_h);
        int active = w->has_focus;

        /* KDE style task button with rounded corners */
        fill_rounded_rect(btn_x, tb_y + 6, bw, tb_h - 12, 6,
                          active ? C_ACCENT : (hov ? C_TASKBAR_HOVER : C_SURFACE_VAR));

        /* Active indicator dot (KDE style) */
        if (active) {
            fill_rounded_rect(btn_x + bw/2 - 3, tb_y + tb_h - 10, 6, 3, 2, C_WHITE);
        }

        /* Title text */
        {
            int maxc = (bw-12)/8; if(maxc>28) maxc=28;
            char tmp[32]; d_strncpy(tmp, w->title, maxc); tmp[maxc]=0;
            draw_text(btn_x+8, tb_y+14, tmp, active ? C_WHITE : C_TEXT_SEC);
        }

        g_taskbar.win_btns[g_taskbar.win_btn_count].win_id = w->id;
        d_strncpy(g_taskbar.win_btns[g_taskbar.win_btn_count].title, w->title, 31);
        g_taskbar.win_btns[g_taskbar.win_btn_count].active = active;
        g_taskbar.win_btn_count++;

        btn_x += bw + 4;
    }

    /* System tray area - KDE style */
    int tray_x = g_w - 120;

    /* Clock */
    {
        char clock_str[16];
        int h = g_taskbar.clock_hour, m = g_taskbar.clock_min;
        clock_str[0] = '0' + h/10; clock_str[1] = '0' + h%10;
        clock_str[2] = ':'; clock_str[3] = '0' + m/10; clock_str[4] = '0' + m%10;
        clock_str[5] = 0;
        int cw = text_width(clock_str);
        draw_text(g_w - cw - 12, tb_y + 14, clock_str, C_TEXT);
    }
}

/* Cursor */
static int cursor_saved = 0;
static int cursor_old_x = 0, cursor_old_y = 0;
static uint32_t cursor_bg[16][16];

static void erase_cursor(void) {
    if (!cursor_saved) return;
    for (int dy = 0; dy < 16; dy++)
        for (int dx = 0; dx < 16; dx++) {
            int px = cursor_old_x + dx, py = cursor_old_y + dy;
            if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                g_fb[py * g_stride + px] = cursor_bg[dy][dx];
        }
    cursor_saved = 0;
}

static void draw_cursor(void) {
    int x = g_mouse.x, y = g_mouse.y;
    if (x < 0 || y < 0) return;
    for (int dy = 0; dy < 16; dy++)
        for (int dx = 0; dx < 16; dx++) {
            int px = x + dx, py = y + dy;
            if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                cursor_bg[dy][dx] = g_fb[py * g_stride + px];
        }
    cursor_old_x = x; cursor_old_y = y; cursor_saved = 1;

    /* KDE Breeze style cursor - sleek arrow */
    static const uint8_t cursor_bits[16][16] = {
        {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0},
        {1,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0},
        {1,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0},
        {1,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0},
        {1,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0},
        {1,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0},
        {1,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0},
        {1,2,2,2,2,2,1,1,1,1,0,0,0,0,0,0},
        {1,2,2,1,0,0,1,0,0,0,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,1,0,0,0,0,0,0,0,0},
        {1,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0},
        {1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    };
    for (int dy = 0; dy < 16; dy++)
        for (int dx = 0; dx < 16; dx++) {
            int px = x + dx, py = y + dy;
            if (px < 0 || px >= g_w || py < 0 || py >= g_h) continue;
            uint8_t bit = cursor_bits[dy][dx];
            if (bit == 1) g_fb[py * g_stride + px] = C_WHITE;
            else if (bit == 2) g_fb[py * g_stride + px] = C_BLACK;
        }
}

/* ========== Main Compositor Loop ========== */

static void composite(void) {
    /* Clear with Mica background */
    fill_rect_vgrad(0, 0, g_w, g_h - g_taskbar.height, C_MICA_BG, C_MICA_BG2);

    /* Sort windows by z-order and draw */
    for (int z = 0; z < MAX_WINDOWS * 2; z++) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            wm_window_t *w = &g_windows[i];
            if (w->id && w->z_order == z) {
                draw_window(w);
                break;
            }
        }
    }

    /* Taskbar */
    draw_taskbar();

    /* Cursor */
    draw_cursor();

    /* Flip */
    fb_flip();
}

/* ========== Input Handling ========== */

static void handle_mouse_move(int x, int y) {
    g_mouse.x = x;
    g_mouse.y = y;

    /* Window dragging */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm_window_t *w = &g_windows[i];
        if (w->id && w->is_dragging) {
            w->x = x - w->drag_off_x;
            w->y = y - w->drag_off_y;
            return;
        }
    }
}

static void handle_mouse_click(int button, int pressed) {
    if (button == 0) { /* Left button */
        g_mouse.left = pressed;
        int mx = g_mouse.x, my = g_mouse.y;

        if (pressed) {
            /* Check taskbar buttons */
            int tb_y = g_h - g_taskbar.height;
            if (my >= tb_y) {
                /* Start button */
                if (mx < 44) {
                    g_taskbar.start_open = !g_taskbar.start_open;
                    return;
                }
                /* Window buttons in taskbar */
                int btn_x = 52;
                for (int i = 0; i < g_taskbar.win_btn_count; i++) {
                    if (mx >= btn_x && mx < btn_x + 140) {
                        /* Find and focus/minimize window */
                        for (int j = 0; j < MAX_WINDOWS; j++) {
                            wm_window_t *w = &g_windows[j];
                            if (w->id == g_taskbar.win_btns[i].win_id) {
                                if (w->has_focus && w->state == WIN_VISIBLE) {
                                    w->state = WIN_MINIMIZED;
                                } else {
                                    if (w->state == WIN_MINIMIZED) w->state = WIN_VISIBLE;
                                    wm_set_focus(w);
                                }
                                break;
                            }
                        }
                        return;
                    }
                    btn_x += 144;
                }
                return;
            }

            /* Check windows (top to bottom) */
            wm_window_t *hit = wm_hit_test(mx, my);
            if (hit) {
                wm_set_focus(hit);

                /* Titlebar click - start drag */
                if (my < hit->y + TITLEBAR_H && (hit->flags & WIN_FLAG_TITLEBAR)) {
                    /* Check control buttons */
                    int btn_y = hit->y + 8;
                    int btn_r = 7;

                    /* Close button */
                    if (hit->flags & WIN_FLAG_CLOSE) {
                        int bx = hit->x + hit->w - 36;
                        if (mx >= bx-btn_r && mx <= bx+btn_r && my >= btn_y-btn_r && my <= btn_y+btn_r) {
                            hit->id = 0; hit->state = WIN_HIDDEN; g_win_count--;
                            return;
                        }
                    }
                    /* Maximize button */
                    if (hit->flags & WIN_FLAG_MAX) {
                        int bx = hit->x + hit->w - 62;
                        if (mx >= bx-btn_r && mx <= bx+btn_r && my >= btn_y-btn_r && my <= btn_y+btn_r) {
                            if (hit->state == WIN_MAXIMIZED || hit->state == WIN_SNAP_LEFT || hit->state == WIN_SNAP_RIGHT) {
                                hit->x = hit->prev_x; hit->y = hit->prev_y;
                                hit->w = hit->prev_w; hit->h = hit->prev_h;
                                hit->state = WIN_VISIBLE;
                            } else {
                                hit->prev_x = hit->x; hit->prev_y = hit->y;
                                hit->prev_w = hit->w; hit->prev_h = hit->h;
                                hit->x = 0; hit->y = 0;
                                hit->w = g_w; hit->h = g_h - g_taskbar.height;
                                hit->state = WIN_MAXIMIZED;
                            }
                            return;
                        }
                    }
                    /* Minimize button */
                    if (hit->flags & WIN_FLAG_MIN) {
                        int bx = hit->x + hit->w - 88;
                        if (mx >= bx-btn_r && mx <= bx+btn_r && my >= btn_y-btn_r && my <= btn_y+btn_r) {
                            hit->state = WIN_MINIMIZED;
                            hit->has_focus = 0;
                            return;
                        }
                    }

                    /* Start drag */
                    hit->is_dragging = 1;
                    hit->drag_off_x = mx - hit->x;
                    hit->drag_off_y = my - hit->y;
                }
            }
        } else {
            /* Mouse released - stop dragging, check snap */
            for (int i = 0; i < MAX_WINDOWS; i++) {
                wm_window_t *w = &g_windows[i];
                if (w->id && w->is_dragging) {
                    w->is_dragging = 0;
                    if (w->state == WIN_VISIBLE) {
                        wm_check_snap(w, mx, my);
                    }
                }
            }
        }
    }
}

/* ========== Main Entry ========== */

void _start(void) {
    serial_puts("[WM] zrdemage starting...\n");

    /* Get framebuffer info */
    g_fbi.base = fb_base;
    g_fbi.w = fb_width;
    g_fbi.h = fb_height;
    g_fbi.pitch = fb_pitch;

    if (g_fbi.base == 0 || g_fbi.w == 0 || g_fbi.h == 0) {
        sc2(SYS_GETFBINFO, (long)&g_fbi, 0);
    }

    g_w = (int)g_fbi.w;
    g_h = (int)g_fbi.h;
    g_stride = (int)g_fbi.pitch / 4;
    g_fb = (uint32_t*)(uint64_t)g_fbi.base;

    /* Detect BGR format */
    if (g_fbi.format == 1 || g_fbi.format == 0x34324258) g_fb_bgr = 1;

    serial_puts("[WM] Framebuffer initialized\n");

    /* Create demo windows */
    wm_window_t *w1 = wm_create("Welcome - ZratroOS", 80, 60, 600, 400, WIN_FLAG_DEFAULT);
    if (w1) wm_set_focus(w1);

    wm_window_t *w2 = wm_create("System Monitor", 200, 120, 480, 320, WIN_FLAG_DEFAULT);
    if (w2) { w2->titlebar_color = C_SURFACE_VAR; }

    /* Main loop */
    uint32_t keycodes[16];
    uint64_t last_time = 0;

    while (1) {
        {
            long fd = sys_open("/tmp/wm_request", 0);
            if (fd >= 0) {
                char req[256] = {0};
                long rn = sys_read(fd, req, sizeof(req)-1);
                sys_close(fd);
                if (rn > 0) {
                    req[rn] = 0;
                    char title[64] = {0};
                    char *nl = (char*)0;
                    for (char *p = req; *p; p++) { if (*p == '\n') { nl = p; break; } }
                    if (nl) {
                        int tlen = nl - req;
                        if (tlen > 63) tlen = 63;
                        for (int i = 0; i < tlen; i++) title[i] = req[i];
                        title[tlen] = 0;
                    } else {
                        for (int i = 0; i < rn && i < 63; i++) title[i] = req[i];
                    }
                    int wx = 80 + (g_windows[0].id ? 40 : 0);
                    int wy = 60 + (g_windows[0].id ? 30 : 0);
                    wm_window_t *win = wm_create(title, wx, wy, 600, 400, 1);
                    if (win) wm_set_focus(win);
                    serial_puts("[WM] Created window for: ");
                    serial_puts(title);
                    serial_puts("\n");
                }
            }
        }

        int nkeys = (int)sys_input_poll(keycodes, 16);
        for (int i = 0; i < nkeys; i++) {
            uint32_t kc = keycodes[i];
            int pressed = 1;
            if (kc & 0x80000000) { pressed = 0; kc &= 0x7FFFFFFF; }
            if (kc < 256) g_key_state[kc] = pressed;

            /* Keyboard shortcuts */
            if (pressed && kc == KEY_F11 && g_focused) {
                /* Toggle maximize */
                if (g_focused->state == WIN_MAXIMIZED) {
                    g_focused->x = g_focused->prev_x; g_focused->y = g_focused->prev_y;
                    g_focused->w = g_focused->prev_w; g_focused->h = g_focused->prev_h;
                    g_focused->state = WIN_VISIBLE;
                } else {
                    g_focused->prev_x = g_focused->x; g_focused->prev_y = g_focused->y;
                    g_focused->prev_w = g_focused->w; g_focused->prev_h = g_focused->h;
                    g_focused->x = 0; g_focused->y = 0;
                    g_focused->w = g_w; g_focused->h = g_h - g_taskbar.height;
                    g_focused->state = WIN_MAXIMIZED;
                }
            }
        }

        /* Update clock */
        uint64_t now = (uint64_t)sc1(SYS_TIME, 0);
        if (now != last_time) {
            g_taskbar.clock_hour = (int)((now / 3600) % 24);
            g_taskbar.clock_min = (int)((now / 60) % 60);
            last_time = now;
        }

        /* Composite and render */
        erase_cursor();
        composite();

        /* Yield */
        for (volatile int delay = 0; delay < 10000; delay++);
    }
}
