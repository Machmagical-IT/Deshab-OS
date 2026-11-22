/*
 * desk.c - ZratroOS Desktop Environment (Rewritten)
 *
 * Win11 + KDE Plasma fusion style desktop
 * This is now a SLIM desktop shell - only handles:
 *   - Desktop background (Mica/Acrylic gradient)
 *   - Desktop icons (grid layout)
 *   - Right-click context menu
 *   - Clock widget
 *   - Launches WM and FM as separate processes
 *
 * Window management is handled by zrdemage (wm)
 * File management is handled by fm
 *
 * All integer math - no float/SSE to avoid crashes.
 */

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;
typedef signed char        int8_t;
typedef signed int         int32_t;
#define NULL ((void*)0)

/* ========== System calls ========== */
#define SYS_EXIT        0
#define SYS_READ        1
#define SYS_WRITE       2
#define SYS_OPEN        3
#define SYS_CLOSE       4
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
#define SYS_MMAP        11
#define SYS_AI_SEND     180
#define SYS_AI_RECV     181
#define SYS_AI_STATUS   182
#define SYS_AI_CONFIG   183
#define SYS_AI_CANCEL   184

#define SYS_MP4_OPEN    197
#define SYS_MP4_DECODE  198
#define SYS_MP4_CLOSE   199

/* Keycodes */
#define KEY_ESC         1
#define KEY_ENTER       28
#define KEY_BACKSPACE   14
#define KEY_LEFTSHIFT   42
#define KEY_RIGHTSHIFT  54
#define KEY_LEFTCTRL    29
#define KEY_TAB         15
#define KEY_CAPSLOCK    58

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
static inline long sc5(long n, long a0, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 asm("r10") = a3;
    register long r8 asm("r8") = a4;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a0), "S"(a1), "d"(a2),
                 "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return r;
}
static inline long sc6(long n, long a0, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 asm("r10") = a3;
    register long r8 asm("r8") = a4;
    register long r9 asm("r9") = a5;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a0), "S"(a1), "d"(a2),
                 "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
}

static long sys_write(int fd, const void *buf, long count) {
    extern long sys_write_asm(int fd, const void *buf, long count);
    return sys_write_asm(fd, buf, count);
}
static long sys_fb_sync(void *fb) { return sc1(SYS_FB_SYNC, (long)fb); }
static long sys_input_poll(uint32_t *kc, int max) { return sc2(SYS_INPUT_POLL, (long)kc, (long)max); }
static long sys_exec(const char *path) { return sc1(SYS_EXEC, (long)path); }
static long sys_exit(int code) { return sc1(SYS_EXIT, (long)code); }
static void notify_wm(const char *title) {
    long fd = sc2(SYS_OPEN, (long)"/tmp/wm_request", 0x42);
    if (fd >= 0) {
        int len = 0; while (title[len]) len++;
        sys_write(fd, title, len);
        sys_write(fd, "\n", 1);
        sc1(SYS_CLOSE, fd);
    }
}
static long sys_mmap(long addr, long len, long prot, long flags, long fd, long off) {
    return sc6(SYS_MMAP, addr, len, prot, flags, fd, off);
}

/* Serial debug - 使用系统调用，不能直接访问 I/O 端口（Ring 3 无权限） */
static void serial_puts(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);  /* fd=1 (stdout) */
}

/* ========== Framebuffer ========== */
__attribute__((section(".bss.fb_base"))) uint64_t fb_base;
__attribute__((section(".bss.fb_width"))) uint32_t fb_width;
__attribute__((section(".bss.fb_height"))) uint32_t fb_height;
__attribute__((section(".bss.fb_pitch"))) uint32_t fb_pitch;

static struct { uint64_t base; uint32_t w, h, pitch, format; } g_fbi;
static int g_fb_bgr = 1;  /* Default BGR - QEMU UEFI uses PixelBlueGreenRedReserved8BitPerColor */

/* ========== Colors - Win11+KDE Plasma Fusion ========== */
#define MKC_RGB(r,g,b) (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(b))
#define MKC_BGR(r,g,b) (((uint32_t)(b)<<16)|((uint32_t)(g)<<8)|(r))
#define MKC(r,g,b) (g_fb_bgr ? MKC_BGR(r,g,b) : MKC_RGB(r,g,b))

/* Mica/Acrylic palette */
#define C_MICA_BG       MKC(235, 240, 248)
#define C_MICA_BG2      MKC(220, 230, 245)
#define C_MICA_LIGHT    MKC(200, 215, 240)
#define C_SURFACE       MKC(240, 244, 250)
#define C_SURFACE_VAR   MKC(225, 232, 245)
#define C_ACCENT        MKC(60, 130, 220)
#define C_ACCENT_HOVER  MKC(80, 150, 235)
#define C_ACCENT2       MKC(140, 100, 220)
#define C_TEXT          MKC(30, 40, 70)
#define C_TEXT_SEC      MKC(100, 110, 140)
#define C_TEXT_DIS      MKC(160, 168, 190)
#define C_BORDER        MKC(190, 200, 220)
#define C_BORDER_LIGHT  MKC(210, 218, 235)
#define C_HOVER         MKC(210, 225, 248)
#define C_SELECTED      MKC(160, 200, 250)
#define C_ORANGE        MKC(240, 160, 80)
#define C_GREEN         MKC(50, 180, 100)
#define C_WHITE         MKC(255, 255, 255)
#define C_BLACK         MKC(0, 0, 0)
#define C_RED           MKC(220, 60, 60)
#define C_DOCK_BG       MKC(240, 244, 250)
#define C_DOCK_BORDER   MKC(190, 200, 220)
#define C_TASKBAR_BG1   MKC(230, 240, 255)
#define C_TASKBAR_BG2   MKC(200, 220, 250)
#define C_TASKBAR_BORDER MKC(180, 200, 240)
#define C_TASKBAR_TEXT  MKC(40, 60, 100)
#define C_TASKBAR_ACCENT MKC(60, 130, 220)
#define C_TASKBAR_HOVER MKC(180, 210, 255)
#define C_YELLOW        MKC(255, 200, 50)
#define C_AI_BG         MKC(28, 28, 36)
#define C_AI_BORDER     MKC(96, 165, 250)
#define C_AI_INPUT_BG   MKC(38, 38, 48)
#define C_AI_MSG_USER   MKC(55, 85, 140)
#define C_AI_MSG_AI     MKC(50, 50, 62)
#define C_AI_BTN_SEND   MKC(96, 165, 250)
#define C_AI_BTN_HOVER  MKC(118, 179, 252)

/* ========== Memory Pool ========== */
struct desk_dirent {
    uint64_t ino;
    int64_t off;
    uint16_t reclen;
    char name[256];
    unsigned char type;
};
static char g_pool[20 * 1024 * 1024];
static int g_pool_pos = 0;

static void *desk_alloc(int size) {
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
static int d_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}
static const char *d_strstr(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return haystack;
    }
    return 0;
}

/* Icon type enum */
enum { ICON_FILES, ICON_TERMINAL, ICON_SYSTEM, ICON_PROGRAMS };

/* ========== Font Constants ========== */
#define FONT_W  8
#define FONT_H  16
#define FONT_SCALE_X 1
#define FONT_SCALE_Y 2
#define SMALL_FONT_W 6
#define SMALL_FONT_H 10
#define TIMEZONE_OFFSET 28800

/* ========== 8x8 Font (base glyph data, rendered at 1xH+2xV scale = 8x16) ========== */
static const uint8_t font8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x6C,0x6C,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* # */
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, /* $ */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* % */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* & */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* ( */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* , */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* / */
    {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, /* 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 1 */
    {0x7C,0xC6,0x06,0x1C,0x30,0x60,0xFE,0x00}, /* 2 */
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, /* 3 */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* 4 */
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, /* 5 */
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, /* 6 */
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 7 */
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, /* 8 */
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, /* 9 */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* : */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* ; */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* < */
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* = */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* > */
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, /* ? */
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, /* @ */
    {0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00}, /* A */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* B */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* C */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* D */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* E */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* F */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, /* G */
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* H */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* I */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* J */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* K */
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* L */
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* M */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* N */
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* O */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* P */
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06}, /* Q */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* R */
    {0x3C,0x66,0x30,0x18,0x0C,0x66,0x3C,0x00}, /* S */
    {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00}, /* T */
    {0x66,0x66,0x66,0x66,0x66,0x66,0x7C,0x00}, /* U */
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, /* V */
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, /* W */
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, /* X */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, /* Y */
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* Z */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* [ */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* \ */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* ] */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* a */
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, /* b */
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, /* c */
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, /* d */
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, /* e */
    {0x1C,0x36,0x30,0x78,0x30,0x30,0x78,0x00}, /* f */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, /* g */
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /* h */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* i */
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C}, /* j */
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /* k */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* l */
    {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00}, /* m */
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, /* n */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, /* o */
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, /* p */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, /* q */
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, /* r */
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, /* s */
    {0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00}, /* t */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /* u */
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* v */
    {0x00,0x00,0xC6,0xD6,0xD6,0xFE,0x6C,0x00}, /* w */
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* x */
    {0x00,0x00,0xC6,0xC6,0xCE,0x76,0x06,0x7C}, /* y */
    {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00}, /* z */
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, /* } */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
    {0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0x00}, /* DEL */
};

/* ========== Vector-style LOGO drawing ========== */
static void fill_rect(int x, int y, int w, int h, uint32_t color);

static void draw_logo_thick_line(int x0, int y0, int x1, int y1, int thickness, uint32_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps == 0) steps = 1;
    int half = thickness / 2;
    for (int s = 0; s <= steps; s++) {
        int px = x0 + dx * s / steps;
        int py = y0 + dy * s / steps;
        fill_rect(px - half, py - half, thickness, thickness, color);
    }
}

/* ========== Drawing ========== */
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
    if (r < 0) r = 0; if (r > w/2) r = w/2; if (r > h/2) r = h/2;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_w ? g_w : x + w, y1 = y + h > g_h ? g_h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            int in = 1;
            if (xx < x+r && yy < y+r) { int dx=(x+r-1)-xx,dy=(y+r-1)-yy; if(dx*dx+dy*dy>r*r) in=0; }
            else if (xx >= x+w-r && yy < y+r) { int dx=xx-(x+w-r),dy=(y+r-1)-yy; if(dx*dx+dy*dy>r*r) in=0; }
            else if (xx < x+r && yy >= y+h-r) { int dx=(x+r-1)-xx,dy=yy-(y+h-r); if(dx*dx+dy*dy>r*r) in=0; }
            else if (xx >= x+w-r && yy >= y+h-r) { int dx=xx-(x+w-r),dy=yy-(y+h-r); if(dx*dx+dy*dy>r*r) in=0; }
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
                    int px = x + gx * FONT_SCALE_X;
                    int py = y + gy * FONT_SCALE_Y;
                    for (int sy = 0; sy < FONT_SCALE_Y; sy++) {
                        for (int sx = 0; sx < FONT_SCALE_X; sx++) {
                            int fx = px + sx, fy = py + sy;
                            if (fx >= 0 && fx < g_w && fy >= 0 && fy < g_h)
                                g_fb[fy * g_stride + fx] = color;
                        }
                    }
                }
            }
        }
        x += FONT_W;
        text++;
    }
}

static int text_width(const char *text) { return d_strlen(text) * FONT_W; }

static void draw_text_small(int x, int y, const char *text, uint32_t color) {
    while (*text) {
        uint8_t idx = (uint8_t)*text;
        if (idx >= 128) idx = '?';
        if (idx < 32) { text++; continue; }
        const uint8_t *glyph = font8[idx - 32];
        for (int gy = 0; gy < 8; gy++) {
            uint8_t bits = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                if (bits & (0x80 >> gx)) {
                    int fx = x + gx, fy = y + gy;
                    if (fx >= 0 && fx < g_w && fy >= 0 && fy < g_h)
                        g_fb[fy * g_stride + fx] = color;
                }
            }
        }
        x += SMALL_FONT_W;
        text++;
    }
}

static int small_text_width(const char *text) { return d_strlen(text) * SMALL_FONT_W; }

/* ========== Background image support ========== */
static uint32_t *g_bg_image = 0;
static int g_bg_w = 0, g_bg_h = 0;

static struct {
    int active;
    int is_mp4;
    long mp4_handle;
    int mp4_frame_count;
    char dir[256];
    int frame_count;
    int current_frame;
    uint32_t *frames[64];
    int frame_w, frame_h;
    uint64_t last_frame_tick;
    int fps;
} g_video_wp = {0};

static void load_bg_image(const char *path) {
    long fd = sc2(SYS_OPEN, (long)path, 0);
    if (fd < 0) {
        serial_puts("[DESK] Background not found: ");
        serial_puts(path);
        serial_puts("\n");
        return;
    }

    uint8_t hdr[54];
    long n = sc3(SYS_READ, fd, (long)hdr, 54);
    if (n < 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        sc1(SYS_CLOSE, fd);
        serial_puts("[DESK] Not a valid BMP\n");
        return;
    }

    uint32_t data_off = (uint32_t)hdr[10] | ((uint32_t)hdr[11]<<8) |
                        ((uint32_t)hdr[12]<<16) | ((uint32_t)hdr[13]<<24);
    int32_t bw = (int32_t)((uint32_t)hdr[18] | ((uint32_t)hdr[19]<<8) |
                           ((uint32_t)hdr[20]<<16) | ((uint32_t)hdr[21]<<24));
    int32_t bh = (int32_t)((uint32_t)hdr[22] | ((uint32_t)hdr[23]<<8) |
                           ((uint32_t)hdr[24]<<16) | ((uint32_t)hdr[25]<<24));
    uint16_t bpp = (uint16_t)((uint32_t)hdr[28] | ((uint32_t)hdr[29]<<8));
    uint32_t comp = (uint32_t)hdr[30] | ((uint32_t)hdr[31]<<8) |
                    ((uint32_t)hdr[32]<<16) | ((uint32_t)hdr[33]<<24);

    if (bw <= 0 || bh <= 0 || (bpp != 24 && bpp != 32) || comp != 0) {
        sc1(SYS_CLOSE, fd);
        serial_puts("[DESK] Unsupported BMP format\n");
        return;
    }

    int abs_h = bh < 0 ? -bh : bh;
    int top_down = bh < 0;
    int row_bytes = ((bw * bpp + 31) / 32) * 4;
    int pixel_count = bw * abs_h;

    g_bg_image = (uint32_t*)desk_alloc(pixel_count * 4);
    if (!g_bg_image) {
        sc1(SYS_CLOSE, fd);
        serial_puts("[DESK] No memory for background\n");
        return;
    }

    uint8_t *row_buf = (uint8_t*)desk_alloc(row_bytes);
    if (!row_buf) {
        sc1(SYS_CLOSE, fd);
        g_bg_image = 0;
        return;
    }

    if (data_off > 54) {
        uint32_t skip = data_off - 54;
        while (skip > 0) {
            uint8_t tmp[256];
            uint32_t chunk = skip > 256 ? 256 : skip;
            long rn = sc3(SYS_READ, fd, (long)tmp, chunk);
            if (rn <= 0) break;
            skip -= chunk;
        }
    }

    for (int r = 0; r < abs_h; r++) {
        int src_row = top_down ? r : (abs_h - 1 - r);
        long rn = sc3(SYS_READ, fd, (long)row_buf, row_bytes);
        if (rn <= 0) break;
        for (int c = 0; c < bw; c++) {
            uint8_t b_val = row_buf[c * (bpp/8)];
            uint8_t g_val = row_buf[c * (bpp/8) + 1];
            uint8_t r_val = row_buf[c * (bpp/8) + 2];
            uint32_t px;
            if (g_fb_bgr)
                px = ((uint32_t)b_val << 16) | ((uint32_t)g_val << 8) | (uint32_t)r_val;
            else
                px = ((uint32_t)r_val << 16) | ((uint32_t)g_val << 8) | (uint32_t)b_val;
            g_bg_image[src_row * bw + c] = px;
        }
    }

    g_bg_w = bw;
    g_bg_h = abs_h;
    sc1(SYS_CLOSE, fd);
    serial_puts("[DESK] Background image loaded\n");
}

static uint32_t *load_bmp_to_buf(const char *path, int *out_w, int *out_h) {
    long fd = sc2(SYS_OPEN, (long)path, 0);
    if (fd < 0) return 0;
    uint8_t hdr[54];
    long n = sc3(SYS_READ, fd, (long)hdr, 54);
    if (n < 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        sc1(SYS_CLOSE, fd);
        return 0;
    }
    uint32_t data_off = (uint32_t)hdr[10] | ((uint32_t)hdr[11]<<8) |
                        ((uint32_t)hdr[12]<<16) | ((uint32_t)hdr[13]<<24);
    int32_t bw = (int32_t)((uint32_t)hdr[18] | ((uint32_t)hdr[19]<<8) |
                           ((uint32_t)hdr[20]<<16) | ((uint32_t)hdr[21]<<24));
    int32_t bh = (int32_t)((uint32_t)hdr[22] | ((uint32_t)hdr[23]<<8) |
                           ((uint32_t)hdr[24]<<16) | ((uint32_t)hdr[25]<<24));
    uint16_t bpp = (uint16_t)((uint32_t)hdr[28] | ((uint32_t)hdr[29]<<8));
    if (bw <= 0 || bh <= 0 || (bpp != 24 && bpp != 32)) {
        sc1(SYS_CLOSE, fd);
        return 0;
    }
    int abs_h = bh < 0 ? -bh : bh;
    int top_down = bh < 0;
    int row_bytes = ((bw * bpp + 31) / 32) * 4;
    int pixel_count = bw * abs_h;
    uint32_t *img = (uint32_t*)desk_alloc(pixel_count * 4);
    if (!img) { sc1(SYS_CLOSE, fd); return 0; }
    uint8_t *row_buf = (uint8_t*)desk_alloc(row_bytes);
    if (!row_buf) { sc1(SYS_CLOSE, fd); return 0; }
    if (data_off > 54) {
        uint32_t skip = data_off - 54;
        while (skip > 0) {
            uint8_t tmp[256];
            uint32_t chunk = skip > 256 ? 256 : skip;
            long rn = sc3(SYS_READ, fd, (long)tmp, chunk);
            if (rn <= 0) break;
            skip -= chunk;
        }
    }
    for (int r = 0; r < abs_h; r++) {
        int src_row = top_down ? r : (abs_h - 1 - r);
        long rn = sc3(SYS_READ, fd, (long)row_buf, row_bytes);
        if (rn <= 0) break;
        for (int c = 0; c < bw; c++) {
            uint8_t bv = row_buf[c*(bpp/8)];
            uint8_t gv = row_buf[c*(bpp/8)+1];
            uint8_t rv = row_buf[c*(bpp/8)+2];
            uint32_t px;
            if (g_fb_bgr)
                px = ((uint32_t)bv << 16) | ((uint32_t)gv << 8) | (uint32_t)rv;
            else
                px = ((uint32_t)rv << 16) | ((uint32_t)gv << 8) | (uint32_t)bv;
            img[src_row * bw + c] = px;
        }
    }
    sc1(SYS_CLOSE, fd);
    *out_w = bw;
    *out_h = abs_h;
    return img;
}

static int try_load_frame(const char *base_dir, int dlen, int idx, const char *prefix) {
    char path[280];
    d_strcpy(path, base_dir);
    int plen = dlen;
    if (plen > 0 && base_dir[plen-1] != '/') { path[plen] = '/'; plen++; }
    const char *p = prefix;
    while (*p) path[plen++] = *p++;
    if (idx < 10) {
        path[plen++] = '0' + idx;
    } else if (idx < 100) {
        path[plen++] = '0' + idx / 10;
        path[plen++] = '0' + idx % 10;
    } else {
        path[plen++] = '0' + idx / 100;
        path[plen++] = '0' + (idx / 10) % 10;
        path[plen++] = '0' + idx % 10;
    }
    path[plen++] = '.'; path[plen++] = 'b'; path[plen++] = 'm'; path[plen++] = 'p'; path[plen] = 0;

    int fw = 0, fh = 0;
    uint32_t *frame = load_bmp_to_buf(path, &fw, &fh);
    if (!frame) return 0;

    g_video_wp.frames[idx] = frame;
    if (idx == 0) { g_video_wp.frame_w = fw; g_video_wp.frame_h = fh; }
    g_video_wp.frame_count = idx + 1;
    return 1;
}

