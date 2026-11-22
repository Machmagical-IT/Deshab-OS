/* ===========================================================================
 *  Nutro+OS - games: Snakes and Minesweeper
 * ===========================================================================
 */
#include "kernel.h"

static u32 rnd_seed = 0x1234567u;

static u32 rnd(void)
{
    rnd_seed = rnd_seed * 1103515245u + 12345u;
    return (rnd_seed >> 16) & 0x7FFF;
}

/* ================================================================ Snake == */
#define SN_W 26
#define SN_H 17
#define SN_CELL 14
#define SN_MAX (SN_W * SN_H)

typedef struct {
    window_t *win;
    s16 bx[SN_MAX], by[SN_MAX];
    int len, dir, next_dir;
    int food_x, food_y;
    int alive, grow, score, best;
    u32 last_step;
    int started;
} snake_t;

static snake_t g_snake;

static void snake_place_food(snake_t *s)
{
    for (int guard = 0; guard < 500; guard++) {
        int x = (int)(rnd() % SN_W);
        int y = (int)(rnd() % SN_H);
        int busy = 0;
        for (int i = 0; i < s->len; i++)
            if (s->bx[i] == x && s->by[i] == y) { busy = 1; break; }
        if (!busy) { s->food_x = x; s->food_y = y; return; }
    }
}

static void snake_reset(snake_t *s)
{
    s->len = 4;
    s->dir = 1;
    s->next_dir = 1;
    s->alive = 1;
    s->grow = 0;
    s->score = 0;
    s->last_step = timer_ms();
    s->started = 1;
    for (int i = 0; i < s->len; i++) {
        s->bx[i] = (s16)(8 - i);
        s->by[i] = 8;
    }
    snake_place_food(s);
}

static void snake_step(snake_t *s)
{
    if (!s->alive) return;
    s->dir = s->next_dir;

    int nx = s->bx[0], ny = s->by[0];
    switch (s->dir) {
    case 0: ny--; break;               /* up */
    case 1: nx++; break;               /* right */
    case 2: ny++; break;               /* down */
    case 3: nx--; break;               /* left */
    }

    if (nx < 0 || ny < 0 || nx >= SN_W || ny >= SN_H) { s->alive = 0; return; }
    for (int i = 0; i < s->len - 1; i++)
        if (s->bx[i] == nx && s->by[i] == ny) { s->alive = 0; return; }

    if (nx == s->food_x && ny == s->food_y) {
        s->grow = 1;
        s->score += 10;
        if (s->score > s->best) s->best = s->score;
        snake_place_food(s);
    }

    int last = s->len - 1;
    if (!s->grow) {
        for (int i = last; i > 0; i--) { s->bx[i] = s->bx[i - 1]; s->by[i] = s->by[i - 1]; }
    } else {
        if (s->len < SN_MAX - 1) {
            s->len++;
            last = s->len - 1;
            for (int i = last; i > 0; i--) { s->bx[i] = s->bx[i - 1]; s->by[i] = s->by[i - 1]; }
        }
        s->grow = 0;
    }
    s->bx[0] = (s16)nx;
    s->by[0] = (s16)ny;
}

