// =============================================================================
// shell 模块 - eDEXUI 风格内嵌终端
// =============================================================================
// 设计:
//   - CYAN 边框的终端区域, 深色内部
//   - 闪烁光标 (500ms 间隔)
//   - 键盘输入实时显示
//   - AI 输入框: "AI>" 提示, 调用 SYS_AI_SEND(180) / SYS_AI_RECV(181)
//   - 终端输出缓冲区 (环形, 最多 50 行 x 120 列)
//   - 完整内置命令: cd, pwd, ls, cat, echo, clear, mkdir, rm, touch, whoami, uname, help
//   - 外部程序执行: fork+exec
// =============================================================================

use crate::ui::{
    DrawContext, CYAN, BG_DARK, BG_PANEL, TEXT, TEXT_DIM, GREEN, GLOW_CYAN, WHITE, YELLOW,
    scancode_to_ascii, syscall2, syscall3,
};

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
const SYS_READ: u64 = 1;
const SYS_WRITE: u64 = 2;
const SYS_OPEN: u64 = 3;
const SYS_CLOSE: u64 = 4;
const SYS_EXEC: u64 = 7;
const SYS_GETCWD: u64 = 22;
const SYS_CHDIR: u64 = 23;
const SYS_IOCTL: u64 = 24;
const SYS_OPENDIR: u64 = 28;
const SYS_READDIR: u64 = 29;
const SYS_CLOSEDIR: u64 = 30;
const SYS_FORK: u64 = 31;
const SYS_UNLINK: u64 = 37;
const SYS_MKDIR: u64 = 38;
const SYS_WAITPID: u64 = 19;
const SYS_EXIT: u64 = 0;
const SYS_GETUID: u64 = 190;
const SYS_AI_SEND: u64 = 180;
const SYS_AI_RECV: u64 = 181;
const SYS_INPUT_POLL: u64 = 188;

/// O_CREAT 标志
const O_CREAT: u64 = 0x40;

/// 终端参数
const MAX_LINES: usize = 50;
const MAX_COLS: usize = 120;
const AI_INPUT_MAX: usize = 256;
const AI_OUTPUT_MAX: usize = 2048;
const AI_CHAT_LINES: usize = 32;
const AI_CHAT_LINE_LEN: usize = 200;

/// 目录项大小 (FmDirent)
const DIRENT_SIZE: usize = 275;

/// 键盘事件类型
const EVT_KEY_DOWN: u32 = 1;

// =============================================================================
// ShellState - 终端状态
// =============================================================================

pub struct ShellState {
    /// 终端输出行缓冲区 (每行最多 MAX_COLS 字节)
    lines: [[u8; MAX_COLS]; MAX_LINES],
    /// 每行有效长度
    line_lens: [usize; MAX_LINES],
    /// 当前写入行号 (环形)
    write_line: usize,
    /// 当前输入缓冲区
    input_buf: [u8; MAX_COLS],
    /// 当前输入长度
    input_len: usize,
    /// AI 输入缓冲区
    ai_input_buf: [u8; AI_INPUT_MAX],
    /// AI 输入长度
    ai_input_len: usize,
    /// AI 输出缓冲区
    ai_output: [u8; AI_OUTPUT_MAX],
    /// AI 输出有效长度
    ai_output_len: usize,
    /// 是否在 AI 输入模式
    ai_mode: bool,
    /// AI 聊天记录 (角色: 'u'=用户, 'a'=AI, 's'=系统)
    ai_chat_role: [u8; AI_CHAT_LINES],
    ai_chat_text: [[u8; AI_CHAT_LINE_LEN]; AI_CHAT_LINES],
    ai_chat_len: [usize; AI_CHAT_LINES],
    ai_chat_count: usize,
    ai_chat_scroll: usize,
    /// AI 是否正在等待响应
    ai_waiting: bool,
    /// AI 服务名
    ai_service_name: [u8; 32],
    ai_service_name_len: usize,
    /// AI 模型名
    ai_model_name: [u8; 64],
    ai_model_name_len: usize,
    /// AI 是否已连接 (有 API key)
    ai_connected: bool,
    /// 光标闪烁计时器
    cursor_timer: u64,
    /// 光标是否可见
    cursor_visible: bool,
    /// 终端区域坐标
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
    /// AI 面板宽度 (右侧)
    ai_panel_w: i32,
    /// 当前工作目录
    cwd: [u8; 256],
    /// 当前工作目录长度
    cwd_len: usize,
    /// 是否为 root 用户
    is_root: bool,
}