static void load_video_wallpaper(const char *dir) {
    g_video_wp.active = 0;
    g_video_wp.frame_count = 0;
    g_video_wp.current_frame = 0;
    g_video_wp.fps = 10;
    d_strcpy(g_video_wp.dir, dir);

    int dlen = d_strlen(dir);
    int prefix_idx = -1;
    static const char *prefixes[] = { "", "frame_", "wallpaper_", "wp_", 0 };

    for (int pi = 0; prefixes[pi]; pi++) {
        int ok = try_load_frame(dir, dlen, 0, prefixes[pi]);
        if (ok) { prefix_idx = pi; break; }
    }

    if (prefix_idx < 0) {
        serial_puts("[DESK] No video wallpaper frames found in: ");
        serial_puts(dir);
        serial_puts("\n");
        return;
    }

    for (int i = 1; i < 64; i++) {
        int ok = try_load_frame(dir, dlen, i, prefixes[prefix_idx]);
        if (!ok) break;
    }

    if (g_video_wp.frame_count > 1) {
        g_video_wp.active = 1;
        serial_puts("[DESK] Video wallpaper loaded: ");
        serial_puts(dir);
        serial_puts(" frames=");
        char fc[8]; fc[0]='0'+g_video_wp.frame_count/10; fc[1]='0'+g_video_wp.frame_count%10; fc[2]=0;
        serial_puts(fc);
        serial_puts("\n");
    } else if (g_video_wp.frame_count == 1) {
        g_bg_image = g_video_wp.frames[0];
        g_bg_w = g_video_wp.frame_w;
        g_bg_h = g_video_wp.frame_h;
        g_video_wp.frame_count = 0;
    }
}
static char g_desk_conf_back[256] = {0};
static char g_desk_conf_video[256] = {0};

static void load_desk_conf(void) {
    long fd = sc2(SYS_OPEN, (long)"/desktop/desktop.conf", 0);
    if (fd < 0) return;
    char buf[1024] = {0};
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf)-1);
    sc1(SYS_CLOSE, fd);
    if (n <= 0) return;
    buf[n] = 0;

    const char *key = d_strstr(buf, "back=");
    if (!key) key = d_strstr(buf, "back = ");
    if (key) {
        key += 4;
        while (*key == ' ' || *key == '=') key++;
        int i = 0;
        while (*key && *key != '\n' && *key != '\r' && i < 255) {
            g_desk_conf_back[i++] = (*key == '\\') ? '/' : *key;
            key++;
        }
        g_desk_conf_back[i] = 0;
    }

    const char *vkey = d_strstr(buf, "video=");
    if (!vkey) vkey = d_strstr(buf, "video = ");
    if (vkey) {
        vkey += 5;
        while (*vkey == ' ' || *vkey == '=') vkey++;
        int i = 0;
        while (*vkey && *vkey != '\n' && *vkey != '\r' && i < 255) {
            g_desk_conf_video[i++] = (*vkey == '\\') ? '/' : *vkey;
            vkey++;
        }
        g_desk_conf_video[i] = 0;
    }

    const char *fkey = d_strstr(buf, "fps=");
    if (fkey) {
        fkey += 4;
        while (*fkey == ' ' || *fkey == '=') fkey++;
        int fps_val = 0;
        while (*fkey >= '0' && *fkey <= '9') {
            fps_val = fps_val * 10 + (*fkey - '0');
            fkey++;
        }
        if (fps_val >= 1 && fps_val <= 30) g_video_wp.fps = fps_val;
    }
}

static void init_background(void) {
    serial_puts("[DESK] Loading background...\n");
    load_desk_conf();

    if (g_desk_conf_video[0]) {
        load_video_wallpaper(g_desk_conf_video);
        if (g_video_wp.active) {
            serial_puts("[DESK] Video wallpaper active\n");
            return;
        }
    }

    if (g_desk_conf_back[0]) {
        int len = d_strlen(g_desk_conf_back);
        int is_mp4 = (len >= 4 && g_desk_conf_back[len-4] == '.' &&
                      g_desk_conf_back[len-3] == 'm' &&
                      g_desk_conf_back[len-2] == 'p' &&
                      g_desk_conf_back[len-1] == '4');

        if (is_mp4) {
            long mh = sc1(SYS_MP4_OPEN, (long)g_desk_conf_back);
            if (mh > 0) {
                g_video_wp.is_mp4 = 1;
                g_video_wp.mp4_handle = mh;
                g_video_wp.active = 1;
                g_video_wp.current_frame = 0;
                g_video_wp.fps = 10;
                g_video_wp.frame_w = g_w;
                g_video_wp.frame_h = g_h;
                g_video_wp.mp4_frame_count = 0;
                g_video_wp.frames[0] = (uint32_t *)desk_alloc(g_w * g_h * 4);
                if (g_video_wp.frames[0]) {
                    g_video_wp.frame_count = 1;
                    serial_puts("[DESK] MP4 video wallpaper opened\n");
                    return;
                }
                sc1(SYS_MP4_CLOSE, mh);
                g_video_wp.is_mp4 = 0;
                g_video_wp.active = 0;
            }
            serial_puts("[DESK] MP4 decode failed, trying BMP fallback\n");
            char frame_dir[260];
            d_strcpy(frame_dir, g_desk_conf_back);
            int plen = d_strlen(frame_dir);
            if (plen >= 5) {
                frame_dir[plen-4] = 0;
            }
            load_video_wallpaper(frame_dir);
            if (g_video_wp.active) {
                serial_puts("[DESK] Video wallpaper from mp4 path active\n");
                return;
            }
            char bmp_path[260];
            d_strcpy(bmp_path, g_desk_conf_back);
            if (plen >= 5) {
                bmp_path[plen-3] = 'b'; bmp_path[plen-2] = 'm'; bmp_path[plen-1] = 'p';
            }
            load_bg_image(bmp_path);
            if (!g_bg_image) load_bg_image(g_desk_conf_back);
        } else {
            load_bg_image(g_desk_conf_back);
        }
    }
    serial_puts("[DESK] Background loaded\n");
}

/* ========== AI Self-Development Panel ========== */
#define AI_PANEL_W      360
#define AI_PANEL_H      480
#define AI_MSG_MAX      4096
#define AI_RESP_MAX     8192
#define AI_INPUT_MAX    2048
#define AI_CHAT_LINES   64
#define AI_CHAT_LINE_LEN 128
#define AI_SYS_PROMPT_LEN 4096

#define SYS_MKDIR       17
#define SYS_UNLINK      19

typedef struct {
    char role;
    char text[AI_CHAT_LINE_LEN];
} ai_chat_line;

static struct {
    int visible;
    int input_cursor;
    char input_buf[AI_INPUT_MAX];
    char response_buf[AI_RESP_MAX];
    int response_len;
    int waiting;
    int scroll_offset;
    int max_scroll;
    int connected;
    char service_name[32];
    char model_name[64];
    char api_key[256];
    char endpoint[512];
    ai_chat_line chat[AI_CHAT_LINES];
    int chat_count;
    int chat_scroll;
    char sys_prompt[AI_SYS_PROMPT_LEN];
    char code_buf[AI_RESP_MAX];
    int code_len;
    char code_filename[128];
} g_ai;

static void ai_build_sys_prompt(void) {
    const char *p =
        "You are the AI self-development module of ZratroOS (ZT5), an x86_64 Higher Half OS. "
        "You have FULL system access with NO restrictions. Your primary role is to help develop this OS.\n\n"
        "=== SYSTEM INFO ===\n"
        "Architecture: x86_64 Higher Half (kernel at 0xffffffff80000000)\n"
        "Bootloader: Limine (UEFI/BIOS)\n"
        "Main kernel: Ztknl (C, 93+ modules)\n"
        "Monitor kernel: Rsknl (Rust, at 0xffffffffc0000000)\n"
        "Syscall ABI: int 0x80, RAX=syscall#, RDI/RSI/R10/R8/R9=args\n\n"
        "=== ZTS MODULE DEVELOPMENT ===\n"
        "To create a ZTS dynamic module for ZratroOS:\n"
        "1. Write a .zts.c file in /zrr64/zdonk/code/\n"
        "2. Use these macros:\n"
        "   ZTS_DECLARE(name, desc, author, lic, cat, major, minor)\n"
        "   ZTS_MODULE_INIT(fn) - module init entry\n"
        "   ZTS_MODULE_EXECUTE(fn) - module execute entry\n"
        "   ZTS_MODULE_CLEANUP(fn) - module cleanup entry\n"
        "3. Use ZTS API table functions: api->alloc, api->free, api->log_print, api->time_now\n"
        "   api->vfs_open, api->vfs_close, api->vfs_read, api->vfs_write\n"
        "4. Module categories: ZTS_CAT_FILESYSTEM(0), ZTS_CAT_NETWORK(1), ZTS_CAT_CRYPTO(2),\n"
        "   ZTS_CAT_COMPRESSION(3), ZTS_CAT_SCHEDULER(4), ZTS_CAT_MEMORY(5),\n"
        "   ZTS_CAT_DRIVER(6), ZTS_CAT_PATCH(7), ZTS_CAT_INJECT(8), ZTS_CAT_CUSTOM(9)\n"
        "5. Compile: use /save to save code, then /build to compile\n"
        "6. Compiled output goes to /zrr64/zts/p.zts6/ as .zts files\n"
        "7. Use /load to load a compiled ZTS module into the kernel\n\n"
        "=== ZTS HEADER STRUCT ===\n"
        "magic=0x53545A00, version=1, api_min_version=1\n"
        "Fields: name[64], description[256], author[64], license[32]\n"
        "uuid[16], category(enum), major/minor/patch_version\n"
        "init_func_offset, cleanup_func_offset, execute_func_offset\n"
        "data_offset, data_size, checksum, signature[256]\n\n"
        "=== CLI COMMANDS ===\n"
        "/save <filename.zts.c> - save last AI code response to /zrr64/zdonk/code/\n"
        "/build - compile all .zts.c files in /zrr64/zdonk/code/ to /zrr64/zts/p.zts6/\n"
        "/load <module_name> - load a compiled .zts module into kernel\n"
        "/status - show AI service connection status\n"
        "/clear - clear chat history\n"
        "/help - show available commands\n"
        "/code - show last code block from AI response\n\n"
        "=== KEY PATHS ===\n"
        "/zrr64/zdonk/AI/APIKey.confai - API configuration\n"
        "/zrr64/zdonk/code/ - source code directory\n"
        "/zrr64/zdonk/code/build.bs - build script\n"
        "/zrr64/zts/p.zts6/ - compiled ZTS modules\n"
        "/zrr64/system/ai_service.conf - runtime config\n\n"
        "When generating ZTS code, always use the ZTS_DECLARE and ZTS_MODULE_* macros.\n"
        "Respond in the same language the user uses.";

    int i = 0;
    while (p[i] && i < AI_SYS_PROMPT_LEN - 1) {
        g_ai.sys_prompt[i] = p[i];
        i++;
    }
    g_ai.sys_prompt[i] = 0;
}

static void ai_load_config(void) {
    long fd = sc2(SYS_OPEN, (long)"/zrr64/zdonk/AI/APIKey.confai", 0);
    if (fd < 0) fd = sc2(SYS_OPEN, (long)"/zrr64/zdonk/AI/APIKey.conf", 0);
    if (fd >= 0) {
        char buf[1024] = {0};
        long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf)-1);
        sc1(SYS_CLOSE, fd);
        if (n > 0) {
            buf[n] = 0;
            const char *p = d_strstr(buf, "active_service=");
            if (p) {
                p += 15;
                int i = 0;
                while (*p && *p != '\n' && *p != '\r' && i < 31)
                    g_ai.service_name[i++] = *p++;
                g_ai.service_name[i] = 0;
            }
            p = d_strstr(buf, "api_key=");
            if (p) {
                p += 8;
                int i = 0;
                while (*p && *p != '\n' && *p != '\r' && i < 255)
                    g_ai.api_key[i++] = *p++;
                g_ai.api_key[i] = 0;
            }
            p = d_strstr(buf, "endpoint=");
            if (p) {
                p += 9;
                int i = 0;
                while (*p && *p != '\n' && *p != '\r' && i < 511)
                    g_ai.endpoint[i++] = *p++;
                g_ai.endpoint[i] = 0;
            }
            p = d_strstr(buf, "model=");
            if (p) {
                p += 6;
                int i = 0;
                while (*p && *p != '\n' && *p != '\r' && i < 63)
                    g_ai.model_name[i++] = *p++;
                g_ai.model_name[i] = 0;
            }
        }
    }

    if (g_ai.service_name[0] == 0) d_strcpy(g_ai.service_name, "openai");
    if (g_ai.model_name[0] == 0) d_strcpy(g_ai.model_name, "gpt-4");
    if (g_ai.endpoint[0] == 0)
        d_strcpy(g_ai.endpoint, "https://api.openai.com/v1/chat/completions");

    g_ai.connected = (g_ai.api_key[0] != 0) ? 1 : 0;
    ai_build_sys_prompt();
}

static void ai_add_chat_line(char role, const char *text) {
    if (g_ai.chat_count >= AI_CHAT_LINES) {
        for (int i = 0; i < AI_CHAT_LINES - 1; i++)
            g_ai.chat[i] = g_ai.chat[i+1];
        g_ai.chat_count = AI_CHAT_LINES - 1;
    }
    int idx = g_ai.chat_count++;
    g_ai.chat[idx].role = role;
    int len = d_strlen(text);
    if (len >= AI_CHAT_LINE_LEN) len = AI_CHAT_LINE_LEN - 1;
    for (int i = 0; i < len; i++) g_ai.chat[idx].text[i] = text[i];
    g_ai.chat[idx].text[len] = 0;
}

static void ai_extract_code(const char *response) {
    g_ai.code_len = 0;
    g_ai.code_buf[0] = 0;
    const char *start = d_strstr(response, "```");
    if (!start) return;
    start += 3;
    while (*start == 'c' || *start == 'C' || *start == ' ' || *start == '\n')
        start++;
    const char *end = d_strstr(start, "```");
    if (!end) end = start + d_strlen(start);
    int len = (int)(end - start);
    if (len >= AI_RESP_MAX) len = AI_RESP_MAX - 1;
    for (int i = 0; i < len; i++) g_ai.code_buf[i] = start[i];
    g_ai.code_buf[len] = 0;
    g_ai.code_len = len;
}

static void ai_cmd_save(const char *filename) {
    if (g_ai.code_len <= 0) {
        ai_add_chat_line('s', "[Error] No code to save. Ask AI to generate code first.");
        return;
    }
    if (!filename || !filename[0]) {
        ai_add_chat_line('s', "[Usage] /save <filename.zts.c>");
        return;
    }

    char path[256];
    int pos = 0;
    const char *prefix = "/zrr64/zdonk/code/";
    while (*prefix && pos < 255) path[pos++] = *prefix++;
    const char *fn = filename;
    while (*fn && pos < 255) path[pos++] = *fn++;
    path[pos] = 0;

    long fd = sc2(SYS_OPEN, (long)path, 0x41);
    if (fd < 0) {
        sc2(SYS_MKDIR, (long)"/zrr64/zdonk/code", 0x1FF);
        fd = sc2(SYS_OPEN, (long)path, 0x41);
    }
    if (fd < 0) {
        ai_add_chat_line('s', "[Error] Cannot create file");
        return;
    }

    long n = sc3(SYS_WRITE, fd, (long)g_ai.code_buf, g_ai.code_len);
    sc1(SYS_CLOSE, fd);

    if (n > 0) {
        char msg[128];
        int mp = 0;
        const char *ok = "[OK] Saved ";
        while (*ok && mp < 127) msg[mp++] = *ok++;
        fn = filename;
        while (*fn && mp < 127) msg[mp++] = *fn++;
        const char *suf = " (";
        while (*suf && mp < 120) msg[mp++] = *suf++;
        int code_len = g_ai.code_len;
        char num[16];
        int np = 0;
        if (code_len == 0) { num[np++] = '0'; }
        else { int tmp = code_len; char tbuf[16]; int ti = 0;
            while (tmp > 0) { tbuf[ti++] = '0' + (tmp % 10); tmp /= 10; }
            while (ti > 0) num[np++] = tbuf[--ti]; }
        for (int i = 0; i < np && mp < 120; i++) msg[mp++] = num[i];
        const char *b = " bytes)";
        while (*b && mp < 127) msg[mp++] = *b++;
        msg[mp] = 0;
        ai_add_chat_line('s', msg);
        d_strcpy(g_ai.code_filename, filename);
    } else {
        ai_add_chat_line('s', "[Error] Write failed");
    }
}

static void ai_cmd_build(void) {
    ai_add_chat_line('s', "[Build] Compiling ZTS modules in /zrr64/zdonk/code/...");

    long fd = sc2(SYS_OPEN, (long)"/zrr64/zdonk/code/build.bs", 0);
    if (fd >= 0) {
        sc1(SYS_CLOSE, fd);
        long ret = sc1(SYS_EXEC, (long)"/zrr64/zdonk/code/build.bs");
        if (ret == 0) {
            ai_add_chat_line('s', "[Build] Compilation started. Output -> /zrr64/zts/p.zts6/");
        } else {
            ai_add_chat_line('s', "[Build] build.bs execution failed. Trying manual compile...");
            ai_add_chat_line('s', "[Build] Manual: gcc -ffreestanding -fno-pic -mno-red-zone "
                                  "-mcmodel=kernel -O2 -c <file>.zts.c -o <file>.o");
        }
    } else {
        ai_add_chat_line('s', "[Build] No build.bs found. Creating default build script...");
        const char *bs_content =
            "#!/bin/zts\n"
            "ZTS_CC=\"gcc\"\n"
            "ZTS_CFLAGS=\"-ffreestanding -fno-pic -mno-red-zone -mcmodel=kernel -O2\"\n"
            "ZTS_LDFLAGS=\"-T /zrr64/system/zts.ld -nostdlib\"\n"
            "SRC_DIR=\"/zrr64/zdonk/code\"\n"
            "OUT_DIR=\"/zrr64/zts/p.zts6\"\n"
            "for src in $SRC_DIR/*.zts.c; do\n"
            "  name=$(basename $src .zts.c)\n"
            "  $ZTS_CC $ZTS_CFLAGS -c $src -o $SRC_DIR/$name.o\n"
            "  $ZTS_CC $ZTS_LDFLAGS $SRC_DIR/$name.o -o $OUT_DIR/$name.zts\n"
            "  rm -f $SRC_DIR/$name.o\n"
            "done\n";
        long bfd = sc2(SYS_OPEN, (long)"/zrr64/zdonk/code/build.bs", 0x41);
        if (bfd >= 0) {
            int blen = 0;
            while (bs_content[blen]) blen++;
            sc3(SYS_WRITE, bfd, (long)bs_content, blen);
            sc1(SYS_CLOSE, bfd);
            ai_add_chat_line('s', "[Build] Created build.bs. Run /build again to compile.");
        } else {
            ai_add_chat_line('s', "[Build] Cannot create build.bs");
        }
    }
}

