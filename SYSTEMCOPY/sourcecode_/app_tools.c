/* ===========================================================================
 *  Nutro+OS - tools: Calculator, System Monitor and the PC speaker player
 * ===========================================================================
 */
#include "kernel.h"

extern const boot_info_t *boot_info_get(void);
extern fat32_t *fs_get(void);

/* =========================================================== Calculator == */
#define CALC_COLS 4
#define CALC_ROWS 5
#define CALC_BW 66
#define CALC_BH 42
#define CALC_GAP 6

static const char *calc_labels[CALC_ROWS][CALC_COLS] = {
    { "C", "+-", "%", "/" },
    { "7", "8", "9", "*" },
    { "4", "5", "6", "-" },
    { "1", "2", "3", "+" },
    { "0", ".", "=", "=" },
};

#define FIX 100                     /* fixed point scale for the calculator */

typedef struct {
    window_t *win;
    char entry[20];
    int  entry_len;
    int  has_dot;
    s32  acc;               /* fixed point, x FIX */
    char op;
    int  fresh;             /* next digit replaces the entry */
} calc_t;

static calc_t g_calc;

/* 32 bit fixed point helpers (no 64 bit division needed) */
static s32 fix_mul(s32 a, s32 b) { return (a / FIX) * b + (a % FIX) * b / FIX; }
static s32 fix_div(s32 a, s32 b)
{
    if (!b) return 0;
    if (a > 20000000) a = 20000000;
    if (a < -20000000) a = -20000000;
    return (a * FIX) / b;
}

static s32 calc_value(calc_t *c)
{
    s32 v = 0;
    int frac = 0;
    for (int i = 0; i < c->entry_len; i++) {
        char ch = c->entry[i];
        if (ch == '.') { frac = 1; continue; }
        v = v * 10 + (ch - '0');
        if (v > 20000000) v = 20000000;
        if (frac) frac++;
    }
    if (frac == 0) return v * FIX;
    int digits = frac - 1;                /* fraction digits typed */
    if (digits <= 2) for (int i = 0; i < 2 - digits; i++) v *= 10;
    else             for (int i = 0; i < digits - 2; i++) v /= 10;
    return v;
}

static void calc_format(s32 v, char *out)
{
    int neg = v < 0;
    if (neg) v = -v;
    s32 ip = v / FIX;
    s32 fp = v % FIX;
    char tmp[32];
    if (ip > 999999999) ip = 999999999;
    itoa((s32)ip, tmp);
    int n = 0;
    if (neg) out[n++] = '-';
    for (int i = 0; tmp[i]; i++) out[n++] = tmp[i];
    if (fp) {
        out[n++] = '.';
        char f[3];
        f[0] = (char)('0' + (int)((fp / 10) % 10));
        f[1] = (char)('0' + (int)(fp % 10));
        int last = 1;
        while (last >= 0 && f[last] == '0') last--;
        for (int i = 0; i <= last; i++) out[n++] = f[i];
    }
    out[n] = 0;
}

static void calc_set_entry(calc_t *c, s32 v)
{
    calc_format(v, c->entry);
    c->entry_len = (int)strlen(c->entry);
    c->has_dot = 0;
    for (int i = 0; i < c->entry_len; i++) if (c->entry[i] == '.') c->has_dot = 1;
}

static void calc_apply(calc_t *c, char op)
{
    s32 v = calc_value(c);
    if (c->op == 0) c->acc = v;
    else {
        switch (c->op) {
        case '+': c->acc += v; break;
        case '-': c->acc -= v; break;
        case '*': c->acc = fix_mul(c->acc, v); break;
        case '/':
            if (v) c->acc = fix_div(c->acc, v);
            else { calc_set_entry(c, 0); return; }
            break;
        }
    }
    c->op = op;
    calc_set_entry(c, c->acc);
    c->fresh = 1;
}

