// =============================================================================
// fileman 模块 - eDEXUI 风格文件管理器
// =============================================================================
// 设计:
//   - CYAN 边框的文件浏览区域
//   - 目录列表: [D] 文件夹 / [F] 文件 图标
//   - 鼠标点击导航 (点击文件夹进入, 点击文件显示信息)
//   - 面包屑路径导航
//   - 通过 SYS_READDIR(29) 读取目录项
// =============================================================================

use crate::ui::{
    DrawContext, CYAN, BG_PANEL, TEXT, TEXT_DIM, GREEN, GLOW_CYAN, WHITE,
    syscall1, syscall2, syscall3, buf_len,
};

/// 系统调用号
const SYS_OPENDIR: u64 = 28;
const SYS_CLOSEDIR: u64 = 30;
const SYS_READ: u64 = 1;
const SYS_READDIR: u64 = 29;
const SYS_STAT: u64 = 6;

/// 目录项结构体 (与内核 fm_dirent 对应)
/// 总大小 275 字节:
///   offset 0:   d_ino (u64, 8字节)
///   offset 8:   d_off (u64, 8字节)
///   offset 16:  d_reclen (u16, 2字节)
///   offset 18:  d_name (256字节)
///   offset 274: d_type (u8, 1字节)
#[repr(C)]
#[derive(Clone, Copy)]
struct FmDirent {
    d_ino: u64,
    d_off: u64,
    d_reclen: u16,
    d_name: [u8; 256],
    d_type: u8,
}

const DIRENT_SIZE: usize = 275;
const MAX_ENTRIES: usize = 32;
const MAX_PATH_LEN: usize = 256;

/// 文件类型 (与内核 fm_dirent.type 对应)
const FM_TYPE_FILE: u8 = 1;
const FM_TYPE_DIR: u8 = 2;

/// stat 结构体 (与内核对应, packed)
#[repr(C, packed)]
struct FileStat {
    st_dev: u64,      // 8
    st_ino: u64,      // 8
    st_mode: u32,     // 4
    st_nlink: u32,    // 4
    st_uid: u32,      // 4
    st_gid: u32,      // 4
    st_rdev: u64,     // 8
    st_size: u64,     // 8
    st_atime: u64,    // 8
    st_mtime: u64,    // 8
    st_ctime: u64,    // 8
    st_blksize: u64,  // 8
    st_blocks: u64,   // 8
}

// =============================================================================
// FileManState - 文件管理器状态
// =============================================================================

pub struct FileManState {
    /// 当前路径
    current_path: [u8; MAX_PATH_LEN],
    /// 当前路径有效长度
    path_len: usize,
    /// 目录项列表
    entries: [FmDirent; MAX_ENTRIES],
    /// 目录项数量
    entry_count: usize,
    /// 选中项索引
    selected: i32,
    /// 滚动偏移
    scroll_offset: i32,
    /// 文件管理器区域坐标
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

impl FileManState {
    pub fn new(x: i32, y: i32, w: i32, h: i32, is_root: bool) -> Self {
        let mut state = FileManState {
            current_path: [0u8; MAX_PATH_LEN],
            path_len: 0,
            entries: [unsafe { core::mem::zeroed() }; MAX_ENTRIES],
            entry_count: 0,
            selected: -1,
            scroll_offset: 0,
            x,
            y,
            w,
            h,
        };
        // 基于用户身份设置默认路径
        if is_root {
            state.set_path(b"/desktop/root");
        } else {
            state.set_path(b"/desktop/user");
        }
        state.refresh();
        state
    }

    /// 设置当前路径
    fn set_path(&mut self, path: &[u8]) {
        let len = path.len().min(MAX_PATH_LEN);
        self.current_path = [0u8; MAX_PATH_LEN];
        self.current_path[..len].copy_from_slice(&path[..len]);
        self.path_len = len;
    }

