/*
 * fm.c - ZratroOS File Manager
 *
 * Win11 + KDE Plasma fusion style file manager
 * Features:
 *   - Dolphin-style sidebar with places
 *   - Win11 Fluent Design address bar
 *   - Icon and list view modes
 *   - Correct file info display (size, type, dates)
 *   - Navigation history (back/forward/up)
 *   - Breadcrumb path navigation
 *
 * Compiles to: SYSTEM/prgm/sys/fm
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
#define SYS_STAT        6
#define SYS_OPENDIR     20
#define SYS_READDIR     21
#define SYS_CLOSEDIR    22
#define SYS_MMAP        11
#define SYS_FB_SYNC     155
#define SYS_GETFBINFO   156
#define SYS_INPUT_POLL  188
#define SYS_TIME        114

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
static long sys_fb_sync(void *fb) { return sc1(SYS_FB_SYNC, (long)fb); }
static long sys_input_poll(uint32_t *kc, int max) { return sc2(SYS_INPUT_POLL, (long)kc, (long)max); }

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
static int g_fb_bgr = 0;

/* ========== Colors - Win11+KDE Fusion ========== */
#define MKC_RGB(r,g,b) (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(b))
#define MKC_BGR(r,g,b) (((uint32_t)(b)<<16)|((uint32_t)(g)<<8)|(r))
#define MKC(r,g,b) (g_fb_bgr ? MKC_BGR(r,g,b) : MKC_RGB(r,g,b))

#define C_MICA_BG       MKC(32, 32, 38)
#define C_MICA_BG2      MKC(44, 44, 52)
#define C_SURFACE       MKC(40, 40, 48)
#define C_SURFACE_VAR   MKC(49, 49, 57)
#define C_ACCENT        MKC(96, 165, 250)
#define C_ACCENT_HOVER  MKC(118, 179, 252)
#define C_TEXT          MKC(235, 235, 242)
#define C_TEXT_SEC      MKC(154, 154, 170)
#define C_TEXT_DIS      MKC(104, 104, 120)
#define C_BORDER        MKC(62, 62, 72)
#define C_BORDER_LIGHT  MKC(78, 78, 90)
#define C_HOVER         MKC(55, 55, 68)
#define C_SELECTED      MKC(60, 100, 200)
#define C_ORANGE        MKC(247, 179, 112)
#define C_GREEN         MKC(80, 220, 120)
#define C_WHITE         MKC(255, 255, 255)
#define C_BLACK         MKC(0, 0, 0)
#define C_SIDEBAR_BG    MKC(28, 28, 36)
#define C_TOOLBAR_BG    MKC(36, 36, 44)
#define C_ADDR_BG       MKC(48, 48, 58)

/* ========== Memory Pool ========== */
static char g_pool[8 * 1024 * 1024];
static int g_pool_pos = 0;

