// =============================================================================
// desktop 模块 - 桌面环境主协调器
// =============================================================================
// 设计:
//   - 协调所有桌面组件: shell, fileman, sidebar, taskbar, systray
//   - 管理桌面布局 (响应式)
//   - 处理全局事件分发 (键盘/鼠标)
//   - 管理应用切换 (Ctrl+Tab, ZT5 返回桌面)
//   - 60fps 渲染循环
// =============================================================================

use crate::init::FbInfo;
use crate::ui::{
    Cursor, DrawContext,
    BG_DARK, CYAN, DIM, GLOW_CYAN, WHITE, TEXT,
    scancode_to_ascii, syscall1, syscall2, syscall3,
    ease_in_out, ease_out, ease_out_back,
    ParticleSystem, CYAN as CYAN_COLOR,
};
use crate::shell::ShellState;
use crate::fileman::FileManState;
use crate::sidebar::{SidebarAction, SidebarState};
use crate::taskbar::{TaskbarAction, TaskbarMode, TaskbarState};
use crate::systray::SysTrayState;

/// 串口调试输出
unsafe fn log(s: &[u8]) {
    core::arch::asm!(
        "int 0x80",
        inlateout("rax") 2u64 => _,
        in("rdi") 1u64,
        in("rsi") s.as_ptr() as u64,
        in("rdx") s.len() as u64,
        out("rcx") _,
        out("r11") _,
    );
}

/// 系统调用号
const SYS_INPUT_POLL: u64 = 188;
const SYS_TIME: u64 = 13;
#[allow(dead_code)]
const SYS_FORK: u64 = 31;
const SYS_WAIT: u64 = 32;
const SYS_EXEC: u64 = 7;
#[allow(dead_code)]
const SYS_EXIT: u64 = 0;

/// 事件类型 (与内核 sys_input_poll 一致)
/// 高 16 位 = 类型, 低 16 位 = 数据
const EVT_KEY_DOWN: u32 = 1;    // (1<<16)|keycode
const EVT_KEY_UP: u32 = 2;      // (2<<16)|keycode
const EVT_MOUSE_X: u32 = 3;     // (3<<16)|x_coord
const EVT_MOUSE_Y: u32 = 4;     // (4<<16)|y_coord
const EVT_MOUSE_LBTN: u32 = 5;  // (5<<16)|0or1
const EVT_MOUSE_RBTN: u32 = 6;  // (6<<16)|0or1

/// 桌面应用状态
#[derive(Clone, Copy, PartialEq)]
pub enum DesktopApp {
    Desktop,
    Shell,
    FileManager,
}

// =============================================================================
// DesktopState - 桌面环境状态
// =============================================================================

pub struct DesktopState {
    /// 当前活动应用
    active_app: DesktopApp,
    /// Shell 终端
    shell: ShellState,
    /// 文件管理器
    fileman: FileManState,
    /// 侧边栏
    sidebar: SidebarState,
    /// 任务栏
    taskbar: TaskbarState,
    /// 系统托盘
    systray: SysTrayState,
    /// 是否为 root 用户
    is_root: bool,
    /// 全局 tick 计数 (用于动画)
    tick: u64,
    /// 上次时间更新 tick
    last_time_update: u64,
    /// 屏幕尺寸
    screen_w: i32,
    screen_h: i32,
    /// 鼠标按键状态 (用于检测点击)
    mouse_lbtn_down: bool,
    /// 外部 GUI 程序退出后需要完整重绘
    needs_full_redraw: bool,
}