static void calc_key(calc_t *c, char k)
{
    if (k >= '0' && k <= '9') {
        if (c->fresh) { c->entry_len = 0; c->entry[0] = 0; c->has_dot = 0; c->fresh = 0; }
        if (c->entry_len < 12) {
            c->entry[c->entry_len++] = k;
            c->entry[c->entry_len] = 0;
        }
        return;
    }
    switch (k) {
    case '.':
        if (c->fresh) { c->entry_len = 0; c->entry[0] = 0; c->fresh = 0; }
        if (!c->has_dot && c->entry_len < 12) {
            c->entry[c->entry_len++] = '.';
            c->entry[c->entry_len] = 0;
            c->has_dot = 1;
        }
        break;
    case '+': case '-': case '*': case '/': calc_apply(c, k); break;
    case '=':
        calc_apply(c, 0);
        c->op = 0;
        c->fresh = 1;
        break;
    case 'c':
        c->entry_len = 0;
        c->entry[0] = 0;
        c->has_dot = 0;
        c->acc = 0;
        c->op = 0;
        c->fresh = 0;
        break;
    }
}

static void calc_draw(window_t *w)
{
    calc_t *c = (calc_t *)w->user;
    int cx = win_content_x(w), cy = win_content_y(w);
    fill_rect(cx, cy, w->w, w->h - TITLEBAR_H, COL_WIN_CONTENT);

    /* display */
    char shown[40];
    if (c->entry_len) ksnprintf(shown, sizeof(shown), "%s", c->entry);
    else ksnprintf(shown, sizeof(shown), "%d", 0);
    fill_round_rect(cx + 12, cy + 12, w->w - 24, 52, 8, 0x0C1622);
    int tw = (int)strlen(shown) * 8;
    draw_text(cx + w->w - 24 - tw, cy + 28, shown, 0xE8F4FF);

    char opbuf[4] = { c->op ? c->op : ' ', 0 };
    draw_text(cx + 22, cy + 28, opbuf, COL_ACCENT);

    for (int r = 0; r < CALC_ROWS; r++) {
        for (int col = 0; col < CALC_COLS; col++) {
            const char *lab = calc_labels[r][col];
            if (col == 3 && r == 4) continue;              /* the "=" cell spans two */
            int bw = CALC_BW;
            if (r == 4 && col == 2) bw = CALC_BW * 2 + CALC_GAP;
            int bx = cx + 12 + col * (CALC_BW + CALC_GAP);
            int by = cy + 76 + r * (CALC_BH + CALC_GAP);

            int style = UI_BORDERED;
            if (lab[0] == '=') style = UI_FILLED;
            else if (lab[0] == 'C') style = UI_DESTRUCT;
            else if (lab[0] == '/' || lab[0] == '*' || lab[0] == '-' || lab[0] == '+' || lab[0] == '%') style = UI_TINTED;
            ui_button(bx, by, bw, CALC_BH, lab, style);
        }
    }
}

static int calc_event(window_t *w, int type, int a, int b, int c)
{
    UNUSED(c);
    if (!w->user) return 0;

    if (type == EV_KEY) {
        char k = (char)a;
        if (k == '\n') k = '=';
        else if (k == '\b') { if (g_calc.entry_len > 0) g_calc.entry[--g_calc.entry_len] = 0; gui_invalidate(); return 1; }
        else if ((u8)k == KEY_ESC) k = 'c';
        if ((k >= '0' && k <= '9') || k == '.' || k == '+' || k == '-' ||
            k == '*' || k == '/' || k == '=' || k == 'c') {
            calc_key(&g_calc, k);
            gui_invalidate();
        }
        return 1;
    }
    if (type != EV_MOUSE_DOWN) return 0;

    int cx = win_content_x(w), cy = win_content_y(w);
    for (int r = 0; r < CALC_ROWS; r++) {
        for (int col = 0; col < CALC_COLS; col++) {
            int bw = (r == 4 && col == 2) ? CALC_BW * 2 + CALC_GAP : CALC_BW;
            if (col == 3 && r == 4) continue;
            int bx = cx + 12 + col * (CALC_BW + CALC_GAP);
            int by = cy + 76 + r * (CALC_BH + CALC_GAP);
            if (a >= bx && a < bx + bw && b >= by && b < by + CALC_BH) {
                calc_key(&g_calc, calc_labels[r][col][0]);
                gui_invalidate();
                return 1;
            }
        }
    }
    return 1;
}

window_t *app_calc_open(void)
{
    calc_t *c = &g_calc;
    if (c->win && c->win->visible) { win_focus(c->win); return c->win; }

    memset(c, 0, sizeof(*c));
    window_t *w = win_create("Calculator", 700, 120,
                             CALC_COLS * (CALC_BW + CALC_GAP) + 22, CALC_ROWS * (CALC_BH + CALC_GAP) + 92,
                             calc_draw, calc_event, c);
    c->win = w;
    return w;
}

