// =============================================================================
// systray 模块 - 桌面右下角系统状态区域
// =============================================================================
// 设计:
//   - 位于桌面右下角独立区域 (不在任务栏内)
//   - 网络状态图标 + 连接名称
//   - 音量控制图标 + 可拖动滑块
//   - 亮度图标 + 可拖动滑块
//   - 时间显示 (UTC+8)
//   - 尝试从 /zrr64/icon.dll 加载图标资源
//   - 网络面板: WiFi 扫描/连接/断开
// =============================================================================

use crate::ui::{
    DrawContext, CYAN, BG_PANEL, TEXT_DIM, GREEN, GLOW_CYAN, DIM, WHITE,
    syscall1, syscall2, syscall3, buf_len,
};

/// 系统调用号
const SYS_OPEN: u64 = 3;
const SYS_CLOSE: u64 = 4;
const SYS_READ: u64 = 1;
const SYS_WRITE: u64 = 2;
const SYS_TIME: u64 = 13;
const SYS_IOCTL: u64 = 24;

/// WiFi 扫描 ioctl 命令
const WIFI_IOCTL_SCAN: u64 = 0x1001;
/// WiFi 连接 ioctl 命令
const WIFI_IOCTL_CONNECT: u64 = 0x1002;
/// WiFi 断开 ioctl 命令
const WIFI_IOCTL_DISCONNECT: u64 = 0x1003;
/// WiFi 获取扫描结果 ioctl 命令
const WIFI_IOCTL_GET_SCAN_RESULTS: u64 = 0x1004;

/// 图标尺寸 (16x16 像素)
const ICON_SIZE: i32 = 16;

/// 网络面板尺寸
const NET_PANEL_W: i32 = 280;
const NET_PANEL_H: i32 = 200;

// =============================================================================
// DragTarget - 拖动目标枚举
// =============================================================================

#[derive(Clone, Copy, PartialEq)]
pub enum DragTarget {
    None,
    Volume,
    Brightness,
}

// =============================================================================
// SysTrayState - 系统托盘状态
// =============================================================================

pub struct SysTrayState {
    /// 网络连接名称
    network_name: [u8; 64],
    /// 是否已连接网络
    network_connected: bool,
    /// 音量 (0~100)
    pub volume: u8,
    /// 亮度 (0~100)
    pub brightness: u8,
    /// 当前时间 (缓存)
    time_str: [u8; 20],
    /// 日期字符串
    date_str: [u8; 20],
    /// icon.dll 是否存在
    icon_dll_loaded: bool,
    /// WiFi 图标数据 (16x16 ARGB, 从 icon.dll 加载)
    wifi_icon: [u32; 256],
    /// 音量图标数据 (16x16 ARGB)
    vol_icon: [u32; 256],
    /// 亮度图标数据 (16x16 ARGB)
    bri_icon: [u32; 256],
    /// 托盘区域坐标 (右下角)
    pub x: i32,
    pub y: i32,
    /// 区域尺寸
    w: i32,
    h: i32,
    /// 当前正在拖动的目标
    dragging: DragTarget,
    /// 音量条位置 (由 draw() 记录)
    vol_bar_x: i32,
    vol_bar_y: i32,
    vol_bar_w: i32,
    /// 亮度条位置 (由 draw() 记录)
    bri_bar_x: i32,
    bri_bar_y: i32,
    bri_bar_w: i32,
    /// WiFi 可用网络列表
    wifi_list: [[u8; 64]; 8],
    /// WiFi 列表数量
    wifi_list_count: usize,
    /// 网络面板是否展开
    network_panel_open: bool,
    /// 网络面板区域坐标
    net_panel_x: i32,
    net_panel_y: i32,
    /// 选中的 WiFi 索引
    selected_wifi: usize,
    /// WiFi 密码输入缓冲区
    wifi_password: [u8; 64],
    /// WiFi 密码长度
    wifi_password_len: usize,
    /// 是否在密码输入模式
    wifi_password_mode: bool,
}

impl SysTrayState {
    pub fn new(screen_w: i32, screen_h: i32) -> Self {
        // 右下角区域: 宽 280px, 高 120px, 距右下角 8px
        let w = 280;
        let h = 120;
        let x = screen_w - w - 8;
        let y = screen_h - h - 52; // 在任务栏上方

        let mut state = SysTrayState {
            network_name: [0u8; 64],
            network_connected: false,
            volume: 75,
            brightness: 80,
            time_str: [0u8; 20],
            date_str: [0u8; 20],
            icon_dll_loaded: false,
            wifi_icon: [0u32; 256],
            vol_icon: [0u32; 256],
            bri_icon: [0u32; 256],
            x,
            y,
            w,
            h,
            dragging: DragTarget::None,
            vol_bar_x: 0,
            vol_bar_y: 0,
            vol_bar_w: 0,
            bri_bar_x: 0,
            bri_bar_y: 0,
            bri_bar_w: 0,
            wifi_list: [[0u8; 64]; 8],
            wifi_list_count: 0,
            network_panel_open: false,
            net_panel_x: 0,
            net_panel_y: 0,
            selected_wifi: 0,
            wifi_password: [0u8; 64],
            wifi_password_len: 0,
            wifi_password_mode: false,
        };
        state.load_settings();
        state.load_network_config();
        state.load_icon_dll();
        state.update_time();
        state
    }

