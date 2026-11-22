/* ===========================================================================
 *  Nutro+OS - FAT32 file browser + text viewer
 * ===========================================================================
 */
#include "kernel.h"

static fat32_t g_fs;
static int     g_fs_ready;

fat32_t *fs_get(void)
{
    if (!g_fs_ready) {
        if (fat32_mount(&g_fs, *(u32 *)(0x4000 + 56)) != 0) return NULL;
        g_fs_ready = 1;
        debug_write("fat32: volume mounted\n");
    }
    return &g_fs;
}

/* ------------------------------------------------------------- browser --- */
#define MAX_ENTRIES 96
#define ROW_H       22

typedef struct {
    window_t *win;
    u32  dir_cluster;
    u32  history[8];
    int  hist_len;
    char cwd[64];
    fat_entry_t entries[MAX_ENTRIES];
    int  count;
    int  selected;
} files_t;

static files_t g_files;

static void files_reload(files_t *f)
{
    fat32_t *fs = fs_get();
    f->count = 0;
    f->selected = 0;
    if (!fs) return;
    int n = fat32_list(fs, f->dir_cluster, f->entries, MAX_ENTRIES);
    f->count = n > 0 ? n : 0;
}

static void files_draw(window_t *w)
{
    files_t *f = (files_t *)w->user;
    int cx = win_content_x(w), cy = win_content_y(w);
    int wd = w->w, ht = w->h - TITLEBAR_H;

    fill_rect(cx, cy, wd, ht, COL_WIN_CONTENT);

    /* path bar */
    fill_rect(cx, cy, wd, 26, 0xEFF1F6);
    draw_text(cx + 10, cy + 5, "FAT32 volume:", COL_TEXT_DIM);
    draw_text(cx + 130, cy + 5, f->cwd, COL_ACCENT);
    draw_text(cx + wd - 84, cy + 5, "Enter: open", COL_TEXT_DIM);

    /* column header */
    draw_text(cx + 12, cy + 32, "NAME", COL_TEXT_DIM);
    draw_text(cx + wd - 120, cy + 32, "SIZE", COL_TEXT_DIM);

    int rows = (ht - 60) / ROW_H;
    for (int i = 0; i < rows && i < f->count; i++) {
        fat_entry_t *e = &f->entries[i];
        int ry = cy + 50 + i * ROW_H;
        if (i == f->selected) {
            fill_round_rect(cx + 6, ry - 2, wd - 12, ROW_H - 2, 5, COL_SELECT);
        }
        u32 col = e->is_dir ? COL_ACCENT_ORANGE : COL_TEXT;
        fill_round_rect(cx + 14, ry + 3, 12, 12, 3, col);
        draw_text(cx + 34, ry + 1, e->name, e->is_dir ? COL_ACCENT_ORANGE : COL_TEXT);
        if (!e->is_dir) {
            char sz[24];
            ksnprintf(sz, sizeof(sz), "%u", e->size);
            int tw = text_width(sz);
            draw_text(cx + wd - 24 - tw, ry + 1, sz, COL_TEXT_DIM);
        } else {
            draw_text(cx + wd - 70, ry + 1, "<DIR>", COL_TEXT_DIM);
        }
    }
    if (f->count == 0) draw_text(cx + 24, cy + 60, "(empty)", COL_TEXT_DIM);
}

static void files_open_entry(files_t *f, fat_entry_t *e)
{
    if (e->is_dir) {
        if (f->hist_len < 8) f->history[f->hist_len++] = f->dir_cluster;
        f->dir_cluster = e->first_cluster;
        if (f->cwd[0] != '/' || f->cwd[1] != 0) strcat(f->cwd, "/");
        strcat(f->cwd, e->name);
        files_reload(f);
        gui_invalidate();
        return;
    }
    /* load and show text files */
    fat32_t *fs = fs_get();
    if (!fs) return;
    if (e->size > 64 * 1024) return;
    char *buf = (char *)kmalloc(e->size + 1);
    if (!buf) return;
    int got = fat32_read_file(fs, e->first_cluster, e->size, buf, e->size);
    if (got < 0) return;
    buf[got] = 0;
    viewer_open_text(e->name, buf, (u32)got);
}