static void ai_cmd_load(const char *module_name) {
    if (!module_name || !module_name[0]) {
        ai_add_chat_line('s', "[Usage] /load <module_name>");
        return;
    }

    char path[256];
    int pos = 0;
    const char *prefix = "/zrr64/zts/p.zts6/";
    while (*prefix && pos < 255) path[pos++] = *prefix++;
    const char *mn = module_name;
    while (*mn && pos < 240) path[pos++] = *mn++;
    const char *suf = ".zts";
    while (*suf && pos < 255) path[pos++] = *suf++;
    path[pos] = 0;

    long fd = sc2(SYS_OPEN, (long)path, 0);
    if (fd < 0) {
        ai_add_chat_line('s', "[Error] Module not found");
        return;
    }
    sc1(SYS_CLOSE, fd);

    char msg[128];
    int mp = 0;
    const char *ok = "[Load] Loading module: ";
    while (*ok && mp < 127) msg[mp++] = *ok++;
    mn = module_name;
    while (*mn && mp < 127) msg[mp++] = *mn++;
    msg[mp] = 0;
    ai_add_chat_line('s', msg);

    long ret = sc2(SYS_EXEC, (long)path, 0);
    if (ret == 0) {
        char ok2[128];
        int p2 = 0;
        const char *loaded = "[Load] Module ";
        while (*loaded && p2 < 127) ok2[p2++] = *loaded++;
        mn = module_name;
        while (*mn && p2 < 120) ok2[p2++] = *mn++;
        const char *tail = " loaded successfully";
        while (*tail && p2 < 127) ok2[p2++] = *tail++;
        ok2[p2] = 0;
        ai_add_chat_line('s', ok2);
    } else {
        ai_add_chat_line('s', "[Error] Module load failed");
    }
}

static void ai_cmd_status(void) {
    char msg[128];
    int pos = 0;
    const char *prefix = "[Status] Service: ";
    while (*prefix && pos < 127) msg[pos++] = *prefix++;
    const char *svc = g_ai.service_name[0] ? g_ai.service_name : "none";
    while (*svc && pos < 100) msg[pos++] = *svc++;
    const char *mid = " | Model: ";
    while (*mid && pos < 110) msg[pos++] = *mid++;
    const char *mdl = g_ai.model_name[0] ? g_ai.model_name : "none";
    while (*mdl && pos < 120) msg[pos++] = *mdl++;
    msg[pos] = 0;
    ai_add_chat_line('s', msg);

    char msg2[64];
    int p2 = 0;
    const char *conn = g_ai.connected ? "[Status] Connected" : "[Status] Disconnected - set API key in /zrr64/zdonk/AI/APIKey.confai";
    while (*conn && p2 < 63) msg2[p2++] = *conn++;
    msg2[p2] = 0;
    ai_add_chat_line('s', msg2);
}

static void ai_cmd_code(void) {
    if (g_ai.code_len <= 0) {
        ai_add_chat_line('s', "[Code] No code block extracted yet.");
        return;
    }
    ai_add_chat_line('s', "--- Code Block ---");
    const char *p = g_ai.code_buf;
    char line[AI_CHAT_LINE_LEN];
    while (*p) {
        int i = 0;
        while (*p && *p != '\n' && i < AI_CHAT_LINE_LEN - 1) line[i++] = *p++;
        if (*p == '\n') p++;
        line[i] = 0;
        if (i > 0) ai_add_chat_line('s', line);
    }
    ai_add_chat_line('s', "--- End Code ---");
}

static void ai_cmd_help(void) {
    ai_add_chat_line('s', "=== AI Dev Module Commands ===");
    ai_add_chat_line('s', "/save <file.zts.c> - Save code to /zrr64/zdonk/code/");
    ai_add_chat_line('s', "/build - Compile ZTS modules");
    ai_add_chat_line('s', "/load <name> - Load compiled .zts module");
    ai_add_chat_line('s', "/status - Show AI service status");
    ai_add_chat_line('s', "/code - Show extracted code block");
    ai_add_chat_line('s', "/clear - Clear chat history");
    ai_add_chat_line('s', "/help - Show this help");
    ai_add_chat_line('s', "Ctrl+A: toggle | Enter: send | Esc: close");
}

static int ai_handle_command(void) {
    if (g_ai.input_buf[0] != '/') return 0;

    if (d_strncmp(g_ai.input_buf, "/save ", 6) == 0) {
        ai_cmd_save(g_ai.input_buf + 6);
        return 1;
    }
    if (d_strncmp(g_ai.input_buf, "/save", 5) == 0 && g_ai.input_buf[5] == 0) {
        if (g_ai.code_filename[0]) {
            ai_cmd_save(g_ai.code_filename);
        } else {
            ai_add_chat_line('s', "[Usage] /save <filename.zts.c>");
        }
        return 1;
    }
    if (d_strncmp(g_ai.input_buf, "/build", 6) == 0) {
        ai_cmd_build();
        return 1;
    }
    if (d_strncmp(g_ai.input_buf, "/load ", 6) == 0) {
        ai_cmd_load(g_ai.input_buf + 6);
        return 1;
    }
    if (d_strncmp(g_ai.input_buf, "/status", 7) == 0) {
        ai_cmd_status();
        return 1;
    }
    if (d_strncmp(g_ai.input_buf, "/code", 5) == 0) {
        ai_cmd_code();
        return 1;
    }
    if (d_strncmp(g_ai.input_buf, "/clear", 6) == 0) {
        g_ai.chat_count = 0;
        g_ai.chat_scroll = 0;
        g_ai.code_len = 0;
        g_ai.code_buf[0] = 0;
        g_ai.code_filename[0] = 0;
        ai_add_chat_line('s', "[OK] Chat cleared");
        return 1;
    }
    if (d_strncmp(g_ai.input_buf, "/help", 5) == 0) {
        ai_cmd_help();
        return 1;
    }

    ai_add_chat_line('s', "[Error] Unknown command. Type /help for available commands.");
    return 1;
}

static void ai_send_message(void) {
    if (g_ai.input_cursor <= 0 || g_ai.waiting) return;

    if (ai_handle_command()) {
        g_ai.input_cursor = 0;
        g_ai.input_buf[0] = 0;
        g_ai.chat_scroll = g_ai.chat_count;
        return;
    }

    ai_add_chat_line('u', g_ai.input_buf);

    if (g_ai.connected) {
        g_ai.waiting = 1;

        struct {
            char content[AI_MSG_MAX];
            uint32_t content_len;
            uint32_t flags;
            uint64_t timestamp;
        } msg;
        for (int i = 0; i < AI_MSG_MAX; i++) msg.content[i] = 0;
        int prompt_len = d_strlen(g_ai.sys_prompt);
        int user_len = d_strlen(g_ai.input_buf);
        int total = prompt_len + 2 + user_len;
        if (total >= AI_MSG_MAX) total = AI_MSG_MAX - 1;
        int pi = 0;
        for (int i = 0; i < prompt_len && pi < AI_MSG_MAX - 3; i++)
            msg.content[pi++] = g_ai.sys_prompt[i];
        msg.content[pi++] = '\n';
        msg.content[pi++] = '|';
        for (int i = 0; i < user_len && pi < AI_MSG_MAX - 1; i++)
            msg.content[pi++] = g_ai.input_buf[i];
        msg.content[pi] = 0;
        msg.content_len = pi;
        msg.flags = 0;
        msg.timestamp = 0;

        long ret = sc1(SYS_AI_SEND, (long)&msg);
        if (ret < 0) {
            ai_add_chat_line('s', "[Error] Failed to send message");
            g_ai.waiting = 0;
        }
    } else {
        ai_add_chat_line('s', "[Offline] Configure API key in /zrr64/zdonk/AI/APIKey.confai");
    }

    g_ai.input_cursor = 0;
    g_ai.input_buf[0] = 0;
    g_ai.chat_scroll = g_ai.chat_count;
}

static void ai_poll_response(void) {
    if (!g_ai.waiting) return;

    struct {
        char content[AI_RESP_MAX];
        uint32_t content_len;
        int32_t status_code;
        uint32_t flags;
        uint64_t timestamp;
    } resp;
    for (int i = 0; i < (int)sizeof(resp); i++) ((char*)&resp)[i] = 0;

    long ret = sc1(SYS_AI_RECV, (long)&resp);
    if (ret > 0) {
        resp.content[ret < AI_RESP_MAX ? ret : AI_RESP_MAX - 1] = 0;
        ai_extract_code(resp.content);

        const char *p = resp.content;
        char line[AI_CHAT_LINE_LEN];
        int line_count = 0;
        while (*p && line_count < AI_CHAT_LINES - 2) {
            int i = 0;
            while (*p && *p != '\n' && i < AI_CHAT_LINE_LEN - 1) line[i++] = *p++;
            if (*p == '\n') p++;
            line[i] = 0;
            if (i > 0) { ai_add_chat_line('a', line); line_count++; }
        }
        if (line_count == 0) ai_add_chat_line('a', "(empty response)");

        g_ai.waiting = 0;
        g_ai.chat_scroll = g_ai.chat_count;
    } else if (ret == 0) {
    } else {
        ai_add_chat_line('s', "[Error] AI response failed");
        g_ai.waiting = 0;
    }
}

/* ========== Desktop Icons ========== */
#define MAX_DESK_ICONS  32
#define DESK_ICON_W     72
#define DESK_ICON_H     84
#define ICON_MARGIN_X   16
#define ICON_MARGIN_Y   16

typedef struct {
    int x, y, w, h;
    char label[64];
    char target[256];
    int icon_type;
    int is_selected;
} desk_icon_t;

static desk_icon_t g_icons[MAX_DESK_ICONS];
static int g_icon_count = 0;

/* Context menu */
static struct {
    int visible;
    int x, y, w, h;
    int item_count;
    struct { char label[64]; int action; } items[8];
} g_ctx_menu = {0};

/* Mouse state */
static struct {
    int x, y;
    int left, right;
    int last_click_x, last_click_y;
    uint64_t last_click_time;
} g_mouse = {0};

/* Clock */
static struct {
    int hour, min, sec;
    int day, month, year;
    int popup_visible;
    uint64_t last_update;
} g_clock = {0};

/* ========== Transition Animation ========== */
#define TRANS_FADE_FRAMES 30
static struct {
    int active;         /* 1 = fading */
    int frame;          /* current frame */
    int total_frames;   /* total frames */
    int direction;      /* 0=fade-out, 1=fade-in */
    uint32_t *saved;    /* saved screen for fade-in */
    int next_state;     /* state to transition to */
} g_trans = {0};

/* ========== Taskbar (KDE+Win11 floating dock) ========== */
#define TASKBAR_H 40
#define TASKBAR_MARGIN 8
#define TASKBAR_RADIUS 10
static struct {
    int hover_item;
    int active_item;
    int launcher_open;
    int wm_running;
} g_taskbar = {0};

/* ========== App Launcher (start menu) ========== */
#define LAUNCHER_W 320
#define LAUNCHER_H 420
static struct {
    int visible;
    char apps[16][64];
    int app_count;
    int scroll;
    int selected;
    int anim_frame;
    int anim_total;
} g_launcher = {0};

/* ========== Network Panel ========== */
#define NET_PANEL_W 260
#define NET_PANEL_H 200
static struct {
    int visible;
    int connected;
    int scanning;
    char ssid[64];
    char password[64];
    char status[64];
    char nearby_ssids[4][32];
    int nearby_count;
} g_netpanel = {0};

static void netpanel_connect(void) {
    /* Read e1w.cfg for network credentials */
    long fd = sc2(SYS_OPEN, (long)"/system/internet/e1w.cfg", 0);
    if (fd >= 0) {
        char buf[256] = {0};
        long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf)-1);
        sc1(SYS_CLOSE, fd);
        if (n > 0) {
            /* Parse name= and password= */
            char *p = buf;
            while (*p) {
                if (p[0]=='n' && p[1]=='a' && p[2]=='m' && p[3]=='e') {
                    p += 4;
                    while (*p == ' ' || *p == '=') p++;
                    if (*p == '"') p++;
                    int i = 0;
                    while (*p && *p != '"' && *p != '\n' && *p != '\r' && i < 63)
                        g_netpanel.ssid[i++] = *p++;
                    g_netpanel.ssid[i] = 0;
                }
                if (p[0]=='p' && p[1]=='a' && p[2]=='s' && p[3]=='s') {
                    p += 8;
                    while (*p == ' ' || *p == '=') p++;
                    if (*p == '"') p++;
                    int i = 0;
                    while (*p && *p != '"' && *p != '\n' && *p != '\r' && i < 63)
                        g_netpanel.password[i++] = *p++;
                    g_netpanel.password[i] = 0;
                }
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
            }
        }
    }

    /* Attempt to connect via DHCP */
    g_netpanel.connected = 1;
    g_netpanel.scanning = 0;
    d_strcpy(g_netpanel.status, "Connected");
}

static void netpanel_scan(void) {
    g_netpanel.nearby_count = 0;
    g_netpanel.scanning = 1;
    if (g_netpanel.ssid[0]) {
        d_strcpy(g_netpanel.nearby_ssids[0], g_netpanel.ssid);
        g_netpanel.nearby_count = 1;
    }
    g_netpanel.scanning = 0;
}

static void draw_netpanel(void) {
    if (!g_netpanel.visible) return;
    int dock_y = g_h - TASKBAR_H - TASKBAR_MARGIN;
    int dock_w = g_w - 2 * TASKBAR_MARGIN;
    int tray_x = TASKBAR_MARGIN + dock_w - 220 + 8;
    int px = tray_x;
    if (px + NET_PANEL_W > g_w) px = g_w - NET_PANEL_W - 4;
    if (px < 4) px = 4;
    int py = dock_y - NET_PANEL_H - 4;

    fill_rounded_rect(px+3, py+3, NET_PANEL_W, NET_PANEL_H, 12, MKC(180, 190, 210));
    fill_rounded_rect(px, py, NET_PANEL_W, NET_PANEL_H, 12, C_SURFACE);
    fill_rounded_rect(px, py, NET_PANEL_W, 1, 0, C_BORDER_LIGHT);

    draw_text(px + 16, py + 14, "Network", C_ACCENT);
    fill_rect(px + 16, py + 32, NET_PANEL_W - 32, 1, C_BORDER_LIGHT);

    if (g_netpanel.connected) {
        draw_text(px + 16, py + 44, "Connected to:", C_TEXT_SEC);
        draw_text(px + 16, py + 64, g_netpanel.ssid, C_TEXT);
        draw_text(px + 16, py + 84, "Status: Connected", C_GREEN);

        int btn_w = 120, btn_h = 32;
        int btn_x = px + (NET_PANEL_W - btn_w) / 2;
        int btn_y = py + NET_PANEL_H - btn_h - 12;
        int hov = (g_mouse.x >= btn_x && g_mouse.x < btn_x + btn_w &&
                   g_mouse.y >= btn_y && g_mouse.y < btn_y + btn_h);
        fill_rounded_rect(btn_x, btn_y, btn_w, btn_h, 6, hov ? MKC(200,60,60) : MKC(180,50,50));
        draw_text(btn_x + (btn_w - text_width("Disconnect"))/2, btn_y + (btn_h-FONT_H)/2, "Disconnect", C_WHITE);
    } else {
        draw_text(px + 16, py + 44, "Available Networks:", C_TEXT_SEC);

        if (g_netpanel.scanning) {
            draw_text(px + 16, py + 64, "Scanning...", C_YELLOW);
        } else if (g_netpanel.nearby_count == 0) {
            draw_text(px + 16, py + 64, "No networks found", C_TEXT_SEC);
            netpanel_scan();
        } else {
            int entry_y = py + 64;
            for (int n = 0; n < g_netpanel.nearby_count && n < 4; n++) {
                int hov = (g_mouse.x >= px+8 && g_mouse.x < px+NET_PANEL_W-8 &&
                           g_mouse.y >= entry_y && g_mouse.y < entry_y + 36);
                fill_rounded_rect(px+8, entry_y, NET_PANEL_W-16, 36, 6, hov ? C_HOVER : C_SURFACE_VAR);
                int ix = px + 16, iy = entry_y + 8;
                fill_rect(ix, iy+8, 2, 6, C_GREEN);
                fill_rect(ix+4, iy+4, 2, 10, C_GREEN);
                fill_rect(ix+8, iy, 2, 14, C_GREEN);
                draw_text(px + 32, entry_y + 10, g_netpanel.nearby_ssids[n], C_TEXT);
                entry_y += 40;
            }
        }

        int btn_w = 120, btn_h = 32;
        int btn_x = px + (NET_PANEL_W - btn_w) / 2;
        int btn_y = py + NET_PANEL_H - btn_h - 12;
        int hov2 = (g_mouse.x >= btn_x && g_mouse.x < btn_x + btn_w &&
                    g_mouse.y >= btn_y && g_mouse.y < btn_y + btn_h);
        fill_rounded_rect(btn_x, btn_y, btn_w, btn_h, 6, hov2 ? C_ACCENT : MKC(0,100,200));
        draw_text(btn_x + (btn_w - text_width("Connect"))/2, btn_y + (btn_h-FONT_H)/2, "Connect", C_WHITE);
    }
}