    /// 从 /ini/usrroot/user.cfg 加载音量和亮度设置
    fn load_settings(&mut self) {
        let cfg_path = b"/ini/usrroot/user.cfg\0";

        unsafe {
            let fd = syscall2(SYS_OPEN, cfg_path.as_ptr() as u64, 0);
            if fd == u64::MAX || (fd as i64) < 0 {
                return;
            }

            let mut buf = [0u8; 128];
            let n = syscall3(SYS_READ, fd, buf.as_mut_ptr() as u64, 128u64);
            syscall2(SYS_CLOSE, fd, 0);

            if (n as i64) <= 0 {
                return;
            }

            let len = n as usize;
            // 解析 vol=XX 和 bri=XX
            let mut pos = 0usize;
            while pos < len {
                // 跳过换行
                while pos < len && (buf[pos] == b'\n' || buf[pos] == b'\r') {
                    pos += 1;
                }
                if pos >= len { break; }

                if pos + 4 <= len && &buf[pos..pos + 4] == b"vol=" {
                    pos += 4;
                    let val = parse_u8_from_buf(&buf[pos..len]);
                    if let Some(v) = val.0 {
                        self.volume = v.min(100);
                    }
                    pos += val.1;
                } else if pos + 4 <= len && &buf[pos..pos + 4] == b"bri=" {
                    pos += 4;
                    let val = parse_u8_from_buf(&buf[pos..len]);
                    if let Some(v) = val.0 {
                        self.brightness = v.min(100);
                    }
                    pos += val.1;
                } else {
                    // 跳过未知行
                    while pos < len && buf[pos] != b'\n' && buf[pos] != b'\r' {
                        pos += 1;
                    }
                }
            }
        }
    }

    /// 保存音量和亮度设置到 /ini/usrroot/user.cfg
    fn save_settings(&self) {
        let cfg_path = b"/ini/usrroot/user.cfg\0";

        // 构造内容: vol=XX\nbri=XX\n
        let mut content = [0u8; 32];
        let mut pos = 0;

        // vol=
        content[pos] = b'v'; pos += 1;
        content[pos] = b'o'; pos += 1;
        content[pos] = b'l'; pos += 1;
        content[pos] = b'='; pos += 1;
        pos += u8_to_bytes(self.volume, &mut content[pos..]);
        content[pos] = b'\n'; pos += 1;

        // bri=
        content[pos] = b'b'; pos += 1;
        content[pos] = b'r'; pos += 1;
        content[pos] = b'i'; pos += 1;
        content[pos] = b'='; pos += 1;
        pos += u8_to_bytes(self.brightness, &mut content[pos..]);
        content[pos] = b'\n'; pos += 1;

        unsafe {
            let fd = syscall2(SYS_OPEN, cfg_path.as_ptr() as u64, 0x41); // O_WRONLY|O_CREAT
            if fd == u64::MAX || (fd as i64) < 0 {
                return;
            }
            syscall3(SYS_WRITE, fd, content.as_ptr() as u64, pos as u64);
            syscall2(SYS_CLOSE, fd, 0);
        }
    }

    /// 处理鼠标点击交互
    pub fn handle_click(&mut self, mouse_x: i32, mouse_y: i32) {
        // 检查网络面板内的点击
        if self.network_panel_open {
            let px = self.net_panel_x;
            let py = self.net_panel_y;

            // 检查 WiFi 列表项点击 (每行 20px, 从 py+28 开始)
            for i in 0..self.wifi_list_count {
                let item_y = py + 28 + i as i32 * 20;
                if mouse_x >= px + 4 && mouse_x <= px + NET_PANEL_W - 4
                    && mouse_y >= item_y && mouse_y <= item_y + 18
                {
                    self.selected_wifi = i;
                    return;
                }
            }

            // 检查扫描按钮 (底部左侧)
            let scan_btn_y = py + NET_PANEL_H - 28;
            if mouse_x >= px + 8 && mouse_x <= px + 88
                && mouse_y >= scan_btn_y && mouse_y <= scan_btn_y + 22
            {
                self.wifi_scan();
                return;
            }

            // 检查连接按钮 (底部中间)
            let conn_btn_x = px + 96;
            if mouse_x >= conn_btn_x && mouse_x <= conn_btn_x + 80
                && mouse_y >= scan_btn_y && mouse_y <= scan_btn_y + 22
            {
                self.wifi_connect();
                return;
            }

            // 检查断开按钮 (底部右侧)
            let disc_btn_x = px + 184;
            if mouse_x >= disc_btn_x && mouse_x <= disc_btn_x + 80
                && mouse_y >= scan_btn_y && mouse_y <= scan_btn_y + 22
            {
                self.wifi_disconnect();
                return;
            }

            // 点击面板外部关闭
            if mouse_x < px || mouse_x > px + NET_PANEL_W
                || mouse_y < py || mouse_y > py + NET_PANEL_H
            {
                self.network_panel_open = false;
            }
            return;
        }

        // 检查网络图标区域点击 (第一行, x+10 到 x+34, row1_y 到 row1_y+16)
        let row1_y = self.y + 26;
        if mouse_x >= self.x + 10 && mouse_x <= self.x + 34
            && mouse_y >= row1_y && mouse_y <= row1_y + 16
        {
            self.network_panel_open = !self.network_panel_open;
            if self.network_panel_open {
                self.wifi_scan();
            }
        }
    }