impl ShellState {
    pub fn new(x: i32, y: i32, w: i32, h: i32, is_root: bool) -> Self {
        unsafe { log(b"[SH] new start\n"); }
        let ai_panel_w = 280; // AI 面板宽度 (增大以显示更多内容)
        let mut cwd = [0u8; 256];
        cwd[0] = b'/';

        // 加载 AI 配置
        let mut ai_service_name = [0u8; 32];
        let mut ai_service_name_len = 0;
        let mut ai_model_name = [0u8; 64];
        let mut ai_model_name_len = 0;
        let mut ai_connected = false;

        // 从 /zrr64/zdonk/AI/APIKey.confai 读取配置
        // 注意: 如果该文件不存在, 所有 AI 配置使用默认值
        unsafe { log(b"[SH] opening AI config\n"); }
        unsafe {
            let fd = syscall2(SYS_OPEN, b"/zrr64/zdonk/AI/APIKey.confai\0".as_ptr() as u64, 0u64);
            // 输出 fd 值用于调试
            let mut fd_buf = [b'F', b'D', b'=', b'?', b'?', b'\n'];
            let mut v = fd;
            if v > 999 { v = 999; }
            let mut digits = [0u8; 3];
            let mut dpos = 0;
            if v == 0 { digits[dpos] = b'0'; dpos += 1; }
            else {
                let mut vv = v;
                while vv > 0 && dpos < 3 { digits[dpos] = b'0' + (vv % 10) as u8; vv /= 10; dpos += 1; }
            }
            for i in (0..dpos).rev() { fd_buf[3 + (dpos - 1 - i)] = digits[i]; }
            log(&fd_buf);

            if (fd as i64) >= 0 && fd < 256 {
                // 读取到栈上小缓冲区, 避免大数组
                let mut small_buf = [0u8; 512];
                unsafe { log(b"[SH] reading AI config\n"); }
                let n = syscall3(SYS_READ, fd, small_buf.as_mut_ptr() as u64, 512u64);
                unsafe { log(b"[SH] AI config read done\n"); }
                syscall2(SYS_CLOSE, fd, 0u64);
                if (n as i64) > 0 {
                    let len = n as usize;
                    // 解析 active_service
                    if let Some(pos) = find_substr(&small_buf[..len], b"active_service=") {
                        let start = pos + 15;
                        let mut end = start;
                        while end < len && small_buf[end] != b'\n' && small_buf[end] != b'\r' && small_buf[end] != 0 {
                            end += 1;
                        }
                        let svc_len = (end - start).min(31);
                        ai_service_name[..svc_len].copy_from_slice(&small_buf[start..start + svc_len]);
                        ai_service_name_len = svc_len;
                    }
                    // 解析 api_key
                    if let Some(pos) = find_substr(&small_buf[..len], b"api_key=") {
                        let start = pos + 8;
                        let mut end = start;
                        while end < len && small_buf[end] != b'\n' && small_buf[end] != b'\r' && small_buf[end] != 0 {
                            end += 1;
                        }
                        let key = &small_buf[start..end];
                        if key.len() > 0 && !starts_with(key, b"sk-your-") {
                            ai_connected = true;
                        }
                    }
                    // 解析 model
                    if let Some(pos) = find_substr(&small_buf[..len], b"model=") {
                        let start = pos + 6;
                        let mut end = start;
                        while end < len && small_buf[end] != b'\n' && small_buf[end] != b'\r' && small_buf[end] != 0 {
                            end += 1;
                        }
                        let mdl_len = (end - start).min(63);
                        ai_model_name[..mdl_len].copy_from_slice(&small_buf[start..start + mdl_len]);
                        ai_model_name_len = mdl_len;
                    }
                }
            }
        }

        if ai_service_name_len == 0 {
            ai_service_name[0] = b's'; ai_service_name[1] = b'i'; ai_service_name[2] = b'l';
            ai_service_name[3] = b'i'; ai_service_name[4] = b'c'; ai_service_name[5] = b'o';
            ai_service_name[6] = b'n'; ai_service_name[7] = b'f'; ai_service_name[8] = b'l';
            ai_service_name[9] = b'o'; ai_service_name[10] = b'w';
            ai_service_name_len = 11;
        }
        if ai_model_name_len == 0 {
            ai_model_name[0] = b'd'; ai_model_name[1] = b'e'; ai_model_name[2] = b'e';
            ai_model_name[3] = b'p'; ai_model_name[4] = b's'; ai_model_name[5] = b'e';
            ai_model_name[6] = b'e'; ai_model_name[7] = b'k'; ai_model_name[8] = b'-';
            ai_model_name[9] = b'a'; ai_model_name[10] = b'i'; ai_model_name[11] = b'/';
            ai_model_name[12] = b'D'; ai_model_name[13] = b'e'; ai_model_name[14] = b'e';
            ai_model_name[15] = b'p'; ai_model_name[16] = b'S'; ai_model_name[17] = b'e';
            ai_model_name[18] = b'e'; ai_model_name[19] = b'k';
            ai_model_name_len = 20;
        }

        unsafe { log(b"[SH] building struct\n"); }
        let s = ShellState {
            lines: [[0u8; MAX_COLS]; MAX_LINES],
            line_lens: [0usize; MAX_LINES],
            write_line: 0,
            input_buf: [0u8; MAX_COLS],
            input_len: 0,
            ai_input_buf: [0u8; AI_INPUT_MAX],
            ai_input_len: 0,
            ai_output: [0u8; AI_OUTPUT_MAX],
            ai_output_len: 0,
            ai_mode: false,
            ai_chat_role: [0u8; AI_CHAT_LINES],
            ai_chat_text: [[0u8; AI_CHAT_LINE_LEN]; AI_CHAT_LINES],
            ai_chat_len: [0usize; AI_CHAT_LINES],
            ai_chat_count: 0,
            ai_chat_scroll: 0,
            ai_waiting: false,
            ai_service_name,
            ai_service_name_len,
            ai_model_name,
            ai_model_name_len,
            ai_connected,
            cursor_timer: 0,
            cursor_visible: true,
            x,
            y,
            w,
            h,
            ai_panel_w,
            cwd,
            cwd_len: 1,
            is_root,
        };
        unsafe { log(b"[SH] new complete\n"); }
        s
    }

    /// 向终端追加一行输出
    pub fn write_line(&mut self, text: &[u8]) {
        let line_idx = self.write_line % MAX_LINES;
        let copy_len = text.len().min(MAX_COLS);
        self.lines[line_idx][..copy_len].copy_from_slice(&text[..copy_len]);
        self.line_lens[line_idx] = copy_len;
        self.write_line += 1;
    }

    /// 构建提示符
    fn build_prompt(&self) -> ([u8; 64], usize) {
        let mut buf = [0u8; 64];
        let mut pos = 0;
        // 用户名
        if self.is_root {
            buf[pos] = b'r'; pos += 1;
            buf[pos] = b'o'; pos += 1;
            buf[pos] = b'o'; pos += 1;
            buf[pos] = b't'; pos += 1;
        } else {
            buf[pos] = b'u'; pos += 1;
            buf[pos] = b's'; pos += 1;
            buf[pos] = b'e'; pos += 1;
            buf[pos] = b'r'; pos += 1;
        }
        buf[pos] = b'@'; pos += 1;
        // 主机名
        buf[pos] = b'z'; pos += 1;
        buf[pos] = b'r'; pos += 1;
        buf[pos] = b'a'; pos += 1;
        buf[pos] = b't'; pos += 1;
        buf[pos] = b'r'; pos += 1;
        buf[pos] = b'o'; pos += 1;
        buf[pos] = b':'; pos += 1;
        // 当前目录
        let cwd_display_len = self.cwd_len.min(32);
        buf[pos..pos + cwd_display_len].copy_from_slice(&self.cwd[..cwd_display_len]);
        pos += cwd_display_len;
        buf[pos] = b'$'; pos += 1;
        buf[pos] = b' '; pos += 1;
        (buf, pos)
    }

