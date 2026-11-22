/* ===========================================================================
 *  Nutro+OS - terminal application
 * ===========================================================================
 */
#include "kernel.h"

#define TERM_COLS 74
#define TERM_ROWS 20
#define TERM_LINE_MAX 120

typedef struct {
    window_t *win;
    char  grid[TERM_ROWS][TERM_COLS + 1];
    int   cx, cy;
    char  input[TERM_LINE_MAX];
    int   input_len;
} term_t;

static term_t g_term;
extern fat32_t *fs_get(void);

static void term_putc(term_t *t, char c)
{
    if (c == '\n') {
        t->cx = 0;
        t->cy++;
    } else if (c == '\r') {
        t->cx = 0;
    } else {
        if (t->cx >= TERM_COLS) { t->cx = 0; t->cy++; }
        t->grid[t->cy][t->cx++] = c;
    }
    if (t->cy >= TERM_ROWS) {
        memmove(t->grid[0], t->grid[1], (TERM_ROWS - 1) * (TERM_COLS + 1));
        memset(t->grid[TERM_ROWS - 1], 0, TERM_COLS + 1);
        t->cy = TERM_ROWS - 1;
    }
}

static void term_puts(term_t *t, const char *s)
{
    while (*s) term_putc(t, *s++);
}

static void term_prompt(term_t *t)
{
    const char *p = "Nutro+> ";
    while (*p) term_putc(t, *p++);
}

/* ------------------------------------------------------------ commands --- */
/* resolve "DOCS/SUB/FILE.TXT": walk the directories, return the leaf name */
static u32 path_walk(fat32_t *fs, const char *path, char *leaf, int leaf_sz)
{
    u32 dir = fs->root_cluster;
    char buf[64];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    char *p = buf;
    for (;;) {
        char *slash = NULL;
        for (char *q = p; *q; q++) if (*q == '/') { slash = q; break; }
        if (!slash) break;
        *slash = 0;
        if (*p) {
            if (!strcmp(p, ".") || !strcmp(p, "..")) { }
            else {
                u32 sub = fat32_find_dir(fs, dir, p);
                if (!sub) return 0;
                dir = sub;
            }
        }
        p = slash + 1;
    }
    strncpy(leaf, p, (u32)leaf_sz - 1);
    leaf[leaf_sz - 1] = 0;
    return dir;
}

static void cmd_ls(term_t *t, const char *arg)
{
    fat32_t *fs = fs_get();
    if (!fs) { term_puts(t, "ATA/FAT32 unavailable\n"); return; }

    u32 dir = fs->root_cluster;
    if (arg && *arg) {
        char leaf[64];
        dir = path_walk(fs, arg, leaf, sizeof(leaf));
        if (!dir) { term_puts(t, "no such directory\n"); return; }
        if (leaf[0]) {
            u32 sub = fat32_find_dir(fs, dir, leaf);
            if (!sub) { term_puts(t, "no such directory\n"); return; }
            dir = sub;
        }
    }

    fat_entry_t entries[64];
    int n = fat32_list(fs, dir, entries, 64);
    if (n < 0) { term_puts(t, "read error\n"); return; }

    char line[64];
    ksnprintf(line, sizeof(line), "%d entries:\n", n);
    term_puts(t, line);
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir) {
            term_puts(t, "  [DIR] ");
            term_puts(t, entries[i].name);
            term_puts(t, "\n");
        } else {
            char sz[16];
            utoa(entries[i].size, sz);
            term_puts(t, "        ");
            term_puts(t, entries[i].name);
            term_puts(t, "   ");
            term_puts(t, sz);
            term_puts(t, " bytes\n");
        }
    }
}

static void cmd_cat(term_t *t, const char *arg)
{
    fat32_t *fs = fs_get();
    if (!fs || !arg || !*arg) { term_puts(t, "usage: cat <FILE> or <DIR/FILE>\n"); return; }

    char leaf[64];
    u32 dir = path_walk(fs, arg, leaf, sizeof(leaf));
    if (!dir || !leaf[0]) { term_puts(t, "no such file\n"); return; }

    fat_entry_t entries[64];
    int n = fat32_list(fs, dir, entries, 64);
    if (n < 0) { term_puts(t, "read error\n"); return; }
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir) continue;
        if (strcmp(entries[i].name, leaf) != 0) continue;
        char *buf = (char *)kmalloc(entries[i].size + 1);
        if (!buf) { term_puts(t, "out of memory\n"); return; }
        int got = fat32_read_file(fs, entries[i].first_cluster, entries[i].size, buf, entries[i].size);
        if (got < 0) { term_puts(t, "read error\n"); return; }
        buf[got] = 0;
        term_puts(t, buf);
        term_puts(t, "\n");
        return;
    }
    term_puts(t, "file not found\n");
}

