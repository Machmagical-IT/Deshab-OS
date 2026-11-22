// =============================================================================
// taskbar 模块 - eDEXUI 风格任务栏
// =============================================================================
// 设计:
//   - 两种模式:
//     1. 桌面模式: 圆角浮动样式, 距底部有间距
//     2. 应用模式: 方角贴底样式, 全宽
//   - 模式切换时有过渡动画 (圆角渐变 + 位置滑动)
//   - 左侧: ZT5 Logo 按钮
//   - Logo 后: APP 快捷方式区域 (从桌面目录读取 .prd 文件)
//   - 右侧: 系统托盘区域 (由 systray 模块绘制)
// =============================================================================

use crate::ui::{
    DrawContext, CYAN, BG_PANEL, TEXT, TEXT_DIM, GLOW_CYAN, DIM, WHITE,
    syscall2, syscall3, buf_len,
    ease_in_out,
};

/// 系统调用号
const SYS_OPEN: u64 = 3;
const SYS_CLOSE: u64 = 4;
const SYS_READ: u64 = 1;
const SYS_READDIR: u64 = 29;

/// 目录项结构体 (与 fileman 相同)
#[repr(C)]
struct FmDirent {
    d_ino: u64,
    d_off: u64,
    d_reclen: u16,
    d_name: [u8; 256],
    d_type: u8,
}

const DIRENT_SIZE: usize = 275;
const DT_REG: u8 = 1;
const DT_DIR: u8 = 2;
const MAX_SHORTCUTS: usize = 8;

/// 任务栏模式
#[derive(Clone, Copy, PartialEq)]
pub enum TaskbarMode {
    Desktop,
    App,
}

/// 过渡动画参数
const TRANSITION_STEPS: i32 = 10;
const TRANSITION_DELAY_MS: u32 = 16;

// =============================================================================
// TaskbarState - 任务栏状态
// =============================================================================

pub struct TaskbarState {
    /// 当前模式
    pub mode: TaskbarMode,
    /// 目标模式 (用于动画过渡)
    target_mode: TaskbarMode,
    /// 过渡进度 (0.0 ~ 1.0, 用 0~100 整数表示)
    transition: i32,
    /// 是否正在过渡中
    transitioning: bool,
    /// 当前圆角半径 (动画中变化)
    current_radius: i32,
    /// 当前 Y 偏移 (动画中变化)
    current_y_offset: i32,
    /// 当前左右边距 (动画中变化)
    current_margin: i32,
    /// ZT5 Logo 是否被点击
    pub zt5_clicked: bool,
    /// 屏幕尺寸
    screen_w: i32,
    screen_h: i32,
    /// APP 快捷方式列表 (显示名)
    shortcuts: [[u8; 64]; MAX_SHORTCUTS],
    /// APP 快捷方式可执行路径 (从 .prd 文件读取)
    exec_paths: [[u8; 128]; MAX_SHORTCUTS],
    /// 快捷方式数量
    shortcut_count: usize,
    /// 选中快捷方式索引
    selected_shortcut: i32,
    /// hover 快捷方式索引 (-1 表示无)
    pub hover_shortcut: i32,
    /// 全局 tick (用于呼吸动画)
    pub tick: u64,
}

impl TaskbarState {
    pub fn new(screen_w: i32, screen_h: i32, is_root: bool) -> Self {
        let mut state = TaskbarState {
            mode: TaskbarMode::Desktop,
            target_mode: TaskbarMode::Desktop,
            transition: 0,
            transitioning: false,
            current_radius: 8,
            current_y_offset: 8,
            current_margin: 8,
            zt5_clicked: false,
            screen_w,
            screen_h,
            shortcuts: [[0u8; 64]; MAX_SHORTCUTS],
            exec_paths: [[0u8; 128]; MAX_SHORTCUTS],
            shortcut_count: 0,
            selected_shortcut: -1,
            hover_shortcut: -1,
            tick: 0,
        };
        state.load_shortcuts(is_root);
        state
    }