    /// 处理键盘输入
    pub fn handle_key(&mut self, scancode: u8) -> ShellAction {
        let ch = scancode_to_ascii(scancode);

        // Tab 键切换 AI 模式
        if ch == b'\t' {
            self.ai_mode = !self.ai_mode;
            return ShellAction::None;
        }

        if self.ai_mode {
            return self.handle_ai_key(ch);
        }

        match ch {
            b'\n' => {
                // 提交当前输入行
                let input_len = self.input_len;
                if input_len > 0 {
                    // 复制输入到临时缓冲区避免借用冲突
                    let mut tmp = [0u8; MAX_COLS];
                    tmp[..input_len].copy_from_slice(&self.input_buf[..input_len]);

                    // 将输入写入终端输出 (带提示符)
                    let (prompt, prompt_len) = self.build_prompt();
                    let mut display = [0u8; MAX_COLS];
                    let mut dpos = 0;
                    display[..prompt_len].copy_from_slice(&prompt[..prompt_len]);
                    dpos += prompt_len;
                    let copy = input_len.min(MAX_COLS - dpos);
                    display[dpos..dpos + copy].copy_from_slice(&tmp[..copy]);
                    dpos += copy;
                    self.write_line(&display[..dpos]);

                    // 解析命令和参数
                    let input = &tmp[..input_len];
                    let (cmd, args) = split_cmd_args(input);

                    // 命令分发
                    if buf_eq(cmd, b"help") {
                        self.cmd_help();
                    } else if buf_eq(cmd, b"clear") {
                        self.cmd_clear();
                    } else if buf_eq(cmd, b"ai") {
                        self.ai_mode = true;
                        self.write_line(b"[AI mode activated - Tab to exit]");
                    } else if buf_eq(cmd, b"ver") {
                        self.write_line(b"ZT5 Desktop v1.5.0 [Ztknl kernel]");
                    } else if buf_eq(cmd, b"cd") {
                        self.cmd_cd(args);
                    } else if buf_eq(cmd, b"pwd") {
                        self.cmd_pwd();
                    } else if buf_eq(cmd, b"ls") {
                        self.cmd_ls(args);
                    } else if buf_eq(cmd, b"cat") {
                        self.cmd_cat(args);
                    } else if buf_eq(cmd, b"echo") {
                        self.cmd_echo(args);
                    } else if buf_eq(cmd, b"mkdir") {
                        self.cmd_mkdir(args);
                    } else if buf_eq(cmd, b"rm") {
                        self.cmd_rm(args);
                    } else if buf_eq(cmd, b"touch") {
                        self.cmd_touch(args);
                    } else if buf_eq(cmd, b"whoami") {
                        self.cmd_whoami();
                    } else if buf_eq(cmd, b"uname") {
                        self.cmd_uname();
                    } else if !cmd.is_empty() {
                        // 尝试 fork+exec 执行外部程序
                        self.cmd_exec(cmd, args);
                    }
                }
                self.input_buf = [0u8; MAX_COLS];
                self.input_len = 0;
                ShellAction::None
            }
            b'\x08' => {
                // 退格
                if self.input_len > 0 {
                    self.input_len -= 1;
                    self.input_buf[self.input_len] = 0;
                }
                ShellAction::None
            }
            c if c >= 0x20 && c < 0x7F => {
                if self.input_len < MAX_COLS - 1 {
                    self.input_buf[self.input_len] = c;
                    self.input_len += 1;
                }
                ShellAction::None
            }
            _ => ShellAction::None,
        }
    }

    // =========================================================================
    // 内置命令实现
    // =========================================================================

    /// help - 显示帮助
    fn cmd_help(&mut self) {
        self.write_line(b"ZT5 Shell - Built-in commands:");
        self.write_line(b"  cd [dir]     Change directory");
        self.write_line(b"  pwd          Print working directory");
        self.write_line(b"  ls [dir]     List directory contents");
        self.write_line(b"  cat <file>   Display file contents");
        self.write_line(b"  echo <text>  Print text");
        self.write_line(b"  mkdir <dir>  Create directory");
        self.write_line(b"  rm <file>    Remove file");
        self.write_line(b"  touch <file> Create empty file");
        self.write_line(b"  whoami       Show current user");
        self.write_line(b"  uname        Show system info");
        self.write_line(b"  clear        Clear screen");
        self.write_line(b"  ai           Enter AI mode");
        self.write_line(b"  ver          Show version");
    }

    /// clear - 清屏
    fn cmd_clear(&mut self) {
        self.lines = [[0u8; MAX_COLS]; MAX_LINES];
        self.line_lens = [0usize; MAX_LINES];
        self.write_line = 0;
    }

    /// cd - 切换目录
    fn cmd_cd(&mut self, args: &[u8]) {
        let mut path = [0u8; 256];

        if args.is_empty() {
            // 无参数回到用户主目录
            if self.is_root {
                path[..6].copy_from_slice(b"/root\0");
            } else {
                path[..5].copy_from_slice(b"/usr\0");
            }
        } else {
            // 构造路径 (需要 null 结尾)
            let len = args.len().min(254);
            path[..len].copy_from_slice(&args[..len]);
            path[len] = 0;
        }

        unsafe {
            let ret = syscall2(SYS_CHDIR, path.as_ptr() as u64, 0);
            if (ret as i64) < 0 {
                self.write_line(b"cd: no such directory");
            } else {
                // 从内核获取更新后的 cwd
                let mut cwd_buf = [0u8; 256];
                let n = syscall2(SYS_GETCWD, cwd_buf.as_mut_ptr() as u64, 256u64);
                if (n as i64) > 0 {
                    let len = (n as usize).min(255);
                    // 去掉末尾的 null
                    let mut actual_len = 0;
                    while actual_len < len && cwd_buf[actual_len] != 0 {
                        actual_len += 1;
                    }
                    self.cwd = [0u8; 256];
                    self.cwd[..actual_len].copy_from_slice(&cwd_buf[..actual_len]);
                    self.cwd_len = actual_len;
                }
            }
        }
    }

    /// pwd - 显示当前目录
    fn cmd_pwd(&mut self) {
        unsafe {
            let mut buf = [0u8; 256];
            let n = syscall2(SYS_GETCWD, buf.as_mut_ptr() as u64, 256u64);
            if (n as i64) > 0 {
                let len = (n as usize).min(255);
                let mut actual_len = 0;
                while actual_len < len && buf[actual_len] != 0 {
                    actual_len += 1;
                }
                if actual_len > 0 {
                    let mut out = [0u8; 256];
                    out[..actual_len].copy_from_slice(&buf[..actual_len]);
                    self.write_line(&out[..actual_len]);
                } else {
                    self.write_line(b"/");
                }
            } else {
                // 回退到本地 cwd
                if self.cwd_len > 0 {
                    let mut out = [0u8; 256];
                    out[..self.cwd_len].copy_from_slice(&self.cwd[..self.cwd_len]);
                    let len = self.cwd_len;
                    self.write_line(&out[..len]);
                } else {
                    self.write_line(b"/");
                }
            }
        }
    }

