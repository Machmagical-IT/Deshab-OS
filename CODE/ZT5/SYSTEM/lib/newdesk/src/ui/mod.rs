// =============================================================================
// ui 模块 - eDEXUI 风格图形基础层
// =============================================================================
// 设计理念:
//   - 线绘 (line-drawn) 美学: UI 元素由线条构成
//   - eDEXUI 极简风格: 高对比、清晰、无杂乱
//   - 双缓冲: 所有绘制到 backbuf, 统一 flip 到真实帧缓冲区
//   - 发光效果: CYAN 线条后绘制 GLOW_CYAN 宽线产生辉光
//   - 鼠标光标: 保存/恢复像素避免 corruption
//
// 像素格式: 32位 (ARGB/XRGB), 每像素占 4 字节
// 坐标系: 左上角 (0,0), X 向右增大, Y 向下增大
// =============================================================================

use crate::init::FbInfo;

// ── 颜色结构体 ──

#[derive(Clone, Copy)]
pub struct Color {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

impl Color {
    pub const fn rgb(r: u8, g: u8, b: u8) -> Self {
        Color { r, g, b, a: 255 }
    }
}

// ── eDEXUI 配色方案 ──

pub const CYAN: Color = Color::rgb(0, 255, 255);
pub const WHITE: Color = Color::rgb(255, 255, 255);
pub const DIM: Color = Color::rgb(60, 60, 80);
pub const GREEN: Color = Color::rgb(0, 255, 100);
pub const RED: Color = Color::rgb(255, 60, 60);
pub const BG_DARK: Color = Color::rgb(8, 8, 20);
pub const BG_PANEL: Color = Color::rgb(12, 12, 30);
pub const TEXT: Color = Color::rgb(220, 220, 240);
pub const TEXT_DIM: Color = Color::rgb(100, 100, 130);
pub const GLOW_CYAN: Color = Color::rgb(0, 80, 80);
pub const YELLOW: Color = Color::rgb(255, 200, 0);

// ── 动画参数 ──

const ANIM_DELAY_MS: u32 = 4;
const ANIM_PIXELS_PER_FRAME: u32 = 16;

// ── 后备缓冲区 ──

const BACKBUF_MAX_PIXELS: usize = 2560 * 1440;

static mut BACKBUF: [u32; BACKBUF_MAX_PIXELS] = [0u32; BACKBUF_MAX_PIXELS];

/// 从帧缓冲区信息创建 DrawContext
pub fn create_draw_context(fb_info: &FbInfo) -> DrawContext {
    unsafe {
        let stride = fb_info.pitch as usize / 4;
        let needed = fb_info.height as usize * stride;
        let buf_len = if needed <= BACKBUF_MAX_PIXELS {
            needed
        } else {
            BACKBUF_MAX_PIXELS / stride * stride
        };
        DrawContext::new(fb_info, &mut BACKBUF[..buf_len])
    }
}

// =============================================================================
// 系统调用封装
// =============================================================================

#[inline(always)]
pub unsafe fn syscall1(n: u64, a0: u64) -> u64 {
    let ret: u64;
    core::arch::asm!(
        "int 0x80",
        inlateout("rax") n => ret,
        in("rdi") a0,
        out("rcx") _,
        out("r11") _,
    );
    ret
}

#[inline(always)]
pub unsafe fn syscall2(n: u64, a0: u64, a1: u64) -> u64 {
    let ret: u64;
    core::arch::asm!(
        "int 0x80",
        inlateout("rax") n => ret,
        in("rdi") a0,
        in("rsi") a1,
        out("rcx") _,
        out("r11") _,
    );
    ret
}

#[inline(always)]
pub unsafe fn syscall3(n: u64, a0: u64, a1: u64, a2: u64) -> u64 {
    let ret: u64;
    core::arch::asm!(
        "int 0x80",
        inlateout("rax") n => ret,
        in("rdi") a0,
        in("rsi") a1,
        in("rdx") a2,
        out("rcx") _,
        out("r11") _,
    );
    ret
}

#[inline(always)]
pub unsafe fn syscall4(n: u64, a0: u64, a1: u64, a2: u64, a3: u64) -> u64 {
    let ret: u64;
    core::arch::asm!(
        "int 0x80",
        inlateout("rax") n => ret,
        in("rdi") a0,
        in("rsi") a1,
        in("rdx") a2,
        in("r10") a3,
        out("rcx") _,
        out("r11") _,
    );
    ret
}

/// 输出字符串到串口
pub unsafe fn serial_write(s: &[u8]) {
    let _ = core::arch::asm!(
        "int 0x80",
        inlateout("rax") 2u64 => _,
        in("rdi") 1u64,
        in("rsi") s.as_ptr() as u64,
        in("rdx") s.len() as u64,
        out("rcx") _,
        out("r11") _,
    );
}

// =============================================================================
// PS/2 键码到 ASCII 转换
// =============================================================================

pub fn scancode_to_ascii(scancode: u8) -> u8 {
    match scancode {
        0x01 => b'\x1B',
        0x02 => b'1', 0x03 => b'2', 0x04 => b'3', 0x05 => b'4',
        0x06 => b'5', 0x07 => b'6', 0x08 => b'7', 0x09 => b'8',
        0x0A => b'9', 0x0B => b'0',
        0x0C => b'-', 0x0D => b'=',
        0x0E => b'\x08',
        0x0F => b'\t',
        0x10 => b'q', 0x11 => b'w', 0x12 => b'e', 0x13 => b'r',
        0x14 => b't', 0x15 => b'y', 0x16 => b'u', 0x17 => b'i',
        0x18 => b'o', 0x19 => b'p',
        0x1A => b'[', 0x1B => b']',
        0x1C => b'\n',
        0x1D => 0, // Ctrl
        0x1E => b'a', 0x1F => b's', 0x20 => b'd', 0x21 => b'f',
        0x22 => b'g', 0x23 => b'h', 0x24 => b'j', 0x25 => b'k',
        0x26 => b'l',
        0x27 => b';', 0x28 => b'\'', 0x29 => b'`',
        0x2A => 0, // LShift
        0x2B => b'\\',
        0x2C => b'z', 0x2D => b'x', 0x2E => b'c', 0x2F => b'v',
        0x30 => b'b', 0x31 => b'n', 0x32 => b'm',
        0x33 => b',', 0x34 => b'.', 0x35 => b'/',
        0x36 => 0, // RShift
        0x37 => b'*',
        0x38 => 0, // Alt
        0x39 => b' ',
        _ => 0,
    }
}

// =============================================================================
// 工具函数
// =============================================================================

/// 整数平方根 (Newton 法)
pub fn integer_sqrt(n: i32) -> i32 {
    if n <= 0 { return 0; }
    let mut x = n;
    let mut y = (x + 1) / 2;
    while y < x {
        x = y;
        y = (x + n / x) / 2;
    }
    x
}

/// 获取字节数组有效长度 (到第一个 \0)
pub fn buf_len(buf: &[u8]) -> usize {
    buf.iter().position(|&b| b == 0).unwrap_or(buf.len())
}

/// 比较两个字节数组
pub fn buf_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() { return false; }
    for i in 0..a.len() {
        if a[i] != b[i] { return false; }
    }
    true
}

// =============================================================================
// DrawContext - 帧缓冲区绘制上下文
// =============================================================================

pub struct DrawContext {
    pub fb_info: FbInfo,
    pub backbuf: &'static mut [u32],
    pub stride: usize,
    pub is_bgr: bool,
    dirty_y0: i32,
    dirty_y1: i32,
}

