// =============================================================================
// sidebar 模块 - eDEXUI 风格左侧边栏
// =============================================================================
// 设计:
//   - 左侧窄栏, 包含简易记事本
//   - 记事本: 小型文本编辑区域, 支持键盘输入
//   - 快捷方式已移至任务栏 APP 区域
// =============================================================================

use crate::ui::{
    DrawContext, BG_PANEL, TEXT, GREEN, GLOW_CYAN, DIM,
    scancode_to_ascii,
};

const NOTEPAD_MAX_CHARS: usize = 512;

// =============================================================================
// SidebarState - 侧边栏状态
// =============================================================================

pub struct SidebarState {
    /// 记事本内容
    notepad: [u8; NOTEPAD_MAX_CHARS],
    /// 记事本内容长度
    notepad_len: usize,
    /// 是否在记事本编辑模式
    notepad_active: bool,
    /// 侧边栏区域坐标
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
    /// 全局 tick (用于脉冲动画)
    pub tick: u64,
    /// 上次输入时间 (用于字符淡入)
    last_input_tick: u64,
}

impl SidebarState {
    pub fn new(x: i32, y: i32, w: i32, h: i32) -> Self {
        SidebarState {
            notepad: [0u8; NOTEPAD_MAX_CHARS],
            notepad_len: 0,
            notepad_active: false,
            x,
            y,
            w,
            h,
            tick: 0,
            last_input_tick: 0,
        }
    }

    /// 处理鼠标点击
    pub fn handle_click(&mut self, mx: i32, my: i32) -> SidebarAction {
        let x = self.x;
        let y = self.y;
        let w = self.w;
        let h = self.h;

        // 记事本标题区域
        let notepad_title_y = y + 4;
        // 记事本内容区域
        let np_y = notepad_title_y + 18;
        let np_h = y + h - np_y - 4;

        if mx >= x && mx <= x + w && my >= notepad_title_y && my < np_y + np_h {
            self.notepad_active = true;
            return SidebarAction::NotepadFocus;
        }

        self.notepad_active = false;
        SidebarAction::None
    }

    /// 处理键盘输入 (记事本模式)
    pub fn handle_key(&mut self, scancode: u8) {
        if !self.notepad_active {
            return;
        }

        let ch = scancode_to_ascii(scancode);
        match ch {
            b'\n' => {
                if self.notepad_len < NOTEPAD_MAX_CHARS - 1 {
                    self.notepad[self.notepad_len] = b'\n';
                    self.notepad_len += 1;
                    self.last_input_tick = self.tick;
                }
            }
            b'\x08' => {
                if self.notepad_len > 0 {
                    self.notepad_len -= 1;
                    self.notepad[self.notepad_len] = 0;
                    self.last_input_tick = self.tick;
                }
            }
            c if c >= 0x20 && c < 0x7F => {
                if self.notepad_len < NOTEPAD_MAX_CHARS - 1 {
                    self.notepad[self.notepad_len] = c;
                    self.notepad_len += 1;
                    self.last_input_tick = self.tick;
                }
            }
            _ => {}
        }
    }

    /// 更新 tick
    pub fn update(&mut self) {
        self.tick = self.tick.saturating_add(16);
    }