impl DesktopState {
    pub fn new(fb_info: &FbInfo, is_root: bool) -> Self {
        unsafe { log(b"[DT] DesktopState::new start\n"); }
        let screen_w = fb_info.width as i32;
        let screen_h = fb_info.height as i32;

        // 布局计算
        let sidebar_w = 160;
        let taskbar_h = 40;
        let taskbar_margin = 8;
        let main_y = 8;
        let main_h = screen_h - main_y - taskbar_h - taskbar_margin * 2 - 4;
        let main_x = sidebar_w + 12;
        let main_w = screen_w - main_x - 8;

        // Shell 区域 (主区域上半部分)
        let shell_h = main_h * 2 / 3;
        unsafe { log(b"[DT] creating ShellState\n"); }
        let shell = ShellState::new(main_x, main_y, main_w, shell_h, is_root);
        unsafe { log(b"[DT] ShellState OK\n"); }

        // 文件管理器 (主区域下半部分)
        let fileman_y = main_y + shell_h + 4;
        let fileman_h = main_h - shell_h - 4;
        unsafe { log(b"[DT] creating FileManState\n"); }
        let fileman = FileManState::new(main_x, fileman_y, main_w / 2 - 2, fileman_h, is_root);
        unsafe { log(b"[DT] FileManState OK\n"); }

        // 侧边栏 (不再需要 is_root 参数)
        let sidebar = SidebarState::new(8, main_y, sidebar_w, main_h);

        // 任务栏 (传入 is_root 参数以加载快捷方式)
        unsafe { log(b"[DT] creating TaskbarState\n"); }
        let taskbar = TaskbarState::new(screen_w, screen_h, is_root);
        unsafe { log(b"[DT] TaskbarState OK\n"); }

        // 系统托盘 (右下角独立区域)
        let systray = SysTrayState::new(screen_w, screen_h);
        unsafe { log(b"[DT] SysTrayState OK\n"); }

        DesktopState {
            active_app: DesktopApp::Desktop,
            shell,
            fileman,
            sidebar,
            taskbar,
            systray,
            is_root,
            tick: 0,
            last_time_update: 0,
            screen_w,
            screen_h,
            mouse_lbtn_down: false,
            needs_full_redraw: false,
        }
    }

