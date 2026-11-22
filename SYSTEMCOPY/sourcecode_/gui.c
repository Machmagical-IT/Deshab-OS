/* ===========================================================================
void fill_round_rect_blend(int x, int y, int w, int h, int radius, u32 rgb, int alpha);
 *  Nutro+OS - window manager, macOS 13 (Ventura) inspired
 *
 *  * top menu bar + bottom dock instead of taskbar/start menu
 *  * windows: 14 px corners, traffic light controls, centred title
 *  * every window keeps its content in an off-screen cache, so moving,
 *    raising or dragging a window is a raw memory blit - no re-drawing
 *  * only dirty rectangles are recomposed and copied to the framebuffer
 *  * the cursor is an overlay with a save-under box (~250 px per move)
 * ===========================================================================
 */
#include "kernel.h"
void fill_round_rect_blend(int x, int y, int w, int h, int radius, u32 rgb, int alpha);

#define MENUBAR_H   26
#define DOCK_H      60
#define DOCK_ICON   46
#define DOCK_GAP    12

#define SHADOW      9
#define CUR_W       12
#define CUR_H       19

#define TL_R        6                    /* traffic light radius */
#define TL_GAP      18

static window_t *g_windows = NULL;       /* head = topmost */
static window_t *g_focused = NULL;
static int g_next_id = 1;

static int g_mouse_x, g_mouse_y;
static u8  g_btn_prev;
static int g_dragging, g_drag_dx, g_drag_dy;
static int g_pending_app = -1;

static u8 *g_wallpaper;

/* the window whose content is currently being drawn into its own cache */
static window_t *g_caching;

/* dirty rectangles */
#define MAX_DIRTY 4
static struct { int x, y, w, h; } dr[MAX_DIRTY];
static int dr_count;
static int d_full = 1;

static u8  cur_save[CUR_W * CUR_H * 4];
static int cur_drawn, cur_x, cur_y;

static u32 perf_frames, perf_cursor, perf_px;

static const char *cursor_art[CUR_H] = {
    "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ",
    "#....#      ", "#.....#     ", "#......#    ", "#.......#   ", "#........#  ",
    "#....#####  ", "#..#.#      ", "#.# #.#     ", "##  #.#     ", "#    #.#    ",
    "     #.#    ", "      #.#   ", "      #.#   ", "       #    "
};

/* forward declarations for the animation helpers */
static void an_start(window_t *w, int kind, int x, int y, int wd, int ht, u32 dur, int flags);
static void an_from_scale(window_t *w, int scale_pct);
static void an_minimise(window_t *w);
static void an_advance(void);

static int dock_y(void) { return (int)fb_height - DOCK_H - 6; }
static int dock_w(void) { return g_app_count * (DOCK_ICON + DOCK_GAP) - DOCK_GAP + 24; }
static int dock_x(void) { return ((int)fb_width - dock_w()) / 2; }

/* -------------------------------------------------------- invalidation --- */
static int rects_overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return !(ax >= bx + bw || bx >= ax + aw || ay >= by + bh || by >= ay + ah);
}

static void inval(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_width) w = (int)fb_width - x;
    if (y + h > (int)fb_height) h = (int)fb_height - y;
    if (w <= 0 || h <= 0) return;

    for (int i = 0; i < dr_count; i++) {
        if (rects_overlap(x, y, w, h, dr[i].x, dr[i].y, dr[i].w, dr[i].h)) {
            int nx0 = x < dr[i].x ? x : dr[i].x;
            int ny0 = y < dr[i].y ? y : dr[i].y;
            int nx1 = x + w > dr[i].x + dr[i].w ? x + w : dr[i].x + dr[i].w;
            int ny1 = y + h > dr[i].y + dr[i].h ? y + h : dr[i].y + dr[i].h;
            dr[i].x = nx0; dr[i].y = ny0; dr[i].w = nx1 - nx0; dr[i].h = ny1 - ny0;
            if (dr[i].w * dr[i].h > (int)(fb_width * fb_height * 55 / 100)) d_full = 1;
            return;
        }
    }
    if (dr_count < MAX_DIRTY) {
        dr[dr_count].x = x; dr[dr_count].y = y;
        dr[dr_count].w = w; dr[dr_count].h = h;
        dr_count++;
        return;
    }
    int nx0 = x, ny0 = y, nx1 = x + w, ny1 = y + h;
    for (int i = 0; i < dr_count; i++) {
        if (dr[i].x < nx0) nx0 = dr[i].x;
        if (dr[i].y < ny0) ny0 = dr[i].y;
        if (dr[i].x + dr[i].w > nx1) nx1 = dr[i].x + dr[i].w;
        if (dr[i].y + dr[i].h > ny1) ny1 = dr[i].y + dr[i].h;
    }
    dr[0].x = nx0; dr[0].y = ny0; dr[0].w = nx1 - nx0; dr[0].h = ny1 - ny0;
    dr_count = 1;
    if (dr[0].w * dr[0].h > (int)(fb_width * fb_height * 55 / 100)) d_full = 1;
}

/* the frame moved/changed but the app content did not */
static void inval_frame(window_t *w)
{
    if (!w || !w->visible) return;
    inval(w->x - SHADOW, w->y - SHADOW, w->w + SHADOW * 2, w->h + SHADOW * 2);
}

/* the app content changed - the cache has to be painted again */
static void inval_content(window_t *w)
{
    if (!w) return;
    w->cache_dirty = 1;
    inval_frame(w);
}

void win_set_dirty_content(window_t *w) { inval_content(w); }

static void inval_menubar(void)
{
    inval(0, 0, (int)fb_width, MENUBAR_H);
}

static void inval_dock(void)
{
    inval(dock_x() - 8, dock_y() - 10, dock_w() + 16, DOCK_H + 16);
}

void gui_invalidate(void)
{
    if (g_focused && g_focused->visible) inval_content(g_focused);
    else d_full = 1;
}