    /// 处理鼠标拖动交互
    ///
    /// - 鼠标左键按下时，检查是否在音量条或亮度条区域
    /// - 鼠标移动且 dragging 时，根据鼠标 X 位置计算新的音量/亮度值
    /// - 鼠标左键释放时，停止拖动并保存设置
    pub fn handle_mouse_drag(&mut self, mouse_x: i32, mouse_y: i32, left_btn_down: bool) {
        if left_btn_down {
            if self.dragging == DragTarget::None {
                // 检查是否点击在音量条区域 (上下扩展 6px 方便点击)
                if mouse_x >= self.vol_bar_x && mouse_x <= self.vol_bar_x + self.vol_bar_w
                    && mouse_y >= self.vol_bar_y - 6 && mouse_y <= self.vol_bar_y + 10
                {
                    self.dragging = DragTarget::Volume;
                    self.volume = self.calc_value_from_x(mouse_x, self.vol_bar_x, self.vol_bar_w);
                }
                // 检查是否点击在亮度条区域
                else if mouse_x >= self.bri_bar_x && mouse_x <= self.bri_bar_x + self.bri_bar_w
                    && mouse_y >= self.bri_bar_y - 6 && mouse_y <= self.bri_bar_y + 10
                {
                    self.dragging = DragTarget::Brightness;
                    self.brightness = self.calc_value_from_x(mouse_x, self.bri_bar_x, self.bri_bar_w);
                }
            } else {
                // 正在拖动，更新值
                match self.dragging {
                    DragTarget::Volume => {
                        self.volume = self.calc_value_from_x(mouse_x, self.vol_bar_x, self.vol_bar_w);
                    }
                    DragTarget::Brightness => {
                        self.brightness = self.calc_value_from_x(mouse_x, self.bri_bar_x, self.bri_bar_w);
                    }
                    DragTarget::None => {}
                }
            }
        } else {
            // 鼠标释放
            if self.dragging != DragTarget::None {
                self.save_settings();
                self.dragging = DragTarget::None;
            }
        }
    }

    /// 处理键盘输入 (用于 WiFi 密码输入)
    pub fn handle_key(&mut self, scancode: u8) {
        if !self.wifi_password_mode {
            return;
        }

        // 简单的 ASCII 转换
        let ch = if scancode == 0x1C {
            b'\n'
        } else if scancode == 0x0E {
            b'\x08'
        } else if scancode >= 0x10 && scancode <= 0x35 {
            // 简单的 QWERTY 映射 (小写)
            const MAP: &[u8] = b"qwertyuiopasdfghjklzxcvbnm";
            let idx = (scancode - 0x10) as usize;
            if idx < MAP.len() { MAP[idx] } else { return }
        } else if scancode >= 0x02 && scancode <= 0x0A {
            b'0' + (scancode - 0x01)
        } else {
            return;
        };

        match ch {
            b'\n' => {
                // 确认密码并连接
                self.wifi_connect();
                self.wifi_password_mode = false;
            }
            b'\x08' => {
                if self.wifi_password_len > 0 {
                    self.wifi_password_len -= 1;
                    self.wifi_password[self.wifi_password_len] = 0;
                }
            }
            c if c >= 0x20 && c < 0x7F => {
                if self.wifi_password_len < 63 {
                    self.wifi_password[self.wifi_password_len] = c;
                    self.wifi_password_len += 1;
                }
            }
            _ => {}
        }
    }

    /// 根据鼠标 X 坐标计算 0~100 的值
    fn calc_value_from_x(&self, mouse_x: i32, bar_x: i32, bar_w: i32) -> u8 {
        if bar_w <= 0 { return 50; }
        let rel = mouse_x - bar_x;
        let val = (rel as i32 * 100) / bar_w;
        val.max(0).min(100) as u8
    }