    /// 初始化桌面 (带加载动画)
    pub fn init(&mut self, ctx: &mut DrawContext, cursor: &mut Cursor) {
        unsafe { log(b"[DT] init start\n"); }

        // ── 加载动画 ──
        let cx = self.screen_w / 2;
        let cy = self.screen_h / 2;

        // 进度条参数
        let bar_w: i32 = 400;
        let bar_h: i32 = 6;
        let bar_x = cx - bar_w / 2;
        let bar_y = cy + 10;

        // 状态文字列表 (打字机效果)
        let stages: &[(&[u8], u32)] = &[
            (b"Initializing display...", 500),
            (b"Loading desktop...", 500),
            (b"Starting services...", 500),
            (b"Loading applications...", 500),
            (b"Ready.", 800),
        ];

        // ── 阶段 0: 黑色背景 + ZT5 大字淡入 ──
        ctx.clear(&BG_DARK);
        // ZT5 大字 + 发光
        ctx.draw_text_large(cx - 24, cy - 60, b"ZT5", &CYAN);
        // 副标题
        ctx.draw_text(cx - 60, cy - 20, b"ZratroOS  v5", &DIM);
        // 进度条轨道
        ctx.fill_rect(bar_x, bar_y, bar_w, bar_h, &DIM);
        ctx.draw_rect_glow(bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, &DIM, &GLOW_CYAN);
        ctx.flip_full();
        DrawContext::delay_ms(400);

        // ── 阶段 1-5: 逐阶段进度 + 打字机状态文字 ──
        let total_stages = stages.len() as i32;
        for (i, (text, delay)) in stages.iter().enumerate() {
            let progress = (i + 1) as f32 / total_stages as f32;
            let eased = ease_in_out(progress);

            // 清除进度条区域并重绘
            ctx.fill_rect(bar_x, bar_y, bar_w, bar_h, &DIM);

            // 绘制带流光的进度条 (使用 i*200ms 作为时间相位)
            let time_phase = (i as u64) * 200;
            ctx.draw_progress_bar_glow(bar_x, bar_y, bar_w, bar_h, eased, time_phase);

            // 清除状态文字区域
            ctx.fill_rect(cx - 100, cy + 24, 200, 16, &BG_DARK);

            // 打字机效果显示状态文字
            for c_idx in 0..text.len() {
                // 重绘进度条 (避免被清除文字时擦掉)
                ctx.fill_rect(bar_x, bar_y, bar_w, bar_h, &DIM);
                ctx.draw_progress_bar_glow(bar_x, bar_y, bar_w, bar_h, eased, time_phase + c_idx as u64 * 30);

                // 绘制已显示的文字
                ctx.draw_text(cx - 100, cy + 24, &text[..c_idx + 1], &TEXT);
                // 光标
                let cursor_x = cx - 100 + (c_idx + 1) as i32 * 8;
                ctx.draw_text(cursor_x, cy + 24, b"_", &CYAN);

                ctx.flip_full();
                DrawContext::delay_ms(25);
            }

            // 移除光标, 显示完整文字
            ctx.fill_rect(cx - 100, cy + 24, 200, 16, &BG_DARK);
            ctx.draw_text(cx - 100, cy + 24, text, if i == stages.len() - 1 { &CYAN } else { &TEXT });
            ctx.flip_full();
            DrawContext::delay_ms(*delay);
        }

        // ── 完成动画: 进度条脉冲 + 旋转加载圈 ──
        for frame in 0..20 {
            ctx.fill_rect(bar_x, bar_y, bar_w, bar_h, &DIM);
            ctx.draw_progress_bar_glow(bar_x, bar_y, bar_w, bar_h, 1.0, frame as u64 * 50);
            // 旋转加载圈在进度条右侧
            ctx.draw_spinner(cx + bar_w / 2 + 30, cy + 12, 8, frame as u64 * 50);
            ctx.flip_full();
            DrawContext::delay_ms(30);
        }

        // 直接清屏切换到桌面 (fade_to_black 在高分辨率下太慢)
        ctx.clear(&BG_DARK);
        ctx.flip_full();
        unsafe { log(b"[DT] loading animation done\n"); }

        // 屏幕边框
        ctx.draw_rect_glow(4, 4, self.screen_w - 8, self.screen_h - 8, &WHITE, &GLOW_CYAN);
        ctx.flip();

        // 系统信息
        ctx.draw_text(12, 12, b"ZT5 DESKTOP", &CYAN);
        ctx.draw_hline_dashed(12, self.screen_w - 12, 30, 6, 4, &DIM);
        ctx.flip();

        // 绘制所有组件
        unsafe { log(b"[DT] redraw_all start\n"); }
        self.redraw_all(ctx, cursor);
        unsafe { log(b"[DT] redraw_all done, flipping\n"); }

        ctx.flip_full();
        unsafe { log(b"[DT] flip done\n"); }

        // 启用鼠标光标
        cursor.visible = true;
        cursor.draw(ctx);
        ctx.flip();
        unsafe { log(b"[DT] init complete\n"); }
    }

    /// 重绘所有组件
    fn redraw_all(&mut self, ctx: &mut DrawContext, cursor: &mut Cursor) {
        // 侧边栏
        self.sidebar.draw(ctx);

        // Shell 终端
        self.shell.draw(ctx);

        // 文件管理器
        self.fileman.draw(ctx);

        // 任务栏
        let active_app_name: &[u8] = match self.active_app {
            DesktopApp::Desktop => b"DESKTOP",
            DesktopApp::Shell => b"TERM   ",
            DesktopApp::FileManager => b"FILES  ",
        };
        self.taskbar.draw(ctx, active_app_name);

        // 系统托盘
        self.systray.draw(ctx);

        // 鼠标光标
        if cursor.visible {
            cursor.draw(ctx);
        }
    }