/* ========================================================= System monitor == */
#define SM_SAMPLES 96

typedef struct {
    window_t *win;
    u8 heap_hist[SM_SAMPLES];
    u8 tick_hist[SM_SAMPLES];
    int head, paused, span;
    u32 last_sample;
    u32 last_ticks;
} sysmon_t;

static sysmon_t g_sysmon;

static void sm_sample(sysmon_t *s)
{
    u32 now = timer_ms();
    if (s->paused || now - s->last_sample < 100) return;
    u32 dt = now - s->last_sample;
    s->last_sample = now;

    u32 ticks = timer_ticks();
    u32 dtick = ticks - s->last_ticks;
    s->last_ticks = ticks;

    u32 used = heap_used(), size = heap_size();
    u8 hp = size ? (u8)((used * 100) / size) : 0;
    u32 expected = dt / 10;                      /* 100 Hz PIT */
    u8 tp = expected ? (u8)((dtick * 100) / (expected * 2)) : 0;
    if (tp > 100) tp = 100;

    s->heap_hist[s->head] = hp;
    s->tick_hist[s->head] = tp;
    s->head = (s->head + 1) % SM_SAMPLES;
}

static void sm_bars(window_t *w, int x, int y, const char *label, const char *value, u32 col, int pct)
{
    draw_text(x, y, label, COL_TEXT_DIM);
    int vlen = (int)strlen(value);
    draw_text(x + w->w - 40 - vlen * 8, y, value, COL_TEXT);
    ui_progress(x, y + 16, w->w - 40, 10, pct, col);
}

static void sm_graph2(window_t *w, int x, int y, int gw, int gh, const u8 *hist, int head, u32 col, int span)
{
    UNUSED(w);
    fill_round_rect(x, y, gw, gh, 6, 0xF2F4F8);
    if (span < 4) span = 4;
    if (span > SM_SAMPLES) span = SM_SAMPLES;
    int step = gw / span;
    if (step < 1) step = 1;
    for (int i = 0; i < span; i++) {
        int idx = (head - span + i + SM_SAMPLES * 2) % SM_SAMPLES;
        int v = hist[idx];
        int h = v * (gh - 4) / 100;
        if (h < 1) h = 1;
        fill_rect(x + 2 + i * step, y + gh - 2 - h, step > 1 ? step - 1 : 1, h, col);
    }
}

static void sysmon_draw(window_t *w)
{
    sysmon_t *s = (sysmon_t *)w->user;
    sm_sample(s);

    int cx = win_content_x(w), cy = win_content_y(w);
    int wd = w->w, ht = w->h - TITLEBAR_H;
    fill_rect(cx, cy, wd, ht, COL_WIN_CONTENT);

    const boot_info_t *bi = boot_info_get();
    u32 used = heap_used(), size = heap_size();
    char buf[64];

    draw_text_bold(cx + 16, cy + 10, "SYSTEM MONITOR", COL_ACCENT);
    ui_toggle(cx + wd - 176, cy + 4, s->paused);
    draw_text(cx + wd - 126, cy + 10, "pause", COL_TEXT_DIM);
    ui_slider(cx + wd - 70, cy + 0, 54, s->span, 20, SM_SAMPLES);
    ksnprintf(buf, sizeof(buf), "uptime %u.%u s", timer_ms() / 1000, (timer_ms() % 1000) / 100);
    draw_text(cx + wd - 24 - (int)strlen(buf) * 8, cy + 10, buf, COL_TEXT_DIM);

    ksnprintf(buf, sizeof(buf), "%u / %u KiB", used / 1024, size / 1024);
    sm_bars(w, cx + 16, cy + 34, "kernel heap", buf, COL_ACCENT, size ? (int)(used * 100 / size) : 0);

    ksnprintf(buf, sizeof(buf), "%u KiB", mem_total_kb(bi));
    sm_bars(w, cx + 16, cy + 68, "usable RAM", buf, COL_ACCENT_GREEN, 100);

    ksnprintf(buf, sizeof(buf), "%ux%u %ubpp", fb_width, fb_height, fb_bpp);
    sm_bars(w, cx + 16, cy + 102, "framebuffer", buf, COL_ACCENT_ORANGE, 100);

    int wins = gui_window_count();
    ksnprintf(buf, sizeof(buf), "%d open", wins);
    sm_bars(w, cx + 16, cy + 136, "windows", buf, COL_ACCENT_PINK, wins * 100 / GUI_MAX_WINDOWS);

    draw_text(cx + 16, cy + 172, "heap usage history", COL_TEXT_DIM);
    sm_graph2(w, cx + 16, cy + 190, wd - 32, 54, s->heap_hist, s->head, COL_ACCENT, s->span);

    draw_text(cx + 16, cy + 254, "timer/CPU activity", COL_TEXT_DIM);
    sm_graph2(w, cx + 16, cy + 272, wd - 32, 54, s->tick_hist, s->head, COL_ACCENT_GREEN, s->span);

    int mx, my;
    mouse_get_pos(&mx, &my);
    ksnprintf(buf, sizeof(buf), "mouse %d,%d   ticks %u   cursor age %u", mx, my, timer_ticks(), timer_ms());
    draw_text(cx + 16, cy + ht - 22, buf, COL_TEXT_DIM);
}