void gui_invalidate_rect(int x, int y, int w, int h)
{
    /* refresh the cache of the window that owns this screen rectangle */
    for (window_t *it = g_windows; it; it = it->next) {
        if (!it->visible) continue;
        if (rects_overlap(x, y, w, h, it->x, it->y, it->w, it->h)) { it->cache_dirty = 1; break; }
    }
    inval(x, y, w, h);
}

int gui_mouse_over(int x, int y, int w, int h)
{
    return g_mouse_x >= x && g_mouse_x < x + w && g_mouse_y >= y && g_mouse_y < y + h;
}

int gui_mouse_buttons(void) { return g_btn_prev; }

int gui_window_count(void)
{
    int n = 0;
    for (window_t *w = g_windows; w; w = w->next) if (w->visible) n++;
    return n;
}

/* ------------------------------------------------------------- helpers --- */
int win_content_x(window_t *w) { return (g_caching == w) ? 0 : w->x; }
int win_content_y(window_t *w) { return (g_caching == w) ? 0 : w->y + TITLEBAR_H; }
int win_abs_x(window_t *w) { return w->x; }
int win_abs_content_y(window_t *w) { return w->y + TITLEBAR_H; }

window_t *win_focused(void) { return g_focused; }

static int inside(window_t *w, int px, int py)
{
    return px >= w->x && px < w->x + w->w && py >= w->y && py < w->y + w->h;
}

static void unlink_window(window_t *w)
{
    if (w->prev) w->prev->next = w->next;
    else g_windows = w->next;
    if (w->next) w->next->prev = w->prev;
    w->next = w->prev = NULL;
}

static void raise_window(window_t *w)
{
    if (g_windows == w) return;
    unlink_window(w);
    w->prev = NULL;
    w->next = g_windows;
    if (g_windows) g_windows->prev = w;
    g_windows = w;
}

void win_focus(window_t *w)
{
    if (!w || !w->visible || g_focused == w) return;
    window_t *old = g_focused;
    g_focused = w;
    raise_window(w);
    inval_frame(old);
    inval_frame(w);
    inval_menubar();
    inval_dock();
}

window_t *win_create(const char *title, int x, int y, int w, int h,
                     void (*draw)(window_t *), int (*event)(window_t *, int, int, int, int),
                     void *user)
{
    window_t *win = (window_t *)kmalloc(sizeof(window_t));
    if (!win) return NULL;
    memset(win, 0, sizeof(*win));
    win->x = x; win->y = y; win->w = w; win->h = h;
    strncpy(win->title, title, sizeof(win->title) - 1);
    win->visible = 1;
    win->closable = 1;
    win->cache_dirty = 1;
    win->app_id = g_pending_app;
    win->id = g_next_id++;
    win->draw = draw;
    win->event = event;
    win->user = user;
    win->prev = NULL;
    win->next = g_windows;
    if (g_windows) g_windows->prev = win;
    g_windows = win;
    g_focused = win;
    an_from_scale(win, 88);              /* pop in */
    inval_menubar();
    inval_dock();
    return win;
}

/* closing plays a short shrink animation first (macOS style) */
void win_close(window_t *w)
{
    if (!w || w->dying) return;
    w->dying = 1;
    w->an_kind = 0;
    int nw = w->w * 88 / 100, nh = w->h * 88 / 100;
    an_start(w, 1, w->x + (w->w - nw) / 2, w->y + (w->h - nh) / 2, nw, nh, 150, 1);
    if (g_focused == w) {
        g_focused = NULL;
        for (window_t *it = g_windows; it; it = it->next)
            if (it->visible && !it->dying) { g_focused = it; break; }
    }
    inval_menubar();
    inval_dock();
}

/* --------------------------------------------------------- traffic light -- */
static void tl_pos(window_t *w, int idx, int *cx, int *cy)
{
    *cx = w->x + 18 + idx * TL_GAP;
    *cy = w->y + TITLEBAR_H / 2;
}

static int tl_hit(window_t *w, int px, int py, int *which)
{
    for (int i = 0; i < 3; i++) {
        int cx, cy;
        tl_pos(w, i, &cx, &cy);
        int dx = px - cx, dy = py - cy;
        if (dx * dx + dy * dy <= (TL_R + 3) * (TL_R + 3)) { *which = i; return 1; }
    }
    return 0;
}

static void win_toggle_zoom(window_t *w)
{
    if (!w->zoomed) {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
        w->x = 8;
        w->y = MENUBAR_H + 8;
        w->w = (int)fb_width - 16;
        w->h = dock_y() - MENUBAR_H - 16;
        w->zoomed = 1;
    } else {
        w->x = w->sx; w->y = w->sy; w->w = w->sw; w->h = w->sh;
        w->zoomed = 0;
    }
    inval_menubar();
}

/* -------------------------------------------------------------- cursor --- */
static void lfb_write(int x, int y, u32 val)
{
    if (x < 0 || y < 0 || x >= (int)fb_width || y >= (int)fb_height) return;
    u8 *p = fb_mem + (u32)y * fb_pitch + (u32)x * (fb_bpp >> 3);
    if (fb_bpp == 32) *(u32 *)p = val;
    else if (fb_bpp == 24) { p[0] = (u8)val; p[1] = (u8)(val >> 8); p[2] = (u8)(val >> 16); }
    else *(u16 *)p = (u16)val;
}

static void cursor_erase(void)
{
    if (!cur_drawn) return;
    u32 nb = fb_bpp >> 3;
    for (int row = 0; row < CUR_H; row++)
        for (int col = 0; col < CUR_W; col++) {
            int px = cur_x + col, py = cur_y + row;
            if (px < 0 || py < 0 || px >= (int)fb_width || py >= (int)fb_height) continue;
            u8 *dst = fb_mem + (u32)py * fb_pitch + (u32)px * nb;
            const u8 *s = cur_save + (row * CUR_W + col) * 4;
            if (nb == 4) *(u32 *)dst = *(u32 *)s;
            else if (nb == 3) { dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2]; }
            else *(u16 *)dst = *(u16 *)s;
        }
    cur_drawn = 0;
}