    /// 尝试从 /zrr64/icon.dll 加载图标
    fn load_icon_dll(&mut self) {
        let icon_path = b"/zrr64/icon.dll\0";

        unsafe {
            let fd = syscall2(SYS_OPEN, icon_path.as_ptr() as u64, 0);
            if fd == u64::MAX || (fd as i64) < 0 {
                return;
            }

            // 读取文件头部来确认是有效的 PE 文件
            let mut header = [0u8; 512];
            let n = syscall3(SYS_READ, fd, header.as_mut_ptr() as u64, 512u64);

            syscall2(SYS_CLOSE, fd, 0);

            if (n as i64) >= 2 && header[0] == b'M' && header[1] == b'Z' {
                // PE 文件存在且有效, 标记为已加载
                // 注意: 完整的 PE 资源解析在 no_std 下过于复杂
                // 这里只确认文件存在, 图标仍使用内置绘制
                self.icon_dll_loaded = true;
            }
        }
    }

    /// 从 /system/internet/e1w.cfg 读取网络配置
    fn load_network_config(&mut self) {
        let config_path = b"/system/internet/e1w.cfg\0";

        unsafe {
            let fd = syscall2(SYS_OPEN, config_path.as_ptr() as u64, 0);
            if fd == u64::MAX || (fd as i64) < 0 {
                return;
            }

            let mut buf = [0u8; 256];
            let n = syscall3(SYS_READ, fd, buf.as_mut_ptr() as u64, 256u64);
            syscall2(SYS_CLOSE, fd, 0);

            if (n as i64) <= 0 {
                return;
            }

            let len = n as usize;
            self.network_connected = false;
            self.network_name = [0u8; 64];

            // 简单解析: 找到 name = " 后提取到下一个 "
            let mut name_pos = 0usize;
            for i in 0..len {
                if i + 7 < len && &buf[i..i + 7] == b"name = " {
                    let start = i + 7;
                    if start < len && buf[start] == b'"' {
                        for j in (start + 1)..len {
                            if buf[j] == b'"' { break; }
                            if name_pos < 63 {
                                self.network_name[name_pos] = buf[j];
                                name_pos += 1;
                            }
                        }
                    } else {
                        for j in start..len {
                            if buf[j] == b'\n' || buf[j] == b'\r' { break; }
                            if name_pos < 63 {
                                self.network_name[name_pos] = buf[j];
                                name_pos += 1;
                            }
                        }
                    }
                    break;
                }
            }

            if name_pos > 0 {
                self.network_connected = true;
            }
        }
    }

    /// WiFi 扫描
    fn wifi_scan(&mut self) {
        // 清空列表
        self.wifi_list = [[0u8; 64]; 8];
        self.wifi_list_count = 0;

        unsafe {
            // 尝试打开网络设备
            let dev_path = b"/dev/e1000\0";
            let mut fd = syscall2(SYS_OPEN, dev_path.as_ptr() as u64, 0);
            if fd == u64::MAX || (fd as i64) < 0 {
                // 尝试备用路径
                let dev_path2 = b"/dev/net0\0";
                fd = syscall2(SYS_OPEN, dev_path2.as_ptr() as u64, 0);
            }

            if fd == u64::MAX || (fd as i64) < 0 {
                return;
            }

            // 发送扫描命令
            syscall3(SYS_IOCTL, fd, WIFI_IOCTL_SCAN, 0);

            // 读取扫描结果
            let mut scan_buf = [0u8; 512];
            let n = syscall3(SYS_IOCTL, fd, WIFI_IOCTL_GET_SCAN_RESULTS, scan_buf.as_mut_ptr() as u64);

            syscall2(SYS_CLOSE, fd, 0);

            if (n as i64) <= 0 {
                return;
            }

            // 解析扫描结果: 每个网络名称以 null 分隔, 以双 null 结尾
            let len = (n as usize).min(512);
            let mut pos = 0;
            while pos < len && self.wifi_list_count < 8 {
                let mut name_len = 0;
                while pos + name_len < len && scan_buf[pos + name_len] != 0 && name_len < 63 {
                    name_len += 1;
                }
                if name_len > 0 {
                    self.wifi_list[self.wifi_list_count][..name_len]
                        .copy_from_slice(&scan_buf[pos..pos + name_len]);
                    self.wifi_list_count += 1;
                }
                pos += name_len + 1; // 跳过 null
                if pos < len && scan_buf[pos] == 0 {
                    break; // 双 null 结尾
                }
            }
        }
    }