static int sysmon_event(window_t *w, int type, int a, int b, int c)
{
    sysmon_t *s = (sysmon_t *)w->user;
    int cx = win_content_x(w), cy = win_content_y(w);
    UNUSED(c);
    if (type == EV_MOUSE_DOWN) {
        if (a >= cx + w->w - 180 && a < cx + w->w - 130 && b >= cy && b < cy + 30) {
            s->paused = !s->paused;
            gui_invalidate();
        }
        return 1;
    }
    if (type == EV_MOUSE_MOVE && (gui_mouse_buttons() & 1)) {
        int sx = cx + w->w - 70;
        if (a >= sx - 10 && a <= sx + 64 && b >= cy - 14 && b <= cy + 34) {
            int v = ui_slider_value(sx, 54, 20, SM_SAMPLES, a);
            if (v != s->span) { s->span = v; gui_invalidate(); }
        }
        return 1;
    }
    return 0;
}

window_t *app_sysmon_open(void)
{
    sysmon_t *s = &g_sysmon;
    if (s->win && s->win->visible) { win_focus(s->win); return s->win; }

    memset(s, 0, sizeof(*s));
    s->span = SM_SAMPLES;
    s->last_sample = timer_ms();
    s->last_ticks = timer_ticks();

    window_t *w = win_create("System Monitor", 300, 60, 520, 380, sysmon_draw, sysmon_event, s);
    if (w) w->anim = 1;                 /* statistics only need 1 Hz */
    s->win = w;
    return w;
}

/* ============================================================== Music ===== */
/* PC speaker: PIT channel 2 + the port 0x61 gate */
static void spk_freq(u32 hz)
{
    if (!hz) return;
    u32 div = 1193182u / hz;
    outb(0x43, 0xB6);
    outb(0x42, (u8)(div & 0xFF));
    outb(0x42, (u8)((div >> 8) & 0xFF));
}

static void spk_enable(int on)
{
    u8 v = inb(0x61);
    if (on) outb(0x61, (u8)(v | 3));
    else outb(0x61, (u8)(v & ~3));
}

#define NOTE_C4 262
#define NOTE_D4 294
#define NOTE_E4 330
#define NOTE_F4 349
#define NOTE_G4 392
#define NOTE_A4 440
#define NOTE_AS4 466
#define NOTE_B4 494
#define NOTE_C5 523
#define NOTE_D5 587
#define NOTE_E5 659
#define NOTE_F5 698
#define NOTE_G5 784
#define NOTE_A5 880

typedef struct { u16 freq; u16 ms; } note_t;
typedef struct {
    const char *name;
    const char *desc;
    const note_t *notes;
    int count;
} song_t;