    /// 从桌面目录加载 .prd 快捷方式
    fn load_shortcuts(&mut self, is_root: bool) {
        self.shortcuts = [[0u8; 64]; MAX_SHORTCUTS];
        self.exec_paths = [[0u8; 128]; MAX_SHORTCUTS];
        self.shortcut_count = 0;

        // 选择桌面路径
        let (desktop_path, desktop_prefix): (&[u8], &[u8]) = if is_root {
            (b"/desktop/root\0", b"/desktop/root/")
        } else {
            (b"/desktop/user\0", b"/desktop/user/")
        };

        unsafe {
            let fd = syscall2(SYS_OPEN, desktop_path.as_ptr() as u64, 0);
            if fd == u64::MAX || (fd as i64) < 0 {
                // 打开失败, 不显示任何快捷方式
                return;
            }

            let mut buf = [0u8; DIRENT_SIZE * MAX_SHORTCUTS];
            let mut total = 0usize;

            for _i in 0..MAX_SHORTCUTS {
                let n = syscall3(
                    SYS_READDIR,
                    fd,
                    buf.as_mut_ptr().add(total) as u64,
                    DIRENT_SIZE as u64,
                );
                if n == 0 || (n as i64) < 0 {
                    break;
                }

                let offset = total;
                if offset + DIRENT_SIZE > buf.len() {
                    break;
                }
                let entry = &buf[offset..offset + DIRENT_SIZE];

                // 只处理 .prd 文件
                let d_type = entry[274];
                if d_type != DT_REG {
                    total += DIRENT_SIZE;
                    continue;
                }

                let name = &entry[18..274];
                let name_len = buf_len(name);

                // 检查 .prd 后缀
                if name_len >= 4 && name[name_len - 4] == b'.' && name[name_len - 3] == b'p'
                    && name[name_len - 2] == b'r' && name[name_len - 1] == b'd'
                {
                    let slot = self.shortcut_count;
                    // 去掉 .prd 后缀作为显示名
                    let display_len = (name_len - 4).min(63);
                    self.shortcuts[slot][..display_len]
                        .copy_from_slice(&name[..display_len]);

                    // 读取 .prd 文件内容作为 exec path
                    let mut full_path = [0u8; 192];
                    let prefix_len = desktop_prefix.len();
                    full_path[..prefix_len].copy_from_slice(desktop_prefix);
                    let copy_len = name_len.min(full_path.len() - prefix_len - 1);
                    full_path[prefix_len..prefix_len + copy_len]
                        .copy_from_slice(&name[..copy_len]);
                    full_path[prefix_len + copy_len] = 0;

                    let pfd = syscall2(SYS_OPEN, full_path.as_ptr() as u64, 0);
                    if pfd != u64::MAX && (pfd as i64) >= 0 {
                        let mut tmp = [0u8; 128];
                        let rn = syscall3(
                            SYS_READ,
                            pfd,
                            tmp.as_mut_ptr() as u64,
                            (tmp.len() - 1) as u64,
                        );
                        syscall2(SYS_CLOSE, pfd, 0);
                        let rlen = if (rn as i64) > 0 { rn as usize } else { 0 };
                        // 去除尾部空白/换行
                        let mut elen = rlen;
                        while elen > 0 {
                            let b = tmp[elen - 1];
                            if b == b'\n' || b == b'\r' || b == b' ' || b == b'\t' || b == 0 {
                                elen -= 1;
                            } else {
                                break;
                            }
                        }
                        let copy = elen.min(127);
                        self.exec_paths[slot][..copy].copy_from_slice(&tmp[..copy]);
                        self.exec_paths[slot][copy] = 0;
                    }

                    self.shortcut_count += 1;
                }

                total += DIRENT_SIZE;
            }

            syscall2(SYS_CLOSE, fd, 0);
        }
    }

    /// 获取指定索引快捷方式的可执行路径 (以 \0 结尾)
    pub fn get_exec_path(&self, idx: usize) -> Option<&[u8]> {
        if idx >= self.shortcut_count {
            return None;
        }
        let path = &self.exec_paths[idx];
        if path[0] == 0 {
            return None;
        }
        Some(path)
    }

    /// 获取指定索引快捷方式的显示名 (不含 .prd 后缀, 以 \0 终止前的有效字节)
    pub fn get_display_name(&self, idx: usize) -> Option<&[u8]> {
        if idx >= self.shortcut_count {
            return None;
        }
        let name = &self.shortcuts[idx];
        Some(name)
    }

    /// 切换到指定模式
    pub fn set_mode(&mut self, mode: TaskbarMode) {
        if self.mode == mode && !self.transitioning {
            return;
        }
        self.target_mode = mode;
        self.transition = 0;
        self.transitioning = true;
    }

