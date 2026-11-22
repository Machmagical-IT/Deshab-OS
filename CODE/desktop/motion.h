/* motion.h — 桌面动效轨道（16.16 定点，无浮点、无 SSE）
 *
 * 弹簧：半隐式欧拉。stiffness 是每 16ms 的误差增益（误差 >> 8 后乘它）。
 * damping 是每 16ms 保留的速度，0..256，缺省 160（约 0.625）。
 * dt 会按 16ms 归一，1ms 与 16ms 步长走同一条曲线。
 *
 * ease-out：三次曲线 1-(1-t)^3，t 用 0..256 整数。
 * 位移进入 1px 且速度归零后 active=0，value 吸附到 target。
 *
 * 绘制仍读窗口逻辑矩形。UI 要跟手动效时读 win_present_rect。
 */
#ifndef DESHAB_MOTION_H
#define DESHAB_MOTION_H

#define MOTION_ONE   65536
#define MOTION_STIFF 26
#define MOTION_KEEP  160

#define MOTION_MODE_IDLE   0
#define MOTION_MODE_SPRING 1
#define MOTION_MODE_EASE   2

typedef struct motion_track {
    int active;
    int mode;
    int value;          /* 16.16 */
    int target;
    int vel;
    int stiffness;
    int damping;        /* 每 16ms 速度保留量，0..256 */
    int ease_elapsed;   /* ms */
    int ease_dur_ms;
    int ease_from;
} motion_track;

void motion_bind(motion_track *t, int px);
void motion_spring_to(motion_track *t, int px);
void motion_ease_to(motion_track *t, int px, int duration_ms);
int  motion_px(const motion_track *t);

/* 推进全部窗口呈现轨道。本帧有轨道处于活动（含本帧刚吸附）返回 1。 */
int  motion_tick(int dt_ms);

/* 推进单条非窗口轨道（Dock / Launchpad 用）。步前处于活动返回 1。 */
int  motion_step(motion_track *t, int dt_ms);

/* dev：把一条虚拟轨道从 0 弹簧到 100，串口打印 [motion] settled。 */
void motion_selftest(void);

#endif /* DESHAB_MOTION_H */
