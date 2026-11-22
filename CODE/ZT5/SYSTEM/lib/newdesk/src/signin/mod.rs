// =============================================================================
// signin 模块 - eDEXUI 风格登录界面
// =============================================================================
// 设计:
//   - 深色背景 (BG_DARK) + 青色/白色发光线绘元素
//   - 屏幕四周发光边框
//   - 中央圆角矩形登录框（发光效果）
//   - 底部圆角任务栏
//   - 启动时所有元素逐像素绘制，产生流动效果
//   - 鼠标光标支持
//   - 登录成功后淡入淡出过渡到桌面
// =============================================================================

use crate::init::FbInfo;
use crate::ui::{
    Cursor, DrawContext,
    BG_DARK, BG_PANEL, CYAN, DIM, GREEN, GLOW_CYAN, RED, TEXT, TEXT_DIM, WHITE,
    scancode_to_ascii, syscall2, buf_len, buf_eq,
    ParticleSystem, Color,
};

/// 系统调用号
const SYS_INPUT_POLL: u64 = 188;

/// 键盘事件类型 (与内核 sys_input_poll 一致)
const EVT_KEY_DOWN: u32 = 1;    // (1<<16)|keycode
/// 鼠标事件类型
const EVT_MOUSE_X: u32 = 3;     // (3<<16)|x_coord
const EVT_MOUSE_Y: u32 = 4;     // (4<<16)|y_coord
const EVT_MOUSE_LBTN: u32 = 5;  // (5<<16)|0or1

/// 登录结果
#[derive(Clone, Copy, PartialEq)]
pub enum SignInResult {
    /// 仍在登录中
    Pending,
    /// 登录成功
    Success,
}

/// 登录成功后返回的登录信息
pub struct SignInInfo {
    /// 用户是否为 root
    pub is_root: bool,
}