static void cursor_move_to(int x, int y)
{
    if (cur_drawn && x == cur_x && y == cur_y) return;
    cursor_erase();
    u32 nb = fb_bpp >> 3;
    for (int row = 0; row < CUR_H; row++) {
        const char *line = cursor_art[row];
        for (int col = 0; col < CUR_W; col++) {
            int px = x + col, py = y + row;
            if (px < 0 || py < 0 || px >= (int)fb_width || py >= (int)fb_height) continue;
            u8 *dst = fb_mem + (u32)py * fb_pitch + (u32)px * nb;
            u8 *s = cur_save + (row * CUR_W + col) * 4;
            if (nb == 4) *(u32 *)s = *(u32 *)dst;
            else if (nb == 3) { s[0] = dst[0]; s[1] = dst[1]; s[2] = dst[2]; }
            else *(u16 *)s = *(u16 *)dst;
            if (line[col] == '#') lfb_write(px, py, pack_color(0x000000));
            else if (line[col] == '.') lfb_write(px, py, pack_color(0xFFFFFF));
        }
    }
    cur_x = x; cur_y = y;
    cur_drawn = 1;
    perf_cursor++;
}

static void cursor_clear_for(int rx, int ry, int rw, int rh)
{
    if (!cur_drawn) return;
    if (rects_overlap(cur_x, cur_y, CUR_W, CUR_H, rx, ry, rw, rh)) cursor_erase();
}

/* -------------------------------------------------------------- painting -- */
/* ------------------------------------------------------- animations ------ */
static int an_active(window_t *w)
{
    return w->an_kind && (timer_ms() - w->an_start) < w->an_dur;
}

/* eased progress 0..1024 (ease-out cubic, like a SwiftUI spring settle) */
static int an_progress(window_t *w)
{
    if (!w->an_kind || !w->an_dur) return 1024;
    u32 dt = timer_ms() - w->an_start;
    if (dt >= w->an_dur) return 1024;
    u32 u = 1024 - (dt * 1024 / w->an_dur);
    u32 e = 1024 - (u * u / 1024) * u / 1024;
    return (int)e;
}

/* the rectangle the window occupies right now (animated or final) */
static void an_rect(window_t *w, int *x, int *y, int *wd, int *ht)
{
    if (!w->an_kind) {
        *x = w->x; *y = w->y; *wd = w->w; *ht = w->h;
        return;
    }
    int t = an_progress(w);
    *x  = w->an_fx + ((int)w->x  - w->an_fx)  * t / 1024;
    *y  = w->an_fy + ((int)w->y  - w->an_fy)  * t / 1024;
    *wd = w->an_fw + ((int)w->w - w->an_fw) * t / 1024;
    *ht = w->an_fh + ((int)w->h - w->an_fh) * t / 1024;
    if (*wd < 8) *wd = 8;
    if (*ht < 8) *ht = 8;
}

static void an_start(window_t *w, int kind, int x, int y, int wd, int ht, u32 dur, int flags)
{
    w->an_kind = (u8)kind;
    w->an_flags = (u8)flags;
    w->an_start = timer_ms();
    w->an_dur = dur;
    w->an_fx = (s16)x; w->an_fy = (s16)y; w->an_fw = (s16)wd; w->an_fh = (s16)ht;
    w->an_lx = (s16)x; w->an_ly = (s16)y; w->an_lw = (s16)wd; w->an_lh = (s16)ht;
    inval(x - SHADOW, y - SHADOW, wd + SHADOW * 2, ht + SHADOW * 2);
}

/* shrink towards the centre of the given rectangle */
static void an_from_scale(window_t *w, int scale_pct)
{
    int x, y, wd, ht;
    x = w->x; y = w->y; wd = w->w; ht = w->h;
    int nw = wd * scale_pct / 100, nh = ht * scale_pct / 100;
    int nx = x + (wd - nw) / 2, ny = y + (ht - nh) / 2;
    an_start(w, 1, nx, ny, nw, nh, 170, 0);
}

/* dock icon centre for an app (used by the minimise animation) */
static int dock_icon_cx(int index);
static int dock_icon_cy(void);

static void an_minimise(window_t *w)
{
    if (!w->cache) { w->visible = 0; w->minimized = 1; inval_frame(w); return; }
    int cx = (w->app_id >= 0) ? dock_icon_cx(w->app_id) : (int)fb_width / 2;
    int cy = dock_icon_cy();
    an_start(w, 3, cx - 6, cy - 6, 12, 12, 220, 2);
}

/* finish windows whose animation is done; invalidate the moving rects */
static void an_advance(void)
{
    window_t *w = g_windows;
    while (w) {
        window_t *next = w->next;        /* the list may change below */
        if (!w->an_kind) { w = next; continue; }
        if (an_active(w)) {
            int x, y, wd, ht;
            an_rect(w, &x, &y, &wd, &ht);
            /* invalidate the union of the previous and the current frame */
            int ux0 = x < w->an_lx ? x : w->an_lx;
            int uy0 = y < w->an_ly ? y : w->an_ly;
            int ux1 = (x + wd > w->an_lx + w->an_lw) ? x + wd : w->an_lx + w->an_lw;
            int uy1 = (y + ht > w->an_ly + w->an_lh) ? y + ht : w->an_ly + w->an_lh;
            inval(ux0 - SHADOW, uy0 - SHADOW, ux1 - ux0 + SHADOW * 2, uy1 - uy0 + SHADOW * 2);
            w->an_lx = (s16)x; w->an_ly = (s16)y; w->an_lw = (s16)wd; w->an_lh = (s16)ht;
            w = next;
            continue;
        }
        /* finished */
        u8 kind = w->an_kind, flags = w->an_flags;
        w->an_kind = 0;
        inval(w->an_lx - SHADOW, w->an_ly - SHADOW, w->an_lw + SHADOW * 2, w->an_lh + SHADOW * 2);
        if (flags & 1) {
            /* really close it now */
            w->visible = 0;
            if (w->event) w->event(w, EV_CLOSE, 0, 0, 0);
            inval_frame(w);
            unlink_window(w);
            if (g_focused == w) {
                g_focused = NULL;
                for (window_t *it = g_windows; it; it = it->next)
                    if (it->visible && !it->dying) { g_focused = it; break; }
            }
            inval_menubar();
            inval_dock();
        } else if (flags & 2) {
            w->visible = 0;
            w->minimized = 1;
            g_focused = NULL;
            for (window_t *it = g_windows; it; it = it->next)
                if (it->visible && !it->dying) { g_focused = it; break; }
            inval_menubar();
            inval_dock();
        } else {
            inval_frame(w);
            if (kind == 2) { w->cache_dirty = 1; w->cache = NULL; }   /* size changed */
        }
        w = next;
    }
}