    /// WiFi 连接
    fn wifi_connect(&mut self) {
        if self.selected_wifi >= self.wifi_list_count {
            return;
        }

        // 获取选中的 WiFi 名称
        let mut ssid = [0u8; 64];
        ssid.copy_from_slice(&self.wifi_list[self.selected_wifi]);
        let mut ssid_len = 0;
        while ssid_len < 64 && ssid[ssid_len] != 0 {
            ssid_len += 1;
        }

        // 写入配置到 /system/internet/e1w.cfg
        // 格式: name = "SSID"\npassword = "PASS"\n
        let config_path = b"/system/internet/e1w.cfg\0";
        let mut content = [0u8; 256];
        let mut pos = 0;

        // name = "
        content[pos] = b'n'; pos += 1;
        content[pos] = b'a'; pos += 1;
        content[pos] = b'm'; pos += 1;
        content[pos] = b'e'; pos += 1;
        content[pos] = b' '; pos += 1;
        content[pos] = b'='; pos += 1;
        content[pos] = b' '; pos += 1;
        content[pos] = b'"'; pos += 1;
        // SSID
        let copy_len = ssid_len.min(200);
        content[pos..pos + copy_len].copy_from_slice(&ssid[..copy_len]);
        pos += copy_len;
        content[pos] = b'"'; pos += 1;
        content[pos] = b'\n'; pos += 1;

        // password = "
        content[pos] = b'p'; pos += 1;
        content[pos] = b'a'; pos += 1;
        content[pos] = b's'; pos += 1;
        content[pos] = b's'; pos += 1;
        content[pos] = b'w'; pos += 1;
        content[pos] = b'o'; pos += 1;
        content[pos] = b'r'; pos += 1;
        content[pos] = b'd'; pos += 1;
        content[pos] = b' '; pos += 1;
        content[pos] = b'='; pos += 1;
        content[pos] = b' '; pos += 1;
        content[pos] = b'"'; pos += 1;
        // 密码
        let pwd_len = self.wifi_password_len.min(60);
        content[pos..pos + pwd_len].copy_from_slice(&self.wifi_password[..pwd_len]);
        pos += pwd_len;
        content[pos] = b'"'; pos += 1;
        content[pos] = b'\n'; pos += 1;

        unsafe {
            let fd = syscall2(SYS_OPEN, config_path.as_ptr() as u64, 0x41); // O_WRONLY|O_CREAT
            if fd == u64::MAX || (fd as i64) < 0 {
                return;
            }
            syscall3(SYS_WRITE, fd, content.as_ptr() as u64, pos as u64);
            syscall2(SYS_CLOSE, fd, 0);
        }

        // 通过 IOCTL 触发连接
        unsafe {
            let dev_path = b"/dev/e1000\0";
            let mut fd = syscall2(SYS_OPEN, dev_path.as_ptr() as u64, 0);
            if fd == u64::MAX || (fd as i64) < 0 {
                let dev_path2 = b"/dev/net0\0";
                fd = syscall2(SYS_OPEN, dev_path2.as_ptr() as u64, 0);
            }
            if fd != u64::MAX && (fd as i64) >= 0 {
                syscall3(SYS_IOCTL, fd, WIFI_IOCTL_CONNECT, 0);
                syscall2(SYS_CLOSE, fd, 0);
            }
        }

        // 更新状态
        self.network_name = [0u8; 64];
        self.network_name[..ssid_len].copy_from_slice(&ssid[..ssid_len]);
        self.network_connected = true;

        // 清空密码
        self.wifi_password = [0u8; 64];
        self.wifi_password_len = 0;
        self.wifi_password_mode = false;
    }

    /// WiFi 断开
    fn wifi_disconnect(&mut self) {
        // 清空配置文件
        let config_path = b"/system/internet/e1w.cfg\0";
        unsafe {
            let fd = syscall2(SYS_OPEN, config_path.as_ptr() as u64, 0x41);
            if fd != u64::MAX && (fd as i64) >= 0 {
                syscall3(SYS_WRITE, fd, 0, 0);
                syscall2(SYS_CLOSE, fd, 0);
            }
        }

        // 通过 IOCTL 断开
        unsafe {
            let dev_path = b"/dev/e1000\0";
            let mut fd = syscall2(SYS_OPEN, dev_path.as_ptr() as u64, 0);
            if fd == u64::MAX || (fd as i64) < 0 {
                let dev_path2 = b"/dev/net0\0";
                fd = syscall2(SYS_OPEN, dev_path2.as_ptr() as u64, 0);
            }
            if fd != u64::MAX && (fd as i64) >= 0 {
                syscall3(SYS_IOCTL, fd, WIFI_IOCTL_DISCONNECT, 0);
                syscall2(SYS_CLOSE, fd, 0);
            }
        }

        // 更新状态
        self.network_name = [0u8; 64];
        self.network_connected = false;
    }