/// 启动登录界面
///
/// eDEXUI 风格启动序列:
/// 1. 深色背景
/// 2. 屏幕边框逐像素生长（发光效果）
/// 3. 系统信息文字打字机效果
/// 4. 登录框圆角矩形逐像素生长（发光效果）
/// 5. 底部任务栏逐像素生长
/// 6. 等待用户输入用户名和密码
/// 7. 验证密码，成功则淡出过渡到桌面
pub fn start(_username: &[u8], password_hash: &[u8], fb_info: &FbInfo, ctx: &mut DrawContext, cursor: &mut Cursor) -> (SignInResult, SignInInfo) {
    let screen_w = fb_info.width as i32;
    let screen_h = fb_info.height as i32;

    // ── 第一阶段：深色背景 ──
    ctx.clear(&BG_DARK);
    ctx.flip_full();

    // ── 第二阶段：屏幕边框逐像素生长 (发光效果) ──
    ctx.draw_screen_border_animated_glow(&WHITE, &GLOW_CYAN);

    // ── 第三阶段：系统信息打字机效果 ──
    let info_x = 12;
    let info_y = 12;
    ctx.draw_text_animated(info_x, info_y, b"== ZT5", &CYAN);
    ctx.draw_text_animated(info_x, info_y + 20, b"v1.5.0 [kernel: Ztknl]", &TEXT_DIM);
    ctx.draw_text_animated(info_x, info_y + 40, b"arch: x86_64", &TEXT_DIM);

    // 右上角状态
    let status_x = screen_w - 12 - 18 * 8;
    ctx.draw_text(status_x, info_y, b"SECURE BOOT [ OK ]", &GREEN);
    ctx.flip();
    DrawContext::delay_ms(200);

    // 虚线分割线
    ctx.draw_hline_dashed(12, screen_w - 12, info_y + 60, 6, 4, &DIM);
    ctx.flip();
    DrawContext::delay_ms(300);

    // ── 第四阶段：登录框 (发光效果) ──
    let box_w: i32 = 440;
    let box_h: i32 = 280;
    let box_x = screen_w / 2 - box_w / 2;
    let box_y = screen_h / 2 - box_h / 2 - 20;

    // 标题
    ctx.draw_text_animated(box_x + 10, box_y - 22, b"SIGN IN", &CYAN);

    // 发光圆角矩形
    ctx.draw_rounded_rect_animated_glow(box_x, box_y, box_w, box_h, 8, &CYAN, &GLOW_CYAN);

    // 内部分割线
    ctx.draw_hline_dashed(box_x + 10, box_x + box_w - 10, box_y + 40, 4, 4, &DIM);
    ctx.flip();
    DrawContext::delay_ms(200);

    // ── 第五阶段：底部任务栏 (发光效果) ──
    let bar_margin = 8;
    let bar_h = 36;
    let bar_y = screen_h - bar_margin - bar_h;
    let bar_w = screen_w - bar_margin * 2;
    ctx.draw_rounded_rect_animated_glow(bar_margin, bar_y, bar_w, bar_h, 6, &CYAN, &GLOW_CYAN);

    // 任务栏文字
    ctx.draw_text(20, bar_y + 4, b"ZT5", &CYAN);
    ctx.draw_text(60, bar_y + 4, b"|", &DIM);
    ctx.draw_text(72, bar_y + 4, b"DESKTOP", &TEXT_DIM);
    ctx.flip();
    DrawContext::delay_ms(300);

    // ── 第六阶段：登录框内提示文字 ──
    let label_x = box_x + 20;
    let input_x = box_x + 64;
    ctx.draw_text(label_x, box_y + 55, b"user>", &TEXT);
    ctx.flip();

    // ── 启用鼠标光标 ──
    cursor.visible = true;
    cursor.draw(ctx);
    ctx.flip();

    // ── 登录循环 ──
    let mut input_username = [0u8; 256];
    let mut username_len: usize = 0;
    let mut input_password = [0u8; 256];
    let mut password_len: usize = 0;
    let mut in_password = false;
    let mut username_cursor_x = input_x;
    let mut password_cursor_x = input_x;
    let mut events = [0u32; 32];

    // 粒子系统 (背景流动)
    let mut particles = ParticleSystem::new();
    let mut anim_tick: u64 = 0;
    let mut last_input_time: u64 = 0;

    loop {
        anim_tick += 16;

        // 生成背景粒子
        particles.spawn_ambient(screen_w, screen_h, anim_tick);
        particles.update(16);

        // 每帧重绘登录框 (呼吸效果需要持续更新)
        cursor.restore(ctx);
        // 重绘登录框背景
        ctx.fill_rect(box_x + 1, box_y + 1, box_w - 2, box_h - 2, &BG_PANEL);
        // 呼吸发光边框
        ctx.draw_pulse_rect_glow(box_x, box_y, box_w, box_h, 8, &CYAN, &GLOW_CYAN, anim_tick, 2000);

        // 重绘登录框内容
        ctx.draw_hline_dashed(box_x + 10, box_x + box_w - 10, box_y + 40, 4, 4, &DIM);
        ctx.draw_text(label_x, box_y + 55, b"user>", &TEXT);
        if in_password {
            ctx.draw_text(label_x, box_y + 95, b"pass>", &TEXT);
        }
        // 重绘已输入的用户名
        if username_len > 0 {
            ctx.draw_text(input_x, box_y + 55, &input_username[..username_len], &TEXT);
        }
        // 重绘已输入的密码 (星号)
        if password_len > 0 {
            for i in 0..password_len {
                ctx.draw_text(input_x + i as i32 * 8, box_y + 95, b"*", &CYAN);
            }
        }

        // 绘制粒子
        particles.draw(ctx);

        // 输入框光标闪烁 (聚焦的输入框)
        let blink = (anim_tick / 500) % 2 == 0;
        if blink {
            let cursor_blink_x = if in_password { password_cursor_x } else { username_cursor_x };
            let cursor_blink_y = if in_password { box_y + 95 } else { box_y + 55 };
            // 聚焦输入框高亮
            let focus_color = if anim_tick - last_input_time < 300 { &WHITE } else { &CYAN };
            ctx.draw_text(cursor_blink_x, cursor_blink_y, b"_", focus_color);
        }

        cursor.draw(ctx);
        ctx.flip_full();

        let count = unsafe {
            syscall2(SYS_INPUT_POLL, events.as_mut_ptr() as u64, events.len() as u64)
        };

        if count > 0 {
            for i in 0..count as usize {
                let evt = events[i];
                let evt_type = (evt >> 16) & 0xFFFF;
                let evt_data = evt & 0xFFFF;

                if evt_type == EVT_KEY_DOWN {
                    let ch = scancode_to_ascii(evt_data as u8);
                    last_input_time = anim_tick;

                    if !in_password {
                        // 用户名输入
                        match ch {
                            b'\n' => {
                                if username_len > 0 {
                                    in_password = true;
                                    cursor.restore(ctx);
                                    ctx.draw_text(label_x, box_y + 95, b"pass>", &TEXT);
                                    cursor.draw(ctx);
                                    ctx.flip();
                                }
                            }
                            b'\x08' => {
                                if username_len > 0 {
                                    username_len -= 1;
                                    input_username[username_len] = 0;
                                    username_cursor_x -= 8;
                                }
                            }
                            c if c >= 0x20 && c < 0x7F => {
                                if username_len < 255 {
                                    input_username[username_len] = c;
                                    username_len += 1;
                                    username_cursor_x += 8;
                                }
                            }
                            _ => {}
                        }
                    } else {
                        // 密码输入
                        match ch {
                            b'\n' => {
                                // 验证密码
                                if verify_password(&input_password, password_hash)
                                    && !is_empty(&input_username)
                                {
                                    // 登录成功 - 粒子爆发效果
                                    cursor.restore(ctx);
                                    for _ in 0..30 {
                                        particles.spawn(
                                            (box_x + box_w / 2) as f32,
                                            (box_y + box_h / 2) as f32,
                                            ((anim_tick as i32 % 100) as f32 - 50.0) * 2.0,
                                            ((anim_tick as i32 % 80) as f32 - 40.0) * 2.0,
                                            1.5,
                                            3,
                                            GREEN,
                                        );
                                    }
                                    ctx.draw_text(box_x + 20, box_y + box_h - 35, b"ACCESS GRANTED", &GREEN);
                                    particles.draw(ctx);
                                    cursor.draw(ctx);
                                    ctx.flip();
                                    DrawContext::delay_ms(1000);

                                    // 登录成功 - 直接清屏切换到桌面
                                    cursor.visible = false;
                                    ctx.clear(&BG_DARK);
                                    ctx.flip_full();

                                    // 根据用户输入的用户名判断是否为 root
                                    let is_root = username_len == 4
                                        && input_username[0] == b'r'
                                        && input_username[1] == b'o'
                                        && input_username[2] == b'o'
                                        && input_username[3] == b't';

                                    return (SignInResult::Success, SignInInfo { is_root });
                                } else {
                                    // 登录失败 - 红色粒子爆发
                                    cursor.restore(ctx);
                                    for _ in 0..20 {
                                        particles.spawn(
                                            (box_x + box_w / 2) as f32,
                                            (box_y + box_h / 2) as f32,
                                            ((anim_tick as i32 % 100) as f32 - 50.0) * 2.0,
                                            ((anim_tick as i32 % 80) as f32 - 40.0) * 2.0,
                                            1.0,
                                            2,
                                            RED,
                                        );
                                    }
                                    ctx.draw_text(box_x + 20, box_y + box_h - 35, b"ACCESS DENIED", &RED);
                                    particles.draw(ctx);
                                    cursor.draw(ctx);
                                    ctx.flip();
                                    DrawContext::delay_ms(800);

                                    // 抖动动画
                                    shake_animation(ctx, box_x, box_y, box_w, box_h, cursor);

                                    // 清除错误信息和输入区域
                                    cursor.restore(ctx);
                                    ctx.fill_rect(box_x + 20, box_y + box_h - 35, box_w - 40, 16, &BG_DARK);
                                    ctx.fill_rect(label_x, box_y + 55, box_w - 40, 60, &BG_DARK);

                                    // 重置输入
                                    input_username = [0u8; 256];
                                    username_len = 0;
                                    input_password = [0u8; 256];
                                    password_len = 0;
                                    in_password = false;
                                    username_cursor_x = input_x;
                                    password_cursor_x = input_x;

                                    cursor.draw(ctx);
                                    ctx.flip();
                                }
                            }
                            b'\x08' => {
                                if password_len > 0 {
                                    password_len -= 1;
                                    input_password[password_len] = 0;
                                    password_cursor_x -= 8;
                                }
                            }
                            c if c >= 0x20 && c < 0x7F => {
                                if password_len < 255 {
                                    input_password[password_len] = c;
                                    password_len += 1;
                                    password_cursor_x += 8;
                                }
                            }
                            _ => {}
                        }
                    }
                } else if evt_type == EVT_MOUSE_X {
                    // 鼠标 X 坐标更新
                    let mx = (evt & 0xFFFF) as i32;
                    let clamped_x = mx.max(0).min(screen_w - 1);
                    cursor.update_position(clamped_x, cursor.y, ctx);
                } else if evt_type == EVT_MOUSE_Y {
                    // 鼠标 Y 坐标更新
                    let my = (evt & 0xFFFF) as i32;
                    let clamped_y = my.max(0).min(screen_h - 1);
                    cursor.update_position(cursor.x, clamped_y, ctx);
                }
            }
        }

        DrawContext::delay_ms(16);
    }
}