static void render_cache(window_t *w)
{
    int cw = w->w, chh = w->h - TITLEBAR_H;
    if (cw <= 0 || chh <= 0) { w->cache_dirty = 0; return; }
    u32 pitch = (u32)cw * (fb_bpp >> 3);
    if (!w->cache) {
        w->cache = (u8 *)kmalloc(pitch * (u32)chh);
        if (!w->cache) { w->cache_dirty = 0; return; }
    }
    memset(w->cache, 0, pitch * (u32)chh);

    u8 *saved = fb_get_target();
    fb_set_target2(w->cache, pitch);
    fb_reset_clip();
    fb_set_clip(0, 0, cw, chh);
    g_caching = w;
    if (w->draw) w->draw(w);
    g_caching = NULL;
    fb_reset_clip();
    fb_set_target2(saved, fb_pitch);
    w->cache_dirty = 0;
}

static void draw_window(window_t *w)
{
    if (!w->visible) return;
    int x, y, wd, ht;
    an_rect(w, &x, &y, &wd, &ht);
    int active = (w == g_focused) && !w->dying;

    /* layered soft shadow - four rings fading outward */
    fill_round_rect_blend(x + 4, y + 9, wd, ht, 16, 0x4E5866, 22);
    fill_round_rect_blend(x + 3, y + 7, wd, ht, 15, 0x515B69, 34);
    fill_round_rect_blend(x + 2, y + 5, wd, ht, 14, 0x566070, 58);
    fill_round_rect_blend(x + 1, y + 3, wd, ht, 14, 0x5C6675, 86);
    fill_round_rect(x, y, wd, ht, 14, COL_WIN_BG);

    /* title bar */
    u32 tb = active ? COL_TITLE_ACTIVE : COL_TITLE_INACTIVE;
    fill_round_rect(x, y, wd, TITLEBAR_H + 10, 14, tb);
    fill_rect(x, y + TITLEBAR_H - 4, wd, 4, tb);
    fill_hline(x + 1, y + TITLEBAR_H, wd - 2, COL_BORDER);

    /* traffic lights */
    int hov = (g_mouse_y >= y && g_mouse_y < y + TITLEBAR_H && g_mouse_x >= x && g_mouse_x < x + 70);
    static const u32 tl_col[3] = { COL_CLOSE, COL_MIN, COL_ZOOM };
    for (int i = 0; i < 3; i++) {
        int cx, cy;
        tl_pos(w, i, &cx, &cy);
        fill_circle(cx, cy, TL_R, active ? tl_col[i] : 0xC8CCD4);
        if (hov && active) {
            u32 fg = 0x5A3000;
            if (i == 0) { draw_line(cx - 2, cy - 2, cx + 2, cy + 2, fg, 1); draw_line(cx - 2, cy + 2, cx + 2, cy - 2, fg, 1); }
            else if (i == 1) fill_rect(cx - 3, cy, 6, 1, fg);
            else { fill_rect(cx - 3, cy, 6, 1, fg); fill_rect(cx, cy - 3, 1, 6, fg); }
        }
    }

    /* centred title */
    int tw = text_width(w->title);
    draw_text_bold(x + (wd - tw) / 2, y + (TITLEBAR_H - 16) / 2, w->title,
                   active ? COL_TEXT : COL_TEXT_DIM);

    /* content from the cache (scaled while a transition runs) */
    if (w->cache_dirty || !w->cache) render_cache(w);
    if (w->cache) {
        int cw = w->w, chh = w->h - TITLEBAR_H;
        int dw = wd, dh = ht - TITLEBAR_H;
        u32 cpitch = (u32)cw * (fb_bpp >> 3);
        if (dw == cw && dh == chh)
            fb_blit_buffer(fb_get_target(), fb_get_pitch(), x, y + TITLEBAR_H,
                           w->cache, cpitch, cw, chh);
        else if (dw > 0 && dh > 0)
            fb_blit_scaled(fb_get_target(), fb_get_pitch(), x, y + TITLEBAR_H, dw, dh,
                           w->cache, cpitch, cw, chh);
    }

    draw_round_rect(x, y, wd, ht, 14, active ? 0xA9AFBC : COL_BORDER);
}

static void draw_menubar(void)
{
    fill_rect(0, 0, (int)fb_width, MENUBAR_H, COL_MENUBAR);
    fill_rect_blend(0, 0, (int)fb_width, MENUBAR_H, 0xFFFFFF, 60);
    fill_hline(0, MENUBAR_H, (int)fb_width, COL_BORDER);

    /* logo + name */
    fill_round_rect(8, 5, 16, 16, 5, COL_ACCENT);
    draw_text(12, 5, "N", 0xFFFFFF);
    draw_text_bold(30, 5, "Nutro+OS", COL_MENUBAR_TEXT);

    if (g_focused && g_focused->visible) {
        draw_text(30 + 8 * 9 + 12, 5, g_focused->title, COL_TEXT_DIM);
    }

    /* right side: version + date/time */
    rtc_time_t t;
    rtc_read(&t);
    char buf[40];
    ksnprintf(buf, sizeof(buf), "v%s   %04d-%02d-%02d  %02d:%02d:%02d",
              NUTRO_VERSION, t.year, t.month, t.day, t.hour, t.minute, t.second);
    int tw = text_width(buf);
    draw_text((int)fb_width - 12 - tw, 5, buf, COL_MENUBAR_TEXT);
}