static int files_event(window_t *w, int type, int a, int b, int c)
{
    files_t *f = (files_t *)w->user;
    int cx = win_content_x(w), cy = win_content_y(w);
    UNUSED(c);

    if (type == EV_MOUSE_DOWN) {
        int rel = (b - cy - 50) / ROW_H;
        if (a >= cx && b >= cy + 50 && rel >= 0 && rel < f->count) {
            if (f->selected == rel) {          /* second click opens */
                files_open_entry(f, &f->entries[rel]);
            } else {
                f->selected = rel;
                gui_invalidate();
            }
        }
        return 1;
    }
    if (type == EV_KEY) {
        u8 ch = (u8)a;                  /* unsigned: KEY_* codes are >= 0x90 */
        if (ch == KEY_DOWN) { if (f->selected + 1 < f->count) f->selected++; gui_invalidate(); }
        else if (ch == KEY_UP) { if (f->selected > 0) f->selected--; gui_invalidate(); }
        else if (ch == '\n') { if (f->count) files_open_entry(f, &f->entries[f->selected]); }
        else if (ch == KEY_ESC) {
            if (f->hist_len > 0) {
                f->dir_cluster = f->history[--f->hist_len];
                char *last = NULL;
                for (char *p = f->cwd + 1; *p; p++) if (*p == '/') last = p;
                if (last) *last = 0;
                else f->cwd[1] = 0;
                files_reload(f);
                gui_invalidate();
            }
        }
        return 1;
    }
    return 0;
}

window_t *app_files_open(void)
{
    files_t *f = &g_files;
    if (f->win && f->win->visible) { win_focus(f->win); return f->win; }

    memset(f, 0, sizeof(*f));
    fat32_t *fs = fs_get();
    f->dir_cluster = fs ? fs->root_cluster : 2;
    strcpy(f->cwd, "/");
    files_reload(f);

    window_t *w = win_create("Files - FAT32", 380, 120, 560, 400, files_draw, files_event, f);
    f->win = w;
    return w;
}

/* -------------------------------------------------------------- viewer --- */
typedef struct {
    window_t *win;
    char *text;
    u32   len;
    int   scroll;
    int   lines;
} viewer_t;

static viewer_t g_viewer;

static void viewer_draw(window_t *w)
{
    viewer_t *v = (viewer_t *)w->user;
    int cx = win_content_x(w), cy = win_content_y(w);
    int ht = w->h - TITLEBAR_H;

    fill_rect(cx, cy, w->w, ht, COL_WIN_CONTENT);

    int rows = (ht - 20) / 16;
    int row = 0;
    const char *p = v->text;
    int line = 0;

    while (*p && row < rows) {
        if (line < v->scroll) {
            while (*p && *p != '\n') p++;
            if (*p) p++;
            line++;
            continue;
        }
        char buf[110];
        int n = 0;
        while (*p && *p != '\n' && *p != '\r' && n < 100) buf[n++] = *p++;
        buf[n] = 0;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        draw_text(cx + 12, cy + 10 + row * 16, buf, COL_TEXT);
        row++;
        line++;
    }
    if (v->scroll > 0)
        draw_text(cx + w->w - 26, cy + 6, "^", COL_TEXT_DIM);
    draw_text(cx + 12, cy + ht - 18, "(up/down keys scroll)", COL_TEXT_DIM);
}

static int viewer_event(window_t *w, int type, int a, int b, int c)
{
    viewer_t *v = (viewer_t *)w->user;
    UNUSED(b); UNUSED(c);
    if (type != EV_KEY) return 0;
    u8 ch = (u8)a;
    if (ch == KEY_DOWN) { v->scroll++; gui_invalidate(); }
    else if (ch == KEY_UP) { if (v->scroll > 0) v->scroll--; gui_invalidate(); }
    else if (ch == KEY_ESC) win_close(w);
    return 1;
}

window_t *viewer_open_text(const char *title, const char *text, u32 len)
{
    viewer_t *v = &g_viewer;
    v->text = (char *)text;
    v->len = len;
    v->scroll = 0;
    v->lines = 0;

    char t[48];
    ksnprintf(t, sizeof(t), "Viewer - %s", title);
    window_t *w = win_create(t, 260, 150, 560, 360, viewer_draw, viewer_event, v);
    v->win = w;
    return w;
}

window_t *app_viewer_open_default(void)
{
    fat32_t *fs = fs_get();
    if (!fs) return NULL;
    fat_entry_t entries[32];
    int n = fat32_list(fs, fs->root_cluster, entries, 32);
    for (int i = 0; i < n; i++) {
        if (entries[i].is_dir) continue;
        char *buf = (char *)kmalloc(entries[i].size + 1);
        if (!buf) continue;
        int got = fat32_read_file(fs, entries[i].first_cluster, entries[i].size, buf, entries[i].size);
        if (got > 0) { buf[got] = 0; return viewer_open_text(entries[i].name, buf, (u32)got); }
    }
    return NULL;
}