    /// ls - 列出目录内容
    fn cmd_ls(&mut self, args: &[u8]) {
        let dir_path: &[u8];
        let mut owned = [0u8; 256];

        if args.is_empty() {
            // 列出当前目录
            let len = self.cwd_len;
            owned[..len].copy_from_slice(&self.cwd[..len]);
            owned[len] = 0;
            dir_path = &owned[..len + 1];
        } else {
            let len = args.len().min(254);
            owned[..len].copy_from_slice(&args[..len]);
            owned[len] = 0;
            dir_path = &owned[..len + 1];
        }

        unsafe {
            let fd = syscall2(SYS_OPENDIR, dir_path.as_ptr() as u64, 0);
            if fd == u64::MAX || (fd as i64) < 0 {
                self.write_line(b"ls: cannot open directory");
                return;
            }

            let mut dirent_buf = [0u8; DIRENT_SIZE];
            let mut line_buf = [0u8; MAX_COLS];
            let mut line_pos = 0;

            loop {
                let n = syscall3(SYS_READDIR, fd, dirent_buf.as_mut_ptr() as u64, DIRENT_SIZE as u64);
                if n == 0 || (n as i64) <= 0 {
                    break;
                }

                // 解析 FmDirent:
                // d_ino: 8 bytes (offset 0)
                // d_off: 8 bytes (offset 8)
                // d_reclen: 2 bytes (offset 16)
                // d_name: 256 bytes (offset 18)
                // d_type: 1 byte (offset 274)
                let d_type = dirent_buf[274];

                // 找到 d_name 的有效长度 (到第一个 null)
                let mut name_len = 0usize;
                for i in 0..256 {
                    if dirent_buf[18 + i] == 0 { break; }
                    name_len += 1;
                }

                if name_len == 0 { continue; }

                // 跳过 . 和 ..
                if name_len == 1 && dirent_buf[18] == b'.' { continue; }
                if name_len == 2 && dirent_buf[18] == b'.' && dirent_buf[19] == b'.' { continue; }

                // 添加前缀 [D] 或 [F]
                if line_pos + 4 + name_len + 2 > MAX_COLS {
                    // 当前行满了，输出
                    self.write_line(&line_buf[..line_pos]);
                    line_pos = 0;
                }

                if d_type == 2 {
                    // 目录
                    line_buf[line_pos] = b'['; line_pos += 1;
                    line_buf[line_pos] = b'D'; line_pos += 1;
                    line_buf[line_pos] = b']'; line_pos += 1;
                } else {
                    // 文件
                    line_buf[line_pos] = b'['; line_pos += 1;
                    line_buf[line_pos] = b'F'; line_pos += 1;
                    line_buf[line_pos] = b']'; line_pos += 1;
                }
                line_buf[line_pos] = b' '; line_pos += 1;
                line_buf[line_pos..line_pos + name_len].copy_from_slice(&dirent_buf[18..18 + name_len]);
                line_pos += name_len;
                line_buf[line_pos] = b' '; line_pos += 1;
            }

            syscall2(SYS_CLOSEDIR, fd, 0);

            // 输出剩余内容
            if line_pos > 0 {
                self.write_line(&line_buf[..line_pos]);
            }
        }
    }

    /// cat - 显示文件内容
    fn cmd_cat(&mut self, args: &[u8]) {
        if args.is_empty() {
            self.write_line(b"cat: missing file operand");
            return;
        }

        let mut path = [0u8; 256];
        let len = args.len().min(254);
        path[..len].copy_from_slice(&args[..len]);
        path[len] = 0;

        unsafe {
            let fd = syscall2(SYS_OPEN, path.as_ptr() as u64, 0);
            if fd == u64::MAX || (fd as i64) < 0 {
                self.write_line(b"cat: no such file");
                return;
            }

            let mut buf = [0u8; 512];
            let mut line_count = 0;
            let mut line_buf = [0u8; MAX_COLS];
            let mut line_pos = 0;

            loop {
                if line_count >= 50 { break; }
                let n = syscall3(SYS_READ, fd, buf.as_mut_ptr() as u64, 512u64);
                if n == 0 || (n as i64) <= 0 { break; }

                let read_len = n as usize;
                for i in 0..read_len {
                    if line_count >= 50 { break; }
                    let ch = buf[i];
                    if ch == b'\n' {
                        self.write_line(&line_buf[..line_pos]);
                        line_pos = 0;
                        line_count += 1;
                    } else if ch >= 0x20 && ch < 0x7F {
                        if line_pos < MAX_COLS {
                            line_buf[line_pos] = ch;
                            line_pos += 1;
                        }
                    }
                }
            }

            // 输出最后一行 (如果没有以换行结尾)
            if line_pos > 0 && line_count < 50 {
                self.write_line(&line_buf[..line_pos]);
            }

            syscall2(SYS_CLOSE, fd, 0);
        }
    }

    /// echo - 输出文本
    fn cmd_echo(&mut self, args: &[u8]) {
        if args.is_empty() {
            self.write_line(b"");
        } else {
            self.write_line(args);
        }
    }

    /// mkdir - 创建目录
    fn cmd_mkdir(&mut self, args: &[u8]) {
        if args.is_empty() {
            self.write_line(b"mkdir: missing operand");
            return;
        }

        let mut path = [0u8; 256];
        let len = args.len().min(254);
        path[..len].copy_from_slice(&args[..len]);
        path[len] = 0;

        unsafe {
            let ret = syscall2(SYS_MKDIR, path.as_ptr() as u64, 0);
            if (ret as i64) < 0 {
                // 内核 mkdir 尚未完全实现
                self.write_line(b"mkdir: cannot create directory");
            }
        }
    }

    /// rm - 删除文件
    fn cmd_rm(&mut self, args: &[u8]) {
        if args.is_empty() {
            self.write_line(b"rm: missing operand");
            return;
        }

        let mut path = [0u8; 256];
        let len = args.len().min(254);
        path[..len].copy_from_slice(&args[..len]);
        path[len] = 0;

        unsafe {
            let ret = syscall2(SYS_UNLINK, path.as_ptr() as u64, 0);
            if (ret as i64) < 0 {
                self.write_line(b"rm: cannot remove file");
            }
        }
    }

    /// touch - 创建空文件
    fn cmd_touch(&mut self, args: &[u8]) {
        if args.is_empty() {
            self.write_line(b"touch: missing operand");
            return;
        }

        let mut path = [0u8; 256];
        let len = args.len().min(254);
        path[..len].copy_from_slice(&args[..len]);
        path[len] = 0;

        unsafe {
            let fd = syscall2(SYS_OPEN, path.as_ptr() as u64, O_CREAT);
            if fd == u64::MAX || (fd as i64) < 0 {
                self.write_line(b"touch: cannot create file");
            } else {
                syscall2(SYS_CLOSE, fd, 0);
            }
        }
    }

    /// whoami - 显示当前用户
    fn cmd_whoami(&mut self) {
        if self.is_root {
            self.write_line(b"root");
        } else {
            self.write_line(b"user");
        }
    }