static const note_t song_scale[] = {
    { NOTE_C4,220 },{ NOTE_D4,220 },{ NOTE_E4,220 },{ NOTE_F4,220 },{ NOTE_G4,220 },
    { NOTE_A4,220 },{ NOTE_B4,220 },{ NOTE_C5,440 },{ 0,150 },
};
static const note_t song_joy[] = {
    { NOTE_E4,300 },{ NOTE_E4,300 },{ NOTE_F4,300 },{ NOTE_G4,300 },
    { NOTE_G4,300 },{ NOTE_F4,300 },{ NOTE_E4,300 },{ NOTE_D4,300 },
    { NOTE_C4,300 },{ NOTE_C4,300 },{ NOTE_D4,300 },{ NOTE_E4,300 },
    { NOTE_E4,450 },{ NOTE_D4,150 },{ NOTE_D4,600 },{ 0,250 },
};
static const note_t song_bday[] = {
    { NOTE_G4,200 },{ NOTE_G4,200 },{ NOTE_A4,400 },{ NOTE_G4,400 },{ NOTE_C5,400 },{ NOTE_B4,800 },
    { NOTE_G4,200 },{ NOTE_G4,200 },{ NOTE_A4,400 },{ NOTE_G4,400 },{ NOTE_D5,400 },{ NOTE_C5,800 },
    { 0,200 },
};
static const song_t songs[] = {
    { "Do-Re-Mi",      "C major scale",      song_scale, (int)(sizeof(song_scale) / sizeof(note_t)) },
    { "Ode to Joy",    "Beethoven",          song_joy,   (int)(sizeof(song_joy) / sizeof(note_t)) },
    { "Happy Birthday", "traditional",       song_bday,  (int)(sizeof(song_bday) / sizeof(note_t)) },
};
#define SONG_COUNT 3

typedef struct {
    window_t *win;
    int song;
    int playing;
    int note;
    u32 note_start;
    int level;               /* visualiser history */
    u8 bars[32];
    int loop, tempo;
} music_t;

static music_t g_music;

static const char *note_name(u16 f)
{
    switch (f) {
    case NOTE_C4: return "C4"; case NOTE_D4: return "D4"; case NOTE_E4: return "E4";
    case NOTE_F4: return "F4"; case NOTE_G4: return "G4"; case NOTE_A4: return "A4";
    case NOTE_AS4: return "A#4"; case NOTE_B4: return "B4"; case NOTE_C5: return "C5";
    case NOTE_D5: return "D5"; case NOTE_E5: return "E5"; case NOTE_F5: return "F5";
    case NOTE_G5: return "G5"; case NOTE_A5: return "A5";
    default: return "rest";
    }
}

static void music_stop(music_t *m)
{
    spk_enable(0);
    m->playing = 0;
    m->note = 0;
}

static void music_advance(music_t *m)
{
    const song_t *sg = &songs[m->song];
    const note_t *n = &sg->notes[m->note];
    if (n->freq) { spk_freq(n->freq); spk_enable(1); }
    else spk_enable(0);
    m->note_start = timer_ms();

    /* scroll the visualiser */
    memmove(m->bars, m->bars + 1, sizeof(m->bars) - 1);
    m->bars[sizeof(m->bars) - 1] = (u8)(n->freq ? (n->freq / 20) : 0);
}