static void *fm_alloc(int size) {
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
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x07,0x00,0x07,0x00,0x00,0x00},{0x00,0x14,0x7F,0x14,0x7F,0x14,0x00,0x00},
    {0x00,0x24,0x2A,0x7F,0x2A,0x12,0x00,0x00},{0x00,0x23,0x13,0x08,0x64,0x62,0x00,0x00},
    {0x00,0x36,0x49,0x55,0x22,0x50,0x00,0x00},{0x00,0x00,0x05,0x03,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1C,0x22,0x41,0x00,0x00,0x00},{0x00,0x00,0x41,0x22,0x1C,0x00,0x00,0x00},
    {0x00,0x14,0x08,0x3E,0x08,0x14,0x00,0x00},{0x00,0x08,0x08,0x3E,0x08,0x08,0x00,0x00},
    {0x00,0x00,0x50,0x30,0x00,0x00,0x00,0x00},{0x00,0x08,0x08,0x08,0x08,0x08,0x00,0x00},
    {0x00,0x00,0x60,0x60,0x00,0x00,0x00,0x00},{0x00,0x20,0x10,0x08,0x04,0x02,0x00,0x00},
    {0x00,0x3E,0x51,0x49,0x45,0x3E,0x00,0x00},{0x00,0x00,0x42,0x7F,0x40,0x00,0x00,0x00},
    {0x00,0x42,0x61,0x51,0x49,0x46,0x00,0x00},{0x00,0x21,0x41,0x45,0x4B,0x31,0x00,0x00},
    {0x00,0x18,0x14,0x12,0x7F,0x10,0x00,0x00},{0x00,0x27,0x45,0x45,0x45,0x39,0x00,0x00},
    {0x00,0x3C,0x4A,0x49,0x49,0x30,0x00,0x00},{0x00,0x01,0x71,0x09,0x05,0x03,0x00,0x00},
    {0x00,0x36,0x49,0x49,0x49,0x36,0x00,0x00},{0x00,0x06,0x49,0x49,0x29,0x1E,0x00,0x00},
    {0x00,0x00,0x36,0x36,0x00,0x00,0x00,0x00},{0x00,0x00,0x56,0x36,0x00,0x00,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41,0x00,0x00,0x00},{0x00,0x14,0x14,0x14,0x14,0x14,0x00,0x00},
    {0x00,0x00,0x41,0x22,0x14,0x08,0x00,0x00},{0x00,0x02,0x01,0x51,0x09,0x06,0x00,0x00},
    {0x00,0x32,0x49,0x79,0x41,0x3E,0x00,0x00},{0x00,0x7E,0x11,0x11,0x11,0x7E,0x00,0x00},
    {0x00,0x7F,0x49,0x49,0x49,0x36,0x00,0x00},{0x00,0x3E,0x41,0x41,0x41,0x22,0x00,0x00},
    {0x00,0x7F,0x41,0x41,0x22,0x1C,0x00,0x00},{0x00,0x7F,0x49,0x49,0x49,0x41,0x00,0x00},
    {0x00,0x7F,0x09,0x09,0x09,0x01,0x00,0x00},{0x00,0x3E,0x41,0x49,0x49,0x7A,0x00,0x00},
    {0x00,0x7F,0x08,0x08,0x08,0x7F,0x00,0x00},{0x00,0x00,0x41,0x7F,0x41,0x00,0x00,0x00},
    {0x00,0x20,0x40,0x41,0x3F,0x01,0x00,0x00},{0x00,0x7F,0x08,0x14,0x22,0x41,0x00,0x00},
    {0x00,0x7F,0x40,0x40,0x40,0x40,0x00,0x00},{0x00,0x7F,0x02,0x0C,0x02,0x7F,0x00,0x00},
    {0x00,0x7F,0x04,0x08,0x10,0x7F,0x00,0x00},{0x00,0x3E,0x41,0x41,0x41,0x3E,0x00,0x00},
    {0x00,0x7F,0x09,0x09,0x09,0x06,0x00,0x00},{0x00,0x3E,0x41,0x51,0x21,0x5E,0x00,0x00},
    {0x00,0x7F,0x09,0x19,0x29,0x46,0x00,0x00},{0x00,0x46,0x49,0x49,0x49,0x31,0x00,0x00},
    {0x00,0x01,0x01,0x7F,0x01,0x01,0x00,0x00},{0x00,0x3F,0x40,0x40,0x40,0x3F,0x00,0x00},
    {0x00,0x1F,0x20,0x40,0x20,0x1F,0x00,0x00},{0x00,0x3F,0x40,0x38,0x40,0x3F,0x00,0x00},
    {0x00,0x63,0x14,0x08,0x14,0x63,0x00,0x00},{0x00,0x07,0x08,0x70,0x08,0x07,0x00,0x00},
    {0x00,0x61,0x51,0x49,0x45,0x43,0x00,0x00},{0x00,0x00,0x7F,0x41,0x41,0x00,0x00,0x00},
    {0x00,0x02,0x04,0x08,0x10,0x20,0x00,0x00},{0x00,0x41,0x41,0x7F,0x00,0x00,0x00,0x00},
    {0x00,0x04,0x02,0x01,0x02,0x04,0x00,0x00},{0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40},
    {0x00,0x00,0x01,0x02,0x04,0x00,0x00,0x00},{0x00,0x20,0x54,0x54,0x54,0x78,0x00,0x00},
    {0x00,0x7F,0x48,0x44,0x44,0x38,0x00,0x00},{0x00,0x38,0x44,0x44,0x44,0x20,0x00,0x00},
    {0x00,0x38,0x44,0x44,0x48,0x7F,0x00,0x00},{0x00,0x38,0x54,0x54,0x54,0x18,0x00,0x00},
    {0x00,0x08,0x7E,0x09,0x01,0x02,0x00,0x00},{0x00,0x18,0xA4,0xA4,0xA4,0x7C,0x00,0x00},
    {0x00,0x7F,0x08,0x04,0x04,0x78,0x00,0x00},{0x00,0x00,0x44,0x7D,0x40,0x00,0x00,0x00},
    {0x00,0x40,0x80,0x84,0x7D,0x00,0x00,0x00},{0x00,0x7F,0x10,0x28,0x44,0x00,0x00,0x00},
    {0x00,0x00,0x41,0x7F,0x40,0x00,0x00,0x00},{0x00,0x7C,0x04,0x18,0x04,0x78,0x00,0x00},
    {0x00,0x7C,0x08,0x04,0x04,0x78,0x00,0x00},{0x00,0x38,0x44,0x44,0x44,0x38,0x00,0x00},
    {0x00,0xFC,0x24,0x24,0x24,0x18,0x00,0x00},{0x00,0x18,0x24,0x24,0x18,0xFC,0x00,0x00},
    {0x00,0x7C,0x08,0x04,0x04,0x08,0x00,0x00},{0x00,0x48,0x54,0x54,0x54,0x20,0x00,0x00},
    {0x00,0x04,0x3F,0x44,0x40,0x20,0x00,0x00},{0x00,0x3C,0x40,0x40,0x20,0x7C,0x00,0x00},
    {0x00,0x1C,0x20,0x40,0x20,0x1C,0x00,0x00},{0x00,0x3C,0x40,0x30,0x40,0x3C,0x00,0x00},
    {0x00,0x44,0x28,0x10,0x28,0x44,0x00,0x00},{0x00,0x1C,0xA0,0xA0,0xA0,0x7C,0x00,0x00},
    {0x00,0x44,0x64,0x54,0x4C,0x44,0x00,0x00},
};