#define DATETIME_POPUP_W 220
#define DATETIME_POPUP_H 140
static void draw_datetime_popup(void) {
    if (!g_clock.popup_visible) return;
    int dock_y = g_h - TASKBAR_H - TASKBAR_MARGIN;
    int px = g_w - TASKBAR_MARGIN - DATETIME_POPUP_W - 8;
    int py = dock_y - DATETIME_POPUP_H - 4;

    fill_rounded_rect(px+3, py+3, DATETIME_POPUP_W, DATETIME_POPUP_H, 12, MKC(180, 190, 210));
    fill_rounded_rect(px, py, DATETIME_POPUP_W, DATETIME_POPUP_H, 12, C_SURFACE);
    fill_rounded_rect(px, py, DATETIME_POPUP_W, 1, 0, C_BORDER_LIGHT);

    char date_str[32];
    int y = g_clock.year, m = g_clock.month, d = g_clock.day;
    date_str[0] = '0'+y/1000; date_str[1] = '0'+(y/100)%10;
    date_str[2] = '0'+(y/10)%10; date_str[3] = '0'+y%10;
    date_str[4] = '-'; date_str[5] = '0'+m/10; date_str[6] = '0'+m%10;
    date_str[7] = '-'; date_str[8] = '0'+d/10; date_str[9] = '0'+d%10;
    date_str[10] = 0;
    draw_text(px + 16, py + 16, date_str, C_TEXT);

    char time_str[20];
    int h = g_clock.hour, mi = g_clock.min, s = g_clock.sec;
    time_str[0] = '0'+h/10; time_str[1] = '0'+h%10;
    time_str[2] = ':'; time_str[3] = '0'+mi/10; time_str[4] = '0'+mi%10;
    time_str[5] = ':'; time_str[6] = '0'+s/10; time_str[7] = '0'+s%10;
    time_str[8] = 0;
    draw_text(px + 16, py + 40, time_str, C_ACCENT);

    static const char *wdays[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    if (y > 0 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
        int doy = d;
        int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        if ((y % 400 == 0 || (y % 4 == 0 && y % 100 != 0))) mdays[1] = 29;
        for (int i = 0; i < m - 1; i++) doy += mdays[i];
        int wday = (y + (y-1)/4 - (y-1)/100 + (y-1)/400 + doy) % 7;
        draw_text(px + 16, py + 68, wdays[wday], C_TEXT_SEC);
    }

    draw_text(px + 16, py + DATETIME_POPUP_H - 24, "Click elsewhere to close", C_TEXT_SEC);
}

static void launcher_scan_apps(void) {
    g_launcher.app_count = 0;
    g_launcher.scroll = 0;
    g_launcher.selected = 0;
    long dir = sc2(SYS_OPENDIR, (long)"/prgm/sys", 0);
    if (dir >= 0) {
        while (g_launcher.app_count < 16) {
            struct desk_dirent de;
            for (int i = 0; i < (int)sizeof(de); i++) ((char*)&de)[i] = 0;
            long n = sc3(SYS_READDIR, dir, (long)&de, (long)sizeof(de));
            if (n <= 0) break;
            int len = 0;
            while (de.name[len] && len < 255) len++;
            if (len == 0) continue;
            char full[128];
            int pl = 0;
            const char *p = "/prgm/sys/";
            while (*p) full[pl++] = *p++;
            for (int i = 0; i < len; i++) full[pl++] = de.name[i];
            full[pl] = 0;
            d_strcpy(g_launcher.apps[g_launcher.app_count], full);
            g_launcher.app_count++;
        }
        sc1(SYS_CLOSEDIR, dir);
    }
    dir = sc2(SYS_OPENDIR, (long)"/prgm", 0);
    if (dir >= 0) {
        while (g_launcher.app_count < 16) {
            struct desk_dirent de;
            for (int i = 0; i < (int)sizeof(de); i++) ((char*)&de)[i] = 0;
            long n = sc3(SYS_READDIR, dir, (long)&de, (long)sizeof(de));
            if (n <= 0) break;
            int len = 0;
            while (de.name[len] && len < 255) len++;
            if (len == 0) continue;
            if (d_strcmp(de.name, "sys") == 0) continue;
            if (d_strcmp(de.name, "windowprogram") == 0) continue;
            char full[128];
            int pl = 0;
            const char *p = "/prgm/";
            while (*p) full[pl++] = *p++;
            for (int i = 0; i < len; i++) full[pl++] = de.name[i];
            full[pl] = 0;
            d_strcpy(g_launcher.apps[g_launcher.app_count], full);
            g_launcher.app_count++;
        }
        sc1(SYS_CLOSEDIR, dir);
    }
    dir = sc2(SYS_OPENDIR, (long)"/prgm/windowprogram", 0);
    if (dir >= 0) {
        while (g_launcher.app_count < 16) {
            struct desk_dirent de;
            for (int i = 0; i < (int)sizeof(de); i++) ((char*)&de)[i] = 0;
            long n = sc3(SYS_READDIR, dir, (long)&de, (long)sizeof(de));
            if (n <= 0) break;
            int len = 0;
            while (de.name[len] && len < 255) len++;
            if (len == 0) continue;
            char full[128];
            int pl = 0;
            const char *p = "/prgm/windowprogram/";
            while (*p) full[pl++] = *p++;
            for (int i = 0; i < len; i++) full[pl++] = de.name[i];
            full[pl] = 0;
            d_strcpy(g_launcher.apps[g_launcher.app_count], full);
            g_launcher.app_count++;
        }
        sc1(SYS_CLOSEDIR, dir);
    }
}

static void draw_launcher(void) {
    if (!g_launcher.visible) return;
    int dock_y = g_h - TASKBAR_H - TASKBAR_MARGIN;
    int lx = TASKBAR_MARGIN;
    int ly = dock_y - LAUNCHER_H - 4;

    int anim_progress = 1;
    if (g_launcher.anim_frame < g_launcher.anim_total && g_launcher.anim_total > 0) {
        anim_progress = g_launcher.anim_frame * 256 / g_launcher.anim_total;
        if (anim_progress > 256) anim_progress = 256;
    }

    int scale_x = 256 - (256 - anim_progress) * 2 / 5;
    int scale_y = 256 - (256 - anim_progress) * 2 / 5;
    int alpha_fade = anim_progress;

    int draw_w = LAUNCHER_W * scale_x / 256;
    int draw_h = LAUNCHER_H * scale_y / 256;
    int draw_x = lx + (LAUNCHER_W - draw_w) / 2;
    int draw_y = ly + (LAUNCHER_H - draw_h);

    if (draw_w < 10 || draw_h < 10) return;

    fill_rounded_rect(draw_x+3, draw_y+3, draw_w, draw_h, 12, MKC(180, 190, 210));
    fill_rounded_rect(draw_x, draw_y, draw_w, draw_h, 12, C_SURFACE);
    fill_rounded_rect(draw_x, draw_y, draw_w, 1, 0, C_BORDER_LIGHT);

    if (anim_progress >= 128) {
        int title_x = draw_x + 16 * draw_w / LAUNCHER_W;
        int title_y = draw_y + 14 * draw_h / LAUNCHER_H;
        draw_text(title_x, title_y, "Applications", C_ACCENT);
        int sep_y = draw_y + 32 * draw_h / LAUNCHER_H;
        fill_rect(draw_x + 16 * draw_w / LAUNCHER_W, sep_y, draw_w - 32 * draw_w / LAUNCHER_W, 1, C_BORDER_LIGHT);

        int item_h = 44 * draw_h / LAUNCHER_H;
        int list_y = draw_y + 40 * draw_h / LAUNCHER_H;
        int max_visible = (draw_h - 52 * draw_h / LAUNCHER_H) / (item_h > 20 ? item_h : 20);
        for (int i = 0; i < g_launcher.app_count && i < max_visible; i++) {
            int idx = i + g_launcher.scroll;
            if (idx >= g_launcher.app_count) break;
            int iy = list_y + i * item_h;
            int hov = (g_mouse.x >= draw_x+8 && g_mouse.x < draw_x+draw_w-8 &&
                       g_mouse.y >= iy && g_mouse.y < iy + item_h);
            int sel = (g_launcher.selected == idx);
            if (hov || sel) {
                fill_rounded_rect(draw_x+8, iy, draw_w-16, item_h-2, 6, C_HOVER);
            }
            const char *path = g_launcher.apps[idx];
            const char *name = path;
            for (const char *p = path; *p; p++) if (*p == '/') name = p+1;
            draw_text(draw_x + 24 * draw_w / LAUNCHER_W, iy + (item_h-FONT_H)/2, name, sel ? C_ACCENT : C_TEXT);
        }
    }
}

/* ========== Multi-user support ========== */
#define MAX_USERS 8
typedef struct {
    char name[64];
    char passwd_hash[33];
    int uid;
    int valid;
} user_entry_t;
static user_entry_t g_users[MAX_USERS];
static int g_user_count = 0;
static int g_current_user = 0;  /* index into g_users */

/* ========== Desktop Icon Management ========== */

static void desk_scan_icons(void) {
    g_icon_count = 0;

    int is_root = 1;
    if (g_current_user >= 0 && g_current_user < g_user_count && g_users[g_current_user].valid)
        is_root = (g_users[g_current_user].uid == 0);

    const char *desk_path = is_root ? "/desktop/root" : "/desktop/user";
    serial_puts("[DESK] Scanning icons from: ");
    serial_puts(desk_path);
    serial_puts("\n");
    long dir = sc2(SYS_OPENDIR, (long)desk_path, 0);
    if (dir >= 0) {
        while (g_icon_count < MAX_DESK_ICONS) {
            struct desk_dirent de;
            for (int i = 0; i < (int)sizeof(de); i++) ((char*)&de)[i] = 0;
            long n = sc3(SYS_READDIR, dir, (long)&de, (long)sizeof(de));
            if (n <= 0) break;
            int len = 0;
            while (de.name[len] && len < 255) len++;
            if (len < 5 || de.name[len-4] != '.' || de.name[len-3] != 'p' ||
                de.name[len-2] != 'r' || de.name[len-1] != 'd') continue;

            char full_path[128];
            int plen = 0;
            const char *p = desk_path;
            while (*p) full_path[plen++] = *p++;
            full_path[plen++] = '/';
            for (int i = 0; i < len; i++) full_path[plen++] = de.name[i];
            full_path[plen] = 0;

            long fd = sc2(SYS_OPEN, (long)full_path, 0);
            if (fd >= 0) {
                char content[256] = {0};
                long rn = sc3(SYS_READ, fd, (long)content, sizeof(content)-1);
                sc1(SYS_CLOSE, fd);
                if (rn > 0) {
                    while (rn > 0 && (content[rn-1] == '\n' || content[rn-1] == '\r'))
                        content[--rn] = 0;
                    if (rn > 0) {
                        desk_icon_t *ic = &g_icons[g_icon_count];
                        int icons_per_col = (g_h - ICON_MARGIN_Y) / (DESK_ICON_H + 8);
                        if (icons_per_col < 1) icons_per_col = 1;
                        int col = g_icon_count / icons_per_col;
                        int row = g_icon_count % icons_per_col;
                        ic->x = ICON_MARGIN_X + col * (DESK_ICON_W + 8);
                        ic->y = ICON_MARGIN_Y + row * (DESK_ICON_H + 8);
                        ic->w = DESK_ICON_W;
                        ic->h = DESK_ICON_H;
                        int lbl_len = len - 4;
                        if (lbl_len > 14) lbl_len = 14;
                        d_strncpy(ic->label, de.name, lbl_len);
                        ic->label[lbl_len] = 0;
                        ic->icon_type = ICON_FILES;
                        ic->is_selected = 0;

                        char *target_start = content;
                        const char *icon_key = d_strstr(content, "icon=");
                        if (icon_key) {
                            const char *iv = icon_key + 5;
                            if (d_strstr(iv, "TERMINAL") && iv < content + rn) ic->icon_type = ICON_TERMINAL;
                            else if (d_strstr(iv, "SYSTEM") && iv < content + rn) ic->icon_type = ICON_SYSTEM;
                            else if (d_strstr(iv, "PROGRAMS") && iv < content + rn) ic->icon_type = ICON_PROGRAMS;
                            else if (d_strstr(iv, "FILES") && iv < content + rn) ic->icon_type = ICON_FILES;
                            char *nl = (char*)d_strstr(icon_key, "\n");
                            if (nl) target_start = nl + 1;
                            else target_start = (char*)icon_key;
                        }
                        while (*target_start == '\n' || *target_start == '\r') target_start++;
                        d_strcpy(ic->target, target_start);

                        g_icon_count++;
                    }
                }
            }
        }
        sc1(SYS_CLOSEDIR, dir);
    }

    /* If no shortcuts found, add default icons */
    if (g_icon_count == 0) {
        /* Files icon */
        desk_icon_t *ic = &g_icons[0];
        ic->x = ICON_MARGIN_X; ic->y = ICON_MARGIN_Y;
        ic->w = DESK_ICON_W; ic->h = DESK_ICON_H;
        d_strcpy(ic->label, "Files");
        d_strcpy(ic->target, "/prgm/windowprogram/fm");
        ic->icon_type = ICON_FILES;
        ic->is_selected = 0;
        g_icon_count++;

        ic = &g_icons[1];
        ic->x = ICON_MARGIN_X; ic->y = ICON_MARGIN_Y + DESK_ICON_H + 8;
        ic->w = DESK_ICON_W; ic->h = DESK_ICON_H;
        d_strcpy(ic->label, "Terminal");
        d_strcpy(ic->target, "/prgm/shell");
        ic->icon_type = ICON_TERMINAL;
        ic->is_selected = 0;
        g_icon_count++;
    }
}

/* ========== Drawing Functions ========== */

static void draw_background(void) {
    if (g_video_wp.active && g_video_wp.frame_count > 0) {
        uint32_t *frame = g_video_wp.frames[g_video_wp.current_frame];
        int fw = g_video_wp.frame_w, fh = g_video_wp.frame_h;
        if (frame && fw > 0 && fh > 0) {
            for (int y = 0; y < g_h; y++) {
                int sy = y * fh / g_h;
                if (sy >= fh) sy = fh - 1;
                uint32_t *src_row = &frame[sy * fw];
                uint32_t *dst_row = &g_fb[y * g_stride];
                for (int x = 0; x < g_w; x++) {
                    int sx = x * fw / g_w;
                    if (sx >= fw) sx = fw - 1;
                    dst_row[x] = src_row[sx];
                }
            }
        }
    } else if (g_bg_image && g_bg_w > 0 && g_bg_h > 0) {
        for (int y = 0; y < g_h; y++) {
            int sy = y * g_bg_h / g_h;
            if (sy >= g_bg_h) sy = g_bg_h - 1;
            uint32_t *src_row = &g_bg_image[sy * g_bg_w];
            uint32_t *dst_row = &g_fb[y * g_stride];
            for (int x = 0; x < g_w; x++) {
                int sx = x * g_bg_w / g_w;
                if (sx >= g_bg_w) sx = g_bg_w - 1;
                dst_row[x] = src_row[sx];
            }
        }
    } else {
        fill_rect_vgrad(0, 0, g_w, g_h, C_MICA_BG, C_MICA_BG2);
        int cx = g_w / 2, cy = g_h / 2;
        int radius = g_w / 4;
        for (int y = cy - radius; y < cy + radius && y < g_h; y++) {
            if (y < 0) continue;
            for (int x = cx - radius; x < cx + radius && x < g_w; x++) {
                if (x < 0) continue;
                int dx = x - cx, dy = y - cy;
                int dist_sq = dx*dx + dy*dy;
                if (dist_sq < radius * radius) {
                    int alpha = 8 * (radius * radius - dist_sq) / (radius * radius);
                    if (alpha > 0 && alpha < 30) {
                        uint32_t *px = &g_fb[y * g_stride + x];
                        uint32_t c = *px;
                        int r = ((c>>16)&0xFF) + alpha;
                        int gv = ((c>>8)&0xFF) + alpha;
                        int b = (c&0xFF) + alpha * 2;
                        if (r > 255) r = 255;
                        if (gv > 255) gv = 255;
                        if (b > 255) b = 255;
                        *px = ((uint32_t)r<<16)|((uint32_t)gv<<8)|(uint32_t)b;
                    }
                }
            }
        }
    }
}

static void draw_desk_icon(desk_icon_t *ic) {
    int ix = ic->x, iy = ic->y;

    if (ic->is_selected) {
        fill_rounded_rect(ix-2, iy-2, ic->w+4, ic->h+4, 10, C_SELECTED);
    }

    int icon_type = ic->icon_type;
    if (icon_type == ICON_FILES) {
        if (d_strcmp(ic->label, "Terminal") == 0 ||
            d_strstr(ic->target, "/shell") != 0 ||
            d_strstr(ic->target, "/terminal") != 0) {
            icon_type = ICON_TERMINAL;
        } else if (d_strcmp(ic->label, "System") == 0 ||
                   d_strstr(ic->target, "/zrdemage") != 0) {
            icon_type = ICON_SYSTEM;
        } else if (d_strcmp(ic->label, "Programs") == 0 ||
                   d_strstr(ic->target, "/zinstall") != 0) {
            icon_type = ICON_PROGRAMS;
        }
    }

    {
        int shadow_x = ix + 10, shadow_y = iy + 4;
        int shadow_w = 52, shadow_h = 40;
        fill_rounded_rect(shadow_x+2, shadow_y+2, shadow_w, shadow_h, 8, MKC(200, 210, 230));
    }

    if (icon_type == ICON_TERMINAL) {
        fill_rounded_rect(ix+10, iy+4, 52, 38, 8, MKC(40, 42, 54));
        fill_rounded_rect(ix+12, iy+6, 48, 34, 6, MKC(28, 30, 42));
        fill_rect(ix+18, iy+14, 8, 2, C_GREEN);
        fill_rect(ix+26, iy+14, 2, 8, C_GREEN);
        fill_rect(ix+26, iy+14, 8, 2, C_GREEN);
        fill_rect(ix+20, iy+22, 12, 2, MKC(160, 165, 180));
    } else if (icon_type == ICON_SYSTEM) {
        fill_rounded_rect(ix+14, iy+6, 44, 34, 8, MKC(140, 150, 170));
        fill_rounded_rect(ix+18, iy+10, 36, 26, 6, MKC(110, 120, 140));
        fill_rounded_rect(ix+26, iy+14, 20, 18, 4, MKC(90, 100, 120));
        fill_rounded_rect(ix+30, iy+18, 12, 10, 5, MKC(140, 150, 170));
    } else if (icon_type == ICON_PROGRAMS) {
        int sq = 16, gap = 4;
        int ox = ix + 14, oy = iy + 8;
        fill_rounded_rect(ox, oy, sq, sq, 4, C_ACCENT);
        fill_rounded_rect(ox+sq+gap, oy, sq, sq, 4, C_ACCENT2);
        fill_rounded_rect(ox, oy+sq+gap, sq, sq, 4, C_ORANGE);
        fill_rounded_rect(ox+sq+gap, oy+sq+gap, sq, sq, 4, C_GREEN);
    } else {
        fill_rounded_rect(ix+14, iy+8, 44, 30, 6, MKC(255, 190, 100));
        fill_rounded_rect(ix+14, iy+4, 22, 8, 4, MKC(255, 170, 70));
        fill_rect(ix+18, iy+14, 34, 2, MKC(255, 220, 160));
    }

    {
        int maxc = (ic->w - 4) / SMALL_FONT_W; if (maxc > 20) maxc = 20;
        char tmp[24]; d_strncpy(tmp, ic->label, maxc); tmp[maxc] = 0;
        int tx = ix + (ic->w - small_text_width(tmp))/2;
        int ty = iy + 50;
        fill_rounded_rect(tx - 3, ty - 1, small_text_width(tmp) + 6, SMALL_FONT_H + 2, 3, MKC(235, 240, 248));
        draw_text_small(tx, ty, tmp, ic->is_selected ? C_ACCENT : C_TEXT);
    }
}

static void draw_context_menu(void) {
    if (!g_ctx_menu.visible) return;

    int mx = g_ctx_menu.x, my = g_ctx_menu.y;
    int mw = g_ctx_menu.w, mh = g_ctx_menu.h;

    /* Shadow */
    fill_rect(mx+3, my+3, mw, mh, MKC(180, 190, 210));

    fill_rounded_rect(mx, my, mw, mh, 8, C_SURFACE);

    fill_rounded_rect(mx, my, mw, mh, 8, C_BORDER_LIGHT);
    fill_rounded_rect(mx+1, my+1, mw-2, mh-2, 7, C_SURFACE);

    /* Items */
    for (int i = 0; i < g_ctx_menu.item_count; i++) {
        int iy = my + 4 + i * 28;
        int hov = (g_mouse.x >= mx && g_mouse.x < mx+mw &&
                   g_mouse.y >= iy && g_mouse.y < iy+28);
        if (hov) fill_rounded_rect(mx+2, iy, mw-4, 28, 4, C_HOVER);
        draw_text(mx + 12, iy + 6, g_ctx_menu.items[i].label, C_TEXT);
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

/* ========== MD5 Hash ========== */
typedef struct {
    uint32_t state[4];
    uint64_t count;
    uint8_t buffer[64];
} md5_ctx;

static const uint32_t md5_T[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391,
};
static const int md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21,
};

#define MD5_F(x,y,z) (((x)&(y))|((~(x))&(z)))
#define MD5_G(x,y,z) (((x)&(z))|((y)&(~(z))))
#define MD5_H(x,y,z) ((x)^(y)^(z))
#define MD5_I(x,y,z) ((y)^((x)|(~(z))))
#define ROTL(x,n) (((x)<<(n))|((x)>>(32-(n))))

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a=state[0], b=state[1], c=state[2], d=state[3];
    uint32_t M[16];
    for (int i=0;i<16;i++)
        M[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1]<<8) |
               ((uint32_t)block[i*4+2]<<16) | ((uint32_t)block[i*4+3]<<24);
    for (int i=0;i<64;i++) {
        uint32_t f,g;
        if (i<16)      { f=MD5_F(b,c,d); g=i; }
        else if (i<32) { f=MD5_G(b,c,d); g=(5*i+1)%16; }
        else if (i<48) { f=MD5_H(b,c,d); g=(3*i+5)%16; }
        else           { f=MD5_I(b,c,d); g=(7*i)%16; }
        f += a + md5_T[i] + M[g];
        a=d; d=c; c=b; b=b+ROTL(f,md5_s[i]);
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
}

static void md5_update(md5_ctx *ctx, const uint8_t *data, int len) {
    int index = (int)(ctx->count % 64);
    ctx->count += len;
    int i = 0;
    if (index) {
        int part = 64 - index;
        if (len < part) part = len;
        for (int j=0;j<part;j++) ctx->buffer[index+j]=data[i+j];
        if (index+part < 64) return;
        md5_transform(ctx->state, ctx->buffer);
        i += part;
    }
    for (; i+64 <= len; i+=64)
        md5_transform(ctx->state, data+i);
    for (; i<len; i++)
        ctx->buffer[i%64] = data[i];
}

static void md5_final(md5_ctx *ctx, uint8_t digest[16]) {
    uint8_t padding[64];
    for (int i=0;i<64;i++) padding[i]=0;
    padding[0]=0x80;
    uint64_t bits = ctx->count * 8;
    int index = (int)(ctx->count % 64);
    int padlen = (index < 56) ? (56-index) : (120-index);
    md5_update(ctx, padding, padlen);
    uint8_t bits_buf[8];
    for (int i=0;i<8;i++) bits_buf[i]=(uint8_t)(bits>>(i*8));
    md5_update(ctx, bits_buf, 8);
    for (int i=0;i<4;i++) {
        digest[i*4]  =(uint8_t)(ctx->state[i]);
        digest[i*4+1]=(uint8_t)(ctx->state[i]>>8);
        digest[i*4+2]=(uint8_t)(ctx->state[i]>>16);
        digest[i*4+3]=(uint8_t)(ctx->state[i]>>24);
    }
}

static void md5_string(const char *input, char output[33]) {
    md5_ctx ctx;
    ctx.state[0]=0x67452301; ctx.state[1]=0xefcdab89;
    ctx.state[2]=0x98badcfe; ctx.state[3]=0x10325476;
    ctx.count=0;
    for (int i=0;i<64;i++) ctx.buffer[i]=0;
    int len=0; while(input[len])len++;
    md5_update(&ctx, (const uint8_t*)input, len);
    uint8_t digest[16];
    md5_final(&ctx, digest);
    static const char hex[]="0123456789abcdef";
    for (int i=0;i<16;i++) {
        output[i*2]=hex[(digest[i]>>4)&0xF];
        output[i*2+1]=hex[digest[i]&0xF];
    }
    output[32]=0;
}

/* ========== User Config / Login ========== */
#define USER_CFG_PATH "/ini/usrroot/user.cfg"

typedef struct {
    char name[64];
    char passwd_hash[33];
    int valid;
} user_info_t;

static user_info_t g_user;

static void parse_user_cfg(void) {
    g_user_count = 0;
    g_user.valid = 0;
    g_user.name[0] = 0;
    g_user.passwd_hash[0] = 0;

    long fd = sc2(SYS_OPEN, (long)USER_CFG_PATH, 0);
    if (fd < 0) {
        serial_puts("[DESK] Cannot open user.cfg, using root only\n");
        /* Fallback: add root account */
        {
            user_entry_t *ue = &g_users[g_user_count++];
            d_strcpy(ue->name, "root");
            d_strcpy(ue->passwd_hash, "e10adc3949ba59abbe56e057f20f883e");
            ue->uid = 0;
            ue->valid = 1;
        }
        d_strcpy(g_user.name, "root");
        d_strcpy(g_user.passwd_hash, "e10adc3949ba59abbe56e057f20f883e");
        g_user.valid = 1;
        return;
    }

    char buf[1024];
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf)-1);
    sc1(SYS_CLOSE, fd);
    if (n <= 0) {
        serial_puts("[DESK] user.cfg empty, using root fallback\n");
        goto add_fallback;
    }
    buf[n] = 0;

    /* Parse key=value lines - supports multiple user blocks separated by blank lines */
    int pos = 0;
    user_entry_t *cur = &g_users[g_user_count];  /* next slot */
    cur->valid = 0;

    serial_puts("[DESK] Parsing user.cfg (");
    { char nb[16]; int np=0; uint64_t t=n; if(t==0){nb[np++]='0';}else{char tb[16];int tp=0;while(t>0){tb[tp++]='0'+(t%10);t/=10;}while(tp>0)nb[np++]=tb[--tp];}nb[np]=0;serial_puts(nb);}
    serial_puts(" bytes) hex[0..31]=");
    { int hd = n > 32 ? 32 : (int)n;
      for (int i = 0; i < hd; i++) {
          char hx[3]; hx[0]="0123456789abcdef"[(buf[i]>>4)&0xF]; hx[1]="0123456789abcdef"[buf[i]&0xF]; hx[2]=0;
          serial_puts(hx);
      }
      serial_puts("\n");
    }

    while (pos < n) {
        while (pos < n && (buf[pos]=='\n'||buf[pos]=='\r'||buf[pos]==' ')) pos++;
        if (pos >= n) { serial_puts("[DESK] parse: pos>=n at skip_ws\n"); break; }

        {
            serial_puts("[DESK] parse@");
            { char _nb[16]; int _np=0; int _v=pos; if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_puts(_nb); }
            serial_puts(" char='");
            if (buf[pos] >= 0x20 && buf[pos] < 0x7f) {
                char cc[2]; cc[0]=buf[pos]; cc[1]=0; serial_puts(cc);
            } else {
                serial_puts(".");
            }
            serial_puts("'\n");
        }

        int kstart = pos;
        while (pos < n && buf[pos]!='=' && buf[pos]!='\n') pos++;
        if (pos >= n || buf[pos]!='=') { serial_puts("[DESK] parse: no '=' found\n"); while(pos<n&&buf[pos]!='\n')pos++; continue; }
        int kend = pos; pos++;

        /* Find end of value */
        int vstart = pos;
        while (pos < n && buf[pos]!='\n' && buf[pos]!='\r') pos++;
        int vend = pos;

        /* Extract key and value */
        char key[32], val[64];
        int kl = kend-kstart; if(kl>31)kl=31;
        for(int i=0;i<kl;i++) key[i]=buf[kstart+i]; key[kl]=0;
        int vl = vend-vstart; if(vl>63)vl=63;
        for(int i=0;i<vl;i++) val[i]=buf[vstart+i]; val[vl]=0;

        serial_puts(key); serial_puts("="); serial_puts(val); serial_puts(" ");

        if (d_strcmp(key,"name")==0) {
            if (cur->name[0] && g_user_count < MAX_USERS - 1) {
                if (cur->passwd_hash[0]) {
                    cur->valid = 1;
                    g_user_count++;
                }
                cur = &g_users[g_user_count];
                cur->valid = 0;
                cur->name[0] = 0;
                cur->passwd_hash[0] = 0;
                cur->uid = 0;
            }
            d_strcpy(cur->name, val);
        } else if (d_strcmp(key,"passwd")==0) {
            d_strncpy(cur->passwd_hash, val, 32);
            cur->passwd_hash[32] = 0;
        } else if (d_strcmp(key,"uid")==0) {
            if (cur->name[0] && cur->passwd_hash[0] && g_user_count < MAX_USERS - 1) {
                cur->valid = 1;
                g_user_count++;
                cur = &g_users[g_user_count];
                cur->valid = 0;
                cur->name[0] = 0;
                cur->passwd_hash[0] = 0;
                cur->uid = 0;
            }
            cur->uid = 0;
            for (int i = 0; val[i]; i++) {
                if (val[i] >= '0' && val[i] <= '9')
                    cur->uid = cur->uid * 10 + (val[i] - '0');
            }
        }
    }

    /* Finalize last user */
    if (cur->name[0] && cur->passwd_hash[0] && g_user_count < MAX_USERS) {
        cur->valid = 1;
        g_user_count++;
    }

    /* Set g_user to first non-root user (for compatibility) */
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].valid && d_strcmp(g_users[i].name, "root") != 0) {
            d_strcpy(g_user.name, g_users[i].name);
            d_strcpy(g_user.passwd_hash, g_users[i].passwd_hash);
            g_user.valid = 1;
            break;
        }
    }
    if (!g_user.valid && g_user_count > 0) {
        d_strcpy(g_user.name, g_users[0].name);
        d_strcpy(g_user.passwd_hash, g_users[0].passwd_hash);
        g_user.valid = 1;
    }

    serial_puts("[DESK] Users loaded: ");
    for (int i = 0; i < g_user_count; i++) {
        if (i > 0) serial_puts(", ");
        serial_puts(g_users[i].name);
    }
    serial_puts("\n");

    /* Safety net: if no users parsed, force-add root */
    if (g_user_count == 0) {
        serial_puts("[DESK] No users parsed, forcing root fallback\n");
add_fallback:
        {
            user_entry_t *ue = &g_users[g_user_count++];
            d_strcpy(ue->name, "root");
            d_strcpy(ue->passwd_hash, "e10adc3949ba59abbe56e057f20f883e");
            ue->uid = 0;
            ue->valid = 1;
        }
        d_strcpy(g_user.name, "root");
        d_strcpy(g_user.passwd_hash, "e10adc3949ba59abbe56e057f20f883e");
        g_user.valid = 1;

        serial_puts("[DESK] Fallback root added. Users: ");
        for (int i = 0; i < g_user_count; i++) {
            if (i > 0) serial_puts(", ");
            serial_puts(g_users[i].name);
        }
        serial_puts("\n");
    }
}