/* ------------------------------------------------- pictographic icons ---- */
/* every app gets a recognisable symbol built from the basic primitives */
static void glyph_terminal(int cx, int cy, int s)
{
    int w = s / 2;
    fill_round_rect(cx - s / 2, cy - s / 2, s, s, s / 6, 0x0E1218);
    int x0 = cx - w / 2 - 2, y0 = cy - 5;
    draw_line(x0, y0 - 5, x0 + 6, y0, 0x54D98C, 2);
    draw_line(x0 + 6, y0, x0, y0 + 5, 0x54D98C, 2);
    fill_rect(x0 + 9, y0 + 4, 9, 2, 0x54D98C);
}
static void glyph_files(int cx, int cy, int s)
{
    int w = s * 3 / 4, h = s * 3 / 5;
    fill_round_rect(cx - w / 2, cy - h / 2 - 2, w / 3, 6, 2, 0x6FB7FF);
    fill_round_rect(cx - w / 2, cy - h / 2 + 2, w, h, 3, 0x8FC4FF);
    fill_gradient_v(cx - w / 2 + 1, cy - h / 2 + 3, w - 2, h - 3, 0xE9F3FF, 0x4E93E8);
}
static void glyph_clock(int cx, int cy, int s)
{
    int r = s / 2 - 1;
    fill_circle(cx, cy, r + 1, 0x2A2F3A);
    fill_circle(cx, cy, r, 0xFFFFFF);
    for (int i = 0; i < 12; i += 3) {
        int x1 = cx + (r - 4) * fsin_deg(i * 30) / 1000;
        int y1 = cy - (r - 4) * fcos_deg(i * 30) / 1000;
        int x2 = cx + (r - 1) * fsin_deg(i * 30) / 1000;
        int y2 = cy - (r - 1) * fcos_deg(i * 30) / 1000;
        draw_line(x1, y1, x2, y2, 0x3A3F4A, 1);
    }
    draw_line(cx, cy, cx, cy - r * 2 / 3, 0x1D1D22, 2);
    draw_line(cx, cy, cx + r / 2, cy + r / 3, 0xC0261C, 1);
    fill_circle(cx, cy, 2, 0x1D1D22);
}
static void glyph_calc(int cx, int cy, int s)
{
    fill_round_rect(cx - s / 2, cy - s / 2, s, s, s / 6, 0x232833);
    fill_round_rect(cx - s / 2 + 4, cy - s / 2 + 4, s - 8, s / 4, 2, 0x9CCBFF);
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 3; c++)
            fill_round_rect(cx - s / 2 + 5 + c * (s / 4), cy - 2 + r * (s / 4) - 2, s / 6, s / 6, 2, 0xE8ECF4);
}
static void glyph_snake(int cx, int cy, int s)
{
    int step = s / 5;
    for (int i = 0; i < 5; i++) {
        int x = cx - s / 3 + i * step;
        int y = cy + ((i & 1) ? -1 : 1) * s / 6;
        fill_round_rect(x - 3, y - 3, 7, 7, 3, i ? 0xD8F5C8 : 0xFFFFFF);
    }
    fill_circle(cx + s / 3 + 1, cy - s / 8, 4, 0xFF8A5C);
}
static void glyph_mine(int cx, int cy, int s)
{
    int r = s / 4 + 1;
    for (int i = 0; i < 8; i++) {
        int a = i * 45;
        int x = cx + r * fsin_deg(a) / 1000, y = cy - r * fcos_deg(a) / 1000;
        draw_line(cx, cy, x, y, 0xFFF3F2, 2);
    }
    fill_circle(cx, cy, r, 0xFFFFFF);
    fill_circle(cx, cy, 2, 0x2A2F3A);
    draw_line(cx + s / 3, cy + s / 3, cx + s / 3, cy - s / 4, 0xFFFFFF, 2);
    fill_rect(cx + s / 3, cy - s / 4, s / 3, s / 5, 0xFF5F57);
}
static void glyph_music(int cx, int cy, int s)
{
    int x = cx + 2, top = cy - s / 3, bot = cy + s / 8;
    draw_line(x, top, x, bot, 0xFFFFFF, 2);
    draw_line(x, top, x + s / 3, top + 5, 0xFFFFFF, 2);
    fill_circle(x - 4, bot, 5, 0xFFFFFF);
    fill_circle(x + s / 3 - 4, bot - 5, 5, 0xFFFFFF);
}
static void glyph_sysmon(int cx, int cy, int s)
{
    int base = cy + s / 3;
    for (int i = 0; i < 4; i++) {
        int h = s / 5 + (i % 3) * (s / 6);
        fill_round_rect(cx - s / 3 + i * (s / 5), base - h, s / 7, h, 2, 0xFFFFFF);
    }
    for (int i = 0; i < 3; i++)
        draw_line(cx - s / 3 + i * (s / 5) + 2, base - s / 5 - i * (s / 8),
                  cx - s / 3 + (i + 1) * (s / 5) + 2, base - s / 4 - i * (s / 9), 0xE8FFF4, 1);
}
static void glyph_paint(int cx, int cy, int s)
{
    fill_circle(cx - 1, cy, s / 2 - 2, 0xFFFFFF);
    fill_circle(cx + s / 4, cy + s / 4, s / 5, 0xD6408C);
    draw_line(cx + s / 6, cy + s / 5, cx + s / 2 - 1, cy + s / 2 - 3, 0xC9A227, 4);
    fill_circle(cx - s / 5, cy - s / 5, 3, 0xFF5F57);
    fill_circle(cx + 1, cy - s / 4, 3, 0x4FA8FF);
    fill_circle(cx - s / 4, cy + 1, 3, 0xFFE066);
}
static void glyph_viewer(int cx, int cy, int s)
{
    int w = s * 2 / 3, h = s * 4 / 5;
    fill_round_rect(cx - w / 2, cy - h / 2 + 1, w, h, 3, 0xB9BEC9);
    fill_round_rect(cx - w / 2, cy - h / 2, w, h, 3, 0xFFFFFF);
    for (int i = 0; i < 4; i++)
        fill_rect(cx - w / 2 + 4, cy - h / 2 + 6 + i * (h / 6), w - 8, 2, 0x8A90A0);
    fill_round_rect(cx - w / 2, cy - h / 2, w / 3, h / 8, 2, COL_ACCENT);
}
static void glyph_about(int cx, int cy, int s)
{
    fill_circle(cx, cy, s / 2 - 1, 0xFFFFFF);
    draw_circle(cx, cy, s / 2 - 1, 0xD2D6DE, 1);
    fill_circle(cx, cy - s / 4, 4, 0x5A6070);
    fill_round_rect(cx - 3, cy - s / 10, 6, s / 2 - 2, 3, 0x5A6070);
}