    /// 绘制侧边栏
    pub fn draw(&mut self, ctx: &mut DrawContext) {
        let x = self.x;
        let y = self.y;
        let w = self.w;
        let h = self.h;

        // 背景
        ctx.fill_rect(x + 1, y + 1, w - 2, h - 2, &BG_PANEL);

        // 边框: 激活时脉冲发光, 否则普通发光
        if self.notepad_active {
            ctx.draw_pulse_rect_glow(x, y, w, h, 0, &GREEN, &GLOW_CYAN, self.tick, 1500);
        } else {
            ctx.draw_rect_glow(x, y, w, h, &GREEN, &GLOW_CYAN);
        }

        // ── 记事本区域 (全高度) ──
        let notepad_y = y + 4;
        // 标题颜色: 激活时更亮
        let title_color = if self.notepad_active { &GREEN } else { &DIM };
        ctx.draw_text(x + 6, notepad_y, b"NOTEPAD", title_color);

        // 记事本边框
        let np_y = notepad_y + 18;
        let np_h = y + h - np_y - 4;
        if np_h > 20 {
            // 激活时边框颜色更亮
            let np_border = if self.notepad_active { &GREEN } else { &DIM };
            ctx.draw_rect(x + 4, np_y, w - 8, np_h, np_border);

            // 记事本内容
            if self.notepad_len > 0 {
                let max_cols = ((w - 16) / 8) as usize;
                let mut row = 0i32;
                let mut col = 0usize;
                let mut line_start = 0usize;

                // 字符淡入: 最近 300ms 内输入的字符更亮
                let time_since_input = self.tick - self.last_input_tick;

                for i in 0..self.notepad_len {
                    if self.notepad[i] == b'\n' || col >= max_cols {
                        let line_y = np_y + 2 + row * 16;
                        if line_y + 16 > np_y + np_h {
                            break;
                        }
                        let line_end = if self.notepad[i] == b'\n' { i } else { i + 1 };
                        let line_len = line_end - line_start;
                        if line_len > 0 {
                            // 逐字符绘制, 最近的字符更亮
                            for c_off in 0..line_len.min(max_cols) {
                                let char_idx = line_start + c_off;
                                if char_idx == self.notepad_len - 1 && time_since_input < 300 {
                                    // 最新字符淡入效果
                                    let alpha = 1.0 - (time_since_input as f32 / 300.0);
                                    let bright = 1.0 - alpha * 0.5;
                                    ctx.draw_text(
                                        x + 6 + c_off as i32 * 8,
                                        line_y,
                                        &[self.notepad[char_idx]],
                                        &crate::ui::Color::rgb(
                                            ((TEXT.r as f32) * bright) as u8,
                                            ((TEXT.g as f32) * bright) as u8,
                                            ((TEXT.b as f32) * bright) as u8,
                                        ),
                                    );
                                    continue;
                                }
                                ctx.draw_text(
                                    x + 6 + c_off as i32 * 8,
                                    line_y,
                                    &[self.notepad[char_idx]],
                                    &TEXT,
                                );
                            }
                        }
                        row += 1;
                        col = 0;
                        line_start = if self.notepad[i] == b'\n' { i + 1 } else { i };
                    } else {
                        col += 1;
                    }
                }

                // 最后一行 (如果没有以 \n 结尾)
                if line_start < self.notepad_len {
                    let line_y = np_y + 2 + row * 16;
                    if line_y + 16 <= np_y + np_h {
                        let remaining = self.notepad_len - line_start;
                        for c_off in 0..remaining.min(max_cols) {
                            let char_idx = line_start + c_off;
                            if char_idx == self.notepad_len - 1 && time_since_input < 300 {
                                let alpha = 1.0 - (time_since_input as f32 / 300.0);
                                let bright = 1.0 - alpha * 0.5;
                                ctx.draw_text(
                                    x + 6 + c_off as i32 * 8,
                                    line_y,
                                    &[self.notepad[char_idx]],
                                    &crate::ui::Color::rgb(
                                        ((TEXT.r as f32) * bright) as u8,
                                        ((TEXT.g as f32) * bright) as u8,
                                        ((TEXT.b as f32) * bright) as u8,
                                    ),
                                );
                                continue;
                            }
                            ctx.draw_text(
                                x + 6 + c_off as i32 * 8,
                                line_y,
                                &[self.notepad[char_idx]],
                                &TEXT,
                            );
                        }
                    }
                }
            }

            // 记事本激活指示 (闪烁光标)
            if self.notepad_active {
                let blink = (self.tick / 500) % 2 == 0;
                if blink {
                    // 在最后一行末尾显示光标
                    let max_cols = ((w - 16) / 8) as usize;
                    let mut row = 0i32;
                    let mut col = 0usize;
                    let mut line_start = 0usize;
                    for i in 0..self.notepad_len {
                        if self.notepad[i] == b'\n' || col >= max_cols {
                            row += 1;
                            col = 0;
                            line_start = if self.notepad[i] == b'\n' { i + 1 } else { i };
                        } else {
                            col += 1;
                        }
                    }
                    let cursor_x = x + 6 + col as i32 * 8;
                    let cursor_y = np_y + 2 + row * 16;
                    if cursor_y + 16 <= np_y + np_h {
                        ctx.draw_text(cursor_x, cursor_y, b"_", &GREEN);
                    }
                }
            }
        }
    }
}

/// 侧边栏动作
#[derive(Clone, Copy, PartialEq)]
pub enum SidebarAction {
    None,
    NotepadFocus,
}