    /// 更新时间显示
    pub fn update_time(&mut self) {
        let timestamp: u64;
        unsafe {
            timestamp = syscall1(SYS_TIME, 0);
        }

        let secs = timestamp + 8 * 3600;
        let days = secs / 86400;
        let time_of_day = secs % 86400;
        let hour = (time_of_day / 3600) as u8;
        let minute = ((time_of_day % 3600) / 60) as u8;
        let second = (time_of_day % 60) as u8;

        let (year, month, day) = days_to_date(days);

        // HH:MM:SS
        self.time_str = [0u8; 20];
        self.time_str[0] = b'0' + hour / 10;
        self.time_str[1] = b'0' + hour % 10;
        self.time_str[2] = b':';
        self.time_str[3] = b'0' + minute / 10;
        self.time_str[4] = b'0' + minute % 10;
        self.time_str[5] = b':';
        self.time_str[6] = b'0' + second / 10;
        self.time_str[7] = b'0' + second % 10;

        // YYYY-MM-DD
        self.date_str = [0u8; 20];
        let mut pos = 0;
        pos += u16_to_bytes(year, &mut self.date_str[pos..]);
        self.date_str[pos] = b'-';
        pos += 1;
        self.date_str[pos] = b'0' + month / 10;
        pos += 1;
        self.date_str[pos] = b'0' + month % 10;
        pos += 1;
        self.date_str[pos] = b'-';
        pos += 1;
        self.date_str[pos] = b'0' + day / 10;
        pos += 1;
        self.date_str[pos] = b'0' + day % 10;
    }

    /// 绘制 16x16 图标到指定位置
    fn draw_icon(&self, ctx: &mut DrawContext, x: i32, y: i32, icon_data: &[u32; 256]) {
        for dy in 0..ICON_SIZE {
            for dx in 0..ICON_SIZE {
                let pixel = icon_data[dy as usize * ICON_SIZE as usize + dx as usize];
                if pixel != 0 {
                    ctx.draw_pixel_raw(x + dx, y + dy, pixel);
                }
            }
        }
    }

    /// 绘制系统托盘 (右下角独立区域)
    pub fn draw(&mut self, ctx: &mut DrawContext) {
        let x = self.x;
        let y = self.y;
        let w = self.w;
        let h = self.h;

        // 背景面板
        ctx.fill_rect(x + 1, y + 1, w - 2, h - 2, &BG_PANEL);

        // 边框 (带发光)
        ctx.draw_rect_glow(x, y, w, h, &CYAN, &GLOW_CYAN);

        // 标题
        ctx.draw_text(x + 6, y + 4, b"SYSTEM", &CYAN);
        if self.icon_dll_loaded {
            ctx.draw_text(x + w - 80, y + 4, b"[icons]", &TEXT_DIM);
        }

        // 分割线
        ctx.draw_hline_dashed(x + 4, x + w - 4, y + 20, 4, 4, &GLOW_CYAN);

        let row1_y = y + 26;
        let row2_y = y + 50;
        let row3_y = y + 74;

        // ── 第一行: 网络 ──
        if self.network_connected {
            // WiFi 图标 (三层弧线)
            for i in 0..3 {
                let arc_r = 4 + i * 4;
                let arc_y = row1_y + 8 - i * 2;
                ctx.draw_line(x + 18 - arc_r, arc_y, x + 18, arc_y - 3, &GREEN);
                ctx.draw_line(x + 18, arc_y - 3, x + 18 + arc_r, arc_y, &GREEN);
            }
            ctx.draw_pixel(x + 18, row1_y + 10, &GREEN);
        } else {
            ctx.draw_line(x + 14, row1_y + 2, x + 22, row1_y + 10, &DIM);
            ctx.draw_line(x + 22, row1_y + 2, x + 14, row1_y + 10, &DIM);
        }
        // 网络名称
        let name_len = buf_len(&self.network_name);
        if name_len > 0 {
            let display_len = name_len.min(16);
            ctx.draw_text(x + 36, row1_y + 2, &self.network_name[..display_len], &TEXT_DIM);
        } else {
            ctx.draw_text(x + 36, row1_y + 2, b"NET: --", &TEXT_DIM);
        }

        // ── 第二行: 音量 + 亮度 ──
        // 音量图标
        ctx.draw_rect(x + 10, row2_y + 2, 6, 8, &CYAN);
        ctx.draw_line(x + 16, row2_y + 2, x + 22, row2_y - 2, &CYAN);
        ctx.draw_line(x + 22, row2_y - 2, x + 22, row2_y + 14, &CYAN);
        ctx.draw_line(x + 22, row2_y + 14, x + 16, row2_y + 10, &CYAN);
        // 音量条
        let vol_bar_x = x + 28;
        let vol_bar_w: i32 = 60;
        let vol_bar_y = row2_y + 6;
        let vol_fill_w = (self.volume as i32 * vol_bar_w) / 100;
        ctx.draw_rect(vol_bar_x, vol_bar_y, vol_bar_w, 4, &DIM);
        ctx.fill_rect(vol_bar_x, vol_bar_y, vol_fill_w, 4, &CYAN);
        // 音量滑块手柄
        let vol_handle_x = vol_bar_x + vol_fill_w - 3;
        ctx.fill_rect(vol_handle_x, vol_bar_y - 3, 6, 10, &WHITE);
        // 音量百分比
        let mut vol_str = [0u8; 8];
        let vol_len = u8_to_bytes(self.volume, &mut vol_str);
        ctx.draw_text(x + 92, row2_y + 2, &vol_str[..vol_len], &TEXT_DIM);
        ctx.draw_text(x + 92 + vol_len as i32 * 8, row2_y + 2, b"%", &TEXT_DIM);

        // 亮度图标
        ctx.draw_rect(x + 120, row2_y + 2, 8, 8, &CYAN);
        ctx.draw_line(x + 124, row2_y - 2, x + 124, row2_y, &CYAN);
        ctx.draw_line(x + 124, row2_y + 10, x + 124, row2_y + 12, &CYAN);
        ctx.draw_line(x + 116, row2_y + 6, x + 118, row2_y + 6, &CYAN);
        ctx.draw_line(x + 130, row2_y + 6, x + 132, row2_y + 6, &CYAN);
        // 亮度条
        let bri_bar_x = x + 138;
        let bri_bar_w: i32 = 60;
        let bri_bar_y = row2_y + 6;
        let bri_fill_w = (self.brightness as i32 * bri_bar_w) / 100;
        ctx.draw_rect(bri_bar_x, bri_bar_y, bri_bar_w, 4, &DIM);
        ctx.fill_rect(bri_bar_x, bri_bar_y, bri_fill_w, 4, &CYAN);
        // 亮度滑块手柄
        let bri_handle_x = bri_bar_x + bri_fill_w - 3;
        ctx.fill_rect(bri_handle_x, bri_bar_y - 3, 6, 10, &WHITE);
        // 亮度百分比
        let mut bri_str = [0u8; 8];
        let bri_len = u8_to_bytes(self.brightness, &mut bri_str);
        ctx.draw_text(x + 202, row2_y + 2, &bri_str[..bri_len], &TEXT_DIM);
        ctx.draw_text(x + 202 + bri_len as i32 * 8, row2_y + 2, b"%", &TEXT_DIM);

        // ── 第三行: 日期时间 ──
        ctx.draw_text(x + 10, row3_y + 2, &self.date_str[..10], &WHITE);
        ctx.draw_text(x + 10 + 10 * 8 + 8, row3_y + 2, &self.time_str[..8], &CYAN);

        // 记录条的位置供 handle_mouse_drag 使用
        self.vol_bar_x = vol_bar_x;
        self.vol_bar_y = vol_bar_y;
        self.vol_bar_w = vol_bar_w;
        self.bri_bar_x = bri_bar_x;
        self.bri_bar_y = bri_bar_y;
        self.bri_bar_w = bri_bar_w;

        // ── 网络面板 ──
        if self.network_panel_open {
            self.draw_network_panel(ctx);
        }
    }