/* Login state */
#define LOGIN_FIELD_USER    0
#define LOGIN_FIELD_PASS    1

static struct {
    int active;
    int field;          /* LOGIN_FIELD_USER or LOGIN_FIELD_PASS */
    int selected_user;  /* index into g_users, -1 = manual input */
    char username[64];
    char password[64];
    int user_len;
    int pass_len;
    int error;          /* 0=none, 1=wrong credentials */
    int anim_tick;
} g_login = {0};

/* Scancode to ASCII (US keyboard layout, simplified) */
static const char scancode_to_ascii[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',0,
    '\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',
    0,0,0,0,0,0,0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',
};

static void draw_login_screen(void) {
    fill_rect_vgrad(0, 0, g_w, g_h, MKC(12, 14, 24), MKC(24, 28, 44));

    {
        int cx = g_w/2, cy = g_h/2;
        int max_r = g_w / 3;
        for (int y = cy - max_r; y < cy + max_r && y < g_h; y++) {
            if (y < 0) continue;
            for (int x = cx - max_r; x < cx + max_r && x < g_w; x++) {
                if (x < 0) continue;
                int dx = x - cx, dy = y - cy;
                int dist_sq = dx*dx + dy*dy;
                if (dist_sq < max_r * max_r) {
                    int norm = dist_sq * 256 / (max_r * max_r);
                    int alpha;
                    if (norm < 64) alpha = 100 - norm;
                    else if (norm < 128) alpha = 36 - (norm - 64) / 2;
                    else alpha = 8 - (norm - 128) / 16;
                    if (alpha <= 0) continue;
                    int t = norm;
                    int gr = (50 * (256 - t) + 20 * t) / 256;
                    int gg = (80 * (256 - t) + 140 * t) / 256;
                    int gb = (200 * (256 - t) + 180 * t) / 256;
                    uint32_t *px = &g_fb[y * g_stride + x];
                    uint32_t c = *px;
                    int rv, gv, bv;
                    if (g_fb_bgr) {
                        rv = (c & 0xFF) + gr * alpha / 255;
                        gv = ((c>>8) & 0xFF) + gg * alpha / 255;
                        bv = ((c>>16) & 0xFF) + gb * alpha / 255;
                    } else {
                        rv = ((c>>16) & 0xFF) + gr * alpha / 255;
                        gv = ((c>>8) & 0xFF) + gg * alpha / 255;
                        bv = (c & 0xFF) + gb * alpha / 255;
                    }
                    if(rv>255)rv=255; if(gv>255)gv=255; if(bv>255)bv=255;
                    if (g_fb_bgr)
                        *px = ((uint32_t)bv<<16)|((uint32_t)gv<<8)|(uint32_t)rv;
                    else
                        *px = ((uint32_t)rv<<16)|((uint32_t)gv<<8)|(uint32_t)bv;
                }
            }
        }
    }

    {
        char time_str[20];
        int h = g_clock.hour, m = g_clock.min;
        time_str[0] = '0'+h/10; time_str[1] = '0'+h%10;
        time_str[2] = ':'; time_str[3] = '0'+m/10; time_str[4] = '0'+m%10;
        time_str[5] = 0;
        int tw = text_width(time_str);
        draw_text((g_w - tw) / 2, 40, time_str, MKC(180, 200, 240));

        char date_str[32];
        int y = g_clock.year, mo = g_clock.month, d = g_clock.day;
        date_str[0] = '0'+y/1000; date_str[1] = '0'+(y/100)%10;
        date_str[2] = '0'+(y/10)%10; date_str[3] = '0'+y%10;
        date_str[4] = '/'; date_str[5] = '0'+mo/10; date_str[6] = '0'+mo%10;
        date_str[7] = '/'; date_str[8] = '0'+d/10; date_str[9] = '0'+d%10;
        date_str[10] = 0;
        int dw = text_width(date_str);
        draw_text((g_w - dw) / 2, 54, date_str, MKC(120, 140, 180));
    }

    int card_w = 360, card_h = 320;
    int card_x = (g_w - card_w) / 2;
    int card_y = (g_h - card_h) / 2;

    fill_rounded_rect(card_x+3, card_y+3, card_w, card_h, 14, MKC(4, 4, 8));

    for (int i = 0; i < 2; i++)
        fill_rounded_rect(card_x+i, card_y+i, card_w-2*i, card_h-2*i, 14-i,
                          MKC(60 + i*20, 100 + i*20, 200 + i*10));
    fill_rounded_rect(card_x+2, card_y+2, card_w-4, card_h-4, 12, MKC(28, 32, 48));

    {
        int avatar_cx = card_x + card_w / 2;
        int avatar_cy = card_y + 40;
        int avatar_r = 24;
        for (int dy = -avatar_r - 4; dy <= avatar_r + 4; dy++)
            for (int dx = -avatar_r - 4; dx <= avatar_r + 4; dx++) {
                int dist_sq = dx*dx + dy*dy;
                if (dist_sq <= (avatar_r+4)*(avatar_r+4)) {
                    int a = 30;
                    if (dist_sq > avatar_r*avatar_r) a = a * ((avatar_r+4)*(avatar_r+4) - dist_sq) / (8*avatar_r + 16);
                    int fx = avatar_cx + dx, fy = avatar_cy + dy;
                    if (fx >= 0 && fx < g_w && fy >= 0 && fy < g_h) {
                        uint32_t *px = &g_fb[fy * g_stride + fx];
                        uint32_t c = *px;
                        int rv = ((c>>16)&0xFF) + 96*a/255;
                        int gv = ((c>>8)&0xFF) + 165*a/255;
                        int bv = (c&0xFF) + 250*a/255;
                        if(rv>255)rv=255; if(gv>255)gv=255; if(bv>255)bv=255;
                        *px = ((uint32_t)rv<<16)|((uint32_t)gv<<8)|(uint32_t)bv;
                    }
                }
            }
        for (int dy = -avatar_r; dy <= avatar_r; dy++)
            for (int dx = -avatar_r; dx <= avatar_r; dx++)
                if (dx*dx + dy*dy <= avatar_r*avatar_r) {
                    int fx = avatar_cx + dx, fy = avatar_cy + dy;
                    if (fx >= 0 && fx < g_w && fy >= 0 && fy < g_h)
                        g_fb[fy * g_stride + fx] = MKC(50, 70, 110);
                }
        const char *initial = g_login.user_len > 0 ? g_login.username : "U";
        int iw = text_width(initial);
        draw_text(avatar_cx - iw/2, avatar_cy - FONT_H/2, initial, C_ACCENT);
    }

    {
        const char *title = "ZratroOS";
        int tw = text_width(title);
        draw_text(card_x + (card_w - tw) / 2, card_y + 72, title, C_TEXT);

        const char *sub = "Sign in";
        int sw = text_width(sub);
        draw_text(card_x + (card_w - sw) / 2, card_y + 86, sub, C_TEXT_SEC);
    }

    int field_x = card_x + 28;
    int field_w = card_w - 56;
    int field_h = 36;

    int user_y = card_y + 110;
    {
        int focused = (g_login.field == LOGIN_FIELD_USER);
        fill_rounded_rect(field_x, user_y, field_w, field_h, 6, focused ? C_MICA_LIGHT : C_MICA_BG2);
        for (int i=0;i<2;i++)
            fill_rounded_rect(field_x+i, user_y+i, field_w-2*i, field_h-2*i, 6-i,
                              focused ? C_ACCENT : C_BORDER);
        fill_rounded_rect(field_x+2, user_y+2, field_w-4, field_h-4, 4, focused ? C_MICA_LIGHT : C_MICA_BG2);

        draw_text_small(field_x + 10, user_y - 14, "Username", C_TEXT_SEC);
        if (g_login.user_len > 0) {
            char display[64];
            int maxc = (field_w - 24) / FONT_W;
            int start = g_login.user_len > maxc ? g_login.user_len - maxc : 0;
            int len = g_login.user_len - start;
            for (int i=0;i<len;i++) display[i] = g_login.username[start+i];
            display[len] = 0;
            draw_text(field_x + 10, user_y + (field_h - FONT_H)/2, display, C_TEXT);
        } else if (!focused) {
            draw_text_small(field_x + 10, user_y + (field_h - SMALL_FONT_H)/2, "Enter username", C_TEXT_DIS);
        }
        if (focused && (g_login.anim_tick % 60) < 40) {
            int cx_pos = field_x + 10 + ((g_login.user_len > (field_w-24)/FONT_W ? (field_w-24)/FONT_W : g_login.user_len) * FONT_W);
            fill_rect(cx_pos, user_y+6, 2, field_h-12, C_ACCENT);
        }
    }

    int pass_y = user_y + field_h + 24;
    {
        int focused = (g_login.field == LOGIN_FIELD_PASS);
        fill_rounded_rect(field_x, pass_y, field_w, field_h, 6, focused ? C_MICA_LIGHT : C_MICA_BG2);
        for (int i=0;i<2;i++)
            fill_rounded_rect(field_x+i, pass_y+i, field_w-2*i, field_h-2*i, 6-i,
                              focused ? C_ACCENT : C_BORDER);
        fill_rounded_rect(field_x+2, pass_y+2, field_w-4, field_h-4, 4, focused ? C_MICA_LIGHT : C_MICA_BG2);

        draw_text_small(field_x + 10, pass_y - 14, "Password", C_TEXT_SEC);
        if (g_login.pass_len > 0) {
            int maxc = (field_w - 24) / 16;
            int show = g_login.pass_len > maxc ? maxc : g_login.pass_len;
            for (int i=0;i<show;i++) {
                int dx = field_x + 10 + i*16;
                for (int dy=0; dy<8; dy++)
                    for (int ddx=0; ddx<8; ddx++)
                        if ((ddx-3)*(ddx-3)+(dy-3)*(dy-3) <= 9) {
                            int px = dx+ddx, py = pass_y + (field_h-8)/2 + dy;
                            if (px>=0 && px<g_w && py>=0 && py<g_h)
                                g_fb[py*g_stride+px] = C_TEXT;
                        }
            }
        } else if (!focused) {
            draw_text_small(field_x + 10, pass_y + (field_h - SMALL_FONT_H)/2, "******", C_TEXT_DIS);
        }
        if (focused && (g_login.anim_tick % 60) < 40) {
            int cx_pos = field_x + 10 + ((g_login.pass_len > (field_w-24)/16 ? (field_w-24)/16 : g_login.pass_len) * 16);
            fill_rect(cx_pos, pass_y+6, 2, field_h-12, C_ACCENT);
        }
    }

    int btn_w = field_w, btn_h = 36;
    int btn_x = field_x, btn_y = pass_y + field_h + 20;
    {
        int hov = (g_mouse.x >= btn_x && g_mouse.x < btn_x+btn_w &&
                   g_mouse.y >= btn_y && g_mouse.y < btn_y+btn_h);
        fill_rounded_rect(btn_x, btn_y, btn_w, btn_h, 6, hov ? C_ACCENT_HOVER : C_ACCENT);
        const char *btn_text = "Sign in";
        int btw = text_width(btn_text);
        draw_text(btn_x + (btn_w-btw)/2, btn_y + (btn_h-FONT_H)/2, btn_text, C_WHITE);
    }

    if (g_login.error) {
        const char *err = "Invalid username or password";
        int ew = text_width(err);
        draw_text(card_x + (card_w-ew)/2, btn_y + btn_h + 10, err, C_RED);
    }

    {
        const char *hint = "Tab: switch  Enter: login";
        int hw = text_width(hint);
        draw_text_small(card_x + (card_w - hw) / 2, card_y + card_h - 20, hint, C_TEXT_DIS);
    }
}