    /// 主循环
    pub fn run(&mut self, ctx: &mut DrawContext, cursor: &mut Cursor) -> ! {
        unsafe { log(b"[DT] run loop start\n"); }
        let mut events = [0u32; 32];

        loop {
            self.tick += 16; // 约 16ms per frame

            // 更新光标闪烁
            self.shell.update_cursor(self.tick);

            // 更新任务栏动画
            self.taskbar.update();

            // 更新侧边栏 tick (用于脉冲动画)
            self.sidebar.update();

            // 每 1 秒更新时间
            if self.tick - self.last_time_update >= 1000 {
                self.systray.update_time();
                self.last_time_update = self.tick;
            }

            // 轮询输入事件
            let count = unsafe {
                syscall2(SYS_INPUT_POLL, events.as_mut_ptr() as u64, events.len() as u64)
            };

            if count > 0 {
                for i in 0..count as usize {
                    let evt = events[i];
                    let evt_type = (evt >> 16) & 0xFFFF;
                    let evt_data = (evt & 0xFFFF) as i32;

                    match evt_type {
                        EVT_KEY_DOWN => {
                            self.handle_key(evt_data as u8, cursor);
                        }
                        EVT_MOUSE_X => {
                            let mx = evt_data.max(0).min(self.screen_w - 1);
                            cursor.update_position(mx, cursor.y, ctx);
                            // 传递鼠标拖动给 systray
                            self.systray.handle_mouse_drag(mx, cursor.y, self.mouse_lbtn_down);
                            // 更新任务栏 hover 状态
                            self.taskbar.update_hover(mx, cursor.y);
                        }
                        EVT_MOUSE_Y => {
                            let my = evt_data.max(0).min(self.screen_h - 1);
                            cursor.update_position(cursor.x, my, ctx);
                            // 传递鼠标拖动给 systray
                            self.systray.handle_mouse_drag(cursor.x, my, self.mouse_lbtn_down);
                            // 更新任务栏 hover 状态
                            self.taskbar.update_hover(cursor.x, my);
                        }
                        EVT_MOUSE_LBTN => {
                            if evt_data == 1 {
                                // 左键按下
                                self.mouse_lbtn_down = true;
                                // 通知 systray 按下
                                self.systray.handle_mouse_drag(cursor.x, cursor.y, true);
                            } else {
                                // 左键释放 (点击完成)
                                // 通知 systray 释放
                                self.systray.handle_mouse_drag(cursor.x, cursor.y, false);
                                if self.mouse_lbtn_down {
                                    self.handle_click(cursor.x, cursor.y, cursor);
                                    self.mouse_lbtn_down = false;
                                }
                            }
                        }
                        _ => {}
                    }
                }
            }

            // 重绘
            cursor.restore(ctx);
            // 如果外部 GUI 程序刚退出, 先清屏避免残留
            if self.needs_full_redraw {
                ctx.clear(&BG_DARK);
                self.needs_full_redraw = false;
            }
            self.redraw_all(ctx, cursor);
            ctx.flip_full();

            DrawContext::delay_ms(16); // ~60fps
        }
    }

    /// 处理键盘事件
    fn handle_key(&mut self, scancode: u8, _cursor: &mut Cursor) {
        let ch = scancode_to_ascii(scancode);

        // 全局快捷键
        // Ctrl+Tab: 切换应用 (用 Esc 键模拟, 因为 Ctrl 检测复杂)
        if ch == 0x1B {
            // Esc 键 - 切换应用
            self.active_app = match self.active_app {
                DesktopApp::Desktop => DesktopApp::Shell,
                DesktopApp::Shell => DesktopApp::FileManager,
                DesktopApp::FileManager => DesktopApp::Desktop,
            };

            // 切换任务栏模式
            match self.active_app {
                DesktopApp::Desktop => self.taskbar.set_mode(TaskbarMode::Desktop),
                _ => self.taskbar.set_mode(TaskbarMode::App),
            }
            return;
        }

        // 系统托盘键盘事件 (WiFi 密码输入)
        self.systray.handle_key(scancode);

        // 根据当前活动应用分发键盘事件
        match self.active_app {
            DesktopApp::Shell => {
                self.shell.handle_key(scancode);
            }
            DesktopApp::FileManager => {
                // 文件管理器暂不处理键盘 (未来可添加快捷键)
            }
            DesktopApp::Desktop => {
                // 桌面模式: 键盘事件发给侧边栏记事本
                self.sidebar.handle_key(scancode);
            }
        }
    }