    /// 刷新目录列表
    pub fn refresh(&mut self) {
        self.entries = [unsafe { core::mem::zeroed() }; MAX_ENTRIES];
        self.entry_count = 0;
        self.selected = -1;
        self.scroll_offset = 0;

        // 确保路径以 \0 结尾
        let path_null_terminated = if self.path_len < MAX_PATH_LEN - 1 {
            let mut p = self.current_path;
            p[self.path_len] = 0;
            p
        } else {
            self.current_path
        };

        unsafe {
            let fd = syscall1(SYS_OPENDIR, path_null_terminated.as_ptr() as u64);
            if fd == u64::MAX || (fd as i64) < 0 {
                return;
            }

            // 读取目录项
            let mut buf = [0u8; DIRENT_SIZE * MAX_ENTRIES];
            let mut total = 0usize;

            for i in 0..MAX_ENTRIES {
                let n = syscall3(
                    SYS_READDIR,
                    fd,
                    buf.as_mut_ptr().add(total) as u64,
                    DIRENT_SIZE as u64,
                );
                if n == 0 || (n as i64) < 0 {
                    break;
                }
                // 解析目录项
                let offset = total;
                if offset + DIRENT_SIZE > buf.len() {
                    break;
                }
                let entry = &buf[offset..offset + DIRENT_SIZE];
                self.entries[i].d_ino = u64::from_le_bytes([
                    entry[0], entry[1], entry[2], entry[3],
                    entry[4], entry[5], entry[6], entry[7],
                ]);
                self.entries[i].d_off = u64::from_le_bytes([
                    entry[8], entry[9], entry[10], entry[11],
                    entry[12], entry[13], entry[14], entry[15],
                ]);
                self.entries[i].d_reclen = u16::from_le_bytes([entry[16], entry[17]]);
                self.entries[i].d_name.copy_from_slice(&entry[18..274]);
                self.entries[i].d_type = entry[274];
                self.entry_count += 1;
                total += DIRENT_SIZE;
            }

            syscall1(SYS_CLOSEDIR, fd);
        }
    }

    /// 处理鼠标点击
    pub fn handle_click(&mut self, mx: i32, my: i32) -> FileManAction {
        let x = self.x;
        let y = self.y;
        let w = self.w;
        let h = self.h;

        // 检查点击是否在文件列表区域内
        let list_y_start = y + 40; // 标题 + 面包屑
        let list_y_end = y + h - 4;
        let list_x_start = x + 4;
        let list_x_end = x + w - 4;

        if mx < list_x_start || mx > list_x_end || my < list_y_start || my > list_y_end {
            return FileManAction::None;
        }

        // 计算点击的行号
        let line_h: i32 = 18;
        let click_row = (my - list_y_start) / line_h + self.scroll_offset;

        if click_row < 0 || click_row as usize >= self.entry_count {
            return FileManAction::None;
        }

        self.selected = click_row;

        let entry = &self.entries[click_row as usize];

        // 如果是目录, 进入该目录
        if entry.d_type == FM_TYPE_DIR {
            let name_len = buf_len(&entry.d_name);
            if name_len == 0 {
                return FileManAction::None;
            }

            // 特殊处理: "." 不做任何事
            if name_len == 1 && entry.d_name[0] == b'.' {
                return FileManAction::None;
            }

            // ".." 返回上级目录
            if name_len == 2 && entry.d_name[0] == b'.' && entry.d_name[1] == b'.' {
                self.navigate_up();
                return FileManAction::Navigate;
            }

            // 构建新路径: current_path + "/" + name
            let mut new_path = [0u8; MAX_PATH_LEN];
            let mut pos = self.path_len;
            new_path[..pos].copy_from_slice(&self.current_path[..pos]);

            // 添加分隔符 (如果路径不是以 / 结尾)
            if pos > 0 && new_path[pos - 1] != b'/' {
                if pos < MAX_PATH_LEN - 1 {
                    new_path[pos] = b'/';
                    pos += 1;
                }
            }

            // 添加目录名
            let copy_len = name_len.min(MAX_PATH_LEN - pos);
            new_path[pos..pos + copy_len].copy_from_slice(&entry.d_name[..copy_len]);
            pos += copy_len;

            self.set_path(&new_path[..pos]);
            self.refresh();
            return FileManAction::Navigate;
        }

        FileManAction::Select
    }