impl DrawContext {
    pub fn new(fb_info: &FbInfo, backbuf: &'static mut [u32]) -> Self {
        let stride = fb_info.pitch as usize / 4;
        let is_bgr = fb_info.format == 6 || fb_info.format == 7;
        DrawContext {
            fb_info: *fb_info,
            backbuf,
            stride,
            is_bgr,
            dirty_y0: fb_info.height as i32,
            dirty_y1: 0,
        }
    }

    #[inline(always)]
    pub fn color_to_pixel(&self, color: &Color) -> u32 {
        if self.is_bgr {
            ((color.a as u32) << 24) | ((color.b as u32) << 16)
                | ((color.g as u32) << 8)
                | (color.r as u32)
        } else {
            ((color.a as u32) << 24) | ((color.r as u32) << 16)
                | ((color.g as u32) << 8)
                | (color.b as u32)
        }
    }

    #[inline(always)]
    fn mark_dirty(&mut self, y: i32) {
        if y < self.dirty_y0 { self.dirty_y0 = y.max(0); }
        if y + 1 > self.dirty_y1 { self.dirty_y1 = (y + 1).min(self.fb_info.height as i32); }
    }

    fn reset_dirty(&mut self) {
        self.dirty_y0 = self.fb_info.height as i32;
        self.dirty_y1 = 0;
    }

    pub fn flip(&mut self) {
        let real_fb = self.fb_info.base as *mut u32;
        if real_fb.is_null() { return; }
        if self.dirty_y0 >= self.dirty_y1 { return; }

        let screen_h = self.fb_info.height as usize;
        let y0 = self.dirty_y0 as usize;
        let y1 = self.dirty_y1.min(screen_h as i32) as usize;
        let stride = self.stride;

        for y in y0..y1 {
            let src_offset = y * stride;
            let copy_len = stride.min(self.backbuf.len().saturating_sub(src_offset));
            if copy_len == 0 { break; }
            unsafe {
                core::ptr::copy_nonoverlapping(
                    self.backbuf.as_ptr().add(src_offset),
                    real_fb.add(src_offset),
                    copy_len,
                );
            }
        }
        /* 写屏障: 确保 framebuffer 写入对 GPU 可见 (实机需要) */
        unsafe { core::arch::asm!("sfence", options(nostack, preserves_flags)); }
        self.reset_dirty();
    }

    pub fn flip_full(&mut self) {
        let real_fb = self.fb_info.base as *mut u32;
        if real_fb.is_null() { return; }
        let total = self.fb_info.height as usize * self.stride;
        let copy_len = total.min(self.backbuf.len());
        unsafe {
            core::ptr::copy_nonoverlapping(self.backbuf.as_ptr(), real_fb, copy_len);
            /* 写屏障: 确保 framebuffer 写入对 GPU 可见 (实机需要) */
            core::arch::asm!("sfence", options(nostack, preserves_flags));
        }
        self.reset_dirty();
    }

    pub fn clear(&mut self, color: &Color) {
        let pixel = self.color_to_pixel(color);
        let total = self.fb_info.height as usize * self.stride;
        let len = total.min(self.backbuf.len());
        for i in 0..len {
            self.backbuf[i] = pixel;
        }
        self.dirty_y0 = 0;
        self.dirty_y1 = self.fb_info.height as i32;
    }

    pub fn fill_rect(&mut self, x: i32, y: i32, w: i32, h: i32, color: &Color) {
        let pixel = self.color_to_pixel(color);
        let screen_w = self.fb_info.width as i32;
        let screen_h = self.fb_info.height as i32;
        let x0 = x.max(0) as usize;
        let y0 = y.max(0) as usize;
        let x1 = (x + w).min(screen_w) as usize;
        let y1 = (y + h).min(screen_h) as usize;
        for yy in y0..y1 {
            let row_offset = yy * self.stride;
            if row_offset + x1 > self.backbuf.len() { break; }
            self.mark_dirty(yy as i32);
            for xx in x0..x1 {
                self.backbuf[row_offset + xx] = pixel;
            }
        }
    }

    #[inline(always)]
    pub fn draw_pixel(&mut self, x: i32, y: i32, color: &Color) {
        let screen_w = self.fb_info.width as i32;
        let screen_h = self.fb_info.height as i32;
        if x >= 0 && x < screen_w && y >= 0 && y < screen_h {
            let idx = y as usize * self.stride + x as usize;
            if idx < self.backbuf.len() {
                self.backbuf[idx] = self.color_to_pixel(color);
                self.mark_dirty(y);
            }
        }
    }

    /// 绘制原始像素值 (用于光标恢复等)
    #[inline(always)]
    pub fn draw_pixel_raw(&mut self, x: i32, y: i32, pixel: u32) {
        let screen_w = self.fb_info.width as i32;
        let screen_h = self.fb_info.height as i32;
        if x >= 0 && x < screen_w && y >= 0 && y < screen_h {
            let idx = y as usize * self.stride + x as usize;
            if idx < self.backbuf.len() {
                self.backbuf[idx] = pixel;
                self.mark_dirty(y);
            }
        }
    }

    /// 读取原始像素值 (用于光标保存)
    #[inline(always)]
    pub fn read_pixel_raw(&self, x: i32, y: i32) -> u32 {
        let screen_w = self.fb_info.width as i32;
        let screen_h = self.fb_info.height as i32;
        if x >= 0 && x < screen_w && y >= 0 && y < screen_h {
            let idx = y as usize * self.stride + x as usize;
            if idx < self.backbuf.len() {
                return self.backbuf[idx];
            }
        }
        0
    }

    pub fn draw_line(&mut self, x0: i32, y0: i32, x1: i32, y1: i32, color: &Color) {
        let pixel = self.color_to_pixel(color);
        let screen_w = self.fb_info.width as i32;
        let screen_h = self.fb_info.height as i32;
        let dx = (x1 - x0).abs();
        let dy = -(y1 - y0).abs();
        let sx = if x0 < x1 { 1 } else { -1 };
        let sy = if y0 < y1 { 1 } else { -1 };
        let mut err = dx + dy;
        let mut cx = x0;
        let mut cy = y0;
        loop {
            if cx >= 0 && cx < screen_w && cy >= 0 && cy < screen_h {
                let idx = cy as usize * self.stride + cx as usize;
                if idx < self.backbuf.len() {
                    self.backbuf[idx] = pixel;
                    self.mark_dirty(cy);
                }
            }
            if cx == x1 && cy == y1 { break; }
            let e2 = 2 * err;
            if e2 >= dy { err += dy; cx += sx; }
            if e2 <= dx { err += dx; cy += sy; }
        }
    }

    pub fn draw_rect(&mut self, x: i32, y: i32, w: i32, h: i32, color: &Color) {
        self.draw_line(x, y, x + w - 1, y, color);
        self.draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
        self.draw_line(x, y, x, y + h - 1, color);
        self.draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
    }

    pub fn draw_rounded_rect(&mut self, x: i32, y: i32, w: i32, h: i32, r: i32, color: &Color) {
        let r = r.min(w / 2).min(h / 2).max(0);
        // 直线段
        self.draw_line(x + r, y, x + w - 1 - r, y, color);
        self.draw_line(x + r, y + h - 1, x + w - 1 - r, y + h - 1, color);
        self.draw_line(x, y + r, x, y + h - 1 - r, color);
        self.draw_line(x + w - 1, y + r, x + w - 1, y + h - 1 - r, color);
        // 圆角 (扫描线法, 连续无间断)
        self.draw_corner_smooth(x + r, y + r, r, 4, color);
        self.draw_corner_smooth(x + w - 1 - r, y + r, r, 1, color);
        self.draw_corner_smooth(x + w - 1 - r, y + h - 1 - r, r, 2, color);
        self.draw_corner_smooth(x + r, y + h - 1 - r, r, 3, color);
    }

