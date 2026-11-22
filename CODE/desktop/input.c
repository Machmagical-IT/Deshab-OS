/* input.c — PS/2 鼠标键盘、光标管理、RTC/TSC 时钟（M0 自 main.c 平移）
 */
#include "desktop.h"

/* ---- 全局状态 ---- */
int g_mouse_x = 400, g_mouse_y = 300;
int g_mouse_btn = 0;      /* bit0=左 bit1=右 */
int g_left_pressed = 0, g_left_released = 0, g_right_pressed = 0;
static int g_mouse_has_pkt = 0;

/* 光标背景保存 */
static u32 g_cursor_save[CURSOR_SIZE * CURSOR_SIZE];
int g_cursor_saved = 0;
int g_cursor_old_x = -1, g_cursor_old_y = -1;

/* 键盘状态 */
int g_shift = 0, g_ctrl = 0, g_alt = 0;
int g_e0 = 0;

/* 时钟 */
int g_rtc_boot_sec = 0;         /* 开机时刻（秒级当天秒） */
int g_last_sec = -1;
u64 g_tsc_per_sec = 0;

/* ============================================================
 *  箭头光标形状（24×24，1=前景 0=透明）
 * ============================================================ */

static const u8 cursor_shape[24][24] = {
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

/* ============================================================
 *  RTC 时钟
 * ============================================================ */

static int bcd2bin(u8 v) { return (v >> 4) * 10 + (v & 0xF); }

void rtc_read(int *yy, int *mo, int *dd, int *hh, int *mi, int *ss) {
    outb(0x70, 0x00); *ss = bcd2bin(inb(0x71));
    outb(0x70, 0x02); *mi = bcd2bin(inb(0x71));
    outb(0x70, 0x04); *hh = bcd2bin(inb(0x71));
    outb(0x70, 0x07); *dd = bcd2bin(inb(0x71));
    outb(0x70, 0x08); *mo = bcd2bin(inb(0x71));
    outb(0x70, 0x09); *yy = bcd2bin(inb(0x71));
}

int rtc_day_sec(void) {
    int y, mo, d, h, mi, s;
    rtc_read(&y, &mo, &d, &h, &mi, &s);
    return h * 3600 + mi * 60 + s;
}

/* 格式化 HH:MM:SS */
void fmt_hms(char *out, int sec) {
    int h = sec / 3600, m = (sec / 60) % 60, s = sec % 60;
    out[0] = '0' + h / 10; out[1] = '0' + h % 10; out[2] = ':';
    out[3] = '0' + m / 10; out[4] = '0' + m % 10; out[5] = ':';
    out[6] = '0' + s / 10; out[7] = '0' + s % 10; out[8] = 0;
}

/* 校准 TSC（约 1 秒）：用于双击间隔判定 */
void tsc_calibrate(void) {
    int s0 = rtc_day_sec();
    while (rtc_day_sec() == s0) __asm__("pause");
    u64 t0 = rdtsc();
    int s1 = rtc_day_sec();
    while (rtc_day_sec() == s1) __asm__("pause");
    g_tsc_per_sec = rdtsc() - t0;
}

/* ============================================================
 *  光标管理
 * ============================================================ */

void cursor_save_bg(int mx, int my) {
    for (int r = 0; r < CURSOR_SIZE; r++) {
        for (int c = 0; c < CURSOR_SIZE; c++) {
            int x = mx + c, y = my + r;
            if (x >= 0 && (u64)x < g_fb_w && y >= 0 && (u64)y < g_fb_h) {
                u32 *line = (u32 *)((u8 *)g_fb.fb + (u64)y * g_fb.pitch);
                g_cursor_save[r * CURSOR_SIZE + c] = line[x];
            } else {
                g_cursor_save[r * CURSOR_SIZE + c] = 0;
            }
        }
    }
    g_cursor_saved = 1;
    g_cursor_old_x = mx;
    g_cursor_old_y = my;
}

void cursor_restore_bg(void) {
    if (!g_cursor_saved) return;
    for (int r = 0; r < CURSOR_SIZE; r++) {
        for (int c = 0; c < CURSOR_SIZE; c++) {
            int x = g_cursor_old_x + c, y = g_cursor_old_y + r;
            if (x >= 0 && (u64)x < g_fb_w && y >= 0 && (u64)y < g_fb_h) {
                u32 *line = (u32 *)((u8 *)g_fb.fb + (u64)y * g_fb.pitch);
                line[x] = g_cursor_save[r * CURSOR_SIZE + c];
            }
        }
    }
    g_cursor_saved = 0;
}

void cursor_draw(int mx, int my) {
    /* 白色指针 + 黑色描边：描边 = 空心像素且 8 邻域有实心（不出保存区） */
    for (int r = 0; r < CURSOR_SIZE; r++) {
        for (int c = 0; c < CURSOR_SIZE; c++) {
            if (cursor_shape[r][c]) continue;
            int near = 0;
            for (int dy = -1; dy <= 1 && !near; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int rr = r + dy, cc = c + dx;
                    if (rr < 0 || rr >= CURSOR_SIZE || cc < 0 || cc >= CURSOR_SIZE) continue;
                    if (cursor_shape[rr][cc]) { near = 1; break; }
                }
            }
            if (near) du_pixel(&g_fb, mx + c, my + r, 0xFF000000u);
        }
    }
    for (int r = 0; r < CURSOR_SIZE; r++) {
        for (int c = 0; c < CURSOR_SIZE; c++) {
            if (cursor_shape[r][c]) {
                du_pixel(&g_fb, mx + c, my + r, 0xFFFFFFFFu);   /* 白色指针 */
            }
        }
    }
}