static void music_draw(window_t *w)
{
    music_t *m = (music_t *)w->user;
    int cx = win_content_x(w), cy = win_content_y(w);
    int wd = w->w, ht = w->h - TITLEBAR_H;

    if (m->playing) {
        for (int guard = 0; guard < 8; guard++) {
            const song_t *sg = &songs[m->song];
            const note_t *n = &sg->notes[m->note];
            if (timer_ms() - m->note_start < (u32)n->ms * 100 / (u32)m->tempo) break;
            m->note++;
            if (m->note >= sg->count) {
                if (m->song + 1 < SONG_COUNT) { m->song++; m->note = 0; }
                else { music_stop(m); break; }
            }
            music_advance(m);
        }
    }

    fill_rect(cx, cy, wd, ht, COL_WIN_CONTENT);
    draw_text_bold(cx + 16, cy + 10, "PC SPEAKER PLAYER", COL_ACCENT_PURPLE);

    for (int i = 0; i < SONG_COUNT; i++) {
        int iy = cy + 34 + i * 34;
        int sel = (i == m->song);
        fill_round_rect(cx + 14, iy, wd - 28, 28, 6, sel ? COL_SELECT : 0xEDEFF4);
        draw_text(cx + 26, iy + 7, songs[i].name, sel ? 0xFFFFFF : COL_TEXT);
        draw_text(cx + 190, iy + 7, songs[i].desc, COL_TEXT_DIM);
    }

    /* transport */
    int by = cy + 146;
    ui_button(cx + 14, by, 96, 30, m->playing ? "STOP" : "PLAY", m->playing ? UI_DESTRUCT : UI_FILLED);
    ui_button(cx + 118, by, 96, 30, "NEXT", UI_BORDERED);
    ui_toggle(cx + 226, by + 3, m->loop);
    draw_text(cx + 276, by + 8, "loop", COL_TEXT_DIM);

    char buf[64];
    const song_t *sg = &songs[m->song];
    u16 f = m->playing ? sg->notes[m->note].freq : 0;
    ksnprintf(buf, sizeof(buf), "%s   note %d/%d   %s   %u Hz",
              sg->name, m->playing ? m->note + 1 : 0, sg->count,
              m->playing ? note_name(f) : "-", m->playing ? f : 0);
    draw_text(cx + 16, by + 44, buf, COL_TEXT);

    /* spectrum bars */
    int gx = cx + 16, gy = by + 68, gw = wd - 32, gh = ht - (by + 68 - cy) - 30;
    fill_round_rect(gx, gy, gw, gh, 6, 0xF2F4F8);
    int step = gw / (int)sizeof(m->bars);
    for (unsigned i = 0; i < sizeof(m->bars); i++) {
        int h = m->bars[i] * (gh - 6) / 64;
        if (h < 1) h = 1;
        if (h > gh - 6) h = gh - 6;
        u32 col = (u32)(0x40 + i * 3) << 16 | 0x60 << 8 | 0xC0;
        fill_rect(gx + 3 + (int)i * step, gy + gh - 3 - h, step > 1 ? step - 1 : 1, h, col);
    }

    draw_text(cx + 16, cy + ht - 20, "host QEMU needs an audio backend to be audible", COL_TEXT_DIM);
}

static int music_event(window_t *w, int type, int a, int b, int c)
{
    music_t *m = (music_t *)w->user;
    UNUSED(c);

    if (type == EV_MOUSE_DOWN) {
        int cx = win_content_x(w), cy = win_content_y(w);
        for (int i = 0; i < SONG_COUNT; i++) {
            int iy = cy + 34 + i * 34;
            if (a >= cx + 14 && a < cx + w->w - 14 && b >= iy && b < iy + 28) {
                m->song = i;
                m->note = 0;
                gui_invalidate();
                return 1;
            }
        }
        int by = cy + 146;
        if (b >= by && b < by + 30) {
            if (a >= cx + 226 && a < cx + 268) { m->loop = !m->loop; gui_invalidate(); return 1; }
            if (a >= cx + 14 && a < cx + 110) {
                if (m->playing) music_stop(m);
                else { m->playing = 1; m->note = 0; music_advance(m); }
            } else if (a >= cx + 118 && a < cx + 214) {
                m->song = (m->song + 1) % SONG_COUNT;
                m->note = 0;
                if (m->playing) music_advance(m);
            }
            gui_invalidate();
        }
        return 1;
    }
    if (type == EV_MOUSE_MOVE) {
        int ccx = win_content_x(w), ccy = win_content_y(w);
        int sx = ccx + 200, sy = ccy + 6;
        if ((gui_mouse_buttons() & 1) && b >= sy - 14 && b <= sy + 26 && a >= sx - 10 && a <= sx + 170) {
            int v = ui_slider_value(sx, 160, 60, 200, a);
            if (v != m->tempo) { m->tempo = v; gui_invalidate(); }
        }
        return 1;
    }
    if (type == EV_KEY) {
        if ((u8)a == ' ') { if (m->playing) music_stop(m); else { m->playing = 1; m->note = 0; music_advance(m); } gui_invalidate(); }
        return 1;
    }
    if (type == EV_CLOSE) music_stop(m);
    return 0;
}

window_t *app_music_open(void)
{
    music_t *m = &g_music;
    if (m->win && m->win->visible) { win_focus(m->win); return m->win; }

    memset(m, 0, sizeof(*m));
    m->tempo = 100;
    window_t *w = win_create("Music", 340, 170, 420, 330, music_draw, music_event, m);
    if (w) w->anim = 2;
    m->win = w;
    return w;
}