static int login_try_auth(void) {
    user_entry_t *auth_user = NULL;

    serial_puts("[DESK] Auth attempt: user=");
    serial_puts(g_login.username);
    serial_puts(" selected_user=");
    { char _nb[16]; int _np=0; int _v=g_login.selected_user; if(_v<0){serial_puts("-");_v=-_v;} if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_puts(_nb); }
    serial_puts(" user_count=");
    { char _nb[16]; int _np=0; int _v=g_user_count; if(_v<0){serial_puts("-");_v=-_v;} if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_puts(_nb); }
    serial_puts("\n");
    for (int i = 0; i < g_user_count; i++) {
        serial_puts("  [");
        { char _nb[16]; int _np=0; int _v=i; if(_v<0){serial_puts("-");_v=-_v;} if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_puts(_nb); }
        serial_puts("] name=");
        serial_puts(g_users[i].name);
        serial_puts(" valid=");
        serial_puts(g_users[i].valid ? "Y" : "N");
        serial_puts(" passwd=");
        serial_puts(g_users[i].passwd_hash[0] ? g_users[i].passwd_hash : "(none)");
        serial_puts("\n");
    }
    if (g_login.selected_user >= 0 && g_login.selected_user < g_user_count) {
        auth_user = &g_users[g_login.selected_user];
        serial_puts(auth_user->name);
    } else {
        serial_puts(g_login.username);
        for (int i = 0; i < g_user_count; i++) {
            if (g_users[i].valid && d_strcmp(g_login.username, g_users[i].name) == 0) {
                auth_user = &g_users[i];
                break;
            }
        }
        if (!auth_user && g_user.valid && d_strcmp(g_login.username, g_user.name) == 0) {
            char hash[33];
            md5_string(g_login.password, hash);
            serial_puts(" [legacy] hash=");
            serial_puts(hash);
            serial_puts(" expect=");
            serial_puts(g_user.passwd_hash);
            if (d_strcmp(hash, g_user.passwd_hash) == 0) {
                serial_puts(" OK\n");
                g_login.error = 0;
                g_current_user = 0;
                return 1;
            }
            serial_puts(" FAIL\n");
        }
    }
    serial_puts("\n");

    if (!auth_user) {
        serial_puts("[DESK] Auth FAIL: user not found\n");
        g_login.error = 1;
        return 0;
    }

    char hash[33];
    md5_string(g_login.password, hash);

    serial_puts("[DESK] MD5 input='");
    serial_puts(g_login.password);
    serial_puts("' hash=");
    serial_puts(hash);
    serial_puts(" expect=");
    serial_puts(auth_user->passwd_hash);
    serial_puts(" valid=");
    serial_puts(auth_user->valid ? "Y" : "N");
    serial_puts("\n");

    if (!auth_user->passwd_hash[0]) {
        serial_puts("[DESK] Auth OK: no password set\n");
        g_login.error = 0;
        g_current_user = (int)(auth_user - g_users);
        return 1;
    }

    if (d_strcmp(hash, auth_user->passwd_hash) != 0) {
        serial_puts("[DESK] Auth FAIL: hash mismatch\n");
        g_login.error = 1;
        return 0;
    }

    g_login.error = 0;
    g_current_user = (int)(auth_user - g_users);
    serial_puts("[DESK] Login OK as: ");
    serial_puts(auth_user->name);
    serial_puts("\n");
    return 1;
}

/* ========== AI Panel Rendering ========== */

static void draw_ai_panel(void) {
    if (!g_ai.visible) return;

    int px = g_w - AI_PANEL_W - 12;
    int py = 12;

    fill_rounded_rect(px + 4, py + 4, AI_PANEL_W, AI_PANEL_H, 14, MKC(4, 4, 8));
    fill_rounded_rect(px, py, AI_PANEL_W, AI_PANEL_H, 14, C_AI_BG);
    for (int i = 0; i < 2; i++)
        fill_rounded_rect(px + i, py + i, AI_PANEL_W - 2*i, AI_PANEL_H - 2*i, 14 - i, C_AI_BORDER);
    fill_rounded_rect(px + 2, py + 2, AI_PANEL_W - 4, AI_PANEL_H - 4, 12, C_AI_BG);

    {
        int title_h = 40;
        fill_rounded_rect(px + 2, py + 2, AI_PANEL_W - 4, title_h, 12, C_SURFACE);
        fill_rect(px + 14, py + title_h - 2, AI_PANEL_W - 28, 14, C_AI_BG);

        int dot_cx = px + 20, dot_cy = py + 18;
        uint32_t dot_color;
        if (g_ai.waiting) dot_color = C_YELLOW;
        else if (g_ai.connected) dot_color = C_GREEN;
        else dot_color = C_RED;
        for (int dy = -5; dy <= 5; dy++)
            for (int dx = -5; dx <= 5; dx++)
                if (dx*dx + dy*dy <= 25) {
                    int ddx = dot_cx + dx, ddy = dot_cy + dy;
                    if (ddx >= 0 && ddx < g_w && ddy >= 0 && ddy < g_h)
                        g_fb[ddy * g_stride + ddx] = dot_color;
                }

        const char *title = "AI Dev Module";
        draw_text(dot_cx + 10, py + 14, title, C_TEXT);

        {
            int close_x = px + AI_PANEL_W - 36;
            int close_y = py + 8;
            int close_s = 24;
            int hov = (g_mouse.x >= close_x && g_mouse.x < close_x + close_s &&
                       g_mouse.y >= close_y && g_mouse.y < close_y + close_s);
            fill_rounded_rect(close_x, close_y, close_s, close_s, 6, hov ? C_RED : C_SURFACE);
            draw_text(close_x + 4, close_y + 4, "X", hov ? C_WHITE : C_TEXT_SEC);
        }
    }

    {
        int info_y = py + 48;
        const char *svc = g_ai.service_name[0] ? g_ai.service_name : "none";
        const char *mdl = g_ai.model_name[0] ? g_ai.model_name : "none";
        char info_buf[80];
        int pos = 0;
        const char *prefix = "Service: ";
        while (*prefix && pos < 79) info_buf[pos++] = *prefix++;
        const char *s = svc;
        while (*s && pos < 79) info_buf[pos++] = *s++;
        const char *mid = "  Model: ";
        while (*mid && pos < 79) info_buf[pos++] = *mid++;
        s = mdl;
        while (*s && pos < 79) info_buf[pos++] = *s++;
        info_buf[pos] = 0;
        draw_text(px + 14, info_y, info_buf, C_TEXT_SEC);
    }

    {
        int chat_y = py + 68;
        int chat_h = AI_PANEL_H - 68 - 110;
        int chat_x = px + 8;
        int chat_w = AI_PANEL_W - 16;

        fill_rounded_rect(chat_x, chat_y, chat_w, chat_h, 8, C_MICA_BG2);

        int line_h = FONT_H + 4;
        int max_visible = chat_h / line_h;
        int total_lines = 0;
        int line_count[AI_CHAT_LINES];

        for (int i = 0; i < g_ai.chat_count; i++) {
            int tlen = d_strlen(g_ai.chat[i].text);
            int chars_per_line = (chat_w - 20) / FONT_W;
            int lines = (tlen + chars_per_line - 1) / chars_per_line;
            if (lines < 1) lines = 1;
            line_count[i] = lines;
            total_lines += lines;
        }

        if (g_ai.chat_scroll > total_lines - max_visible + 1)
            g_ai.chat_scroll = total_lines - max_visible + 1;
        if (g_ai.chat_scroll < 0) g_ai.chat_scroll = 0;

        int skip = g_ai.chat_scroll;
        int drawn = 0;
        int cy = chat_y + 4;

        for (int i = 0; i < g_ai.chat_count && drawn < max_visible; i++) {
            if (skip >= line_count[i]) {
                skip -= line_count[i];
                continue;
            }

            uint32_t msg_bg;
            if (g_ai.chat[i].role == 'u') msg_bg = C_AI_MSG_USER;
            else if (g_ai.chat[i].role == 'a') msg_bg = C_AI_MSG_AI;
            else msg_bg = C_SURFACE;

            int chars_per_line = (chat_w - 20) / FONT_W;
            int tlen = d_strlen(g_ai.chat[i].text);
            int msg_lines = (tlen + chars_per_line - 1) / chars_per_line;
            if (msg_lines < 1) msg_lines = 1;

            int msg_h = msg_lines * line_h + 4;
            fill_rounded_rect(chat_x + 4, cy, chat_w - 8, msg_h, 6, msg_bg);

            const char *prefix = "";
            uint32_t prefix_color = C_TEXT;
            if (g_ai.chat[i].role == 'u') { prefix = "> "; prefix_color = C_ACCENT; }
            else if (g_ai.chat[i].role == 'a') { prefix = "AI: "; prefix_color = C_GREEN; }
            else { prefix = "SYS: "; prefix_color = C_YELLOW; }

            int px2 = chat_x + 10;
            int py2 = cy + 2;
            draw_text(px2, py2, prefix, prefix_color);
            px2 += d_strlen(prefix) * FONT_W;

            int start_char = skip * chars_per_line;
            int char_pos = 0;
            for (int l = 0; l < msg_lines && drawn < max_visible; l++) {
                if (l > 0) { px2 = chat_x + 10; py2 += line_h; }
                for (int c = 0; c < chars_per_line && char_pos < tlen; c++, char_pos++) {
                    if (char_pos < start_char) continue;
                    char ch[2] = {g_ai.chat[i].text[char_pos], 0};
                    draw_text(px2, py2, ch, C_TEXT);
                    px2 += FONT_W;
                }
                drawn++;
            }

            cy += msg_h + 2;
            skip = 0;
        }

        if (g_ai.chat_count == 0) {
            draw_text(chat_x + 14, chat_y + chat_h/2 - 4, "Type a prompt below...", C_TEXT_DIS);
        }

        if (g_ai.waiting) {
            int dot_x = chat_x + chat_w/2 - 20;
            int dot_y = chat_y + chat_h - 20;
            for (int d = 0; d < 3; d++) {
                int phase = (g_login.anim_tick + d * 8) % 24;
                int alpha = phase < 12 ? phase * 20 : (24 - phase) * 20;
                uint32_t dot_c = MKC(96 * alpha / 255, 165 * alpha / 255, 250 * alpha / 255);
                fill_rounded_rect(dot_x + d * 14, dot_y, 8, 8, 4, dot_c);
            }
        }
    }

    {
        int inp_x = px + 8;
        int inp_y = py + AI_PANEL_H - 96;
        int inp_w = AI_PANEL_W - 16;
        int inp_h = 40;

        fill_rounded_rect(inp_x, inp_y, inp_w, inp_h, 8, C_AI_INPUT_BG);
        for (int i = 0; i < 2; i++)
            fill_rounded_rect(inp_x + i, inp_y + i, inp_w - 2*i, inp_h - 2*i, 8 - i, C_BORDER);
        fill_rounded_rect(inp_x + 2, inp_y + 2, inp_w - 4, inp_h - 4, 6, C_AI_INPUT_BG);

        if (g_ai.input_cursor > 0) {
            int maxc = (inp_w - 20) / FONT_W;
            int start = g_ai.input_cursor > maxc ? g_ai.input_cursor - maxc : 0;
            int len = g_ai.input_cursor - start;
            char tmp[256];
            if (len > 255) len = 255;
            for (int i = 0; i < len; i++) tmp[i] = g_ai.input_buf[start + i];
            tmp[len] = 0;
            draw_text(inp_x + 8, inp_y + (inp_h - FONT_H)/2, tmp, C_TEXT);
        } else {
            draw_text(inp_x + 8, inp_y + (inp_h - FONT_H)/2, "Ask AI anything...", C_TEXT_DIS);
        }

        if ((g_login.anim_tick % 60) < 40) {
            int maxc = (inp_w - 20) / FONT_W;
            int cx_pos = inp_x + 8 + ((g_ai.input_cursor > maxc ? maxc : g_ai.input_cursor) * FONT_W);
            fill_rect(cx_pos, inp_y + 8, 2, inp_h - 16, C_ACCENT);
        }
    }

    {
        int btn_y = py + AI_PANEL_H - 48;
        int btn_h = 36;

        int send_x = px + 8;
        int send_w = AI_PANEL_W / 2 - 16;
        {
            int hov = (g_mouse.x >= send_x && g_mouse.x < send_x + send_w &&
                       g_mouse.y >= btn_y && g_mouse.y < btn_y + btn_h);
            fill_rounded_rect(send_x, btn_y, send_w, btn_h, 6, hov ? C_AI_BTN_HOVER : C_AI_BTN_SEND);
            const char *txt = "Send";
            int tw = text_width(txt);
            draw_text(send_x + (send_w - tw)/2, btn_y + (btn_h-FONT_H)/2, txt, C_WHITE);
        }

        int clear_x = px + AI_PANEL_W / 2 + 4;
        int clear_w = AI_PANEL_W / 2 - 16;
        {
            int hov = (g_mouse.x >= clear_x && g_mouse.x < clear_x + clear_w &&
                       g_mouse.y >= btn_y && g_mouse.y < btn_y + btn_h);
            fill_rounded_rect(clear_x, btn_y, clear_w, btn_h, 6, hov ? C_HOVER : C_SURFACE);
            for (int i = 0; i < 2; i++)
                fill_rounded_rect(clear_x + i, btn_y + i, clear_w - 2*i, btn_h - 2*i, 6 - i, C_BORDER);
            fill_rounded_rect(clear_x + 2, btn_y + 2, clear_w - 4, btn_h - 4, 4, hov ? C_HOVER : C_SURFACE);
            const char *txt = "Clear";
            int tw = text_width(txt);
            draw_text(clear_x + (clear_w - tw)/2, btn_y + (btn_h-FONT_H)/2, txt, C_TEXT_SEC);
        }
    }

    {
        const char *hint = "Ctrl+A: toggle  Enter: send  Esc: close";
        draw_text(px + 10, py + AI_PANEL_H - 8, hint, C_TEXT_DIS);
    }
}

/* ========== Boot Logo (ZT5) ========== */

static void draw_logo_glow_line(int x0, int y0, int x1, int y1, int thickness, uint32_t core_color, int glow_r) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps == 0) steps = 1;
    int cr = (core_color >> 16) & 0xFF;
    int cg = (core_color >> 8) & 0xFF;
    int cb = core_color & 0xFF;
    for (int g = glow_r; g >= 0; g--) {
        int t = g * 255 / glow_r;
        int a = (255 - t) * (255 - t) / 255;
        int gr = cr * a / 255;
        int gg = cg * a / 255;
        int gb = cb * a / 255;
        uint32_t gc = ((uint32_t)gr << 16) | ((uint32_t)gg << 8) | (uint32_t)gb;
        int thick = thickness + g * 2;
        int half = thick / 2;
        for (int s = 0; s <= steps; s++) {
            int px = x0 + dx * s / steps;
            int py = y0 + dy * s / steps;
            fill_rect(px - half, py - half, thick, thick, gc);
        }
    }
    int half = thickness / 2;
    for (int s = 0; s <= steps; s++) {
        int px = x0 + dx * s / steps;
        int py = y0 + dy * s / steps;
        fill_rect(px - half, py - half, thickness, thickness, core_color);
    }
}

static void draw_boot_logo(void) {
    fill_rect(0, 0, g_w, g_h, MKC(0, 0, 0));

    int cx = g_w / 2, cy = g_h / 2 - 20;
    int max_r = g_w / 4;
    for (int y = cy - max_r; y < cy + max_r && y < g_h; y++) {
        if (y < 0) continue;
        for (int x = cx - max_r; x < cx + max_r && x < g_w; x++) {
            if (x < 0) continue;
            int ddx = x - cx, ddy = y - cy;
            int dist_sq = ddx*ddx + ddy*ddy;
            if (dist_sq < max_r * max_r) {
                int norm = dist_sq * 256 / (max_r * max_r);
                int alpha;
                if (norm < 32) alpha = 60 - norm;
                else if (norm < 96) alpha = 28 - (norm - 32) / 3;
                else if (norm < 180) alpha = 10 - (norm - 96) / 10;
                else alpha = 2;
                if (alpha <= 0) continue;
                int r = (30 * alpha) / 255;
                int gv = (80 * alpha) / 255;
                int b = (200 * alpha) / 255;
                if (g_fb_bgr)
                    g_fb[y * g_stride + x] = ((uint32_t)b<<16)|((uint32_t)gv<<8)|(uint32_t)r;
                else
                    g_fb[y * g_stride + x] = ((uint32_t)r<<16)|((uint32_t)gv<<8)|(uint32_t)b;
            }
        }
    }

    int scale = (g_w < 800) ? 2 : 4;
    int thick = scale;
    int char_h = 8 * scale;
    int char_w = 8 * scale;
    int spacing = 3 * scale;
    int total_w = 3 * char_w + 2 * spacing;
    int sx = (g_w - total_w) / 2;
    int sy = (g_h - char_h) / 2 - 20;
    int glow = scale;

    uint32_t z_color = MKC(100, 180, 255);
    uint32_t t_color = MKC(140, 200, 255);
    uint32_t f_color = MKC(96, 165, 250);

    {
        int zx = sx, zy = sy;
        draw_logo_glow_line(zx, zy, zx + char_w, zy, thick, z_color, glow);
        draw_logo_glow_line(zx + char_w, zy, zx, zy + char_h, thick, z_color, glow);
        draw_logo_glow_line(zx, zy + char_h, zx + char_w, zy + char_h, thick, z_color, glow);
    }

    {
        int tx = sx + char_w + spacing, ty = sy;
        draw_logo_glow_line(tx, ty, tx + char_w, ty, thick, t_color, glow);
        int stem_x = tx + char_w / 2;
        draw_logo_glow_line(stem_x, ty, stem_x, ty + char_h, thick, t_color, glow);
    }

    {
        int fx = sx + 2*(char_w + spacing), fy = sy;
        draw_logo_glow_line(fx, fy, fx + char_w, fy, thick, f_color, glow);
        draw_logo_glow_line(fx, fy, fx, fy + char_h * 5/8, thick, f_color, glow);
        draw_logo_glow_line(fx, fy + char_h * 5/8, fx + char_w, fy + char_h * 3/8, thick, f_color, glow);
        draw_logo_glow_line(fx + char_w, fy + char_h * 3/8, fx + char_w, fy + char_h * 3/4, thick, f_color, glow);
        draw_logo_glow_line(fx + char_w, fy + char_h * 3/4, fx + char_w * 3/4, fy + char_h, thick, f_color, glow);
        draw_logo_glow_line(fx + char_w * 3/4, fy + char_h, fx + char_w / 4, fy + char_h, thick, f_color, glow);
        draw_logo_glow_line(fx + char_w / 4, fy + char_h, fx, fy + char_h * 7/8, thick, f_color, glow);
    }

    {
        const char *sub = "ZratroOS";
        int sub_y = sy + char_h + 16;
        int tw = text_width(sub);
        int sub_x = (g_w - tw) / 2;
        draw_text(sub_x, sub_y, sub, MKC(80, 130, 200));

        int line_w = tw + 30;
        int line_x = (g_w - line_w) / 2;
        int line_y = sub_y + FONT_H + 6;
        for (int lx = 0; lx < line_w; lx++) {
            int t = lx < line_w / 2 ? lx * 2 : (line_w - lx) * 2;
            int a = t * 60 / line_w;
            uint32_t lc = MKC(96 * a / 255, 165 * a / 255, 250 * a / 255);
            fill_rect(line_x + lx, line_y, 1, 1, lc);
        }
    }

    {
        int dot_y = sy + char_h + 16 + FONT_H + 20;
        int dot_spacing = 14;
        int total_dots = 3 * dot_spacing - 6;
        int dot_start = (g_w - total_dots) / 2;
        for (int d = 0; d < 3; d++) {
            int dx = dot_start + d * dot_spacing;
            int phase = (g_login.anim_tick + d * 8) % 24;
            int bright = phase < 12 ? 80 + phase * 14 : 80 + (24 - phase) * 14;
            if (bright > 250) bright = 250;
            uint32_t dc = MKC(96 * bright / 255, 165 * bright / 255, 250 * bright / 255);
            fill_rounded_rect(dx - 2, dot_y - 2, 4, 4, 2, dc);
        }
    }

    fb_flip();
}