/// 检查字节数组是否为空
fn is_empty(buf: &[u8; 256]) -> bool {
    buf.iter().all(|&b| b == 0)
}

/// 验证密码
fn verify_password(input_password: &[u8; 256], stored_hash: &[u8]) -> bool {
    let input_len = buf_len(input_password);
    let hash_len = buf_len(stored_hash);

    // 如果存储的哈希为空，允许任意密码
    if hash_len == 0 {
        return input_len > 0;
    }

    // 计算输入密码的 MD5 哈希
    let input_hash = crate::init::md5(&input_password[..input_len]);
    let input_hex = crate::init::bytes_to_hex(&input_hash);

    // 比较十六进制哈希字符串
    buf_eq(&input_hex[..32], &stored_hash[..hash_len])
}

/// 登录框抖动动画
fn shake_animation(ctx: &mut DrawContext, box_x: i32, box_y: i32, box_w: i32, box_h: i32, cursor: &mut Cursor) {
    let shake_offsets: [(i32, u32); 4] = [
        (-8, 50),
        (16, 50),
        (-16, 50),
        (8, 50),
    ];

    let mut current_offset: i32 = 0;

    for (offset, delay) in shake_offsets.iter() {
        let delta = *offset - current_offset;

        // 恢复光标 (避免光标跟着抖动)
        cursor.restore(ctx);

        ctx.move_rect(
            box_x + current_offset,
            box_y,
            box_x + current_offset + delta,
            box_y,
            box_w,
            box_h,
        );

        // 清除残留区域
        if delta > 0 {
            ctx.fill_rect(box_x + current_offset, box_y, delta, box_h, &BG_DARK);
        } else {
            ctx.fill_rect(box_x + current_offset + box_w + delta, box_y, -delta, box_h, &BG_DARK);
        }

        current_offset = *offset;

        // 重绘光标
        cursor.draw(ctx);
        ctx.flip();
        DrawContext::delay_ms(*delay);
    }
}