    /// 平滑圆角绘制 (扫描线法, 连续无锯齿)
    /// quadrant: 1=右上, 2=右下, 3=左下, 4=左上
    fn draw_corner_smooth(&mut self, cx: i32, cy: i32, r: i32, quadrant: i32, color: &Color) {
        if r <= 0 { return; }
        let r2 = r * r;
        // 对每个 y 偏移, 计算对应的 x 偏移, 画连续的水平线段
        for dy in 0..=r {
            let dx = integer_sqrt(r2 - dy * dy);
            match quadrant {
                1 => {
                    // 右上: 圆心在左下, 弧线向右上凸
                    // dy 从 0(圆心同高) 到 r(圆心上方 r)
                    // dx 从 r 到 0
                    let py = cy - dy;
                    let px_start = cx + dx;
                    let px_end = cx + r;
                    if px_end >= px_start {
                        self.draw_line(px_start, py, px_end, py, color);
                    }
                }
                2 => {
                    // 右下: 圆心在左上, 弧线向右下凸
                    let py = cy + dy;
                    let px_start = cx + dx;
                    let px_end = cx + r;
                    if px_end >= px_start {
                        self.draw_line(px_start, py, px_end, py, color);
                    }
                }
                3 => {
                    // 左下: 圆心在右上, 弧线向左下凸
                    let py = cy + dy;
                    let px_start = cx - r;
                    let px_end = cx - dx;
                    if px_end >= px_start {
                        self.draw_line(px_start, py, px_end, py, color);
                    }
                }
                4 => {
                    // 左上: 圆心在右下, 弧线向左上凸
                    let py = cy - dy;
                    let px_start = cx - r;
                    let px_end = cx - dx;
                    if px_end >= px_start {
                        self.draw_line(px_start, py, px_end, py, color);
                    }
                }
                _ => {}
            }
        }
    }

    fn draw_arc(&mut self, cx: i32, cy: i32, r: i32, quadrant: i32, color: &Color) {
        // 保留旧接口, 内部调用平滑版本
        self.draw_corner_smooth(cx, cy, r, quadrant, color);
    }

    pub fn draw_hline_dashed(&mut self, x0: i32, x1: i32, y: i32, dash_len: i32, gap_len: i32, color: &Color) {
        let mut x = x0.min(x1);
        let end = x0.max(x1);
        while x < end {
            let seg_end = (x + dash_len).min(end);
            self.draw_line(x, y, seg_end, y, color);
            x = seg_end + gap_len;
        }
    }

    pub fn draw_text(&mut self, x: i32, y: i32, text: &[u8], color: &Color) {
        let pixel = self.color_to_pixel(color);
        let screen_w = self.fb_info.width as i32;
        let screen_h = self.fb_info.height as i32;
        let mut cur_x = x;
        for &ch in text {
            if ch == b'\0' { break; }
            if ch as usize >= 128 { cur_x += 8; continue; }
            let glyph = &FONT_8X16[(ch as usize) * 16..(ch as usize) * 16 + 16];
            for row in 0..16u32 {
                let py = y + row as i32;
                if py < 0 || py >= screen_h { continue; }
                let bits = glyph[row as usize];
                for col in 0..8u32 {
                    let px = cur_x + col as i32;
                    if px < 0 || px >= screen_w { continue; }
                    if bits & (0x80 >> col) != 0 {
                        let idx = py as usize * self.stride + px as usize;
                        if idx < self.backbuf.len() {
                            self.backbuf[idx] = pixel;
                        }
                    }
                }
            }
            self.mark_dirty(y);
            self.mark_dirty(y + 15);
            cur_x += 8;
        }
    }