static void snake_draw(window_t *w)
{
    snake_t *s = (snake_t *)w->user;
    if (!s->started) snake_reset(s);

    /* advance the game from the wall clock - the GUI only repaints.
   The snake pauses while the window is not focused. */
    int focused = (win_focused() == w);
    u32 now = timer_ms();
    if (focused) {
        while (s->alive && now - s->last_step >= 130) {
            s->last_step += 130;
            snake_step(s);
        }
    }
    if (now - s->last_step > 1000) s->last_step = now;

    int cx = win_content_x(w), cy = win_content_y(w);
    int wd = w->w, ht = w->h - TITLEBAR_H;

    fill_rect(cx, cy, wd, ht, COL_WIN_CONTENT);

    char buf[64];
    ksnprintf(buf, sizeof(buf), "score %d    best %d    len %d%s",
              s->score, s->best, s->len, focused ? "" : "   (paused)");
    draw_text_bold(cx + 16, cy + 8, buf, COL_ACCENT_GREEN);
    draw_text(cx + 16, cy + 26, "arrow keys steer", COL_TEXT_DIM);
    ui_button(cx + wd - 96, cy + 4, 84, 24, "New game", UI_TINTED);

    int bx = cx + 12, by = cy + 48;
    fill_round_rect(bx - 4, by - 4, SN_W * SN_CELL + 8, SN_H * SN_CELL + 8, 6, 0x0A1018);

    /* board chequer */
    for (int y = 0; y < SN_H; y++)
        for (int x = 0; x < SN_W; x++)
            if ((x + y) & 1)
                fill_rect(bx + x * SN_CELL, by + y * SN_CELL, SN_CELL, SN_CELL, 0x101823);

    /* food */
    fill_circle(bx + s->food_x * SN_CELL + SN_CELL / 2,
                by + s->food_y * SN_CELL + SN_CELL / 2, 5, COL_CLOSE);

    /* body: head bright, tail fades */
    for (int i = s->len - 1; i >= 0; i--) {
        int px = bx + s->bx[i] * SN_CELL;
        int py = by + s->by[i] * SN_CELL;
        u32 col;
        if (i == 0) col = 0xE8FFE8;
        else {
            int f = 200 - (i * 120 / (s->len > 1 ? s->len : 2));
            col = ((u32)(40 + f / 4) << 16) | ((u32)(120 + f) << 8) | 0x60;
        }
        fill_round_rect(px + 1, py + 1, SN_CELL - 2, SN_CELL - 2, 4, col);
    }

    if (!s->alive) {
        fill_rect_blend(bx, by + SN_H * SN_CELL / 2 - 26, SN_W * SN_CELL, 52, 0x000000, 190);
        const char *msg = "GAME OVER";
        int tw = (int)strlen(msg) * 8 * 2;
        draw_text_scaled(bx + (SN_W * SN_CELL - tw) / 2, by + SN_H * SN_CELL / 2 - 18, msg, COL_CLOSE, 2);
        const char *hint = "press ENTER";
        tw = (int)strlen(hint) * 8;
        draw_text(bx + (SN_W * SN_CELL - tw) / 2, by + SN_H * SN_CELL / 2 + 10, hint, COL_TEXT);
    }
}

static int snake_event(window_t *w, int type, int a, int b, int c)
{
    snake_t *s = (snake_t *)w->user;
    UNUSED(b); UNUSED(c);

    if (type == EV_MOUSE_DOWN) { win_focus(w); return 1; }
    if (type != EV_KEY) return 0;

    if (type == EV_MOUSE_DOWN) {
        if (ui_hit(win_content_x(w) + w->w - 100, win_content_y(w) + 0, 92, 30)) {
            snake_reset(s);
            gui_invalidate();
            return 1;
        }
    }
    u8 ch = (u8)a;
    if (ch == KEY_UP && s->dir != 2) s->next_dir = 0;
    else if (ch == KEY_RIGHT && s->dir != 3) s->next_dir = 1;
    else if (ch == KEY_DOWN && s->dir != 0) s->next_dir = 2;
    else if (ch == KEY_LEFT && s->dir != 1) s->next_dir = 3;
    else if (ch == '\n' || ch == ' ') snake_reset(s);
    else return 0;
    gui_invalidate();
    return 1;
}

window_t *app_snake_open(void)
{
    snake_t *s = &g_snake;
    if (s->win && s->win->visible) { win_focus(s->win); return s->win; }

    memset(s, 0, sizeof(*s));
    s->started = 0;
    snake_reset(s);
    if (!rnd_seed) rnd_seed = timer_ticks() + 1;

    window_t *w = win_create("Snakes", 520, 120, SN_W * SN_CELL + 40, SN_H * SN_CELL + 96,
                             snake_draw, snake_event, s);
    if (w) w->anim = 2;                 /* 20 Hz repaint */
    s->win = w;
    return w;
}

