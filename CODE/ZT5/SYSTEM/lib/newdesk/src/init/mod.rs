// =============================================================================
// init 模块 - 系统初始化与配置读取
// =============================================================================
// 职责:
//   1. 获取内核帧缓冲区 (framebuffer) 信息
//   2. 读取用户配置文件
//   3. 解析配置项
// =============================================================================



/// 帧缓冲区信息结构体
///
/// 包含帧缓冲区的完整描述信息，用于后续所有图形绘制操作。
/// 通过 SYS_GETFBINFO (156号) 系统调用从内核获取。
/// 内核会将 Limine 提供的物理帧缓冲区映射到用户进程的虚拟地址空间。
///
/// 布局与内核 struct fb_info_user (packed, 24字节) 一致:
///   offset 0:  base    (u64, 8字节)
///   offset 8:  width   (u32, 4字节)
///   offset 12: height  (u32, 4字节)
///   offset 16: pitch   (u32, 4字节)
///   offset 20: format  (u32, 4字节)
///
/// 注意: 虽然内核使用 __attribute__((packed))，但此结构体各字段
/// 在自然对齐下偏移量与 packed 完全相同，因此使用 #[repr(C)] 即可，
/// 避免 packed 带来的引用 UB 问题。
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FbInfo {
    /// 帧缓冲区在用户空间的虚拟基地址
    pub base: u64,
    /// 屏幕宽度（像素）
    pub width: u32,
    /// 屏幕高度（像素）
    pub height: u32,
    /// 每行字节数 (pitch)
    pub pitch: u32,
    /// 像素格式
    /// 3 = XRGB8888, 4 = ARGB8888 (RGB模式)
    /// 6 = XBGR8888, 7 = ABGR8888 (BGR模式)
    pub format: u32,
}

/// 系统调用号定义
///
/// ZratroOS 使用 int 0x80 中断进行系统调用，
/// RAX 存放系统调用号，RDI/RSI/RDX/R10/R8/R9 存放参数
const SYS_EXIT: u64 = 0;
const SYS_READ: u64 = 1;
const SYS_WRITE: u64 = 2;
const SYS_OPEN: u64 = 3;
const SYS_CLOSE: u64 = 4;
const SYS_SLEEP: u64 = 9;
const SYS_MMAP: u64 = 19;
const SYS_GETFBINFO: u64 = 156;
const SYS_FB_SYNC: u64 = 155;