/* ============================================================
 *  输入总线（P6b：srv.input.kbd.poll / srv.input.mouse.poll）
 *  PS/2 初始化与 IRQ handler 归 ps2kbd.drv / ps2mouse.drv；
 *  本模块只消费事件（直读 0x60 会与 IRQ handler 抢数据）。
 * ============================================================ */
#include "../DKM/dkm_shared.h"
#include "../DKM/dkm_input.h"

static dkm_input g_input;
static int g_mouse_seen_pkt = 0;

/* 0 = kbd 就绪；-1 = srv.input.kbd.poll 缺席（调用方 INPUT-E01）。 */
int dt_input_init(void) {
    return dkm_input_open(&g_input, (const struct dkm_kernel_api *)g_kernel_api);
}

static u8 g_inj[32];
static int g_inj_r, g_inj_w, g_inj_n;

void dt_kbd_inject(u8 sc) {
    if (g_inj_n >= 32) return;
    g_inj[g_inj_w] = sc;
    g_inj_w = (g_inj_w + 1) & 31;
    g_inj_n++;
}

int dt_kbd_poll(u8 *sc) {
    if (g_inj_n) {
        *sc = g_inj[g_inj_r];
        g_inj_r = (g_inj_r + 1) & 31;
        g_inj_n--;
        return 1;
    }
    return dkm_input_kbd(&g_input, sc);
}

/* 兼容名：PS/2 已由 ps2mouse.drv 初始化，此处不再碰端口。 */
void ps2_mouse_init(void) {
    if (!g_input.mouse_poll)
        slog("mouse: srv.input.mouse.poll missing (INPUT-E02, mouse disabled)");
}

/* 返回 1 = 有新鼠标数据包（相对位移 + 按钮位已更新）。 */
int ps2_mouse_poll(void) {
    int dx = 0, dy = 0;
    u32 btns = 0;
    if (!dkm_input_mouse_step(&g_input, &dx, &dy, &btns)) return 0;

    /* 上电后首个有效包留档：串口无此行 = 鼠标数据根本没进来 */
    if (!g_mouse_seen_pkt) {
        g_mouse_seen_pkt = 1;
        slog("mouse first pkt via srv.input.mouse.poll");
    }

    if (g_input.mouse_abs) {
        g_mouse_x = g_input.mouse_x;
        g_mouse_y = g_input.mouse_y;
    } else {
        g_mouse_x += dx;
        g_mouse_y += dy;
    }
    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_y < 0) g_mouse_y = 0;
    if (g_mouse_x >= (int)g_fb_w - CURSOR_SIZE) g_mouse_x = (int)g_fb_w - CURSOR_SIZE;
    if (g_mouse_y >= (int)g_fb_h - CURSOR_SIZE) g_mouse_y = (int)g_fb_h - CURSOR_SIZE;

    int old_btn = g_mouse_btn;
    g_mouse_btn = (int)(btns & 0x03u);
    if ((g_mouse_btn & 1) && !(old_btn & 1)) g_left_pressed = 1;
    if (!(g_mouse_btn & 1) && (old_btn & 1)) g_left_released = 1;
    if ((g_mouse_btn & 2) && !(old_btn & 2)) g_right_pressed = 1;
    if (dx == 0 && dy == 0 && g_mouse_btn == old_btn) return 0;

    /* VSCode Phase 2: IDE attached 时把相对位移 + 按钮状态镜像到 Linux guest。
     * dy 为屏幕坐标系（正=下），与 Linux REL_Y 约定一致。 */
    if (g_input_forward_enabled && g_lxc_svc && g_lxc_svc->input_forward_mouse) {
        g_lxc_svc->input_forward_mouse(dx, dy, (u8)g_mouse_btn);
    }

    g_mouse_has_pkt = 1;
    return 1;
}

/* ELF 占用 CPU 时主循环不跑。光标直接画在真实 fb 上；
 * 客户区里的像素由应用每帧重画，这里恢复时跳过，避免盖住新内容。 */
static int g_hc_on;
static int g_hc_x, g_hc_y, g_hc_w, g_hc_h;
static u32 g_hc_save[CURSOR_SIZE * CURSOR_SIZE];
static int g_hc_saved, g_hc_ox, g_hc_oy;