static void draw_app_glyph(int index, int cx, int cy, int s)
{
    switch (index) {
    case 0: glyph_terminal(cx, cy, s); break;
    case 1: glyph_files(cx, cy, s);    break;
    case 2: glyph_clock(cx, cy, s);    break;
    case 3: glyph_calc(cx, cy, s);     break;
    case 4: glyph_snake(cx, cy, s);    break;
    case 5: glyph_mine(cx, cy, s);     break;
    case 6: glyph_music(cx, cy, s);    break;
    case 7: glyph_sysmon(cx, cy, s);   break;
    case 8: glyph_paint(cx, cy, s);    break;
    case 9: glyph_viewer(cx, cy, s);   break;
    default: glyph_about(cx, cy, s);   break;
    }
}

/* a macOS style squircle tile with a gradient, inner glow and shadow */
static void draw_app_tile(int index, int cx, int cy, int size, int focused)
{
    app_def_t *app = &g_apps[index];
    int x = cx - size / 2, y = cy - size / 2;
    int r = size / 4;

    fill_round_rect_blend(x + 1, y + 3, size, size, r, 0x8E97A6, 120);  /* soft shadow */
    fill_round_rect(x, y, size, size, r, app->accent);
    fill_gradient_v(x + 1, y + 1, size - 2, size * 3 / 5, 0xFFFFFF, app->accent);
    fill_rect_blend(x + 1, y + 1, size - 2, size * 3 / 5, 0xFFFFFF, 55);
    draw_round_rect(x, y, size, size, r, 0xFFFFFF);              /* rim highlight */

    draw_app_glyph(index, cx, cy, size * 3 / 5);

    if (focused) fill_circle(cx, y + size + 7, 3, 0x3C414B);
}

static int app_is_running(int index)
{
    for (window_t *w = g_windows; w; w = w->next)
        if (w->visible && w->app_id == index) return 1;
    return 0;
}

/* animated dock magnification (target 1024 = 1.0) */
static int dock_scale[APP_COUNT_MAX];
static int dock_bounce_app = -1;
static u32 dock_bounce_start;
static u8  dock_animating;

static int dock_icon_cx(int index)
{
    return dock_x() + 12 + DOCK_ICON / 2 + index * (DOCK_ICON + DOCK_GAP);
}
static int dock_icon_cy(void) { return dock_y() + DOCK_H / 2 - 4; }

static void draw_dock(void)
{
    int dx = dock_x(), dy = dock_y();
    /* translucent plate */
    fill_round_rect(dx + 2, dy + 3, dock_w(), DOCK_H, 18, 0xA8ADB8);
    fill_rect_blend(dx, dy, dock_w(), DOCK_H, 0xFFFFFF, 70);
    fill_round_rect(dx, dy, dock_w(), DOCK_H, 18, COL_DOCK);
    fill_rect_blend(dx, dy, dock_w(), DOCK_H, 0xF7F9FF, 90);
    draw_round_rect(dx, dy, dock_w(), DOCK_H, 18, 0xC2C7D2);

    int cy = dy + DOCK_H / 2 - 4;
    for (int i = 0; i < g_app_count; i++) {
        int cx = dock_icon_cx(i);
        int target = gui_mouse_over(cx - DOCK_ICON / 2 - 2, dy, DOCK_ICON + 4, DOCK_H) ? 1210 : 1024;
        int s = dock_scale[i] ? dock_scale[i] : 1024;
        if (s < target)      s += (target - s) / 3 + 1;
        else if (s > target) s -= (s - target) / 3 + 1;
        if (s != target) dock_animating = 1;
        dock_scale[i] = s;

        int size = DOCK_ICON * s / 1024;
        int icy = cy - (size - DOCK_ICON) * 2 / 3;

        if (i == dock_bounce_app) {
            u32 dt = timer_ms() - dock_bounce_start;
            if (dt < 460) {
                icy -= 12 * fsin_deg((int)(dt * 360 / 460)) / 1000;
                dock_animating = 1;
            } else {
                dock_bounce_app = -1;
            }
        }
        draw_app_tile(i, cx, icy, size, app_is_running(i));
    }
}