    /// 处理鼠标点击
    fn handle_click(&mut self, mx: i32, my: i32, _cursor: &mut Cursor) {
        // 任务栏点击
        let taskbar_action = self.taskbar.handle_click(mx, my);
        match taskbar_action {
            TaskbarAction::ZT5Click => {
                // ZT5 Logo: 返回桌面
                self.active_app = DesktopApp::Desktop;
                self.taskbar.set_mode(TaskbarMode::Desktop);
                self.taskbar.zt5_clicked = false;
                return;
            }
            TaskbarAction::LaunchShortcut(idx) => {
                // 优先按显示名匹配内置 App: Files/Terminal 切到内置组件
                let name = self.taskbar_shortcut_name(idx);
                if name_eq(&name, b"Files") {
                    self.active_app = DesktopApp::FileManager;
                    self.taskbar.set_mode(TaskbarMode::App);
                    return;
                }
                if name_eq(&name, b"Terminal") {
                    self.active_app = DesktopApp::Shell;
                    self.taskbar.set_mode(TaskbarMode::App);
                    return;
                }
                // 其它快捷方式: 读取 .prd 中的可执行路径, fork+exec 启动
                let mut path_buf = [0u8; 128];
                let mut path_len = 0usize;
                if let Some(path) = self.taskbar.get_exec_path(idx) {
                    let n = path.len().min(path_buf.len());
                    path_buf[..n].copy_from_slice(&path[..n]);
                    path_len = n;
                }
                if path_len > 0 {
                    self.launch_external(&path_buf);
                    // 子进程退出后, 标记需要完整重绘
                    self.needs_full_redraw = true;
                }
                return;
            }
            TaskbarAction::None => {}
        }

        // 侧边栏点击
        let sidebar_action = self.sidebar.handle_click(mx, my);
        match sidebar_action {
            SidebarAction::NotepadFocus => {
                self.active_app = DesktopApp::Desktop;
                return;
            }
            SidebarAction::None => {}
        }

        // 文件管理器点击
        let fileman_action = self.fileman.handle_click(mx, my);
        if fileman_action != crate::fileman::FileManAction::None {
            self.active_app = DesktopApp::FileManager;
            self.taskbar.set_mode(TaskbarMode::App);
            return;
        }

        // 系统托盘点击 (网络面板)
        self.systray.handle_click(mx, my);

        // Shell 终端区域点击
        let shell = &self.shell;
        if mx >= shell.x && mx <= shell.x + shell.w
            && my >= shell.y && my <= shell.y + shell.h
        {
            self.active_app = DesktopApp::Shell;
            self.taskbar.set_mode(TaskbarMode::App);
        }
    }

    /// 取得任务栏指定索引的显示名 (拷贝到固定数组中, 避免借用问题)
    fn taskbar_shortcut_name(&self, idx: usize) -> [u8; 64] {
        let mut out = [0u8; 64];
        if let Some(name) = self.taskbar.get_display_name(idx) {
            let n = name.len().min(out.len());
            out[..n].copy_from_slice(&name[..n]);
        }
        out
    }

    /// 启动外部 GUI 程序: sys_exec 创建新进程, sys_wait 等待其退出
    fn launch_external(&mut self, path_cstr: &[u8]) {
        unsafe {
            log(b"[DT] launch_external: ");
            log(path_cstr);
            log(b"\n");
            let pid = syscall1(SYS_EXEC, path_cstr.as_ptr() as u64);
            if (pid as i64) <= 0 {
                log(b"[DT] exec failed\n");
                return;
            }
            // 等待子进程退出, 期间不绘制 (避免帧缓冲区竞争)
            let mut status: u64 = 0;
            syscall1(SYS_WAIT, &mut status as *mut u64 as u64);
            log(b"[DT] child exited\n");
        }
    }
}

/// 比较两个名称缓冲区前缀 (name 是 \0 终止的固定数组, target 是字面值)
fn name_eq(name: &[u8], target: &[u8]) -> bool {
    if target.len() > name.len() {
        return false;
    }
    for i in 0..target.len() {
        if name[i] != target[i] {
            return false;
        }
    }
    // 紧接 target 之后应为 \0 或越界
    name.len() == target.len() || name[target.len()] == 0
}
