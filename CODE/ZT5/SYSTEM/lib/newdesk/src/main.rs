// =============================================================================
// newdesk - 新版系统桌面主入口
// =============================================================================
// 替代旧的 desk (C语言版)，使用 Rust 重写
// 功能：初始化帧缓冲区 → 读取用户配置 → 进入登录界面 → 桌面环境
// =============================================================================
// 系统调用约定 (ZratroOS int 0x80):
//   RAX = 系统调用号, RDI/RSI/RDX/R10/R8/R9 = 参数1-6
//   返回值在 RAX 中
// =============================================================================

#![no_std]
#![no_main]

mod init;
mod signin;
mod ui;
mod desktop;
mod shell;
mod fileman;
mod sidebar;
mod taskbar;
mod systray;

use core::panic::PanicInfo;

use ui::{create_draw_context, Cursor};

/// 系统调用号
const SYS_WRITE: u64 = 2;
const SYS_EXIT: u64 = 0;

/// 应用状态机
#[derive(Clone, Copy, PartialEq)]
enum AppState {
    SignIn,
    Desktop,
}

/// panic 处理函数 - 输出诊断信息后退出进程
#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    unsafe {
        serial_write(b"[NEWDESK] PANIC: ");
        if let Some(loc) = info.location() {
            let mut buf = [0u8; 128];
            let mut pos = 0;
            // 文件名
            let file = loc.file().as_bytes();
            let copy_len = file.len().min(buf.len() - pos - 1);
            buf[pos..pos + copy_len].copy_from_slice(&file[..copy_len]);
            pos += copy_len;
            // 行号
            buf[pos] = b':';
            pos += 1;
            let mut line = loc.line();
            let mut digits = [0u8; 20];
            let mut dpos = 0;
            if line == 0 {
                digits[dpos] = b'0';
                dpos += 1;
            } else {
                while line > 0 {
                    digits[dpos] = b'0' + (line % 10) as u8;
                    dpos += 1;
                    line /= 10;
                }
            }
            for i in (0..dpos).rev() {
                if pos < buf.len() {
                    buf[pos] = digits[i];
                    pos += 1;
                }
            }
            serial_write(&buf[..pos]);
        }
        serial_write(b"\n");

        // 退出进程，让 init 可以重启
        core::arch::asm!(
            "int 0x80",
            inlateout("rax") SYS_EXIT => _,
            in("rdi") 1u64,
            out("rcx") _,
            out("r11") _,
        );
    }
    loop {}
}

/// 输出字符串到串口
unsafe fn serial_write(s: &[u8]) {
    let _ = core::arch::asm!(
        "int 0x80",
        inlateout("rax") SYS_WRITE => _,
        in("rdi") 1u64,       /* fd=1 (stdout) */
        in("rsi") s.as_ptr() as u64,
        in("rdx") s.len() as u64,
        out("rcx") _,
        out("r11") _,
    );
}

/// ZratroOS 程序入口点
///
/// 执行流程:
/// 1. 获取帧缓冲区信息
/// 2. 读取用户配置文件
/// 3. 解析配置中的用户名和密码哈希
/// 4. 创建绘图上下文和鼠标光标
/// 5. 进入登录界面
/// 6. 登录成功后进入桌面环境
#[no_mangle]
pub extern "C" fn _start() -> ! {
    unsafe { serial_write(b"[ND] newdesk starting\n"); }

    // ── 第一步：获取帧缓冲区信息 ──
    let fb_info = init::get_framebuffer_info();
    unsafe {
        serial_write(b"[ND] framebuffer OK: base=0x");
        // 输出 fb_info.base 的十六进制值
        let mut hex = [0u8; 18];
        let mut pos = 0;
        let mut v = fb_info.base;
        if v == 0 { hex[pos] = b'0'; pos += 1; }
        else {
            let mut digits = [0u8; 16];
            let mut dp = 0;
            while v > 0 { digits[dp] = b"0123456789ABCDEF"[(v & 0xF) as usize]; v >>= 4; dp += 1; }
            for i in (0..dp).rev() { hex[pos] = digits[i]; pos += 1; }
        }
        hex[pos] = b'\n'; serial_write(&hex[..pos+1]);

        serial_write(b"[ND] w=");
        let mut wbuf = [0u8; 8]; let mut wp = 0;
        let mut wv = fb_info.width;
        if wv == 0 { wbuf[wp] = b'0'; wp += 1; }
        else { let mut d = [0u8; 8]; let mut dp = 0; while wv > 0 { d[dp] = b'0' + (wv % 10) as u8; wv /= 10; dp += 1; } for i in (0..dp).rev() { wbuf[wp] = d[i]; wp += 1; } }
        wbuf[wp] = b' '; serial_write(&wbuf[..wp+1]);

        serial_write(b"h=");
        let mut hbuf = [0u8; 8]; let mut hp = 0;
        let mut hv = fb_info.height;
        if hv == 0 { hbuf[hp] = b'0'; hp += 1; }
        else { let mut d = [0u8; 8]; let mut dp = 0; while hv > 0 { d[dp] = b'0' + (hv % 10) as u8; hv /= 10; dp += 1; } for i in (0..dp).rev() { hbuf[hp] = d[i]; hp += 1; } }
        hbuf[hp] = b' '; serial_write(&hbuf[..hp+1]);

        serial_write(b"fmt=");
        let mut fbuf = [0u8; 8]; let mut fp = 0;
        let mut fv = fb_info.format;
        if fv == 0 { fbuf[fp] = b'0'; fp += 1; }
        else { let mut d = [0u8; 8]; let mut dp = 0; while fv > 0 { d[dp] = b'0' + (fv % 10) as u8; fv /= 10; dp += 1; } for i in (0..dp).rev() { fbuf[fp] = d[i]; fp += 1; } }
        fbuf[fp] = b'\n'; serial_write(&fbuf[..fp+1]);
    }

    // ── 第二步：读取用户配置文件 ──
    let config = init::read_user_config();
    unsafe { serial_write(b"[ND] config loaded\n"); }

    // ── 第三步：解析用户配置 ──
    let username = init::parse_config_value(&config, b"name");
    let password_hash = init::parse_config_value(&config, b"passwd");

    // ── 第四步：创建绘图上下文和鼠标光标 ──
    let mut ctx = create_draw_context(&fb_info);
    let mut cursor = Cursor::new();
    unsafe { serial_write(b"[ND] draw context created\n"); }

    // ── 第五步：状态机主循环 ──
    let mut state = AppState::SignIn;
    let mut is_root = false;

    loop {
        match state {
            AppState::SignIn => {
                let (result, info) = signin::start(&username, &password_hash, &fb_info, &mut ctx, &mut cursor);
                if result == signin::SignInResult::Success {
                    is_root = info.is_root;
                    state = AppState::Desktop;
                }
            }
            AppState::Desktop => {
                // 使用登录界面返回的用户身份
                unsafe { serial_write(b"[ND] creating DesktopState\n"); }
                let mut desktop = desktop::DesktopState::new(&fb_info, is_root);
                unsafe { serial_write(b"[ND] DesktopState created, calling init\n"); }
                desktop.init(&mut ctx, &mut cursor);
                unsafe { serial_write(b"[ND] init done, calling run\n"); }

                // 进入桌面主循环 (永不返回)
                desktop.run(&mut ctx, &mut cursor);
            }
        }
    }
}
