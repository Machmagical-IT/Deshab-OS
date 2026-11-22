/*
 * zinstall_gui.c - ZratroOS Software Installer (GUI Version)
 *
 * Win11 Fluent Design style graphical software center
 * Features:
 *   - Package list with search
 *   - Install/Update buttons
 *   - Progress indicator
 *   - Category tabs
 *
 * Compiles to: SYSTEM/prgm/windowprogram/zinstall
 */

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;
#define NULL ((void*)0)

/* ========== System calls ========== */
#define SYS_EXIT        0
#define SYS_READ        1
#define SYS_WRITE       2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_MKDIR       17
#define SYS_SOCKET      29
#define SYS_CONNECT     33
#define SYS_SEND        34
#define SYS_RECV        35
#define SYS_OPENDIR     20
#define SYS_READDIR     21
#define SYS_CLOSEDIR    22
#define SYS_MMAP        19
#define SYS_SLEEP       9
#define SYS_FB_SYNC     155
#define SYS_GETFBINFO   156
#define SYS_INPUT_POLL  188
#define SYS_TIME        114
#define SYS_EXEC        102
#define SYS_FORK        57

#define O_RDONLY    0
#define O_WRONLY    1
#define O_CREAT     0x40
#define O_TRUNC     0x200

#define AF_INET     2
#define SOCK_STREAM 1

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char sin_zero[8];
};

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
static long sys_open(const char *path, int flags) { return sc2(SYS_OPEN, (long)path, flags); }
static long sys_close(int fd) { return sc1(SYS_CLOSE, fd); }
static long sys_mkdir(const char *path) { return sc1(SYS_MKDIR, (long)path); }
static long sys_socket(int domain, int type, int proto) { return sc3(SYS_SOCKET, domain, type, proto); }
static long sys_connect(int fd, const void *addr, int len) { return sc3(SYS_CONNECT, fd, (long)addr, len); }
static long sys_send(int fd, const void *buf, long len, int flags) { return sc3(SYS_SEND, fd, (long)buf, len); }
static long sys_recv(int fd, void *buf, long len, int flags) { return sc3(SYS_RECV, fd, (long)buf, len); }
static long sys_exit(int code) { return sc1(SYS_EXIT, code); }
static long sys_mmap(long addr, long len, long prot, long flags, long fd, long off) {
    return sc6(SYS_MMAP, addr, len, prot, flags, fd, off);
}

static void serial_puts(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);
}

/* ========== Framebuffer ========== */
__attribute__((section(".bss.fb_base"))) uint64_t fb_base;
__attribute__((section(".bss.fb_width"))) uint32_t fb_width;
__attribute__((section(".bss.fb_height"))) uint32_t fb_height;
__attribute__((section(".bss.fb_pitch"))) uint32_t fb_pitch;

static struct { uint64_t base; uint32_t w, h, pitch, format; } g_fbi;
static int g_fb_bgr = 0;
static int g_w, g_h, g_stride;
static uint32_t *g_fb;

#define MKC(r,g,b) (g_fb_bgr ? (((uint32_t)(b)<<16)|((uint32_t)(g)<<8)|(r)) : (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(b)))

#define C_BG          MKC(32,32,40)
#define C_SURFACE     MKC(44,44,54)
#define C_SURFACE2    MKC(55,55,68)
#define C_ACCENT      MKC(0,120,215)
#define C_ACCENT_HOV  MKC(0,100,180)
#define C_TEXT         MKC(240,240,240)
#define C_TEXT_SEC     MKC(160,160,175)
#define C_GREEN       MKC(76,175,80)
#define C_RED         MKC(244,67,54)
#define C_YELLOW      MKC(255,193,7)
#define C_BORDER      MKC(60,60,75)
#define C_INPUT_BG    MKC(38,38,48)
#define C_HOVER       MKC(50,50,65)
#define C_SELECTED    MKC(40,80,140)

/* ========== String helpers ========== */
static int z_strlen(const char *s) { int n=0; while(s[n])n++; return n; }
static int z_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}
static char *z_strcpy(char *d, const char *s) {
    char *r = d; while ((*d++ = *s++)); return r;
}
static char *z_strcat(char *d, const char *s) {
    char *r = d; while (*d) d++; while ((*d++ = *s++)); return r;
}
static int z_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (uint8_t)a[i] - (uint8_t)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}