    /// 返回上级目录
    fn navigate_up(&mut self) {
        if self.path_len <= 1 {
            return; // 已经在根目录
        }

        // 找到最后一个 /
        let mut last_slash = 0usize;
        for i in (0..self.path_len).rev() {
            if self.current_path[i] == b'/' {
                last_slash = i;
                break;
            }
        }

        if last_slash == 0 {
            // 回到根目录
            self.set_path(b"/");
        } else {
            // 复制路径到临时缓冲区避免借用冲突
            let mut tmp = [0u8; MAX_PATH_LEN];
            tmp[..last_slash].copy_from_slice(&self.current_path[..last_slash]);
            self.set_path(&tmp[..last_slash]);
        }
        self.refresh();
    }

    /// 获取文件的 stat 信息
    fn stat_entry(&self, entry: &FmDirent) -> Option<FileStat> {
        // 构建完整路径
        let name_len = buf_len(&entry.d_name);
        if name_len == 0 {
            return None;
        }

        let mut full_path = [0u8; MAX_PATH_LEN];
        let mut pos = self.path_len;
        full_path[..pos].copy_from_slice(&self.current_path[..pos]);

        if pos > 0 && full_path[pos - 1] != b'/' {
            if pos < MAX_PATH_LEN - 1 {
                full_path[pos] = b'/';
                pos += 1;
            }
        }

        let copy_len = name_len.min(MAX_PATH_LEN - pos);
        full_path[pos..pos + copy_len].copy_from_slice(&entry.d_name[..copy_len]);
        pos += copy_len;

        // 确保以 \0 结尾
        if pos < MAX_PATH_LEN {
            full_path[pos] = 0;
        }

        let mut stat_buf: FileStat = unsafe { core::mem::zeroed() };

        unsafe {
            let ret = syscall2(SYS_STAT, full_path.as_ptr() as u64, &mut stat_buf as *mut FileStat as u64);
            if ret == u64::MAX || (ret as i64) < 0 {
                return None;
            }
        }

        Some(stat_buf)
    }