    /// uname - 显示系统信息
    fn cmd_uname(&mut self) {
        self.write_line(b"ZratroOS ZT5 x86_64");
    }

    /// exec - 执行外部程序 (在终端内显示结果)
    fn cmd_exec(&mut self, cmd: &[u8], _args: &[u8]) {
        // 注意: 不能 fork+exec GUI 程序 (会抢占帧缓冲区导致死锁)
        // 对于非 GUI 的 CLI 程序, 可以 fork+exec+waitpid
        // 构造完整路径 /prgm/<cmd>\0
        let mut path = [0u8; 256];
        path[0] = b'/';
        path[1] = b'p';
        path[2] = b'r';
        path[3] = b'g';
        path[4] = b'm';
        path[5] = b'/';
        let cmd_len = cmd.len().min(249);
        path[6..6 + cmd_len].copy_from_slice(&cmd[..cmd_len]);
        path[6 + cmd_len] = 0;

        // 先检查文件是否存在
        unsafe {
            let fd = syscall2(SYS_OPEN, path.as_ptr() as u64, 0);
            if fd == u64::MAX || (fd as i64) < 0 {
                let mut msg = [0u8; 64];
                let mut pos = 0;
                let cmd_display_len = cmd.len().min(32);
                msg[pos] = b'c'; pos += 1;
                msg[pos] = b'o'; pos += 1;
                msg[pos] = b'm'; pos += 1;
                msg[pos] = b'm'; pos += 1;
                msg[pos] = b'a'; pos += 1;
                msg[pos] = b'n'; pos += 1;
                msg[pos] = b'd'; pos += 1;
                msg[pos] = b' '; pos += 1;
                msg[pos..pos + cmd_display_len].copy_from_slice(&cmd[..cmd_display_len]);
                pos += cmd_display_len;
                msg[pos] = b':'; pos += 1;
                msg[pos] = b' '; pos += 1;
                msg[pos] = b'n'; pos += 1;
                msg[pos] = b'o'; pos += 1;
                msg[pos] = b't'; pos += 1;
                msg[pos] = b' '; pos += 1;
                msg[pos] = b'f'; pos += 1;
                msg[pos] = b'o'; pos += 1;
                msg[pos] = b'u'; pos += 1;
                msg[pos] = b'n'; pos += 1;
                msg[pos] = b'd'; pos += 1;
                self.write_line(&msg[..pos]);
                return;
            }
            syscall2(SYS_CLOSE, fd, 0);
        }

        // 文件存在但无法在桌面内执行 (会抢占帧缓冲区)
        let mut msg = [0u8; 64];
        let mut pos = 0;
        let cmd_display_len = cmd.len().min(32);
        msg[pos..pos + cmd_display_len].copy_from_slice(&cmd[..cmd_display_len]);
        pos += cmd_display_len;
        msg[pos] = b':'; pos += 1;
        msg[pos] = b' '; pos += 1;
        msg[pos] = b'r'; pos += 1;
        msg[pos] = b'u'; pos += 1;
        msg[pos] = b'n'; pos += 1;
        msg[pos] = b' '; pos += 1;
        msg[pos] = b'i'; pos += 1;
        msg[pos] = b'n'; pos += 1;
        msg[pos] = b' '; pos += 1;
        msg[pos] = b't'; pos += 1;
        msg[pos] = b'e'; pos += 1;
        msg[pos] = b'r'; pos += 1;
        msg[pos] = b'm'; pos += 1;
        msg[pos] = b'i'; pos += 1;
        msg[pos] = b'n'; pos += 1;
        msg[pos] = b'a'; pos += 1;
        msg[pos] = b'l'; pos += 1;
        self.write_line(&msg[..pos]);
    }

    // =========================================================================
    // AI 模式
    // =========================================================================

    /// AI 模式下的键盘处理
    fn handle_ai_key(&mut self, ch: u8) -> ShellAction {
        match ch {
            b'\n' => {
                if self.ai_input_len > 0 {
                    // 检查斜杠命令
                    if self.ai_input_buf[0] == b'/' {
                        self.handle_ai_slash_command();
                        self.ai_input_buf = [0u8; AI_INPUT_MAX];
                        self.ai_input_len = 0;
                        return ShellAction::None;
                    }

                    // 保存用户输入到聊天记录 (先复制避免借用冲突)
                    let mut user_msg = [0u8; AI_INPUT_MAX];
                    let user_len = self.ai_input_len;
                    user_msg[..user_len].copy_from_slice(&self.ai_input_buf[..user_len]);

                    // 提示词优化
                    let (optimized, opt_len) = optimize_prompt(&self.ai_input_buf[..self.ai_input_len]);

                    // 发送给 AI 子系统
                    self.ai_output = [0u8; AI_OUTPUT_MAX];
                    self.ai_output_len = 0;
                    self.ai_waiting = true;

                    unsafe {
                        // SYS_AI_SEND(input_ptr, input_len, flags)
                        let _ret = syscall3(
                            SYS_AI_SEND,
                            optimized.as_ptr() as u64,
                            opt_len as u64,
                            0, // flags
                        );

                        // SYS_AI_RECV(buf_ptr, buf_len, timeout)
                        let recv = syscall3(
                            SYS_AI_RECV,
                            self.ai_output.as_mut_ptr() as u64,
                            AI_OUTPUT_MAX as u64,
                            5000u64, // 5s timeout
                        );
                        if (recv as i64) > 0 {
                            self.ai_output_len = recv as usize;
                        }
                    }

                    self.ai_waiting = false;

                    // 添加用户消息到聊天记录
                    self.ai_chat_add(b'u', &user_msg[..user_len]);

                    // 将 AI 响应添加到聊天记录 (先复制到临时缓冲区)
                    if self.ai_output_len > 0 {
                        let mut tmp_output = [0u8; AI_OUTPUT_MAX];
                        let out_len = self.ai_output_len;
                        tmp_output[..out_len].copy_from_slice(&self.ai_output[..out_len]);

                        let mut line_start = 0usize;
                        for i in 0..out_len {
                            if tmp_output[i] == b'\n' {
                                if i > line_start {
                                    self.ai_chat_add(b'a', &tmp_output[line_start..i]);
                                }
                                line_start = i + 1;
                            }
                        }
                        if line_start < out_len {
                            self.ai_chat_add(b'a', &tmp_output[line_start..out_len]);
                        }
                    } else {
                        self.ai_chat_add(b's', b"[no response]");
                    }

                    // 也写入终端输出
                    self.write_line(b"[AI] query sent");
                    if self.ai_output_len > 0 {
                        let display_len = self.ai_output_len.min(MAX_COLS);
                        let mut tmp = [0u8; MAX_COLS];
                        tmp[..display_len].copy_from_slice(&self.ai_output[..display_len]);
                        self.write_line(&tmp[..display_len]);
                    } else {
                        self.write_line(b"[AI] no response - check API key in /zrr64/zdonk/AI/APIKey.confai");
                    }

                    self.ai_input_buf = [0u8; AI_INPUT_MAX];
                    self.ai_input_len = 0;
                }
                ShellAction::None
            }
            b'\x08' => {
                if self.ai_input_len > 0 {
                    self.ai_input_len -= 1;
                    self.ai_input_buf[self.ai_input_len] = 0;
                }
                ShellAction::None
            }
            c if c >= 0x20 && c < 0x7F => {
                if self.ai_input_len < AI_INPUT_MAX - 1 {
                    self.ai_input_buf[self.ai_input_len] = c;
                    self.ai_input_len += 1;
                }
                ShellAction::None
            }
            _ => ShellAction::None,
        }
    }