    /// 更新过渡动画
    pub fn update(&mut self) {
        self.tick = self.tick.saturating_add(16);

        if !self.transitioning {
            return;
        }

        self.transition += 4; // 加快过渡速度 (25 帧完成)
        if self.transition >= 100 {
            self.transition = 100;
            self.transitioning = false;
            self.mode = self.target_mode;
        }

        // 使用 ease_in_out 缓动函数
        let t = ease_in_out(self.transition as f32 / 100.0);

        let (from_radius, to_radius) = match (self.mode, self.target_mode) {
            (TaskbarMode::Desktop, TaskbarMode::App) => (8, 0),
            (TaskbarMode::App, TaskbarMode::Desktop) => (0, 8),
            _ => (self.current_radius, self.current_radius),
        };

        let (from_margin, to_margin) = match (self.mode, self.target_mode) {
            (TaskbarMode::Desktop, TaskbarMode::App) => (8, 0),
            (TaskbarMode::App, TaskbarMode::Desktop) => (0, 8),
            _ => (self.current_margin, self.current_margin),
        };

        let (from_y_offset, to_y_offset) = match (self.mode, self.target_mode) {
            (TaskbarMode::Desktop, TaskbarMode::App) => (8, 0),
            (TaskbarMode::App, TaskbarMode::Desktop) => (0, 8),
            _ => (self.current_y_offset, self.current_y_offset),
        };

        // ease_in_out 插值
        self.current_radius = (from_radius as f32 + (to_radius as f32 - from_radius as f32) * t) as i32;
        self.current_margin = (from_margin as f32 + (to_margin as f32 - from_margin as f32) * t) as i32;
        self.current_y_offset = (from_y_offset as f32 + (to_y_offset as f32 - from_y_offset as f32) * t) as i32;
    }

    /// 处理鼠标点击
    pub fn handle_click(&mut self, mx: i32, my: i32) -> TaskbarAction {
        let bar_h = 40;
        let bar_y = self.screen_h - self.current_y_offset - bar_h;
        let bar_x = self.current_margin;
        let bar_w = self.screen_w - self.current_margin * 2;

        // 检查是否在任务栏范围内
        if my < bar_y || my > bar_y + bar_h || mx < bar_x || mx > bar_x + bar_w {
            return TaskbarAction::None;
        }

        // ZT5 Logo 区域 (左侧 60px)
        if mx < bar_x + 60 {
            self.zt5_clicked = true;
            return TaskbarAction::ZT5Click;
        }

        // APP 快捷方式区域 (分隔线之后)
        let sep_x = bar_x + 60 + 8; // logo_x + logo_w + 8
        let shortcut_start_x = sep_x + 8 + 4; // 分隔线后留4px间距
        let shortcut_w: i32 = 80; // 每个快捷方式宽度
        let shortcut_gap: i32 = 4; // 快捷方式间距

        if mx >= shortcut_start_x && self.shortcut_count > 0 {
            for i in 0..self.shortcut_count {
                let sx = shortcut_start_x + i as i32 * (shortcut_w + shortcut_gap);
                if mx >= sx && mx < sx + shortcut_w {
                    self.selected_shortcut = i as i32;
                    return TaskbarAction::LaunchShortcut(i);
                }
            }
        }

        TaskbarAction::None
    }

    /// 更新 hover 状态 (鼠标移动时调用)
    pub fn update_hover(&mut self, mx: i32, my: i32) {
        let bar_h = 40;
        let bar_y = self.screen_h - self.current_y_offset - bar_h;
        let bar_x = self.current_margin;
        let bar_w = self.screen_w - self.current_margin * 2;

        self.hover_shortcut = -1;

        if my < bar_y || my > bar_y + bar_h || mx < bar_x || mx > bar_x + bar_w {
            return;
        }

        let sep_x = bar_x + 60 + 8;
        let shortcut_start_x = sep_x + 8 + 4;
        let shortcut_w: i32 = 80;
        let shortcut_gap: i32 = 4;

        if mx >= shortcut_start_x && self.shortcut_count > 0 {
            for i in 0..self.shortcut_count {
                let sx = shortcut_start_x + i as i32 * (shortcut_w + shortcut_gap);
                if mx >= sx && mx < sx + shortcut_w {
                    self.hover_shortcut = i as i32;
                    return;
                }
            }
        }
    }

    /// 获取任务栏的 Y 坐标
    pub fn get_bar_y(&self) -> i32 {
        self.screen_h - self.current_y_offset - 40
    }

    /// 获取任务栏高度
    pub fn get_bar_h(&self) -> i32 {
        40
    }