    /// 绘制网络面板
    fn draw_network_panel(&mut self, ctx: &mut DrawContext) {
        let px = self.x;
        let py = self.y - NET_PANEL_H - 4;
        self.net_panel_x = px;
        self.net_panel_y = py;

        // 面板背景
        ctx.fill_rect(px + 1, py + 1, NET_PANEL_W - 2, NET_PANEL_H - 2, &BG_PANEL);

        // 面板边框
        ctx.draw_rect_glow(px, py, NET_PANEL_W, NET_PANEL_H, &CYAN, &GLOW_CYAN);

        // 标题
        ctx.draw_text(px + 6, py + 4, b"NETWORK", &CYAN);

        // 分割线
        ctx.draw_hline_dashed(px + 4, px + NET_PANEL_W - 4, py + 20, 4, 4, &GLOW_CYAN);

        // WiFi 列表
        for i in 0..self.wifi_list_count {
            let item_y = py + 28 + i as i32 * 20;
            if item_y + 18 > py + NET_PANEL_H - 32 {
                break;
            }

            let name_len = buf_len(&self.wifi_list[i]);
            if name_len == 0 { continue; }

            // 选中高亮
            if i == self.selected_wifi {
                ctx.fill_rect(px + 4, item_y, NET_PANEL_W - 8, 18, &GLOW_CYAN);
            }

            // WiFi 图标 (小)
            ctx.draw_line(px + 8, item_y + 10, px + 12, item_y + 4, &CYAN);
            ctx.draw_line(px + 12, item_y + 4, px + 16, item_y + 10, &CYAN);
            ctx.draw_pixel(px + 12, item_y + 12, &CYAN);

            // 网络名称
            let display_len = name_len.min(28);
            let color = if i == self.selected_wifi { &WHITE } else { &TEXT_DIM };
            ctx.draw_text(px + 22, item_y + 2, &self.wifi_list[i][..display_len], color);

            // 如果是当前连接的网络, 用绿色标记
            let current_name_len = buf_len(&self.network_name);
            if current_name_len > 0 && name_len == current_name_len {
                let mut is_current = true;
                for j in 0..current_name_len {
                    if self.wifi_list[i][j] != self.network_name[j] {
                        is_current = false;
                        break;
                    }
                }
                if is_current {
                    ctx.draw_text(px + NET_PANEL_W - 50, item_y + 2, b"[CONN]", &GREEN);
                }
            }
        }

        // 如果没有扫描到网络
        if self.wifi_list_count == 0 {
            ctx.draw_text(px + 6, py + 30, b"No networks found", &TEXT_DIM);
            ctx.draw_text(px + 6, py + 48, b"Click SCAN to search", &TEXT_DIM);
        }

        // 密码输入模式提示
        if self.wifi_password_mode {
            ctx.draw_text(px + 6, py + NET_PANEL_H - 52, b"Password:", &CYAN);
            // 显示密码 (用 * 遮盖)
            let pwd_display = [b'*'; 60];
            let display_len = self.wifi_password_len.min(30);
            ctx.draw_text(px + 6, py + NET_PANEL_H - 38, &pwd_display[..display_len], &WHITE);
        }

        // 底部按钮区域
        let btn_y = py + NET_PANEL_H - 28;

        // 扫描按钮
        ctx.draw_rect(px + 8, btn_y, 80, 22, &CYAN);
        ctx.fill_rect(px + 9, btn_y + 1, 78, 20, &BG_PANEL);
        ctx.draw_text(px + 24, btn_y + 4, b"SCAN", &CYAN);

        // 连接按钮
        ctx.draw_rect(px + 96, btn_y, 80, 22, &GREEN);
        ctx.fill_rect(px + 97, btn_y + 1, 78, 20, &BG_PANEL);
        ctx.draw_text(px + 108, btn_y + 4, b"CONNECT", &GREEN);

        // 断开按钮
        ctx.draw_rect(px + 184, btn_y, 80, 22, &DIM);
        ctx.fill_rect(px + 185, btn_y + 1, 78, 20, &BG_PANEL);
        ctx.draw_text(px + 194, btn_y + 4, b"DISCONNECT", &TEXT_DIM);
    }
}