/* ========== Drawing ========== */
static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++) {
        uint32_t *p = &g_fb[row * g_stride + x];
        for (int col = 0; col < w; col++) *p++ = c;
    }
}

static void fill_rounded_rect(int x, int y, int w, int h, int r, uint32_t c) {
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    fill_rect(x + r, y, w - 2*r, h, c);
    fill_rect(x, y + r, w, h - 2*r, c);
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            if (dx*dx + dy*dy <= r*r) {
                int px1 = x + r - 1 - dx, py1 = y + r - 1 - dy;
                int px2 = x + w - r + dx, py2 = y + r - 1 - dy;
                int px3 = x + r - 1 - dx, py3 = y + h - r + dy;
                int px4 = x + w - r + dx, py4 = y + h - r + dy;
                if (px1 >= 0 && px1 < g_w && py1 >= 0 && py1 < g_h) g_fb[py1*g_stride+px1] = c;
                if (px2 >= 0 && px2 < g_w && py2 >= 0 && py2 < g_h) g_fb[py2*g_stride+px2] = c;
                if (px3 >= 0 && px3 < g_w && py3 >= 0 && py3 < g_h) g_fb[py3*g_stride+px3] = c;
                if (px4 >= 0 && px4 < g_w && py4 >= 0 && py4 < g_h) g_fb[py4*g_stride+px4] = c;
            }
        }
    }
}