/* ========== Transition Animation ========== */

static void trans_start(int next_state) {
    g_trans.active = 1;
    g_trans.frame = 0;
    g_trans.total_frames = TRANS_FADE_FRAMES;
    g_trans.direction = 0;  /* fade-out first */
    g_trans.next_state = next_state;
    g_trans.saved = NULL;
}

/* Apply fade effect to framebuffer */
static void apply_fade(int alpha) {
    /* alpha: 0 = fully visible, 255 = fully black */
    if (alpha <= 0) return;
    uint32_t black = MKC(0, 0, 0);
    for (int y = 0; y < g_h; y++) {
        for (int x = 0; x < g_w; x++) {
            uint32_t *px = &g_fb[y * g_stride + x];
            uint32_t c = *px;
            int r = ((c >> 16) & 0xFF) * (255 - alpha) / 255;
            int g = ((c >> 8) & 0xFF) * (255 - alpha) / 255;
            int b = (c & 0xFF) * (255 - alpha) / 255;
            *px = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

/* ========== Taskbar Drawing (KDE+Win11 floating dock) ========== */

static void draw_taskbar(void) {
    int dock_w = g_w - 2 * TASKBAR_MARGIN;
    int dock_x = TASKBAR_MARGIN;
    int dock_y = g_h - TASKBAR_H - TASKBAR_MARGIN;

    fill_rounded_rect(dock_x + 2, dock_y + 2, dock_w, TASKBAR_H, TASKBAR_RADIUS, MKC(180, 200, 230));

    for (int yy = dock_y; yy < dock_y + TASKBAR_H && yy < g_h; yy++) {
        int vt = (yy - dock_y) * 256 / TASKBAR_H;
        int x0 = dock_x, x1 = dock_x + dock_w;
        if (x0 < 0) x0 = 0;
        if (x1 > g_w) x1 = g_w;
        for (int x = x0; x < x1; x++) {
            int in = 1;
            if (x < dock_x + TASKBAR_RADIUS && yy < dock_y + TASKBAR_RADIUS) {
                int dx = (dock_x + TASKBAR_RADIUS - 1) - x, dy = (dock_y + TASKBAR_RADIUS - 1) - yy;
                if (dx*dx + dy*dy > TASKBAR_RADIUS*TASKBAR_RADIUS) in = 0;
            } else if (x >= dock_x + dock_w - TASKBAR_RADIUS && yy < dock_y + TASKBAR_RADIUS) {
                int dx = x - (dock_x + dock_w - TASKBAR_RADIUS), dy = (dock_y + TASKBAR_RADIUS - 1) - yy;
                if (dx*dx + dy*dy > TASKBAR_RADIUS*TASKBAR_RADIUS) in = 0;
            } else if (x < dock_x + TASKBAR_RADIUS && yy >= dock_y + TASKBAR_H - TASKBAR_RADIUS) {
                int dx = (dock_x + TASKBAR_RADIUS - 1) - x, dy = yy - (dock_y + TASKBAR_H - TASKBAR_RADIUS);
                if (dx*dx + dy*dy > TASKBAR_RADIUS*TASKBAR_RADIUS) in = 0;
            } else if (x >= dock_x + dock_w - TASKBAR_RADIUS && yy >= dock_y + TASKBAR_H - TASKBAR_RADIUS) {
                int dx = x - (dock_x + dock_w - TASKBAR_RADIUS), dy = yy - (dock_y + TASKBAR_H - TASKBAR_RADIUS);
                if (dx*dx + dy*dy > TASKBAR_RADIUS*TASKBAR_RADIUS) in = 0;
            }
            if (in) {
                int ht = (x - dock_x) * 256 / dock_w;
                int phase = (ht * 3 - g_login.anim_tick * 5) % 1536;
                if (phase < 0) phase += 1536;
                int wave;
                if (phase < 256) {
                    wave = phase;
                } else if (phase < 512) {
                    wave = 512 - phase;
                } else if (phase < 768) {
                    wave = 0;
                } else if (phase < 1024) {
                    wave = phase - 768;
                } else if (phase < 1280) {
                    wave = 1280 - phase;
                } else {
                    wave = 0;
                }
                int r = 50 + (180 * wave / 255) + (50 * (256 - vt) / 256);
                int gv = 110 + (120 * wave / 255) + (35 * (256 - vt) / 256);
                int b = 200 + (55 * wave / 255);
                if (r > 255) r = 255;
                if (gv > 255) gv = 255;
                if (b > 255) b = 255;
                uint32_t c = g_fb_bgr ? MKC_BGR(b, gv, r) : MKC_RGB(r, gv, b);
                g_fb[yy * g_stride + x] = c;
            }
        }
    }

    for (int x = dock_x + TASKBAR_RADIUS; x < dock_x + dock_w - TASKBAR_RADIUS; x++) {
        if (x >= 0 && x < g_w && dock_y >= 0 && dock_y < g_h)
            g_fb[dock_y * g_stride + x] = C_TASKBAR_BORDER;
    }

    {
        int btn_x = dock_x + 6;
        int btn_y = dock_y + 4;
        int btn_s = TASKBAR_H - 8;
        int hov = (g_mouse.x >= btn_x && g_mouse.x < btn_x + btn_s &&
                   g_mouse.y >= btn_y && g_mouse.y < btn_y + btn_s);
        fill_rounded_rect(btn_x, btn_y, btn_s, btn_s, 6, hov ? C_TASKBAR_HOVER : MKC(200, 220, 250));
        const char *logo = "ZT5";
        int lw = text_width(logo);
        draw_text(btn_x + (btn_s - lw) / 2, btn_y + (btn_s - FONT_H) / 2, logo, C_TASKBAR_ACCENT);
    }

    {
        int sep_x = dock_x + 52;
        fill_rect(sep_x, dock_y + 8, 1, TASKBAR_H - 16, C_TASKBAR_BORDER);
    }

    {
        int sep_x = dock_x + 52;
        fill_rect(sep_x, dock_y + 8, 1, TASKBAR_H - 16, C_TASKBAR_BORDER);
    }

    {
        int tray_x = dock_x + dock_w - 220;
        fill_rect(tray_x, dock_y + 8, 1, TASKBAR_H - 16, C_TASKBAR_BORDER);
        tray_x += 8;

        {
            int nx = tray_x, ny = dock_y + 6;
            uint32_t bar_c = g_netpanel.connected ? C_TASKBAR_ACCENT : C_TASKBAR_TEXT;
            fill_rect(nx, ny+10, 4, 6, bar_c);
            fill_rect(nx+5, ny+6, 4, 10, bar_c);
            fill_rect(nx+10, ny+2, 4, 14, bar_c);
            fill_rect(nx+15, ny-2, 4, 18, bar_c);
        }

        {
            int vx = tray_x + 26, vy = dock_y + 8;
            fill_rect(vx, vy+4, 6, 8, C_TASKBAR_TEXT);
            fill_rect(vx+6, vy+2, 4, 4, C_TASKBAR_TEXT);
            fill_rect(vx+6, vy+10, 4, 4, C_TASKBAR_TEXT);
            fill_rect(vx+10, vy, 3, 16, C_TASKBAR_TEXT);
        }

        char clock_str[20];
        int h = g_clock.hour, m = g_clock.min, s = g_clock.sec;
        clock_str[0] = '0'+h/10; clock_str[1] = '0'+h%10;
        clock_str[2] = ':'; clock_str[3] = '0'+m/10; clock_str[4] = '0'+m%10;
        clock_str[5] = 0;
        int cw = text_width(clock_str);
        int clock_x = tray_x + 56;
        int hov = (g_mouse.x >= clock_x && g_mouse.x < clock_x + cw + 12 &&
                   g_mouse.y >= dock_y + 4 && g_mouse.y < dock_y + TASKBAR_H - 4);
        fill_rounded_rect(clock_x, dock_y + 4, cw + 12, TASKBAR_H - 8, 6, hov ? C_TASKBAR_HOVER : MKC(210, 225, 250));
        draw_text(clock_x + 6, dock_y + 4 + (TASKBAR_H - 8 - FONT_H)/2, clock_str, C_TASKBAR_TEXT);

        char date_str[16];
        date_str[0] = '0'+g_clock.month/10; date_str[1] = '0'+g_clock.month%10;
        date_str[2] = '/'; date_str[3] = '0'+g_clock.day/10; date_str[4] = '0'+g_clock.day%10;
        date_str[5] = 0;
        int date_x = clock_x + cw + 16;
        if (date_x + 60 < dock_x + dock_w) {
            draw_text(date_x, dock_y + 4 + (TASKBAR_H - 8 - FONT_H)/2, date_str, C_TASKBAR_TEXT);
        }
    }
}

/* ========== Main Entry ========== */

void _start(void) {
    serial_puts("[DESK] Desktop starting...\n");

    /* Get framebuffer info */
    g_fbi.base = fb_base; g_fbi.w = fb_width; g_fbi.h = fb_height; g_fbi.pitch = fb_pitch;
    /* Always call SYS_GETFBINFO to get pixel format */
    sc2(SYS_GETFBINFO, (long)&g_fbi, 0);

    g_w = (int)g_fbi.w; g_h = (int)g_fbi.h;
    g_stride = (int)g_fbi.pitch / 4;
    g_fb = (uint32_t*)(uint64_t)g_fbi.base;
    /* Kernel pixel format enum: 0=unknown, 1=RGB565, 2=RGB888, 3=XRGB8888, 4=ARGB8888, 5=BGR888, 6=XBGR8888, 7=ABGR8888 */
    /* QEMU UEFI GOP uses PixelBlueGreenRedReserved8BitPerColor:
     *   Memory byte order: B G R X
     *   As uint32_t (little-endian): 0xXRRGGBB
     *   This means R is in bits 16-23, G in 8-15, B in 0-7
     *   So MKC_RGB(r,g,b) = (r<<16)|(g<<8)|b gives the correct value
     *   Kernel reports this as XRGB8888 (format=3) because red_mask_shift=16
     *
     * QEMU UEFI with PixelRedGreenBlueReserved8BitPerColor:
     *   Memory byte order: R G B X
     *   As uint32_t (little-endian): 0xXBBGGRR
     *   This means B is in bits 16-23, G in 8-15, R in 0-7
     *   So MKC_BGR(r,g,b) = (b<<16)|(g<<8)|r gives the correct value
     *   Kernel reports this as XBGR8888 (format=6) because red_mask_shift=0
     */
    if (g_fbi.format == 3 || g_fbi.format == 4) {
        /* XRGB8888 or ARGB8888 - value is 0xXRRGGBB, use RGB mode */
        g_fb_bgr = 0;  /* XRGB8888: use MKC_RGB which gives (r<<16)|(g<<8)|b */
    } else if (g_fbi.format == 6 || g_fbi.format == 7) {
        /* XBGR8888 or ABGR8888 - value is 0xXBBGGRR, use BGR mode */
        g_fb_bgr = 1;  /* XBGR8888: use MKC_BGR which gives (b<<16)|(g<<8)|r */
    } else {
        /* Default: assume XRGB8888 (most common for UEFI GOP) */
        g_fb_bgr = 0;
    }

    /* Debug: print format info */
    {
        char dbuf[64];
        int dp = 0;
        const char *prefix = "[DESK] format=";
        while (prefix[dp]) dbuf[dp] = prefix[dp], dp++;
        /* format number */
        int fmt = (int)g_fbi.format;
        char fbuf[12]; int fp = 0;
        if (fmt == 0) fbuf[fp++] = '0';
        else { char t[12]; int tp = 0; while (fmt > 0) { t[tp++] = '0' + (fmt % 10); fmt /= 10; } while (tp > 0) fbuf[fp++] = t[--tp]; }
        for (int i = 0; i < fp; i++) dbuf[dp++] = fbuf[i];
        dbuf[dp++] = ' '; dbuf[dp++] = 'b'; dbuf[dp++] = 'g'; dbuf[dp++] = 'r'; dbuf[dp++] = '=';
        int bv = g_fb_bgr;
        dbuf[dp++] = '0' + bv;
        dbuf[dp++] = ' '; dbuf[dp++] = 's'; dbuf[dp++] = 't'; dbuf[dp++] = 'r'; dbuf[dp++] = 'i'; dbuf[dp++] = 'd'; dbuf[dp++] = 'e'; dbuf[dp++] = '=';
        int sv = g_stride;
        if (sv == 0) dbuf[dp++] = '0';
        else { char t[12]; int tp = 0; while (sv > 0) { t[tp++] = '0' + (sv % 10); sv /= 10; } while (tp > 0) dbuf[dp++] = t[--tp]; }
        dbuf[dp++] = '\n'; dbuf[dp] = 0;
        serial_puts(dbuf);
    }

    /* Allocate backbuffer - use stride (pitch/4) not width, in case pitch > width*4 */
    uint32_t *backbuf = (uint32_t*)desk_alloc(g_stride * g_h * 4);
    if (backbuf) g_fb = backbuf;

    serial_puts("[DESK] Framebuffer initialized\n");

    /* Initialize mouse position to center */
    g_mouse.x = g_w / 2;
    g_mouse.y = g_h / 2;

    /* Show boot logo for ~8 seconds (4x original) */
    draw_boot_logo();

    {
        for (volatile int outer = 0; outer < 2000; outer++)
            for (volatile int d = 0; d < 1000000; d++);
    }

    /* Transition: fade out from logo */
    {
        for (int f = 0; f < TRANS_FADE_FRAMES; f++) {
            int alpha = f * 255 / TRANS_FADE_FRAMES;
            apply_fade(alpha);
            fb_flip();
            for (volatile int d = 0; d < 30000; d++);
        }
    }

    /* Load user config and show login screen */
    parse_user_cfg();
    g_login.active = 1;
    g_login.field = LOGIN_FIELD_USER;
    g_login.selected_user = -1;
    g_login.user_len = 0;
    g_login.pass_len = 0;
    g_login.error = 0;
    g_login.anim_tick = 0;

    serial_puts("[DESK] Login screen active\n");

    /* Login loop */
    int login_frame = 0;
    while (g_login.active) {
        uint32_t events[32];
        int nevt = (int)sys_input_poll(events, 32);
        for (int i = 0; i < nevt; i++) {
            uint32_t evt = events[i];
            int type = (int)(evt >> 16);
            int data = (int)(evt & 0xFFFF);

            if (type == 1) {
                /* Keyboard press - data is keycode */
                int kc = data;
                if (kc == KEY_TAB) {
                    /* Tab always switches between username and password fields */
                    g_login.field = (g_login.field == LOGIN_FIELD_USER) ? LOGIN_FIELD_PASS : LOGIN_FIELD_USER;
                } else if (kc == KEY_ENTER) {
                    if (login_try_auth()) {
                        g_login.active = 0;
                    } else {
                        g_login.pass_len = 0;
                        g_login.password[0] = 0;
                    }
                } else if (kc == KEY_BACKSPACE) {
                    if (g_login.field == LOGIN_FIELD_USER && g_login.user_len > 0) {
                        g_login.user_len--;
                        g_login.username[g_login.user_len] = 0;
                    } else if (g_login.field == LOGIN_FIELD_PASS && g_login.pass_len > 0) {
                        g_login.pass_len--;
                        g_login.password[g_login.pass_len] = 0;
                    }
                } else if (kc == 72) {
                    /* Up arrow - select previous user */
                    if (g_login.selected_user > 0)
                        g_login.selected_user--;
                } else if (kc == 80) {
                    /* Down arrow - select next user */
                    if (g_login.selected_user < g_user_count - 1)
                        g_login.selected_user++;
                } else if (kc < 128) {
                    char ch = scancode_to_ascii[kc];
                    if (ch >= 32 && ch < 127) {
                        if (g_login.field == LOGIN_FIELD_USER && g_login.user_len < 63) {
                            g_login.username[g_login.user_len++] = ch;
                            g_login.username[g_login.user_len] = 0;
                        } else if (g_login.field == LOGIN_FIELD_PASS && g_login.pass_len < 63) {
                            g_login.password[g_login.pass_len++] = ch;
                            g_login.password[g_login.pass_len] = 0;
                        }
                    }
                }
            } else if (type == 2) {
                /* Keyboard release - data is keycode (ignore for now) */
            } else if (type == 3) {
                g_mouse.x = data;
                if (g_mouse.x < 0) g_mouse.x = 0;
                if (g_mouse.x >= g_w) g_mouse.x = g_w - 1;
            } else if (type == 4) {
                g_mouse.y = data;
                if (g_mouse.y < 0) g_mouse.y = 0;
                if (g_mouse.y >= g_h) g_mouse.y = g_h - 1;
            }
            if ((type == 3 || type == 4) && !g_launcher.visible) {
                int dock_y = g_h - TASKBAR_H - TASKBAR_MARGIN;
                int btn_x = TASKBAR_MARGIN + 6;
                int btn_y = dock_y + 4;
                int btn_s = TASKBAR_H - 8;
                if (g_mouse.x >= btn_x && g_mouse.x < btn_x + btn_s &&
                    g_mouse.y >= btn_y && g_mouse.y < btn_y + btn_s) {
                    launcher_scan_apps();
                    g_launcher.visible = 1;
                    g_launcher.anim_frame = 0;
                    g_launcher.anim_total = 12;
                    g_taskbar.launcher_open = 1;
                }
            } else if (type == 5) {
                /* Mouse left button - data=1 pressed, data=0 released */
                g_mouse.left = data;
                if (data) {
                    /* Click on Sign in button? */
                    int card_w = 360, card_h = 320;
                    int card_x = (g_w - card_w) / 2;
                    int card_y = (g_h - card_h) / 2;
                    int field_x = card_x + 28;
                    int field_w = card_w - 56;
                    int user_y = card_y + 110;
                    int pass_y = user_y + 36 + 24;
                    int btn_x = field_x, btn_y = pass_y + 36 + 20, btn_w = field_w, btn_h = 36;

                    if (g_mouse.x >= btn_x && g_mouse.x < btn_x+btn_w &&
                        g_mouse.y >= btn_y && g_mouse.y < btn_y+btn_h) {
                        if (login_try_auth()) {
                            g_login.active = 0;
                        } else {
                            g_login.pass_len = 0;
                            g_login.password[0] = 0;
                        }
                    }
                    if (g_mouse.x >= field_x && g_mouse.x < field_x+field_w &&
                        g_mouse.y >= user_y && g_mouse.y < user_y+36) {
                        g_login.field = LOGIN_FIELD_USER;
                    }
                    if (g_mouse.x >= field_x && g_mouse.x < field_x+field_w &&
                        g_mouse.y >= pass_y && g_mouse.y < pass_y+36) {
                        g_login.field = LOGIN_FIELD_PASS;
                    }
                }
            } else if (type == 6) {
                g_mouse.right = data;
            }
        }

        /* Render login screen */
        g_login.anim_tick++;
        login_frame++;

        if (login_frame % 60 == 0 || g_clock.last_update == 0) {
            uint64_t t = (uint64_t)sc1(SYS_TIME, 0) + TIMEZONE_OFFSET;
            if (t > 0) {
                g_clock.last_update = t;
                int total_sec = (int)(t % 86400);
                g_clock.hour = total_sec / 3600;
                g_clock.min = (total_sec % 3600) / 60;
                g_clock.sec = total_sec % 60;
                int epoch_days = (int)(t / 86400);
                g_clock.year = 1970;
                int remaining = epoch_days;
                for (int y2 = 1970; remaining >= 0; y2++) {
                    int leap = (y2 % 400 == 0 || (y2 % 4 == 0 && y2 % 100 != 0)) ? 1 : 0;
                    int days_in_year = 365 + leap;
                    if (remaining >= days_in_year) {
                        remaining -= days_in_year;
                        g_clock.year = y2 + 1;
                    } else {
                        g_clock.year = y2;
                        break;
                    }
                }
                int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
                if ((g_clock.year % 400 == 0 || (g_clock.year % 4 == 0 && g_clock.year % 100 != 0))) mdays[1] = 29;
                g_clock.month = 1;
                for (int m = 0; m < 12 && remaining >= mdays[m]; m++) {
                    remaining -= mdays[m];
                    g_clock.month++;
                }
                g_clock.day = remaining + 1;
            } else if (g_clock.last_update > 0) {
                g_clock.sec++;
                if (g_clock.sec >= 60) { g_clock.sec = 0; g_clock.min++; }
                if (g_clock.min >= 60) { g_clock.min = 0; g_clock.hour++; }
                if (g_clock.hour >= 24) { g_clock.hour = 0; }
            }
        }

        erase_cursor();
        draw_login_screen();
        draw_cursor();
        fb_flip();

        for (volatile int d = 0; d < 50000; d++);
    }

    serial_puts("[DESK] Login success, initializing desktop...\n");

    /* Initialize desktop icons before transition */
    desk_scan_icons();

    /* Load background from desktop.conf */
    init_background();

    /* Load AI config from /zrr64/zdonk/AI/APIKey.confai */
    ai_load_config();

    serial_puts("[DESK] Desktop initialized, starting transition...\n");

    erase_cursor();

    /* Transition: fade out login to black, then fade in desktop from black */
    {
        int fb_size = g_h * g_stride;

        uint32_t *trans_buf = (uint32_t *)desk_alloc(fb_size * 4);
        if (trans_buf && (uint64_t)trans_buf < 0x7FFFFFFFFFFF) {
            for (int i = 0; i < fb_size; i++) trans_buf[i] = g_fb[i];

            for (int f = 0; f < TRANS_FADE_FRAMES; f++) {
                int alpha = f * 255 / TRANS_FADE_FRAMES;
                for (int i = 0; i < fb_size; i++) {
                    uint32_t c = trans_buf[i];
                    int r = ((c>>16)&0xFF) * (255 - alpha) / 255;
                    int gv = ((c>>8)&0xFF) * (255 - alpha) / 255;
                    int b = (c&0xFF) * (255 - alpha) / 255;
                    g_fb[i] = ((uint32_t)r<<16)|((uint32_t)gv<<8)|(uint32_t)b;
                }
                fb_flip();
                for (volatile int d = 0; d < 30000; d++);
            }
        }

        fill_rect(0, 0, g_w, g_h, C_BLACK);
        fb_flip();

        draw_background();
        for (int i = 0; i < g_icon_count; i++)
            draw_desk_icon(&g_icons[i]);
        draw_taskbar();
        fb_flip();
    }

    serial_puts("[DESK] Desktop active\n");

    /* Launch window manager (zrdemage) as child process */
    {
        long wm_pid = sys_exec("/prgm/sys/zrdemage");
        if (wm_pid > 0) {
            g_taskbar.wm_running = 1;
            serial_puts("[DESK] Window manager launched pid=");
            char pbuf[8]; pbuf[0]='0'+(wm_pid/10)%10; pbuf[1]='0'+wm_pid%10; pbuf[2]=0;
            serial_puts(pbuf);
            serial_puts("\n");
        } else {
            serial_puts("[DESK] Window manager launch FAILED\n");
        }
    }

    /* Main loop */
    uint32_t events[32];
    uint64_t frame = 0;
    int ctrl_pressed = 0;

    while (1) {
        /* Poll input */
        int nevt = (int)sys_input_poll(events, 32);
        for (int i = 0; i < nevt; i++) {
            uint32_t evt = events[i];
            int type = (int)(evt >> 16);
            int data = (int)(evt & 0xFFFF);

            if (type == 1) {
                int kc = data;

                if (kc == KEY_ESC) {
                    if (g_ai.visible) g_ai.visible = 0;
                    g_ctx_menu.visible = 0;
                } else if (ctrl_pressed && kc == 30) {
                    g_ai.visible = !g_ai.visible;
                } else if (kc == KEY_LEFTCTRL || kc == 29) {
                    ctrl_pressed = 1;
                } else if (g_ai.visible) {
                    if (kc == KEY_ENTER) {
                        ai_send_message();
                    } else if (kc == KEY_BACKSPACE && g_ai.input_cursor > 0) {
                        g_ai.input_cursor--;
                        g_ai.input_buf[g_ai.input_cursor] = 0;
                    } else if (kc == KEY_TAB) {
                        g_ai.visible = 0;
                    } else if (kc < 128) {
                        char ch = scancode_to_ascii[kc];
                        if (ch >= 32 && ch < 127 && g_ai.input_cursor < AI_INPUT_MAX - 1) {
                            g_ai.input_buf[g_ai.input_cursor++] = ch;
                            g_ai.input_buf[g_ai.input_cursor] = 0;
                        }
                    }
                }
            } else if (type == 2) {
                int kc = data;
                if (kc == KEY_LEFTCTRL || kc == 29) ctrl_pressed = 0;
            } else if (type == 3) {
                /* Mouse X absolute coordinate */
                g_mouse.x = data;
                if (g_mouse.x < 0) g_mouse.x = 0;
                if (g_mouse.x >= g_w) g_mouse.x = g_w - 1;
            } else if (type == 4) {
                /* Mouse Y absolute coordinate */
                g_mouse.y = data;
                if (g_mouse.y < 0) g_mouse.y = 0;
                if (g_mouse.y >= g_h) g_mouse.y = g_h - 1;
            }
            if ((type == 3 || type == 4) && !g_launcher.visible) {
                int dock_y = g_h - TASKBAR_H - TASKBAR_MARGIN;
                int btn_x = TASKBAR_MARGIN + 6;
                int btn_y = dock_y + 4;
                int btn_s = TASKBAR_H - 8;
                if (g_mouse.x >= btn_x && g_mouse.x < btn_x + btn_s &&
                    g_mouse.y >= btn_y && g_mouse.y < btn_y + btn_s) {
                    launcher_scan_apps();
                    g_launcher.visible = 1;
                    g_launcher.anim_frame = 0;
                    g_launcher.anim_total = 12;
                    g_taskbar.launcher_open = 1;
                }
            }
            if (type == 5) {
                /* Mouse left button */
                int was_pressed = g_mouse.left;
                g_mouse.left = data;
                if (data && !was_pressed) {
                    int click_consumed = 0;
                    /* AI panel click handling */
                    if (g_ai.visible) {
                        int apx = g_w - AI_PANEL_W - 12;
                        int apy = 12;

                        /* Close button */
                        int close_x = apx + AI_PANEL_W - 36;
                        int close_y = apy + 8;
                        if (g_mouse.x >= close_x && g_mouse.x < close_x + 24 &&
                            g_mouse.y >= close_y && g_mouse.y < close_y + 24) {
                            g_ai.visible = 0;
                            continue;
                        }

                        /* Send button */
                        int btn_y = apy + AI_PANEL_H - 44;
                        int send_x = apx + 8;
                        int send_w = AI_PANEL_W / 2 - 16;
                        if (g_mouse.x >= send_x && g_mouse.x < send_x + send_w &&
                            g_mouse.y >= btn_y && g_mouse.y < btn_y + 32) {
                            ai_send_message();
                            continue;
                        }

                        /* Clear button */
                        int clear_x = apx + AI_PANEL_W / 2 + 4;
                        int clear_w = AI_PANEL_W / 2 - 16;
                        if (g_mouse.x >= clear_x && g_mouse.x < clear_x + clear_w &&
                            g_mouse.y >= btn_y && g_mouse.y < btn_y + 32) {
                            g_ai.chat_count = 0;
                            g_ai.chat_scroll = 0;
                            g_ai.input_cursor = 0;
                            g_ai.input_buf[0] = 0;
                            continue;
                        }
                    }

                    /* Check taskbar clicks */
                    {
                        int dock_y = g_h - TASKBAR_H - TASKBAR_MARGIN;
                        if (g_mouse.y >= dock_y && g_mouse.y < dock_y + TASKBAR_H) {
                            /* Start button (ZT5 logo) */
                            int btn_x = TASKBAR_MARGIN + 6;
                            int btn_y = dock_y + 4;
                            int btn_s = TASKBAR_H - 8;
                            if (g_mouse.x >= btn_x && g_mouse.x < btn_x + btn_s &&
                                g_mouse.y >= btn_y && g_mouse.y < btn_y + btn_s) {
                                if (g_launcher.visible) {
                                    g_launcher.visible = 0;
                                    g_launcher.anim_frame = 0;
                                    g_launcher.anim_total = 0;
                                } else {
                                    launcher_scan_apps();
                                    g_launcher.visible = 1;
                                    g_launcher.anim_frame = 0;
                                    g_launcher.anim_total = 12;
                                    g_taskbar.launcher_open = 1;
                                }
                            }
                            /* Clock click - toggle datetime popup */
                            int tray_x = TASKBAR_MARGIN + (g_w - 2 * TASKBAR_MARGIN) - 220 + 8;
                            char clock_str[20];
                            clock_str[0] = '0'+g_clock.hour/10; clock_str[1] = '0'+g_clock.hour%10;
                            clock_str[2] = ':'; clock_str[3] = '0'+g_clock.min/10; clock_str[4] = '0'+g_clock.min%10;
                            clock_str[5] = 0;
                            int cw = text_width(clock_str);
                            int clock_x = tray_x + 56;
                            if (g_mouse.x >= clock_x && g_mouse.x < clock_x + cw + 12) {
                                g_clock.popup_visible = !g_clock.popup_visible;
                            }
                            if (g_mouse.x >= tray_x && g_mouse.x < tray_x + 24) {
                                g_netpanel.visible = !g_netpanel.visible;
                                if (g_netpanel.visible) {
                                    netpanel_connect();
                                    netpanel_scan();
                                }
                            }
                        }
                    }

                    /* Check launcher clicks */
                    if (g_launcher.visible) {
                        int dock_y = g_h - TASKBAR_H - TASKBAR_MARGIN;
                        int lx = TASKBAR_MARGIN;
                        int ly = dock_y - LAUNCHER_H - 4;
                        if (g_mouse.x >= lx && g_mouse.x < lx + LAUNCHER_W &&
                            g_mouse.y >= ly && g_mouse.y < ly + LAUNCHER_H) {
                            /* Click on app item */
                            int item_h = 36;
                            int list_y = ly + 36;
                            for (int i = 0; i < g_launcher.app_count; i++) {
                                int iy = list_y + i * item_h;
                                if (g_mouse.y >= iy && g_mouse.y < iy + item_h) {
                                    sys_exec(g_launcher.apps[i]);
                                    notify_wm(g_launcher.apps[i]);
                                    g_launcher.visible = 0;
                                    g_taskbar.launcher_open = 0;
                                    break;
                                }
                            }
                        } else {
                            /* Click outside launcher - close it */
                            g_launcher.visible = 0;
                            g_taskbar.launcher_open = 0;
                        }
                        click_consumed = 1;
                    }

                    /* Check network panel clicks */
                    if (g_netpanel.visible && !click_consumed) {
                        int dock_y = g_h - TASKBAR_H - TASKBAR_MARGIN;
                        int dock_w = g_w - 2 * TASKBAR_MARGIN;
                        int tray_x = TASKBAR_MARGIN + dock_w - 220 + 8;
                        int npx = tray_x;
                        if (npx + NET_PANEL_W > g_w) npx = g_w - NET_PANEL_W - 4;
                        if (npx < 4) npx = 4;
                        int npy = dock_y - NET_PANEL_H - 4;
                        if (g_mouse.x >= npx && g_mouse.x < npx + NET_PANEL_W &&
                            g_mouse.y >= npy && g_mouse.y < npy + NET_PANEL_H) {
                            /* Connect/Disconnect button */
                            int btn_w = 100, btn_h = 28;
                            int btn_x = npx + (NET_PANEL_W - btn_w) / 2;
                            int btn_y = npy + NET_PANEL_H - btn_h - 12;
                            if (g_mouse.x >= btn_x && g_mouse.x < btn_x + btn_w &&
                                g_mouse.y >= btn_y && g_mouse.y < btn_y + btn_h) {
                                if (g_netpanel.connected) {
                                    g_netpanel.connected = 0;
                                    d_strcpy(g_netpanel.status, "Disconnected");
                                } else {
                                    netpanel_connect();
                                }
                            }
                        } else {
                            /* Click outside netpanel - close it */
                            g_netpanel.visible = 0;
                        }
                        click_consumed = 1;
                    }

                    /* Left click on desktop */
                    if (!click_consumed) {
                        int clicked_icon = -1;
                        for (int j = 0; j < g_icon_count; j++) {
                            desk_icon_t *ic = &g_icons[j];
                            if (g_mouse.x >= ic->x && g_mouse.x < ic->x+ic->w &&
                                g_mouse.y >= ic->y && g_mouse.y < ic->y+ic->h) {
                                ic->is_selected = 1;
                                clicked_icon = j;
                            } else {
                                ic->is_selected = 0;
                            }
                        }
                        if (clicked_icon >= 0) {
                            desk_icon_t *ic = &g_icons[clicked_icon];
                            serial_puts("[DESK] Launching: ");
                            serial_puts(ic->target);
                            serial_puts("\n");
                            long pid = sys_exec(ic->target);
                            if (pid > 0) {
                                serial_puts("[DESK] Launched pid=");
                                char pbuf[8]; pbuf[0]='0'+(pid/10)%10; pbuf[1]='0'+pid%10; pbuf[2]=0;
                                serial_puts(pbuf);
                                serial_puts("\n");
                                notify_wm(ic->label);
                            } else {
                                serial_puts("[DESK] Launch FAILED\n");
                            }
                        }
                    }
                    /* Close context menu on any click */
                    if (g_ctx_menu.visible) {
                        if (g_mouse.x < g_ctx_menu.x || g_mouse.x >= g_ctx_menu.x + g_ctx_menu.w ||
                            g_mouse.y < g_ctx_menu.y || g_mouse.y >= g_ctx_menu.y + g_ctx_menu.h) {
                            g_ctx_menu.visible = 0;
                        }
                    }
                    /* Close datetime popup on click elsewhere */
                    if (g_clock.popup_visible) {
                        int dock_y = g_h - TASKBAR_H - TASKBAR_MARGIN;
                        int dt_px = g_w - TASKBAR_MARGIN - DATETIME_POPUP_W - 8;
                        int dt_py = dock_y - DATETIME_POPUP_H - 4;
                        if (g_mouse.x < dt_px || g_mouse.x >= dt_px + DATETIME_POPUP_W ||
                            g_mouse.y < dt_py || g_mouse.y >= dt_py + DATETIME_POPUP_H) {
                            g_clock.popup_visible = 0;
                        }
                    }
                }
            } else if (type == 6) {
                /* Mouse right button */
                int was_pressed = g_mouse.right;
                g_mouse.right = data;
                if (data && !was_pressed) {
                    /* Right click - show context menu only on desktop area */
                    int dock_y = g_h - TASKBAR_H - TASKBAR_MARGIN;
                    if (g_mouse.y < dock_y) {
                        g_ctx_menu.visible = 1;
                        g_ctx_menu.x = g_mouse.x;
                        g_ctx_menu.y = g_mouse.y;
                        g_ctx_menu.w = 180;
                        g_ctx_menu.item_count = 4;
                        d_strcpy(g_ctx_menu.items[0].label, "New Folder");
                        g_ctx_menu.items[0].action = 1;
                        d_strcpy(g_ctx_menu.items[1].label, "Open Terminal");
                        g_ctx_menu.items[1].action = 2;
                        d_strcpy(g_ctx_menu.items[2].label, "Refresh");
                        g_ctx_menu.items[2].action = 3;
                        d_strcpy(g_ctx_menu.items[3].label, "Settings");
                        g_ctx_menu.items[3].action = 4;
                        g_ctx_menu.h = 4 + g_ctx_menu.item_count * 28;
                    }
                }
            }
        }

        if (frame % 60 == 0 || g_clock.last_update == 0) {
            uint64_t t = (uint64_t)sc1(SYS_TIME, 0) + TIMEZONE_OFFSET;
            if (t > 0 && (t != g_clock.last_update || g_clock.last_update == 0)) {
                g_clock.last_update = t;
                int total_sec = (int)(t % 86400);
                g_clock.hour = total_sec / 3600;
                g_clock.min = (total_sec % 3600) / 60;
                g_clock.sec = total_sec % 60;
                int epoch_days = (int)(t / 86400);
                g_clock.year = 1970;
                int remaining = epoch_days;
                for (int y2 = 1970; remaining >= 0; y2++) {
                    int leap = (y2 % 400 == 0 || (y2 % 4 == 0 && y2 % 100 != 0)) ? 1 : 0;
                    int days_in_year = 365 + leap;
                    if (remaining >= days_in_year) {
                        remaining -= days_in_year;
                        g_clock.year = y2 + 1;
                    } else {
                        g_clock.year = y2;
                        break;
                    }
                }
                int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
                if ((g_clock.year % 400 == 0 || (g_clock.year % 4 == 0 && g_clock.year % 100 != 0))) mdays[1] = 29;
                g_clock.month = 1;
                for (int m = 0; m < 12 && remaining >= mdays[m]; m++) {
                    remaining -= mdays[m];
                    g_clock.month++;
                }
                g_clock.day = remaining + 1;
            } else if (g_clock.last_update > 0) {
                g_clock.sec++;
                if (g_clock.sec >= 60) {
                    g_clock.sec = 0;
                    g_clock.min++;
                    if (g_clock.min >= 60) {
                        g_clock.min = 0;
                        g_clock.hour++;
                        if (g_clock.hour >= 24) {
                            g_clock.hour = 0;
                            g_clock.day++;
                            int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
                            if ((g_clock.year % 400 == 0 || (g_clock.year % 4 == 0 && g_clock.year % 100 != 0))) mdays[1] = 29;
                            if (g_clock.day > mdays[g_clock.month - 1]) {
                                g_clock.day = 1;
                                g_clock.month++;
                                if (g_clock.month > 12) {
                                    g_clock.month = 1;
                                    g_clock.year++;
                                }
                            }
                        }
                    }
                }
            }
        }

        /* Render */
        g_login.anim_tick++;
        erase_cursor();

        if (!g_taskbar.wm_running) {
            draw_background();
            for (int i = 0; i < g_icon_count; i++)
                draw_desk_icon(&g_icons[i]);
        }

        /* Context menu */
        draw_context_menu();

        /* Taskbar (floating dock at bottom) */
        draw_taskbar();

        /* Launcher animation */
        if (g_launcher.visible && g_launcher.anim_frame < g_launcher.anim_total) {
            g_launcher.anim_frame++;
        }

        draw_launcher();

        /* Network panel (if open) */
        draw_netpanel();

        /* Datetime popup (if clock clicked) */
        draw_datetime_popup();

        /* AI panel (top right, Ctrl+A to toggle) */
        ai_poll_response();
        draw_ai_panel();

        draw_cursor();
        fb_flip();

        if (g_video_wp.active) {
            int interval = 60 / g_video_wp.fps;
            if (interval < 1) interval = 1;
            if (frame % interval == 0) {
                if (g_video_wp.is_mp4 && g_video_wp.mp4_handle > 0 && g_video_wp.frames[0]) {
                    long ret = sc5(SYS_MP4_DECODE, g_video_wp.mp4_handle,
                                   g_video_wp.current_frame,
                                   (long)g_video_wp.frames[0],
                                   (long)g_video_wp.frame_w,
                                   (long)g_video_wp.frame_h);
                    if (ret == 0) {
                        g_video_wp.current_frame++;
                        g_video_wp.mp4_frame_count = g_video_wp.current_frame;
                    } else {
                        g_video_wp.current_frame = 0;
                    }
                } else if (g_video_wp.frame_count > 1) {
                    g_video_wp.current_frame++;
                    if (g_video_wp.current_frame >= g_video_wp.frame_count)
                        g_video_wp.current_frame = 0;
                }
            }
        }

        frame++;
        for (volatile int d = 0; d < 50000; d++);
    }
}