/// 从字节切片解析 u8 值，返回 (Option<u8>, 消耗的字节数)
fn parse_u8_from_buf(buf: &[u8]) -> (Option<u8>, usize) {
    let mut val: u8 = 0;
    let mut count = 0usize;
    for &b in buf {
        if b >= b'0' && b <= b'9' {
            val = val.saturating_mul(10).saturating_add(b - b'0');
            count += 1;
        } else {
            break;
        }
    }
    if count > 0 {
        (Some(val), count)
    } else {
        (None, 0)
    }
}

/// u8 转十进制字节
fn u8_to_bytes(mut n: u8, buf: &mut [u8]) -> usize {
    if n == 0 {
        buf[0] = b'0';
        return 1;
    }
    let mut digits = [0u8; 3];
    let mut pos = 0;
    while n > 0 {
        digits[pos] = b'0' + (n % 10);
        pos += 1;
        n /= 10;
    }
    for i in 0..pos {
        buf[i] = digits[pos - 1 - i];
    }
    pos
}

/// Unix 天数转年月日
fn days_to_date(days_since_epoch: u64) -> (u16, u8, u8) {
    let mut year = 1970u16;
    let mut remaining = days_since_epoch;

    loop {
        let days_in_year = if is_leap_year(year) { 366u64 } else { 365u64 };
        if remaining < days_in_year {
            break;
        }
        remaining -= days_in_year;
        year += 1;
    }

    let leap = is_leap_year(year);
    let month_days: [u64; 12] = if leap {
        [31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    } else {
        [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    };

    let mut month = 1u8;
    for &md in &month_days {
        if remaining < md {
            break;
        }
        remaining -= md;
        month += 1;
    }

    let day = (remaining + 1) as u8;
    (year, month, day)
}

/// 判断闰年
fn is_leap_year(year: u16) -> bool {
    (year % 4 == 0 && year % 100 != 0) || year % 400 == 0
}

/// u16 转十进制字节
fn u16_to_bytes(mut n: u16, buf: &mut [u8]) -> usize {
    if n == 0 {
        if buf.len() > 0 {
            buf[0] = b'0';
        }
        return 1;
    }
    let mut digits = [0u8; 5];
    let mut pos = 0;
    while n > 0 {
        digits[pos] = b'0' + (n % 10) as u8;
        pos += 1;
        n /= 10;
    }
    for i in 0..pos {
        if i < buf.len() {
            buf[i] = digits[pos - 1 - i];
        }
    }
    pos
}