    /// 绘制大号文字 (2x缩放, 每字符 16x32 像素)
    pub fn draw_text_large(&mut self, x: i32, y: i32, text: &[u8], color: &Color) {
        let pixel = self.color_to_pixel(color);
        let screen_w = self.fb_info.width as i32;
        let screen_h = self.fb_info.height as i32;
        let mut cur_x = x;
        for &ch in text {
            if ch == b'\0' { break; }
            if ch as usize >= 128 { cur_x += 16; continue; }
            let glyph = &FONT_8X16[(ch as usize) * 16..(ch as usize) * 16 + 16];
            for row in 0..16u32 {
                let bits = glyph[row as usize];
                for col in 0..8u32 {
                    if bits & (0x80 >> col) != 0 {
                        // 2x2 像素块
                        for dy in 0..2i32 {
                            for dx in 0..2i32 {
                                let px = cur_x + col as i32 * 2 + dx;
                                let py = y + row as i32 * 2 + dy;
                                if px >= 0 && px < screen_w && py >= 0 && py < screen_h {
                                    let idx = py as usize * self.stride + px as usize;
                                    if idx < self.backbuf.len() {
                                        self.backbuf[idx] = pixel;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            self.mark_dirty(y);
            self.mark_dirty(y + 31);
            cur_x += 16;
        }
    }

    pub fn move_rect(&mut self, src_x: i32, src_y: i32, dst_x: i32, dst_y: i32, w: i32, h: i32) {
        let screen_w = self.fb_info.width as i32;
        let screen_h = self.fb_info.height as i32;
        let (row_start, row_end, row_step): (i32, i32, i32) = if dst_y > src_y {
            (h - 1, -1, -1)
        } else {
            (0, h, 1)
        };
        let (col_start, col_end, col_step): (i32, i32, i32) = if dst_x > src_x {
            (w - 1, -1, -1)
        } else {
            (0, w, 1)
        };
        let mut row = row_start;
        while row != row_end {
            let sy = src_y + row;
            let dy = dst_y + row;
            if sy < 0 || sy >= screen_h || dy < 0 || dy >= screen_h { row += row_step; continue; }
            self.mark_dirty(dy);
            let mut col = col_start;
            while col != col_end {
                let sx = src_x + col;
                let dx = dst_x + col;
                if sx >= 0 && sx < screen_w && dx >= 0 && dx < screen_w {
                    let src_idx = sy as usize * self.stride + sx as usize;
                    let dst_idx = dy as usize * self.stride + dx as usize;
                    if src_idx < self.backbuf.len() && dst_idx < self.backbuf.len() {
                        self.backbuf[dst_idx] = self.backbuf[src_idx];
                    }
                }
                col += col_step;
            }
            row += row_step;
        }
    }

    pub fn delay_ms(ms: u32) {
        crate::init::sys_sleep(ms as u64);
    }

    // ─────────────────────────────────────────────
    // 填充圆角矩形
    // ─────────────────────────────────────────────

    pub fn fill_rounded_rect(&mut self, x: i32, y: i32, w: i32, h: i32, r: i32, color: &Color) {
        let r = r.min(w / 2).min(h / 2).max(0);
        let pixel = self.color_to_pixel(color);
        let screen_w = self.fb_info.width as i32;
        let screen_h = self.fb_info.height as i32;
        let r2 = r * r;

        for row in 0..h {
            let py = y + row;
            if py < 0 || py >= screen_h { continue; }

            let (x_start, x_end) = if row < r {
                let dy = r - row;
                let dx = integer_sqrt(r2 - dy * dy);
                (x + r - dx, x + w - r + dx)
            } else if row >= h - r {
                let dy = row - (h - r - 1);
                let dx = integer_sqrt(r2 - dy * dy);
                (x + r - dx, x + w - r + dx)
            } else {
                (x, x + w)
            };

            self.mark_dirty(py);
            let xs = x_start.max(0) as usize;
            let xe = x_end.min(screen_w) as usize;
            let row_offset = py as usize * self.stride;
            for px in xs..xe {
                let idx = row_offset + px;
                if idx < self.backbuf.len() {
                    self.backbuf[idx] = pixel;
                }
            }
        }
    }

    // ─────────────────────────────────────────────
    // 发光效果绘制 (即时, 无动画)
    // ─────────────────────────────────────────────

    /// 绘制带发光效果的线段
    pub fn draw_line_glow(&mut self, x0: i32, y0: i32, x1: i32, y1: i32, color: &Color, glow: &Color) {
        // 先绘制发光 (偏移线)
        if y0 == y1 {
            // 水平线 - 垂直偏移
            self.draw_line(x0, y0 - 1, x1, y1 - 1, glow);
            self.draw_line(x0, y0 + 1, x1, y1 + 1, glow);
        } else if x0 == x1 {
            // 垂直线 - 水平偏移
            self.draw_line(x0 - 1, y0, x1 - 1, y1, glow);
            self.draw_line(x0 + 1, y0, x1 + 1, y1, glow);
        } else {
            // 对角线 - 四方向偏移
            self.draw_line(x0 - 1, y0, x1 - 1, y1, glow);
            self.draw_line(x0 + 1, y0, x1 + 1, y1, glow);
            self.draw_line(x0, y0 - 1, x1, y1 - 1, glow);
            self.draw_line(x0, y0 + 1, x1, y1 + 1, glow);
        }
        // 主线
        self.draw_line(x0, y0, x1, y1, color);
    }

    /// 绘制带发光效果的矩形
    pub fn draw_rect_glow(&mut self, x: i32, y: i32, w: i32, h: i32, color: &Color, glow: &Color) {
        self.draw_line_glow(x, y, x + w - 1, y, color, glow);
        self.draw_line_glow(x + w - 1, y, x + w - 1, y + h - 1, color, glow);
        self.draw_line_glow(x + w - 1, y + h - 1, x, y + h - 1, color, glow);
        self.draw_line_glow(x, y + h - 1, x, y, color, glow);
    }

    /// 绘制带发光效果的圆角矩形
    pub fn draw_rounded_rect_glow(&mut self, x: i32, y: i32, w: i32, h: i32, r: i32, color: &Color, glow: &Color) {
        let r = r.min(w / 2).min(h / 2).max(0);
        // 发光直线段
        self.draw_line_glow(x + r, y, x + w - 1 - r, y, color, glow);
        self.draw_line_glow(x + r, y + h - 1, x + w - 1 - r, y + h - 1, color, glow);
        self.draw_line_glow(x, y + r, x, y + h - 1 - r, color, glow);
        self.draw_line_glow(x + w - 1, y + r, x + w - 1, y + h - 1 - r, color, glow);
        // 发光圆角 (扫描线法, 连续平滑)
        self.draw_corner_smooth_glow(x + r, y + r, r, 4, color, glow);
        self.draw_corner_smooth_glow(x + w - 1 - r, y + r, r, 1, color, glow);
        self.draw_corner_smooth_glow(x + w - 1 - r, y + h - 1 - r, r, 2, color, glow);
        self.draw_corner_smooth_glow(x + r, y + h - 1 - r, r, 3, color, glow);
    }

    /// 平滑圆角发光绘制 (扫描线法)
    fn draw_corner_smooth_glow(&mut self, cx: i32, cy: i32, r: i32, quadrant: i32, color: &Color, glow: &Color) {
        if r <= 0 { return; }
        let r2 = r * r;
        for dy in 0..=r {
            let dx = integer_sqrt(r2 - dy * dy);
            match quadrant {
                1 => {
                    let py = cy - dy;
                    let px_start = cx + dx;
                    let px_end = cx + r;
                    if px_end >= px_start {
                        // 发光 (上下偏移)
                        self.draw_line(px_start, py - 1, px_end, py - 1, glow);
                        self.draw_line(px_start, py + 1, px_end, py + 1, glow);
                        // 主线
                        self.draw_line(px_start, py, px_end, py, color);
                    }
                }
                2 => {
                    let py = cy + dy;
                    let px_start = cx + dx;
                    let px_end = cx + r;
                    if px_end >= px_start {
                        self.draw_line(px_start, py - 1, px_end, py - 1, glow);
                        self.draw_line(px_start, py + 1, px_end, py + 1, glow);
                        self.draw_line(px_start, py, px_end, py, color);
                    }
                }
                3 => {
                    let py = cy + dy;
                    let px_start = cx - r;
                    let px_end = cx - dx;
                    if px_end >= px_start {
                        self.draw_line(px_start, py - 1, px_end, py - 1, glow);
                        self.draw_line(px_start, py + 1, px_end, py + 1, glow);
                        self.draw_line(px_start, py, px_end, py, color);
                    }
                }
                4 => {
                    let py = cy - dy;
                    let px_start = cx - r;
                    let px_end = cx - dx;
                    if px_end >= px_start {
                        self.draw_line(px_start, py - 1, px_end, py - 1, glow);
                        self.draw_line(px_start, py + 1, px_end, py + 1, glow);
                        self.draw_line(px_start, py, px_end, py, color);
                    }
                }
                _ => {}
            }
        }
    }

    fn draw_arc_glow(&mut self, cx: i32, cy: i32, r: i32, quadrant: i32, color: &Color, glow: &Color) {
        // 保留旧接口, 内部调用平滑版本
        self.draw_corner_smooth_glow(cx, cy, r, quadrant, color, glow);
    }

    // ─────────────────────────────────────────────
    // 动画绘制函数
    // ─────────────────────────────────────────────

    pub fn draw_line_animated(&mut self, x0: i32, y0: i32, x1: i32, y1: i32, color: &Color) {
        let pixel = self.color_to_pixel(color);
        let screen_w = self.fb_info.width as i32;
        let screen_h = self.fb_info.height as i32;
        let dx = (x1 - x0).abs();
        let dy = -(y1 - y0).abs();
        let sx = if x0 < x1 { 1 } else { -1 };
        let sy = if y0 < y1 { 1 } else { -1 };
        let mut err = dx + dy;
        let mut cx = x0;
        let mut cy = y0;
        let mut count = 0u32;
        loop {
            if cx >= 0 && cx < screen_w && cy >= 0 && cy < screen_h {
                let idx = cy as usize * self.stride + cx as usize;
                if idx < self.backbuf.len() {
                    self.backbuf[idx] = pixel;
                    self.mark_dirty(cy);
                }
            }
            count += 1;
            if count % ANIM_PIXELS_PER_FRAME == 0 {
                self.flip();
                Self::delay_ms(ANIM_DELAY_MS);
            }
            if cx == x1 && cy == y1 { break; }
            let e2 = 2 * err;
            if e2 >= dy { err += dy; cx += sx; }
            if e2 <= dx { err += dx; cy += sy; }
        }
    }

    /// 带发光效果的动画线段
    pub fn draw_line_animated_glow(&mut self, x0: i32, y0: i32, x1: i32, y1: i32, color: &Color, glow: &Color) {
        let glow_pixel = self.color_to_pixel(glow);
        let main_pixel = self.color_to_pixel(color);
        let screen_w = self.fb_info.width as i32;
        let screen_h = self.fb_info.height as i32;
        let dx = (x1 - x0).abs();
        let dy = -(y1 - y0).abs();
        let sx = if x0 < x1 { 1 } else { -1 };
        let sy = if y0 < y1 { 1 } else { -1 };
        let mut err = dx + dy;
        let mut cx = x0;
        let mut cy = y0;
        let mut count = 0u32;
        loop {
            // 发光像素 (4 邻居)
            for &(ox, oy) in &[(-1i32, 0i32), (1, 0), (0, -1), (0, 1)] {
                let gx = cx + ox;
                let gy = cy + oy;
                if gx >= 0 && gx < screen_w && gy >= 0 && gy < screen_h {
                    let idx = gy as usize * self.stride + gx as usize;
                    if idx < self.backbuf.len() {
                        self.backbuf[idx] = glow_pixel;
                        self.mark_dirty(gy);
                    }
                }
            }
            // 主像素
            if cx >= 0 && cx < screen_w && cy >= 0 && cy < screen_h {
                let idx = cy as usize * self.stride + cx as usize;
                if idx < self.backbuf.len() {
                    self.backbuf[idx] = main_pixel;
                    self.mark_dirty(cy);
                }
            }
            count += 1;
            if count % ANIM_PIXELS_PER_FRAME == 0 {
                self.flip();
                Self::delay_ms(ANIM_DELAY_MS);
            }
            if cx == x1 && cy == y1 { break; }
            let e2 = 2 * err;
            if e2 >= dy { err += dy; cx += sx; }
            if e2 <= dx { err += dx; cy += sy; }
        }
    }

    pub fn draw_rect_animated(&mut self, x: i32, y: i32, w: i32, h: i32, color: &Color) {
        self.draw_line_animated(x, y, x + w - 1, y, color);
        self.draw_line_animated(x + w - 1, y, x + w - 1, y + h - 1, color);
        self.draw_line_animated(x + w - 1, y + h - 1, x, y + h - 1, color);
        self.draw_line_animated(x, y + h - 1, x, y, color);
    }

    pub fn draw_rounded_rect_animated(&mut self, x: i32, y: i32, w: i32, h: i32, r: i32, color: &Color) {
        let r = r.min(w / 2).min(h / 2).max(0);
        self.draw_line_animated(x + r, y, x + w - 1 - r, y, color);
        self.draw_arc_animated(x + w - 1 - r, y + r, r, 1, color);
        self.draw_line_animated(x + w - 1, y + r, x + w - 1, y + h - 1 - r, color);
        self.draw_arc_animated(x + w - 1 - r, y + h - 1 - r, r, 2, color);
        self.draw_line_animated(x + w - 1 - r, y + h - 1, x + r, y + h - 1, color);
        self.draw_arc_animated(x + r, y + h - 1 - r, r, 3, color);
        self.draw_line_animated(x, y + h - 1 - r, x, y + r, color);
        self.draw_arc_animated(x + r, y + r, r, 4, color);
    }

    /// 带发光效果的动画圆角矩形
    pub fn draw_rounded_rect_animated_glow(&mut self, x: i32, y: i32, w: i32, h: i32, r: i32, color: &Color, glow: &Color) {
        let r = r.min(w / 2).min(h / 2).max(0);
        self.draw_line_animated_glow(x + r, y, x + w - 1 - r, y, color, glow);
        self.draw_arc_animated_glow(x + w - 1 - r, y + r, r, 1, color, glow);
        self.draw_line_animated_glow(x + w - 1, y + r, x + w - 1, y + h - 1 - r, color, glow);
        self.draw_arc_animated_glow(x + w - 1 - r, y + h - 1 - r, r, 2, color, glow);
        self.draw_line_animated_glow(x + w - 1 - r, y + h - 1, x + r, y + h - 1, color, glow);
        self.draw_arc_animated_glow(x + r, y + h - 1 - r, r, 3, color, glow);
        self.draw_line_animated_glow(x, y + h - 1 - r, x, y + r, color, glow);
        self.draw_arc_animated_glow(x + r, y + r, r, 4, color, glow);
    }

    fn draw_arc_animated(&mut self, cx: i32, cy: i32, r: i32, quadrant: i32, color: &Color) {
        if r <= 0 { return; }
        let r2 = r * r;
        let mut count = 0u32;
        for dy in 0..=r {
            let dx = integer_sqrt(r2 - dy * dy);
            let (py, px_start, px_end) = match quadrant {
                1 => (cy - dy, cx + dx, cx + r),
                2 => (cy + dy, cx + dx, cx + r),
                3 => (cy + dy, cx - r, cx - dx),
                4 => (cy - dy, cx - r, cx - dx),
                _ => continue,
            };
            if px_end >= px_start {
                for px in px_start..=px_end {
                    self.draw_pixel(px, py, color);
                    count += 1;
                    if count % ANIM_PIXELS_PER_FRAME == 0 {
                        self.flip();
                        Self::delay_ms(ANIM_DELAY_MS);
                    }
                }
            }
        }
    }

    fn draw_arc_animated_glow(&mut self, cx: i32, cy: i32, r: i32, quadrant: i32, color: &Color, glow: &Color) {
        if r <= 0 { return; }
        let r2 = r * r;
        let mut count = 0u32;
        for dy in 0..=r {
            let dx = integer_sqrt(r2 - dy * dy);
            let (py, px_start, px_end) = match quadrant {
                1 => (cy - dy, cx + dx, cx + r),
                2 => (cy + dy, cx + dx, cx + r),
                3 => (cy + dy, cx - r, cx - dx),
                4 => (cy - dy, cx - r, cx - dx),
                _ => continue,
            };
            if px_end >= px_start {
                for px in px_start..=px_end {
                    // 发光 (4 邻居)
                    for &(ox, oy) in &[(-1i32, 0i32), (1, 0), (0, -1), (0, 1)] {
                        self.draw_pixel(px + ox, py + oy, glow);
                    }
                    self.draw_pixel(px, py, color);
                    count += 1;
                    if count % ANIM_PIXELS_PER_FRAME == 0 {
                        self.flip();
                        Self::delay_ms(ANIM_DELAY_MS);
                    }
                }
            }
        }
    }

    pub fn draw_text_animated(&mut self, x: i32, y: i32, text: &[u8], color: &Color) {
        let mut cur_x = x;
        for &ch in text {
            if ch == b'\0' { break; }
            self.draw_text(cur_x, y, &[ch], color);
            self.flip();
            Self::delay_ms(30);
            cur_x += 8;
        }
    }

    pub fn draw_screen_border_animated(&mut self, color: &Color) {
        let w = self.fb_info.width as i32;
        let h = self.fb_info.height as i32;
        let margin = 4;
        self.draw_line_animated(margin, margin, w - 1 - margin, margin, color);
        self.draw_line_animated(w - 1 - margin, margin, w - 1 - margin, h - 1 - margin, color);
        self.draw_line_animated(w - 1 - margin, h - 1 - margin, margin, h - 1 - margin, color);
        self.draw_line_animated(margin, h - 1 - margin, margin, margin, color);
    }

    /// 带发光效果的动画屏幕边框
    pub fn draw_screen_border_animated_glow(&mut self, color: &Color, glow: &Color) {
        let w = self.fb_info.width as i32;
        let h = self.fb_info.height as i32;
        let margin = 4;
        self.draw_line_animated_glow(margin, margin, w - 1 - margin, margin, color, glow);
        self.draw_line_animated_glow(w - 1 - margin, margin, w - 1 - margin, h - 1 - margin, color, glow);
        self.draw_line_animated_glow(w - 1 - margin, h - 1 - margin, margin, h - 1 - margin, color, glow);
        self.draw_line_animated_glow(margin, h - 1 - margin, margin, margin, color, glow);
    }

    pub fn draw_screen_border(&mut self, color: &Color) {
        let w = self.fb_info.width as i32;
        let h = self.fb_info.height as i32;
        let margin = 4;
        self.draw_rect(margin, margin, w - 2 * margin, h - 2 * margin, color);
    }

    // ─────────────────────────────────────────────
    // 淡出效果
    // ─────────────────────────────────────────────

    /// 将画面渐暗 (每步将所有像素通道乘以 factor/255)
    fn darken(&mut self, amount: u8) {
        let w = self.fb_info.width as usize;
        let h = self.fb_info.height as usize;
        let factor = 255 - amount as u32;

        for y in 0..h {
            for x in 0..w {
                let idx = y * self.stride + x;
                if idx >= self.backbuf.len() { break; }
                let base = self.backbuf[idx];
                if self.is_bgr {
                    let r = (((base) & 0xFF) * factor / 255) as u32;
                    let g = (((base >> 8) & 0xFF) * factor / 255) as u32;
                    let b = (((base >> 16) & 0xFF) * factor / 255) as u32;
                    self.backbuf[idx] = (0xFFu32 << 24) | (b << 16) | (g << 8) | r;
                } else {
                    let r = (((base >> 16) & 0xFF) * factor / 255) as u32;
                    let g = (((base >> 8) & 0xFF) * factor / 255) as u32;
                    let b = ((base & 0xFF) * factor / 255) as u32;
                    self.backbuf[idx] = (0xFFu32 << 24) | (r << 16) | (g << 8) | b;
                }
            }
        }
        self.dirty_y0 = 0;
        self.dirty_y1 = h as i32;
    }

    /// 渐变到黑色
    pub fn fade_to_black(&mut self, steps: u32, step_delay_ms: u32) {
        let darken_per_step = 255 / steps.max(1);
        for _ in 0..steps {
            self.darken(darken_per_step as u8);
            self.flip_full();
            Self::delay_ms(step_delay_ms);
        }
        // 确保完全黑屏
        self.clear(&BG_DARK);
        self.flip_full();
    }

    // ─────────────────────────────────────────────
    // 高级动画工具
    // ─────────────────────────────────────────────

    /// 绘制带流光效果的进度条
    /// `progress` 范围 0.0~1.0, `time_ms` 用于驱动流光相位
    pub fn draw_progress_bar_glow(
        &mut self,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        progress: f32,
        time_ms: u64,
    ) {
        // 背景轨道
        self.fill_rect(x, y, w, h, &DIM);

        let fill_w = ((w as f32) * progress.clamp(0.0, 1.0)) as i32;
        if fill_w <= 0 {
            return;
        }

        // 主填充
        self.fill_rect(x, y, fill_w, h, &CYAN);

        // 上下发光
        self.draw_line(x, y - 1, x + fill_w, y - 1, &GLOW_CYAN);
        self.draw_line(x, y + h, x + fill_w, y + h, &GLOW_CYAN);

        // 流光高亮 (在填充区域内移动的亮带)
        let shimmer_w = 40i32;
        let cycle = 1200u64;
        let phase = (time_ms % cycle) as i32;
        let shimmer_x = x + phase * (fill_w + shimmer_w) / (cycle as i32) - shimmer_w;
        let sx0 = shimmer_x.max(x);
        let sx1 = (shimmer_x + shimmer_w).min(x + fill_w);
        if sx1 > sx0 {
            for sx in sx0..sx1 {
                let dist = ((sx - shimmer_x) * 255 / shimmer_w).abs();
                let alpha = 255 - dist.min(255);
                let r = (alpha as u32 * 255 / 255) as u8;
                let g = (alpha as u32 * 255 / 255) as u8;
                let b = (alpha as u32 * 255 / 255) as u8;
                self.draw_pixel(sx, y, &Color::rgb(r, g, b));
                self.draw_pixel(sx, y + h - 1, &Color::rgb(r, g, b));
            }
        }
    }

    /// 绘制旋转加载圈 (8 段圆弧, 逐段渐暗)
    /// `cx, cy` 圆心, `radius` 半径, `time_ms` 驱动旋转
    pub fn draw_spinner(&mut self, cx: i32, cy: i32, radius: i32, time_ms: u64) {
        let segments = 8i32;
        let cycle = 800u64;
        let offset = (time_ms % cycle) as i32 * segments / (cycle as i32);

        for i in 0..segments {
            let angle_idx = (i + offset) % segments;
            let brightness = 255 - (angle_idx * 255 / segments);
            let c = Color::rgb(
                (brightness as u32 * 0 / 255) as u8,
                (brightness as u32 * 255 / 255) as u8,
                (brightness as u32 * 255 / 255) as u8,
            );

            let a0 = (i as f32) * (2.0 * 3.14159 / segments as f32);
            let a1 = ((i + 1) as f32) * (2.0 * 3.14159 / segments as f32);
            let steps = 4;
            for s in 0..steps {
                let t = s as f32 / steps as f32;
                let a = a0 + (a1 - a0) * t;
                // 使用手动 cos/sin 近似
                let px = cx + (cos_approx(a) * radius as f32) as i32;
                let py = cy + (sin_approx(a) * radius as f32) as i32;
                self.draw_pixel(px, py, &c);
                self.draw_pixel(px + 1, py, &c);
                self.draw_pixel(px, py + 1, &c);
            }
        }
    }

    /// 绘制脉冲发光边框 (呼吸效果)
    /// `time_ms` 驱动脉冲相位, `speed` 越大越快
    pub fn draw_pulse_rect_glow(
        &mut self,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        r: i32,
        base_color: &Color,
        glow_color: &Color,
        time_ms: u64,
        speed: u32,
    ) {
        let cycle = (1000u64).max(speed as u64 * 2);
        let phase = (time_ms % cycle) as f32 / cycle as f32;
        // 使用 sin_approx 近似正弦波: 0->1->0 平滑过渡
        let pulse = (sin_approx(phase * 3.14159) * 0.5 + 0.5);

        // 边框线本身也参与呼吸 (亮度 0.3 ~ 1.0)
        let border_intensity = pulse * 0.7 + 0.3;
        let pulsed_border = Color::rgb(
            ((base_color.r as f32) * border_intensity) as u8,
            ((base_color.g as f32) * border_intensity) as u8,
            ((base_color.b as f32) * border_intensity) as u8,
        );

        // 发光色变化更大 (0.1 ~ 1.0), 低谷时几乎消失, 高峰时全亮
        let glow_intensity = pulse * 0.9 + 0.1;
        let enhanced_glow = Color::rgb(
            ((glow_color.r as f32) * glow_intensity) as u8,
            ((glow_color.g as f32) * glow_intensity) as u8,
            ((glow_color.b as f32) * glow_intensity) as u8,
        );

        if r > 0 {
            self.draw_rounded_rect_glow(x, y, w, h, r, &pulsed_border, &enhanced_glow);
        } else {
            self.draw_rect_glow(x, y, w, h, &pulsed_border, &enhanced_glow);
        }
    }

    /// 绘制粒子 (单个)
    pub fn draw_particle(&mut self, px: i32, py: i32, size: i32, color: &Color) {
        if size <= 1 {
            self.draw_pixel(px, py, color);
        } else {
            let half = size / 2;
            self.fill_rect(px - half, py - half, size, size, color);
        }
    }
}

// =============================================================================
// 缓动函数 (Easing Functions)
// =============================================================================

/// 线性插值
pub fn lerp(a: f32, b: f32, t: f32) -> f32 {
    a + (b - a) * t
}

/// 线性缓动
pub fn ease_linear(t: f32) -> f32 {
    t.clamp(0.0, 1.0)
}

/// ease-in (加速)
pub fn ease_in(t: f32) -> f32 {
    let t = t.clamp(0.0, 1.0);
    t * t
}

/// ease-out (减速)
pub fn ease_out(t: f32) -> f32 {
    let t = t.clamp(0.0, 1.0);
    1.0 - (1.0 - t) * (1.0 - t)
}

/// ease-in-out (先加速后减速)
pub fn ease_in_out(t: f32) -> f32 {
    let t = t.clamp(0.0, 1.0);
    if t < 0.5 {
        2.0 * t * t
    } else {
        1.0 - 2.0 * (1.0 - t) * (1.0 - t)
    }
}

/// ease-out-back (轻微回弹)
pub fn ease_out_back(t: f32) -> f32 {
    let t = t.clamp(0.0, 1.0);
    let c1 = 1.70158;
    let c3 = c1 + 1.0;
    // 使用手动立方和平方代替 powi
    let tm1 = t - 1.0;
    1.0 + c3 * tm1 * tm1 * tm1 + c1 * tm1 * tm1
}

/// ease-out-elastic (弹性)
pub fn ease_out_elastic(t: f32) -> f32 {
    let t = t.clamp(0.0, 1.0);
    if t == 0.0 {
        0.0
    } else if t == 1.0 {
        1.0
    } else {
        let c4 = (2.0 * 3.14159) / 3.0;
        // 使用手动实现的 sin 和 powf (避免 std 依赖)
        let arg = (t - 0.075) * c4;
        let sin_val = sin_approx(arg);
        // 2^(-10t) = exp(-10t * ln2), 简化为近似
        let pow_val = exp_approx(-10.0 * t * 0.693147);
        pow_val * sin_val + 1.0
    }
}

/// ease-out-bounce (弹跳)
pub fn ease_out_bounce(t: f32) -> f32 {
    let t = t.clamp(0.0, 1.0);
    let n1 = 7.5625;
    let d1 = 2.75;
    if t < 1.0 / d1 {
        n1 * t * t
    } else if t < 2.0 / d1 {
        let t = t - 1.5 / d1;
        n1 * t * t + 0.75
    } else if t < 2.5 / d1 {
        let t = t - 2.25 / d1;
        n1 * t * t + 0.9375
    } else {
        let t = t - 2.625 / d1;
        n1 * t * t + 0.984375
    }
}

/// 正弦近似 (泰勒级数, 精度足够用于动画)
fn sin_approx(x: f32) -> f32 {
    // 归一化到 [-pi, pi]
    let pi = 3.14159265;
    let two_pi = 2.0 * pi;
    let mut x = x % two_pi;
    if x > pi { x -= two_pi; }
    if x < -pi { x += two_pi; }
    // 泰勒级数: sin(x) = x - x^3/6 + x^5/120 - x^7/5040
    let x2 = x * x;
    let x3 = x2 * x;
    let x5 = x3 * x2;
    let x7 = x5 * x2;
    x - x3 / 6.0 + x5 / 120.0 - x7 / 5040.0
}

/// 余弦近似 (基于 sin)
fn cos_approx(x: f32) -> f32 {
    sin_approx(x + 3.14159265 / 2.0)
}

/// 指数近似 (用于 powf)
fn exp_approx(x: f32) -> f32 {
    // e^x ≈ 1 + x + x^2/2 + x^3/6 + x^4/24
    // 对于负 x, 结果趋近 0
    if x < -20.0 { return 0.0; }
    let x2 = x * x;
    let x3 = x2 * x;
    let x4 = x3 * x;
    1.0 + x + x2 / 2.0 + x3 / 6.0 + x4 / 24.0
}

// =============================================================================
// 粒子系统
// =============================================================================

/// 粒子最大数量
pub const MAX_PARTICLES: usize = 64;

/// 粒子结构体
#[derive(Clone, Copy)]
pub struct Particle {
    pub x: f32,
    pub y: f32,
    pub vx: f32,
    pub vy: f32,
    pub life: f32,
    pub max_life: f32,
    pub size: i32,
    pub color: Color,
}

/// 粒子系统
pub struct ParticleSystem {
    pub particles: [Particle; MAX_PARTICLES],
    pub count: usize,
}

impl ParticleSystem {
    pub fn new() -> Self {
        ParticleSystem {
            particles: [Particle {
                x: 0.0, y: 0.0, vx: 0.0, vy: 0.0,
                life: 0.0, max_life: 0.0, size: 1,
                color: Color::rgb(0, 255, 255),
            }; MAX_PARTICLES],
            count: 0,
        }
    }

    /// 生成一个粒子
    pub fn spawn(&mut self, x: f32, y: f32, vx: f32, vy: f32, life: f32, size: i32, color: Color) {
        if self.count >= MAX_PARTICLES {
            // 回收最老的粒子
            self.count = 0;
        }
        let p = &mut self.particles[self.count];
        p.x = x;
        p.y = y;
        p.vx = vx;
        p.vy = vy;
        p.life = life;
        p.max_life = life;
        p.size = size;
        p.color = color;
        self.count += 1;
    }

    /// 更新所有粒子 (dt_ms 毫秒)
    pub fn update(&mut self, dt_ms: u32) {
        let dt = dt_ms as f32 / 1000.0;
        for i in 0..self.count {
            let p = &mut self.particles[i];
            if p.life <= 0.0 { continue; }
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            p.life -= dt;
        }
    }

    /// 绘制所有活跃粒子
    pub fn draw(&self, ctx: &mut DrawContext) {
        for i in 0..self.count {
            let p = &self.particles[i];
            if p.life <= 0.0 { continue; }
            let alpha = (p.life / p.max_life).clamp(0.0, 1.0);
            let c = Color::rgb(
                ((p.color.r as f32) * alpha) as u8,
                ((p.color.g as f32) * alpha) as u8,
                ((p.color.b as f32) * alpha) as u8,
            );
            ctx.draw_particle(p.x as i32, p.y as i32, p.size, &c);
        }
    }

    /// 生成背景流动粒子 (从底部向上飘)
    pub fn spawn_ambient(&mut self, screen_w: i32, screen_h: i32, time_ms: u64) {
        if self.count < MAX_PARTICLES && time_ms % 100 < 16 {
            let x = (time_ms as f32 * 0.7) as i32 % screen_w;
            self.spawn(
                x as f32,
                screen_h as f32,
                (time_ms as f32 * 0.01) % 2.0 - 1.0,
                -20.0 - (time_ms as f32 * 0.001) % 30.0,
                3.0,
                2,
                CYAN,
            );
        }
    }
}

// =============================================================================
// Cursor - 鼠标光标管理
// =============================================================================

const CURSOR_W: usize = 12;
const CURSOR_H: usize = 16;

/// 标准箭头光标形状 (12x16, MSB 在左)
const CURSOR_SHAPE: [u16; 16] = [
    0b1000_0000_0000_0000,
    0b1100_0000_0000_0000,
    0b1110_0000_0000_0000,
    0b1111_0000_0000_0000,
    0b1111_1000_0000_0000,
    0b1111_1100_0000_0000,
    0b1111_1110_0000_0000,
    0b1111_1111_0000_0000,
    0b1111_1100_0000_0000,
    0b1101_1000_0000_0000,
    0b1001_1000_0000_0000,
    0b0000_1100_0000_0000,
    0b0000_1100_0000_0000,
    0b0000_0110_0000_0000,
    0b0000_0110_0000_0000,
    0b0000_0010_0000_0000,
];

pub struct Cursor {
    pub x: i32,
    pub y: i32,
    pub visible: bool,
    save_buf: [u32; CURSOR_W * CURSOR_H],
    saved: bool,
    old_x: i32,
    old_y: i32,
}

impl Cursor {
    pub fn new() -> Self {
        Cursor {
            x: 0,
            y: 0,
            visible: false,
            save_buf: [0u32; CURSOR_W * CURSOR_H],
            saved: false,
            old_x: 0,
            old_y: 0,
        }
    }

    /// 保存光标下方的像素
    pub fn save(&mut self, ctx: &DrawContext) {
        for dy in 0..CURSOR_H {
            for dx in 0..CURSOR_W {
                let px = self.x + dx as i32;
                let py = self.y + dy as i32;
                self.save_buf[dy * CURSOR_W + dx] = ctx.read_pixel_raw(px, py);
            }
        }
        self.saved = true;
        self.old_x = self.x;
        self.old_y = self.y;
    }

    /// 恢复之前保存的像素
    pub fn restore(&mut self, ctx: &mut DrawContext) {
        if !self.saved { return; }
        for dy in 0..CURSOR_H {
            for dx in 0..CURSOR_W {
                let px = self.old_x + dx as i32;
                let py = self.old_y + dy as i32;
                ctx.draw_pixel_raw(px, py, self.save_buf[dy * CURSOR_W + dx]);
            }
        }
        self.saved = false;
    }

    /// 绘制光标 (白色箭头)
    pub fn draw(&mut self, ctx: &mut DrawContext) {
        if !self.visible { return; }
        // 先保存
        self.save(ctx);
        // 绘制光标像素
        for dy in 0..CURSOR_H {
            let row_bits = CURSOR_SHAPE[dy];
            for dx in 0..CURSOR_W {
                if row_bits & (0x8000 >> dx) != 0 {
                    let px = self.x + dx as i32;
                    let py = self.y + dy as i32;
                    ctx.draw_pixel(px, py, &WHITE);
                }
            }
        }
    }

    /// 更新光标位置 (先恢复旧位置, 再保存并绘制新位置)
    pub fn update_position(&mut self, new_x: i32, new_y: i32, ctx: &mut DrawContext) {
        self.restore(ctx);
        self.x = new_x;
        self.y = new_y;
        self.draw(ctx);
    }
}

// =============================================================================
// 8x16 像素位图字体数据
// =============================================================================

static FONT_8X16: [u8; 2048] = [
    // 0x00 - 0x1F: 控制字符 (空白)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 0x20: 空格
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x18, 0x3C, 0x3C, 0x3C, 0x18, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00,
    0x00, 0x66, 0x66, 0x66, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x6C, 0x6C, 0xFE, 0x6C, 0x6C, 0x6C, 0xFE, 0x6C, 0x6C, 0x00, 0x00, 0x00, 0x00,
    0x18, 0x18, 0x7C, 0xC6, 0xC2, 0xC0, 0x7C, 0x06, 0x06, 0x86, 0xC6, 0x7C, 0x18, 0x18, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xC2, 0xC6, 0x0C, 0x18, 0x30, 0x60, 0xC6, 0x86, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x38, 0x6C, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0xCC, 0xCC, 0x76, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x30, 0x30, 0x30, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x30, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7C, 0xC6, 0xCE, 0xDE, 0xF6, 0xE6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x18, 0x38, 0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7C, 0xC6, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0xC6, 0xFE, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7C, 0xC6, 0x06, 0x06, 0x3C, 0x06, 0x06, 0x06, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0C, 0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x0C, 0x0C, 0x1E, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xFE, 0xC0, 0xC0, 0xC0, 0xFC, 0x06, 0x06, 0x06, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x38, 0x60, 0xC0, 0xC0, 0xFC, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xFE, 0xC6, 0x06, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0x7E, 0x06, 0x06, 0x06, 0x0C, 0x78, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7C, 0xC6, 0xC6, 0x0C, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xDE, 0xDE, 0xDE, 0xDC, 0xC0, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x10, 0x38, 0x6C, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xFC, 0x66, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x66, 0x66, 0xFC, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x3C, 0x66, 0xC2, 0xC0, 0xC0, 0xC0, 0xC0, 0xC2, 0x66, 0x3C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xF8, 0x6C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x6C, 0xF8, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xFE, 0x66, 0x62, 0x68, 0x78, 0x68, 0x60, 0x62, 0x66, 0xFE, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xFE, 0x66, 0x62, 0x68, 0x78, 0x68, 0x60, 0x60, 0x60, 0xF0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x3C, 0x66, 0xC2, 0xC0, 0xC0, 0xDE, 0xC6, 0xC6, 0x66, 0x3A, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0xCC, 0x78, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xE6, 0x66, 0x66, 0x6C, 0x78, 0x78, 0x6C, 0x66, 0x66, 0xE6, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xF0, 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x62, 0x66, 0xFE, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xC6, 0xEE, 0xFE, 0xFE, 0xD6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xC6, 0xE6, 0xF6, 0xFE, 0xDE, 0xCE, 0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xFC, 0x66, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x60, 0xF0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xD6, 0xDE, 0x7C, 0x06, 0x06, 0x00, 0x00,
    0x00, 0x00, 0xFC, 0x66, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x66, 0xE6, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC0, 0x70, 0x1C, 0x06, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x7E, 0x5A, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x10, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0xD6, 0xD6, 0xD6, 0xFE, 0xEE, 0x6C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xC6, 0xC6, 0x6C, 0x6C, 0x38, 0x38, 0x6C, 0x6C, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xFE, 0xC6, 0x86, 0x0C, 0x18, 0x30, 0x60, 0xC2, 0xC6, 0xFE, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
    0x00, 0x30, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x0C, 0x7C, 0xCC, 0xCC, 0xCC, 0x76, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xE0, 0x60, 0x60, 0x78, 0x6C, 0x66, 0x66, 0x66, 0x66, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC0, 0xC0, 0xC0, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x1C, 0x0C, 0x0C, 0x3C, 0x6C, 0xCC, 0xCC, 0xCC, 0xCC, 0x76, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0xC6, 0xFE, 0xC0, 0xC0, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x1C, 0x36, 0x30, 0x30, 0x78, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x7C, 0x0C, 0xCC, 0x78, 0x00,
    0x00, 0x00, 0xE0, 0x60, 0x60, 0x6C, 0x76, 0x66, 0x66, 0x66, 0x66, 0xE6, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x18, 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x06, 0x06, 0x00, 0x0E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x66, 0x66, 0x3C, 0x00, 0x00,
    0x00, 0x00, 0xE0, 0x60, 0x60, 0x66, 0x6C, 0x78, 0x78, 0x6C, 0x66, 0xE6, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xEC, 0xFE, 0xD6, 0xD6, 0xD6, 0xD6, 0xC6, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xDC, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xDC, 0x66, 0x66, 0x66, 0x66, 0x66, 0x7C, 0x60, 0x60, 0xF0, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x7C, 0x0C, 0x0C, 0x1E, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xDC, 0x76, 0x66, 0x60, 0x60, 0x60, 0xF0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0xC6, 0xC0, 0x70, 0x1C, 0xC6, 0x7C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x10, 0x30, 0x30, 0xFC, 0x30, 0x30, 0x30, 0x30, 0x36, 0x1C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x76, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xC6, 0xC6, 0xD6, 0xD6, 0xD6, 0xFE, 0x6C, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xC6, 0x6C, 0x38, 0x38, 0x6C, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xCE, 0x76, 0x06, 0xC6, 0x7C, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0xCE, 0x98, 0x18, 0x30, 0x62, 0xFE, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0E, 0x18, 0x18, 0x18, 0x70, 0x18, 0x18, 0x18, 0x18, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x70, 0x18, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x18, 0x18, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
];