static void draw_text(int x, int y, const char *s, uint32_t c) {
    static const uint8_t font[][8] = {
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
        {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, /* ! */
        {0x6c,0x6c,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
        {0x6c,0x6c,0xfe,0x6c,0xfe,0x6c,0x6c,0x00}, /* # */
        {0x18,0x3e,0x68,0x3c,0x16,0x7c,0x18,0x00}, /* $ */
        {0x00,0xc6,0xcc,0x18,0x30,0x66,0xc6,0x00}, /* % */
        {0x38,0x6c,0x38,0x76,0xdc,0xcc,0x76,0x00}, /* & */
        {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* ' */
        {0x0c,0x18,0x30,0x30,0x30,0x18,0x0c,0x00}, /* ( */
        {0x30,0x18,0x0c,0x0c,0x0c,0x18,0x30,0x00}, /* ) */
        {0x00,0x66,0x3c,0xff,0x3c,0x66,0x00,0x00}, /* * */
        {0x00,0x18,0x18,0x7e,0x18,0x18,0x00,0x00}, /* + */
        {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* , */
        {0x00,0x00,0x00,0x7e,0x00,0x00,0x00,0x00}, /* - */
        {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . */
        {0x06,0x0c,0x18,0x30,0x60,0xc0,0x80,0x00}, /* / */
        {0x7c,0xc6,0xce,0xde,0xf6,0xe6,0x7c,0x00}, /* 0 */
        {0x18,0x38,0x18,0x18,0x18,0x18,0x7e,0x00}, /* 1 */
        {0x7c,0xc6,0x06,0x1c,0x30,0x60,0xfe,0x00}, /* 2 */
        {0x7c,0xc6,0x06,0x3c,0x06,0xc6,0x7c,0x00}, /* 3 */
        {0x1c,0x3c,0x6c,0xcc,0xfe,0x0c,0x1e,0x00}, /* 4 */
        {0xfe,0xc0,0xfc,0x06,0x06,0xc6,0x7c,0x00}, /* 5 */
        {0x38,0x60,0xc0,0xfc,0xc6,0xc6,0x7c,0x00}, /* 6 */
        {0xfe,0xc6,0x0c,0x18,0x30,0x30,0x30,0x00}, /* 7 */
        {0x7c,0xc6,0xc6,0x7c,0xc6,0xc6,0x7c,0x00}, /* 8 */
        {0x7c,0xc6,0xc6,0x7e,0x06,0x0c,0x78,0x00}, /* 9 */
        {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* : */
        {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* ; */
        {0x06,0x0c,0x18,0x30,0x18,0x0c,0x06,0x00}, /* < */
        {0x00,0x00,0x7e,0x00,0x7e,0x00,0x00,0x00}, /* = */
        {0x60,0x30,0x18,0x0c,0x18,0x30,0x60,0x00}, /* > */
        {0x7c,0xc6,0x0c,0x18,0x18,0x00,0x18,0x00}, /* ? */
        {0x7c,0xc6,0xde,0xde,0xde,0xc0,0x78,0x00}, /* @ */
        {0x38,0x6c,0xc6,0xc6,0xfe,0xc6,0xc6,0x00}, /* A */
        {0xfc,0x66,0x66,0x7c,0x66,0x66,0xfc,0x00}, /* B */
        {0x3c,0x66,0xc0,0xc0,0xc0,0x66,0x3c,0x00}, /* C */
        {0xf8,0x6c,0x66,0x66,0x66,0x6c,0xf8,0x00}, /* D */
        {0xfe,0x62,0x68,0x78,0x68,0x62,0xfe,0x00}, /* E */
        {0xfe,0x62,0x68,0x78,0x68,0x60,0xf0,0x00}, /* F */
        {0x3c,0x66,0xc0,0xc0,0xce,0x66,0x3e,0x00}, /* G */
        {0xc6,0xc6,0xc6,0xfe,0xc6,0xc6,0xc6,0x00}, /* H */
        {0x3c,0x18,0x18,0x18,0x18,0x18,0x3c,0x00}, /* I */
        {0x1e,0x0c,0x0c,0x0c,0xcc,0xcc,0x78,0x00}, /* J */
        {0xe6,0x66,0x6c,0x78,0x6c,0x66,0xe6,0x00}, /* K */
        {0xf0,0x60,0x60,0x60,0x62,0x66,0xfe,0x00}, /* L */
        {0xc6,0xee,0xfe,0xfe,0xd6,0xc6,0xc6,0x00}, /* M */
        {0xc6,0xe6,0xf6,0xde,0xce,0xc6,0xc6,0x00}, /* N */
        {0x7c,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00}, /* O */
        {0xfc,0x66,0x66,0x7c,0x60,0x60,0xf0,0x00}, /* P */
        {0x7c,0xc6,0xc6,0xc6,0xd6,0xde,0x7c,0x06}, /* Q */
        {0xfc,0x66,0x66,0x7c,0x6c,0x66,0xe6,0x00}, /* R */
        {0x3c,0x66,0x30,0x18,0x0c,0x66,0x3c,0x00}, /* S */
        {0x7e,0x5a,0x18,0x18,0x18,0x18,0x3c,0x00}, /* T */
        {0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0x00}, /* U */
        {0xc6,0xc6,0xc6,0xc6,0xc6,0x6c,0x38,0x00}, /* V */
        {0xc6,0xc6,0xc6,0xd6,0xfe,0xee,0xc6,0x00}, /* W */
        {0xc6,0xc6,0x6c,0x38,0x6c,0xc6,0xc6,0x00}, /* X */
        {0x66,0x66,0x66,0x3c,0x18,0x18,0x3c,0x00}, /* Y */
        {0xfe,0xc6,0x8c,0x18,0x32,0x66,0xfe,0x00}, /* Z */
        {0x3c,0x30,0x30,0x30,0x30,0x30,0x3c,0x00}, /* [ */
        {0xc0,0x60,0x30,0x18,0x0c,0x06,0x02,0x00}, /* \ */
        {0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x3c,0x00}, /* ] */
        {0x10,0x38,0x6c,0xc6,0x00,0x00,0x00,0x00}, /* ^ */
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff}, /* _ */
    };
    while (*s) {
        int idx = -1;
        if (*s >= ' ' && *s <= '_') idx = *s - ' ';
        else if (*s >= 'a' && *s <= 'z') idx = *s - 'a' + 'A' - ' ';
        if (idx >= 0) {
            for (int row = 0; row < 8; row++) {
                uint8_t bits = font[idx][row];
                for (int col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        int px = x + col, py = y + row;
                        if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                            g_fb[py * g_stride + px] = c;
                    }
                }
            }
        }
        x += 8;
        s++;
    }
}

static int text_width(const char *s) {
    return z_strlen(s) * 8;
}

static uint32_t *g_real_fb = 0;  /* 真实帧缓冲区 (0x08000000) */

/* 静态 backbuf (BSS 段, 用户空间) - 最大支持 2560x1440 */
#define BACKBUF_MAX_PIXELS (2560 * 1440)
static uint32_t g_backbuf[BACKBUF_MAX_PIXELS];

static void fb_flip(void) {
    if (!g_real_fb || g_fb == g_real_fb) return;
    /* 双缓冲: backbuf → 真实帧缓冲区, 使用 8 字节对齐快速拷贝 */
    long total_qwords = (long)g_stride * (long)g_h / 2;
    uint64_t *src64 = (uint64_t *)g_fb;
    uint64_t *dst64 = (uint64_t *)g_real_fb;
    for (long i = 0; i < total_qwords; i++) dst64[i] = src64[i];
    /* 写屏障: 确保 framebuffer 写入对 GPU 可见 (实机需要) */
    asm volatile("sfence" ::: "memory");
}

/* ========== Mouse ========== */
static struct { int x, y, left; } g_mouse = {0};

/* ========== Package data ========== */
#define MAX_PKGS 32
static struct {
    char name[64];
    char desc[128];
    char url[256];
    int installed;
} g_pkgs[MAX_PKGS];
static int g_pkg_count = 0;

/* ========== App state ========== */
static struct {
    int scroll;
    int selected;
    int tab;
    char search[64];
    int search_len;
    int searching;
    int installing;
    int install_progress;
    char status[128];
    int running;
} g_app = {0};

/* ========== Network ========== */
static uint32_t resolve_host(const char *hostname) {
    (void)hostname;
    return (47 << 24) | (98 << 16) | (168 << 8) | 141;
}

static int http_get(const char *host, const char *path, char *response, int max_len) {
    int sock = sys_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = 0x5000;
    addr.sin_addr = resolve_host(host);
    for (int i = 0; i < 8; i++) addr.sin_zero[i] = 0;

    if (sys_connect(sock, &addr, sizeof(addr)) < 0) {
        sys_close(sock);
        return -1;
    }

    char req[512];
    int pos = 0;
    const char *m = "GET ", *hv = " HTTP/1.1\r\nHost: ", *cl = "\r\nConnection: close\r\n\r\n";
    while (*m) req[pos++] = *m++;
    while (*path) req[pos++] = *path++;
    while (*hv) req[pos++] = *hv++;
    while (*host) req[pos++] = *host++;
    while (*cl) req[pos++] = *cl++;
    sys_send(sock, req, pos, 0);

    int total = 0;
    while (total < max_len - 1) {
        long n = sys_recv(sock, response + total, max_len - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    response[total] = 0;
    sys_close(sock);
    return total;
}

static char *http_body(char *response) {
    char *p = response;
    while (*p) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n')
            return p + 4;
        p++;
    }
    return response;
}

static void parse_packages(const char *body) {
    g_pkg_count = 0;
    const char *p = body;
    while (*p && g_pkg_count < MAX_PKGS) {
        int i = 0;
        char name[64] = {0}, desc[128] = {0}, url[256] = {0};
        while (*p && *p != '|' && *p != '\n' && i < 63) name[i++] = *p++;
        name[i] = 0;
        if (*p == '|') p++;
        i = 0;
        while (*p && *p != '|' && *p != '\n' && i < 127) desc[i++] = *p++;
        desc[i] = 0;
        if (*p == '|') p++;
        i = 0;
        while (*p && *p != '\n' && *p != '\r' && i < 255) url[i++] = *p++;
        url[i] = 0;
        if (name[0] && url[0]) {
            z_strcpy(g_pkgs[g_pkg_count].name, name);
            z_strcpy(g_pkgs[g_pkg_count].desc, desc);
            z_strcpy(g_pkgs[g_pkg_count].url, url);
            g_pkgs[g_pkg_count].installed = 0;
            g_pkg_count++;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

static void fetch_packages(void) {
    z_strcpy(g_app.status, "Fetching package list...");
    g_app.searching = 1;
    char response[65536];
    int len = http_get("www.deaicup.com", "/zratro/osprogram/list.txt", response, sizeof(response));
    if (len <= 0) {
        z_strcpy(g_app.status, "Cannot fetch list - check network");
        g_app.searching = 0;
        return;
    }
    char *body = http_body(response);
    parse_packages(body);
    z_strcpy(g_app.status, "Ready");
    g_app.searching = 0;
}

static void install_package(int idx) {
    if (idx < 0 || idx >= g_pkg_count) return;
    g_app.installing = 1;
    g_app.install_progress = 0;
    z_strcpy(g_app.status, "Downloading ");
    z_strcat(g_app.status, g_pkgs[idx].name);

    char response[65536];
    int len = http_get("www.deaicup.com", g_pkgs[idx].url + 22, response, sizeof(response));
    if (len <= 0) {
        z_strcpy(g_app.status, "Download failed");
        g_app.installing = 0;
        return;
    }

    char *body = http_body(response);
    int body_len = len - (body - response);

    char path[128] = "/prgm/windowprogram/";
    z_strcat(path, g_pkgs[idx].name);

    long fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        z_strcpy(g_app.status, "Cannot create file");
        g_app.installing = 0;
        return;
    }
    sys_write(fd, body, body_len);
    sys_close(fd);

    g_pkgs[idx].installed = 1;
    z_strcpy(g_app.status, "Installed: ");
    z_strcat(g_app.status, path);
    g_app.installing = 0;
    g_app.install_progress = 100;
}

/* ========== GUI Drawing ========== */
#define WIN_MARGIN 60
#define TITLEBAR_H 36
#define TAB_H 36

static void draw_window(void) {
    int wx = WIN_MARGIN, wy = WIN_MARGIN;
    int ww = g_w - 2 * WIN_MARGIN, wh = g_h - 2 * WIN_MARGIN;

    fill_rounded_rect(wx + 4, wy + 4, ww, wh, 12, MKC(4,4,8));
    fill_rounded_rect(wx, wy, ww, wh, 12, C_SURFACE);
    for (int i = 0; i < 2; i++)
        fill_rounded_rect(wx + i, wy + i, ww - 2*i, wh - 2*i, 12 - i, C_BORDER);
    fill_rounded_rect(wx + 2, wy + 2, ww - 4, wh - 4, 10, C_SURFACE);

    /* Title bar */
    fill_rounded_rect(wx + 2, wy + 2, ww - 4, TITLEBAR_H, 10, MKC(28,28,36));
    fill_rect(wx + 12, wy + TITLEBAR_H - 2, ww - 24, 2, C_SURFACE);
    draw_text(wx + 16, wy + 12, "ZratroOS Software Center", C_TEXT);

    /* Close button */
    int close_x = wx + ww - 32, close_y = wy + 8;
    int hov = (g_mouse.x >= close_x && g_mouse.x < close_x + 24 &&
               g_mouse.y >= close_y && g_mouse.y < close_y + 24);
    fill_rounded_rect(close_x, close_y, 24, 24, 4, hov ? C_RED : MKC(60,60,70));
    draw_text(close_x + 8, close_y + 8, "X", C_TEXT);

    /* Tabs */
    int tab_y = wy + TITLEBAR_H + 4;
    const char *tabs[] = {"All", "Installed", "Search"};
    for (int t = 0; t < 3; t++) {
        int tab_x = wx + 16 + t * 90;
        int is_sel = (g_app.tab == t);
        int tab_hov = (g_mouse.x >= tab_x && g_mouse.x < tab_x + 80 &&
                       g_mouse.y >= tab_y && g_mouse.y < tab_y + TAB_H);
        fill_rounded_rect(tab_x, tab_y, 80, TAB_H, 6, is_sel ? C_ACCENT : (tab_hov ? C_HOVER : C_SURFACE2));
        int tw = text_width(tabs[t]);
        draw_text(tab_x + (80 - tw)/2, tab_y + (TAB_H - 8)/2, tabs[t], is_sel ? C_TEXT : C_TEXT_SEC);
    }

    /* Search bar (visible on Search tab) */
    int content_y = tab_y + TAB_H + 8;
    if (g_app.tab == 2) {
        int sb_x = wx + 16, sb_y = content_y;
        int sb_w = ww - 32, sb_h = 28;
        fill_rounded_rect(sb_x, sb_y, sb_w, sb_h, 6, C_INPUT_BG);
        fill_rounded_rect(sb_x, sb_y, sb_w, 1, 0, C_BORDER);
        if (g_app.search_len > 0) {
            draw_text(sb_x + 8, sb_y + 10, g_app.search, C_TEXT);
        } else {
            draw_text(sb_x + 8, sb_y + 10, "Type to search...", C_TEXT_SEC);
        }
        content_y += sb_h + 8;
    }

    /* Package list */
    int list_x = wx + 16, list_w = ww - 32;
    int item_h = 56;
    int visible_items = (wy + wh - content_y - 40) / item_h;
    if (visible_items < 1) visible_items = 1;

    int display_count = 0;
    for (int i = 0; i < g_pkg_count && display_count < visible_items; i++) {
        int show = 0;
        if (g_app.tab == 0) show = 1;
        else if (g_app.tab == 1) show = g_pkgs[i].installed;
        else if (g_app.tab == 2) {
            if (g_app.search_len == 0) show = 1;
            else {
                const char *n = g_pkgs[i].name;
                const char *q = g_app.search;
                for (int j = 0; n[j]; j++) {
                    int k = 0;
                    while (q[k] && n[j+k] == q[k]) k++;
                    if (q[k] == 0) { show = 1; break; }
                }
            }
        }
        if (!show) continue;

        int iy = content_y + display_count * item_h;
        int is_sel = (g_app.selected == i);
        int is_hov = (g_mouse.x >= list_x && g_mouse.x < list_x + list_w &&
                      g_mouse.y >= iy && g_mouse.y < iy + item_h);

        fill_rounded_rect(list_x, iy, list_w, item_h - 4, 8, is_sel ? C_SELECTED : (is_hov ? C_HOVER : C_SURFACE2));

        /* Package icon */
        int icon_x = list_x + 12, icon_y = iy + 8;
        fill_rounded_rect(icon_x, icon_y, 36, 36, 8, g_pkgs[i].installed ? C_GREEN : C_ACCENT);
        draw_text(icon_x + 6, icon_y + 14, "P", C_TEXT);

        /* Package name + desc */
        draw_text(list_x + 56, iy + 8, g_pkgs[i].name, C_TEXT);
        if (g_pkgs[i].desc[0])
            draw_text(list_x + 56, iy + 22, g_pkgs[i].desc, C_TEXT_SEC);

        /* Install/Installed button */
        int btn_w = 72, btn_h = 24;
        int btn_x = list_x + list_w - btn_w - 12;
        int btn_y = iy + (item_h - 4 - btn_h) / 2;
        int btn_hov = (g_mouse.x >= btn_x && g_mouse.x < btn_x + btn_w &&
                       g_mouse.y >= btn_y && g_mouse.y < btn_y + btn_h);
        if (g_pkgs[i].installed) {
            fill_rounded_rect(btn_x, btn_y, btn_w, btn_h, 4, MKC(40,80,40));
            draw_text(btn_x + 8, btn_y + 8, "Installed", C_GREEN);
        } else {
            fill_rounded_rect(btn_x, btn_y, btn_w, btn_h, 4, btn_hov ? C_ACCENT_HOV : C_ACCENT);
            draw_text(btn_x + 14, btn_y + 8, "Install", C_TEXT);
        }

        display_count++;
    }

    if (g_pkg_count == 0 && !g_app.searching) {
        const char *msg = "No packages loaded. Click Refresh.";
        int mw = text_width(msg);
        draw_text(wx + (ww - mw)/2, content_y + 40, msg, C_TEXT_SEC);
    }

    if (g_app.searching) {
        const char *msg = "Loading...";
        int mw = text_width(msg);
        draw_text(wx + (ww - mw)/2, content_y + 40, msg, C_YELLOW);
    }

    /* Progress bar (if installing) */
    if (g_app.installing) {
        int pb_y = wy + wh - 36;
        fill_rounded_rect(wx + 16, pb_y, ww - 32, 6, 3, C_INPUT_BG);
        int pw = (ww - 32) * g_app.install_progress / 100;
        if (pw > 0) fill_rounded_rect(wx + 16, pb_y, pw, 6, 3, C_ACCENT);
    }

    /* Status bar */
    int status_y = wy + wh - 24;
    draw_text(wx + 16, status_y, g_app.status, C_TEXT_SEC);

    /* Refresh button */
    int ref_x = wx + ww - 100, ref_y = status_y - 4;
    int ref_hov = (g_mouse.x >= ref_x && g_mouse.x < ref_x + 80 &&
                   g_mouse.y >= ref_y && g_mouse.y < ref_y + 20);
    fill_rounded_rect(ref_x, ref_y, 80, 20, 4, ref_hov ? C_HOVER : C_SURFACE2);
    draw_text(ref_x + 14, ref_y + 6, "Refresh", C_TEXT_SEC);
}

/* ========== Main ========== */
void _start(void) {
    serial_puts("[ZINSTALL-GUI] Starting...\n");

    g_fbi.base = fb_base; g_fbi.w = fb_width; g_fbi.h = fb_height; g_fbi.pitch = fb_pitch;
    sc2(SYS_GETFBINFO, (long)&g_fbi, 0);
    g_w = (int)g_fbi.w; g_h = (int)g_fbi.h;
    g_stride = (int)g_fbi.pitch / 4;
    g_fb = (uint32_t*)(uint64_t)g_fbi.base;

    if (g_fbi.format == 3 || g_fbi.format == 4) g_fb_bgr = 0;
    else if (g_fbi.format == 6 || g_fbi.format == 7) g_fb_bgr = 1;
    else g_fb_bgr = 0;

    /* 双缓冲 (使用静态 BSS backbuf, 而不是 mmap):
     * 注意: 内核 sys_mmap 使用 kmalloc 返回内核地址, 用户态访问会崩溃
     * 改用静态 BSS 数组 (位于用户空间), 安全可靠 */
    g_real_fb = g_fb;
    long needed = (long)g_stride * (long)g_h;
    if (needed > 0 && needed <= BACKBUF_MAX_PIXELS) {
        g_fb = g_backbuf;
        serial_puts("[ZINSTALL-GUI] Using static backbuf\n");
    } else {
        serial_puts("[ZINSTALL-GUI] WARNING: resolution exceeds backbuf, direct fb write\n");
    }

    g_mouse.x = g_w / 2;
    g_mouse.y = g_h / 2;
    g_app.running = 1;
    g_app.tab = 0;
    z_strcpy(g_app.status, "Ready - click Refresh to load packages");

    fetch_packages();

    while (g_app.running) {
        uint32_t events[32];
        int nevt = (int)sys_input_poll(events, 32);
        for (int i = 0; i < nevt; i++) {
            uint32_t evt = events[i];
            int type = (int)(evt >> 16);
            int data = (int)(evt & 0xFFFF);

            if (type == 3) {
                g_mouse.x = data;
                if (g_mouse.x < 0) g_mouse.x = 0;
                if (g_mouse.x >= g_w) g_mouse.x = g_w - 1;
            } else if (type == 4) {
                g_mouse.y = data;
                if (g_mouse.y < 0) g_mouse.y = 0;
                if (g_mouse.y >= g_h) g_mouse.y = g_h - 1;
            } else if (type == 5) {
                int was = g_mouse.left;
                g_mouse.left = data;
                if (data && !was) {
                    int wx = WIN_MARGIN, wy = WIN_MARGIN;
                    int ww = g_w - 2 * WIN_MARGIN, wh = g_h - 2 * WIN_MARGIN;

                    /* Close button */
                    int close_x = wx + ww - 32, close_y = wy + 8;
                    if (g_mouse.x >= close_x && g_mouse.x < close_x + 24 &&
                        g_mouse.y >= close_y && g_mouse.y < close_y + 24) {
                        g_app.running = 0;
                        continue;
                    }

                    /* Tab clicks */
                    int tab_y = wy + TITLEBAR_H + 4;
                    for (int t = 0; t < 3; t++) {
                        int tab_x = wx + 16 + t * 90;
                        if (g_mouse.x >= tab_x && g_mouse.x < tab_x + 80 &&
                            g_mouse.y >= tab_y && g_mouse.y < tab_y + TAB_H) {
                            g_app.tab = t;
                        }
                    }

                    /* Package list clicks */
                    int content_y = tab_y + TAB_H + 8;
                    if (g_app.tab == 2) content_y += 36;
                    int item_h = 56;
                    int list_x = wx + 16, list_w = ww - 32;
                    int visible_items = (wy + wh - content_y - 40) / item_h;
                    int display_count = 0;
                    for (int pi = 0; pi < g_pkg_count && display_count < visible_items; pi++) {
                        int show = 0;
                        if (g_app.tab == 0) show = 1;
                        else if (g_app.tab == 1) show = g_pkgs[pi].installed;
                        else if (g_app.tab == 2) {
                            if (g_app.search_len == 0) show = 1;
                            else {
                                const char *n = g_pkgs[pi].name;
                                const char *q = g_app.search;
                                for (int j = 0; n[j]; j++) {
                                    int k = 0;
                                    while (q[k] && n[j+k] == q[k]) k++;
                                    if (q[k] == 0) { show = 1; break; }
                                }
                            }
                        }
                        if (!show) continue;

                        int iy = content_y + display_count * item_h;
                        if (g_mouse.x >= list_x && g_mouse.x < list_x + list_w &&
                            g_mouse.y >= iy && g_mouse.y < iy + item_h) {
                            g_app.selected = pi;
                            /* Install button */
                            int btn_w = 72, btn_h = 24;
                            int btn_x = list_x + list_w - btn_w - 12;
                            int btn_y = iy + (item_h - 4 - btn_h) / 2;
                            if (g_mouse.x >= btn_x && g_mouse.x < btn_x + btn_w &&
                                g_mouse.y >= btn_y && g_mouse.y < btn_y + btn_h) {
                                if (!g_pkgs[pi].installed) {
                                    install_package(pi);
                                }
                            }
                        }
                        display_count++;
                    }

                    /* Refresh button */
                    int ref_x = wx + ww - 100, ref_y = wy + wh - 28;
                    if (g_mouse.x >= ref_x && g_mouse.x < ref_x + 80 &&
                        g_mouse.y >= ref_y && g_mouse.y < ref_y + 20) {
                        fetch_packages();
                    }
                }
            } else if (type == 1) {
                int kc = data;
                if (kc == 1) {
                    g_app.running = 0;
                } else if (kc == 14) {
                    if (g_app.tab == 2 && g_app.search_len > 0) {
                        g_app.search[--g_app.search_len] = 0;
                    }
                } else if (kc < 128) {
                    static const char sc2c[] = {
                        0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,
                        '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
                        0,'a','s','d','f','g','h','j','k','l',';','\'','`',0,
                        '\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',
                    };
                    char ch = (kc < (int)sizeof(sc2c)) ? sc2c[kc] : 0;
                    if (ch >= ' ' && ch < 127 && g_app.tab == 2 && g_app.search_len < 63) {
                        g_app.search[g_app.search_len++] = ch;
                        g_app.search[g_app.search_len] = 0;
                    }
                }
            }
        }

        fill_rect(0, 0, g_w, g_h, C_BG);
        draw_window();

        /* 绘制鼠标光标 (7×7 白色十字) */
        {
            int cx = g_mouse.x, cy = g_mouse.y;
            uint32_t cur_col = MKC(255,255,255);
            for (int dy = -3; dy <= 3; dy++) {
                int px = cx + dy, py = cy;
                if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                    g_fb[py * g_stride + px] = cur_col;
                py = cy + dy; px = cx;
                if (px >= 0 && px < g_w && py >= 0 && py < g_h)
                    g_fb[py * g_stride + px] = cur_col;
            }
            if (cx >= 0 && cx < g_w && cy >= 0 && cy < g_h)
                g_fb[cy * g_stride + cx] = cur_col;
        }

        fb_flip();

        sc1(SYS_SLEEP, 2); /* 休眠约 2 ticks (~16ms), 与 desk 一致 */
    }

    sys_exit(0);
}