static int host_client_pixel(int x, int y) {
    return g_hc_on && x >= g_hc_x && y >= g_hc_y &&
           x < g_hc_x + g_hc_w && y < g_hc_y + g_hc_h;
}

void desktop_host_begin(int x, int y, int w, int h) {
    g_hc_x = x; g_hc_y = y; g_hc_w = w; g_hc_h = h;
    g_hc_on = 1;
    g_hc_saved = 0;
}

void desktop_host_end(void) {
    desktop_host_pump();
    if (g_hc_saved) {
        for (int r = 0; r < CURSOR_SIZE; r++) {
            for (int c = 0; c < CURSOR_SIZE; c++) {
                int x = g_hc_ox + c, y = g_hc_oy + r;
                if (x < 0 || y < 0 || (u64)x >= g_fb_w || (u64)y >= g_fb_h) continue;
                ((u32 *)(g_real_fb + (u64)y * g_fb_pitch))[x] = g_hc_save[r * CURSOR_SIZE + c];
            }
        }
    }
    g_hc_saved = 0;
    g_hc_on = 0;
}

void desktop_host_pump(void) {
    du_u32 *old;
    if (!g_hc_on || !g_real_fb) return;
    if (g_hc_saved) {
        for (int r = 0; r < CURSOR_SIZE; r++) {
            for (int c = 0; c < CURSOR_SIZE; c++) {
                int x = g_hc_ox + c, y = g_hc_oy + r;
                if (x < 0 || y < 0 || (u64)x >= g_fb_w || (u64)y >= g_fb_h) continue;
                if (host_client_pixel(x, y)) continue;
                ((u32 *)(g_real_fb + (u64)y * g_fb_pitch))[x] = g_hc_save[r * CURSOR_SIZE + c];
            }
        }
        g_hc_saved = 0;
    }
    ps2_mouse_poll();
    for (int r = 0; r < CURSOR_SIZE; r++) {
        for (int c = 0; c < CURSOR_SIZE; c++) {
            int x = g_mouse_x + c, y = g_mouse_y + r;
            if (x < 0 || y < 0 || (u64)x >= g_fb_w || (u64)y >= g_fb_h)
                g_hc_save[r * CURSOR_SIZE + c] = 0;
            else
                g_hc_save[r * CURSOR_SIZE + c] = ((u32 *)(g_real_fb + (u64)y * g_fb_pitch))[x];
        }
    }
    g_hc_ox = g_mouse_x;
    g_hc_oy = g_mouse_y;
    g_hc_saved = 1;
    old = g_fb.fb;
    g_fb.fb = (du_u32 *)g_real_fb;
    cursor_draw(g_mouse_x, g_mouse_y);
    g_fb.fb = old;
}

/* ============================================================
 *  键盘扫描码 → ASCII
 * ============================================================ */

char scan_to_ascii(u8 sc, int shift) {
    static const char normal[58] = {
        0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, '\t',
        'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
        'a','s','d','f','g','h','j','k','l',';','\'', '`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };
    static const char shifted[58] = {
        0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, '\t',
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
        'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
        'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
    };
    if (sc >= 58) return 0;
    return shift ? shifted[sc] : normal[sc];
}

/* VSCode Phase 2: PS/2 set-1 扫描码 → Linux keycode（linux/input-event-codes.h）。
 *   base : 已去除 0x80 release 位的基础扫描码（0x01..0x39 主键区 + 少数扩展）
 *   e0   : 上一字节为 0xE0 前缀时置 1，选取扩展键变体
 * 返回 0 表示该扫描码无映射（调用方应丢弃）。
 *
 * 巧合：主键区 0x01..0x39 的 Linux keycode 与扫描码数值完全一致
 * （IBM XT 布局历史遗留），故直接透传；仅 E0 扩展键需单独查表。 */
u16 scancode_to_linux_keycode(u8 base, int e0) {
    if (e0) {
        switch (base) {
        case 0x1D: return 97;   /* KEY_RIGHTCTRL */
        case 0x38: return 100; /* KEY_RIGHTALT */
        case 0x48: return 103; /* KEY_UP */
        case 0x4B: return 105; /* KEY_LEFT */
        case 0x4D: return 106; /* KEY_RIGHT */
        case 0x50: return 108; /* KEY_DOWN */
        case 0x52: return 110; /* KEY_INSERT */
        case 0x53: return 111; /* KEY_DELETE */
        case 0x47: return 102; /* KEY_HOME */
        case 0x4F: return 107; /* KEY_END */
        case 0x49: return 104; /* KEY_PAGEUP */
        case 0x51: return 109; /* KEY_PAGEDOWN */
        default:   return 0;
        }
    }
    /* 主键区：0x01..0x39 透传，超出范围无映射 */
    if (base >= 0x01 && base <= 0x39) return (u16)base;
    return 0;
}