/* --------------------------------------------------------- composition --- */
static void draw_scene(int rx, int ry, int rw, int rh)
{
    fb_reset_clip();
    fb_copy_rect(fb_back, rx, ry, rw, rh, g_wallpaper, rx, ry);

    window_t *list[GUI_MAX_WINDOWS];
    int n = 0;
    for (window_t *it = g_windows; it && n < GUI_MAX_WINDOWS; it = it->next) list[n++] = it;

    fb_set_clip(rx, ry, rw, rh);
    for (int i = n - 1; i >= 0; i--) {
        window_t *win = list[i];
        if (!win->visible) continue;
        if (!rects_overlap(win->x - SHADOW, win->y - SHADOW, win->w + SHADOW * 2, win->h + SHADOW * 2,
                           rx, ry, rw, rh)) continue;
        draw_window(win);
    }
    draw_dock();
    draw_menubar();
    fb_reset_clip();
}

static void render(void)
{
    fb_set_target(fb_back);
    if (d_full || dr_count == 0) {
        draw_scene(0, 0, (int)fb_width, (int)fb_height);
        cursor_clear_for(0, 0, (int)fb_width, (int)fb_height);
        fb_present();
        perf_px += fb_width * fb_height;
    } else {
        for (int i = 0; i < dr_count; i++) {
            int x = dr[i].x, y = dr[i].y, w = dr[i].w, h = dr[i].h;
            draw_scene(x, y, w, h);
            cursor_clear_for(x, y, w, h);
            fb_copy_rect(fb_mem, x, y, w, h, fb_back, x, y);
            perf_px += (u32)w * (u32)h;
        }
    }
    d_full = 0;
    dr_count = 0;
    cursor_move_to(g_mouse_x, g_mouse_y);
    perf_frames++;
}

void gui_perf(int *frames, int *cursor_moves)
{
    if (frames) *frames = (int)perf_frames;
    if (cursor_moves) *cursor_moves = (int)perf_cursor;
    perf_frames = perf_cursor = perf_px = 0;
}

/* -------------------------------------------------------------- events --- */
void gui_open_app(int index)
{
    if (index < 0 || index >= g_app_count) return;
    g_pending_app = index;
    window_t *w = g_apps[index].open ? g_apps[index].open() : NULL;
    g_pending_app = -1;
    if (w) win_focus(w);
    /* bounce the dock icon (macOS launch feedback) */
    dock_bounce_app = index;
    dock_bounce_start = timer_ms();
    inval_dock();
}

static int dock_hit(int x, int y, int *index)
{
    if (y < dock_y() || y >= dock_y() + DOCK_H) return 0;
    int dx = dock_x();
    for (int i = 0; i < g_app_count; i++) {
        int cx = dx + 12 + DOCK_ICON / 2 + i * (DOCK_ICON + DOCK_GAP);
        if (x >= cx - DOCK_ICON / 2 - 3 && x < cx + DOCK_ICON / 2 + 3) { *index = i; return 1; }
    }
    return 2;                       /* clicked the plate itself */
}

static void mouse_press(int button)
{
    int idx;

    if (dock_hit(g_mouse_x, g_mouse_y, &idx)) {
        if (idx != 2) gui_open_app(idx);
        return;
    }

    window_t *hit = NULL;
    for (window_t *w = g_windows; w; w = w->next)
        if (w->visible && !w->dying && inside(w, g_mouse_x, g_mouse_y)) { hit = w; break; }

    if (!hit) return;

    win_focus(hit);

    if (button == 1 && g_mouse_y >= hit->y && g_mouse_y < hit->y + TITLEBAR_H) {
        int which;
        if (tl_hit(hit, g_mouse_x, g_mouse_y, &which)) {
            if (which == 0) {
                win_close(hit);                     /* animated close */
            } else if (which == 1) {
                an_minimise(hit);                   /* fly into the dock */
            } else {
                int ox = hit->x, oy = hit->y, ow = hit->w, oh = hit->h;
                win_toggle_zoom(hit);
                an_start(hit, 2, ox, oy, ow, oh, 220, 0);
            }
            return;
        }
        g_dragging = 1;
        g_drag_dx = g_mouse_x - hit->x;
        g_drag_dy = g_mouse_y - hit->y;
        return;
    }

    if (hit->event) hit->event(hit, EV_MOUSE_DOWN, g_mouse_x, g_mouse_y, button);
}

static void handle_mouse(mouse_event_t *me)
{
    mouse_get_pos(&g_mouse_x, &g_mouse_y);

    if (!me->is_button) {
        if (g_dragging && g_focused) {
            int ox = g_focused->x, oy = g_focused->y;
            int nx = g_mouse_x - g_drag_dx;
            int ny = g_mouse_y - g_drag_dy;
            if (nx < 0) nx = 0;
            if (ny < MENUBAR_H) ny = MENUBAR_H;
            if (nx + g_focused->w > (int)fb_width) nx = (int)fb_width - g_focused->w;
            if (ny + g_focused->h > dock_y() - 4) ny = dock_y() - 4 - g_focused->h;
            if (nx != ox || ny != oy) {
                g_focused->x = nx;
                g_focused->y = ny;
                inval(ox - SHADOW, oy - SHADOW, g_focused->w + SHADOW * 2, g_focused->h + SHADOW * 2);
                inval(nx - SHADOW, ny - SHADOW, g_focused->w + SHADOW * 2, g_focused->h + SHADOW * 2);
            }
        } else {
            /* hover feedback: dock magnification and traffic light glyphs */
            if (g_mouse_y >= dock_y() - 6) inval_dock();
            if (g_focused && g_focused->visible &&
                g_mouse_x >= g_focused->x && g_mouse_x < g_focused->x + 70 &&
                g_mouse_y >= g_focused->y && g_mouse_y < g_focused->y + TITLEBAR_H)
                inval_frame(g_focused);
            if (g_focused && g_focused->event && inside(g_focused, g_mouse_x, g_mouse_y))
                g_focused->event(g_focused, EV_MOUSE_MOVE, g_mouse_x, g_mouse_y, me->buttons);
        }
        return;
    }

    u8 b = me->buttons;
    int lb = b & 1, rb = (b >> 1) & 1;
    int lb_prev = g_btn_prev & 1, rb_prev = (g_btn_prev >> 1) & 1;
    g_btn_prev = b;

    if (lb && !lb_prev) mouse_press(1);
    if (rb && !rb_prev) mouse_press(2);
    if (!lb && lb_prev) {
        g_dragging = 0;
        if (g_focused && g_focused->event)
            g_focused->event(g_focused, EV_MOUSE_UP, g_mouse_x, g_mouse_y, 1);
        if (g_focused) inval_frame(g_focused);
    }
}