    /// 绘制文件管理器
    pub fn draw(&mut self, ctx: &mut DrawContext) {
        let x = self.x;
        let y = self.y;
        let w = self.w;
        let h = self.h;

        // 背景
        ctx.fill_rect(x + 1, y + 1, w - 2, h - 2, &BG_PANEL);

        // 边框 (带发光)
        ctx.draw_rect_glow(x, y, w, h, &CYAN, &GLOW_CYAN);

        // 标题栏
        ctx.draw_text(x + 8, y + 4, b"FILES", &CYAN);

        // 面包屑路径
        let crumb_y = y + 20;
        ctx.draw_hline_dashed(x + 4, x + w - 4, crumb_y, 4, 4, &GLOW_CYAN);
        if self.path_len > 0 {
            // 路径可能很长, 截断显示
            let max_display = ((w - 16) / 8) as usize;
            let display_path = if self.path_len > max_display {
                &self.current_path[self.path_len - max_display..]
            } else {
                &self.current_path[..self.path_len]
            };
            ctx.draw_text(x + 6, crumb_y + 4, display_path, &TEXT_DIM);
        }

        // 文件列表
        let list_y = crumb_y + 20;
        let line_h: i32 = 18;
        let max_visible = ((y + h - 4 - list_y) / line_h) as usize;
        let start = self.scroll_offset as usize;
        let end = (start + max_visible).min(self.entry_count);

        for i in start..end {
            let entry = &self.entries[i];
            let row_y = list_y + (i - start) as i32 * line_h;
            let name_len = buf_len(&entry.d_name);
            if name_len == 0 {
                continue;
            }

            // 选中高亮
            if i as i32 == self.selected {
                ctx.fill_rect(x + 2, row_y, w - 4, line_h, &GLOW_CYAN);
            }

            // 文件类型图标
            if entry.d_type == FM_TYPE_DIR {
                ctx.draw_text(x + 6, row_y + 1, b"[D]", &GREEN);
            } else {
                ctx.draw_text(x + 6, row_y + 1, b"[F]", &TEXT_DIM);
            }

            // 文件名 (限制宽度, 为大小和日期留空间)
            let name_max_w = ((w - 56) / 8) as usize;
            let name_display_len = name_len.min(name_max_w);
            ctx.draw_text(
                x + 32,
                row_y + 1,
                &entry.d_name[..name_display_len],
                if i as i32 == self.selected { &WHITE } else { &TEXT },
            );

            // 文件大小和修改时间
            if entry.d_type == FM_TYPE_DIR {
                // 目录: 显示 "--"
                let size_x = x + w - 16 * 8;
                if size_x > x + 32 + name_display_len as i32 * 8 {
                    ctx.draw_text(size_x, row_y + 1, b"--", &TEXT_DIM);
                }
            } else {
                // 普通文件: 获取 stat 信息
                if let Some(stat) = self.stat_entry(entry) {
                    // 从 packed 结构体安全读取 (避免未对齐访问)
                    let st_size = unsafe { core::ptr::read_unaligned(core::ptr::addr_of!(stat.st_size)) };
                    let st_mtime = unsafe { core::ptr::read_unaligned(core::ptr::addr_of!(stat.st_mtime)) };

                    // 格式化文件大小
                    let mut size_str = [0u8; 16];
                    let size_len = format_size(st_size, &mut size_str);

                    // 格式化修改时间 (MM-DD HH:MM)
                    let mut date_str = [0u8; 12];
                    let date_len = format_datetime(st_mtime, &mut date_str);

                    // 绘制日期 (右对齐)
                    let date_x = x + w - date_len as i32 * 8 - 4;
                    if date_x > x + 32 + name_display_len as i32 * 8 {
                        ctx.draw_text(date_x, row_y + 1, &date_str[..date_len], &TEXT_DIM);
                    }

                    // 绘制大小 (日期左侧)
                    let size_x = date_x - size_len as i32 * 8 - 8;
                    if size_x > x + 32 + name_display_len as i32 * 8 {
                        ctx.draw_text(size_x, row_y + 1, &size_str[..size_len], &TEXT_DIM);
                    }
                }
            }
        }

        // 底部状态栏
        let status_y = y + h - 18;
        ctx.draw_hline_dashed(x + 4, x + w - 4, status_y, 4, 4, &GLOW_CYAN);

        // 显示条目数
        let mut count_str = [0u8; 32];
        let count_len = u32_to_bytes(self.entry_count as u32, &mut count_str);
        ctx.draw_text(x + 6, status_y + 2, b"items: ", &TEXT_DIM);
        ctx.draw_text(x + 6 + 7 * 8, status_y + 2, &count_str[..count_len], &TEXT_DIM);
    }
}

/// 文件管理器动作
#[derive(Clone, Copy, PartialEq)]
pub enum FileManAction {
    None,
    Navigate,
    Select,
}

/// u32 转十进制字节
fn u32_to_bytes(mut n: u32, buf: &mut [u8; 32]) -> usize {
    if n == 0 {
        buf[0] = b'0';
        return 1;
    }
    let mut digits = [0u8; 10];
    let mut pos = 0;
    while n > 0 {
        digits[pos] = b'0' + (n % 10) as u8;
        pos += 1;
        n /= 10;
    }
    for i in 0..pos {
        buf[i] = digits[pos - 1 - i];
    }
    pos
}

/// 格式化文件大小 (如 "1.2K", "3.4M", 或字节数)
fn format_size(size: u64, buf: &mut [u8; 16]) -> usize {
    if size < 1024 {
        // 字节数
        u64_to_bytes(size, buf)
    } else if size < 1024 * 1024 {
        // KB
        let kb = size / 1024;
        let frac = (size % 1024) * 10 / 1024;
        let len = u64_to_bytes(kb, buf);
        if len + 3 < 16 {
            buf[len] = b'.';
            buf[len + 1] = b'0' + (frac as u8);
            buf[len + 2] = b'K';
            return len + 3;
        }
        len
    } else if size < 1024 * 1024 * 1024 {
        // MB
        let mb = size / (1024 * 1024);
        let frac = (size % (1024 * 1024)) * 10 / (1024 * 1024);
        let len = u64_to_bytes(mb, buf);
        if len + 3 < 16 {
            buf[len] = b'.';
            buf[len + 1] = b'0' + (frac as u8);
            buf[len + 2] = b'M';
            return len + 3;
        }
        len
    } else {
        // GB
        let gb = size / (1024 * 1024 * 1024);
        let frac = (size % (1024 * 1024 * 1024)) * 10 / (1024 * 1024 * 1024);
        let len = u64_to_bytes(gb, buf);
        if len + 3 < 16 {
            buf[len] = b'.';
            buf[len + 1] = b'0' + (frac as u8);
            buf[len + 2] = b'G';
            return len + 3;
        }
        len
    }
}

/// u64 转十进制字节
fn u64_to_bytes(mut n: u64, buf: &mut [u8; 16]) -> usize {
    if n == 0 {
        buf[0] = b'0';
        return 1;
    }
    let mut digits = [0u8; 20];
    let mut pos = 0;
    while n > 0 {
        digits[pos] = b'0' + (n % 10) as u8;
        pos += 1;
        n /= 10;
    }
    for i in 0..pos {
        buf[i] = digits[pos - 1 - i];
    }
    pos
}

/// 格式化时间戳为 "MM-DD HH:MM"
/// 时间戳为 Unix 时间戳 (秒)
fn format_datetime(timestamp: u64, buf: &mut [u8; 12]) -> usize {
    // 简化的 Unix 时间戳转换
    // 从 1970-01-01 开始计算
    let mut days = timestamp / 86400;
    let secs_in_day = timestamp % 86400;
    let hour = (secs_in_day / 3600) as u32;
    let minute = ((secs_in_day % 3600) / 60) as u32;

    // 计算年份 (简化, 不处理闰秒)
    let mut year = 1970u32;
    loop {
        let days_in_year = if is_leap_year(year) { 366 } else { 365 };
        if days < days_in_year as u64 {
            break;
        }
        days -= days_in_year as u64;
        year += 1;
    }

    // 计算月份
    let days_in_month = [31, if is_leap_year(year) { 29 } else { 28 }, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    let mut month = 0u32;
    let mut remaining_days = days as u32;
    for m in 0..12 {
        if remaining_days < days_in_month[m] {
            month = m as u32;
            break;
        }
        remaining_days -= days_in_month[m];
    }
    let day = remaining_days + 1; // 1-based

    // 格式: MM-DD HH:MM
    buf[0] = b'0' + ((month + 1) / 10) as u8;
    buf[1] = b'0' + ((month + 1) % 10) as u8;
    buf[2] = b'-';
    buf[3] = b'0' + (day / 10) as u8;
    buf[4] = b'0' + (day % 10) as u8;
    buf[5] = b' ';
    buf[6] = b'0' + (hour / 10) as u8;
    buf[7] = b'0' + (hour % 10) as u8;
    buf[8] = b':';
    buf[9] = b'0' + (minute / 10) as u8;
    buf[10] = b'0' + (minute % 10) as u8;
    11
}

/// 判断闰年
fn is_leap_year(year: u32) -> bool {
    (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)
}