    /// 处理 AI 斜杠命令
    fn handle_ai_slash_command(&mut self) {
        let input = &self.ai_input_buf[..self.ai_input_len];

        if buf_eq(input, b"/help") {
            self.ai_chat_add(b's', b"AI Commands:");
            self.ai_chat_add(b's', b"  /help    - Show this help");
            self.ai_chat_add(b's', b"  /clear   - Clear chat history");
            self.ai_chat_add(b's', b"  /status  - Show connection status");
            self.ai_chat_add(b's', b"  /optimize <text> - Optimize prompt");
            self.ai_chat_add(b's', b"  Tab      - Toggle AI/Shell mode");
        } else if buf_eq(input, b"/clear") {
            self.ai_chat_role = [0u8; AI_CHAT_LINES];
            self.ai_chat_text = [[0u8; AI_CHAT_LINE_LEN]; AI_CHAT_LINES];
            self.ai_chat_len = [0usize; AI_CHAT_LINES];
            self.ai_chat_count = 0;
            self.ai_chat_scroll = 0;
            self.ai_chat_add(b's', b"Chat cleared.");
        } else if buf_eq(input, b"/status") {
            let mut status = [0u8; AI_CHAT_LINE_LEN];
            let mut pos = 0;

            // 连接状态
            status[pos] = b'['; pos += 1;
            if self.ai_connected {
                status[pos] = b'O'; pos += 1;
                status[pos] = b'K'; pos += 1;
            } else {
                status[pos] = b'N'; pos += 1;
                status[pos] = b'O'; pos += 1;
            }
            status[pos] = b']'; pos += 1;
            status[pos] = b' '; pos += 1;

            // 服务名
            let svc_len = self.ai_service_name_len.min(AI_CHAT_LINE_LEN - pos - 2);
            status[pos..pos + svc_len].copy_from_slice(&self.ai_service_name[..svc_len]);
            pos += svc_len;
            status[pos] = b'/'; pos += 1;

            // 模型名
            let mdl_len = self.ai_model_name_len.min(AI_CHAT_LINE_LEN - pos);
            status[pos..pos + mdl_len].copy_from_slice(&self.ai_model_name[..mdl_len]);
            pos += mdl_len;

            self.ai_chat_add(b's', &status[..pos]);

            if !self.ai_connected {
                self.ai_chat_add(b's', b"Set API key in /zrr64/zdonk/AI/APIKey.confai");
            }
        } else if starts_with(input, b"/optimize ") {
            // 提示词优化预览
            let text = &input[10..]; // 跳过 "/optimize "
            let (optimized, opt_len) = optimize_prompt(text);

            self.ai_chat_add(b's', b"Optimized prompt:");
            if opt_len > 0 {
                let display_len = opt_len.min(AI_CHAT_LINE_LEN);
                self.ai_chat_add(b'a', &optimized[..display_len]);
            }
        } else {
            self.ai_chat_add(b's', b"Unknown command. Type /help for help.");
        }
    }

    /// 添加一条聊天记录
    fn ai_chat_add(&mut self, role: u8, text: &[u8]) {
        if self.ai_chat_count >= AI_CHAT_LINES {
            // 环形缓冲区: 移动所有记录向前一位
            // 先复制到临时缓冲区避免借用冲突
            let mut tmp_role = [0u8; AI_CHAT_LINES];
            let mut tmp_len = [0usize; AI_CHAT_LINES];
            let mut tmp_text = [[0u8; AI_CHAT_LINE_LEN]; AI_CHAT_LINES];
            for i in 0..AI_CHAT_LINES - 1 {
                tmp_role[i] = self.ai_chat_role[i + 1];
                tmp_len[i] = self.ai_chat_len[i + 1];
                tmp_text[i].copy_from_slice(&self.ai_chat_text[i + 1]);
            }
            self.ai_chat_role = tmp_role;
            self.ai_chat_len = tmp_len;
            self.ai_chat_text = tmp_text;
            self.ai_chat_count = AI_CHAT_LINES - 1;
        }

        let idx = self.ai_chat_count;
        self.ai_chat_role[idx] = role;
        let copy_len = text.len().min(AI_CHAT_LINE_LEN);
        self.ai_chat_text[idx][..copy_len].copy_from_slice(&text[..copy_len]);
        self.ai_chat_len[idx] = copy_len;
        self.ai_chat_count += 1;
    }

    /// 更新光标闪烁
    pub fn update_cursor(&mut self, tick: u64) {
        // 每 500ms 切换一次
        if tick - self.cursor_timer >= 500 {
            self.cursor_visible = !self.cursor_visible;
            self.cursor_timer = tick;
        }
    }