/* ========================================================== Minesweeper == */
#define MG_W 9
#define MG_H 9
#define MG_MINES 10
#define MG_CELL 26

typedef struct {
    window_t *win;
    u8 mine[MG_H][MG_W];
    u8 open[MG_H][MG_W];
    u8 flag[MG_H][MG_W];
    int started, over, won, revealed, flags;
    u32 t0;
} mines_t;

static mines_t g_mines;

static void mines_reset(mines_t *m)
{
    memset(m->mine, 0, sizeof(m->mine));
    memset(m->open, 0, sizeof(m->open));
    memset(m->flag, 0, sizeof(m->flag));
    m->started = 0;
    m->over = 0;
    m->won = 0;
    m->revealed = 0;
    m->flags = 0;
    m->t0 = 0;
}

static int mines_neighbours(mines_t *m, int x, int y)
{
    int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= MG_W || ny >= MG_H) continue;
            if (m->mine[ny][nx]) n++;
        }
    return n;
}

static void mines_place(mines_t *m, int sx, int sy)
{
    int placed = 0;
    while (placed < MG_MINES) {
        int x = (int)(rnd() % MG_W);
        int y = (int)(rnd() % MG_H);
        if (m->mine[y][x]) continue;
        if (x >= sx - 1 && x <= sx + 1 && y >= sy - 1 && y <= sy + 1) continue;
        m->mine[y][x] = 1;
        placed++;
    }
}

static void mines_reveal(mines_t *m, int x, int y)
{
    static s16 stack_x[MG_W * MG_H], stack_y[MG_W * MG_H];
    int sp = 0;
    stack_x[sp] = (s16)x;
    stack_y[sp] = (s16)y;
    sp++;

    while (sp > 0) {
        sp--;
        int px = stack_x[sp], py = stack_y[sp];
        if (px < 0 || py < 0 || px >= MG_W || py >= MG_H) continue;
        if (m->open[py][px] || m->flag[py][px]) continue;
        m->open[py][px] = 1;
        m->revealed++;
        if (m->mine[py][px]) { m->over = 1; return; }
        if (mines_neighbours(m, px, py) == 0) {
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    if (sp < MG_W * MG_H) {
                        stack_x[sp] = (s16)(px + dx);
                        stack_y[sp] = (s16)(py + dy);
                        sp++;
                    }
                }
        }
    }
}

static void mines_check_win(mines_t *m)
{
    if (m->revealed >= MG_W * MG_H - MG_MINES && !m->over) m->won = 1;
}

static void mines_cell_rect(mines_t *m, int x, int y, int *px, int *py)
{
    UNUSED(m);
    int cx = 0, cy = 0;
    window_t *w = g_mines.win;
    if (w) { cx = win_content_x(w); cy = win_content_y(w); }
    *px = cx + 16 + x * MG_CELL;
    *py = cy + 64 + y * MG_CELL;
}