/// 执行 1 参数系统调用
///
/// # 安全性
/// 直接使用内联汇编触发 int 0x80 中断，需要确保参数合法
#[inline(always)]
unsafe fn syscall1(n: u64, a0: u64) -> u64 {
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

/// 执行 2 参数系统调用
#[inline(always)]
unsafe fn syscall2(n: u64, a0: u64, a1: u64) -> u64 {
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

/// 执行 3 参数系统调用
#[inline(always)]
unsafe fn syscall3(n: u64, a0: u64, a1: u64, a2: u64) -> u64 {
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

/// 获取帧缓冲区信息
///
/// 通过 SYS_GETFBINFO 系统调用从内核获取帧缓冲区的完整信息。
/// 内核会将 Limine 提供的物理帧缓冲区页面映射到用户进程的虚拟地址空间，
/// 用户程序可以直接写入该地址来操作屏幕像素。
///
/// # 返回值
/// FbInfo 结构体，包含基地址、宽高、pitch 和像素格式
///
/// # 实现细节
/// - 先将 FbInfo 结构体清零
/// - 调用 SYS_GETFBINFO，传入 FbInfo 的指针
/// - 内核会填充 base/width/height/pitch/format 字段
/// - base 字段是用户空间可直接访问的虚拟地址
pub fn get_framebuffer_info() -> FbInfo {
    let mut info: FbInfo = FbInfo {
        base: 0,
        width: 0,
        height: 0,
        pitch: 0,
        format: 0,
    };
    unsafe {
        syscall2(SYS_GETFBINFO, &mut info as *mut FbInfo as u64, 0);
    }

    // 校验帧缓冲区信息
    if info.base == 0 || info.width == 0 || info.height == 0 || info.pitch == 0 {
        // 帧缓冲区信息无效，无法继续
        // 输出错误信息到串口并退出
        const SYS_WRITE: u64 = 2;
        const SYS_EXIT: u64 = 0;
        let msg = b"newdesk: FATAL: invalid framebuffer info (base/width/height/pitch=0)\n";
        unsafe {
            syscall3(SYS_WRITE, 1, msg.as_ptr() as u64, msg.len() as u64);
            syscall1(SYS_EXIT, 1);
        }
    }

    info
}

/// 同步帧缓冲区
///
/// 将用户空间的帧缓冲区内容刷新到真实的硬件帧缓冲区。
/// 在双缓冲模式下，所有绘制操作先写入后备缓冲区，
/// 然后通过此函数将后备缓冲区的内容复制到真实帧缓冲区，
/// 最后调用 SYS_FB_SYNC 通知内核刷新 CPU 缓存。
///
/// # 参数
/// - `fb_ptr`: 帧缓冲区指针（用户空间映射的地址）
/// - `size`: 需要同步的字节数
pub fn fb_sync(fb_ptr: *const u32, size: usize) {
    unsafe {
        syscall2(SYS_FB_SYNC, fb_ptr as u64, size as u64);
    }
}

/// 线程休眠
///
/// 通过 SYS_SLEEP (9号) 系统调用让当前线程休眠指定的时钟节拍数。
/// 内核 APIC Timer 默认频率为 1000Hz，1 tick = 1ms，
/// 因此传入毫秒数即可实现毫秒级延时。
///
/// # 参数
/// - `ticks`: 休眠的时钟节拍数（毫秒数）
pub fn sys_sleep(ticks: u64) {
    unsafe {
        syscall1(SYS_SLEEP, ticks);
    }
}

/// 读取用户配置文件
///
/// 从 /ini/usrroot/user.cfg 读取用户配置信息。
/// 配置文件格式为每行一个键值对，例如:
/// ```text
/// username=admin
/// password_hash=5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8
/// ```
///
/// # 返回值
/// 配置文件的完整内容（字节切片的堆分配版本）
///
/// # 实现流程
/// 1. SYS_OPEN 打开文件
/// 2. 循环 SYS_READ 读取全部内容
/// 3. SYS_CLOSE 关闭文件描述符
pub fn read_user_config() -> [u8; 4096] {
    const CONFIG_PATH: &[u8] = b"/ini/usrroot/user.cfg\0";
    let mut buf = [0u8; 4096];

    unsafe {
        // 打开配置文件，返回文件描述符
        let fd = syscall2(SYS_OPEN, CONFIG_PATH.as_ptr() as u64, 0); // 0 = O_RDONLY
        if fd == u64::MAX || (fd as i64) < 0 {
            // 文件打开失败，返回空缓冲区
            return buf;
        }

        // 循环读取文件内容
        let mut total_read: usize = 0;
        loop {
            let remaining = buf.len() - total_read;
            if remaining == 0 {
                break;
            }
            let n = syscall3(
                SYS_READ,
                fd,
                buf.as_mut_ptr().add(total_read) as u64,
                remaining as u64,
            );
            if n == 0 {
                break; // EOF
            }
            // 检查负数错误码 (内核返回负数作为错误)
            if (n as i64) < 0 {
                break; // 读取错误
            }
            total_read += n as usize;
        }

        // 关闭文件描述符
        syscall1(SYS_CLOSE, fd);
    }

    buf
}

/// 从配置内容中解析指定键的值
///
/// 在配置文件内容中查找形如 "key=value\n" 的行，
/// 返回等号后面的值部分（去除尾部换行符）。
///
/// # 参数
/// - `config`: 配置文件内容
/// - `key`: 要查找的键名
///
/// # 返回值
/// 对应键的值，如果未找到则返回空字符串
///
/// # 实现细节
/// 逐行扫描配置内容，找到以 "key=" 开头的行后，
/// 提取等号后的内容直到行尾
pub fn parse_config_value(config: &[u8; 4096], key: &[u8]) -> [u8; 256] {
    let mut result = [0u8; 256];
    let prefix_len = key.len();
    let config_len = config.iter().position(|&b| b == 0).unwrap_or(config.len());

    // 逐行扫描
    let mut pos = 0;
    while pos < config_len {
        // 找到当前行的结尾
        let line_end = config[pos..config_len]
            .iter()
            .position(|&b| b == b'\n')
            .map(|i| pos + i)
            .unwrap_or(config_len);

        let line = &config[pos..line_end];

        // 检查是否以 "key=" 开头
        if line.len() > prefix_len + 1 && &line[..prefix_len] == key && line[prefix_len] == b'=' {
            // 提取等号后的值
            let value = &line[prefix_len + 1..];
            let copy_len = value.len().min(result.len());
            result[..copy_len].copy_from_slice(&value[..copy_len]);
            break;
        }

        pos = line_end + 1;
    }

    result
}

// =============================================================================
// MD5 哈希实现
// =============================================================================
// 用于密码验证: 将用户输入的明文密码计算 MD5 哈希后与配置文件中的哈希比较

/// MD5 常量表 (T[i] = floor(2^32 * abs(sin(i+1))))
const MD5_T: [u32; 64] = [
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
];

/// MD5 每轮左移位数
const MD5_S: [u32; 64] = [
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
];

/// 处理一个 64 字节的 MD5 块
fn md5_process_block(state: &mut [u32; 4], block: &[u8]) {
    let mut a = state[0];
    let mut b = state[1];
    let mut c = state[2];
    let mut d = state[3];

    // 将块解析为 16 个 little-endian u32
    let mut m = [0u32; 16];
    for i in 0..16 {
        m[i] = block[i * 4] as u32
            | (block[i * 4 + 1] as u32) << 8
            | (block[i * 4 + 2] as u32) << 16
            | (block[i * 4 + 3] as u32) << 24;
    }

    // 64 轮运算
    for i in 0..64 {
        let (f, g): (u32, usize) = match i {
            0..=15 => ((b & c) | (!b & d), i),
            16..=31 => ((d & b) | (!d & c), (5 * i + 1) % 16),
            32..=47 => (b ^ c ^ d, (3 * i + 5) % 16),
            _ => (c ^ (b | !d), (7 * i) % 16),
        };

        let f = f.wrapping_add(a).wrapping_add(MD5_T[i]).wrapping_add(m[g]);
        a = d;
        d = c;
        c = b;
        b = b.wrapping_add(f.rotate_left(MD5_S[i]));
    }

    state[0] = state[0].wrapping_add(a);
    state[1] = state[1].wrapping_add(b);
    state[2] = state[2].wrapping_add(c);
    state[3] = state[3].wrapping_add(d);
}

/// 计算 MD5 哈希
///
/// 输入任意长度的字节切片，返回 16 字节的哈希值
pub fn md5(input: &[u8]) -> [u8; 16] {
    let input_len = input.len();

    // 计算填充后的长度 (必须是 64 的倍数，且最后 8 字节留给长度)
    let padded_len = ((input_len + 8) / 64 + 1) * 64;

    // 使用固定缓冲区 (最大支持 ~250 字节输入，足够密码使用)
    let mut buf = [0u8; 320];
    buf[..input_len].copy_from_slice(input);
    buf[input_len] = 0x80; // 填充 1 位

    // 附加原始长度 (bits, little-endian)
    let bit_len = (input_len as u64) * 8;
    let len_offset = padded_len - 8;
    buf[len_offset] = bit_len as u8;
    buf[len_offset + 1] = (bit_len >> 8) as u8;
    buf[len_offset + 2] = (bit_len >> 16) as u8;
    buf[len_offset + 3] = (bit_len >> 24) as u8;
    buf[len_offset + 4] = (bit_len >> 32) as u8;
    buf[len_offset + 5] = (bit_len >> 40) as u8;
    buf[len_offset + 6] = (bit_len >> 48) as u8;
    buf[len_offset + 7] = (bit_len >> 56) as u8;

    // 处理每个 64 字节块
    let mut state: [u32; 4] = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476];

    let mut offset = 0;
    while offset < padded_len {
        md5_process_block(&mut state, &buf[offset..offset + 64]);
        offset += 64;
    }

    // 输出为 little-endian 字节
    let mut result = [0u8; 16];
    for i in 0..4 {
        result[i * 4] = state[i] as u8;
        result[i * 4 + 1] = (state[i] >> 8) as u8;
        result[i * 4 + 2] = (state[i] >> 16) as u8;
        result[i * 4 + 3] = (state[i] >> 24) as u8;
    }
    result
}

/// 将字节切片转换为小写十六进制字符串
pub fn bytes_to_hex(bytes: &[u8]) -> [u8; 33] {
    let mut result = [0u8; 33];
    const HEX: &[u8; 16] = b"0123456789abcdef";
    for i in 0..16 {
        result[i * 2] = HEX[(bytes[i] >> 4) as usize];
        result[i * 2 + 1] = HEX[(bytes[i] & 0xf) as usize];
    }
    result[32] = 0;
    result
}