    /// 绘制终端
    pub fn draw(&mut self, ctx: &mut DrawContext) {
        let x = self.x;
        let y = self.y;
        let w = self.w;
        let h = self.h;
        let ai_w = self.ai_panel_w;

        // ── 左侧: Shell 终端 ──
        let term_w = w - ai_w - 4; // 减去 AI 面板宽度和间距

        // 终端背景
        ctx.fill_rect(x + 1, y + 1, term_w - 2, h - 2, &BG_PANEL);

        // 终端边框 (带发光)
        ctx.draw_rect_glow(x, y, term_w, h, &CYAN, &GLOW_CYAN);

        // 标题栏
        ctx.draw_text(x + 8, y + 4, b"TERMINAL", &CYAN);
        let mode_label: &[u8] = if self.ai_mode { b"AI" } else { b"SHELL" };
        let mode_color = if self.ai_mode { &GREEN } else { &TEXT_DIM };
        let label_w = mode_label.len() * 8;
        ctx.draw_text(x + term_w - 8 - label_w as i32, y + 4, mode_label, mode_color);

        // 分割线
        ctx.draw_hline_dashed(x + 4, x + term_w - 4, y + 20, 4, 4, &GLOW_CYAN);

        // 终端输出区域
        let text_start_y = y + 24;
        let line_h: i32 = 16;
        let max_visible_lines = ((h - 44) / line_h) as usize;
        let total_lines = self.write_line.min(MAX_LINES);

        // 计算可见行范围 (显示最新的行)
        let start_line = if total_lines > max_visible_lines {
            total_lines - max_visible_lines
        } else {
            0
        };

        for i in 0..max_visible_lines {
            let line_idx = (start_line + i) % MAX_LINES;
            let line_y = text_start_y + i as i32 * line_h;
            let len = self.line_lens[line_idx];
            if len > 0 {
                ctx.draw_text(x + 6, line_y, &self.lines[line_idx][..len], &TEXT);
            }
        }

        // 当前输入行 (带光标)
        let input_y = text_start_y + max_visible_lines as i32 * line_h;
        if input_y + line_h <= y + h - 4 {
            // 提示符
            let (prompt, prompt_len) = self.build_prompt();
            let prompt_w = prompt_len as i32 * 8;
            ctx.draw_text(x + 6, input_y, &prompt[..prompt_len], &GREEN);

            // 输入内容
            if self.input_len > 0 {
                ctx.draw_text(
                    x + 6 + prompt_w,
                    input_y,
                    &self.input_buf[..self.input_len],
                    &TEXT,
                );
            }

            // 闪烁光标
            if self.cursor_visible {
                let cursor_x = x + 6 + prompt_w + self.input_len as i32 * 8;
                ctx.draw_line(cursor_x, input_y, cursor_x, input_y + 14, &WHITE);
            }
        }

        // ── 右侧: AI 面板 ──
        let ai_x = x + term_w + 4;

        // AI 面板背景
        ctx.fill_rect(ai_x + 1, y + 1, ai_w - 2, h - 2, &BG_DARK);

        // AI 面板边框 (带发光)
        ctx.draw_rect_glow(ai_x, y, ai_w, h, &GREEN, &GLOW_CYAN);

        // AI 标题 + 连接状态指示灯
        ctx.draw_text(ai_x + 6, y + 4, b"AI ASSIST", &GREEN);
        // 连接状态圆点: 绿=已连接, 黄=占位key, 红=无配置
        let dot_x = ai_x + ai_w - 16;
        let dot_color = if self.ai_connected { &GREEN } else { &YELLOW };
        ctx.fill_rect(dot_x, y + 6, 8, 8, dot_color);

        // 服务名/模型名 (标题栏右侧)
        if self.ai_service_name_len > 0 {
            let mut svc_label = [0u8; 40];
            let mut pos = 0;
            let svc_len = self.ai_service_name_len.min(16);
            svc_label[..svc_len].copy_from_slice(&self.ai_service_name[..svc_len]);
            pos += svc_len;
            if self.ai_model_name_len > 0 {
                svc_label[pos] = b'/'; pos += 1;
                let mdl_len = self.ai_model_name_len.min(20);
                let avail = 40 - pos;
                let copy = mdl_len.min(avail);
                svc_label[pos..pos + copy].copy_from_slice(&self.ai_model_name[..copy]);
                pos += copy;
            }
            // 在标题栏下方小字显示
            ctx.draw_text(ai_x + 6, y + 14, &svc_label[..pos], &TEXT_DIM);
        }

        // 分割线
        ctx.draw_hline_dashed(ai_x + 4, ai_x + ai_w - 4, y + 24, 4, 4, &GLOW_CYAN);

        // AI 聊天记录区域
        let chat_top = y + 28;
        let chat_bottom = y + h - 48; // 留出底部输入区
        let line_h: i32 = 14;
        let max_chat_visible = ((chat_bottom - chat_top) / line_h) as usize;

        if self.ai_chat_count > 0 {
            // 计算可见范围 (显示最新的记录)
            let start_idx = if self.ai_chat_count > max_chat_visible {
                self.ai_chat_count - max_chat_visible
            } else {
                0
            };

            for i in 0..max_chat_visible {
                let chat_idx = start_idx + i;
                if chat_idx >= self.ai_chat_count { break; }
                let line_y = chat_top + i as i32 * line_h;
                if line_y + line_h > chat_bottom { break; }

                let role = self.ai_chat_role[chat_idx];
                let len = self.ai_chat_len[chat_idx];
                if len == 0 { continue; }

                // 角色前缀和颜色
                let (prefix, color) = match role {
                    b'u' => (b"You: ", &CYAN),
                    b'a' => (b"AI:  ", &GREEN),
                    _    => (b"Sys: ", &TEXT_DIM),
                };

                // 绘制角色前缀
                ctx.draw_text(ai_x + 4, line_y, prefix, color);

                // 绘制消息内容 (限制宽度)
                let max_msg_cols = ((ai_w - 40) / 8) as usize;
                let display_len = len.min(max_msg_cols);
                ctx.draw_text(ai_x + 4 + 4 * 8, line_y, &self.ai_chat_text[chat_idx][..display_len], &TEXT);
            }
        } else {
            // 无聊天记录时显示欢迎信息
            ctx.draw_text(ai_x + 6, chat_top, b"Welcome to AI ASSIST", &TEXT_DIM);
            ctx.draw_text(ai_x + 6, chat_top + 16, b"Type a question or", &TEXT_DIM);
            ctx.draw_text(ai_x + 6, chat_top + 32, b"/help for commands", &TEXT_DIM);
            if !self.ai_connected {
                ctx.draw_text(ai_x + 6, chat_top + 56, b"[No API key set]", &YELLOW);
                ctx.draw_text(ai_x + 6, chat_top + 72, b"Edit APIKey.confai", &YELLOW);
            }
        }

        // 等待动画 (升级版: 旋转点 + 多帧)
        if self.ai_waiting {
            // 旋转点动画: . .. ... 循环
            let dot_phase = (self.cursor_timer / 400) % 3;
            let dots: &[u8] = match dot_phase {
                0 => b".",
                1 => b"..",
                _ => b"...",
            };

            // 旋转字符
            let spin_chars = [b'|', b'/', b'-', b'\\'];
            let spin_idx = ((self.cursor_timer / 100) % 4) as usize;

            let mut anim_buf = [0u8; 24];
            anim_buf[0] = b'[';
            anim_buf[1] = b'T';
            anim_buf[2] = b'h';
            anim_buf[3] = b'i';
            anim_buf[4] = b'n';
            anim_buf[5] = b'k';
            anim_buf[6] = b'i';
            anim_buf[7] = b'n';
            anim_buf[8] = b'g';
            anim_buf[9] = b' ';
            anim_buf[10] = spin_chars[spin_idx];
            anim_buf[11] = b']';
            anim_buf[12] = b' ';
            // 添加点
            for (i, &d) in dots.iter().enumerate() {
                anim_buf[13 + i] = d;
            }
            let total_len = 13 + dots.len();
            ctx.draw_text(ai_x + 6, chat_bottom - 16, &anim_buf[..total_len], &GREEN);

            // 旋转加载圈在文字右侧
            let spinner_x = ai_x + 6 + (total_len as i32 + 1) * 8;
            ctx.draw_spinner(spinner_x, chat_bottom - 8, 6, self.cursor_timer);
        }

        // AI 输入区域 (底部)
        let ai_input_y = chat_bottom + 4;

        // 输入区分割线
        ctx.draw_hline_dashed(ai_x + 4, ai_x + ai_w - 4, ai_input_y, 4, 4, &GLOW_CYAN);

        let ai_input_text_y = ai_input_y + 4;

        // AI 输入提示符
        ctx.draw_text(ai_x + 6, ai_input_text_y, b"AI>", &GREEN);

        // AI 输入内容 (支持多行显示)
        if self.ai_input_len > 0 {
            let max_ai_display = ((ai_w - 40) / 8) as usize;
            let display_len = self.ai_input_len.min(max_ai_display);
            ctx.draw_text(
                ai_x + 6 + 3 * 8,
                ai_input_text_y,
                &self.ai_input_buf[..display_len],
                &CYAN,
            );
        }

        // AI 输入光标
        if self.ai_mode && self.cursor_visible {
            let ai_cursor_x = ai_x + 6 + 3 * 8 + self.ai_input_len.min(((ai_w - 40) / 8) as usize) as i32 * 8;
            ctx.draw_line(ai_cursor_x, ai_input_text_y, ai_cursor_x, ai_input_text_y + 12, &GREEN);
        }

        // 模式指示 (底部)
        let mode_y = ai_input_text_y + 16;
        if self.ai_mode {
            ctx.draw_text(ai_x + 6, mode_y, b"[AI MODE - Tab to exit]", &GREEN);
        } else {
            ctx.draw_text(ai_x + 6, mode_y, b"[Tab for AI mode]", &TEXT_DIM);
        }
    }
}