    /// 绘制任务栏
    pub fn draw(&mut self, ctx: &mut DrawContext, active_app: &[u8]) {
        let bar_h = 40;
        let bar_y = self.screen_h - self.current_y_offset - bar_h;
        let bar_x = self.current_margin;
        let bar_w = self.screen_w - self.current_margin * 2;
        let radius = self.current_radius;

        // 任务栏背景
        ctx.fill_rounded_rect(bar_x, bar_y, bar_w, bar_h, radius, &BG_PANEL);

        // 任务栏边框 (带发光)
        if radius > 0 {
            ctx.draw_rounded_rect_glow(bar_x, bar_y, bar_w, bar_h, radius, &CYAN, &GLOW_CYAN);
        } else {
            ctx.draw_rect_glow(bar_x, bar_y, bar_w, bar_h, &CYAN, &GLOW_CYAN);
        }

        // ── ZT5 Logo 按钮 (呼吸发光) ──
        let logo_x = bar_x + 8;
        let logo_y = bar_y + 6;
        let logo_w = 44;
        let logo_h = 28;

        // Logo 背景
        if self.zt5_clicked {
            ctx.fill_rounded_rect(logo_x, logo_y, logo_w, logo_h, 4, &GLOW_CYAN);
        }

        // 呼吸发光边框 (周期 2 秒)
        ctx.draw_pulse_rect_glow(logo_x, logo_y, logo_w, logo_h, 4, &CYAN, &GLOW_CYAN, self.tick, 2000);

        // ZT5 文字
        ctx.draw_text(logo_x + 8, logo_y + 6, b"ZT5", &CYAN);

        // ── 分隔线 ──
        let sep_x = logo_x + logo_w + 8;
        ctx.draw_line(sep_x, bar_y + 8, sep_x, bar_y + bar_h - 8, &DIM);

        // ── APP 快捷方式区域 ──
        let shortcut_start_x = sep_x + 8;
        let shortcut_w: i32 = 80;
        let shortcut_gap: i32 = 4;

        for i in 0..self.shortcut_count {
            let sx = shortcut_start_x + i as i32 * (shortcut_w + shortcut_gap);
            let is_selected = i as i32 == self.selected_shortcut;
            let is_hover = i as i32 == self.hover_shortcut;

            // 选中高亮 (持续) 或 hover 高亮 (淡)
            if is_selected {
                ctx.fill_rounded_rect(sx, bar_y + 4, shortcut_w, bar_h - 8, 4, &GLOW_CYAN);
            } else if is_hover {
                // hover 时半透明高亮
                ctx.fill_rounded_rect(sx, bar_y + 4, shortcut_w, bar_h - 8, 4, &BG_PANEL);
                ctx.draw_rounded_rect(sx, bar_y + 4, shortcut_w, bar_h - 8, 4, &GLOW_CYAN);
            }

            // 快捷方式图标 (小方块)
            let icon_x = sx + 4;
            let icon_y = bar_y + 8;
            let icon_color = if is_selected || is_hover { &WHITE } else { &CYAN };
            ctx.draw_rect(icon_x, icon_y, 12, 12, icon_color);
            ctx.draw_line(icon_x + 3, icon_y + 6, icon_x + 9, icon_y + 6, icon_color);
            ctx.draw_line(icon_x + 6, icon_y + 3, icon_x + 6, icon_y + 9, icon_color);

            // 快捷方式名称
            let name_len = buf_len(&self.shortcuts[i]);
            if name_len > 0 {
                let max_name_display = ((shortcut_w - 20) / 8) as usize;
                let display_len = name_len.min(max_name_display);
                ctx.draw_text(
                    sx + 18,
                    bar_y + 10,
                    &self.shortcuts[i][..display_len],
                    if is_selected { &WHITE } else if is_hover { &TEXT } else { &TEXT },
                );
            }
        }

        // ── 应用名称 (快捷方式区域之后) ──
        let app_x = shortcut_start_x + self.shortcut_count as i32 * (shortcut_w + shortcut_gap) + 12;
        let app_len = buf_len(active_app);
        if app_len > 0 && app_x + (app_len as i32) * 8 < bar_x + bar_w - 8 {
            ctx.draw_text(app_x, bar_y + 12, &active_app[..app_len], &TEXT);
        } else if app_x < bar_x + bar_w - 60 {
            ctx.draw_text(app_x, bar_y + 12, b"DESKTOP", &TEXT_DIM);
        }
    }
}

/// 任务栏动作
#[derive(Clone, Copy, PartialEq)]
pub enum TaskbarAction {
    None,
    ZT5Click,
    LaunchShortcut(usize),
}
