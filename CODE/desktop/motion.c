/* motion.c — 16.16 弹簧 / ease-out。窗口轨道由 motion_tick 遍历。 */
#include "desktop.h"

static int iabs_i(int v) { return v < 0 ? -v : v; }

void motion_bind(motion_track *t, int px) {
    if (!t) return;
    t->active = 0;
    t->mode = MOTION_MODE_IDLE;
    t->value = px * MOTION_ONE;
    t->target = t->value;
    t->vel = 0;
    t->stiffness = MOTION_STIFF;
    t->damping = MOTION_KEEP;
    t->ease_elapsed = 0;
    t->ease_dur_ms = 0;
    t->ease_from = t->value;
}

void motion_spring_to(motion_track *t, int px) {
    int tgt;
    if (!t) return;
    if (t->stiffness <= 0) t->stiffness = MOTION_STIFF;
    if (t->damping <= 0 || t->damping > 256) t->damping = MOTION_KEEP;
    tgt = px * MOTION_ONE;
    t->target = tgt;
    t->mode = MOTION_MODE_SPRING;
    if (t->value == tgt && t->vel == 0) {
        t->active = 0;
        t->mode = MOTION_MODE_IDLE;
        return;
    }
    t->active = 1;
}

void motion_ease_to(motion_track *t, int px, int duration_ms) {
    int tgt;
    if (!t) return;
    tgt = px * MOTION_ONE;
    t->target = tgt;
    if (duration_ms <= 0 || t->value == tgt) {
        t->value = tgt;
        t->vel = 0;
        t->active = 0;
        t->mode = MOTION_MODE_IDLE;
        return;
    }
    t->mode = MOTION_MODE_EASE;
    t->active = 1;
    t->ease_from = t->value;
    t->ease_elapsed = 0;
    t->ease_dur_ms = duration_ms;
    t->vel = 0;
}

int motion_px(const motion_track *t) {
    if (!t) return 0;
    return (t->value + MOTION_ONE / 2) / MOTION_ONE;
}

static void step_spring(motion_track *t, int dt) {
    long long vel = t->vel;
    long long err = (long long)t->target - t->value;
    int loss;
    vel += ((err * t->stiffness) >> 8) * dt / 16;
    loss = (256 - t->damping) * dt / 16;
    if (loss < 0) loss = 0;
    if (loss > 255) loss = 255;
    vel = (vel * (256 - loss)) >> 8;
    t->vel = (int)vel;
    t->value = (int)((long long)t->value + vel * dt / 16);
    if (iabs_i(t->target - t->value) < MOTION_ONE && iabs_i(t->vel) < MOTION_ONE) {
        t->value = t->target;
        t->vel = 0;
        t->active = 0;
        t->mode = MOTION_MODE_IDLE;
    }
}

static void step_ease(motion_track *t, int dt) {
    int te, u, u3, eased;
    long long delta;
    t->ease_elapsed += dt;
    if (t->ease_elapsed >= t->ease_dur_ms) {
        t->value = t->target;
        t->vel = 0;
        t->active = 0;
        t->mode = MOTION_MODE_IDLE;
        return;
    }
    te = (int)((long long)t->ease_elapsed * 256 / t->ease_dur_ms);
    if (te < 0) te = 0;
    if (te > 256) te = 256;
    u = 256 - te;
    u3 = (int)(((long long)u * u * u) >> 16);
    eased = 256 - u3;
    if (eased < 0) eased = 0;
    if (eased > 256) eased = 256;
    delta = (long long)t->target - t->ease_from;
    t->value = t->ease_from + (int)(delta * eased / 256);
}

static void step_track(motion_track *t, int dt) {
    if (!t->active) return;
    if (t->mode == MOTION_MODE_SPRING) step_spring(t, dt);
    else if (t->mode == MOTION_MODE_EASE) step_ease(t, dt);
    else t->active = 0;
}

int motion_step(motion_track *t, int dt_ms) {
    int was;
    if (!t) return 0;
    if (dt_ms < 1) dt_ms = 1;
    if (dt_ms > 32) dt_ms = 32;
    was = t->active;
    step_track(t, dt_ms);
    return was;
}

int motion_tick(int dt_ms) {
    int moved = 0;
    int i;
    if (dt_ms < 1) dt_ms = 1;
    if (dt_ms > 32) dt_ms = 32;
    for (i = 0; i < g_win_count; i++) {
        desktop_window *w = &g_windows[i];
        if (w->present_x.active) moved = 1;
        if (w->present_y.active) moved = 1;
        if (w->present_w.active) moved = 1;
        if (w->present_h.active) moved = 1;
        if (w->present_opacity.active) moved = 1;
        step_track(&w->present_x, dt_ms);
        step_track(&w->present_y, dt_ms);
        step_track(&w->present_w, dt_ms);
        step_track(&w->present_h, dt_ms);
        step_track(&w->present_opacity, dt_ms);
    }
    return moved;
}

void motion_selftest(void) {
    motion_track t;
    int i;
    motion_bind(&t, 0);
    motion_spring_to(&t, 100);
    for (i = 0; i < 400 && t.active; i++) step_track(&t, 16);
    if (!t.active && motion_px(&t) == 100)
        slog("[motion] settled");
    else
        slog("[motion] settle failed");
}