/// Shell 动作枚举
#[derive(Clone, Copy, PartialEq)]
pub enum ShellAction {
    None,
    RequestAppSwitch,
}

/// 前缀比较辅助函数
fn buf_eq(buf: &[u8], target: &[u8]) -> bool {
    if buf.len() != target.len() {
        return false;
    }
    for i in 0..target.len() {
        if buf[i] != target[i] {
            return false;
        }
    }
    true
}

/// 分割命令和参数
fn split_cmd_args(input: &[u8]) -> (&[u8], &[u8]) {
    // 跳过前导空格
    let mut start = 0;
    while start < input.len() && input[start] == b' ' {
        start += 1;
    }
    // 找到命令结束位置
    let mut cmd_end = start;
    while cmd_end < input.len() && input[cmd_end] != b' ' {
        cmd_end += 1;
    }
    let cmd = &input[start..cmd_end];

    // 跳过命令和参数之间的空格
    let mut args_start = cmd_end;
    while args_start < input.len() && input[args_start] == b' ' {
        args_start += 1;
    }
    let args = &input[args_start..];

    (cmd, args)
}

/// 在字节切片中查找子串, 返回起始位置
fn find_substr(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || needle.len() > haystack.len() {
        return None;
    }
    for i in 0..=haystack.len() - needle.len() {
        if &haystack[i..i + needle.len()] == needle {
            return Some(i);
        }
    }
    None
}

/// 检查字节切片是否以指定前缀开头
fn starts_with(buf: &[u8], prefix: &[u8]) -> bool {
    if prefix.len() > buf.len() {
        return false;
    }
    &buf[..prefix.len()] == prefix
}

/// 提示词优化 - 从 AIquekao 移植的核心规则
/// 将用户输入分类并添加优化前缀
fn optimize_prompt(input: &[u8]) -> ([u8; AI_INPUT_MAX], usize) {
    let mut result = [0u8; AI_INPUT_MAX];
    let mut pos = 0;

    // 分类关键词检测
    let (prefix, _category): (&[u8], u8) = if contains_any(input, b"code|program|function|implement|debug|fix|bug|compile|error|syntax") {
        (b"You are an expert programmer. ", 1u8)
    } else if contains_any(input, b"explain|what is|how does|why|describe|define|meaning") {
        (b"You are a clear and thorough teacher. ", 2u8)
    } else if contains_any(input, b"write|create|generate|compose|draft|make") {
        (b"You are a creative writer. Be specific and detailed. ", 3u8)
    } else if contains_any(input, b"analyze|compare|evaluate|review|assess|pros cons") {
        (b"You are an analytical thinker. Provide structured analysis. ", 4u8)
    } else if contains_any(input, b"translate|translation|convert|chinese|japanese|korean") {
        (b"You are a professional translator. ", 5u8)
    } else if contains_any(input, b"summarize|summary|brief|tldr|key points") {
        (b"You are a concise summarizer. ", 6u8)
    } else if contains_any(input, b"list|enumerate|steps|how to|guide|tutorial") {
        (b"You are a structured guide. Use numbered steps. ", 7u8)
    } else if contains_any(input, b"optimize|improve|refactor|performance|efficient") {
        (b"You are an optimization expert. ", 8u8)
    } else if contains_any(input, b"design|architecture|pattern|structure|system") {
        (b"You are a system architect. ", 9u8)
    } else {
        (&[], 0u8)
    };

    // 写入优化前缀
    let prefix_len = prefix.len();
    if prefix_len > 0 && pos + prefix_len < AI_INPUT_MAX {
        result[pos..pos + prefix_len].copy_from_slice(prefix);
        pos += prefix_len;
    }

    // 写入原始输入
    let input_copy_len = input.len().min(AI_INPUT_MAX - pos);
    if input_copy_len > 0 {
        result[pos..pos + input_copy_len].copy_from_slice(&input[..input_copy_len]);
        pos += input_copy_len;
    }

    (result, pos)
}

/// 检查输入是否包含任意一个关键词 (用 | 分隔)
fn contains_any(input: &[u8], keywords: &[u8]) -> bool {
    let mut start = 0;
    while start < keywords.len() {
        // 找到 | 分隔符
        let mut end = start;
        while end < keywords.len() && keywords[end] != b'|' {
            end += 1;
        }
        if end > start {
            let keyword = &keywords[start..end];
            if find_substr(input, keyword).is_some() {
                return true;
            }
        }
        start = end + 1;
    }
    false
}