/* FAT 8.3 names are upper case: let the user type either */
static void upcase(char *s)
{
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

static void term_execute(term_t *t, char *line)
{
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg = 0; arg++; }
    while (*arg == ' ') arg++;
    upcase(arg);

    term_puts(t, "\n");

    if (!strcmp(line, "")) { }
    else if (!strcmp(line, "help")) {
        term_puts(t, "commands: help ver mem uptime time date ls [dir] cat <file> echo <text> clear about\n");
    } else if (!strcmp(line, "ver")) {
        term_puts(t, "Nutro+OS 1.0 - 32-bit protected mode, VESA GUI, FAT32\n");
        term_puts(t, "kernel built "); term_puts(t, kernel_build_date()); term_puts(t, "\n");
    } else if (!strcmp(line, "about")) {
        char buf[128];
        ksnprintf(buf, sizeof(buf), "Nutro+OS   %ux%u %u bpp   heap %u/%u KiB used\n",
                  fb_width, fb_height, fb_bpp, heap_used() / 1024, heap_size() / 1024);
        term_puts(t, buf);
    } else if (!strcmp(line, "mem")) {
        char buf[128];
        u32 total = mem_total_kb(boot_info_get());
        ksnprintf(buf, sizeof(buf), "usable RAM %u KiB   heap %u KiB (%u KiB used)\n",
                  total, heap_size() / 1024, heap_used() / 1024);
        term_puts(t, buf);
    } else if (!strcmp(line, "uptime")) {
        char buf[64];
        u32 ms = timer_ms();
        ksnprintf(buf, sizeof(buf), "up %u.%03u s   ticks %u\n", ms / 1000, ms % 1000, timer_ticks());
        term_puts(t, buf);
    } else if (!strcmp(line, "time") || !strcmp(line, "date")) {
        rtc_time_t rt;
        rtc_read(&rt);
        char buf[80];
        ksnprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d\n",
                  rt.year, rt.month, rt.day, rt.hour, rt.minute, rt.second);
        term_puts(t, buf);
    } else if (!strcmp(line, "ls")) {
        cmd_ls(t, arg);
    } else if (!strcmp(line, "cat")) {
        cmd_cat(t, arg);
    } else if (!strcmp(line, "echo")) {
        term_puts(t, arg);
        term_puts(t, "\n");
    } else if (!strcmp(line, "clear")) {
        memset(t->grid, 0, sizeof(t->grid));
        t->cx = t->cy = 0;
    } else {
        term_puts(t, "unknown command: ");
        term_puts(t, line);
        term_puts(t, "\n");
    }

    term_prompt(t);
    t->input_len = 0;
    t->input[0] = 0;
    gui_invalidate();
}

/* --------------------------------------------------------------- widget --- */
static void term_draw(window_t *w)
{
    term_t *t = (term_t *)w->user;
    int cx = win_content_x(w), cy = win_content_y(w);

    fill_rect(cx, cy, w->w, w->h - TITLEBAR_H, 0x0C1016);

    for (int row = 0; row <= t->cy && row < TERM_ROWS; row++) {
        const char *line = t->grid[row];
        if (!line[0]) continue;
        draw_text(cx + 8, cy + 6 + row * 16, line, 0xC8E6B0);
    }

    /* the current input line with a block cursor */
    char buf[TERM_LINE_MAX + 8];
    ksnprintf(buf, sizeof(buf), "Nutro+> %s", t->input);
    int y = cy + 6 + t->cy * 16;
    draw_text(cx + 8, y, buf, 0xE8ECF4);
    fill_rect(cx + 8 + (int)strlen(buf) * 8, y, 8, 15, 0x9AA4B8);
}

static int term_event(window_t *w, int type, int a, int b, int c)
{
    term_t *t = (term_t *)w->user;
    UNUSED(b); UNUSED(c);

    if (type == EV_MOUSE_DOWN) { win_focus(w); return 1; }

    if (type == EV_KEY) {
        u8 ch = (u8)a;
        if (ch == '\n') {
            t->input[t->input_len] = 0;
            char line[TERM_LINE_MAX];
            strncpy(line, t->input, TERM_LINE_MAX - 1);
            line[TERM_LINE_MAX - 1] = 0;
            term_execute(t, line);
        } else if (ch == '\b') {
            if (t->input_len > 0) t->input[--t->input_len] = 0;
            gui_invalidate();
        } else if (ch >= 32 && ch < 127) {
            if (t->input_len < TERM_LINE_MAX - 2) {
                t->input[t->input_len++] = ch;
                t->input[t->input_len] = 0;
            }
            gui_invalidate();
        }
        return 1;
    }
    return 0;
}

window_t *app_terminal_open(void)
{
    term_t *t = &g_term;
    if (t->win && t->win->visible) { win_focus(t->win); return t->win; }

    memset(t, 0, sizeof(*t));
    term_puts(t, "Nutro+OS 1.0 terminal - type 'help' for commands\n");
    term_prompt(t);

    window_t *w = win_create("Terminal", 120, 90, 640, 380, term_draw, term_event, t);
    t->win = w;
    return w;
}