static void mines_draw(window_t *w)
{
    mines_t *m = (mines_t *)w->user;
    int cx = win_content_x(w), cy = win_content_y(w);
    int wd = w->w, ht = w->h - TITLEBAR_H;

    fill_rect(cx, cy, wd, ht, COL_WIN_CONTENT);

    char buf[64];
    int flag_count = 0;
    for (int y = 0; y < MG_H; y++)
        for (int x = 0; x < MG_W; x++) if (m->flag[y][x]) flag_count++;
    m->flags = flag_count;

    ksnprintf(buf, sizeof(buf), "mines %d   flags %d   time %u s",
              MG_MINES, flag_count, m->started ? (timer_ms() - m->t0) / 1000 : 0);
    draw_text_bold(cx + 16, cy + 8, buf, COL_ACCENT_ORANGE);
    draw_text(cx + 16, cy + 26, "L-click dig   R-click flag", COL_TEXT_DIM);
    ui_button(cx + wd - 76, cy + 4, 64, 22, "New", UI_TINTED);

    for (int y = 0; y < MG_H; y++) {
        for (int x = 0; x < MG_W; x++) {
            int px, py;
            mines_cell_rect(m, x, y, &px, &py);
            int border = MG_CELL - 2;

            if (m->open[y][x]) {
                fill_rect(px, py, border, border, 0xE8EBF2);
                if (m->mine[y][x]) {
                    fill_circle(px + border / 2, py + border / 2, 7, COL_CLOSE);
                } else {
                    int n = mines_neighbours(m, x, y);
                    if (n) {
                        static const u32 numcol[9] = {
                            0, 0x6FB7FF, 0x54D98C, 0xFF6FA5, 0xB48CFF,
                            0xFFB454, 0x4FE3D0, 0x555A66, 0x9AA4B8
                        };
                        char nb[2] = { (char)('0' + n), 0 };
                        draw_text(px + border / 2 - 4, py + 5, nb, numcol[n]);
                    }
                }
            } else {
                fill_round_rect(px, py, border, border, 5, 0xD6DAE4);
                fill_round_rect(px, py, border, border - 4, 5, 0xEEF1F7);
                if (m->flag[y][x]) {
                    draw_line(px + border / 2, py + 5, px + border / 2, py + border - 5, 0x555A66, 2);
                    fill_rect(px + border / 2, py + 5, 7, 6, COL_CLOSE);
                }
                if (m->over && m->mine[y][x] && !m->flag[y][x])
                    fill_circle(px + border / 2, py + border / 2, 6, 0xFFC9C4);
            }
        }
    }

    if (m->over || m->won) {
        fill_rect_blend(cx + 10, cy + 120, wd - 20, 56, 0x000000, 200);
        const char *msg = m->won ? "YOU WIN!" : "BOOM - GAME OVER";
        int tw = (int)strlen(msg) * 8 * 2;
        draw_text_scaled(cx + (wd - tw) / 2, cy + 128, msg, m->won ? COL_ACCENT_GREEN : COL_CLOSE, 2);
        const char *hint = "press ENTER to play again";
        tw = (int)strlen(hint) * 8;
        draw_text(cx + (wd - tw) / 2, cy + 154, hint, COL_TEXT);
    }
}

static int mines_event(window_t *w, int type, int a, int b, int c)
{
    mines_t *m = (mines_t *)w->user;

    if (type == EV_KEY) {
        if ((u8)a == '\n') { mines_reset(m); gui_invalidate(); return 1; }
        return 0;
    }
    if (type != EV_MOUSE_DOWN) return 0;

    int cx = win_content_x(w), cy = win_content_y(w);
    if (ui_hit(cx + w->w - 78, cy + 2, 68, 26)) { mines_reset(m); gui_invalidate(); return 1; }
    int gx = (a - cx - 16) / MG_CELL;
    int gy = (b - cy - 64) / MG_CELL;
    if (gx < 0 || gy < 0 || gx >= MG_W || gy >= MG_H) return 1;
    if (m->over || m->won) return 1;

    if (!m->started) {
        m->started = 1;
        m->t0 = timer_ms();
        mines_place(m, gx, gy);
    }

    if (c == 2) {                       /* right button: flag */
        if (!m->open[gy][gx]) m->flag[gy][gx] = (u8)!m->flag[gy][gx];
    } else {                            /* left button: dig */
        if (m->flag[gy][gx]) return 1;
        mines_reveal(m, gx, gy);
        mines_check_win(m);
    }
    gui_invalidate();
    return 1;
}

window_t *app_mines_open(void)
{
    mines_t *m = &g_mines;
    if (m->win && m->win->visible) { win_focus(m->win); return m->win; }

    mines_reset(m);
    if (!rnd_seed) rnd_seed = timer_ticks() + 7;

    window_t *w = win_create("Minesweeper", 260, 130, MG_W * MG_CELL + 32, MG_H * MG_CELL + 96,
                             mines_draw, mines_event, m);
    if (w) w->anim = 1;                 /* keep the timer ticking */
    m->win = w;
    return w;
}