static void handle_key(key_event_t *ke)
{
    if (!ke->pressed) return;
    if (g_focused && g_focused->event)
        g_focused->event(g_focused, EV_KEY, ke->ch, ke->pressed, 0);
    if (g_focused) inval_content(g_focused);
}

/* ---------------------------------------------------------------- init --- */
static void build_wallpaper(void)
{
    int W = (int)fb_width, H = (int)fb_height;

    /* four stop vertical gradient (Ventura-ish) */
    int stops[4] = { (int)COL_WALL_0, (int)COL_WALL_1, (int)COL_WALL_2, (int)COL_WALL_3 };
    for (int y = 0; y < H; y++) {
        int seg = (y * 3) / H;
        if (seg > 2) seg = 2;
        u32 a = (u32)stops[seg], b = (u32)stops[seg + 1];
        int f = (y * 3) % H;
        int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
        int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
        u32 c = ((u32)(ar + (br - ar) * f / H) << 16) |
                ((u32)(ag + (bg - ag) * f / H) << 8) |
                (u32)(ab + (bb - ab) * f / H);
        fill_rect(0, y, W, 1, c);
    }

    /* flowing ribbons */
    for (int w = 0; w < 5; w++) {
        int amp = 60 + w * 26;
        u32 tint = (w & 1) ? 0xFFFFFF : 0x120826;
        for (int x = 0; x < W; x++) {
            /* fixed point wave (1/16 px) so the band edges can be anti-aliased */
            int y16 = (H / 2 + (w - 2) * H / 9) * 16
                      + fsin_deg(x * 100 / (W * (2 + w)) + w * 47) * amp * 16 / 1000;
            int th = 34 + w * 8;
            int y0 = y16 >> 4;
            int topf = y16 & 15;
            if (y0 < 0 || y0 + th + 1 >= H) continue;
            fill_rect_blend(x, y0, 1, 1, tint, (u32)(34 * (16 - topf) / 16));
            fill_rect_blend(x, y0 + 1, 1, th - 1, tint, 34);
            fill_rect_blend(x, y0 + th, 1, 1, tint, (u32)(34 * topf / 16));
        }
    }
    /* soft vignette at the top and bottom */
    for (int y = 0; y < 90; y++) fill_rect_blend(0, y, W, 1, 0x0A0C18, (u32)(70 - y / 2));
    for (int y = H - 110; y < H; y++) fill_rect_blend(0, y, W, 1, 0x0A0C18, (u32)(40 - (H - y) / 4));
}

void gui_init(void)
{
    fb_back = (u8 *)kmalloc(fb_pitch * fb_height);
    g_wallpaper = (u8 *)kmalloc(fb_pitch * fb_height);
    if (!fb_back || !g_wallpaper) {
        debug_write("gui: out of memory\n");
        return;
    }
    memset(g_wallpaper, 0, fb_pitch * fb_height);

    u8 *saved = fb_get_target();
    fb_set_target(g_wallpaper);
    fb_reset_clip();
    build_wallpaper();
    fb_set_target(saved);

    mouse_get_pos(&g_mouse_x, &g_mouse_y);
    d_full = 1;
    debug_write("gui: macOS style desktop ready\n");
}

void gui_run(void)
{
    u32 last_clock = timer_ms(), last_anim = timer_ms(), last_stat = timer_ms();
    int idle_spins = 0;

    for (;;) {
        int busy = 0;

        key_event_t ke;
        while (keyboard_get_event(&ke)) { handle_key(&ke); busy = 1; }

        mouse_event_t me;
        while (mouse_get_event(&me)) { handle_mouse(&me); busy = 1; }

        u32 now = timer_ms();

        /* transitions run at ~30 Hz, idle content ticks at 12.5 Hz */
        int busy_anim = 0;
        for (window_t *w = g_windows; w; w = w->next)
            if (w->an_kind) { busy_anim = 1; break; }
        u32 tick = (busy_anim || dock_animating || dock_bounce_app >= 0) ? 33 : 80;

        if (now - last_anim >= tick) {
            last_anim = now;
            an_advance();                    /* window transitions */
            dock_animating = 0;              /* re-armed by draw_dock */
            if (busy_anim || dock_bounce_app >= 0) inval_dock();
            for (window_t *w = g_windows; w; w = w->next)
                if (w->visible && w->anim == 2 && w == g_focused) inval_content(w);
        }
        if (now - last_clock >= 1000) {
            last_clock = now;
            inval(0, 0, (int)fb_width, MENUBAR_H);       /* the clock lives there */
            for (window_t *w = g_windows; w; w = w->next)
                if (w->visible && w->anim >= 1) inval_content(w);
        }

        if (d_full || dr_count > 0) { render(); busy = 1; }
        else if (g_mouse_x != cur_x || g_mouse_y != cur_y) {
            cursor_move_to(g_mouse_x, g_mouse_y);
            busy = 1;
        }

        if (now - last_stat >= 1000) {
            last_stat = now;
            if (perf_frames || perf_cursor)
                debug_printf("gui: %u frames  %u cursor  %u kpix/s\n",
                             perf_frames, perf_cursor, perf_px / 1000);
            gui_perf(NULL, NULL);
        }

        if (!busy) {
            /* spin a little so bursts of mouse packets are picked up without
               a halt/wake round trip, then sleep until the next interrupt */
            if (idle_spins < 3) {
                idle_spins++;
                for (volatile int i = 0; i < 3000; i++) ;
            } else {
                cpu_halt();
            }
        } else {
            idle_spins = 0;
        }
    }
}