/* ========== Drawing ========== */
static uint32_t *g_fb;
static int g_w, g_h, g_stride;

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (x >= g_w || y >= g_h || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_w ? g_w : x + w, y1 = y + h > g_h ? g_h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &g_fb[yy * g_stride + x0];
        for (int xx = x1 - x0; xx > 0; xx--) *row++ = color;
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

static int text_width(const char *text) { return d_strlen(text) * 8; }

/* ========== File Manager Data Structures ========== */
#define MAX_ITEMS       512
#define PATH_MAX        256
#define NAME_MAX        128
#define MAX_HISTORY     32

typedef enum { FM_DIR = 0, FM_FILE, FM_SYMLINK, FM_UNKNOWN } fm_type_t;

typedef struct {
    char name[NAME_MAX];
    char path[PATH_MAX];
    fm_type_t type;
    uint64_t size;
    uint32_t mode;
    uint64_t mtime;
    int is_selected;
    int is_hovered;
} fm_item_t;

typedef struct {
    fm_item_t items[MAX_ITEMS];
    int count;
    int selected_count;
    int scroll_y;
    int view_mode; /* 0=icons, 1=list, 2=details */
    char current_path[PATH_MAX];
    char history[MAX_HISTORY][PATH_MAX];
    int history_pos;
    int history_count;
} fm_filelist_t;

/* Sidebar places */
typedef struct {
    char name[64];
    char path[PATH_MAX];
    uint32_t icon_color;
} fm_place_t;

/* Main FM state */
static struct {
    fm_filelist_t files;
    fm_place_t places[16];
    int place_count;

    /* Layout */
    int sidebar_w;
    int toolbar_h;
    int addr_h;
    int status_h;
    int content_x, content_y, content_w, content_h;

    /* Address bar */
    char address_bar[PATH_MAX];
    int addr_focused;
    int addr_cursor;

    /* Mouse */
    int mouse_x, mouse_y;
    int hover_item;
    int hover_btn; /* -1=none, 0=back, 1=forward, 2=up, 3=refresh, 4=view */

    /* Window position (managed by WM) */
    int win_x, win_y, win_w, win_h;
} g_fm;

/* ========== File System Operations ========== */

/* stat structure matching kernel */
struct fm_stat {
    uint64_t size;
    uint32_t type;
    uint32_t attr;
    uint64_t ctime;
    uint64_t mtime;
    uint64_t atime;
    uint32_t inode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint32_t blksize;
    uint64_t blocks;
};

/* dirent structure matching kernel */
struct fm_dirent {
    uint64_t ino;
    int64_t off;
    uint16_t reclen;
    char name[256];
    unsigned char type;
};

static void fm_get_parent(const char *path, char *parent, int max_len) {
    int len = d_strlen(path);
    if (len <= 1) { d_strncpy(parent, "/", max_len); return; }
    int end = len - 1;
    if (path[end] == '/') end--;
    int last_slash = -1;
    for (int i = end; i >= 0; i--) { if (path[i] == '/') { last_slash = i; break; } }
    if (last_slash <= 0) d_strncpy(parent, "/", max_len);
    else { d_strncpy(parent, path, last_slash < max_len ? last_slash : max_len-1); parent[last_slash] = 0; }
}

static void fm_get_basename(const char *path, char *name, int max_len) {
    int len = d_strlen(path);
    int end = len-1; if (end>0 && path[end]=='/') end--;
    int last_slash = -1;
    for (int i = end; i >= 0; i--) { if (path[i]=='/') { last_slash=i; break; } }
    int start = last_slash+1;
    int copy_len = len-start; if (copy_len >= max_len) copy_len = max_len-1;
    d_strncpy(name, path+start, copy_len); name[copy_len] = 0;
}

static void fm_join_path(char *result, const char *dir, const char *file, int max_len) {
    int dl = d_strlen(dir), fl = d_strlen(file);
    int need_slash = (dl > 0 && dir[dl-1] != '/') ? 1 : 0;
    int total = dl + need_slash + fl;
    if (total >= max_len) total = max_len - 1;
    d_strncpy(result, dir, dl < total ? dl : total);
    int pos = dl < total ? dl : total;
    if (need_slash && pos < total) result[pos++] = '/';
    int fc = total - pos;
    if (fc > 0) { d_strncpy(result+pos, file, fc); result[pos+fc]=0; }
    else result[pos]=0;
}

static void fm_load_directory(const char *path) {
    fm_filelist_t *fl = &g_fm.files;
    fl->count = 0;
    fl->selected_count = 0;

    serial_puts("[FM] Loading: ");
    serial_puts(path);
    serial_puts("\n");

    /* Add parent dir entry */
    if (d_strcmp(path, "/") != 0) {
        fm_item_t *it = &fl->items[0];
        d_strcpy(it->name, "..");
        fm_get_parent(path, it->path, PATH_MAX);
        it->type = FM_DIR; it->size = 0; it->is_selected = 0; it->is_hovered = 0;
        it->mode = 0; it->mtime = 0;
        fl->count++;
    }

    /* Try opendir/readdir */
    long dir_fd = sc1(SYS_OPENDIR, (long)path);

    if (dir_fd >= 0) {
        struct fm_dirent de;
        while (fl->count < MAX_ITEMS) {
            long ret = sc3(SYS_READDIR, dir_fd, (long)&de, (long)sizeof(de));
            if (ret <= 0) break;
            if (d_strcmp(de.name, ".") == 0) continue;

            fm_item_t *it = &fl->items[fl->count];

            /* Extract basename - kernel may return full path */
            const char *basename = de.name;
            for (const char *p = de.name; *p; p++) {
                if (*p == '/' && *(p+1)) basename = p + 1;
            }
            d_strncpy(it->name, basename, NAME_MAX-1);
            it->name[NAME_MAX-1] = '\0';

            /* Build full path */
            if (de.name[0] == '/') {
                d_strncpy(it->path, de.name, PATH_MAX-1);
                it->path[PATH_MAX-1] = '\0';
            } else {
                fm_join_path(it->path, path, de.name, PATH_MAX);
            }

            /* Determine type */
            it->type = (de.type == 2) ? FM_DIR : (de.type == 3) ? FM_SYMLINK : FM_FILE;
            it->size = 0;
            it->is_selected = 0;
            it->is_hovered = 0;
            it->mode = 0;
            it->mtime = 0;

            /* Get detailed file info via stat */
            struct fm_stat st;
            long stat_ret = sc2(SYS_STAT, (long)it->path, (long)&st);
            if (stat_ret == 0) {
                it->size = st.size;
                it->mode = st.mode;
                it->mtime = st.mtime;
                /* Fix type from stat if readdir type was wrong */
                if (st.type == 2) it->type = FM_DIR;
                else if (st.type == 3) it->type = FM_SYMLINK;
            }

            fl->count++;
        }
        sc1(SYS_CLOSEDIR, dir_fd);
        serial_puts("[FM] Directory loaded, items: ");
        { char buf[16]; int n=fl->count, len=0; if(n==0) buf[len++]='0'; else { char nb[16]; int ni=0; while(n>0){nb[ni++]='0'+(n%10);n/=10;} for(int j=0;j<ni;j++) buf[len++]=nb[ni-1-j]; } buf[len]=0; serial_puts(buf); }
        serial_puts("\n");
    } else {
        serial_puts("[FM] opendir failed for: ");
        serial_puts(path);
        serial_puts("\n");
    }

    fl->scroll_y = 0;
    d_strncpy(g_fm.address_bar, path, PATH_MAX);
}

static void fm_navigate(const char *path) {
    fm_filelist_t *fl = &g_fm.files;
    if (fl->history_pos < MAX_HISTORY - 1) fl->history_pos++;
    d_strcpy(fl->history[fl->history_pos], path);
    fl->history_count = fl->history_pos + 1;
    d_strncpy(fl->current_path, path, PATH_MAX);
    fm_load_directory(path);
}

static void fm_go_back(void) {
    fm_filelist_t *fl = &g_fm.files;
    if (fl->history_pos <= 0) return;
    fl->history_pos--;
    const char *p = fl->history[fl->history_pos];
    d_strcpy(fl->current_path, p);
    fm_load_directory(p);
}

static void fm_go_forward(void) {
    fm_filelist_t *fl = &g_fm.files;
    if (fl->history_pos >= fl->history_count - 1) return;
    fl->history_pos++;
    const char *p = fl->history[fl->history_pos];
    d_strcpy(fl->current_path, p);
    fm_load_directory(p);
}

static void fm_go_up(void) {
    char parent[PATH_MAX];
    fm_get_parent(g_fm.files.current_path, parent, PATH_MAX);
    fm_navigate(parent);
}

/* ========== Init places ========== */
static void fm_init_places(void) {
    fm_place_t *p = g_fm.places;
    g_fm.place_count = 0;

    d_strcpy(p[0].name, "Home"); d_strcpy(p[0].path, "/"); p[0].icon_color = C_ACCENT;
    d_strcpy(p[1].name, "System"); d_strcpy(p[1].path, "/system"); p[1].icon_color = C_ORANGE;
    d_strcpy(p[2].name, "Programs"); d_strcpy(p[2].path, "/prgm"); p[2].icon_color = C_GREEN;
    d_strcpy(p[3].name, "Devices"); d_strcpy(p[3].path, "/dev"); p[3].icon_color = C_TEXT_SEC;
    g_fm.place_count = 4;
}

/* ========== Format file size ========== */
static void fm_format_size(uint64_t size, char *buf, int max_len) {
    int len = 0;
    if (size >= 1024ULL * 1024 * 1024) {
        int gb = (int)(size / (1024*1024*1024));
        int gb_frac = (int)((size % (1024*1024*1024)) / (1024*1024*102));
        len = 0;
        { char nb[16]; int ni=0,n=gb; if(n==0) nb[ni++]='0'; else while(n>0){nb[ni++]='0'+(n%10);n/=10;} for(int j=0;j<ni;j++) buf[len++]=nb[ni-1-j]; }
        buf[len++]='.'; buf[len++]='0'+(gb_frac%10);
        buf[len++]='G'; buf[len++]='B'; buf[len]=0;
    } else if (size >= 1024 * 1024) {
        int mb = (int)(size / (1024*1024));
        len = 0;
        { char nb[16]; int ni=0,n=mb; if(n==0) nb[ni++]='0'; else while(n>0){nb[ni++]='0'+(n%10);n/=10;} for(int j=0;j<ni;j++) buf[len++]=nb[ni-1-j]; }
        buf[len++]='M'; buf[len++]='B'; buf[len]=0;
    } else if (size >= 1024) {
        int kb = (int)(size / 1024);
        len = 0;
        { char nb[16]; int ni=0,n=kb; if(n==0) nb[ni++]='0'; else while(n>0){nb[ni++]='0'+(n%10);n/=10;} for(int j=0;j<ni;j++) buf[len++]=nb[ni-1-j]; }
        buf[len++]='K'; buf[len++]='B'; buf[len]=0;
    } else {
        len = 0;
        if (size == 0) { buf[len++] = '0'; }
        else { char nb[16]; int ni=0; uint64_t n=size; while(n>0){nb[ni++]='0'+(int)(n%10);n/=10;} for(int j=0;j<ni;j++) buf[len++]=nb[ni-1-j]; }
        buf[len++]='B'; buf[len]=0;
    }
}

/* ========== Drawing ========== */

static void fm_draw_folder_icon(int x, int y, int size) {
    /* KDE Dolphin style folder icon */
    fill_rounded_rect(x+2, y+4, size-4, size-8, 2, C_ORANGE);
    fill_rect(x+4, y, size/2, 4, C_ORANGE);
    /* Tab highlight */
    fill_rect(x+4, y+6, size-10, 2, MKC(255, 200, 140));
}

static void fm_draw_file_icon(int x, int y, int size) {
    /* Document icon */
    fill_rect(x+4, y+2, size-8, size-4, C_SURFACE_VAR);
    fill_rect(x+6, y+6, size-14, 2, C_TEXT_DIS);
    fill_rect(x+6, y+10, size-14, 2, C_TEXT_DIS);
    fill_rect(x+6, y+14, size-18, 2, C_TEXT_DIS);
    /* Corner fold */
    fill_rect(x+size-10, y+2, 6, 6, C_BORDER);
}

static void fm_draw_icon(int x, int y, int size, fm_type_t type) {
    switch (type) {
        case FM_DIR: fm_draw_folder_icon(x, y, size); break;
        case FM_FILE: fm_draw_file_icon(x, y, size); break;
        case FM_SYMLINK:
            fill_rounded_rect(x+4, y+4, size-8, size-8, 4, MKC(128,226,190));
            break;
        default: fill_rect(x+4, y+4, size-8, size-8, C_BORDER); break;
    }
}

/* Render entire FM UI */
static void fm_render(void) {
    int ox = g_fm.win_x, oy = g_fm.win_y;
    int ww = g_fm.win_w, wh = g_fm.win_h;

    /* Background */
    fill_rect(ox, oy, ww, wh, C_MICA_BG);

    /* ===== Toolbar (KDE Dolphin style) ===== */
    int tb_y = oy;
    int tb_h = g_fm.toolbar_h;
    fill_rect(ox, tb_y, ww, tb_h, C_TOOLBAR_BG);

    /* Navigation buttons - KDE Breeze style */
    const char *btn_labels[] = {"<", ">", "^", "R", "V"};
    int btn_count = 5;
    for (int i = 0; i < btn_count; i++) {
        int bx = ox + 6 + i * 36;
        int by = tb_y + 6;
        int bw = 30, bh = 28;
        int hov = (g_fm.hover_btn == i);
        fill_rounded_rect(bx, by, bw, bh, 6, hov ? C_HOVER : C_SURFACE_VAR);
        draw_text(bx + 10, by + 10, btn_labels[i], hov ? C_TEXT : C_TEXT_SEC);
    }

    /* ===== Address bar (Win11 Fluent style) ===== */
    int ab_y = tb_y + tb_h;
    int ab_h = g_fm.addr_h;
    fill_rect(ox, ab_y, ww, ab_h, C_TOOLBAR_BG);

    /* Breadcrumb-style address bar */
    int ab_x = ox + 8;
    int ab_w = ww - 16;
    fill_rounded_rect(ab_x, ab_y + 4, ab_w, ab_h - 8, 6,
                      g_fm.addr_focused ? C_ACCENT : C_ADDR_BG);

    /* Draw path segments as breadcrumbs */
    {
        int bx = ab_x + 10;
        const char *path = g_fm.address_bar;
        if (path[0] == '/') {
            draw_text(bx, ab_y + 10, "/", C_ACCENT);
            bx += 8;
            path++;
        }
        while (*path && bx < ab_x + ab_w - 20) {
            /* Extract segment */
            char seg[64]; int si = 0;
            while (*path && *path != '/' && si < 63) seg[si++] = *path++;
            seg[si] = 0;
            if (*path == '/') path++;

            if (si > 0) {
                draw_text(bx, ab_y + 10, seg, C_TEXT);
                bx += text_width(seg);
                if (*path || (bx + 12 < ab_x + ab_w - 20)) {
                    draw_text(bx, ab_y + 10, " > ", C_TEXT_DIS);
                    bx += 24;
                }
            }
        }
    }

    /* ===== Sidebar (KDE Dolphin style) ===== */
    int sb_x = ox;
    int sb_y = ab_y + ab_h;
    int sb_w = g_fm.sidebar_w;
    int sb_h = wh - tb_h - ab_h - g_fm.status_h;

    fill_rect(sb_x, sb_y, sb_w, sb_h, C_SIDEBAR_BG);
    fill_rect(sb_x + sb_w - 1, sb_y, 1, sb_h, C_BORDER);

    /* Places header */
    draw_text(sb_x + 12, sb_y + 10, "Places", C_TEXT_DIS);

    for (int i = 0; i < g_fm.place_count; i++) {
        int py = sb_y + 30 + i * 28;
        int hov = (g_fm.hover_item == -(i+1));
        if (hov) fill_rounded_rect(sb_x + 4, py - 2, sb_w - 8, 24, 4, C_HOVER);

        /* Icon */
        fill_rounded_rect(sb_x + 12, py + 2, 16, 16, 3, g_fm.places[i].icon_color);
        /* Name */
        draw_text(sb_x + 34, py + 4, g_fm.places[i].name, C_TEXT);
    }

    /* ===== Content area ===== */
    int cx = ox + sb_w;
    int cy = sb_y;
    int cw = ww - sb_w;
    int ch = sb_h;

    fill_rect(cx, cy, cw, ch, C_MICA_BG);

    /* Column headers for details view */
    if (g_fm.files.view_mode == 2) {
        fill_rect(cx, cy, cw, 22, C_TOOLBAR_BG);
        draw_text(cx + 28, cy + 6, "Name", C_TEXT_SEC);
        draw_text(cx + cw - 200, cy + 6, "Size", C_TEXT_SEC);
        draw_text(cx + cw - 120, cy + 6, "Type", C_TEXT_SEC);
        draw_text(cx + cw - 60, cy + 6, "Modified", C_TEXT_SEC);
        cy += 22; ch -= 22;
    }

    /* File items */
    fm_filelist_t *fl = &g_fm.files;

    if (fl->view_mode == 0) {
        /* Icons view - Win11 style grid */
        int icon_w = 80, icon_h = 90;
        int cols = cw / icon_w;
        if (cols < 1) cols = 1;

        for (int i = 0; i < fl->count; i++) {
            fm_item_t *it = &fl->items[i];
            int col = i % cols, row = i / cols;
            int ix = cx + col * icon_w;
            int iy = cy + row * icon_h - fl->scroll_y;
            if (iy + icon_h < cy || iy > cy + ch) continue;

            /* Selection highlight */
            if (it->is_selected)
                fill_rounded_rect(ix+2, iy+2, icon_w-4, icon_h-4, 6, C_SELECTED);

            /* Hover */
            if (it->is_hovered)
                fill_rounded_rect(ix+2, iy+2, icon_w-4, icon_h-4, 6, C_HOVER);

            /* Icon */
            fm_draw_icon(ix+24, iy+6, 32, it->type);

            /* Name (truncated) */
            {
                int maxc = (icon_w - 4) / 8; if (maxc > 14) maxc = 14;
                char tmp[16]; d_strncpy(tmp, it->name, maxc); tmp[maxc] = 0;
                draw_text(ix+4, iy+46, tmp, C_TEXT);
            }
        }
    } else if (fl->view_mode == 1) {
        /* List view */
        int item_h = 24;
        for (int i = 0; i < fl->count; i++) {
            fm_item_t *it = &fl->items[i];
            int iy = cy + i * item_h - fl->scroll_y;
            if (iy + item_h < cy || iy > cy + ch) continue;

            if (it->is_selected)
                fill_rounded_rect(cx+2, iy, cw-4, item_h, 4, C_SELECTED);
            if (it->is_hovered)
                fill_rounded_rect(cx+2, iy, cw-4, item_h, 4, C_HOVER);

            fm_draw_icon(cx+4, iy+4, 16, it->type);
            draw_text(cx+24, iy+6, it->name, C_TEXT);

            if (it->type == FM_FILE) {
                char sz[16]; fm_format_size(it->size, sz, 16);
                draw_text(cx+cw-60, iy+6, sz, C_TEXT_SEC);
            }
        }
    } else {
        /* Details view */
        int item_h = 22;
        for (int i = 0; i < fl->count; i++) {
            fm_item_t *it = &fl->items[i];
            int iy = cy + i * item_h - fl->scroll_y;
            if (iy + item_h < cy || iy > cy + ch) continue;

            if (it->is_selected)
                fill_rounded_rect(cx+2, iy, cw-4, item_h, 4, C_SELECTED);
            if (it->is_hovered)
                fill_rounded_rect(cx+2, iy, cw-4, item_h, 4, C_HOVER);

            fm_draw_icon(cx+4, iy+3, 16, it->type);
            draw_text(cx+28, iy+5, it->name, C_TEXT);

            /* Size */
            if (it->type == FM_FILE) {
                char sz[16]; fm_format_size(it->size, sz, 16);
                draw_text(cx+cw-200, iy+5, sz, C_TEXT_SEC);
            } else {
                draw_text(cx+cw-200, iy+5, "--", C_TEXT_DIS);
            }

            /* Type */
            {
                const char *type_str = "File";
                if (it->type == FM_DIR) type_str = "Directory";
                else if (it->type == FM_SYMLINK) type_str = "Symlink";
                draw_text(cx+cw-120, iy+5, type_str, C_TEXT_SEC);
            }

            /* Modified time */
            if (it->mtime > 0) {
                char time_buf[16];
                int h = (int)((it->mtime / 3600) % 24);
                int m = (int)((it->mtime / 60) % 60);
                time_buf[0]='0'+h/10; time_buf[1]='0'+h%10;
                time_buf[2]=':'; time_buf[3]='0'+m/10; time_buf[4]='0'+m%10;
                time_buf[5]=0;
                draw_text(cx+cw-60, iy+5, time_buf, C_TEXT_DIS);
            }
        }
    }

    /* ===== Status bar ===== */
    int st_y = oy + wh - g_fm.status_h;
    fill_rect(ox, st_y, ww, g_fm.status_h, C_TOOLBAR_BG);
    fill_rect(ox, st_y, ww, 1, C_BORDER);

    /* Item count */
    {
        char status[128];
        int pos = 0;
        const char *prefix = "Items: ";
        while (*prefix) status[pos++] = *prefix++;
        int n = fl->count;
        if (n == 0) { status[pos++] = '0'; }
        else { char nb[16]; int ni=0; int tmp=n; while(tmp>0){nb[ni++]='0'+(tmp%10);tmp/=10;} for(int j=0;j<ni;j++) status[pos++]=nb[ni-1-j]; }
        status[pos] = 0;
        draw_text(ox + 12, st_y + 6, status, C_TEXT_SEC);
    }

    /* Current path */
    draw_text(ox + 120, st_y + 6, fl->current_path, C_TEXT_DIS);
}

/* ========== Input Handling ========== */

static void handle_click(int mx, int my, int button) {
    if (button != 0) return; /* left click only */

    int ox = g_fm.win_x, oy = g_fm.win_y;
    int ww = g_fm.win_w, wh = g_fm.win_h;

    /* Check toolbar buttons */
    int tb_y = oy;
    for (int i = 0; i < 5; i++) {
        int bx = ox + 6 + i * 36;
        int by = tb_y + 6;
        if (mx >= bx && mx < bx + 30 && my >= by && my < by + 28) {
            switch (i) {
                case 0: fm_go_back(); break;
                case 1: fm_go_forward(); break;
                case 2: fm_go_up(); break;
                case 3: fm_load_directory(g_fm.files.current_path); break;
                case 4: g_fm.files.view_mode = (g_fm.files.view_mode + 1) % 3; break;
            }
            return;
        }
    }

    /* Check sidebar places */
    int sb_y = oy + g_fm.toolbar_h + g_fm.addr_h;
    for (int i = 0; i < g_fm.place_count; i++) {
        int py = sb_y + 30 + i * 28;
        if (mx >= ox && mx < ox + g_fm.sidebar_w && my >= py - 2 && my < py + 22) {
            fm_navigate(g_fm.places[i].path);
            return;
        }
    }

    /* Check file items in content area */
    int cx = ox + g_fm.sidebar_w;
    int cy = sb_y;
    int cw = ww - g_fm.sidebar_w;
    int ch = wh - g_fm.toolbar_h - g_fm.addr_h - g_fm.status_h;

    fm_filelist_t *fl = &g_fm.files;

    if (fl->view_mode == 0) {
        /* Icons view */
        int icon_w = 80, icon_h = 90;
        int cols = cw / icon_w; if (cols < 1) cols = 1;
        for (int i = 0; i < fl->count; i++) {
            int col = i % cols, row = i / cols;
            int ix = cx + col * icon_w;
            int iy = cy + row * icon_h - fl->scroll_y;
            if (mx >= ix && mx < ix + icon_w && my >= iy && my < iy + icon_h) {
                fm_item_t *it = &fl->items[i];
                /* Deselect all */
                for (int j = 0; j < fl->count; j++) fl->items[j].is_selected = 0;
                it->is_selected = 1;

                /* Double-click: open */
                if (it->type == FM_DIR) {
                    fm_navigate(it->path);
                }
                return;
            }
        }
    } else {
        /* List/Details view */
        int item_h = (fl->view_mode == 1) ? 24 : 22;
        for (int i = 0; i < fl->count; i++) {
            int iy = cy + i * item_h - fl->scroll_y;
            if (mx >= cx && mx < cx + cw && my >= iy && my < iy + item_h) {
                fm_item_t *it = &fl->items[i];
                for (int j = 0; j < fl->count; j++) fl->items[j].is_selected = 0;
                it->is_selected = 1;
                if (it->type == FM_DIR) {
                    fm_navigate(it->path);
                }
                return;
            }
        }
    }
}

/* ========== Main Entry ========== */

void _start(void) {
    serial_puts("[FM] File Manager starting...\n");

    /* Get framebuffer info */
    g_fbi.base = fb_base; g_fbi.w = fb_width; g_fbi.h = fb_height; g_fbi.pitch = fb_pitch;
    if (g_fbi.base == 0 || g_fbi.w == 0) sc2(SYS_GETFBINFO, (long)&g_fbi, 0);

    g_w = (int)g_fbi.w; g_h = (int)g_fbi.h;
    g_stride = (int)g_fbi.pitch / 4;
    g_fb = (uint32_t*)(uint64_t)g_fbi.base;
    if (g_fbi.format == 1 || g_fbi.format == 0x34324258) g_fb_bgr = 1;

    /* Initialize FM state */
    g_fm.sidebar_w = 180;
    g_fm.toolbar_h = 40;
    g_fm.addr_h = 36;
    g_fm.status_h = 28;
    g_fm.win_x = 100; g_fm.win_y = 60;
    g_fm.win_w = 700; g_fm.win_h = 480;
    g_fm.hover_item = -1;
    g_fm.hover_btn = -1;
    g_fm.files.view_mode = 1; /* Start with list view */

    fm_init_places();
    fm_navigate("/");

    /* Main loop */
    uint32_t keycodes[16];

    while (1) {
        /* Poll input */
        int nkeys = (int)sys_input_poll(keycodes, 16);
        for (int i = 0; i < nkeys; i++) {
            uint32_t kc = keycodes[i];
            int pressed = !(kc & 0x80000000);
            kc &= 0x7FFFFFFF;
            (void)pressed; (void)kc;
        }

        /* Render */
        fm_render();
        sys_fb_sync(g_fb);

        /* Yield */
        for (volatile int delay = 0; delay < 5000; delay++);
    }
}
